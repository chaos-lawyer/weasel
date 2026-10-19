#include "stdafx.h"
#include "WeaselTSF.h"
#include "EditSession.h"
#include "ResponseParser.h"
#include "CandidateList.h"
#include <WordCommitTrace.h>
#include <ExternalTemplate.h>

#include <utility>

static void TraceWordCommit(const wchar_t* message) {
  OutputDebugStringW(message);
  weasel::AppendWordCommitTrace(message);
}

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
                                (LPVOID*)&pInsertAtSelection) != S_OK) {
    TraceWordCommit(
        L"[weasel_tsf_commit_probe] event=start stage=query_insert "
        L"result=failed\n");
    return hr;
  }
  if (pInsertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, NULL, 0,
                                                &pRangeComposition) != S_OK) {
    TraceWordCommit(
        L"[weasel_tsf_commit_probe] event=start stage=query_range "
        L"result=failed\n");
    return hr;
  }

  com_ptr<ITfContextComposition> pContextComposition;
  com_ptr<ITfComposition> pComposition;
  if (_pContext->QueryInterface(IID_ITfContextComposition,
                                (LPVOID*)&pContextComposition) != S_OK) {
    TraceWordCommit(
        L"[weasel_tsf_commit_probe] event=start stage=query_composition "
        L"result=failed\n");
    return hr;
  }
  const HRESULT start_hr = pContextComposition->StartComposition(
      ec, pRangeComposition, _pTextService, &pComposition);
  if (start_hr == S_OK && pComposition != NULL) {
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
    hr = S_OK;
  }
  wchar_t message[192];
  swprintf_s(message,
             L"[weasel_tsf_commit_probe] event=start stage=callback "
             L"start_hr=0x%08X composition=%d return_hr=0x%08X\n",
             static_cast<unsigned int>(start_hr), pComposition ? 1 : 0,
             static_cast<unsigned int>(hr));
  TraceWordCommit(message);

  return hr;
}

void WeaselTSF::_StartComposition(com_ptr<ITfContext> pContext,
                                  BOOL fCUASWorkaroundEnabled) {
  com_ptr<CStartCompositionEditSession> pStartCompositionEditSession;
  pStartCompositionEditSession.Attach(
      new CStartCompositionEditSession(this, pContext, fCUASWorkaroundEnabled));
  _cand->StartUI();
  if (pStartCompositionEditSession != nullptr) {
    HRESULT hr = E_FAIL;
    const HRESULT request_hr = pContext->RequestEditSession(
        _tfClientId, pStartCompositionEditSession,
        TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    wchar_t message[192];
    swprintf_s(message,
               L"[weasel_tsf_commit_probe] event=start stage=request "
               L"request_hr=0x%08X session_hr=0x%08X\n",
               static_cast<unsigned int>(request_hr),
               static_cast<unsigned int>(hr));
    TraceWordCommit(message);
  }
}

/* End Composition */
class CEndCompositionEditSession : public CEditSession {
 public:
  CEndCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                             com_ptr<ITfContext> pContext,
                             com_ptr<ITfComposition> pComposition,
                             BOOL clear = TRUE,
                             ULONG_PTR undoInputTag = 0,
                             HRESULT* completionResult = nullptr)
      : CEditSession(pTextService, pContext),
        _clear(clear),
        _undoInputTag(undoInputTag),
        _completionResult(completionResult) {
    _pComposition = pComposition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  BOOL _clear;
  ULONG_PTR _undoInputTag;
  HRESULT* _completionResult;
};

STDMETHODIMP CEndCompositionEditSession::DoEditSession(TfEditCookie ec) {
  if (_completionResult)
    *_completionResult = E_FAIL;
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
  if (_completionResult)
    *_completionResult = hr;
  _pTextService->_CompleteCursorPlacement(ec, _pContext, _clear || FAILED(hr));
  _pTextService->_CompletePunctuationCommit(_pContext,
                                            !_clear && SUCCEEDED(hr));
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
  if (_pComposition == nullptr) {
    TraceWordCommit(
        L"[weasel_tsf_commit_probe] event=preedit stage=composition "
        L"result=missing\n");
    return E_FAIL;
  }
  const HRESULT range_hr = _pComposition->GetRange(&pRangeComposition);
  if (range_hr != S_OK) {
    wchar_t message[192];
    swprintf_s(message,
               L"[weasel_tsf_commit_probe] event=preedit stage=get_range "
               L"hr=0x%08X\n",
               static_cast<unsigned int>(range_hr));
    TraceWordCommit(message);
    return E_FAIL;
  }

  const HRESULT write_hr = pRangeComposition->SetText(
      ec, 0, preedit.c_str(), static_cast<LONG>(preedit.length()));
  if (write_hr != S_OK) {
    wchar_t message[192];
    swprintf_s(message,
               L"[weasel_tsf_commit_probe] event=preedit stage=set_text "
               L"hr=0x%08X\n",
               static_cast<unsigned int>(write_hr));
    TraceWordCommit(message);
    return E_FAIL;
  }

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
    HRESULT hr = E_FAIL;
    const HRESULT request_hr = pContext->RequestEditSession(
        _tfClientId, pEditSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    if (FAILED(request_hr) || FAILED(hr)) {
      wchar_t message[192];
      swprintf_s(message,
                 L"[weasel_tsf_commit_probe] event=preedit stage=request "
                 L"request_hr=0x%08X session_hr=0x%08X\n",
                 static_cast<unsigned int>(request_hr),
                 static_cast<unsigned int>(hr));
      TraceWordCommit(message);
    }
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

// Return to the host message loop after cursor movement before asking Rime
// to insert punctuation. A TSF edit cookie or key marker is not that boundary.
class CDeferredPunctuationJump {
 public:
  CDeferredPunctuationJump(com_ptr<WeaselTSF> service, UINT serial)
      : _service(service), _serial(serial) {}

  static bool Post(com_ptr<WeaselTSF> service, UINT serial) {
    std::unique_ptr<CDeferredPunctuationJump> callback(
        new CDeferredPunctuationJump(service, serial));
    HWND window =
        CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                        GetModuleHandleW(nullptr), nullptr);
    if (!window)
      return false;
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(callback.get()));
    if (GetWindowLongPtrW(window, GWLP_USERDATA) !=
            reinterpret_cast<LONG_PTR>(callback.get()) ||
        !SetWindowLongPtrW(window, GWLP_WNDPROC,
                           reinterpret_cast<LONG_PTR>(&WindowProc))) {
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      DestroyWindow(window);
      return false;
    }
    callback.release();  // The private window owns the service reference.
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
    auto* callback = reinterpret_cast<CDeferredPunctuationJump*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_APP && callback) {
      callback->_service->_FinishPunctuationJump(callback->_serial);
      DestroyWindow(window);
      return 0;
    }
    if (message == WM_NCDESTROY && callback) {
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      delete callback;
    }
    return DefWindowProcW(window, message, wParam, lParam);
  }
  com_ptr<WeaselTSF> _service;
  UINT _serial;
};

