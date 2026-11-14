#include "monitor.h"
#include "askpass.h" // releaseAskpass
#include <QApplication>
#include <QCoreApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QMenu>
#include <QSettings>
#include <QPainter>
#include <algorithm>
#include <climits>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

static QString ui(const char *text)
{
    return QCoreApplication::translate("CrossTerm", text);
}

qint64 monitorClock()
{
    static const QElapsedTimer clock = []
    {
        QElapsedTimer started;
        started.start();
        return started;
    }();
    return clock.elapsed();
}

QString monitorScript()
{
    // No quotes or backslashes: it is passed as sh -c '<script>' through ssh. One sample takes a second (two readings
    // of the counters), the pause fills the rest of the period. The loop ends with the connection: a write to the
    // closed output kills the shell (SIGPIPE) or, where that signal is ignored, makes echo fail.
    return QString("while echo @@host; do uname -n; echo @@user; id -un; echo @@uptime; cat /proc/uptime; "
                   "echo @@stat1; head -n 1 /proc/stat; echo @@net1; cat /proc/net/dev; sleep 1; "
                   "echo @@stat2; head -n 1 /proc/stat; echo @@net2; cat /proc/net/dev; "
                   "echo @@mem; grep ^Mem /proc/meminfo; echo @@df; df -P; "
                   "echo @@procs; ls /proc | grep -c ^[0-9]; echo @@fds; cat /proc/sys/fs/file-nr; "
                   "echo @@tcp; cut -c35-36 /proc/net/tcp | grep -c 01; "
                   "cut -c83-84 /proc/net/tcp6 2>/dev/null | grep -c 01; "
                   "echo @@end || exit; sleep %1; done")
        .arg(MonitorPeriod - 1);
}

struct Cpu
{
    qint64 total = -1, idle = 0;
};

// Numbers as the remote prints them, hostile ones included: counters stay within 0..2^53, so that sums and
// differences of them cannot overflow.
constexpr qint64 CounterMax = Q_INT64_C(1) << 53;
static qint64 counter(const QString &text)
{
    return qBound<qint64>(0, text.toLongLong(), CounterMax);
}
static qint64 added(qint64 sum, qint64 value)
{
    return qMin(CounterMax, sum + value);
}

static Cpu cpuLine(const QString &line)
{
    const auto f = line.simplified().split(' ');
    Cpu cpu;
    if (f.value(0) != "cpu" || f.size() < 5)
        return cpu;
    cpu.total = 0;
    // user nice system idle iowait irq softirq steal (guest is already included in user)
    for (int i = 1; i < f.size() && i <= 8; ++i)
        cpu.total = added(cpu.total, counter(f[i]));
    cpu.idle = added(counter(f[4]), counter(f.value(5)));
    return cpu;
}

