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
#include <memory>
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
  // Matches MAX_SERVICE_NAME_LENGTH from the RPC svcctl interface.
  constexpr static int kMaxServiceNameLength = 256;

  ServiceStatusChangedNotifierAPC() = default;

  // Best-effort Exit() so a caller who forgets does not leave the worker
  // thread orphaned forever: once this object is destroyed, nothing else
  // can ever call Exit() on it, and the worker would otherwise block on
  // exit_event_ with no one left able to signal it.
  ~ServiceStatusChangedNotifierAPC();

  // Copying or moving would let two objects share the same state_, and
  // therefore the same exit_event_ and running_ flag - exactly the kind of
  // unintended aliasing state_ (see below) exists to prevent between this
  // object and its own detached worker, not to introduce between two
  // objects.
  ServiceStatusChangedNotifierAPC(const ServiceStatusChangedNotifierAPC&) =
      delete;
  ServiceStatusChangedNotifierAPC&
  operator=(const ServiceStatusChangedNotifierAPC&) = delete;
  ServiceStatusChangedNotifierAPC(ServiceStatusChangedNotifierAPC&&) = delete;
  ServiceStatusChangedNotifierAPC&
  operator=(ServiceStatusChangedNotifierAPC&&) = delete;

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

  // Signals the worker to stop and waits up to 5 seconds. Returns false on
  // timeout - the worker may still be running; the caller decides whether
  // to retry, ignore it, or proceed with shutdown regardless.
  [[nodiscard]] bool Exit() const;

 private:
  using ChangeList = std::list<std::tuple<std::wstring, DWORD, DWORD>>;

  using Context = struct {
    ChangeList* change_list;
  };

  struct ServiceData {
    std::array<wchar_t, kMaxServiceNameLength + 1> service_name{L'\0'};
    SC_HANDLE service_handle{nullptr};
    SERVICE_NOTIFY notify_buffer{};
    DWORD system_error_code{ERROR_SUCCESS};
  };

  using ServiceDataMap = std::unordered_map<std::wstring, ServiceData>;

  // Holds exactly what a detached worker thread needs to outlive this
  // object safely: running_ and exit_event_ (plus its mutex). The worker
  // captures a shared_ptr to this, not `this`, so starting a notifier and
  // then destroying it before Exit() completes cannot leave the worker
  // dereferencing freed memory. Nothing else in this class needs this
  // treatment: every other member function here is already static and
  // touches no instance state.
  struct State {
    std::atomic_bool running{false};
    std::mutex exit_event_mutex;
    HANDLE exit_event{nullptr};
  };

  static VOID CALLBACK NotifyCallbackFunc(IN PVOID parameter) noexcept;

  static void SubscribeToServiceChange(const std::wstring& service_name,
                                       DWORD notify_mask, Context& context,
                                       ServiceDataMap& service_data_map,
                                       ChangeList& change_list) noexcept;

  static void ProcessChanges(DWORD notify_mask,
                             const ActionFunction& action_function,
                             Context& context, ServiceDataMap& service_data_map,
                             ChangeList& change_list) noexcept;

  // noexcept is load-bearing: do not remove it. See the static_assert in
  // StartImpl(), below, which depends on it. Takes state by value (a
  // shared_ptr copy, so a cheap atomic refcount bump) rather than binding
  // to `this`, so this worker's running_/exit_event_ stay valid for as
  // long as the thread runs, even if the owning
  // ServiceStatusChangedNotifierAPC object is destroyed first.
  //
  // Run() does not release running_ or exit_event_. StartImpl() hands the
  // thread a cleanup guard alongside Run()'s arguments, and that guard is
  // destroyed only after Run() has returned and released its own resources.
  // Do not call Run() from anywhere else without equivalent cleanup.
  static void Run(std::shared_ptr<State> state,
                  const std::vector<std::wstring>& service_list,
                  DWORD notify_mask,
                  const ActionFunction& action_function) noexcept;

  // Holds the actual implementation, out-of-line in sscn.cpp, so that the
  // WinAPI-heavy body stays compiled once instead of being re-instantiated
  // per caller. The public Start() template above is the only caller,
  // after it has already erased the concrete callable into ActionFunction
  // and verified it at compile time.
  [[nodiscard]] bool StartImpl(const std::vector<std::wstring>& service_list,
                               DWORD notify_mask,
                               const ActionFunction& action_function);

  std::shared_ptr<State> state_ = std::make_shared<State>();
};

}  // namespace amitgdev

// NOLINTEND(misc-include-cleaner)

#endif  // AMITGDEV_SSCN_HPP
