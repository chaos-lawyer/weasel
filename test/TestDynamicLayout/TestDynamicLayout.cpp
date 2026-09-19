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
#include <DynamicCandidateSelectKeys.h>

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

// 5. Schema 布局继承测试
// schema 未配置布局时必须保留全局 AUTO，而不是继承当前解析出的横排默认值。
void test_configured_layout_inheritance() {
  BOOST_TEST(ResolveConfiguredLayoutType(UIStyle::LAYOUT_AUTO,
                                         UIStyle::LAYOUT_HORIZONTAL,
                                         false) == UIStyle::LAYOUT_AUTO);

  // schema 显式配置布局时，应以 schema 为准。
  BOOST_TEST(ResolveConfiguredLayoutType(UIStyle::LAYOUT_AUTO,
                                         UIStyle::LAYOUT_VERTICAL,
                                         true) == UIStyle::LAYOUT_VERTICAL);
}

// 6. 候选导航随布局切换
void test_layout_aware_candidate_navigation() {
  CandidateNavigationConfig config;
  config.enabled = true;
  config.horizontal.previous_candidate = {1, 10, true};
  config.horizontal.next_candidate = {2, 10, true};
  config.horizontal.previous_page = {3, 0, true};
  config.horizontal.next_page = {4, 0, true};
  config.vertical.previous_candidate = {3, 0, true};
  config.vertical.next_candidate = {4, 0, true};
  config.vertical.previous_page = {1, 10, true};
  config.vertical.next_page = {2, 10, true};

  BOOST_TEST(
      ResolveCandidateNavigation(CandidateLayout::Horizontal, config, 3, 0) ==
      CandidateNavigationAction::PreviousPage);
  BOOST_TEST(
      ResolveCandidateNavigation(CandidateLayout::Horizontal, config, 4, 0) ==
      CandidateNavigationAction::NextPage);
  BOOST_TEST(
      ResolveCandidateNavigation(CandidateLayout::Horizontal, config, 1, 10) ==
      CandidateNavigationAction::PreviousCandidate);
  BOOST_TEST(
      ResolveCandidateNavigation(CandidateLayout::Horizontal, config, 2, 10) ==
      CandidateNavigationAction::NextCandidate);

  BOOST_TEST(
      ResolveCandidateNavigation(CandidateLayout::Vertical, config, 3, 0) ==
      CandidateNavigationAction::PreviousCandidate);
  BOOST_TEST(
      ResolveCandidateNavigation(CandidateLayout::Vertical, config, 4, 0) ==
      CandidateNavigationAction::NextCandidate);
  BOOST_TEST(
      ResolveCandidateNavigation(CandidateLayout::Vertical, config, 1, 10) ==
      CandidateNavigationAction::PreviousPage);
  BOOST_TEST(
      ResolveCandidateNavigation(CandidateLayout::Vertical, config, 2, 10) ==
      CandidateNavigationAction::NextPage);

  config.enabled = false;
  BOOST_TEST(
      ResolveCandidateNavigation(CandidateLayout::Horizontal, config, 3, 0) ==
      CandidateNavigationAction::PassThrough);
}

// 7. 候选词文本长度规则测试
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

// 8. 候选词数量规则测试
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

// 9. 规则优先级测试 (First match wins)
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

// 10. 错误配置与极端情况容错测试
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

