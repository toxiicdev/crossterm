// Selectable UI appearances: the default one (dark/light) and macOS (light/dark).
// Header-only: stylesheet, base colors, macOS-style tab painting.
#pragma once
#include <QColor>
#include <QDir>
#include <QImage>
#include <QFontMetrics>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QPainter>
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <array>

enum class Appearance
{
    ClassicDark,
    ClassicLight,
    MacLight,
    MacDark
};

inline bool appearanceIsMac(Appearance a)
{
    return a == Appearance::MacLight || a == Appearance::MacDark;
}

inline bool appearanceIsDark(Appearance a)
{
    return a == Appearance::ClassicDark || a == Appearance::MacDark;
}

// Keys saved in QSettings("theme"): "dark"/"light" stay compatible with existing settings.
inline QString appearanceKey(Appearance a)
{
    switch (a)
    {
    case Appearance::ClassicLight:
        return "light";
    case Appearance::MacLight:
        return "mac-light";
    case Appearance::MacDark:
        return "mac-dark";
    default:
        return "dark";
    }
}

// "system" and "mac-system" follow the OS color scheme; missing or unknown keys use "mac-system".
inline Appearance appearanceFromKey(const QString &key, bool systemDark)
{
    for (auto a : {Appearance::ClassicDark, Appearance::ClassicLight, Appearance::MacLight, Appearance::MacDark})
        if (key == appearanceKey(a))
            return a;
    if (key == "system")
        return systemDark ? Appearance::ClassicDark : Appearance::ClassicLight;
    return systemDark ? Appearance::MacDark : Appearance::MacLight;
}

// English names; main.cpp passes them to the translation system.
inline QString appearanceName(Appearance a)
{
    switch (a)
    {
    case Appearance::ClassicLight:
        return "Classic light";
    case Appearance::MacLight:
        return "macOS light";
    case Appearance::MacDark:
        return "macOS dark";
    default:
        return "Classic dark";
    }
}

struct AppearancePalette
{
    QColor window, sidebar, text, secondaryText, accent, separator, terminalBackground;
    int toolbarIconSize;
    bool textUnderIcon;
};

inline AppearancePalette appearancePalette(Appearance a)
{
    switch (a)
    {
    case Appearance::ClassicLight:
        return {"#f3f3f3", "#f3f3f3", "#1e1e1e", "#6a6a6a", "#3d8fe0", "#d0d0d0", "#ffffff", 28, true};
    case Appearance::MacLight:
        return {"#ececec", "#e3e3e8", "#1d1d1f", "#6e6e73", "#007aff", "#d1d1d6", "#ffffff", 28, true};
    case Appearance::MacDark:
        return {"#1e1e1e", "#262628", "#e5e5e7", "#98989d", "#0a84ff", "#38383a", "#1e1e1e", 28, true};
    default:
        return {"#141414", "#141414", "#e6e6e6", "#8a8a8a", "#3d8fe0", "#3a3a3a", "#000000", 28, true};
    }
}

