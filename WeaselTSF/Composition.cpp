#include "stdafx.h"
#include "WeaselTSF.h"
#include "EditSession.h"
#include "ResponseParser.h"
#include "CandidateList.h"

/* Start Composition */
class CStartCompositionEditSession : public CEditSession {
 public:
  CStartCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                               com_ptr<ITfContext> pContext,
                               BOOL fCUASWorkaroundEnabled)
      : CEditSession(pTextService, pContext) {
    _fCUASWorkaroundEnabled = fCUASWorkaroundEnabled;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  BOOL _fCUASWorkaroundEnabled;
};

STDMETHODIMP CStartCompositionEditSession::DoEditSession(TfEditCookie ec) {
  HRESULT hr = E_FAIL;
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  if (_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                (LPVOID*)&pInsertAtSelection) != S_OK)
    return hr;
  if (pInsertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, NULL, 0,
                                                &pRangeComposition) != S_OK)
    return hr;

  com_ptr<ITfContextComposition> pContextComposition;
  com_ptr<ITfComposition> pComposition;
  if (_pContext->QueryInterface(IID_ITfContextComposition,
                                (LPVOID*)&pContextComposition) != S_OK)
    return hr;
  if ((pContextComposition->StartComposition(
           ec, pRangeComposition, _pTextService, &pComposition) == S_OK) &&
      (pComposition != NULL)) {
    _pTextService->_SetComposition(pComposition);

    /* set selection */
    TF_SELECTION tfSelection;
    pRangeComposition->Collapse(ec, TF_ANCHOR_END);
    tfSelection.range = pRangeComposition;
    tfSelection.style.ase = TF_AE_NONE;
    tfSelection.style.fInterimChar = FALSE;
    _pContext->SetSelection(ec, 1, &tfSelection);

    // The old composition's range is still visible while its asynchronous
    // end session is pending. Position only after the new composition has
    // actually been created, not from the response handler's stale range.
    _pTextService->_UpdateCompositionWindow(_pContext);
  }

  return hr;
}

void WeaselTSF::_StartComposition(com_ptr<ITfContext> pContext,
                                  BOOL fCUASWorkaroundEnabled) {
  com_ptr<CStartCompositionEditSession> pStartCompositionEditSession;
  pStartCompositionEditSession.Attach(
      new CStartCompositionEditSession(this, pContext, fCUASWorkaroundEnabled));
  _cand->StartUI();
  if (pStartCompositionEditSession != nullptr) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pStartCompositionEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
}

/* End Composition */
class CEndCompositionEditSession : public CEditSession {
 public:
  CEndCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                             com_ptr<ITfContext> pContext,
                             com_ptr<ITfComposition> pComposition,
                             BOOL clear = TRUE,
                             ULONG_PTR undoInputTag = 0)
      : CEditSession(pTextService, pContext),
        _clear(clear),
        _undoInputTag(undoInputTag) {
    _pComposition = pComposition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  BOOL _clear;
  ULONG_PTR _undoInputTag;
};

STDMETHODIMP CEndCompositionEditSession::DoEditSession(TfEditCookie ec) {
  /* Clear the dummy text we set before, if any. */
  if (_pComposition == nullptr)
    return S_OK;
  // Avoid null pointer dereference
  if (!_pTextService || !_pContext)
    return S_OK;

  _pTextService->_ClearCompositionDisplayAttributes(ec, _pContext);

  com_ptr<ITfRange> pCompositionRange;
  if (_clear && _pComposition->GetRange(&pCompositionRange) == S_OK)
    pCompositionRange->SetText(ec, 0, L"", 0);

  // Drop ownership before EndComposition(). Some applications notify
  // OnCompositionTerminated synchronously while the old composition ends.
  // Keeping it as the current composition makes that normal notification
  // look like an external abort and can clear a new Rime composition during
  // auto-commit.
  if (_pTextService && _pTextService->_IsCurrentComposition(_pComposition))
    _pTextService->_FinalizeComposition();
  HRESULT hr = _pComposition->EndComposition(ec);
  // Send undo only after the old composition's edit session has ended it.
  if (SUCCEEDED(hr) && _undoInputTag)
    _pTextService->_SimulateUndo(_undoInputTag);
  return S_OK;
}

