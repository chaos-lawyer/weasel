#include "stdafx.h"
#include "WeaselIPC.h"
#include "WeaselTSF.h"
#include <KeyEvent.h>
#include "CandidateList.h"

static weasel::KeyEvent prevKeyEvent;
static BOOL prevfEaten = FALSE;
static int keyCountToSimulate = 0;

namespace {
constexpr ULONG_PTR kUndoInputMarker = 0x57530000;
constexpr ULONG_PTR kUndoInputMarkerMask = 0xffff0000;
thread_local unsigned int undoInputSerial = 0;
}  // namespace

bool WeaselTSF::_ProcessKeyEvent(ITfContext* pContext,
                                 WPARAM wParam,
                                 LPARAM lParam,
                                 BOOL* pfEaten) {
  const ULONG_PTR inputTag = static_cast<ULONG_PTR>(GetMessageExtraInfo());
  const bool keyUp = KeyInfo(lParam).isKeyUp;
  if ((inputTag & kUndoInputMarkerMask) == kUndoInputMarker &&
      (wParam == VK_CONTROL || wParam == 'Z' || wParam == VK_F24)) {
    // Bypass Rime for the entire Ctrl+Z sequence, including both key-ups.
    // The tagged F24 marker is private and must never reach the host app.
    *pfEaten = (wParam == VK_F24);
    if (wParam == VK_F24 && !keyUp && inputTag == _undo_input_tag) {
      const bool resume =
          _undo_context == pContext && _undo_focus == GetFocus();
      _CancelUndo();
      if (resume && _IsKeyboardOpen() && !_IsKeyboardDisabled()) {
        // F35 is a configuration notification, not a Windows keystroke.
        m_client.ProcessKeyEvent(weasel::KeyEvent(ibus::F35, 0));
        return true;
      }
    }
    return false;
  }
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
  _CancelUndo();
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

void WeaselTSF::_CancelUndo() {
  _undo_input_tag = 0;
  _undo_context = nullptr;
  _undo_focus = nullptr;
}

void WeaselTSF::_RequestUndo(com_ptr<ITfContext> pContext) {
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
  const UINT sent = ::SendInput(6, inputs, sizeof(INPUT));
  if (sent != 6) {
    _CancelUndo();
    // A partially injected shortcut must not leave Ctrl or Z held down.
    if (sent > 0 && sent < 4)
      ::SendInput(2, inputs + 2, sizeof(INPUT));
  }
}
