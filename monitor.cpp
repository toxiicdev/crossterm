#include "monitor.h"
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QTimer>

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
           "echo @@mem; grep ^Mem /proc/meminfo; echo @@df; df -P /; echo @@end";
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
        else if (section == "df" && fields.size() >= 5 && fields[4].endsWith('%'))
        {
            bool ok = false;
            const int percent = fields[4].chopped(1).toInt(&ok);
            if (ok)
                stats.diskPercent = percent;
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
    QString uptime;
    if (s.uptimeSeconds >= 86400)
        uptime = s.uptimeSeconds < 2 * 86400 ? ui("1 day") : ui("%1 days").arg(s.uptimeSeconds / 86400);
    else if (s.uptimeSeconds >= 0)
        uptime = ui("%1 h %2 min").arg(s.uptimeSeconds / 3600).arg(s.uptimeSeconds / 60 % 60);
    return {s.host,
            s.cpu < 0 ? QString{} : ui("CPU %1%").arg(qRound(s.cpu)),
            s.memTotal <= 0 || s.memUsed < 0 ? QString{} : ui("%1 GB / %2 GB").arg(gb(s.memUsed), gb(s.memTotal)),
            s.txPerSec < 0 ? QString{} : ui("↑ %1 Mb/s").arg(mbit(s.txPerSec)),
            s.rxPerSec < 0 ? QString{} : ui("↓ %1 Mb/s").arg(mbit(s.rxPerSec)),
            uptime,
            s.user,
            s.diskPercent < 0 ? QString{} : ui("/: %1%").arg(s.diskPercent)};
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
        process = new QProcess(this);
        auto env = QProcessEnvironment::systemEnvironment();
        for (const auto &item : environment + command.environment)
        {
            if (item.contains('='))
                env.insert(item.section('=', 0, 0), item.section('=', 1));
        }
        process->setProcessEnvironment(env);
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
    if (process->state() == QProcess::NotRunning)
        process->start(command.program, arguments());
}

MonitorBar::MonitorBar(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 1, 6, 1);
    layout->setSpacing(14);
    auto small = font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    for (int i = 0; i < 8; ++i)
    {
        auto *label = new QLabel(this);
        label->setFont(small);
        layout->addWidget(label);
        labels << label;
    }
    layout->addStretch();
}

void MonitorBar::show(const MonitorStats &stats)
{
    const auto texts = monitorTexts(stats);
    for (int i = 0; i < labels.size(); ++i)
    {
        labels[i]->setText(texts.value(i));
        labels[i]->setVisible(!texts.value(i).isEmpty());
    }
}

void MonitorBar::showError(const QString &message)
{
    for (auto *label : labels)
        label->hide();
    labels[0]->setText(message);
    labels[0]->show();
}
