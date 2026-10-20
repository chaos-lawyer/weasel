"""Compile the real TSF caret methods against read-only mock editor/Win32 APIs.

This checks lifecycle and selection behavior, not Windows/MSVC integration.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'WeaselTSF/Composition.cpp').read_text()
start = source.index('class CExternalCaretEditSession')
end = source.index('bool WeaselTSF::_ExternalTemplateContextMatches', start)
methods = source[start:end]

stub = r'''
#include <ExternalCaret.h>
#include <cassert>
#include <functional>
#include <iostream>
using UINT = unsigned int;
using DWORD = uint32_t;
using ULONGLONG = uint64_t;
using HRESULT = int32_t;
using UINT_PTR = uintptr_t;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
using LRESULT = intptr_t;
using DWORD_PTR = uintptr_t;
using HWND = void*;
using BOOL = bool;
using ULONG = unsigned long;
using TfEditCookie = int;
constexpr HRESULT S_OK = 0, E_FAIL = -1, TF_S_ASYNC = 1;
constexpr int TF_ES_ASYNCDONTCARE = 0, TF_ES_READ = 2;
constexpr int TF_DEFAULT_SELECTION = 0, TF_AE_START = 1;
constexpr int TF_ANCHOR_START = 1, TF_ANCHOR_END = 2;
constexpr int GA_ROOT = 2, WM_COPYDATA = 0x4a;
constexpr int SMTO_ABORTIFHUNG = 2, SMTO_ERRORONEXIT = 32;
constexpr BOOL FALSE = false;
#define STDMETHODIMP HRESULT
#define FAILED(hr) ((hr) < 0)
#define SUCCEEDED(hr) ((hr) >= 0)
struct RECT { int32_t left = 0, top = 0, right = 0, bottom = 0; };
struct COPYDATASTRUCT { uintptr_t dwData; DWORD cbData; void* lpData; };
template<class T> struct com_ptr {
  T* p = nullptr;
  com_ptr() = default;
  com_ptr(T* value) : p(value) {}
  operator T*() const { return p; }
  T* operator->() const { return p; }
  T** operator&() { return &p; }
  void Attach(T* value) { p = value; }
};
HWND handle(uintptr_t value) { return reinterpret_cast<HWND>(value); }
uint64_t now = 1000;
HWND foreground = handle(42), focus = handle(43);
bool window_valid = true, clipped_result = false, layout_ok = true;
bool read_only = true, disabled = false;
DWORD receiver_pid = 77;
int anchor = 0, selected_count = 0, replies = 0, active_end = TF_ANCHOR_END;
RECT text_rect{-1800, 200, -1798, 222};
std::vector<uint8_t> last_reply;
std::function<void()> during_layout;
uint64_t GetTickCount64() { return now; }
HWND GetAncestor(HWND window, int) { return window == handle(43) ? handle(42) : window; }
HWND GetForegroundWindow() { return foreground; }
HWND GetFocus() { return focus; }
BOOL IsWindow(HWND) { return window_valid; }
DWORD GetWindowThreadProcessId(HWND, DWORD* pid) { if (pid) *pid = receiver_pid; return 1; }
BOOL GetWindowRect(HWND, RECT* rect) { *rect = {-1920, 0, 0, 1080}; return true; }
BOOL SendMessageTimeoutW(HWND window, UINT msg, WPARAM sender, LPARAM payload,
                        UINT, UINT timeout, DWORD_PTR*) {
  assert(window == handle(123) && msg == WM_COPYDATA && sender == 99 && timeout == 20);
  const auto* packet = reinterpret_cast<COPYDATASTRUCT*>(payload);
  assert(packet->dwData == weasel::external_caret::kReplyTag);
  const auto* bytes = static_cast<const uint8_t*>(packet->lpData);
  last_reply.assign(bytes, bytes + packet->cbData);
  ++replies;
  return true;
}
struct ITfRange {
  HRESULT Clone(ITfRange** result) { *result = this; return S_OK; }
  HRESULT Collapse(TfEditCookie ec, int end) { assert(ec == 7); anchor = end; return S_OK; }
} range;
struct TF_SELECTION { ITfRange* range; struct { int ase; } style; };
struct ITfContextView {
  HRESULT GetTextExt(TfEditCookie ec, ITfRange*, RECT* rect, BOOL* clipped) {
    assert(ec == 7); *rect = text_rect; *clipped = clipped_result;
    if (during_layout) during_layout();
    return layout_ok ? S_OK : E_FAIL;
  }
} view;
struct ITfContext;
struct WeaselTSF;
class CEditSession {
 public:
  CEditSession(com_ptr<WeaselTSF> service, com_ptr<ITfContext> context)
      : _pTextService(service), _pContext(context) {}
  virtual ~CEditSession() = default;
  virtual HRESULT DoEditSession(TfEditCookie) = 0;
 protected:
  com_ptr<WeaselTSF> _pTextService;
  com_ptr<ITfContext> _pContext;
};
CEditSession* pending = nullptr;
struct ITfContext {
  HRESULT GetSelection(TfEditCookie ec, int, int, TF_SELECTION* value, ULONG* count) {
    assert(ec == 7); ++selected_count;
    value->range = &range; value->style.ase = active_end; *count = 1; return S_OK;
  }
  HRESULT GetActiveView(ITfContextView** result) { *result = &view; return S_OK; }
  HRESULT RequestEditSession(int, CEditSession* session, int flags, HRESULT* result) {
    assert(read_only && flags == TF_ES_READ); pending = session; *result = TF_S_ASYNC; return S_OK;
  }
} context, other_context;
ITfContext* current_context = &context;
struct ITfDocumentMgr { HRESULT GetTop(ITfContext** result) { *result = current_context; return S_OK; } } document;
struct ThreadMgr { HRESULT GetFocus(ITfDocumentMgr** result) { *result = &document; return S_OK; } } thread_mgr;
struct WeaselTSF {
  HWND _external_template_window = handle(99);
  ThreadMgr* _pThreadMgr = &thread_mgr;
  int _tfClientId = 1;
  UINT _external_caret_id = 0;
  ULONGLONG _external_caret_deadline = 0;
  BOOL _IsKeyboardDisabled() { return disabled; }
  LRESULT _ReceiveExternalCaret(HWND, const COPYDATASTRUCT*);
  bool _ExternalCaretContextMatches(ITfContext*, HWND);
  HRESULT _ReadExternalCaret(TfEditCookie, ITfContext*, UINT, HWND, HWND, DWORD, ULONGLONG);
};
'''

tests = r'''
std::vector<uint8_t> request_wire(UINT id, uint64_t deadline) {
  using weasel::external_template::AppendNumber;
  std::vector<uint8_t> wire;
  AppendNumber(wire, 1, 4); AppendNumber(wire, id, 4); AppendNumber(wire, 42, 8);
  AppendNumber(wire, 123, 8); AppendNumber(wire, deadline, 8);
  return wire;
}
int main() {
  WeaselTSF service;
  auto wire = request_wire(1, 1100);
  COPYDATASTRUCT packet{weasel::external_caret::kRequestTag, static_cast<DWORD>(wire.size()), wire.data()};
  assert(service._ReceiveExternalCaret(handle(124), &packet) == 0);
  assert(service._ReceiveExternalCaret(handle(123), &packet) == 1);
  assert(service._ReceiveExternalCaret(handle(123), &packet) == 0);  // one pending query
  assert(pending && replies == 0 && selected_count == 0);
  pending->DoEditSession(7); delete pending; pending = nullptr;
  assert(replies == 1 && selected_count == 1 && anchor == TF_ANCHOR_END);
  assert(last_reply == weasel::external_caret::Encode({1, 42, true, -1800, 200, -1798, 222}));
  assert(service._external_caret_deadline == 0);
  auto run = [&](UINT id) {
    service._external_caret_id = id; service._external_caret_deadline = 1100;
    service._ReadExternalCaret(7, &context, id, handle(42), handle(123), 77, 1100);
  };
  active_end = TF_AE_START;
  run(2); assert(anchor == TF_ANCHOR_START && replies == 2);
  now = 1100;
  run(3); assert(replies == 2); now = 1000;  // expiration
  service._external_caret_id = 99; service._external_caret_deadline = 1100;
  service._ReadExternalCaret(7, &context, 3, handle(42), handle(123), 77, 1100);
  assert(replies == 2);  // superseded request
  foreground = handle(88); run(4); assert(replies == 2); foreground = handle(42);
  current_context = &other_context; run(5); assert(replies == 2); current_context = &context;
  receiver_pid = 88; run(6); assert(replies == 2); receiver_pid = 77;
  disabled = true; run(7); assert(replies == 2); disabled = false;
  layout_ok = false; run(8); assert(replies == 3);
  assert(weasel::external_template::ReadNumber(last_reply.data() + 16, 4) == 0);
  layout_ok = true; clipped_result = true; run(9); assert(replies == 4);
  assert(weasel::external_template::ReadNumber(last_reply.data() + 16, 4) == 0);
  clipped_result = false; text_rect = {0, 0, 0, 0}; run(10); assert(replies == 5);
  assert(weasel::external_template::ReadNumber(last_reply.data() + 16, 4) == 0);
  text_rect = {-1800, 200, -1798, 222};
  during_layout = [] { foreground = handle(88); }; run(11); assert(replies == 5);
  during_layout = {}; foreground = handle(42);
  std::cout << "PASS real TSF caret methods: read-only session, active selection edge, async, expiry, focus/context/window validation, unavailable layout\n";
}
'''

with tempfile.TemporaryDirectory(prefix='weasel-external-caret-') as temporary:
    folder = Path(temporary)
    cpp = folder / 'test.cpp'
    cpp.write_text(stub + methods + tests)
    executable = folder / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'include'), str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
