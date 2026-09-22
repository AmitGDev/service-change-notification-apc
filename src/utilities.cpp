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

// clang-format off
// NOLINTBEGIN(misc-include-cleaner,cppcoreguidelines-pro-bounds-array-to-pointer-decay)
#include <Windows.h>
// clang-format on

#include <Psapi.h>
#include <TlHelp32.h>

#include <cwchar>

bool IsExecutableRuns(const std::wstring& full_path) {
  const std::wstring base_filename =
      full_path.substr(full_path.find_last_of(L"/\\") + 1);

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);

  HANDLE const snapshot =  // NOLINT(misc-misplaced-const)
      CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

  if (snapshot == INVALID_HANDLE_VALUE) {
    return false;
  }

  for (BOOL has_entry = Process32FirstW(snapshot, &entry); has_entry;
       has_entry = Process32NextW(snapshot, &entry)) {
    if (std::wcscmp(entry.szExeFile, base_filename.c_str()) != 0) {
      continue;
    }

    HANDLE const process_handle =  // NOLINT(misc-misplaced-const)
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                    entry.th32ProcessID);

    if (process_handle == nullptr) {
      continue;
    }

    WCHAR module_full_path[MAX_PATH + 1]{L'\0'};

    const DWORD length = GetModuleFileNameExW(process_handle, nullptr,
                                              module_full_path, MAX_PATH);

    const bool is_matching_process =
        length > 0 && length < MAX_PATH &&
        std::wcscmp(full_path.c_str(), module_full_path) == 0;

    CloseHandle(process_handle);

    if (is_matching_process) {
      CloseHandle(snapshot);
      return true;
    }
  }

  CloseHandle(snapshot);
  return false;
}

// NOLINTEND(misc-include-cleaner,cppcoreguidelines-pro-bounds-array-to-pointer-decay)