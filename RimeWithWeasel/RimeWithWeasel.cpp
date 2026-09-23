#include "stdafx.h"
#include <logging.h>
#include <RimeWithWeasel.h>
#include <StringAlgorithm.hpp>
#include <WeaselConstants.h>
#include <WeaselUtility.h>

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <array>
#include <vector>
#include <regex>
#include <rime_api.h>
#include "LlmClient.h"

#define TRANSPARENT_COLOR 0x00000000
#define ARGB2ABGR(value)                                 \
  ((value & 0xff000000) | ((value & 0x000000ff) << 16) | \
   (value & 0x0000ff00) | ((value & 0x00ff0000) >> 16))
#define RGBA2ABGR(value)                                   \
  (((value & 0xff) << 24) | ((value & 0xff000000) >> 24) | \
   ((value & 0x00ff0000) >> 8) | ((value & 0x0000ff00) << 8))
typedef enum { COLOR_ABGR = 0, COLOR_ARGB, COLOR_RGBA } ColorFormat;

using namespace weasel;

// The selection resolver consumes the IPC mask, before librime expansion.
static_assert(select_keys_ibus::RELEASE_MASK == ibus::RELEASE_MASK);
static_assert(select_keys_ibus::SUPER_MASK == ibus::SUPER_MASK);
static_assert(select_keys_ibus::HYPER_MASK == ibus::HYPER_MASK);
static_assert(select_keys_ibus::META_MASK == ibus::META_MASK);

static RimeApi* rime_api;

struct DisplayPreedit {
  std::string text;
  size_t internal_prefix_length = 0;
  size_t display_prefix_length = 0;
};

static DisplayPreedit _GetDisplayPreedit(RimeSessionId session_id,
                                         const char* preedit) {
  DisplayPreedit result;
  result.text = preedit ? preedit : "";
  if (!session_id || result.text.empty()) {
    return result;
  }

  char display[256] = {0};
  char internal_prefix[64] = {0};
  if (!rime_api->get_property(session_id, "tab_mode_display", display,
                              sizeof(display)) ||
      !rime_api->get_property(session_id, "tab_mode_prefix", internal_prefix,
                              sizeof(internal_prefix)) ||
      display[0] == '\0' || internal_prefix[0] == '\0') {
    return result;
  }

  const size_t prefix_length = strlen(internal_prefix);
  if (result.text.compare(0, prefix_length, internal_prefix) != 0) {
    return result;
  }

  result.text.replace(0, prefix_length, display);
  result.internal_prefix_length = prefix_length;
  result.display_prefix_length = strlen(display);
  return result;
}

static int _MapDisplayPreeditPosition(int position,
                                      const DisplayPreedit& preedit) {
  if (position <= 0) {
    return 0;
  }
  if (preedit.internal_prefix_length == 0) {
    return position;
  }
  if (static_cast<size_t>(position) <= preedit.internal_prefix_length) {
    return static_cast<int>(preedit.display_prefix_length);
  }
  return position - static_cast<int>(preedit.internal_prefix_length) +
         static_cast<int>(preedit.display_prefix_length);
}

static std::string _TrimConfigValue(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

static std::filesystem::path _ResolveLlmConfigPath(
    const std::wstring& custom_config_path) {
  if (custom_config_path.empty()) {
    return WeaselUserDataPath() / L"dicts" / L"llm_config.txt";
  }
  std::filesystem::path p(custom_config_path);
  if (p.is_absolute()) {
    return p;
  }
  return WeaselUserDataPath() / p;
}

static std::map<std::string, std::string> _ReadLlmTextConfig(
    const std::wstring& custom_config_path = L"") {
  std::map<std::string, std::string> values;
  const auto config_path = _ResolveLlmConfigPath(custom_config_path);
  std::ifstream file(config_path, std::ios::binary);
  std::string line;
  while (std::getline(file, line)) {
    if (values.empty() && line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xef &&
        static_cast<unsigned char>(line[1]) == 0xbb &&
        static_cast<unsigned char>(line[2]) == 0xbf)
      line.erase(0, 3);
    line = _TrimConfigValue(line);
    if (line.empty() || line[0] == '#' || line[0] == ';')
      continue;
    const auto separator = line.find('=');
    if (separator == std::string::npos)
      continue;
    auto key = _TrimConfigValue(line.substr(0, separator));
    auto value = _TrimConfigValue(line.substr(separator + 1));
    if (!key.empty())
      values[key] = value;
  }
  return values;
}

static std::string _LlmValue(const std::map<std::string, std::string>& values,
                             const char* key,
                             const std::string& default_val = "") {
  auto it = values.find(key);
  return it == values.end() ? default_val : it->second;
}

static std::string _DecodePromptEscapes(const std::string& value) {
  std::string result;
  result.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\' || i + 1 >= value.size()) {
      result.push_back(value[i]);
      continue;
    }
    const char next = value[++i];
    switch (next) {
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      case '\\':
        result.push_back('\\');
        break;
      default:
        result.push_back('\\');
        result.push_back(next);
        break;
    }
  }
  return result;
}

static bool _LlmBool(const std::map<std::string, std::string>& values,
                     const char* key,
                     bool default_val = false) {
  auto it = values.find(key);
  if (it == values.end())
    return default_val;
  std::string value = it->second;
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  if (value == "1" || value == "true" || value == "yes" || value == "on")
    return true;
  if (value == "0" || value == "false" || value == "no" || value == "off")
    return false;
  return default_val;
}

static int _LlmInt(const std::map<std::string, std::string>& values,
                   const char* key,
                   int default_val = -1) {
  auto it = values.find(key);
  if (it == values.end())
    return default_val;
  try {
    size_t consumed = 0;
    int value = std::stoi(it->second, &consumed);
    return consumed == it->second.size() ? value : default_val;
  } catch (...) {
    return default_val;
  }
}

static double _LlmDouble(const std::map<std::string, std::string>& values,
                         const char* key,
                         double default_val = 0.0) {
  auto it = values.find(key);
  if (it == values.end())
    return default_val;
  try {
    size_t consumed = 0;
    double value = std::stod(it->second, &consumed);
    return consumed == it->second.size() ? value : default_val;
  } catch (...) {
    return default_val;
  }
}

static bool _HasLlmTextConfig(
    const std::map<std::string, std::string>& values) {
  if (!_LlmBool(values, "enabled", true))
    return false;
  if (_LlmValue(values, "base_url").empty() ||
      _LlmValue(values, "api_key").empty() ||
      _LlmValue(values, "model").empty()) {
    return false;
  }
  return true;
}

WeaselSessionId _GenerateNewWeaselSessionId(SessionStatusMap sm, DWORD pid) {
  if (sm.empty())
    return (WeaselSessionId)(pid + 1);
  return (WeaselSessionId)(sm.rbegin()->first + 1);
}

int expand_ibus_modifier(int m) {
  return (m & 0xff) | ((m & 0xff00) << 16);
}

static CandidateNavigationConfig _DefaultCandidateNavigationConfig() {
  CandidateNavigationConfig config;
  const auto key = [](uint32_t keycode, uint32_t modifiers = 0) {
    return CandidateNavigationKeyBinding{keycode, modifiers, true};
  };
  config.horizontal.previous_candidate = key(ibus::Left, ibus::CONTROL_MASK);
  config.horizontal.next_candidate = key(ibus::Right, ibus::CONTROL_MASK);
  config.horizontal.previous_page = key(ibus::Up);
  config.horizontal.next_page = key(ibus::Down);
  config.vertical.previous_candidate = key(ibus::Up);
  config.vertical.next_candidate = key(ibus::Down);
  config.vertical.previous_page = key(ibus::Left, ibus::CONTROL_MASK);
  config.vertical.next_page = key(ibus::Right, ibus::CONTROL_MASK);
  return config;
}

static bool _ParseCandidateNavigationKey(
    const std::string& value,
    CandidateNavigationKeyBinding& binding) {
  std::string normalized = value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) { return std::tolower(ch); });
  normalized.erase(
      std::remove_if(normalized.begin(), normalized.end(),
                     [](unsigned char ch) { return std::isspace(ch); }),
      normalized.end());
  if (normalized.empty() || normalized == "none") {
    binding = CandidateNavigationKeyBinding{};
    return true;
  }

  uint32_t modifiers = 0;
  size_t begin = 0;
  std::string key_name;
  while (begin <= normalized.size()) {
    const size_t end = normalized.find('+', begin);
    const std::string token = end == std::string::npos
                                  ? normalized.substr(begin)
                                  : normalized.substr(begin, end - begin);
    if (end == std::string::npos) {
      key_name = token;
      break;
    }
    if (token == "control" || token == "ctrl")
      modifiers |= ibus::CONTROL_MASK;
    else if (token == "shift")
      modifiers |= ibus::SHIFT_MASK;
    else if (token == "alt")
      modifiers |= ibus::ALT_MASK;
    else if (token == "super" || token == "win")
      modifiers |= ibus::SUPER_MASK;
    else
      return false;
    begin = end + 1;
  }

  static const std::map<std::string, uint32_t> keycodes = {
      {"left", ibus::Left},     {"right", ibus::Right},
      {"up", ibus::Up},         {"down", ibus::Down},
      {"page_up", ibus::Prior}, {"pageup", ibus::Prior},
      {"prior", ibus::Prior},   {"page_down", ibus::Next},
      {"pagedown", ibus::Next}, {"next", ibus::Next},
      {"home", ibus::Home},     {"end", ibus::End},
  };
  const auto found = keycodes.find(key_name);
  if (found == keycodes.end())
    return false;
  binding = CandidateNavigationKeyBinding{found->second, modifiers, true};
  return true;
}

RimeWithWeaselHandler::RimeWithWeaselHandler(UI* ui)
    : m_ui(ui),
      m_active_session(0),
      m_disabled(true),
      m_current_dark_mode(false),
      m_global_ascii_mode(false),
      m_show_notifications_time(1200),
      m_base_configured_layout_type(UIStyle::LAYOUT_VERTICAL),
      m_base_fullscreen(false),
      _UpdateUICallback(NULL) {
  m_ui->InServer() = true;
  rime_api = rime_get_api();
  assert(rime_api);
  m_pid = GetCurrentProcessId();
  uint16_t msbit = 0;
  for (auto i = 31; i >= 0; i--) {
    if (m_pid & (1 << i)) {
      msbit = i;
      break;
    }
  }
  m_pid = (m_pid << (31 - msbit));
  _Setup();
}

RimeWithWeaselHandler::~RimeWithWeaselHandler() {
  m_show_notifications.clear();
  m_session_status_map.clear();
  m_app_options.clear();
}

bool add_session = false;
void _UpdateUIStyle(RimeConfig* config, UI* ui, bool initialize);
bool _UpdateUIStyleColor(RimeConfig* config,
                         UIStyle& style,
                         const std::string& color = std::string());
void _LoadAppOptions(RimeConfig* config, AppOptionsByAppName& app_options);

void _RefreshTrayIcon(const RimeSessionId session_id,
                      const std::function<void()> _UpdateUICallback) {
  // Dangerous, don't touch
  static char app_name[256] = {0};
  auto ret = rime_api->get_property(session_id, "client_app", app_name,
                                    sizeof(app_name) - 1);
  if (!ret || u8tow(app_name) == std::wstring(L"explorer.exe"))
    boost::thread th([=]() {
      ::Sleep(100);
      if (_UpdateUICallback)
        _UpdateUICallback();
    });
  else if (_UpdateUICallback)
    _UpdateUICallback();
}

void RimeWithWeaselHandler::_Setup() {
  RIME_STRUCT(RimeTraits, weasel_traits);
  std::string shared_dir = wtou8(WeaselSharedDataPath().wstring());
  std::string user_dir = wtou8(WeaselUserDataPath().wstring());
  weasel_traits.shared_data_dir = shared_dir.c_str();
  weasel_traits.user_data_dir = user_dir.c_str();
  weasel_traits.prebuilt_data_dir = weasel_traits.shared_data_dir;
  std::string distribution_name = wtou8(get_weasel_ime_name());
  weasel_traits.distribution_name = distribution_name.c_str();
  weasel_traits.distribution_code_name = WEASEL_CODE_NAME;
  weasel_traits.distribution_version = WEASEL_VERSION;
  weasel_traits.app_name = "rime.weasel";
  std::string log_dir = WeaselLogPath().u8string();
  weasel_traits.log_dir = log_dir.c_str();
  rime_api->setup(&weasel_traits);
  rime_api->set_notification_handler(&RimeWithWeaselHandler::OnNotify, this);
}

