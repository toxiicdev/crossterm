#include "profiles.h"
#include "terminal.h"
#include "sftp-panel.h"
#include "icons.h"
#include "i18n.h"
#include "askpass.h"
#include "credentials.h"
#include "transports.h"
#include "mosh.h"
#include "proxy.h"
#include "monitor.h"
#include "xserver.h"
#include "mobaimport.h"
#include "servers.h"
#include "appearance.h"
#include "ftp.h"
#include "vnc.h"
#include "rdp.h"
#include "mobaimport.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QtWidgets>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#ifndef Q_OS_WIN
#include <csignal>
#include <unistd.h>
#endif

static QString platform()
{
#ifdef Q_OS_WIN
    return "windows";
#elif defined(Q_OS_MACOS)
    return "macos";
#else
    return "linux";
#endif
}

static Command localShell()
{
    if (platform() == "windows")
        return {"powershell.exe", {}, {}};
    return {qEnvironmentVariable("SHELL", "/bin/sh"), {}, {}};
}

static bool launch(const Command &command)
{
    QString program = command.program;
    if (!QFileInfo(program).isAbsolute())
        program = QStandardPaths::findExecutable(program);
    return !program.isEmpty() && QProcess::startDetached(program, command.arguments);
}

// Dark theme: background, border and header colors.
static const char *darkStyle = R"(
QWidget { background: #141414; color: #e6e6e6; }
QMenuBar { background: #141414; padding: 1px 0; }
QMenuBar::item { padding: 2px 7px; background: transparent; }
QMenuBar::item:selected, QMenu::item:selected { background: #2f4f6f; }
QMenu { background: #1f1f1f; border: 1px solid #4a4a4a; }
QMenu::item { padding: 4px 24px 4px 28px; }
QMenu::item:disabled { color: #6a6a6a; }
QMenu::separator { height: 1px; background: #3a3a3a; margin: 3px 6px; }
QToolBar { border: none; spacing: 2px; background: #141414; }
QToolBar#main QToolButton { padding: 1px 4px; min-width: 46px; }
QToolButton { border: 1px solid transparent; border-radius: 0; background: transparent; }
QToolButton:hover { background: #2a3a4a; border-color: #3d5a78; }
QToolButton:pressed, QToolButton:checked { background: #253545; border-color: #3d5a78; }
QToolButton::menu-indicator { image: none; width: 0; }
QLineEdit, QPlainTextEdit, QSpinBox, QComboBox { background: #1c1c1c; border: 1px solid #6a6a6a; padding: 1px 3px; selection-background-color: #2f5f8f; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: #fefefe; }
QLineEdit#quick { font-size: 12pt; color: #e6e6e6; padding-left: 8px; }
QLineEdit#sftpPath { color: #9fda9f; }
QTreeView, QListView { background: #141414; border: none; outline: 0; show-decoration-selected: 1; }
QTreeView::item { height: 18px; }
QTreeWidget#sessions::item { height: 24px; }
QTreeView::item:selected, QListView::item:selected { background: #2f4f6f; color: #ffffff; }
QTreeView::item:hover { background: #222a33; }
QHeaderView::section { background: #323232; color: #e6e6e6; border: none; border-right: 1px solid #141414; padding: 4px 6px; }
QSplitter::handle { background: #2a2a2a; }
QScrollBar:vertical { background: #1c1c1c; width: 12px; margin: 0; }
QScrollBar::handle:vertical { background: #4a4a4a; min-height: 24px; }
QScrollBar:horizontal { background: #1c1c1c; height: 12px; margin: 0; }
QScrollBar::handle:horizontal { background: #4a4a4a; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QPushButton { background: #2a2a2a; border: 1px solid #555; padding: 4px 14px; }
QPushButton:hover { background: #33414f; border-color: #3d5a78; }
QPushButton:disabled, QCheckBox:disabled { color: #6a6a6a; }
QPushButton#protocol { background: transparent; border: 1px solid transparent; padding: 2px; min-width: 52px; }
QPushButton#protocol:checked { background: #253545; border-color: #3d73a8; }
QGroupBox { border: 1px solid #3a3a3a; margin-top: 10px; padding-top: 8px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #8fc0ff; }
QTabWidget::pane { border: 1px solid #3a3a3a; }
QTabWidget > QTabBar::tab { background: #232323; padding: 5px 12px; border: 1px solid #3a3a3a; border-bottom: none; }
QTabWidget > QTabBar::tab:selected { background: #2f4f6f; }
QWidget#strip { background: #141414; }
QToolButton#stripButton { background: #262626; border: 1px solid #3a3a3a; border-left: none; margin: 0; }
QToolButton#stripButton:checked { background: #141414; border-right-color: #141414; }
QLabel#empty { color: #8a8a8a; }
QCheckBox::indicator { width: 12px; height: 12px; border: 1px solid #9a9a9a; background: #1c1c1c; }
QCheckBox::indicator:checked { background: #3d8fe0; border-color: #8fc0ff; }
QCheckBox::indicator:disabled { border-color: #4a4a4a; }
QToolTip { background: #2a2a2a; color: #e6e6e6; border: 1px solid #555; }
QProgressBar { background: #1c1c1c; border: 1px solid #4a4a4a; color: #e6e6e6; text-align: center; font-size: 7pt; }
QProgressBar::chunk { background: #3d8fe0; }
)";

// Appearances: classic dark (default), classic light, macOS light and dark (appearance.h).
static Appearance appearance = Appearance::MacLight;
// Saved "theme" key: a fixed appearance or "mac-system"/"system", which follow the OS dark/light setting.
static QString themeKey = "mac-system";
static bool systemDark()
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}
static QString themed(const QString &dark)
{
    return appearanceColors(appearance, dark);
}

// UI font per appearance (not in the stylesheet, which would also force it on the terminal).
static void applyAppearanceFont(Appearance look)
{
    QFont font;
    if (appearanceIsMac(look))
    {
        font.setFamilies({"SF Pro Text", "Segoe UI Variable", "Segoe UI"});
        font.setPixelSize(13);
    }
    else
    {
        font.setFamily("Segoe UI");
        font.setPointSize(9);
    }
    qApp->setFont(font);
}

// Trapezoid tabs; the Home tab shows only its icon.
class Tabs : public QTabBar
{
public:
    using QTabBar::QTabBar;

protected:
    // Without this the minimum width includes the scroll arrows even when every tab fits,
    // which leaves a gap between the last tab (Home at startup) and the "+" button.
    QSize minimumSizeHint() const override
    {
        const auto minimum = QTabBar::minimumSizeHint();
        return {std::min(minimum.width(), sizeHint().width()), minimum.height()};
    }
    QSize tabSizeHint(int index) const override
    {
        if (appearanceIsMac(appearance))
            return macTabSizeHint(tabText(index), fontMetrics());
        if (tabText(index).isEmpty())
            return {48, 26};
        const int text = fontMetrics().horizontalAdvance(tabText(index));
        return {std::clamp(text + 70, 90, 240), 26};
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        for (int i = count() - 1; i >= 0; --i)
            if (i != currentIndex())
                paintTab(p, i);
        if (currentIndex() >= 0)
            paintTab(p, currentIndex());
    }

private:
    void paintTab(QPainter &p, int i)
    {
        if (appearanceIsMac(appearance))
        {
            paintMacTab(p, tabRect(i), tabIcon(i), tabText(i), i == currentIndex(), appearanceIsDark(appearance), fontMetrics());
            return;
        }
        const QRectF r = tabRect(i);
        const bool current = i == currentIndex();
        const QPolygonF shape{{r.left() + 1, r.bottom() + 1},
                              {r.left() + 10, r.top() + 3},
                              {r.right() - 10, r.top() + 3},
                              {r.right() - 1, r.bottom() + 1}};
        p.setPen(QColor(themed("#6a6a6a")));
        p.setBrush(QColor(themed(current ? "#1c1c1c" : "#2a2a2a")));
        p.drawPolygon(shape);
        if (current)
        {
            p.setPen(QColor(themed("#1c1c1c")));
            p.drawLine(QPointF(r.left() + 2, r.bottom()), QPointF(r.right() - 2, r.bottom()));
        }
        const QRect iconBox(int(r.left()) + 16, int(r.center().y()) - 7, 16, 16);
        tabIcon(i).paint(&p, iconBox);
        if (tabText(i).isEmpty())
            return;
        p.setPen(QColor(themed(current ? "#ffffff" : "#b0b0b0")));
        const QRect textBox(iconBox.right() + 6, int(r.top()) + 3, int(r.width()) - 64, int(r.height()) - 3);
        p.drawText(textBox, Qt::AlignVCenter | Qt::AlignLeft,
                   fontMetrics().elidedText(tabText(i), Qt::ElideRight, textBox.width()));
    }
};

struct Page
{
    QWidget *widget = nullptr;
    Terminal *terminal = nullptr;
    SftpPanel *sftp = nullptr;
    QJsonObject profile;
    Command command;
    std::function<QIODevice *()> device; // telnet, rlogin, serial, mosh: recreated on every restart
    RemoteMonitor *monitor = nullptr;    // Remote monitoring of the SSH session
    MonitorStats stats;
    QString monitorError;
};

class Window : public QMainWindow
{
    QString storage;
    QJsonArray profiles;
    bool writable = true;
    QLineEdit *quick = new QLineEdit;
    QTreeWidget *tree = new QTreeWidget;
    QStackedWidget *side = new QStackedWidget;
    QStackedWidget *sftpStack = new QStackedWidget;
    QList<QToolButton *> strip;
    Tabs *bar = new Tabs;
    QWidget *area = new QWidget;
    QVBoxLayout *grid = new QVBoxLayout(area);
    QSplitter *splitRoot = nullptr;
    Page *home = new Page;
    QListWidget *recent = new QListWidget;
    QSet<QString> expanded;
    int splitMode = 1, counter = 0;
    QString lastFilter;
    ServersDialog *serversDialog = nullptr;
    FtpServer ftpServer; // stays alive: servers keep running after the dialog closes
    XServerManager xserver, xdmcp;
    MonitorBar *monitorBar = new MonitorBar;
    QList<QAction *> shortcutActions;
    QHash<QString, QKeySequence> defaultShortcuts;
    QList<std::pair<QPointer<QWidget>, QString>> inlineStyles;
    void style(QWidget *w, const QString &dark)
    {
        inlineStyles.append({w, dark});
        w->setStyleSheet(themed(dark));
    }
    QJsonArray macros, recordingSteps;
    bool recording = false, playing = false;
    QListWidget *macroList = new QListWidget;
    QLabel *macroHint = new QLabel;
    QPushButton *recoverButton = new QPushButton;
    QWidget *findBar = new QWidget;
    QLineEdit *findText = new QLineEdit;
    QCheckBox *findCase = new QCheckBox;
    QLabel *findStatus = new QLabel;
    QJsonArray tunnels;
    QHash<QString, QProcess *> running;
    QHash<QString, QString> tunnelStatus;
    std::function<void()> tunnelsChanged;
    Texts texts;
    QList<std::function<void()>> retranslators;
    QWidget *leftPanel = nullptr;
    QAction *multiAction = nullptr, *undoImport = nullptr;
    bool multiExec = false, forwarding = false;

public:
    bool persistGeometry = true;

private:
    bool moving = false;

    Page *page(int index) const { return index < 0 ? nullptr : reinterpret_cast<Page *>(bar->tabData(index).value<quintptr>()); }
    Page *current() const { return page(bar->currentIndex()); }

    void showSide(int index)
    {
        for (int i = 0; i < strip.size(); ++i)
            strip[i]->setChecked(i == index);
        side->setCurrentIndex(index);
    }

    void addPage(Page *p, const QString &title, const QIcon &tabIcon)
    {
        p->widget->hide();
        const int index = bar->addTab(tabIcon, title);
        bar->setTabData(index, quintptr(p));
        bar->setTabToolTip(index, title);
        if (p != home)
        {
            auto *close = new QToolButton;
            close->setText(QString::fromUtf8("×"));
            texts.tip(close, N("Close tab"));
            close->setFixedSize(16, 16);
            close->setStyleSheet(
                "QToolButton { color: #c0c0c0; font-size: 12pt; border: none; background: transparent; padding: 0 0 2px 0; }QToolButton:hover { color: #ffffff; background: #c42b1c; }");
            connect(close, &QToolButton::clicked, this, [this, p] { closePage(p); });
            bar->setTabButton(index, QTabBar::RightSide, close);
        }
        bar->setCurrentIndex(index);
        layoutPages();
    }

    // Columns taken by text in the terminal: wide CJK characters take two, combining marks none.
    static int cellWidth(const QString &text)
    {
        int width = 0;
        for (const auto c : text.toUcs4())
        {
            const auto category = QChar::category(c);
            if (category == QChar::Mark_NonSpacing || category == QChar::Mark_Enclosing)
                continue;
            const bool wide = (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) ||
                              (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
                              (c >= 0xFFE0 && c <= 0xFFE6);
            width += wide ? 2 : 1;
        }
        return width;
    }

    // Welcome box shown at the top of every terminal tab, before the shell or the server's MOTD.
    static QString welcomeBanner(const QJsonObject &profile, const Command &command)
    {
        const QString reset = "\x1b[0m", frame = "\x1b[90m", green = "\x1b[1;32m", yellow = "\x1b[33m", magenta = "\x1b[35m",
                      cyan = "\x1b[4;36m", red = "\x1b[31m";
        struct Row
        {
            QString plain, colored;
            bool centered = false;
        };
        QList<Row> rows;
        auto add = [&rows](const QString &plain, const QString &colored, bool centered = false)
        { rows.append({plain, colored, centered}); };
        const QString title = QString("• CrossTerm - Open Source Terminal v%1 •").arg(CROSSTERM_VERSION);
        add(title, green + title + reset, true);
        const auto subtitle = "(" + L("Remote server toolbox: SSH, SFTP, X11 and more") + ")";
        add(subtitle, yellow + subtitle + reset, true);
        add({}, {});
        const auto protocol = profile["protocol"].toString();
        const auto host = profile["host"].toString();
        if (!host.isEmpty())
        {
            static const QHash<QString, QString> names{{"ssh", "SSH"},       {"sftp", "SFTP"}, {"telnet", "Telnet"},
                                                       {"rlogin", "Rlogin"}, {"rsh", "Rsh"},   {"mosh", "Mosh"},
                                                       {"serial", "Serial"}, {"wsl", "WSL"}};
            const auto user = profile["user"].toString();
            const auto who = user.isEmpty() ? host : user + "@" + host;
            const auto session = L("%1 session to %2").arg(names.value(protocol, protocol.toUpper()), "\x01");
            add("➤ " + QString(session).replace("\x01", who), "➤ " + QString(session).replace("\x01", magenta + who + reset));
            if (protocol == "ssh")
            {
                const bool x11 = std::any_of(command.environment.begin(), command.environment.end(),
                                             [](const QString &e) { return e.startsWith("DISPLAY="); });
                const QList<std::pair<QString, bool>> features{
                    {profile["jumps"].toArray().isEmpty() ? L("Direct SSH") : L("SSH gateway"), true},
                    {L("SSH compression"), profile["compression"].toBool()},
                    {L("SFTP browser"), true},
                    {L("X11-Forwarding"), x11}};
                int label = 0;
                for (const auto &f : features)
                    label = std::max(label, cellWidth(f.first));
                for (const auto &[name, on] : features)
                {
                    const auto pad = QString(label - cellWidth(name), ' ');
                    QString note =
                        on && name == L("X11-Forwarding") ? "  " + L("(remote display forwarded through SSH)") : QString();
                    add("  • " + name + pad + " : " + (on ? "✔" : "✘") + note,
                        "  • " + name + pad + " : " + (on ? green + "✔" : red + "✘") + reset + note);
                }
            }
            add({}, {});
        }
        const QString link = "github.com/toxiicdev/crossterm";
        add("➤ " + link, "➤ " + cyan + link + reset);
        int inner = 0;
        for (const auto &r : rows)
            inner = std::max(inner, cellWidth(r.plain));
        inner += 4;
        QString out = frame + "┌" + QString(inner, QChar(0x2500)) + "┐" + reset + "\r\n";
        for (const auto &r : rows)
        {
            const int free = inner - cellWidth(r.plain);
            const int left = r.centered ? free / 2 : 2;
            out +=
                frame + "│" + reset + QString(left, ' ') + r.colored + QString(free - left, ' ') + frame + "│" + reset + "\r\n";
        }
        return out + frame + "└" + QString(inner, QChar(0x2500)) + "┘" + reset + "\r\n\r\n";
    }

    // Per-session settings (zoom, Remote monitoring, Follow terminal folder) live in QSettings under this key.
    static QString sessionKey(const QJsonObject &profile)
    {
        if (profile.isEmpty())
            return "session/local";
        const auto id = profile["id"].toString();
        return "session/" + (id.isEmpty() ? credentialTarget(profile, "quick").replace('/', '_') : id);
    }
    bool monitoringByDefault = true; // the e2e turns it off: its test server has no /proc

    Terminal *makeTerminal(Page *p)
    {
        Terminal *t = nullptr;
        if (!p->device)
            t = new Terminal(p->command, area);
        else if (auto *device = p->device())
            t = new Terminal(device, area);
        else
            return new Terminal(Command{p->profile["protocol"].toString(), {}, {}}, area); // shows "Cannot start"
        // Last Ctrl+wheel zoom of this session.
        const auto key = sessionKey(p->profile);
        if (const int zoom = QSettings().value(key + "/zoom").toInt(); zoom > 0)
            t->setFontPointSize(zoom);
        t->onZoom = [key](int points) { QSettings().setValue(key + "/zoom", points); };
        // Harmless X11 notices: Windows ssh has no xauth, and servers without xauth refuse forwarding.
        t->hideOutput({"Warning: No xauth data; using fake authentication data for X11 forwarding.",
                       "X11 forwarding request failed on channel 0"});
        if (!t->finished())
            t->printLocal(welcomeBanner(p->profile, p->command));
        return t;
    }

    void terminal(const Command &command, const QString &name, const QJsonObject &profile = {},
                  std::function<QIODevice *()> device = {})
    {
        auto *p = new Page;
        p->profile = profile;
        p->command = command;
        p->device = std::move(device);
        p->terminal = makeTerminal(p);
        p->terminal->installEventFilter(this);
        startLog(p->terminal, name);
        p->widget = p->terminal;
        wire(p);
        const auto protocol = profile["protocol"].toString();
        if ((protocol == "ssh" || protocol == "sftp" || protocol == "mosh") && !profile["host"].toString().isEmpty())
        {
            p->sftp = new SftpPanel(profile, platform(), sftpStack);
            p->sftp->folderSource = [p] { return p->terminal ? p->terminal->workingDirectory() : QString(); };
            p->sftp->onMonitoring = [this, p](bool on) { toggleMonitor(p, on); };
            p->sftp->pasteToTerminal = [p](const QString &text)
            {
                if (p->terminal)
                    p->terminal->paste(text);
            };
            sftpStack->addWidget(p->sftp);
            // Last Remote monitoring (on by default) and Follow terminal folder state of this session.
            const auto key = sessionKey(profile);
            // A session never opened before starts from the last choice made in any session.
            QSettings settings;
            p->sftp->setFollow(settings.value(key + "/follow", settings.value("session/last/follow", false)).toBool());
            connect(p->sftp->followBox(), &QCheckBox::toggled, this,
                    [key](bool on)
                    {
                        QSettings().setValue(key + "/follow", on);
                        QSettings().setValue("session/last/follow", on);
                    });
            connect(p->sftp->monitoring, &QPushButton::toggled, this,
                    [key](bool on)
                    {
                        QSettings().setValue(key + "/monitoring", on);
                        QSettings().setValue("session/last/monitoring", on);
                    });
            if (settings.value(key + "/monitoring", settings.value("session/last/monitoring", monitoringByDefault)).toBool())
                QTimer::singleShot(0, p->sftp, [sftp = p->sftp] { sftp->monitoring->setChecked(true); });
        }
        addPage(p, QString("%1. %2").arg(++counter).arg(name), icon(protocol.isEmpty() ? "shell" : protocol));
        if (p->sftp)
            showSide(3);
        p->terminal->setFocus();
    }

    void wire(Page *p)
    {
        p->terminal->onClose = [this, p] { QTimer::singleShot(0, this, [this, p] { closePage(p); }); };
        p->terminal->onRestart = [this, p]
        {
            QTimer::singleShot(0, this,
                               [this, p]
                               {
                                   auto *old = p->terminal;
                                   for (auto &entry : p->command.environment)
                                   {
                                       if (entry.startsWith("CROSSTERM_ASKPASS_TOKEN="))
                                           entry =
                                               "CROSSTERM_ASKPASS_TOKEN=" + QUuid::createUuid().toString(QUuid::WithoutBraces);
                                   }
                                   p->terminal = makeTerminal(p);
                                   p->terminal->installEventFilter(this);
                                   p->widget = p->terminal;
                                   wire(p);
                                   old->hide();
                                   old->setParent(area);
                                   old->deleteLater();
                                   layoutPages();
                               });
        };
    }

    void closePage(Page *p)
    {
        for (int i = 0; i < bar->count(); ++i)
        {
            if (page(i) != p || p == home)
                continue;
            // Detach from the splitters before removeTab: currentChanged rebuilds the Split and would delete the widget.
            p->widget->hide();
            p->widget->setParent(area);
            bar->removeTab(i);
            p->widget->deleteLater();
            if (p->sftp)
                p->sftp->deleteLater();
            delete p->monitor;
            delete p;
            layoutPages();
            return;
        }
    }

    // Split: 1 terminal, 2 side by side, 2 stacked, 4 in a grid (current tab and the following ones).
    void layoutPages()
    {
        // Pages are reparented to area before deleting the previous layout's splitters.
        for (int i = 0; i < bar->count(); ++i)
        {
            auto *w = page(i)->widget;
            w->hide();
            if (w->parentWidget() != area)
                w->setParent(area);
        }
        delete splitRoot;
        splitRoot = nullptr;
        auto *now = current();
        if (!now)
            return;
        QList<Page *> visible{now};
        const int wanted = splitMode == 4 ? 4 : splitMode == 1 ? 1 : 2;
        for (int k = 1; now != home && k < bar->count() && visible.size() < wanted; ++k)
        {
            auto *next = page((bar->currentIndex() + k) % bar->count());
            if (next != home)
                visible << next;
        }
        splitRoot = new QSplitter(splitMode == 2 ? Qt::Horizontal : Qt::Vertical, area);
        splitRoot->setHandleWidth(4);
        splitRoot->setChildrenCollapsible(false);
        if (splitMode == 4 && visible.size() > 2)
        {
            auto *top = new QSplitter(Qt::Horizontal, splitRoot), *bottom = new QSplitter(Qt::Horizontal, splitRoot);
            for (int k = 0; k < visible.size(); ++k)
                (k < 2 ? top : bottom)->addWidget(visible[k]->widget);
            for (auto *row : {top, bottom})
                row->setHandleWidth(4), row->setChildrenCollapsible(false);
        }
        else
        {
            for (auto *v : visible)
                splitRoot->addWidget(v->widget);
        }
        for (auto *v : visible)
            v->widget->show();
        grid->addWidget(splitRoot);
        monitorBar->setVisible(now->monitor);
        if (now->monitor && !now->monitorError.isEmpty())
            monitorBar->showError(now->monitorError);
        else if (now->monitor && !now->stats.host.isEmpty())
            monitorBar->show(now->stats);
        else if (now->monitor)
            monitorBar->showError(L("Loading…")); // until the first data arrives
        sftpStack->setCurrentIndex(0);
        if (now->sftp)
            sftpStack->setCurrentWidget(now->sftp);
        setWindowTitle(
            (now == home ? QString("CrossTerm") : bar->tabText(bar->currentIndex()).section(". ", 1) + " - CrossTerm") +
            (multiExec ? " [" + L("MultiExec") + "]" : QString()) + (recording ? " [" + L("Recording macro") + "]" : QString()));
        if (now->terminal)
            now->terminal->setFocus();
    }

    int selection() const
    {
        const auto *item = tree->currentItem();
        return item && item->data(0, Qt::UserRole + 1).toBool() ? item->data(0, Qt::UserRole).toInt() : -1;
    }

    QStringList folderPath(const QString &folder) const
    {
        return folder.split(QRegularExpression("[\\\\/]"), Qt::SkipEmptyParts);
    }

    void refresh()
    {
        if (tree->topLevelItemCount() && lastFilter.isEmpty())
        {
            expanded.clear();
            QTreeWidgetItemIterator it(tree);
            for (; *it; ++it)
                if ((*it)->isExpanded())
                    expanded << (*it)->data(0, Qt::UserRole + 2).toString();
        }
        tree->clear();
        auto *root = new QTreeWidgetItem(tree, {L("User sessions")});
        root->setIcon(0, icon("user"));
        root->setData(0, Qt::UserRole + 2, QString("/"));
        const auto filter = quick->text().trimmed();
        lastFilter = filter;
        QMap<QString, QTreeWidgetItem *> folders;
        QList<std::pair<QString, int>> order;
        for (int i = 0; i < profiles.size(); ++i)
            order.append({profiles[i].toObject()["name"].toString(), i});
        std::sort(order.begin(), order.end(), [](auto &a, auto &b) { return a.first.compare(b.first, Qt::CaseInsensitive) < 0; });
        for (const auto &[name, i] : order)
        {
            const auto p = profiles[i].toObject();
            const auto protocol = p["protocol"].toString();
            if (!(name + " " + p["host"].toString() + " " + p["folder"].toString() + " " + protocol)
                     .contains(filter, Qt::CaseInsensitive))
                continue;
            QTreeWidgetItem *parent = root;
            QString key;
            for (const auto &part : folderPath(p["folder"].toString()))
            {
                key += "/" + part;
                auto *&f = folders[key.toCaseFolded()];
                if (!f)
                {
                    f = new QTreeWidgetItem(parent, {part});
                    f->setIcon(0, icon("folder"));
                    f->setData(0, Qt::UserRole + 2, key);
                }
                parent = f;
            }
            const bool review = !p["issues"].toArray().isEmpty();
            auto *row = new QTreeWidgetItem(parent, {name});
            row->setData(0, Qt::UserRole, i);
            row->setData(0, Qt::UserRole + 1, true);
            row->setIcon(0, icon(protocol));
            row->setToolTip(0, QString("%1\n%2 %3%4")
                                   .arg(name, protocol.toUpper(), p["host"].toString(),
                                        review ? "\n" + L("Needs review: ") + describe(p["issues"].toArray().first().toString())
                                               : QString()));
            if (review)
                row->setForeground(0, QColor("#e8b04a"));
        }
        // Favorites: copies of the starred sessions, at the top of the tree.
        QTreeWidgetItem *favorites = nullptr;
        for (const auto &[name, i] : order)
        {
            const auto p = profiles[i].toObject();
            if (!p["favorite"].toBool() || !(name + " " + p["host"].toString()).contains(filter, Qt::CaseInsensitive))
                continue;
            if (!favorites)
            {
                favorites = new QTreeWidgetItem({L("Favorites")});
                favorites->setIcon(0, icon("star"));
                favorites->setData(0, Qt::UserRole + 2, QString("/*favorites"));
            }
            auto *row = new QTreeWidgetItem(favorites, {name});
            row->setData(0, Qt::UserRole, i);
            row->setData(0, Qt::UserRole + 1, true);
            row->setIcon(0, icon(p["protocol"].toString()));
        }
        // Folders before sessions, both sorted alphabetically.
        std::function<void(QTreeWidgetItem *)> arrange = [&](QTreeWidgetItem *item)
        {
            auto children = item->takeChildren();
            std::stable_sort(children.begin(), children.end(),
                             [](auto *a, auto *b)
                             {
                                 const bool fa = !a->data(0, Qt::UserRole + 1).toBool(),
                                            fb = !b->data(0, Qt::UserRole + 1).toBool();
                                 return fa != fb ? fa : a->text(0).compare(b->text(0), Qt::CaseInsensitive) < 0;
                             });
            item->addChildren(children);
            for (auto *child : children)
                arrange(child);
        };
        arrange(root);
        if (favorites)
            root->insertChild(0, favorites);
        if (!filter.isEmpty())
            tree->expandAll();
        else
        {
            QTreeWidgetItemIterator it(tree);
            for (; *it; ++it)
                (*it)->setExpanded(expanded.isEmpty() ? *it == root
                                                      : expanded.contains((*it)->data(0, Qt::UserRole + 2).toString()));
        }
        refreshRecent();
    }

    void refreshRecent()
    {
        recent->clear();
        for (const auto &id : QSettings().value("recent").toStringList())
        {
            for (int i = 0; i < profiles.size(); ++i)
            {
                const auto p = profiles[i].toObject();
                if (p["id"].toString() != id)
                    continue;
                auto *item = new QListWidgetItem(icon(p["protocol"].toString()), p["name"].toString(), recent);
                item->setData(Qt::UserRole, i);
            }
        }
    }

    bool save(const QJsonArray &next)
    {
        if (!writable)
        {
            QMessageBox::warning(this, L("Sessions"), L("Fix the sessions file before saving it."));
            return false;
        }
        QDir().mkpath(QFileInfo(storage).absolutePath());
        QSaveFile file(storage);
        const auto bytes =
            QJsonDocument(QJsonObject{{"version", 1}, {"sessions", next}, {"tunnels", tunnels}, {"macros", macros}}).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        {
            QMessageBox::warning(this, L("Save"), file.errorString());
            return false;
        }
        profiles = next;
        refresh();
        return true;
    }

    void importPasswords(QWidget *parent, const Import &source, const QJsonArray &selected)
    {
        if (source.passwordProfiles.isEmpty())
            return;
        if (!vaultAvailable())
        {
            QMessageBox::information(parent, L("Import"),
                                     L("Password migration requires the system vault, available on Windows."));
            return;
        }
        QSet<QString> handled;
        // Passwords protected by the MobaXterm master password: asked once, then every password is decrypted
        // in memory and saved in the system vault. A wrong master password decrypts nothing.
        QHash<QString, QString> decrypted; // profile id -> password
        if (!source.encryptedPasswords.isEmpty())
        {
            for (int attempt = 0; attempt < 3 && decrypted.isEmpty(); ++attempt)
            {
                bool accepted = false;
                auto master = QInputDialog::getText(
                    parent, L("MobaXterm master password"),
                    (attempt ? L("Wrong master password.") + "\n" : QString()) +
                        L("Enter the MobaXterm master password to import %1 saved password(s) into the system vault. Cancel to type them one by one.")
                            .arg(source.encryptedPasswords.size()),
                    QLineEdit::Password, {}, &accepted);
                if (!accepted || master.isEmpty())
                    break;
                for (auto it = source.encryptedPasswords.begin(); it != source.encryptedPasswords.end(); ++it)
                {
                    if (auto plain = decryptMobaPassword(it.value(), master))
                        decrypted.insert(it.key(), *plain);
                }
                master.fill(QChar(0));
            }
        }
        // Decrypted passwords go to every installed session with the same user, host and port, including
        // sessions imported earlier (skipped as duplicates now), so re-running the import fills them in.
        int saved = 0;
        for (const auto &value : source.profiles)
        {
            const auto profile = value.toObject();
            const auto it = decrypted.constFind(profile["id"].toString());
            if (it == decrypted.constEnd())
                continue;
            const auto target = credentialTarget(profile, QCoreApplication::applicationName());
            if (!handled.contains(target) && targetInUse(target) && storeSecret(target, profile["user"].toString(), it.value()))
            {
                handled.insert(target);
                ++saved;
            }
        }
        for (const auto &value : selected)
        {
            const auto profile = value.toObject();
            if (!source.passwordProfiles.contains(profile["id"].toString()) || decrypted.contains(profile["id"].toString()))
                continue;
            const auto target = credentialTarget(profile, QCoreApplication::applicationName());
            const auto user = profile["user"].toString();
            const auto host = profile["host"].toString();
            const auto port = profile["port"].toInt();
            if (handled.contains(target) || readSecret(target) || user.isEmpty() || !validHost(host) || port < 1 || port > 65535)
                continue;
            const bool installed =
                std::any_of(profiles.begin(), profiles.end(),
                            [&](const auto &entry)
                            {
                                const auto current = entry.toObject();
                                return current["name"] == profile["name"] && current["folder"] == profile["folder"] &&
                                       current["protocol"] == profile["protocol"] &&
                                       credentialTarget(current, QCoreApplication::applicationName()) == target;
                            });
            if (!installed)
                continue;
            handled.insert(target);
            bool accepted = false;
            auto secret =
                QInputDialog::getText(parent, L("Import saved password"),
                                      L("Enter the password for %1 on %2 (port %3). Cancel to skip.").arg(user, host).arg(port),
                                      QLineEdit::Password, {}, &accepted);
            if (accepted && !secret.isEmpty() && !storeSecret(target, user, secret))
                QMessageBox::warning(parent, L("Import"), L("The password could not be saved in the system vault."));
            secret.fill(QChar(0));
        }
        for (auto &plain : decrypted)
            plain.fill(QChar(0));
        if (saved)
            QMessageBox::information(parent, L("Import"), L("%1 password(s) imported into the system vault.").arg(saved));
    }

    void importLocal(const QString &source)
    {
        const auto result = localSessions(source);
        const auto next = mergeProfiles(profiles, result.profiles);
        const auto count = next.size() - profiles.size();
        if (count && !save(next))
            return;
        QMessageBox::information(this, L("Import"),
                                 count || result.warnings.isEmpty() ? L("%1 new session(s).").arg(count)
                                                                    : result.warnings.join('\n'));
    }

    void importFile(const QString &filter = {})
    {
        const auto ini = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/MobaXterm/MobaXterm.ini";
        const auto name = QFileDialog::getOpenFileName(
            this, L("Import sessions"), QFileInfo::exists(ini) ? ini : QString{},
            filter.isEmpty() ? L("Session files (*.ini *.mxtsessions *.moba *.mobaconf *.json *.csv *.xml *.reg *.xs *.dat)")
                             : filter);
        if (name.isEmpty())
            return;
        QFile file(name);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
        {
            QMessageBox::warning(this, L("Import"), L("Cannot read the file, or it is larger than 16 MB."));
            return;
        }
        const auto data = file.readAll();
        Import imported;
        if (name.endsWith(".json", Qt::CaseInsensitive))
        {
            const auto config = QJsonDocument::fromJson(data).object();
            if (config["version"].toInt() != 1 || !config["sessions"].isArray())
            {
                QMessageBox::warning(this, L("Import"), L("Unrecognized CrossTerm/RelayDesk format."));
                return;
            }
            for (const auto &entry : config["sessions"].toArray())
            {
                auto profile = entry.toObject();
                if (profile["name"].toString().isEmpty())
                    continue;
                profile["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
                imported.profiles.append(profile);
            }
        }
        else
        {
            imported = readForeign(data, name);
        }
        const auto next = mergeProfiles(profiles, imported.profiles);
        const auto added = next.size() - profiles.size();
        const bool changed = next != profiles;
        if (changed && !save(next))
            return;
        const auto summary =
            L("%1 new session(s).").arg(added) + (changed && !added ? " " + L("Existing sessions updated.") : QString());
        importPasswords(this, imported, imported.profiles);
        QStringList notes{summary};
        for (const auto &warning : imported.warnings)
            notes << describe(warning);
        QMessageBox::information(this, L("Import"), notes.join('\n'));
    }

    void exportFile()
    {
        const auto name =
            QFileDialog::getSaveFileName(this, L("Export sessions"), "CrossTerm-sessions.json", L("CrossTerm sessions (*.json)"));
        if (name.isEmpty())
            return;
        QSaveFile file(name);
        const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"sessions", profiles}}).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            QMessageBox::warning(this, L("Export"), file.errorString());
    }

    // "Session settings" dialog: protocol row, basic settings, advanced tabs and bookmark.
    void edit(int index, const QString &folderHint = {})
    {
        auto profile = index < 0 ? QJsonObject{{"folder", folderHint}} : profiles[index].toObject();
        QDialog editor(this);
        editor.setWindowTitle(L("Session settings"));
        auto *layout = new QVBoxLayout(&editor);
        QStringList warnings;
        for (const auto &issue : profile["issues"].toArray())
            warnings << describe(issue.toString());
        if (!warnings.isEmpty())
        {
            auto *notice = new QLabel(L("Needs review: ") + warnings.join(' '));
            notice->setWordWrap(true);
            notice->setStyleSheet("color: #e8b04a;");
            layout->addWidget(notice);
        }
        const QStringList protocols{"ssh", "telnet", "rsh", "rlogin", "rdp", "vnc", "ftp", "sftp", "serial", "mosh", "wsl"};
        const QStringList labels{"SSH", "Telnet", "Rsh", "Rlogin", "RDP", "VNC", "FTP", "SFTP", "Serial", "Mosh", "WSL"};
        auto *row = new QHBoxLayout;
        auto *group = new QButtonGroup(&editor);
        for (int i = 0; i < protocols.size(); ++i)
        {
            auto *b = new QPushButton(icon(protocols[i]), "");
            b->setObjectName("protocol");
            b->setIconSize(QSize(32, 32));
            b->setCheckable(true);
            b->setToolTip(L("%1 session").arg(labels[i]));
            auto *cell = new QVBoxLayout;
            cell->addWidget(b, 0, Qt::AlignHCenter);
            auto *caption = new QLabel(labels[i]);
            caption->setAlignment(Qt::AlignCenter);
            cell->addWidget(caption);
            row->addLayout(cell);
            group->addButton(b, i);
        }
        row->addStretch();
        layout->addLayout(row);
        auto *basic = new QGroupBox;
        auto *form = new QGridLayout(basic);
        auto *host = new QLineEdit(profile["host"].toString());
        auto *specify = new QCheckBox(L("Specify username"));
        auto *user = new QLineEdit(profile["user"].toString());
        specify->setChecked(!user->text().isEmpty());
        user->setEnabled(specify->isChecked());
        connect(specify, &QCheckBox::toggled, user, &QWidget::setEnabled);
        auto *port = new QSpinBox;
        port->setRange(1, 65535);
        port->setValue(profile["port"].toInt(22));
        form->addWidget(new QLabel(L("Remote host *")), 0, 0);
        form->addWidget(host, 0, 1);
        form->addWidget(specify, 0, 2);
        form->addWidget(user, 0, 3);
        form->addWidget(new QLabel(L("Port")), 0, 4);
        form->addWidget(port, 0, 5);
        // Protocol-specific fields: serial speed, mosh UDP ports.
        auto *baudLabel = new QLabel(L("Speed (bps)"));
        auto *baud = new QComboBox;
        for (const int b : {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600})
            baud->addItem(QString::number(b), b);
        baud->setCurrentText(QString::number(profile["baud"].toInt(9600)));
        auto *serialHint = new QLabel(L("Serial port") + ": " + serialPorts().join(", "));
        serialHint->setObjectName("empty");
        auto *moshLabel = new QLabel(L("Mosh UDP ports"));
        auto *moshPorts = new QLineEdit(profile["moshPorts"].toString());
        moshPorts->setPlaceholderText("60001:60999");
        form->addWidget(baudLabel, 1, 0);
        form->addWidget(baud, 1, 1);
        form->addWidget(serialHint, 1, 2, 1, 4);
        form->addWidget(moshLabel, 2, 0);
        form->addWidget(moshPorts, 2, 1);
        auto *commandLabel = new QLabel(L("Remote command"));
        auto *remoteCommand = new QLineEdit(profile["command"].toString());
        auto *domainLabel = new QLabel(L("Domain"));
        auto *domain = new QLineEdit(profile["domain"].toString());
        auto *redirectDrives = new QCheckBox(L("Redirect local drives"));
        redirectDrives->setChecked(profile["redirectDrives"].toBool());
        auto *ftpSecurity = new QComboBox;
        ftpSecurity->addItem(L("Plain FTP"), "none");
        ftpSecurity->addItem(L("FTPS (explicit TLS)"), "explicit");
        ftpSecurity->addItem(L("FTPS (implicit TLS)"), "implicit");
        ftpSecurity->setCurrentIndex(std::max(0, ftpSecurity->findData(profile["ftpSecurity"].toString("none"))));
        auto *ftpPassive = new QCheckBox(L("Passive mode"));
        ftpPassive->setChecked(profile["ftpPassive"].toBool(true));
        form->addWidget(commandLabel, 3, 0);
        form->addWidget(remoteCommand, 3, 1, 1, 3);
        form->addWidget(domainLabel, 4, 0);
        form->addWidget(domain, 4, 1);
        form->addWidget(redirectDrives, 4, 2, 1, 2);
        form->addWidget(ftpSecurity, 5, 1);
        form->addWidget(ftpPassive, 5, 2, 1, 2);
        form->setColumnStretch(1, 2);
        form->setColumnStretch(3, 1);
        layout->addWidget(basic);

        auto *tabs = new QTabWidget;
        auto *advanced = new QWidget;
        auto *adv = new QFormLayout(advanced);
        auto *x11 = new QCheckBox(L("X11-Forwarding"));
        x11->setChecked(profile["x11"].toBool());
        auto *compression = new QCheckBox(L("Compression"));
        compression->setChecked(profile["compression"].toBool());
        auto *useKey = new QCheckBox(L("Use private key"));
        auto *key = new QLineEdit(profile["identity"].toString());
        useKey->setChecked(!key->text().isEmpty());
        key->setEnabled(useKey->isChecked());
        connect(useKey, &QCheckBox::toggled, key, &QWidget::setEnabled);
        auto *browse = key->addAction(icon("folder"), QLineEdit::TrailingPosition);
        connect(browse, &QAction::triggered, &editor,
                [&]
                {
                    const auto file = QFileDialog::getOpenFileName(&editor, L("Private key"), QDir::homePath() + "/.ssh");
                    if (!file.isEmpty())
                        key->setText(QDir::toNativeSeparators(file));
                });
        QStringList gatewayRows;
        for (const auto &value : profile["jumps"].toArray())
        {
            const auto hop = value.toObject();
            gatewayRows << QString("%1,%2,%3").arg(hop["host"].toString()).arg(hop["port"].toInt()).arg(hop["user"].toString());
        }
        auto *gateways = new QPlainTextEdit(gatewayRows.join('\n'));
        gateways->setMaximumHeight(60);
        gateways->setPlaceholderText(L("host,port,user (one per line)"));
        auto *flags = new QHBoxLayout;
        flags->addWidget(x11);
        flags->addWidget(compression);
        flags->addStretch();
        adv->addRow(flags);
        auto *keyRow = new QHBoxLayout;
        keyRow->addWidget(useKey);
        keyRow->addWidget(key, 1);
        adv->addRow(keyRow);
        adv->addRow(L("SSH gateway (jump hosts)"), gateways);
        auto *proxyRow = new QHBoxLayout;
        auto *proxyType = new QComboBox;
        proxyType->addItem(L("No proxy"), "none");
        proxyType->addItem("HTTP", "http");
        proxyType->addItem("SOCKS5", "socks5");
        proxyType->addItem("SOCKS4", "socks4");
        proxyType->setCurrentIndex(std::max(0, proxyType->findData(profile["proxyType"].toString("none"))));
        auto *proxyHost = new QLineEdit(profile["proxyHost"].toString());
        proxyHost->setPlaceholderText(L("Proxy host"));
        auto *proxyPort = new QSpinBox;
        proxyPort->setRange(1, 65535);
        proxyPort->setValue(profile["proxyPort"].toInt(1080));
        proxyRow->addWidget(proxyType);
        proxyRow->addWidget(proxyHost, 1);
        proxyRow->addWidget(proxyPort);
        adv->addRow(L("Proxy"), proxyRow);
        const auto application = QCoreApplication::applicationName();
        const auto oldTarget = credentialTarget(profile, application);
        const bool hasSaved = index >= 0 && readSecret(oldTarget).has_value();
        auto *password = new QLineEdit;
        password->setEchoMode(QLineEdit::Password);
        password->setPlaceholderText(hasSaved ? L("Saved in the system vault (leave empty to keep it)") : QString());
        auto *savePassword = new QCheckBox(L("Save password in the system vault"));
        savePassword->setChecked(hasSaved);
        if (vaultAvailable())
        {
            adv->addRow(L("Password"), password);
            adv->addRow(savePassword);
        }
        tabs->addTab(advanced, icon("ssh", 16), L("Advanced SSH settings"));
        auto *bookmark = new QWidget;
        auto *bm = new QFormLayout(bookmark);
        auto *name = new QLineEdit(profile["name"].toString());
        name->setPlaceholderText(L("Default: remote host"));
        auto *folder = new QComboBox;
        folder->setEditable(true);
        QStringList folders{""};
        for (const auto &v : profiles)
            folders << v.toObject()["folder"].toString();
        folders.removeDuplicates();
        folders.sort(Qt::CaseInsensitive);
        folder->addItems(folders);
        folder->setCurrentText(profile["folder"].toString());
        bm->addRow(L("Session name"), name);
        bm->addRow(L("Folder"), folder);
        auto *autostart = new QCheckBox(L("Open this session when CrossTerm starts"));
        autostart->setChecked(profile["autostart"].toBool());
        bm->addRow(autostart);
        tabs->addTab(bookmark, icon("star", 16), L("Bookmark settings"));
        layout->addWidget(tabs);

        auto select = [&](int id, bool resetPort)
        {
            const auto protocol = protocols.value(id);
            basic->setTitle(L("Basic %1 settings").arg(labels.value(id)));
            const bool ssh = protocol == "ssh" || protocol == "sftp" || protocol == "mosh";
            const bool named = ssh || protocol == "rdp" || protocol == "ftp" || protocol == "rlogin" || protocol == "rsh";
            specify->setVisible(named);
            user->setVisible(named);
            for (auto *w : std::initializer_list<QWidget *>{baudLabel, baud, serialHint})
                w->setVisible(protocol == "serial");
            moshLabel->setVisible(protocol == "mosh");
            moshPorts->setVisible(protocol == "mosh");
            commandLabel->setVisible(protocol == "rsh");
            remoteCommand->setVisible(protocol == "rsh");
            for (auto *w : std::initializer_list<QWidget *>{domainLabel, domain, redirectDrives})
                w->setVisible(protocol == "rdp");
            ftpSecurity->setVisible(protocol == "ftp");
            ftpPassive->setVisible(protocol == "ftp");
            port->setEnabled(protocol != "serial" && protocol != "wsl");
            tabs->setTabEnabled(0, ssh);
            x11->setEnabled(protocol == "ssh");
            if (!ssh)
                tabs->setCurrentIndex(1);
            if (resetPort)
                port->setValue(defaultPort(protocol));
        };
        connect(group, &QButtonGroup::idClicked, &editor, [&](int id) { select(id, true); });
        const int start = std::max(0, int(protocols.indexOf(profile["protocol"].toString("ssh"))));
        group->button(start)->setChecked(true);
        select(start, false);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setIcon(icon("ok", 16));
        buttons->button(QDialogButtonBox::Cancel)->setIcon(icon("delete", 16));
        buttons->setCenterButtons(true);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &editor,
                [&]
                {
                    const auto protocol = protocols.value(group->checkedId());
                    const bool ssh = protocol == "ssh" || protocol == "sftp" || protocol == "mosh";
                    QJsonArray jumps;
                    for (const auto &line : ssh ? gateways->toPlainText().split('\n', Qt::SkipEmptyParts) : QStringList{})
                    {
                        const auto fields = line.split(',');
                        jumps.append(QJsonObject{{"host", fields.value(0).trimmed()},
                                                 {"port", fields.value(1, "22").toInt()},
                                                 {"user", fields.value(2).trimmed()}});
                    }
                    const auto title = name->text().trimmed().isEmpty() ? host->text().trimmed() : name->text().trimmed();
                    auto next = profiles;
                    profile =
                        QJsonObject{{"id", profile["id"].toString(QUuid::createUuid().toString(QUuid::WithoutBraces))},
                                    {"name", title},
                                    {"folder", folder->currentText().trimmed()},
                                    {"protocol", protocol},
                                    {"host", host->text().trimmed()},
                                    {"port", port->value()},
                                    {"user", specify->isChecked() && user->isVisible() ? user->text().trimmed() : QString()},
                                    {"identity", ssh && useKey->isChecked() ? key->text().trimmed() : QString()},
                                    {"jumps", jumps},
                                    {"compression", ssh && compression->isChecked()},
                                    {"x11", protocol == "ssh" && x11->isChecked()},
                                    {"issues", QJsonArray{}},
                                    {"baud", baud->currentData().toInt()},
                                    {"moshPorts", moshPorts->text().trimmed()},
                                    {"command", remoteCommand->text().trimmed()},
                                    {"domain", domain->text().trimmed()},
                                    {"redirectDrives", redirectDrives->isChecked()},
                                    {"ftpSecurity", ftpSecurity->currentData().toString()},
                                    {"ftpPassive", ftpPassive->isChecked()},
                                    {"proxyType", ssh ? proxyType->currentData().toString() : QString("none")},
                                    {"proxyHost", proxyHost->text().trimmed()},
                                    {"proxyPort", proxyPort->value()},
                                    {"autostart", autostart->isChecked()},
                                    {"favorite", profile["favorite"].toBool()}};
                    // Validation: mosh like SSH; serial and WSL only have the port or distribution name.
                    auto check = profile;
                    if (protocol == "mosh")
                        check["protocol"] = "ssh";
                    auto command = protocol == "serial" ? Command{} : sessionCommand(check, platform(), false);
                    if (protocol == "serial" && host->text().trimmed().isEmpty())
                        command.error = "Indirizzo o porta da verificare.";
                    if (title.isEmpty() || !command.error.isEmpty())
                    {
                        QMessageBox::warning(&editor, L("Session settings"),
                                             title.isEmpty() ? L("Enter the remote host.") : describe(command.error));
                        return;
                    }
                    if (index < 0)
                        next.append(profile);
                    else
                        next.replace(index, profile);
                    if (!save(next))
                        return;
                    // Passwords: only in the system vault, never in the sessions file.
                    const auto newTarget = credentialTarget(profile, application);
                    if (ssh && vaultAvailable() && savePassword->isChecked())
                    {
                        auto secret = password->text();
                        if (secret.isEmpty() && hasSaved && newTarget != oldTarget)
                            secret = readSecret(oldTarget).value_or(QString());
                        if (!secret.isEmpty() && !storeSecret(newTarget, profile["user"].toString(), secret))
                            QMessageBox::warning(&editor, L("Session settings"),
                                                 L("The password could not be saved in the system vault."));
                        secret.fill(QChar(0));
                    }
                    if (hasSaved && (!savePassword->isChecked() || newTarget != oldTarget) && !targetInUse(oldTarget))
                        removeSecret(oldTarget);
                    password->clear();
                    editor.accept();
                });
        editor.resize(640, editor.sizeHint().height());
        if (editor.exec() == QDialog::Accepted && index < 0)
        {
            for (int i = 0; i < profiles.size(); ++i)
                if (profiles[i].toObject()["id"] == profile["id"])
                {
                    open(i);
                    break;
                }
        }
    }

    // Mosh: starts mosh-server over SSH (askpass for passwords), reads "MOSH CONNECT" and opens the UDP channel.
    QIODevice *moshConnect(const QJsonObject &profile)
    {
        auto ssh = profile;
        ssh["protocol"] = "ssh";
        ssh["x11"] = false;
        auto command = sessionCommand(ssh, platform(), false);
        const auto program = QStandardPaths::findExecutable(command.program);
        if (!command.error.isEmpty() || program.isEmpty())
            return nullptr;
        QProcess bootstrap;
        hideConsoleWindow(&bootstrap);
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto &entry : askpassEnvironment(credentialTarget(profile, QCoreApplication::applicationName()), true))
            environment.insert(entry.section('=', 0, 0), entry.section('=', 1));
        bootstrap.setProcessEnvironment(environment);
        bootstrap.setProcessChannelMode(QProcess::MergedChannels);
        const auto remote = moshServerArguments(profile["moshPorts"].toString()).join(' ');
        bootstrap.start(program, QStringList{"-tt"} + command.arguments + QStringList{remote});
        QEventLoop loop;
        connect(&bootstrap, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &loop, &QEventLoop::quit);
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        if (bootstrap.state() != QProcess::NotRunning)
            loop.exec();
        if (bootstrap.state() != QProcess::NotRunning)
            bootstrap.kill();
        int udpPort = 0;
        QByteArray key;
        if (!parseMoshConnect(bootstrap.readAll(), udpPort, key))
            return nullptr;
        auto *device = new MoshDevice(profile["host"].toString(), quint16(udpPort), key);
        key.fill('\0');
        if (device->isOpen())
            return device;
        delete device;
        return nullptr;
    }

public:
    // FTP: file browser inside the tab. Password: vault, then session memory, then prompt.
    FtpPanel *openFtp(const QJsonObject &profile)
    {
        const auto target = credentialTarget(profile, QCoreApplication::applicationName());
        auto password = [this, target, profile]() -> QString
        {
            if (const auto saved = readSecret(target))
                return *saved;
            if (const auto cached = SessionSecrets::instance().get(target))
                return *cached;
            bool ok = false;
            const auto typed = QInputDialog::getText(
                this, L("Authentication"), L("Password for %1@%2:").arg(profile["user"].toString(), profile["host"].toString()),
                QLineEdit::Password, {}, &ok);
            if (ok)
                SessionSecrets::instance().put(target, typed);
            return typed;
        };
        auto *p = new Page;
        p->profile = profile;
        auto *panel = new FtpPanel(profile, password, area);
        // FTPS certificate accepted by the user: the fingerprint is stored in the profile and not asked again.
        const auto id = profile["id"].toString();
        panel->certificateTrusted = [this, id](const QString &fingerprint)
        {
            auto next = profiles;
            for (int i = 0; i < next.size(); ++i)
            {
                auto stored = next[i].toObject();
                if (!id.isEmpty() && stored["id"].toString() == id)
                {
                    stored["ftpFingerprint"] = fingerprint;
                    next.replace(i, stored);
                    save(next);
                }
            }
        };
        p->widget = panel;
        addPage(p, QString("%1. %2").arg(++counter).arg(profile["name"].toString()), icon("ftp"));
        return panel;
    }

    // RDP: Windows Remote Desktop control embedded in the tab (embedded xfreerdp on Linux).
    RdpView *openRdp(const QJsonObject &profile)
    {
        const auto target = credentialTarget(profile, QCoreApplication::applicationName());
        auto password = [this, target, profile]() -> QString
        {
            if (const auto saved = readSecret(target))
                return *saved;
            if (const auto cached = SessionSecrets::instance().get(target))
                return *cached;
            bool ok = false;
            const auto typed = QInputDialog::getText(
                this, L("Authentication"), L("Password for %1@%2:").arg(profile["user"].toString(), profile["host"].toString()),
                QLineEdit::Password, {}, &ok);
            if (ok)
                SessionSecrets::instance().put(target, typed);
            return typed;
        };
        RdpView::Settings settings;
        settings.host = profile["host"].toString();
        settings.port = quint16(profile["port"].toInt(3389));
        settings.user = profile["user"].toString();
        settings.domain = profile["domain"].toString();
        settings.redirectDrives = profile["redirectDrives"].toBool();
        settings.redirectClipboard = profile["redirectClipboard"].toBool(true);
        auto *p = new Page;
        p->profile = profile;
        auto *view = new RdpView(settings, password, area);
        view->onDisconnected = [target](const QString &) { SessionSecrets::instance().forget(target); };
        p->widget = view;
        addPage(p, QString("%1. %2").arg(++counter).arg(profile["name"].toString()), icon("rdp"));
        view->connectToHost();
        return view;
    }

    // VNC: built-in viewer inside the tab (RFB). Password: vault, then session memory, then prompt.
    VncView *openVnc(const QJsonObject &profile)
    {
        const auto target = credentialTarget(profile, QCoreApplication::applicationName());
        auto password = [this, target, profile]() -> QString
        {
            if (const auto saved = readSecret(target))
                return *saved;
            if (const auto cached = SessionSecrets::instance().get(target))
                return *cached;
            bool ok = false;
            const auto typed =
                QInputDialog::getText(this, L("Authentication"), L("VNC password for %1:").arg(profile["host"].toString()),
                                      QLineEdit::Password, {}, &ok);
            if (ok)
                SessionSecrets::instance().put(target, typed);
            return typed;
        };
        auto *p = new Page;
        p->profile = profile;
        auto *view = new VncView(profile["host"].toString(), quint16(profile["port"].toInt(5900)), password, area);
        view->onDisconnected = [target](const QString &) { SessionSecrets::instance().forget(target); };
        p->widget = view;
        addPage(p, QString("%1. %2").arg(++counter).arg(profile["name"].toString()), icon("vnc"));
        view->setFocus();
        return view;
    }

private:
    // Sessions without a local process: the terminal talks directly to the device.
    bool openDevice(const QJsonObject &profile)
    {
        const auto protocol = profile["protocol"].toString();
        const auto host = profile["host"].toString().trimmed();
        const auto port = quint16(profile["port"].toInt(defaultPort(protocol)));
        const auto name = profile["name"].toString();
        if ((protocol == "telnet" || protocol == "rlogin" || protocol == "mosh") && !validHost(host))
        {
            QMessageBox::warning(this, L("Open session"), describe("Indirizzo o porta da verificare."));
            return true;
        }
        if (protocol == "ftp")
        {
            openFtp(profile);
            return true;
        }
        if (protocol == "vnc")
        {
            openVnc(profile);
            return true;
        }
        if (protocol == "rdp")
        {
            openRdp(profile);
            return true;
        }
        if (protocol == "rsh")
        {
            const auto user = profile["user"].toString(), remote = profile["command"].toString();
            terminal({}, name, profile, [host, port, user, remote] { return RshDevice::create(host, user, remote, port); });
            return true;
        }
        if (protocol == "telnet")
            terminal({}, name, profile, [host, port] { return new TelnetDevice(host, port); });
        else if (protocol == "rlogin")
        {
            const auto user = profile["user"].toString();
            terminal({}, name, profile, [host, port, user] { return new RloginDevice(host, user, port); });
        }
        else if (protocol == "serial")
        {
            SerialDevice::Settings settings;
            settings.portName = host;
            settings.baud = profile["baud"].toInt(9600);
            terminal({}, name, profile, [settings] { return new SerialDevice(settings); });
        }
        else if (protocol == "mosh")
            terminal(
                {}, name, profile,
                [this, profile]
                {
                    auto *device = moshConnect(profile);
                    if (!device)
                        QMessageBox::warning(
                            this, L("Open session"),
                            L("mosh-server could not be started on the remote host. Check that mosh is installed there and that the UDP ports are reachable."));
                    return device;
                });
        else
            return false;
        return true;
    }

    void open(int i)
    {
        if (i < 0 || i >= profiles.size())
            return;
        const auto profile = profiles[i].toObject();
        if (!profile["issues"].toArray().isEmpty())
        {
            edit(i);
            return;
        }
        if (openDevice(profile))
            return;
        const bool x11 = profile["x11"].toBool() && profile["protocol"].toString() == "ssh";
        if (x11 && !startXServer() && !qEnvironmentVariableIsSet("DISPLAY"))
            QMessageBox::warning(this, L("X server"),
                                 L("No X server found. Put VcXsrv in the xserver folder next to CrossTerm or install one."));
        auto command = sessionCommand(profile, platform(), x11 && !xDisplay().isEmpty());
        if (x11 && !xDisplay().isEmpty())
            command.environment << "DISPLAY=" + xDisplay();
        const auto protocol = profile["protocol"].toString();
        const auto target = credentialTarget(profile, QCoreApplication::applicationName());
        // Always via askpass: a password typed once stays available for SFTP, monitoring and tunnels.
        // Prompts (password, host key) appear inside the terminal tab, identified by this id.
        if (protocol == "ssh" || protocol == "sftp")
            command.environment << askpassEnvironment(target, true, QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (command.error.isEmpty())
        {
            auto ids = QSettings().value("recent").toStringList();
            ids.removeAll(profile["id"].toString());
            ids.prepend(profile["id"].toString());
            QSettings().setValue("recent", ids.mid(0, 8));
            refreshRecent();
        }
        if (command.error.isEmpty() && protocol != "rdp" && protocol != "vnc")
        {
            terminal(command, profile["name"].toString(), profile);
            return;
        }
        if (!command.error.isEmpty() || !launch(command))
            QMessageBox::warning(this, L("Open session"),
                                 command.error.isEmpty() ? L("Client not found: %1").arg(command.program)
                                                         : describe(command.error));
    }

    void remove(int i)
    {
        if (i < 0 ||
            QMessageBox::question(this, L("Delete session"),
                                  L("Delete session \"%1\"?").arg(profiles[i].toObject()["name"].toString())) != QMessageBox::Yes)
            return;
        const auto target = credentialTarget(profiles[i].toObject(), QCoreApplication::applicationName());
        auto next = profiles;
        next.removeAt(i);
        if (save(next) && !targetInUse(target))
            removeSecret(target);
    }

    // Deletes a session folder with every session in it and in its subfolders (folder like "Production\Web").
    void removeFolder(const QString &folder)
    {
        if (!deletableFolder(folder))
            return;
        const auto indices = sessionsIn(folder);
        if (QMessageBox::question(
                this, L("Delete folder"),
                L("Delete folder \"%1\" and its %2 session(s)?").arg(folderPath(folder).join('\\')).arg(indices.size())) ==
            QMessageBox::Yes)
            removeSessions(indices);
    }

    // Folders that can be deleted: not the root "User sessions" (empty) and not the Favorites view ("*...").
    bool deletableFolder(const QString &folder) const { return !folderPath(folder).isEmpty() && !folder.startsWith('*'); }

    // Session indices inside a folder and its subfolders; the tree groups folders case-insensitively.
    QList<int> sessionsIn(const QString &folder) const
    {
        QList<int> result;
        const auto prefix = folderPath(folder).join('/').toCaseFolded();
        for (int i = 0; i < profiles.size(); ++i)
        {
            const auto other = folderPath(profiles[i].toObject()["folder"].toString()).join('/').toCaseFolded();
            if (other == prefix || other.startsWith(prefix + '/'))
                result << i;
        }
        return result;
    }

    // Deletes sessions by index; vault passwords no longer used by any session are removed too.
    void removeSessions(QList<int> indices)
    {
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        QStringList targets;
        auto next = profiles;
        for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        {
            targets << credentialTarget(profiles[*it].toObject(), QCoreApplication::applicationName());
            next.removeAt(*it);
        }
        if (!save(next))
            return;
        for (const auto &target : targets)
        {
            if (!targetInUse(target))
                removeSecret(target);
        }
    }

    // Multiple selection: selected sessions plus, with folders, every session inside them.
    QList<int> selectedSessions(bool withFolders) const
    {
        QList<int> result;
        for (auto *item : tree->selectedItems())
        {
            if (item->data(0, Qt::UserRole + 1).toBool())
                result << item->data(0, Qt::UserRole).toInt();
            else if (const auto folder = item->data(0, Qt::UserRole + 2).toString().mid(1);
                     withFolders && deletableFolder(folder))
                result << sessionsIn(folder);
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    // Del/Canc: one session or one folder keeps its own question; several items are deleted together.
    void removeSelected()
    {
        if (tree->selectedItems().size() <= 1)
        {
            if (selection() >= 0)
                remove(selection());
            else
                removeFolder(selectedFolder());
            return;
        }
        const auto indices = selectedSessions(true);
        if (indices.isEmpty())
            return;
        QStringList names;
        for (const int i : indices.mid(0, 10))
            names << profiles[i].toObject()["name"].toString();
        if (QMessageBox::question(this, L("Delete session"),
                                  L("Delete %1 selected session(s)?").arg(indices.size()) + "\n" + names.join('\n')) ==
            QMessageBox::Yes)
            removeSessions(indices);
    }

    void openSelected()
    {
        for (const int i : selectedSessions(false))
            open(i);
    }

    QString selectedFolder() const
    {
        const auto *item = tree->currentItem();
        return item && !item->data(0, Qt::UserRole + 1).toBool()
                   ? item->data(0, Qt::UserRole + 2).toString().mid(1).replace('/', '\\')
                   : QString();
    }

    bool targetInUse(const QString &target) const
    {
        for (const auto &v : profiles)
        {
            if (credentialTarget(v.toObject(), QCoreApplication::applicationName()) == target)
                return true;
        }
        return false;
    }

    void duplicate(int i)
    {
        if (i < 0)
            return;
        auto copy = profiles[i].toObject();
        copy["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        copy["name"] = L("%1 (copy)").arg(copy["name"].toString());
        auto next = profiles;
        next.append(copy);
        save(next);
    }

    void quickConnect()
    {
        const auto text = quick->text().trimmed();
        if (text.isEmpty())
            return;
        QList<int> matches;
        QTreeWidgetItemIterator it(tree);
        for (; *it; ++it)
            if ((*it)->data(0, Qt::UserRole + 1).toBool())
                matches << (*it)->data(0, Qt::UserRole).toInt();
        if (matches.size() == 1)
        {
            open(matches[0]);
            quick->clear();
            return;
        }
        // [ssh://|telnet://][user@]host[:port]
        static const QRegularExpression target("^(?:(ssh|telnet|sftp)://)?(?:([^@\\s]+)@)?(\\[[^\\]]+\\]|[^:\\s]+)(?::(\\d+))?$");
        const auto m = target.match(text);
        if (!m.hasMatch())
        {
            QMessageBox::warning(this, L("Quick connect"), L("Use [user@]host[:port]."));
            return;
        }
        const auto protocol = m.captured(1).isEmpty() ? QString("ssh") : m.captured(1);
        QJsonObject profile{{"protocol", protocol},
                            {"host", m.captured(3).remove('[').remove(']')},
                            {"user", m.captured(2)},
                            {"port", m.captured(4).isEmpty() ? defaultPort(protocol) : m.captured(4).toInt()}};
        const auto command = sessionCommand(profile, platform(), qEnvironmentVariableIsSet("DISPLAY"));
        if (!command.error.isEmpty())
        {
            QMessageBox::warning(this, L("Quick connect"), describe(command.error));
            return;
        }
        terminal(command, profile["host"].toString(), profile);
        quick->clear();
    }

    void settingsDialog()
    {
        QDialog dialog(this);
        dialog.setWindowTitle(L("CrossTerm Configuration"));
        auto *form = new QFormLayout(&dialog);
        QSettings settings;
        auto *font = new QFontComboBox;
        font->setFontFilters(QFontComboBox::MonospacedFonts);
        font->setCurrentFont(QFont(settings
                                       .value("terminal/font", platform() == "windows"
                                                                   ? QString("Consolas")
                                                                   : QFontDatabase::systemFont(QFontDatabase::FixedFont).family())
                                       .toString()));
        auto *size = new QSpinBox;
        size->setRange(6, 32);
        size->setValue(settings.value("terminal/size", 10).toInt());
        auto *paste = new QCheckBox(L("Paste using right-click"));
        paste->setChecked(settings.value("terminal/rightPaste", true).toBool());
        auto *theme = new QComboBox;
        theme->addItem(L("macOS (follow system)"), "mac-system");
        for (const auto look : {Appearance::MacLight, Appearance::MacDark})
            theme->addItem(L(appearanceName(look).toUtf8().constData()), appearanceKey(look));
        theme->addItem(L("Classic (follow system)"), "system");
        for (const auto look : {Appearance::ClassicDark, Appearance::ClassicLight})
            theme->addItem(L(appearanceName(look).toUtf8().constData()), appearanceKey(look));
        theme->setCurrentIndex(std::max(0, theme->findData(themeKey)));
        auto *scheme = new QComboBox;
        for (const auto &c : Terminal::schemes())
            scheme->addItem(c.name);
        scheme->setCurrentText(settings.value("terminal/scheme", "CrossTerm Dark").toString());
        form->addRow(L("Theme"), theme);
        auto *prediction = new QComboBox;
        prediction->addItem(L("Adaptive (slow connections only)"), "adaptive");
        prediction->addItem(L("Always"), "always");
        prediction->addItem(L("Never"), "never");
        prediction->setCurrentIndex(std::max(0, prediction->findData(settings.value("mosh/prediction", "adaptive").toString())));
        form->addRow(L("Mosh local echo"), prediction);
        auto *parallel = new QSpinBox;
        parallel->setRange(1, 20);
        parallel->setValue(std::clamp(settings.value("sftp/parallel", 5).toInt(), 1, 20));
        form->addRow(L("Simultaneous SFTP transfers"), parallel);
        auto pemField = [&](const QString &key)
        {
            auto *edit = new QLineEdit(settings.value(key).toString());
            auto *pick = edit->addAction(icon("folder"), QLineEdit::TrailingPosition);
            connect(pick, &QAction::triggered, &dialog,
                    [&dialog, edit]
                    {
                        const auto file =
                            QFileDialog::getOpenFileName(&dialog, L("PEM file"), edit->text(), "PEM (*.pem *.crt *.key)");
                        if (!file.isEmpty())
                            edit->setText(QDir::toNativeSeparators(file));
                    });
            return edit;
        };
        auto *ftpCertificate = pemField("servers/ftpCertificate");
        auto *ftpKey = pemField("servers/ftpKey");
        form->addRow(L("FTP server certificate (FTPS)"), ftpCertificate);
        form->addRow(L("FTP server private key"), ftpKey);
        form->addRow(L("Terminal colors"), scheme);
        form->addRow(L("Terminal font"), font);
        form->addRow(L("Font size"), size);
        form->addRow(paste);
        auto *logEnabled = new QCheckBox(L("Save terminal sessions to log files"));
        logEnabled->setChecked(settings.value("log/enabled").toBool());
        auto *logFolder = new QLineEdit(
            settings.value("log/folder", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/CrossTerm logs")
                .toString());
        auto *pick = logFolder->addAction(icon("folder"), QLineEdit::TrailingPosition);
        connect(pick, &QAction::triggered, &dialog,
                [&]
                {
                    const auto folder = QFileDialog::getExistingDirectory(&dialog, L("Log folder"), logFolder->text());
                    if (!folder.isEmpty())
                        logFolder->setText(QDir::toNativeSeparators(folder));
                });
        form->addRow(logEnabled);
        form->addRow(L("Log folder"), logFolder);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;
        settings.setValue("terminal/font", font->currentFont().family());
        settings.setValue("terminal/size", size->value());
        settings.setValue("terminal/rightPaste", paste->isChecked());
        applyThemeKey(theme->currentData().toString());
        settings.setValue("mosh/prediction", prediction->currentData().toString());
        settings.setValue("sftp/parallel", parallel->value());
        settings.setValue("servers/ftpCertificate", ftpCertificate->text().trimmed());
        settings.setValue("servers/ftpKey", ftpKey->text().trimmed());
        applyScheme(scheme->currentText());
        settings.setValue("log/enabled", logEnabled->isChecked());
        settings.setValue("log/folder", logFolder->text().trimmed());
        for (int i = 0; i < bar->count(); ++i)
        {
            if (auto *t = page(i)->terminal)
            {
                auto f = t->font();
                f.setFamily(font->currentFont().family());
                f.setPointSize(size->value());
                t->setFont(f);
            }
        }
    }

    QWidget *homePage()
    {
        auto *w = new QWidget(area);
        style(w, "background: #1c1c1c;");
        auto *v = new QVBoxLayout(w);
        v->addStretch(2);
        auto *logo = new QLabel("CrossTerm");
        logo->setAlignment(Qt::AlignCenter);
        style(logo, "font-size: 30pt; font-weight: 600; color: #f0f0f0;");
        v->addWidget(logo);
        auto *start = texts.text(new QPushButton(icon("session"), QString()), N("Start local terminal"));
        start->setIconSize(QSize(28, 28));
        start->setStyleSheet("font-size: 12pt; padding: 10px 24px;");
        connect(start, &QPushButton::clicked, this, [this] { terminal(localShell(), L("Local terminal")); });
        v->addWidget(start, 0, Qt::AlignHCenter);
        v->addSpacing(18);
        texts.text(recoverButton, N("Recover previous sessions"));
        recoverButton->setVisible(!QSettings().value("lastSessions").toStringList().isEmpty());
        connect(recoverButton, &QPushButton::clicked, this,
                [this] { recoverSessions(QSettings().value("lastSessions").toStringList()); });
        v->addWidget(recoverButton, 0, Qt::AlignHCenter);
        auto *caption = texts.text(new QLabel, N("Recent sessions"));
        caption->setAlignment(Qt::AlignCenter);
        caption->setObjectName("empty");
        v->addWidget(caption);
        recent->setMaximumSize(360, 190);
        recent->setIconSize(QSize(16, 16));
        texts.bind(recent, &QWidget::setAccessibleName, N("Recent sessions"));
        connect(recent, &QListWidget::itemActivated, this,
                [this](QListWidgetItem *item) { open(item->data(Qt::UserRole).toInt()); });
        v->addWidget(recent, 0, Qt::AlignHCenter);
        v->addStretch(3);
        return w;
    }

    void tabMenu(const QPoint &at)
    {
        const int index = bar->tabAt(at);
        auto *p = page(index);
        if (!p || p == home)
            return;
        QMenu menu;
        menu.addAction(L("Rename tab"), [&] { renameTab(index); });
        if (auto *rdp = dynamic_cast<RdpView *>(p->widget))
        {
            menu.addAction(L("Fullscreen"), [rdp] { rdp->setFullScreen(true); });
            menu.addAction(L("Send Ctrl+Alt+Del"), [rdp] { rdp->sendCtrlAltDel(); });
            menu.addAction(L("Reconnect"), [rdp] { rdp->connectToHost(); });
            menu.addSeparator();
        }
        if (auto *view = dynamic_cast<VncView *>(p->widget))
        {
            auto *scale = menu.addAction(L("Scale to fit"), [view](bool on) { view->setScaled(on); });
            scale->setCheckable(true);
            scale->setChecked(view->isScaled());
            auto *viewOnly = menu.addAction(L("View only"), [view](bool on) { view->setViewOnly(on); });
            viewOnly->setCheckable(true);
            viewOnly->setChecked(view->isViewOnly());
            menu.addAction(L("Send Ctrl+Alt+Del"), [view] { view->sendCtrlAltDel(); });
            menu.addSeparator();
        }
        menu.addAction(L("Duplicate tab"), [&] { terminal(p->command, bar->tabText(index).section(". ", 1), p->profile); });
        menu.addSeparator();
        menu.addAction(L("Close tab"), [&] { closePage(p); });
        menu.addAction(L("Close all tabs except this one"), [&] { closeAll(p); });
        menu.addAction(L("Close all tabs"), [&] { closeAll(nullptr); });
        menu.exec(bar->mapToGlobal(at));
    }

    void treeMenu(const QPoint &at)
    {
        auto *item = tree->itemAt(at);
        QMenu menu;
        if (item && item->isSelected() && tree->selectedItems().size() > 1)
        {
            const auto sessions = selectedSessions(false).size(), all = selectedSessions(true).size();
            if (sessions)
                menu.addAction(icon("session", 16), L("Open %1 selected session(s)").arg(sessions), [&] { openSelected(); });
            if (all)
                menu.addAction(icon("delete", 16), L("Delete %1 selected session(s)").arg(all), [&] { removeSelected(); });
            if (!menu.isEmpty())
                menu.exec(tree->viewport()->mapToGlobal(at));
            return;
        }
        const int i = item && item->data(0, Qt::UserRole + 1).toBool() ? item->data(0, Qt::UserRole).toInt() : -1;
        if (i >= 0)
        {
            menu.addAction(icon("session", 16), L("Execute"), [&] { open(i); });
            menu.addAction(L("Edit session"), [&] { edit(i); });
            menu.addAction(L("Duplicate session"), [&] { duplicate(i); });
            menu.addAction(icon("star", 16),
                           profiles[i].toObject()["favorite"].toBool() ? L("Remove from favorites") : L("Add to favorites"),
                           [&] { toggleFavorite(i); });
            menu.addAction(icon("delete", 16), L("Delete session"), [&] { remove(i); });
            menu.addSeparator();
        }
        const auto folder = item ? item->data(0, Qt::UserRole + 2).toString().mid(1).replace('/', '\\') : QString();
        menu.addAction(icon("session", 16), L("New session"),
                       [&] { edit(-1, i >= 0 ? profiles[i].toObject()["folder"].toString() : folder); });
        if (i < 0 && !folderPath(folder).isEmpty() && !folder.startsWith('*'))
            menu.addAction(icon("delete", 16), L("Delete folder"), [&] { removeFolder(folder); });
        menu.addAction(L("Import from MobaXterm..."), [&] { importWizard(); });
        menu.addAction(L("Import sessions from file"), [&] { importFile(); });
        menu.addAction(L("Export sessions to file"), [&] { exportFile(); });
        menu.exec(tree->viewport()->mapToGlobal(at));
    }

    QLabel *emptyPage()
    {
        auto *label = new QLabel;
        label->setObjectName("empty");
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        label->setMargin(16);
        return label;
    }

    // --- SSH tunnels: each tunnel is an "ssh -N" process with -L, -R or -D.
    QJsonObject profileById(const QString &id) const
    {
        for (const auto &v : profiles)
        {
            if (v.toObject()["id"].toString() == id)
                return v.toObject();
        }
        return {};
    }

    int tunnelIndex(const QString &id) const
    {
        for (int i = 0; i < tunnels.size(); ++i)
        {
            if (tunnels[i].toObject()["id"].toString() == id)
                return i;
        }
        return -1;
    }

    void setTunnelStatus(const QString &id, const QString &status)
    {
        tunnelStatus[id] = status;
        if (tunnelsChanged)
            tunnelsChanged();
    }

    void startTunnel(const QString &id)
    {
        const int index = tunnelIndex(id);
        if (index < 0 || running.contains(id))
            return;
        const auto tunnel = tunnels[index].toObject();
        const auto profile = profileById(tunnel["session"].toString());
        if (profile.isEmpty())
        {
            setTunnelStatus(id, L("The SSH session of this tunnel no longer exists."));
            return;
        }
        const auto command = tunnelCommand(tunnel, profile, platform());
        if (!command.error.isEmpty())
        {
            setTunnelStatus(id, describe(command.error));
            return;
        }
        const auto program = QStandardPaths::findExecutable(command.program);
        if (program.isEmpty())
        {
            setTunnelStatus(id, L("Client not found: %1").arg(command.program));
            return;
        }
        auto *process = hideConsoleWindow(new QProcess(this));
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto &entry : askpassEnvironment(credentialTarget(profile, QCoreApplication::applicationName()), true))
            environment.insert(entry.section('=', 0, 0), entry.section('=', 1));
        process->setProcessEnvironment(environment);
        process->setProcessChannelMode(QProcess::MergedChannels);
        running.insert(id, process);
        connect(process, &QProcess::started, this, [this, id] { setTunnelStatus(id, L("Running")); });
        connect(process, &QProcess::errorOccurred, this,
                [this, id, process](QProcess::ProcessError error)
                {
                    if (error == QProcess::FailedToStart)
                    {
                        running.remove(id);
                        process->deleteLater();
                        setTunnelStatus(id, process->errorString());
                    }
                });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, id, process](int code, QProcess::ExitStatus)
                {
                    const auto output = QString::fromUtf8(process->readAll()).trimmed();
                    const bool stopped = process->property("stopping").toBool();
                    running.remove(id);
                    process->deleteLater();
                    setTunnelStatus(id, stopped ? L("Stopped") : L("Stopped with error %1: %2").arg(code).arg(output.right(300)));
                });
        setTunnelStatus(id, L("Starting…"));
        process->start(program, command.arguments);
    }

    void stopTunnel(const QString &id)
    {
        if (auto *process = running.value(id))
        {
            process->setProperty("stopping", true);
            process->kill();
            process->waitForFinished(3000);
        }
    }

public:
    void startAutostartTunnels()
    {
        for (const auto &v : tunnels)
        {
            if (v.toObject()["autostart"].toBool())
                startTunnel(v.toObject()["id"].toString());
        }
    }

private:
    bool editTunnel(QJsonObject &tunnel)
    {
        QDialog dialog(this);
        dialog.setWindowTitle(L("SSH tunnel"));
        auto *form = new QFormLayout(&dialog);
        auto *name = new QLineEdit(tunnel["name"].toString());
        auto *type = new QComboBox;
        type->addItem(L("Local port forwarding"), "local");
        type->addItem(L("Remote port forwarding"), "remote");
        type->addItem(L("Dynamic port forwarding (SOCKS proxy)"), "dynamic");
        type->setCurrentIndex(std::max(0, type->findData(tunnel["type"].toString("local"))));
        auto *listen = new QSpinBox;
        listen->setRange(1, 65535);
        listen->setValue(tunnel["listenPort"].toInt(8080));
        auto *destination = new QLineEdit(tunnel["destHost"].toString("127.0.0.1"));
        auto *destPort = new QSpinBox;
        destPort->setRange(1, 65535);
        destPort->setValue(tunnel["destPort"].toInt(80));
        auto *server = new QComboBox;
        for (const auto &v : profiles)
        {
            const auto p = v.toObject();
            if (p["protocol"].toString() == "ssh" || p["protocol"].toString() == "sftp")
                server->addItem(icon("ssh", 16), p["name"].toString(), p["id"].toString());
        }
        server->setCurrentIndex(std::max(0, server->findData(tunnel["session"].toString())));
        auto *autostart = new QCheckBox(L("Start this tunnel when CrossTerm starts"));
        autostart->setChecked(tunnel["autostart"].toBool());
        auto *listenLabel = new QLabel;
        form->addRow(L("Name"), name);
        form->addRow(L("Type"), type);
        form->addRow(listenLabel, listen);
        form->addRow(L("Destination host"), destination);
        form->addRow(L("Destination port"), destPort);
        form->addRow(L("SSH server"), server);
        form->addRow(autostart);
        auto update = [=]
        {
            const auto kind = type->currentData().toString();
            listenLabel->setText(kind == "remote" ? L("Port on the SSH server") : L("Local port"));
            destination->setEnabled(kind != "dynamic");
            destPort->setEnabled(kind != "dynamic");
        };
        connect(type, &QComboBox::currentIndexChanged, &dialog, update);
        update();
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog,
                [&]
                {
                    QJsonObject next{{"id", tunnel["id"].toString(QUuid::createUuid().toString(QUuid::WithoutBraces))},
                                     {"name", name->text().trimmed()},
                                     {"type", type->currentData().toString()},
                                     {"listenPort", listen->value()},
                                     {"destHost", destination->text().trimmed()},
                                     {"destPort", destPort->value()},
                                     {"session", server->currentData().toString()},
                                     {"autostart", autostart->isChecked()}};
                    if (next["name"].toString().isEmpty())
                        next["name"] = QString("%1 → %2").arg(listen->value()).arg(server->currentText());
                    const auto check = tunnelCommand(next, profileById(next["session"].toString()), platform());
                    if (server->count() == 0 || !check.error.isEmpty())
                    {
                        QMessageBox::warning(&dialog, L("SSH tunnel"),
                                             server->count() ? describe(check.error) : L("Create an SSH session first."));
                        return;
                    }
                    tunnel = next;
                    dialog.accept();
                });
        return dialog.exec() == QDialog::Accepted;
    }

    void tunnelDialog()
    {
        QDialog dialog(this);
        dialog.setWindowTitle(L("SSH tunnels"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *list = new QTreeWidget;
        list->setRootIsDecorated(false);
        list->setHeaderLabels({L("Name"), L("Type"), L("Forwarded port"), L("Destination"), L("SSH server"), L("Status")});
        list->setColumnWidth(0, 170);
        layout->addWidget(list, 1);
        auto *row = new QHBoxLayout;
        auto *add = new QPushButton(icon("plus", 16), L("New tunnel"));
        auto *change = new QPushButton(L("Edit"));
        auto *erase = new QPushButton(icon("delete", 16), L("Delete"));
        auto *start = new QPushButton(icon("ok", 16), L("Start"));
        auto *stop = new QPushButton(L("Stop"));
        auto *startAll = new QPushButton(L("Start all"));
        auto *stopAll = new QPushButton(L("Stop all"));
        for (auto *b : {add, change, erase, start, stop, startAll, stopAll})
            row->addWidget(b);
        row->addStretch();
        layout->addLayout(row);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        auto selected = [list]
        { return list->currentItem() ? list->currentItem()->data(0, Qt::UserRole).toString() : QString(); };
        auto fill = [=]
        {
            const auto keep = selected();
            list->clear();
            const QHash<QString, QString> kinds{
                {"local", L("Local")}, {"remote", L("Remote")}, {"dynamic", L("Dynamic (SOCKS)")}};
            for (const auto &v : tunnels)
            {
                const auto t = v.toObject();
                const auto id = t["id"].toString();
                const bool dynamic = t["type"].toString() == "dynamic";
                auto *item = new QTreeWidgetItem(
                    list, {t["name"].toString(), kinds.value(t["type"].toString()), QString::number(t["listenPort"].toInt()),
                           dynamic ? QString("-") : QString("%1:%2").arg(t["destHost"].toString()).arg(t["destPort"].toInt()),
                           profileById(t["session"].toString())["name"].toString(),
                           tunnelStatus.value(id, running.contains(id) ? L("Running") : L("Stopped"))});
                item->setData(0, Qt::UserRole, id);
                item->setIcon(0, icon(running.contains(id) ? "ok" : "tunneling", 16));
                if (id == keep)
                    list->setCurrentItem(item);
            }
        };
        tunnelsChanged = fill;
        auto persist = [this] { save(profiles); };
        connect(add, &QPushButton::clicked, &dialog,
                [=]
                {
                    QJsonObject tunnel;
                    if (editTunnel(tunnel))
                    {
                        tunnels.append(tunnel);
                        persist();
                        fill();
                    }
                });
        connect(change, &QPushButton::clicked, &dialog,
                [=]
                {
                    const int index = tunnelIndex(selected());
                    if (index < 0)
                        return;
                    auto tunnel = tunnels[index].toObject();
                    if (editTunnel(tunnel))
                    {
                        const bool wasRunning = running.contains(tunnel["id"].toString());
                        stopTunnel(tunnel["id"].toString());
                        tunnels.replace(index, tunnel);
                        persist();
                        if (wasRunning)
                            startTunnel(tunnel["id"].toString());
                        fill();
                    }
                });
        connect(erase, &QPushButton::clicked, &dialog,
                [=, &dialog]
                {
                    const int index = tunnelIndex(selected());
                    if (index < 0 ||
                        QMessageBox::question(&dialog, L("SSH tunnels"), L("Delete the selected tunnel?")) != QMessageBox::Yes)
                        return;
                    stopTunnel(selected());
                    tunnels.removeAt(index);
                    persist();
                    fill();
                });
        connect(start, &QPushButton::clicked, &dialog, [=] { startTunnel(selected()); });
        connect(stop, &QPushButton::clicked, &dialog, [=] { stopTunnel(selected()); });
        connect(startAll, &QPushButton::clicked, &dialog,
                [=]
                {
                    for (const auto &v : tunnels)
                        startTunnel(v.toObject()["id"].toString());
                });
        connect(stopAll, &QPushButton::clicked, &dialog,
                [=]
                {
                    for (const auto &id : running.keys())
                        stopTunnel(id);
                });
        connect(list, &QTreeWidget::itemDoubleClicked, &dialog, [change] { change->click(); });
        fill();
        dialog.resize(820, 360);
        dialog.setGeometry(fitToArea(dialog.geometry(), screen()->availableGeometry()));
        dialog.exec();
        tunnelsChanged = nullptr;
    }

    void showFind()
    {
        findBar->show();
        findText->setFocus();
        findText->selectAll();
    }

    void hideFind()
    {
        findBar->hide();
        if (current()->terminal)
            current()->terminal->setFocus();
    }

    bool doFind(bool backward)
    {
        auto *t = current()->terminal;
        const bool found =
            t && t->find(findText->text(), backward, findCase->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive);
        findStatus->setText(found || findText->text().isEmpty() ? QString() : L("Not found"));
        return found;
    }

    // --- Macros: key sequences recorded from a terminal and replayed in the active one.
    void startRecording()
    {
        recording = true;
        recordingSteps = {};
        layoutPages();
    }

    bool stopRecording(const QString &name)
    {
        recording = false;
        layoutPages();
        if (name.trimmed().isEmpty() || recordingSteps.isEmpty())
            return false;
        macros.append(QJsonObject{
            {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"name", name.trimmed()}, {"steps", recordingSteps}});
        recordingSteps = {};
        save(profiles);
        refreshMacros();
        return true;
    }

    void playMacro(int index, Terminal *target)
    {
        if (!target || index < 0 || index >= macros.size() || target->finished())
            return;
        playing = true;
        QPointer<Terminal> guard(target);
        for (const auto &v : macros[index].toObject()["steps"].toArray())
        {
            const auto step = v.toObject();
            if (step.contains("sleep")) // pause step in macros imported from MobaXterm
            {
                QEventLoop pause;
                QTimer::singleShot(std::clamp(step["sleep"].toInt(), 0, 60000), &pause, &QEventLoop::quit);
                pause.exec();
                if (!guard || target->finished())
                    break;
                continue;
            }
            QKeyEvent key(QEvent::KeyPress, step["key"].toInt(), Qt::KeyboardModifiers(step["modifiers"].toInt()),
                          step["text"].toString());
            QApplication::sendEvent(target, &key);
        }
        playing = false;
        target->setFocus();
    }

    void refreshMacros()
    {
        macroList->clear();
        for (const auto &v : macros)
            new QListWidgetItem(icon("macros", 16), v.toObject()["name"].toString(), macroList);
        macroHint->setVisible(macros.isEmpty());
    }

    void macroMenu(QMenu *menu)
    {
        menu->clear();
        if (recording)
            menu->addAction(L("Stop recording"), this, [this] { stopRecordingDialog(); });
        else
            menu->addAction(icon("macros", 16), L("Record new macro"), this, [this] { startRecording(); });
        menu->addSeparator();
        if (macros.isEmpty())
            menu->addAction(L("No macro recorded"))->setEnabled(false);
        for (int i = 0; i < macros.size(); ++i)
            menu->addAction(macros[i].toObject()["name"].toString(), this, [this, i] { playMacro(i, current()->terminal); });
    }

    void stopRecordingDialog()
    {
        const auto name = QInputDialog::getText(this, L("Macros"), L("Macro name:")).trimmed();
        if (!stopRecording(name) && !name.isEmpty())
            QMessageBox::information(this, L("Macros"), L("No keys were recorded."));
    }

    // --- Restore the sessions that were open at the last exit.
    QStringList openSessionIds() const
    {
        QStringList ids;
        for (int i = 0; i < bar->count(); ++i)
        {
            const auto *p = page(i);
            if (p == home || !p->terminal)
                continue;
            if (p->profile.contains("id"))
                ids << p->profile["id"].toString();
            else if (p->profile.isEmpty())
                ids << "local";
        }
        return ids;
    }

    void recoverSessions(const QStringList &ids)
    {
        for (const auto &id : ids)
        {
            if (id == "local")
            {
                terminal(localShell(), L("Local terminal"));
                continue;
            }
            for (int i = 0; i < profiles.size(); ++i)
            {
                if (profiles[i].toObject()["id"].toString() == id)
                    open(i);
            }
        }
        QSettings().remove("lastSessions");
        recoverButton->hide();
    }

    // --- Terminal output: manual save and automatic logging.
    bool saveOutput(Terminal *t, const QString &file)
    {
        QSaveFile out(file);
        const auto bytes = t->allText().toUtf8() + "\n";
        return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size() && out.commit();
    }

    void startLog(Terminal *t, const QString &name)
    {
        QSettings settings;
        if (!settings.value("log/enabled").toBool())
            return;
        const auto folder = settings.value("log/folder").toString();
        if (folder.isEmpty() || !QDir().mkpath(folder))
            return;
        auto safe = name;
        safe.replace(QRegularExpression("[^\\w.-]+"), "_");
        t->setLogFile(QDir(folder).filePath(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + "_" + safe + ".log"));
    }

    // --- Favorites.
    void toggleFavorite(int i)
    {
        if (i < 0 || i >= profiles.size())
            return;
        auto next = profiles;
        auto p = next[i].toObject();
        p["favorite"] = !p["favorite"].toBool();
        next.replace(i, p);
        save(next);
    }

    // Names shown in the selector (extracted for translation).
    static void appearanceNames()
    {
        (void)N("Classic dark"), (void)N("Classic light"), (void)N("macOS light"), (void)N("macOS dark");
    }

    void applyTheme(bool light) { applyAppearance(light ? Appearance::ClassicLight : Appearance::ClassicDark); }

    void applyThemeKey(const QString &key) { applyAppearance(appearanceFromKey(key, systemDark()), key); }

    void applyAppearance(Appearance look, const QString &key = {})
    {
        themeKey = key.isEmpty() ? appearanceKey(look) : key;
        QSettings().setValue("theme", themeKey);
        appearance = look;
        applyAppearanceFont(look);
        qApp->setStyleSheet(appearanceStyleSheet(look, darkStyle));
        const auto palette = appearancePalette(look);
        if (auto *tools = findChild<QToolBar *>("main"))
        {
            tools->setIconSize(QSize(palette.toolbarIconSize, palette.toolbarIconSize));
            tools->setToolButtonStyle(palette.textUnderIcon ? Qt::ToolButtonTextUnderIcon : Qt::ToolButtonIconOnly);
        }
        for (const auto &[widget, dark] : inlineStyles)
        {
            if (widget)
                widget->setStyleSheet(themed(dark));
        }
        bar->update();
    }

    void applyScheme(const QString &name)
    {
        QSettings().setValue("terminal/scheme", name);
        for (int i = 0; i < bar->count(); ++i)
        {
            if (auto *t = page(i)->terminal)
                t->applyScheme();
        }
    }

    // --- Configurable shortcuts: QSettings "shortcuts/<source text>" (empty string = none).
    void applyShortcuts()
    {
        QSettings settings;
        for (auto *a : shortcutActions)
        {
            const auto key = "shortcuts/" + a->objectName();
            a->setShortcut(settings.contains(key) ? QKeySequence(settings.value(key).toString())
                                                  : defaultShortcuts.value(a->objectName()));
        }
    }

public:
    // Sets a shortcut; returns the name of the conflicting action (empty if none).
    QString setShortcut(const QString &id, const QKeySequence &keys)
    {
        for (auto *a : shortcutActions)
        {
            if (!keys.isEmpty() && a->objectName() != id && a->shortcut() == keys)
                return a->text();
        }
        QSettings().setValue("shortcuts/" + id, keys.toString(QKeySequence::PortableText));
        applyShortcuts();
        return {};
    }
    QKeySequence shortcutOf(const QString &id) const
    {
        for (auto *a : shortcutActions)
        {
            if (a->objectName() == id)
                return a->shortcut();
        }
        return {};
    }
    void restoreDefaultShortcuts()
    {
        QSettings().remove("shortcuts");
        applyShortcuts();
    }

private:
    void shortcutsDialog()
    {
        QDialog dialog(this);
        dialog.setWindowTitle(L("Keyboard shortcuts"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *table = new QTableWidget(int(shortcutActions.size()), 2);
        table->setHorizontalHeaderLabels({L("Action"), L("Shortcut")});
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->verticalHeader()->hide();
        QList<QKeySequenceEdit *> editors;
        for (int row = 0; row < shortcutActions.size(); ++row)
        {
            auto *a = shortcutActions[row];
            auto *name = new QTableWidgetItem(a->icon(), a->text().remove('&'));
            name->setFlags(Qt::ItemIsEnabled);
            table->setItem(row, 0, name);
            auto *edit = new QKeySequenceEdit(a->shortcut());
            edit->setClearButtonEnabled(true);
            table->setCellWidget(row, 1, edit);
            editors << edit;
        }
        layout->addWidget(table, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog,
                [&]
                {
                    for (int row = 0; row < shortcutActions.size(); ++row)
                        editors[row]->setKeySequence(defaultShortcuts.value(shortcutActions[row]->objectName()));
                });
        connect(buttons, &QDialogButtonBox::accepted, &dialog,
                [&]
                {
                    QHash<QString, int> used;
                    for (int row = 0; row < editors.size(); ++row)
                    {
                        const auto keys = editors[row]->keySequence().toString(QKeySequence::PortableText);
                        if (keys.isEmpty())
                            continue;
                        if (used.contains(keys))
                        {
                            QMessageBox::warning(&dialog, L("Keyboard shortcuts"),
                                                 L("%1 is assigned to two actions.")
                                                     .arg(editors[row]->keySequence().toString(QKeySequence::NativeText)));
                            return;
                        }
                        used.insert(keys, row);
                    }
                    QSettings settings;
                    for (int row = 0; row < editors.size(); ++row)
                    {
                        const auto id = shortcutActions[row]->objectName();
                        const auto keys = editors[row]->keySequence();
                        if (keys == defaultShortcuts.value(id))
                            settings.remove("shortcuts/" + id);
                        else
                            settings.setValue("shortcuts/" + id, keys.toString(QKeySequence::PortableText));
                    }
                    applyShortcuts();
                    dialog.accept();
                });
        dialog.resize(560, 520);
        dialog.setGeometry(fitToArea(dialog.geometry(), screen()->availableGeometry()));
        dialog.exec();
    }

public:
    // --- Sessions opened automatically at startup (profile "autostart" option).
    int startAutostartSessions()
    {
        int opened = 0;
        for (int i = 0; i < profiles.size(); ++i)
        {
            if (profiles[i].toObject()["autostart"].toBool())
            {
                open(i);
                ++opened;
            }
        }
        return opened;
    }

private:
    // --- Built-in X server (VcXsrv in dist\\xserver or an installed server).
public:
    bool startXServer()
    {
        if (xserver.isRunning())
            return true;
        for (const auto &server : detectXServers())
        {
            if (xserver.start(server, "multiwindow"))
                return true;
        }
        return false;
    }

    QString xDisplay() const { return xserver.isRunning() ? xserver.displayVariable() : qEnvironmentVariable("DISPLAY"); }

private:
    void xserverStatus()
    {
        const auto servers = detectXServers();
        QString text;
        if (xserver.isRunning())
            text = L("X server running on display %1.").arg(xserver.displayVariable());
        else if (servers.isEmpty())
            text = L("No X server found. Put VcXsrv in the xserver folder next to CrossTerm or install one.");
        else
            text = L("X server stopped. Available: %1").arg(servers.first().name);
        if (!xserver.error().isEmpty())
            text += "\n" + xserver.error();
        QMessageBox::information(this, L("X server"), text);
    }

    void xdmcpSession()
    {
        bool ok = false;
        const auto host =
            QInputDialog::getText(this, L("XDMCP session"), L("Remote host (empty: broadcast on the local network):"),
                                  QLineEdit::Normal, {}, &ok)
                .trimmed();
        if (!ok)
            return;
        if (!host.isEmpty() && !validHost(host))
        {
            QMessageBox::warning(this, L("XDMCP session"), describe("Indirizzo o porta da verificare."));
            return;
        }
        xdmcp.stop();
        const auto servers = detectXServers();
        if (servers.isEmpty() || !xdmcp.start(servers.first(), host.isEmpty() ? "broadcast" : "xdmcp", host))
            QMessageBox::warning(this, L("XDMCP session"), servers.isEmpty() ? L("No X server found.") : xdmcp.error());
    }

    void retryLogins()
    {
        for (int i = 0; i < bar->count(); ++i)
        {
            auto *p = page(i);
            if (!p || !p->sftp)
                continue;
            p->sftp->retryAutomatic();
            if (!p->monitorError.isEmpty() && p->sftp->monitoring->isChecked())
                toggleMonitor(p, true);
        }
    }

    void toggleMonitor(Page *p, bool on)
    {
        delete p->monitor;
        p->monitor = nullptr;
        p->stats = {};
        p->monitorError.clear();
        if (on)
        {
            auto ssh = p->profile;
            ssh["protocol"] = "ssh";
            ssh["x11"] = false;
            const auto command = sessionCommand(ssh, platform(), false);
            p->monitor = new RemoteMonitor(
                command, askpassEnvironment(credentialTarget(ssh, QCoreApplication::applicationName()), false), 5, this);
            p->monitor->updated = [this, p](const MonitorStats &stats)
            {
                p->stats = stats;
                p->monitorError.clear();
                if (current() == p)
                    monitorBar->show(stats);
            };
            p->monitor->failed = [this, p](const QString &message)
            {
                p->monitorError = message;
                if (current() == p)
                    monitorBar->showError(message);
            };
            p->monitor->start();
        }
        layoutPages();
    }

public:
    MonitorStats monitorStats(int tab) const
    {
        auto *p = page(tab);
        return p ? p->stats : MonitorStats{};
    }
    void setMonitoring(int tab, bool on)
    {
        if (auto *p = page(tab); p && p->sftp)
            p->sftp->monitoring->setChecked(on);
    }

private:
public:
    ServersDialog *showServers()
    {
        if (!serversDialog)
        {
            serversDialog = new ServersDialog(this);
            serversDialog->setFtpControl(
                [this](const QString &root, quint16 port, bool allowWrite, bool bindAll)
                {
                    ftpServer.bindAllInterfaces = bindAll;
                    // FTPS if a PEM certificate and key are set in Configuration.
                    ftpServer.certificateFile = QSettings().value("servers/ftpCertificate").toString();
                    ftpServer.privateKeyFile = QSettings().value("servers/ftpKey").toString();
                    if (!allowWrite)
                        return ftpServer.start(root, port, true); // anonymous, read-only
                    // Uploads enabled: dedicated user with a random password, shown only once.
                    const auto password = QString::number(QRandomGenerator::system()->generate64(), 36);
                    if (!ftpServer.start(root, port, false, "crossterm", password))
                        return false;
                    QMessageBox::information(serversDialog, L("FTP server"),
                                             L("User: %1\nPassword: %2").arg("crossterm", password));
                    return true;
                },
                [this] { ftpServer.stop(); }, [this] { return ftpServer.isRunning(); });
        }
        serversDialog->refresh();
        serversDialog->show();
        serversDialog->raise();
        return serversDialog;
    }

private:
    void renameTab(int index)
    {
        auto *p = page(index);
        if (!p || p == home)
            return;
        const auto now = bar->tabText(index).section(". ", 1);
        const auto text = QInputDialog::getText(this, L("Rename tab"), L("Tab name:"), QLineEdit::Normal, now).trimmed();
        if (text.isEmpty())
            return;
        bar->setTabText(index, bar->tabText(index).section(". ", 0, 0) + ". " + text);
        layoutPages();
    }

    void closeAll(Page *except)
    {
        for (int i = bar->count() - 1; i >= 0; --i)
        {
            if (page(i) != except)
                closePage(page(i));
        }
    }

    void setLanguage(const QString &code)
    {
        QSettings().setValue("language", code);
        applyLanguage(code.isEmpty() ? systemLanguage() : code);
    }

    void retranslate()
    {
        texts.apply();
        for (const auto &f : retranslators)
            f();
        for (int i = 0; i < sftpStack->count(); ++i)
        {
            if (auto *panel = dynamic_cast<SftpPanel *>(sftpStack->widget(i)))
                panel->retranslate();
        }
        for (int i = 0; i < bar->count(); ++i)
        {
            if (auto *ftp = dynamic_cast<FtpPanel *>(page(i)->widget))
                ftp->retranslate();
        }
        refresh();
        layoutPages();
        // Labels change width: the toolbar must recompute the button sizes.
        for (auto *button : findChildren<QToolButton *>())
            button->updateGeometry();
        for (auto *bar : findChildren<QToolBar *>())
            bar->layout()->invalidate();
    }

    // Fits the window to the work area of its monitor (excluding the taskbar).
    void fitToScreen()
    {
        if (isMaximized() || isFullScreen())
            return;
        auto *screen = QGuiApplication::screenAt(frameGeometry().center());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        const auto frame = frameGeometry();
        const auto fitted = fitToArea(frame, screen->availableGeometry());
        if (fitted == frame)
            return;
        const QSize margins = frame.size() - size();
        resize(fitted.size() - margins);
        move(fitted.topLeft());
    }

public:
    void restoreWindow()
    {
        const auto geometry = QSettings().value("window/geometry").toByteArray();
        if (geometry.isEmpty() || !restoreGeometry(geometry))
            resize(1200, 740);
        fitToScreen();
    }

private:
    void restoreBackup()
    {
        const auto backup = storage + ".before-import";
        QFile file(backup);
        if (!file.open(QIODevice::ReadOnly))
            return;
        const auto doc = QJsonDocument::fromJson(file.readAll()).object();
        if (doc["version"].toInt() != 1 || !doc["sessions"].isArray())
        {
            QMessageBox::warning(this, L("Undo last import"), L("The backup file is damaged."));
            return;
        }
        if (QMessageBox::question(this, L("Undo last import"), L("Restore the sessions as they were before the last import?")) !=
            QMessageBox::Yes)
            return;
        file.close();
        if (doc.contains("tunnels"))
            tunnels = doc["tunnels"].toArray();
        if (doc.contains("macros"))
        {
            macros = doc["macros"].toArray();
            refreshMacros();
        }
        if (save(doc["sessions"].toArray()))
        {
            QFile::remove(backup);
            undoImport->setEnabled(false);
        }
    }

    static QStringList mobaCandidates()
    {
        QStringList paths{
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/MobaXterm/MobaXterm.ini",
            qEnvironmentVariable("APPDATA") + "/MobaXterm/MobaXterm.ini",
            qEnvironmentVariable("ProgramFiles(x86)") + "/Mobatek/MobaXterm/MobaXterm.ini",
            qEnvironmentVariable("ProgramFiles") + "/Mobatek/MobaXterm/MobaXterm.ini",
        };
        QStringList found;
        for (const auto &path : paths)
        {
            if (!path.startsWith('/') && QFileInfo(path).isFile())
                found << QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
        }
        found.removeDuplicates();
        return found;
    }

    // Applies the MobaXterm extras: settings (QSettings), macros and tunnels. Tunnel sessions are
    // relinked to the final profiles by host, port and user; servers not found become new sessions.
    QJsonArray applyExtras(QJsonArray next, const MobaExtras &extras, bool settings, bool withMacros, bool withTunnels)
    {
        if (settings)
        {
            QSettings store;
            for (auto it = extras.settings.begin(); it != extras.settings.end(); ++it)
                store.setValue(it.key(), it.value().toVariant());
            applyScheme(store.value("terminal/scheme", "CrossTerm Dark").toString());
        }
        if (withMacros)
        {
            for (const auto &m : extras.macros)
            {
                const bool exists = std::any_of(macros.begin(), macros.end(), [&](const QJsonValue &v)
                                                { return v.toObject()["name"] == m.toObject()["name"]; });
                if (!exists)
                    macros.append(m);
            }
            refreshMacros();
        }
        if (withTunnels)
        {
            next = applyImport(next, extras.tunnelServers, ImportPolicy::Merge);
            QJsonArray sources = extras.tunnelServers;
            for (const auto &v : lastImported)
                sources.append(v);
            auto endpoint = [](const QJsonObject &p)
            {
                return (p["host"].toString() + ":" + QString::number(p["port"].toInt()) + "@" + p["user"].toString())
                    .toCaseFolded();
            };
            for (const auto &v : extras.tunnels)
            {
                auto tunnel = v.toObject();
                QString target;
                for (const auto &s : sources)
                {
                    if (s.toObject()["id"] == tunnel["session"])
                        target = endpoint(s.toObject());
                }
                for (const auto &candidate : next)
                {
                    if (!target.isEmpty() && endpoint(candidate.toObject()) == target)
                        tunnel["session"] = candidate.toObject()["id"];
                }
                const bool exists = std::any_of(tunnels.begin(), tunnels.end(),
                                                [&](const QJsonValue &t) { return t.toObject()["name"] == tunnel["name"]; });
                if (!exists)
                    tunnels.append(tunnel);
            }
        }
        return next;
    }
    QJsonArray lastImported; // sessions read by the last wizard run, to relink tunnels

    // Wizard: detection, preview, selection, duplicate handling, backup and undo.
    // Reads the source read-only. MobaXterm-protected passwords are not decrypted.
    void importWizard(const QString &preset = {})
    {
        QDialog dialog(this);
        dialog.setWindowTitle(L("Import from MobaXterm"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *sourceRow = new QHBoxLayout;
        auto *source = new QComboBox;
        source->setEditable(true);
        source->addItems(mobaCandidates());
        if (!preset.isEmpty())
            source->setCurrentText(preset);
        auto *browse = new QPushButton(L("Browse..."));
        auto *load = new QPushButton(L("Load"));
        sourceRow->addWidget(new QLabel(L("Source file")));
        sourceRow->addWidget(source, 1);
        sourceRow->addWidget(browse);
        sourceRow->addWidget(load);
        layout->addLayout(sourceRow);
        auto *summary = new QLabel(source->count() ? QString() : L("No MobaXterm installation found. Choose a file."));
        summary->setWordWrap(true);
        layout->addWidget(summary);
        auto *preview = new QTreeWidget;
        preview->setHeaderLabels({L("Session"), L("Protocol"), L("Host"), L("User"), L("Status")});
        preview->setColumnWidth(0, 260);
        layout->addWidget(preview, 1);
        auto *policyRow = new QHBoxLayout;
        auto *policy = new QComboBox;
        policy->addItem(L("Skip sessions that already exist"), int(ImportPolicy::Skip));
        policy->addItem(L("Merge missing details into existing sessions"), int(ImportPolicy::Merge));
        policy->addItem(L("Replace existing sessions"), int(ImportPolicy::Replace));
        policy->setCurrentIndex(1);
        policyRow->addWidget(new QLabel(L("Existing sessions")));
        policyRow->addWidget(policy, 1);
        layout->addLayout(policyRow);
        // MobaXterm extras: terminal preferences, macros and SSH tunnels ([Font], [Colors], [Macros], [PortForwarding]).
        MobaExtras extras;
        auto *importSettings = new QCheckBox(L("Import terminal settings (font, colors, right-click paste)"));
        auto *importMacros = new QCheckBox;
        auto *importTunnels = new QCheckBox;
        for (auto *box : {importSettings, importMacros, importTunnels})
        {
            box->setChecked(true);
            box->hide();
            layout->addWidget(box);
        }
        auto *note = new QLabel(
            L("The source file is only read, never modified. A backup of your sessions is kept so you can undo the import."));
        note->setWordWrap(true);
        note->setObjectName("empty");
        layout->addWidget(note);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        auto *run = buttons->addButton(L("Import"), QDialogButtonBox::AcceptRole);
        run->setEnabled(false);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        Import imported;
        auto identity = [](const QJsonObject &p)
        { return (p["name"].toString() + "\n" + p["folder"].toString() + "\n" + p["protocol"].toString()).toCaseFolded(); };
        auto fill = [&]
        {
            preview->clear();
            QSet<QString> existing;
            for (const auto &v : profiles)
                existing << identity(v.toObject());
            QMap<QString, QTreeWidgetItem *> folders;
            for (int i = 0; i < imported.profiles.size(); ++i)
            {
                const auto p = imported.profiles[i].toObject();
                const auto folder = p["folder"].toString();
                auto *&parent = folders[folder];
                if (!parent)
                {
                    parent = new QTreeWidgetItem(preview, {folder.isEmpty() ? L("User sessions") : folder});
                    parent->setIcon(0, icon("folder"));
                    parent->setFlags(parent->flags() | Qt::ItemIsAutoTristate | Qt::ItemIsUserCheckable);
                    parent->setCheckState(0, Qt::Checked);
                }
                QString state = existing.contains(identity(p)) ? L("Already exists") : L("New");
                if (!p["issues"].toArray().isEmpty())
                    state += " - " + L("needs review");
                auto *row = new QTreeWidgetItem(parent, {p["name"].toString(), p["protocol"].toString().toUpper(),
                                                         p["host"].toString(), p["user"].toString(), state});
                row->setIcon(0, icon(p["protocol"].toString()));
                row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
                row->setCheckState(0, Qt::Checked);
                row->setData(0, Qt::UserRole, i);
            }
            preview->expandAll();
        };
        auto read = [&]
        {
            const auto name = source->currentText().trimmed();
            QFile file(name);
            if (name.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
            {
                summary->setText(L("Cannot read the file, or it is larger than 16 MB."));
                run->setEnabled(false);
                return;
            }
            const auto raw = file.readAll();
            imported = readForeign(raw, name);
            extras = name.endsWith(".csv", Qt::CaseInsensitive) ? MobaExtras{} : parseMobaExtras(raw, imported.profiles);
            importSettings->setVisible(!extras.settings.isEmpty());
            importMacros->setVisible(!extras.macros.isEmpty());
            importTunnels->setVisible(!extras.tunnels.isEmpty());
            importMacros->setText(L("Import %1 macro(s)").arg(extras.macros.size()));
            importTunnels->setText(L("Import %1 SSH tunnel(s)").arg(extras.tunnels.size()));
            QStringList lines{L("%1 session(s) found.").arg(imported.profiles.size())};
            if (imported.accounts)
                lines << L("%1 saved account(s): user names are imported.").arg(imported.accounts);
            if (imported.secrets)
                lines
                    << L("%1 saved password(s) are protected by MobaXterm. After the import, CrossTerm asks for your MobaXterm master password to move them into the system vault.")
                           .arg(imported.secrets);
            for (const auto &w : imported.warnings)
                lines << describe(w);
            summary->setText(lines.join('\n'));
            fill();
            run->setEnabled(!imported.profiles.isEmpty());
        };
        connect(browse, &QPushButton::clicked, &dialog,
                [&]
                {
                    const auto file = QFileDialog::getOpenFileName(
                        &dialog, L("Import sessions"), source->currentText(),
                        L("MobaXterm or CSV files (*.ini *.mxtsessions *.moba *.mobaconf *.csv *.xml *.reg *.xs *.dat)"));
                    if (!file.isEmpty())
                    {
                        source->setCurrentText(QDir::toNativeSeparators(file));
                        read();
                    }
                });
        connect(load, &QPushButton::clicked, &dialog, read);
        connect(source, &QComboBox::activated, &dialog, read);
        connect(
            run, &QPushButton::clicked, &dialog,
            [&]
            {
                QJsonArray chosen;
                lastImported = imported.profiles;
                QTreeWidgetItemIterator it(preview);
                for (; *it; ++it)
                {
                    if ((*it)->parent() && (*it)->checkState(0) == Qt::Checked)
                        chosen.append(imported.profiles[(*it)->data(0, Qt::UserRole).toInt()]);
                }
                const auto next = applyImport(profiles, chosen, ImportPolicy(policy->currentData().toInt()));
                if (next == profiles && !((importSettings->isVisible() && importSettings->isChecked()) ||
                                          (importMacros->isVisible() && importMacros->isChecked()) ||
                                          (importTunnels->isVisible() && importTunnels->isChecked())))
                {
                    importPasswords(&dialog, imported, chosen);
                    QMessageBox::information(&dialog, L("Import"), L("The selected sessions already exist."));
                    return;
                }
                // Backup before modifying, for "Undo last import".
                QSaveFile backup(storage + ".before-import");
                const auto bytes =
                    QJsonDocument(QJsonObject{{"version", 1}, {"sessions", profiles}, {"tunnels", tunnels}, {"macros", macros}})
                        .toJson();
                if (!writable || !backup.open(QIODevice::WriteOnly) || backup.write(bytes) != bytes.size() || !backup.commit())
                {
                    QMessageBox::warning(&dialog, L("Import"), L("Cannot create the backup; nothing was imported."));
                    return;
                }
                const auto before = profiles.size();
                if (!save(applyExtras(next, extras, importSettings->isVisible() && importSettings->isChecked(),
                                      importMacros->isVisible() && importMacros->isChecked(),
                                      importTunnels->isVisible() && importTunnels->isChecked())))
                    return;
                undoImport->setEnabled(true);
                importPasswords(&dialog, imported, chosen);
                QMessageBox::information(&dialog, L("Import"), L("%1 new session(s).").arg(next.size() - before));
                dialog.accept();
            });
        if (source->count())
            read();
        dialog.resize(760, 520);
        dialog.setGeometry(fitToArea(dialog.geometry(), screen()->availableGeometry()));
        dialog.exec();
    }

protected:
    void changeEvent(QEvent *event) override
    {
        if (event->type() == QEvent::LanguageChange)
            retranslate();
        QMainWindow::changeEvent(event);
    }

    void closeEvent(QCloseEvent *event) override
    {
        for (const auto &id : running.keys())
            stopTunnel(id);
        if (serversDialog)
        {
            serversDialog->http().stop();
            serversDialog->tftp().stop();
            ftpServer.stop();
        }
        if (persistGeometry)
        {
            QSettings().setValue("window/geometry", saveGeometry());
            QSettings().setValue("lastSessions", openSessionIds());
        }
        QMainWindow::closeEvent(event);
    }

    // MultiExec: keys pressed in one terminal are replicated to all other open terminals.
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (recording && !playing && !forwarding && event->type() == QEvent::KeyPress && dynamic_cast<Terminal *>(watched))
        {
            const auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() < Qt::Key_Shift || key->key() > Qt::Key_Alt)
                recordingSteps.append(
                    QJsonObject{{"key", key->key()}, {"modifiers", int(key->modifiers())}, {"text", key->text()}});
        }
        if (multiExec && !forwarding && event->type() == QEvent::KeyPress && dynamic_cast<Terminal *>(watched))
        {
            const auto *key = static_cast<QKeyEvent *>(event);
            forwarding = true;
            for (int i = 0; i < bar->count(); ++i)
            {
                auto *t = page(i)->terminal;
                if (!t || t == watched || t->finished())
                    continue;
                QKeyEvent copy(QEvent::KeyPress, key->key(), key->modifiers(), key->text());
                QApplication::sendEvent(t, &copy);
            }
            forwarding = false;
        }
        return QMainWindow::eventFilter(watched, event);
    }

public:
    bool multiExecEnabled(bool on)
    {
        multiAction->setChecked(on);
        return multiExec;
    }

private:
public:
    Terminal *smokeTerminal()
    {
#ifdef Q_OS_WIN
        terminal({"cmd.exe", {"/d", "/q"}, {}}, "Test");
#else
        terminal({"/bin/sh", {}, {}}, "Test");
#endif
        return current()->terminal;
    }

    // In-process end-to-end test (no global input): opens the first profile, which must point to a test
    // SSH/SFTP server, checks the SFTP browser, Split and dialogs, and saves screenshots via grab().
    int e2e(const QString &out)
    {
        QStringList log;
        bool ok = true;
        auto check = [&](bool value, const QString &what)
        {
            log << QString(value ? "PASS " : "FAIL ") + what;
            ok &= value;
            QFile progress(out + "/e2e.txt"); // written at every step: if the test hangs, this shows where
            if (progress.open(QIODevice::WriteOnly))
                progress.write(log.join('\n').toUtf8());
        };
        auto wait = [](int ms, const std::function<bool()> &done)
        {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < ms && !done())
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            return done();
        };
        auto shot = [&](const QString &name, QWidget *w = nullptr) { (w ? w : this)->grab().save(out + "/" + name); };
        auto type = [](QWidget *w, const QString &text)
        {
            for (const auto c : text)
            {
                QKeyEvent key(QEvent::KeyPress, c == '\r' ? int(Qt::Key_Return) : int(c.toUpper().unicode()), Qt::NoModifier,
                              c == '\r' ? QString() : QString(c));
                QApplication::sendEvent(w, &key);
            }
        };
        auto listedNames = [](SftpPanel *panel)
        {
            QStringList names;
            for (int i = 0; i < panel->view()->topLevelItemCount(); ++i)
                names << panel->view()->topLevelItem(i)->text(0);
            return names;
        };
        // Prompts to re-enter passwords after an import are skipped (Cancel), as a user would do.
        QTimer skipPasswordPrompts;
        connect(&skipPasswordPrompts, &QTimer::timeout, this,
                []
                {
                    if (auto *prompt = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
                        prompt->reject();
                    // OK-only information messages (Yes/No questions are left to the individual tests).
                    if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                        box && box->standardButtons() == QMessageBox::Ok)
                        box->accept();
                });
        skipPasswordPrompts.start(250);
        monitoringByDefault = false; // the SSH test server has no /proc; the default is checked separately
        resize(1349, 819);
        wait(500, [] { return false; });
        shot("01-start.png");
        if (auto *plus = findChild<QToolButton *>("newTab"))
        {
            const int gap = plus->mapTo(this, QPoint(0, 0)).x() - bar->mapTo(this, QPoint(bar->tabRect(0).right(), 0)).x();
            check(bar->count() == 1 && gap >= 0 && gap <= 8, QString("+ button right after the Home tab (gap %1px)").arg(gap));
        }
        check(strip.value(0) && strip[0]->width() == 45 && strip[0]->iconSize() == QSize(34, 34),
              "side strip buttons at their configured size");
        {
            MonitorBar sample;
            MonitorStats stats;
            stats.host = "toxiicdev";
            stats.user = "root";
            stats.memUsed = qint64(2.62 * (1LL << 30));
            stats.memTotal = qint64(31.24 * (1LL << 30));
            stats.txPerSec = 45000;
            stats.rxPerSec = 40000;
            stats.uptimeSeconds = 22 * 86400 + 3600;
            stats.disks = {{"/", 52}, {"/boot", 6}};
            for (const double cpu : {3.0, 12.0, 7.0, 25.0, 9.0, 7.0})
            {
                stats.cpu = cpu;
                sample.show(stats);
            }
            sample.resize(sample.sizeHint());
            shot("15-monitor-bar.png", &sample);
            check(sample.findChildren<QFrame *>("monitorCell").size() == 11, "monitoring bar has its eleven cells");
        }
        open(0);
        auto *p = current();
        check(p != home && p->terminal && p->sftp, "SSH tab opens with terminal and SFTP panel");
        if (!p->sftp)
        {
            QFile f(out + "/e2e.txt");
            f.open(QIODevice::WriteOnly);
            f.write(log.join('\n').toUtf8());
            return 1;
        }
        check(side->currentIndex() == 3, "sidebar switches to SFTP");
        check(focusWidget() == p->terminal, "terminal has focus after opening");
        wait(8000,
             [&]
             {
                 const auto c = p->terminal->contents();
                 return c.contains("(yes/no") || c.contains("$ ");
             });
        if (p->terminal->contents().contains("(yes/no"))
            type(p->terminal, "yes\r");
        check(wait(8000, [&] { return p->terminal->contents().contains("test@localhost:~$"); }), "interactive SSH shell prompt");
        type(p->terminal, "hello\r");
        check(wait(3000, [&] { return p->terminal->contents().contains("you typed: hello"); }), "terminal round-trip over SSH");
        auto *panel = p->sftp;
        wait(10000, [&] { return !panel->busy(); });
        panel->run(".");
        check(wait(10000, [&] { return !panel->busy() && listedNames(panel).contains("README.md"); }),
              "SFTP listing shows README.md");
        check(panel->currentPath() == "/", "SFTP resolves absolute path");
        panel->run("/docs");
        check(wait(10000, [&] { return !panel->busy() && listedNames(panel).contains("a.txt"); }), "SFTP navigates into docs");
        panel->run("/docs/..");
        wait(10000, [&] { return !panel->busy(); });
        check(panel->currentPath() == "/", "SFTP parent navigation");
        QTemporaryDir local;
        QFile upload(local.filePath("upload-e2e.txt"));
        upload.open(QIODevice::WriteOnly);
        upload.write("crossterm upload\n");
        upload.close();
        panel->upload({upload.fileName()});
        check(wait(10000, [&] { return !panel->busy() && listedNames(panel).contains("upload-e2e.txt"); }), "SFTP upload");
        const auto target = local.filePath("download-e2e.txt");
        panel->run("/", {"get " + sftpQuote("upload-e2e.txt") + " " + sftpQuote(QDir::fromNativeSeparators(target))});
        wait(10000, [&] { return !panel->busy(); });
        QFile downloaded(target);
        check(downloaded.open(QIODevice::ReadOnly) && downloaded.readAll() == "crossterm upload\n",
              "SFTP download content matches");
        panel->run("/",
                   {"mkdir " + sftpQuote("new dir"), "rename " + sftpQuote("upload-e2e.txt") + " " + sftpQuote("renamed.txt")});
        check(wait(10000,
                   [&]
                   {
                       return !panel->busy() && listedNames(panel).contains("new dir") &&
                              listedNames(panel).contains("renamed.txt");
                   }),
              "SFTP mkdir and rename");
        panel->run("/", {"rm " + sftpQuote("renamed.txt"), "rmdir " + sftpQuote("new dir")});
        check(wait(10000,
                   [&]
                   {
                       return !panel->busy() && !listedNames(panel).contains("renamed.txt") &&
                              !listedNames(panel).contains("new dir");
                   }),
              "SFTP delete file and directory");
        shot("02-ssh-sftp.png");
        terminal(localShell(), "Local terminal");
        auto *local2 = current();
        check(bar->currentIndex() == 2 && focusWidget() == local2->terminal, "new local tab gets focus");
        check(side->currentIndex() == 3 && sftpStack->currentIndex() == 0, "SFTP sidebar shows empty state for local tab");
        if (platform() == "windows")
        {
            wait(8000, [&] { return local2->terminal->contents().contains("PS "); });
            type(local2->terminal, "Get-Dat");
            QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
            QApplication::sendEvent(local2->terminal, &tab);
            check(wait(5000, [&] { return local2->terminal->contents().contains("Get-Date"); }), "Tab completion in PowerShell");
            type(local2->terminal, "\r");
        }
        bar->setCurrentIndex(1);
        check(focusWidget() == p->terminal && sftpStack->currentWidget() == panel,
              "switching back restores focus and SFTP panel");
        splitMode = 2;
        layoutPages();
        wait(600, [] { return false; });
        check(p->terminal->isVisible() && local2->terminal->isVisible(), "split vertical shows two terminals");
        {
            auto *split = area->findChild<QSplitter *>();
            check(split && split->count() == 2 && split->orientation() == Qt::Horizontal && split->handleWidth() > 0,
                  "split view uses draggable separators");
        }
        shot("03-split-vertical.png");
        splitMode = 4;
        layoutPages();
        splitMode = 1;
        layoutPages();
        check(!local2->terminal->isVisible(), "single terminal mode restored");
        bar->moveTab(2, 0);
        check(page(0) == home, "home tab stays first");
        QTimer::singleShot(700, this,
                           [&]
                           {
                               if (auto *dialog = QApplication::activeModalWidget())
                               {
                                   shot("04-session-dialog.png", dialog);
                                   static_cast<QDialog *>(dialog)->reject();
                               }
                           });
        edit(-1);
        showSide(0);
        quick->setText("db-primary");
        check(tree->topLevelItem(0)->child(0) && tree->topLevelItem(0)->child(0)->text(0) == "Production",
              "quick connect filters tree");
        quick->clear();
        // MultiExec: the same input reaches all open terminals.
        terminal(localShell(), L("Local terminal"));
        auto *local3 = current();
        wait(8000, [&] { return local3->terminal->contents().contains(platform() == "windows" ? "PS " : "$"); });
        multiExecEnabled(true);
        type(local3->terminal, "echo MULTI-OK\r");
        check(
            wait(6000, [&]
                 { return local2->terminal->contents().contains("MULTI-") && local3->terminal->contents().contains("MULTI-"); }),
            "MultiExec mirrors input to every terminal");
        check(windowTitle().contains("["), "MultiExec shown in window title");
        multiExecEnabled(false);
        closePage(local3);

        // SFTP: follow terminal folder, remote edit with reload, recursive delete.
        {
            panel->run("/");
            wait(15000, [&] { return !panel->busy(); });
            panel->setFollow(true);
            type(p->terminal, "cd docs\r");
            check(wait(15000, [&] { return panel->currentPath() == "/docs"; }), "SFTP browser follows the terminal folder");
            type(p->terminal, "cd ..\r");
            check(wait(15000, [&] { return panel->currentPath() == "/"; }), "SFTP browser follows back to the parent");
            panel->setFollow(false);

            panel->launchEditor = false;
            const auto local = QDir(QDir::temp().filePath("CrossTerm-sftp")).filePath("README.md");
            QFile::remove(local);
            const int queued = panel->transferCount();
            panel->openRemote("README.md", 16);
            wait(20000, [&] { return panel->transfersIdle() && panel->transferCount() == queued + 1; });
            QFile original(local);
            const auto content = original.open(QIODevice::ReadOnly) ? original.readAll() : QByteArray();
            original.close();
            check(content == "hello crossterm\n", "remote file opened for editing");
            wait(800, [] { return false; });
            auto overwrite = [&](const QByteArray &data)
            {
                QFile f(local);
                f.open(QIODevice::WriteOnly | QIODevice::Truncate);
                f.write(data);
            };
            overwrite("edited by e2e\n");
            check(wait(20000, [&] { return panel->transferCount() == queued + 2 && panel->transfersIdle(); }) &&
                      panel->transferState(queued + 1) == "done",
                  "saving the local copy uploads the change");
            QTemporaryDir check1;
            panel->download({"README.md"}, check1.path());
            wait(20000, [&] { return panel->transfersIdle(); });
            QFile again(check1.filePath("README.md"));
            check(again.open(QIODevice::ReadOnly) && again.readAll() == "edited by e2e\n", "server has the edited content");
            const int beforeRestore = panel->transferCount();
            overwrite(content);
            wait(20000, [&] { return panel->transferCount() == beforeRestore + 1 && panel->transfersIdle(); });

            QTemporaryDir seed;
            QFile seedFile(seed.filePath("f.txt"));
            seedFile.open(QIODevice::WriteOnly);
            seedFile.write("x");
            seedFile.close();
            const auto seedPath = sftpQuote(QDir::fromNativeSeparators(seedFile.fileName()));
            wait(15000, [&] { return !panel->busy(); });
            panel->run(
                "/", {"mkdir tree", "mkdir tree/sub", "put " + seedPath + " tree/sub/f.txt", "put " + seedPath + " tree/g.txt"});
            wait(15000, [&] { return !panel->busy(); });
            check(listedNames(panel).contains("tree"), "nested test folder created");
            panel->removeRecursive({"/tree"}, {});
            check(wait(30000, [&] { return !panel->busy() && !listedNames(panel).contains("tree"); }),
                  "folder with contents deleted recursively");
        }

        // Light theme, terminal color schemes, CSV import from the wizard.
        {
            applyTheme(true);
            QCoreApplication::processEvents();
            check(qApp->styleSheet().contains("#f3f3f3") && !qApp->styleSheet().contains("#141414"), "light theme applied live");
            shot("11-light-theme.png");
            applyAppearance(Appearance::MacLight);
            QCoreApplication::processEvents();
            check(qApp->styleSheet().contains("#007aff", Qt::CaseInsensitive) &&
                      findChild<QToolBar *>("main")->iconSize().width() == 28,
                  "macOS light appearance applied live");
            shot("13-macos-light.png");
            applyAppearance(Appearance::MacDark);
            QCoreApplication::processEvents();
            check(qApp->styleSheet().contains("#0a84ff", Qt::CaseInsensitive), "macOS dark appearance applied live");
            shot("14-macos-dark.png");
            applyTheme(false);
            check(qApp->styleSheet().contains("#141414") && findChild<QToolBar *>("main")->iconSize().width() == 28,
                  "dark theme restored");
            applyScheme("Solarized Dark");
            check(p->terminal->background() == QColor("#002b36"), "terminal color scheme applied live");
            applyScheme("CrossTerm Dark");

            QTemporaryDir csvDir;
            const auto csv = csvDir.filePath("sessions.csv");
            {
                QFile f(csv);
                f.open(QIODevice::WriteOnly);
                f.write("name,host,port,user,protocol,folder\nweb-01,web01.example.test,22,deploy,ssh,Production\\Web\n"
                        "csv-new,csv.example.test,2201,ops,ssh,CSV\n");
            }
            const auto before = profiles.size();
            QTimer::singleShot(800, this,
                               [&]
                               {
                                   auto *dialog = QApplication::activeModalWidget();
                                   if (!dialog)
                                       return;
                                   QTimer::singleShot(400, this,
                                                      []
                                                      {
                                                          if (auto *box =
                                                                  qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                                                              if (box->standardButtons() == QMessageBox::Ok)
                                                                  box->accept();
                                                      });
                                   for (auto *button : dialog->findChildren<QPushButton *>())
                                   {
                                       if (button->text() == L("Import"))
                                           button->click();
                                   }
                               });
            importWizard(csv);
            check(profiles.size() == before + 1, "CSV import adds only the new session");
            QTimer::singleShot(400, this,
                               []
                               {
                                   if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                                       box->button(QMessageBox::Yes)->click();
                               });
            restoreBackup();
            check(profiles.size() == before, "CSV import undone");
        }

        // Transfer queue: real progress, ordering, cancel and resume (reget).
        {
            QTemporaryDir work;
            QByteArray big(64 * 1024 * 1024, '\0');
            for (int i = 0; i < big.size(); ++i)
                big[i] = char((quint32(i) * 2654435761u) >> 24);
            auto writeFile = [&](const QString &name, const QByteArray &data)
            {
                QFile f(work.filePath(name));
                f.open(QIODevice::WriteOnly);
                f.write(data);
                return f.fileName();
            };
            const auto bigPath = writeFile("big.bin", big);
            const auto smallA = writeFile("queue-a.txt", "A\n"), smallB = writeFile("queue-b.txt", "B\n");
            panel->run("/");
            wait(15000, [&] { return !panel->busy(); });
            const int base = panel->transferCount();
            panel->upload({bigPath, smallA, smallB});
            QSet<int> uploadSeen;
            wait(120000,
                 [&]
                 {
                     uploadSeen << panel->transferProgress(base);
                     return panel->transfersIdle();
                 });
            const bool uploaded = panel->transferState(base) == "done" && panel->transferState(base + 1) == "done" &&
                                  panel->transferState(base + 2) == "done";
            check(uploaded, "queued uploads complete in order");
            check(std::any_of(uploadSeen.begin(), uploadSeen.end(), [](int v) { return v > 0 && v < 100; }),
                  "upload progress reported while transferring");
            wait(15000, [&] { return !panel->busy() && listedNames(panel).contains("big.bin"); });
            check(listedNames(panel).contains("big.bin") && listedNames(panel).contains("queue-b.txt"),
                  "listing refreshed after upload");
            QDir().mkpath(work.filePath("dl"));
            panel->download({"big.bin"}, work.filePath("dl"));
            const int download = base + 3;
            QSet<int> downloadSeen;
            wait(60000,
                 [&]
                 {
                     downloadSeen << panel->transferProgress(download);
                     return panel->transferProgress(download) >= 5 || panel->transferState(download) == "done";
                 });
            panel->cancelTransfer(download);
            wait(10000, [&] { return panel->transferState(download) != "running"; });
            const auto partial = QFileInfo(work.filePath("dl/big.bin")).size();
            check(panel->transferState(download) == "cancelled" && partial > 0 && partial < big.size(),
                  "download can be cancelled midway");
            panel->resumeTransfer(download);
            wait(120000,
                 [&]
                 {
                     downloadSeen << panel->transferProgress(download);
                     return panel->transfersIdle();
                 });
            QFile result(work.filePath("dl/big.bin"));
            check(panel->transferState(download) == "done" && result.open(QIODevice::ReadOnly) && result.readAll() == big,
                  "resumed download completes with identical content");
            check(std::any_of(downloadSeen.begin(), downloadSeen.end(), [](int v) { return v > 0 && v < 100; }),
                  "download progress reported while transferring");
            bar->setCurrentIndex(1);
            showSide(3);
            wait(300, [] { return false; });
            shot("09-transfer-queue.png");
            panel->run("/", {"rm big.bin", "rm queue-a.txt", "rm queue-b.txt"});
            wait(15000, [&] { return !panel->busy(); });
        }

        // Terminal search: text in the scrollback, case sensitivity.
        if (platform() == "windows")
        {
            terminal(localShell(), L("Local terminal"));
            auto *ts = current();
            wait(8000, [&] { return ts->terminal->contents().contains("PS "); });
            type(ts->terminal, "Write-Output FIND-UNIQUE-42\r");
            type(ts->terminal, "1..200 | ForEach-Object { \"filler $_\" }\r");
            wait(10000, [&] { return ts->terminal->contents().contains("filler 200"); });
            showFind();
            findText->setText("find-unique-42");
            check(doFind(true) && ts->terminal->selection().compare("FIND-UNIQUE-42", Qt::CaseInsensitive) == 0,
                  "terminal search finds text in the scrollback");
            shot("10-terminal-search.png");
            findCase->setChecked(true);
            check(!doFind(true) && !findStatus->text().isEmpty(), "match case search reports not found");
            findCase->setChecked(false);
            hideFind();
            closePage(ts);
        }

        // Macros, output saving, logging, session restore and favorites.
        if (platform() == "windows")
        {
            QTemporaryDir work;
            QSettings().setValue("log/enabled", true);
            QSettings().setValue("log/folder", work.filePath("logs"));
            terminal(localShell(), L("Local terminal"));
            auto *m1 = current();
            wait(8000, [&] { return m1->terminal->contents().contains("PS "); });
            const int before = int(macros.size());
            startRecording();
            check(windowTitle().contains(L("Recording macro")), "recording shown in the window title");
            type(m1->terminal, "echo MACRO-RUN\r");
            check(stopRecording("e2e macro") && macros.size() == before + 1 &&
                      macros.last().toObject()["steps"].toArray().size() == 15,
                  "macro recorded with every key");
            check(macroList->count() == macros.size(), "macro listed in the sidebar");
            terminal(localShell(), L("Local terminal"));
            auto *m2 = current();
            wait(8000, [&] { return m2->terminal->contents().contains("PS "); });
            playMacro(int(macros.size()) - 1, m2->terminal);
            check(wait(6000, [&] { return m2->terminal->contents().count("MACRO-RUN") >= 2; }),
                  "macro replays into another terminal");
            {
                QFile stored(storage); // closed before the next save (QSaveFile on Windows)
                stored.open(QIODevice::ReadOnly);
                check(QJsonDocument::fromJson(stored.readAll()).object()["macros"].toArray().size() == macros.size(),
                      "macros saved in the sessions file");
            }
            macros.removeLast();
            save(profiles);
            refreshMacros();

            const auto outputFile = work.filePath("output.txt");
            QFile output(outputFile);
            check(saveOutput(m2->terminal, outputFile) && output.open(QIODevice::ReadOnly) &&
                      output.readAll().contains("MACRO-RUN"),
                  "terminal output saved to a file");
            type(m2->terminal, "echo LOGGED-LINE\r");
            wait(4000, [&] { return m2->terminal->contents().count("LOGGED-LINE") >= 2; });
            closePage(m2);
            closePage(m1);
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            bool logged = false;
            for (const auto &file : QDir(work.filePath("logs")).entryList({"*.log"}, QDir::Files))
            {
                QFile log(QDir(work.filePath("logs")).filePath(file));
                logged |= log.open(QIODevice::ReadOnly) && log.readAll().contains("LOGGED-LINE");
            }
            check(logged, "session log file written");
            QSettings().setValue("log/enabled", false);

            const int tabs = bar->count();
            const auto ids = openSessionIds();
            check(ids.contains(profiles[0].toObject()["id"].toString()), "open sessions remembered for recovery");
            recoverSessions({profiles[0].toObject()["id"].toString(), "local"});
            check(bar->count() == tabs + 2, "previous sessions recovered");
            closePage(page(bar->count() - 1));
            closePage(page(bar->count() - 1));

            toggleFavorite(1);
            auto *first = tree->topLevelItem(0)->child(0);
            check(first && first->text(0) == L("Favorites") && first->childCount() == 1 &&
                      first->child(0)->text(0) == profiles[1].toObject()["name"].toString(),
                  "favorites shown at the top of the tree");
            toggleFavorite(1);
            check(tree->topLevelItem(0)->child(0)->text(0) != L("Favorites"), "favorite removed");
        }

        // Configurable shortcuts and sessions opened at startup.
        {
            check(shortcutOf("Find...") == QKeySequence("Ctrl+Shift+F"), "default shortcut registered");
            check(setShortcut("Find...", QKeySequence("Ctrl+Alt+F")).isEmpty() &&
                      shortcutOf("Find...") == QKeySequence("Ctrl+Alt+F"),
                  "shortcut changed and applied");
            check(!setShortcut("Find...", QKeySequence("Ctrl+Shift+T")).isEmpty() &&
                      shortcutOf("Find...") == QKeySequence("Ctrl+Alt+F"),
                  "conflicting shortcut refused");
            check(shortcutOf("Next tab") == QKeySequence("Ctrl+Tab"), "tab switching shortcut is configurable");
            restoreDefaultShortcuts();
            check(shortcutOf("Find...") == QKeySequence("Ctrl+Shift+F"), "default shortcuts restored");

            auto next = profiles;
            auto first = next[0].toObject();
            first["autostart"] = true;
            next.replace(0, first);
            save(next);
            const int tabs = bar->count();
            check(startAutostartSessions() == 1 && bar->count() == tabs + 1, "sessions marked for autostart are opened");
            closePage(page(bar->count() - 1));
            first["autostart"] = false;
            next.replace(0, first);
            save(next);
        }

        // Native Telnet and Rlogin against local test servers.
        {
            QTcpServer telnetServer, rloginServer;
            telnetServer.listen(QHostAddress::LocalHost);
            rloginServer.listen(QHostAddress::LocalHost);
            QByteArray telnetReceived, rloginReceived;
            connect(&telnetServer, &QTcpServer::newConnection, this,
                    [&]
                    {
                        auto *c = telnetServer.nextPendingConnection();
                        c->write("\xff\xfd\x1f"
                                 "TELNET-OK\r\n"); // IAC DO NAWS + text
                        connect(c, &QTcpSocket::readyRead, this, [&, c] { telnetReceived += c->readAll(); });
                    });
            connect(&rloginServer, &QTcpServer::newConnection, this,
                    [&]
                    {
                        auto *c = rloginServer.nextPendingConnection();
                        connect(c, &QTcpSocket::readyRead, this,
                                [&, c]
                                {
                                    const bool first = rloginReceived.isEmpty();
                                    rloginReceived += c->readAll();
                                    if (first)
                                        c->write(QByteArray("\0RLOGIN-OK\r\n", 12));
                                });
                    });
            auto deviceProfile = [](const QString &protocol, int port)
            {
                return QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                   {"name", protocol + "-test"},
                                   {"protocol", protocol},
                                   {"host", "127.0.0.1"},
                                   {"port", port},
                                   {"user", "tester"}};
            };
            openDevice(deviceProfile("telnet", telnetServer.serverPort()));
            auto *tp = current();
            check(wait(5000, [&] { return tp->terminal->contents().contains("TELNET-OK"); }),
                  "native telnet session shows server output");
            type(tp->terminal, "abc\r");
            check(wait(5000, [&] { return telnetReceived.contains("abc"); }) && telnetReceived.contains("\xff\xfb\x1f"),
                  "telnet sends keys and negotiates NAWS");
            closePage(tp);
            openDevice(deviceProfile("rlogin", rloginServer.serverPort()));
            auto *rp = current();
            check(wait(5000, [&] { return rp->terminal->contents().contains("RLOGIN-OK"); }) &&
                      rloginReceived.contains(QByteArray("tester\0xterm", 12)),
                  "native rlogin handshake and output");
            closePage(rp);
        }

        // Real Linux container: Mosh, Remote monitoring and X11 forwarding (skipped if the container does not respond).
        if (QTcpSocket probe; (probe.connectToHost("127.0.0.1", 2223), probe.waitForConnected(1000)))
        {
            const auto key = profiles[0].toObject()["identity"].toString();
            auto linuxProfile = [&](const QString &protocol)
            {
                return QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                   {"name", "linux-" + protocol},
                                   {"protocol", protocol},
                                   {"host", "127.0.0.1"},
                                   {"port", 2223},
                                   {"user", "linuxtest"},
                                   {"identity", key},
                                   {"moshPorts", "60001:60010"},
                                   {"issues", QJsonArray{}}};
            };
            openDevice(linuxProfile("mosh"));
            auto *mp = current();
            wait(20000, [&] { return mp->terminal->contents().contains("$"); });
            type(mp->terminal, "echo MOSH-$((40+2))\r");
            check(wait(15000, [&] { return mp->terminal->contents().contains("MOSH-42"); }),
                  "Mosh session against a real mosh-server");
            type(mp->terminal, "exit\r");
            wait(5000, [&] { return mp->terminal->finished(); });
            closePage(mp);

            terminal(sessionCommand(linuxProfile("ssh"), platform(), false), "linux-ssh", linuxProfile("ssh"));
            const int monitorTab = bar->currentIndex();
            setMonitoring(monitorTab, true);
            check(wait(30000, [&] { return monitorStats(monitorTab).user == "linuxtest"; }) &&
                      monitorStats(monitorTab).cpu >= 0 && monitorStats(monitorTab).memTotal > 0 &&
                      monitorStats(monitorTab).diskPercent >= 0,
                  "remote monitoring reads a real Linux server");
            check(monitorBar->isVisible(), "monitoring bar shown under the terminal");
            shot("12-remote-monitoring.png");
            setMonitoring(monitorTab, false);
            closePage(current());

            // Password session (like an imported MobaXterm one) on a real OpenSSH server: automatic login from the
            // vault, SFTP listing in the Windows OpenSSH format, monitoring still working after several runs.
            {
                const QJsonObject rootProfile{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                              {"name", "linux-root"},
                                              {"protocol", "ssh"},
                                              {"host", "127.0.0.1"},
                                              {"port", 2223},
                                              {"user", "root"},
                                              {"issues", QJsonArray{}}};
                const auto target = credentialTarget(rootProfile, QCoreApplication::applicationName());
                storeSecret(target, "root", "test-password-123");
                profiles.append(rootProfile);
                open(int(profiles.size()) - 1);
                profiles.removeLast();
                auto *rp = current();
                check(wait(20000, [&] { return rp->terminal->contents().contains("root@"); }),
                      "password session logs in on a real OpenSSH server from the vault");
                wait(20000, [&] { return !rp->sftp->busy(); });
                rp->sftp->run("/root", {}, false);
                check(wait(20000,
                           [&]
                           {
                               const auto names = listedNames(rp->sftp);
                               return !rp->sftp->busy() && names.contains("README.md") && names.contains("docs");
                           }),
                      "SFTP lists a real OpenSSH server (Windows sftp listing format): " + rp->sftp->message());
                const int rootTab = bar->currentIndex();
                setMonitoring(rootTab, true);
                check(wait(30000, [&] { return monitorStats(rootTab).user == "root"; }),
                      "monitoring with a saved password reads the server");
                wait(12000, [] { return false; }); // at least two more runs (5 s interval)
                check(rp->monitorError.isEmpty() && monitorStats(rootTab).user == "root",
                      "monitoring keeps working with a saved password: " + rp->monitorError);
                check(monitorBar->isVisible(), "monitoring bar visible for the password session");
                shot("16-password-monitoring.png");
                setMonitoring(rootTab, false);
                closePage(rp);
                removeSecret(target);
            }

            if (!detectXServers().isEmpty())
            {
                auto x11 = linuxProfile("ssh");
                x11["x11"] = true;
                profiles.append(x11);
                open(int(profiles.size()) - 1);
                profiles.removeLast();
                auto *xp = current();
                check(xserver.isRunning(), "integrated X server started for an X11 session");
                wait(15000, [&] { return xp->terminal->contents().contains("$"); });
                type(xp->terminal, "echo X11=$DISPLAY\r");
                check(wait(10000,
                           [&] { return QRegularExpression("X11=[\\w.-]*:\\d+").match(xp->terminal->contents()).hasMatch(); }),
                      "X11 forwarding sets DISPLAY on the server");
                type(xp->terminal, "exit\r");
                wait(5000, [&] { return xp->terminal->finished(); });
                closePage(xp);
                xserver.stop();
                check(!xserver.isRunning(), "integrated X server stopped");
            }
        }

        // Wizard: MobaXterm macros, tunnels and preferences imported with the sessions, undoable.
        {
            QTemporaryDir dir;
            const auto ini = dir.filePath("MobaXterm.ini");
            QFile f(ini);
            f.open(QIODevice::WriteOnly);
            f.write(
                "[Bookmarks]\r\nSubRep=\r\nImgNum=42\r\ngw=#109#0%gw.example.test%22%carol#\r\n"
                "[Font]\r\nFont2=Consolas\r\nFontHeight2=11\r\n"
                "[Macros]\r\nHello=12:0:0:echo hello|256:13:0:ENTER\r\n"
                "[PortForwarding]\r\n0000.Db=Local;carol@gw.example.test:22;db.internal.test:5432;15432;0;No SSH key selected;;;0\r\n");
            f.close();
            const auto sessionsBefore = profiles.size();
            const auto macrosBefore = macros.size(), tunnelsBefore = tunnels.size();
            QTimer::singleShot(800, this,
                               [&]
                               {
                                   auto *dialog = QApplication::activeModalWidget();
                                   if (!dialog)
                                       return;
                                   QTimer::singleShot(400, this,
                                                      []
                                                      {
                                                          if (auto *box =
                                                                  qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                                                              if (box->standardButtons() == QMessageBox::Ok)
                                                                  box->accept();
                                                      });
                                   for (auto *button : dialog->findChildren<QPushButton *>())
                                   {
                                       if (button->text() == L("Import"))
                                           button->click();
                                   }
                               });
            importWizard(ini);
            bool linked = false;
            for (const auto &t : tunnels)
            {
                for (const auto &p : profiles)
                    linked |= t.toObject()["name"] == "Db" && p.toObject()["id"] == t.toObject()["session"] &&
                              p.toObject()["host"] == "gw.example.test";
            }
            check(profiles.size() == sessionsBefore + 1 && macros.size() == macrosBefore + 1 &&
                      tunnels.size() == tunnelsBefore + 1,
                  "MobaXterm macros and tunnels imported with the sessions");
            check(linked, "imported tunnel linked to its SSH session");
            check(QSettings().value("terminal/size").toInt() == 11, "MobaXterm terminal settings imported");
            QTimer::singleShot(400, this,
                               []
                               {
                                   if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                                       box->button(QMessageBox::Yes)->click();
                               });
            restoreBackup();
            check(profiles.size() == sessionsBefore && macros.size() == macrosBefore && tunnels.size() == tunnelsBefore,
                  "undo removes imported sessions, macros and tunnels");
            QSettings().remove("terminal/size");
            QSettings().remove("terminal/font");
        }

        // FTP: in-tab session against the built-in FTP server; Servers dialog.
        {
            QTemporaryDir root;
            QFile seed(root.filePath("ftp-file.txt"));
            seed.open(QIODevice::WriteOnly);
            seed.write("ftp\n");
            seed.close();
            check(ftpServer.start(root.path(), 0, true), "built-in FTP server starts");
            auto *panel = openFtp(QJsonObject{{"name", "ftp-test"},
                                              {"protocol", "ftp"},
                                              {"host", "127.0.0.1"},
                                              {"port", int(ftpServer.port())},
                                              {"user", "anonymous"}});
            check(wait(15000,
                       [&]
                       {
                           for (const auto &entry : panel->entries())
                           {
                               if (entry.name == "ftp-file.txt")
                                   return true;
                           }
                           return false;
                       }),
                  "FTP session browses the server in its tab");
            shot("15-ftp-session.png");
            closePage(current());
            ftpServer.stop();
            auto *servers = showServers();
            check(servers && servers->isVisible(), "Servers dialog opens");
            servers->hide();
        }

        // VNC: tab with the built-in viewer; closed port -> reason shown, no crash.
        {
            QTcpServer closed;
            closed.listen(QHostAddress::LocalHost);
            const auto port = closed.serverPort();
            closed.close();
            auto *view =
                openVnc(QJsonObject{{"name", "vnc-test"}, {"protocol", "vnc"}, {"host", "127.0.0.1"}, {"port", int(port)}});
            QString reason;
            const auto previous = view->onDisconnected;
            view->onDisconnected = [&, previous](const QString &why)
            {
                reason = why;
                previous(why);
            };
            check(current()->widget == view && wait(10000, [&] { return !reason.isEmpty(); }) && !view->isConnected(),
                  "VNC tab opens and reports why the connection failed");
            closePage(current());
        }

        // Embedded RDP: tab with the Remote Desktop control; closed port -> reason reported. Rsh against a local server.
        {
            QTcpServer closed;
            closed.listen(QHostAddress::LocalHost);
            const auto port = closed.serverPort();
            closed.close();
            const QJsonObject rdpProfile{
                {"name", "rdp-test"}, {"protocol", "rdp"}, {"host", "127.0.0.1"}, {"port", int(port)}, {"user", "tester"}};
            SessionSecrets::instance().put(credentialTarget(rdpProfile, QCoreApplication::applicationName()), "not-used");
            auto *rdp = openRdp(rdpProfile);
            QString reason;
            const auto previous = rdp->onDisconnected;
            rdp->onDisconnected = [&, previous](const QString &why)
            {
                reason = why;
                previous(why);
            };
            check(current()->widget == rdp && rdp->controlCreated(), "RDP tab hosts the Remote Desktop control");
            check(wait(60000, [&] { return !reason.isEmpty(); }) && !rdp->isConnected(),
                  "RDP tab reports why the connection failed");
            closePage(current());

            QTcpServer rshServer;
            rshServer.listen(QHostAddress::LocalHost);
            QByteArray handshake;
            connect(&rshServer, &QTcpServer::newConnection, this,
                    [&]
                    {
                        auto *c = rshServer.nextPendingConnection();
                        connect(c, &QTcpSocket::readyRead, this,
                                [&, c]
                                {
                                    const bool first = handshake.isEmpty();
                                    handshake += c->readAll();
                                    if (first)
                                        c->write(QByteArray("\0RSH-OK\r\n", 9));
                                });
                    });
            openDevice(QJsonObject{{"name", "rsh-test"},
                                   {"protocol", "rsh"},
                                   {"host", "127.0.0.1"},
                                   {"port", int(rshServer.serverPort())},
                                   {"user", "tester"},
                                   {"command", "uptime"}});
            auto *rp = current();
            check(wait(5000, [&] { return rp->terminal->contents().contains("RSH-OK"); }) &&
                      handshake.contains(QByteArray("tester\0uptime\0", 14)),
                  "rsh session sends the command and shows the output");
            closePage(rp);
        }

        // Vault: wrong password rejected without dialogs, then password login for SSH and the SFTP browser.
        {
            int pw = -1;
            for (int i = 0; i < profiles.size(); ++i)
            {
                if (profiles[i].toObject()["name"] == "pw-test")
                    pw = i;
            }
            check(pw >= 0, "password test session present");
            const auto target = credentialTarget(profiles[pw].toObject(), QCoreApplication::applicationName());
            qputenv("CROSSTERM_ASKPASS_NONINTERACTIVE", "1");
            check(storeSecret(target, "pwuser", "wrong-password"), "store password in the system vault");
            open(pw);
            auto *pp = current();
            check(wait(20000, [&] { return pp->terminal->contents().contains("Permission denied"); }),
                  "wrong saved password is rejected without prompting");
            wait(20000, [&] { return !pp->sftp->busy(); });
            pp->sftp->run("/");
            wait(20000, [&] { return !pp->sftp->busy(); });
            check(pp->sftp->message().contains("Permission denied"), "SFTP panel reports authentication failure");
            check(storeSecret(target, "pwuser", "test-password-123"), "update password in the system vault");
            pp->sftp->run("/");
            check(wait(20000, [&] { return !pp->sftp->busy() && listedNames(pp->sftp).contains("README.md"); }),
                  "SFTP password login through the system vault");
            QKeyEvent restart(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier, "r");
            QApplication::sendEvent(pp->terminal, &restart);
            check(wait(20000, [&] { return pp->terminal->contents().contains("test@localhost:~$"); }),
                  "SSH password login through the system vault");
            QFile stored(storage);
            stored.open(QIODevice::ReadOnly);
            check(!stored.readAll().contains("test-password"), "password never written to the sessions file");
            check(removeSecret(target) && !readSecret(target), "vault entry removed");
            closePage(pp);
            qunsetenv("CROSSTERM_ASKPASS_NONINTERACTIVE");
        }

        // Session password: typed once (simulated here), reused by askpass, the SSH terminal and the SFTP browser.
        {
            int pw = -1;
            for (int i = 0; i < profiles.size(); ++i)
            {
                if (profiles[i].toObject()["name"] == "pw-test")
                    pw = i;
            }
            const auto target = credentialTarget(profiles[pw].toObject(), QCoreApplication::applicationName());
            removeSecret(target);
            qputenv("CROSSTERM_ASKPASS_NONINTERACTIVE", "1");
            SessionSecrets::instance().put(target, "test-password-123");
            QProcess askpass;
            auto environment = QProcessEnvironment::systemEnvironment();
            for (const auto &entry : askpassEnvironment(target, false))
                environment.insert(entry.section('=', 0, 0), entry.section('=', 1));
            askpass.setProcessEnvironment(environment);
            askpass.start(QCoreApplication::applicationFilePath(), {"pwuser@127.0.0.1's password: "});
            wait(15000, [&] { return askpass.state() == QProcess::NotRunning; }); // the local server must stay up
            check(askpass.readAllStandardOutput() == "test-password-123\n",
                  "askpass returns the password typed earlier in the session");
            open(pw);
            auto *pp = current();
            check(wait(20000, [&] { return pp->terminal->contents().contains("test@localhost:~$"); }),
                  "SSH terminal reuses the session password");
            wait(20000, [&] { return !pp->sftp->busy(); });
            pp->sftp->run("/");
            check(wait(20000, [&] { return !pp->sftp->busy() && listedNames(pp->sftp).contains("README.md"); }),
                  "SFTP browser reuses the session password without the vault");
            SessionSecrets::instance().forget(target);
            check(!SessionSecrets::instance().get(target), "session password forgotten on request");
            closePage(pp);
            qunsetenv("CROSSTERM_ASKPASS_NONINTERACTIVE");

            // No cached password: the prompt appears inside the terminal (no dialog), the password typed there
            // is cached and the SFTP browser, which failed before the login, lists on its own.
            open(pw);
            auto *typed = current();
            // The terminal starts narrow and reflows once laid out: wait for the final width.
            wait(5000, [&] { return typed->terminal->allText().contains("CrossTerm - Open Source Terminal v"); });
            const auto bannerText = typed->terminal->allText();
            check(bannerText.contains("CrossTerm - Open Source Terminal v") &&
                      bannerText.contains("github.com/toxiicdev/crossterm"),
                  "welcome banner at the top of the terminal" +
                      (bannerText.contains("github.com") ? QString() : " | " + bannerText.left(300)));
            check(wait(20000, [&] { return typed->terminal->prompting(); }), "password prompt shown inside the terminal");
            for (const auto c : QString("test-password-123"))
            {
                QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
                QCoreApplication::sendEvent(typed->terminal, &press);
            }
            check(!typed->terminal->allText().contains("test-password-123"), "typed password not echoed");
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
            QCoreApplication::sendEvent(typed->terminal, &enter);
            check(wait(20000, [&] { return typed->terminal->contents().contains("test@localhost:~$"); }),
                  "password typed in the terminal logs in");
            wait(5000, [&] { return SessionSecrets::instance().get(target).has_value(); }); // PUT arrives on its own socket
            check(SessionSecrets::instance().get(target).value_or(QString()) == "test-password-123",
                  QString("typed password cached for the session (cached: %1)")
                      .arg(SessionSecrets::instance().get(target).has_value() ? "other value" : "none"));
            check(wait(30000, [&] { return !typed->sftp->busy() && listedNames(typed->sftp).contains("README.md"); }),
                  "SFTP browser lists on its own after the terminal login");
            const int size = typed->terminal->font().pointSize();
            QWheelEvent zoom(QPointF(10, 10), typed->terminal->mapToGlobal(QPointF(10, 10)), {}, {0, 120}, Qt::NoButton,
                             Qt::ControlModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(typed->terminal, &zoom);
            check(typed->terminal->font().pointSize() == size + 1, "Ctrl+wheel zooms the terminal font");
            SessionSecrets::instance().forget(target);
            closePage(typed);
        }

        // SSH tunnels: local (-L), remote (-R) and dynamic (-D, SOCKS5) to a local test service.
        {
            QTcpServer echo;
            echo.listen(QHostAddress::LocalHost, 2291);
            connect(&echo, &QTcpServer::newConnection, this,
                    [&]
                    {
                        while (auto *client = echo.nextPendingConnection())
                        {
                            client->write("TUNNEL-OK\n");
                            client->flush();
                            client->disconnectFromHost();
                        }
                    });
            const auto session = profiles[0].toObject()["id"].toString();
            tunnels = QJsonArray{
                QJsonObject{{"id", "t-local"},
                            {"name", "local"},
                            {"type", "local"},
                            {"listenPort", 2290},
                            {"destHost", "127.0.0.1"},
                            {"destPort", 2291},
                            {"session", session}},
                QJsonObject{{"id", "t-remote"},
                            {"name", "remote"},
                            {"type", "remote"},
                            {"listenPort", 2292},
                            {"destHost", "127.0.0.1"},
                            {"destPort", 2291},
                            {"session", session}},
                QJsonObject{
                    {"id", "t-dynamic"}, {"name", "socks"}, {"type", "dynamic"}, {"listenPort", 2293}, {"session", session}}};
            for (const auto &id : {"t-local", "t-remote", "t-dynamic"})
                startTunnel(id);
            auto through = [&](quint16 port, bool socks)
            {
                QByteArray data;
                for (int attempt = 0; attempt < 20 && !data.contains("TUNNEL-OK"); ++attempt)
                {
                    QTcpSocket socket;
                    socket.connectToHost(QHostAddress::LocalHost, port);
                    if (!wait(1500, [&] { return socket.state() == QAbstractSocket::ConnectedState; }))
                    {
                        wait(500, [] { return false; });
                        continue;
                    }
                    if (socks)
                    {
                        socket.write(QByteArray("\x05\x01\x00", 3));
                        wait(3000, [&] { return socket.bytesAvailable() >= 2; });
                        socket.read(2);
                        socket.write(QByteArray("\x05\x01\x00\x01\x7f\x00\x00\x01\x08\xf3", 10)); // 127.0.0.1:2291
                        wait(3000, [&] { return socket.bytesAvailable() >= 10; });
                        socket.read(10);
                    }
                    wait(4000,
                         [&]
                         {
                             data += socket.readAll();
                             return data.contains("TUNNEL-OK");
                         });
                }
                return data.contains("TUNNEL-OK");
            };
            check(through(2290, false), "local port forwarding carries traffic");
            check(through(2292, false), "remote port forwarding carries traffic");
            check(through(2293, true), "dynamic SOCKS forwarding carries traffic");
            check(running.size() == 3 && tunnelStatus.value("t-local") == L("Running"), "tunnels report running state");
            QTimer::singleShot(700, this,
                               [&]
                               {
                                   if (auto *d = QApplication::activeModalWidget())
                                   {
                                       shot("08-tunnels.png", d);
                                       static_cast<QDialog *>(d)->reject();
                                   }
                               });
            tunnelDialog();
            for (const auto &id : running.keys())
                stopTunnel(id);
            wait(2000, [&] { return running.isEmpty(); });
            QTcpSocket closed;
            closed.connectToHost(QHostAddress::LocalHost, 2290);
            wait(1500, [&] { return closed.state() == QAbstractSocket::UnconnectedState; });
            check(running.isEmpty() && tunnelStatus.value("t-local") == L("Stopped") &&
                      closed.state() != QAbstractSocket::ConnectedState,
                  "stopping tunnels closes the forwarded ports");
            tunnels = {};
        }

        // Per-session settings: Remote monitoring (on by default, "Loading…" until data), Follow terminal folder
        // and zoom are restored when the session is opened again.
        {
            const auto key = sessionKey(profiles[0].toObject());
            QSettings().remove(key);
            QSettings().remove("session/last");
            monitoringByDefault = true;
            open(0);
            auto *sp = current();
            wait(500, [] { return false; });
            check(sp->sftp->monitoring->isChecked() && sp->monitor && monitorBar->isVisible(),
                  "Remote monitoring on by default for a new session");
            const auto barText = monitorBar->findChild<QLabel *>("monitorError")->text();
            check(barText == L("Loading…") || !sp->monitorError.isEmpty(),
                  "monitoring bar shows Loading… until data: " + barText);
            sp->sftp->monitoring->setChecked(false);
            sp->sftp->setFollow(true);
            sp->terminal->setFontPointSize(10);
            QWheelEvent zoomIn(QPointF(10, 10), sp->terminal->mapToGlobal(QPointF(10, 10)), {}, {0, 240}, Qt::NoButton,
                               Qt::ControlModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(sp->terminal, &zoomIn);
            closePage(sp);
            open(0);
            sp = current();
            wait(500, [] { return false; });
            check(!sp->sftp->monitoring->isChecked() && !sp->monitor, "Remote monitoring off remembered for the session");
            check(sp->sftp->followBox()->isChecked(), "Follow terminal folder remembered for the session");
            check(sp->terminal->font().pointSize() == 12, "terminal zoom remembered for the session");
            sp->sftp->setFollow(false);
            closePage(sp);
            QSettings().remove(key);
            QSettings().remove("session/last");
            monitoringByDefault = false;
        }

        // Del on a session folder deletes it with its sessions and subfolders, after confirmation.
        {
            auto next = profiles;
            for (const auto &[name, folder] : {std::pair{"del-a", "Trash"}, std::pair{"del-b", "trash\\Sub"}})
                next.append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                        {"name", name},
                                        {"protocol", "ssh"},
                                        {"host", "192.0.2.1"},
                                        {"folder", folder}});
            save(next);
            const int before = profiles.size();
            showSide(0);
            QTreeWidgetItem *trash = nullptr;
            for (QTreeWidgetItemIterator it(tree); *it; ++it)
            {
                if ((*it)->data(0, Qt::UserRole + 2).toString().compare("/Trash", Qt::CaseInsensitive) == 0)
                    trash = *it;
            }
            check(trash != nullptr, "test folder shown in the session tree");
            if (trash)
            {
                tree->setCurrentItem(trash);
                tree->setFocus();
                QTimer::singleShot(300, this,
                                   []
                                   {
                                       if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                                           box->button(QMessageBox::Yes)->click();
                                   });
                for (auto *shortcut : tree->findChildren<QShortcut *>())
                {
                    if (shortcut->key() == QKeySequence(QKeySequence::Delete))
                        emit shortcut->activated(); // the tree's Del/Canc shortcut
                }
                wait(2000, [&] { return profiles.size() == before - 2; });
                bool left = false;
                for (const auto &v : profiles)
                    left |= v.toObject()["name"].toString().startsWith("del-");
                check(profiles.size() == before - 2 && !left, "Del deletes a session folder with its subfolders");
            }

            // Multiple selection: Enter opens every selected session, Del deletes them with one question.
            auto more = profiles;
            for (const auto *name : {"multi-a", "multi-b"})
                more.append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                        {"name", name},
                                        {"protocol", "telnet"},
                                        {"host", "127.0.0.1"},
                                        {"port", 1},
                                        {"folder", "Multi"}});
            save(more);
            const int count = profiles.size();
            tree->clearSelection();
            for (QTreeWidgetItemIterator it(tree); *it; ++it)
            {
                if ((*it)->text(0).startsWith("multi-"))
                    (*it)->setSelected(true);
            }
            const int tabs = bar->count();
            for (QTreeWidgetItemIterator it(tree); *it; ++it)
            {
                if ((*it)->text(0) == "multi-a")
                    emit tree->itemActivated(*it, 0);
            }
            check(bar->count() == tabs + 2, "Enter on a multiple selection opens every selected session");
            while (bar->count() > tabs)
                closePage(page(bar->count() - 1));
            QTimer::singleShot(300, this,
                               []
                               {
                                   if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                                       box->button(QMessageBox::Yes)->click();
                               });
            removeSelected();
            check(profiles.size() == count - 2, "Del deletes every selected session");
        }

        // Languages: live switching, catalog loaded, RTL for Arabic, Urdu and Persian.
        for (const auto &language : languages())
        {
            setLanguage(language.code);
            QCoreApplication::processEvents();
            const auto catalog = loadCatalog(language.code);
            const QStringList menuSources{"Terminal", "Sessions", "View", "X server", "Tools", "Settings", "Macros", "Help"};
            bool menuOk = menuBar()->actions().size() == menuSources.size();
            for (int m = 0; menuOk && m < menuSources.size(); ++m)
                menuOk = menuBar()->actions()[m]->text() == catalog.value(menuSources[m], menuSources[m]);
            const bool treeOk = tree->topLevelItem(0)->text(0) == catalog.value("User sessions", "User sessions");
            const bool sftpOk = panel->view()->headerItem()->text(0) == catalog.value("Name", "Name");
            const bool directionOk = qApp->isRightToLeft() == language.rtl;
            const bool complete = QString(language.code) == "en" || catalog.size() >= 150;
            wait(150, [] { return false; });
            bool fits = true; // no toolbar label truncated
            for (auto *button : findChild<QToolBar *>("main")->findChildren<QToolButton *>())
            {
                if (button->isVisible() && !button->text().isEmpty() && button->width() < button->sizeHint().width())
                    fits = false;
            }
            check(menuOk && treeOk && sftpOk && directionOk && complete && fits,
                  QString("language %1 applied live").arg(language.code));
            if (QString(language.code) == "it" || QString(language.code) == "ar" || QString(language.code) == "ja")
                shot(QString("06-language-%1.png").arg(language.code));
        }
        setLanguage("en");
        QCoreApplication::processEvents();

        // Guided import from a synthetic MobaXterm.ini: preview, duplicates, backup and undo.
        QTemporaryDir mobaDir;
        const auto ini = mobaDir.filePath("MobaXterm.ini");
        {
            QFile f(ini);
            f.open(QIODevice::WriteOnly);
            f.write(
                "[Credentials]\r\nDEPLOY=deploy:protected-value\r\n[Passwords]\r\nssh22@web01.example.test=protected\r\n[Bookmarks]\r\nSubRep=\r\nImgNum=42\r\n[Bookmarks_1]\r\nSubRep=Production\\Web\r\nImgNum=41\r\nweb-01=#109#0%web01.example.test%22%[deploy]%%-1%-1%%%%%0%0%0%%%-1%0%0%0%%1080%%0%0%1#MobaFont%10%0%0%-1%15%236,236,236%30,30,30%180,180,192%0%-1%0%%xterm%-1%-1%_Std_Colors_0_%80%24%0%1%-1%<none>%%0%0%-1#0# #-1\r\nweb-03=#109#0%web03.example.test%2222%[deploy]#\r\n[Bookmarks_2]\r\nSubRep=Imported\r\nImgNum=41\r\nlegacy-telnet=#98#1%192.0.2.50%23#\r\n");
        }
        const auto before = profiles.size();
        const auto originalIni = QFileInfo(ini).lastModified();
        QTimer::singleShot(800, this,
                           [&]
                           {
                               auto *dialog = QApplication::activeModalWidget();
                               if (!dialog)
                                   return;
                               auto *preview = dialog->findChild<QTreeWidget *>();
                               int rows = 0, duplicates = 0;
                               QTreeWidgetItemIterator it(preview);
                               for (; *it; ++it)
                               {
                                   if (!(*it)->parent())
                                       continue;
                                   ++rows;
                                   if ((*it)->text(4).startsWith(L("Already exists")))
                                       ++duplicates;
                               }
                               const auto labels = dialog->findChildren<QLabel *>();
                               const bool warned = std::any_of(labels.begin(), labels.end(), [](QLabel *l)
                                                               { return l->text().contains("protected by MobaXterm"); });
                               check(rows == 3 && duplicates == 1, "import preview lists sessions and marks duplicates");
                               check(warned, "import explains that protected passwords are not imported");
                               shot("07-import-wizard.png", dialog);
                               QTimer::singleShot(400, this,
                                                  []
                                                  {
                                                      if (auto *box =
                                                              qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                                                          if (box->standardButtons() == QMessageBox::Ok)
                                                              box->accept();
                                                  });
                               for (auto *button : dialog->findChildren<QPushButton *>())
                               {
                                   if (button->text() == L("Import"))
                                       button->click();
                               }
                           });
        importWizard(ini);
        check(profiles.size() == before + 2, "import adds only new sessions (merge policy)");
        check(QFileInfo::exists(storage + ".before-import"), "backup created before import");
        check(QFileInfo(ini).lastModified() == originalIni, "source MobaXterm file left untouched");
        bool userFromAccount = false;
        for (const auto &v : profiles)
            userFromAccount |= v.toObject()["name"] == "web-03" && v.toObject()["user"] == "deploy";
        check(userFromAccount, "named account resolves to the user name");
        check(!QString(QJsonDocument(profiles).toJson()).contains("protected"), "no secret copied into the sessions file");
        QTimer::singleShot(400, this,
                           []
                           {
                               if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                                   box->button(QMessageBox::Yes)->click();
                           });
        restoreBackup();
        check(profiles.size() == before && !QFileInfo::exists(storage + ".before-import"),
              "undo last import restores the sessions");

        // Window: a saved geometry larger than the screen is fitted to the work area.
        {
            const auto area = QGuiApplication::primaryScreen()->availableGeometry();
            QMainWindow probe;
            probe.setGeometry(area.adjusted(-200, -200, 600, 600));
            QSettings().setValue("window/geometry", probe.saveGeometry());
            restoreWindow();
            wait(300, [] { return false; });
            const auto frame = frameGeometry();
            check(area.contains(frame) || (area.adjusted(-1, -1, 1, 1).contains(frame)),
                  "oversized saved window fits the work area");
            move(-20000, -20000);
            QDialog huge;
            huge.setAttribute(Qt::WA_ShowWithoutActivating);
            huge.setGeometry(area.adjusted(-300, -300, 900, 900));
            huge.show();
            wait(600, [] { return false; });
            check(QGuiApplication::primaryScreen()->availableGeometry().adjusted(-1, -1, 1, 1).contains(huge.frameGeometry()),
                  "dialogs larger than the screen are fitted to the work area");
            setGeometry(QRect(area.topLeft() + QPoint(60, 60), QSize(900, 600)));
            QDialog centered(this);
            centered.setAttribute(Qt::WA_ShowWithoutActivating);
            centered.resize(300, 200);
            centered.show();
            wait(300, [] { return false; });
            const auto offset = centered.frameGeometry().center() - frameGeometry().center();
            check(offset.manhattanLength() < 40,
                  QString("dialogs open centered on the main window (offset %1,%2)").arg(offset.x()).arg(offset.y()));
            centered.close();
            move(-20000, -20000);
        }

        resize(860, 600);
        wait(600, [] { return false; });
        shot("05-narrow.png");
        closePage(local2);
        closePage(p);
        check(bar->count() == 1 && current() == home, "closing tabs returns to home");
        QFile f(out + "/e2e.txt");
        if (f.open(QIODevice::WriteOnly))
            f.write(log.join('\n').toUtf8());
        return ok ? 0 : 1;
    }

    explicit Window(QString file) : storage(std::move(file))
    {
        SessionSecrets::instance().start();
        // Once the terminal logs in (password typed, host key accepted), SFTP and monitoring retry on their own.
        SessionSecrets::instance().stored = [window = QPointer<Window>(this)](const QString &)
        {
            if (window)
                window->retryLogins();
        };
        SessionSecrets::instance().ask = [window = QPointer<Window>(this)](const QString &id, const QString &prompt, bool echo,
                                                                           std::function<void(std::optional<QString>)> answer)
        {
            if (!window || id.isEmpty())
                return false;
            for (int i = 0; i < window->bar->count(); ++i)
            {
                auto *p = window->page(i);
                if (p && p->terminal && !p->terminal->finished() &&
                    p->command.environment.contains("CROSSTERM_ASKPASS_TERMINAL=" + id))
                {
                    p->terminal->prompt(prompt, echo, std::move(answer));
                    return true;
                }
            }
            return false;
        };
        auto *hosts = new QFileSystemWatcher(this);
        const auto sshFolder = QDir::home().filePath(".ssh");
        const auto knownHosts = sshFolder + "/known_hosts";
        auto watchHosts = [hosts, sshFolder, knownHosts]
        {
            if (QFileInfo::exists(sshFolder) && !hosts->directories().contains(sshFolder))
                hosts->addPath(sshFolder);
            if (QFileInfo::exists(knownHosts) && !hosts->files().contains(knownHosts))
                hosts->addPath(knownHosts);
        };
        watchHosts();
        connect(hosts, &QFileSystemWatcher::fileChanged, this, [this, watchHosts] { watchHosts(), retryLogins(); });
        connect(hosts, &QFileSystemWatcher::directoryChanged, this, [this, watchHosts] { watchHosts(), retryLogins(); });
        setWindowTitle("CrossTerm");
        setWindowIcon(QIcon(":/assets/crossterm.png")); // same icon on every platform (assets/)
        resize(1200, 740); // restoreWindow() then applies the last saved size
        qApp->setStyle("Fusion");
        QFont interfaceFont("Segoe UI");
        interfaceFont.setPointSize(9);
        qApp->setFont(interfaceFont);
        themeKey = QSettings().value("theme", "mac-system").toString();
        appearance = appearanceFromKey(themeKey, systemDark());
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
                [this]
                {
                    if (themeKey == "system" || themeKey == "mac-system")
                        applyThemeKey(themeKey);
                });
        applyAppearanceFont(appearance);
        qApp->setStyleSheet(appearanceStyleSheet(appearance, darkStyle));

        // Menus (registered texts: retranslated on language change)
        auto menu = [&](const char *title)
        {
            auto *m = menuBar()->addMenu(QString());
            texts.bind(m, &QMenu::setTitle, title);
            return m;
        };
        auto item = [&](QMenu *m, const char *text, auto slot, const QString &iconName = {})
        {
            auto *a = m->addAction(iconName.isEmpty() ? QIcon() : icon(iconName, 16), QString());
            texts.text(a, text);
            a->setObjectName(text);
            shortcutActions << a;
            connect(a, &QAction::triggered, this, slot);
            return a;
        };
        auto *terminalMenu = menu(N("Terminal"));
        auto *sessionsMenu = menu(N("Sessions"));
        auto *viewMenu = menu(N("View"));
        auto *xMenu = menu(N("X server"));
        auto *toolsMenu = menu(N("Tools"));
        auto *settingsMenu = menu(N("Settings"));
        auto *macrosMenu = menu(N("Macros"));
        auto *helpMenu = menu(N("Help"));

        item(
            terminalMenu, N("Open new tab"), [this] { terminal(localShell(), L("Local terminal")); }, "shell")
            ->setShortcut(QKeySequence("Ctrl+Shift+T"));
        item(terminalMenu, N("Rename current tab"), [this] { renameTab(bar->currentIndex()); });
        item(terminalMenu, N("Close current tab"), [this] { closePage(current()); });
        item(terminalMenu, N("Close all tabs"), [this] { closeAll(nullptr); });
        terminalMenu->addSeparator();
        item(terminalMenu, N("Save terminal output..."),
             [this]
             {
                 auto *t = current()->terminal;
                 if (!t)
                     return;
                 const auto file = QFileDialog::getSaveFileName(this, L("Save terminal output..."), "terminal.txt",
                                                                L("Text files (*.txt *.log)"));
                 if (!file.isEmpty() && !saveOutput(t, file))
                     QMessageBox::warning(this, L("Save"), L("The file could not be written."));
             });
        item(terminalMenu, N("Find..."), [this] { showFind(); })->setShortcut(QKeySequence("Ctrl+Shift+F"));
        item(terminalMenu, N("Copy all"),
             [this]
             {
                 if (current()->terminal)
                     QApplication::clipboard()->setText(current()->terminal->contents());
             });
        item(terminalMenu, N("Paste"),
             [this]
             {
                 if (current()->terminal)
                     current()->terminal->paste(QApplication::clipboard()->text());
             });
        terminalMenu->addSeparator();
        item(terminalMenu, N("Exit"), [this] { close(); }, "exit");

        item(sessionsMenu, N("New session"), [this] { edit(-1); }, "session")->setShortcut(QKeySequence("Ctrl+Shift+N"));
        item(sessionsMenu, N("Import from MobaXterm..."), [this] { importWizard(); }, "folder");
        item(sessionsMenu, N("Import sessions from file"), [this] { importFile(); });
        item(sessionsMenu, N("Import SCRT sessions"), [this] { importFile("SecureCRT (*.ini *.xml *.csv)"); });
        item(sessionsMenu, N("Import Exceed sessions"), [this] { importFile("Exceed Xstart (*.xs)"); });
        item(sessionsMenu, N("Import PuTTY sessions"), [this] { importLocal("PuTTY"); });
        item(sessionsMenu, N("Import MRemote sessions"), [this] { importFile("mRemote / mRemoteNG (*.xml *.csv)"); });
        item(sessionsMenu, N("Import PuTTYCM sessions"), [this] { importFile("PuTTYCM (*.xml *.dat)"); });
        item(sessionsMenu, N("Import SuperPuTTY sessions"), [this] { importFile("SuperPuTTY (*.xml)"); });
        item(sessionsMenu, N("Import WSL sessions"), [this] { importLocal("WSL"); });
        item(sessionsMenu, N("Export sessions to file"), [this] { exportFile(); });
        undoImport = item(sessionsMenu, N("Undo last import"), [this] { restoreBackup(); });
        undoImport->setEnabled(QFileInfo::exists(storage + ".before-import"));

        auto *splitMenu = new QMenu(this);
        texts.bind(splitMenu, &QMenu::setTitle, N("Split"));
        const char *splits[]{N("Single terminal mode"), N("2 terminals mode (vertical split)"),
                             N("2 terminals mode (horizontal split)"), N("4 terminals mode")};
        auto *splitGroup = new QActionGroup(this);
        for (int i = 0; i < 4; ++i)
        {
            auto *a = item(
                splitMenu, splits[i],
                [this, i]
                {
                    splitMode = i + 1;
                    layoutPages();
                },
                "split");
            a->setCheckable(true);
            a->setChecked(i == 0);
            splitGroup->addAction(a);
        }
        auto *sidebarToggle = item(viewMenu, N("Show left sidebar"), [] {});
        sidebarToggle->setCheckable(true);
        sidebarToggle->setChecked(true);
        sidebarToggle->setShortcut(QKeySequence("Ctrl+Shift+B"));
        auto *fullscreen = item(viewMenu, N("Fullscreen"), [this] { isFullScreen() ? showNormal() : showFullScreen(); });
        fullscreen->setShortcut(QKeySequence(Qt::Key_F11));
        fullscreen->setShortcutContext(Qt::ApplicationShortcut);
        viewMenu->addMenu(splitMenu);

        item(xMenu, N("Start X server"),
             [this]
             {
                 if (!startXServer())
                     xserverStatus();
             });
        item(xMenu, N("Stop X server"), [this] { xserver.stop(); });
        item(xMenu, N("XDMCP session..."), [this] { xdmcpSession(); });
        xMenu->addSeparator();
        item(xMenu, N("X server status"), [this] { xserverStatus(); });
        item(toolsMenu, N("Local terminal"), [this] { terminal(localShell(), L("Local terminal")); }, "shell");
        item(toolsMenu, N("SSH tunnels"), [this] { tunnelDialog(); }, "tunneling");
        item(toolsMenu, N("Port scanner..."),
             [this]
             {
                 auto *scanner = new PortScannerDialog(this);
                 scanner->setAttribute(Qt::WA_DeleteOnClose);
                 scanner->show();
             });
        item(settingsMenu, N("Configuration"), [this] { settingsDialog(); }, "settings");
        auto *languageMenu = new QMenu(this);
        texts.bind(languageMenu, &QMenu::setTitle, N("Language"));
        auto *languageGroup = new QActionGroup(this);
        auto *automatic = item(languageMenu, N("Automatic (system language)"), [this] { setLanguage({}); });
        automatic->setCheckable(true);
        automatic->setChecked(QSettings().value("language").toString().isEmpty());
        languageGroup->addAction(automatic);
        languageMenu->addSeparator();
        for (const auto &language : languages())
        {
            auto *a = languageMenu->addAction(QString::fromUtf8(language.name));
            a->setCheckable(true);
            a->setChecked(QSettings().value("language").toString() == language.code);
            a->setData(QString(language.code));
            languageGroup->addAction(a);
            const QString code = language.code;
            connect(a, &QAction::triggered, this, [this, code] { setLanguage(code); });
        }
        settingsMenu->addMenu(languageMenu);
        item(settingsMenu, N("Keyboard shortcuts..."), [this] { shortcutsDialog(); });
        connect(macrosMenu, &QMenu::aboutToShow, this, [this, macrosMenu] { macroMenu(macrosMenu); });
        macroMenu(macrosMenu);
        item(
            helpMenu, N("About CrossTerm"),
            [this]
            {
                QMessageBox::about(this, L("About CrossTerm"),
                                   "CrossTerm 0.4\n" + L("Terminal and session manager built with C++17 and Qt Widgets."));
            },
            "help");

        // Main toolbar
        auto *tools = addToolBar("Main");
        tools->setObjectName("main");
        tools->setMovable(false);
        tools->setFloatable(false);
        tools->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        tools->setIconSize(QSize(appearancePalette(appearance).toolbarIconSize, appearancePalette(appearance).toolbarIconSize));
        tools->toggleViewAction()->setEnabled(false);
        auto button = [&](const QString &name, const char *label, const char *tip, QMenu *popup = nullptr)
        {
            auto *a = tools->addAction(icon(name), QString());
            texts.text(a, label);
            texts.tip(a, tip);
            if (popup)
            {
                // No setMenu: in Qt 6 it would replace the action of the menu shared with the menu bar,
                // which would lose its translated title. The menu opens below the button.
                auto *widget = tools->widgetForAction(a);
                connect(a, &QAction::triggered, this,
                        [popup, widget] { popup->exec(widget->mapToGlobal(QPoint(0, widget->height()))); });
            }
            return a;
        };
        auto unavailable = [&](const char *text)
        {
            auto *m = new QMenu(this);
            item(m, text, [] {})->setEnabled(false);
            return m;
        };
        connect(button("session", N("Session"), N("Create a new session")), &QAction::triggered, this, [this] { edit(-1); });
        connect(button("servers", N("Servers"), N("Built-in HTTP, TFTP and FTP servers")), &QAction::triggered, this,
                [this] { showServers(); });
        auto *toolsPopup = new QMenu(this);
        item(toolsPopup, N("Local terminal"), [this] { terminal(localShell(), L("Local terminal")); }, "shell");
        item(toolsPopup, N("Import from MobaXterm..."), [this] { importWizard(); }, "folder");
        button("tools", N("Tools"), N("Tools"), toolsPopup);
        connect(button("sessions", N("Sessions"), N("Show sessions")), &QAction::triggered, this, [this] { showSide(0); });
        button("view", N("View"), N("View settings"), viewMenu);
        button("split", N("Split"), N("Split terminal"), splitMenu);
        multiAction = button("multiexec", N("MultiExec"), N("Type the same keys in all open terminals"));
        multiAction->setCheckable(true);
        connect(multiAction, &QAction::toggled, this,
                [this](bool on)
                {
                    multiExec = on;
                    layoutPages();
                });
        connect(button("tunneling", N("Tunneling"), N("SSH tunnels: local, remote and dynamic port forwarding")),
                &QAction::triggered, this, [this] { tunnelDialog(); });
        button("packages", N("Packages"), N("Plugins (not available)"), unavailable(N("Not available in CrossTerm yet")));
        connect(button("settings", N("Settings"), N("Configuration")), &QAction::triggered, this, [this] { settingsDialog(); });
        button("help", N("Help"), N("Help"), helpMenu);
        auto *spacer = new QWidget;
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tools->addWidget(spacer);
        button("xserver", N("X server"), N("X server"), xMenu);
        connect(button("exit", N("Exit"), N("Exit CrossTerm")), &QAction::triggered, this, &QWidget::close);

        // Left column: quick connect, vertical strip, panels
        auto *left = new QWidget;
        auto *leftLayout = new QVBoxLayout(left);
        leftLayout->setContentsMargins(2, 2, 0, 0);
        leftLayout->setSpacing(2);
        quick->setObjectName("quick");
        texts.bind(quick, &QLineEdit::setPlaceholderText, N("Quick connect..."));
        texts.bind(quick, &QWidget::setAccessibleName, N("Quick connect"));
        quick->setFixedHeight(24);
        quick->setClearButtonEnabled(true);
        leftLayout->addWidget(quick);
        auto *body = new QHBoxLayout;
        body->setSpacing(0);
        auto *stripWidget = new QWidget;
        stripWidget->setObjectName("strip");
        auto *stripLayout = new QVBoxLayout(stripWidget);
        stripLayout->setContentsMargins(0, 0, 0, 0);
        stripLayout->setSpacing(0);
        const QStringList stripIcons{"star", "tools", "macros", "sftp"};
        const char *stripTips[]{N("Sessions"), N("Tools"), N("Macros"), N("Sftp")};
        for (int i = 0; i < stripIcons.size(); ++i)
        {
            auto *b = new QToolButton;
            b->setObjectName("stripButton");
            b->setIcon(toggleIcon(stripIcons[i]));
            b->setIconSize(QSize(34, 34));
            b->setFixedSize(45, 53);
            b->setCheckable(true);
            texts.tip(b, stripTips[i]);
            texts.bind(b, &QWidget::setAccessibleName, stripTips[i]);
            connect(b, &QToolButton::clicked, this, [this, i] { showSide(i); });
            stripLayout->addWidget(b);
            strip << b;
        }
        stripLayout->addStretch();
        body->addWidget(stripWidget);
        tree->setHeaderHidden(true);
        tree->setSelectionMode(QAbstractItemView::ExtendedSelection); // Ctrl/Shift+click: open or delete several

        tree->setIconSize(QSize(20, 20));
        tree->setObjectName("sessions");
        tree->setIndentation(16);
        tree->setContextMenuPolicy(Qt::CustomContextMenu);
        texts.bind(tree, &QWidget::setAccessibleName, N("User sessions"));
        tree->setExpandsOnDoubleClick(true);
        side->addWidget(tree);
        auto *toolList = new QListWidget;
        toolList->setIconSize(QSize(20, 20));
        toolList->addItem(new QListWidgetItem(icon("shell"), QString()));
        toolList->addItem(new QListWidgetItem(icon("session"), QString()));
        toolList->addItem(new QListWidgetItem(icon("folder"), QString()));
        retranslators << [toolList]
        {
            toolList->item(0)->setText(L("Local terminal"));
            toolList->item(1)->setText(L("New session"));
            toolList->item(2)->setText(L("Import from MobaXterm..."));
        };
        connect(toolList, &QListWidget::itemActivated, this,
                [this, toolList](QListWidgetItem *item)
                {
                    const int row = toolList->row(item);
                    if (row == 0)
                        terminal(localShell(), L("Local terminal"));
                    else if (row == 1)
                        edit(-1);
                    else
                        importWizard();
                });
        side->addWidget(toolList);
        auto *macroPage = new QWidget;
        auto *macroLayout = new QVBoxLayout(macroPage);
        macroLayout->setContentsMargins(0, 0, 0, 0);
        auto *record = texts.text(new QPushButton(icon("macros", 16), QString()), N("Record new macro"));
        connect(record, &QPushButton::clicked, this, [this] { recording ? stopRecordingDialog() : startRecording(); });
        retranslators << [this, record] { record->setText(recording ? L("Stop recording") : L("Record new macro")); };
        macroLayout->addWidget(record);
        texts.text(macroHint, N("No macro recorded. Click Record, type in a terminal, then stop the recording."));
        macroHint->setObjectName("empty");
        macroHint->setWordWrap(true);
        macroLayout->addWidget(macroHint);
        macroList->setContextMenuPolicy(Qt::CustomContextMenu);
        macroLayout->addWidget(macroList, 1);
        connect(macroList, &QListWidget::itemActivated, this,
                [this](QListWidgetItem *item) { playMacro(macroList->row(item), current()->terminal); });
        connect(macroList, &QListWidget::customContextMenuRequested, this,
                [this](const QPoint &at)
                {
                    const int row = macroList->row(macroList->itemAt(at));
                    if (row < 0)
                        return;
                    QMenu menu;
                    menu.addAction(L("Play"), [&] { playMacro(row, current()->terminal); });
                    menu.addAction(L("Rename"),
                                   [&]
                                   {
                                       auto macro = macros[row].toObject();
                                       const auto name = QInputDialog::getText(this, L("Macros"), L("Macro name:"),
                                                                               QLineEdit::Normal, macro["name"].toString())
                                                             .trimmed();
                                       if (name.isEmpty())
                                           return;
                                       macro["name"] = name;
                                       macros.replace(row, macro);
                                       save(profiles);
                                       refreshMacros();
                                   });
                    menu.addAction(icon("delete", 16), L("Delete"),
                                   [&]
                                   {
                                       macros.removeAt(row);
                                       save(profiles);
                                       refreshMacros();
                                   });
                    menu.exec(macroList->viewport()->mapToGlobal(at));
                });
        side->addWidget(macroPage);
        sftpStack->addWidget(texts.text(emptyPage(), N("Open an SSH session to browse remote files here.")));
        side->addWidget(sftpStack);
        body->addWidget(side, 1);
        leftLayout->addLayout(body, 1);

        // Right column: tabs + terminal area
        auto *right = new QWidget;
        auto *rightLayout = new QVBoxLayout(right);
        rightLayout->setContentsMargins(0, 2, 0, 0);
        rightLayout->setSpacing(0);
        auto *tabRow = new QHBoxLayout;
        tabRow->setSpacing(0);
        bar->setMovable(true);
        bar->setTabsClosable(false); // custom "×" buttons in addPage
        bar->setExpanding(false);
        bar->setDrawBase(false);
        bar->setElideMode(Qt::ElideRight);
        bar->setFocusPolicy(Qt::NoFocus);
        bar->setUsesScrollButtons(true);
        bar->setContextMenuPolicy(Qt::CustomContextMenu);
        bar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        auto *plus = new QToolButton;
        plus->setObjectName("newTab");
        plus->setIcon(icon("plus"));
        plus->setIconSize(QSize(12, 12));
        texts.tip(plus, N("Start a new local terminal"));
        plus->setFixedSize(34, 24); // as tall as the tab pill, next to the last tab
        style(plus, "QToolButton { background: #2a2a2a; border: 1px solid #6a6a6a; border-radius: 6px; margin-left: 4px; }");
        connect(plus, &QToolButton::clicked, this, [this] { terminal(localShell(), L("Local terminal")); });
        tabRow->addWidget(bar);
        tabRow->addWidget(plus, 0, Qt::AlignVCenter);
        tabRow->addStretch(1);
        rightLayout->addLayout(tabRow);
        auto *rule = new QFrame;
        rule->setFixedHeight(1);
        style(rule, "background: #6a6a6a;");
        rightLayout->addWidget(rule);
        // Terminal search bar (hidden until needed).
        auto *findLayout = new QHBoxLayout(findBar);
        findLayout->setContentsMargins(6, 3, 6, 3);
        texts.bind(findText, &QLineEdit::setPlaceholderText, N("Find in terminal"));
        findText->setClearButtonEnabled(true);
        auto *previous = texts.text(new QPushButton, N("Previous"));
        auto *nextMatch = texts.text(new QPushButton, N("Next"));
        texts.text(findCase, N("Match case"));
        auto *closeFind = new QToolButton;
        closeFind->setText(QString::fromUtf8("\u00d7"));
        texts.tip(closeFind, N("Close"));
        findStatus->setObjectName("empty");
        findLayout->addWidget(findText, 1);
        findLayout->addWidget(previous);
        findLayout->addWidget(nextMatch);
        findLayout->addWidget(findCase);
        findLayout->addWidget(findStatus);
        findLayout->addWidget(closeFind);
        findBar->hide();
        rightLayout->addWidget(findBar);
        connect(previous, &QPushButton::clicked, this, [this] { doFind(true); });
        connect(nextMatch, &QPushButton::clicked, this, [this] { doFind(false); });
        connect(closeFind, &QToolButton::clicked, this, [this] { hideFind(); });
        connect(findText, &QLineEdit::returnPressed, this,
                [this] { doFind(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier); });
        auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), findBar, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
        connect(escape, &QShortcut::activated, this, [this] { hideFind(); });
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(3);
        style(area, "background: #4a4a4a;"); // visible only as a separator in Split mode
        rightLayout->addWidget(area, 1);
        monitorBar->hide();
        rightLayout->addWidget(monitorBar);

        auto *splitter = new QSplitter;
        splitter->addWidget(left);
        splitter->addWidget(right);
        splitter->setSizes({263, 937});
        splitter->setStretchFactor(1, 1);
        splitter->setCollapsible(1, false);
        splitter->setHandleWidth(3);
        setCentralWidget(splitter);
        connect(sidebarToggle, &QAction::toggled, left, &QWidget::setVisible);
        leftPanel = left;

        home->widget = homePage();
        addPage(home, "", icon("home"));
        bar->setTabButton(0, QTabBar::RightSide, nullptr);
        retranslators << [this] { bar->setTabToolTip(0, L("Home")); };
        showSide(0);

        connect(bar, &QTabBar::currentChanged, this, [this] { layoutPages(); });
        connect(bar, &QTabBar::tabCloseRequested, this, [this](int index) { closePage(page(index)); });
        connect(bar, &QTabBar::tabMoved, this,
                [this](int from, int to)
                {
                    if (moving || page(0) == home)
                        return;
                    moving = true; // the Home tab always stays first
                    bar->moveTab(to, from);
                    moving = false;
                });
        connect(bar, &QTabBar::customContextMenuRequested, this, [this](const QPoint &at) { tabMenu(at); });
        connect(tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &at) { treeMenu(at); });
        connect(tree, &QTreeWidget::itemActivated, this,
                [this](QTreeWidgetItem *item)
                {
                    if (item->isSelected() && tree->selectedItems().size() > 1)
                        openSelected(); // Enter on a multiple selection opens every selected session
                    else if (item->data(0, Qt::UserRole + 1).toBool())
                        open(item->data(0, Qt::UserRole).toInt());
                });
        auto *del = new QShortcut(QKeySequence::Delete, tree, nullptr, nullptr, Qt::WidgetShortcut);
        connect(del, &QShortcut::activated, this, [this] { removeSelected(); });
        auto *rename = new QShortcut(QKeySequence(Qt::Key_F2), tree, nullptr, nullptr, Qt::WidgetShortcut);
        connect(rename, &QShortcut::activated, this,
                [this]
                {
                    if (selection() >= 0)
                        edit(selection());
                });
        connect(quick, &QLineEdit::textChanged, this,
                [this](const QString &text)
                {
                    refresh();
                    if (!text.isEmpty())
                        showSide(0);
                });
        connect(quick, &QLineEdit::returnPressed, this, [this] { quickConnect(); });
        auto cycle = [this](int step)
        {
            if (bar->count())
                bar->setCurrentIndex((bar->currentIndex() + step + bar->count()) % bar->count());
        };
        auto tabAction = [&](const char *text, const char *keys, int step)
        {
            auto *a = new QAction(this);
            texts.text(a, text);
            a->setObjectName(text);
            a->setShortcut(QKeySequence(keys));
            a->setShortcutContext(Qt::ApplicationShortcut);
            addAction(a);
            connect(a, &QAction::triggered, this, [cycle, step] { cycle(step); });
            shortcutActions << a;
        };
        tabAction(N("Next tab"), "Ctrl+Tab", 1);
        tabAction(N("Previous tab"), "Ctrl+Shift+Tab", -1);
        // Shortcuts: factory defaults stay available for "Restore defaults".
        for (auto *a : shortcutActions)
            defaultShortcuts.insert(a->objectName(), a->shortcut());
        applyShortcuts();

        QFile config(storage);
        if (config.exists())
        {
            const auto doc =
                config.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(config.readAll()).object() : QJsonObject{};
            writable = doc["version"].toInt() == 1 && doc["sessions"].isArray();
            if (writable)
            {
                profiles = doc["sessions"].toArray();
                tunnels = doc["tunnels"].toArray();
                macros = doc["macros"].toArray();
            }
            else
                QMessageBox::warning(this, L("Sessions"), L("The sessions file cannot be read; changes are disabled."));
        }
        connect(qApp, &QGuiApplication::screenRemoved, this, [this] { QTimer::singleShot(0, this, [this] { fitToScreen(); }); });
        refresh();
        refreshMacros();
        tree->setFocus();
        tree->setCurrentItem(tree->topLevelItem(0));
    }
};

// Every dialog stays within the work area of the monitor it appears on (also on small or high-DPI screens).
class DialogFitter : public QObject
{
public:
    QPointer<QWidget> main; // owner for dialogs opened without a parent

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *dialog = qobject_cast<QDialog *>(watched);
        if (dialog && event->type() == QEvent::Show && dialog->isWindow() && !dialog->property("placed").toBool())
        {
            // First show: centered on the window that opened it (or the main window), before it appears.
            dialog->setProperty("placed", true);
            QWidget *owner = dialog->parentWidget() ? dialog->parentWidget()->window() : main.data();
            if (owner && owner != dialog && owner->isVisible())
                dialog->move(owner->frameGeometry().center() - QPoint(dialog->width() / 2, dialog->height() / 2));
        }
        if (dialog && event->type() == QEvent::Show && dialog->isWindow())
            QTimer::singleShot(0, dialog,
                               [dialog]
                               {
                                   auto *screen = QGuiApplication::screenAt(dialog->frameGeometry().center());
                                   if (!screen)
                                       screen = dialog->screen();
                                   const auto frame = dialog->frameGeometry();
                                   const auto fitted = fitToArea(frame, screen->availableGeometry());
                                   if (fitted == frame)
                                       return;
                                   dialog->resize(fitted.size() - (frame.size() - dialog->size()));
                                   dialog->move(fitted.topLeft());
                               });
        return QObject::eventFilter(watched, event);
    }
};

// Every process started by CrossTerm (shells, ssh/sftp, tunnels, monitoring, X server, editors) dies with it,
// also on a crash. Windows: a kill-on-close job object that children inherit; the OS closes the handle when we exit.
// Linux/macOS: a crash handler kills our process group; terminal children also get PR_SET_PDEATHSIG (terminal.cpp).
// ponytail: SIGKILL of CrossTerm on Linux/macOS still leaves QProcess children; per-child pdeathsig if that matters.
#ifdef Q_OS_WIN
static void bindChildrenToLifetime()
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
        return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, GetCurrentProcess()))
        CloseHandle(job);
    // On success the handle is kept open on purpose until the process ends.
}
#else
static void killProcessGroup(int signal)
{
    ::signal(SIGTERM, SIG_IGN);
    ::kill(0, SIGTERM); // our group: every child that kept it (QProcess children do)
    ::signal(signal, SIG_DFL);
    ::raise(signal);
}

static void bindChildrenToLifetime()
{
    if (::getpgrp() != ::getpid())
        ::setpgid(0, 0); // own group, so the kill below never reaches the shell that started us
    for (const int s : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGTERM, SIGHUP})
        ::signal(s, killProcessGroup);
}
#endif

int main(int argc, char **argv)
{
    if (isHiddenShellInvocation(argc))
        return runHiddenShell(); // %COMSPEC% for VcXsrv, before anything else
    if (!isAskpassInvocation(argc))
        bindChildrenToLifetime();
    {
        QStringList arguments;
        for (int i = 0; i < argc; ++i)
            arguments << QString::fromLocal8Bit(argv[i]);
        if (arguments.contains("--proxy-connect"))
            return runProxyConnect(arguments);
    }
    if (isAskpassInvocation(argc))
        return runAskpass(argc, argv); // ssh prompt helper: no window unless a dialog is needed
    QApplication app(argc, argv);
    QApplication::setWindowIcon(QIcon(":/assets/crossterm.png")); // Dock / taskbar while running
    // Portable mode: INI settings and sessions in CrossTerm-data next to the executable.
    const auto portable = portableRoot(QCoreApplication::applicationDirPath(), app.arguments());
    if (!portable.isEmpty())
    {
        QDir().mkpath(portable);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, portable);
    }
    app.setOrganizationName("CrossTerm");
    app.setApplicationName("CrossTerm");
    if (app.arguments().contains("--smoke-test") || app.arguments().contains("--e2e"))
        app.setApplicationName("CrossTerm-Test"); // tests do not touch the user's language, window and recents
    const auto location = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const bool smoke = app.arguments().contains("--smoke-test");
    QTemporaryDir temporary;
    if (smoke && !temporary.isValid())
        return 1;
    const int custom = app.arguments().indexOf("--config"); // alternate sessions file (test data)
    const auto storage = smoke                                               ? temporary.filePath("sessions.json")
                         : custom > 0 && custom + 1 < app.arguments().size() ? app.arguments()[custom + 1]
                         : !portable.isEmpty()                               ? portable + "/sessions.json"
                                                                             : location + "/sessions.json";
    QDir().mkpath(QFileInfo(storage).absolutePath());
    QLockFile lock(storage + ".lock");
    if (!lock.tryLock())
    {
        QMessageBox::warning(nullptr, "CrossTerm", L("The configuration is already in use by another CrossTerm window."));
        return 1;
    }
    applyLanguage(currentLanguage());
    DialogFitter fitter;
    app.installEventFilter(&fitter);
    Window window(storage);
    fitter.main = &window;
    const int e2e = app.arguments().indexOf("--e2e");
    if (!smoke && e2e < 0)
    {
        window.restoreWindow();
        window.startAutostartTunnels();
        window.startAutostartSessions();
        // askpass counters left over from previous sessions (they contain only a number).
        for (const auto &old : QDir::temp().entryList({"crossterm-askpass-*"}, QDir::Files))
            QFile::remove(QDir::temp().filePath(old));
    }
    if (smoke || e2e > 0)
    {
        window.persistGeometry = false;
        // Tests without focus or a visible window: off-screen and never activated.
        window.setAttribute(Qt::WA_ShowWithoutActivating);
        window.move(-20000, -20000);
    }
    window.show();
    if (e2e > 0 && e2e + 1 < app.arguments().size())
        return window.e2e(app.arguments()[e2e + 1]);
    if (smoke)
    {
        auto *test = window.smokeTerminal();
        QTimer::singleShot(300, test,
                           [test]
                           {
                               for (const auto c : QString("echo CROSSTERM-PTY-OK"))
                               {
                                   QKeyEvent key(QEvent::KeyPress, c.toUpper().unicode(), Qt::NoModifier, QString(c));
                                   QApplication::sendEvent(test, &key);
                               }
                               QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
                               tab.setAccepted(false);
                               QApplication::sendEvent(test, &tab);
                               test->setProperty("tabConsumed", tab.isAccepted());
                               QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                               QApplication::sendEvent(test, &enter);
                           });
        QTimer::singleShot(1600, &app,
                           [&app, &window, test]
                           {
                               app.exit(test->property("tabConsumed").toBool() && window.focusWidget() == test &&
                                                test->contents().split('\n').contains("CROSSTERM-PTY-OK")
                                            ? 0
                                            : 1);
                           });
    }
    return app.exec();
}
