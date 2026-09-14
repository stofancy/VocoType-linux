#!/usr/bin/env python3
"""Qwen3-ASR GGUF 的常驻 JSONL worker。

这个适配器只依赖 Python 标准库：模型由 transcribe.cpp 的共享库常驻加载，
音频由 ``wave`` 解码后以 16 kHz 单声道 float32 PCM 传给 C API。stdout 只
用于 Core 的 JSONL IPC，第三方库的诊断输出留在 stderr。
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import contextlib
import json
import os
from pathlib import Path
import select
import struct
import sys
import time
import wave


TRANSCRIBE_OK = 0
TRANSCRIBE_BACKEND_AUTO = 0
TRANSCRIBE_EXT_SLOT_RUN = 0
TRANSCRIBE_EXT_KIND_QWEN3_ASR_RUN = 0x52534151  # little-endian "QASR"
INT_MAX = 2_147_483_647
TARGET_RATE = 16_000


class CModelLoadParams(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint64),
        ("backend", ctypes.c_int),
        ("device", ctypes.c_void_p),
    ]


class CSessionParams(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint64),
        ("n_threads", ctypes.c_int),
        ("kv_type", ctypes.c_int),
        ("n_ctx", ctypes.c_int32),
    ]


class CRunParams(ctypes.Structure):
    # 布局对应固定版本的 include/transcribe.h；升级原生库时须同步检查。
    _fields_ = [
        ("struct_size", ctypes.c_uint64),
        ("task", ctypes.c_int),
        ("timestamps", ctypes.c_int),
        ("pnc", ctypes.c_int),
        ("itn", ctypes.c_int),
        ("diarize", ctypes.c_int),
        ("language", ctypes.c_char_p),
        ("target_language", ctypes.c_char_p),
        ("keep_special_tags", ctypes.c_bool),
        ("family", ctypes.c_void_p),
        ("spec_k_drafts", ctypes.c_int32),
    ]


class CExt(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint64), ("kind", ctypes.c_uint32)]


class CQwenRunExt(ctypes.Structure):
    _fields_ = [
        ("ext", CExt),
        ("context", ctypes.c_char_p),
    ]


class CTimings(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint64),
        ("load_ms", ctypes.c_float),
        ("mel_ms", ctypes.c_float),
        ("encode_ms", ctypes.c_float),
        ("decode_ms", ctypes.c_float),
    ]


def build_context(hotwords: str) -> str:
    """仅提供正确术语作为识别背景，不把润色指令交给 ASR。"""
    terms = hotwords.split()
    config_home = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    path = Path(
        os.environ.get(
            "VOCOTYPE_PROFILE_CONFIG", config_home / "vocotype/slm-profiles.json"
        )
    )
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        for entry in document.get("vocabulary", []):
            word = entry.get("canonical", "")
            if isinstance(word, str) and word.strip():
                terms.append(word.strip())
    except FileNotFoundError:
        pass
    except (ValueError, TypeError, OSError, AttributeError) as error:
        print(f"Qwen GGUF ASR 术语读取失败：{type(error).__name__}", file=sys.stderr)
    terms = list(dict.fromkeys(terms))
    return "相关术语：" + "、".join(terms) if terms else ""


def _find_model_file(model_dir: str) -> Path:
    path = Path(model_dir).expanduser()
    if path.is_file():
        return path
    if not path.is_dir():
        raise FileNotFoundError(f"GGUF model path not found: {path}")
    candidates = sorted(path.glob("*.gguf"))
    if not candidates:
        raise FileNotFoundError(f"no GGUF model in directory: {path}")
    if len(candidates) > 1:
        preferred = [candidate for candidate in candidates if "Qwen3-ASR" in candidate.name]
        candidates = preferred or candidates
    return candidates[0]


def _library_candidates(explicit: str | None) -> list[Path]:
    result: list[Path] = []
    if explicit:
        result.append(Path(explicit).expanduser())
    env_path = os.environ.get("VOCOTYPE_TRANSCRIBE_LIBRARY", "")
    if env_path:
        result.append(Path(env_path).expanduser())
    worker_dir = Path(__file__).resolve().parent
    for base in (worker_dir, worker_dir.parent, Path(sys.argv[0]).resolve().parent):
        result.extend((base / name for name in ("libtranscribe.so", "lib/libtranscribe.so")))
    found = ctypes.util.find_library("transcribe")
    if found:
        result.append(Path(found))
    return list(dict.fromkeys(result))


def _load_library(explicit: str | None) -> tuple[ctypes.CDLL, Path | None]:
    errors: list[str] = []
    for candidate in _library_candidates(explicit):
        try:
            library = ctypes.CDLL(str(candidate), mode=os.RTLD_LOCAL)
            return library, candidate
        except OSError as error:
            errors.append(f"{candidate}: {error}")
    raise RuntimeError(
        "unable to load libtranscribe.so; set VOCOTYPE_TRANSCRIBE_LIBRARY"
        + (" (" + "; ".join(errors) + ")" if errors else "")
    )


def _configure_library(library: ctypes.CDLL) -> None:
    library.transcribe_open.argtypes = [
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    library.transcribe_open.restype = ctypes.c_int
    library.transcribe_session_free.argtypes = [ctypes.c_void_p]
    library.transcribe_session_free.restype = None
    library.transcribe_run.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.c_void_p,
    ]
    library.transcribe_run.restype = ctypes.c_int
    library.transcribe_full_text.argtypes = [ctypes.c_void_p]
    library.transcribe_full_text.restype = ctypes.c_char_p
    library.transcribe_raw_text.argtypes = [ctypes.c_void_p]
    library.transcribe_raw_text.restype = ctypes.c_char_p
    library.transcribe_detected_language.argtypes = [ctypes.c_void_p]
    library.transcribe_detected_language.restype = ctypes.c_char_p
    library.transcribe_status_string.argtypes = [ctypes.c_int]
    library.transcribe_status_string.restype = ctypes.c_char_p
    if hasattr(library, "transcribe_get_timings"):
        library.transcribe_get_timings.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(CTimings),
        ]
        library.transcribe_get_timings.restype = ctypes.c_int
    if hasattr(library, "transcribe_get_model"):
        library.transcribe_get_model.argtypes = [ctypes.c_void_p]
        library.transcribe_get_model.restype = ctypes.c_void_p
    if hasattr(library, "transcribe_model_backend"):
        library.transcribe_model_backend.argtypes = [ctypes.c_void_p]
        library.transcribe_model_backend.restype = ctypes.c_char_p
    if hasattr(library, "transcribe_model_accepts_ext_kind"):
        library.transcribe_model_accepts_ext_kind.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_uint32,
        ]
        library.transcribe_model_accepts_ext_kind.restype = ctypes.c_bool
    if hasattr(library, "transcribe_qwen3_asr_run_ext_init"):
        library.transcribe_qwen3_asr_run_ext_init.argtypes = [
            ctypes.POINTER(CQwenRunExt)
        ]
        library.transcribe_qwen3_asr_run_ext_init.restype = None
    if hasattr(library, "transcribe_run_params_init"):
        library.transcribe_run_params_init.argtypes = [ctypes.POINTER(CRunParams)]
        library.transcribe_run_params_init.restype = None
    if hasattr(library, "transcribe_abi_struct_size"):
        library.transcribe_abi_struct_size.argtypes = [ctypes.c_int]
        library.transcribe_abi_struct_size.restype = ctypes.c_size_t


def _decode_pcm(raw: bytes, width: int, channels: int) -> list[float]:
    if channels < 1:
        raise ValueError("WAV reports no channels")
    frame_width = width * channels
    if width not in (1, 2, 3, 4):
        raise ValueError(f"unsupported WAV sample width: {width} bytes")
    if len(raw) % frame_width:
        raise ValueError("WAV payload is not aligned to complete frames")
    frames = len(raw) // frame_width
    result = [0.0] * frames
    scale = float(1 << (8 * width - 1))
    offset = 0
    for frame in range(frames):
        total = 0
        for _channel in range(channels):
            if width == 1:
                value = raw[offset] - 128
            elif width == 2:
                value = struct.unpack_from("<h", raw, offset)[0]
            elif width == 3:
                value = int.from_bytes(raw[offset : offset + 3], "little", signed=True)
            else:
                value = struct.unpack_from("<i", raw, offset)[0]
            total += value
            offset += width
        result[frame] = max(-1.0, min(1.0, total / (channels * scale)))
    return result


def _resample(samples: list[float], source_rate: int) -> list[float]:
    if source_rate == TARGET_RATE:
        return samples
    if source_rate <= 0:
        raise ValueError(f"invalid WAV sample rate: {source_rate}")
    out_count = int(round(len(samples) * TARGET_RATE / source_rate))
    if out_count <= 0:
        raise ValueError("WAV is empty")
    if len(samples) == 1:
        return [samples[0]] * out_count
    result = [0.0] * out_count
    ratio = source_rate / TARGET_RATE
    for index in range(out_count):
        position = index * ratio
        left = min(len(samples) - 1, int(position))
        right = min(len(samples) - 1, left + 1)
        fraction = position - left
        result[index] = samples[left] + (samples[right] - samples[left]) * fraction
    return result


def load_wav_16k(path: str) -> list[float]:
    """读取 PCM WAV，单声道化并线性重采样到 C API 要求的 16 kHz。"""
    with wave.open(path, "rb") as source:
        channels = source.getnchannels()
        width = source.getsampwidth()
        rate = source.getframerate()
        frames = source.getnframes()
        if frames <= 0:
            raise ValueError("WAV is empty")
        if frames > INT_MAX:
            raise ValueError("WAV is too long for transcribe.cpp int sample count")
        raw = source.readframes(frames)
    return _resample(_decode_pcm(raw, width, channels), rate)


def _cstring(value: bytes | None) -> str:
    return value.decode("utf-8", errors="replace") if value else ""


class NativeWorker:
    def __init__(self, model_path: Path, library_path: str | None, threads: int, n_ctx: int):
        self.library, self.library_path = _load_library(library_path)
        _configure_library(self.library)
        self.model = ctypes.c_void_p()
        self.session = ctypes.c_void_p()
        self.threads = threads
        self.n_ctx = n_ctx
        self.contextual_hotword = False
        self.warmup_ms = 0.0
        self._open(model_path)
        # 就绪前预热，避免第一句录音承担 Vulkan 图与内核初始化开销。
        started = time.monotonic()
        self.transcribe([0.0] * TARGET_RATE, "")
        self.warmup_ms = (time.monotonic() - started) * 1000.0

    def _open(self, model_path: Path) -> None:
        if not model_path.is_file():
            raise FileNotFoundError(f"GGUF model not found: {model_path}")
        load_params = None
        if hasattr(self.library, "transcribe_init_backends_default"):
            self.library.transcribe_init_backends_default.argtypes = []
            self.library.transcribe_init_backends_default.restype = ctypes.c_int
            status = self.library.transcribe_init_backends_default()
            if status != TRANSCRIBE_OK:
                print(f"transcribe backend init returned {status}", file=sys.stderr)

        session_params = CSessionParams()
        if self.n_ctx > 0 or self.threads > 0:
            if not hasattr(self.library, "transcribe_session_params_init"):
                raise RuntimeError("native library lacks session params initializer")
            self.library.transcribe_session_params_init.argtypes = [
                ctypes.POINTER(CSessionParams)
            ]
            self.library.transcribe_session_params_init.restype = None
            self.library.transcribe_session_params_init(ctypes.byref(session_params))
            session_params.n_threads = self.threads
            if self.n_ctx > 0:
                session_params.n_ctx = self.n_ctx
            session_arg = ctypes.cast(ctypes.byref(session_params), ctypes.c_void_p)
        else:
            session_arg = None

        status = self.library.transcribe_open(
            os.fsencode(str(model_path)), load_params, session_arg, ctypes.byref(self.session)
        )
        if status != TRANSCRIBE_OK or not self.session:
            detail = _cstring(self.library.transcribe_status_string(status))
            raise RuntimeError(f"transcribe_open failed ({status}): {detail}")
        if hasattr(self.library, "transcribe_get_model") and hasattr(
            self.library, "transcribe_model_accepts_ext_kind"
        ):
            model = self.library.transcribe_get_model(self.session)
            self.contextual_hotword = bool(
                model
                and self.library.transcribe_model_accepts_ext_kind(
                    model, TRANSCRIBE_EXT_SLOT_RUN, TRANSCRIBE_EXT_KIND_QWEN3_ASR_RUN
                )
                and hasattr(self.library, "transcribe_qwen3_asr_run_ext_init")
            )

    def close(self) -> None:
        if self.session:
            self.library.transcribe_session_free(self.session)
            self.session = ctypes.c_void_p()

    def backend(self) -> str:
        if not self.session or not hasattr(self.library, "transcribe_get_model"):
            return ""
        model = self.library.transcribe_get_model(self.session)
        if not model or not hasattr(self.library, "transcribe_model_backend"):
            return ""
        return _cstring(self.library.transcribe_model_backend(model))

    def transcribe(self, samples: list[float], context: str) -> dict[str, object]:
        if not samples:
            raise ValueError("audio has no samples")
        if len(samples) > INT_MAX:
            raise ValueError("audio exceeds native sample count limit")
        pcm = (ctypes.c_float * len(samples))(*samples)
        run_params: ctypes.c_void_p | ctypes._Pointer[CRunParams] | None = None
        ext = None
        context_bytes = None
        if self.contextual_hotword and context:
            context_bytes = context.encode("utf-8")
            ext = CQwenRunExt()
            self.library.transcribe_qwen3_asr_run_ext_init(ctypes.byref(ext))
            ext.context = ctypes.c_char_p(context_bytes)
            params = CRunParams()
            self.library.transcribe_run_params_init(ctypes.byref(params))
            params.family = ctypes.addressof(ext)
            run_params = ctypes.byref(params)
        status = self.library.transcribe_run(
            self.session, pcm, len(samples), run_params
        )
        if status != TRANSCRIBE_OK:
            detail = _cstring(self.library.transcribe_status_string(status))
            raise RuntimeError(f"transcribe_run failed ({status}): {detail}")
        text = _cstring(self.library.transcribe_full_text(self.session))
        raw = _cstring(self.library.transcribe_raw_text(self.session))
        language = _cstring(self.library.transcribe_detected_language(self.session))
        result: dict[str, object] = {
            "text": text,
            "raw_text": raw or text,
            "language": language,
        }
        if hasattr(self.library, "transcribe_get_timings"):
            timings = CTimings()
            timings.struct_size = ctypes.sizeof(CTimings)
            if self.library.transcribe_get_timings(self.session, ctypes.byref(timings)) == 0:
                result["timings"] = {
                    "mel_ms": float(timings.mel_ms),
                    "encode_ms": float(timings.encode_ms),
                    "decode_ms": float(timings.decode_ms),
                }
        return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--asr-model-dir", "--model-path", dest="model_path", required=True)
    parser.add_argument("--library", default=None)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--n-ctx", type=int, default=0)
    parser.add_argument("--idle-timeout-ms", type=int, default=300000)
    args = parser.parse_args()
    if args.threads < 0 or args.threads > 64:
        raise SystemExit("--threads must be between 0 and 64")
    if args.n_ctx < 0:
        raise SystemExit("--n-ctx cannot be negative")
    idle_timeout_s = max(1.0, args.idle_timeout_ms / 1000.0)
    protocol = sys.stdout

    def emit(value: dict[str, object]) -> None:
        protocol.write(json.dumps(value, ensure_ascii=False) + "\n")
        protocol.flush()

    native: NativeWorker | None = None
    try:
        model_path = _find_model_file(args.model_path)
        with contextlib.redirect_stdout(sys.stderr):
            native = NativeWorker(model_path, args.library, args.threads, args.n_ctx)
        emit(
            {
                "type": "ready",
                "success": True,
                "sample_rate": TARGET_RATE,
                "contextual_hotword": native.contextual_hotword,
                "vad": False,
                "punctuation": False,
                "model": model_path.name,
                "backend": native.backend(),
                "n_ctx": args.n_ctx,
                "warmup_ms": native.warmup_ms,
            }
        )
    except Exception as error:
        emit({"type": "ready", "success": False, "error": str(error)})
        if native is not None:
            native.close()
        return 1

    last_activity = time.monotonic()
    try:
        while True:
            remaining = idle_timeout_s - (time.monotonic() - last_activity)
            if remaining <= 0:
                break
            readable, _, _ = select.select([sys.stdin], [], [], remaining)
            if not readable:
                break
            line = sys.stdin.readline()
            if not line:
                break
            last_activity = time.monotonic()
            try:
                request = json.loads(line)
                kind = request.get("type")
                if kind == "stop":
                    emit({"success": True})
                    break
                if kind == "ping":
                    emit({"success": True, "prepared": True})
                    continue
                if kind == "prepare":
                    hotwords = request.get("hotwords", "")
                    context = build_context(hotwords if isinstance(hotwords, str) else "")
                    emit(
                        {
                            "success": True,
                            "prepared": True,
                            "hotwords": hotwords,
                            "context": context,
                            "contextual_hotword": native.contextual_hotword,
                        }
                    )
                    continue
                if kind != "transcribe":
                    emit({"success": False, "error": "unknown_request"})
                    continue
                audio_path = request.get("audio_path", "")
                if not isinstance(audio_path, str) or not audio_path:
                    raise ValueError("audio_path is required")
                hotwords = request.get("hotwords", "")
                context = build_context(hotwords if isinstance(hotwords, str) else "")
                started = time.monotonic()
                samples = load_wav_16k(audio_path)
                try:
                    result = native.transcribe(samples, context)
                finally:
                    del samples  # 不在空闲阶段保留上一段录音的浮点数组。
                result.update(
                    {
                        "success": True,
                        "latency_ms": (time.monotonic() - started) * 1000.0,
                        "hotwords": hotwords,
                        "contextual_hotword": native.contextual_hotword,
                    }
                )
                emit(result)
            except Exception as error:
                print(f"Qwen GGUF request failed: {type(error).__name__}: {error}", file=sys.stderr)
                emit({"success": False, "error": str(error)})
    finally:
        if native is not None:
            native.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
