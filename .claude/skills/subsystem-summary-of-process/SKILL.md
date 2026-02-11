---
name: subsystem-summary-of-process
description: "read this skill for a token-efficient summary of the process subsystem"
---

# Process Subsystem — Technical Summary

## Overview

The process subsystem provides asynchronous subprocess management for stellar-core, wrapping platform-specific process spawning (POSIX `posix_spawnp` and Windows `CreateProcess`) behind a unified asio-integrated interface. It enables running external commands (e.g., history archival tools like `gzip`, `gunzip`, `curl`) asynchronously, with configurable concurrency limits, output file capture, graceful shutdown, and process lifecycle tracking. No facilities exist for reading/writing subprocess I/O ports — this is strictly for "run a command, wait to see if it worked."

## Key Files

- **ProcessManager.h** — Abstract interface `ProcessManager` and the `ProcessExitEvent` class for async process completion notification.
- **ProcessManagerImpl.h / ProcessManagerImpl.cpp** — Concrete implementation of `ProcessManager`; contains all lifecycle management, platform-specific spawning, signal handling, and shutdown logic.
- **PosixSpawnFileActions.h / PosixSpawnFileActions.cpp** — POSIX-only RAII wrapper around `posix_spawn_file_actions_t` for redirecting subprocess stdout to a file.

---

## Key Classes and Data Structures

### `ProcessManager` (abstract, inherits `std::enable_shared_from_this<ProcessManager>`, `NonMovableOrCopyable`)

The public interface for subprocess management. One `ProcessManager` exists per `Application` instance, created via the static factory `ProcessManager::create(Application&)`.

**Key virtual methods:**
- `runProcess(cmdLine, outputFile)` → `std::weak_ptr<ProcessExitEvent>` — Queues or immediately launches a subprocess. If `outputFile` is non-empty, stdout is captured to a temp file and atomically renamed on success.
- `getNumRunningProcesses()` — Count of active (non-shutting-down) child processes.
- `getNumRunningOrShuttingDownProcesses()` — Count of all tracked child processes including those being terminated.
- `tryProcessShutdown(pe)` — Synchronously cancels a `ProcessExitEvent` and attempts to terminate the associated process (SIGTERM on POSIX, `GenerateConsoleCtrlEvent` on Windows). Returns `true` if the termination signal was sent successfully.
- `shutdown()` — Marks the manager as shut down, cancels all pending processes, and attempts polite shutdown of all running processes.
- `isShutdown()` — Returns whether shutdown has been initiated.

### `ProcessExitEvent`

An asio-compatible event object that clients use to await subprocess completion. It simulates an event notifier using a `RealTimer` set to maximum duration. When the subprocess exits (or is cancelled), the timer is cancelled with an appropriate error code.

**Members:**
- `mTimer` (`std::shared_ptr<RealTimer>`) — The underlying asio timer used for async waiting.
- `mImpl` (`std::shared_ptr<Impl>`) — Platform-specific implementation details (command line, output file, process handle, lifecycle state).
- `mEc` (`std::shared_ptr<asio::error_code>`) — Shared error code for communicating the real exit status past asio's timer cancellation (which always delivers `operation_aborted`).

**Key methods:**
- `async_wait(handler)` — Registers a callback that fires when the process exits. The handler receives an `asio::error_code` where a zero value means success, and a non-zero value encodes the process exit status.

### `ProcessExitEvent::Impl` (internal, inherits `std::enable_shared_from_this`)

Holds all per-process state and manages the platform-specific spawning logic.

**Members:**
- `mOuterTimer` / `mOuterEc` — Shared pointers back to the owning `ProcessExitEvent`'s timer and error code.
- `mCmdLine` (`std::string const`) — The command line to execute.
- `mOutFile` (`std::string const`) — The desired final output file path (may be empty if no capture).
- `mTempFile` (`std::string const`) — Temporary file path for capturing stdout before atomic rename.
- `mLifecycle` (`ProcessLifecycle`) — Tracks the process through its states.
- `mProcessId` (`int`) — PID of the spawned process (-1 before launch).
- `mProcessHandle` (Windows only, `asio::windows::object_handle`) — Waitable handle for the Windows process object.
- `mProcManagerImpl` (`std::weak_ptr<ProcessManagerImpl>`) — Weak back-reference to the owning manager.

**Key methods:**
- `run()` — Platform-specific process spawning. On POSIX: splits the command line, sets up `PosixSpawnFileActions` for output redirection, sets `FD_CLOEXEC` on all open file descriptors ≥ 3, then calls `posix_spawnp()`. On Windows: sets up `STARTUPINFOEX` with inheritable handles, calls `CreateProcess()`, and registers an async wait on the process handle.
- `finish()` — Called on termination; renames the temp file to the final output file (atomic move). Returns `false` if the rename fails.
- `cancel(ec)` — Sets the outer error code and cancels the outer timer, firing all registered `async_wait` handlers.

### `ProcessManagerImpl` (inherits `ProcessManager`)

The concrete implementation that manages the full lifecycle of subprocesses.