void RimeWithWeaselHandler::Initialize() {
  m_disabled = _IsDeployerRunning();
  if (m_disabled) {
    return;
  }

  LOG(INFO) << "Initializing la rime.";
  rime_api->initialize(NULL);
  if (rime_api->start_maintenance(/*full_check = */ False)) {
    m_disabled = true;
    rime_api->join_maintenance_thread();
  }

  RimeConfig config = {NULL};
  if (rime_api->config_open("weasel", &config)) {
    if (m_ui) {
      _UpdateUIStyle(&config, m_ui, true);
      _UpdateShowNotifications(&config, true);
      m_current_dark_mode = IsUserDarkMode();
      if (m_current_dark_mode) {
        const int BUF_SIZE = 255;
        char buffer[BUF_SIZE + 1] = {0};
        if (rime_api->config_get_string(&config, "style/color_scheme_dark",
                                        buffer, BUF_SIZE)) {
          std::string color_name(buffer);
          _UpdateUIStyleColor(&config, m_ui->style(), color_name);
        }
      }
      m_base_style = m_ui->style();
      m_base_configured_layout_type = m_base_style.layout_type;
      Bool is_fs = False;
      rime_api->config_get_bool(&config, "style/fullscreen", &is_fs);
      m_base_fullscreen = !!is_fs;
      _LoadDynamicLayoutConfig(&config, m_base_dynamic_layout,
                               m_base_configured_layout_type);
      if (m_base_configured_layout_type == UIStyle::LAYOUT_AUTO) {
        m_base_style.layout_type =
            (m_base_dynamic_layout.default_layout ==
             weasel::CandidateLayout::Vertical)
                ? (m_base_fullscreen ? UIStyle::LAYOUT_VERTICAL_FULLSCREEN
                                     : UIStyle::LAYOUT_VERTICAL)
                : (m_base_fullscreen ? UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN
                                     : UIStyle::LAYOUT_HORIZONTAL);
        m_ui->style().layout_type = m_base_style.layout_type;
      }
    }
    Bool global_ascii = false;
    if (rime_api->config_get_bool(&config, "global_ascii", &global_ascii))
      m_global_ascii_mode = !!global_ascii;
    if (!rime_api->config_get_int(&config, "show_notifications_time",
                                  &m_show_notifications_time))
      m_show_notifications_time = 1200;
    _LoadAppOptions(&config, m_app_options);
    rime_api->config_close(&config);
  }
  m_last_schema_id.clear();
}

void RimeWithWeaselHandler::Finalize() {
  m_active_session = 0;
  m_disabled = true;
  for (auto& worker : m_llm_workers) {
    if (worker.thread.joinable())
      worker.thread.join();
  }
  m_llm_workers.clear();
  m_session_status_map.clear();
  LOG(INFO) << "Finalizing la rime.";
  rime_api->finalize();
}

DWORD RimeWithWeaselHandler::FindSession(WeaselSessionId ipc_id) {
  if (m_disabled)
    return 0;
  Bool found = rime_api->find_session(to_session_id(ipc_id));
  DLOG(INFO) << "Find session: session_id = " << to_session_id(ipc_id)
             << ", found = " << found;
  return found ? (ipc_id) : 0;
}

DWORD RimeWithWeaselHandler::AddSession(LPWSTR buffer, EatLine eat) {
  if (m_disabled) {
    DLOG(INFO) << "Trying to resume service.";
    EndMaintenance();
    if (m_disabled)
      return 0;
  }
  RimeSessionId session_id = (RimeSessionId)rime_api->create_session();
  if (m_global_ascii_mode) {
    for (const auto& pair : m_session_status_map) {
      if (pair.first) {
        rime_api->set_option(session_id, "ascii_mode",
                             !!pair.second.status.is_ascii_mode);
        break;
      }
    }
  }

  WeaselSessionId ipc_id =
      _GenerateNewWeaselSessionId(m_session_status_map, m_pid);
  DLOG(INFO) << "Add session: created session_id = " << session_id
             << ", ipc_id = " << ipc_id;
  SessionStatus& session_status = new_session_status(ipc_id);
  session_status.style = m_base_style;
  session_status.session_id = session_id;
  session_status.configured_layout_type = m_base_configured_layout_type;
  session_status.fullscreen = m_base_fullscreen;
  session_status.dynamic_layout_config = m_base_dynamic_layout;
  _ReadClientInfo(ipc_id, buffer);

  RIME_STRUCT(RimeStatus, status);
  if (rime_api->get_status(session_id, &status)) {
    std::string schema_id = status.schema_id;
    m_last_schema_id = schema_id;
    _LoadSchemaSpecificSettings(ipc_id, schema_id);
    _LoadAppInlinePreeditSet(ipc_id, true);
    _UpdateInlinePreeditStatus(ipc_id);
    _RefreshTrayIcon(session_id, _UpdateUICallback);
    session_status.status = status;
    session_status.__synced = false;
    rime_api->free_status(&status);
  }
  m_ui->style() = session_status.style;
  // show session's welcome message :-) if any
  if (eat) {
    _Respond(ipc_id, eat);
  }
  add_session = true;
  _UpdateUI(ipc_id);
  add_session = false;
  m_active_session = ipc_id;
  return ipc_id;
}

DWORD RimeWithWeaselHandler::RemoveSession(WeaselSessionId ipc_id) {
  if (m_ui)
    m_ui->Hide();
  if (m_disabled)
    return 0;
  DLOG(INFO) << "Remove session: session_id = " << to_session_id(ipc_id);
  // TODO: force committing? otherwise current composition would be lost
  rime_api->destroy_session(to_session_id(ipc_id));
  m_session_status_map.erase(ipc_id);
  m_active_session = 0;
  return 0;
}

void RimeWithWeaselHandler::UpdateColorTheme(BOOL darkMode) {
  RimeConfig config = {NULL};
  if (rime_api->config_open("weasel", &config)) {
    if (m_ui) {
      _UpdateUIStyle(&config, m_ui, true);
      m_current_dark_mode = darkMode;
      if (darkMode) {
        const int BUF_SIZE = 255;
        char buffer[BUF_SIZE + 1] = {0};
        if (rime_api->config_get_string(&config, "style/color_scheme_dark",
                                        buffer, BUF_SIZE)) {
          std::string color_name(buffer);
          _UpdateUIStyleColor(&config, m_ui->style(), color_name);
        }
      }
      m_base_style = m_ui->style();
    }
    rime_api->config_close(&config);
  }

  for (auto& pair : m_session_status_map) {
    RIME_STRUCT(RimeStatus, status);
    if (rime_api->get_status(to_session_id(pair.first), &status)) {
      _LoadSchemaSpecificSettings(pair.first, std::string(status.schema_id));
      _LoadAppInlinePreeditSet(pair.first, true);
      _UpdateInlinePreeditStatus(pair.first);
      pair.second.status = status;
      pair.second.__synced = false;
      rime_api->free_status(&status);
    }
  }
  m_ui->style() = get_session_status(m_active_session).style;
}

BOOL RimeWithWeaselHandler::ProcessKeyEvent(KeyEvent keyEvent,
                                            WeaselSessionId ipc_id,
                                            EatLine eat) {
  DLOG(INFO) << "Process key event: keycode = " << keyEvent.keycode
             << ", mask = " << keyEvent.mask << ", ipc_id = " << ipc_id;
  if (m_disabled)
    return FALSE;
  RimeSessionId session_id = to_session_id(ipc_id);
  SessionStatus& session_status = get_session_status(ipc_id);
  const bool repeated_llm_tab = keyEvent.keycode == ibus::Tab &&
                                !session_status.llm_request_id.empty() &&
                                !session_status.llm_raw_input.empty();
  const bool numeric_llm_selection = !session_status.llm_candidates.empty() &&
                                     keyEvent.keycode >= '1' &&
                                     keyEvent.keycode <= '9';
  const bool selecting_llm_candidate =
      (keyEvent.keycode == ibus::Select &&
       !session_status.llm_commit_text.empty()) ||
      numeric_llm_selection;
  if (!(keyEvent.mask & ibus::Modifier::RELEASE_MASK) && !repeated_llm_tab &&
      !selecting_llm_candidate) {
    ++session_status.llm_generation;
    session_status.llm_request_pending = false;
    session_status.llm_request_id.clear();
    session_status.llm_raw_input.clear();
    session_status.llm_schema_id.clear();
    session_status.llm_context.clear();
    session_status.llm_candidates.clear();
    session_status.llm_commit_text.clear();
    session_status.llm_request_submitted = false;
  }
  _RemapCandidateNavigationKey(session_status, session_id, keyEvent);

  char runtime_select_keys_buf[256] = {0};
  const bool has_runtime_select_keys =
      rime_api->get_property(session_id, "candidate_select_keys",
                             runtime_select_keys_buf,
                             sizeof(runtime_select_keys_buf)) &&
      runtime_select_keys_buf[0] != '\0';

  Bool handled = False;
  bool custom_key_processed = false;

  if (has_runtime_select_keys) {
    RIME_STRUCT(RimeContext, ctx);
    if (rime_api->get_context(session_id, &ctx)) {
      const size_t num_candidates =
          ctx.menu.num_candidates + session_status.llm_candidates.size();
      rime_api->free_context(&ctx);

      if (num_candidates > 0) {
        const auto runtime_keys =
            weasel::ParseSelectKeys(runtime_select_keys_buf);
        size_t selected_index = 0;
        const auto action = weasel::ResolveDynamicCandidateSelection(
            keyEvent.keycode, keyEvent.mask, runtime_keys, num_candidates,
            selected_index);
        if (action == weasel::DynamicCandidateSelectAction::SelectCandidate) {
          if (selected_index < session_status.llm_candidates.size()) {
            session_status.llm_commit_text =
                session_status.llm_candidates[selected_index].text;
            rime_api->clear_composition(session_id);
            handled = True;
          } else {
            handled = rime_api->select_candidate_on_current_page(
                session_id,
                selected_index - session_status.llm_candidates.size());
            ++session_status.llm_generation;
            session_status.llm_request_id.clear();
            session_status.llm_candidates.clear();
          }
          custom_key_processed = true;
        } else if (action == weasel::DynamicCandidateSelectAction::Swallow) {
          handled = True;
          custom_key_processed = true;
        }
      }
    }
  }

  if (!custom_key_processed) {
    if (!session_status.llm_candidates.empty() &&
        !(keyEvent.mask & ibus::Modifier::RELEASE_MASK) &&
        keyEvent.keycode >= '1' && keyEvent.keycode <= '9') {
      const size_t selected = static_cast<size_t>(keyEvent.keycode - '1');
      if (selected < session_status.llm_candidates.size()) {
        session_status.llm_commit_text =
            session_status.llm_candidates[selected].text;
        rime_api->clear_composition(session_id);
        handled = True;
        custom_key_processed = true;
      } else {
        const size_t local_index =
            selected - session_status.llm_candidates.size();
        RIME_STRUCT(RimeContext, selection_context);
        if (rime_api->get_context(session_id, &selection_context)) {
          if (local_index <
              static_cast<size_t>(selection_context.menu.num_candidates)) {
            handled = rime_api->select_candidate_on_current_page(session_id,
                                                                 local_index);
            custom_key_processed = true;
            ++session_status.llm_generation;
            session_status.llm_request_id.clear();
            session_status.llm_candidates.clear();
          }
          rime_api->free_context(&selection_context);
        }
      }
    }
    if (!custom_key_processed)
      handled = rime_api->process_key(session_id, keyEvent.keycode,
                                      expand_ibus_modifier(keyEvent.mask));
  }

  if (!(keyEvent.mask & ibus::Modifier::RELEASE_MASK)) {
    char trigger[8] = {0};
    char raw_input[256] = {0};
    char config_path[1024] = {0};
    if (rime_api->get_property(session_id, "llm_trigger", trigger,
                               sizeof(trigger)) &&
        trigger[0] == '1' &&
        rime_api->get_property(session_id, "llm_raw_input", raw_input,
                               sizeof(raw_input))) {
      rime_api->set_property(session_id, "llm_trigger", "");
      std::wstring custom_config_path;
      if (rime_api->get_property(session_id, "llm_config_path", config_path,
                                 sizeof(config_path)) &&
          config_path[0] != '\0') {
        custom_config_path = u8tow(config_path);
      }
      RIME_STRUCT(RimeStatus, status);
      if (rime_api->get_status(session_id, &status) && status.schema_id) {
        const auto text_config = _ReadLlmTextConfig(custom_config_path);
        if (_HasLlmTextConfig(text_config) &&
            _LlmBool(text_config, "enabled", true)) {
          const bool same_request =
              session_status.llm_raw_input == raw_input &&
              session_status.llm_schema_id == status.schema_id &&
              session_status.llm_config_path == custom_config_path &&
              !session_status.llm_request_id.empty();
          session_status.llm_config_path = custom_config_path;
          session_status.llm_request_id =
              std::to_wstring(ipc_id) + L"-" +
              std::to_wstring(session_status.llm_generation);
          session_status.llm_raw_input = raw_input;
          session_status.llm_schema_id = status.schema_id;
          session_status.llm_context_enabled =
              _LlmBool(text_config, "context_enabled", true);
          session_status.llm_context_chars = std::max(
              0, std::min(2000, _LlmInt(text_config, "context_chars", 500)));
          session_status.llm_boundary_search_chars = std::max(
              0, std::min(500,
                          _LlmInt(text_config, "boundary_search_chars", 100)));
          session_status.llm_request_pending = true;
          if (!same_request)
            session_status.llm_request_submitted = false;
        }
      }
      rime_api->free_status(&status);
    }
  }
  // vim_mode when keydown only
  if (!handled && !(keyEvent.mask & ibus::Modifier::RELEASE_MASK)) {
    bool isVimBackInCommandMode =
        (keyEvent.keycode == ibus::Keycode::Escape) ||
        ((keyEvent.mask & (1 << 2)) &&
         (keyEvent.keycode == ibus::Keycode::XK_c ||
          keyEvent.keycode == ibus::Keycode::XK_C ||
          keyEvent.keycode == ibus::Keycode::XK_bracketleft));
    if (isVimBackInCommandMode &&
        rime_api->get_option(session_id, "vim_mode") &&
        !rime_api->get_option(session_id, "ascii_mode")) {
      rime_api->set_option(session_id, "ascii_mode", True);
    }
  }
  _Respond(ipc_id, eat);
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
  return (BOOL)handled;
}

