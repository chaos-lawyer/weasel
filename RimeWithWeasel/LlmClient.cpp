#include "stdafx.h"
#include "LlmClient.h"

#include <Windows.h>
#include <winhttp.h>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

namespace weasel_llm {
namespace {
using CachedResult = std::pair<std::chrono::steady_clock::time_point,
                               std::vector<CandidateItem>>;
std::mutex g_cache_mutex;
std::map<std::string, CachedResult> g_cache;
std::mutex g_debug_log_mutex;

const std::set<std::string>& Syllables() {
  static const std::set<std::string> values = [] {
    std::set<std::string> result;
    std::istringstream input(
        "a ai an ang ao e ei en eng er o ou ba bai ban bang bao bei ben beng "
        "bi bian biao bie bin bing bo bu pa pai pan pang pao pei pen peng "
        "pi pian piao pie pin ping po pu ma mai man mang mao me mei men meng "
        "mi mian miao mie min ming mo mou mu fa fan fang fei fen feng fo fu "
        "da dai dan dang dao de dei den deng di dia dian diao die ding diu "
        "dong dou du duan dui dun duo ta tai tan tang tao te teng ti tian "
        "tiao tie ting tong tou tu tuan tui tun tuo na nai nan nang nao ne nei "
        "nen neng ni nian niang niao nie nin ning niu nong nou nu nuan nuo "
        "nv nue la lai lan lang lao le lei leng li lia lian liang liao lie lin "
        "ling liu long lou lu luan lun luo lv lve ga gai gan gang gao ge gei "
        "gen geng gong gou gu gua guai guan guang gui gun guo ka kai kan kang "
        "kao ke ken keng kong kou ku kua kuai kuan kuang kui kun kuo ha hai "
        "han hang hao he hei hen heng hong hou hu hua huai huan huang hui hun "
        "huo ji jia jian jiang jiao jie jin jing jiong jiu ju juan jue jun qi "
        "qia qian qiang qiao qie qin qing qiong qiu qu quan que qun xi xia "
        "xian "
        "xiang xiao xie xin xing xiong xiu xu xuan xue xun zha zhai zhan zhang "
        "zhao zhe zhei zhen zheng zhi zhong zhou zhu zhua zhuai zhuan zhuang "
        "zhui zhun zhuo cha chai chan chang chao che chen cheng chi chong chou "
        "chu chua chuai chuan chuang chui chun chuo sha shai shan shang shao "
        "she shei shen sheng shi shou shu shua shuai shuan shuang shui shun "
        "shuo re ren reng ri rong rou ru ruan rui run ruo za zai zan zang zao "
        "ze zei zen zeng zi zong zou zu zuan zui zun zuo ca cai can cang cao "
        "ce cen ceng ci cong cou cu cuan cui cun cuo sa sai san sang sao se "
        "sen seng si song sou su suan sui sun suo ya yan yang yao ye yi yin "
        "ying yong you yu yuan yue yun wa wai wan wang wei wen weng wo wu");
    std::string value;
    while (input >> value)
      result.insert(value);
    return result;
  }();
  return values;
}

std::string InitialKey(const std::string& initial) {
  static const std::map<std::string, std::string> keys = {
      {"b", "b"}, {"p", "p"}, {"m", "m"},  {"f", "f"},  {"d", "d"},  {"t", "t"},
      {"n", "n"}, {"l", "l"}, {"g", "g"},  {"k", "k"},  {"h", "h"},  {"j", "j"},
      {"q", "q"}, {"x", "x"}, {"zh", "v"}, {"ch", "i"}, {"sh", "u"}, {"r", "r"},
      {"z", "z"}, {"c", "c"}, {"s", "s"},  {"y", "y"},  {"w", "w"}};
  auto it = keys.find(initial);
  return it == keys.end() ? std::string() : it->second;
}

std::string FinalKey(const std::string& final) {
  static const std::map<std::string, std::string> keys = {
      {"a", "a"},    {"o", "o"},    {"e", "e"},   {"i", "i"},    {"u", "u"},
      {"v", "v"},    {"ai", "d"},   {"ei", "w"},  {"ao", "c"},   {"ou", "z"},
      {"an", "j"},   {"en", "f"},   {"ang", "h"}, {"eng", "g"},  {"ong", "s"},
      {"ia", "x"},   {"ie", "p"},   {"iao", "n"}, {"iu", "q"},   {"ian", "m"},
      {"in", "b"},   {"iang", "l"}, {"ing", "k"}, {"iong", "s"}, {"ua", "x"},
      {"uo", "o"},   {"uai", "k"},  {"ui", "v"},  {"uan", "r"},  {"un", "y"},
      {"uang", "l"}, {"ue", "t"},   {"ve", "t"},  {"er", "r"}};
  auto it = keys.find(final);
  return it == keys.end() ? std::string() : it->second;
}

std::map<std::string, std::string> XiaoheCodes() {
  std::map<std::string, std::string> codes;
  const std::vector<std::string> initials = {
      "",  "b", "p", "m",  "f",  "d",  "t", "n", "l", "g", "k", "h",
      "j", "q", "x", "zh", "ch", "sh", "r", "z", "c", "s", "y", "w"};
  const std::vector<std::string> finals = {
      "a",  "o",   "e",  "i",    "u",   "v",    "ai", "ei", "ao",
      "ou", "an",  "en", "ang",  "eng", "ong",  "ia", "ie", "iao",
      "iu", "ian", "in", "iang", "ing", "iong", "ua", "uo", "uai",
      "ui", "uan", "un", "uang", "ue",  "ve"};
  for (const auto& initial : initials) {
    for (const auto& final : finals) {
      std::string syllable = initial + final;
      if (!Syllables().count(syllable))
        continue;
      std::string first = InitialKey(initial);
      std::string second = FinalKey(final);
      if (initial.empty()) {
        first = final.empty() ? std::string() : final.substr(0, 1);
      }
      if (first.empty() || second.empty())
        continue;
      codes[first + second] = syllable;
    }
  }
  return codes;
}

std::wstring Wide(const std::string& value) {
  if (value.empty())
    return {};
  int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                 static_cast<int>(value.size()), nullptr, 0);
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      &result[0], size);
  return result;
}

