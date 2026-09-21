#include <iostream>
#include <string>
#include <vector>
#include <cassert>
#include <sstream>

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
#define BOOST_TEST_EQ(a, b)                                                    \
  do {                                                                         \
    if (!((a) == (b))) {                                                       \
      std::cerr << "FAIL: " << #a << " == " << #b << " (" << (a) << " vs "     \
                << (b) << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
      ++g_errors;                                                              \
    }                                                                          \
  } while (0)
namespace boost {
inline int report_errors() {
  return g_errors;
}
}  // namespace boost
#endif

#include <CandidateDetailPanel.h>
#include <WeaselIPCData.h>

using namespace weasel;

// 1. 默认右侧定位测试：右侧空间充裕时，放置在候选窗口右侧并保持与顶部对齐
void test_position_right_normal() {
  DetailPanelRect candidate_win = {500, 300, 700, 600};  // w=200, h=300
  DetailPanelRect work_area = {0, 0, 1920, 1080};
  DetailPanelGeometryConfig config;
  config.preferred_position = DetailPanelPosition::Right;
  config.gap = 10;

  int detail_w = 320;
  int detail_h = 240;

  DetailPanelRect pos = CalculateDetailPanelPosition(
      candidate_win, work_area, detail_w, detail_h, config);

  // x 应为 candidate_win.right + gap = 700 + 10 = 710
  BOOST_TEST_EQ(pos.left, 710);
  BOOST_TEST_EQ(pos.top, 300);
  BOOST_TEST_EQ(pos.right, 710 + 320);
  BOOST_TEST_EQ(pos.bottom, 300 + 240);
}

// 2. 屏幕右边缘翻转测试：右侧空间不足时，自动翻转至候选窗口左侧
void test_position_right_flip_to_left() {
  // 候选窗口靠屏幕最右侧
  DetailPanelRect candidate_win = {1700, 300, 1900, 600};  // w=200, right=1900
  DetailPanelRect work_area = {
      0, 0, 1920, 1080};  // right space = 1920 - (1900 + 10) = 10 < 320
  DetailPanelGeometryConfig config;
  config.preferred_position = DetailPanelPosition::Right;
  config.gap = 10;

  int detail_w = 320;
  int detail_h = 240;

  DetailPanelRect pos = CalculateDetailPanelPosition(
      candidate_win, work_area, detail_w, detail_h, config);

  // 翻转到左侧：x = candidate_win.left - gap - detail_w = 1700 - 10 - 320 =
  // 1370
  BOOST_TEST_EQ(pos.left, 1370);
  BOOST_TEST_EQ(pos.top, 300);
  BOOST_TEST_EQ(pos.right, 1370 + 320);
  BOOST_TEST_EQ(pos.bottom, 300 + 240);
}

// 3. 屏幕左右两侧均受限时的安全工作区 Clamp 测试
void test_position_both_sides_constrained() {
  // 超小工作区或窗口过宽
  DetailPanelRect candidate_win = {100, 200, 600, 500};
  DetailPanelRect work_area = {0, 0, 800, 600};
  DetailPanelGeometryConfig config;
  config.preferred_position = DetailPanelPosition::Right;
  config.gap = 10;

  // 右侧空间 800 - 610 = 190 < 320；左侧空间 100 - 10 = 90 < 320
  int detail_w = 320;
  int detail_h = 200;

  DetailPanelRect pos = CalculateDetailPanelPosition(
      candidate_win, work_area, detail_w, detail_h, config);

  // 右侧空间大于左侧空间，靠最右侧对齐并限制在工作区内
  BOOST_TEST(pos.right <= work_area.right);
  BOOST_TEST(pos.left >= work_area.left);
  BOOST_TEST_EQ(pos.width(), 320);
}

