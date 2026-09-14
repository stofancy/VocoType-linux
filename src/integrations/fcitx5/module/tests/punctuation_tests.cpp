#include "vocotype/common/punctuation.hpp"

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
}