std::string Utf8(const std::wstring& value) {
  if (value.empty())
    return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                 static_cast<int>(value.size()), nullptr, 0,
                                 nullptr, nullptr);
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      &result[0], size, nullptr, nullptr);
  return result;
}

std::string JsonString(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char ch : value) {
    switch (ch) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (ch < 0x20)
          out << ' ';
        else
          out << ch;
    }
  }
  out << '"';
  return out.str();
}

void WriteContextDebugLog(const std::wstring& log_path,
                          const std::string& mode,
                          int preview_chars,
                          const std::wstring& context,
                          const InputPaths& input,
                          const std::string& source,
                          const std::wstring& diagnostic) {
  if (log_path.empty() || (mode != "preview" && mode != "full"))
    return;

  const bool full = mode == "full";
  const size_t preview_length =
      preview_chars > 0 ? static_cast<size_t>(preview_chars) : 0;
  const bool truncated = !full && context.size() > preview_length;
  const std::wstring visible_context =
      truncated ? context.substr(context.size() - preview_length) : context;
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto timestamp_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

  std::string record =
      "{\"context_source\":" + JsonString(source) +
      ",\"context_diagnostic\":" + JsonString(Utf8(diagnostic)) +
      ",\"timestamp_ms\":" + std::to_string(timestamp_ms) +
      ",\"mode\":" + JsonString(mode) +
      ",\"context_length_utf16\":" + std::to_string(context.size()) +
      ",\"context_truncated\":" + (truncated ? "true" : "false") +
      (full ? ",\"context\":" : ",\"context_tail\":") +
      JsonString(Utf8(visible_context)) +
      ",\"raw_input\":" + JsonString(input.raw_input) + ",\"phonetic\":" +
      (input.has_phonetic ? JsonString(input.phonetic) : "null") +
      ",\"initials\":" + JsonString(input.initials) + "}";

  std::lock_guard<std::mutex> lock(g_debug_log_mutex);
  std::ofstream log(std::filesystem::path(log_path),
                    std::ios::binary | std::ios::app);
  if (log)
    log << record << '\n';
}

