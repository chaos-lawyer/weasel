#include "stdafx.h"
#include "WeaselIPC.h"
#include "WeaselTSF.h"
#include <KeyEvent.h>
#include "CandidateList.h"
#include <algorithm>
#include <vector>

static weasel::KeyEvent prevKeyEvent;
static BOOL prevfEaten = FALSE;
static int keyCountToSimulate = 0;

namespace {
constexpr ULONG_PTR kUndoInputMarker = 0x57530000;
constexpr ULONG_PTR kUndoInputMarkerMask = 0xffff0000;
thread_local unsigned int undoInputSerial = 0;
thread_local std::vector<WeaselTSF*> reeditMouseOwners;
LRESULT CALLBACK ReeditMouseProc(int code, WPARAM message, LPARAM data) {
  if (code >= 0 && (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                    message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN ||
                    message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)) {
    for (auto* owner : reeditMouseOwners) {
      owner->_InvalidateBackspace();
      owner->_ClearTabJumpSession();
    }
  }
  return CallNextHookEx(nullptr, code, message, data);
}
}  // namespace

bool WeaselTSF::_ProcessKeyEvent(ITfContext* pContext,
                                 WPARAM wParam,
                                 LPARAM lParam,
                                 BOOL* pfEaten) {
  const ULONG_PTR inputTag = static_cast<ULONG_PTR>(GetMessageExtraInfo());
  const bool keyUp = KeyInfo(lParam).isKeyUp;
  const bool tagged = (inputTag & kUndoInputMarkerMask) == kUndoInputMarker;
  const bool taggedActionKey = wParam == VK_CONTROL || wParam == 'Z' ||
                               wParam == VK_BACK || wParam == VK_F24;
  const bool drainingKey =
      wParam == VK_F24 ||
      (_undo_is_backspace ? wParam == VK_BACK
                          : (wParam == VK_CONTROL || wParam == 'Z'));
  // TSF callbacks need not retain the message queue's dwExtraInfo. Track the
  // queued shortcut until its marker arrives instead of treating its Ctrl/Z
  // events as user input when the tag is unavailable.
  if ((tagged && taggedActionKey) || (_undo_input_active && drainingKey) ||
      (_undo_marker_release && wParam == VK_F24)) {
    *pfEaten = (wParam == VK_F24);
    if (wParam == VK_F24) {
      if (keyUp) {
        _undo_marker_release = false;
        _undo_is_backspace = false;
      } else if (_undo_input_active &&
                 (!tagged || !_undo_input_tag || inputTag == _undo_input_tag)) {
        const bool resume = _undo_input_tag && _undo_context == pContext &&
                            _undo_focus == GetFocus();
        _undo_input_active = false;
        _undo_marker_release = true;
        _CancelUndo();
        if (resume && (!_isToOpenClose || _IsKeyboardOpen()) &&
            !_IsKeyboardDisabled()) {
          if (_undo_is_backspace) {
            _CompleteBackspace();
            OutputDebugStringW(
                L"Weasel reopen: backspace delivered, notifying "
                L"configuration\n");
          }
          // F35 is a configuration notification, not a Windows keystroke.
          m_client.ProcessKeyEvent(weasel::KeyEvent(ibus::F35, 0));
          return true;
        }
      }
    }
    // No new IPC response: do not replay the cached undo request or preedit.
    return false;
  }
  _TrackReeditKey(wParam, keyUp);
  if (!keyUp)
    _CancelUndo();
  // when _IsKeyboardDisabled don't eat the key,
  // when keyboard closable and keyboard closed, don't eat the key
  if ((_isToOpenClose && !_IsKeyboardOpen()) || _IsKeyboardDisabled()) {
    *pfEaten = FALSE;
    return true;
  }

  // if server connection is Not OK, don't eat it.
  if (!_EnsureServerConnected()) {
    *pfEaten = FALSE;
    return true;
  }
  weasel::KeyEvent ke;
  GetKeyboardState(_lpbKeyState);
  if (!ConvertKeyEvent(static_cast<UINT>(wParam), lParam, _lpbKeyState, ke)) {
    /* Unknown key event */
    *pfEaten = FALSE;
  } else {
    // cheet key code when vertical auto reverse happened, swap up and down
    if (_cand->GetIsReposition()) {
      if (ke.keycode == ibus::Up)
        ke.keycode = ibus::Down;
      else if (ke.keycode == ibus::Down)
        ke.keycode = ibus::Up;
    }
    if (!keyCountToSimulate)
      *pfEaten = (BOOL)m_client.ProcessKeyEvent(ke);

    if (ke.keycode == ibus::Caps_Lock) {
      if (prevKeyEvent.keycode == ibus::Caps_Lock && prevfEaten == TRUE &&
          (ke.mask & ibus::RELEASE_MASK) && (!keyCountToSimulate)) {
        if ((GetKeyState(VK_CAPITAL) & 0x01)) {
          if (_committed || (!*pfEaten && _status.composing)) {
            keyCountToSimulate = 2;
            INPUT inputs[2];
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki = {VK_CAPITAL, 0, 0, 0, 0};
            inputs[1].type = INPUT_KEYBOARD;
            inputs[1].ki = {VK_CAPITAL, 0, KEYEVENTF_KEYUP, 0, 0};
            ::SendInput(sizeof(inputs) / sizeof(INPUT), inputs, sizeof(INPUT));
          }
        }
        *pfEaten = TRUE;
      }
      if (keyCountToSimulate)
        keyCountToSimulate--;
    }

    prevfEaten = *pfEaten;
    prevKeyEvent = ke;
  }
  return true;
}