void RimeWithWeaselHandler::CommitComposition(WeaselSessionId ipc_id) {
  DLOG(INFO) << "Commit composition: ipc_id = " << ipc_id;
  if (m_disabled)
    return;
  auto& llm_status = get_session_status(ipc_id);
  ++llm_status.llm_generation;
  llm_status.llm_request_id.clear();
  llm_status.llm_candidates.clear();
  llm_status.llm_commit_text.clear();
  llm_status.llm_request_submitted = false;
  rime_api->commit_composition(to_session_id(ipc_id));
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
}

void RimeWithWeaselHandler::ClearComposition(WeaselSessionId ipc_id) {
  DLOG(INFO) << "Clear composition: ipc_id = " << ipc_id;
  if (m_disabled)
    return;
  auto& llm_status = get_session_status(ipc_id);
  ++llm_status.llm_generation;
  llm_status.llm_request_id.clear();
  llm_status.llm_candidates.clear();
  llm_status.llm_commit_text.clear();
  llm_status.llm_request_submitted = false;
  rime_api->clear_composition(to_session_id(ipc_id));
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
}

void RimeWithWeaselHandler::SelectCandidateOnCurrentPage(
    size_t index,
    WeaselSessionId ipc_id) {
  DLOG(INFO) << "select candidate on current page, ipc_id = " << ipc_id
             << ", index = " << index;
  if (m_disabled)
    return;
  SessionStatus& session_status = get_session_status(ipc_id);
  if (index < session_status.llm_candidates.size()) {
    session_status.llm_commit_text = session_status.llm_candidates[index].text;
    rime_api->clear_composition(to_session_id(ipc_id));
    return;
  }
  index -= session_status.llm_candidates.size();
  rime_api->select_candidate_on_current_page(to_session_id(ipc_id), index);
  ++session_status.llm_generation;
  session_status.llm_request_id.clear();
  session_status.llm_candidates.clear();
}

bool RimeWithWeaselHandler::HighlightCandidateOnCurrentPage(
    size_t index,
    WeaselSessionId ipc_id,
    EatLine eat) {
  DLOG(INFO) << "highlight candidate on current page, ipc_id = " << ipc_id
             << ", index = " << index;
  bool res = rime_api->highlight_candidate_on_current_page(
      to_session_id(ipc_id), index);
  _Respond(ipc_id, eat);
  _UpdateUI(ipc_id);
  return res;
}

bool RimeWithWeaselHandler::ChangePage(bool backward,
                                       WeaselSessionId ipc_id,
                                       EatLine eat) {
  DLOG(INFO) << "change page, ipc_id = " << ipc_id
             << (backward ? "backward" : "foreward");
  bool res = rime_api->change_page(to_session_id(ipc_id), backward);
  _Respond(ipc_id, eat);
  _UpdateUI(ipc_id);
  return res;
}

void RimeWithWeaselHandler::FocusIn(DWORD client_caps, WeaselSessionId ipc_id) {
  DLOG(INFO) << "Focus in: ipc_id = " << ipc_id
             << ", client_caps = " << client_caps;
  if (m_disabled)
    return;
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
}

void RimeWithWeaselHandler::FocusOut(DWORD param, WeaselSessionId ipc_id) {
  DLOG(INFO) << "Focus out: ipc_id = " << ipc_id;
  auto it = m_session_status_map.find(ipc_id);
  if (it != m_session_status_map.end()) {
    ++it->second.llm_generation;
    it->second.llm_request_id.clear();
    it->second.llm_candidates.clear();
    it->second.llm_commit_text.clear();
    it->second.llm_request_pending = false;
    it->second.llm_request_submitted = false;
  }
  if (m_ui)
    m_ui->Hide();
  m_active_session = 0;
}

void RimeWithWeaselHandler::SubmitLlmContext(WeaselSessionId ipc_id,
                                             const std::wstring& request_id,
                                             const std::wstring& context) {
  auto it = m_session_status_map.find(ipc_id);
  if (it == m_session_status_map.end())
    return;
  SessionStatus& session_status = it->second;
  if (request_id.empty() || request_id != session_status.llm_request_id ||
      session_status.llm_raw_input.empty()) {
    return;
  }
  session_status.llm_context =
      session_status.llm_context_enabled ? context : std::wstring();
  if (session_status.llm_request_submitted)
    return;
  const auto text_config = _ReadLlmTextConfig(session_status.llm_config_path);
  if (!_HasLlmTextConfig(text_config) ||
      !_LlmBool(text_config, "enabled", true))
    return;
  const std::string base_url = _LlmValue(text_config, "base_url");
  const std::string model = _LlmValue(text_config, "model");
  const std::string api_key = _LlmValue(text_config, "api_key");
  const std::string configured_scheme =
      _LlmValue(text_config, "input_scheme", "auto");

  const std::string legacy_comment =
      _LlmValue(text_config, "ai_comment_text", "✦ AI");
  const bool has_legacy_comment =
      text_config.find("ai_comment_text") != text_config.end();
  const std::string predict_comment =
      _LlmValue(text_config, "ai_predict_comment",
                has_legacy_comment ? legacy_comment : "✦ AI预测");
  const std::string continuation_comment =
      _LlmValue(text_config, "ai_continuation_comment",
                has_legacy_comment ? legacy_comment : "✦ AI续写");
  const int continuation_count =
      std::max(0, std::min(5, _LlmInt(text_config, "continuation_count", 2)));

  std::string prompt_both =
      _DecodePromptEscapes(_LlmValue(text_config, "prompt_both"));
  std::string prompt_initials_only =
      _DecodePromptEscapes(_LlmValue(text_config, "prompt_initials_only"));
  if (prompt_both.empty()) {
    prompt_both =
        "你是中文输入法候选生成器，不是聊天助手。\n"
        "用户本次输入只可能采用 phonetic 或 initials 中的一种。\n"
        "请结合 context 语义，返回一个 JSON 对象，包含 predictions "
        "数组（符合输入编码的精确匹配词句）"
        "和 continuations 数组（结合 context "
        "和输入的自然语言后续扩展续写）。只返回纯 JSON。";
  }
  if (prompt_initials_only.empty()) {
    prompt_initials_only =
        "你是中文输入法候选生成器，不是聊天助手。\n"
        "本次只有 initials 是有效输入解释。\n"
        "请结合 context 语义，返回一个 JSON 对象，包含 predictions 数组（符合 "
        "initials 的精确匹配词句）"
        "和 continuations 数组（结合 context "
        "和输入的自然语言后续扩展续写）。只返回纯 JSON。";
  }
  const int timeout_ms =
      std::max(500, std::min(30000, _LlmInt(text_config, "timeout_ms", 3000)));
  const int candidate_count =
      std::max(1, std::min(10, _LlmInt(text_config, "candidate_count", 5)));
  const bool cache_enabled = _LlmBool(text_config, "cache_enabled", true);
  const int cache_ttl_seconds =
      std::max(0, _LlmInt(text_config, "cache_ttl_seconds", 300));
  const int cache_max_entries =
      std::max(0, _LlmInt(text_config, "cache_max_entries", 500));
  const double temperature =
      std::max(0.0, std::min(2.0, _LlmDouble(text_config, "temperature", 0.0)));
  const bool show_ai_comment =
      _LlmBool(text_config, "ai_comment_enabled", true);

  const DWORD ipc_id_snapshot = ipc_id;
  const std::wstring request_snapshot = request_id;
  const uint64_t generation_snapshot = session_status.llm_generation;
  const std::wstring context_snapshot = session_status.llm_context_enabled
                                            ? session_status.llm_context
                                            : std::wstring();
  const auto input_snapshot =
      weasel_llm::ParseInput(session_status.llm_raw_input,
                             session_status.llm_schema_id, configured_scheme);
  session_status.llm_request_submitted = true;
  session_status.llm_ai_comment_enabled = !!show_ai_comment;
  session_status.llm_ai_comment = u8tow(predict_comment.c_str());
  session_status.llm_context = context_snapshot;
  auto worker_it = m_llm_workers.begin();
  while (worker_it != m_llm_workers.end()) {
    if (worker_it->done && worker_it->done->load()) {
      if (worker_it->thread.joinable())
        worker_it->thread.join();
      worker_it = m_llm_workers.erase(worker_it);
    } else {
      ++worker_it;
    }
  }
  m_llm_workers.push_back({});
  auto done = std::make_shared<std::atomic_bool>(false);
  m_llm_workers.back().done = done;
  try {
    m_llm_workers.back().thread = std::thread(
        [this, done, ipc_id_snapshot, request_snapshot, generation_snapshot,
         context_snapshot, input_snapshot, base_url, model, api_key,
         prompt_both, prompt_initials_only, predict_comment,
         continuation_comment, continuation_count, timeout_ms, temperature,
         candidate_count, cache_enabled = !!cache_enabled, cache_ttl_seconds,
         cache_max_entries]() {
          try {
            std::vector<weasel_llm::CandidateItem> candidates;
            candidates = weasel_llm::RequestCandidates(
                u8tow(base_url), u8tow(model), u8tow(api_key),
                u8tow(prompt_both), u8tow(prompt_initials_only),
                u8tow(predict_comment), u8tow(continuation_comment),
                continuation_count, context_snapshot, input_snapshot,
                timeout_ms, temperature, candidate_count, cache_enabled,
                cache_ttl_seconds, cache_max_entries);
            std::vector<LlmCandidateItem> converted;
            converted.reserve(candidates.size());
            for (auto& item : candidates) {
              converted.push_back(
                  {std::move(item.text), std::move(item.comment)});
            }
            {
              std::lock_guard<std::mutex> lock(m_llm_results_mutex);
              m_llm_results.push_back({ipc_id_snapshot, request_snapshot,
                                       generation_snapshot,
                                       std::move(converted)});
            }
            if (_AsyncRefreshCallback)
              _AsyncRefreshCallback(ipc_id_snapshot);
          } catch (...) {
          }
          done->store(true);
        });
  } catch (...) {
    m_llm_workers.pop_back();
    session_status.llm_request_submitted = false;
  }
}

void RimeWithWeaselHandler::RefreshSession(DWORD ipc_id) {
  std::vector<LlmResult> ready;
  {
    std::lock_guard<std::mutex> lock(m_llm_results_mutex);
    auto it = m_llm_results.begin();
    while (it != m_llm_results.end()) {
      if (it->ipc_id == ipc_id) {
        ready.push_back(std::move(*it));
        it = m_llm_results.erase(it);
      } else {
        ++it;
      }
    }
  }
  auto session = m_session_status_map.find(ipc_id);
  if (session == m_session_status_map.end())
    return;
  for (auto& result : ready) {
    auto& status = session->second;
    if (result.request_id == status.llm_request_id &&
        result.generation == status.llm_generation) {
      status.llm_candidates = std::move(result.candidates);
      if (status.llm_candidates.empty())
        status.llm_request_submitted = false;
      break;
    }
  }
  if (!session->second.llm_candidates.empty())
    _UpdateUI(ipc_id);
}

void RimeWithWeaselHandler::UpdateInputPosition(RECT const& rc,
                                                WeaselSessionId ipc_id) {
  DLOG(INFO) << "Update input position: (" << rc.left << ", " << rc.top
             << "), ipc_id = " << ipc_id
             << ", m_active_session = " << m_active_session;
  if (m_ui)
    m_ui->UpdateInputPosition(rc);
  if (m_disabled)
    return;
  if (m_active_session != ipc_id) {
    _UpdateUI(ipc_id);
    m_active_session = ipc_id;
  }
}

std::string RimeWithWeaselHandler::m_message_type;
std::string RimeWithWeaselHandler::m_message_value;
std::string RimeWithWeaselHandler::m_message_label;
std::string RimeWithWeaselHandler::m_option_name;
std::mutex RimeWithWeaselHandler::m_notifier_mutex;

void RimeWithWeaselHandler::OnNotify(void* context_object,
                                     uintptr_t session_id,
                                     const char* message_type,
                                     const char* message_value) {
  // may be running in a thread when deploying rime
  RimeWithWeaselHandler* self =
      reinterpret_cast<RimeWithWeaselHandler*>(context_object);
  if (!self || !message_type || !message_value)
    return;
  std::lock_guard<std::mutex> lock(m_notifier_mutex);
  // A key event can emit more than one notification before the UI is updated.
  // Do not let an option without a state label inherit the previous option's
  // label and accidentally start a timed notification window.
  m_message_label.clear();
  m_option_name.clear();
  m_message_type = message_type;
  m_message_value = message_value;
  if (RIME_API_AVAILABLE(rime_api, get_state_label) &&
      !strcmp(message_type, "option")) {
    Bool state = message_value[0] != '!';
    const char* option_name = message_value + !state;
    m_option_name = option_name;
    const char* state_label =
        rime_api->get_state_label(session_id, option_name, state);
    if (state_label) {
      m_message_label = std::string(state_label);
    }
  }
}

