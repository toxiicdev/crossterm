// Import from other clients: SecureCRT, Exceed, PuTTY,
// mRemoteNG, PuTTY Connection Manager, SuperPuTTY and WSL distributions. All sources are opened
// read-only and no password is ever imported.
#include "profiles.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>

namespace
{
QString protocolName(QString value)
{
    value = value.trimmed().toLower();
    if (value.startsWith("ssh") || value == "putty")
        return "ssh";
    if (value.startsWith("telnet"))
        return "telnet";
    if (value.startsWith("rlogin"))
        return "rlogin";
    if (value.startsWith("rdp"))
        return "rdp";
    if (value.startsWith("vnc"))
        return "vnc";
    if (value.startsWith("sftp"))
        return "sftp";
    if (value.startsWith("ftp"))
        return "ftp";
    if (value.startsWith("serial"))
        return "serial";
    return value;
}

QJsonObject makeProfile(const QString &name, const QString &folder, const QString &protocol, const QString &host, int port,
                        const QString &user)
{
    const int effective = port > 0 ? port : defaultPort(protocol);
    QJsonArray issues;
    if (!defaultPort(protocol))
        issues.append("Protocollo ancora da implementare.");
    else if (!validHost(host) || effective < 1 || effective > 65535)
        issues.append("Indirizzo o porta da verificare.");
    return QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                       {"name", name.isEmpty() ? host : name},
                       {"folder", folder},
                       {"protocol", protocol},
                       {"host", host},
                       {"port", effective},
                       {"user", user},
                       {"identity", QString()},
                       {"jumps", QJsonArray{}},
                       {"compression", false},
                       {"x11", false},
                       {"issues", issues}};
}

int dword(const QString &value)
{
    bool ok = false;
    const auto text = value.trimmed().section(':', -1);
    const int number = text.toInt(&ok, 16);
    return ok ? number : 0;
}

QString baseName(const QString &file)
{
    return QFileInfo(file).completeBaseName();
}

// SecureCRT: .ini session file with lines S:"Key"=value / D:"Key"=hex.
Import secureCrtIni(const QString &text, const QString &file)
{
    Import result;
    QHash<QString, QString> values;
    static const QRegularExpression line("^([SDB]):\"([^\"]+)\"=(.*)$");
    for (const auto &row : text.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts))
    {
        const auto m = line.match(row.trimmed());
        if (m.hasMatch())
            values.insert(m.captured(2).toLower(), m.captured(1) == "D" ? QString::number(dword(m.captured(3))) : m.captured(3));
    }
    const auto protocol = protocolName(values.value("protocol name", "ssh2"));
    int port = values.value("[ssh2] port").toInt();
    if (!port)
        port = values.value("port").toInt();
    result.profiles.append(
        makeProfile(baseName(file), "SecureCRT", protocol, values.value("hostname"), port, values.value("username")));
    return result;
}

// Exceed Xstart (.xs): [Xstart] section with Host, Method and UserID.
Import exceed(const QString &text, const QString &file)
{
    Import result;
    QString host, method, user;
    bool inside = false;
    for (const auto &row : text.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts))
    {
        const auto t = row.trimmed();
        if (t.startsWith('['))
        {
            inside = t.compare("[Xstart]", Qt::CaseInsensitive) == 0;
            continue;
        }
        if (!inside)
            continue;
        const auto key = t.section('=', 0, 0).trimmed().toLower(), value = t.section('=', 1).trimmed();
        if (key == "host")
            host = value;
        else if (key == "method")
            method = value;
        else if (key == "userid")
            user = value;
    }
    if (host.isEmpty())
        result.warnings << "Una voce di sessione ha un formato non riconosciuto.";
    else
        result.profiles.append(
            makeProfile(baseName(file), "Exceed", protocolName(method.isEmpty() ? "ssh" : method), host, 0, user));
    return result;
}