bool WeaselTSF::_QueuePunctuationJump() {
  if (!_punct_jump_context)
    return true;
  if (_punct_jump_queued_serial == _punct_jump_serial)
    return true;
  if (CDeferredPunctuationJump::Post(this, _punct_jump_serial)) {
    _punct_jump_queued_serial = _punct_jump_serial;
    return true;
  }
  OutputDebugStringW(L"Weasel snippet: deferred punctuation post failed\n");
  _punct_jump_context = nullptr;
  return false;
}

struct CursorSlot {
  LONG offset;
  LONG length;
};

static size_t ParseCursorPlaceholders(const std::wstring& input,
                                      std::wstring* output,
                                      std::vector<CursorSlot>* slots) {
  if (!output || !slots)
    return 0;

  slots->clear();

  struct RawItem {
    size_t pos;
    size_t len;
    std::wstring custom_label;
  };

  std::vector<RawItem> found;

  size_t idx = 0;
  while (idx < input.length()) {
    size_t doll = input.find(L'$', idx);
    if (doll == std::wstring::npos) {
      break;
    }

    size_t match_len = 0;
    std::wstring custom_label;

    if (doll + 1 < input.length()) {
      wchar_t c1 = input[doll + 1];
      if (c1 == L'|') {
        match_len = 2;
      } else if (c1 >= L'1' && c1 <= L'9') {
        match_len = 2;
      } else if (c1 == L'{') {
        size_t close_brace = input.find(L'}', doll + 2);
        if (close_brace != std::wstring::npos) {
          std::wstring inside =
              input.substr(doll + 2, close_brace - (doll + 2));
          match_len = close_brace - doll + 1;
          if (inside == L"cursor") {
            // pure cursor without label
          } else {
            size_t colon = inside.find_first_of(L":：");
            if (colon != std::wstring::npos) {
              std::wstring after_colon = inside.substr(colon + 1);
              if (!after_colon.empty()) {
                custom_label = L"{" + after_colon + L"}";
              }
            } else {
              bool all_digits = !inside.empty();
              for (wchar_t ch : inside) {
                if (ch < L'0' || ch > L'9') {
                  all_digits = false;
                  break;
                }
              }
              if (!all_digits && !inside.empty()) {
                custom_label = L"{" + inside + L"}";
              }
            }
          }
        }
      } else if (doll + 6 < input.length() &&
                 input.compare(doll, 7, L"$cursor") == 0) {
        match_len = 7;
      }
    }

    if (match_len > 0) {
      found.push_back({doll, match_len, custom_label});
      idx = doll + match_len;
    } else {
      idx = doll + 1;
    }
  }

  if (found.empty()) {
    *output = input;
    return 0;
  }

  if (found.size() == 1) {
    if (found[0].custom_label.empty()) {
      // Single cursor placeholder without label: erase it, clean text without
      // placeholder!
      *output = input;
      output->erase(found[0].pos, found[0].len);
      slots->push_back({static_cast<LONG>(found[0].pos), 0});
      return 1;
    }
    // Single cursor placeholder WITH custom label (e.g. ${合同名称}):
    // Replace with custom label and select it so user can overwrite it!
    output->clear();
    output->append(input, 0, found[0].pos);
    LONG cur_offset = static_cast<LONG>(output->length());
    LONG cur_len = static_cast<LONG>(found[0].custom_label.length());
    output->append(found[0].custom_label);
    output->append(input, found[0].pos + found[0].len,
                   input.length() - (found[0].pos + found[0].len));
    slots->push_back({cur_offset, cur_len});
    return 1;
  }

  // Multiple cursor placeholders:
  output->clear();
  size_t last_pos = 0;
  for (size_t i = 0; i < found.size(); ++i) {
    output->append(input, last_pos, found[i].pos - last_pos);
    std::wstring label = found[i].custom_label.empty()
                             ? (L"$" + std::to_wstring(i + 1))
                             : found[i].custom_label;
    LONG cur_offset = static_cast<LONG>(output->length());
    LONG cur_len = static_cast<LONG>(label.length());
    output->append(label);
    slots->push_back({cur_offset, cur_len});
    last_pos = found[i].pos + found[i].len;
  }
  if (last_pos < input.length()) {
    output->append(input, last_pos, input.length() - last_pos);
  }
  return found.size();
}