void RimeWithWeaselHandler::_ReadClientInfo(WeaselSessionId ipc_id,
                                            LPWSTR buffer) {
  std::string app_name;
  // parse request text
  wbufferstream bs(buffer, WEASEL_IPC_BUFFER_LENGTH);
  std::wstring line;
  while (bs.good()) {
    std::getline(bs, line);
    if (!bs.good())
      break;
    // file ends
    if (line == L".")
      break;
    const std::wstring kClientAppKey = L"session.client_app=";
    if (starts_with(line, kClientAppKey)) {
      std::wstring lwr = line;
      to_lower(lwr);
      app_name = wtou8(lwr.substr(kClientAppKey.length()));
    }
  }
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  // set app specific options
  if (!app_name.empty()) {
    rime_api->set_property(session_id, "client_app", app_name.c_str());

    auto it = m_app_options.find(app_name);
    if (it != m_app_options.end()) {
      AppOptions& options(m_app_options[it->first]);
      for (const auto& pair : options) {
        DLOG(INFO) << "set app option: " << pair.first << " = " << pair.second;
        rime_api->set_option(session_id, pair.first.c_str(), Bool(pair.second));
      }
    }
  }
  // inline preedit
  bool inline_preedit = session_status.style.inline_preedit;
  rime_api->set_option(session_id, "inline_preedit", Bool(inline_preedit));
  // show soft cursor on weasel panel but not inline
  rime_api->set_option(session_id, "soft_cursor", Bool(!inline_preedit));
}

void RimeWithWeaselHandler::_GetCandidateInfo(CandidateInfo& cinfo,
                                              RimeContext& ctx,
                                              RimeSessionId session_id) {
  cinfo.candies.resize(ctx.menu.num_candidates);
  cinfo.comments.resize(ctx.menu.num_candidates);
  cinfo.labels.resize(ctx.menu.num_candidates);

  char runtime_labels_buf[256] = {0};
  std::vector<std::wstring> runtime_labels;
  if (session_id &&
      rime_api->get_property(session_id, "candidate_select_labels",
                             runtime_labels_buf, sizeof(runtime_labels_buf)) &&
      runtime_labels_buf[0] != '\0') {
    runtime_labels = weasel::ParseSelectLabels(runtime_labels_buf);
  }

  char runtime_keys_buf[256] = {0};
  std::vector<std::wstring> runtime_keys;
  if (session_id &&
      rime_api->get_property(session_id, "candidate_select_keys",
                             runtime_keys_buf, sizeof(runtime_keys_buf)) &&
      runtime_keys_buf[0] != '\0') {
    runtime_keys = weasel::ParseSelectKeys(runtime_keys_buf);
  }

  const char* const* schema_select_labels =
      (RIME_STRUCT_HAS_MEMBER(ctx, ctx.select_labels) && ctx.select_labels)
          ? ctx.select_labels
          : nullptr;
  const char* schema_select_keys = ctx.menu.select_keys;

  for (int i = 0; i < ctx.menu.num_candidates; ++i) {
    cinfo.candies[i].str = escape_string(u8tow(ctx.menu.candidates[i].text));
    if (ctx.menu.candidates[i].comment) {
      cinfo.comments[i].str =
          escape_string(u8tow(ctx.menu.candidates[i].comment));
    }
    cinfo.labels[i].str = escape_string(weasel::FormatCandidateLabel(
        static_cast<size_t>(i), runtime_labels, runtime_keys,
        schema_select_labels, schema_select_keys));
  }
  const SessionStatus* llm_session = nullptr;
  for (const auto& item : m_session_status_map) {
    if (item.second.session_id == session_id) {
      llm_session = &item.second;
      break;
    }
  }
  if (llm_session && !llm_session->llm_candidates.empty()) {
    const size_t count = llm_session->llm_candidates.size();
    std::vector<Text> old_candies = std::move(cinfo.candies);
    std::vector<Text> old_comments = std::move(cinfo.comments);
    std::vector<Text> old_labels = std::move(cinfo.labels);
    cinfo.candies.resize(count);
    cinfo.comments.resize(count);
    cinfo.labels.resize(count);
    for (size_t i = 0; i < count; ++i) {
      cinfo.candies[i].str = escape_string(llm_session->llm_candidates[i].text);
      cinfo.comments[i].str =
          llm_session->llm_ai_comment_enabled
              ? escape_string(llm_session->llm_candidates[i].comment)
              : std::wstring();
      cinfo.labels[i].str = escape_string(weasel::FormatCandidateLabel(
          i, runtime_labels, runtime_keys, schema_select_labels,
          schema_select_keys));
    }
    cinfo.candies.insert(cinfo.candies.end(), old_candies.begin(),
                         old_candies.end());
    cinfo.comments.insert(cinfo.comments.end(), old_comments.begin(),
                          old_comments.end());
    cinfo.labels.insert(cinfo.labels.end(), old_labels.begin(),
                        old_labels.end());
    for (size_t i = 0; i < old_labels.size(); ++i) {
      cinfo.labels[count + i].str = escape_string(weasel::FormatCandidateLabel(
          count + i, runtime_labels, runtime_keys, schema_select_labels,
          schema_select_keys));
    }
    cinfo.highlighted = 0;
  }
  cinfo.highlighted = ctx.menu.highlighted_candidate_index;
  cinfo.currentPage = ctx.menu.page_no;
  cinfo.is_last_page = ctx.menu.is_last_page;

  cinfo.current_detail.clear();
  cinfo.current_detail_width = 0;
  if (session_id) {
    char detail_buf[8192] = {0};
    if (rime_api->get_property(session_id, "candidate_detail", detail_buf,
                               sizeof(detail_buf)) &&
        detail_buf[0] != '\0') {
      cinfo.current_detail.str = escape_string(u8tow(detail_buf));
    } else if (ctx.menu.highlighted_candidate_index >= 0) {
      std::string indexed_prop =
          "candidate_detail_" +
          std::to_string(ctx.menu.highlighted_candidate_index);
      if (rime_api->get_property(session_id, indexed_prop.c_str(), detail_buf,
                                 sizeof(detail_buf)) &&
          detail_buf[0] != '\0') {
        cinfo.current_detail.str = escape_string(u8tow(detail_buf));
      }
    }

    char detail_width_buf[16] = {0};
    if (!cinfo.current_detail.empty() &&
        rime_api->get_property(session_id, "candidate_detail_width",
                               detail_width_buf, sizeof(detail_width_buf)) &&
        detail_width_buf[0] != '\0') {
      char* end = nullptr;
      const long width = std::strtol(detail_width_buf, &end, 10);
      if (end != detail_width_buf && *end == '\0' && width >= 80 &&
          width <= 2048) {
        cinfo.current_detail_width = static_cast<int>(width);
      }
    }
  }
}

void RimeWithWeaselHandler::StartMaintenance() {
  m_session_status_map.clear();
  Finalize();
  _UpdateUI(0);
}

void RimeWithWeaselHandler::EndMaintenance() {
  if (m_disabled) {
    Initialize();
    _UpdateUI(0);
  }
  m_session_status_map.clear();
}

void RimeWithWeaselHandler::SetOption(WeaselSessionId ipc_id,
                                      const std::string& opt,
                                      bool val) {
  if (opt == "ascii_mode") {
    auto it = m_session_status_map.find(ipc_id);
    if (it != m_session_status_map.end()) {
      ++it->second.llm_generation;
      it->second.llm_request_id.clear();
      it->second.llm_candidates.clear();
      it->second.llm_commit_text.clear();
      it->second.llm_request_pending = false;
      it->second.llm_request_submitted = false;
    } else if (!ipc_id) {
      for (auto& session : m_session_status_map) {
        ++session.second.llm_generation;
        session.second.llm_request_id.clear();
        session.second.llm_candidates.clear();
        session.second.llm_commit_text.clear();
        session.second.llm_request_pending = false;
        session.second.llm_request_submitted = false;
      }
    }
  }
  // from no-session client, not actual typing session
  if (!ipc_id) {
    if (m_global_ascii_mode && opt == "ascii_mode") {
      for (auto& pair : m_session_status_map)
        rime_api->set_option(to_session_id(pair.first), "ascii_mode", val);
    } else {
      rime_api->set_option(to_session_id(m_active_session), opt.c_str(), val);
    }
  } else {
    rime_api->set_option(to_session_id(ipc_id), opt.c_str(), val);
  }
  // refresh UI (and tray icon) so the option change takes effect immediately,
  // e.g. when toggling ascii_mode from the TSF language bar
  _UpdateUI(ipc_id ? ipc_id : m_active_session);
}

void RimeWithWeaselHandler::OnUpdateUI(std::function<void()> const& cb) {
  _UpdateUICallback = cb;
}

bool RimeWithWeaselHandler::_IsDeployerRunning() {
  HANDLE hMutex = CreateMutex(NULL, TRUE, L"WeaselDeployerMutex");
  bool deployer_detected = hMutex && GetLastError() == ERROR_ALREADY_EXISTS;
  if (hMutex) {
    CloseHandle(hMutex);
  }
  return deployer_detected;
}

void RimeWithWeaselHandler::_UpdateUI(WeaselSessionId ipc_id) {
  // if m_ui nullptr, _UpdateUI meaningless
  if (!m_ui)
    return;

  Status& weasel_status = m_ui->status();
  Context weasel_context;

  RimeSessionId session_id = to_session_id(ipc_id);

  if (ipc_id == 0)
    weasel_status.disabled = m_disabled;

  _GetStatus(weasel_status, ipc_id, weasel_context);

  SessionStatus& session_status = get_session_status(ipc_id);
  if (rime_api->get_option(session_id, "inline_preedit"))
    session_status.style.client_caps |= INLINE_PREEDIT_CAPABLE;
  else
    session_status.style.client_caps &= ~INLINE_PREEDIT_CAPABLE;

  if (!_ShowMessage(weasel_context, weasel_status)) {
    m_ui->Hide();
    m_ui->Update(weasel_context, weasel_status);
  }

  _RefreshTrayIcon(session_id, _UpdateUICallback);

  {
    std::lock_guard<std::mutex> lock(m_notifier_mutex);
    m_message_type.clear();
    m_message_value.clear();
    m_message_label.clear();
    m_option_name.clear();
  }
}

void RimeWithWeaselHandler::_LoadSchemaSpecificSettings(
    WeaselSessionId ipc_id,
    const std::string& schema_id) {
  if (!m_ui)
    return;
  RimeConfig config;
  if (!rime_api->schema_open(schema_id.c_str(), &config))
    return;
  const auto schema_overrides_layout = [this, &config]() {
    constexpr int BUF_SIZE = 255;
    char buffer[BUF_SIZE + 1] = {0};
    Bool bool_value = False;
    return rime_api->config_get_string(&config, "style/layout/type", buffer,
                                       BUF_SIZE) ||
           rime_api->config_get_bool(&config, "style/horizontal",
                                     &bool_value) ||
           rime_api->config_get_bool(&config, "style/vertical_text",
                                     &bool_value) ||
           rime_api->config_get_string(&config, "style/text_orientation",
                                       buffer, BUF_SIZE);
  }();
  _UpdateShowNotifications(&config);
  m_ui->style() = m_base_style;
  _UpdateUIStyle(&config, m_ui, false);
  SessionStatus& session_status = get_session_status(ipc_id);
  session_status.style = m_ui->style();
  UIStyle& style = session_status.style;
  session_status.configured_layout_type = weasel::ResolveConfiguredLayoutType(
      m_base_configured_layout_type, style.layout_type,
      schema_overrides_layout);
  Bool is_fs = m_base_fullscreen ? True : False;
  rime_api->config_get_bool(&config, "style/fullscreen", &is_fs);
  session_status.fullscreen = !!is_fs;
  _LoadDynamicLayoutConfig(&config, session_status.dynamic_layout_config,
                           session_status.configured_layout_type);
  const auto schema_navigation =
      session_status.dynamic_layout_config.navigation;
  if (session_status.configured_layout_type == UIStyle::LAYOUT_AUTO &&
      session_status.dynamic_layout_config.rules.empty() &&
      !m_base_dynamic_layout.rules.empty()) {
    session_status.dynamic_layout_config = m_base_dynamic_layout;
  }
  if (schema_navigation.configured) {
    session_status.dynamic_layout_config.navigation = schema_navigation;
  } else {
    session_status.dynamic_layout_config.navigation =
        m_base_dynamic_layout.navigation;
  }
  if (session_status.configured_layout_type == UIStyle::LAYOUT_AUTO) {
    style.layout_type =
        (session_status.dynamic_layout_config.default_layout ==
         weasel::CandidateLayout::Vertical)
            ? (session_status.fullscreen ? UIStyle::LAYOUT_VERTICAL_FULLSCREEN
                                         : UIStyle::LAYOUT_VERTICAL)
            : (session_status.fullscreen ? UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN
                                         : UIStyle::LAYOUT_HORIZONTAL);
    m_ui->style().layout_type = style.layout_type;
  }
  // load schema color style config
  const int BUF_SIZE = 255;
  char buffer[BUF_SIZE + 1] = {0};
  const auto update_color_scheme = [&]() {
    std::string color_name(buffer);
    RimeConfigIterator preset = {0};
    if (rime_api->config_begin_map(
            &preset, &config, ("preset_color_schemes/" + color_name).c_str())) {
      _UpdateUIStyleColor(&config, style, color_name);
      rime_api->config_end(&preset);
    } else {
      RimeConfig weaselconfig;
      if (rime_api->config_open("weasel", &weaselconfig)) {
        _UpdateUIStyleColor(&weaselconfig, style, color_name);
        rime_api->config_close(&weaselconfig);
      }
    }
  };
  const char* key =
      m_current_dark_mode ? "style/color_scheme_dark" : "style/color_scheme";
  if (rime_api->config_get_string(&config, key, buffer, BUF_SIZE))
    update_color_scheme();
  // load schema icon start
  {
    const auto load_icon = [](RimeConfig& config, const char* key1,
                              const char* key2) {
      const auto user_dir = WeaselUserDataPath();
      const auto shared_dir = WeaselSharedDataPath();
      const int BUF_SIZE = 255;
      char buffer[BUF_SIZE + 1] = {0};
      if (rime_api->config_get_string(&config, key1, buffer, BUF_SIZE) ||
          (key2 != NULL &&
           rime_api->config_get_string(&config, key2, buffer, BUF_SIZE))) {
        auto resource = u8tow(buffer);
        if (fs::is_regular_file(user_dir / resource))
          return (user_dir / resource).wstring();
        else if (fs::is_regular_file(shared_dir / resource))
          return (shared_dir / resource).wstring();
      }
      return std::wstring();
    };
    style.current_zhung_icon =
        load_icon(config, "schema/icon", "schema/zhung_icon");
    style.current_ascii_icon = load_icon(config, "schema/ascii_icon", NULL);
    style.current_full_icon = load_icon(config, "schema/full_icon", NULL);
    style.current_half_icon = load_icon(config, "schema/half_icon", NULL);
  }
  // load schema icon end
  rime_api->config_close(&config);
}

