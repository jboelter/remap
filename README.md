# remap

> Launch a terminal app with session-scoped Enter key remapping, without changing Windows Terminal globally.

`remap` is a small C++ wrapper that runs **inside Windows Terminal**, launches a child command behind an **inner ConPTY**, and remaps `Enter` / `Shift+Enter` only for that one session. An optional double-tap Enter override is also available for tools that need both "submit" and "newline" behaviors.

Use it when you want a tool such as `copilot` to treat Enter and Shift+Enter behavior differently, but you do **not** want to change your global Windows Terminal keybindings for every tab, shell, or command.

The `remap` tool sits between the outer session and the child process and rewrites the keys you care about.

## Disclaimer

This is 100% AI generated code. It is not intended for production use, and may contain bugs, security issues, or other problems. Use at your own risk. Pull requests to fix issues are welcome, but may also be AI reviewed.

## Important

- Remove or disable any existing Windows Terminal keybindings for `Enter` or `Shift+Enter` that send input or override the default behavior.
- If Windows Terminal handles those keys first, `remap` will never receive them, so the remapping logic will not run.

## Why you would use it

- keep custom Enter behavior scoped to a single launched command
- preserve normal Windows Terminal rendering, scrollback, panes, and UX
- experiment with plain Enter vs. Shift+Enter behavior without changing global terminal settings
- optionally enable a double-tap Enter override for tools that need both "submit" and "newline" behaviors
- use the companion `tap-timer` tool to measure or simulate repeated key timing

## What it does

- requires an explicit child command after `--`
- creates an inner ConPTY for the child
- forwards child output back to the outer terminal
- captures console input in the proxy process
- remaps:
  - physical `Enter`
  - physical `Shift+Enter`

## Current presets

- **default / `--swap-enter`**
  - `Enter -> ESC CR`
  - `Shift+Enter -> CR`

- **`--standard-enter`**
  - `Enter -> CR`
  - `Shift+Enter -> ESC CR`

## Build

From a normal Windows shell with Visual Studio 2022 C++ tools installed:

```shell
cmake -S . -B build\windows\x64 --fresh -G "Visual Studio 17 2022" -A x64
cmake --build build\windows\x64 --config Debug
```

That places the x64 binaries under:

```text
build\windows\x64\debug\
```

For ARM64:

```shell
cmake -S . -B build\windows\arm64 --fresh -G "Visual Studio 17 2022" -A ARM64
cmake --build build\windows\arm64 --config Debug
```

Or use the helper script from a normal command prompt:

```shell
.\build.bat
```

Optional build type:

```shell
.\build.bat Release
```

Or choose the architecture explicitly:

```shell
.\build.bat Debug x64
.\build.bat Release arm64
```

Local builds embed the version tag `dev` by default. To stamp a specific version into the binaries, pass it through CMake:

```shell
cmake -S . -B build\windows\x64 --fresh -G "Visual Studio 17 2022" -A x64 "-DREMAP_VERSION_TAG=v1.2.3"
cmake --build build\windows\x64 --config Release
```

Or use the helper script:

```shell
.\build.bat Release x64 "v1.2.3"
```

That places the binaries under:

```text
build\windows\<x64|arm64>\<debug|release|relwithdebinfo|minsizerel>\
```

## Usage

Run the proxy with an explicit child command:

```shell
remap.exe -- copilot
```

Pick the alternate mapping preset:

```shell
remap.exe --standard-enter -- copilot
```

Enable the double-tap Enter override with the default 150ms window:

```shell
remap.exe --double-tap -- copilot
```

Or tune the double-tap window explicitly:

```shell
remap.exe --double-tap --delay 150 -- copilot
```

Launch a different child command:

```shell
remap.exe -- child-command arg1 arg2
```

Or set a working directory:

```shell
remap.exe --cwd D:\repo -- copilot
```

Show the embedded version:

```shell
remap.exe --version
tap-timer.exe --version
```

## Releases

Pull requests and pushes to `main` run the build, validation, and packaging matrix and upload the `.zip` files as workflow artifacts.

Pushing a version tag on `main` publishes a GitHub Release through GitHub Actions.

```shell
git tag v1.2.3
git push origin v1.2.3
```

Tags with a hyphen publish prereleases automatically:

```shell
git tag v1.2.3-rc1
git push origin v1.2.3-rc1
```

Each release publishes Windows archives for `x64|arm64` and `Debug|Release` using this naming scheme:

```text
remap-<tag>-windows-<x64|arm64>-<debug|release>.zip
```

Each archive contains `remap.exe`, `tap-timer.exe`, `remap.pdb`, and `tap-timer.pdb`. The binaries are built with the MSVC runtime linked statically, so they do not require the VC++ redistributable. The Windows `Comments` field stores the full git commit hash used for the build. Debug archives are included intentionally for troubleshooting, and release archives keep symbols instead of stripping them.

## Demo

Run the remap tool with the `tap-timer` child to see how it captures and rewrites Enter keys:

```shell
> .\build\windows\<arch>\debug\remap.exe --double-tap --delay 200 -- .\build\windows\<arch>\debug\tap-timer.exe
tap-timer: press keys to measure repeated taps.
Output format:
  <key>
  <key> <key> <ms>
Press Esc to exit.

Alt+VK(18)       # single tap Enter
Alt+Enter        
Enter            # single tap Shift+Enter
Enter            # double tap Enter
Esc
```
