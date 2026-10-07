#pragma once

#include <Windows.h>

#include <cwchar>
#include <string>

namespace weasel {

// Temporary, text-free diagnostic. Each process keeps at most 2 MiB.
inline void AppendWordCommitTrace(const wchar_t* message,
                                  bool word_process_only = true) {
  if (!message)
    return;
  if (word_process_only) {
    wchar_t module[MAX_PATH] = {0};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (!length || length == MAX_PATH)
      return;
    const wchar_t* name = std::wcsrchr(module, L'\\');
    if (_wcsicmp(name ? name + 1 : module, L"winword.exe") != 0)
      return;
  }

  wchar_t temp[MAX_PATH] = {0};
  const DWORD length = GetTempPathW(MAX_PATH, temp);
  if (!length || length >= MAX_PATH)
    return;
  const std::wstring directory = std::wstring(temp) + L"rime.weasel";
  CreateDirectoryW(directory.c_str(), nullptr);

  const DWORD pid = GetCurrentProcessId();
  const std::wstring path =
      directory + L"\\word_commit_trace_" + std::to_wstring(pid) + L".log";
  const std::wstring mutex_name =
      L"Local\\WeaselWordCommitTrace_" + std::to_wstring(pid);
  HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
  if (!mutex)
    return;
  const DWORD wait = WaitForSingleObject(mutex, INFINITE);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
    CloseHandle(mutex);
    return;
  }

  HANDLE file =
      CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t prefix[96];
    swprintf_s(prefix, L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu ",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
               now.wSecond, now.wMilliseconds, pid, GetCurrentThreadId());
    std::wstring line(prefix);
    line.append(message);
    if (line.empty() || line.back() != L'\n')
      line.push_back(L'\n');

    LARGE_INTEGER size = {};
    if (GetFileSizeEx(file, &size)) {
      const DWORD bytes = static_cast<DWORD>(line.size() * sizeof(wchar_t));
      const LARGE_INTEGER zero = {};
      if (size.QuadPart + bytes + sizeof(wchar_t) > 2 * 1024 * 1024) {
        SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
        SetEndOfFile(file);
        size.QuadPart = 0;
      }
      SetFilePointerEx(file, zero, nullptr, FILE_END);
      DWORD written = 0;
      if (size.QuadPart == 0) {
        const wchar_t bom = 0xfeff;
        WriteFile(file, &bom, sizeof(bom), &written, nullptr);
      }
      WriteFile(file, line.data(), bytes, &written, nullptr);
    }
    CloseHandle(file);
  }
  ReleaseMutex(mutex);
  CloseHandle(mutex);
}

}  // namespace weasel
