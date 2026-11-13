// UI appearances: valid and complete style sheets, keys, contrast, macOS tab painting.
// With CROSSTERM_APPEARANCE_PREVIEW set, saves appearance-mac-light.png and appearance-mac-dark.png.
#include <QtWidgets>
#include "../appearance.h"
#include <cmath>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const QByteArray &what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what.constData());
    failures += !ok;
}

static QStringList styleWarnings;
static void collect(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    if (type != QtDebugMsg && (message.contains("parse", Qt::CaseInsensitive) || message.contains("Unknown property")))
        styleWarnings << message;
}

// darkStyle read from main.cpp, so the test follows the real default sheet.
static QString classicDarkStyle()
{
    QFile file(CROSSTERM_SOURCE_DIR "/main.cpp");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString source = QString::fromUtf8(file.readAll());
    const qsizetype start = source.indexOf("darkStyle = R\"(");
    const qsizetype end = source.indexOf(")\";", start);
    return start < 0 || end < 0 ? QString() : source.mid(start + 15, end - start - 15);
}

static double luminance(const QColor &c)
{
    auto channel = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

static double contrast(const QColor &a, const QColor &b)
{
    const double x = luminance(a), y = luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

// Sample tab bar using paintMacTab, as Tabs in main.cpp will.
class MacTabs : public QTabBar
{
public:
    bool dark = false;
    using QTabBar::QTabBar;

protected:
    QSize tabSizeHint(int index) const override { return macTabSizeHint(tabText(index), fontMetrics()); }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        for (int i = 0; i < count(); ++i)
            paintMacTab(p, tabRect(i), tabIcon(i), tabText(i), i == currentIndex(), dark, fontMetrics());
    }
};

// Test window with every objectName styled by the default sheet.
static QMainWindow *sampleWindow(bool dark)
{
    auto *w = new QMainWindow;
    const auto *style = QApplication::style();
    auto *file = w->menuBar()->addMenu("File");
    file->addAction("New session");
    file->addSeparator();
    file->addAction("Quit")->setEnabled(false);
    w->menuBar()->addMenu("View");
    auto *tools = w->addToolBar("Main");
    tools->setObjectName("main");
    tools->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tools->setIconSize(QSize(20, 20));
    tools->addAction(style->standardIcon(QStyle::SP_ComputerIcon), "Session");
    tools->addAction(style->standardIcon(QStyle::SP_DriveNetIcon), "Servers");
    tools->addAction(style->standardIcon(QStyle::SP_DirIcon), "Tools")->setCheckable(true);
    tools->addAction(style->standardIcon(QStyle::SP_FileDialogDetailedView), "Split");

    auto *central = new QWidget;
    auto *row = new QHBoxLayout(central);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto *strip = new QWidget;
    strip->setObjectName("strip");
    auto *stripLayout = new QVBoxLayout(strip);
    stripLayout->setContentsMargins(0, 4, 0, 4);
    for (int i = 0; i < 3; ++i)
    {
        auto *b = new QToolButton;
        b->setObjectName("stripButton");
        b->setCheckable(true);
        b->setChecked(i == 0);
        b->setIcon(style->standardIcon(i == 0 ? QStyle::SP_DirHomeIcon : QStyle::SP_FileIcon));
        b->setIconSize(QSize(20, 20));
        stripLayout->addWidget(b);
    }
    stripLayout->addStretch();
    row->addWidget(strip);

    auto *side = new QWidget;
    auto *sideLayout = new QVBoxLayout(side);
    auto *quick = new QLineEdit;
    quick->setObjectName("quick");
    quick->setPlaceholderText("Quick connect...");
    sideLayout->addWidget(quick);
    auto *sessions = new QTreeWidget;
    sessions->setObjectName("sessions");
    sessions->setHeaderHidden(true);
    auto *group = new QTreeWidgetItem(sessions, {"User sessions"});
    for (const char *name : {"web-01", "db-primary", "router"})
        new QTreeWidgetItem(group, {name});
    group->setExpanded(true);
    sessions->setCurrentItem(group->child(1));
    sideLayout->addWidget(sessions);
    auto *empty = new QLabel("No macros recorded");
    empty->setObjectName("empty");
    sideLayout->addWidget(empty);
    auto *sftpTools = new QToolBar;
    sftpTools->setObjectName("sftpTools");
    sftpTools->setIconSize(QSize(18, 18));
    sftpTools->addAction(style->standardIcon(QStyle::SP_ArrowUp), "Up");
    sftpTools->addAction(style->standardIcon(QStyle::SP_BrowserReload), "Refresh");
    sideLayout->addWidget(sftpTools);
    auto *path = new QLineEdit("/home/user");
    path->setObjectName("sftpPath");
    sideLayout->addWidget(path);
    auto *status = new QLabel("Connected, 12 items");
    status->setObjectName("sftpStatus");
    sideLayout->addWidget(status);
    auto *progress = new QProgressBar;
    progress->setValue(64);
    progress->setMaximumHeight(14);
    sideLayout->addWidget(progress);
    side->setFixedWidth(230);
    row->addWidget(side);

    auto *content = new QWidget;
    auto *contentLayout = new QVBoxLayout(content);
    auto *tabs = new MacTabs;
    tabs->dark = dark;
    tabs->addTab(style->standardIcon(QStyle::SP_DirHomeIcon), QString());
    tabs->addTab(style->standardIcon(QStyle::SP_ComputerIcon), "web-01");
    tabs->addTab(style->standardIcon(QStyle::SP_ComputerIcon), "db-primary");
    tabs->setCurrentIndex(1);
    contentLayout->addWidget(tabs);
    auto *protocols = new QHBoxLayout;
    for (const char *name : {"SSH", "Telnet", "RDP"})
    {
        auto *b = new QPushButton(name);
        b->setObjectName("protocol");
        b->setCheckable(true);
        b->setChecked(QByteArray(name) == "SSH");
        protocols->addWidget(b);
    }
    contentLayout->addLayout(protocols);
    auto *box = new QGroupBox("Basic SSH settings");
    auto *form = new QFormLayout(box);
    form->addRow("Remote host", new QLineEdit("example.org"));
    auto *port = new QSpinBox;
    port->setRange(1, 65535);
    port->setValue(22);
    form->addRow("Port", port);
    auto *combo = new QComboBox;
    combo->addItems({"Password", "Public key"});
    form->addRow("Auth", combo);
    auto *check = new QCheckBox("Remember password");
    check->setChecked(true);
    form->addRow(check);
    form->addRow(new QCheckBox("X11 forwarding"));
    auto *radio = new QRadioButton("Compression");
    radio->setChecked(true);
    form->addRow(radio);
    contentLayout->addWidget(box);
    auto *pages = new QTabWidget;
    pages->addTab(new QLabel("Advanced"), "Advanced");
    pages->addTab(new QLabel("Terminal"), "Terminal");
    pages->addTab(new QLabel("Network"), "Network");
    contentLayout->addWidget(pages, 1);
    auto *table = new QTreeWidget;
    table->setHeaderLabels({"Name", "Size"});
    new QTreeWidgetItem(table, {"notes.txt", "4 KB"});
    contentLayout->addWidget(table);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setDefault(true);
    contentLayout->addWidget(buttons);
    row->addWidget(content, 1);
    w->setCentralWidget(central);
    w->resize(820, 640);
    return w;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    qInstallMessageHandler(collect);
    // The offscreen platform has no fonts: previews load the system ones, if present.
    if (qEnvironmentVariableIsSet("CROSSTERM_APPEARANCE_PREVIEW"))
        for (const char *font : {"segoeui.ttf", "seguisb.ttf", "segoeuib.ttf", "consola.ttf"})
            QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR", "C:/Windows")).filePath("Fonts/") + font);
    const QString dark = classicDarkStyle();
    check(dark.contains("QWidget#strip"), "darkStyle read from main.cpp");

    // Keys: compatible with saved "dark"/"light", round-trip for every appearance.
    const QList<Appearance> all{Appearance::ClassicDark, Appearance::ClassicLight, Appearance::MacLight, Appearance::MacDark};
    for (auto a : all)
        check(appearanceFromKey(appearanceKey(a), false) == a && appearanceFromKey(appearanceKey(a), true) == a,
              "key round-trip " + appearanceKey(a).toUtf8());
    check(appearanceKey(Appearance::ClassicDark) == "dark" && appearanceKey(Appearance::ClassicLight) == "light", "legacy keys");
    check(appearanceFromKey("", false) == Appearance::MacLight && appearanceFromKey("bogus", true) == Appearance::MacDark,
          "missing or unknown key: macOS following the system");
    check(appearanceFromKey("mac-system", true) == Appearance::MacDark &&
              appearanceFromKey("mac-system", false) == Appearance::MacLight,
          "mac-system follows the system");
    check(appearanceFromKey("system", true) == Appearance::ClassicDark &&
              appearanceFromKey("system", false) == Appearance::ClassicLight,
          "system follows the system");
    check(appearanceName(Appearance::MacLight) == "macOS light", "names");

    // Default dark appearance: the current style sheet is returned unchanged.
    check(appearanceStyleSheet(Appearance::ClassicDark, dark) == dark, "ClassicDark returns darkStyle unchanged");
    check(appearanceStyleSheet(Appearance::ClassicLight, dark).contains("background: #f3f3f3"), "ClassicLight converted");

    // Every selector of the default sheet (types and objectName) appears in the macOS sheets.
    QSet<QString> selectors;
    static const QRegularExpression selector("(Q\\w+|#\\w+)");
    for (const QString &line : dark.split('\n'))
        for (auto it = selector.globalMatch(line.section('{', 0, 0)); it.hasNext();)
            selectors << it.next().captured(1);
    for (auto a : {Appearance::MacLight, Appearance::MacDark})
    {
        const QString sheet = appearanceStyleSheet(a, dark);
        QStringList missing;
        for (const QString &s : selectors)
            if (!sheet.contains(s))
                missing << s;
        check(missing.isEmpty(),
              "all default selectors styled in " + appearanceKey(a).toUtf8() + " " + missing.join(' ').toUtf8());
        check(!sheet.contains('@'), "no unresolved placeholder in " + appearanceKey(a).toUtf8());
    }

    for (bool isDark : {false, true})
    {
        const QString dir = macIndicatorDir(isDark);
        check(QImage(dir + "/check.png").width() == 28 && QImage(dir + "/chevrons.png").height() == 24,
              "indicator images generated in " + dir.toUtf8());
    }

    // Each sheet applies without parse warnings on a representative window.
    for (auto a : all)
    {
        styleWarnings.clear();
        app.setStyleSheet(appearanceStyleSheet(a, dark));
        auto *w = sampleWindow(appearanceIsDark(a));
        w->show();
        QApplication::processEvents();
        w->grab();
        auto *menu = w->menuBar()->actions().first()->menu();
        menu->popup(QPoint(10, 10));
        QApplication::processEvents();
        menu->grab();
        menu->hide();
        if (appearanceIsMac(a) && qEnvironmentVariableIsSet("CROSSTERM_APPEARANCE_PREVIEW"))
        {
            const QString name = "appearance-" + appearanceKey(a) + ".png";
            check(w->grab().save(name), "preview saved " + name.toUtf8());
        }
        delete w;
        check(styleWarnings.isEmpty(),
              "stylesheet parses cleanly: " + appearanceKey(a).toUtf8() + " " + styleWarnings.join(" | ").toUtf8());
    }
    // Sanity check of the check: a broken sheet must trigger the warning intercepted above.
    styleWarnings.clear();
    app.setStyleSheet("QWidget { color: red; QLabel {");
    QLabel probe("probe");
    probe.show();
    QApplication::processEvents();
    check(!styleWarnings.isEmpty(), "broken stylesheet is detected");
    app.setStyleSheet({});

    // Palette: text/window contrast at least 4.5 (WCAG AA), 20px icons with text below on macOS.
    for (auto a : {Appearance::MacLight, Appearance::MacDark})
    {
        const auto p = appearancePalette(a);
        check(contrast(p.text, p.window) >= 4.5, "text/window contrast " + appearanceKey(a).toUtf8());
        check(contrast(p.text, p.sidebar) >= 4.5, "text/sidebar contrast " + appearanceKey(a).toUtf8());
        check(p.toolbarIconSize == 20 && p.textUnderIcon, "toolbar 20px text under icon " + appearanceKey(a).toUtf8());
    }
    check(appearancePalette(Appearance::MacLight).separator == QColor("#d1d1d6") &&
              appearancePalette(Appearance::MacDark).separator == QColor("#38383a"),
          "separators");
    check(appearancePalette(Appearance::ClassicDark).toolbarIconSize == 24, "Classic keeps 24px icons");

    // Tabs: the current one is drawn differently from a non-current one; reasonable sizes.
    const QFontMetrics fm(app.font());
    const QIcon icon = app.style()->standardIcon(QStyle::SP_ComputerIcon);
    for (bool isDark : {false, true})
    {
        auto render = [&](bool current)
        {
            QImage image(160, 32, QImage::Format_ARGB32_Premultiplied);
            image.fill(appearancePalette(isDark ? Appearance::MacDark : Appearance::MacLight).window);
            QPainter p(&image);
            paintMacTab(p, QRectF(0, 0, 160, 32), icon, "web-01", current, isDark, fm);
            return image;
        };
        check(render(true) != render(false), QByteArray("current tab differs from non-current ") + (isDark ? "dark" : "light"));
    }
    const QSize home = macTabSizeHint({}, fm), named = macTabSizeHint("db-primary", fm),
                longName = macTabSizeHint(QString(200, 'x'), fm);
    check(home.width() < named.width() && longName.width() <= 240 && named.height() >= 26, "tab size hints");

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
