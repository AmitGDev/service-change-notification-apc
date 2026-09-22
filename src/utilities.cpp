/*
    utilities.cpp
    Copyright (c) 2024-2026, Amit Gefen

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include "utilities.hpp"

#include <string>

#include "scope.hpp"

// clang-format off
// NOLINTBEGIN(misc-include-cleaner,cppcoreguidelines-pro-bounds-array-to-pointer-decay)
#include <Windows.h>
// clang-format on

#include <Psapi.h>
#include <TlHelp32.h>

#include <cwchar>

// Find accessible matching process
bool IsExecutableRuns(const std::wstring& full_path) {
  if (full_path.empty() || full_path.size() > MAX_PATH) {
    return false;
  }

  const std::wstring base_filename =
      full_path.substr(full_path.find_last_of(L"/\\") + 1);

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

  if (snapshot == INVALID_HANDLE_VALUE) {
    return false;
  }

  const auto snapshot_guard =
      amitgdev::scope_exit{[snapshot] noexcept { CloseHandle(snapshot); }};

  for (BOOL has_entry = Process32FirstW(snapshot, &entry); has_entry;
       has_entry = Process32NextW(snapshot, &entry)) {
    if (std::wcscmp(entry.szExeFile, base_filename.c_str()) != 0) {
      continue;
    }

    // OpenProcess can legitimately fail due to permissions/protected processes.
    // The code treats that as "not this process"
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                 FALSE, entry.th32ProcessID);

    if (process == nullptr) {
      continue;
    }

    const auto process_guard =
        amitgdev::scope_exit{[process] noexcept { CloseHandle(process); }};

    WCHAR module_full_path[MAX_PATH + 1]{L'\0'};

    const DWORD length =
        GetModuleFileNameExW(process, nullptr, module_full_path, MAX_PATH);

    if (length > 0 && length < MAX_PATH &&
        std::wcscmp(full_path.c_str(), module_full_path) == 0) {
      return true;
    }
  }

  return false;
}

// NOLINTEND(misc-include-cleaner,cppcoreguidelines-pro-bounds-array-to-pointer-decay)