bool parseMonitor(const QByteArray &output, MonitorStats &stats)
{
    stats = {};
    QString section;
    Cpu cpu[2];
    qint64 rx[2]{-1, -1}, tx[2]{-1, -1};
    qint64 memFree = -1, memAvailable = -1;
    for (const auto &raw : QString::fromUtf8(output).split('\n'))
    {
        const auto line = raw.trimmed();
        if (line.startsWith("@@"))
        {
            section = line.mid(2);
            continue;
        }
        if (line.isEmpty())
            continue;
        const auto fields = line.simplified().split(' ');
        if (section == "host" && stats.host.isEmpty())
            stats.host = line;
        else if (section == "user" && stats.user.isEmpty())
            stats.user = line;
        else if (section == "uptime")
        {
            const double seconds = fields[0].toDouble();
            stats.uptimeSeconds = seconds > 0 ? qint64(qMin(seconds, double(CounterMax))) : 0; // "nan" and "inf" too
        }
        else if (section == "stat1" || section == "stat2")
            cpu[section == "stat2"] = cpuLine(line);
        else if ((section == "net1" || section == "net2") && line.contains(':'))
        {
            const int i = section == "net2";
            const auto name = line.section(':', 0, 0).trimmed();
            const auto counters = line.section(':', 1).simplified().split(' ');
            if (name == "lo" || counters.size() < 9)
                continue;
            rx[i] = added(qMax<qint64>(rx[i], 0), counter(counters[0]));
            tx[i] = added(qMax<qint64>(tx[i], 0), counter(counters[8]));
        }
        else if (section == "mem")
        {
            const qint64 kb = qMin(counter(fields.value(1)), CounterMax / 1024) * 1024;
            if (fields[0] == "MemTotal:")
                stats.memTotal = kb;
            else if (fields[0] == "MemAvailable:")
                memAvailable = kb;
            else if (fields[0] == "MemFree:")
                memFree = kb;
        }
        else if (section == "procs")
            stats.processes = qMax(0, fields[0].toInt());
        else if (section == "fds")
            stats.fileDescriptors = counter(fields[0]);
        else if (section == "tcp") // one count for IPv4, one for IPv6 (ESTABLISHED = state 01)
            stats.clients = int(qMin<qint64>(qint64(qMax(stats.clients, 0)) + qMax(0, fields[0].toInt()), INT_MAX));
        else if (section == "df" && fields.size() >= 6 && fields[4].endsWith('%'))
        {
            // Local block devices only (no tmpfs, overlay, snap loops); "/" is always kept.
            bool ok = false;
            const int percent = qBound(0, fields[4].chopped(1).toInt(&ok), 100);
            const auto mount = fields.mid(5).join(' ');
            if (!ok)
                continue;
            if (mount == "/")
                stats.diskPercent = percent;
            const bool device = fields[0].startsWith("/dev/") && !fields[0].startsWith("/dev/loop");
            const bool known =
                std::any_of(stats.disks.begin(), stats.disks.end(), [&](const auto &d) { return d.first == mount; });
            // Bind mounts of single files and container plumbing (Docker's /etc/hosts, /var/lib/docker...) are not disks.
            static const QStringList system{"/snap/", "/etc/", "/proc/", "/sys/", "/dev/", "/run/", "/var/lib/docker/"};
            const bool plumbing =
                std::any_of(system.begin(), system.end(), [&](const QString &prefix) { return mount.startsWith(prefix); });
            if ((device || mount == "/") && !known && !plumbing)
                stats.disks.append({mount, percent});
        }
    }
    // Samples are 1 s apart: the differences are already per second.
    const qint64 total = cpu[1].total - cpu[0].total;
    if (cpu[0].total >= 0 && cpu[1].total >= 0)
        stats.cpu = total > 0 ? qBound(0.0, 100.0 * double(total - (cpu[1].idle - cpu[0].idle)) / double(total), 100.0) : 0;
    if (rx[0] >= 0 && rx[1] >= 0)
    {
        stats.rxPerSec = qMax<qint64>(0, rx[1] - rx[0]); // counters were reset: no negative values
        stats.txPerSec = qMax<qint64>(0, tx[1] - tx[0]);
    }
    const qint64 available = memAvailable >= 0 ? memAvailable : memFree; // kernel < 3.14 without MemAvailable
    if (stats.memTotal > 0 && available >= 0)
        stats.memUsed = qMax<qint64>(0, stats.memTotal - available);
    return !stats.host.isEmpty() && (stats.cpu >= 0 || stats.memTotal > 0);
}

QStringList monitorTexts(const MonitorStats &s)
{
    const auto gb = [](qint64 bytes) { return QString::number(double(bytes) / (1024.0 * 1024 * 1024), 'f', 2); };
    const auto mbit = [](qint64 bytes) { return QString::number(double(bytes) * 8 / 1e6, 'f', 2); };
    QStringList disks;
    for (const auto &[mount, percent] : s.disks)
        disks << QString("%1: %2%").arg(mount).arg(percent);
    if (disks.isEmpty() && s.diskPercent >= 0)
        disks << QString("/: %1%").arg(s.diskPercent);
    QString uptime;
    if (s.uptimeSeconds >= 86400)
        uptime = s.uptimeSeconds < 2 * 86400 ? ui("1 day") : ui("%1 days").arg(s.uptimeSeconds / 86400);
    else if (s.uptimeSeconds >= 0)
        uptime = ui("%1 h %2 min").arg(s.uptimeSeconds / 3600).arg(s.uptimeSeconds / 60 % 60);
    return {s.host,
            s.cpu < 0 ? QString{} : QString("%1%").arg(qRound(s.cpu)),
            s.memTotal <= 0 || s.memUsed < 0 ? QString{} : ui("%1 GB / %2 GB").arg(gb(s.memUsed), gb(s.memTotal)),
            s.txPerSec < 0 ? QString{} : ui("%1 Mb/s").arg(mbit(s.txPerSec)),
            s.rxPerSec < 0 ? QString{} : ui("%1 Mb/s").arg(mbit(s.rxPerSec)),
            uptime,
            s.user,
            disks.join("   "),
            s.clients < 0 ? QString{} : ui("%1 clients").arg(s.clients),
            s.processes < 0 ? QString{} : ui("%1 processes").arg(s.processes),
            s.fileDescriptors < 0 ? QString{} : ui("%1 files open").arg(s.fileDescriptors)};
}