class CTabJumpEditSession : public CEditSession {
 public:
  CTabJumpEditSession(com_ptr<WeaselTSF> pTextService,
                      com_ptr<ITfContext> pContext,
                      com_ptr<ITfRange> pRange,
                      bool is_selection)
      : CEditSession(pTextService, pContext),
        _pRange(pRange),
        _is_selection(is_selection) {}

  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    if (!_pRange || !_pContext)
      return E_FAIL;
    TF_SELECTION tfSelection = {};
    if (!_is_selection && FAILED(_pRange->Collapse(ec, TF_ANCHOR_END)))
      return E_FAIL;
    tfSelection.range = _pRange;
    tfSelection.style.ase = TF_AE_NONE;
    tfSelection.style.fInterimChar = FALSE;
    return _pContext->SetSelection(ec, 1, &tfSelection);
  }

 private:
  com_ptr<ITfRange> _pRange;
  bool _is_selection;
};

// Transitory text stores (including Chromium and IMM bridges) do not keep
// document ranges after committing. Navigate their unchanged template suffix
// with ordinary keys, delivered only after EndComposition releases its lock.
static LONG CursorKeyCount(const std::wstring& text, size_t start, size_t end) {
  LONG count = 0;
  for (size_t i = start; i < end; ++i) {
    if (i + 1 < end && ((text[i] >= 0xd800 && text[i] <= 0xdbff &&
                         text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) ||
                        (text[i] == L'\r' && text[i + 1] == L'\n')))
      ++i;
    ++count;
  }
  return count;
}

bool WeaselTSF::_SendCursorKeys(WORD direction,
                                LONG steps,
                                LONG selectionLength,
                                bool collapseSelection) {
  // Bound queued input and reject invalid movement before allocating it.
  if (steps < 0 || steps > 4096 || selectionLength < 0 ||
      selectionLength > 64 || (direction != VK_LEFT && direction != VK_RIGHT))
    return false;
  // Do not inherit the key used to select the candidate as a modifier.
  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000) ||
      (shift && !_punct_jump_context))
    return false;
  std::vector<INPUT> inputs;
  auto key = [&](WORD vk, DWORD flags) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = flags;
    if (vk == VK_LEFT || vk == VK_RIGHT)
      input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    input.ki.dwExtraInfo = kCursorInputMarker;
    inputs.push_back(input);
  };
  auto press = [&](WORD vk) {
    key(vk, 0);
    key(vk, KEYEVENTF_KEYUP);
  };
  // Shifted punctuation retains its captured Rime mask; navigation itself
  // must not extend the document selection.
  if (shift)
    key(VK_SHIFT, KEYEVENTF_KEYUP);
  if (collapseSelection)
    press(VK_RIGHT);
  for (LONG i = 0; i < steps; ++i)
    press(direction);
  if (selectionLength > 0) {
    key(VK_SHIFT, 0);
    for (LONG i = 0; i < selectionLength; ++i)
      press(VK_RIGHT);
    key(VK_SHIFT, KEYEVENTF_KEYUP);
  }
  if (shift)
    key(VK_SHIFT, 0);
  // Drain even if TSF drops dwExtraInfo on the simulated navigation events.
  press(VK_F23);
  _cursor_input_active = true;
  const UINT sent =
      SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
  if (sent != inputs.size()) {
    _cursor_input_active = false;
    if (sent > 0) {
      // Release exactly the keys whose key-down was accepted. A short batch
      // must never leave an arrow or Shift logically held down.
      std::vector<INPUT> releases;
      for (UINT i = 0; i < sent; ++i) {
        if (inputs[i].ki.dwFlags & KEYEVENTF_KEYUP)
          continue;
        bool released = false;
        for (UINT j = i + 1; j < sent; ++j) {
          if (inputs[j].ki.wVk == inputs[i].ki.wVk &&
              (inputs[j].ki.dwFlags & KEYEVENTF_KEYUP)) {
            released = true;
            break;
          }
        }
        if (!released) {
          INPUT release = inputs[i];
          release.ki.dwFlags |= KEYEVENTF_KEYUP;
          releases.push_back(release);
        }
      }
      if (!releases.empty())
        SendInput(static_cast<UINT>(releases.size()), releases.data(),
                  sizeof(INPUT));
    }
    return false;
  }
  return true;
}

