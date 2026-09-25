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
    std::wstring diagnostic = L"context_disabled";
    if (llm_trigger.context_enabled) {
      const LONG context_chars =
          std::clamp(static_cast<LONG>(llm_trigger.context_chars), 0L, 2000L);
      const LONG search_chars = std::clamp(
          static_cast<LONG>(llm_trigger.boundary_search_chars), 0L, 500L);
      std::wstring raw_context = _ReadTextBeforeCaret(
          ec, _pEditSessionContext, context_chars + search_chars, diagnostic);
      if (static_cast<LONG>(raw_context.size()) > context_chars) {
        const size_t cut = raw_context.size() - context_chars;
        const size_t limit = (cut + search_chars < raw_context.size())
                                 ? cut + search_chars
                                 : raw_context.size();
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
    m_client.SubmitLlmContext(llm_trigger.request_id, prefix_context,
                              diagnostic);
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

  if (!context->cinfo.candies.empty() &&
      context->cinfo.candies[0].str == L"AI分析中...") {
    if (!_llm_timer_id)
      _StartLlmPolling(_pEditSessionContext);
  } else if (_llm_timer_id) {
    _StopLlmPolling();
  }

  return TRUE;
}

std::wstring WeaselTSF::_ReadTextBeforeCaret(TfEditCookie ec,
                                             ITfContext* pContext,
                                             LONG maxChars,
                                             std::wstring& diagnostic) {
  diagnostic.clear();
  const auto record = [&diagnostic](const wchar_t* stage, HRESULT hr) {
    diagnostic += std::wstring(stage) + L"=" +
                  std::to_wstring(static_cast<unsigned long>(hr)) + L";";
    return SUCCEEDED(hr);
  };
  if (!pContext || maxChars <= 0) {
    diagnostic = !pContext ? L"null_context" : L"zero_limit";
    return {};
  }
  com_ptr<ITfRange> range;
  // Read before the composition, excluding inline preedit text. Selection may
  // point inside that temporary text instead of at the committed-text boundary.
  if (_pComposition) {
    ITfRange* composition_range = nullptr;
    HRESULT hr = _pComposition->GetRange(&composition_range);
    record(L"CompositionRange", hr);
    if (SUCCEEDED(hr) && composition_range)
      range.Attach(composition_range);
  }
  if (!range) {
    TF_SELECTION selection = {};
    ULONG fetched = 0;
    HRESULT hr = pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection,
                                        &fetched);
    record(L"GetSelection", hr);
    diagnostic += L"fetched=" + std::to_wstring(fetched) + L";";
    if (selection.range)
      range.Attach(selection.range);
    if (FAILED(hr) || !fetched || !range)
      return {};
  }
  if (!record(L"Collapse", range->Collapse(ec, TF_ANCHOR_START)))
    return {};
  LONG shifted = 0;
  if (!record(L"ShiftStart",
              range->ShiftStart(ec, -maxChars, &shifted, nullptr)))
    return {};
  diagnostic += L"shifted=" + std::to_wstring(shifted) + L";";
  std::vector<WCHAR> buffer(static_cast<size_t>(maxChars));
  ULONG chars_read = 0;
  if (!record(L"GetText",
              range->GetText(ec, 0, buffer.data(),
                             static_cast<ULONG>(buffer.size()), &chars_read)))
    return {};
  diagnostic += L"read=" + std::to_wstring(chars_read) + L";";
  return std::wstring(buffer.data(), chars_read);
}
