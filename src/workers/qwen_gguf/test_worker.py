"""GGUF worker 的轻量测试，不加载 native library 或模型。"""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
import wave
from unittest.mock import patch


_spec = importlib.util.spec_from_file_location(
    "qwen_gguf_worker", Path(__file__).with_name("worker.py")
)
assert _spec is not None and _spec.loader is not None
worker = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(worker)


class WorkerTest(unittest.TestCase):
    def test_context_hot_reload_and_alias_filter(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            config = Path(root) / "profiles.json"
            with patch.dict(os.environ, {"VOCOTYPE_PROFILE_CONFIG": str(config)}):
                config.write_text(
                    json.dumps(
                        {
                            "vocabulary": [
                                {"canonical": "Claude Code", "aliases": ["cloud"]},
                                {"canonical": "Claude Code", "aliases": ["cloud"]},
                            ]
                        }
                    ),
                    encoding="utf-8",
                )
                self.assertEqual(
                    worker.build_context("VoCoType VoCoType"),
                    "相关术语：VoCoType、Claude Code",
                )
                config.write_text(
                    json.dumps({"vocabulary": [{"canonical": "智谱", "aliases": ["质谱"]}]}),
                    encoding="utf-8",
                )
                self.assertEqual(worker.build_context(""), "相关术语：智谱")

    def test_wav_decode_downmix_and_resample(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            audio = Path(root) / "stereo-8k.wav"
            with wave.open(str(audio), "wb") as output:
                output.setnchannels(2)
                output.setsampwidth(2)
                output.setframerate(8000)
                output.writeframes(struct.pack("<hhhh", 32767, -32768, 0, 0))
            samples = worker.load_wav_16k(str(audio))
            self.assertEqual(len(samples), 4)
            self.assertAlmostEqual(samples[0], -1 / 65536, places=5)
            self.assertAlmostEqual(samples[-1], 0.0, places=5)


if __name__ == "__main__":
    unittest.main()
