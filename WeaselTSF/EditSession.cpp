#include "stdafx.h"
#include "WeaselTSF.h"
#include "CandidateList.h"
#include "ResponseParser.h"
#include <algorithm>
#include <vector>

STDMETHODIMP WeaselTSF::DoEditSession(TfEditCookie ec) {
  // get commit string from server
  std::wstring commit;
  weasel::Config config;
  weasel::LlmTriggerInfo llm_trigger;
  auto context = std::make_shared<weasel::Context>();
  weasel::ResponseParser parser(&commit, context.get(), &_status, &config,
                                &_cand->style(), &llm_trigger);

  bool ok = m_client.GetResponseData(std::ref(parser));

  if (ok && !llm_trigger.request_id.empty()) {
    std::wstring prefix_context;
    if (llm_trigger.context_enabled) {
      const LONG context_chars =
          max(0L, min(2000L, static_cast<LONG>(llm_trigger.context_chars)));
      const LONG search_chars = max(
          0L, min(500L, static_cast<LONG>(llm_trigger.boundary_search_chars)));
      std::wstring raw_context = _ReadTextBeforeCaret(
          ec, _pEditSessionContext, context_chars + search_chars);
      if (static_cast<LONG>(raw_context.size()) > context_chars) {
        const size_t cut = raw_context.size() - context_chars;
        const size_t limit = min(raw_context.size(), cut + search_chars);
        const wchar_t* boundaries[] = {L"\r\n\r\n", L"\n\n", L"\r\n", L"\n",
                                       L"。",       L"！",   L"？",   L"；",
                                       L"：",       L"，"};
        const size_t lengths[] = {4, 2, 2, 1, 1, 1, 1, 1, 1, 1};
        size_t start = std::wstring::npos;
        size_t skip = 0;
        for (size_t kind = 0; kind < _countof(boundaries); ++kind) {
          const size_t found = raw_context.find(boundaries[kind], cut);
          if (found != std::wstring::npos && found <= limit &&
              start == std::wstring::npos) {
            start = found;
            skip = lengths[kind];
          }
        }
        raw_context.erase(0, start != std::wstring::npos ? start + skip : cut);
      }
      prefix_context = std::move(raw_context);
    }
    m_client.SubmitLlmContext(llm_trigger.request_id, prefix_context);
    _StartLlmPolling(_pEditSessionContext);
  }

  _UpdateLanguageBar(_status);

  bool compositionEnded = false;
  if (ok) {
    compositionEnded = false;
    if (!commit.empty()) {
      // For auto-selecting, commit and preedit can both exist.
      // Commit the old TSF composition. If Rime immediately has a new
      // preedit (top-word input), _EndComposition() drops the local pointer
      // synchronously, so the following state check starts a new TSF
      // composition instead of observing the old one.
      if (!_IsComposing()) {
        _StartComposition(_pEditSessionContext,
                          _fCUASWorkaroundEnabled && !config.inline_preedit);
      }
      _InsertText(_pEditSessionContext, commit);
      // Keep the candidate UI alive while the replacement composition is
      // being created; otherwise the key-down path destroys the old window
      // and the new one cannot be positioned until key-up.
      _EndComposition(_pEditSessionContext, false, !_status.composing);
      compositionEnded = true;
      _committed = TRUE;
    } else {
      _committed = FALSE;
    }
    if (_status.composing && (compositionEnded || !_IsComposing())) {
      _StartComposition(_pEditSessionContext,
                        _fCUASWorkaroundEnabled && !config.inline_preedit);
    } else if (!_status.composing && _IsComposing()) {
      _EndComposition(_pEditSessionContext, true);
    }
    if (_IsComposing() && config.inline_preedit) {
      _ShowInlinePreedit(_pEditSessionContext, context);
    }
  }

  if (ok && !compositionEnded)
    _UpdateCompositionWindow(_pEditSessionContext);
  // Keep the existing candidate window alive during top-word input, but
  // publish the new candidates in this key-down edit session. Positioning is
  // still updated by the queued read session after the new composition is
  // created.
  _UpdateUI(*context, _status);

  if (_llm_timer_id) {
    if (context->cinfo.candies.empty() ||
        context->cinfo.candies[0].str != L"AI分析中...") {
      _StopLlmPolling();
    }
  }

  return TRUE;
}

std::wstring WeaselTSF::_ReadTextBeforeCaret(TfEditCookie ec,
                                             ITfContext* pContext,
                                             LONG maxChars) {
  if (!pContext || maxChars <= 0)
    return std::wstring();

  TF_SELECTION selection = {};
  ULONG fetched = 0;
  if (FAILED(pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection,
                                    &fetched)) ||
      fetched == 0 || !selection.range) {
    return std::wstring();
  }

  com_ptr<ITfRange> range;
  ITfRange* cloned_range = nullptr;
  const HRESULT clone_result = selection.range->Clone(&cloned_range);
  selection.range->Release();
  if (FAILED(clone_result) || !cloned_range)
    return std::wstring();
  range.Attach(cloned_range);
  const TfAnchor anchor =
      selection.style.ase == TF_AE_START ? TF_ANCHOR_START : TF_ANCHOR_END;
  if (FAILED(range->Collapse(ec, anchor)))
    return std::wstring();

  LONG shifted = 0;
  if (FAILED(range->ShiftStart(ec, -maxChars, &shifted, nullptr)))
    return std::wstring();

  std::vector<WCHAR> buffer(static_cast<size_t>(maxChars) + 1, L'\0');
  ULONG charsRead = 0;
  if (FAILED(range->GetText(ec, 0, buffer.data(),
                            static_cast<ULONG>(buffer.size()), &charsRead))) {
    return std::wstring();
  }
  return std::wstring(buffer.data(), charsRead);
}
