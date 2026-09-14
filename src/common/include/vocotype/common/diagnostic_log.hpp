#pragma once

#include "vocotype/common/slm_profiles.hpp"

#include <cerrno>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vocotype::common {

[[nodiscard]] inline std::filesystem::path diagnostic_log_path();

namespace diagnostic_detail {

// 顶层查询日志保留 active 与 .1 两个文件；会话音频和事件共用 5 GB
// 的十进制总预算。单条 JSONL 记录不能超过顶层日志轮转上限。
inline constexpr std::uintmax_t kMaxLogBytes = 10U * 1024U * 1024U;
inline constexpr std::uintmax_t kRetentionBytes = 5'000'000'000ULL;

inline bool missing_error(const std::error_code &error) noexcept {
  return error == std::make_error_code(std::errc::no_such_file_or_directory);
}

inline void report_failure(std::string_view message) noexcept {
  try {
    std::cerr << "VoCoType: 诊断日志不可用（" << message << "）\n";
  } catch (...) {
  }
}

inline std::string timestamp_now() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds =
      std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - seconds)
                          .count();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#if defined(_WIN32)
  if (::gmtime_s(&utc, &time) != 0) {
    throw std::runtime_error("无法生成诊断日志时间戳");
  }
#else
  if (::gmtime_r(&time, &utc) == nullptr) {
    throw std::runtime_error("无法生成诊断日志时间戳");
  }
#endif
  char date[32] = {};
  if (std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
    throw std::runtime_error("无法生成诊断日志时间戳");
  }
  std::ostringstream output;
  output << date << '.' << std::setfill('0') << std::setw(3) << millis << 'Z';
  return output.str();
}

inline bool diagnostics_enabled() {
  const Json document = load_profile_document();
  const auto diagnostics = document.find("diagnostics");
  return diagnostics != document.end() && diagnostics->is_object() &&
         diagnostics->value("enabled", false);
}

#if defined(_WIN32)

class FileLock final {
public:
  explicit FileLock(const std::filesystem::path &) {
    static std::mutex mutex;
    lock_ = std::unique_lock<std::mutex>(mutex);
  }

private:
  std::unique_lock<std::mutex> lock_;
};

inline void set_private_mode(const std::filesystem::path &) {}

#else

class FileLock final {
public:
  explicit FileLock(const std::filesystem::path &log_path) {
    std::filesystem::path lock_path = log_path;
    lock_path += ".lock";
    int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    descriptor_ = ::open(lock_path.c_str(), flags, 0600);
    if (descriptor_ < 0) {
      throw std::runtime_error("无法打开诊断日志锁文件");
    }
    if (::fchmod(descriptor_, 0600) != 0 ||
        ::flock(descriptor_, LOCK_EX) != 0) {
      const int saved_errno = errno;
      ::close(descriptor_);
      descriptor_ = -1;
      throw std::system_error(saved_errno, std::generic_category(),
                              "无法锁定诊断日志");
    }
  }

  FileLock(const FileLock &) = delete;
  FileLock &operator=(const FileLock &) = delete;

  ~FileLock() {
    if (descriptor_ >= 0) {
      (void)::flock(descriptor_, LOCK_UN);
      (void)::close(descriptor_);
    }
  }

private:
  int descriptor_ = -1;
};

inline void set_private_mode(const std::filesystem::path &path) {
  if (::chmod(path.c_str(), 0600) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "无法设置诊断日志权限");
  }
}

#endif

inline std::uintmax_t current_size(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    if (missing_error(error)) {
      return 0;
    }
    if (error) {
      throw std::system_error(error, "无法检查诊断日志");
    }
    return 0;
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    throw std::system_error(error, "无法读取诊断日志大小");
  }
  return size;
}