// 11. 动态选择键与标签测试
void test_dynamic_candidate_select_keys_and_labels() {
  std::vector<std::wstring> empty_labels;
  std::vector<std::wstring> empty_keys;

  // 11.1 原生回退测试：无任何设置时，默认返回 1, 2, 3... 0
  BOOST_TEST(FormatCandidateLabel(0, empty_labels, empty_keys, nullptr,
                                  nullptr) == L"1");
  BOOST_TEST(FormatCandidateLabel(8, empty_labels, empty_keys, nullptr,
                                  nullptr) == L"9");
  BOOST_TEST(FormatCandidateLabel(9, empty_labels, empty_keys, nullptr,
                                  nullptr) == L"0");

  // 11.2 Schema 优先级测试：ctx.select_labels 和 ctx.menu.select_keys
  const char* schema_labels[] = {"一", "二", "三"};
  BOOST_TEST(FormatCandidateLabel(0, empty_labels, empty_keys, schema_labels,
                                  nullptr) == L"一");
  BOOST_TEST(FormatCandidateLabel(1, empty_labels, empty_keys, schema_labels,
                                  nullptr) == L"二");

  const char* schema_keys = "asdfg";
  BOOST_TEST(FormatCandidateLabel(0, empty_labels, empty_keys, nullptr,
                                  schema_keys) == L"a");
  BOOST_TEST(FormatCandidateLabel(2, empty_labels, empty_keys, nullptr,
                                  schema_keys) == L"d");

  // 11.3 运行时 candidate_select_keys: 优先级高于 schema
  auto runtime_keys = ParseSelectKeys("abcde");
  BOOST_TEST(runtime_keys.size() == 5);
  BOOST_TEST(FormatCandidateLabel(0, empty_labels, runtime_keys, schema_labels,
                                  schema_keys) == L"a");
  BOOST_TEST(FormatCandidateLabel(4, empty_labels, runtime_keys, schema_labels,
                                  schema_keys) == L"e");
  // 超出 key 范围返回空串
  BOOST_TEST(FormatCandidateLabel(5, empty_labels, runtime_keys, schema_labels,
                                  schema_keys) == L"");

  // 11.4 运行时 candidate_select_labels: 优先级最高
  auto runtime_labels = ParseSelectLabels("① ② ③ ④ ⑤");
  BOOST_TEST(runtime_labels.size() == 5);
  BOOST_TEST(FormatCandidateLabel(0, runtime_labels, runtime_keys,
                                  schema_labels, schema_keys) == L"①");
  BOOST_TEST(FormatCandidateLabel(1, runtime_labels, runtime_keys,
                                  schema_labels, schema_keys) == L"②");

  // 无空格紧凑格式 candidate_select_labels
  auto runtime_labels_compact = ParseSelectLabels("甲乙丙");
  BOOST_TEST(runtime_labels_compact.size() == 3);
  BOOST_TEST(runtime_labels_compact[0] == L"甲");
  BOOST_TEST(runtime_labels_compact[1] == L"乙");
  BOOST_TEST(runtime_labels_compact[2] == L"丙");

  // 11.5 按键解析与选词命中测试
  size_t selected_idx = 999;
  // 'a' 选中候选 0
  auto act_a =
      ResolveDynamicCandidateSelection('a', 0, runtime_keys, 5, selected_idx);
  BOOST_TEST(act_a == DynamicCandidateSelectAction::SelectCandidate);
  BOOST_TEST(selected_idx == 0);

  // 'c' 选中候选 2
  auto act_c =
      ResolveDynamicCandidateSelection('c', 0, runtime_keys, 5, selected_idx);
  BOOST_TEST(act_c == DynamicCandidateSelectAction::SelectCandidate);
  BOOST_TEST(selected_idx == 2);

  // 大小写敏感测试：'A' 不匹配 "abcde"
  auto act_cap_a =
      ResolveDynamicCandidateSelection('A', 0, runtime_keys, 5, selected_idx);
  BOOST_TEST(act_cap_a == DynamicCandidateSelectAction::PassThrough);

  // 非字母符号按键测试："asdfghjkl;"
  auto symbol_keys = ParseSelectKeys("asdfghjkl;");
  BOOST_TEST(symbol_keys.size() == 10);
  auto act_semi =
      ResolveDynamicCandidateSelection(';', 0, symbol_keys, 10, selected_idx);
  BOOST_TEST(act_semi == DynamicCandidateSelectAction::SelectCandidate);
  BOOST_TEST(selected_idx == 9);

  // 11.6 越界安全防护测试：当前页仅 2 个候选，按下 'c' (index 2)
  // 应吞掉而非崩溃或越界
  auto act_oob =
      ResolveDynamicCandidateSelection('c', 0, runtime_keys, 2, selected_idx);
  BOOST_TEST(act_oob == DynamicCandidateSelectAction::Swallow);

  // 11.7 修饰键放行测试：Ctrl+a, Alt+a 必须放行，不作为选词
  auto act_ctrl = ResolveDynamicCandidateSelection(
      'a', select_keys_ibus::CONTROL_MASK, runtime_keys, 5, selected_idx);
  BOOST_TEST(act_ctrl == DynamicCandidateSelectAction::PassThrough);

  auto act_alt = ResolveDynamicCandidateSelection(
      'a', select_keys_ibus::ALT_MASK, runtime_keys, 5, selected_idx);
  BOOST_TEST(act_alt == DynamicCandidateSelectAction::PassThrough);

  // 11.8 KeyRelease 吞键测试：选词键的按键弹起予以消费，避免送入宿主应用
  auto act_release = ResolveDynamicCandidateSelection(
      'a', select_keys_ibus::RELEASE_MASK, runtime_keys, 5, selected_idx);
  BOOST_TEST(act_release == DynamicCandidateSelectAction::Swallow);

  // 11.9 未配置动态选择键时的放行测试
  auto act_empty =
      ResolveDynamicCandidateSelection('a', 0, empty_keys, 5, selected_idx);
  BOOST_TEST(act_empty == DynamicCandidateSelectAction::PassThrough);
}

int main() {
  test_compatibility();
  test_auto_default();
  test_option_rules();
  test_dynamic_sequence();
  test_configured_layout_inheritance();
  test_layout_aware_candidate_navigation();
  test_candidate_length_rules();
  test_candidate_count_rules();
  test_rule_priority();
  test_invalid_config_resilience();
  test_dynamic_candidate_select_keys_and_labels();

  std::cout << "All DynamicCandidateLayout and DynamicCandidateSelectKeys "
               "tests passed successfully!"
            << std::endl;
  return boost::report_errors();
}
