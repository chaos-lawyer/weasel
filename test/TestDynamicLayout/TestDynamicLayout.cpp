#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <cassert>
#if __has_include(<boost/detail/lightweight_test.hpp>)
#include <boost/detail/lightweight_test.hpp>
#else
static int g_errors = 0;
#define BOOST_TEST(expr)                                                      \
  do {                                                                        \
    if (!(expr)) {                                                            \
      std::cerr << "FAIL: " << #expr << " at " << __FILE__ << ":" << __LINE__ \
                << std::endl;                                                 \
      ++g_errors;                                                             \
    }                                                                         \
  } while (0)
namespace boost {
inline int report_errors() {
  return g_errors;
}
}  // namespace boost
#endif
#include <DynamicCandidateLayout.h>

using namespace weasel;

// 1. 兼容性测试
// 当 dynamic_layout 未启用时，应始终保持原静态 default_fallback 布局
void test_compatibility() {
  DynamicLayoutConfig config;
  config.enabled = false;
  config.default_layout = CandidateLayout::Horizontal;

  CandidateInfo cinfo;
  auto no_option = [](const std::string&) { return false; };

  // 模拟静态 horizontal=true (LAYOUT_HORIZONTAL)
  CandidateLayout layout_h = ResolveCandidateLayout(
      cinfo, CandidateLayout::Horizontal, config, no_option);
  BOOST_TEST(layout_h == CandidateLayout::Horizontal);

  // 模拟静态 horizontal=false (LAYOUT_VERTICAL)
  CandidateLayout layout_v = ResolveCandidateLayout(
      cinfo, CandidateLayout::Vertical, config, no_option);
  BOOST_TEST(layout_v == CandidateLayout::Vertical);
}

// 2. Auto 默认值测试
// 启用了 type: auto，但没有规则命中时，应使用 dynamic_layout.default
void test_auto_default() {
  DynamicLayoutConfig config;
  config.enabled = true;
  config.default_layout = CandidateLayout::Horizontal;

  CandidateInfo cinfo;
  auto no_option = [](const std::string&) { return false; };

  CandidateLayout res1 = ResolveCandidateLayout(
      cinfo, CandidateLayout::Horizontal, config, no_option);
  BOOST_TEST(res1 == CandidateLayout::Horizontal);

  config.default_layout = CandidateLayout::Vertical;
  CandidateLayout res2 = ResolveCandidateLayout(
      cinfo, CandidateLayout::Vertical, config, no_option);
  BOOST_TEST(res2 == CandidateLayout::Vertical);
}

// 3. Option 规则测试
// 测试 vertical_layout=true/false
void test_option_rules() {
  DynamicLayoutConfig config;
  config.enabled = true;
  config.default_layout = CandidateLayout::Horizontal;

  DynamicLayoutRule rule;
  rule.type = LayoutRuleType::Option;
  rule.option_name = "vertical_layout";
  rule.option_value = true;
  rule.target_layout = CandidateLayout::Vertical;
  config.rules.push_back(rule);

  CandidateInfo cinfo;

  // option 为 false 时 -> default (Horizontal)
  std::map<std::string, bool> opts;
  opts["vertical_layout"] = false;
  auto getter = [&](const std::string& name) {
    auto it = opts.find(name);
    return (it != opts.end()) ? it->second : false;
  };

  CandidateLayout res_false = ResolveCandidateLayout(
      cinfo, CandidateLayout::Horizontal, config, getter);
  BOOST_TEST(res_false == CandidateLayout::Horizontal);

  // option 为 true 时 -> Vertical
  opts["vertical_layout"] = true;
  CandidateLayout res_true = ResolveCandidateLayout(
      cinfo, CandidateLayout::Horizontal, config, getter);
  BOOST_TEST(res_true == CandidateLayout::Vertical);
}

// 4. 动态变化序列测试
// 连续模拟 false -> true -> false，确保布局状态往返平滑且准确
void test_dynamic_sequence() {
  DynamicLayoutConfig config;
  config.enabled = true;
  config.default_layout = CandidateLayout::Horizontal;

  DynamicLayoutRule rule;
  rule.type = LayoutRuleType::Option;
  rule.option_name = "vertical_layout";
  rule.option_value = true;
  rule.target_layout = CandidateLayout::Vertical;
  config.rules.push_back(rule);

  CandidateInfo cinfo;
  bool is_vertical = false;
  auto getter = [&](const std::string& name) {
    if (name == "vertical_layout")
      return is_vertical;
    return false;
  };

  // Step 1: 普通输入 -> Horizontal
  is_vertical = false;
  BOOST_TEST(ResolveCandidateLayout(cinfo, CandidateLayout::Horizontal, config,
                                    getter) == CandidateLayout::Horizontal);

  // Step 2: 进入特定模式 (vertical_layout = true) -> Vertical
  is_vertical = true;
  BOOST_TEST(ResolveCandidateLayout(cinfo, CandidateLayout::Horizontal, config,
                                    getter) == CandidateLayout::Vertical);

  // Step 3: 退出特定模式 (vertical_layout = false) -> 恢复 Horizontal
  is_vertical = false;
  BOOST_TEST(ResolveCandidateLayout(cinfo, CandidateLayout::Horizontal, config,
                                    getter) == CandidateLayout::Horizontal);
}

