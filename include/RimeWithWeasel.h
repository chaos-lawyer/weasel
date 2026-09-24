#pragma once
#include <WeaselIPC.h>
#include <WeaselUI.h>
#include <map>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>

#include <DynamicCandidateLayout.h>
#include <DynamicCandidateSelectKeys.h>
#include <rime_api.h>

struct CaseInsensitiveCompare {
  bool operator()(const std::string& str1, const std::string& str2) const {
    std::string str1Lower, str2Lower;
    std::transform(str1.begin(), str1.end(), std::back_inserter(str1Lower),
                   [](char c) { return std::tolower(c); });
    std::transform(str2.begin(), str2.end(), std::back_inserter(str2Lower),
                   [](char c) { return std::tolower(c); });
    return str1Lower < str2Lower;
  }
};

typedef std::map<std::string, bool> AppOptions;
typedef std::map<std::string, AppOptions, CaseInsensitiveCompare>
    AppOptionsByAppName;

struct LlmCandidateItem {
  std::wstring text;
  std::wstring comment;
};

struct SessionStatus {
  SessionStatus()
      : style(weasel::UIStyle()),
        configured_layout_type(weasel::UIStyle::LAYOUT_VERTICAL),
        fullscreen(false),
        __synced(false),
        session_id(0),
        llm_generation(0),
        llm_context_enabled(false),
        llm_context_chars(0),
        llm_boundary_search_chars(0),
        llm_request_pending(false),
        llm_request_submitted(false),
        llm_loading(false),
        llm_is_error(false),
        llm_ai_comment_enabled(false),
        llm_ai_comment() {
    RIME_STRUCT(RimeStatus, status);
  }
  weasel::UIStyle style;
  weasel::UIStyle::LayoutType configured_layout_type;
  bool fullscreen;
  weasel::DynamicLayoutConfig dynamic_layout_config;
  RimeStatus status;
  bool __synced;
  RimeSessionId session_id;
  uint64_t llm_generation;
  std::wstring llm_request_id;
  std::wstring llm_config_path;
  std::string llm_raw_input;
  std::string llm_schema_id;
  bool llm_context_enabled;
  int llm_context_chars;
  int llm_boundary_search_chars;
  bool llm_request_pending;
  bool llm_request_submitted;
  bool llm_loading;
  bool llm_is_error;
  bool llm_ai_comment_enabled;
  std::wstring llm_ai_comment;
  std::wstring llm_context;
  std::vector<LlmCandidateItem> llm_candidates;
  std::wstring llm_commit_text;
};
typedef std::map<DWORD, SessionStatus> SessionStatusMap;
typedef DWORD WeaselSessionId;
class RimeWithWeaselHandler : public weasel::RequestHandler {
 public:
  RimeWithWeaselHandler(weasel::UI* ui);
  virtual ~RimeWithWeaselHandler();
  virtual void Initialize();
  virtual void Finalize();
  virtual DWORD FindSession(WeaselSessionId ipc_id);
  virtual DWORD AddSession(LPWSTR buffer, EatLine eat = 0);
  virtual DWORD RemoveSession(WeaselSessionId ipc_id);
  virtual BOOL ProcessKeyEvent(weasel::KeyEvent keyEvent,
                               WeaselSessionId ipc_id,
                               EatLine eat);
  virtual void CommitComposition(WeaselSessionId ipc_id);
  virtual void ClearComposition(WeaselSessionId ipc_id);
  virtual void SelectCandidateOnCurrentPage(size_t index,
                                            WeaselSessionId ipc_id);
  virtual bool HighlightCandidateOnCurrentPage(size_t index,
                                               WeaselSessionId ipc_id,
                                               EatLine eat);
  virtual bool ChangePage(bool backward, WeaselSessionId ipc_id, EatLine eat);
  virtual void FocusIn(DWORD param, WeaselSessionId ipc_id);
  virtual void FocusOut(DWORD param, WeaselSessionId ipc_id);
  virtual void SubmitLlmContext(WeaselSessionId ipc_id,
                                const std::wstring& request_id,
                                const std::wstring& context,
                                EatLine eat = 0);
  virtual bool PollSession(WeaselSessionId ipc_id, EatLine eat = 0);
  virtual void RefreshSession(DWORD ipc_id);
  virtual void UpdateInputPosition(RECT const& rc, WeaselSessionId ipc_id);
  virtual void StartMaintenance();
  virtual void EndMaintenance();
  virtual void SetOption(WeaselSessionId ipc_id,
                         const std::string& opt,
                         bool val);
  virtual void UpdateColorTheme(BOOL darkMode);

