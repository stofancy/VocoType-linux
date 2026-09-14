#!/usr/bin/env python3
"""Qwen3-ASR 本地试用适配器：复用 Core 的 JSONL worker 协议。"""
from __future__ import annotations

import argparse
import contextlib
import json
import os
from pathlib import Path
import select
import sys
import time


def build_context(hotwords: str) -> str:
    """仅提供正确术语作为识别背景，不把写作指令交给 ASR。"""
    terms = hotwords.split()
    config_home = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    path = Path(os.environ.get("VOCOTYPE_PROFILE_CONFIG", config_home / "vocotype/slm-profiles.json"))
    try:
        document = json.loads(path.read_text())
        for entry in document.get("vocabulary", []):
            word = entry.get("canonical", "")
            if isinstance(word, str) and word.strip():
                terms.append(word.strip())
    except FileNotFoundError:
        pass
    except (ValueError, TypeError, OSError, AttributeError) as error:
        print(f"Qwen ASR 术语读取失败：{type(error).__name__}", file=sys.stderr)
    terms = list(dict.fromkeys(terms))
    return "相关术语：" + "、".join(terms) if terms else ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--asr-model-dir", required=True)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--idle-timeout-ms", type=int, default=300000)
    args = parser.parse_args()
    protocol = sys.stdout

    def emit(value: dict) -> None:
        protocol.write(json.dumps(value, ensure_ascii=False) + "\n")
        protocol.flush()

    # 第三方库可能打印进度；stdout 仅用于 IPC。
    with contextlib.redirect_stdout(sys.stderr):
        try:
            import torch
            import numpy as np
            from qwen_asr import Qwen3ASRModel
            torch.set_num_threads(args.threads)
            model = Qwen3ASRModel.from_pretrained(
                args.asr_model_dir, dtype=torch.bfloat16, device_map="cuda:0",
                attn_implementation="sdpa", max_inference_batch_size=1,
                max_new_tokens=1024,
            )
            # 启动阶段完成 CUDA 首次初始化，不占用第一句用户输入。
            with torch.inference_mode():
                model.transcribe(audio=(np.zeros(16000, dtype=np.float32), 16000), language=None)
        except Exception as error:
            emit({"type": "ready", "success": False, "error": str(error)})
            return 1
        emit({"type": "ready", "success": True, "contextual_hotword": True})
        while select.select([sys.stdin], [], [], max(1, args.idle_timeout_ms / 1000))[0]:
            line = sys.stdin.readline()
            if not line:
                break
            try:
                request = json.loads(line)
                kind = request.get("type")
                if kind == "stop":
                    emit({"success": True})
                    break
                if kind in ("ping", "prepare"):
                    emit({"success": True, "prepared": True})
                    continue
                if kind != "transcribe":
                    emit({"success": False, "error": "unknown_request"})
                    continue
                started = time.monotonic()
                with torch.inference_mode():
                    result = model.transcribe(
                        audio=request["audio_path"], language=None,
                        context=build_context(request.get("hotwords", "")),
                    )[0]
                emit({"success": True, "text": result.text, "raw_text": result.text,
                      "language": result.language,
                      "latency_ms": (time.monotonic() - started) * 1000})
            except Exception as error:
                emit({"success": False, "error": str(error)})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
