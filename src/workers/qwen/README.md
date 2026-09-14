# Qwen3-ASR 本地试用 worker

通过现有 Core JSONL 子进程接口替换最终 ASR，不修改 Fcitx5 实时预览。模型在 CUDA 上用 bfloat16 / SDPA 推理，启动时用静音预热。stdout 只用于 IPC，第三方输出定向 stderr。

上下文合并 Core 传来的热词和 `slm-profiles.json` 中 vocabulary 的 canonical；后者保留多词短语，不携带 aliases 或完整后处理指令。每次请求重新读取。DeepSeek 后处理保持独立。

## 本机安装

- 独立环境：`~/.local/share/vocotype-qwen/venv`，Python 3.12。
- 实测版本：qwen-asr 0.0.6、torch 2.14.0、transformers 4.57.6；RTX 5070 Ti，驱动 610.57.04。
- 模型：ModelScope `Qwen/Qwen3-ASR-1.7B`，位于 `~/.cache/modelscope/hub/models/Qwen/`。
- 入口：`~/.local/share/vocotype-qwen/vocotype-qwen-worker`，调用同目录 `worker.py`。
- 主配置 `asr` 指向上述 model、model_dir、worker_path，use_vad/use_punc=false（不加载 Paraformer 辅助模型），startup_timeout_s=60、idle_timeout_s=3600。
- 原配置备份：`~/.config/vocotype/config.pre-qwen-trial-20260914.json`，包含凭据，仅本机 0600 保存，不提交。

最小安装命令：

```bash
uv venv --python 3.12 ~/.local/share/vocotype-qwen/venv
uv pip install --python ~/.local/share/vocotype-qwen/venv/bin/python qwen-asr==0.0.6 modelscope
```

## 验证与限制

官方 4.204 秒中文短句样本：首次推理约 2.25 秒，第二次约 0.11 秒，worker GPU 占用约 4.8 GiB。首次独立进程加载约 10.9 秒；启动预热后才报告 ready。这不是中英混说准确率评测，也不代表长句延迟。空闲一小时或服务重启后需重新加载；该试用不提供 OOM 自动回退。

`python3 src/workers/qwen/test_worker.py` 测试术语热加载、去重、带空格正确词保留及 aliases 排除；实际 GPU worker 和 Core IPC 使用官方音频做过转写验证。未采集用户录音。

回退只恢复备份中的 `asr` 字段到当前 config.json（保留期间其他设置），然后重启 `vocotype-fcitx5-backend.service`。这会恢复原 Paraformer 最终识别；原模型和系统安装均保留。
