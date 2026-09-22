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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <ranges>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace amitgdev {

ServiceStatusChangedNotifierAPC::~ServiceStatusChangedNotifierAPC() {
  // Exit() is not noexcept, but a destructor must not let anything escape.
  // Best-effort: if signaling or waiting somehow throws, there is nothing
  // more a destructor can do about it, and the object is being destroyed
  // regardless.
  try {
    static_cast<void>(Exit());
  } catch (...) {  // NOLINT(bugprone-empty-catch)
  }
}

// Windows invokes this callback on the worker thread when the SCM notification
// APC is delivered while the thread is in an alertable wait.
VOID CALLBACK ServiceStatusChangedNotifierAPC::NotifyCallbackFunc(
    IN PVOID parameter) noexcept {
  try {
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

    // This class only registers per-service, so pszServiceNames here is
    // always the single name SubscribeToServiceChange() pre-seeded, never
    // an OS-allocated MULTI_SZ buffer (that only happens for SCM-level
    // create/delete notifications, which this class does not use).
    std::wstring service_names;
    if (notification->pszServiceNames != nullptr) {
      service_names = notification->pszServiceNames;
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

    context->change_list->emplace_back(
        std::move(service_names), notification->ServiceStatus.dwCurrentState,
        notification_status);
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // This is an OS APC callback. No exception may escape it.
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
    const auto& [service_name, current_state, notification_status] =
        change_list.front();

    // ERROR_SERVICE_MARKED_FOR_DELETE means the prior notification failed
    // because the service is being deleted out from under this handle, not
    // an ordinary status change. The notification-result fields are not
    // valid in this case. The handle must be closed and must not be
    // resubscribed.
    if (notification_status == ERROR_SERVICE_MARKED_FOR_DELETE) {
      if (action_function) {
        try {
          action_function(service_name, 0);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
      }

      if (const auto service_data_it = service_data_map.find(service_name);
          service_data_it != service_data_map.end()) {
        auto& service_data = service_data_it->second;

        if (service_data.service_handle != nullptr) {
          CloseServiceHandle(service_data.service_handle);
          service_data.service_handle = nullptr;
        }
      }

      change_list.pop_front();
      continue;
    }

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
// StartImpl(), below, which depends on it.
//
// The running flag and exit event are not released here. They belong to the
// cleanup guard that StartImpl() passes to the thread alongside this
// function's arguments; it is destroyed only after this function has returned
// and its local resources (service handles, SCM handle) are released.
void ServiceStatusChangedNotifierAPC::Run(
    std::shared_ptr<State> state, const std::vector<std::wstring>& service_list,
    const DWORD notify_mask, const ActionFunction& action_function) noexcept {
  // Prevent exceptions from escaping the detached thread.
  try {
    ServiceDataMap service_data_map;
    ChangeList change_list{};
    Context context{&change_list};

    if (SC_HANDLE scm = OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASE,
                                       SC_MANAGER_ALL_ACCESS);
        scm != nullptr) {
      const auto scm_handle_guard =
          scope_exit{[scm] noexcept { CloseServiceHandle(scm); }};

      // Zero is outside the valid service-state range, so it doubles as a
      // sentinel: an entry seeded here means "just subscribe", not "dispatch
      // a state change" (see the current_state > 0 check in ProcessChanges).
      for (const auto& service_name : service_list) {
        // Create Map Item
        auto& service_data = service_data_map[service_name];

        service_name.copy(service_data.service_name.data(),
                          service_data.service_name.size() - 1);
        service_data.service_name.at(std::min(
            service_name.length(), service_data.service_name.size() - 1)) =
            L'\0';

        service_data.service_handle =
            OpenServiceW(scm, service_name.c_str(), SERVICE_ALL_ACCESS);

        // Create "Change" Item
        if (service_data.service_handle != nullptr) {
          constexpr int kNoCurrentStateYet = 0;
          change_list.emplace_back(service_name, kNoCurrentStateYet,
                                   ERROR_SUCCESS);
        }
      }
    }

    // Must be declared after context, change_list, and service_data_map so it
    // is destroyed before them. Closing the service handles cancels outstanding
    // SCM notifications before the callback context and change list are
    // destroyed.
    const auto service_handle_guard = scope_exit{[&service_data_map] noexcept {
      for (auto& service_data : service_data_map | std::views::values) {
        if (service_data.service_handle != nullptr) {
          CloseServiceHandle(service_data.service_handle);
          service_data.service_handle = nullptr;
        }
      }
    }};

    while (true) {
      ProcessChanges(notify_mask, action_function, context, service_data_map,
                     change_list);

      // Read without the mutex: exit_event_ is written only by StartImpl()
      // before the worker thread starts and by the cleanup guard after this
      // function returns. While this loop is active, its value cannot
      // change. Exit() only reads and signals the handle; it never writes
      // it.
      // Enter an alertable wait so the SCM notification APC can be delivered
      // to this worker thread. After the callback returns, the wait returns
      // WAIT_IO_COMPLETION and the loop continues with ProcessChanges().
      if (const DWORD wait_result =
              WaitForSingleObjectEx(state->exit_event, INFINITE, TRUE);
          wait_result == WAIT_OBJECT_0 || wait_result == WAIT_FAILED) {
        break;
      }
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Swallowed: there is no owner to propagate the exception to.
  }
}

bool ServiceStatusChangedNotifierAPC::StartImpl(
    const std::vector<std::wstring>& service_list, const DWORD notify_mask,
    const ActionFunction& action_function) {
  // The thread's entry lambda below is noexcept only because Run() is.
  static_assert(
      std::is_nothrow_invocable_v<
          decltype(&ServiceStatusChangedNotifierAPC::Run),
          std::shared_ptr<ServiceStatusChangedNotifierAPC::State>,
          const std::vector<std::wstring>&, DWORD,
          const ServiceStatusChangedNotifierAPC::ActionFunction&>,
      "ServiceStatusChangedNotifierAPC::Run must be noexcept: it is called "
      "from a noexcept std::jthread entry point, and an exception escaping it "
      "terminates the process instead of unwinding.");

  // Prevent multiple concurrent workers from being started.
  if (state_->running.exchange(true, std::memory_order_relaxed)) {
    return false;
  }

  // Single owner of the start-up acquisitions (running flag and exit event).
  // It fires on any early return or exception in this function, including
  // exceptions other than std::system_error (e.g. std::bad_alloc while the
  // jthread copies its arguments), which would otherwise leave running_ true
  // and make every later Start() fail permanently. Once moved into the thread
  // it fires when the thread is done with it. Either way it runs exactly once.
  // It owns a shared_ptr, not a reference to *state_, because in the worker it
  // can outlive this object.
  auto cleanup_guard = scope_exit{[state = state_] noexcept {
    const std::lock_guard<std::mutex> lock(state->exit_event_mutex);

    if (state->exit_event != nullptr) {
      CloseHandle(state->exit_event);
      state->exit_event = nullptr;
    }

    state->running.store(false, std::memory_order_release);
  }};

  // Create the event before starting the worker so an immediate Exit()
  // can always signal it.
  {
    const std::lock_guard<std::mutex> lock(state_->exit_event_mutex);

    state_->exit_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  if (state_->exit_event == nullptr) {
    return false;
  }

  try {
    // The guard travels as a std::jthread argument, so the thread machinery
    // makes every copy and move internally and the lambda has no captures of
    // its own. The guard parameter is unnamed: it exists only so the guard is
    // destroyed after Run() has returned and released its locals.
    std::jthread worker(
        [](std::shared_ptr<State> state,
           const std::vector<std::wstring>& thread_service_list,
           const DWORD thread_notify_mask,
           const ActionFunction& thread_action_function,
           auto /*cleanup*/) noexcept {
          Run(std::move(state), thread_service_list, thread_notify_mask,
              thread_action_function);
        },
        state_, service_list, notify_mask, action_function,
        std::move(cleanup_guard));

    worker.detach();
    return true;
  } catch (...) {
    // Thread creation or argument construction failed. The cleanup guard
    // restores the pre-start state during unwinding.
    return false;
  }
}

// Attempts to stop the worker thread and waits briefly for it to finish.
// Returns true if the worker thread is not running.
bool ServiceStatusChangedNotifierAPC::Exit() const {
  {
    const std::lock_guard<std::mutex> lock(state_->exit_event_mutex);

    if (state_->exit_event != nullptr) {
      SetEvent(state_->exit_event);
    }
  }

  for (int i = 0; i < 50; ++i) {
    if (!state_->running.load(std::memory_order_acquire)) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  return !state_->running.load(std::memory_order_acquire);
}

}  // namespace amitgdev

// NOLINTEND(misc-include-cleaner)
