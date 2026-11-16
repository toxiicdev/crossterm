# Feature status

Statuses: **Done** (real code exercised by an automated test), **Partial** (works with stated limits), **Not supported** (outside supported mechanisms or not applicable to the platform). A feature is Done only if a test exercises it.

Tests: CTest (`profiles` plus one executable per area in `tests\`), `--smoke-test` and `--e2e`. The e2e run uses:
- a test SSH/SFTP server on 127.0.0.1:2222;
- a real Linux container (alpine, sshd, mosh-server, procps, xeyes) on 127.0.0.1:2223;
- the VcXsrv bundled in `dist\xserver`.

## Window and interface

| Feature | Status | Verification / notes |
| --- | --- | --- |
| Main layout (menu, toolbar, quick connect, sidebar, tabs) | Done | e2e |
| Last position, size and maximized state; fit to the work area | Done | `fitToArea` in CTest; e2e with a saved geometry larger than the screen; refit on `screenRemoved` |
| Dialogs always kept on screen | Done | Global filter on dialog Show events; e2e with a dialog larger than the screen |
| 30 languages, live switching, RTL | Done | CTest checks every catalog; e2e switches all 30 languages live |
| Translations | Partial | Generated, then reviewed for terminology, meaning and length by back-translation; automatic checks in CTest. Not a substitute for native speakers |
| Appearances: macOS and Classic, light/dark or following the system (default macOS) | Done | Settings > Configuration > Theme, live; `test_appearance` (all style sheets, contrast) and e2e |
| Terminal color schemes | Done | CrossTerm Dark/Light, Solarized Dark/Light |
| Configurable shortcuts | Done | Settings > Keyboard shortcuts: every menu action plus tab switching; conflicts rejected; reset; e2e |

## Terminal and tabs

| Feature | Status | Verification / notes |
| --- | --- | --- |
| Local terminal (ConPTY / forkpty) | Done | smoke test, e2e |
| Tabs, draggable Split, MultiExec | Done | e2e |
| Reflow on resize | Done | `test_terminal` (60 random resizes without loss); two libvterm defects worked around without patching it |
| Scrollback, selection, search, output saving and logging | Done | e2e |
| Macros, session recovery, favorites, sessions opened at startup | Done | e2e |
| Macros that include another macro (no loops) | Done | e2e |
| Paste confirmation: several lines, download piped to a shell, writes to startup files, archives extracted into /, look-alike letters, invisible characters; one dialog with MultiExec | Done | `test_pasteguard`, e2e |
| Close tabs to the left / right, close ended tabs | Done | e2e |
| Log lines with date and time in milliseconds (option) | Done | e2e |

## Protocols

| Feature | Status | Verification / notes |
| --- | --- | --- |
| SSH (keys, agent, jump hosts, compression, keepalive) | Done | e2e |
| Multi-line PAM prompts (judged by their last line), clickable links in prompt dialogs | Partial | Code path shared with the inline prompts; no PAM server in the tests |
| New password accepted after the saved one was rejected: asked before replacing it | Partial | Not covered by a test (needs a password server with interactive prompts) |
| SSH with password: system vault and in-memory session password | Done | CrossTerm askpass. A password typed once is reused by terminal, SFTP, monitoring, tunnels and Mosh; e2e |
| HTTP / SOCKS5 / SOCKS4(a) proxy for SSH | Done | CrossTerm acts as ProxyCommand; `test_services` runs real ssh to the container through all three proxy types |
| X11 forwarding with integrated X server | Done | VcXsrv in `dist\xserver` started on demand; `-Y` on Windows; e2e: `DISPLAY` set on the real Linux server |
| XDMCP | Partial | X server started in `-query`/`-broadcast` mode; not tried with a real display manager |
| Native Telnet (NAWS, TTYPE) | Done | `test_transports`; e2e against a local server |
| Native Rlogin | Done | `test_transports`; e2e. Qt cannot read the OOB urgent byte, so the window size is sent proactively |
| Serial port (Win32 / termios) | Partial | Port listing and error handling tested; no hardware for a live test |
| Native Mosh (SSP, AES-128-OCB) with local echo prediction | Done | RFC 7253 and FIPS-197 vectors; e2e against a real mosh-server 1.4. Adaptive/always/never prediction tested with a simulated high-latency server, not yet with a real mosh-server |
| Built-in VNC (RFB 3.3–3.8, Raw/CopyRect/RRE/Hextile/ZRLE) | Partial | `test_vnc` against a simulated RFB server (all encodings, authentication, keyboard, mouse, clipboard); not tried with a real VNC server |
| Built-in FTP/FTPS (browser in the tab) | Done | Explicit and implicit FTPS with certificate fingerprint confirmation; passive and active mode; REST/APPE resume; recursive folders. `test_ftp` with a generated certificate; e2e against the built-in server |
| Embedded RDP | Partial | Windows: Remote Desktop control (mstscax) in the tab, with full screen, Ctrl+Alt+Del and reconnect; Linux: embedded xfreerdp (not compiled here). Tested up to connection refusal: no real RDP server available |
| WSL | Done | WSL sessions and "Import WSL sessions" |
| Rsh | Done | Reserved local port, remote command, server errors; without a command it falls back to rlogin. `test_transports` and e2e with a simulated server |

## SFTP browser

| Feature | Status | Verification / notes |
| --- | --- | --- |
| List, navigate, upload, download, mkdir, rename, delete (recursive too) | Done | e2e |
| Queue with progress, cancel, resume | Done | e2e with 64 MB |
| Drag and drop from the desktop and to Explorer | Done | Upload: e2e. To Explorer: download on drop, tested with `test_sftpdrag`; a real drop in Explorer cannot be automated |
| Remote editing with upload on save, Follow terminal folder | Done | e2e |
| Column widths of the SFTP and FTP lists kept across restarts | Partial | Not covered by a test |
| Remote monitoring (CPU, RAM, network, uptime, user, disk) | Done | Bar under the terminal; e2e against the real Linux server |

## Sessions, credentials and import

| Feature | Status | Verification / notes |
| --- | --- | --- |
| MobaXterm import (sessions, folders, accounts) with preview, Skip/Merge/Replace, backup, undo | Done | CTest and e2e |
| MobaXterm macros, tunnels and preferences import | Done | Macros checked against a real exported file (WM_CHAR, Enter as RETURN, SLEEP pauses imported as waits); tunnel format has no public sample file; `test_mobaimport` and e2e |
| Import from SecureCRT, Exceed, PuTTY, mRemoteNG, PuTTYCM, SuperPuTTY, WSL, CSV | Done | CTest |
| Passwords saved by MobaXterm | Not supported | No supported mechanism to export them. CrossTerm asks for each password once and stores it in the vault |
| Vault: Windows | Done | CTest and e2e |
| Vault: macOS (Keychain) and Linux (Secret Service) | Partial | Written but not yet compiled or run on those systems |
| Portable mode, export and backup | Done | CTest; tried on a copy of `dist` |

## Other

| Feature | Status | Verification / notes |
| --- | --- | --- |
| Local, remote and dynamic SSH tunnels | Done | e2e with real traffic |
| Built-in servers: HTTP, TFTP, FTP/FTPS | Done | `test_servers` and `test_ftp` (root folder jail, Range, TFTP options, FTPS with a PEM certificate set in Configuration); Servers dialog in e2e |
| Port scanner | Done | `test_servers` |
| Wake On LAN | Done | `test_servers` (packet formats, UDP send) and e2e |
| Session tree: open SFTP with the SSH settings, copy/paste settings, settings for new sessions, collapse all, own icon per session | Done | e2e (open SFTP needs a server: not tested) |
| Duplicate SSH tunnel | Done | e2e |
| Cygwin packages / plugins | Not supported | Not applicable to a native Qt client |