// PuTTY .reg export: [HKEY_CURRENT_USER\Software\SimonTatham\PuTTY\Sessions\<name>].
Import puttyReg(const QString &text)
{
    Import result;
    QString name;
    QHash<QString, QString> values;
    auto flush = [&]
    {
        if (!name.isEmpty() && !values.value("hostname").isEmpty())
            result.profiles.append(makeProfile(QUrl::fromPercentEncoding(name.toUtf8()), "PuTTY",
                                               protocolName(values.value("protocol", "ssh")), values.value("hostname"),
                                               values.value("portnumber").toInt(), values.value("username")));
        name.clear();
        values.clear();
    };
    static const QRegularExpression section("^\\[HKEY_[^\\]]*\\\\SimonTatham\\\\PuTTY\\\\Sessions\\\\([^\\]]+)\\]$",
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression entry("^\"([^\"]+)\"=(?:\"(.*)\"|dword:([0-9a-fA-F]+))$");
    for (const auto &row : text.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts))
    {
        const auto t = row.trimmed();
        if (t.startsWith('['))
        {
            flush();
            const auto m = section.match(t);
            if (m.hasMatch())
                name = m.captured(1);
            continue;
        }
        const auto m = entry.match(t);
        if (m.hasMatch() && !name.isEmpty())
            values.insert(m.captured(1).toLower(),
                          m.captured(3).isEmpty() ? m.captured(2) : QString::number(m.captured(3).toInt(nullptr, 16)));
    }
    flush();
    return result;
}

// XML: mRemoteNG (Node), PuTTY Connection Manager (connection_info), SuperPuTTY (SessionData), SecureCRT (VanDyke).
Import xml(const QByteArray &data)
{
    Import result;
    QXmlStreamReader xml(data);
    QStringList containers;       // open mRemoteNG folders
    QList<bool> nodeIsContainer;  // to close nested Nodes correctly
    QHash<QString, QString> info; // PuTTYCM <connection_info> fields
    QString field;
    QStringList crtKeys; // path of SecureCRT <key> elements
    QHash<QString, QString> crt;
    auto crtFlush = [&]
    {
        if (!crt.value("hostname").isEmpty())
            result.profiles.append(makeProfile(crtKeys.isEmpty() ? QString() : crtKeys.last(),
                                               crtKeys.size() > 2 ? crtKeys.mid(1, crtKeys.size() - 2).join('\\') : "SecureCRT",
                                               protocolName(crt.value("protocol name", "ssh2")), crt.value("hostname"),
                                               crt.value("[ssh2] port", crt.value("port")).toInt(), crt.value("username")));
        crt.clear();
    };
    while (!xml.atEnd())
    {
        xml.readNext();
        const auto name = xml.name().toString();
        if (xml.isStartElement())
        {
            const auto a = xml.attributes();
            if (name == "Connections" && a.value("Protection").size() && a.value("FullFileEncryption") == QLatin1String("true"))
                result.warnings << "Una voce di sessione ha un formato non riconosciuto.";
            if (name == "Node")
            {
                const bool container = a.value("Type") == QLatin1String("Container");
                nodeIsContainer << container;
                if (container)
                    containers << a.value("Name").toString();
                else
                    result.profiles.append(makeProfile(
                        a.value("Name").toString(), containers.join('\\'), protocolName(a.value("Protocol").toString()),
                        a.value("Hostname").toString(), a.value("Port").toInt(), a.value("Username").toString()));
            }
            else if (name == "SessionData")
            {
                const auto id = a.value("SessionId").toString();
                const auto title = a.value("SessionName").toString();
                result.profiles.append(makeProfile(title.isEmpty() ? id.section('/', -1) : title,
                                                   id.section('/', 0, -2).replace('/', '\\'),
                                                   protocolName(a.value("Proto").toString()), a.value("Host").toString(),
                                                   a.value("Port").toInt(), a.value("Username").toString()));
            }
            else if (name == "connection_info" || name == "login")
                field.clear();
            else if (name == "key")
                crtKeys << a.value("name").toString();
            else if ((name == "string" || name == "dword") && !crtKeys.isEmpty())
            {
                const auto key = a.value("name").toString().toLower();
                const auto value = xml.readElementText();
                crt.insert(key, name == "dword" ? QString::number(value.toInt()) : value);
            }
            else
                field = name;
        }
        else if (xml.isCharacters() && !field.isEmpty() && !xml.isWhitespace())
            info.insert(field, xml.text().toString().trimmed());
        else if (xml.isEndElement())
        {
            if (name == "Node" && !nodeIsContainer.isEmpty() && nodeIsContainer.takeLast() && !containers.isEmpty())
                containers.removeLast();
            else if (name == "connection")
            {
                if (!info.value("hostname").isEmpty())
                    result.profiles.append(makeProfile(info.value("name"), "PuTTYCM", protocolName(info.value("protocol", "ssh")),
                                                       info.value("hostname"), info.value("port").toInt(), info.value("login")));
                info.clear();
            }
            else if (name == "key")
            {
                crtFlush();
                if (!crtKeys.isEmpty())
                    crtKeys.removeLast();
            }
            field.clear();
        }
    }
    if (xml.hasError() && result.profiles.isEmpty())
        result.warnings << "Una voce di sessione ha un formato non riconosciuto.";
    return result;
}
} // namespace