  void OnUpdateUI(std::function<void()> const& cb);
  void SetAsyncRefresh(std::function<void(DWORD)> const& cb) {
    _AsyncRefreshCallback = cb;
  }

 private:
  void _Setup();
  bool _IsDeployerRunning();
  void _UpdateUI(WeaselSessionId ipc_id);
  void _LoadSchemaSpecificSettings(WeaselSessionId ipc_id,
                                   const std::string& schema_id);
  void _LoadAppInlinePreeditSet(WeaselSessionId ipc_id,
                                bool ignore_app_name = false);
  bool _ShowMessage(weasel::Context& ctx, weasel::Status& status);
  bool _Respond(WeaselSessionId ipc_id, EatLine eat);
  void _ReadClientInfo(WeaselSessionId ipc_id, LPWSTR buffer);
  void _GetCandidateInfo(weasel::CandidateInfo& cinfo,
                         RimeContext& ctx,
                         RimeSessionId session_id);
  void _GetStatus(weasel::Status& stat,
                  WeaselSessionId ipc_id,
                  weasel::Context& ctx);
  void _GetContext(weasel::Context& ctx, RimeSessionId session_id);
  void _UpdateShowNotifications(RimeConfig* config, bool initialize = false);
  void _LoadDynamicLayoutConfig(RimeConfig* config,
                                weasel::DynamicLayoutConfig& dlc,
                                weasel::UIStyle::LayoutType configured_type);
  void _ResolveLayoutForSession(SessionStatus& session_status,
                                const weasel::CandidateInfo& cinfo);
  void _RemapCandidateNavigationKey(SessionStatus& session_status,
                                    RimeSessionId session_id,
                                    weasel::KeyEvent& key_event);

  void _UpdateInlinePreeditStatus(WeaselSessionId ipc_id);
  void _StartLlmWorker(SessionStatus& session_status,
                       WeaselSessionId ipc_id,
                       const std::wstring& context);

  RimeSessionId to_session_id(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id].session_id;
  }
  SessionStatus& get_session_status(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id];
  }
  SessionStatus& new_session_status(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id] = SessionStatus();
  }

  AppOptionsByAppName m_app_options;
  weasel::UI* m_ui;  // reference
  DWORD m_active_session;
  bool m_disabled;
  std::string m_last_schema_id;
  std::string m_last_app_name;
  weasel::UIStyle m_base_style;
  weasel::DynamicLayoutConfig m_base_dynamic_layout;
  weasel::UIStyle::LayoutType m_base_configured_layout_type;
  bool m_base_fullscreen;
  std::map<std::string, bool> m_show_notifications;
  std::map<std::string, bool> m_show_notifications_base;
  std::function<void()> _UpdateUICallback;
  std::function<void(DWORD)> _AsyncRefreshCallback;
  struct LlmResult {
    DWORD ipc_id;
    std::wstring request_id;
    uint64_t generation;
    bool success;
    std::vector<LlmCandidateItem> candidates;
  };
  struct LlmWorker {
    std::thread thread;
    std::shared_ptr<std::atomic_bool> done;
  };
  std::mutex m_llm_results_mutex;
  std::vector<LlmResult> m_llm_results;
  std::vector<LlmWorker> m_llm_workers;

  static void OnNotify(void* context_object,
                       uintptr_t session_id,
                       const char* message_type,
                       const char* message_value);
  static std::string m_message_type;
  static std::string m_message_value;
  static std::string m_message_label;
  static std::string m_option_name;
  static std::mutex m_notifier_mutex;
  SessionStatusMap m_session_status_map;
  bool m_current_dark_mode;
  bool m_global_ascii_mode;
  int m_show_notifications_time;
  DWORD m_pid;
};
