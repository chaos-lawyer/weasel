#ifndef WEASEL_CLOUD_STANDALONE
#include "stdafx.h"
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <CloudCandidate.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace cloud {
namespace {
// Callback owns no Rime state. Request closure is drained before destroying it.
struct RequestState {
  std::mutex mutex;
  std::condition_variable cv;
  DWORD status = 0, error = 0, bytes = 0;
  bool closed = false;
  void Reset() {
    std::lock_guard<std::mutex> lock(mutex);
    status = 0;
  }
  bool Wait(DWORD expected, Clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_until(lock, deadline, [&] {
      return status == expected || error;
    }) && !error;
  }
};
void CALLBACK
OnStatus(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD) {
  if (!context)
    return;
  auto& state = *reinterpret_cast<RequestState*>(context);
  std::lock_guard<std::mutex> lock(state.mutex);
  if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
    state.closed = true;
  else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
    state.error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
  state.status = status;
  state.cv.notify_all();
}
void CALLBACK OnRequestStatus(HINTERNET handle,
                              DWORD_PTR context,
                              DWORD status,
                              LPVOID info,
                              DWORD length) {
  if (context && status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
    auto& state = *reinterpret_cast<RequestState*>(context);
    std::lock_guard<std::mutex> lock(state.mutex);
    state.bytes = length;
    state.status = status;
    state.cv.notify_all();
    return;
  }
  OnStatus(handle, context, status, info, length);
}
struct RequestGuard {
  HINTERNET value;
  RequestState& state;
  void Close() {
    if (!value)
      return;
    WinHttpCloseHandle(value);
    value = nullptr;
    std::unique_lock<std::mutex> lock(state.mutex);
    state.cv.wait(lock, [&] { return state.closed; });
  }
  ~RequestGuard() { Close(); }
};
struct Handle {
  HINTERNET value = nullptr;
  ~Handle() {
    if (value)
      WinHttpCloseHandle(value);
  }
};
}  // namespace
HttpResponse QuerySogou(const std::string& payload,
                        const CloudQueryContext& context) {
  HttpResponse result;
  Handle session{WinHttpOpen(
      L"WeaselCloud/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
  if (!session.value)
    return {0, {}, "http_open"};
  WinHttpSetTimeouts(session.value, context.timeout_ms, context.timeout_ms,
                     context.timeout_ms, context.timeout_ms);
  Handle connection{WinHttpConnect(session.value, L"shouji.sogou.com",
                                   INTERNET_DEFAULT_HTTPS_PORT, 0)};
  if (!connection.value)
    return {0, {}, "http_connect"};
  auto request = WinHttpOpenRequest(
      connection.value, L"POST",
      L"/web_ime/"
      L"mobile.php?durtot=0&h=000000000000000&r=store_mf_wandoujia&v=3.7",
      nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
      WINHTTP_FLAG_SECURE);
  if (!request)
    return {0, {}, "http_request"};
  RequestState state;
  DWORD_PTR pointer = reinterpret_cast<DWORD_PTR>(&state);
  bool callback_ready =
      WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &pointer,
                       sizeof(pointer)) &&
      WinHttpSetStatusCallback(
          request, OnRequestStatus,
          WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
          0) != WINHTTP_INVALID_STATUS_CALLBACK;
  if (!callback_ready) {
    WinHttpCloseHandle(request);
    return {0, {}, "http_callback"};
  }
  // Redirects must not downgrade HTTPS or disclose the payload to another host.
  RequestGuard guard{request, state};
  DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects,
                        sizeof(redirects)))
    return {0, {}, "http_redirect_policy"};
  auto close = [&] { guard.Close(); };
  auto fail = [&]() -> HttpResponse {
    DWORD error = GetLastError();
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      if (state.error)
        error = state.error;
    }
    close();
    return {0,
            {},
            Clock::now() >= context.deadline
                ? "timeout"
                : "winhttp_" + std::to_string(error)};
  };
  if (!WinHttpSendRequest(
          request, L"Content-Type: application/octet-stream\r\n", -1L,
          const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()),
          static_cast<DWORD>(payload.size()), pointer) ||
      !state.Wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,
                  context.deadline))
    return fail();
  state.Reset();
  if (!WinHttpReceiveResponse(request, nullptr) ||
      !state.Wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, context.deadline))
    return fail();
  DWORD status = 0, size = sizeof(status);
  if (!WinHttpQueryHeaders(
          request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
          WINHTTP_NO_HEADER_INDEX))
    return fail();
  result.status = static_cast<int>(status);
  if (status == 200) {
    char buffer[4096];
    for (;;) {
      state.Reset();
      if (!WinHttpReadData(request, buffer, sizeof(buffer), nullptr) ||
          !state.Wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, context.deadline))
        return fail();
      if (!state.bytes)
        break;
      if (result.body.size() + state.bytes > 65536) {
        close();
        return {0, {}, "response_too_large"};
      }
      result.body.append(buffer, state.bytes);
    }
  }
  close();
  return result;
}
}  // namespace cloud
