#pragma once

#include "Globals.h"
#include <WeaselIPC.h>
#include <WeaselIPCData.h>

class CCandidateList;
class CLangBarItemButton;
class CCompartmentEventSink;

class WeaselTSF : public ITfTextInputProcessorEx,
                  public ITfThreadMgrEventSink,
                  public ITfTextEditSink,
                  public ITfTextLayoutSink,
                  public ITfKeyEventSink,
                  public ITfCompositionSink,
                  public ITfThreadFocusSink,
                  public ITfActiveLanguageProfileNotifySink,
                  public ITfEditSession,
                  public ITfDisplayAttributeProvider {
 public:
  WeaselTSF();
  ~WeaselTSF();

  /* IUnknown */
  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject);
  STDMETHODIMP_(ULONG) AddRef();
  STDMETHODIMP_(ULONG) Release();

  /* ITfTextInputProcessor */
  STDMETHODIMP Activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId);
  STDMETHODIMP Deactivate();

  /* ITfTextInputProcessorEx */
  STDMETHODIMP ActivateEx(ITfThreadMgr* pThreadMgr,
                          TfClientId tfClientId,
                          DWORD dwFlags);

  /* ITfThreadMgrEventSink */
  STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pDocMgr);
  STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pDocMgr);
  STDMETHODIMP OnSetFocus(ITfDocumentMgr* pDocMgrFocus,
                          ITfDocumentMgr* pDocMgrPrevFocus);
  STDMETHODIMP OnPushContext(ITfContext* pContext);
  STDMETHODIMP OnPopContext(ITfContext* pContext);

  /* ITfTextEditSink */
  STDMETHODIMP OnEndEdit(ITfContext* pic,
                         TfEditCookie ecReadOnly,
                         ITfEditRecord* pEditRecord);

  /* ITfTextLayoutSink */
  STDMETHODIMP OnLayoutChange(ITfContext* pContext,
                              TfLayoutCode lcode,
                              ITfContextView* pContextView);

  /* ITfKeyEventSink */
  STDMETHODIMP OnSetFocus(BOOL fForeground);
  STDMETHODIMP OnTestKeyDown(ITfContext* pContext,
                             WPARAM wParam,
                             LPARAM lParam,
                             BOOL* pfEaten);
  STDMETHODIMP OnKeyDown(ITfContext* pContext,
                         WPARAM wParam,
                         LPARAM lParam,
                         BOOL* pfEaten);
  STDMETHODIMP OnTestKeyUp(ITfContext* pContext,
                           WPARAM wParam,
                           LPARAM lParam,
                           BOOL* pfEaten);
  STDMETHODIMP OnKeyUp(ITfContext* pContext,
                       WPARAM wParam,
                       LPARAM lParam,
                       BOOL* pfEaten);
  STDMETHODIMP OnPreservedKey(ITfContext* pContext,
                              REFGUID rguid,
                              BOOL* pfEaten);

  // ITfThreadFocusSink
  STDMETHODIMP OnSetThreadFocus();
  STDMETHODIMP OnKillThreadFocus();

  /* ITfCompositionSink */
  STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite,
                                       ITfComposition* pComposition);

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

  /* ITfActiveLanguageProfileNotifySink */
  STDMETHODIMP OnActivated(REFCLSID clsid,
                           REFGUID guidProfile,
                           BOOL isActivated);

  // ITfDisplayAttributeProvider
  STDMETHODIMP EnumDisplayAttributeInfo(
      __RPC__deref_out_opt IEnumTfDisplayAttributeInfo** ppEnum);
  STDMETHODIMP GetDisplayAttributeInfo(
      __RPC__in REFGUID guidInfo,
      __RPC__deref_out_opt ITfDisplayAttributeInfo** ppInfo);

  ///* ITfCompartmentEventSink */
  // STDMETHODIMP OnChange(_In_ REFGUID guid);

  /* Compartments */
  BOOL _IsKeyboardDisabled();
  BOOL _IsKeyboardOpen();
  HRESULT _SetKeyboardOpen(BOOL fOpen);
  HRESULT _GetCompartmentDWORD(DWORD& value, const GUID guid);
  HRESULT _SetCompartmentDWORD(const DWORD& value, const GUID guid);

  /* Composition */
  void _StartComposition(com_ptr<ITfContext> pContext,
                         BOOL fCUASWorkaroundEnabled);
  void _EndComposition(com_ptr<ITfContext> pContext,
                       BOOL clear,
                       BOOL endUI = TRUE,
                       ULONG_PTR undoInputTag = 0);
  BOOL _ShowInlinePreedit(com_ptr<ITfContext> pContext,
                          const std::shared_ptr<weasel::Context> context);
  void _UpdateComposition(com_ptr<ITfContext> pContext);
  BOOL _IsComposing();
  BOOL _IsCurrentComposition(ITfComposition* pComposition);
  void _SetComposition(com_ptr<ITfComposition> pComposition);
  void _SetCompositionPosition(const RECT& rc);
  BOOL _UpdateCompositionWindow(com_ptr<ITfContext> pContext);
  void _FinalizeComposition();
  void _AbortComposition(bool clear = true);

  /* Language bar */
  HWND _GetFocusedContextWindow();
  void _HandleLangBarMenuSelect(UINT wID);

  /* IPC */
  bool _EnsureServerConnected();

  /* UI */
  void _UpdateUI(const weasel::Context& ctx, const weasel::Status& status);
  void _StartUI();
  void _EndUI();
  void _ShowUI();
  void _HideUI();
  com_ptr<ITfContext> _GetUIContextDocument();

  /* Cloud candidates: owner-thread polling, only while work is pending. */
  void _StartCloudPolling(com_ptr<ITfContext> context);
  void _StopCloudPolling();
  void _OnCloudTimer(UINT_PTR timer_id);
  HRESULT _PollCloudCandidates(TfEditCookie ec,
                               com_ptr<ITfContext> context,
                               UINT_PTR timer_id);

  /* Reopen the last committed range; configuration owns restored input. */
  void _ForgetLastCommit();
  bool _DetectTransientReset(TfEditCookie ec, com_ptr<ITfContext> context);
  bool _RequestBackspace(TfEditCookie ec,
                         com_ptr<ITfContext> context,
                         const std::wstring& text);
  void _ArmBackspace(com_ptr<ITfContext> context,
                     const std::wstring& text,
                     bool transitory);
  void _InvalidateBackspace();
  void _RemoveReeditMouseGuard();
  void _TrackReeditKey(WPARAM key, bool keyUp);
  struct CommitHistoryEntry {
    com_ptr<ITfRange> range;
    com_ptr<ITfRange> end;
    std::wstring text;
  };
  struct TabJumpTarget {
    com_ptr<ITfRange> range;
    bool is_selection = false;
    LONG steps = 0;
    LONG selection_length = 0;
  };
  static constexpr size_t kMaxCommitHistory = 10;
  void _CompleteBackspace();
  void _SyncLastCommitFields();

  /* Tab Jump / Snippet Session */
  bool _IsCursorTabJumpEnabled() const { return _cursor_tab_jump; }
  static constexpr ULONG_PTR kCursorInputMarker = 0x57534352;
  bool _HandleCursorInput(WPARAM key, bool release, bool test, BOOL* eaten);
  bool _SendCursorKeys(WORD direction,
                       LONG steps,
                       LONG selectionLength,
                       bool collapseSelection = false);
  bool _MatchPunctuationJump(WPARAM key, LPARAM lParam, UINT* code, UINT* mask);
  bool _BeginPunctuationJump(ITfContext* context,
                             WPARAM key,
                             UINT code,
                             UINT mask);
  void _FinishPunctuationJump();
  void _TrackTabJumpKey(ITfContext* context, WPARAM key);
  void _TrackTabJumpNavigation(WPARAM key);
  void _CompleteCursorPlacement(TfEditCookie ec,
                                ITfContext* context,
                                bool clear);
  void _ClearTabJumpSession();
  bool _HasActiveTabJumpSession() const;
  bool _StepTabJump(ITfContext* pContext,
                    bool backward = false,
                    bool toEnd = false);
  void _InitTabJumpSession(com_ptr<ITfContext> context,
                           std::vector<TabJumpTarget>&& targets);

  void _RememberLastCommit(TfEditCookie ec,
                           com_ptr<ITfContext> context,
                           com_ptr<ITfRange> range,
                           const std::wstring& text);
  bool _ValidateLastCommit(TfEditCookie ec, com_ptr<ITfContext> context);
  bool _ReopenLastCommit(TfEditCookie ec,
                         com_ptr<ITfContext> context,
                         const std::wstring& expectedText);

  void _RollbackReopen(TfEditCookie ec, com_ptr<ITfContext> context);
  void _FinishReopen();
  bool _QueueReopen(com_ptr<ITfContext> context,
                    com_ptr<ITfRange> range,
                    const std::wstring& text);
  HRESULT _CompleteReopen(TfEditCookie ec,
                          com_ptr<ITfContext> context,
                          com_ptr<ITfRange> range,
                          const std::wstring& text);

  /* Undo simulation */
  void _RequestUndo(com_ptr<ITfContext> pContext);
  void _RequestInputAction(com_ptr<ITfContext> context, unsigned backspaces);
  void _SimulateUndo(ULONG_PTR inputTag);
  void _CancelUndo();

  /* Display Attribute */
  void _ClearCompositionDisplayAttributes(TfEditCookie ec,
                                          _In_ ITfContext* pContext);
  BOOL _SetCompositionDisplayAttributes(TfEditCookie ec,
                                        _In_ ITfContext* pContext,
                                        ITfRange* pRangeComposition);
  BOOL _InitDisplayAttributeGuidAtom();

  com_ptr<ITfThreadMgr> _GetThreadMgr() { return _pThreadMgr; }
  void HandleUICallback(size_t* const sel,
                        size_t* const hov,
                        bool* const next,
                        bool* const scroll_next);

 private:
  friend class CInsertTextEditSession;
  /* ui callback functions private */
  void _SelectCandidateOnCurrentPage(const size_t index);
  void _HandleMouseHoverEvent(const size_t index);
  void _HandleMousePageEvent(bool* const nextPage, bool* const scrollNextPage);
  /* TSF Related */
  BOOL _InitThreadMgrEventSink();
  void _UninitThreadMgrEventSink();
  // ITfThreadFocusSink
  BOOL _InitThreadFocusSink();
  void _UninitThreadFocusSink();
  DWORD _dwThreadFocusSinkCookie;

  BOOL _InitTextEditSink(com_ptr<ITfDocumentMgr> pDocMgr);

  BOOL _InitKeyEventSink();
  void _UninitKeyEventSink();
  // Tagged input without a new server response must not replay cached data.
  bool _ProcessKeyEvent(ITfContext* pContext,
                        WPARAM wParam,
                        LPARAM lParam,
                        BOOL* pfEaten);

  BOOL _InitPreservedKey();
  void _UninitPreservedKey();

  BOOL _InitLanguageBar();
  void _UninitLanguageBar();
  void _UpdateLanguageBar(weasel::Status stat);
  void _ShowLanguageBar(BOOL show);
  void _EnableLanguageBar(BOOL enable);

  BOOL _InsertText(com_ptr<ITfContext> pContext, const std::wstring& ext);

  void _DeleteCandidateList();

  BOOL _InitCompartment();
  void _UninitCompartment();
  HRESULT _HandleCompartment(REFGUID guidCompartment);

  void _Reconnect();
  std::wstring _GetRootDir();

  bool isImmersive() const {
    return (_activateFlags & TF_TMF_IMMERSIVEMODE) != 0;
  }

  com_ptr<ITfThreadMgr> _pThreadMgr;
  TfClientId _tfClientId;
  DWORD _dwThreadMgrEventSinkCookie;

  com_ptr<ITfContext> _pTextEditSinkContext;
  DWORD _dwTextEditSinkCookie, _dwTextLayoutSinkCookie;
  BYTE _lpbKeyState[256];
  BOOL _fTestKeyDownPending, _fTestKeyUpPending;

  com_ptr<ITfContext> _pEditSessionContext;
  std::wstring _editSessionText;

  com_ptr<CCompartmentEventSink> _pKeyboardCompartmentSink;
  com_ptr<CCompartmentEventSink> _pConvertionCompartmentSink;

  com_ptr<ITfComposition> _pComposition;

  com_ptr<CLangBarItemButton> _pLangBarButton;

  com_ptr<CCandidateList> _cand;

  LONG _cRef;  // COM ref count

  /* CUAS Candidate Window Position Workaround */
  BOOL _fCUASWorkaroundTested, _fCUASWorkaroundEnabled;

  /* Weasel Related */
  weasel::Client m_client;
  DWORD _activateFlags;

  /* IME status */
  weasel::Status _status;

  // guidatom for the display attibute.
  TfGuidAtom _gaDisplayAttributeInput;
  BOOL _async_edit = false;
  BOOL _committed = false;
  BOOL _isToOpenClose = false;
  static VOID CALLBACK _CloudTimerProc(HWND, UINT, UINT_PTR, DWORD);
  UINT_PTR _cloud_timer_id = 0;
  ULONGLONG _cloud_poll_started = 0;
  bool _cloud_poll_queued = false;
  HWND _cloud_focus = nullptr;
  com_ptr<ITfContext> _cloud_context;
  com_ptr<ITfContext> _last_commit_context;
  std::vector<CommitHistoryEntry> _commit_history;
  com_ptr<ITfRange> _last_commit_range;
  com_ptr<ITfRange> _last_commit_end;
  com_ptr<ITfRange> _reopen_range;
  std::wstring _reopen_text;
  bool _reopen_popped = false;
  std::wstring _last_commit_text;
  std::vector<std::wstring> _backspace_history;
  com_ptr<ITfContext> _backspace_context;
  std::wstring _backspace_text;
  HWND _backspace_focus = nullptr;
  HHOOK _reedit_mouse_hook = nullptr;
  bool _backspace_armed = false;
  bool _backspace_chain_valid = false;
  bool _backspace_transitory = false;
  bool _backspace_range_lost = false;
  bool _backspace_h_trigger = false;
  unsigned _undo_backspaces = 0;
  bool _undo_is_backspace = false;
  bool _undo_input_active = false;
  bool _undo_marker_release = false;
  ULONG_PTR _undo_input_tag = 0;
  HWND _undo_focus = nullptr;
  com_ptr<ITfContext> _undo_context;
  bool _cursor_tab_jump = false;
  std::wstring _cursor_punct_jump_keys;
  bool _cursor_punct_jump_any_field = false;
  com_ptr<ITfContext> _punct_jump_context;
  HWND _punct_jump_focus = nullptr;
  UINT _punct_jump_code = 0;
  UINT _punct_jump_mask = 0;
  WPARAM _punct_jump_release = 0;
  com_ptr<ITfContext> _tab_jump_context;
  std::vector<TabJumpTarget> _tab_jump_targets;
  size_t _tab_jump_index = 0;
  LONG _tab_jump_caret_offset = 0;
  bool _tab_jump_keyup = false;
  bool _tab_jump_keyboard = false;
  bool _tab_jump_selection = false;
  HWND _tab_jump_focus = nullptr;
  bool _cursor_input_active = false;
  com_ptr<ITfContext> _cursor_placement_context;
  TabJumpTarget _cursor_placement;
  bool _cursor_placement_keyboard = false;
  HWND _cursor_placement_focus = nullptr;
};
