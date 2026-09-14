# 中英技术术语混说：ASR / 音频理解候选调研

调研日期：2026-09-14。目标是改善中文句子中夹杂英文 API、模型名、代码标识符时的识别，并判断能否把当前“本地 Paraformer → DeepSeek Flash 完整 profile”合并成一次远程调用。

## 先给结论

目前最值得实测的是两条路线：

1. **保留两段式，替换 ASR**：先试本地 `Qwen3-ASR-0.6B`，再试 `qwen-audio-3.0-asr-flash` 或 `gpt-transcribe`，向 ASR 传领域上下文和术语词表，再把逐字稿交给现有 DeepSeek Flash profile。这条路线职责清楚，容易比较，也能保留实时预览。
2. **一次调用生成最终文本**：试 `gemini-3.8-flash` 音频理解，把音频、完整 profile 和词汇表放在同一次请求中。它能在一次模型调用里同时听音频并按完整指令输出，但这是“用 Gemini 替代 ASR + DeepSeek”，不是让 ASR 在内部调用 DeepSeek。

专用 ASR 文档中的 `prompt`、`context`、`custom_vocabulary` 主要用于**提高听写命中率**。它们不等同于“保持原意、删除口头禅、按指定标点风格输出”等完整指令。要支持当前整套 profile，更适合使用音频理解 / audio LLM，或保留后续 DeepSeek 调用。部分 ASR 提示词可能影响标点和风格，但不能据此推断它能可靠执行全部规则。

## 四个重点候选

| 候选 | 中英技术词与提示能力 | 实时预览 / 录完处理 | 完整 profile | 一次完成最终文本 | 证据边界 |
|---|---|---|---|---|---|
| **Qwen ASR（本地 Qwen3-ASR / 云端 Qwen-Audio 3.0 ASR Flash）** | 两者都覆盖中文、英语等多语种并支持上下文；云端版另有带权热词，单次最多 2,000 个。上下文可以是会话历史、领域描述或词表。 | 本地 Qwen3-ASR 统一支持离线与流式，但流式仅支持 vLLM；云端 `-streaming` 为 WebSocket，`-filetrans` / 非实时 HTTP 用于录完处理。 | **否。** 官方把 context / Prompt 定位为术语识别增强，未承诺任意输出风格或语义改写。 | 未证明能替代完整 profile，建议保留 DeepSeek。 | 官方没有发布针对本项目“中文句中夹英文代码名”的误字率或目标机器延迟，不能仅凭支持中英就断言效果。 |
| **OpenAI GPT-Transcribe / GPT-Live-Transcribe** | 官方明确支持自由文本 context、关键词和多个语言提示，用来改善领域术语、多语言音频及 code-switching。 | `gpt-live-transcribe` 返回低延迟增量；`gpt-transcribe` 支持完成音频、流式文件转写及 Realtime 已提交语音轮次。 | **否。** 两个模型是 STT，上下文的定位是识别提示，文档未承诺执行任意后处理指令。 | 未证明能替代完整 profile，建议保留 DeepSeek。若改用 `gpt-audio-1.5`，可把音频和 developer 指令放进一次 Chat Completions，但那是换成通用音频模型。 | OpenAI 明确说支持 code-switching，但未公布中文+英文技术术语的单独指标；中国网络条件下的延迟也需实测。 |
| **Google Gemini 3.5 Transcribe / Gemini 3.8 Flash 音频理解** | 3.5 Transcribe 官方明确支持句内/句间 code-switching；`custom_vocabulary` 最多 1,000 个词，通常建议不超过 100 个。`smart` 模式能去口头禅、处理自我修正、自动段落/列表和标点。 | 3.5 Transcribe Live 通过 WebSocket 返回 interim 与 final；录完版处理文件。3.8 Flash 音频理解需先提供完整音频，适合松键后的最终处理，不提供边说边出的权威转写。 | 专用 3.5 Transcribe **否**，只有固定的 verbatim / smart；3.8 Flash 音频理解 **是**，可同时输入文本指令和音频，并支持系统指令、结构化输出。 | **是，但由 Gemini 同时承担听写和后处理**，不再调用 DeepSeek。 | 官方明确说明支持动态 code-switching，但仍没有针对中文技术口语的公开实测保证。Live Transcribe 最长连续会话 10 分钟。 |
| **火山引擎豆包大模型流式 ASR** | 官方默认中文模型支持中英；支持热词直传、若干轮会话上下文。 | 支持流式、一次性，以及“双向流式”：先给实时分段，再对完整句做二次优化；还可在停顿后额外做一遍更准确识别。 | **否。** `context` 文档示例是热词或会话上下文，没有完整输出规则的承诺。 | 未证明能替代完整 profile，建议保留 DeepSeek。 | 官方给出垂直领域错误率改善的总体宣传数据，没有本项目中英技术术语集的细分结果；实际首字延迟和二遍完成时间需测。 |

### 本地 Qwen3-ASR 与本机硬件

官方开源的 `Qwen3-ASR-0.6B` 支持中文、英文等 30 种语言和 22 种中文方言，统一支持离线与流式，并有 `context` 字符串用于术语提示。它仍是 ASR，不遵循完整写作 profile，也不能替代 DeepSeek。

目标机器并非纯 CPU：Core Ultra 7 265K、32 GB RAM、RTX 5070 Ti 16 GB（Blackwell，驱动 610.57.04）。因此 `0.6B` 是合理的本地首测，`1.7B` 也值得在资源允许时比较。官方高性能和流式示例使用 vLLM/CUDA，并给出 CUDA 12.9 nightly wheel 的安装路径，说明架构方向匹配。