void RimeWithWeaselHandler::_LoadAppInlinePreeditSet(WeaselSessionId ipc_id,
                                                     bool ignore_app_name) {
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  static char _app_name[50];
  rime_api->get_property(session_id, "client_app", _app_name,
                         sizeof(_app_name) - 1);
  std::string app_name(_app_name);
  if (!ignore_app_name && m_last_app_name == app_name)
    return;
  m_last_app_name = app_name;
  bool inline_preedit = session_status.style.inline_preedit;
  bool found = false;
  if (!app_name.empty()) {
    auto it = m_app_options.find(app_name);
    if (it != m_app_options.end()) {
      AppOptions& options(m_app_options[it->first]);
      for (const auto& pair : options) {
        if (pair.first == "inline_preedit") {
          rime_api->set_option(session_id, pair.first.c_str(),
                               Bool(pair.second));
          session_status.style.inline_preedit = Bool(pair.second);
          found = true;
          break;
        }
      }
    }
  }
  if (!found) {
    session_status.style.inline_preedit = m_base_style.inline_preedit;
    // load from schema.
    RIME_STRUCT(RimeStatus, status);
    if (rime_api->get_status(session_id, &status)) {
      std::string schema_id = status.schema_id;
      RimeConfig config;
      if (rime_api->schema_open(schema_id.c_str(), &config)) {
        Bool value = False;
        if (rime_api->config_get_bool(&config, "style/inline_preedit",
                                      &value)) {
          session_status.style.inline_preedit = value;
        }
        rime_api->config_close(&config);
      }
      rime_api->free_status(&status);
    }
  }
  if (session_status.style.inline_preedit != inline_preedit)
    _UpdateInlinePreeditStatus(ipc_id);
}

bool RimeWithWeaselHandler::_ShowMessage(Context& ctx, Status& status) {
  std::lock_guard<std::mutex> lock(m_notifier_mutex);
  if (m_message_type.empty() || m_message_value.empty()) {
    // A timed status notification must not freeze a newer composing context.
    // Returning false lets _UpdateUI cancel the timer and draw the current
    // candidates immediately.
    return m_ui->IsCountingDown() && !status.composing;
  }
  // show as auxiliary string
  std::wstring& tips(ctx.aux.str);
  bool show_icon = false;
  if (m_message_type == "deploy") {
    if (m_message_value == "start")
      if (GetThreadUILanguage() == MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US))
        tips = L"Deploying RIME";
      else
        tips = L"正在部署 RIME";
    else if (m_message_value == "success")
      if (GetThreadUILanguage() == MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US))
        tips = L"Deployed";
      else
        tips = L"部署完成";
    else if (m_message_value == "failure") {
      if (GetThreadUILanguage() ==
          MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL))
        tips = L"有錯誤，請查看日誌 %TEMP%\\rime.weasel\\rime.weasel.*.INFO";
      else if (GetThreadUILanguage() ==
               MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED))
        tips = L"有错误，请查看日志 %TEMP%\\rime.weasel\\rime.weasel.*.INFO";
      else
        tips =
            L"There is an error, please check the logs "
            L"%TEMP%\\rime.weasel\\rime.weasel.*.INFO";
    }
  } else if (m_message_type == "schema") {
    tips = /*L"【" + */ status.schema_name /* + L"】"*/;
  } else if (m_message_type == "option") {
    status.type = SCHEMA;
    if (m_message_value == "!ascii_mode" || m_message_value == "ascii_mode") {
      show_icon = true;
    } else if (m_message_value == "full_shape" ||
               m_message_value == "!full_shape") {
      show_icon = true;
      status.type = FULL_SHAPE;
    } else {
      tips = u8tow(m_message_label);
      // Internal options (for example a dynamic-layout trigger) have no state
      // label and should not create a second, timed candidate window.
      if (tips.empty())
        return false;
    }
  }
  auto counter = m_ui->IsCountingDown();
  if (!show_icon && counter)
    return !status.composing;
  auto foption = m_show_notifications.find(m_option_name);
  auto falways = m_show_notifications.find("always");
  if ((!add_session && (foption != m_show_notifications.end() ||
                        falways != m_show_notifications.end())) ||
      m_message_type == "deploy") {
    m_ui->Update(ctx, status);
    if (m_show_notifications_time)
      m_ui->ShowWithTimeout(m_show_notifications_time);
    return true;
  } else {
    return m_ui->IsCountingDown() && !status.composing;
  }
}
inline std::string _GetLabelText(const std::vector<Text>& labels,
                                 int id,
                                 const wchar_t* format) {
  wchar_t buffer[128];
  swprintf_s<128>(buffer, format, labels.at(id).str.c_str());
  return wtou8(std::wstring(buffer));
}

bool RimeWithWeaselHandler::_Respond(WeaselSessionId ipc_id, EatLine eat) {
  std::wstring body;
  body.reserve(4096);
  std::vector<const char*> actions;
  actions.reserve(8);

  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  RIME_STRUCT(RimeCommit, commit);
  if (rime_api->get_commit(session_id, &commit)) {
    actions.push_back("commit");
    std::wstring commit_text_w = escape_string(u8tow(commit.text));
    body.append(L"commit=").append(commit_text_w).append(L"\n");
    rime_api->free_commit(&commit);
  }
  if (!session_status.llm_commit_text.empty()) {
    actions.push_back("commit");
    body.append(L"commit=")
        .append(escape_string(session_status.llm_commit_text))
        .append(L"\n");
    session_status.llm_commit_text.clear();
    session_status.llm_candidates.clear();
  }

  bool is_composing = false;
  RIME_STRUCT(RimeStatus, status);
  static const std::wstring Bool_wstring[] = {L"0", L"1"};
  if (rime_api->get_status(session_id, &status)) {
    is_composing = !!status.is_composing;
    actions.push_back("status");
    body.append(L"status.ascii_mode=")
        .append(Bool_wstring[!!status.is_ascii_mode])
        .append(L"\n")
        .append(L"status.composing=")
        .append(Bool_wstring[!!status.is_composing])
        .append(L"\n")
        .append(L"status.disabled=")
        .append(Bool_wstring[!!status.is_disabled])
        .append(L"\n")
        .append(L"status.full_shape=")
        .append(Bool_wstring[!!status.is_full_shape])
        .append(L"\n")
        .append(L"status.schema_id=")
        .append(status.schema_id ? u8tow(status.schema_id) : std::wstring())
        .append(L"\n");
    if (m_global_ascii_mode &&
        (session_status.status.is_ascii_mode != status.is_ascii_mode)) {
      for (auto& pair : m_session_status_map) {
        if (pair.first != ipc_id)
          rime_api->set_option(to_session_id(pair.first), "ascii_mode",
                               !!status.is_ascii_mode);
      }
    }
    session_status.status = status;
    rime_api->free_status(&status);
  }

  RIME_STRUCT(RimeContext, ctx);
  if (rime_api->get_context(session_id, &ctx)) {
    bool has_candidates =
        ctx.menu.num_candidates > 0 || !session_status.llm_candidates.empty();
    CandidateInfo cinfo;
    if (has_candidates) {
      _GetCandidateInfo(cinfo, ctx, session_id);
    }
    _ResolveLayoutForSession(session_status, cinfo);
    if (is_composing) {
      const DisplayPreedit display_preedit =
          _GetDisplayPreedit(session_id, ctx.composition.preedit);
      const char* preedit = display_preedit.text.c_str();
      const int start = _MapDisplayPreeditPosition(ctx.composition.sel_start,
                                                   display_preedit);
      const int end =
          _MapDisplayPreeditPosition(ctx.composition.sel_end, display_preedit);
      const int cursor = _MapDisplayPreeditPosition(ctx.composition.cursor_pos,
                                                    display_preedit);
      static const auto u8towstring = [](const char* u8str, int len = 0) {
        return std::to_wstring(utf8towcslen(u8str, len));
      };
      actions.push_back("ctx");
      switch (session_status.style.preedit_type) {
        case UIStyle::PREVIEW: {
          if (ctx.commit_text_preview) {
            const char* first_utf8 = ctx.commit_text_preview;
            const size_t first_len = std::strlen(first_utf8);
            const std::wstring first_w = escape_string(u8tow(first_utf8));
            const std::wstring tmp = u8towstring(first_utf8, (int)first_len);
            body.append(L"ctx.preedit=")
                .append(first_w)
                .append(L"\n")
                .append(L"ctx.preedit.cursor=")
                .append(u8towstring(first_utf8, 0))
                .append(L",")
                .append(tmp)
                .append(L",")
                .append(tmp)
                .append(L"\n");
            break;
          }
          // no preview, fall back to composition
        }
        case UIStyle::COMPOSITION: {
          body.append(L"ctx.preedit=")
              .append(escape_string(u8tow(preedit)))
              .append(L"\n");
          if (start <= end) {
            body.append(L"ctx.preedit.cursor=")
                .append(u8towstring(preedit, start))
                .append(L",")
                .append(u8towstring(preedit, end))
                .append(L",")
                .append(u8towstring(preedit, cursor))
                .append(L"\n");
          }
          break;
        }
        case UIStyle::PREVIEW_ALL: {
          body.append(L"ctx.preedit=")
              .append(escape_string(u8tow(preedit)))
              .append(L"  [");
          auto label_valid = session_status.style.label_font_point > 0;
          auto comment_valid = session_status.style.comment_font_point > 0;
          const std::wstring mark_text_w =
              session_status.style.mark_text.empty()
                  ? std::wstring(L"*")
                  : session_status.style.mark_text;
          for (size_t i = 0; i < cinfo.candies.size(); i++) {
            std::wstring label_w;
            if (label_valid) {
              wchar_t buf_lbl[128];
              swprintf_s<128>(buf_lbl,
                              session_status.style.label_text_format.c_str(),
                              cinfo.labels.at(i).str.c_str());
              label_w = std::wstring(buf_lbl);
            }
            std::wstring comment_w =
                comment_valid ? cinfo.comments.at(i).str : std::wstring();
            std::wstring prefix_w = (static_cast<int>(i) != cinfo.highlighted)
                                        ? std::wstring()
                                        : mark_text_w;
            body.append(L" ")
                .append(prefix_w)
                .append(escape_string(label_w))
                .append(cinfo.candies.at(i).str)
                .append(L" ")
                .append(escape_string(comment_w));
          }
          body.append(L" ]\n");
          if (start <= end) {
            body.append(L"ctx.preedit.cursor=")
                .append(u8towstring(preedit, start))
                .append(L",")
                .append(u8towstring(preedit, end))
                .append(L",")
                .append(u8towstring(preedit, cursor))
                .append(L"\n");
          }
          break;
        }
      }
    }
    if (has_candidates) {
      std::wstringstream ss;
      boost::archive::text_woarchive oa(ss);

      oa << cinfo;

      auto s = ss.str();
      body.append(L"ctx.cand=").append(std::move(s)).append(L"\n");
      if (!cinfo.current_detail.empty()) {
        body.append(L"ctx.cand_detail=")
            .append(cinfo.current_detail.str)
            .append(L"\n");
      }
    }
    rime_api->free_context(&ctx);
  }

  if (session_status.llm_request_pending) {
    body.append(L"ctx.llm.request_id=")
        .append(escape_string(session_status.llm_request_id))
        .append(L"\n")
        .append(L"ctx.llm.context_enabled=")
        .append(Bool_wstring[session_status.llm_context_enabled])
        .append(L"\n")
        .append(L"ctx.llm.context_chars=")
        .append(std::to_wstring(session_status.llm_context_chars))
        .append(L"\n")
        .append(L"ctx.llm.boundary_search_chars=")
        .append(std::to_wstring(session_status.llm_boundary_search_chars))
        .append(L"\n");
    session_status.llm_request_pending = false;
  }

  // configuration information
  actions.push_back("config");
  body.append(L"config.inline_preedit=")
      .append(std::to_wstring((int)session_status.style.inline_preedit))
      .append(L"\n");

  // style
  if (!session_status.__synced) {
    std::wstringstream ss;
    boost::archive::text_woarchive oa(ss);
    oa << session_status.style;

    actions.push_back("style");
    body.append(L"style=").append(ss.str()).append(L"\n");
    session_status.__synced = true;
  }

  // summarize: send header first to avoid vector head-insert cost
  std::wstring header;
  if (actions.empty()) {
    header = L"action=noop\n";
  } else {
    std::string actionList;
    actionList.reserve(64);
    for (size_t i = 0; i < actions.size(); ++i) {
      if (i > 0)
        actionList += ',';
      actionList += actions[i];
    }
    header = std::wstring(L"action=") + u8tow(actionList) + L"\n";
  }
  if (!eat(header))
    return false;

  body.append(L".\n");
  if (!eat(body))
    return false;

  return true;
}

