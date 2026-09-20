"""Exercise the real server key dispatch with a recording Rime API substitute.

This checks the fork's navigation/selection hooks, not Windows shortcut routing.
Run: python3 weasel/test/test_shortcut_forwarding.py
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "RimeWithWeasel/RimeWithWeasel.cpp").read_text()


def extract(signature):
    match = re.search(re.escape(signature) + r"\([\s\S]*?^}", source, re.M)
    assert match, signature
    return match.group()


stub = r'''
#include <cstdint>
#include <cstring>
#include <iostream>
using UINT = unsigned int;
using UINT32 = uint32_t;
using LPARAM = intptr_t;
using LPBYTE = unsigned char*;
using BOOL = int;
using Bool = int;
constexpr int TRUE = 1, FALSE = 0, True = 1, False = 0;
#include <KeyEvent.h>
#include <DynamicCandidateLayout.h>
#include <DynamicCandidateSelectKeys.h>
using namespace weasel;
using RimeSessionId = uintptr_t;
using WeaselSessionId = uint32_t;
using EatLine = int;
struct RimeContext { struct { size_t num_candidates = 0; } menu; };
#define RIME_STRUCT(T, name) T name{}
struct RecordingApi {
  std::string select_keys;
  size_t candidates = 5;
  int calls = 0, selections = 0, key = 0, mask = 0;
  Bool handled = True;
  Bool get_property(RimeSessionId, const char*, char* out, size_t size) {
    std::strncpy(out, select_keys.c_str(), size - 1);
    return !select_keys.empty();
  }
  Bool get_context(RimeSessionId, RimeContext* ctx) {
    ctx->menu.num_candidates = candidates;
    return True;
  }
  void free_context(RimeContext*) {}
  Bool process_key(RimeSessionId, int code, int modifiers) {
    ++calls; key = code; mask = modifiers; return handled;
  }
  Bool select_candidate_on_current_page(RimeSessionId, size_t) {
    ++selections; return True;
  }
  Bool get_option(RimeSessionId, const char*) { return False; }
  void set_option(RimeSessionId, const char*, Bool) {}
} api;
auto* rime_api = &api;
struct SessionStatus { UIStyle style; DynamicLayoutConfig dynamic_layout_config; };
struct RimeWithWeaselHandler {
  bool m_disabled = false;
  WeaselSessionId m_active_session = 0;
  SessionStatus status;
  RimeSessionId to_session_id(WeaselSessionId id) { return id; }
  SessionStatus& get_session_status(WeaselSessionId) { return status; }
  void _Respond(WeaselSessionId, EatLine) {}
  void _UpdateUI(WeaselSessionId) {}
  BOOL ProcessKeyEvent(KeyEvent, WeaselSessionId, EatLine);
  void _RemapCandidateNavigationKey(SessionStatus&, RimeSessionId, KeyEvent&);
};
'''
tests = r'''
int main() {
  int cases = 0;
  for (auto layout : {UIStyle::LAYOUT_HORIZONTAL, UIStyle::LAYOUT_VERTICAL})
  for (bool navigation : {false, true})
  for (auto keys : {"", "abcde", "fF123"})
  for (size_t candidates : {0u, 2u, 5u})
  for (int key : {'f', 'F'})
  for (int lock : {0, static_cast<int>(ibus::LOCK_MASK)})
  for (int release : {0, static_cast<int>(ibus::RELEASE_MASK)})
  for (Bool handled : {False, True}) {
    api = RecordingApi{};
    api.select_keys = keys;
    api.candidates = candidates;
    api.handled = handled;
    RimeWithWeaselHandler server;
    server.status.style.layout_type = layout;
    server.status.dynamic_layout_config.navigation =
        _DefaultCandidateNavigationConfig();
    server.status.dynamic_layout_config.navigation.enabled = navigation;
    const int mask = ibus::CONTROL_MASK | ibus::SHIFT_MASK | lock | release;
    const BOOL result = server.ProcessKeyEvent(KeyEvent(key, mask), 1, 0);
    // Compare with the 0.17.4 contract: one unmodified Rime call, and return
    // its handled result. The release bit expands from wire bit 14 to bit 30.
    const int expected_mask = 5 | lock | (release ? (1 << 30) : 0);
    if (api.calls != 1 || api.selections != 0 || api.key != key ||
        api.mask != expected_mask || result != handled) {
      std::cerr << "FAIL Ctrl+Shift+F forwarding: " << keys << ' '
                << key << ' ' << mask << '\n';
      return 1;
    }
    ++cases;
  }
  std::cout << "PASS " << cases
            << " shortcut cases: key/mask/result preserved, one Rime call\n";
}
'''
functions = "\n".join(extract(signature) for signature in (
    "int expand_ibus_modifier",
    "static CandidateNavigationConfig _DefaultCandidateNavigationConfig",
    "void RimeWithWeaselHandler::_RemapCandidateNavigationKey",
    "BOOL RimeWithWeaselHandler::ProcessKeyEvent",
))
with tempfile.TemporaryDirectory(prefix="weasel-shortcut-forwarding-") as tmp:
    directory = Path(tmp)
    (directory / "logging.h").write_text(
        "#pragma once\n#define DLOG(level) if (false) std::cerr\n")
    cpp = directory / "test.cpp"
    cpp.write_text(stub + functions + tests)
    executable = directory / "test"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17",
                    "-I" + str(directory), "-I" + str(root / "include"),
                    str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