// A sample is a few kB: output that reaches this size without the end of a sample is not one.
constexpr qsizetype SampleLimit = 4 * 1024 * 1024;
// Three samples missed in a row (ms).
constexpr int SilenceLimit = (3 * MonitorPeriod + 1) * 1000;

RemoteMonitor::RemoteMonitor(const Command &command, const QStringList &environment, int intervalSeconds, QObject *parent)
    : QObject(parent), command(command), environment(environment), retry(new QTimer(this)), silence(new QTimer(this)),
      interval(qMax(2, intervalSeconds) * 1000)
{
    retry->setSingleShot(true);
    silence->setSingleShot(true);
    QObject::connect(retry, &QTimer::timeout, this, [this] { launch(); });
    // A connection that hangs (server gone without closing it, "df" on a dead network mount) is killed: the failure
    // is shown instead of stale values, and ended() leads to a new process.
    QObject::connect(silence, &QTimer::timeout, this, [this] { process->kill(); });
}

RemoteMonitor::~RemoteMonitor()
{
    stop();
    // This launch is over: its askpass key, and the attempt counters (of a process that did not end in time, too).
    releaseAskpass(environment + command.environment + (process ? process->processEnvironment().toStringList() : QStringList()));
}

QStringList RemoteMonitor::arguments() const
{
    // sessionCommand ends with "-- host": what follows is the remote command. ConnectTimeout as in the SFTP browser:
    // an unreachable server is reported after seconds, not after the system TCP timeout (minutes). One password
    // prompt: a process without a valid password is one failed login on the server, not three (fail2ban).
    return QStringList{"-T", "-o", "ConnectTimeout=10", "-o", "NumberOfPasswordPrompts=1"} + command.arguments +
           QStringList{"sh -c '" + monitorScript() + "'"};
}

void RemoteMonitor::start()
{
    if (!command.error.isEmpty() || command.program != "ssh" || !command.arguments.contains("--"))
    {
        if (failed)
            failed(command.error.isEmpty() ? ui("Monitoring requires an SSH session.") : command.error);
        return;
    }
    active = true;
    rejected = false; // also ends the back-off after a rejected login
    retry->stop();
    launch();
}

void RemoteMonitor::stop()
{
    active = false;
    retry->stop();
    silence->stop();
    if (process && process->state() != QProcess::NotRunning)
    {
        process->kill();
        process->waitForFinished(1000);
    }
}

void RemoteMonitor::launch()
{
    if (!process)
    {
        process = hideConsoleWindow(new QProcess(this));
        process->setStandardInputFile(QProcess::nullDevice());
        QObject::connect(process, &QProcess::readyReadStandardOutput, this, [this] { read(); });
        QObject::connect(process, &QProcess::readyReadStandardError, this, [this] { readErrors(); });
        QObject::connect(process, &QProcess::finished, this, [this](int code) { ended(code); });
        QObject::connect(process, &QProcess::errorOccurred, this,
                         [this](QProcess::ProcessError error)
                         {
                             if (error == QProcess::FailedToStart)
                                 unavailable();
                         });
    }
    if (process->state() != QProcess::NotRunning)
        return;
    // Fresh askpass attempt token for every process: askpass counts attempts per token, and a reused token
    // makes it treat the saved password as already rejected from the second process on.
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto &item : environment + command.environment)
    {
        if (item.contains('='))
            env.insert(item.section('=', 0, 0), item.section('=', 1));
    }
    if (env.contains("CROSSTERM_ASKPASS_TOKEN"))
        env.insert("CROSSTERM_ASKPASS_TOKEN", QUuid::createUuid().toString(QUuid::WithoutBraces));
    process->setProcessEnvironment(env);
    // Full path from PATH, like the other launchers: a bare name would also be looked up in the current folder
    // (Windows).
    const auto program =
        QDir::isAbsolutePath(command.program) ? command.program : QStandardPaths::findExecutable(command.program);
    if (program.isEmpty())
    {
        unavailable();
        return;
    }
    output.clear();
    errors.clear();
    scanned = 0;
    silence->start(qMax(30000, SilenceLimit)); // the first sample comes after the login, jump hosts included
    process->start(program, arguments());
}