**Members:**
- `mProcessesMutex` (`std::recursive_mutex`) — Guards `mProcesses` and `mPending` since subprocess exits arrive asynchronously.
- `mProcesses` (`std::map<int, std::shared_ptr<ProcessExitEvent>>`) — Maps PID to running or shutting-down processes.
- `mPending` (`std::deque<std::shared_ptr<ProcessExitEvent>>`) — Queue of processes waiting to be launched (when concurrency limit is reached).
- `mIsShutdown` (`bool`) — Set once `shutdown()` is called.
- `mMaxProcesses` (`size_t const`) — Maximum concurrent subprocesses, from `Config::MAX_CONCURRENT_SUBPROCESSES`.
- `mIOContext` (`asio::io_context&`) — The application's I/O context for async operations.
- `mSigChild` (`asio::signal_set`) — On POSIX, listens for `SIGCHLD` to detect child process exits. Unused on Windows.
- `mTmpDir` (`std::unique_ptr<TmpDir>`) — Temporary directory for capturing subprocess output files.
- `mTempFileCount` (`uint64_t`) — Monotonic counter for generating unique temp file names.

### `ProcessLifecycle` (enum, anonymous namespace)

Tracks the state of a subprocess through its lifetime:
- `PENDING` (0) — Queued, not yet spawned.
- `RUNNING` (1) — Spawned, waiting for exit.
- `TRIED_POLITE_SHUTDOWN` (2) — SIGTERM (POSIX) or CTRL_C_EVENT (Windows) sent.
- `TRIED_FORCED_SHUTDOWN` (3) — SIGKILL (POSIX) or TerminateProcess (Windows) sent.
- `TERMINATED` (5) — Exit detected and handled.

### `PosixSpawnFileActions` (POSIX only)

RAII wrapper around `posix_spawn_file_actions_t`. Lazily initializes the actions object on first `addOpen()` call. Provides an implicit conversion to `posix_spawn_file_actions_t*` (returns `nullptr` if never initialized, meaning no file actions).

**Key methods:**
- `addOpen(fildes, fileName, oflag, mode)` — Registers a file-open action for the child process (used to redirect fd 1 / stdout to a temp file).
- `initialize()` — Calls `posix_spawn_file_actions_init()`; idempotent.
- Destructor calls `posix_spawn_file_actions_destroy()` if initialized.

---

## Key Control Flows

### Process Launch Flow

1. Client calls `ProcessManagerImpl::runProcess(cmdLine, outFile)`.
2. A new `ProcessExitEvent` is created with a `RealTimer` set to max duration.
3. A `ProcessExitEvent::Impl` is created holding the command line, output file, a generated temp file path, and a weak reference to the manager.
4. The event is pushed onto `mPending`.
5. `maybeRunPendingProcesses()` is called, which pops events from `mPending` while `getNumRunningOrShuttingDownProcesses() < mMaxProcesses`.
6. For each dequeued event, `Impl::run()` is called:
   - **POSIX:** Command line is split on whitespace into argv. `PosixSpawnFileActions` is set up if output capture is needed. All file descriptors ≥ 3 are marked `FD_CLOEXEC`. `posix_spawnp()` is called. Lifecycle transitions to `RUNNING`.
   - **Windows:** `STARTUPINFOEX` and handle inheritance are configured. `CreateProcess()` is called with `CREATE_NEW_PROCESS_GROUP`. An async wait is registered on the process handle. Lifecycle transitions to `RUNNING`.
7. The PID is recorded in `mProcesses`.
8. A `weak_ptr<ProcessExitEvent>` is returned to the caller, who calls `async_wait()` to register a completion handler.

### Process Exit Handling (POSIX)

1. `SIGCHLD` arrives, handled by `asio::signal_set` → `handleSignalChild()`.
2. `handleSignalChild()` re-registers the signal handler (via `startWaitingForSignalChild()`), then calls `reapChildren()`.
3. `reapChildren()` iterates all tracked PIDs, calling `waitpid(pid, &status, WNOHANG)` for each.
4. For each successfully reaped child, `handleProcessTermination(pid, status)` is called.
5. `handleProcessTermination()` maps the exit status to an `asio::error_code` (via `mapExitStatusToErrorCode`), calls `Impl::finish()` to rename the temp output file, removes the process from `mProcesses`, calls `maybeRunPendingProcesses()` to launch queued processes, then fires the callback via `Impl::cancel(ec)`.

### Process Exit Handling (Windows)

1. The `asio::windows::object_handle::async_wait` fires when the process handle becomes signaled.
2. The callback calls `GetExitCodeProcess()` and passes the result to `handleProcessTermination()`.
3. The rest follows the same flow as POSIX.

### Shutdown Flow

1. `shutdown()` sets `mIsShutdown = true`.
2. All pending (not yet launched) processes are cancelled with `ABORT_ERROR_CODE`.
3. `tryProcessShutdownAll()` iterates all running processes and calls `tryProcessShutdown()` on each.
4. `tryProcessShutdown()` uses a two-phase approach based on lifecycle state:
   - If `RUNNING`: calls `politeShutdown()` (SIGTERM / CTRL_C_EVENT), advances to `TRIED_POLITE_SHUTDOWN`.
   - If `TRIED_POLITE_SHUTDOWN`: calls `forcedShutdown()` (SIGKILL / TerminateProcess), advances to `TRIED_FORCED_SHUTDOWN`.