// Blend foreground and background ARGB colors taking alpha into account.
// Returns an ABGR COLORREF with premultiplied alpha blended result.
static inline COLORREF blend_colors(COLORREF fcolor, COLORREF bcolor) {
  // Extract ARGB channels from both colors.
  BYTE fA = (fcolor >> 24) & 0xFF;
  BYTE fB = (fcolor >> 16) & 0xFF;
  BYTE fG = (fcolor >> 8) & 0xFF;
  BYTE fR = fcolor & 0xFF;
  BYTE bA = (bcolor >> 24) & 0xFF;
  BYTE bB = (bcolor >> 16) & 0xFF;
  BYTE bG = (bcolor >> 8) & 0xFF;
  BYTE bR = bcolor & 0xFF;
  // Convert alpha to [0,1]
  float fAlpha = fA / 255.0f;
  float bAlpha = bA / 255.0f;
  // Result alpha
  float retAlpha = fAlpha + (1 - fAlpha) * bAlpha;
  if (retAlpha <= 1e-6f) {
    // Fully transparent result — return background unchanged as fallback.
    return bcolor;
  }
  auto mix = [&](float fc, float bc) -> BYTE {
    return static_cast<BYTE>((fc * fAlpha + bc * bAlpha * (1 - fAlpha)) /
                             retAlpha);
  };
  BYTE retR = mix(fR, bR);
  BYTE retG = mix(fG, bG);
  BYTE retB = mix(fB, bB);
  BYTE outA = static_cast<BYTE>(retAlpha * 255.0f);
  return (static_cast<COLORREF>(outA) << 24) | (retB << 16) | (retG << 8) |
         retR;
}
// parse color value, with fallback value
static Bool _RimeGetColor(RimeConfig* config,
                          const std::string& key,
                          int& value,
                          const ColorFormat& fmt,
                          const unsigned int& fallback) {
  char color[256] = {0};
  if (!rime_api->config_get_string(config, key.c_str(), color, 256)) {
    value = fallback;
    return False;
  }
  const auto color_str = std::string(color);
  // adjudge if str is 0x 0X # hex color format, return trimmed hex part
  // out part is 6 or 8 length hex string without white space
  const auto parse_color_code = [](const std::string& str, std::string& out) {
    if (str.empty())
      return false;
    size_t start = 0;
    if (str[0] == '#') {
      start = 1;
    } else if (str.size() >= 2 &&
               (str.compare(0, 2, "0x") == 0 || str.compare(0, 2, "0X") == 0)) {
      start = 2;
    } else {
      return false;
    }
    const std::string hex_part = str.substr(start);
    if (hex_part.empty())
      return false;
    if ((start == 1 || start == 2) && hex_part.length() != 3 &&
        hex_part.length() != 4 && hex_part.length() != 6 &&
        hex_part.length() != 8) {
      return false;
    }
    for (char c : hex_part) {
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')))
        return false;
    }
    out = str.substr(start).substr(0, 8);
#define _2C(c) std::string(2, c)
    if (out.size() == 3)
      out = _2C(out[0]) + _2C(out[1]) + _2C(out[2]);
    else if (out.size() == 4)
      out = _2C(out[0]) + _2C(out[1]) + _2C(out[2]) + _2C(out[3]);
#undef _2C
    return true;
  };
  auto hex_color = std::string();
  if (parse_color_code(color_str, hex_color)) {
    value = std::stoul(hex_color, 0, 16);
    if (hex_color.length() == 6)
      value = (fmt != COLOR_RGBA) ? (value | 0xff000000)
                                  : (((unsigned int)value << 8) | 0x000000ff);
  } else {
    if (!rime_api->config_get_int(config, key.c_str(), &value)) {
      value = fallback;
      return False;
    }
    if (value <= 0xffffff)
      value = (fmt != COLOR_RGBA) ? (value | 0xff000000)
                                  : (((unsigned int)value << 8) | 0x000000ff);
    else if (value > 0xffffffff)
      value &= 0xffffffff;
  }
  if (fmt == COLOR_ARGB)
    value = ARGB2ABGR(value);
  else if (fmt == COLOR_RGBA)
    value = RGBA2ABGR(value);
  value &= 0xffffffff;
  return True;
}

template <typename T, size_t N>
using Array = std::array<std::pair<const char*, T>, N>;

// parset bool type configuration to T type value trueValue / falseValue
template <typename T>
void _RimeGetBool(RimeConfig* config,
                  const char* key,
                  bool cond,
                  T& value,
                  const T& trueValue = true,
                  const T& falseValue = false) {
  Bool tempb = False;
  if (rime_api->config_get_bool(config, key, &tempb) || cond)
    value = (!!tempb) ? trueValue : falseValue;
}
// parse string option to T type value, with fallback
template <typename T, size_t N>
void _RimeParseStringOptWithFallback(RimeConfig* config,
                                     const char* key,
                                     T& value,
                                     const Array<T, N>& arr,
                                     const T& fallback) {
  char str_buff[256] = {0};
  if (rime_api->config_get_string(config, key, str_buff, 255)) {
    for (size_t i = 0; i < N; ++i) {
      if (strcmp(arr[i].first, str_buff) == 0) {
        value = arr[i].second;
        return;
      }
    }
  }
  value = fallback;
}

template <typename T>
void _RimeGetIntStr(RimeConfig* config,
                    const char* key,
                    T& value,
                    const char* fb_key = nullptr,
                    const void* fb_value = nullptr,
                    const std::function<void(T&)>& func = nullptr) {
  if constexpr (std::is_same<T, int>::value) {
    if (!rime_api->config_get_int(config, key, &value) && fb_key != 0)
      rime_api->config_get_int(config, fb_key, &value);
  } else if constexpr (std::is_same<T, std::wstring>::value) {
    const int BUF_SIZE = 2047;
    char buffer[BUF_SIZE + 1] = {0};
    if (rime_api->config_get_string(config, key, buffer, BUF_SIZE) ||
        rime_api->config_get_string(config, fb_key, buffer, BUF_SIZE)) {
      value = u8tow(buffer);
    } else if (fb_value) {
      value = *(T*)fb_value;
    }
  }
  if (func)
    func(value);
}

// Helper to iterate a Rime map and invoke callback with key/path
static void ForEachRimeMap(
    RimeConfig* config,
    const std::string& path,
    const std::function<void(const char* key, const char* child_path)>& cb) {
  RimeConfigIterator iter;
  if (!rime_api->config_begin_map(&iter, config, path.c_str()))
    return;
  while (rime_api->config_next(&iter)) {
    cb(iter.key, iter.path);
  }
  rime_api->config_end(&iter);
}

// Helper to iterate a Rime list and invoke callback with item path
static void ForEachRimeList(
    RimeConfig* config,
    const std::string& path,
    const std::function<void(const char* item_path)>& cb) {
  RimeConfigIterator iter;
  if (!rime_api->config_begin_list(&iter, config, path.c_str()))
    return;
  while (rime_api->config_next(&iter)) {
    cb(iter.path);
  }
  rime_api->config_end(&iter);
}

void RimeWithWeaselHandler::_UpdateShowNotifications(RimeConfig* config,
                                                     bool initialize) {
  Bool show_notifications = true;
  if (initialize)
    m_show_notifications_base.clear();
  m_show_notifications.clear();

  if (rime_api->config_get_bool(config, "show_notifications",
                                &show_notifications)) {
    // config read as bool, for global all on or off
    if (show_notifications)
      m_show_notifications["always"] = true;
    if (initialize)
      m_show_notifications_base = m_show_notifications;
  } else {
    // read as list using helper
    ForEachRimeList(config, "show_notifications", [&](const char* item_path) {
      char buffer[256] = {0};
      if (rime_api->config_get_string(config, item_path, buffer, 256))
        m_show_notifications[std::string(buffer)] = true;
    });
    if (initialize)
      m_show_notifications_base = m_show_notifications;
    if (m_show_notifications.empty()) {
      // not configured, or incorrect type
      if (initialize)
        m_show_notifications_base["always"] = true;
      m_show_notifications = m_show_notifications_base;
    }
  }
}

void RimeWithWeaselHandler::_LoadDynamicLayoutConfig(
    RimeConfig* config,
    weasel::DynamicLayoutConfig& dlc,
    UIStyle::LayoutType configured_type) {
  dlc.rules.clear();
  dlc.enabled = (configured_type == UIStyle::LAYOUT_AUTO);
  dlc.navigation = _DefaultCandidateNavigationConfig();
  weasel::CandidateLayout default_layout = weasel::CandidateLayout::Horizontal;
  constexpr int BUF_SIZE = 255;
  char buffer[BUF_SIZE + 1] = {0};
  Bool navigation_enabled = False;
  if (rime_api->config_get_bool(config, "dynamic_layout/navigation/enabled",
                                &navigation_enabled)) {
    dlc.navigation.configured = true;
    dlc.navigation.enabled = !!navigation_enabled;
  }

  const auto load_navigation_binding =
      [config, &dlc, &buffer](const char* path,
                              CandidateNavigationKeyBinding& binding) {
        if (!rime_api->config_get_string(config, path, buffer,
                                         static_cast<int>(sizeof(buffer) - 1)))
          return;
        dlc.navigation.configured = true;
        if (!_ParseCandidateNavigationKey(buffer, binding)) {
          LOG(WARNING) << "DynamicLayout: invalid navigation key '" << buffer
                       << "' at " << path << ", binding disabled";
          binding = CandidateNavigationKeyBinding{};
        }
      };
  load_navigation_binding(
      "dynamic_layout/navigation/horizontal/previous_candidate",
      dlc.navigation.horizontal.previous_candidate);
  load_navigation_binding("dynamic_layout/navigation/horizontal/next_candidate",
                          dlc.navigation.horizontal.next_candidate);
  load_navigation_binding("dynamic_layout/navigation/horizontal/previous_page",
                          dlc.navigation.horizontal.previous_page);
  load_navigation_binding("dynamic_layout/navigation/horizontal/next_page",
                          dlc.navigation.horizontal.next_page);
  load_navigation_binding(
      "dynamic_layout/navigation/vertical/previous_candidate",
      dlc.navigation.vertical.previous_candidate);
  load_navigation_binding("dynamic_layout/navigation/vertical/next_candidate",
                          dlc.navigation.vertical.next_candidate);
  load_navigation_binding("dynamic_layout/navigation/vertical/previous_page",
                          dlc.navigation.vertical.previous_page);
  load_navigation_binding("dynamic_layout/navigation/vertical/next_page",
                          dlc.navigation.vertical.next_page);

  if (rime_api->config_get_string(config, "dynamic_layout/default", buffer,
                                  BUF_SIZE)) {
    if (strcmp(buffer, "vertical") == 0) {
      default_layout = weasel::CandidateLayout::Vertical;
    } else if (strcmp(buffer, "horizontal") == 0) {
      default_layout = weasel::CandidateLayout::Horizontal;
    } else {
      LOG(WARNING) << "DynamicLayout: unknown default layout '" << buffer
                   << "', falling back to horizontal";
    }
  }
  dlc.default_layout = default_layout;

  if (!dlc.enabled) {
    return;
  }

  ForEachRimeList(config, "dynamic_layout/rules", [&](const char* item_path) {
    weasel::DynamicLayoutRule rule;
    constexpr int RULE_BUF_SIZE = 255;
    char val_buf[RULE_BUF_SIZE + 1] = {0};

    // Layout target (required for a valid rule)
    std::string layout_path = std::string(item_path) + "/layout";
    if (!rime_api->config_get_string(config, layout_path.c_str(), val_buf,
                                     RULE_BUF_SIZE)) {
      LOG(WARNING) << "DynamicLayout: rule at " << item_path
                   << " missing 'layout' field, ignored";
      return;
    }
    if (strcmp(val_buf, "vertical") == 0) {
      rule.target_layout = weasel::CandidateLayout::Vertical;
    } else if (strcmp(val_buf, "horizontal") == 0) {
      rule.target_layout = weasel::CandidateLayout::Horizontal;
    } else {
      LOG(WARNING) << "DynamicLayout: unknown rule layout '" << val_buf
                   << "' at " << item_path << ", ignored";
      return;
    }

    // 1. Check option rule
    std::string option_path = std::string(item_path) + "/option";
    if (rime_api->config_get_string(config, option_path.c_str(), val_buf,
                                    RULE_BUF_SIZE)) {
      rule.type = weasel::LayoutRuleType::Option;
      rule.option_name = val_buf;
      Bool bool_val = True;
      std::string value_path = std::string(item_path) + "/value";
      if (rime_api->config_get_bool(config, value_path.c_str(), &bool_val)) {
        rule.option_value = !!bool_val;
      } else {
        rule.option_value = true;
      }
      dlc.rules.push_back(rule);
      return;
    }

    // 2. Check candidate_max_text_length_gt rule
    std::string max_len_path =
        std::string(item_path) + "/candidate_max_text_length_gt";
    int int_val = 0;
    if (rime_api->config_get_int(config, max_len_path.c_str(), &int_val)) {
      if (int_val >= 0) {
        rule.type = weasel::LayoutRuleType::CandidateMaxTextLengthGt;
        rule.threshold = int_val;
        dlc.rules.push_back(rule);
        return;
      } else {
        LOG(WARNING) << "DynamicLayout: negative threshold for "
                        "candidate_max_text_length_gt at "
                     << item_path << ", ignored";
        return;
      }
    }

    // 3. Check candidate_count_gt rule
    std::string count_path = std::string(item_path) + "/candidate_count_gt";
    if (rime_api->config_get_int(config, count_path.c_str(), &int_val)) {
      if (int_val >= 0) {
        rule.type = weasel::LayoutRuleType::CandidateCountGt;
        rule.threshold = int_val;
        dlc.rules.push_back(rule);
        return;
      } else {
        LOG(WARNING)
            << "DynamicLayout: negative threshold for candidate_count_gt at "
            << item_path << ", ignored";
        return;
      }
    }

    LOG(WARNING)
        << "DynamicLayout: rule at " << item_path
        << " has no recognized condition (option, "
           "candidate_max_text_length_gt, candidate_count_gt), ignored";
  });
}