inline void rotate(const std::filesystem::path &path) {
  const std::filesystem::path rotated = path.string() + ".1";
  std::error_code error;
  std::filesystem::remove(rotated, error);
  if (error) {
    throw std::system_error(error, "无法清理旧诊断日志");
  }
  std::filesystem::rename(path, rotated, error);
  if (error) {
    throw std::system_error(error, "无法轮转诊断日志");
  }
  set_private_mode(rotated);
}

#if defined(_WIN32)
inline void append_bytes(const std::filesystem::path &path,
                         const std::string &line) {
  std::ofstream output(path, std::ios::binary | std::ios::app);
  output.exceptions(std::ios::badbit | std::ios::failbit);
  output << line;
}
#else
inline void append_bytes(const std::filesystem::path &path,
                         const std::string &line) {
  int flags = O_WRONLY | O_CREAT | O_APPEND;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  const int descriptor = ::open(path.c_str(), flags, 0600);
  if (descriptor < 0) {
    throw std::runtime_error("无法打开诊断日志");
  }
  try {
    if (::fchmod(descriptor, 0600) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "无法设置诊断日志权限");
    }
    std::size_t offset = 0;
    while (offset < line.size()) {
      const ssize_t written =
          ::write(descriptor, line.data() + offset, line.size() - offset);
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        throw std::runtime_error("写入诊断日志失败");
      }
      offset += static_cast<std::size_t>(written);
    }
  } catch (...) {
    (void)::close(descriptor);
    throw;
  }
  if (::close(descriptor) != 0) {
    throw std::runtime_error("关闭诊断日志失败");
  }
}
#endif

inline bool valid_trace_id(std::string_view trace_id) noexcept {
  if (trace_id.empty() || trace_id.size() > 128U) {
    return false;
  }
  const unsigned char first = static_cast<unsigned char>(trace_id.front());
  if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
        (first >= '0' && first <= '9'))) {
    return false;
  }
  for (const unsigned char character : trace_id) {
    if (!((character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '-' ||
          character == '_' || character == '.')) {
      return false;
    }
  }
  return true;
}

inline std::mutex &active_sessions_mutex() {
  static std::mutex mutex;
  return mutex;
}

inline std::unordered_set<std::string> &active_sessions() {
  static std::unordered_set<std::string> sessions;
  return sessions;
}

inline bool locally_active(std::string_view trace_id) {
  std::lock_guard lock(active_sessions_mutex());
  return active_sessions().contains(std::string(trace_id));
}

inline void ensure_private_directory(const std::filesystem::path &path) {
  if (path.empty()) {
    return;
  }
  std::filesystem::create_directories(path);
#if !defined(_WIN32)
  if (::chmod(path.c_str(), 0700) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "无法设置诊断目录权限");
  }
#endif
}

inline std::filesystem::path session_directory(
    const std::filesystem::path &log_path, std::string_view trace_id) {
  if (!valid_trace_id(trace_id)) {
    throw std::invalid_argument("诊断 trace_id 无效");
  }
  return log_path.parent_path() / "samples" / std::string(trace_id);
}

inline std::filesystem::path active_marker(
    const std::filesystem::path &directory) {
  return directory / ".active";
}

inline void write_active_marker(const std::filesystem::path &directory) {
  const std::filesystem::path marker = active_marker(directory);
  std::ofstream output(marker, std::ios::binary | std::ios::trunc);
  output.exceptions(std::ios::badbit | std::ios::failbit);
  output << detail::process_id() << '\n';
  output.close();
#if !defined(_WIN32)
  if (::chmod(marker.c_str(), 0600) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "无法设置诊断会话标记权限");
  }
#endif
}

inline bool externally_active(const std::filesystem::path &directory) {
  const std::filesystem::path marker = active_marker(directory);
  std::error_code error;
  if (!std::filesystem::is_regular_file(marker, error)) {
    if (missing_error(error)) {
      return false;
    }
    if (error) {
      throw std::system_error(error, "无法检查诊断会话状态");
    }
    return false;
  }
  std::ifstream input(marker, std::ios::binary);
  long long process = 0;
  input >> process;
  if (!input || process <= 0) {
    return true;
  }
#if defined(_WIN32)
  return true;
#else
  errno = 0;
  const int result = ::kill(static_cast<pid_t>(process), 0);
  if (result == 0 || errno == EPERM) {
    return true;
  }
  std::filesystem::remove(marker, error);
  if (error) {
    throw std::system_error(error, "无法清理过期诊断会话标记");
  }
  return false;
#endif
}

