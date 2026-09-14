#include "vocotype/common/diagnostic_log.hpp"
#include "vocotype/core/dispatcher.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using vocotype::common::Json;
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  const auto root = std::filesystem::temp_directory_path() /
      ("vocotype-diagnostics-test-" + std::to_string(getpid()));
  try {
    require(argc == 2, "缺少假 ASR worker");
    std::filesystem::create_directories(root / "model");
    setenv("XDG_STATE_HOME", root.c_str(), 1);
    setenv("VOCOTYPE_PROFILE_CONFIG", (root / "profiles.json").c_str(), 1);
    auto profile = vocotype::common::default_profile_document();
    vocotype::common::save_profile_document(profile);
    auto path = vocotype::common::diagnostic_log_path();
    vocotype::common::append_diagnostic_event({{"event", "off"}});
    require(!std::filesystem::exists(path), "关闭时创建了日志");
    profile["diagnostics"]["enabled"] = true;
    vocotype::common::save_profile_document(profile);
    vocotype::common::append_diagnostic_event({{"event", "on"}});
    struct stat info{};
    require(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
            "日志权限不是0600");
    std::filesystem::resize_file(path, 5U * 1024U * 1024U);
    vocotype::common::append_diagnostic_event({{"event", "rotate"}});
    require(std::filesystem::exists(path.string() + ".1"), "没有轮转");
    auto config = vocotype::core::parse_config(Json::object());
    config.offline_asr.enabled = true;
    config.offline_asr.worker_path = argv[1];
    config.offline_asr.model_dir = (root / "model").string();
    config.offline_asr.use_vad = false;
    config.offline_asr.use_punc = false;
    config.slm.enabled = false;
    {
      vocotype::core::CoreDispatcher dispatcher(config);
      auto audio = root / "sample.wav";
      std::ofstream(audio) << "RIFFfake";
      auto started = dispatcher.dispatch({{"type", "transcribe_start"},
          {"audio_path", audio.string()}, {"long_mode", true}});
      require(started.value("success", false), "任务未启动");
      auto trace = started.at("trace_id");
      for (int i = 0; i < 200; ++i) {
        auto poll = dispatcher.dispatch({{"type", "polish_poll"},
            {"task_id", started.at("task_id")}});
        require(poll.at("trace_id") == trace, "trace不一致");
        if (poll.value("status", "") == "final") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    bool asr = false, slm = false, final = false;
    std::ifstream input(path); std::string line;
    while (std::getline(input, line)) {
      auto event = Json::parse(line);
      auto kind = event.value("event", "");
      if (kind == "asr_result") {
        asr = event.at("raw_text") == "原生最终转写" && event.contains("model")
            && event.contains("latency_ms");
      }
      if (kind == "slm_result") slm = !event.at("called").get<bool>();
      if (kind == "transcription_result") final = event.at("success").get<bool>();
    }
    require(asr && slm && final, "缺少正确的阶段记录");
    auto size = std::filesystem::file_size(path);
    profile["diagnostics"]["enabled"] = false;
    vocotype::common::save_profile_document(profile);
    vocotype::common::append_diagnostic_event({{"event", "off-again"}});
    require(std::filesystem::file_size(path) == size, "关闭后仍写日志");
    std::filesystem::remove_all(root);
    std::cout << "诊断日志测试通过\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    std::filesystem::remove_all(root);
    return 1;
  }
}