void RemoteMonitor::unavailable()
{
    silence->stop();
    if (active)
        retry->start(interval);
    if (failed)
        failed(ui("Cannot start ssh."));
}

void RemoteMonitor::readErrors()
{
    errors += process->readAllStandardError();
    if (errors.size() > 8192)
        errors = errors.right(4096);
}

// For failed(): the last line ssh printed, or its exit code.
static QString failure(const QString &errors, int code)
{
    const auto last = errors.section('\n', -1).trimmed();
    return last.isEmpty() ? ui("Monitoring failed (exit code %1).").arg(code) : last;
}

void RemoteMonitor::read()
{
    output += process->readAllStandardOutput();
    // updated() and failed() may call stop(), which empties the output.
    for (qsizetype newline; active && (newline = output.indexOf('\n', scanned)) >= 0;)
    {
        const bool end = QByteArrayView(output).mid(scanned, newline - scanned).trimmed() == "@@end";
        scanned = newline + 1;
        if (!end)
            continue;
        const auto sample = output.left(scanned);
        output.remove(0, scanned);
        scanned = 0;
        silence->start(SilenceLimit);
        rejected = false; // logged in: back to the normal pace
        readErrors();
        const auto error = QString::fromLocal8Bit(errors).trimmed();
        errors.clear();
        MonitorStats stats;
        if (parseMonitor(sample, stats))
        {
            stats.readAt = monitorClock();
            stats.refreshSeconds = MonitorPeriod;
            if (updated)
                updated(stats);
        }
        else if (failed) // not a Linux server: the script ran, without the values
            failed(failure(error, 0));
    }
    if (output.size() > SampleLimit)
        process->kill();
}

void RemoteMonitor::ended(int code)
{
    silence->stop();
    // Attempt counter that askpass keeps per token (askpass.h): every process has its own token.
    const auto token = process->processEnvironment().value("CROSSTERM_ASKPASS_TOKEN");
    if (!token.isEmpty())
        QFile::remove(QDir::temp().filePath("crossterm-askpass-" + token));
    readErrors();
    const auto error = QString::fromLocal8Bit(errors).trimmed();
    output.clear();
    errors.clear();
    scanned = 0;
    if (!active)
        return; // stopped by stop()
    // Login rejected (no password available yet, or a wrong one): every new process would add failed attempts on
    // the server (fail2ban), so they back off to one per minute. A key loaded or an agent unlocked later is picked
    // up that way; the window starts monitoring again at once when a password is typed (retryLogins).
    if (code == 255 && (error.contains("Permission denied (") || error.contains("Too many authentication failures")))
        rejected = true;
    retry->start(rejected ? qMax(60000, interval) : interval);
    if (failed)
        failed(failure(error, code));
}

// CPU history: the last samples as a filled green area on black, like a small task manager graph.
class Sparkline : public QWidget
{
public:
    QList<double> values;
    explicit Sparkline(QWidget *parent) : QWidget(parent) { setFixedSize(64, 16); }
    void add(double value)
    {
        values.append(value);
        while (values.size() > 32)
            values.removeFirst();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (values.isEmpty())
            return;
        QPolygonF area;
        const double step = double(width() - 1) / 31;
        const double x0 = width() - 1 - step * (values.size() - 1);
        area << QPointF(x0, height());
        for (int i = 0; i < values.size(); ++i)
            area << QPointF(x0 + step * i, height() - 1 - (height() - 2) * values[i] / 100.0);
        area << QPointF(width() - 1, height());
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#3ce43c"), 1));
        p.setBrush(QColor(60, 228, 60, 90));
        p.drawPolygon(area);
    }
};