// Converts the colors of a style written for the default dark appearance (main sheet, inline styles, tab painting)
// into the colors of the chosen appearance. For ClassicLight this is the same conversion as themed() in main.cpp.
inline QString appearanceColors(Appearance a, const QString &classicDark)
{
    static const QHash<QString, QString> classicLight{
        {"#141414", "#f3f3f3"}, {"#1c1c1c", "#ffffff"}, {"#1f1f1f", "#ffffff"}, {"#e6e6e6", "#1e1e1e"}, {"#2f4f6f", "#cce4f7"},
        {"#ffffff", "#000000"}, {"#323232", "#e3e3e3"}, {"#2a2a2a", "#e8e8e8"}, {"#4a4a4a", "#c4c4c4"}, {"#3a3a3a", "#d0d0d0"},
        {"#6a6a6a", "#a0a0a0"}, {"#262626", "#e6e6e6"}, {"#253545", "#cce4f7"}, {"#2a3a4a", "#e5f1fb"}, {"#3d5a78", "#7eb4ea"},
        {"#33414f", "#e5f1fb"}, {"#555", "#b0b0b0"},    {"#222a33", "#eef5fb"}, {"#9fda9f", "#2e7d32"}, {"#8fc0ff", "#1f5fa8"},
        {"#8a8a8a", "#6a6a6a"}, {"#fefefe", "#1e1e1e"}, {"#9a9a9a", "#808080"}, {"#2f5f8f", "#99c9ef"}, {"#232323", "#ececec"},
        {"#3d73a8", "#7eb4ea"}, {"#f0f0f0", "#202020"}, {"#b0b0b0", "#505050"}, {"#c0c0c0", "#505050"}, {"#1e1e1e", "#f0f0f0"}};
    // For macOS only the colors used by inline styles and by the default tabs are converted.
    static const QHash<QString, QString> macLight{
        {"#141414", "#ececec"}, {"#1c1c1c", "#ffffff"}, {"#1f1f1f", "#f6f6f6"}, {"#e6e6e6", "#1d1d1f"}, {"#2f4f6f", "#007aff"},
        {"#ffffff", "#000000"}, {"#2a2a2a", "#ffffff"}, {"#4a4a4a", "#d1d1d6"}, {"#3a3a3a", "#d1d1d6"}, {"#6a6a6a", "#c7c7cc"},
        {"#f0f0f0", "#1d1d1f"}, {"#b0b0b0", "#6e6e73"}, {"#c0c0c0", "#6e6e73"}, {"#8a8a8a", "#6e6e73"}, {"#9fda9f", "#248a3d"},
        {"#8fc0ff", "#007aff"}, {"#3d8fe0", "#007aff"}};
    static const QHash<QString, QString> macDark{
        {"#141414", "#1e1e1e"}, {"#1c1c1c", "#1e1e1e"}, {"#1f1f1f", "#2c2c2e"}, {"#e6e6e6", "#e5e5e7"},
        {"#2f4f6f", "#0a84ff"}, {"#2a2a2a", "#3a3a3c"}, {"#4a4a4a", "#38383a"}, {"#3a3a3a", "#38383a"},
        {"#6a6a6a", "#4a4a4c"}, {"#f0f0f0", "#e5e5e7"}, {"#b0b0b0", "#98989d"}, {"#c0c0c0", "#98989d"},
        {"#8a8a8a", "#98989d"}, {"#9fda9f", "#30d158"}, {"#8fc0ff", "#0a84ff"}, {"#3d8fe0", "#0a84ff"}};
    if (a == Appearance::ClassicDark)
        return classicDark;
    const auto &map = a == Appearance::ClassicLight ? classicLight : a == Appearance::MacLight ? macLight : macDark;
    static const QRegularExpression hex("#[0-9a-fA-F]{6}\\b|#[0-9a-fA-F]{3}\\b");
    QString result;
    int last = 0;
    for (auto it = hex.globalMatch(classicDark); it.hasNext();)
    {
        const auto m = it.next();
        result += classicDark.mid(last, m.capturedStart() - last) + map.value(m.captured().toLower(), m.captured());
        last = int(m.capturedEnd());
    }
    return result + classicDark.mid(last);
}

// Indicators (arrows, check mark) drawn at runtime into the temp folder: Qt stylesheets do not accept inline images.
// Drawn at twice the displayed size, so they stay sharp at 200% scaling.
inline QString macIndicatorDir(bool dark)
{
    const QString dir = QDir::temp().filePath(dark ? "crossterm-appearance-dark" : "crossterm-appearance-light");
    QDir().mkpath(dir);
    const QColor arrow(dark ? "#c7c7cc" : "#4a4a4f");
    auto save = [&](const QString &name, QSize size, const QColor &color, const QList<QPolygonF> &lines)
    {
        QImage image(size * 2, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(2, 2);
        p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const auto &line : lines)
            p.drawPolyline(line);
        p.end();
        image.save(dir + "/" + name);
    };
    save("up.png", {8, 5}, arrow, {QPolygonF{{1, 4}, {4, 1}, {7, 4}}});
    save("down.png", {8, 5}, arrow, {QPolygonF{{1, 1}, {4, 4}, {7, 1}}});
    save("chevrons.png", {8, 12}, arrow, {QPolygonF{{1.5, 4.5}, {4, 2}, {6.5, 4.5}}, QPolygonF{{1.5, 7.5}, {4, 10}, {6.5, 7.5}}});
    save("check.png", {14, 14}, Qt::white, {QPolygonF{{3.5, 7.5}, {6, 10}, {10.5, 4}}});
    return dir;
}

