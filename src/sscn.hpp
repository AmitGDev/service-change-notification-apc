#ifndef AMITGDEV_SSCN_HPP
#define AMITGDEV_SSCN_HPP

/*
    sscn.hpp
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

// clang-format off
// NOLINTBEGIN(misc-include-cleaner)
#include <Windows.h>
// clang-format on

#include <array>
#include <atomic>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace amitgdev {

// Service status change notifier using Windows APC notifications.
class ServiceStatusChangedNotifierAPC final {
 public:
  using ActionFunction = std::function<void(const std::wstring& service_name,
                                            DWORD current_state)>;

  // Require a genuinely noexcept callback at the API boundary. The callback can
  // still throw despite this contract, so ProcessChanges() keeps a defensive
  // runtime catch as the final backstop.
  template <typename F>
    requires std::is_nothrow_invocable_v<F&, const std::wstring&, DWORD>
  [[nodiscard]] bool Start(const std::vector<std::wstring>& service_list,
                           DWORD notify_mask, F&& action_function) {
    return StartImpl(service_list, notify_mask,
                     ActionFunction(std::forward<F>(action_function)));
  }

  [[nodiscard]] bool Exit() const;

 private:
  using ChangeList = std::list<std::tuple<std::wstring, DWORD, DWORD>>;

  using Context = struct {
    ChangeList* change_list;
  };

  struct ServiceData {
    std::array<wchar_t, MAX_PATH + 1> service_name{L'\0'};
    SC_HANDLE service_handle{nullptr};
    SERVICE_NOTIFY notify_buffer{};
    DWORD system_error_code{ERROR_SUCCESS};
  };

  using ServiceDataMap = std::unordered_map<std::wstring, ServiceData>;

  static VOID CALLBACK NotifyCallbackFunc(IN PVOID parameter);

  static void CloseServiceHandles(ServiceDataMap& service_data_map) noexcept;

  static void SubscribeToServiceChange(const std::wstring& service_name,
                                       DWORD notify_mask, Context& context,
                                       ServiceDataMap& service_data_map,
                                       ChangeList& change_list) noexcept;

  static void ProcessChanges(DWORD notify_mask,
                             const ActionFunction& action_function,
                             Context& context, ServiceDataMap& service_data_map,
                             ChangeList& change_list) noexcept;

  // noexcept is load-bearing: do not remove it. See the static_assert in
  // StartImpl(), below, which depends on it.
  void Run(const std::vector<std::wstring>& service_list, DWORD notify_mask,
           const ActionFunction& action_function) noexcept;

  // Holds the actual implementation, out-of-line in sscn.cpp, so that the
  // WinAPI-heavy body stays compiled once instead of being re-instantiated
  // per caller. The public Start() template above is the only caller,
  // after it has already erased the concrete callable into ActionFunction
  // and verified it at compile time.
  [[nodiscard]] bool StartImpl(const std::vector<std::wstring>& service_list,
                               DWORD notify_mask,
                               const ActionFunction& action_function);

  std::atomic_bool running_{false};
  mutable std::mutex exit_event_mutex_;
  HANDLE exit_event_{nullptr};
};

}  // namespace amitgdev

// NOLINTEND(misc-include-cleaner)

#endif  // AMITGDEV_SSCN_HPP