inline bool session_active(const std::filesystem::path &directory,
                           std::string_view trace_id) {
  return locally_active(trace_id) || externally_active(directory);
}

inline void remove_active_marker(const std::filesystem::path &directory) {
  std::error_code error;
  std::filesystem::remove(active_marker(directory), error);
  if (error) {
    throw std::system_error(error, "无法清理诊断会话标记");
  }
}

inline void remove_stale_temporary_files(
    const std::filesystem::path &samples) {
  std::error_code error;
  if (!std::filesystem::exists(samples, error)) {
    if (missing_error(error)) {
      return;
    }
    if (error) {
      throw std::system_error(error, "无法检查诊断会话目录");
    }
    return;
  }
  for (std::filesystem::recursive_directory_iterator iterator(samples, error),
       end;
       iterator != end; iterator.increment(error)) {
    if (error) {
      throw std::system_error(error, "无法遍历诊断会话目录");
    }
    const auto &entry = *iterator;
    if (!entry.is_regular_file(error)) {
      if (error) {
        throw std::system_error(error, "无法检查诊断临时文件");
      }
      continue;
    }
    if (entry.path().filename().string().starts_with(".tmp-")) {
      std::filesystem::remove(entry.path(), error);
      if (error) {
        throw std::system_error(error, "无法清理诊断临时文件");
      }
    }
  }
}

inline std::uintmax_t add_file_size(std::uintmax_t current,
                                    const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    if (error == std::make_error_code(std::errc::no_such_file_or_directory)) {
      return current;
    }
    if (error) {
      throw std::system_error(error, "无法检查诊断日志文件");
    }
    return current;
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    throw std::system_error(error, "无法读取诊断日志文件大小");
  }
  if (size > kRetentionBytes - std::min(current, kRetentionBytes)) {
    return kRetentionBytes;
  }
  return current + size;
}

inline std::uintmax_t retention_usage(const std::filesystem::path &log_path) {
  std::uintmax_t total = 0;
  total = add_file_size(total, log_path);
  total = add_file_size(total, log_path.string() + ".1");
  const std::filesystem::path samples = log_path.parent_path() / "samples";
  std::error_code error;
  if (!std::filesystem::exists(samples, error)) {
    if (missing_error(error)) {
      return total;
    }
    if (error) {
      throw std::system_error(error, "无法检查诊断会话目录");
    }
    return total;
  }
  for (std::filesystem::recursive_directory_iterator iterator(samples, error),
       end;
       iterator != end; iterator.increment(error)) {
    if (error) {
      throw std::system_error(error, "无法遍历诊断会话目录");
    }
    if (iterator->is_regular_file(error)) {
      if (error) {
        throw std::system_error(error, "无法检查诊断会话文件");
      }
      total = add_file_size(total, iterator->path());
    } else if (error) {
      throw std::system_error(error, "无法检查诊断会话文件");
    }
  }
  return total;
}

struct SessionCandidate {
  std::filesystem::path path;
  std::filesystem::file_time_type modified;
};