void WeaselTSF::_CompleteCursorPlacement(TfEditCookie ec,
                                         ITfContext* context,
                                         bool clear) {
  if (!_cursor_placement_context || context != _cursor_placement_context)
    return;
  const auto target = _cursor_placement;
  const bool keyboard = _cursor_placement_keyboard;
  const HWND focus = _cursor_placement_focus;
  _cursor_placement_context = nullptr;
  _cursor_placement = {};
  if (clear || focus != GetFocus()) {
    _ClearTabJumpSession();
    return;
  }
  HRESULT hr = E_FAIL;
  if (!keyboard && target.range) {
    TF_SELECTION selection = {};
    selection.range = target.range;
    selection.style.ase = TF_AE_NONE;
    hr = context->SetSelection(ec, 1, &selection);
  }
  if (FAILED(hr)) {
    // Some persistent stores also reject selection/range operations. The
    // caret is still at the commit end, so the same suffix fallback is safe.
    _tab_jump_keyboard = true;
    hr = _SendCursorKeys(VK_LEFT, target.steps, target.selection_length)
             ? S_OK
             : E_FAIL;
  }
  if (FAILED(hr)) {
    OutputDebugStringW(L"Weasel snippet: initial cursor placement failed\n");
    _ClearTabJumpSession();
  }
}

void WeaselTSF::_ClearTabJumpSession() {
  _tab_jump_targets.clear();
  _tab_jump_index = 0;
  _tab_jump_caret_offset = 0;
  _tab_jump_context = nullptr;
  _tab_jump_selection = false;
  _cursor_placement_context = nullptr;
  _cursor_placement = {};
}

bool WeaselTSF::_HasActiveTabJumpSession() const {
  return !_tab_jump_targets.empty() &&
         _tab_jump_index < _tab_jump_targets.size();
}

void WeaselTSF::_InitTabJumpSession(com_ptr<ITfContext> context,
                                    std::vector<TabJumpTarget>&& targets) {
  _tab_jump_context = context;
  _tab_jump_targets = std::move(targets);
  _tab_jump_index = 0;
  _tab_jump_caret_offset = 0;
  _tab_jump_focus = GetFocus();
}

bool WeaselTSF::_StepTabJump(ITfContext* pContext, bool backward, bool toEnd) {
  if (!_HasActiveTabJumpSession())
    return false;
  if (pContext != _tab_jump_context || _tab_jump_focus != GetFocus()) {
    _ClearTabJumpSession();
    return false;
  }
  if (backward) {
    if (_tab_jump_index == 0)
      return true;
    const size_t target_index = _tab_jump_index - 1;
    const auto target = _tab_jump_targets[target_index];
    bool ok = false;
    if (_tab_jump_keyboard) {
      const LONG movement = target.steps - _tab_jump_caret_offset;
      ok = _SendCursorKeys(movement < 0 ? VK_RIGHT : VK_LEFT,
                           movement < 0 ? -movement : movement,
                           target.selection_length, _tab_jump_selection);
    } else if (target.range) {
      HRESULT hr = E_FAIL;
      CTabJumpEditSession* session = new CTabJumpEditSession(
          this, _tab_jump_context, target.range, target.is_selection);
      const HRESULT request = _tab_jump_context->RequestEditSession(
          _tfClientId, session, TF_ES_SYNC | TF_ES_READWRITE, &hr);
      session->Release();
      ok = SUCCEEDED(request) && hr == S_OK;
    }
    if (!ok) {
      _ClearTabJumpSession();
      return false;
    }
    _tab_jump_selection = target.is_selection;
    _tab_jump_caret_offset = 0;
    _tab_jump_index = target_index;
    return true;
  } else {
    if (_tab_jump_index + 1 >= _tab_jump_targets.size()) {
      _ClearTabJumpSession();
      return false;
    }
    const size_t target_index =
        toEnd ? _tab_jump_targets.size() - 1 : _tab_jump_index + 1;
    const auto target = _tab_jump_targets[target_index];
    bool ok = false;
    if (_tab_jump_keyboard) {
      LONG movement = _tab_jump_caret_offset;
      for (size_t i = _tab_jump_index + 1; i <= target_index; ++i) {
        movement += _tab_jump_targets[i].steps;
        if (i < target_index)
          movement += _tab_jump_targets[i].selection_length;
        if (movement > 4096 || movement < -4096) {
          _ClearTabJumpSession();
          return false;
        }
      }
      ok = _SendCursorKeys(movement < 0 ? VK_LEFT : VK_RIGHT,
                           movement < 0 ? -movement : movement,
                           target.selection_length, _tab_jump_selection);
    } else if (target.range) {
      HRESULT hr = E_FAIL;
      CTabJumpEditSession* session = new CTabJumpEditSession(
          this, _tab_jump_context, target.range, target.is_selection);
      const HRESULT request = _tab_jump_context->RequestEditSession(
          _tfClientId, session, TF_ES_SYNC | TF_ES_READWRITE, &hr);
      session->Release();
      ok = SUCCEEDED(request) && hr == S_OK;
    }
    if (!ok) {
      _ClearTabJumpSession();
      return false;
    }
    _tab_jump_selection = target.is_selection;
    _tab_jump_caret_offset = 0;
    _tab_jump_index = target_index;
    if (_tab_jump_index + 1 >= _tab_jump_targets.size())
      _ClearTabJumpSession();
    return true;
  }
}

