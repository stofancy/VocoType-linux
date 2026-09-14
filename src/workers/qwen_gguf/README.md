# Qwen3-ASR GGUF worker

这是面向 `handy-computer/transcribe.cpp` 的 Qwen3-ASR-1.7B GGUF 常驻
JSONL worker。它使用 C API 一次加载模型，后续请求复用同一个 session，避免
每次语音输入重新加载模型。

模型文件来自 `handy-computer/Qwen3-ASR-1.7B-gguf`，当前试用固定为：

- `Qwen3-ASR-1.7B-Q4_K_M.gguf`：1,319,830,496 字节，SHA-256
  `b7afe3674f653fa84f712ed2440353c6e7cf7f93697fef76b05a26538b24844e`
- `Qwen3-ASR-1.7B-Q8_0.gguf`：2,185,030,624 字节，SHA-256
  `9a0d81792dfea2d5f278b8a63deb3ea6e02139ce42c2301f32ea19c4f77526b7`

下载仓库的 `transcribe.cpp` 固定在父仓库 commit
`9eed7f0919ac97c71c71dcd5dcc765c969aa2b05`。运行时通过
`VOCOTYPE_TRANSCRIBE_LIBRARY` 指定 `libtranscribe.so`，例如：

```bash
VOCOTYPE_TRANSCRIBE_LIBRARY=/path/to/libtranscribe.so \
  ./worker.py --asr-model-dir /path/to/Qwen3-ASR-1.7B-Q4_K_M.gguf \
  --n-ctx 1024 --idle-timeout-ms 3600000
```

`--asr-model-dir` 兼容现有 Core 的参数名，也接受一个包含 GGUF 文件的
目录；`--n-ctx` 为 0 时使用原生库默认值，设置较小的值可以降低 KV cache
占用，但过小会缩短可接受的录音长度。worker 会把非 16 kHz、非单声道的
PCM WAV 在内存中转换到原生库要求的 16 kHz 单声道 float32。

`build_context()` 每次请求都会重新读取 profile 配置，只加入 canonical
术语，不把 aliases 当作识别词，也不把润色指令传给 ASR。当前
`transcribe.cpp` 公共 API 没有 Qwen context 参数，因此 worker 会探测可选的
Qwen run extension；在原生库应用同目录的
`qwen3-asr-context.patch` 并重新编译后，术语上下文才会真正插入 system
turn。未应用补丁时响应会明确返回 `contextual_hotword: false`，不会伪称热词
已经生效。

测试不加载模型、不需要 CUDA：

```bash
python3 src/workers/qwen_gguf/test_worker.py
```


## 本机安装与回退

原生库使用 Vulkan，编译时需要 Vulkan 开发头文件、loader、glslc、SPIRV-Headers。
固定源码提交后应用本目录补丁，再构建共享库：

```bash
git checkout 9eed7f0919ac97c71c71dcd5dcc765c969aa2b05
git apply /path/to/qwen_gguf/qwen3-asr-context.patch
cmake -S . -B build -DTRANSCRIBE_VULKAN=ON -DTRANSCRIBE_BUILD_SHARED=ON -DTRANSCRIBE_BUILD_TESTS=OFF
cmake --build build -j4
```

本机已安装至 `~/.local/share/vocotype-qwen/gguf-runtime/`，`lib/` 包含
libtranscribe 和其 ggml 依赖。启动脚本设置该目录的 `LD_LIBRARY_PATH` 与
`VOCOTYPE_TRANSCRIBE_LIBRARY`，使用系统 Python 标准库，无需 torch。

Core 要求 `asr.model_dir` 是目录，因此分别建立 `gguf/Q4_K_M/`、`gguf/Q8_0/`，
其中 `model.gguf` 是对应下载文件的相对符号链接，不复制模型。
本机当前配置使用 Q4 目录，`asr.model` 为 `Qwen/Qwen3-ASR-1.7B-Q4_K_M`，
`asr.worker_path` 指向 `gguf-runtime/vocotype-qwen-gguf-worker`。
未设置额外 `n_ctx` 上限；原生库按音频需要分配 KV，默认窗口不是启动即全量分配。

改用 Q8 时只需同步修改 `asr.model` 和 `asr.model_dir`，然后重启用户服务
`vocotype-fcitx5-backend.service`。回退 BF16 时，从
`~/.config/vocotype/config.pre-gguf-trial-20260914.json` 仅恢复 `asr` 对象并重启服务，
保留其他后续配置。不要同时常驻 BF16 和量化 worker，以免显存不足。

## 本机验证

RTX 5070 Ti，4.204 秒官方中文样本，术语 context 启用，模型常驻后：

| 版本 | worker 请求耗时 | 进程显存 | 进程 RSS |
|---|---:|---:|---:|
| Q4_K_M | 约 77 ms | 1903 MiB | 约 210 MiB |
| Q8_0 | 约 89 ms | 3065 MiB | 约 272 MiB |

两者均正确返回“甚至出现交易几乎停滞的情况。”。数据是短音频单次末次测量，
包含 worker WAV 处理，不含 DeepSeek；不代表中英混说准确率或长录音峰值。
系统服务统计可能另含文件缓存和驱动内存，因此不应把 RSS 当作完整系统开销。
实际 Core Q4 + DeepSeek 验证约 88 ms + 899 ms，总计约 991 ms。