inline bool reserve_retention(const std::filesystem::path &log_path,
                              std::uintmax_t incoming,
                              std::string_view protected_trace_id = {}) {
  if (incoming > kRetentionBytes) {
    return false;
  }
  const std::filesystem::path samples = log_path.parent_path() / "samples";
  remove_stale_temporary_files(samples);
  std::uintmax_t total = retention_usage(log_path);
  if (total <= kRetentionBytes - incoming) {
    return true;
  }

  std::vector<SessionCandidate> candidates;
  std::error_code error;
  if (std::filesystem::exists(samples, error)) {
    if (error) {
      throw std::system_error(error, "无法检查诊断会话目录");
    }
    for (std::filesystem::directory_iterator iterator(samples, error), end;
         iterator != end; iterator.increment(error)) {
      if (error) {
        throw std::system_error(error, "无法遍历诊断会话目录");
      }
      if (!iterator->is_directory(error)) {
        if (error) {
          throw std::system_error(error, "无法检查诊断会话目录");
        }
        continue;
      }
      const std::string trace_id = iterator->path().filename().string();
      if (!valid_trace_id(trace_id) ||
          (!protected_trace_id.empty() && trace_id == protected_trace_id) ||
          session_active(iterator->path(), trace_id)) {
        continue;
      }
      auto modified = std::filesystem::last_write_time(iterator->path(),
                                                        error);
      if (error) {
        modified = std::filesystem::file_time_type::min();
        error.clear();
      }
      candidates.push_back({iterator->path(), modified});
    }
  } else if (error && !missing_error(error)) {
    throw std::system_error(error, "无法检查诊断会话目录");
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const SessionCandidate &left, const SessionCandidate &right) {
              return left.modified < right.modified;
            });
  for (const SessionCandidate &candidate : candidates) {
    std::filesystem::remove_all(candidate.path, error);
    if (error) {
      throw std::system_error(error, "无法清理旧诊断会话");
    }
    total = retention_usage(log_path);
    if (total <= kRetentionBytes - incoming) {
      return true;
    }
  }
  return total <= kRetentionBytes - incoming;
}

} // namespace diagnostic_detail

// 返回跨前端共享的诊断日志路径。路径本身不创建文件或目录。
[[nodiscard]] inline std::filesystem::path diagnostic_log_path() {
  if (const char *xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr &&
      *xdg != '\0') {
    return detail::expand_tilde(std::filesystem::path(xdg)) /
           "vocotype" / "transcription.jsonl";
  }
  const char *home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    throw std::runtime_error("HOME 未设置，无法确定诊断日志路径");
  }
  return std::filesystem::path(home) / ".local" / "state" / "vocotype" /
         "transcription.jsonl";
}

// 返回按 trace_id 分组保存的诊断录音根目录。函数本身不创建目录。
[[nodiscard]] inline std::filesystem::path diagnostic_samples_path() {
  return diagnostic_log_path().parent_path() / "samples";
}

