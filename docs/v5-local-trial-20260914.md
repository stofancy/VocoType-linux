# v5 本地试用调整（2026-09-14）

## 当前交互

- 后处理主键：`Shift+space`。极速仍为 `Shift+Super+m`，编辑仍为 `Control+F9`（此前用户报告系统冲突，未调整）。
- 当前关闭 `asr_streaming.enabled`，保留 `PanelStyle=animated`；录音时仅显示“🎤 录音中”及一至三个点循环，松键后显示“处理中”，由 Qwen3-ASR 识别完整录音，再经 DeepSeek 提交。
- 不展示后处理原文、增量结果、“润色中”及等待计时。错误处理与取消行为保留。
- 恢复旧 Python 分支的完整后处理提示词，包括最小编辑和技术术语保真。
- `PunctuationStyle=english` 在 Fcitx5 最终提交边界应用旧版标点映射；默认 `chinese`。

## 快捷键检查

KDE KGlobalAccel 实时查询确认 Shift+空格与 Meta+S 均可用；Fcitx5 全局热键、全半角、快速输入和本地 Rime 绑定未发现 Shift+空格占用。模块只增加 Shift+空格例外，其余安全检查保留。应用自身快捷键可能各有差异，最终以日常输入框体验为准。

热键通过 Fcitx5 D-Bus `SetConfig` 更新并 `ReloadAddonConfig` 回读验证，避免只改磁盘时被进程内旧配置覆盖。

## 构建与验证

- 独立分支：`codex/v5-quiet-input`，基于 `dddc37a`。
- Fcitx5 模块与 Core 构建通过，3 项模块 CTest 和 2 项 Core CTest 全部通过。
- 标点测试覆盖中文引号、括号、复合标点、技术字符串、空文本和 ASCII 保真。
- 本机缺少 curl 开发头文件，通过下载并解包 Fedora libcurl-devel 到构建目录解决，无系统包安装。
- 已核实运行进程加载用户目录新模块、新 Core；服务 socket ping 成功。
- 最终视觉表现与 Shift+空格按住说话仍需用户在实际输入框试用。

## 用户级安装与回退

安装资产：

- `~/.local/lib/fcitx5/vocotype.so`
- `~/.local/share/fcitx5/addon/vocotype.conf`：指定上述模块绝对路径。
- `~/.local/lib/vocotype-v5-trial/bin/vocotype-core`
- `~/.config/systemd/user/vocotype-fcitx5-backend.service.d/50-local-trial.conf`：设置 `VOCOTYPE_CORE`。
- 原始默认热键备份：`~/.config/fcitx5/conf/vocotype.conf.pre-codex-20260914`。

系统 RPM 文件未替换。回退时移走用户 addon 定义与 systemd 的 `50-local-trial.conf`，执行 `systemctl --user daemon-reload`，重启后端与 Fcitx5，并通过 D-Bus 恢复默认或 Super 系热键。旧 Python 回退资产仍保留。

## 后处理模板与语义词汇（本次实现）

两类配置分别编辑：Profile 保存名称与完整后处理提示词；语义词汇保存正确术语、常见误识别和适用语境，对所有 Profile 生效。两者集中保存在 `~/.config/vocotype/slm-profiles.json`，不保存模型连接或凭据。

每次后处理读取一次配置快照。词汇作为提示词上下文交给模型，不加入 `terms.yaml` 的本地精确替换；极速模式不调用模型，因此不会应用这些语义纠错规则。标点风格仍由最终提交边界确定。

DeepSeek 使用 `deepseek-flash`，通过 `extra_body.thinking.type=disabled` 明确关闭思考。2026-09-14 现场查询官方 `/models` 确认可用；短句流式实测首正文约 0.74 秒、全部完成约 0.81 秒，无 reasoning_content。通过实际 Core 的同句后处理约 0.72 秒。该样本不能代表所有请求耗时或完整 ASR 链路。

