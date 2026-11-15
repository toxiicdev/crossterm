#pragma once
// Icons: system icon font glyphs where available, otherwise drawn at runtime with QPainter.
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScreen>
#include <QtMath>

// Modern icons from the system icon font (Windows 11: Segoe Fluent Icons, Windows 10: Segoe MDL2 Assets),
// one glyph and accent colour per name. Names without a glyph, or systems without the font, use the drawings below.
// Painted in logical pixels: the device pixel ratio of the target scales the glyph.
static bool fluentIcon(const QString &name, int size, QPainter &p)
{
    static const QHash<QString, std::pair<char16_t, const char *>> glyphs{
        {"session", {0xE7F4, "#3b8eea"}},  {"servers", {0xE968, "#29b6f6"}},   {"tools", {0xE90F, "#f06a5a"}},
        {"sessions", {0xE734, "#f5c518"}}, {"star", {0xE735, "#f5c518"}},      {"view", {0xE890, "#4fc3a1"}},
        {"split", {0xE8A9, "#5c9ded"}},    {"multiexec", {0xE762, "#9a7cf0"}}, {"tunneling", {0xE71B, "#36c08f"}},
        {"packages", {0xE7B8, "#5fa8ff"}}, {"settings", {0xE713, "#5f9bf5"}},  {"help", {0xE897, "#3b8eea"}},
        {"xserver", {0xE770, "#e5534b"}},  {"exit", {0xE7E8, "#e5534b"}},      {"macros", {0xE768, "#f39a1e"}},
        {"sftp", {0xE774, "#e8a33d"}},     {"monitor", {0xE9D9, "#3cb44a"}},   {"user", {0xE77B, "#d9a35b"}},
        {"home", {0xE80F, "#e5534b"}},     {"plus", {0xE710, "#848484"}},      {"ok", {0xE73E, "#3cb44a"}},
        {"refresh", {0xE72C, "#3cb44a"}},  {"parent", {0xE74A, "#5c9ded"}},    {"download", {0xE896, "#2f7fe0"}},
        {"upload", {0xE898, "#1fb5a8"}},   {"delete", {0xE74D, "#e5534b"}},    {"rename", {0xE8AC, "#9a7cf0"}},
        {"folder", {0xE8D5, "#f5c518"}},   {"newfolder", {0xE8F4, "#f5c518"}}, {"file", {0xE8A5, "#b8c0cc"}},
        {"newfile", {0xE8A5, "#5fa8ff"}},  {"open", {0xE8E5, "#5fa8ff"}},      {"edit", {0xE70F, "#4fc3a1"}},
        {"openwith", {0xE7AC, "#9a7cf0"}}, {"copy", {0xE8C8, "#b8c0cc"}},      {"terminal", {0xE756, "#4fc3a1"}},
        {"info", {0xE946, "#3b8eea"}},     {"lock", {0xE72E, "#d9a35b"}},      {"ssh", {0xE756, "#f5c518"}},
        {"shell", {0xE756, "#4fc3a1"}},    {"telnet", {0xE756, "#5fa8ff"}},    {"rlogin", {0xE756, "#9a7cf0"}},
        {"rsh", {0xE756, "#9a7cf0"}},      {"mosh", {0xE756, "#36c08f"}},      {"wsl", {0xE756, "#f39a1e"}},
        {"rdp", {0xE8AF, "#3b8eea"}},      {"vnc", {0xE7F4, "#36c08f"}},       {"ftp", {0xE8B7, "#f5c518"}},
        {"serial", {0xE88E, "#b8c0cc"}},   {"back", {0xE72B, "#5c9ded"}},      {"forward", {0xE72A, "#5c9ded"}},
        {"eye", {0xE7B3, "#4fc3a1"}}};
    static const QString family = []
    {
        for (const char *f : {"Segoe Fluent Icons", "Segoe MDL2 Assets"})
            if (QFontDatabase::hasFamily(f))
                return QString(f);
        return QString();
    }();
    const auto it = glyphs.find(name);
    if (family.isEmpty() || it == glyphs.end())
        return false;
    QFont font(family);
    font.setPixelSize(qRound(size * 0.78));
    const QChar glyph(it->first);
    if (!QFontMetrics(font).inFont(glyph))
        return false;
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(font);
    p.setPen(QColor(it->second));
    p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, QString(glyph));
    return true;
}

