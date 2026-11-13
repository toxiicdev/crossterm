# CrossTerm

CrossTerm is a cross-platform, all-in-one toolbox for managing remote servers: a tabbed terminal with SSH, SFTP, Telnet, Rlogin, Rsh, Mosh, serial, FTP/FTPS, VNC, RDP and WSL sessions, plus SSH tunnels, an integrated X server, remote monitoring and small network servers. It is a native C++17 / Qt Widgets application with no web engine.

The local terminal uses ConPTY on Windows and forkpty on Linux and macOS. Terminal emulation is handled by libvterm (revision 934bc2fbf21800ac3458a499df8820ca5fb45fd3, license in vendor/libvterm/LICENSE).

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

Requires Qt 6.5+, CMake 3.21+ and a C++17 compiler.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Translations

Catalogs live in `i18n/*.json`. After changing user-visible strings run `python i18n/extract.py` and update the catalogs: the `profiles` test fails if a key is missing from any language.

## Tests

- `tests\test_*.cpp`: one executable per area, collected by CTest.
- `CrossTerm.exe --smoke-test`: local terminal, Tab and focus.
- `CrossTerm.exe --config <sessions.json> --e2e <folder>`: end-to-end run against a test SSH/SFTP server (first profile, key authentication). It saves screenshots and `e2e.txt` in the folder.

UI tests open the window off screen without activating it, so they never steal focus.

The status of each feature is in [FEATURES.md](FEATURES.md). Tested on Windows; Linux and macOS builds still need verification on those platforms.
