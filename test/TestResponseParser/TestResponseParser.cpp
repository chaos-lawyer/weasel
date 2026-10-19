// TestResponseParser.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <boost/detail/lightweight_test.hpp>
#include <ResponseParser.h>
#include <string>

void test_1() {
  WCHAR resp[] = L"action=noop\n";
  DWORD len = wcslen(resp);
  std::wstring commit;
  weasel::Context ctx;
  weasel::Status status;
  weasel::ResponseParser parser(&commit, &ctx, &status);
  parser(resp, len);
  BOOST_TEST(commit.empty());
  BOOST_TEST(ctx.empty());
}

void test_2() {
  WCHAR resp[] =
      L"action=commit\n"
      L"commit=教這句話上屏=3.14\n";
  DWORD len = wcslen(resp);
  std::wstring commit;
  weasel::Context ctx;
  weasel::Status status;
  ctx.aux.str = L"從前的值";
  weasel::ResponseParser parser(&commit, &ctx, &status);
  parser(resp, len);
  BOOST_TEST(commit == L"教這句話上屏=3.14");
  BOOST_TEST(ctx.preedit.empty());
  BOOST_TEST(ctx.aux.str == L"從前的值");
  BOOST_TEST(ctx.cinfo.candies.empty());
}

void test_3() {
  WCHAR resp[] =
      L"action=ctx\n"
      L"ctx.preedit=寫作串=3.14\n"
      L"ctx.aux=sie'zuoh'chuan=3.14\n";
  DWORD len = wcslen(resp);
  std::wstring commit;
  weasel::Context ctx;
  weasel::Status status;
  weasel::ResponseParser parser(&commit, &ctx, &status);
  parser(resp, len);
  BOOST_TEST(commit.empty());
  BOOST_TEST(ctx.preedit.str == L"寫作串=3.14");
  BOOST_TEST(ctx.preedit.attributes.empty());
  BOOST_TEST(ctx.aux.str == L"sie'zuoh'chuan=3.14");
}

void test_4() {
  WCHAR resp[] =
      L"action=commit,ctx\n"
      L"ctx.preedit=候選乙=3.14\n"
      L"ctx.preedit.cursor=0,3\n"
      L"ctx.cand.length=2\n"
      L"ctx.cand.0=候選甲\n"
      L"ctx.cand.1=候選乙\n"
      L"ctx.cand.cursor=1\n"
      L"ctx.cand.page=0/1\n";
  DWORD len = wcslen(resp);
  std::wstring commit;
  weasel::Context ctx;
  weasel::Status status;
  weasel::ResponseParser parser(&commit, &ctx, &status);
  parser(resp, len);
  BOOST_TEST(commit.empty());
  BOOST_TEST(ctx.preedit.str == L"候選乙=3.14");
  BOOST_ASSERT(1 == ctx.preedit.attributes.size());
  weasel::TextAttribute attr0 = ctx.preedit.attributes[0];
  BOOST_TEST_EQ(weasel::HIGHLIGHTED, attr0.type);
  BOOST_TEST_EQ(0, attr0.range.start);
  BOOST_TEST_EQ(3, attr0.range.end);
  BOOST_TEST(ctx.aux.empty());
  weasel::CandidateInfo& c = ctx.cinfo;
  BOOST_ASSERT(2 == c.candies.size());
  BOOST_TEST(c.candies[0].str == L"候選甲");
  BOOST_TEST(c.candies[1].str == L"候選乙");
  BOOST_TEST_EQ(1, c.highlighted);
  BOOST_TEST_EQ(0, c.currentPage);
  BOOST_TEST_EQ(1, c.totalPages);
}

void test_5() {
  WCHAR resp[] =
      L"action=ctx\n"
      L"ctx.undo=ctrl_z\n";
  DWORD len = wcslen(resp);
  std::wstring commit;
  weasel::Context ctx;
  weasel::Status status;
  weasel::ResponseParser parser(&commit, &ctx, &status);
  parser(resp, len);
  BOOST_TEST(ctx.undo_action == L"ctrl_z");
}

void test_cloud_pending() {
  WCHAR waiting[] = L"action=ctx\nctx.cloud_pending=1\n";
  WCHAR ready[] = L"action=ctx\nctx.cloud_pending=0\n";
  weasel::Context ctx;
  weasel::ResponseParser parser(nullptr, &ctx, nullptr);
  parser(waiting, wcslen(waiting));
  BOOST_TEST(ctx.cloud_pending);
  weasel::Context cleared;
  BOOST_TEST(ctx != cleared);
  parser(ready, wcslen(ready));
  BOOST_TEST(!ctx.cloud_pending);
  parser(waiting, wcslen(waiting));
  ctx.clear();
  BOOST_TEST(!ctx.cloud_pending);
}

void test_cursor_punctuation_config() {
  WCHAR enabled[] =
      L"action=config\n"
      L"config.cursor_tab_jump=1\n"
      L"config.cursor_punct_jump_keys=,.;:!?\n"
      L"config.cursor_punct_jump_any_field=1\n";
  WCHAR disabled[] =
      L"action=config\n"
      L"config.cursor_punct_jump_keys=\n"
      L"config.cursor_punct_jump_any_field=0\n";
  weasel::Config config;
  // External templates refresh policy without asking for candidate context.
  weasel::ResponseParser parser(nullptr, nullptr, nullptr, &config);
  parser(enabled, wcslen(enabled));
  BOOST_TEST(config.cursor_tab_jump);
  BOOST_TEST(config.cursor_punct_jump_any_field);
  BOOST_TEST(config.cursor_punct_jump_keys == L",.;:!?");
  parser(disabled, wcslen(disabled));
  BOOST_TEST(!config.cursor_punct_jump_any_field);
  BOOST_TEST(config.cursor_punct_jump_keys.empty());
  config.cursor_punct_jump_keys = L",";
  config.reset();
  BOOST_TEST(config.cursor_punct_jump_keys.empty());
}

void test_cursor_punctuation_split() {
  WCHAR response[] =
      L"action=commit,ctx\n"
      L"commit=民法典\n"
      L"ctx.cursor_punct_split=1\n";
  std::wstring commit;
  weasel::Context ctx;
  weasel::Context empty;
  weasel::ResponseParser parser(&commit, &ctx);
  parser(response, wcslen(response));
  BOOST_TEST(commit == L"民法典");
  BOOST_TEST(ctx.cursor_punct_split);
  BOOST_TEST(ctx != empty);
  ctx.clear();
  BOOST_TEST(!ctx.cursor_punct_split);
  BOOST_TEST(ctx == empty);
  WCHAR normal[] = L"action=ctx\nctx.preedit=继续\n";
  parser(normal, wcslen(normal));
  BOOST_TEST(!ctx.cursor_punct_split);
}

int _tmain(int argc, _TCHAR* argv[]) {
  test_1();
  test_2();
  test_3();
  test_4();
  test_5();
  test_cloud_pending();
  test_cursor_punctuation_config();
  test_cursor_punctuation_split();

  system("pause");
  return boost::report_errors();
}
