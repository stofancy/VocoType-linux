# v5 本地试用调整（2026-09-14）

## 当前交互

- 后处理主键：`Shift+space`。极速仍为 `Shift+Super+m`，编辑仍为 `Control+F9`（此前用户报告系统冲突，未调整）。
- 开启 `asr_streaming.enabled` 和 `PanelStyle=animated`；录音时显示“🎤 录音中”及一至三个点循环，下方保留实时识别文字，松键后同一位置显示“处理中”，完成后只提交最终文本。
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

设置位置：**VoCoType 设置 → 后处理模板 → 记录诊断日志**，点击“保存并应用”后生效，无需重启。项目默认关闭，本机按用户要求开启。旁边可打开日志目录。

开关保存在 `slm-profiles.json` 的 `diagnostics.enabled`；文件为 `~/.local/state/vocotype/transcription.jsonl`（遵循 `XDG_STATE_HOME`）。每个文件最多 5 MiB，保留一份 `.1` 轮转备份，Linux 文件权限为 0600。日志记录文本，不保存音频、凭据或完整请求/提示词。

Core 记录同次输入的 ASR 原始返回、规整文本、后处理结果、模型与模板标识及各阶段耗时。Fcitx5 在实际调用提交后记录 `commit`，通过 `trace_id` 关联；这证明输入法发起了提交，不代表目标应用已持久化保存。关闭开关只停止后续记录，不删除既有日志。

ASR 选型与提示词能力调研见 [调研报告](asr-options-20260914.md)。本次没有更换 ASR，也没有删除现有标点规则。

诊断验收：Core 4 项 CTest、Fcitx5 3 项 CTest通过；隔离 IPC 配合假 ASR/SLM 验证两步不同文本、模板、模型和 trace 对应。实际后端用生成的半秒静音 WAV 验证阶段落盘及 0600 权限，服务与用户模块已重新加载。实际输入框的 commit 记录随下一次用户听写验证；未伪造提交事件。

## Qwen3-ASR 试用

最终识别已切换为本地 GPU Qwen3-ASR-1.7B，实时预览保留 Paraformer online；DeepSeek 与最终标点逻辑保留。复用已有 JSONL worker 接口，正确术语作为 ASR context 热加载。安装、测试与回退见 [worker 说明](../src/workers/qwen/README.md)。实际中英混说体验待用户试用。
