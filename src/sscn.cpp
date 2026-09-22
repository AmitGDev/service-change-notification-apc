/*
    sscn.cpp
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

#include "sscn.hpp"

#include "scope.hpp"

// clang-format off
// NOLINTBEGIN(misc-include-cleaner)
#include <Windows.h>
// clang-format on

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <ranges>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace amitgdev {

static std::wstring CopyServiceNames(const wchar_t* const service_names) {
  if (service_names == nullptr) {
    return {};
  }

  return service_names;
}

VOID CALLBACK
ServiceStatusChangedNotifierAPC::NotifyCallbackFunc(IN PVOID parameter) {
  auto const* const notification =
      static_cast<const SERVICE_NOTIFY*>(parameter);

  if (notification == nullptr) {
    return;
  }

  auto const* const context =
      static_cast<const Context*>(notification->pContext);

  if (context == nullptr || context->change_list == nullptr) {
    return;
  }

  std::wstring service_names;

  if (notification->pszServiceNames != nullptr) {
    service_names = CopyServiceNames(notification->pszServiceNames);
  }

  const DWORD notification_status = notification->dwNotificationStatus;

  if (notification_status == ERROR_SERVICE_MARKED_FOR_DELETE) {
    context->change_list->emplace_back(std::move(service_names), 0,
                                       notification_status);

    return;
  }

  if (notification_status != ERROR_SUCCESS) {
    return;
  }

  context->change_list->emplace_back(std::move(service_names),
                                     notification->ServiceStatus.dwCurrentState,
                                     notification_status);
}

void ServiceStatusChangedNotifierAPC::CloseServiceHandles(
    ServiceDataMap& service_data_map) noexcept {
  for (auto& service_data : service_data_map | std::views::values) {
    if (service_data.service_handle != nullptr) {
      CloseServiceHandle(service_data.service_handle);
      service_data.service_handle = nullptr;
    }
  }
}

void ServiceStatusChangedNotifierAPC::SubscribeToServiceChange(
    const std::wstring& service_name, const DWORD notify_mask, Context& context,
    ServiceDataMap& service_data_map, ChangeList& change_list) noexcept {
  const auto service_data_it = service_data_map.find(service_name);

  if (service_data_it == service_data_map.end()) {
    change_list.pop_front();
    return;
  }

  auto& service_data = service_data_it->second;

  if (service_data.service_handle == nullptr) {
    change_list.pop_front();
    return;
  }

  std::memset(&service_data.notify_buffer, 0,
              sizeof(service_data.notify_buffer));

  service_data.notify_buffer.pContext = &context;
  service_data.notify_buffer.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE;
  service_data.notify_buffer.pfnNotifyCallback = NotifyCallbackFunc;
  service_data.notify_buffer.pszServiceNames = service_data.service_name.data();

  service_data.system_error_code = NotifyServiceStatusChangeW(
      service_data.service_handle, notify_mask, &service_data.notify_buffer);

  change_list.pop_front();
}

void ServiceStatusChangedNotifierAPC::ProcessChanges(
    const DWORD notify_mask, const ActionFunction& action_function,
    Context& context, ServiceDataMap& service_data_map,
    ChangeList& change_list) noexcept {
  while (!change_list.empty()) {
    const auto& [service_name, current_state, extended] = change_list.front();

    static_cast<void>(extended);

    if (action_function && current_state > 0) {
      // The callback is required to be noexcept, but can still throw.
      // This catch is the final backstop against process termination.
      try {
        action_function(service_name, current_state);
      } catch (...) {  // NOLINT(bugprone-empty-catch)
      }
    }

    SubscribeToServiceChange(service_name, notify_mask, context,
                             service_data_map, change_list);
  }
}

// noexcept is load-bearing: do not remove it. See the static_assert in
// Start(), below, which depends on it.
void ServiceStatusChangedNotifierAPC::Run(
    const std::vector<std::wstring>& service_list, const DWORD notify_mask,
    const ActionFunction& action_function) noexcept {
  const auto running_guard = scope_exit{[this] noexcept { running_ = false; }};

  // Run() is a detached std::jthread entry point with no owner to catch or
  // rethrow to, so anything thrown here would otherwise call
  // std::terminate(). The try/catch below turns that into a clean shutdown
  // of just this notifier instance instead.
  try {
    ServiceDataMap service_data_map;
    ChangeList change_list{};
    Context context{&change_list};

    // exit_event_ is guaranteed non-null here: Start() creates it and does
    // not launch this thread until creation succeeds. Closing it is still
    // this function's job, under exit_event_mutex_ so Exit() can never
    // observe a handle value mid-close.
    const auto exit_event_guard = scope_exit{[this] noexcept {
      const std::lock_guard<std::mutex> lock(exit_event_mutex_);

      CloseHandle(exit_event_);
      exit_event_ = nullptr;
    }};

    SC_HANDLE scm = OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASE,
                                   SC_MANAGER_ALL_ACCESS);

    if (scm != nullptr) {
      const auto close_scm =
          scope_exit{[scm] noexcept { CloseServiceHandle(scm); }};

      for (const auto& service_name : service_list) {
        auto& service_data = service_data_map[service_name];

        service_name.copy(service_data.service_name.data(),
                          service_data.service_name.size() - 1);

        service_data.service_name.at(std::min(
            service_name.length(), service_data.service_name.size() - 1)) =
            L'\0';

        service_data.service_handle =
            OpenServiceW(scm, service_name.c_str(), SERVICE_ALL_ACCESS);

        if (service_data.service_handle != nullptr) {
          change_list.emplace_back(service_name, 0, ERROR_SUCCESS);
        }
      }
    }

    const auto service_handles_guard = scope_exit{[&service_data_map] noexcept {
      CloseServiceHandles(service_data_map);
    }};

    while (true) {
      ProcessChanges(notify_mask, action_function, context, service_data_map,
                     change_list);

      // Read without the lock: exit_event_ is only ever written by Start()
      // (before this thread starts) and by exit_event_guard above (after
      // this loop ends), so nothing concurrent can change it while this
      // thread is using it. Exit() only reads and signals, never writes.
      const DWORD wait_result =
          WaitForSingleObjectEx(exit_event_, INFINITE, TRUE);

      if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_FAILED) {
        break;
      }
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Swallowed: no owner to hand this to. running_guard still fires on the
    // way out either way.
  }
}

bool ServiceStatusChangedNotifierAPC::StartImpl(
    const std::vector<std::wstring>& service_list, const DWORD notify_mask,
    const ActionFunction& action_function) {
  // Detached thread entry points must remain noexcept.
  static_assert(
      std::is_nothrow_invocable_v<
          decltype(&ServiceStatusChangedNotifierAPC::Run),
          ServiceStatusChangedNotifierAPC*, const std::vector<std::wstring>&,
          DWORD, const ServiceStatusChangedNotifierAPC::ActionFunction&>,
      "ServiceStatusChangedNotifierAPC::Run must be noexcept: it is a "
      "std::jthread entry point, and an exception escaping it terminates the "
      "process instead of unwinding.");

  bool expected = false;

  if (!running_.compare_exchange_strong(expected, true)) {
    return false;
  }

  // Create the event here, before the worker thread exists, rather than
  // inside Run(): that way Start() never returns true while exit_event_ is
  // still null, closing the window where a caller's immediate Exit() call
  // would see nullptr, skip SetEvent, and time out without ever having told
  // the worker to stop.
  {
    const std::lock_guard<std::mutex> lock(exit_event_mutex_);

    exit_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  if (exit_event_ == nullptr) {
    running_ = false;
    return false;
  }

  try {
    std::jthread worker(&ServiceStatusChangedNotifierAPC::Run, this,
                        service_list, notify_mask, action_function);

    worker.detach();
    return true;
  } catch (const std::system_error&) {
    const std::lock_guard<std::mutex> lock(exit_event_mutex_);

    CloseHandle(exit_event_);
    exit_event_ = nullptr;
    running_ = false;
    return false;
  }
}

bool ServiceStatusChangedNotifierAPC::Exit() const {
  {
    const std::lock_guard<std::mutex> lock(exit_event_mutex_);

    if (exit_event_ != nullptr) {
      SetEvent(exit_event_);
    }
  }

  for (int i = 0; i < 50; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (!running_.load()) {
      break;
    }
  }

  return !running_.load();
}

}  // namespace amitgdev

// NOLINTEND(misc-include-cleaner)
