#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QHash>
#include <QProcess>
#include <QStringList>
#include <optional>

struct Import
{
    QJsonArray profiles;
    QStringList warnings;
    QStringList passwordProfiles; // IDs of sessions with a saved MobaXterm password
    // Profile ID -> password as MobaXterm stores it (encrypted with the master password). Kept in memory only:
    // decrypted with the master password the user types, then written to the system vault, never to a file.
    QHash<QString, QString> encryptedPasswords;
    int accounts = 0; // named credentials ([Credentials])
    int secrets = 0;  // MobaXterm-protected passwords
};

// MobaXterm master-password encryption (AES-256-CFB8, key = SHA-512 of the master password).
// decrypt returns nullopt when the result is not text, i.e. the master password is wrong.
std::optional<QString> decryptMobaPassword(const QString &base64, const QString &master);
QString encryptMobaPassword(const QString &plain, const QString &master);          // tests
QByteArray aes256EncryptBlock(const QByteArray &key32, const QByteArray &block16); // tests (FIPS-197 vector)

// Background console programs (ssh, sftp) started from the GUI: on Windows they would get a console window that
// flashes on screen. They get a console of their own that is never shown: unlike CREATE_NO_WINDOW (no console at
// all), the programs they start in turn (sftp -> ssh) inherit it instead of opening a visible one.
// Call before start(); no effect on other platforms.
QProcess *hideConsoleWindow(QProcess *process);
// Programs and documents opened for the user (browser, editor, "Open with"): they stay open when CrossTerm closes or
// crashes, unlike its own children, which die with it (Windows: outside the kill-on-close job; Unix: own session,
// out of the process group that the crash handler kills).
// native: Windows only, appended to the command line as it is (no quoting).
bool startForUser(const QString &program, const QStringList &arguments, const QString &native = {});
bool openForUser(const class QUrl &url); // a URL or a local file, with the system's default program
// Path quoted for an sftp batch line ("./" before a leading dash); empty when it holds control characters.
QString sftpQuote(QString text);

// "Hidden shell" mode (Windows): CrossTerm.exe started as %COMSPEC% with CROSSTERM_HIDDEN_SHELL set runs
// "cmd.exe <same arguments>" inside an invisible pseudoconsole and returns its exit code.
bool isHiddenShellInvocation(int argc);
int runHiddenShell();

enum class ImportPolicy
{
    Skip,
    Merge,
    Replace
};

struct Command
{
    QString program;
    QStringList arguments;
    QString error;
    QStringList environment; // extra "NAME=value" variables for the process (never secrets)
};

Import parseMoba(const QByteArray &bytes, const QString &drive = {});
QJsonArray mergeProfiles(QJsonArray current, const QJsonArray &incoming);
Command sessionCommand(const QJsonObject &profile, const QString &platform, bool display);
// Error to show when the ssh/sftp program of a terminal session ends with exit code code (-1 = killed by a signal):
// "Could not connect to host", "Connection closed by the server" or "Connection to host lost", chosen from ssh's last
// lines on screen; empty for a normal end (any other code is the status of the remote shell).
QString sshExitError(int code, const QString &screen, const QString &host);
bool validHost(const QString &host);
int defaultPort(const QString &protocol);

struct RemoteEntry
{
    QString name, modified, owner, group, access;
    qint64 size = 0;
    bool dir = false, link = false;
};
// One line of the sftp clients "ls -la"; false for lines that are not entries (including . and ..).
bool parseListing(const QString &line, RemoteEntry &entry);

// Skip: adds only new sessions (same name, folder and protocol = existing).
// Merge: same as mergeProfiles. Replace: replaces existing sessions, keeping their id.
QJsonArray applyImport(QJsonArray current, const QJsonArray &incoming, ImportPolicy policy);
// Resizes and moves a window rectangle so that it fits entirely within the available area.
QRect fitToArea(QRect window, const QRect &area);
// Localized text of a warning or error produced by parseMoba/sessionCommand.
QString describe(const QString &message);
// SSH tunnel: {"type": "local"|"remote"|"dynamic", "listenPort", "destHost", "destPort"} on the given SSH profile.
Command tunnelCommand(const QJsonObject &tunnel, const QJsonObject &profile, const QString &platform);
// CSV import with a header row (name, protocol, host, port, user, folder; any order, "," or ";" separator).
Import parseCsv(const QByteArray &bytes);
// Portable mode: data folder next to the executable if the CrossTerm.portable file or the --portable option is present.
QString portableRoot(const QString &executableDir, const QStringList &arguments);
// Import from other clients (SecureCRT, Exceed, PuTTY .reg, mRemoteNG, PuTTYCM, SuperPuTTY, CSV, MobaXterm),
// detected from the file content and name.
Import readForeign(const QByteArray &data, const QString &fileName);
// Sessions found on this computer: "PuTTY" (registry or ~/.putty) and "WSL" (installed distributions).
Import localSessions(const QString &source);