// 读取诊断录音的 WAV 时长。录音格式由前端生成，但这里仍按 WAV 的
// fmt/data chunk 解析，避免用文件大小假定采样率；文件损坏或格式无法
// 解析时返回空值，调用方应省略该字段而不是写入一个伪造的 0。
[[nodiscard]] inline std::optional<double> diagnostic_audio_duration_ms(
    const std::filesystem::path &path) noexcept {
  try {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
      return std::nullopt;
    }
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size < 12U) {
      return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return std::nullopt;
    }
    const auto read_exact = [&input](char *buffer, std::streamsize count) {
      input.read(buffer, count);
      return input.good() && input.gcount() == count;
    };
    const auto little_endian_u16 = [](const char *bytes) {
      return static_cast<std::uint32_t>(
          static_cast<unsigned char>(bytes[0]) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1]))
           << 8U));
    };
    const auto little_endian_u32 = [](const char *bytes) {
      return static_cast<std::uint32_t>(
          static_cast<unsigned char>(bytes[0]) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1]))
           << 8U) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2]))
           << 16U) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3]))
           << 24U));
    };

    char header[12] = {};
    if (!read_exact(header, sizeof(header)) ||
        std::string_view(header, 4) != "RIFF" ||
        std::string_view(header + 8, 4) != "WAVE") {
      return std::nullopt;
    }

    std::uint32_t audio_format = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t block_align = 0;
    std::uint32_t data_bytes = 0;
    bool have_format = false;
    bool have_data = false;
    std::uintmax_t offset = 12U;
    while (offset + 8U <= size) {
      char chunk_header[8] = {};
      if (!read_exact(chunk_header, sizeof(chunk_header))) {
        return std::nullopt;
      }
      offset += 8U;
      const std::uint32_t chunk_size = little_endian_u32(chunk_header + 4);
      if (static_cast<std::uintmax_t>(chunk_size) > size - offset) {
        return std::nullopt;
      }
      const std::string_view chunk_id(chunk_header, 4);
      if (chunk_id == "fmt " && chunk_size >= 16U) {
        char format[16] = {};
        if (!read_exact(format, sizeof(format))) {
          return std::nullopt;
        }
        audio_format = little_endian_u16(format);
        sample_rate = little_endian_u32(format + 4);
        block_align = little_endian_u16(format + 12);
        have_format = (audio_format == 1U || audio_format == 3U) &&
                      sample_rate > 0U && block_align > 0U;
        if (chunk_size > 16U) {
          input.seekg(static_cast<std::streamoff>(chunk_size - 16U),
                      std::ios::cur);
          if (!input) {
            return std::nullopt;
          }
        }
      } else if (chunk_id == "data") {
        data_bytes = chunk_size;
        have_data = true;
        input.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
        if (!input) {
          return std::nullopt;
        }
      } else {
        input.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
        if (!input) {
          return std::nullopt;
        }
      }
      offset += chunk_size;
      if ((chunk_size & 1U) != 0U) {
        if (offset >= size) {
          return std::nullopt;
        }
        input.seekg(1, std::ios::cur);
        if (!input) {
          return std::nullopt;
        }
        ++offset;
      }
      if (have_format && have_data) {
        break;
      }
    }
    if (!have_format || !have_data) {
      return std::nullopt;
    }
    if (data_bytes % block_align != 0U) {
      return std::nullopt;
    }
    const double frames = static_cast<double>(data_bytes) /
                          static_cast<double>(block_align);
    return frames * 1000.0 / static_cast<double>(sample_rate);
  } catch (...) {
    return std::nullopt;
  }
}

inline void append_diagnostic_session_event(std::string_view trace_id,
                                            Json event) noexcept;

// 标记一个可能仍在运行的转录会话，预算清理时跳过该会话。
inline void begin_diagnostic_session(std::string_view trace_id) noexcept {
  try {
    if (!diagnostic_detail::valid_trace_id(trace_id)) {
      diagnostic_detail::report_failure("诊断 trace_id 无效");
      return;
    }
    std::lock_guard lock(diagnostic_detail::active_sessions_mutex());
    diagnostic_detail::active_sessions().insert(std::string(trace_id));
  } catch (const std::exception &error) {
    diagnostic_detail::report_failure(error.what());
  } catch (...) {
    diagnostic_detail::report_failure("未知错误");
  }
}

// 结束一个转录会话并删除跨进程清理所用的活动标记。
inline void end_diagnostic_session(std::string_view trace_id) noexcept {
  try {
    if (!diagnostic_detail::valid_trace_id(trace_id)) {
      return;
    }
    {
      std::lock_guard lock(diagnostic_detail::active_sessions_mutex());
      diagnostic_detail::active_sessions().erase(std::string(trace_id));
    }
    const std::filesystem::path path = diagnostic_log_path();
    const std::filesystem::path directory =
        diagnostic_detail::session_directory(path, trace_id);
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
      if (diagnostic_detail::missing_error(error)) {
        return;
      }
      if (error) {
        throw std::system_error(error, "无法检查诊断会话目录");
      }
      return;
    }
    diagnostic_detail::FileLock lock(path);
    diagnostic_detail::remove_active_marker(directory);
  } catch (const std::exception &error) {
    diagnostic_detail::report_failure(error.what());
  } catch (...) {
    diagnostic_detail::report_failure("未知错误");
  }
}

