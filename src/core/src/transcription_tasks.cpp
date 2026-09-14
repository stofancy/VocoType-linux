#include "vocotype/core/transcription_tasks.hpp"

#include "vocotype/common/diagnostic_log.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>

namespace vocotype::core {
namespace {

Json error_response(const std::string &error) {
  return {{"success", false}, {"error", error}};
}

constexpr auto kTaskTtl = std::chrono::minutes(5);

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

} // namespace

struct TranscriptionTaskManager::Task {
  Task(std::string value, std::string trace)
      : task_id(std::move(value)), trace_id(std::move(trace)) {}

  void append_diagnostic(const std::string &event_name,
                         Json fields = Json::object()) const noexcept {
    try {
      Json event{{"event", event_name},
                 {"task_id", task_id},
                 {"trace_id", trace_id}};
      if (fields.is_object()) {
        for (auto &[key, value] : fields.items()) {
          event[key] = std::move(value);
        }
      }
      vocotype::common::append_diagnostic_event(std::move(event));
    } catch (...) {
      // 诊断日志不能改变转录任务的控制流。
    }
  }

  void add_event_locked(const std::string &kind, const std::string &text,
                        const std::string &event_reason = "") {
    ++seq;
    Json event{{"seq", seq},
               {"kind", kind},
               {"text", text},
               {"task_id", task_id},
               {"trace_id", trace_id}};
    if (!event_reason.empty()) {
      event["reason"] = event_reason;
    }
    if (kind == "delta") {
      event["preview"] = text;
      preview = text;
    }
    events.push_back(std::move(event));
    if (events.size() > 200U) {
      events.erase(events.begin(),
                   events.begin() + static_cast<long>(events.size() - 200U));
    }
  }

  void set_phase(const std::string &value, const std::string &status_text) {
    std::lock_guard lock(mutex);
    if (cancelled || status != "running") {
      return;
    }
    phase = value;
    add_event_locked("status", status_text);
  }

  bool accept_stream_event(const SlmStreamEvent &stream_event) {
    std::lock_guard lock(mutex);
    if (cancelled || status != "running") {
      return false;
    }
    if (stream_event.kind == "heartbeat" || stream_event.kind == "final" ||
        stream_event.kind == "error") {
      return true;
    }
    if (stream_event.kind == "status") {
      add_event_locked("status", stream_event.text);
      return true;
    }
    if (stream_event.kind == "delta") {
      ++seq;
      const std::string full_preview = stream_event.preview.empty()
                                           ? preview + stream_event.text
                                           : stream_event.preview;
      events.push_back({{"seq", seq},
                        {"kind", "delta"},
                        {"text", stream_event.text},
                        {"preview", full_preview},
                        {"task_id", task_id},
                        {"trace_id", trace_id}});
      preview = full_preview;
      if (events.size() > 200U) {
        events.erase(events.begin(),
                     events.begin() + static_cast<long>(events.size() - 200U));
      }
    }
    return true;
  }

  void set_original(const std::string &value) {
    std::lock_guard lock(mutex);
    if (!cancelled) {
      original_text = value;
    }
  }

  bool mark_final(const std::string &value, const std::string &final_reason) {
    std::lock_guard lock(mutex);
    if (cancelled || status != "running") {
      return false;
    }
    status = "final";
    phase = "done";
    preview = value;
    final_text = value;
    reason = final_reason;
    add_event_locked("final", value, final_reason);
    done_at = std::chrono::steady_clock::now();
    return true;
  }

  bool mark_error(const std::string &message, const std::string &error_reason) {
    std::lock_guard lock(mutex);
    if (cancelled || status != "running") {
      return false;
    }
    status = "error";
    phase = "done";
    error = message;
    reason = error_reason;
    add_event_locked("error", message, error_reason);
    done_at = std::chrono::steady_clock::now();
    return true;
  }

  bool cancel() {
    std::lock_guard lock(mutex);
    if (status != "running") {
      return false;
    }
    cancelled = true;
    status = "cancelled";
    phase = "done";
    reason = "cancelled";
    add_event_locked("cancelled", "已取消", "cancelled");
    done_at = std::chrono::steady_clock::now();
    return true;
  }

