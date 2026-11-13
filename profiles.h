#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QStringList>

struct Import
{
    QJsonArray profiles;
    QStringList warnings;
    QStringList passwordProfiles; // IDs only, never encrypted values
    int accounts = 0;             // named credentials: only the username is imported
    int secrets = 0;              // MobaXterm-protected passwords: neither imported nor decrypted
};

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