static LlmResponse ParseResponse(
    const std::string& body,
    const std::wstring& default_predict_comment,
    const std::wstring& default_continuation_comment,
    int continuation_count,
    int limit) {
  LlmResponse response;
  try {
    std::stringstream stream(body);
    boost::property_tree::ptree root;
    boost::property_tree::read_json(stream, root);

    auto error_opt = root.get_child_optional("error");
    if (error_opt) {
      std::string err_msg = error_opt->get<std::string>("message", "");
      if (err_msg.empty()) {
        err_msg = error_opt->get<std::string>("code", "未知错误");
      }
      response.success = false;
      response.error_message = Wide(err_msg);
      return response;
    }

    std::string content;
    auto choices_opt = root.get_child_optional("choices");
    if (choices_opt && !choices_opt->empty()) {
      for (const auto& choice : *choices_opt) {
        auto msg_opt = choice.second.get_child_optional("message");
        if (msg_opt) {
          content = msg_opt->get<std::string>("content", "");
          if (content.empty()) {
            content = msg_opt->get<std::string>("reasoning_content", "");
          }
          if (!content.empty())
            break;
        }
        if (content.empty()) {
          content = choice.second.get<std::string>("text", "");
          if (!content.empty())
            break;
        }
      }
    }
    auto begin = content.find('{');
    auto end = content.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end < begin) {
      response.success = false;
      response.error_message = L"模型返回内容不包含有效 JSON";
      return response;
    }
    std::stringstream json(content.substr(begin, end - begin + 1));
    boost::property_tree::ptree parsed;
    boost::property_tree::read_json(json, parsed);
    std::set<std::wstring> seen;

    const auto add_item = [&](const std::string& text_raw,
                              const std::string& comment_raw,
                              const std::wstring& fallback_comment) {
      if (text_raw.empty())
        return;
      std::wstring text = Wide(text_raw);
      if (text.empty() || !seen.insert(text).second)
        return;
      std::wstring comment =
          comment_raw.empty() ? fallback_comment : Wide(comment_raw);
      response.candidates.push_back({std::move(text), std::move(comment)});
    };

    auto predictions_opt = parsed.get_child_optional("predictions");
    auto continuations_opt = parsed.get_child_optional("continuations");

    if (predictions_opt || continuations_opt) {
      if (predictions_opt) {
        for (const auto& item : *predictions_opt) {
          if (static_cast<int>(response.candidates.size()) >= limit)
            break;
          if (item.second.empty()) {
            add_item(item.second.get_value<std::string>(), "",
                     default_predict_comment);
          } else {
            add_item(item.second.get<std::string>("text", ""),
                     item.second.get<std::string>("comment", ""),
                     default_predict_comment);
          }
        }
      }
      if (continuations_opt) {
        for (const auto& item : *continuations_opt) {
          if (static_cast<int>(response.candidates.size()) >= limit)
            break;
          if (item.second.empty()) {
            add_item(item.second.get_value<std::string>(), "",
                     default_continuation_comment);
          } else {
            add_item(item.second.get<std::string>("text", ""),
                     item.second.get<std::string>("comment", ""),
                     default_continuation_comment);
          }
        }
      }
    } else {
      auto candidates_opt = parsed.get_child_optional("candidates");
      if (candidates_opt) {
        std::vector<std::pair<std::string, std::string>> raw_items;
        for (const auto& item : *candidates_opt) {
          if (item.second.empty()) {
            auto val = item.second.get_value<std::string>();
            if (!val.empty())
              raw_items.push_back({val, ""});
          } else {
            auto text = item.second.get<std::string>("text", "");
            auto comment = item.second.get<std::string>("comment", "");
            if (comment.empty()) {
              auto type = item.second.get<std::string>("type", "");
              if (type == "continuation") {
                comment = Utf8(default_continuation_comment);
              } else if (type == "prediction" || type == "predict") {
                comment = Utf8(default_predict_comment);
              }
            }
            if (!text.empty())
              raw_items.push_back({text, comment});
          }
        }
        const size_t total = raw_items.size();
        const size_t continuation_start =
            (continuation_count > 0 && total > 1)
                ? (total > static_cast<size_t>(continuation_count)
                       ? total - continuation_count
                       : 1)
                : total;
        for (size_t i = 0; i < total; ++i) {
          if (static_cast<int>(response.candidates.size()) >= limit)
            break;
          const std::wstring fallback = (i >= continuation_start)
                                            ? default_continuation_comment
                                            : default_predict_comment;
          add_item(raw_items[i].first, raw_items[i].second, fallback);
        }
      }
    }
    if (response.candidates.empty()) {
      response.success = false;
      response.error_message = L"模型未返回有效候选";
    } else {
      response.success = true;
    }
  } catch (const std::exception& e) {
    response.success = false;
    response.error_message = Wide(e.what());
    response.candidates.clear();
  } catch (...) {
    response.success = false;
    response.error_message = L"解析模型返回数据异常";
    response.candidates.clear();
  }
  return response;
}
}  // namespace