// Drawings on a 32 x 32 grid; the colours do not depend on the appearance.
static void paintIcon(QPainter &p, const QString &name)
{
    const QColor yellow("#ffcc00"), blue("#2f8fe8"), green("#3cb44a"), red("#e8432e"), grey("#9aa4ad"), white("#f2f2f2"),
        dark("#2a2a2a"), orange("#f39a1e"), purple("#8a5cf0");
    auto monitor = [&](QColor screen)
    {
        p.setPen(QPen(grey, 1.5));
        p.setBrush(dark);
        p.drawRoundedRect(QRectF(3, 4, 26, 19), 2, 2);
        p.setPen(Qt::NoPen);
        p.setBrush(screen);
        p.drawRect(QRectF(6, 7, 20, 13));
        p.setBrush(grey);
        p.drawRect(QRectF(13, 23, 6, 3));
        p.drawRect(QRectF(9, 26, 14, 2));
    };
    auto star = [&](QPointF c, double r, QColor fill)
    {
        QPolygonF poly;
        for (int i = 0; i < 10; ++i)
        {
            const double a = -M_PI / 2 + i * M_PI / 5, rr = i % 2 ? r * 0.45 : r;
            poly << c + QPointF(rr * qCos(a), rr * qSin(a));
        }
        p.setPen(QPen(fill.darker(140), 1));
        p.setBrush(fill);
        p.drawPolygon(poly);
    };
    auto folder = [&](QColor fill)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(fill.darker(115));
        p.drawRoundedRect(QRectF(3, 6, 12, 6), 1.5, 1.5);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(3, 9, 26, 18), 2, 2);
    };
    auto arrow = [&](QColor c, bool up, double x = 16)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        if (up)
            p.drawPolygon(QPolygonF{{x, 4}, {x + 9, 14}, {x + 3, 14}, {x + 3, 26}, {x - 3, 26}, {x - 3, 14}, {x - 9, 14}});
        else
            p.drawPolygon(QPolygonF{{x, 28}, {x + 9, 18}, {x + 3, 18}, {x + 3, 5}, {x - 3, 5}, {x - 3, 18}, {x - 9, 18}});
    };
    auto text = [&](const QString &t, QColor c, int px, QRectF r = QRectF(0, 0, 32, 32))
    {
#ifdef Q_OS_WIN
        QFont f("Segoe UI");
#else
        QFont f = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
#endif
        f.setPixelSize(px);
        f.setBold(true);
        p.setFont(f);
        p.setPen(c);
        p.drawText(r, Qt::AlignCenter, t);
    };
    if (name == "session")
    {
        monitor(QColor("#1b3a5c"));
        p.setPen(Qt::NoPen);
        p.setBrush(red);
        p.drawEllipse(QPointF(13, 12), 3.5, 3.5);
        p.setBrush(green);
        p.drawEllipse(QPointF(19, 12), 3.5, 3.5);
        p.setBrush(blue.lighter(130));
        p.drawEllipse(QPointF(16, 16.5), 3.5, 3.5);
    }
    else if (name == "servers")
    {
        p.setPen(QPen(blue, 2.2));
        for (int i = 0; i < 6; ++i)
        {
            const double a = i * M_PI / 3;
            p.drawLine(QPointF(16, 16), QPointF(16 + 11 * qCos(a), 16 + 11 * qSin(a)));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#38c8f0"));
        for (int i = 0; i < 6; ++i)
        {
            const double a = i * M_PI / 3;
            p.drawEllipse(QPointF(16 + 11 * qCos(a), 16 + 11 * qSin(a)), 3, 3);
        }
        p.setBrush(red);
        p.drawEllipse(QPointF(16, 16), 4, 4);
    }
    else if (name == "tools")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(red);
        p.drawRoundedRect(QRectF(11, 10, 10, 19), 3, 3);
        p.setBrush(grey);
        p.drawPolygon(QPolygonF{{13, 10}, {16, 2}, {19, 10}});
        p.setBrush(white);
        p.drawRect(QRectF(15, 15, 2, 7));
        p.drawRect(QRectF(12.5, 17.5, 7, 2));
    }
    else if (name == "sessions" || name == "star")
    {
        star({16, 16.5}, 13, yellow);
    }
    else if (name == "view")
    {
        monitor(QColor("#1d4d2b"));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#ffd2a6"));
        p.drawEllipse(QPointF(16, 11), 3, 3);
        p.setBrush(green);
        p.drawRoundedRect(QRectF(11, 14.5, 10, 5.5), 2, 2);
    }
    else if (name == "split")
    {
        monitor(QColor("#14324f"));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#4aa8ff"));
        p.drawRect(QRectF(7, 8, 8.5, 5));
        p.drawRect(QRectF(16.5, 8, 8.5, 5));
        p.drawRect(QRectF(7, 14, 8.5, 5));
        p.drawRect(QRectF(16.5, 14, 8.5, 5));
    }
    else if (name == "multiexec")
    {
        p.setPen(QPen(QColor("#4a64d8"), 4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(16, 28), QPointF(16, 16));
        p.drawLine(QPointF(16, 16), QPointF(7, 6));
        p.drawLine(QPointF(16, 16), QPointF(25, 6));
    }
    else if (name == "tunneling")
    {
        monitor(QColor("#1d4d2b"));
        p.setPen(QPen(white, 1.6));
        p.drawLine(QPointF(8, 12), QPointF(24, 12));
        p.drawLine(QPointF(8, 16), QPointF(24, 16));
        p.setPen(Qt::NoPen);
        p.setBrush(white);
        p.drawPolygon(QPolygonF{{24, 12}, {20, 9.5}, {20, 14.5}});
        p.drawPolygon(QPolygonF{{8, 16}, {12, 13.5}, {12, 18.5}});
        p.setBrush(red);
        p.drawRect(QRectF(3, 28, 26, 2));
    }
    else if (name == "packages")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#8fa9c4"));
        p.drawRoundedRect(QRectF(4, 14, 24, 14), 3, 3);
        p.setBrush(QColor("#6f8aa6"));
        p.drawRect(QRectF(4, 20, 24, 2));
        arrow(blue, false);
    }
    else if (name == "settings")
    {
        auto gear = [&](QPointF c, double r)
        {
            p.setPen(QPen(blue, r * 0.55, Qt::DotLine));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, r, r);
            p.setPen(Qt::NoPen);
            p.setBrush(blue);
            p.drawEllipse(c, r * 0.8, r * 0.8);
            p.setCompositionMode(QPainter::CompositionMode_Clear); // hole: transparent on every background
            p.drawEllipse(c, r * 0.35, r * 0.35);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        };
        gear({12, 20}, 8);
        gear({23, 9}, 5.5);
    }
    else if (name == "help")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#1e90ff"));
        p.drawEllipse(QRectF(3, 3, 26, 26));
        text("?", white, 20);
    }
    else if (name == "xserver")
    {
        p.setPen(QPen(QColor("#e83e3e"), 5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(7, 6), QPointF(25, 26));
        p.setPen(QPen(green, 5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(25, 6), QPointF(16, 16));
        p.setPen(QPen(blue, 5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(16, 16), QPointF(7, 26));
    }
    else if (name == "exit")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#e8382c"));
        p.drawEllipse(QRectF(3, 3, 26, 26));
        p.setPen(QPen(white, 2.6, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(9.5, 9.5, 13, 13), 120 * 16, 300 * 16);
        p.drawLine(QPointF(16, 7.5), QPointF(16, 15));
    }
    else if (name == "macros")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#7d93a8"));
        p.drawPolygon(QPolygonF{{3, 15}, {29, 4}, {22, 28}, {15, 20}});
        p.setBrush(QColor("#5b6f82"));
        p.drawPolygon(QPolygonF{{15, 20}, {29, 4}, {13, 27}});
    }
    else if (name == "sftp")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(orange);
        p.drawEllipse(QRectF(3, 3, 26, 26));
        p.setPen(QPen(QColor("#fff2c0"), 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(10, 3, 12, 26));
        p.drawLine(QPointF(3, 16), QPointF(29, 16));
        p.drawLine(QPointF(5, 10), QPointF(27, 10));
        p.drawLine(QPointF(5, 22), QPointF(27, 22));
    }
    else if (name == "folder")
    {
        folder(QColor("#f2c12e"));
    }
    else if (name == "back" || name == "forward")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(blue);
        QPolygonF shape{{4, 16}, {15, 5}, {15, 12}, {28, 12}, {28, 20}, {15, 20}, {15, 27}};
        if (name == "forward")
            for (auto &point : shape)
                point.setX(32 - point.x());
        p.drawPolygon(shape);
    }
    else if (name == "eye")
    {
        QPainterPath lid;
        lid.moveTo(2, 16);
        lid.quadTo(16, 2, 30, 16);
        lid.quadTo(16, 30, 2, 16);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#4fc3a1"));
        p.drawPath(lid);
        p.setBrush(white);
        p.drawEllipse(QPointF(16, 16), 6, 6);
        p.setBrush(QColor("#1d4d2b"));
        p.drawEllipse(QPointF(16, 16), 3.2, 3.2);
    }
    else if (name == "parent")
    {
        folder(QColor("#f2c12e"));
        arrow(blue, true, 17);
    }
    else if (name == "download")
    {
        arrow(QColor("#3b7fd9"), false);
        p.setBrush(QColor("#3b7fd9"));
        p.drawRect(QRectF(5, 28, 22, 2.5));
    }
    else if (name == "upload")
    {
        arrow(QColor("#2ea89a"), true);
        p.setBrush(QColor("#2ea89a"));
        p.drawRect(QRectF(5, 1.5, 22, 2.5));
    }
    else if (name == "refresh")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(green);
        p.drawEllipse(QRectF(3, 3, 26, 26));
        p.setPen(QPen(white, 2.6, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(9, 9, 14, 14), 60 * 16, 290 * 16);
        p.setPen(Qt::NoPen);
        p.setBrush(white);
        p.drawPolygon(QPolygonF{{19, 5}, {25, 10}, {18, 12}});
    }
    else if (name == "newfolder")
    {
        folder(QColor("#f2c12e"));
        p.setBrush(green);
        p.drawEllipse(QRectF(17, 17, 13, 13));
        p.setPen(QPen(white, 2));
        p.drawLine(QPointF(23.5, 19.5), QPointF(23.5, 27.5));
        p.drawLine(QPointF(19.5, 23.5), QPointF(27.5, 23.5));
    }
    else if (name == "newfile" || name == "file")
    {
        p.setPen(QPen(QColor("#8fb8e8"), 1));
        p.setBrush(name == "file" ? white : QColor("#cfe3fa"));
        p.drawPolygon(QPolygonF{{7, 3}, {20, 3}, {26, 9}, {26, 29}, {7, 29}});
        if (name == "file")
        {
            p.setPen(QPen(QColor("#9aa4ad"), 1.2));
            for (int y = 12; y < 27; y += 4)
                p.drawLine(QPointF(10, y), QPointF(23, y));
        }
    }
    else if (name == "delete")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#b83a3a"));
        p.drawEllipse(QRectF(3, 3, 26, 26));
        p.setPen(QPen(QColor("#ffd0d0"), 2.8, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(11, 11), QPointF(21, 21));
        p.drawLine(QPointF(21, 11), QPointF(11, 21));
    }
    else if (name == "rename")
    {
        text("A", purple, 28);
    }
    else if (name == "ok")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#38a845"));
        p.drawEllipse(QRectF(2, 2, 28, 28));
        p.setPen(QPen(white, 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(QPolygonF{{9, 16.5}, {14, 21.5}, {23, 11}});
    }
    else if (name == "home")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#e84a2e"));
        p.drawPolygon(QPolygonF{{16, 3}, {30, 16}, {2, 16}});
        p.setBrush(white);
        p.drawRect(QRectF(7, 15, 18, 13));
        p.setBrush(QColor("#e84a2e"));
        p.drawRect(QRectF(13.5, 19, 5, 9));
    }
    else if (name == "plus")
    {
        p.setPen(QPen(QColor("#848484"), 4)); // gray: at least 3:1 on the button of every appearance
        p.drawLine(QPointF(16, 6), QPointF(16, 26));
        p.drawLine(QPointF(6, 16), QPointF(26, 16));
    }
    else if (name == "user")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#8fb3dc"));
        p.drawRoundedRect(QRectF(2, 4, 28, 24), 3, 3);
        p.setBrush(QColor("#ffd2a6"));
        p.drawEllipse(QPointF(16, 13), 5, 5);
        p.setBrush(QColor("#3a6ea8"));
        p.drawRoundedRect(QRectF(8, 19, 16, 9), 4, 4);
    }
    else if (name == "monitor")
    {
        monitor(QColor("#0f3a2a"));
        p.setPen(QPen(QColor("#3cf07a"), 1.6));
        p.drawPolyline(QPolygonF{{6, 15}, {11, 15}, {13, 9}, {16, 19}, {19, 12}, {21, 15}, {26, 15}});
    }
    else
    { // protocols: key or initial on a tile
        const QMap<QString, QColor> colors{{"ssh", yellow}, {"sftp", orange}, {"telnet", QColor("#5ac8fa")},
                                           {"rdp", blue},   {"vnc", green},   {"ftp", QColor("#c08040")},
                                           {"shell", grey}};
        p.setPen(QPen(QColor("#b0b0b0"), 1));
        p.setBrush(dark);
        p.drawRect(QRectF(2.5, 2.5, 27, 27));
        if (name == "ssh")
        {
            p.setPen(QPen(yellow, 3, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(11, 12), 4.5, 4.5);
            p.drawLine(QPointF(14.5, 15.5), QPointF(25, 26));
            p.drawLine(QPointF(21, 22), QPointF(24, 19));
            p.drawLine(QPointF(23.5, 24.5), QPointF(26, 22));
        }
        else
            text(name.left(1).toUpper(), colors.value(name, grey), 18);
    }
}