// macOS stylesheet (Sonoma/Sequoia): @name@ placeholders are replaced with the light or dark variant colors.
inline QString macStyleSheet(bool dark)
{
    static const char *sheet = R"(
QWidget { color: @text@; } /* the font is the application font: the terminal keeps its own monospace font */
QMainWindow, QDialog, QMessageBox, QDockWidget, QStatusBar { background: @window@; }
QMainWindow::separator { background: @separator@; width: 1px; height: 1px; }
QMenuBar { background: @window@; padding: 2px 4px; border-bottom: 1px solid @separator@; }
QMenuBar::item { padding: 3px 9px; background: transparent; border-radius: 5px; }
QMenuBar::item:selected { background: @hover@; }
QMenuBar::item:pressed { background: @accent@; color: #ffffff; }
QMenu { background: @menu@; border: 1px solid @menuBorder@; border-radius: 8px; padding: 5px; }
QMenu::item { padding: 4px 22px 4px 24px; border-radius: 5px; background: transparent; }
QMenu::item:selected { background: @accent@; color: #ffffff; }
QMenu::item:disabled { color: @disabled@; }
QMenu::separator { height: 1px; background: @separator@; margin: 4px 10px; }
QMenu::icon { padding-left: 6px; }
QToolBar { background: @window@; border: none; border-bottom: 1px solid @separator@; spacing: 4px; padding: 3px 6px; }
QToolBar::separator { background: @separator@; width: 1px; margin: 6px 4px; }
QToolBar#main QToolButton { padding: 3px 6px; min-width: 52px; }
QToolBar#sftpTools { background: @window@; border-bottom: 1px solid @separator@; padding: 2px 4px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 2px; }
QToolButton:hover { background: @hover@; }
QToolButton:pressed, QToolButton:checked { background: @pressed@; }
QToolButton::menu-indicator { image: none; width: 0; }
QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: @field@; border: 1px solid @fieldBorder@; border-radius: 6px; padding: 3px 6px; selection-background-color: @selection@; selection-color: @text@; }
QPlainTextEdit, QTextEdit { padding: 2px; }
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border: 2px solid @accentRing@; padding: 2px 5px; }
QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled { color: @disabled@; background: @window@; }
QComboBox { padding-right: 22px; }
QComboBox::drop-down { border: none; background: transparent; width: 20px; subcontrol-origin: padding; subcontrol-position: center right; }
QComboBox::down-arrow { image: url(@img@/chevrons.png); width: 8px; height: 12px; }
QSpinBox, QDoubleSpinBox { padding-right: 20px; }
QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 18px; border: none; background: transparent; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: @hover@; border-radius: 3px; }
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(@img@/up.png); width: 8px; height: 5px; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(@img@/down.png); width: 8px; height: 5px; }
QComboBox QAbstractItemView { background: @menu@; border: 1px solid @menuBorder@; border-radius: 6px; padding: 4px; outline: 0; selection-background-color: @accent@; selection-color: #ffffff; }
QLineEdit#quick { font-size: 14px; padding: 4px 10px; border-radius: 7px; background: @search@; border-color: @search@; }
QLineEdit#quick:focus { background: @field@; border: 2px solid @accentRing@; padding: 3px 9px; }
QLineEdit#sftpPath { color: @green@; font-family: "SF Mono", "Cascadia Mono", Consolas, monospace; font-size: 12px; }
QTreeView, QListView { background: @sidebar@; border: none; outline: 0; show-decoration-selected: 0; padding: 2px 6px; }
QTableView { background: @field@; border: 1px solid @separator@; border-radius: 6px; gridline-color: @separator@; outline: 0; }
QTreeView::item { height: 22px; padding: 0 4px; }
QListView::item { height: 22px; padding: 0 4px; border-radius: 5px; }
QTreeWidget#sessions { background: @sidebar@; }
QTreeWidget#sessions::item { height: 26px; }
QTreeView::item:hover, QListView::item:hover { background: @hover@; }
QTreeView::item:selected, QListView::item:selected, QTableView::item:selected { background: @accent@; color: #ffffff; }
QTreeView::item:selected:!active, QListView::item:selected:!active { background: @pressed@; color: @text@; }
QHeaderView { background: @header@; border: none; }
QHeaderView::section { background: @header@; color: @secondary@; border: none; border-bottom: 1px solid @separator@; border-right: 1px solid @separator@; padding: 3px 8px; font-weight: 600; }
QHeaderView::section:last { border-right: none; }
QSplitter::handle { background: @separator@; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QAbstractScrollArea { background: @window@; }
QAbstractScrollArea::corner { background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: @scroll@; min-height: 28px; border-radius: 3px; margin: 0 1px; }
QScrollBar::handle:horizontal { background: @scroll@; min-width: 28px; border-radius: 3px; margin: 1px 0; }
QScrollBar::handle:hover { background: @scrollHover@; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }
QPushButton { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @buttonTop@, stop:1 @buttonBottom@); border: 1px solid @buttonBorder@; border-radius: 6px; padding: 4px 16px; min-height: 16px; }
QPushButton:hover { border-color: @buttonHoverBorder@; }
QPushButton:pressed { background: @pressed@; }
QPushButton:default { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @accentTop@, stop:1 @accent@); border-color: @accentBorder@; color: #ffffff; }
QPushButton:default:pressed { background: @accentBorder@; }
QPushButton:disabled { color: @disabled@; background: @window@; border-color: @separator@; }
QPushButton#protocol { background: transparent; border: 1px solid transparent; border-radius: 8px; padding: 4px; min-width: 56px; }
QPushButton#protocol:hover { background: @hover@; }
QPushButton#protocol:checked { background: @accentTint@; border-color: @accent@; }
QGroupBox { background: @inset@; border: 1px solid @insetBorder@; border-radius: 8px; margin-top: 20px; padding: 10px 8px 8px 8px; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 4px; padding: 0 2px 4px 2px; color: @secondary@; font-weight: 600; }
QTabWidget::pane { background: @inset@; border: 1px solid @insetBorder@; border-radius: 8px; top: -12px; }
QTabWidget::tab-bar { alignment: center; }
QTabWidget > QTabBar::tab { background: @segment@; color: @text@; border: 1px solid @segmentBorder@; border-left: none; padding: 3px 14px; margin: 0; min-width: 40px; }
QTabWidget > QTabBar::tab:first { border-left: 1px solid @segmentBorder@; border-top-left-radius: 6px; border-bottom-left-radius: 6px; }
QTabWidget > QTabBar::tab:last { border-top-right-radius: 6px; border-bottom-right-radius: 6px; }
QTabWidget > QTabBar::tab:only-one { border-left: 1px solid @segmentBorder@; border-radius: 6px; }
QTabWidget > QTabBar::tab:selected { background: @segmentOn@; color: @segmentOnText@; }
QTabWidget > QTabBar::tab:hover:!selected { background: @hover@; }
QWidget#strip { background: @sidebar@; border-right: 1px solid @separator@; }
QToolButton#stripButton { background: transparent; border: none; border-radius: 6px; margin: 2px 4px; padding: 4px; }
QToolButton#stripButton:hover { background: @hover@; }
QToolButton#stripButton:checked { background: @pill@; }
QLabel { background: transparent; }
QLabel#empty { color: @secondary@; }
QLabel#sftpStatus { color: @secondary@; font-size: 12px; padding: 2px 4px; }
QCheckBox, QRadioButton { spacing: 6px; background: transparent; }
QCheckBox:disabled, QRadioButton:disabled { color: @disabled@; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid @fieldBorder@; border-radius: 4px; background: @field@; }
QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: @accent@; }
QCheckBox::indicator:checked { background: @accent@; border-color: @accentBorder@; image: url(@img@/check.png); }
QCheckBox::indicator:indeterminate { background: @accentTint@; border-color: @accent@; }
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled { background: @window@; border-color: @separator@; }
QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid @fieldBorder@; border-radius: 8px; background: @field@; }
QRadioButton::indicator:checked { width: 8px; height: 8px; border: 4px solid @accent@; border-radius: 8px; background: #ffffff; }
QSlider::groove:horizontal { height: 4px; background: @segment@; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @accent@; border-radius: 2px; }
QSlider::handle:horizontal { width: 16px; height: 16px; margin: -6px 0; border-radius: 8px; background: @buttonTop@; border: 1px solid @buttonBorder@; }
QToolTip { background: @menu@; color: @text@; border: 1px solid @menuBorder@; border-radius: 6px; padding: 4px 8px; }
QProgressBar { background: @segment@; border: none; border-radius: 4px; color: @text@; text-align: center; font-size: 10px; max-height: 14px; }
QProgressBar::chunk { background: @accent@; border-radius: 4px; }
)";
    // Light/dark pairs; the values follow the macOS system colors.
    static const QList<std::array<const char *, 3>> colors{{"window", "#ececec", "#1e1e1e"},
                                                           {"sidebar", "#e3e3e8", "#262628"},
                                                           {"text", "#1d1d1f", "#e5e5e7"},
                                                           {"secondary", "#6e6e73", "#98989d"},
                                                           {"disabled", "#a1a1a6", "#5f5f63"},
                                                           {"accent", "#007aff", "#0a84ff"},
                                                           {"accentTop", "#2f8fff", "#3796ff"},
                                                           {"accentBorder", "#0062cc", "#0070e0"},
                                                           {"accentRing", "#5aa5ff", "#3b97ff"},
                                                           {"accentTint", "#d6e8ff", "#163a63"},
                                                           {"selection", "#b3d7ff", "#1f4f80"},
                                                           {"separator", "#d1d1d6", "#38383a"},
                                                           {"field", "#ffffff", "#2c2c2e"},
                                                           {"fieldBorder", "#c7c7cc", "#48484a"},
                                                           {"search", "#e0e0e5", "#2c2c2e"},
                                                           {"hover", "#dcdce1", "#323234"},
                                                           {"pressed", "#d1d1d6", "#3a3a3c"},
                                                           {"pill", "#d1d1d8", "#3a3a3c"},
                                                           {"menu", "#f6f6f6", "#2c2c2e"},
                                                           {"menuBorder", "#c8c8cc", "#48484a"},
                                                           {"header", "#f5f5f7", "#262628"},
                                                           {"inset", "#e4e4e6", "#252527"},
                                                           {"insetBorder", "#d6d6da", "#323234"},
                                                           {"segment", "#dcdce0", "#2c2c2e"},
                                                           {"segmentBorder", "#c7c7cc", "#3a3a3c"},
                                                           {"segmentOn", "#ffffff", "#636366"},
                                                           {"segmentOnText", "#1d1d1f", "#ffffff"},
                                                           {"buttonTop", "#ffffff", "#5a5a5e"},
                                                           {"buttonBottom", "#f6f6f6", "#4e4e52"},
                                                           {"buttonBorder", "#c4c4c8", "#3a3a3c"},
                                                           {"buttonHoverBorder", "#a8a8ae", "#6a6a6e"},
                                                           {"scroll", "#b8b8bd", "#5a5a5e"},
                                                           {"scrollHover", "#8e8e93", "#7c7c80"},
                                                           {"green", "#248a3d", "#30d158"}};
    QString result = sheet;
    result.replace("@img@", macIndicatorDir(dark));
    for (const auto &c : colors)
        result.replace(QString("@%1@").arg(c[0]), dark ? c[2] : c[1]);
    return result;
}