void WeaselTSF::_EndComposition(com_ptr<ITfContext> pContext,
                                BOOL clear,
                                BOOL endUI,
                                ULONG_PTR undoInputTag) {
  CEndCompositionEditSession* pEditSession;
  HRESULT hr;
  com_ptr<ITfComposition> pComposition = _pComposition;

  if (!pContext && pComposition) {
    com_ptr<ITfRange> pRange;
    if (SUCCEEDED(pComposition->GetRange(&pRange)) && pRange) {
      pRange->GetContext(&pContext);
    }
  }

  if (endUI)
    _cand->EndUI();
  if (pContext &&
      (pEditSession = new CEndCompositionEditSession(
           this, pContext, pComposition, clear, undoInputTag)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }
}

/* Get Text Extent */
class CGetTextExtentEditSession : public CEditSession {
 public:
  CGetTextExtentEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfContextView> pContextView,
                            com_ptr<ITfComposition> pComposition,
                            bool enhancedPosition)
      : CEditSession(pTextService, pContext) {
    _pContextView = pContextView;
    _pComposition = pComposition;
    _enhancedPosition = enhancedPosition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfContextView> _pContextView;
  com_ptr<ITfComposition> _pComposition;
  bool _enhancedPosition;
};

STDMETHODIMP CGetTextExtentEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  ITfRange* pRange;
  RECT rc;
  BOOL fClipped;
  TF_SELECTION selection;
  ULONG nSelection;

  if (FAILED(_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                       (LPVOID*)&pInsertAtSelection)))
    return E_FAIL;
  if (FAILED(_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection,
                                     &nSelection)))
    return E_FAIL;

  if (_pComposition != nullptr && _pComposition->GetRange(&pRange) == S_OK) {
    pRange->Collapse(ec, TF_ANCHOR_START);
  } else {
    // composition end
    // note: selection.range is always an empty range
    pRange = selection.range;
  }

  if ((_pContextView->GetTextExt(ec, pRange, &rc, &fClipped)) == S_OK &&
      (rc.left != 0 || rc.top != 0)) {
    // get the foreground window pos and check if rc from GetTextExt is out of
    // window
    if (_enhancedPosition) {
      HWND hwnd;
      RECT rcForegroundWindow;
      hwnd = GetForegroundWindow();
      ::GetWindowRect(hwnd, &rcForegroundWindow);

      if (rc.left < rcForegroundWindow.left ||
          rc.left > rcForegroundWindow.right ||
          rc.top < rcForegroundWindow.top ||
          rc.top > rcForegroundWindow.bottom) {
        POINT pt;
        bool hasCaret = ::GetCaretPos(&pt);
        int offsetx = rcForegroundWindow.left - rc.left + (hasCaret ? pt.x : 0);
        int offsety = rcForegroundWindow.top - rc.top + (hasCaret ? pt.y : 0);
        rc.left += offsetx;
        rc.right += offsetx;
        rc.top += offsety;
        rc.bottom += offsety;
      }
    }
    _pTextService->_SetCompositionPosition(rc);
  }
  return S_OK;
}

/* Composition Window Handling */
BOOL WeaselTSF::_UpdateCompositionWindow(com_ptr<ITfContext> pContext) {
  com_ptr<ITfContextView> pContextView;
  if (pContext->GetActiveView(&pContextView) != S_OK)
    return FALSE;
  com_ptr<CGetTextExtentEditSession> pEditSession;
  pEditSession.Attach(
      new CGetTextExtentEditSession(this, pContext, pContextView, _pComposition,
                                    _cand->style().enhanced_position));
  if (pEditSession == NULL) {
    return FALSE;
  }
  HRESULT hr;
  pContext->RequestEditSession(_tfClientId, pEditSession,
                               TF_ES_ASYNCDONTCARE | TF_ES_READ, &hr);
  return SUCCEEDED(hr);
}

void WeaselTSF::_SetCompositionPosition(const RECT& rc) {
  /* Test if rect is valid.
   * If it is invalid during CUAS test, we need to apply CUAS workaround */
  if (!_fCUASWorkaroundTested) {
    _fCUASWorkaroundTested = TRUE;
    if (rc.top == rc.bottom) {
      _fCUASWorkaroundEnabled = TRUE;
      return;
    }
  }
  RECT _rc;
  _rc.left = _rc.right = rc.left;
  _rc.top = _rc.bottom = rc.bottom;
  m_client.UpdateInputPosition(rc);
  _cand->UpdateInputPosition(rc);
}

