#pragma once

#include "vocotype/common/slm_profiles.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vocotype::common {

namespace diagnostic_detail {

inline constexpr std::uintmax_t kMaxLogBytes = 5U * 1024U * 1024U;

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

// 按当前 profile 快照的 diagnostics.enabled 写入一条 JSONL 事件。所有
// 错误仅报告到 stderr，不能影响语音输入主流程。
inline void append_diagnostic_event(Json event) noexcept {
  try {
    if (!diagnostic_detail::diagnostics_enabled()) {
      return;
    }

    const std::filesystem::path path = diagnostic_log_path();
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    diagnostic_detail::FileLock lock(path);

    if (!event.is_object()) {
      Json value = std::move(event);
      event = Json{{"event", "diagnostic"}, {"value", std::move(value)}};
    }
    event["timestamp"] = diagnostic_detail::timestamp_now();
    const std::string line = event.dump() + '\n';
    if (line.size() > diagnostic_detail::kMaxLogBytes) {
      diagnostic_detail::report_failure("单条记录超过 5 MiB，已丢弃");
      return;
    }

    const std::uintmax_t size = diagnostic_detail::current_size(path);
    if (size > diagnostic_detail::kMaxLogBytes ||
        line.size() > diagnostic_detail::kMaxLogBytes - size) {
      diagnostic_detail::rotate(path);
    }
    diagnostic_detail::append_bytes(path, line);
  } catch (const std::exception &error) {
    diagnostic_detail::report_failure(error.what());
  } catch (...) {
    diagnostic_detail::report_failure("未知错误");
  }
}

} // namespace vocotype::common