// 按当前 profile 快照的 diagnostics.enabled 写入一条 JSONL 事件。所有
// 错误仅报告到 stderr，不能影响语音输入主流程。
inline void append_diagnostic_event(Json event) noexcept {
  try {
    if (!diagnostic_detail::diagnostics_enabled()) {
      return;
    }

    const std::filesystem::path path = diagnostic_log_path();
    const std::filesystem::path parent = path.parent_path();
    diagnostic_detail::ensure_private_directory(parent);
    if (!event.is_object()) {
      Json value = std::move(event);
      event = Json{{"event", "diagnostic"}, {"value", std::move(value)}};
    }
    const std::string trace_id = event.value("trace_id", std::string());
    event["timestamp"] = diagnostic_detail::timestamp_now();
    const std::string line = event.dump() + '\n';
    if (line.size() > diagnostic_detail::kMaxLogBytes) {
      diagnostic_detail::report_failure("单条记录超过 10 MiB，已丢弃");
      return;
    }
    {
      diagnostic_detail::FileLock lock(path);
      const std::uintmax_t size = diagnostic_detail::current_size(path);
      if (size > diagnostic_detail::kMaxLogBytes ||
          line.size() > diagnostic_detail::kMaxLogBytes - size) {
        diagnostic_detail::rotate(path);
      }
      if (!diagnostic_detail::reserve_retention(path, line.size())) {
        diagnostic_detail::report_failure("总预算不足，已跳过记录");
        return;
      }
      diagnostic_detail::append_bytes(path, line);
    }
    if (diagnostic_detail::valid_trace_id(trace_id)) {
      append_diagnostic_session_event(trace_id, std::move(event));
    }
  } catch (const std::exception &error) {
    diagnostic_detail::report_failure(error.what());
  } catch (...) {
    diagnostic_detail::report_failure("未知错误");
  }
}

// 将一条阶段事件写入指定会话的 events.jsonl。会话事件与顶层事件共用
// 稳定锁和总预算，但不会重复写入顶层查询日志。
inline void append_diagnostic_session_event(std::string_view trace_id,
                                            Json event) noexcept {
  try {
    if (!diagnostic_detail::diagnostics_enabled()) {
      return;
    }
    const std::filesystem::path log_path = diagnostic_log_path();
    const std::filesystem::path parent = log_path.parent_path();
    diagnostic_detail::ensure_private_directory(parent);
    diagnostic_detail::FileLock lock(log_path);
    const std::filesystem::path samples = parent / "samples";
    const std::filesystem::path directory =
        diagnostic_detail::session_directory(log_path, trace_id);
    const bool locally_running = diagnostic_detail::locally_active(trace_id);
    std::error_code directory_error;
    if (!std::filesystem::is_directory(directory, directory_error) &&
        diagnostic_detail::missing_error(directory_error) &&
        !locally_running) {
      // 没有已知会话目录时不为迟到的跨进程事件重新制造孤儿会话。
      return;
    }
    if (directory_error &&
        !diagnostic_detail::missing_error(directory_error)) {
      throw std::system_error(directory_error, "无法检查诊断会话目录");
    }
    diagnostic_detail::ensure_private_directory(samples);
    diagnostic_detail::ensure_private_directory(directory);
    if (locally_running) {
      diagnostic_detail::write_active_marker(directory);
    }
    if (!event.is_object()) {
      Json value = std::move(event);
      event = Json{{"event", "diagnostic"}, {"value", std::move(value)}};
    }
    event["timestamp"] = diagnostic_detail::timestamp_now();
    const std::string line = event.dump() + '\n';
    if (line.size() > diagnostic_detail::kMaxLogBytes) {
      diagnostic_detail::report_failure("会话单条记录超过 10 MiB，已丢弃");
      return;
    }
    if (!diagnostic_detail::reserve_retention(log_path, line.size(),
                                              trace_id)) {
      diagnostic_detail::report_failure("总预算不足，已跳过会话记录");
      return;
    }
    diagnostic_detail::append_bytes(directory / "events.jsonl", line);
  } catch (const std::exception &error) {
    diagnostic_detail::report_failure(error.what());
  } catch (...) {
    diagnostic_detail::report_failure("未知错误");
  }
}