/* Inline Preedit */
class CInlinePreeditEditSession : public CEditSession {
 public:
  CInlinePreeditEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfComposition> pComposition,
                            const std::shared_ptr<weasel::Context> context)
      : CEditSession(pTextService, pContext),
        _pComposition(pComposition),
        _context(context) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  const std::shared_ptr<weasel::Context> _context;
};

STDMETHODIMP CInlinePreeditEditSession::DoEditSession(TfEditCookie ec) {
  std::wstring preedit = _context->preedit.str;

  com_ptr<ITfRange> pRangeComposition;
  if (_pComposition == nullptr)
    return E_FAIL;
  if ((_pComposition->GetRange(&pRangeComposition)) != S_OK)
    return E_FAIL;

  if ((pRangeComposition->SetText(ec, 0, preedit.c_str(),
                                  static_cast<LONG>(preedit.length()))) != S_OK)
    return E_FAIL;

  /* TODO: Check the availability and correctness of these values */
  int sel_cursor = -1;
  for (size_t i = 0; i < _context->preedit.attributes.size(); i++) {
    if (_context->preedit.attributes.at(i).type == weasel::HIGHLIGHTED) {
      sel_cursor = _context->preedit.attributes.at(i).range.cursor;
      break;
    }
  }

  _pTextService->_SetCompositionDisplayAttributes(ec, _pContext,
                                                  pRangeComposition);

  /* Set caret */
  LONG cch;
  TF_SELECTION tfSelection;
  if (sel_cursor < 0) {
    pRangeComposition->Collapse(ec, TF_ANCHOR_END);
  } else {
    pRangeComposition->Collapse(ec, TF_ANCHOR_START);
    pRangeComposition->ShiftStart(ec, sel_cursor, &cch, NULL);
  }
  tfSelection.range = pRangeComposition;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;
  _pContext->SetSelection(ec, 1, &tfSelection);

  return S_OK;
}

BOOL WeaselTSF::_ShowInlinePreedit(
    com_ptr<ITfContext> pContext,
    const std::shared_ptr<weasel::Context> context) {
  com_ptr<CInlinePreeditEditSession> pEditSession;
  pEditSession.Attach(
      new CInlinePreeditEditSession(this, pContext, _pComposition, context));
  if (pEditSession != NULL) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
  return TRUE;
}

/* Update Composition */
class CInsertTextEditSession : public CEditSession {
 public:
  CInsertTextEditSession(com_ptr<WeaselTSF> pTextService,
                         com_ptr<ITfContext> pContext,
                         com_ptr<ITfComposition> pComposition,
                         const std::wstring& text)
      : CEditSession(pTextService, pContext),
        _text(text),
        _pComposition(pComposition) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  std::wstring _text;
  com_ptr<ITfComposition> _pComposition;
};

STDMETHODIMP CInsertTextEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfRange> pRange;
  TF_SELECTION tfSelection;
  HRESULT hRet = S_OK;

  if (_pComposition == nullptr)
    return E_FAIL;
  if (FAILED(_pComposition->GetRange(&pRange)))
    return E_FAIL;

  if (FAILED(pRange->SetText(ec, 0, _text.c_str(),
                             static_cast<LONG>(_text.length()))))
    return E_FAIL;

  _pTextService->_RememberLastCommit(ec, _pContext, pRange, _text);

  /* update the selection to an insertion point just past the inserted text. */
  pRange->Collapse(ec, TF_ANCHOR_END);

  tfSelection.range = pRange;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;

  _pContext->SetSelection(ec, 1, &tfSelection);

  return hRet;
}

