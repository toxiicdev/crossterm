#include "monitor.h"
#include <QApplication>
#include <QCoreApplication>
#include <QFrame>
#include <QPainter>
#include <algorithm>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QTimer>
#include <QUuid>

static QString ui(const char *text)
{
    return QCoreApplication::translate("CrossTerm", text);
}

QString monitorScript()
{
    // No quotes or backslashes: it is passed as sh -c '<script>' through ssh.
    return "echo @@host; uname -n; echo @@user; id -un; echo @@uptime; cat /proc/uptime; "
           "echo @@stat1; head -n 1 /proc/stat; echo @@net1; cat /proc/net/dev; sleep 1; "
           "echo @@stat2; head -n 1 /proc/stat; echo @@net2; cat /proc/net/dev; "
           "echo @@mem; grep ^Mem /proc/meminfo; echo @@df; df -P; echo @@end";
}

struct Cpu
{
    qint64 total = -1, idle = 0;
};

static Cpu cpuLine(const QString &line)
{
    const auto f = line.simplified().split(' ');
    Cpu cpu;
    if (f.value(0) != "cpu" || f.size() < 5)
        return cpu;
    cpu.total = 0;
    // user nice system idle iowait irq softirq steal (guest is already included in user)
    for (int i = 1; i < f.size() && i <= 8; ++i)
        cpu.total += f[i].toLongLong();
    cpu.idle = f[4].toLongLong() + f.value(5).toLongLong();
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
            stats.uptimeSeconds = qint64(fields[0].toDouble());
        else if (section == "stat1" || section == "stat2")
            cpu[section == "stat2"] = cpuLine(line);
        else if ((section == "net1" || section == "net2") && line.contains(':'))
        {
            const int i = section == "net2";
            const auto name = line.section(':', 0, 0).trimmed();
            const auto counters = line.section(':', 1).simplified().split(' ');
            if (name == "lo" || counters.size() < 9)
                continue;
            rx[i] = qMax<qint64>(rx[i], 0) + counters[0].toLongLong();
            tx[i] = qMax<qint64>(tx[i], 0) + counters[8].toLongLong();
        }
        else if (section == "mem")
        {
            const qint64 kb = fields.value(1).toLongLong() * 1024;
            if (fields[0] == "MemTotal:")
                stats.memTotal = kb;
            else if (fields[0] == "MemAvailable:")
                memAvailable = kb;
            else if (fields[0] == "MemFree:")
                memFree = kb;
        }
        else if (section == "df" && fields.size() >= 6 && fields[4].endsWith('%'))
        {
            // Local block devices only (no tmpfs, overlay, snap loops); "/" is always kept.
            bool ok = false;
            const int percent = fields[4].chopped(1).toInt(&ok);
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
        stats.memUsed = stats.memTotal - available;
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
            disks.join("   ")};
}

RemoteMonitor::RemoteMonitor(const Command &command, const QStringList &environment, int intervalSeconds, QObject *parent)
    : QObject(parent), command(command), environment(environment), timer(new QTimer(this))
{
    timer->setInterval(qMax(2, intervalSeconds) * 1000); // the script itself already takes about 1 s
    QObject::connect(timer, &QTimer::timeout, this, [this] { run(); });
}

RemoteMonitor::~RemoteMonitor()
{
    stop();
}

QStringList RemoteMonitor::arguments() const
{
    // sessionCommand ends with "-- host": what follows is the remote command.
    return QStringList{"-T"} + command.arguments + QStringList{"sh -c '" + monitorScript() + "'"};
}

void RemoteMonitor::start()
{
    if (!command.error.isEmpty() || command.program != "ssh" || !command.arguments.contains("--"))
    {
        if (failed)
            failed(command.error.isEmpty() ? ui("Monitoring requires an SSH session.") : command.error);
        return;
    }
    timer->start();
    run();
}

void RemoteMonitor::stop()
{
    timer->stop();
    if (process && process->state() != QProcess::NotRunning)
    {
        process->kill();
        process->waitForFinished(1000);
    }
}

bool RemoteMonitor::isActive() const
{
    return timer->isActive();
}

void RemoteMonitor::run()
{
    if (!process)
    {
        process = hideConsoleWindow(new QProcess(this));
        process->setStandardInputFile(QProcess::nullDevice());
        QObject::connect(process, &QProcess::finished, this,
                         [this](int code, QProcess::ExitStatus status)
                         {
                             if (!timer->isActive())
                                 return; // stopped by stop()
                             MonitorStats stats;
                             if (status == QProcess::NormalExit && parseMonitor(process->readAllStandardOutput(), stats))
                             {
                                 if (updated)
                                     updated(stats);
                                 return;
                             }
                             const auto lines = QString::fromLocal8Bit(process->readAllStandardError()).trimmed().split('\n');
                             if (failed)
                                 failed(lines.last().trimmed().isEmpty() ? ui("Monitoring failed (exit code %1).").arg(code)
                                                                         : lines.last().trimmed());
                         });
        QObject::connect(process, &QProcess::errorOccurred, this,
                         [this](QProcess::ProcessError error)
                         {
                             if (error == QProcess::FailedToStart && failed)
                                 failed(ui("Cannot start ssh."));
                         });
    }
    if (process->state() != QProcess::NotRunning)
        return;
    // Fresh askpass attempt token on every run: askpass counts attempts per token, and a reused token
    // makes it treat the saved password as already rejected from the second run on.
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto &item : environment + command.environment)
    {
        if (item.contains('='))
            env.insert(item.section('=', 0, 0), item.section('=', 1));
    }
    if (env.contains("CROSSTERM_ASKPASS_TOKEN"))
        env.insert("CROSSTERM_ASKPASS_TOKEN", QUuid::createUuid().toString(QUuid::WithoutBraces));
    process->setProcessEnvironment(env);
    process->start(command.program, arguments());
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
    for (int i = 0; i < 8; ++i)
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
        label->setObjectName("monitorText");
        row->addWidget(label);
        if (i == 1)
        {
            cpuGraph = new Sparkline(cell);
            row->addWidget(cpuGraph);
        }
        layout->addWidget(cell);
        cells << cell;
        labels << label;
    }
    error = new QLabel(this);
    error->setObjectName("monitorError");
    error->hide();
    layout->addWidget(error);
    layout->addStretch();
}

void MonitorBar::show(const MonitorStats &stats)
{
    const auto texts = monitorTexts(stats);
    error->hide();
    for (int i = 0; i < labels.size(); ++i)
    {
        labels[i]->setText(texts.value(i));
        cells[i]->setVisible(!texts.value(i).isEmpty());
    }
    if (stats.cpu >= 0)
        cpuGraph->add(stats.cpu);
}

void MonitorBar::showError(const QString &message)
{
    for (auto *cell : cells)
        cell->hide();
    error->setText(message);
    error->show();
}
