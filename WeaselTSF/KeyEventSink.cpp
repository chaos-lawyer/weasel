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
    for (auto* owner : reeditMouseOwners)
      owner->_InvalidateBackspace();
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
          if (_undo_is_backspace)
            OutputDebugStringW(
                L"Weasel reopen: backspace delivered, notifying "
                L"configuration\n");
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

STDMETHODIMP WeaselTSF::OnSetFocus(BOOL fForeground) {
  if (fForeground)
    m_client.FocusIn();
  else {
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
  _fTestKeyUpPending = FALSE;
  if (_fTestKeyDownPending) {
    *pfEaten = TRUE;
    return S_OK;
  }
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
  _fTestKeyUpPending = FALSE;
  if (_fTestKeyDownPending) {
    _fTestKeyDownPending = FALSE;
    *pfEaten = TRUE;
  } else {
    if (_ProcessKeyEvent(pContext, wParam, lParam, pfEaten))
      _UpdateComposition(pContext);
  }
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnTestKeyUp(ITfContext* pContext,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    BOOL* pfEaten) {
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
  if (keyUp || !_backspace_armed)
    return;
  const bool modified = GetKeyState(VK_CONTROL) < 0 ||
                        GetKeyState(VK_MENU) < 0 || GetKeyState(VK_SHIFT) < 0;
  if (!modified && key == 'H' && !_backspace_h_trigger) {
    _backspace_h_trigger = true;
    return;
  }
  if (!modified && key == VK_TAB && _backspace_h_trigger)
    return;
  _InvalidateBackspace();
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
