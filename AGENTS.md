# AGENTS.md

This file applies to the entire repository rooted at `C:\ddev\tmux4win\tmux`.

## Working Model

- Treat this as a flat, in-place tmux source tree. Most production code lives in the repository root as many small `*.c` and `*.h` files, not in `src/` or `include/`.
- Match the surrounding tmux/OpenBSD C style. Use tabs, keep function definitions and declarations in the existing house style, and prefer small focused changes over broad rewrites.
- Check `git status --short` before editing. This tree is often a live build workspace with local work in progress, especially around the Win32 port.

## Repo Map

- Root source clusters:
  `cmd-*.c` command handlers; `server-*.c`; `screen-*.c`; `tty-*.c`; `window-*.c`; `layout-*.c`; `grid*.c`; `input*.c`; `format*.c`; plus central files such as `tmux.c`, `tmux.h`, `client.c`, `cfg.c`, `proc.c`, `spawn.c`, and `job.c`.
- Protocol and process files:
  `tmux-protocol.h`, `proc.c`, `ipc-startup.c`, `server.c`, `server-client.c`, and `client.c` define the client/server protocol, server lifecycle, and client attach behavior.
- Portability layers:
  root `osdep-*.c` files for platform selection, `compat/` for replacement libc/system pieces, and the native Windows layer in `win32-*.c` plus `win32-platform.h`.
- Header overlay directories:
  `arpa/`, `netinet/`, and `sys/` are compatibility/header shims, not standalone subsystems.
- Support directories:
  `regress/` shell-based regression tests; `fuzz/` optional fuzz targets; `tools/` helper scripts and design notes; `.github/` contribution and CI metadata; `logo/` and `presentations/` non-code assets; `testcwd/` and `testcwd2/` fixture directories.

## Build And Generated Files

- This is an Autotools project. Canonical inputs are `configure.ac`, `Makefile.am`, and `autogen.sh`.
- Generated or build-produced files already live in-tree. Avoid editing them unless the task explicitly requires it:
  `configure`, `Makefile`, `Makefile.in`, `aclocal.m4`, `cmd-parse.c`, `tmux.1.man`, `tmux.1.mdoc`, `.deps/`, `autom4te.cache/`, `*.o`, `tmux.exe`, and `tmux-*.log`.
- `cmd-parse.c` is generated from `cmd-parse.y`. If parser behavior changes, edit `cmd-parse.y` first and regenerate only when the task calls for it.
- The build chooses `osdep-@PLATFORM@.c` and optional `compat/` pieces conditionally. Do not hardcode platform assumptions without checking `configure.ac` and `Makefile.am`.

## Build And Test Entry Points

- Release-style build from a prepared tree:
  `./configure && make`
- If Autotools inputs changed or the tree came from version control without fresh generated files:
  `sh autogen.sh`, then `./configure && make`
- On this Windows host, run build/configure commands only through MSYS2 bash with UCRT64 first in `PATH`. From PowerShell, the normal build command is:
  `C:\msys64\usr\bin\bash.exe -lc 'source /c/ddev/tmux4win/sysroot-env.sh; export PATH=/ucrt64/bin:/usr/bin:$PATH; cd /c/ddev/tmux4win/tmux; make -j4'`
- If configure must be rerun on this host, use the same MSYS2/UCRT64 environment:
  `C:\msys64\usr\bin\bash.exe -lc 'source /c/ddev/tmux4win/sysroot-env.sh; export PATH=/ucrt64/bin:/usr/bin:$PATH; cd /c/ddev/tmux4win/tmux; ./configure && make -j4'`
- Main regression harness:
  `cd regress && make`
  The harness is shell-driven and intentionally serialized by `regress/Makefile`.
- On this Windows host, run the regression harness through MSYS2 bash with the same environment:
  `C:\msys64\usr\bin\bash.exe -lc 'source /c/ddev/tmux4win/sysroot-env.sh; export PATH=/ucrt64/bin:/usr/bin:$PATH; cd /c/ddev/tmux4win/tmux/regress; make'`
- Optional fuzzing lives under `fuzz/` and is enabled through `--enable-fuzzing`.
- tmux debug logs are normally produced with `tmux -v` or `tmux -vv` in the working directory.

