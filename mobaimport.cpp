#include "mobaimport.h"
#include <QColor>
#include <QHash>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUuid>

// Formats observed in MobaXterm 21.3 files (see report): [PortForwarding] "NNNN.name=Type;user@server:port;
// dest:port;local port;autostart;key;adapter;proxy;autoreconnect"; [Macros] "name=step|step",
// step "msg:wparam:lparam:text" (12 = text, 256/257 = WM_KEYDOWN/UP, 258 = WM_CHAR, 0 = SLEEP=... commands).

using Section = QList<QPair<QString, QString>>;

static QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

static QHash<QString, Section> readSections(const QByteArray &bytes)
{
    // Only the useful sections: [Credentials], [Passwords] etc. are not even stored.
    static const QStringList wanted{"font", "colors", "cursor", "misc", "macros", "portforwarding"};
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8.decode(bytes);
    if (utf8.hasError())
        text = QString::fromLatin1(bytes);
    QHash<QString, Section> sections;
    QString current;
    for (const auto &raw : text.split(QRegularExpression("[\r\n]+")))
    {
        const auto line = raw.trimmed();
        if (line.startsWith('[') && line.endsWith(']'))
        {
            current = line.mid(1, line.size() - 2).trimmed().toLower();
            continue;
        }
        const int eq = line.indexOf('=');
        if (wanted.contains(current) && !line.isEmpty() && !line.startsWith(';'))
            sections[current].append({eq > 0 ? line.left(eq).trimmed() : QString{}, eq > 0 ? line.mid(eq + 1) : line});
    }
    return sections;
}

static QString value(const Section &section, const QString &key)
{
    for (const auto &[k, v] : section)
    {
        if (k.compare(key, Qt::CaseInsensitive) == 0)
            return v.trimmed();
    }
    return {};
}

static QColor rgb(const QString &text)
{
    const auto parts = text.split(',');
    bool r = false, g = false, b = false;
    if (parts.size() != 3)
        return {};
    const QColor color(parts[0].toInt(&r), parts[1].toInt(&g), parts[2].toInt(&b));
    return r && g && b && color.isValid() ? color : QColor{};
}

static int distance(const QColor &a, const QColor &b)
{
    const int dr = a.red() - b.red(), dg = a.green() - b.green(), db = a.blue() - b.blue();
    return dr * dr + dg * dg + db * db;
}

