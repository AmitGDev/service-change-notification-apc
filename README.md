# Windows Service Notifications - Asynchronous Procedure Call (APC)

A C++23 Windows component for monitoring the state of selected Windows services asynchronously through the Service Control Manager (SCM).

The project provides `ServiceStatusChangedNotifierAPC`, a small notifier that registers `NotifyServiceStatusChange` callbacks for selected services and invokes an application-defined action when a requested service-state transition occurs.

## How It Works

The notifier:

1. Opens the Windows Service Control Manager.
2. Opens the services selected by the caller.
3. Registers `NotifyServiceStatusChange` for the requested notification mask.
4. Waits for notifications on a dedicated worker thread.
5. Processes the SCM callback through an alertable wait.
6. Dispatches the resulting service-state change to the caller's action function.

The worker thread uses an alertable wait:

```cpp
WaitForSingleObjectEx(exit_event_, INFINITE, TRUE);
```

The final `TRUE` allows the thread to process APC, which is required for the registered notification callback to execute.

## Example

The example monitors two Windows services:

```cpp
std::vector<std::wstring>{
    L"W32Time",
    L"WebClient",
}
```

and requests notifications when either service enters the stopped state:

```cpp
SERVICE_NOTIFY_STOPPED
```

The supplied callback receives the service name and current state and can perform application-specific processing. It must be declared `noexcept`; `Start()` rejects a callable that is not at compile time. The callback owns its own exception safety - `ServiceStatusChangedNotifierAPC` has no way to know what the callback does internally, so a callback whose body can throw should guard itself:

```cpp
[](const std::wstring& service_name, DWORD current_state) noexcept {
  try {
    // Handle the service state change.
  } catch (...) {
    // This callback's own failure, not the notifier's problem.
  }
}
```

The callback also runs synchronously inside the worker thread's own loop, so it must return quickly. There is no timeout or cancellation on this call from the notifier's side; a callback that blocks indefinitely (an unbounded network call, a lock also held elsewhere in the caller's application, an infinite loop) stalls the worker thread until it is unstuck or the process ends. `Exit()`'s bounded timeout protects the caller's own thread from waiting forever, but does not free a stuck worker. Offload any real work to another thread if it might not return quickly.

The example logs the notification and demonstrates triggering an application-defined action when a monitored service stops.

## Service Creation and Deletion

Windows also supports SCM-level notifications for service creation and deletion. These notifications can contain multiple service names in a `MULTI_SZ` buffer and use a `/` prefix to distinguish created services from deleted services.

That is a different notification mechanism from the per-service status monitoring implemented here and is not used by the current example.

## Requirements

- A C++23-compatible compiler and standard library.
- Administrator privileges when running the example.

## Build and Run

- [Build on Windows](https://github.com/AmitGDev/cpp-ci-template#build-on-windows)

## Code Style

- [C++](https://github.com/AmitGDev/cpp-coding-standard)
- [Python](https://github.com/AmitGDev/cpp-ci-template#python)
- [CMake](https://github.com/AmitGDev/cpp-ci-template#cmake)

## License

MIT License. Copyright © 2024-2026 Amit Gefen.

---

**Last Updated:** 2026-10-23  
**Maintainer:** AmitGDev