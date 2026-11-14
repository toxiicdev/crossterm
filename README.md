# CrossTerm

CrossTerm is a cross-platform, all-in-one toolbox for managing remote servers: a tabbed terminal with SSH, SFTP, Telnet, Rlogin, Rsh, Mosh, serial, FTP/FTPS, VNC, RDP and WSL sessions, plus SSH tunnels, an integrated X server, remote monitoring and small network servers. It is a native C++17 / Qt Widgets application with no web engine.

The local terminal uses ConPTY on Windows and forkpty on Linux and macOS. Terminal emulation is handled by libvterm (revision 934bc2fbf21800ac3458a499df8820ca5fb45fd3, license in vendor/libvterm/LICENSE). The vendored copy carries local changes: four fixes in `src/screen.c` and `src/state.c` (resize of wrapped lines and of a shrunk scroll region, repeat of a combining character, no `abort()` on resize) and the faint attribute (SGR 2, `VTERM_ATTR_DIM`, in `include/vterm.h`, `src/pen.c`, `src/screen.c`, `src/vterm.c` and `src/vterm_internal.h`). Keep them when updating it. The encrypted sync file (`src/syncvault.cpp`) uses Monocypher 4.0.3, unmodified (XChaCha20-Poly1305 and Argon2id; BSD-2 or CC0, license in vendor/monocypher/LICENCE.md).

## Features

- **Sessions in tabs**: SSH, Telnet, Rlogin, Rsh, serial, Mosh (native, with local echo prediction), VNC (native RFB client), FTP/FTPS (built-in browser), RDP (embedded on Windows and Linux) and WSL. Split view with draggable separators and MultiExec.
- **SSH**: keys, agent, jump hosts, compression, keepalive, HTTP/SOCKS proxy (CrossTerm acts as ProxyCommand), X11 forwarding with the integrated X server (VcXsrv in `dist\xserver`, started on demand) and XDMCP.
- **Passwords**: stored in the system vault (Windows Credential Manager, macOS Keychain, Linux Secret Service), never in the sessions file or on the command line. OpenSSH receives them from CrossTerm acting as SSH_ASKPASS. Password and host-key prompts appear inside the SSH terminal, without dialogs; a password typed there once is kept in memory and reused by SFTP, remote monitoring, tunnels and Mosh, which connect on their own as soon as the terminal logs in.
- **Process lifetime**: every process CrossTerm starts (shells, ssh, sftp, tunnels, X server) ends with it, also after a crash.
- **SFTP browser**: upload, download, transfer queue with resume, drag and drop to and from the desktop, remote editing with automatic upload, recursive delete, Follow terminal folder.
- **Remote monitoring**: CPU, RAM, network, uptime, user and disk bar under the terminal.
- **SSH tunnels**: local (-L), remote (-R) and dynamic SOCKS (-D), optionally started with CrossTerm.
- **Tools**: built-in HTTP, TFTP and FTP/FTPS servers, port scanner, macros, favorites, session recovery, sessions opened at startup, configurable keyboard shortcuts.
- **Terminal**: reflow on resize, scrollback, search (Ctrl+Shift+F), output saving, automatic session logging, color schemes.
- **Appearance**: macOS or Classic, light, dark or following the system setting (default: macOS following the system), switched live from Settings > Configuration > Theme. Ctrl+mouse wheel zooms the terminal font.
- **30 languages** with live switching and right-to-left layouts. The window reopens at its last position and size and is fitted to the monitor work area.
- **Portable mode**: create an empty `CrossTerm.portable` file next to CrossTerm.exe, or start with `--portable`. Settings and sessions go to `CrossTerm-data`.

## Importing sessions

Sessions > Import reads session files from MobaXterm (MobaXterm.ini, .mxtsessions, .mobaconf), SecureCRT, Exceed, PuTTY, mRemoteNG, PuTTYCM, SuperPuTTY, WSL and CSV (name, host, port, user, protocol, folder). The importer shows a preview with duplicates, lets you choose Skip, Merge or Replace, saves a backup and offers "Undo last import". Macros, tunnels and preferences can be imported too. Source files are only read, never modified.

Passwords protected by the source application are not decrypted: CrossTerm imports the account user names and asks for each password once, then stores it in the system vault.

## Building

Requires Qt 6.5+, CMake 3.21+ and a C++17 compiler. Ready-made scripts in `scripts/` build in Release, run the tests and package the result; each one lists its dependencies at the top.

| Platform | Command | Output |
| --- | --- | --- |
| Windows (MSVC) | `powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Qt C:\Qt\6.8.3\msvc2022_64` | `dist\CrossTerm.exe` with the Qt DLLs (windeployqt); installer `build\CrossTerm-<version>-Windows-Setup.exe` when Inno Setup 6 is installed |
| Linux | `sh scripts/build-linux.sh` | `build/CrossTerm` |
| Linux (AppImage) | `sh scripts/build-appimage.sh` | `build/CrossTerm-<version>-x86_64.AppImage` with Qt bundled (linuxdeploy) |
| macOS | `sh scripts/build-macos.sh` | `build/CrossTerm.app` with Qt bundled (macdeployqt) and `build/CrossTerm-<version>-macOS.dmg` |

Release packages: pushing a tag `v<version>` (matching `project(VERSION)` in CMakeLists.txt) runs `.github/workflows/release.yml`, which builds the Windows installer (with VcXsrv) and the Linux AppImage and attaches them to a draft GitHub release; add the macOS `.dmg` built on a Mac, then publish the draft.

For the integrated X server on Windows, put the VcXsrv files in `dist\xserver` (GPL v3, see its license). Manual build:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The Linux and macOS builds have not been verified on those systems yet.

## Translations

Catalogs live in `i18n/*.json`. After changing user-visible strings run `python i18n/extract.py` and update the catalogs: the `profiles` test fails if a key is missing from any language.

## Tests

- Sources are in `src\`; tests in `tests\`.
- `tests\test.cpp` and `tests\test_*.cpp`: one executable per area, collected by CTest.
- `CrossTerm.exe --smoke-test`: local terminal, Tab and focus.
- `CrossTerm.exe --config <sessions.json> --e2e <folder>`: end-to-end run against a test SSH/SFTP server (first profile, key authentication). It saves screenshots and `e2e.txt` in the folder.

UI tests open the window off screen without activating it, so they never steal focus.

The status of each feature is in [FEATURES.md](FEATURES.md). Tested on Windows; Linux and macOS builds still need verification on those platforms.