BOOL WeaselTSF::_InsertText(com_ptr<ITfContext> pContext,
                            const std::wstring& text) {
  CInsertTextEditSession* pEditSession;
  HRESULT hr;

  if ((pEditSession = new CInsertTextEditSession(this, pContext, _pComposition,
                                                 text)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }

  return TRUE;
}

// Chromium drains queued edit locks before publishing changes to its editor.
// Post a private window message instead, so the current lock request returns
// to the host before composition restoration requests another edit lock.
class CResumeReopenEditSession : public CEditSession {
 public:
  CResumeReopenEditSession(com_ptr<WeaselTSF> service,
                           com_ptr<ITfContext> context,
                           com_ptr<ITfRange> range,
                           const std::wstring& text,
                           TfClientId clientId)
      : CEditSession(service, context),
        _range(range),
        _text(text),
        _clientId(clientId) {}
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    return _pTextService->_CompleteReopen(ec, _pContext, _range, _text);
  }
  bool Post() {
    HWND window =
        CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                        GetModuleHandleW(nullptr), nullptr);
    if (!window)
      return false;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    if (GetWindowLongPtrW(window, GWLP_USERDATA) !=
        reinterpret_cast<LONG_PTR>(this)) {
      DestroyWindow(window);
      return false;
    }
    if (!SetWindowLongPtrW(window, GWLP_WNDPROC,
                           reinterpret_cast<LONG_PTR>(&WindowProc))) {
      DestroyWindow(window);
      return false;
    }
    AddRef();  // The private window owns a reference until WM_NCDESTROY.
    if (!PostMessageW(window, WM_APP, 0, 0)) {
      DestroyWindow(window);
      return false;
    }
    return true;
  }

 private:
  static LRESULT CALLBACK WindowProc(HWND window,
                                     UINT message,
                                     WPARAM wParam,
                                     LPARAM lParam) {
    auto* session = reinterpret_cast<CResumeReopenEditSession*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_APP && session) {
      OutputDebugStringW(
          L"Weasel reopen: restore dispatched after message boundary\n");
      HRESULT result = E_FAIL;
      const HRESULT hr = session->_pContext->RequestEditSession(
          session->_clientId, session, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
          &result);
      if (FAILED(hr) || FAILED(result))
        OutputDebugStringW(
            L"Weasel reopen: posted restore edit lock rejected\n");
      DestroyWindow(window);
      return 0;
    }
    if (message == WM_NCDESTROY && session) {
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      session->Release();
    }
    return DefWindowProcW(window, message, wParam, lParam);
  }
  com_ptr<ITfRange> _range;
  std::wstring _text;
  TfClientId _clientId;
};

bool WeaselTSF::_QueueReopen(com_ptr<ITfContext> context,
                             com_ptr<ITfRange> range,
                             const std::wstring& text) {
  com_ptr<CResumeReopenEditSession> session;
  session.Attach(
      new CResumeReopenEditSession(this, context, range, text, _tfClientId));
  return session->Post();
}

void WeaselTSF::_ForgetLastCommit() {
  _last_commit_range = nullptr;
  _last_commit_end = nullptr;
  _last_commit_context = nullptr;
  _last_commit_text.clear();
  _InvalidateBackspace();
  _backspace_context = nullptr;
  _backspace_text.clear();
  _backspace_range_lost = false;
}

void WeaselTSF::_RememberLastCommit(TfEditCookie ec,
                                    com_ptr<ITfContext> context,
                                    com_ptr<ITfRange> range,
                                    const std::wstring& text) {
  _ForgetLastCommit();
  com_ptr<ITfRange> saved, end;
  if (text.empty() || !range || FAILED(range->Clone(&saved)) ||
      FAILED(range->Clone(&end)) || FAILED(end->Collapse(ec, TF_ANCHOR_END))) {
    OutputDebugStringW(L"Weasel reopen: cannot record committed range\n");
    return;
  }
  // Keep a separate end anchor: some hosts collapse the original range when
  // composition ends. Never reconstruct from an arbitrary current caret.
  saved->SetGravity(ec, TF_GRAVITY_FORWARD, TF_GRAVITY_BACKWARD);
  end->SetGravity(ec, TF_GRAVITY_BACKWARD, TF_GRAVITY_BACKWARD);
  _last_commit_context = context;
  _last_commit_range = saved;
  _last_commit_end = end;
  _last_commit_text = text;
  TF_STATUS status = {};
  const bool hasStatus = SUCCEEDED(context->GetStatus(&status));
  _ArmBackspace(context, text,
                hasStatus && (status.dwStaticFlags & TF_SS_TRANSITORY));
  if (hasStatus) {
    OutputDebugStringW(
        status.dwStaticFlags & TF_SS_TRANSITORY
            ? L"Weasel reopen: recorded in transitory context\n"
            : L"Weasel reopen: recorded in persistent context\n");
  }
}

