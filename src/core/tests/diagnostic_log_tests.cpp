#include "vocotype/common/diagnostic_log.hpp"
#include "vocotype/core/dispatcher.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using vocotype::common::Json;

constexpr std::uintmax_t kRetentionBytes = 5'000'000'000ULL;

void require(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

class EnvironmentValue final {
public:
  explicit EnvironmentValue(const char *name) : name_(name) {
    if (const char *value = std::getenv(name); value != nullptr) {
      previous_ = value;
    }
  }

  EnvironmentValue(const EnvironmentValue &) = delete;
  EnvironmentValue &operator=(const EnvironmentValue &) = delete;

  ~EnvironmentValue() {
    if (previous_.has_value()) {
      (void)::setenv(name_.c_str(), previous_->c_str(), 1);
    } else {
      (void)::unsetenv(name_.c_str());
    }
  }

  void set(const std::filesystem::path &value) {
    (void)::setenv(name_.c_str(), value.c_str(), 1);
  }

private:
  std::string name_;
  std::optional<std::string> previous_;
};

bool private_file(const std::filesystem::path &path) {
  struct stat metadata {};
  return ::stat(path.c_str(), &metadata) == 0 &&
         (metadata.st_mode & 0777) == 0600;
}

std::vector<Json> read_events(const std::filesystem::path &path) {
  std::vector<Json> events;
  std::ifstream input(path, std::ios::binary);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      events.push_back(Json::parse(line));
    }
  }
  return events;
}