5. The destructor (`~ProcessManagerImpl()`) ensures cleanup: it cancels the `SIGCHLD` handler, calls `shutdown()`, then loops up to 3 times sleeping 10ms, reaping children, and re-triggering progressively more forceful shutdown.

### Concurrency Control

- The maximum number of concurrent subprocesses is controlled by `mMaxProcesses` (from `Config::MAX_CONCURRENT_SUBPROCESSES`).
- When the limit is reached, new processes are queued in `mPending` (a FIFO deque).
- After each process exit (`handleProcessTermination`), `maybeRunPendingProcesses()` is called to launch queued processes up to the limit.
- All access to `mProcesses` and `mPending` is guarded by `mProcessesMutex` (a recursive mutex), since process exits arrive asynchronously from signal handlers or async I/O callbacks.

---

## Ownership Relationships

```
Application
 └── ProcessManagerImpl (shared_ptr, via ProcessManager::create)
      ├── mProcesses: map<pid, shared_ptr<ProcessExitEvent>>
      │    └── ProcessExitEvent
      │         ├── mTimer: shared_ptr<RealTimer>
      │         ├── mEc: shared_ptr<asio::error_code>
      │         └── mImpl: shared_ptr<ProcessExitEvent::Impl>
      │              └── mProcManagerImpl: weak_ptr<ProcessManagerImpl> (back-ref)
      ├── mPending: deque<shared_ptr<ProcessExitEvent>>
      ├── mSigChild: asio::signal_set (POSIX only)
      └── mTmpDir: unique_ptr<TmpDir>
```

- `ProcessManagerImpl` owns all `ProcessExitEvent` objects (via `mProcesses` and `mPending`).
- `ProcessExitEvent::Impl` holds a `weak_ptr` back to `ProcessManagerImpl` to avoid circular ownership.
- Callers receive a `weak_ptr<ProcessExitEvent>` from `runProcess()`, so the manager controls the event's lifetime.
- The `Impl::run()` method on Windows captures a `shared_from_this()` to keep `Impl` alive through the async wait callback.

---

## Key Data Flows

1. **Command → Process:** `runProcess(cmdLine, outFile)` → queued in `mPending` → dequeued by `maybeRunPendingProcesses()` → `Impl::run()` spawns the OS process.
2. **Process Exit → Callback:** OS signal (SIGCHLD) or handle wait → `reapChildren()` / handle callback → `handleProcessTermination()` → `Impl::finish()` (rename temp file) → `Impl::cancel(ec)` → timer cancelled → `async_wait` handler fires with exit code.
3. **Output Capture:** Subprocess stdout is redirected to a temp file in `mTmpDir`. On successful termination, `Impl::finish()` atomically renames the temp file to the requested output file path. If the output file already exists, the rename fails and an error is propagated.
4. **Exit Code Mapping:** `mapExitStatusToErrorCode()` translates OS-level exit status (including POSIX `WIFEXITED`/`WEXITSTATUS` macros) into `asio::error_code`. Special handling for exit code 127 on Linux (likely missing command). On Windows, the exit code is used directly.
5. **Shutdown Signal Flow:** `shutdown()` → cancel pending → polite shutdown (SIGTERM) → forced shutdown (SIGKILL) → destructor reaps remaining children with retry loop.

---

## Platform Differences

| Aspect | POSIX | Windows |
|--------|-------|---------|
| Process spawning | `posix_spawnp()` | `CreateProcess()` with `EXTENDED_STARTUPINFO_PRESENT` |
| Exit detection | `SIGCHLD` via `asio::signal_set` + `waitpid(WNOHANG)` | `asio::windows::object_handle::async_wait` |
| Output redirection | `posix_spawn_file_actions_addopen()` on fd 1 | `CreateFile()` + `STARTF_USESTDHANDLES` |
| Polite shutdown | `kill(pid, SIGTERM)` | `GenerateConsoleCtrlEvent(CTRL_C_EVENT, pid)` |
| Forced shutdown | `kill(pid, SIGKILL)` | `TerminateProcess()` |
| FD cleanup | `FD_CLOEXEC` on fds 3..SC_OPEN_MAX (with gap heuristic) | Handle inheritance via `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` |

---

## Error Handling Notes

- If `posix_spawnp()` or `CreateProcess()` fails, the `ProcessExitEvent` is cancelled with `std::errc::io_error` and the error is logged.
- On Linux, exit code 127 triggers a prominent warning about a likely missing command (since `posix_spawnp` does not fault on file-not-found in the parent).
- The timer-based `async_wait` pattern works around asio always delivering `operation_aborted` on timer cancel: the real error code is stored in a shared `mEc` variable and the handler reads from that instead of using the asio-provided code.
- `checkInvariants()` validates consistency: pending processes must be in `PENDING` state, running processes must not be `PENDING`, and PIDs must match map keys. Called at key state transitions.