bool WeaselTSF::_HandleCursorInput(WPARAM key,
                                   bool release,
                                   bool test,
                                   BOOL* eaten) {
  const bool tagged =
      static_cast<ULONG_PTR>(GetMessageExtraInfo()) == kCursorInputMarker;
  if (!tagged && !(_cursor_input_active &&
                   (key == VK_LEFT || key == VK_RIGHT || key == VK_SHIFT ||
                    key == VK_PACKET || key == VK_F23)))
    return false;
  *eaten = key == VK_F23;
  if (key == VK_F23 && release && !test) {
    _cursor_input_active = false;
    _FinishPunctuationJump();
  }
  return true;
}

bool WeaselTSF::_MatchPunctuationJump(WPARAM key,
                                      LPARAM lParam,
                                      UINT* code,
                                      UINT* mask) {
  // Match physical punctuation keys only, never committed text or letters
  // translated by a schema into punctuation. A snippet has N fields + end.
  if (!_cursor_tab_jump || _cursor_punct_jump_keys.empty() ||
      !_HasActiveTabJumpSession() || _IsComposing() || _cursor_input_active ||
      (!_cursor_punct_jump_any_field &&
       _tab_jump_index + 2 != _tab_jump_targets.size()) ||
      (key >= 'A' && key <= 'Z') || key == VK_PACKET ||
      ((_isToOpenClose && !_IsKeyboardOpen()) || _IsKeyboardDisabled()) ||
      (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000) ||
      (GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
    return false;
  BYTE state[256] = {};
  GetKeyboardState(state);
  weasel::KeyEvent event;
  if (!ConvertKeyEvent(static_cast<UINT>(key), lParam, state, event))
    return false;
  const UINT c = event.keycode;
  const bool punctuation = (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
                           (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
  if (!punctuation || _cursor_punct_jump_keys.find(static_cast<wchar_t>(c)) ==
                          std::wstring::npos)
    return false;
  *code = c;
  *mask = event.mask;
  return true;
}

bool WeaselTSF::_BeginPunctuationJump(ITfContext* context,
                                      WPARAM key,
                                      UINT code,
                                      UINT mask) {
  if (context != _tab_jump_context || _tab_jump_focus != GetFocus())
    return false;
  const bool keyboard = _tab_jump_keyboard;
  _punct_jump_context = context;
  _punct_jump_focus = GetFocus();
  _punct_jump_code = code;
  _punct_jump_mask = mask;
  _punct_jump_release = key;
  if (!_StepTabJump(context, false, true)) {
    _punct_jump_context = nullptr;
    _punct_jump_release = 0;
    return false;
  }
  if (!keyboard)
    _FinishPunctuationJump();
  return true;
}

void WeaselTSF::_FinishPunctuationJump() {
  if (!_punct_jump_context)
    return;
  const auto context = _punct_jump_context;
  const HWND focus = _punct_jump_focus;
  const UINT code = _punct_jump_code;
  const UINT mask = _punct_jump_mask;
  _punct_jump_context = nullptr;
  if (focus != GetFocus() || (_isToOpenClose && !_IsKeyboardOpen()) ||
      _IsKeyboardDisabled())
    return;
  // In fallback hosts the cursor keys must reach the editor before Rime can
  // commit punctuation. F23's release is the end-of-navigation barrier.
  const bool handled = m_client.ProcessKeyEvent(weasel::KeyEvent(code, mask));
  _UpdateComposition(context);
  if (!handled) {
    // ASCII mode and pass-through schemas still receive the original symbol.
    INPUT inputs[4] = {};
    for (size_t i = 0; i < 2; ++i) {
      auto& input = inputs[i];
      input.type = INPUT_KEYBOARD;
      input.ki.wScan = static_cast<WORD>(code);
      input.ki.dwFlags = KEYEVENTF_UNICODE;
      input.ki.dwExtraInfo = kCursorInputMarker;
    }
    inputs[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    for (size_t i = 2; i < 4; ++i) {
      inputs[i].type = INPUT_KEYBOARD;
      inputs[i].ki.wVk = VK_F23;
      inputs[i].ki.dwExtraInfo = kCursorInputMarker;
    }
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    _cursor_input_active = true;
    if (SendInput(4, inputs, sizeof(INPUT)) != 4)
      _cursor_input_active = false;
  }
  m_client.ProcessKeyEvent(weasel::KeyEvent(code, mask | ibus::RELEASE_MASK));
  _UpdateComposition(context);
}

void WeaselTSF::_TrackTabJumpKey(ITfContext* context, WPARAM key) {
  if (!_HasActiveTabJumpSession())
    return;
  const bool composing = _IsComposing();
  const bool horizontal = key == VK_LEFT || key == VK_RIGHT;
  const bool navigation = key >= VK_PRIOR && key <= VK_DOWN;
  // Candidate paging and preedit navigation belong to Rime. Only document
  // navigation can invalidate the relative distances used by fallback hosts.
  if (context != _tab_jump_context || _tab_jump_focus != GetFocus() ||
      key == VK_ESCAPE ||
      (!composing && ((navigation && !horizontal) ||
                      (horizontal && (GetKeyState(VK_SHIFT) & 0x8000)) ||
                      (GetKeyState(VK_CONTROL) & 0x8000) ||
                      (GetKeyState(VK_MENU) & 0x8000)))) {
    _ClearTabJumpSession();
  } else if (key == VK_BACK || key == VK_DELETE || key == VK_SPACE ||
             key == VK_RETURN || (key >= VK_NUMPAD0 && key <= VK_DIVIDE) ||
             (key >= '0' && key <= 'Z') || key >= VK_OEM_1) {
    _tab_jump_selection = false;
  }
}

void WeaselTSF::_TrackTabJumpNavigation(WPARAM key) {
  if (!_HasActiveTabJumpSession())
    return;
  if (key == VK_DELETE && _tab_jump_keyboard && _tab_jump_caret_offset > 0) {
    --_tab_jump_caret_offset;
    return;
  }
  if (key != VK_LEFT && key != VK_RIGHT)
    return;
  if (_tab_jump_keyboard) {
    // The offset is the distance from the document caret to the current
    // field end. Count actual key-down deliveries, never OnTestKeyDown calls.
    if (_tab_jump_selection) {
      _tab_jump_caret_offset =
          key == VK_LEFT ? _tab_jump_targets[_tab_jump_index].selection_length
                         : 0;
    } else if (key == VK_LEFT) {
      ++_tab_jump_caret_offset;
    } else {
      --_tab_jump_caret_offset;
    }
    if (_tab_jump_caret_offset < -4096 || _tab_jump_caret_offset > 4096) {
      _ClearTabJumpSession();
      return;
    }
  }
  _tab_jump_selection = false;
}

STDMETHODIMP WeaselTSF::OnSetFocus(BOOL fForeground) {
  if (fForeground)
    m_client.FocusIn();
  else {
    _ClearTabJumpSession();
    _punct_jump_context = nullptr;
    _punct_jump_release = 0;
    m_client.FocusOut();
    _AbortComposition();
  }

  return S_OK;
}

/* Some apps sends strange OnTestKeyDown/OnKeyDown combinations:
 *  Some sends OnKeyDown() only. (QQ2012)
 *  Some sends multiple OnTestKeyDown() for a single key event. (MS WORD 2010
 * x64)
 *
 * We assume every key event will eventually cause a OnKeyDown() call.
 * We use _fTestKeyDownPending to omit multiple OnTestKeyDown() calls,
 *  and for OnKeyDown() to check if the key has already been sent to the server.
 */

STDMETHODIMP WeaselTSF::OnTestKeyDown(ITfContext* pContext,
                                      WPARAM wParam,
                                      LPARAM lParam,
                                      BOOL* pfEaten) {
  if (_HandleCursorInput(wParam, false, true, pfEaten))
    return S_OK;
  _fTestKeyUpPending = FALSE;
  if (_fTestKeyDownPending) {
    *pfEaten = TRUE;
    return S_OK;
  }
  UINT punctCode = 0, punctMask = 0;
  if (_MatchPunctuationJump(wParam, lParam, &punctCode, &punctMask)) {
    *pfEaten = TRUE;
    _fTestKeyDownPending = TRUE;
    return S_OK;
  }
  if (_HasActiveTabJumpSession() && !_IsComposing() && wParam == VK_TAB &&
      !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
    *pfEaten = TRUE;
    _fTestKeyDownPending = TRUE;
    return S_OK;
  }
  _TrackTabJumpKey(pContext, wParam);
  if (_ProcessKeyEvent(pContext, wParam, lParam, pfEaten))
    _UpdateComposition(pContext);
  if (*pfEaten)
    _fTestKeyDownPending = TRUE;
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnKeyDown(ITfContext* pContext,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL* pfEaten) {
  if (_HandleCursorInput(wParam, false, false, pfEaten))
    return S_OK;
  UINT punctCode = 0, punctMask = 0;
  if (_MatchPunctuationJump(wParam, lParam, &punctCode, &punctMask)) {
    _fTestKeyDownPending = FALSE;
    if (_BeginPunctuationJump(pContext, wParam, punctCode, punctMask)) {
      *pfEaten = TRUE;
      return S_OK;
    }
  }
  _TrackTabJumpKey(pContext, wParam);
  _fTestKeyUpPending = FALSE;
  if (_HasActiveTabJumpSession() && !_IsComposing() && wParam == VK_TAB &&
      !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
    _fTestKeyDownPending = FALSE;
    const bool backward = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    *pfEaten = _StepTabJump(pContext, backward);
    _tab_jump_keyup = *pfEaten != FALSE;
    return S_OK;
  }
  if (_fTestKeyDownPending) {
    _fTestKeyDownPending = FALSE;
    *pfEaten = TRUE;
  } else if (_ProcessKeyEvent(pContext, wParam, lParam, pfEaten)) {
    _UpdateComposition(pContext);
  }
  // Only uneaten keys reach the document. Preedit/candidate arrow keys must
  // not alter fallback distances, and repeated tests must not count twice.
  if (!*pfEaten)
    _TrackTabJumpNavigation(wParam);
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnTestKeyUp(ITfContext* pContext,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    BOOL* pfEaten) {
  if (_HandleCursorInput(wParam, true, true, pfEaten))
    return S_OK;
  if (_punct_jump_release && wParam == _punct_jump_release) {
    *pfEaten = TRUE;
    return S_OK;
  }
  if (wParam == VK_TAB && _tab_jump_keyup) {
    *pfEaten = TRUE;
    return S_OK;
  }
  _fTestKeyDownPending = FALSE;
  if (_fTestKeyUpPending) {
    *pfEaten = TRUE;
    return S_OK;
  }
  if (_ProcessKeyEvent(pContext, wParam, lParam, pfEaten))
    _UpdateComposition(pContext);
  if (*pfEaten)
    _fTestKeyUpPending = TRUE;
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnKeyUp(ITfContext* pContext,
                                WPARAM wParam,
                                LPARAM lParam,
                                BOOL* pfEaten) {
  if (_HandleCursorInput(wParam, true, false, pfEaten))
    return S_OK;
  if (_punct_jump_release && wParam == _punct_jump_release) {
    _punct_jump_release = 0;
    *pfEaten = TRUE;
    return S_OK;
  }
  if (wParam == VK_TAB && _tab_jump_keyup) {
    _tab_jump_keyup = false;
    *pfEaten = TRUE;
    return S_OK;
  }
  _fTestKeyDownPending = FALSE;
  if (_fTestKeyUpPending) {
    _fTestKeyUpPending = FALSE;
    *pfEaten = TRUE;
  } else {
    if (_ProcessKeyEvent(pContext, wParam, lParam, pfEaten) && !_async_edit)
      _UpdateComposition(pContext);
  }
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnPreservedKey(ITfContext* pContext,
                                       REFGUID rguid,
                                       BOOL* pfEaten) {
  *pfEaten = FALSE;
  return S_OK;
}

BOOL WeaselTSF::_InitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
  HRESULT hr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return FALSE;

  hr = pKeystrokeMgr->AdviseKeyEventSink(_tfClientId, (ITfKeyEventSink*)this,
                                         TRUE);

  return (hr == S_OK);
}

void WeaselTSF::_UninitKeyEventSink() {
  _RemoveReeditMouseGuard();
  _ForgetLastCommit();
  _CancelUndo();
  _undo_input_active = false;
  _undo_marker_release = false;
  _undo_is_backspace = false;
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return;

  pKeystrokeMgr->UnadviseKeyEventSink(_tfClientId);
}

BOOL WeaselTSF::_InitPreservedKey() {
  return TRUE;
#if 0
	com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (_pThreadMgr->QueryInterface(pKeystrokeMgr.GetAddressOf()) != S_OK)
	{
		return FALSE;
	}
	TF_PRESERVEDKEY preservedKeyImeMode;

	/* Define SHIFT ONLY for now */
	preservedKeyImeMode.uVKey = VK_SHIFT;
	preservedKeyImeMode.uModifiers = TF_MOD_ON_KEYUP;

	auto hr = pKeystrokeMgr->PreserveKey(
		_tfClientId,
		GUID_IME_MODE_PRESERVED_KEY,
		&preservedKeyImeMode, L"", 0);

	return SUCCEEDED(hr);
#endif
}

void WeaselTSF::_UninitPreservedKey() {}

void WeaselTSF::_InvalidateBackspace() {
  _backspace_armed = false;
  _backspace_chain_valid = false;
  _backspace_h_trigger = false;
}

void WeaselTSF::_ArmBackspace(com_ptr<ITfContext> context,
                              const std::wstring& text,
                              bool transitory) {
  if (!_reedit_mouse_hook) {
    _reedit_mouse_hook = SetWindowsHookExW(WH_MOUSE, ReeditMouseProc, nullptr,
                                           GetCurrentThreadId());
    if (_reedit_mouse_hook)
      reeditMouseOwners.push_back(this);
    else
      OutputDebugStringW(
          L"Weasel reopen: mouse guard unavailable, backspace disabled\n");
  }
  _backspace_context = context;
  _backspace_text = text;
  _backspace_focus = GetFocus();
  _backspace_transitory = transitory;
  _backspace_range_lost = false;
  _backspace_h_trigger = false;
  _backspace_armed = _reedit_mouse_hook != nullptr;
  _backspace_chain_valid = _backspace_armed;
}

void WeaselTSF::_RemoveReeditMouseGuard() {
  if (_reedit_mouse_hook) {
    UnhookWindowsHookEx(_reedit_mouse_hook);
    _reedit_mouse_hook = nullptr;
    reeditMouseOwners.erase(
        std::remove(reeditMouseOwners.begin(), reeditMouseOwners.end(), this),
        reeditMouseOwners.end());
  }
  _InvalidateBackspace();
}

void WeaselTSF::_TrackReeditKey(WPARAM key, bool keyUp) {
  if (keyUp || (!_backspace_armed && !_backspace_chain_valid))
    return;
  const bool modified = GetKeyState(VK_CONTROL) < 0 ||
                        GetKeyState(VK_MENU) < 0 || GetKeyState(VK_SHIFT) < 0;
  if (_backspace_armed && !modified && key == 'H' && !_backspace_h_trigger) {
    _backspace_h_trigger = true;
    return;
  }
  if (!modified && key == VK_TAB && _backspace_h_trigger)
    return;
  // Ordinary typing starts the next adjacent commit. It cancels immediate
  // re-edit eligibility, but not the observed chain in a temporary context.
  // Navigation, modifiers and mouse input invalidate both.
  const bool typing =
      !modified &&
      (key == VK_SPACE || key == VK_RETURN ||
       (key >= VK_NUMPAD0 && key <= VK_DIVIDE) || (key >= '0' && key <= '9') ||
       (key >= 'A' && key <= 'Z') || (key >= 0x60 && key <= 0x6f) ||
       (key >= 0xba && key <= 0xc0) || (key >= 0xdb && key <= 0xde) ||
       key == 0xe2);
  const bool keepChain = _backspace_chain_valid && typing &&
                         _backspace_context == _pEditSessionContext &&
                         _backspace_focus == GetFocus();
  _InvalidateBackspace();
  _backspace_chain_valid = keepChain;
}

void WeaselTSF::_CancelUndo() {
  _undo_backspaces = 0;
  _undo_input_tag = 0;
  _undo_context = nullptr;
  _undo_focus = nullptr;
}

void WeaselTSF::_RequestUndo(com_ptr<ITfContext> pContext) {
  _RequestInputAction(pContext, 0);
}

void WeaselTSF::_RequestInputAction(com_ptr<ITfContext> pContext,
                                    unsigned backspaces) {
  if (_undo_input_active || _undo_marker_release || backspaces > 128)
    return;
  _undo_backspaces = backspaces;
  _undo_is_backspace = backspaces != 0;
  _undo_input_tag = kUndoInputMarker | (++undoInputSerial & 0xffff);
  _undo_context = pContext;
  _undo_focus = GetFocus();
  if (_IsComposing())
    _EndComposition(pContext, true, true, _undo_input_tag);
  else
    _SimulateUndo(_undo_input_tag);
}

void WeaselTSF::_SimulateUndo(ULONG_PTR inputTag) {
  if (!inputTag || inputTag != _undo_input_tag)
    return;

  com_ptr<ITfDocumentMgr> document;
  com_ptr<ITfContext> context;
  if (!_pThreadMgr || FAILED(_pThreadMgr->GetFocus(&document)) || !document ||
      FAILED(document->GetTop(&context)) || context != _undo_context ||
      GetFocus() != _undo_focus || _IsComposing()) {
    _CancelUndo();
    return;
  }

  if (_undo_backspaces &&
      (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0 ||
       GetKeyState(VK_SHIFT) < 0 || _IsKeyboardDisabled() ||
       (_isToOpenClose && !_IsKeyboardOpen()))) {
    _CancelUndo();
    return;
  }

  if (_undo_backspaces) {
    const UINT total = _undo_backspaces * 2 + 2;
    std::vector<INPUT> inputs(total);
    for (UINT i = 0; i < total; ++i) {
      inputs[i].type = INPUT_KEYBOARD;
      inputs[i].ki.dwExtraInfo = inputTag;
      inputs[i].ki.wVk = i < total - 2 ? VK_BACK : VK_F24;
      inputs[i].ki.dwFlags = i % 2 ? KEYEVENTF_KEYUP : 0;
    }
    _undo_input_active = true;
    const UINT sent = ::SendInput(total, inputs.data(), sizeof(INPUT));
    if (sent != total) {
      _CancelUndo();
      _undo_input_active = false;
      _undo_marker_release = sent == total - 1;
      // Release a partially injected BackSpace/marker, without notifying Lua.
      if (sent < total && sent % 2)
        ::SendInput(1, inputs.data() + sent, sizeof(INPUT));
      OutputDebugStringW(L"Weasel reopen: backspace injection incomplete\n");
    }
    return;
  }

  INPUT inputs[6] = {};
  for (auto& input : inputs) {
    input.type = INPUT_KEYBOARD;
    input.ki.dwExtraInfo = inputTag;
  }
  inputs[0].ki.wVk = VK_CONTROL;
  inputs[1].ki.wVk = 'Z';
  inputs[2].ki.wVk = 'Z';
  inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
  inputs[3].ki.wVk = VK_CONTROL;
  inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
  // Windows queues these after Ctrl+Z. The marker triggers a Lua notification
  // only after the host has received the preceding undo keystrokes.
  inputs[4].ki.wVk = VK_F24;
  inputs[5].ki.wVk = VK_F24;
  inputs[5].ki.dwFlags = KEYEVENTF_KEYUP;
  _undo_input_active = true;
  const UINT sent = ::SendInput(6, inputs, sizeof(INPUT));
  if (sent != 6) {
    _CancelUndo();
    _undo_input_active = false;
    _undo_marker_release = (sent == 5);
    // A partially injected shortcut must not leave Ctrl or Z held down.
    if (sent > 0 && sent < 4)
      ::SendInput(2, inputs + 2, sizeof(INPUT));
    else if (sent == 5)
      ::SendInput(1, inputs + 5, sizeof(INPUT));
  }
}