InputPaths ParseInput(const std::string& raw_input,
                      const std::string& schema_id,
                      const std::string& configured_scheme) {
  InputPaths paths;
  paths.schema_id = schema_id;
  paths.raw_input = raw_input;
  std::string scheme = configured_scheme;
  if (scheme == "auto")
    scheme = schema_id.find("flypy") != std::string::npos ||
                     schema_id.find("double_pinyin") != std::string::npos
                 ? "xiaohe"
                 : "quanpin";
  std::string initials;
  for (size_t i = 0; i < raw_input.size(); ++i) {
    std::string initial(1, static_cast<char>(std::tolower(raw_input[i])));
    if (scheme == "xiaohe") {
      if (initial == "v")
        initial = "zh";
      else if (initial == "i")
        initial = "ch";
      else if (initial == "u")
        initial = "sh";
    }
    if (!initials.empty())
      initials += ' ';
    initials += initial;
  }
  paths.initials = initials;
  if (scheme == "xiaohe" && raw_input.size() % 2 == 0) {
    static const auto codes = XiaoheCodes();
    std::string phonetic;
    for (size_t i = 0; i < raw_input.size(); i += 2) {
      auto it = codes.find(raw_input.substr(i, 2));
      if (it == codes.end()) {
        phonetic.clear();
        break;
      }
      if (!phonetic.empty())
        phonetic += ' ';
      phonetic += it->second;
    }
    if (!phonetic.empty()) {
      paths.phonetic = phonetic;
      paths.has_phonetic = true;
    }
  } else if (scheme == "quanpin") {
    // Find one complete segmentation using the local syllable inventory only.
    std::vector<std::string> best(raw_input.size() + 1);
    std::vector<bool> reachable(raw_input.size() + 1, false);
    reachable[0] = true;
    for (size_t i = 0; i < raw_input.size(); ++i) {
      if (!reachable[i])
        continue;
      for (size_t length = 1; length <= 6 && i + length <= raw_input.size();
           ++length) {
        std::string syllable = raw_input.substr(i, length);
        if (!Syllables().count(syllable))
          continue;
        best[i + length] =
            best[i].empty() ? syllable : best[i] + " " + syllable;
        reachable[i + length] = true;
      }
    }
    if (!raw_input.empty() && reachable.back()) {
      paths.phonetic = best.back();
      paths.has_phonetic = true;
    }
  }
  return paths;
}

