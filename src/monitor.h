#pragma once
#include "profiles.h"
#include <QObject>
#include <QWidget>
#include <functional>

class QLabel;
class Sparkline;
class QProcess;
class QTimer;

struct MonitorStats
{
    QString host, user;
    double cpu = -1;
    qint64 memUsed = -1, memTotal = -1, rxPerSec = -1, txPerSec = -1, uptimeSeconds = -1; // bytes, bytes/s, seconds
    int diskPercent = -1;                                                                 // root filesystem
    QList<QPair<QString, int>> disks; // mount point -> percent used, local block devices only
    int clients = -1, processes = -1; // established TCP connections, running processes
    qint64 fileDescriptors = -1;      // open file handles (system-wide)
};

// POSIX sh script for Linux (no quotes): "@@<name>" sections read by parseMonitor.
QString monitorScript();
// Pure parser for the output of monitorScript(); false if everything needed is missing.
bool parseMonitor(const QByteArray &output, MonitorStats &stats);
// Status bar texts in order: host, CPU, RAM, upload, download, uptime, user, disk, clients, processes,
// file descriptors (empty if unknown).
QStringList monitorTexts(const MonitorStats &stats);

// Runs "ssh -T ... -- host sh -c '<script>'" every intervalSeconds (never two overlapping runs).
class RemoteMonitor : public QObject
{
public:
    RemoteMonitor(const Command &command, const QStringList &environment = {}, int intervalSeconds = 5,
                  QObject *parent = nullptr);
    ~RemoteMonitor() override;
    void start();
    void stop();
    bool isActive() const;
    // Actual program and arguments (for tests and diagnostics).
    QString program() const { return command.program; }
    QStringList arguments() const;
    std::function<void(const MonitorStats &)> updated;
    std::function<void(const QString &)> failed;

private:
    void run();
    Command command;
    QStringList environment;
    QTimer *timer;
    QProcess *process = nullptr;
};

class MonitorBar : public QWidget
{
public:
    explicit MonitorBar(QWidget *parent = nullptr);
    using QWidget::show;
    void show(const MonitorStats &stats);
    void showError(const QString &message);

protected:
    // Right click: Copy, Save to file and which values to display (saved in "monitor/hidden").
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void applyVisibility();
    QString summary() const;
    QList<QWidget *> cells; // host, CPU, RAM, upload, download, uptime, user, disks
    QList<QLabel *> labels;
    Sparkline *cpuGraph = nullptr;
    QLabel *error = nullptr;
};