需要分清**硬件总容量**和**当前空闲容量**：采样时显存已用约 9,288 MiB，只剩约 7 GB。0.6B 权重本身较小，成功运行很有希望；1.7B 的权重、KV cache、vLLM 预留和其他进程合计能否容纳，官方资料不能替代实测。不要为了试验打断未知的现有 GPU 任务；等显存可控时分别测 0.6B / 1.7B 的加载峰值、稳定占用、实时因子和首段延迟，并把 vLLM 的 GPU memory utilization 调到与共存预算一致。

## “一次调用”的准确含义

- **要继续使用 DeepSeek Flash 本身**：做不到真正的一次模型调用。ASR 必须先产生文本，DeepSeek 才能处理；可以用自己的网关把两步包装成一个客户端端点，但底层仍是两次推理和两段延迟。
- **允许更换最终模型**：Gemini 3.8 Flash、OpenAI `gpt-audio-1.5`、Qwen Omni / Audio Realtime 这类音频语言模型可以一次接收“音频 + 完整 profile”，直接返回最终文本。
- **实时预览与一次最终调用可以并存**：继续用本地 Paraformer 只显示临时预览；松键后把原始音频一次发送给音频理解模型生成最终文本。临时预览不参与最终结果，因此不增加远程调用次数。

对 VoCoType 当前按住说话、松键提交的交互，第三种形态最实用：用户仍然立即看到本地预览，最终准确性由录完整句后能看全局语义的模型决定。

## 标点替换是否可以取消

**更好的 ASR 本身不足以证明可以删除本地标点替换。** 声学识别准确率与“最终必须使用中文/英文标点”的产品规则是两个责任层；专用 ASR 的自动标点通常只提供固定策略，Gemini 3.5 Transcribe 也只有固定 `verbatim` / `smart` 模式，没有任意标点风格配置。

如果最终文本已经由 DeepSeek 或 Gemini 3.8 Flash 按完整 profile 生成，现有中文标点到英文标点的映射可能重复处理，也可能改变用户希望保留的中文符号。此时可以**关闭替换**（目前是全局 PunctuationStyle 配置，按 profile 控制尚未实现），但不建议在验证前删除机制：生成模型的指令遵循不是硬保证，而本地映射成本和延迟几乎为零。

建议把是否关闭的验收限定为一组真实句子：中文 prose、英文 prose、中英代码标识符、URL、版本号、小数、括号/引号各覆盖；比较最终 profile 合规率与字符误改。若模型在这些样本中已经稳定满足标点规则，关闭替换是合理简化；否则保留最小、上下文安全的兜底规则。

## 推荐实测顺序

1. **本地 `Qwen3-ASR-0.6B`，随后按资源试 1.7B**：用完整术语 context 跑录完批处理，先证明精度和 RTF；若达标再试 vLLM 流式。它零调用费、音频不出机，且本机有 16 GB 独显，值得排在云服务之前。测试时不得把当前约 7 GB 空闲误写成显卡总容量。
2. **Qwen-Audio 3.0 ASR Flash 非实时版**：接入距离近、成本低，能同时传领域上下文与带权术语；先只替换最终 ASR，不动 DeepSeek。北京区公开价为非实时 `0.00022 元/秒`（约 `0.0132 元/分钟`），流式约 `0.0198 元/分钟`。
3. **OpenAI `gpt-transcribe`**：同一批录音传自由 context、关键词和中英文语言提示，与 Qwen 做盲比。公开价 `$0.0045/分钟`；需要实时预览再测 `$0.017/分钟` 的 `gpt-live-transcribe`。
4. **Gemini**：先用 3.5 Transcribe smart 检验官方明确支持的句内 code-switching、自我修正和 custom vocabulary（录完约 `$0.005/分钟`，Live 约 `$0.009/分钟`）；再用同一批样本测 3.8 Flash 的“一次调用完整 profile”。后者只在希望减少一次公网往返时有决策价值，且不能用生成结果反推纯 ASR 准确率。

每项使用同一组用户真实录音，至少分别统计：术语准确率、普通中文误改率、完整句语义保真、最终标点/profile 合规率、首个预览延迟、松键到最终文本延迟。官方资料没有替代这些目标环境数据。

## 直接来源

- [阿里云：语音识别模型选型与 Prompt 上下文](https://help.aliyun.com/zh/model-studio/asr-model)
- [阿里云：热词、权重与上下文增强](https://help.aliyun.com/en/model-studio/improve-asr-accuracy)
- [阿里云：Qwen ASR 价格](https://help.aliyun.com/en/model-studio/model-pricing)
- [Qwen 官方模型卡：Qwen3-ASR-0.6B](https://huggingface.co/Qwen/Qwen3-ASR-0.6B)
- [OpenAI：GPT-Transcribe](https://developers.openai.com/api/docs/models/gpt-transcribe)
- [OpenAI：GPT-Live-Transcribe](https://developers.openai.com/api/docs/models/gpt-live-transcribe)
- [OpenAI：GPT-Audio-1.5](https://developers.openai.com/api/docs/models/gpt-audio-1.5)
- [Google：Gemini 3.5 Transcribe](https://ai.google.dev/gemini-api/docs/transcribe)
- [Google：Gemini Live Transcribe](https://ai.google.dev/gemini-api/docs/live-api/live-transcribe)
- [Google：Gemini 音频理解与系统指令](https://ai.google.dev/gemini-api/docs/audio)
- [Google：Gemini API 价格](https://ai.google.dev/gemini-api/docs/pricing)
- [火山引擎：大模型 ASR 能力与流式/文件模式](https://www.volcengine.com/docs/6561/1354871?lang=zh)
- [火山引擎：流式模式、热词和上下文参数](https://www.volcengine.com/docs/6348/1807452?lang=zh)