void RimeWithWeaselHandler::_ResolveLayoutForSession(
    SessionStatus& session_status,
    const weasel::CandidateInfo& cinfo) {
  if (session_status.configured_layout_type != UIStyle::LAYOUT_AUTO) {
    return;
  }
  RimeSessionId session_id = session_status.session_id;
  auto option_getter = [this, session_id](const std::string& opt) -> bool {
    return rime_api->get_option(session_id, opt.c_str()) != 0;
  };

  weasel::CandidateLayout resolved = weasel::ResolveCandidateLayout(
      cinfo, session_status.dynamic_layout_config.default_layout,
      session_status.dynamic_layout_config, option_getter);

  UIStyle::LayoutType target_type = UIStyle::LAYOUT_HORIZONTAL;
  if (resolved == weasel::CandidateLayout::Vertical) {
    target_type = session_status.fullscreen
                      ? UIStyle::LAYOUT_VERTICAL_FULLSCREEN
                      : UIStyle::LAYOUT_VERTICAL;
  } else {
    target_type = session_status.fullscreen
                      ? UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN
                      : UIStyle::LAYOUT_HORIZONTAL;
  }

  if (session_status.style.layout_type != target_type) {
    DLOG(INFO) << "DynamicLayout: layout changed from "
               << session_status.style.layout_type << " to " << target_type;
    session_status.style.layout_type = target_type;
    session_status.__synced = false;
    if (m_ui) {
      m_ui->style().layout_type = target_type;
    }
  }
}

void RimeWithWeaselHandler::_RemapCandidateNavigationKey(
    SessionStatus& session_status,
    RimeSessionId session_id,
    KeyEvent& key_event) {
  const auto layout_type = session_status.style.layout_type;
  const bool vertical = layout_type == UIStyle::LAYOUT_VERTICAL ||
                        layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN ||
                        layout_type == UIStyle::LAYOUT_VERTICAL_TEXT;
  const auto layout = vertical ? weasel::CandidateLayout::Vertical
                               : weasel::CandidateLayout::Horizontal;
  constexpr uint32_t kNavigationModifiers =
      ibus::SHIFT_MASK | ibus::CONTROL_MASK | ibus::ALT_MASK |
      ibus::SUPER_MASK | ibus::HYPER_MASK | ibus::META_MASK;
  const auto action = weasel::ResolveCandidateNavigation(
      layout, session_status.dynamic_layout_config.navigation,
      key_event.keycode, key_event.mask & kNavigationModifiers);
  if (action == weasel::CandidateNavigationAction::PassThrough)
    return;

  RIME_STRUCT(RimeContext, context);
  if (!rime_api->get_context(session_id, &context))
    return;
  const bool has_candidates = context.menu.num_candidates > 0;
  rime_api->free_context(&context);
  if (!has_candidates)
    return;

  switch (action) {
    case weasel::CandidateNavigationAction::PreviousCandidate:
      key_event.keycode = ibus::Up;
      key_event.mask &= ~kNavigationModifiers;
      break;
    case weasel::CandidateNavigationAction::NextCandidate:
      key_event.keycode = ibus::Down;
      key_event.mask &= ~kNavigationModifiers;
      break;
    case weasel::CandidateNavigationAction::PreviousPage:
      key_event.keycode = ibus::Prior;
      key_event.mask &= ~kNavigationModifiers;
      break;
    case weasel::CandidateNavigationAction::NextPage:
      key_event.keycode = ibus::Next;
      key_event.mask &= ~kNavigationModifiers;
      break;
    case weasel::CandidateNavigationAction::PassThrough:
      break;
  }
}