  [[nodiscard]] bool is_cancelled() const {
    std::lock_guard lock(mutex);
    return cancelled;
  }

  [[nodiscard]] bool expired(std::chrono::steady_clock::time_point now) const {
    std::lock_guard lock(mutex);
    return status != "running" &&
           done_at != std::chrono::steady_clock::time_point{} &&
           now - done_at > kTaskTtl;
  }

  [[nodiscard]] Json snapshot(int after_seq) const {
    std::lock_guard lock(mutex);
    Json selected = Json::array();
    for (const auto &event : events) {
      if (event.value("seq", 0) > after_seq) {
        selected.push_back(event);
      }
    }
    return {{"success", true},
            {"task_id", task_id},
            {"trace_id", trace_id},
            {"status", status},
            {"phase", phase},
            {"events", selected},
            {"last_seq", seq},
            {"preview", preview},
            {"final_text", final_text},
            {"original_text", original_text},
            {"profile_id", profile_id},
            {"profile_name", profile_name},
            {"model", model},
            {"error", error},
            {"reason", reason}};
  }

  void set_slm_metadata(const PolishResult &result) {
    std::lock_guard lock(mutex);
    if (!result.profile_id.empty()) {
      profile_id = result.profile_id;
    }
    if (!result.profile_name.empty()) {
      profile_name = result.profile_name;
    }
    if (!result.model.empty()) {
      model = result.model;
    }
  }

