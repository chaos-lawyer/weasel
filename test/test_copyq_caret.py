"""Run the actual caret-publication methods with stubbed Win32 and Rime APIs.

This verifies session routing and invalidation; it is not a Windows/MSVC build.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "RimeWithWeasel/RimeWithWeasel.cpp").read_text()
methods = []
for name in ("UpdateInputPosition", "FocusOut"):
    match = re.search(r"void RimeWithWeaselHandler::" + name + r"\([\s\S]*?^}", source, re.M)
    assert match, name
    methods.append(match.group())

stub = r'''
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#define WINAPI
#define DLOG(level) if (false) std::cerr
using DWORD = unsigned int;
using UINT = unsigned int;
using HRESULT = int;
using HMONITOR = void*;
using WeaselSessionId = DWORD;
struct RECT { int left, top, right, bottom; };
struct MONITORINFO { size_t cbSize; RECT rcMonitor{}; };
constexpr int MONITOR_DEFAULTTONEAREST = 2;
#define FAILED(result) ((result) < 0)
RECT monitor_rect{1920, 0, 3840, 1080};
bool monitor_ok = true, dpi_available = true, dpi_ok = true;
UINT dpi = 144;
HRESULT MonitorDpi(HMONITOR, int kind, UINT* x, UINT* y) {
  assert(kind == 0); *x = *y = dpi; return dpi_ok ? 0 : -1;
}
void* GetModuleHandleW(const wchar_t*) { return nullptr; }
auto GetProcAddress(void*, const char*) { return dpi_available ? &MonitorDpi : nullptr; }
HMONITOR MonitorFromRect(const RECT*, int) { return nullptr; }
bool GetMonitorInfoW(HMONITOR, MONITORINFO* info) {
  assert(info->cbSize == sizeof(MONITORINFO)); info->rcMonitor = monitor_rect; return monitor_ok;
}
struct Api {
  std::map<DWORD, std::map<std::string, std::string>> properties;
  void set_property(DWORD id, const char* key, const char* value) { properties[id][key] = value; }
} api;
auto* rime_api = &api;
struct UI {
  int positions = 0, hides = 0;
  void UpdateInputPosition(const RECT&) { ++positions; }
  void Hide() { ++hides; }
};
struct Cloud { void Cancel(DWORD) {} };
struct RimeWithWeaselHandler {
  bool m_disabled = false;
  DWORD m_active_session = 0;
  UI* m_ui = nullptr;
  Cloud* m_cloud = nullptr;
  std::map<DWORD, int> m_cloud_queries;
  DWORD to_session_id(DWORD id) { return id ? id + 100 : 0; }
  void _UpdateUI(DWORD) {}
  void UpdateInputPosition(RECT const&, WeaselSessionId);
  void FocusOut(DWORD, WeaselSessionId);
};
'''
tests = r'''
int main() {
  RimeWithWeaselHandler handler;
  UI ui; handler.m_ui = &ui;
  handler.UpdateInputPosition({2220, 420, 2226, 450}, 1);
  assert(api.properties[101]["weasel_caret"] == "2220,450,1920,0,144");
  assert(handler.m_active_session == 1 && ui.positions == 1);
  handler.UpdateInputPosition({2500, 500, 2506, 530}, 2);
  assert(api.properties[102]["weasel_caret"] == "2500,530,1920,0,144");
  assert(api.properties[101]["weasel_caret"] == "2220,450,1920,0,144");
  handler.FocusOut(0, 2);
  assert(api.properties[102]["weasel_caret"].empty());
  assert(!api.properties[101]["weasel_caret"].empty());
  assert(handler.m_active_session == 0 && ui.hides == 1);
  handler.UpdateInputPosition({0, 0, 0, 0}, 1);
  assert(api.properties[101]["weasel_caret"].empty());
  handler.UpdateInputPosition({40, 40, 20, 20}, 1);
  assert(api.properties[101]["weasel_caret"].empty());
  monitor_rect = {-1920, -100, 0, 980};
  handler.UpdateInputPosition({-600, 300, -594, 330}, 1);
  assert(api.properties[101]["weasel_caret"] == "-600,330,-1920,-100,144");
  dpi_available = false;
  handler.UpdateInputPosition({-600, 300, -594, 330}, 1);
  assert(api.properties[101]["weasel_caret"] == "-600,330,-1920,-100,96");
  dpi_available = true; dpi_ok = false;
  handler.UpdateInputPosition({-600, 300, -594, 330}, 1);
  assert(api.properties[101]["weasel_caret"] == "-600,330,-1920,-100,96");
  monitor_ok = false;
  handler.UpdateInputPosition({-600, 300, -594, 330}, 1);
  assert(api.properties[101]["weasel_caret"].empty());
  handler.UpdateInputPosition({-600, 300, -594, 330}, 0);
  assert(api.properties.count(0) == 0);
  std::cout << "PASS caret session routing, updates, focus-out, invalid positions and DPI fallback\n";
}
'''
with tempfile.TemporaryDirectory(prefix="weasel-copyq-caret-") as temporary:
    folder = Path(temporary)
    cpp = folder / "test.cpp"
    cpp.write_text(stub + "\n".join(methods) + tests)
    executable = folder / "test"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