bool WeaselTSF::_ValidateLastCommit(TfEditCookie ec,
                                    com_ptr<ITfContext> context) {
  auto fail = [](const wchar_t* reason) {
    OutputDebugStringW(reason);
    return false;
  };
  if (!_last_commit_range || !_last_commit_end)
    return fail(L"Weasel reopen: validation missing range anchor\n");
  if (context != _last_commit_context)
    return fail(L"Weasel reopen: validation context changed\n");

  const wchar_t* mismatch = L"Weasel reopen: validation text differs\n";
  auto matches = [&](com_ptr<ITfRange> range) {
    std::wstring actual(_last_commit_text.size() + 1, L'\0');
    ULONG fetched = 0;
    if (FAILED(range->GetText(ec, 0, actual.data(),
                              static_cast<ULONG>(actual.size()), &fetched)) ||
        fetched != _last_commit_text.size()) {
      mismatch =
          L"Weasel reopen: validation text unreadable or length differs\n";
      return false;
    }
    actual.resize(fetched);
    mismatch = L"Weasel reopen: validation text differs\n";
    return actual == _last_commit_text;
  };
  if (!matches(_last_commit_range)) {
    com_ptr<ITfRange> recovered;
    LONG shifted = 0;
    const LONG length = static_cast<LONG>(_last_commit_text.size());
    if (FAILED(_last_commit_end->Clone(&recovered)))
      return fail(L"Weasel reopen: validation saved anchor clone failed\n");
    if (FAILED(recovered->ShiftStart(ec, -length, &shifted, nullptr)))
      return fail(L"Weasel reopen: validation backward range shift failed\n");
    if (shifted != -length)
      return fail(
          L"Weasel reopen: validation saved anchor cannot reach committed "
          L"text\n");
    if (!matches(recovered))
      return fail(mismatch);
    recovered->SetGravity(ec, TF_GRAVITY_FORWARD, TF_GRAVITY_BACKWARD);
    _last_commit_range = recovered;
  }

  com_ptr<ITfRange> position;
  if (_IsComposing()) {
    if (FAILED(_pComposition->GetRange(&position)))
      return fail(L"Weasel reopen: validation composition range unavailable\n");
    LONG comparison = 0;
    if (SUCCEEDED(position->CompareStart(ec, _last_commit_end, TF_ANCHOR_START,
                                         &comparison)) &&
        comparison == 0)
      return true;

    // OnEndEdit may run between an asynchronous insert session and its end
    // session. The just-committed composition still covers the saved range;
    // keep the record only if both ranges match and the caret is at its end.
    LONG start = 0, end = 0;
    if (FAILED(position->CompareStart(ec, _last_commit_range, TF_ANCHOR_START,
                                      &start)) ||
        FAILED(position->CompareEnd(ec, _last_commit_range, TF_ANCHOR_END,
                                    &end)) ||
        start != 0 || end != 0)
      return fail(L"Weasel reopen: validation composition is not adjacent\n");
    position = nullptr;
  }
  TF_SELECTION selection = {};
  ULONG count = 0;
  if (FAILED(context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection,
                                   &count)) ||
      count != 1)
    return fail(L"Weasel reopen: validation selection unavailable\n");
  position.Attach(selection.range);
  BOOL empty = FALSE;
  if (FAILED(position->IsEmpty(ec, &empty)) || !empty)
    return fail(L"Weasel reopen: validation selection is not empty\n");
  LONG comparison = 0;
  if (FAILED(position->CompareStart(ec, _last_commit_end, TF_ANCHOR_START,
                                    &comparison)) ||
      comparison != 0)
    return fail(L"Weasel reopen: validation caret is not at saved end\n");
  return true;
}