// 在原始录音按旧逻辑清理前保存一份不可变 WAV 副本。复制使用临时文件
// 和 rename，任何失败都只报告到 stderr 并返回 false。
inline bool save_diagnostic_audio(const std::filesystem::path &source,
                                  std::string_view trace_id) noexcept {
  try {
    if (!diagnostic_detail::diagnostics_enabled()) {
      return false;
    }
    if (!diagnostic_detail::valid_trace_id(trace_id)) {
      diagnostic_detail::report_failure("诊断 trace_id 无效");
      return false;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error)) {
      if (error && !diagnostic_detail::missing_error(error)) {
        diagnostic_detail::report_failure("无法检查原始录音");
      }
      return false;
    }
    const std::filesystem::path log_path = diagnostic_log_path();
    const std::filesystem::path parent = log_path.parent_path();
    diagnostic_detail::ensure_private_directory(parent);
    diagnostic_detail::FileLock lock(log_path);
    const std::filesystem::path samples = parent / "samples";
    const std::filesystem::path directory =
        diagnostic_detail::session_directory(log_path, trace_id);
    diagnostic_detail::ensure_private_directory(samples);
    diagnostic_detail::ensure_private_directory(directory);
    if (diagnostic_detail::locally_active(trace_id)) {
      diagnostic_detail::write_active_marker(directory);
    }

    const std::filesystem::path target = directory / "audio.wav";
    if (std::filesystem::is_regular_file(target, error)) {
      diagnostic_detail::set_private_mode(target);
      return true;
    }
    if (diagnostic_detail::missing_error(error)) {
      error.clear();
    }
    if (error) {
      throw std::system_error(error, "无法检查诊断录音副本");
    }
    const std::uintmax_t source_size =
        std::filesystem::file_size(source, error);
    if (error) {
      throw std::system_error(error, "无法读取原始录音大小");
    }
    if (!diagnostic_detail::reserve_retention(log_path, source_size,
                                              trace_id)) {
      diagnostic_detail::report_failure("总预算不足，已跳过录音保存");
      return false;
    }
    const std::uintmax_t base_usage =
        diagnostic_detail::retention_usage(log_path);
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path temporary =
        directory / (".tmp-audio-" + std::to_string(detail::process_id()) +
                     "-" + std::to_string(stamp));
    try {
      std::ifstream input(source, std::ios::binary);
      input.exceptions(std::ios::badbit);
      if (!input) {
        throw std::runtime_error("无法打开原始录音");
      }
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      output.exceptions(std::ios::badbit | std::ios::failbit);
      std::array<char, 1024U * 1024U> buffer{};
      std::uintmax_t copied = 0;
      while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        if (read <= 0) {
          break;
        }
        const std::uintmax_t bytes = static_cast<std::uintmax_t>(read);
        if (copied > diagnostic_detail::kRetentionBytes - base_usage ||
            bytes > diagnostic_detail::kRetentionBytes - base_usage -
                          copied) {
          throw std::runtime_error("录音保存超出总预算");
        }
        output.write(buffer.data(), read);
        copied += bytes;
      }
      output.close();
      if (input.bad()) {
        throw std::runtime_error("读取原始录音失败");
      }
      diagnostic_detail::set_private_mode(temporary);
      std::filesystem::rename(temporary, target, error);
      if (error) {
        throw std::system_error(error, "无法提交诊断录音副本");
      }
      diagnostic_detail::set_private_mode(target);
    } catch (...) {
      std::error_code cleanup_error;
      std::filesystem::remove(temporary, cleanup_error);
      throw;
    }
    return true;
  } catch (const std::exception &error) {
    diagnostic_detail::report_failure(error.what());
    return false;
  } catch (...) {
    diagnostic_detail::report_failure("未知错误");
    return false;
  }
}

} // namespace vocotype::common
