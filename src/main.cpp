//  ( * RUN "AS ADMIN"! * )

// clang-format off
#include <Windows.h>  // NOLINT(misc-include-cleaner)
// clang-format on

#include <algorithm>
#include <chrono>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sscn.hpp"
#include "utilities.hpp"

// Implements the logic in which we want to ignore 'on_notification_action'ד
static bool MuteLogic() {
  const std::vector<std::wstring> vector_of_exe_full_paths{
      // EXAMPLE: 2 processes that mute the 'Action' (see below):
      L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
      L"C:\\Program Files (x86)\\Internet\\Explorer\\iexplorer.exe",
  };  // Win 7

  for (const auto& exe_full_path : vector_of_exe_full_paths) {
    if (IsExecutableRuns(exe_full_path)) {
      std::cout << "muted" << '\n';
      return true;  // <-- MUTE
    }
  }

  return false;
}

// Implement what to do on service status-changed notification.
// NOLINTBEGIN(misc-include-cleaner)
static void OnNotificationActionFunction(const std::wstring& service_name,
                                         const DWORD current_state) noexcept {
  // NOLINTEND(misc-include-cleaner)
  try {
    std::string str;

    std::ranges::transform(
        service_name, std::back_inserter(str),
        [](const wchar_t chr) { return static_cast<char>(chr); });

    // Log notification
    std::cout << "notification: " << str << " current state: " << current_state
              << '\n';

    // If not 'muted' and stopped, take action.
    // NOLINTBEGIN(misc-include-cleaner)
    if (!MuteLogic() && current_state == SERVICE_NOTIFY_STOPPED) {
      Beep(3000, 200);  // <-- ACTION
                        // NOLINTEND(misc-include-cleaner)
      std::cout << "took action" << '\n';
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Swallowed: this callback owns its own failures. sscn has nothing to
    // say about what happens inside a caller-supplied action function.
  }
}

// MAIN
// *RUN "AS ADMIN"!*
int main() {
  try {
    std::unique_ptr<amitgdev::ServiceStatusChangedNotifierAPC>
        service_status_change_notifier{};

    service_status_change_notifier =
        std::make_unique<amitgdev::ServiceStatusChangedNotifierAPC>();

    if (!service_status_change_notifier->Start(
            std::vector<std::wstring>{
                L"W32Time",
                L"WebClient",
            },                       // <-- Example: For these 2 services,
            SERVICE_NOTIFY_STOPPED,  // notify about service STOPPED (Notify
                                     // Mask).
            OnNotificationActionFunction)) {  // <-- Notify to this function

      std::cout << "service_status_change_notifier.start - failed" << '\n';
      return -1;
    }

    // 5 minutes sleep (in 5 intervals for debugging).
    for (int i = 0; i < 5; i++) {
      std::this_thread::sleep_for(std::chrono::minutes(1));
    }

    if (const bool exited = service_status_change_notifier->Exit(); !exited) {
      std::cout << "service_status_change_notifier.exit timeout" << '\n';
      return -1;
    }
  } catch (...) {
    return -1;
  }
};