// Small icons drawn at runtime: host, CPU, RAM, upload, download, uptime, user, disk.
static QPixmap monitorIcon(int kind)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(16, 16) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    switch (kind)
    {
    case 0: // host: screen
        p.setPen(QPen(QColor("#c8c8c8"), 1.2));
        p.setBrush(QColor("#2d6fb8"));
        p.drawRoundedRect(QRectF(1.5, 2.5, 13, 9), 1.5, 1.5);
        p.drawLine(QPointF(5, 14.5), QPointF(11, 14.5));
        p.drawLine(QPointF(8, 11.5), QPointF(8, 14.5));
        break;
    case 1: // CPU: chip with pins
        p.setPen(QPen(QColor("#9ad84a"), 1));
        for (int i = 0; i < 3; ++i)
        {
            const qreal c = 5 + i * 3;
            p.drawLine(QPointF(c, 0.5), QPointF(c, 3));
            p.drawLine(QPointF(c, 13), QPointF(c, 15.5));
            p.drawLine(QPointF(0.5, c), QPointF(3, c));
            p.drawLine(QPointF(13, c), QPointF(15.5, c));
        }
        p.setBrush(QColor("#4f8a1f"));
        p.drawRect(QRectF(3, 3, 10, 10));
        p.fillRect(QRectF(6, 6, 4, 4), QColor("#c9f08f"));
        break;
    case 2: // RAM: memory module
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#2f9e44"));
        p.drawRect(QRectF(0.5, 4, 15, 7));
        p.setBrush(QColor("#111"));
        for (int i = 0; i < 4; ++i)
            p.drawRect(QRectF(2 + i * 3.5, 5.5, 2, 4));
        p.setBrush(QColor("#d4af37"));
        for (int i = 0; i < 7; ++i)
            p.drawRect(QRectF(1.5 + i * 2, 11, 1, 2));
        break;
    case 3: // upload
    case 4: // download
    {
        const bool up = kind == 3;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(up ? "#1fb5a8" : "#2f7fe0"));
        QPolygonF arrow{{8, up ? 1.0 : 15.0}, {14, up ? 7.5 : 8.5}, {10, up ? 7.5 : 8.5}, {10, up ? 14.0 : 2.0},
                        {6, up ? 14.0 : 2.0}, {6, up ? 7.5 : 8.5},  {2, up ? 7.5 : 8.5}};
        p.drawPolygon(arrow);
        break;
    }
    case 5: // uptime: clock
        p.setPen(QPen(QColor("#d0d0d0"), 1.2));
        p.setBrush(QColor("#3a7d44"));
        p.drawEllipse(QRectF(1.5, 1.5, 13, 13));
        p.drawLine(QPointF(8, 8), QPointF(8, 4));
        p.drawLine(QPointF(8, 8), QPointF(11, 9.5));
        break;
    case 6: // user
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#d9a35b"));
        p.drawEllipse(QRectF(5, 1.5, 6, 6));
        p.drawRoundedRect(QRectF(2.5, 8.5, 11, 7), 3, 3);
        break;
    case 8: // connected clients: three linked nodes
        p.setPen(QPen(QColor("#7aa2f7"), 1.3));
        p.drawLine(QPointF(8, 4), QPointF(3.5, 12));
        p.drawLine(QPointF(8, 4), QPointF(12.5, 12));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#7aa2f7"));
        for (const auto c : {QPointF(8, 4), QPointF(3.5, 12), QPointF(12.5, 12)})
            p.drawEllipse(c, 2.5, 2.5);
        break;
    case 9: // processes: stacked bars
        p.setPen(Qt::NoPen);
        for (int r = 0; r < 3; ++r)
        {
            p.setBrush(QColor(r == 0 ? "#e0af68" : r == 1 ? "#9ece6a" : "#7dcfff"));
            p.drawRoundedRect(QRectF(2, 2.5 + r * 4.5, 12 - r * 3, 3), 1, 1);
        }
        break;
    case 10: // file descriptors: document
        p.setPen(QPen(QColor("#c0caf5"), 1.1));
        p.setBrush(QColor("#565f89"));
        p.drawPolygon(QPolygonF{{3.5, 1.5}, {10, 1.5}, {12.5, 4}, {12.5, 14.5}, {3.5, 14.5}});
        p.drawLine(QPointF(5.5, 7), QPointF(10.5, 7));
        p.drawLine(QPointF(5.5, 10), QPointF(10.5, 10));
        break;
    default: // disk
        p.setPen(QPen(QColor("#9aa4ad"), 1));
        p.setBrush(QColor("#5b6670"));
        p.drawRoundedRect(QRectF(1.5, 4.5, 13, 8), 2, 2);
        p.setBrush(QColor("#3ce43c"));
        p.drawEllipse(QRectF(10.5, 9, 2, 2));
        break;
    }
    return pixmap;
}