static void readTerminal(const QHash<QString, Section> &s, MobaExtras &out)
{
    const auto &font = s.value("font");
    // Recent versions use the "2" suffix (Font2, FontHeight2).
    auto family = value(font, "Font2");
    if (family.isEmpty())
        family = value(font, "Font");
    auto size = value(font, "FontHeight2");
    if (size.isEmpty())
        size = value(font, "FontHeight");
    if (family.compare("MobaFont", Qt::CaseInsensitive) == 0)
        out.warnings.append("Il font MobaFont è interno a MobaXterm: resta il font attuale.");
    else if (!family.isEmpty())
        out.settings["terminal/font"] = family;
    if (size.toInt() > 0)
        out.settings["terminal/size"] = size.toInt();

    const auto &colors = s.value("colors");
    static const QStringList names{"Black", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White"};
    QJsonArray palette;
    for (const auto &prefix : {QString(), QString("Bold")})
    {
        for (const auto &name : names)
        {
            const auto color = rgb(value(colors, prefix + name));
            if (color.isValid())
                palette.append(color.name());
        }
    }
    const auto fg = rgb(value(colors, "ForegroundColour"));
    const auto bg = rgb(value(colors, "BackgroundColour"));
    const auto cursor = rgb(value(colors, "CursorColour"));
    if (fg.isValid() && bg.isValid())
    {
        struct Scheme
        {
            const char *name;
            QColor bg, fg;
        };
        static const Scheme schemes[]{{"CrossTerm Dark", QColor("#1c1c1c"), QColor("#dadada")},
                                      {"CrossTerm Light", QColor("#ffffff"), QColor("#1e1e1e")},
                                      {"Solarized Dark", QColor("#002b36"), QColor("#839496")},
                                      {"Solarized Light", QColor("#fdf6e3"), QColor("#657b83")}};
        // The background counts double: it decides whether the scheme is light or dark.
        const Scheme *best = &schemes[0];
        for (const auto &scheme : schemes)
        {
            if (2 * distance(scheme.bg, bg) + distance(scheme.fg, fg) < 2 * distance(best->bg, bg) + distance(best->fg, fg))
                best = &scheme;
        }
        out.settings["terminal/scheme"] = best->name;
        QJsonObject custom{{"foreground", fg.name()}, {"background", bg.name()}};
        if (cursor.isValid())
            custom["cursor"] = cursor.name();
        if (palette.size() == 16)
            custom["palette"] = palette;
        out.settings["terminal/customColors"] = custom;
    }
    else if (!colors.isEmpty())
        out.warnings.append("Colori MobaXterm non riconosciuti: schema non importato.");

    const auto &cur = s.value("cursor");
    static const QStringList shapes{"block", "underline", "bar"};
    const auto type = value(cur, "CursorType");
    if (!type.isEmpty() && type.toInt() >= 0 && type.toInt() < 3)
        out.settings["terminal/cursorShape"] = shapes[type.toInt()];
    const auto blink = value(cur, "CursorBlinks");
    if (!blink.isEmpty())
        out.settings["terminal/cursorBlink"] = blink != "0";
    // RightClickAction: 1 = paste, 0 = context menu.
    const auto right = value(s.value("misc"), "RightClickAction");
    if (!right.isEmpty())
        out.settings["terminal/rightPaste"] = right == "1";
}

static QString unescape(QString text)
{
    static const QList<QPair<QString, QString>> codes{{"__DIEZE__", "#"},   {"__EQQUAL__", "="}, {"__PTVIRG__", ";"},
                                                      {"__DBLQUO__", "\""}, {"__PIIPE__", "|"},  {"__DBLDOT__", ":"}};
    for (const auto &[code, plain] : codes)
        text.replace(code, plain);
    return text;
}

static void readMacros(const Section &section, MobaExtras &out)
{
    // Windows virtual keys with no printable text.
    static const QHash<int, int> keys{{8, Qt::Key_Backspace}, {9, Qt::Key_Tab},       {13, Qt::Key_Return}, {27, Qt::Key_Escape},
                                      {33, Qt::Key_PageUp},   {34, Qt::Key_PageDown}, {35, Qt::Key_End},    {36, Qt::Key_Home},
                                      {37, Qt::Key_Left},     {38, Qt::Key_Up},       {39, Qt::Key_Right},  {40, Qt::Key_Down},
                                      {45, Qt::Key_Insert},   {46, Qt::Key_Delete}};
    for (const auto &[name, raw] : section)
    {
        QJsonArray steps;
        int modifiers = 0, skipped = 0;
        bool valid = !name.isEmpty();
        for (auto step : raw.trimmed().split('|', Qt::SkipEmptyParts))
        {
            // In the macro format, EQUAL and PIPE are placeholders for "=" and "|" within a single step.
            step.replace("EQUAL", "=").replace("PIPE", "|");
            const auto f = step.split(':');
            bool numeric = false;
            const int message = f.value(0).toInt(&numeric);
            const int code = f.value(1).toInt();
            if (f.size() < 4 || !numeric)
            {
                valid = false;
                break;
            }
            const int modifier = code == 16   ? Qt::ShiftModifier
                                 : code == 17 ? Qt::ControlModifier
                                 : code == 18 ? Qt::AltModifier
                                              : 0;
            if (message == 12)
                steps.append(QJsonObject{{"key", 0}, {"modifiers", 0}, {"text", unescape(f[3])}});
            else if ((message == 256 || message == 260) && modifier)
                modifiers |= modifier;
            else if ((message == 257 || message == 261) && modifier)
                modifiers &= ~modifier;
            else if (message == 256 || message == 260)
            {
                // Printable keys without Ctrl/Alt already arrive as WM_CHAR: skip the duplicate.
                const bool letter = (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9');
                if (keys.contains(code) || (code >= 112 && code <= 123))
                    steps.append(
                        QJsonObject{{"key", keys.value(code, Qt::Key_F1 + code - 112)}, {"modifiers", modifiers}, {"text", ""}});
                else if (letter && (modifiers & (Qt::ControlModifier | Qt::AltModifier)))
                    steps.append(QJsonObject{{"key", code}, {"modifiers", modifiers}, {"text", ""}});
            }
            else if (message == 258)
            {
                if (code >= 32)
                    steps.append(QJsonObject{{"key", 0}, {"modifiers", 0}, {"text", QString(QChar(code))}});
                else if (code > 0)
                {
                    // In actual MobaXterm.ini files Enter is a WM_CHAR "258:13:...:RETURN" with no WM_KEYDOWN:
                    // generate the key, unless the matching WM_KEYDOWN already produced it.
                    const int key = keys.value(code, 0);
                    const auto last = steps.isEmpty() ? QJsonObject{} : steps.last().toObject();
                    if (key && !(last["key"].toInt() == key && last["text"].toString().isEmpty()))
                        steps.append(QJsonObject{{"key", key}, {"modifiers", modifiers}, {"text", ""}});
                    else if (!key && code <= 26) // Ctrl+letter
                        steps.append(
                            QJsonObject{{"key", Qt::Key_A + code - 1}, {"modifiers", int(Qt::ControlModifier)}, {"text", ""}});
                }
            }
            else if (message == 0 && f[3].trimmed().startsWith("SLEEP=", Qt::CaseInsensitive))
                steps.append(QJsonObject{{"sleep", f[3].trimmed().mid(6).toInt()}}); // pause in milliseconds
            else if (message != 257 && message != 261)
                ++skipped; // WAITFOR=, VAR= and unknown messages
        }
        if (!valid || steps.isEmpty())
        {
            out.warnings.append("Macro MobaXterm non riconosciuta (forse cifrata): " + name);
            continue;
        }
        if (skipped)
            out.warnings.append("Macro \"" + name + "\": alcuni passi (attese o comandi speciali) non sono stati importati.");
        out.macros.append(QJsonObject{{"id", uuid()}, {"name", name}, {"steps", steps}});
    }
}

static bool hostPort(const QString &text, QString &host, int &port)
{
    const int colon = text.lastIndexOf(':');
    host = text.left(colon).trimmed();
    if (host.startsWith('[') && host.endsWith(']'))
        host = host.mid(1, host.size() - 2);
    bool ok = false;
    port = text.mid(colon + 1).trimmed().toInt(&ok);
    return colon > 0 && ok && port > 0 && port < 65536 && !host.isEmpty();
}

static bool flag(const QString &text)
{
    return text == "-1" || text == "1" || text.compare("true", Qt::CaseInsensitive) == 0;
}

static void readTunnels(const Section &section, const QJsonArray &profiles, MobaExtras &out)
{
    static const QRegularExpression index("^\\d{4}\\.");
    for (const auto &[key, raw] : section)
    {
        const auto name = index.match(key).hasMatch() ? key.mid(5) : key;
        const auto f = raw.split(';');
        const auto type = f.value(0).trimmed().toLower();
        QString server, destHost;
        int serverPort = 0, destPort = 0;
        bool ok = false;
        const int listen = f.value(3).trimmed().toInt(&ok);
        const auto login = f.value(1).trimmed();
        const int at = login.lastIndexOf('@');
        if (f.size() < 5 || name.isEmpty() || (type != "local" && type != "remote" && type != "dynamic") || !ok || listen < 1 ||
            listen > 65535 || !hostPort(login.mid(at + 1), server, serverPort) ||
            (type != "dynamic" && !hostPort(f.value(2).trimmed(), destHost, destPort)))
        {
            out.warnings.append("Tunnel MobaXterm non riconosciuto: " + name);
            continue;
        }
        auto user = at > 0 ? login.left(at).trimmed() : QString{};
        // <defaultuser> and [credential] do not carry a usable username.
        const bool anyUser = user.isEmpty() || user.startsWith('[') || user.startsWith('<');
        if (anyUser)
            user.clear();
        auto same = [&](const QJsonObject &p)
        {
            return p["protocol"].toString() == "ssh" && p["host"].toString().compare(server, Qt::CaseInsensitive) == 0 &&
                   p["port"].toInt() == serverPort && (anyUser || p["user"].toString() == user);
        };
        QString session;
        for (const auto &list : {profiles, out.tunnelServers})
        {
            for (const auto &p : list)
            {
                if (session.isEmpty() && same(p.toObject()))
                    session = p.toObject()["id"].toString();
            }
        }
        if (session.isEmpty())
        {
            session = uuid();
            auto identity = f.value(5).trimmed();
            if (identity.startsWith("No SSH key", Qt::CaseInsensitive))
                identity.clear();
            out.tunnelServers.append(QJsonObject{{"id", session},
                                                 {"name", (user.isEmpty() ? "" : user + "@") + server},
                                                 {"folder", "Tunnel MobaXterm"},
                                                 {"protocol", "ssh"},
                                                 {"host", server},
                                                 {"port", serverPort},
                                                 {"user", user},
                                                 {"identity", identity},
                                                 {"jumps", QJsonArray{}},
                                                 {"issues", QJsonArray{}}});
        }
        const auto proxy = f.value(7).trimmed();
        if (!proxy.isEmpty() && !proxy.startsWith("No proxy", Qt::CaseInsensitive) && proxy.compare("None", Qt::CaseInsensitive))
            out.warnings.append("Tunnel \"" + name + "\": il proxy non è stato importato.");
        out.tunnels.append(QJsonObject{{"id", uuid()},
                                       {"name", name},
                                       {"type", type},
                                       {"listenPort", listen},
                                       {"destHost", destHost},
                                       {"destPort", destPort},
                                       {"session", session},
                                       {"autostart", flag(f.value(4).trimmed())}});
    }
}

MobaExtras parseMobaExtras(const QByteArray &ini, const QJsonArray &importedProfiles)
{
    MobaExtras out;
    const auto sections = readSections(ini);
    readTerminal(sections, out);
    readMacros(sections.value("macros"), out);
    readTunnels(sections.value("portforwarding"), importedProfiles, out);
    return out;
}
