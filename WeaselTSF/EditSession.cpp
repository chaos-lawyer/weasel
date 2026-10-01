#include "stdafx.h"
#include "WeaselTSF.h"
#include "CandidateList.h"
#include "ResponseParser.h"
#include "EditSession.h"

STDMETHODIMP WeaselTSF::DoEditSession(TfEditCookie ec) {
  // get commit string from server
  std::wstring commit;
  weasel::Config config;
  auto context = std::make_shared<weasel::Context>();
  weasel::ResponseParser parser(&commit, context.get(), &_status, &config,
                                &_cand->style());

  bool ok = m_client.GetResponseData(std::ref(parser));

  _UpdateLanguageBar(_status);

  bool compositionEnded = false;
  if (ok) {
    compositionEnded = false;
    const std::wstring reopenPrefix = L"reopen_last_commit:";
    if (context->undo_action.compare(0, reopenPrefix.size(), reopenPrefix) ==
        0) {
      _StopCloudPolling();
      if (_ReopenLastCommit(ec, _pEditSessionContext,
                            context->undo_action.substr(reopenPrefix.size()))) {
        // Resume configuration and consume its response under the same lock.
        // This avoids another queued key response overwriting the callback.
        if (m_client.ProcessKeyEvent(weasel::KeyEvent(ibus::F35, 0))) {
          const HRESULT result = DoEditSession(ec);
          _FinishReopen();
          return result;
        }
        _RollbackReopen(ec, _pEditSessionContext);
        _EndComposition(_pEditSessionContext, false);
      } else {
        // An asynchronous commit may still be waiting for its end session.
        // Refusing reopen must not clear that committed composition.
        bool clearTrigger = true;
        if (_IsComposing() && _last_commit_range) {
          com_ptr<ITfRange> range;
          LONG start = 0, end = 0;
          if (SUCCEEDED(_pComposition->GetRange(&range)) &&
              SUCCEEDED(range->CompareStart(ec, _last_commit_range,
                                            TF_ANCHOR_START, &start)) &&
              SUCCEEDED(range->CompareEnd(ec, _last_commit_range, TF_ANCHOR_END,
                                          &end)) &&
              start == 0 && end == 0)
            clearTrigger = false;
        }
        _EndComposition(_pEditSessionContext, clearTrigger);
      }
      _ForgetLastCommit();
      _UpdateUI(*context, _status);
      return TRUE;
    }
    if (context->undo_action == L"ctrl_z") {
      // The configuration resumes through F35 after the tagged undo input.
      // Never start a replacement composition in the undo request session.
      _StopCloudPolling();
      _RequestUndo(_pEditSessionContext);
      _UpdateUI(*context, _status);
      return TRUE;
    }
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
      const bool reopening = !_reopen_text.empty();
      if (reopening)
        _RollbackReopen(ec, _pEditSessionContext);
      _EndComposition(_pEditSessionContext, !reopening);
    }
    if (_IsComposing() && config.inline_preedit) {
      _ShowInlinePreedit(_pEditSessionContext, context);
    }
  }

  if (!ok && !_reopen_text.empty()) {
    _RollbackReopen(ec, _pEditSessionContext);
    _EndComposition(_pEditSessionContext, false);
    OutputDebugStringW(L"Weasel reopen: callback response unavailable\n");
  }
  if (ok && !compositionEnded)
    _UpdateCompositionWindow(_pEditSessionContext);
  // Keep the existing candidate window alive during top-word input, but
  // publish the new candidates in this key-down edit session. Positioning is
  // still updated by the queued read session after the new composition is
  // created.
  _UpdateUI(*context, _status);
  if (ok && context->cloud_pending && _status.composing)
    _StartCloudPolling(_pEditSessionContext);
  else
    _StopCloudPolling();

  return TRUE;
}

namespace {
class CloudPollingEditSession : public CEditSession {
 public:
  CloudPollingEditSession(com_ptr<WeaselTSF> service,
                          com_ptr<ITfContext> context,
                          UINT_PTR timer_id)
      : CEditSession(service, context), timer_id_(timer_id) {}
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    return _pTextService->_PollCloudCandidates(ec, _pContext, timer_id_);
  }

 private:
  UINT_PTR timer_id_;
};
}  // namespace

void WeaselTSF::_OnCloudTimer(UINT_PTR timer_id) {
  if (timer_id != _cloud_timer_id)
    return;
  if (!_status.composing || !_cloud_context || _cloud_focus != GetFocus() ||
      GetTickCount64() - _cloud_poll_started > 5000) {
    _StopCloudPolling();
    return;
  }
  if (_cloud_poll_queued)
    return;
  _cloud_poll_queued = true;
  com_ptr<CloudPollingEditSession> session;
  session.Attach(new CloudPollingEditSession(this, _cloud_context, timer_id));
  HRESULT result = E_FAIL;
  const HRESULT hr = _cloud_context->RequestEditSession(
      _tfClientId, session, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &result);
  if (FAILED(hr) || FAILED(result))
    _StopCloudPolling();
}

HRESULT WeaselTSF::_PollCloudCandidates(TfEditCookie ec,
                                        com_ptr<ITfContext> context,
                                        UINT_PTR timer_id) {
  if (timer_id != _cloud_timer_id || context != _cloud_context)
    return S_FALSE;
  _cloud_poll_queued = false;
  if (!_status.composing || _cloud_focus != GetFocus() ||
      context != _pEditSessionContext) {
    _StopCloudPolling();
    return S_FALSE;
  }
  // Request and consume the response inside the same edit session, so a queued
  // callback cannot overwrite another key's cached IPC response.
  if (!m_client.ProcessKeyEvent(weasel::KeyEvent(ibus::F34, 0))) {
    _StopCloudPolling();
    return S_FALSE;
  }
  return DoEditSession(ec);
}