MonitorBar::MonitorBar(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);
    setStyleSheet("QFrame#monitorCell { border: 1px solid palette(mid); border-radius: 2px; }");
    // Display order: host, CPU, RAM, upload, download, clients, processes, descriptors, uptime, user, disks.
    cells.resize(11);
    labels.resize(11);
    for (const int i : {0, 1, 2, 3, 4, 8, 9, 10, 5, 6, 7})
    {
        auto *cell = new QFrame(this);
        cell->setObjectName("monitorCell");
        auto *row = new QHBoxLayout(cell);
        row->setContentsMargins(6, 2, 8, 2);
        row->setSpacing(6);
        auto *icon = new QLabel(cell);
        icon->setPixmap(monitorIcon(i));
        row->addWidget(icon);
        auto *label = new QLabel(cell);
        label->setTextFormat(Qt::PlainText); // server-provided values; render() escapes them when it colours
        label->setObjectName("monitorText");
        row->addWidget(label);
        if (i == 1)
        {
            cpuGraph = new Sparkline(cell);
            row->addWidget(cpuGraph);
        }
        layout->addWidget(cell);
        cells[i] = cell; // indexed like monitorTexts()
        labels[i] = label;
    }
    age = new QLabel(this);
    age->setTextFormat(Qt::PlainText);
    age->setObjectName("monitorAge");
    age->hide();
    layout->addWidget(age);
    error = new QLabel(this);
    error->setTextFormat(Qt::PlainText); // what the server or ssh printed: never markup
    error->setObjectName("monitorError");
    error->hide();
    layout->addWidget(error);
    layout->addStretch();
    ageTimer = new QTimer(this);
    connect(ageTimer, &QTimer::timeout, this, [this] { refreshAge(); });
}

static int level(double percent)
{
    return percent >= 90 ? 2 : percent >= 75 ? 1 : 0;
}

void MonitorBar::show(const MonitorStats &stats)
{
    texts = monitorTexts(stats);
    cpuLevel = stats.cpu < 0 ? 0 : level(qRound(stats.cpu)); // as displayed
    memLevel = stats.memTotal <= 0 || stats.memUsed < 0 ? 0 : level(100.0 * double(stats.memUsed) / double(stats.memTotal));
    diskLevels.clear();
    for (const auto &disk : stats.disks)
        diskLevels << level(disk.second);
    if (diskLevels.isEmpty() && stats.diskPercent >= 0)
        diskLevels << level(stats.diskPercent);
    readAt = stats.readAt >= 0 ? stats.readAt : monitorClock();
    staleAfter = 3000LL * qMax(1, stats.refreshSeconds);
    error->hide();
    refreshAge();
    applyVisibility();
    if (stats.cpu >= 0)
        cpuGraph->add(stats.cpu);
}

void MonitorBar::refreshAge()
{
    const qint64 old = monitorClock() - readAt;
    stale = old > staleAfter;
    age->setVisible(stale);
    if (stale)
        age->setText(ui("updated %1 s ago").arg(old / 1000));
    render();
    if (!isVisible() || !error->isHidden())
        ageTimer->stop();
    else
        ageTimer->start(stale ? 1000 : int(staleAfter - old) + 20);
}

