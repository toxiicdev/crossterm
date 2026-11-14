#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

// Extra data from a MobaXterm.ini beyond the sessions (which parseMoba imports).
struct MobaExtras
{
    // QSettings keys: terminal/font, terminal/size, terminal/scheme, terminal/rightPaste,
    // terminal/cursorShape ("block"|"underline"|"bar"), terminal/cursorBlink and
    // terminal/customColors {"foreground","background","cursor","palette":[16 "#rrggbb"]}.
    QJsonObject settings;
    QJsonArray macros;        // {"id","name","steps":[{"key","modifiers","text"}]}
    QJsonArray tunnels;       // {"id","name","type","listenPort","destHost","destPort","session","autostart"}
    QJsonArray tunnelServers; // SSH profiles to create for tunnels with no matching session
    QStringList warnings;
};

// Reads only [Font], [Colors], [Cursor], [Misc] (RightClickAction), [Macros] and [PortForwarding]:
// sections containing secrets are never read.
MobaExtras parseMobaExtras(const QByteArray &ini, const QJsonArray &importedProfiles);