// Device pixel ratios every icon is drawn at: 1x, 2x, 3x and the exact ratio of each screen (fractional scaling).
static QList<qreal> iconScales()
{
    QList<qreal> scales{1, 2, 3};
    for (const QScreen *screen : QGuiApplication::screens())
        if (const qreal ratio = screen->devicePixelRatio(); !scales.contains(ratio))
            scales << ratio;
    return scales;
}

// One pixmap per device pixel ratio, each drawn at its real resolution: sharp on high-DPI screens.
static QIcon drawIcon(const QString &name, int size)
{
    QIcon result;
    for (const qreal scale : iconScales())
    {
        const int pixels = qRound(size * scale);
        QPixmap pix(pixels, pixels);
        pix.setDevicePixelRatio(qreal(pixels) / size);
        pix.fill(Qt::transparent);
        QPainter p(&pix);
        if (!fluentIcon(name, size, p))
        {
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(size / 32.0, size / 32.0);
            paintIcon(p, name);
        }
        p.end();
        result.addPixmap(pix);
    }
    return result;
}

// Drawn once per name and size: session trees and remote file lists ask for the same icons on every row.
static QIcon icon(const QString &name, int size = 32)
{
    static QHash<std::pair<QString, int>, QIcon> cache;
    const std::pair<QString, int> key{name, size};
    if (const auto it = cache.constFind(key); it != cache.constEnd())
        return *it;
    return *cache.insert(key, drawIcon(name, size));
}

