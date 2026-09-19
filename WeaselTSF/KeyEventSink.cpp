#include "stdafx.h"
#include "WeaselIPC.h"
#include "WeaselTSF.h"
#include <KeyEvent.h>
#include "CandidateList.h"

static weasel::KeyEvent prevKeyEvent;
static BOOL prevfEaten = FALSE;
static int keyCountToSimulate = 0;

namespace {
// {E3F8B74F-7270-4F18-B1CD-C483FF164E29}
const GUID kTranscriptionKeyGuid = {
    0xe3f8b74f,
    0x7270,
    0x4f18,
    {0xb1, 0xcd, 0xc4, 0x83, 0xff, 0x16, 0x4e, 0x29}};
const TF_PRESERVEDKEY kTranscriptionKey = {'F', TF_MOD_CONTROL | TF_MOD_SHIFT};
}  // namespace

void WeaselTSF::_ProcessKeyEvent(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
  // when _IsKeyboardDisabled don't eat the key,
  // when keyboard closable and keyboard closed, don't eat the key
  if ((_isToOpenClose && !_IsKeyboardOpen()) || _IsKeyboardDisabled()) {
    *pfEaten = FALSE;
    return;
  }

  // if server connection is Not OK, don't eat it.
  if (!_EnsureServerConnected()) {
    *pfEaten = FALSE;
    return;
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
}

STDMETHODIMP WeaselTSF::OnSetFocus(BOOL fForeground) {
  if (fForeground)
    m_client.FocusIn();
  else {
    _testKeyDown.Clear();
    _testKeyUp.Clear();
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
 * Cache the identity of each handled test. If a host omits OnKeyDown(),
 *  a later, different key must still reach the server.
 */

STDMETHODIMP WeaselTSF::OnTestKeyDown(ITfContext* pContext,
                                      WPARAM wParam,
                                      LPARAM lParam,
                                      BOOL* pfEaten) {
  _testKeyUp.Clear();
  if (_testKeyDown.Matches(wParam, lParam)) {
    *pfEaten = TRUE;
    return S_OK;
  }
  _testKeyDown.Clear();
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  if (*pfEaten)
    _testKeyDown.Remember(wParam, lParam);
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnKeyDown(ITfContext* pContext,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL* pfEaten) {
  _testKeyUp.Clear();
  if (_testKeyDown.Matches(wParam, lParam)) {
    _testKeyDown.Clear();
    *pfEaten = TRUE;
  } else {
    _testKeyDown.Clear();
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    _UpdateComposition(pContext);
  }
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnTestKeyUp(ITfContext* pContext,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    BOOL* pfEaten) {
  _testKeyDown.Clear();
  if (_testKeyUp.Matches(wParam, lParam)) {
    *pfEaten = TRUE;
    return S_OK;
  }
  _testKeyUp.Clear();
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  if (*pfEaten)
    _testKeyUp.Remember(wParam, lParam);
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnKeyUp(ITfContext* pContext,
                                WPARAM wParam,
                                LPARAM lParam,
                                BOOL* pfEaten) {
  _testKeyDown.Clear();
  if (_testKeyUp.Matches(wParam, lParam)) {
    _testKeyUp.Clear();
    *pfEaten = TRUE;
  } else {
    _testKeyUp.Clear();
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    if (!_async_edit)
      _UpdateComposition(pContext);
  }
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnPreservedKey(ITfContext* pContext,
                                       REFGUID rguid,
                                       BOOL* pfEaten) {
  *pfEaten = FALSE;
  if (!IsEqualGUID(rguid, kTranscriptionKeyGuid) ||
      (_isToOpenClose && !_IsKeyboardOpen()) || _IsKeyboardDisabled() ||
      !_EnsureServerConnected()) {
    return S_OK;
  }

  // The preserved-key callback carries a command GUID, not a character or
  // modifiers. Send the canonical chord to the same Rime processor; it decides
  // whether composing is active and suppresses duplicate/repeated keydowns.
  const weasel::KeyEvent key('F', ibus::CONTROL_MASK | ibus::SHIFT_MASK);
  *pfEaten = (BOOL)m_client.ProcessKeyEvent(key);
  _UpdateComposition(pContext);
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
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return;

  pKeystrokeMgr->UnadviseKeyEventSink(_tfClientId);
}

BOOL WeaselTSF::_InitPreservedKey() {
  com_ptr<ITfKeystrokeMgr> key_manager;
  if (_pThreadMgr->QueryInterface(&key_manager) != S_OK)
    return TRUE;

  // This is an optional routing path for hosts with their own accelerators.
  // Registration failure must not prevent the input method from activating.
  const WCHAR description[] = L"Toggle simplified/traditional candidates";
  key_manager->PreserveKey(_tfClientId, kTranscriptionKeyGuid,
                           &kTranscriptionKey, description,
                           ARRAYSIZE(description) - 1);
  return TRUE;
}

void WeaselTSF::_UninitPreservedKey() {
  com_ptr<ITfKeystrokeMgr> key_manager;
  if (_pThreadMgr->QueryInterface(&key_manager) == S_OK)
    key_manager->UnpreserveKey(kTranscriptionKeyGuid, &kTranscriptionKey);
}
