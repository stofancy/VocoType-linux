#pragma once

#include "vocotype/desktop/config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vocotype::desktop {

struct AsrPreset {
  std::string id;
  std::string label;
  std::string model;
  std::filesystem::path model_dir;
  std::filesystem::path worker_path;

  [[nodiscard]] bool available() const {
    return std::filesystem::is_directory(model_dir) &&
           std::filesystem::is_regular_file(worker_path);
  }
};

inline std::vector<AsrPreset> local_qwen_asr_presets() {
  const auto home = home_path();
  const auto runtime = home / ".local/share/vocotype-qwen";
  const auto gguf_worker = runtime / "gguf-runtime/vocotype-qwen-gguf-worker";
  return {
      {"qwen3-1.7b-q4", "Qwen3-ASR 1.7B · Q4（推荐）",
       "Qwen/Qwen3-ASR-1.7B-Q4_K_M", runtime / "gguf/Q4_K_M",
       gguf_worker},
      {"qwen3-1.7b-q8", "Qwen3-ASR 1.7B · Q8",
       "Qwen/Qwen3-ASR-1.7B-Q8_0", runtime / "gguf/Q8_0", gguf_worker},
      {"qwen3-1.7b-bf16", "Qwen3-ASR 1.7B · BF16（高资源）",
       "Qwen/Qwen3-ASR-1.7B",
       home / ".cache/modelscope/hub/models/Qwen/Qwen3-ASR-1.7B",
       runtime / "vocotype-qwen-worker"},
  };
}

inline std::optional<AsrPreset> find_asr_preset(const std::string &id) {
  for (const auto &preset : local_qwen_asr_presets()) {
    if (preset.id == id)
      return preset;
  }
  return std::nullopt;
}

inline std::string detect_asr_preset(const Json &config) {
  if (!config.is_object())
    return "custom";
  const Json asr = config.value("asr", Json::object());
  if (!asr.is_object())
    return "custom";
  const std::string model = asr.value("model", "");
  const std::string model_dir = asr.value("model_dir", "");
  const std::string worker_path = asr.value("worker_path", "");
  for (const auto &preset : local_qwen_asr_presets()) {
    if (model == preset.model && model_dir == preset.model_dir.string() &&
        worker_path == preset.worker_path.string())
      return preset.id;
  }
  return "custom";
}

inline bool apply_asr_preset(Json &config, const std::string &id) {
  const auto preset = find_asr_preset(id);
  if (!preset)
    return false;
  if (!config.is_object())
    config = Json::object();
  Json &asr = config["asr"];
  if (!asr.is_object())
    asr = Json::object();
  asr["native_enabled"] = true;
  asr["model"] = preset->model;
  asr["model_dir"] = preset->model_dir.string();
  asr["worker_path"] = preset->worker_path.string();
  asr["use_vad"] = false;
  asr["use_punc"] = false;
  asr["startup_timeout_s"] = 60;
  asr["idle_timeout_s"] = 3600;
  return true;
}

} // namespace vocotype::desktop