// 4. 底部溢出垂直贴边安全对齐测试
void test_position_bottom_overflow_clamp() {
  // 候选窗口接近屏幕底部
  DetailPanelRect candidate_win = {500, 950, 700, 1050};
  DetailPanelRect work_area = {0, 0, 1920, 1080};
  DetailPanelGeometryConfig config;
  config.preferred_position = DetailPanelPosition::Right;
  config.gap = 10;

  int detail_w = 320;
  int detail_h = 300;  // 950 + 300 = 1250 > 1080 (overflows bottom)

  DetailPanelRect pos = CalculateDetailPanelPosition(
      candidate_win, work_area, detail_w, detail_h, config);

  // y 应被 clamp 到 work_area.bottom - detail_h = 1080 - 300 = 780
  BOOST_TEST_EQ(pos.top, 780);
  BOOST_TEST_EQ(pos.bottom, 1080);
  BOOST_TEST_EQ(pos.left, 710);
}

// 5. 偏好左侧（Left）及翻转测试
void test_position_left_preference_and_flip() {
  // 左侧正常
  DetailPanelRect candidate_win = {600, 300, 800, 600};
  DetailPanelRect work_area = {0, 0, 1920, 1080};
  DetailPanelGeometryConfig config;
  config.preferred_position = DetailPanelPosition::Left;
  config.gap = 10;

  DetailPanelRect pos =
      CalculateDetailPanelPosition(candidate_win, work_area, 300, 200, config);
  // 600 - 10 - 300 = 290
  BOOST_TEST_EQ(pos.left, 290);

  // 靠左侧过近，自动翻转至右侧
  candidate_win = {50, 300, 250, 600};
  pos =
      CalculateDetailPanelPosition(candidate_win, work_area, 300, 200, config);
  // 翻转到右侧：250 + 10 = 260
  BOOST_TEST_EQ(pos.left, 260);
}

// 6. min_width 与 max_width 约束测试
void test_min_max_width_constraint() {
  DetailPanelRect candidate_win = {200, 200, 400, 400};
  DetailPanelRect work_area = {0, 0, 1920, 1080};
  DetailPanelGeometryConfig config;
  config.min_width = 250;
  config.max_width = 400;

  // 请求宽度 180，应扩展至 min_width 250
  DetailPanelRect pos1 =
      CalculateDetailPanelPosition(candidate_win, work_area, 180, 200, config);
  BOOST_TEST_EQ(pos1.width(), 250);

  // 请求宽度 500，应限制至 max_width 400
  DetailPanelRect pos2 =
      CalculateDetailPanelPosition(candidate_win, work_area, 500, 200, config);
  BOOST_TEST_EQ(pos2.width(), 400);
}

void test_auto_position_resolution() {
  BOOST_TEST(ParseDetailPanelPosition("auto") == DetailPanelPosition::Auto);
  BOOST_TEST(ResolveDetailPanelPosition(DetailPanelPosition::Auto, false) ==
             DetailPanelPosition::Bottom);
  BOOST_TEST(ResolveDetailPanelPosition(DetailPanelPosition::Auto, true) ==
             DetailPanelPosition::Right);
  BOOST_TEST(ResolveDetailPanelPosition(DetailPanelPosition::Top, false) ==
             DetailPanelPosition::Top);
  BOOST_TEST(std::string(DetailPanelPositionToString(
                 DetailPanelPosition::Auto)) == "auto");
}

void test_detail_emphasis_markup() {
  ParsedDetailPanelText parsed =
      ParseDetailPanelText(L"状态：**重点客户**，等级：**A 类**");
  BOOST_TEST(parsed.text == L"状态：重点客户，等级：A 类");
  BOOST_TEST_EQ(parsed.emphasis_ranges.size(), 2u);
  BOOST_TEST_EQ(parsed.emphasis_ranges[0].start, 3u);
  BOOST_TEST_EQ(parsed.emphasis_ranges[0].length, 4u);
  BOOST_TEST_EQ(parsed.emphasis_ranges[1].start, 11u);
  BOOST_TEST_EQ(parsed.emphasis_ranges[1].length, 3u);

  parsed = ParseDetailPanelText(L"字面量：\\**不是强调**");
  BOOST_TEST(parsed.text == L"字面量：**不是强调**");
  BOOST_TEST(parsed.emphasis_ranges.empty());

  parsed = ParseDetailPanelText(L"异常：**未闭合");
  BOOST_TEST(parsed.text == L"异常：**未闭合");
  BOOST_TEST(parsed.emphasis_ranges.empty());

  parsed = ParseDetailPanelText(L"空标记：****");
  BOOST_TEST(parsed.text == L"空标记：****");
  BOOST_TEST(parsed.emphasis_ranges.empty());
}