## Host Toolchain Policy

- Only use MSYS2 `bash` for build and configure commands on this host.
- Only use the MSYS2 UCRT64 toolchain for all compiling on this host.
- In MSYS2 `bash`, always prepend `/ucrt64/bin:/usr/bin` to `PATH` before building or compiling.
- Always use native PowerShell or `cmd` for runtime testing on this host.
- Never use any other host toolchain on this host for building, configuring, or compiling.

## Win32 Port

- Native Win32 support is selected by `configure.ac` for `*mingw*` hosts and wired into the build by `Makefile.am`. The main Windows platform boundary is `tmux.h` plus `win32-platform.h`.
- Core Win32 source roles:
  `win32-conpty.c` for pane/job process and ConPTY handling;
  `win32-event.c` for the shared Windows I/O backend;
  `win32-ipc.c` for AF_UNIX socket setup, startup locking, and handle duplication;
  `win32-proc.c` for detached server startup;
  `win32-terminal.c` for console initialization and resize;
  `win32-error.c` for UTF-8, environment, path, and misc Win32 helpers;
  `win32-glob.c` for glob support.
- Current terminal transport model matters:
  native console clients use the console relay path;
  redirected or non-console clients use duplicated direct handles;
  mixed relay/direct transport is intentionally rejected.
- Treat `TMUX_WIN32_HANDLE_TTY=force` as diagnostic only. Treat `TMUX_WIN32_CONSOLE_RELAY=0` as a test knob, not a product default.
- The detached server assumption is fundamental. `win32-proc.c` starts the server with `DETACHED_PROCESS`, so code that assumes the detached server can directly drive native console handles is usually wrong.

## Win32 Change Impact

- If a change touches `client.c`, `server-client.c`, `tmux-protocol.h`, or `win32-event.c`, also inspect:
  `tools/win32-console-relay-smoke.ps1`,
  `tools/win32-direct-handle-smoke.ps1`,
  `tools/win32-console-relay-plan.md`,
  `tools/win32-port-findings.md`.
- If a change touches `win32-conpty.c`, also inspect the call sites and lifecycle neighbors in:
  `spawn.c`, `job.c`, `input-keys.c`, `cmd-paste-buffer.c`, `server-fn.c`, `window.c`, and `cfg.c`.
- If a change touches `win32-ipc.c`, socket handling in `tmux.c`, or any `-S` / `$TMUX` path behavior, also review the security and path assumptions documented in `tools/win32-port-findings.md`.
- When changing Win32 protocol or transport behavior, keep the code, smoke coverage, and design notes aligned. This branch is actively documenting the Windows port as it evolves.

## Win32 Validation Notes

- `tools/win32-console-relay-smoke.ps1` and `tools/win32-console-direct-probe.ps1` require a real Windows console. Do not expect them to work under redirected CI or a non-interactive runner.
- `tools/win32-direct-handle-smoke.ps1` covers the redirected or non-console path.
- `tools/win32-console-relay-coverage.ps1` launches real-console relay smoke cases from a native PowerShell runner. Useful cases include:
  `baseline`, `input-credit`, `ctrl-j`, `utf8-split`, `invalid-utf8`, `detach-backlog`, `output-loss`, and `transport-loss`.
- Typical native PowerShell smoke commands:
  `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\win32-console-relay-coverage.ps1 -Cases baseline,input-credit,ctrl-j -RemoveArtifactsOnSuccess`
  `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\win32-direct-handle-smoke.ps1`
- `tools/win32-smoke.md` is the general manual MVP checklist.
- Toolchain and probe helpers live in:
  `tools/win32-build-sysroot.sh`,
  `tools/win32-build-probe.sh`,
  `tools/win32-conpty-probe.c`,
  `tools/win32-highlight-trace.md`.

## Practical Editing Guidance

- Prefer editing source and design inputs, not generated artifacts or transient logs.
- Because the tree is flat, even small feature work often spans multiple root files. Search broadly before assuming a single-file fix.
- Preserve platform splits. New Win32 behavior should usually live behind existing `#ifdef TMUX_WIN32` boundaries or in the dedicated Win32 source files rather than leaking Windows-specific code through unrelated paths.