bool WeaselTSF::_DetectTransientReset(TfEditCookie ec,
                                      com_ptr<ITfContext> context) {
  if (!_backspace_armed || !_backspace_transitory || _IsComposing() ||
      context != _backspace_context || !_last_commit_range || !_last_commit_end)
    return false;
  com_ptr<ITfRange> begin, end;
  BOOL savedEmpty = FALSE, anchorEmpty = FALSE;
  LONG atBegin = 1, atEnd = 1;
  if (FAILED(_last_commit_range->IsEmpty(ec, &savedEmpty)) || !savedEmpty ||
      FAILED(_last_commit_end->IsEmpty(ec, &anchorEmpty)) || !anchorEmpty ||
      FAILED(context->GetStart(ec, &begin)) ||
      FAILED(context->GetEnd(ec, &end)) ||
      FAILED(_last_commit_end->CompareStart(ec, begin, TF_ANCHOR_START,
                                            &atBegin)) ||
      FAILED(_last_commit_end->CompareStart(ec, end, TF_ANCHOR_END, &atEnd)) ||
      atBegin != 0 || atEnd != 0)
    return false;
  _backspace_range_lost = true;
  _last_commit_range = nullptr;
  _last_commit_end = nullptr;
  _last_commit_context = nullptr;
  _last_commit_text.clear();
  OutputDebugStringW(
      L"Weasel reopen: temporary context reset, backspace eligible\n");
  return true;
}

bool WeaselTSF::_RequestBackspace(TfEditCookie ec,
                                  com_ptr<ITfContext> context,
                                  const std::wstring& text) {
  if (!_backspace_armed || !_backspace_h_trigger ||
      context != _backspace_context || GetFocus() != _backspace_focus ||
      text.empty() || text != _backspace_text || text.size() > 128 ||
      _undo_input_active || _undo_marker_release)
    return false;
  // A forced override is still subject to exact range validation when the
  // host exposes the range. Only a confirmed empty transitory reset bypasses
  // it.
  if (!_backspace_range_lost && !_ValidateLastCommit(ec, context))
    return false;
  for (wchar_t ch : text) {
    const bool ordinary =
        (ch >= 0x20 && ch <= 0x7e) || (ch >= 0x3400 && ch <= 0x4dbf) ||
        (ch >= 0x4e00 && ch <= 0x9fff) || (ch >= 0xf900 && ch <= 0xfaff) ||
        (ch >= 0x3000 && ch <= 0x3029) || (ch >= 0xff01 && ch <= 0xff60);
    if (!ordinary) {
      OutputDebugStringW(L"Weasel reopen: backspace refuses complex text\n");
      return false;
    }
  }
  const unsigned count = static_cast<unsigned>(text.size());
  _ForgetLastCommit();
  OutputDebugStringW(L"Weasel reopen: using backspace compatibility\n");
  _RequestInputAction(context, count);
  return true;
}