// Full stylesheet for the chosen appearance; classicDarkStyle is darkStyle from main.cpp.
inline QString appearanceStyleSheet(Appearance a, const QString &classicDarkStyle)
{
    if (appearanceIsMac(a))
        return macStyleSheet(a == Appearance::MacDark);
    return appearanceColors(a, classicDarkStyle);
}

// macOS tabs: rounded pills; the current one is raised with a light shadow, the others are text only.
inline QSize macTabSizeHint(const QString &text, const QFontMetrics &fm)
{
    const int height = std::max(30, fm.height() + 14);
    if (text.isEmpty())
        return {44, height};
    return {std::clamp(fm.horizontalAdvance(text) + 64, 100, 240), height}; // room for the icon and the close button
}

inline void paintMacTab(QPainter &p, const QRectF &rect, const QIcon &icon, const QString &text, bool current, bool dark,
                        const QFontMetrics &fm)
{
    const auto palette = appearancePalette(dark ? Appearance::MacDark : Appearance::MacLight);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF pill = rect.adjusted(2, 3, -2, -3);
    if (current)
    {
        // Shadow: two blurred outlines under the pill
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, dark ? 70 : 22));
        p.drawRoundedRect(pill.translated(0, 1.5), 7, 7);
        p.setBrush(QColor(0, 0, 0, dark ? 40 : 12));
        p.drawRoundedRect(pill.adjusted(-0.5, 0, 0.5, 2), 7.5, 7.5);
        p.setPen(QPen(dark ? QColor(255, 255, 255, 28) : QColor(0, 0, 0, 30), 0.8));
        p.setBrush(dark ? QColor("#48484a") : QColor("#ffffff"));
        p.drawRoundedRect(pill, 7, 7);
    }
    const int iconSize = 16;
    const bool iconOnly = text.isEmpty();
    const int iconX = iconOnly ? int(pill.center().x()) - iconSize / 2 : int(pill.left()) + 10;
    const QRect iconBox(iconX, int(pill.center().y()) - iconSize / 2, iconSize, iconSize);
    icon.paint(&p, iconBox);
    if (!iconOnly)
    {
        p.setPen(current ? palette.text : palette.secondaryText);
        const QRect textBox(iconBox.right() + 7, int(pill.top()), int(pill.right()) - iconBox.right() - 7 - 22,
                            int(pill.height()));
        p.drawText(textBox, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(text, Qt::ElideRight, textBox.width()));
    }
    p.restore();
}