// update ui's style parameters, ui has been check before referenced
static void _UpdateUIStyle(RimeConfig* config, UI* ui, bool initialize) {
  UIStyle& style(ui->style());
  const std::function<void(std::wstring&)> rmspace = [](std::wstring& str) {
    str = std::regex_replace(str, std::wregex(L"\\s*(,|:|^|$)\\s*"), L"$1");
  };
  const std::function<void(int&)> _abs = [](int& value) { value = abs(value); };
  // get font faces
  _RimeGetIntStr(config, "style/font_face", style.font_face, 0, 0, rmspace);
  std::wstring* const pFallbackFontFace = initialize ? &style.font_face : NULL;
  _RimeGetIntStr(config, "style/label_font_face", style.label_font_face, 0,
                 pFallbackFontFace, rmspace);
  _RimeGetIntStr(config, "style/comment_font_face", style.comment_font_face, 0,
                 pFallbackFontFace, rmspace);
  // able to set label font/comment font empty, force fallback to font face.
  if (style.label_font_face.empty())
    style.label_font_face = style.font_face;
  if (style.comment_font_face.empty())
    style.comment_font_face = style.font_face;
  // get font points
  _RimeGetIntStr(config, "style/font_point", style.font_point);
  if (style.font_point <= 0)
    style.font_point = 12;
  _RimeGetIntStr(config, "style/label_font_point", style.label_font_point,
                 "style/font_point", 0, _abs);
  _RimeGetIntStr(config, "style/comment_font_point", style.comment_font_point,
                 "style/font_point", 0, _abs);
  _RimeGetIntStr(config, "style/candidate_abbreviate_length",
                 style.candidate_abbreviate_length, 0, 0, _abs);
  _RimeGetBool(config, "style/inline_preedit", initialize,
               style.inline_preedit);
  _RimeGetBool(config, "style/vertical_auto_reverse", initialize,
               style.vertical_auto_reverse);
  static constexpr Array<UIStyle::PreeditType, 3> _preeditArr = {
      {{"composition", UIStyle::COMPOSITION},
       {"preview", UIStyle::PREVIEW},
       {"preview_all", UIStyle::PREVIEW_ALL}}};
  _RimeParseStringOptWithFallback(config, "style/preedit_type",
                                  style.preedit_type, _preeditArr,
                                  style.preedit_type);
  static constexpr Array<UIStyle::AntiAliasMode, 5> _aliasModeArr = {
      {{"force_dword", UIStyle::FORCE_DWORD},
       {"cleartype", UIStyle::CLEARTYPE},
       {"grayscale", UIStyle::GRAYSCALE},
       {"aliased", UIStyle::ALIASED},
       {"default", UIStyle::DEFAULT}}};
  _RimeParseStringOptWithFallback(config, "style/antialias_mode",
                                  style.antialias_mode, _aliasModeArr,
                                  style.antialias_mode);
  static constexpr Array<UIStyle::HoverType, 3> _hoverTypeArr = {
      {{"none", UIStyle::HoverType::NONE},
       {"semi_hilite", UIStyle::HoverType::SEMI_HILITE},
       {"hilite", UIStyle::HoverType::HILITE}}};
  _RimeParseStringOptWithFallback(config, "style/hover_type", style.hover_type,
                                  _hoverTypeArr, style.hover_type);
  static constexpr Array<UIStyle::LayoutAlignType, 3> _alignType = {
      {{"top", UIStyle::ALIGN_TOP},
       {"center", UIStyle::ALIGN_CENTER},
       {"bottom", UIStyle::ALIGN_BOTTOM}}};
  _RimeParseStringOptWithFallback(config, "style/layout/align_type",
                                  style.align_type, _alignType,
                                  style.align_type);
  _RimeGetBool(config, "style/display_tray_icon", initialize,
               style.display_tray_icon);
  _RimeGetBool(config, "style/ascii_tip_follow_cursor", initialize,
               style.ascii_tip_follow_cursor);
  _RimeGetBool(config, "style/horizontal", initialize, style.layout_type,
               UIStyle::LAYOUT_HORIZONTAL, UIStyle::LAYOUT_VERTICAL);
  _RimeGetBool(config, "style/paging_on_scroll", initialize,
               style.paging_on_scroll);
  _RimeGetBool(config, "style/click_to_capture", initialize,
               style.click_to_capture, true, false);
  _RimeGetBool(config, "style/fullscreen", false, style.layout_type,
               ((style.layout_type == UIStyle::LAYOUT_HORIZONTAL)
                    ? UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN
                    : UIStyle::LAYOUT_VERTICAL_FULLSCREEN),
               style.layout_type);
  _RimeGetBool(config, "style/vertical_text", false, style.layout_type,
               UIStyle::LAYOUT_VERTICAL_TEXT, style.layout_type);
  _RimeGetBool(config, "style/vertical_text_left_to_right", false,
               style.vertical_text_left_to_right);
  _RimeGetBool(config, "style/vertical_text_with_wrap", false,
               style.vertical_text_with_wrap);
  static constexpr Array<bool, 2> _text_orientation = {
      {{"horizontal", false}, {"vertical", true}}};
  bool _text_orientation_bool = false;
  _RimeParseStringOptWithFallback(config, "style/text_orientation",
                                  _text_orientation_bool, _text_orientation,
                                  _text_orientation_bool);
  if (_text_orientation_bool)
    style.layout_type = UIStyle::LAYOUT_VERTICAL_TEXT;
  _RimeGetIntStr(config, "style/label_format", style.label_text_format);
  _RimeGetIntStr(config, "style/mark_text", style.mark_text);
  _RimeGetIntStr(config, "style/layout/baseline", style.baseline, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/linespacing", style.linespacing, 0, 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/min_width", style.min_width, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/max_width", style.max_width, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/min_height", style.min_height, 0, 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/max_height", style.max_height, 0, 0,
                 _abs);
  // layout (alternative to style/horizontal)
  static constexpr Array<UIStyle::LayoutType, 6> _layoutArr = {
      {{"vertical", UIStyle::LAYOUT_VERTICAL},
       {"horizontal", UIStyle::LAYOUT_HORIZONTAL},
       {"vertical_text", UIStyle::LAYOUT_VERTICAL_TEXT},
       {"vertical+fullscreen", UIStyle::LAYOUT_VERTICAL_FULLSCREEN},
       {"horizontal+fullscreen", UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN},
       {"auto", UIStyle::LAYOUT_AUTO}}};
  _RimeParseStringOptWithFallback(config, "style/layout/type",
                                  style.layout_type, _layoutArr,
                                  style.layout_type);
  // disable max_width when full screen
  if (style.layout_type == UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN ||
      style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN) {
    style.max_width = 0;
    style.inline_preedit = false;
  }
  _RimeGetIntStr(config, "style/layout/border", style.border,
                 "style/layout/border_width", 0, _abs);
  _RimeGetIntStr(config, "style/layout/margin_x", style.margin_x);
  _RimeGetIntStr(config, "style/layout/margin_y", style.margin_y);
  _RimeGetIntStr(config, "style/layout/spacing", style.spacing, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/candidate_spacing",
                 style.candidate_spacing, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/hilite_spacing", style.hilite_spacing, 0,
                 0, _abs);
  _RimeGetIntStr(config, "style/layout/hilite_padding_x",
                 style.hilite_padding_x, "style/layout/hilite_padding", 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/hilite_padding_y",
                 style.hilite_padding_y, "style/layout/hilite_padding", 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/shadow_radius", style.shadow_radius, 0,
                 0, _abs);
  // disable shadow for fullscreen layout
  style.shadow_radius *=
      (!(style.layout_type == UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN ||
         style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN));
  _RimeGetIntStr(config, "style/layout/shadow_offset_x", style.shadow_offset_x);
  _RimeGetIntStr(config, "style/layout/shadow_offset_y", style.shadow_offset_y);
  // round_corner as alias of hilited_corner_radius
  _RimeGetIntStr(config, "style/layout/hilited_corner_radius",
                 style.round_corner, "style/layout/round_corner", 0, _abs);
  // corner_radius not set, fallback to round_corner
  _RimeGetIntStr(config, "style/layout/corner_radius", style.round_corner_ex,
                 "style/layout/round_corner", 0, _abs);
  // fix padding and spacing settings
  if (style.layout_type != UIStyle::LAYOUT_VERTICAL_TEXT) {
    // hilite_padding vs spacing
    // if hilite_padding over spacing, increase spacing
    style.spacing = max(style.spacing, style.hilite_padding_y * 2);
    // hilite_padding vs candidate_spacing
    if (style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN ||
        style.layout_type == UIStyle::LAYOUT_VERTICAL) {
      // vertical, if hilite_padding_y over candidate spacing,
      // increase candidate spacing
      style.candidate_spacing =
          max(style.candidate_spacing, style.hilite_padding_y * 2);
    } else {
      // horizontal, if hilite_padding_x over candidate
      // spacing, increase candidate spacing
      style.candidate_spacing =
          max(style.candidate_spacing, style.hilite_padding_x * 2);
    }
    // hilite_padding_x vs hilite_spacing
    if (!style.inline_preedit)
      style.hilite_spacing = max(style.hilite_spacing, style.hilite_padding_x);
  } else  // LAYOUT_VERTICAL_TEXT
  {
    // hilite_padding_x vs spacing
    // if hilite_padding over spacing, increase spacing
    style.spacing = max(style.spacing, style.hilite_padding_x * 2);
    // hilite_padding vs candidate_spacing
    // if hilite_padding_x over candidate
    // spacing, increase candidate spacing
    style.candidate_spacing =
        max(style.candidate_spacing, style.hilite_padding_x * 2);
    // vertical_text_with_wrap and hilite_padding_y over candidate_spacing
    if (style.vertical_text_with_wrap)
      style.candidate_spacing =
          max(style.candidate_spacing, style.hilite_padding_y * 2);
    // hilite_padding_y vs hilite_spacing
    if (!style.inline_preedit)
      style.hilite_spacing = max(style.hilite_spacing, style.hilite_padding_y);
  }
  // fix padding and margin settings
  int scale = style.margin_x < 0 ? -1 : 1;
  style.margin_x = scale * max(style.hilite_padding_x, abs(style.margin_x));
  scale = style.margin_y < 0 ? -1 : 1;
  style.margin_y = scale * max(style.hilite_padding_y, abs(style.margin_y));
  // get enhanced_position
  _RimeGetBool(config, "style/enhanced_position", initialize,
               style.enhanced_position, true, false);
  // get candidate_detail_panel
  _RimeGetBool(config, "style/candidate_detail_panel/enabled", initialize,
               style.detail_enabled);
  static constexpr Array<UIStyle::DetailPosition, 5> _detailPosArr = {
      {{"right", UIStyle::DETAIL_POS_RIGHT},
       {"left", UIStyle::DETAIL_POS_LEFT},
       {"top", UIStyle::DETAIL_POS_TOP},
       {"bottom", UIStyle::DETAIL_POS_BOTTOM},
       {"auto", UIStyle::DETAIL_POS_AUTO}}};
  _RimeParseStringOptWithFallback(
      config, "style/candidate_detail_panel/position", style.detail_position,
      _detailPosArr, style.detail_position);
  _RimeGetIntStr(config, "style/candidate_detail_panel/gap", style.detail_gap,
                 0, 0, _abs);
  _RimeGetIntStr(config, "style/candidate_detail_panel/width",
                 style.detail_width, 0, 0, _abs);
  _RimeGetIntStr(config, "style/candidate_detail_panel/min_width",
                 style.detail_min_width, 0, 0, _abs);
  _RimeGetIntStr(config, "style/candidate_detail_panel/max_width",
                 style.detail_max_width, 0, 0, _abs);
  _RimeGetIntStr(config, "style/candidate_detail_panel/max_lines",
                 style.detail_max_lines, 0, 0, _abs);
  _RimeGetIntStr(config, "style/candidate_detail_panel/padding_x",
                 style.detail_padding_x, 0, 0, _abs);
  _RimeGetIntStr(config, "style/candidate_detail_panel/padding_y",
                 style.detail_padding_y, 0, 0, _abs);
  _RimeGetIntStr(config, "style/candidate_detail_panel/font_face",
                 style.detail_font_face, 0, 0, rmspace);
  _RimeGetIntStr(config, "style/candidate_detail_panel/han_font_face",
                 style.detail_han_font_face, 0, 0, rmspace);
  _RimeGetIntStr(config, "style/candidate_detail_panel/latin_font_face",
                 style.detail_latin_font_face, 0, 0, rmspace);
  _RimeGetIntStr(config, "style/candidate_detail_panel/font_point",
                 style.detail_font_point, 0, 0, _abs);
  _RimeGetIntStr(config, "style/candidate_detail_panel/corner_radius",
                 style.detail_corner_radius, 0, 0, _abs);
  _RimeGetIntStr(config, "style/candidate_detail_panel/border_width",
                 style.detail_border_width, 0, 0, _abs);
  _RimeGetIntStr(config, "style/candidate_detail_panel/linespacing",
                 style.detail_linespacing, 0, 0, _abs);
  _RimeGetBool(config, "style/candidate_detail_panel/draw_line_separators",
               initialize, style.detail_draw_line_separators, false);
  // get color scheme
  const int BUF_SIZE = 255;
  char buffer[BUF_SIZE + 1] = {0};
  if (initialize && rime_api->config_get_string(config, "style/color_scheme",
                                                buffer, BUF_SIZE))
    _UpdateUIStyleColor(config, style);
}
// load color configs to style, by "style/color_scheme" or specific scheme name
// "color" which is default empty
static bool _UpdateUIStyleColor(RimeConfig* config,
                                UIStyle& style,
                                const std::string& color) {
  const int BUF_SIZE = 255;
  char buffer[BUF_SIZE + 1] = {0};
  std::string color_mark = "style/color_scheme";
  // color scheme
  if (rime_api->config_get_string(config, color_mark.c_str(), buffer,
                                  BUF_SIZE) ||
      !color.empty()) {
    std::string prefix("preset_color_schemes/");
    prefix += (color.empty()) ? buffer : color;
    // define color format, default abgr if not set
    ColorFormat fmt = COLOR_ABGR;
    static constexpr Array<ColorFormat, 3> _colorFmt = {
        {{"argb", COLOR_ARGB}, {"rgba", COLOR_RGBA}, {"abgr", COLOR_ABGR}}};
    _RimeParseStringOptWithFallback(config, (prefix + "/color_format").c_str(),
                                    fmt, _colorFmt, COLOR_ABGR);
#define COLOR(key, value, fallback) \
  _RimeGetColor(config, (prefix + "/" + key), value, fmt, fallback)
    COLOR("back_color", style.back_color, 0xffffffff);
    COLOR("shadow_color", style.shadow_color, 0);
    COLOR("prevpage_color", style.prevpage_color, 0);
    COLOR("nextpage_color", style.nextpage_color, 0);
    COLOR("text_color", style.text_color, 0xff000000);
    COLOR("candidate_text_color", style.candidate_text_color, style.text_color);
    COLOR("candidate_back_color", style.candidate_back_color, 0);
    COLOR("border_color", style.border_color, style.text_color);
    COLOR("hilited_text_color", style.hilited_text_color, style.text_color);
    COLOR("hilited_back_color", style.hilited_back_color, style.back_color);
    COLOR("hilited_candidate_text_color", style.hilited_candidate_text_color,
          style.hilited_text_color);
    COLOR("hilited_candidate_back_color", style.hilited_candidate_back_color,
          style.hilited_back_color);
    COLOR("hilited_candidate_shadow_color",
          style.hilited_candidate_shadow_color, 0);
    COLOR("hilited_shadow_color", style.hilited_shadow_color, 0);
    COLOR("candidate_shadow_color", style.candidate_shadow_color, 0);
    COLOR("candidate_border_color", style.candidate_border_color, 0);
    COLOR("hilited_candidate_border_color",
          style.hilited_candidate_border_color, 0);
    COLOR("label_color", style.label_text_color,
          blend_colors(style.candidate_text_color, style.candidate_back_color));
    COLOR("hilited_label_color", style.hilited_label_text_color,
          blend_colors(style.hilited_candidate_text_color,
                       style.hilited_candidate_back_color));
    COLOR("comment_text_color", style.comment_text_color,
          style.label_text_color);
    COLOR("hilited_comment_text_color", style.hilited_comment_text_color,
          style.hilited_label_text_color);
    COLOR("hilited_mark_color", style.hilited_mark_color, 0);
    COLOR("candidate_detail_text_color", style.detail_text_color,
          style.comment_text_color ? style.comment_text_color
                                   : style.candidate_text_color);
    COLOR("candidate_detail_back_color", style.detail_back_color,
          style.candidate_back_color ? style.candidate_back_color
                                     : style.back_color);
    COLOR("candidate_detail_border_color", style.detail_border_color,
          style.candidate_border_color ? style.candidate_border_color
                                       : style.border_color);
    COLOR("candidate_detail_shadow_color", style.detail_shadow_color,
          style.shadow_color);
    COLOR("candidate_detail_line_separator_color",
          style.detail_line_separator_color,
          blend_colors(style.detail_border_color ? style.detail_border_color
                                                 : style.border_color,
                       style.detail_back_color ? style.detail_back_color
                                               : style.back_color));
    COLOR("candidate_detail_key_text_color", style.detail_key_text_color,
          blend_colors(style.detail_text_color, style.detail_back_color
                                                    ? style.detail_back_color
                                                    : style.back_color));
    COLOR("candidate_detail_emphasis_text_color",
          style.detail_emphasis_text_color,
          style.hilited_candidate_back_color
              ? style.hilited_candidate_back_color
              : style.hilited_back_color);
#undef COLOR
    return true;
  }
  return false;
}
static void _LoadAppOptions(RimeConfig* config,
                            AppOptionsByAppName& app_options) {
  app_options.clear();
  ForEachRimeMap(
      config, "app_options", [&](const char* app_key, const char* app_path) {
        AppOptions& options(app_options[app_key]);
        ForEachRimeMap(
            config, app_path, [&](const char* opt_key, const char* opt_path) {
              Bool value = False;
              if (rime_api->config_get_bool(config, opt_path, &value)) {
                options[opt_key] = !!value;
              }
            });
      });
}

void RimeWithWeaselHandler::_GetStatus(Status& stat,
                                       WeaselSessionId ipc_id,
                                       Context& ctx) {
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  RIME_STRUCT(RimeStatus, status);
  if (rime_api->get_status(session_id, &status)) {
    std::string schema_id = "";
    if (status.schema_id)
      schema_id = status.schema_id;
    stat.schema_name = u8tow(status.schema_name);
    stat.schema_id = u8tow(status.schema_id);
    stat.ascii_mode = !!status.is_ascii_mode;
    stat.composing = !!status.is_composing;
    stat.disabled = !!status.is_disabled;
    stat.full_shape = !!status.is_full_shape;
    if (schema_id != m_last_schema_id) {
      session_status.__synced = false;
      m_last_schema_id = schema_id;
      if (schema_id != ".default") {  // don't load for schema select menu
        bool inline_preedit = session_status.style.inline_preedit;
        _LoadSchemaSpecificSettings(ipc_id, schema_id);
        _LoadAppInlinePreeditSet(ipc_id, true);
        if (session_status.style.inline_preedit != inline_preedit)
          // in case of inline_preedit set in schema
          _UpdateInlinePreeditStatus(ipc_id);
        // refresh icon after schema changed
        _RefreshTrayIcon(session_id, _UpdateUICallback);
        m_ui->style() = session_status.style;
        if (m_show_notifications.find("schema") != m_show_notifications.end() &&
            m_show_notifications_time > 0) {
          ctx.aux.str = stat.schema_name;
          m_ui->Update(ctx, stat);
          m_ui->ShowWithTimeout(m_show_notifications_time);
        }
      }
    }
    rime_api->free_status(&status);
  }
}

void RimeWithWeaselHandler::_GetContext(Context& weasel_context,
                                        RimeSessionId session_id) {
  RIME_STRUCT(RimeContext, ctx);
  if (rime_api->get_context(session_id, &ctx)) {
    if (ctx.composition.length > 0) {
      const DisplayPreedit display_preedit =
          _GetDisplayPreedit(session_id, ctx.composition.preedit);
      weasel_context.preedit.str = u8tow(display_preedit.text);
      if (ctx.composition.sel_start < ctx.composition.sel_end) {
        TextAttribute attr;
        attr.type = HIGHLIGHTED;
        attr.range.start =
            utf8towcslen(display_preedit.text.c_str(),
                         _MapDisplayPreeditPosition(ctx.composition.sel_start,
                                                    display_preedit));
        attr.range.end =
            utf8towcslen(display_preedit.text.c_str(),
                         _MapDisplayPreeditPosition(ctx.composition.sel_end,
                                                    display_preedit));

        weasel_context.preedit.attributes.push_back(attr);
      }
    }
    if (ctx.menu.num_candidates) {
      CandidateInfo& cinfo(weasel_context.cinfo);
      _GetCandidateInfo(cinfo, ctx, session_id);
    }
    rime_api->free_context(&ctx);
  }
}

void RimeWithWeaselHandler::_UpdateInlinePreeditStatus(WeaselSessionId ipc_id) {
  if (!m_ui)
    return;
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  // set inline_preedit option
  bool inline_preedit = session_status.style.inline_preedit;
  rime_api->set_option(session_id, "inline_preedit", Bool(inline_preedit));
  // show soft cursor on weasel panel but not inline
  rime_api->set_option(session_id, "soft_cursor", Bool(!inline_preedit));
}
