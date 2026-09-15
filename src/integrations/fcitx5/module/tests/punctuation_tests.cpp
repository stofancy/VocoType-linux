#include "vocotype/common/punctuation.hpp"
#include "vocotype/common/spacing.hpp"

#include <iostream>
#include <string>

int main() {
  using vocotype::common::english_punctuation;
  const std::pair<std::string, std::string> cases[] = {
      {"你好，世界。", "你好, 世界. "},
      {"“路径”（【测试】）：《文件》……", "\"路径\"([测试]): <文件>..."},
      {"版本v5.0.8，运行 foo --x=a/b_c。", "版本v5.0.8, 运行 foo --x=a/b_c. "},
      {"甲——乙、丙；丁？好！", "甲 — 乙, 丙; 丁? 好! "},
      {"plain ASCII: foo/bar_v2.3 --flag", "plain ASCII: foo/bar_v2.3 --flag"},
      {"", ""},
  };
  for (const auto &[input, expected] : cases) {
    if (english_punctuation(input) != expected) {
      std::cerr << "标点转换结果不符合预期：" << input << '\n';
      return 1;
    }
  }
  using vocotype::common::space_between_cjk_and_ascii;
  const std::pair<std::string, std::string> spacing_cases[] = {
      {"使用Claude Code处理2026年数据", "使用 Claude Code 处理 2026 年数据"},
      {"版本v5.0.8和320m路程", "版本 v5.0.8 和 320m 路程"},
      {"纯中文，不处理。", "纯中文，不处理。"},
      {"plain ASCII 2026", "plain ASCII 2026"},
  };
  for (const auto &[input, expected] : spacing_cases) {
    if (space_between_cjk_and_ascii(input) != expected) {
      std::cerr << "中英文数字间距结果不符合预期：" << input << '\n';
      return 1;
    }
  }
}
