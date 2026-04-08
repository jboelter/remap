# Copilot Instructions

## Build, test, and lint commands

- Preferred build from a normal Windows shell: `.\build.bat` (defaults to `Debug`).
- Release build: `.\build.bat Release`.
- Equivalent manual CMake flow from a Visual Studio developer environment: `cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Debug` then `cmake --build build`.
- Build a single target: `cmake --build build --target remap` or `cmake --build build --target tap-timer`.
- No automated test suite or lint target is configured in this repository. There is no single-test command.
- The README's focused manual verification path for Enter remapping is: `.\build\remap.exe --double-tap --delay 200 -- .\build\tap-timer.exe`.

## High-level architecture

- This is a Windows-only console proxy. `remap` runs inside the outer Windows Terminal session, creates an inner ConPTY for the child command, captures raw console input from the outer session, rewrites selected keys, writes the resulting VT/UTF-8 byte stream into the child ConPTY, and pumps child output back to the outer console.
- `main.cpp` contains almost all runtime behavior: argument parsing, Windows handle and console RAII guards, ConPTY creation, child process startup, input-event polling, key encoding, delayed double-tap handling, output forwarding, and resize propagation.
- Enter handling is centralized in `RemappedEnterPayload`, `IsPlainEnterCandidate`, and `EncodeKey`, with the timing-sensitive double-tap state handled in the main event loop through `pendingEnterDeadline`. The default mode swaps Enter and Shift+Enter; `--standard-enter` flips that mapping.
- `tap_timing.cpp` builds the separate `tap-timer` helper binary. It reads raw console key events directly and is meant for measuring repeated tap timing and for the README demo that exercises `remap` end to end.

## Key conventions

- Keep the code Windows-native. This project uses Win32 console/ConPTY APIs, wide-string command-line parsing, and explicit UTF-8 conversion rather than cross-platform abstractions.
- Follow the existing RAII cleanup pattern for OS resources and console state. `UniqueHandle`, `UniquePseudoConsole`, `ConsoleModeGuard`, and `CodePageGuard` are the model for new resource-owning code.
- Preserve explicit error reporting. Failures are surfaced with `std::wcerr` and a non-zero exit code instead of being swallowed or hidden behind broad exception handling.
- Treat input processing as event-driven and key-down only. Key translation belongs in `EncodeKey`; timing logic for delayed Enter behavior belongs in the main loop, not in the output pump or process startup path.
- Child commands must be passed only after a literal `--`. Keep using `JoinCommandLine` and `QuoteIfNeeded` for Windows command-line reconstruction instead of manual string concatenation.
- Console mode and resize handling are part of the functional behavior, not setup noise. `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, raw input mode, UTF-8 code pages, and `ResizePseudoConsole` forwarding are all required for correct runtime behavior.
- `tap-timer` is the repo's built-in diagnostic companion for remapping changes. When adjusting Enter timing or encoded payloads, use it instead of adding ad hoc debug code to `remap`.
- If Windows Terminal has its own Enter or Shift+Enter bindings, `remap` will never see those keys. Keep that runtime prerequisite in mind before changing the remapping logic.