官方依据：[模型更名公告](https://www.deepseek.com/en/news/deepseek-v4-1-flash/)、[思考开关](https://api-docs.deepseek.com/guides/thinking_mode/)。旧交接把 `deepseek-flash` 判为无效的结论已被当前官方信息和实测纠正。

### 验收样例

实际 Core 调用 `deepseek-flash` 的四例结果：

| 输入语境 | 输出 |
|---|---|
| 质谱公司的人工智能模型 | 智谱公司的人工智能模型 |
| cloud code 编程助手修改代码 | Claude Code 编程助手修改代码 |
| 文章语言很质朴 | 保留质朴 |
| AWS cloud 服务 | 保留 cloud |

四例后处理耗时约 0.38–1.18 秒。这是本次样本验证，不表示所有语境都不会误改。

“VoCoType 设置 → 后处理模板”中可新建、复制、编辑、删除模板并维护语义词汇；保存后下一次后处理生效。只保存独立模板文件，不写回音频、模型、快捷键配置。

设置中心安装在 `~/.local/bin/vocotype-settings`，用户级桌面入口覆盖系统入口并指向该文件；系统 RPM 文件保留。安装后的真实配置启动探针确认：2 个模板、2 条语义词汇，页面可构建，且启动前后主配置、热键文件和模板文件哈希均未变化。Core 3 项 CTest 通过，覆盖模板文件校验、热加载、流式/非流式提示词、语音编辑隔离。


## 可选诊断日志

设置位置：**VoCoType 设置 → 后处理模板 → 保存录音与诊断日志**，点击“保存并应用”后生效，无需重启。项目默认关闭，本机按用户要求开启。旁边可打开日志目录。

开关保存在 `slm-profiles.json` 的 `diagnostics.enabled`；文件为 `~/.local/state/vocotype/transcription.jsonl`（遵循 `XDG_STATE_HOME`）。录音与日志共用 5,000,000,000 字节（十进制 5 GB）预算，按最旧会话整组清理。每次录音与事件存入 `samples/<trace_id>/`，顶层查询日志保留轮转备份并计入同一预算。Linux 文件权限为 0600、会话目录为 0700。不会保存凭据或完整请求/提示词。

Core 记录同次输入的 ASR 原始返回、规整文本、后处理结果、模型与模板标识及各阶段耗时。Fcitx5 在实际调用提交后记录 `commit`，通过 `trace_id` 关联；这证明输入法发起了提交，不代表目标应用已持久化保存。关闭开关只停止后续记录，不删除既有日志。

ASR 选型与提示词能力调研见 [调研报告](asr-options-20260914.md)。本次没有更换 ASR，也没有删除现有标点规则。

诊断验收：Core 4 项 CTest、Fcitx5 3 项 CTest通过；隔离 IPC 配合假 ASR/SLM 验证两步不同文本、模板、模型和 trace 对应。实际后端用生成的半秒静音 WAV 验证阶段落盘及 0600 权限，服务与用户模块已重新加载。实际输入框的 commit 记录随下一次用户听写验证；未伪造提交事件。

## Qwen3-ASR 试用

最终识别已切换为本地 GPU Qwen3-ASR-1.7B，实时预览保留 Paraformer online；DeepSeek 与最终标点逻辑保留。复用已有 JSONL worker 接口，正确术语作为 ASR context 热加载。安装、测试与回退见 [worker 说明](../src/workers/qwen/README.md)。实际中英混说体验待用户试用。

最新试用决定：暂不引入 vLLM 流式适配，关闭旧 Paraformer 实时预览。已回读后端 streaming_asr=false、final_asr_ready=true，并确认旧 streaming worker 不再运行；千问仍预热待用。


## 录音留存与量化试用（2026-09-14 追加）

用户要求保留录音，以同一音频回放比较 ASR 与后处理。复用现有诊断开关，本机保持开启。关闭后停止新增，不删除已有样本。按键录音当前没有固定最长时长，松键结束；5 GB 是诊断留存预算，不是模型文件预算。16 kHz、单声道 PCM16 录音每分钟约 1.92 MB，实际可保留时长还要扣除文本日志。

Q4_K_M 与 Q8_0 来自 `handy-computer/Qwen3-ASR-1.7B-gguf`；两份文件均已下载并按 Hugging Face LFS SHA256 校验通过。Q4 1,319,830,496 字节，Q8 2,185,030,624 字节。用户要求先试 Q4，保留 Q8 供同音频对照。原 BF16 模型保留用于回退；DeepSeek 提示词保持原样。

量化后端首次验证：`transcribe.cpp` 固定提交 `9eed7f0919ac97c71c71dcd5dcc765c969aa2b05`，Vulkan GPU 设备为 RTX 5070 Ti。官方 4.204 秒中文样本在 Q4、Q8 均返回“甚至出现交易几乎停滞的情况。”，各运行两次后的末次模型推理约 65.6 ms、87.1 ms，模型加载约 785 ms、1224 ms。这不是完整输入链路耗时，也不是中英混说准确率 benchmark。

当前已启用 Q4，量化后端的 Qwen context 扩展已接通，正确术语继续热加载。
Q4/Q8 常驻 worker 的短样本显存分别 1903/3065 MiB，RSS 约 210/272 MiB；
详细安装与回退见 [GGUF worker](../src/workers/qwen_gguf/README.md)。

录音留存验收：实际 BF16 与 Q4 各一次完整 Core/DeepSeek 输入，存储 WAV 与源样本
SHA256 一致，源临时文件仍正常清理；会话事件包含 ASR、DeepSeek 及最终处理结果。
真实 Fcitx commit 会在用户下一次听写时追加，IPC 验证没有伪造提交。
Core 4 项、Fcitx5 3 项测试通过，GGUF 轻测试 2 项通过，设置页探针确认诊断开启。
另将普通 Core 测试与用户 profile 隔离，避免测试假录音污染真实样本库。