// 7. CandidateInfo 数据结构与 equality 测试
void test_candidate_info_detail() {
  CandidateInfo ci1;
  CandidateInfo ci2;

  BOOST_TEST(ci1 == ci2);

  ci1.current_detail.str = L"发布机关：全国人大常委会\n文号：主席令第15号";
  BOOST_TEST(ci1 != ci2);

  ci2.current_detail.str = L"发布机关：全国人大常委会\n文号：主席令第15号";
  BOOST_TEST(ci1 == ci2);

  ci1.clear();
  BOOST_TEST(ci1.current_detail.empty());
  BOOST_TEST(ci1 != ci2);
}

// 8. UIStyle 默认配置与兼容性测试
void test_ui_style_defaults() {
  UIStyle style1;
  UIStyle style2;

  // 默认必须为 false，保证向后兼容
  BOOST_TEST_EQ(style1.detail_enabled, false);
  BOOST_TEST_EQ(style1.detail_gap, 8);
  BOOST_TEST_EQ(style1.detail_width, 320);
  BOOST_TEST_EQ(style1.detail_max_lines, 0);
  BOOST_TEST_EQ(style1.detail_font_point, 0);
  BOOST_TEST(style1.detail_han_font_face.empty());
  BOOST_TEST(style1.detail_latin_font_face.empty());
  BOOST_TEST_EQ(style1.detail_corner_radius, -1);
  BOOST_TEST_EQ(style1.detail_border_width, -1);
  BOOST_TEST_EQ(style1.detail_linespacing, 0);
  BOOST_TEST_EQ(style1.detail_border_color, 0);
  BOOST_TEST_EQ(style1.detail_shadow_color, 0);
  BOOST_TEST_EQ(style1.detail_draw_line_separators, false);
  BOOST_TEST_EQ(style1.detail_line_separator_color, 0);
  BOOST_TEST_EQ(style1.detail_key_text_color, 0);
  BOOST_TEST(style1 == style1);
  BOOST_TEST(!(style1 != style2));

  // 修改任意 detail 属性应能检测到样式变更
  style1.detail_enabled = true;
  BOOST_TEST(style1 != style2);

  style2.detail_enabled = true;
  BOOST_TEST(!(style1 != style2));

  style1.detail_position = UIStyle::DETAIL_POS_LEFT;
  BOOST_TEST(style1 != style2);

  style2.detail_position = UIStyle::DETAIL_POS_LEFT;
  style1.detail_draw_line_separators = true;
  BOOST_TEST(style1 != style2);

  style2.detail_draw_line_separators = true;
  style1.detail_font_point = 11;
  BOOST_TEST(style1 != style2);

  style2.detail_font_point = 11;
  style1.detail_han_font_face = L"A configurable Han font";
  BOOST_TEST(style1 != style2);

  style2.detail_han_font_face = L"A configurable Han font";
  style1.detail_latin_font_face = L"A configurable Latin font";
  BOOST_TEST(style1 != style2);
}

int main() {
  test_position_right_normal();
  test_position_right_flip_to_left();
  test_position_both_sides_constrained();
  test_position_bottom_overflow_clamp();
  test_position_left_preference_and_flip();
  test_min_max_width_constraint();
  test_auto_position_resolution();
  test_detail_emphasis_markup();
  test_candidate_info_detail();
  test_ui_style_defaults();

  std::cout << "All CandidateDetailPanel tests passed successfully!"
            << std::endl;
  return boost::report_errors();
}