void MonitorBar::render()
{
    ensurePolished(); // the text colour of the application style sheet
    const QColor text = palette().color(foregroundRole());
    const bool dark = text.lightness() > 128; // light text: dark appearance
    const QString faded = QString("rgba(%1,%2,%3,45%)").arg(text.red()).arg(text.green()).arg(text.blue());
    // Plain text unless a colour is needed; rich text with every value escaped otherwise.
    const auto span = [&](const QString &value, int level)
    {
        const auto escaped = value.toHtmlEscaped();
        if (stale)
            return QString("<span style=\"color:%1\">%2</span>").arg(faded, escaped);
        if (level == 0)
            return escaped;
        return QString("<span style=\"color:%1;font-weight:%2\">%3</span>")
            .arg(level == 2 ? (dark ? "#ff6b61" : "#c62828") : (dark ? "#ffb340" : "#a35a00"), level == 2 ? "600" : "400",
                 escaped);
    };
    for (int i = 0; i < labels.size(); ++i)
    {
        const auto value = texts.value(i);
        QStringList parts{value};
        QList<int> levels{i == 1 ? cpuLevel : i == 2 ? memLevel : 0};
        if (i == 7 && !diskLevels.isEmpty())
        {
            // Each disk in its own colour (they are joined by monitorTexts()), or all of them in the worst one.
            if (const auto disks = value.split("   "); disks.size() == diskLevels.size())
            {
                parts = disks;
                levels = diskLevels;
            }
            else
                levels = {*std::max_element(diskLevels.begin(), diskLevels.end())};
        }
        const bool plain = !stale && std::all_of(levels.begin(), levels.end(), [](int l) { return l == 0; });
        QStringList rich;
        for (int part = 0; !plain && part < parts.size(); ++part)
            rich << span(parts[part], levels[part]);
        labels[i]->setTextFormat(plain ? Qt::PlainText : Qt::RichText);
        labels[i]->setText(plain ? value : rich.join("&nbsp;&nbsp;&nbsp;"));
    }
}

void MonitorBar::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (readAt >= 0 && error->isHidden())
        refreshAge();
}

void MonitorBar::hideEvent(QHideEvent *event)
{
    ageTimer->stop();
    QWidget::hideEvent(event);
}

void MonitorBar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // Another appearance: the threshold colours follow the new text colour.
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) && readAt >= 0)
        render();
}

static QList<int> hiddenValues()
{
    QList<int> hidden;
    for (const auto &v : QSettings().value("monitor/hidden", QVariantList{8, 9, 10}).toList())
        hidden << v.toInt();
    return hidden;
}

void MonitorBar::applyVisibility()
{
    if (error->isVisible())
        return;
    const auto hidden = hiddenValues();
    for (int i = 0; i < cells.size(); ++i)
        cells[i]->setVisible(!texts.value(i).isEmpty() && !hidden.contains(i));
}

QString MonitorBar::summary() const
{
    if (error->isVisible())
        return error->text();
    QStringList parts;
    for (int i = 0; i < cells.size(); ++i)
    {
        if (cells[i]->isVisible())
            parts << texts.value(i);
    }
    return parts.join(" | ");
}

void MonitorBar::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    menu.addAction(ui("Copy"), [this] { QApplication::clipboard()->setText(summary()); });
    menu.addAction(ui("Save to file"),
                   [this]
                   {
                       const auto file = QFileDialog::getSaveFileName(this, ui("Save to file"), "monitoring.txt");
                       QFile out(file);
                       if (!file.isEmpty() && out.open(QIODevice::WriteOnly | QIODevice::Text))
                           out.write((summary() + "\n").toUtf8());
                   });
    menu.addSeparator();
    // Same order as the bar.
    const QList<std::pair<int, QString>> items{{0, ui("Display host information")},
                                               {1, ui("Display CPU information")},
                                               {2, ui("Display RAM information")},
                                               {3, ui("Display network (upload) information")},
                                               {4, ui("Display network (download) information")},
                                               {8, ui("Display network connected clients")},
                                               {9, ui("Display number of processes")},
                                               {10, ui("Display number of file descriptors")},
                                               {5, ui("Display uptime")},
                                               {6, ui("Display connected users")},
                                               {7, ui("Display partitions information")}};
    const auto hidden = hiddenValues();
    for (const auto &[index, text] : items)
    {
        auto *action = menu.addAction(QIcon(monitorIcon(index)), text);
        action->setCheckable(true);
        action->setChecked(!hidden.contains(index));
        connect(action, &QAction::toggled, this,
                [this, index = index](bool on)
                {
                    QVariantList next;
                    for (const int h : hiddenValues())
                        if (h != index)
                            next << h;
                    if (!on)
                        next << index;
                    QSettings().setValue("monitor/hidden", next);
                    applyVisibility();
                });
    }
    menu.exec(event->globalPos());
}

void MonitorBar::showError(const QString &message)
{
    for (auto *cell : cells)
        cell->hide();
    age->hide();
    ageTimer->stop();
    error->setText(message);
    error->show();
}