  std::string task_id;
  std::string trace_id;
  mutable std::mutex mutex;
  std::string status = "running";
  std::string phase = "asr";
  std::vector<Json> events;
  int seq = 0;
  std::string preview;
  std::string final_text;
  std::string original_text;
  std::string profile_id;
  std::string profile_name;
  std::string model;
  std::string error;
  std::string reason;
  bool cancelled = false;
  std::chrono::steady_clock::time_point done_at{};
};

TranscriptionTaskManager::TranscriptionTaskManager(OfflineAsrProcess &asr,
                                                   const SlmClient &slm)
    : asr_(asr), slm_(slm) {}

TranscriptionTaskManager::~TranscriptionTaskManager() {
  std::lock_guard lock(workers_mutex_);
  for (auto &worker : workers_) {
    if (worker.thread.joinable()) {
      worker.thread.join();
    }
  }
  workers_.clear();
}

void TranscriptionTaskManager::cleanup_finished_workers() {
  std::lock_guard lock(workers_mutex_);
  for (auto iterator = workers_.begin(); iterator != workers_.end();) {
    if (!iterator->finished->load(std::memory_order_acquire)) {
      ++iterator;
      continue;
    }
    if (iterator->thread.joinable()) {
      iterator->thread.join();
    }
    iterator = workers_.erase(iterator);
  }
}

void TranscriptionTaskManager::cleanup_expired_tasks() {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(tasks_mutex_);
  for (auto iterator = tasks_.begin(); iterator != tasks_.end();) {
    if (iterator->second->expired(now)) {
      iterator = tasks_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

std::string TranscriptionTaskManager::next_task_id() {
  return "cpp-" + std::to_string(::getpid()) + "-" + std::to_string(++next_id_);
}

std::string TranscriptionTaskManager::next_trace_id() {
  const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  return "trace-" + std::to_string(::getpid()) + "-" +
         std::to_string(stamp) + "-" + std::to_string(++next_trace_id_);
}

std::shared_ptr<TranscriptionTaskManager::Task>
TranscriptionTaskManager::find_task(const std::string &task_id) const {
  std::lock_guard lock(tasks_mutex_);
  const auto found = tasks_.find(task_id);
  return found == tasks_.end() ? nullptr : found->second;
}

Json TranscriptionTaskManager::start(const Json &request) {
  cleanup_finished_workers();
  cleanup_expired_tasks();
  const std::string audio_path = request.value("audio_path", "");
  if (audio_path.empty()) {
    return error_response("missing_audio_path");
  }
  const auto expanded = expand_user_path(audio_path);
  if (!std::filesystem::is_regular_file(expanded)) {
    return error_response("audio_file_not_found");
  }

  Json owned_request = request;
  owned_request["audio_path"] = std::filesystem::canonical(expanded).string();
  auto task = std::make_shared<Task>(next_task_id(), next_trace_id());
  task->set_phase("asr", "⏳ 正在识别...");
  vocotype::common::begin_diagnostic_session(task->trace_id);
  task->append_diagnostic(
      "transcription_started",
      {{"status", "running"},
       {"phase", "asr"},
       {"long_mode", request.value("long_mode", false)}});

  {
    std::lock_guard lock(tasks_mutex_);
    tasks_[task->task_id] = task;
  }
  {
    auto finished = std::make_shared<std::atomic<bool>>(false);
    WorkerSlot slot;
    slot.finished = finished;
    slot.thread = std::thread(
        [this, task, owned_request, finished]() mutable {
          struct FinishGuard {
            std::shared_ptr<std::atomic<bool>> flag;
            ~FinishGuard() { flag->store(true, std::memory_order_release); }
          } guard{finished};
          run_task(task, std::move(owned_request));
        });
    std::lock_guard lock(workers_mutex_);
    workers_.push_back(std::move(slot));
  }
  return {{"success", true},
          {"task_id", task->task_id},
          {"trace_id", task->trace_id},
          {"status", "running"}};
}

Json TranscriptionTaskManager::poll(const Json &request) const {
  const std::string task_id = request.value("task_id", "");
  if (task_id.empty()) {
    return error_response("missing_task_id");
  }
  const auto task = find_task(task_id);
  if (!task) {
    return error_response("task_not_found");
  }
  return task->snapshot(std::max(0, request.value("after_seq", 0)));
}

Json TranscriptionTaskManager::cancel(const Json &request) {
  const std::string task_id = request.value("task_id", "");
  if (task_id.empty()) {
    return error_response("missing_task_id");
  }
  const auto task = find_task(task_id);
  if (task) {
    if (task->cancel()) {
      task->append_diagnostic("transcription_cancelled",
                              {{"status", "cancelled"},
                               {"reason", "cancelled"}});
    }
  }
  return {{"success", true}};
}

void TranscriptionTaskManager::run_task(const std::shared_ptr<Task> &task,
                                        Json request) {
  const Clock::time_point task_started = Clock::now();
  struct SessionGuard {
    std::string trace_id;
    ~SessionGuard() {
      vocotype::common::end_diagnostic_session(trace_id);
    }
  } session_guard{task->trace_id};
  const std::filesystem::path audio_path = request.value("audio_path", "");
  struct AudioCleanup {
    std::filesystem::path path;
    std::string trace_id;
    bool active = true;

    void remove_now() {
      if (!active) {
        return;
      }
      (void)vocotype::common::save_diagnostic_audio(path, trace_id);
      std::error_code error;
      std::filesystem::remove(path, error);
      active = false;
    }

    ~AudioCleanup() { remove_now(); }
  } cleanup{audio_path, task->trace_id};

  const auto append_final = [&](bool success, const std::string &raw_text,
                                const std::string &normalized_text,
                                const std::string &result_text,
                                const std::string &reason) {
    task->append_diagnostic(
        "transcription_result",
        {{"success", success},
         {"raw_text", raw_text},
         {"normalized_text", normalized_text},
         {"result_text", result_text},
         {"reason", reason},
         {"total_latency_ms", elapsed_ms(task_started)}});
  };

  try {
    const Clock::time_point asr_started = Clock::now();
    Json result = asr_.transcribe(request);
    const double asr_latency = elapsed_ms(asr_started);
    const bool asr_success = result.value("success", false);
    const std::string raw_text =
        result.value("raw_text", result.value("text", std::string()));
    const std::string normalized_text =
        result.value("text", raw_text);
    const std::string asr_reason =
        result.value("reason", asr_success ? "ok" : "asr_error");
    task->append_diagnostic(
        "asr_result",
        {{"model", asr_.model()},
         {"success", asr_success},
         {"raw_text", raw_text},
         {"normalized_text", normalized_text},
         {"reason", asr_reason},
         {"latency_ms", asr_latency}});
    if (task->is_cancelled()) {
      cleanup.remove_now();
      return;
    }
    if (!asr_success) {
      cleanup.remove_now();
      if (task->mark_error(result.value("error", "转录失败"),
                           asr_reason)) {
        append_final(false, raw_text, normalized_text, {}, asr_reason);
      }
      return;
    }

    task->set_original(normalized_text);
    if (!request.value("long_mode", false)) {
      cleanup.remove_now();
      if (task->mark_final(normalized_text, "ok")) {
        append_final(true, raw_text, normalized_text, normalized_text, "ok");
      }
      return;
    }
    if (!slm_.enabled()) {
      task->append_diagnostic(
          "slm_result",
          {{"success", true},
           {"called", false},
           {"original_text", normalized_text},
           {"result_text", normalized_text},
           {"reason", "disabled"},
           {"latency_ms", 0.0}});
      cleanup.remove_now();
      if (task->mark_final(normalized_text, "disabled")) {
        append_final(true, raw_text, normalized_text, normalized_text,
                     "disabled");
      }
      return;
    }

    const int min_chars = request.value("polish_min_chars", -1);
    if (!slm_.should_polish(normalized_text, min_chars)) {
      task->append_diagnostic(
          "slm_result",
          {{"success", true},
           {"called", false},
           {"original_text", normalized_text},
           {"result_text", normalized_text},
           {"reason", "too_short"},
           {"latency_ms", 0.0}});
      cleanup.remove_now();
      if (task->mark_final(normalized_text, "too_short")) {
        append_final(true, raw_text, normalized_text, normalized_text,
                     "too_short");
      }
      return;
    }

    task->set_phase("polishing", "✨ 正在润色...");
    const std::optional<bool> enable_thinking =
        request.contains("enable_thinking")
            ? std::optional<bool>(request.value("enable_thinking", false))
            : std::nullopt;
    const Clock::time_point slm_started = Clock::now();
    const PolishResult polished =
        slm_.remote_stream()
            ? slm_.stream_polish(normalized_text, enable_thinking,
                                 [task](const SlmStreamEvent &event) {
                                   return task->accept_stream_event(event);
                                 })
            : slm_.polish(normalized_text, enable_thinking);
    const double slm_stage_latency = elapsed_ms(slm_started);
    task->set_slm_metadata(polished);
    Json slm_event{
        {"success", polished.success},
        {"called", true},
        {"original_text", normalized_text},
        {"result_text", polished.text},
        {"reason", polished.reason.empty() ? "slm_error" : polished.reason},
        {"latency_ms", polished.latency_ms},
        {"stage_latency_ms", slm_stage_latency}};
    if (!polished.profile_id.empty()) {
      slm_event["profile_id"] = polished.profile_id;
    }
    if (!polished.profile_name.empty()) {
      slm_event["profile_name"] = polished.profile_name;
    }
    if (!polished.model.empty()) {
      slm_event["model"] = polished.model;
    }
    task->append_diagnostic("slm_result", std::move(slm_event));
    if (task->is_cancelled()) {
      cleanup.remove_now();
      return;
    }
    if (!polished.success) {
      cleanup.remove_now();
      const std::string reason =
          polished.reason.empty() ? "slm_error" : polished.reason;
      if (task->mark_error(polished.error.empty() ? "SLM 调用失败" : polished.error, reason)) {
        append_final(false, raw_text, normalized_text, polished.text, reason);
      }
      return;
    }
    cleanup.remove_now();
    const std::string reason =
        polished.reason.empty() ? "ok" : polished.reason;
    if (task->mark_final(polished.text, reason)) {
      append_final(true, raw_text, normalized_text, polished.text, reason);
    }
  } catch (const std::exception &) {
    cleanup.remove_now();
    if (!task->is_cancelled() && task->mark_error("转录失败", "exception")) {
      task->append_diagnostic(
          "transcription_result",
          {{"success", false},
           {"result_text", ""},
           {"reason", "exception"},
           {"total_latency_ms", elapsed_ms(task_started)}});
    }
  } catch (...) {
    cleanup.remove_now();
    if (!task->is_cancelled() && task->mark_error("转录失败", "exception")) {
      task->append_diagnostic(
          "transcription_result",
          {{"success", false},
           {"result_text", ""},
           {"reason", "exception"},
           {"total_latency_ms", elapsed_ms(task_started)}});
    }
  }
}

} // namespace vocotype::core
