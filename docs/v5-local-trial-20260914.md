# v5 本地试用调整（2026-09-14）

## 当前交互

- 后处理主键：`Shift+space`。极速仍为 `Shift+Super+m`，编辑仍为 `Control+F9`（此前用户报告系统冲突，未调整）。
- 录音时显示“录音中”，松键后同一位置显示“处理中”，完成后只提交最终文本。
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