STDMETHODIMP CInsertTextEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfRange> pRange;
  TF_SELECTION tfSelection;
  HRESULT hRet = S_OK;
  HRESULT write_hr = E_FAIL;

  if (_pComposition == nullptr) {
    TraceWordCommit(
        L"[weasel_tsf_commit_probe] event=write stage=composition "
        L"result=missing\n");
    return E_FAIL;
  }
  write_hr = _pComposition->GetRange(&pRange);
  if (FAILED(write_hr)) {
    wchar_t message[192];
    swprintf_s(message,
               L"[weasel_tsf_commit_probe] event=write stage=get_range "
               L"hr=0x%08X\n",
               static_cast<unsigned int>(write_hr));
    TraceWordCommit(message);
    return E_FAIL;
  }

  std::wstring text_to_insert;
  std::vector<CursorSlot> slots;
  const size_t placeholder_count =
      ParseCursorPlaceholders(_text, &text_to_insert, &slots);

  write_hr = pRange->SetText(ec, 0, text_to_insert.c_str(),
                             static_cast<LONG>(text_to_insert.length()));
  if (FAILED(write_hr)) {
    wchar_t message[192];
    swprintf_s(message,
               L"[weasel_tsf_commit_probe] event=write stage=set_text "
               L"hr=0x%08X\n",
               static_cast<unsigned int>(write_hr));
    TraceWordCommit(message);
    return E_FAIL;
  }
  {
    wchar_t message[192];
    swprintf_s(message,
               L"[weasel_tsf_commit_probe] event=write stage=set_text "
               L"result=ok utf16_units=%zu\n",
               text_to_insert.size());
    TraceWordCommit(message);
  }

  if (_pTextService->_punct_jump_waiting_commit &&
      _pTextService->_punct_jump_context == _pContext)
    _pTextService->_punct_jump_commit_written = true;
  _pTextService->_RememberLastCommit(ec, _pContext, pRange, text_to_insert);

  if (placeholder_count > 0 && !slots.empty()) {
    _pTextService->_ClearTabJumpSession();
    TF_STATUS status = {};
    const bool keyboard = FAILED(_pContext->GetStatus(&status)) ||
                          (status.dwStaticFlags & TF_SS_TRANSITORY);
    auto makeRange = [&](const CursorSlot& slot) -> com_ptr<ITfRange> {
      com_ptr<ITfRange> range;
      LONG shifted = 0;
      if (FAILED(pRange->Clone(&range)) || !range ||
          FAILED(range->Collapse(ec, TF_ANCHOR_START)) ||
          FAILED(range->ShiftEnd(ec, slot.offset, &shifted, NULL)) ||
          shifted != slot.offset || FAILED(range->Collapse(ec, TF_ANCHOR_END)))
        return nullptr;
      if (slot.length > 0 &&
          (FAILED(range->ShiftEnd(ec, slot.length, &shifted, NULL)) ||
           shifted != slot.length))
        return nullptr;
      if (FAILED(range->SetGravity(
              ec, TF_GRAVITY_FORWARD,
              slot.length > 0 ? TF_GRAVITY_BACKWARD : TF_GRAVITY_FORWARD)))
        return nullptr;
      return range;
    };
    if (_pTextService->_IsCursorTabJumpEnabled()) {
      std::vector<WeaselTSF::TabJumpTarget> targets;
      for (size_t i = 0; i <= slots.size(); ++i) {
        const CursorSlot slot =
            i < slots.size()
                ? slots[i]
                : CursorSlot{static_cast<LONG>(text_to_insert.size()), 0};
        const size_t previousEnd =
            i > 0 ? (slots[i - 1].offset + slots[i - 1].length) : 0;
        targets.push_back(
            {keyboard ? nullptr : makeRange(slot), slot.length > 0,
             CursorKeyCount(text_to_insert, previousEnd, slot.offset),
             slot.length});
      }
      _pTextService->_InitTabJumpSession(_pContext, std::move(targets));
      _pTextService->_tab_jump_keyboard = keyboard;
      _pTextService->_tab_jump_selection = slots[0].length > 0;
    }
    // Save placement until EndComposition; hosts may force the caret to the
    // commit end regardless of selections made while the text is composing.
    _pTextService->_cursor_placement_context = _pContext;
    _pTextService->_cursor_placement_focus = GetFocus();
    _pTextService->_cursor_placement_keyboard = keyboard;
    _pTextService->_cursor_placement = {
        keyboard ? nullptr : makeRange(slots[0]), slots[0].length > 0,
        CursorKeyCount(text_to_insert, slots[0].offset, text_to_insert.size()),
        slots[0].length};
  }
  // Ordinary commits inside a field must keep the remaining jump targets.
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
  HRESULT hr = E_FAIL;
  HRESULT request_hr = E_FAIL;

  if ((pEditSession = new CInsertTextEditSession(this, pContext, _pComposition,
                                                 text)) != NULL) {
    request_hr = pContext->RequestEditSession(
        _tfClientId, pEditSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }
  wchar_t message[256];
  swprintf_s(message,
             L"[weasel_tsf_commit_probe] event=insert_request pid=%lu "
             L"tid=%lu utf16_units=%zu composition=%d request_hr=0x%08X "
             L"session_hr=0x%08X\n",
             GetCurrentProcessId(), GetCurrentThreadId(), text.size(),
             _pComposition ? 1 : 0, static_cast<unsigned int>(request_hr),
             static_cast<unsigned int>(hr));
  TraceWordCommit(message);

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

static bool IsPunctuationChar(wchar_t ch) {
  // ASCII punctuation
  if ((ch >= 0x20 && ch <= 0x2F) || (ch >= 0x3A && ch <= 0x40) ||
      (ch >= 0x5B && ch <= 0x60) || (ch >= 0x7B && ch <= 0x7E))
    return true;
  // General punctuation (quotes, dashes, ellipsis, bullet, etc.)
  if (ch >= 0x2000 && ch <= 0x206F)
    return true;
  // CJK Symbols and Punctuation
  if (ch >= 0x3000 && ch <= 0x303F)
    return true;
  // Fullwidth ASCII punctuation variants
  if (ch >= 0xFF01 && ch <= 0xFF65)
    return true;
  return false;
}

static bool IsPunctuationText(const std::wstring& str) {
  if (str.empty())
    return false;
  for (wchar_t ch : str) {
    if (!IsPunctuationChar(ch))
      return false;
  }
  return true;
}

void WeaselTSF::_SyncLastCommitFields() {
  if (_commit_history.empty()) {
    _last_commit_range = nullptr;
    _last_commit_end = nullptr;
    _last_commit_text.clear();
  } else {
    _last_commit_range = _commit_history.back().range;
    _last_commit_end = _commit_history.back().end;
    _last_commit_text = _commit_history.back().text;
  }
  if (_backspace_history.empty()) {
    _backspace_text.clear();
  } else {
    _backspace_text = _backspace_history.back();
  }
  // A pop may expose an older commit whose temporary range already vanished.
  if (!_commit_history.empty() && !_last_commit_range && _backspace_transitory)
    _backspace_range_lost = true;
}

void WeaselTSF::_ForgetLastCommit() {
  _commit_history.clear();
  _backspace_history.clear();
  _SyncLastCommitFields();
  _last_commit_context = nullptr;
  _InvalidateBackspace();
  _backspace_context = nullptr;
  _backspace_text.clear();
  _backspace_range_lost = false;
}

void WeaselTSF::_RememberLastCommit(TfEditCookie ec,
                                    com_ptr<ITfContext> context,
                                    com_ptr<ITfRange> range,
                                    const std::wstring& text) {
  if (text.empty() || !range) {
    _ForgetLastCommit();
    OutputDebugStringW(L"Weasel reopen: cannot record committed range\n");
    return;
  }
  const bool temporaryChain = _backspace_range_lost && _backspace_chain_valid &&
                              context == _backspace_context &&
                              GetFocus() == _backspace_focus &&
                              !_commit_history.empty();
  if (context != _last_commit_context ||
      (_backspace_range_lost && !temporaryChain)) {
    if (!temporaryChain)
      _ForgetLastCommit();
    _last_commit_context = context;
  }

  com_ptr<ITfRange> saved, end;
  if (FAILED(range->Clone(&saved)) || FAILED(range->Clone(&end)) ||
      FAILED(end->Collapse(ec, TF_ANCHOR_END))) {
    _ForgetLastCommit();
    OutputDebugStringW(L"Weasel reopen: cannot record committed range\n");
    return;
  }
  // Keep a separate end anchor: some hosts collapse the original range when
  // composition ends. Never reconstruct from an arbitrary current caret.
  saved->SetGravity(ec, TF_GRAVITY_FORWARD, TF_GRAVITY_BACKWARD);
  end->SetGravity(ec, TF_GRAVITY_BACKWARD, TF_GRAVITY_BACKWARD);

  if (temporaryChain && IsPunctuationText(text)) {
    // Earlier ranges belong to an emptied transitory document. Preserve the
    // actual deletion unit without pretending that punctuation's range covers
    // the preceding word in that temporary document.
    _commit_history.back().text += text;
    _backspace_history.back() += text;
    _SyncLastCommitFields();
    _ArmBackspace(context, _backspace_text, true);
    _backspace_range_lost = true;
    return;
  }

  // Check if this is an adjacent trailing punctuation commit that can merge
  // with the top entry
  if (!_commit_history.empty() && IsPunctuationText(text)) {
    LONG comparison = 0;
    if (SUCCEEDED(range->CompareStart(ec, _commit_history.back().end,
                                      TF_ANCHOR_START, &comparison)) &&
        comparison == 0) {
      if (SUCCEEDED(_commit_history.back().range->ShiftEndToRange(
              ec, range, TF_ANCHOR_END))) {
        _commit_history.back().end = end;
        _commit_history.back().text += text;
        if (!_backspace_history.empty()) {
          _backspace_history.back() += text;
        }
        _SyncLastCommitFields();
        TF_STATUS status = {};
        const bool transitory = SUCCEEDED(context->GetStatus(&status)) &&
                                (status.dwStaticFlags & TF_SS_TRANSITORY);
        _ArmBackspace(context, _last_commit_text, transitory);
        OutputDebugStringW(
            L"Weasel reopen: merged trailing punctuation into commit "
            L"history\n");
        return;
      }
    }
  }

  // If there are existing entries, verify if the new commit is contiguous with
  // the previous end
  if (!_commit_history.empty() && !temporaryChain) {
    LONG comparison = 0;
    if (FAILED(range->CompareStart(ec, _commit_history.back().end,
                                   TF_ANCHOR_START, &comparison)) ||
        comparison != 0) {
      // Caret moved or text inserted non-contiguously; discard old history
      _commit_history.clear();
      _backspace_history.clear();
    }
  }

  if (_commit_history.size() >= kMaxCommitHistory) {
    _commit_history.erase(_commit_history.begin());
  }
  _commit_history.push_back({saved, end, text});

  if (_backspace_history.size() >= kMaxCommitHistory) {
    _backspace_history.erase(_backspace_history.begin());
  }
  _backspace_history.push_back(text);

  _SyncLastCommitFields();

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
    if (!_commit_history.empty()) {
      _commit_history.back().range = recovered;
    }
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
  for (auto& entry : _commit_history) {
    entry.range = nullptr;
    entry.end = nullptr;
  }
  _last_commit_context = nullptr;
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
  OutputDebugStringW(L"Weasel reopen: using backspace compatibility\n");
  _RequestInputAction(context, count);
  return true;
}

void WeaselTSF::_CompleteBackspace() {
  if (!_commit_history.empty())
    _commit_history.pop_back();
  if (!_backspace_history.empty())
    _backspace_history.pop_back();
  _SyncLastCommitFields();
  if (_commit_history.empty()) {
    _last_commit_context = nullptr;
    _InvalidateBackspace();
    _backspace_context = nullptr;
    _backspace_range_lost = false;
  }
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
  if (!_commit_history.empty())
    _commit_history.pop_back();
  if (!_backspace_history.empty())
    _backspace_history.pop_back();
  _SyncLastCommitFields();
  if (_commit_history.empty()) {
    _last_commit_context = nullptr;
    _InvalidateBackspace();
    _backspace_context = nullptr;
    _backspace_range_lost = false;
  }
  _reopen_popped = true;
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
    if (m_client.ProcessKeyEvent(weasel::KeyEvent(ibus::F33, 0)))
      return DoEditSession(ec);
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
  _reopen_popped = false;
}

void WeaselTSF::_RollbackReopen(TfEditCookie ec, com_ptr<ITfContext> context) {
  if (_reopen_range && !_reopen_text.empty()) {
    if (SUCCEEDED(
            _reopen_range->SetText(ec, 0, _reopen_text.c_str(),
                                   static_cast<LONG>(_reopen_text.size())))) {
      if (_reopen_popped)
        _RememberLastCommit(ec, context, _reopen_range, _reopen_text);
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

class CUpdateCompositionEditSession : public CEditSession {
 public:
  CUpdateCompositionEditSession(com_ptr<WeaselTSF> service,
                                com_ptr<ITfContext> context,
                                std::wstring response)
      : CEditSession(service, context), response_(std::move(response)) {}

  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    return _pTextService->_DoEditSession(ec, _pContext, &response_);
  }

 private:
  std::wstring response_;
};

void WeaselTSF::_UpdateComposition(com_ptr<ITfContext> pContext) {
  HRESULT session_hr = E_FAIL;

  std::wstring response;
  const bool captured = m_client.GetResponseSnapshot(response);
  const size_t response_units = response.size();
  com_ptr<CUpdateCompositionEditSession> session;
  session.Attach(
      new CUpdateCompositionEditSession(this, pContext, std::move(response)));
  _pEditSessionContext = pContext;

  const HRESULT request_hr = _pEditSessionContext->RequestEditSession(
      _tfClientId, session, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &session_hr);
  _async_edit = !!(session_hr == TF_S_ASYNC);
  wchar_t message[256];
  swprintf_s(message,
             L"[word_commit_edit_probe] event=edit_request "
             L"request_hr=0x%08lx session_hr=0x%08lx async=%d "
             L"snapshot=%d snapshot_units=%zu",
             static_cast<unsigned long>(request_hr),
             static_cast<unsigned long>(session_hr), _async_edit ? 1 : 0,
             captured ? 1 : 0, response_units);
  weasel::AppendWordCommitTrace(message);
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

// External templates enter the same CInsertTextEditSession path as Rime
// commits. The sender never owns an edit cookie or a document range.
class CExternalTemplateEditSession : public CEditSession {
 public:
  CExternalTemplateEditSession(com_ptr<WeaselTSF> service,
                               com_ptr<ITfContext> context,
                               HWND target,
                               UINT id,
                               std::wstring text)
      : CEditSession(service, context),
        _target(target),
        _id(id),
        _text(std::move(text)) {}
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    return _pTextService->_ApplyExternalTemplate(ec, _pContext, _target, _id,
                                                 _text);
  }

 private:
  HWND _target;
  UINT _id;
  std::wstring _text;
};

void WeaselTSF::_InitExternalTemplateWindow() {
  if (_external_template_window || !_pThreadMgr)
    return;
  WNDCLASSW cls = {};
  cls.hInstance = g_hInst;
  cls.lpszClassName = weasel::external_template::kWindowClass;
  cls.lpfnWndProc = _ExternalTemplateWindowProc;
  if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return;
  _external_template_window =
      CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                      nullptr, g_hInst, nullptr);
  if (_external_template_window)
    SetWindowLongPtrW(_external_template_window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));
}

void WeaselTSF::_UninitExternalTemplateWindow() {
  _external_template_status = weasel::external_template::Failed;
  if (_external_template_window) {
    SetWindowLongPtrW(_external_template_window, GWLP_USERDATA, 0);
    DestroyWindow(_external_template_window);
    _external_template_window = nullptr;
    UnregisterClassW(weasel::external_template::kWindowClass, g_hInst);
  }
}

LRESULT CALLBACK WeaselTSF::_ExternalTemplateWindowProc(HWND window,
                                                        UINT message,
                                                        WPARAM wParam,
                                                        LPARAM lParam) {
  auto* service =
      reinterpret_cast<WeaselTSF*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (service) {
    if (message == WM_COPYDATA)
      return service->_ReceiveExternalTemplate(
          reinterpret_cast<const COPYDATASTRUCT*>(lParam));
    if (message == weasel::external_template::kStatusMessage)
      return wParam == service->_external_template_id
                 ? service->_external_template_status
                 : weasel::external_template::Rejected;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

bool WeaselTSF::_ExternalTemplateContextMatches(ITfContext* context,
                                                HWND target) {
  if (!_external_template_window || !_pThreadMgr || !target || !context ||
      _IsKeyboardDisabled() || (_isToOpenClose && !_IsKeyboardOpen()) ||
      _IsComposing() || _status.composing || _cursor_input_active ||
      _punct_jump_context || _undo_input_active || _undo_context ||
      _backspace_context || !_reopen_text.empty())
    return false;
  const HWND root = GetAncestor(target, GA_ROOT);
  const HWND focus = GetFocus();
  if (!root || GetForegroundWindow() != root || !focus ||
      GetAncestor(focus, GA_ROOT) != root)
    return false;
  com_ptr<ITfDocumentMgr> document;
  com_ptr<ITfContext> current;
  return SUCCEEDED(_pThreadMgr->GetFocus(&document)) && document &&
         SUCCEEDED(document->GetTop(&current)) && current == context;
}

LRESULT WeaselTSF::_ReceiveExternalTemplate(const COPYDATASTRUCT* data) {
  using namespace weasel::external_template;
  Request request;
  if (!data || data->dwData != kCopyDataTag ||
      !Decode(data->lpData, data->cbData, request))
    return Rejected;
  if (request.id == _external_template_id)
    return _external_template_status;
  if (_external_template_status == Pending)
    return Rejected;
  const HWND target =
      reinterpret_cast<HWND>(static_cast<UINT_PTR>(request.target));
  if (static_cast<uint64_t>(reinterpret_cast<UINT_PTR>(target)) !=
      request.target)
    return Rejected;
  com_ptr<ITfDocumentMgr> document;
  com_ptr<ITfContext> context;
  if (!_pThreadMgr || FAILED(_pThreadMgr->GetFocus(&document)) || !document ||
      FAILED(document->GetTop(&context)) ||
      !_ExternalTemplateContextMatches(context, target) ||
      !_EnsureServerConnected())
    return Rejected;
  // Refresh policy even when the panel was launched without a preceding
  // Rime key. The response is consumed before any asynchronous edit request.
  m_client.ProcessKeyEvent(weasel::KeyEvent(0));
  weasel::Config config;
  std::wstring commit;
  weasel::ResponseParser parser(&commit, nullptr, &_status, &config,
                                &_cand->style());
  if (!m_client.GetResponseData(std::ref(parser)) || !commit.empty() ||
      !_ExternalTemplateContextMatches(context, target))
    return Rejected;
  _cursor_tab_jump = config.cursor_tab_jump;
  _cursor_punct_jump_keys = config.cursor_punct_jump_keys;
  _cursor_punct_jump_any_field = config.cursor_punct_jump_any_field;
  _external_template_id = request.id;
  _external_template_status = Pending;
  const std::wstring text(request.text.begin(), request.text.end());
  com_ptr<CExternalTemplateEditSession> session;
  session.Attach(new CExternalTemplateEditSession(this, context, target,
                                                  request.id, text));
  HRESULT result = E_FAIL;
  const HRESULT hr = context->RequestEditSession(
      _tfClientId, session, TF_ES_ASYNC | TF_ES_READWRITE, &result);
  if (FAILED(hr) || (FAILED(result) && result != TF_S_ASYNC))
    _external_template_status = Failed;
  return _external_template_status;
}

HRESULT WeaselTSF::_ApplyExternalTemplate(TfEditCookie ec,
                                          ITfContext* context,
                                          HWND target,
                                          UINT id,
                                          const std::wstring& text) {
  using namespace weasel::external_template;
  if (id != _external_template_id || _external_template_status != Pending)
    return S_FALSE;
  if (!_ExternalTemplateContextMatches(context, target)) {
    _external_template_status = Failed;
    return E_FAIL;
  }
  // Perform start/write/end under one actual write lock. In particular Word
  // may defer the request; never retain the sender's focus assumptions.
  CStartCompositionEditSession start(this, context, FALSE);
  HRESULT hr = start.DoEditSession(ec);
  if (SUCCEEDED(hr) && _pComposition) {
    _pEditSessionContext = context;
    CInsertTextEditSession insert(this, context, _pComposition, text);
    hr = insert.DoEditSession(ec);
    HRESULT end_hr = E_FAIL;
    CEndCompositionEditSession end(this, context, _pComposition, FALSE, 0,
                                   &end_hr);
    end.DoEditSession(ec);
    if (SUCCEEDED(hr))
      hr = end_hr;
  } else {
    hr = E_FAIL;
  }
  if (SUCCEEDED(hr))
    _committed = TRUE;
  _external_template_status = SUCCEEDED(hr) ? Applied : Failed;
  return hr;
}
