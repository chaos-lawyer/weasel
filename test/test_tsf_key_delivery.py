"""Compile the actual TSF key callbacks with a recording IPC stub.

This checks callback de-duplication; it does not emulate Windows or an editor.
Run from any directory: python3 weasel/test/test_tsf_key_delivery.py
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'WeaselTSF/KeyEventSink.cpp').read_text()
methods = ('OnTestKeyDown', 'OnKeyDown', 'OnTestKeyUp', 'OnKeyUp')
callbacks = []
for method in methods:
    found = re.search(r'STDMETHODIMP WeaselTSF::' + method + r'\([\s\S]*?^}', source, re.M)
    assert found, method
    callbacks.append(found.group())

stub = r'''
#include <cstdint>
#include <iostream>
#include <vector>
#define STDMETHODIMP int
using BOOL = int;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
constexpr BOOL TRUE = 1, FALSE = 0;
constexpr int S_OK = 0;
struct ITfContext {};
struct WeaselTSF {
  BOOL _fTestKeyDownPending = FALSE, _fTestKeyUpPending = FALSE;
  bool _async_edit = false;
  std::vector<WPARAM> sent;
  void _ProcessKeyEvent(WPARAM key, LPARAM, BOOL* eaten) {
    sent.push_back(key);
    *eaten = TRUE;
  }
  void _UpdateComposition(ITfContext*) {}
  int OnTestKeyDown(ITfContext*, WPARAM, LPARAM, BOOL*);
  int OnKeyDown(ITfContext*, WPARAM, LPARAM, BOOL*);
  int OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL*);
  int OnKeyUp(ITfContext*, WPARAM, LPARAM, BOOL*);
};
'''
tests = r'''
int main() {
  int failures = 0;
  auto check = [&](const WeaselTSF& sink, size_t expected, const char* name) {
    const bool ok = sink.sent.size() == expected;
    std::cout << (ok ? "PASS " : "FAIL ") << name << ": expected "
              << expected << " deliveries, got " << sink.sent.size() << '\n';
    failures += !ok;
  };
  BOOL eaten = FALSE;
  {
    WeaselTSF sink;
    sink.OnTestKeyDown(nullptr, 'A', 0x001E0001, &eaten);
    sink.OnKeyDown(nullptr, 'A', 0, &eaten);
    check(sink, 1, "same key, different test/delivery lParam");
  }
  {
    WeaselTSF sink;
    sink.OnTestKeyDown(nullptr, 'A', 0x001E0001, &eaten);
    sink.OnTestKeyDown(nullptr, 'A', 0, &eaten);
    sink.OnKeyDown(nullptr, 'A', 0x001E0001, &eaten);
    check(sink, 1, "repeated test callbacks with differing metadata");
  }
  {
    WeaselTSF sink;
    sink.OnTestKeyUp(nullptr, 'A', 0xC01E0001u, &eaten);
    sink.OnKeyUp(nullptr, 'A', 0, &eaten);
    check(sink, 1, "same release, differing metadata");
  }
  {
    WeaselTSF sink;
    for (auto key : {'U', 'I', 'U', 'I'}) {
      sink.OnTestKeyDown(nullptr, key, 1, &eaten);
      sink.OnKeyDown(nullptr, key, 0, &eaten);
      sink.OnTestKeyUp(nullptr, key, 0xC0000001u, &eaten);
      sink.OnKeyUp(nullptr, key, 0, &eaten);
    }
    check(sink, 8, "four-letter input: one down and one up per key");
  }
  {
    WeaselTSF sink;
    sink.OnKeyDown(nullptr, 'A', 1, &eaten);
    sink.OnKeyUp(nullptr, 'A', 0xC0000001u, &eaten);
    check(sink, 2, "host without test callbacks");
  }
  {
    WeaselTSF sink;
    for (int i = 0; i < 3; ++i) {
      sink.OnTestKeyDown(nullptr, 'A', 1, &eaten);
      sink.OnKeyDown(nullptr, 'A', 1, &eaten);
    }
    check(sink, 3, "genuine repeated keydowns are not lost");
  }
  return failures ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory(prefix='weasel-key-delivery-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    executable = Path(tmp) / 'test'
    cpp.write_text(stub + '\n'.join(callbacks) + tests)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                    '-I' + str(root / 'include'), str(cpp), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)])
    raise SystemExit(result.returncode)