// 5. 候选词文本长度规则测试
// 包含 Unicode 字符长度、中文及 Emoji 代理对
void test_candidate_length_rules() {
  DynamicLayoutConfig config;
  config.enabled = true;
  config.default_layout = CandidateLayout::Horizontal;

  DynamicLayoutRule rule;
  rule.type = LayoutRuleType::CandidateMaxTextLengthGt;
  rule.threshold = 10;
  rule.target_layout = CandidateLayout::Vertical;
  config.rules.push_back(rule);

  auto no_option = [](const std::string&) { return false; };

  // Unicode 长度计算单元测试（代理对测试：例如 𠮷 为 surrogate pair，长度应算 1
  // 个字符）
  std::wstring emoji = L"😊";  // High surrogate 0xD83D, Low surrogate 0xDE0A
  BOOST_TEST(CalculateUnicodeLength(emoji) == 1);
  std::wstring mixed = L"你好😊世界";  // 2 + 1 + 2 = 5
  BOOST_TEST(CalculateUnicodeLength(mixed) == 5);

  // 候选词最大长度 <= 10 -> Horizontal
  CandidateInfo cinfo1;
  cinfo1.candies.push_back(Text(L"短候选词"));
  cinfo1.candies.push_back(Text(L"一二三四五六七八九十"));  // 10 chars
  BOOST_TEST(ResolveCandidateLayout(cinfo1, CandidateLayout::Horizontal, config,
                                    no_option) == CandidateLayout::Horizontal);

  // 候选词最大长度 > 10 (11 chars) -> Vertical
  CandidateInfo cinfo2;
  cinfo2.candies.push_back(
      Text(L"这是一个非常非常长的候选词文本测试"));  // > 10 chars
  BOOST_TEST(ResolveCandidateLayout(cinfo2, CandidateLayout::Horizontal, config,
                                    no_option) == CandidateLayout::Vertical);
}

// 6. 候选词数量规则测试
// 基于当前页实际可见候选数量
void test_candidate_count_rules() {
  DynamicLayoutConfig config;
  config.enabled = true;
  config.default_layout = CandidateLayout::Horizontal;

  DynamicLayoutRule rule;
  rule.type = LayoutRuleType::CandidateCountGt;
  rule.threshold = 5;
  rule.target_layout = CandidateLayout::Vertical;
  config.rules.push_back(rule);

  auto no_option = [](const std::string&) { return false; };

  // 候选数量 5 <= 5 -> Horizontal
  CandidateInfo cinfo1;
  for (int i = 0; i < 5; ++i) {
    cinfo1.candies.push_back(Text(std::to_wstring(i)));
  }
  BOOST_TEST(ResolveCandidateLayout(cinfo1, CandidateLayout::Horizontal, config,
                                    no_option) == CandidateLayout::Horizontal);

  // 候选数量 6 > 5 -> Vertical
  CandidateInfo cinfo2;
  for (int i = 0; i < 6; ++i) {
    cinfo2.candies.push_back(Text(std::to_wstring(i)));
  }
  BOOST_TEST(ResolveCandidateLayout(cinfo2, CandidateLayout::Horizontal, config,
                                    no_option) == CandidateLayout::Vertical);
}

// 7. 规则优先级测试 (First match wins)
void test_rule_priority() {
  DynamicLayoutConfig config;
  config.enabled = true;
  config.default_layout = CandidateLayout::Vertical;

  // 规则 1: emoji_mode 为 true 时强制 horizontal
  DynamicLayoutRule rule1;
  rule1.type = LayoutRuleType::Option;
  rule1.option_name = "emoji_mode";
  rule1.option_value = true;
  rule1.target_layout = CandidateLayout::Horizontal;
  config.rules.push_back(rule1);

  // 规则 2: 候选长度 > 5 时 vertical
  DynamicLayoutRule rule2;
  rule2.type = LayoutRuleType::CandidateMaxTextLengthGt;
  rule2.threshold = 5;
  rule2.target_layout = CandidateLayout::Vertical;
  config.rules.push_back(rule2);

  CandidateInfo cinfo;
  cinfo.candies.push_back(Text(L"长文本长文本长文本"));  // length > 5

  // 当 emoji_mode = true 时，规则 1 优先命中，保持 Horizontal
  auto emoji_on = [](const std::string& opt) { return opt == "emoji_mode"; };
  BOOST_TEST(ResolveCandidateLayout(cinfo, CandidateLayout::Vertical, config,
                                    emoji_on) == CandidateLayout::Horizontal);

  // 当 emoji_mode = false 时，规则 1 不命中，规则 2 命中，变为 Vertical
  auto emoji_off = [](const std::string&) { return false; };
  BOOST_TEST(ResolveCandidateLayout(cinfo, CandidateLayout::Vertical, config,
                                    emoji_off) == CandidateLayout::Vertical);
}

// 8. 错误配置与极端情况容错测试
void test_invalid_config_resilience() {
  DynamicLayoutConfig config;
  config.enabled = true;
  config.default_layout = CandidateLayout::Horizontal;

  // 负阈值、无效类型等不合规规则不应导致崩溃
  DynamicLayoutRule invalid_rule1;
  invalid_rule1.type = LayoutRuleType::CandidateMaxTextLengthGt;
  invalid_rule1.threshold = -1;
  invalid_rule1.target_layout = CandidateLayout::Vertical;
  config.rules.push_back(invalid_rule1);

  DynamicLayoutRule invalid_rule2;
  invalid_rule2.type = LayoutRuleType::Invalid;
  config.rules.push_back(invalid_rule2);

  CandidateInfo empty_cinfo;
  auto no_option = [](const std::string&) { return false; };

  CandidateLayout res = ResolveCandidateLayout(
      empty_cinfo, CandidateLayout::Horizontal, config, no_option);
  BOOST_TEST(res == CandidateLayout::Horizontal);
}

int main() {
  test_compatibility();
  test_auto_default();
  test_option_rules();
  test_dynamic_sequence();
  test_candidate_length_rules();
  test_candidate_count_rules();
  test_rule_priority();
  test_invalid_config_resilience();

  std::cout << "All DynamicCandidateLayout tests passed successfully!"
            << std::endl;
  return boost::report_errors();
}