Import readForeign(const QByteArray &data, const QString &fileName)
{
    const auto lower = fileName.toLower();
    const auto text = QString::fromUtf8(
        data.startsWith("\xff\xfe")
            ? QString::fromUtf16(reinterpret_cast<const char16_t *>(data.constData() + 2), (data.size() - 2) / 2).toUtf8()
            : data);
    Import result;
    if (lower.endsWith(".csv"))
        result = parseCsv(data);
    else if (text.contains("[Bookmarks", Qt::CaseInsensitive))
        result = parseMoba(data, fileName.mid(1, 1) == ":" ? fileName.left(1) : QString());
    else if (text.contains("\\SimonTatham\\PuTTY\\Sessions\\", Qt::CaseInsensitive))
        result = puttyReg(text);
    else if (text.contains(QRegularExpression("^[SD]:\"Hostname\"=", QRegularExpression::MultilineOption)))
        result = secureCrtIni(text, fileName);
    else if (text.contains("[Xstart]", Qt::CaseInsensitive) || lower.endsWith(".xs"))
        result = exceed(text, fileName);
    else if (text.trimmed().startsWith('<'))
        result = xml(text.toUtf8());
    else
        result = parseMoba(data, fileName.mid(1, 1) == ":" ? fileName.left(1) : QString());
    if (result.profiles.isEmpty() && result.warnings.isEmpty())
        result.warnings << "Nessuna sessione MobaXterm riconosciuta.";
    return result;
}

Import localSessions(const QString &source)
{
    Import result;
    if (source == "PuTTY")
    {
#ifdef Q_OS_WIN
        // PuTTY registry, read-only.
        QSettings registry("HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions", QSettings::NativeFormat);
        for (const auto &group : registry.childGroups())
        {
            registry.beginGroup(group);
            const auto host = registry.value("HostName").toString();
            if (!host.isEmpty())
                result.profiles.append(makeProfile(QUrl::fromPercentEncoding(group.toUtf8()), "PuTTY",
                                                   protocolName(registry.value("Protocol", "ssh").toString()), host,
                                                   registry.value("PortNumber").toInt(), registry.value("UserName").toString()));
            registry.endGroup();
        }
#else
        // PuTTY for Unix: one file per session in ~/.putty/sessions with key=value lines.
        const QDir dir(QDir::homePath() + "/.putty/sessions");
        for (const auto &file : dir.entryList(QDir::Files))
        {
            QFile f(dir.filePath(file));
            if (!f.open(QIODevice::ReadOnly))
                continue;
            QHash<QString, QString> values;
            for (const auto &row : QString::fromUtf8(f.readAll()).split('\n'))
                values.insert(row.section('=', 0, 0).toLower(), row.section('=', 1));
            if (!values.value("hostname").isEmpty())
                result.profiles.append(makeProfile(QUrl::fromPercentEncoding(file.toUtf8()), "PuTTY",
                                                   protocolName(values.value("protocol", "ssh")), values.value("hostname"),
                                                   values.value("portnumber").toInt(), values.value("username")));
        }
#endif
    }
    else if (source == "WSL")
    {
        // "wsl -l -q" lists distributions in UTF-16; Docker Desktops internal ones are excluded.
        QProcess wsl;
        wsl.start("wsl.exe", {"-l", "-q"});
        if (wsl.waitForFinished(10000) && wsl.exitCode() == 0)
        {
            const auto raw = wsl.readAllStandardOutput();
            const auto list = QString::fromUtf16(reinterpret_cast<const char16_t *>(raw.constData()), raw.size() / 2);
            for (auto name : list.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts))
            {
                name = name.remove(QChar(0)).trimmed();
                if (name.isEmpty() || name.startsWith("docker-desktop"))
                    continue;
                auto profile = makeProfile(name, "WSL", "wsl", name, 0, QString());
                profile["issues"] = QJsonArray{};
                result.profiles.append(profile);
            }
        }
    }
    if (result.profiles.isEmpty())
        result.warnings << "Nessuna sessione MobaXterm riconosciuta.";
    return result;
}