bool WeaselTSF::_ReopenLastCommit(TfEditCookie ec,
                                  com_ptr<ITfContext> context,
                                  const std::wstring& expectedText) {
  _FinishReopen();
  if (!_last_commit_range) {
    OutputDebugStringW(L"Weasel reopen: no recorded commit\n");
    return false;
  }
  if (expectedText.empty() || expectedText != _last_commit_text) {
    OutputDebugStringW(L"Weasel reopen: history and last commit differ\n");
    return false;
  }
  if (!_ValidateLastCommit(ec, context)) {
    OutputDebugStringW(
        L"Weasel reopen: range, text or caret validation failed\n");
    return false;
  }

  // End the trigger synchronously under this edit lock. A queued end session
  // could otherwise run after the replacement composition has been created.
  if (_IsComposing()) {
    com_ptr<ITfComposition> trigger = _pComposition;
    com_ptr<ITfRange> range;
    LONG comparison = 0;
    if (FAILED(trigger->GetRange(&range)) ||
        FAILED(range->CompareStart(ec, _last_commit_end, TF_ANCHOR_START,
                                   &comparison)) ||
        comparison != 0 || FAILED(range->SetText(ec, 0, L"", 0))) {
      OutputDebugStringW(L"Weasel reopen: cannot clear trigger range\n");
      return false;
    }
    _ClearCompositionDisplayAttributes(ec, context);
    _FinalizeComposition();
    if (FAILED(trigger->EndComposition(ec))) {
      OutputDebugStringW(L"Weasel reopen: cannot end trigger composition\n");
      return false;
    }
  }
  if (!_ValidateLastCommit(ec, context)) {
    OutputDebugStringW(L"Weasel reopen: range invalid after trigger ended\n");
    return false;
  }

  com_ptr<ITfRange> range;
  if (FAILED(_last_commit_range->Clone(&range)) ||
      FAILED(range->SetGravity(ec, TF_GRAVITY_BACKWARD, TF_GRAVITY_FORWARD))) {
    OutputDebugStringW(L"Weasel reopen: cannot prepare replacement range\n");
    return false;
  }
  // Chromium and some editor hosts treat composition over committed text as
  // a fresh insertion. Delete the verified span first, then start at its now
  // empty position; never delegate removal to the host's reconversion logic.
  _reopen_text = expectedText;
  _reopen_range = range;
  if (FAILED(range->SetText(ec, 0, L"", 0))) {
    _FinishReopen();
    OutputDebugStringW(L"Weasel reopen: target deletion failed\n");
    return false;
  }
  BOOL empty = FALSE;
  if (FAILED(range->IsEmpty(ec, &empty)) || !empty) {
    _RollbackReopen(ec, context);
    OutputDebugStringW(L"Weasel reopen: target range did not become empty\n");
    return false;
  }
  TF_SELECTION selection = {};
  selection.range = range;
  selection.style.ase = TF_AE_NONE;
  selection.style.fInterimChar = FALSE;
  if (FAILED(context->SetSelection(ec, 1, &selection))) {
    _RollbackReopen(ec, context);
    OutputDebugStringW(L"Weasel reopen: cannot position after deletion\n");
    return false;
  }
  _ForgetLastCommit();
  if (!_QueueReopen(context, range, expectedText)) {
    _RollbackReopen(ec, context);
    OutputDebugStringW(L"Weasel reopen: cannot queue restore session\n");
    return false;
  }
  OutputDebugStringW(L"Weasel reopen: deletion staged, restore queued\n");
  return true;
}

HRESULT WeaselTSF::_CompleteReopen(TfEditCookie ec,
                                   com_ptr<ITfContext> context,
                                   com_ptr<ITfRange> range,
                                   const std::wstring& text) {
  BOOL empty = FALSE;
  if (FAILED(range->IsEmpty(ec, &empty)) || !empty) {
    if (_reopen_range == range)
      _FinishReopen();
    OutputDebugStringW(L"Weasel reopen: restore cancelled, target changed\n");
    return S_OK;
  }
  if (_reopen_range != range) {
    // A focus loss/abort cancelled the request between edit sessions. Restore
    // only a still-empty span, without changing the user's current selection.
    if (FAILED(range->SetText(ec, 0, text.c_str(),
                              static_cast<LONG>(text.size()))))
      OutputDebugStringW(
          L"Weasel reopen: cancelled restore text write failed\n");
    return S_OK;
  }
  TF_SELECTION selection = {};
  ULONG count = 0;
  com_ptr<ITfRange> caret;
  LONG start = 1, end = 1;
  if (SUCCEEDED(context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection,
                                      &count)) &&
      count == 1) {
    caret.Attach(selection.range);
    caret->CompareStart(ec, range, TF_ANCHOR_START, &start);
    caret->CompareEnd(ec, range, TF_ANCHOR_END, &end);
  }
  if (context != _pEditSessionContext || _IsComposing() || start != 0 ||
      end != 0) {
    // Keep a moved selection where the user put it. The range was verified
    // empty above, so restoring its text does not replace a new composition.
    if (FAILED(range->SetText(ec, 0, text.c_str(),
                              static_cast<LONG>(text.size()))))
      OutputDebugStringW(
          L"Weasel reopen: cancelled restore text write failed\n");
    _FinishReopen();
    OutputDebugStringW(
        L"Weasel reopen: restore cancelled, context or caret changed\n");
    return S_OK;
  }
  com_ptr<ITfContextComposition> compositions;
  com_ptr<ITfComposition> composition;
  if (FAILED(context->QueryInterface(IID_ITfContextComposition,
                                     (LPVOID*)&compositions)) ||
      FAILED(compositions->StartComposition(ec, range, this, &composition)) ||
      !composition) {
    _RollbackReopen(ec, context);
    OutputDebugStringW(
        L"Weasel reopen: host rejected composition, rolled back\n");
    return S_OK;
  }
  _SetComposition(composition);
  _cand->StartUI();
  OutputDebugStringW(L"Weasel reopen: ready for configuration callback\n");
  if (m_client.ProcessKeyEvent(weasel::KeyEvent(ibus::F35, 0))) {
    const HRESULT result = DoEditSession(ec);
    _FinishReopen();
    return result;
  }
  _RollbackReopen(ec, context);
  _EndComposition(context, false);
  return S_OK;
}