Json wait_for_task(vocotype::core::CoreDispatcher &dispatcher,
                   const Json &started, const char *expected_status) {
  Json poll;
  for (int attempt = 0; attempt < 200; ++attempt) {
    poll = dispatcher.dispatch(
        {{"type", "polish_poll"}, {"task_id", started.at("task_id")}});
    if (poll.value("status", "") != "running") {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(poll.value("status", "") == expected_status, "任务未达到预期状态");
  return poll;
}

void test_disabled_and_rotation(const std::filesystem::path &root) {
  auto profile = vocotype::common::default_profile_document();
  vocotype::common::save_profile_document(profile);
  const auto log_path = vocotype::common::diagnostic_log_path();
  const auto source = root / "disabled.wav";
  {
    std::ofstream output(source, std::ios::binary);
    output << "RIFF-disabled";
  }
  vocotype::common::begin_diagnostic_session("disabled-trace");
  require(!vocotype::common::save_diagnostic_audio(source, "disabled-trace"),
          "关闭诊断时仍保存了录音");
  vocotype::common::append_diagnostic_session_event(
      "disabled-trace", {{"event", "disabled"}});
  vocotype::common::end_diagnostic_session("disabled-trace");
  require(!std::filesystem::exists(log_path), "关闭诊断时创建了日志");
  require(!std::filesystem::exists(vocotype::common::diagnostic_samples_path()),
          "关闭诊断时创建了会话目录");

  profile["diagnostics"]["enabled"] = true;
  vocotype::common::save_profile_document(profile);
  vocotype::common::append_diagnostic_event({{"event", "enabled"}});
  require(private_file(log_path), "日志权限不是 0600");
  std::filesystem::resize_file(log_path, 10U * 1024U * 1024U);
  vocotype::common::append_diagnostic_event({{"event", "rotate"}});
  require(std::filesystem::exists(log_path.string() + ".1"),
          "顶层日志没有按 10 MiB 轮转");
  require(private_file(log_path.string() + ".1"), "轮转日志权限不是 0600");
  require(std::filesystem::file_size(log_path) < 10U * 1024U * 1024U,
          "轮转后的 active 日志超过上限");
  (void)std::filesystem::remove(source);
}

void test_budget_group_cleanup(const std::filesystem::path &root) {
  const auto samples = vocotype::common::diagnostic_samples_path();
  const auto old_session = samples / "old-trace";
  std::filesystem::create_directories(old_session);
  {
    std::ofstream events(old_session / "events.jsonl", std::ios::binary);
    events << "{\"event\":\"old\"}\n";
  }
  {
    std::ofstream audio(old_session / "audio.wav", std::ios::binary);
  }
  std::filesystem::resize_file(old_session / "audio.wav", kRetentionBytes);
  const auto source = root / "budget.wav";
  {
    std::ofstream output(source, std::ios::binary);
    output << "RIFF-budget";
  }
  vocotype::common::begin_diagnostic_session("new-trace");
  require(vocotype::common::save_diagnostic_audio(source, "new-trace"),
          "预算清理后没有保存新录音");
  vocotype::common::end_diagnostic_session("new-trace");
  require(!std::filesystem::exists(old_session), "预算清理没有整组淘汰旧会话");
  const auto saved = samples / "new-trace" / "audio.wav";
  require(std::filesystem::exists(saved) && private_file(saved),
          "新会话录音缺失或权限错误");
  std::ifstream input(saved, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(input)), {});
  require(content == "RIFF-budget", "保存的录音内容被修改");
  (void)std::filesystem::remove(source);
}

void test_task_audio_and_failure(const std::filesystem::path &root,
                                 const std::filesystem::path &worker_path) {
  auto config = vocotype::core::parse_config(Json::object());
  config.offline_asr.enabled = true;
  config.offline_asr.worker_path = worker_path.string();
  config.offline_asr.model_dir = (root / "model").string();
  config.offline_asr.use_vad = false;
  config.offline_asr.use_punc = false;
  config.offline_asr.startup_timeout_ms = 2000;
  config.offline_asr.request_timeout_ms = 1000;

  const auto success_audio = root / "success.wav";
  {
    std::ofstream output(success_audio, std::ios::binary);
    output << "RIFF-success";
  }
  config.slm.enabled = false;
  std::string success_trace;
  std::string success_task;
  {
    vocotype::core::CoreDispatcher dispatcher(config);
    const Json started = dispatcher.dispatch(
        {{"type", "transcribe_start"},
         {"audio_path", success_audio.string()},
         {"long_mode", true}});
    require(started.value("success", false), "成功任务未启动");
    success_trace = started.value("trace_id", "");
    success_task = started.value("task_id", "");
    require(!success_trace.empty() && !success_task.empty(),
            "成功任务缺少 trace/task id");
    const Json poll = wait_for_task(dispatcher, started, "final");
    require(poll.value("trace_id", "") == success_trace,
            "成功任务 trace_id 未保持一致");
  }
  require(!std::filesystem::exists(success_audio), "任务未清理原始录音");
  const auto success_session = vocotype::common::diagnostic_samples_path() /
                               success_trace;
  const auto success_copy = success_session / "audio.wav";
  require(std::filesystem::exists(success_copy) && private_file(success_copy),
          "成功任务未保存诊断录音");
  std::ifstream success_input(success_copy, std::ios::binary);
  std::string success_content((std::istreambuf_iterator<char>(success_input)),
                              {});
  require(success_content == "RIFF-success", "成功任务诊断录音内容错误");
  const auto success_events = read_events(success_session / "events.jsonl");
  bool saw_asr = false;
  bool saw_slm = false;
  bool saw_result = false;
  for (const Json &event : success_events) {
    require(event.value("trace_id", "") == success_trace &&
                event.value("task_id", "") == success_task,
            "会话事件未关联 task/trace");
    if (event.value("event", "") == "asr_result") {
      saw_asr = event.value("raw_text", "") == "原生最终转写" &&
                event.value("normalized_text", "") == "原生最终转写";
    } else if (event.value("event", "") == "slm_result") {
      saw_slm = !event.value("called", true);
    } else if (event.value("event", "") == "transcription_result") {
      saw_result = event.value("success", false);
    }
  }
  require(saw_asr && saw_slm && saw_result, "成功会话阶段事件不完整");

  const auto failure_audio = root / "failure.wav";
  {
    std::ofstream output(failure_audio, std::ios::binary);
    output << "RIFF-failure";
  }
  config.slm.enabled = true;
  config.slm.remote_stream = false;
  config.slm.endpoint = "http://127.0.0.1:1/v1/chat/completions";
  config.slm.timeout_ms = 200;
  config.slm.min_chars = 1;
  std::string failure_trace;
  {
    vocotype::core::CoreDispatcher dispatcher(config);
    const Json started = dispatcher.dispatch(
        {{"type", "transcribe_start"},
         {"audio_path", failure_audio.string()},
         {"long_mode", true}});
    require(started.value("success", false), "失败任务未启动");
    failure_trace = started.value("trace_id", "");
    (void)wait_for_task(dispatcher, started, "error");
  }
  require(!std::filesystem::exists(failure_audio), "失败任务未清理原始录音");
  const auto failure_session = vocotype::common::diagnostic_samples_path() /
                               failure_trace;
  require(std::filesystem::exists(failure_session / "audio.wav"),
          "SLM 失败时未保留诊断录音");
  bool saw_failure = false;
  for (const Json &event : read_events(failure_session / "events.jsonl")) {
    if (event.value("event", "") == "transcription_result" &&
        !event.value("success", true) &&
        event.value("reason", "") == "request_error") {
      saw_failure = true;
    }
  }
  require(saw_failure, "SLM 失败原因没有写入会话日志");
}

} // namespace

int main(int argc, char **argv) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("vocotype-diagnostics-test-" +
                     std::to_string(::getpid()));
  try {
    require(argc == 2, "缺少假 ASR worker");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    EnvironmentValue profile_environment("VOCOTYPE_PROFILE_CONFIG");
    profile_environment.set(root / "profiles.json");
    EnvironmentValue state_environment("XDG_STATE_HOME");
    state_environment.set(root / "state");
    test_disabled_and_rotation(root);
    test_budget_group_cleanup(root);
    test_task_audio_and_failure(root, argv[1]);
    std::filesystem::remove_all(root);
    std::cout << "诊断日志与录音保留测试通过\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "诊断日志与录音保留测试失败：" << error.what() << '\n';
    std::filesystem::remove_all(root);
    return 1;
  }
}
