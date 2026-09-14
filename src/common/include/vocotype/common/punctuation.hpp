#pragma once

#include <string>
#include <utility>

namespace vocotype::common {

// 在最终提交边界转换，避免模型后处理重新引入中文标点。
inline std::string english_punctuation(std::string text) {
  static const std::pair<const char *, const char *> replacements[] = {
      {"——", " — "}, {"……", "..."},
      {"“", "\""}, {"”", "\""}, {"‘", "'"}, {"’", "'"},
      {"「", "\""}, {"」", "\""}, {"『", "\""}, {"』", "\""},
      {"（", "("}, {"）", ")"}, {"【", "["}, {"】", "]"},
      {"《", "<"}, {"》", ">"}, {"〔", "("}, {"〕", ")"},
      {"〖", "["}, {"〗", "]"}, {"、", ", "}, {"·", " · "}, {"～", "~"},
      {"。", ". "}, {"！", "! "}, {"？", "? "},
      {"，", ", "}, {"；", "; "}, {"：", ": "},
  };
  for (const auto &[from, to] : replacements) {
    const std::string source(from), target(to);
    std::size_t pos = 0;
    while ((pos = text.find(source, pos)) != std::string::npos) {
      text.replace(pos, source.size(), target);
      pos += target.size();
    }
  }
  return text;
}

} // namespace vocotype::common
