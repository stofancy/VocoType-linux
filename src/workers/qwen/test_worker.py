"""术语上下文测试，不加载模型或 CUDA。"""
from __future__ import annotations
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("qwen_worker", Path(__file__).with_name("worker.py"))
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)

class ContextTest(unittest.TestCase):
    def test_correct_terms_only_and_hot_reload(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "profiles.json"
            with patch.dict(os.environ, {"VOCOTYPE_PROFILE_CONFIG": str(path)}):
                self.assertEqual(worker.build_context("VoCoType"), "相关术语：VoCoType")
                path.write_text(json.dumps({"vocabulary": [{"canonical": "Claude Code", "aliases": ["cloud"], "context": "不要改普通云"}]}))
                self.assertEqual(worker.build_context("VoCoType VoCoType"), "相关术语：VoCoType、Claude Code")
                path.write_text('{"vocabulary": [{"canonical": "智谱"}]}')
                self.assertEqual(worker.build_context(""), "相关术语：智谱")

if __name__ == "__main__":
    unittest.main()