// Remote file or folder icon as the user's own system shows it (Windows shell, freedesktop theme, macOS): by
// extension, the system's generic file or folder otherwise. Asked each time, so a new file association or icon
// theme shows at the next listing (the system caches them itself); our drawn icons only if it has none.
static QIcon fileIcon(const QString &name, bool dir = false)
{
    const QFileIconProvider provider;
    const auto suffix = QFileInfo(name).suffix().toLower();
    auto shown = dir || suffix.isEmpty() ? provider.icon(dir ? QFileIconProvider::Folder : QFileIconProvider::File)
                                         : provider.icon(QFileInfo("file." + suffix)); // the file need not exist
    return shown.isNull() ? icon(dir ? "folder" : "file") : shown;
}

// Icon with a dimmed "off" state (side stripe, matching inactive tabs), drawn at the 34px of the side strip.
static QIcon toggleIcon(const QString &name, int size = 34)
{
    const QIcon base = icon(name, size);
    QIcon result;
    for (const qreal scale : iconScales())
    {
        const auto on = base.pixmap(QSize(size, size), scale);
        QPixmap off(on.size());
        off.setDevicePixelRatio(on.devicePixelRatio());
        off.fill(Qt::transparent);
        QPainter p(&off);
        p.setOpacity(0.35);
        p.drawPixmap(0, 0, on);
        p.end();
        result.addPixmap(off, QIcon::Normal, QIcon::Off);
        result.addPixmap(on, QIcon::Normal, QIcon::On);
        result.addPixmap(on, QIcon::Active, QIcon::On);
    }
    return result;
}