LlmResponse RequestCandidates(const std::wstring& base_url,
                              const std::wstring& model,
                              const std::wstring& api_key,
                              const std::wstring& prompt_both,
                              const std::wstring& prompt_initials_only,
                              const std::wstring& default_predict_comment,
                              const std::wstring& default_continuation_comment,
                              int continuation_count,
                              const std::wstring& context,
                              const InputPaths& input,
                              int timeout_ms,
                              double temperature,
                              int candidate_count,
                              bool cache_enabled,
                              int cache_ttl_seconds,
                              int cache_max_entries,
                              const std::wstring& debug_log_path,
                              const std::string& debug_context_mode,
                              int debug_context_preview_chars,
                              const std::string& context_source,
                              const std::wstring& context_diagnostic) {
  const std::string cache_key =
      Utf8(base_url) + "\n" + Utf8(model) + "\n" + Utf8(prompt_both) + "\n" +
      Utf8(prompt_initials_only) + "\n" + Utf8(default_predict_comment) + "\n" +
      Utf8(default_continuation_comment) + "\n" +
      std::to_string(continuation_count) + "\n" + std::to_string(temperature) +
      "\n" + std::to_string(candidate_count) + "\n" + input.schema_id + "\n" +
      input.raw_input + "\n" + input.phonetic + "\n" + input.initials + "\n" +
      Utf8(context);
  if (cache_enabled) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto cached = g_cache.find(cache_key);
    if (cached != g_cache.end()) {
      const auto age = std::chrono::steady_clock::now() - cached->second.first;
      if (age <= std::chrono::seconds(cache_ttl_seconds))
        return {true, L"", cached->second.second};
      g_cache.erase(cached);
    }
  }
  if (base_url.empty())
    return {false, L"Base URL 未配置", {}};
  if (model.empty())
    return {false, L"Model 未配置", {}};
  if (api_key.empty())
    return {false, L"API Key 未配置", {}};
  if (prompt_both.empty() || prompt_initials_only.empty())
    return {false, L"Prompt 模板未配置", {}};

  std::wstring url = base_url;
  while (!url.empty() && url.back() == L'/')
    url.pop_back();
  constexpr wchar_t kCompletionsPath[] = L"/chat/completions";
  const size_t suffix_length = _countof(kCompletionsPath) - 1;
  if (url.size() < suffix_length ||
      url.substr(url.size() - suffix_length) != kCompletionsPath)
    url += L"/chat/completions";

  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    return {false, L"Base URL 格式无效", {}};
  std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  if (parts.nScheme != INTERNET_SCHEME_HTTPS && host != L"localhost" &&
      host != L"127.0.0.1" && host != L"::1")
    return {false, L"仅支持 HTTPS 请求 (或 localhost)", {}};
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength)
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
  HINTERNET session =
      WinHttpOpen(L"Weasel-LLM/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    session = WinHttpOpen(L"Weasel-LLM/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (!session) {
    DWORD err = GetLastError();
    return {false, L"WinHTTP 初始化失败 (" + std::to_wstring(err) + L")", {}};
  }
  WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
  if (!connection) {
    DWORD err = GetLastError();
    WinHttpCloseHandle(session);
    return {false, L"连接服务器失败 (" + std::to_wstring(err) + L")", {}};
  }
  HINTERNET request = WinHttpOpenRequest(
      connection, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!request) {
    DWORD err = GetLastError();
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return {false, L"创建请求失败 (" + std::to_wstring(err) + L")", {}};
  }
  std::string user_data =
      "{\"context\":" + JsonString(Utf8(context)) +
      ",\"raw_input\":" + JsonString(input.raw_input) + ",\"phonetic\":" +
      (input.has_phonetic ? JsonString(input.phonetic) : "null") +
      ",\"initials\":" + JsonString(input.initials) + "}";
  const std::string system_prompt =
      Utf8(input.has_phonetic ? prompt_both : prompt_initials_only);
  std::string body =
      "{\"model\":" + JsonString(Utf8(model)) +
      ",\"temperature\":" + std::to_string(temperature) +
      ",\"response_format\":{\"type\":\"json_object\"},"
      "\"messages\":[{\"role\":\"system\",\"content\":" +
      JsonString(system_prompt) +
      "},{\"role\":\"user\",\"content\":" + JsonString(user_data) + "}]}";
  std::wstring headers =
      L"Content-Type: application/json\r\nAuthorization: Bearer " + api_key +
      L"\r\n";
  bool sent = WinHttpSendRequest(
      request, headers.c_str(), static_cast<DWORD>(headers.size()),
      const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
      static_cast<DWORD>(body.size()), 0);
  if (!sent) {
    DWORD err = GetLastError();
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    std::wstring err_msg;
    switch (err) {
      case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        err_msg = L"域名解析失败 (" + host + L")";
        break;
      case ERROR_WINHTTP_CANNOT_CONNECT:
        err_msg = L"无法连接到服务器";
        break;
      case ERROR_WINHTTP_TIMEOUT:
        err_msg = L"请求发送超时";
        break;
      case ERROR_WINHTTP_SECURE_FAILURE:
        err_msg = L"SSL/TLS 证书校验失败";
        break;
      default:
        err_msg = L"发送请求失败 (" + std::to_wstring(err) + L")";
        break;
    }
    return {false, err_msg, {}};
  }
  WriteContextDebugLog(debug_log_path, debug_context_mode,
                       debug_context_preview_chars, context, input,
                       context_source, context_diagnostic);

  bool received = WinHttpReceiveResponse(request, nullptr);
  if (!received) {
    DWORD err = GetLastError();
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    std::wstring err_msg =
        (err == ERROR_WINHTTP_TIMEOUT)
            ? L"等待响应超时 (" + std::to_wstring(timeout_ms) + L"ms)"
            : L"接收响应失败 (" + std::to_wstring(err) + L")";
    return {false, err_msg, {}};
  }

  DWORD status = 0, status_size = sizeof(status);
  WinHttpQueryHeaders(request,
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                      WINHTTP_NO_HEADER_INDEX);
  std::string response;
  DWORD available = 0;
  while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request, &chunk[0], available, &read) || !read)
      break;
    chunk.resize(read);
    response += chunk;
    if (response.size() > 1024 * 1024) {
      response.clear();
      break;
    }
  }
  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);

  if (status != 200) {
    std::wstring detail;
    if (!response.empty()) {
      try {
        std::stringstream ss(response);
        boost::property_tree::ptree err_tree;
        boost::property_tree::read_json(ss, err_tree);
        std::string msg = err_tree.get<std::string>("error.message", "");
        if (msg.empty())
          msg = err_tree.get<std::string>("message", "");
        if (!msg.empty())
          detail = L": " + Wide(msg);
      } catch (...) {
      }
    }
    std::wstring status_text;
    switch (status) {
      case 401:
        status_text = L"HTTP 401 (API Key 无效或未授权)";
        break;
      case 403:
        status_text = L"HTTP 403 (权限不足或禁止访问)";
        break;
      case 404:
        status_text = L"HTTP 404 (接口路径未找到)";
        break;
      case 429:
        status_text = L"HTTP 429 (额度耗尽或请求受限)";
        break;
      case 500:
      case 502:
      case 503:
      case 504:
        status_text = L"HTTP " + std::to_wstring(status) + L" (服务提供商故障)";
        break;
      default:
        status_text = L"HTTP " + std::to_wstring(status);
        break;
    }
    return {false, status_text + detail, {}};
  }

  if (response.empty())
    return {false, L"服务器返回内容为空", {}};

  auto resp = ParseResponse(response, default_predict_comment,
                            default_continuation_comment, continuation_count,
                            candidate_count);
  if (cache_enabled && resp.success && !resp.candidates.empty() &&
      cache_max_entries > 0) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    while (static_cast<int>(g_cache.size()) >= cache_max_entries)
      g_cache.erase(g_cache.begin());
    g_cache[cache_key] = {std::chrono::steady_clock::now(), resp.candidates};
  }
  return resp;
}
}  // namespace weasel_llm