void WeaselTSF::_FinishReopen() {
  _reopen_range = nullptr;
  _reopen_text.clear();
}

void WeaselTSF::_RollbackReopen(TfEditCookie ec, com_ptr<ITfContext> context) {
  if (_reopen_range && !_reopen_text.empty()) {
    if (SUCCEEDED(
            _reopen_range->SetText(ec, 0, _reopen_text.c_str(),
                                   static_cast<LONG>(_reopen_text.size())))) {
      _reopen_range->Collapse(ec, TF_ANCHOR_END);
      TF_SELECTION selection = {};
      selection.range = _reopen_range;
      selection.style.ase = TF_AE_NONE;
      selection.style.fInterimChar = FALSE;
      context->SetSelection(ec, 1, &selection);
    } else {
      OutputDebugStringW(L"Weasel reopen: rollback text write failed\n");
    }
  }
  _FinishReopen();
}

void WeaselTSF::_UpdateComposition(com_ptr<ITfContext> pContext) {
  HRESULT hr;

  _pEditSessionContext = pContext;

  _pEditSessionContext->RequestEditSession(
      _tfClientId, this, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  _async_edit = !!(hr == TF_S_ASYNC);
}

/* Composition State */
STDMETHODIMP WeaselTSF::OnCompositionTerminated(TfEditCookie ecWrite,
                                                ITfComposition* pComposition) {
  // NOTE:
  // This will be called when an edit session ended up with an empty composition
  // string, Even if it is closed normally. Silly M$.

  // EndComposition() may generate this callback for the composition we just
  // closed. Only an active, matching composition is an external termination.
  if (!_IsCurrentComposition(pComposition))
    return S_OK;

  // In inline preedit mode, an external termination (such as switching window
  // via Alt+Tab, mouse click away, or host app terminating composition) must
  // clear the partially composed inline text so the host does not commit it.
  if (_cand->style().inline_preedit) {
    com_ptr<ITfRange> pRange;
    if (pComposition && SUCCEEDED(pComposition->GetRange(&pRange)) && pRange) {
      pRange->SetText(ecWrite, 0, L"", 0);
      com_ptr<ITfContext> pContext;
      if (SUCCEEDED(pRange->GetContext(&pContext)) && pContext) {
        _ClearCompositionDisplayAttributes(ecWrite, pContext);
      }
    }
    _FinalizeComposition();
    _AbortComposition(false);
    return S_OK;
  }

  // A host may terminate the empty TSF composition used for a non-inline
  // preedit. Keep Rime's composing state; the next key will create a fresh
  // TSF composition. Only an inactive Rime session should be aborted here.
  if (_status.composing) {
    _FinalizeComposition();
    return S_OK;
  }

  _AbortComposition();
  return S_OK;
}

void WeaselTSF::_AbortComposition(bool clear) {
  _StopCloudPolling();
  _CancelUndo();
  _ForgetLastCommit();
  _FinishReopen();
  m_client.ClearComposition();
  _status.composing = false;
  if (_IsComposing()) {
    _EndComposition(_pEditSessionContext, clear);
  }
  _committed = TRUE;
  _cand->Destroy();
}

void WeaselTSF::_FinalizeComposition() {
  _pComposition = nullptr;
}

void WeaselTSF::_SetComposition(com_ptr<ITfComposition> pComposition) {
  _pComposition = pComposition;
}

BOOL WeaselTSF::_IsComposing() {
  return _pComposition != NULL;
}

BOOL WeaselTSF::_IsCurrentComposition(ITfComposition* pComposition) {
  return _pComposition != nullptr && _pComposition == pComposition;
}
