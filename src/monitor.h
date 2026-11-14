#pragma once
#include "profiles.h"
#include <QElapsedTimer>
#include <QObject>
#include <QWidget>
#include <functional>

class QLabel;
class Sparkline;
class QProcess;
class QTimer;

// Seconds from one sample of monitorScript() to the next.
constexpr int MonitorPeriod = 5;

struct MonitorStats
{
    QString host, user;
    double cpu = -1;
    qint64 memUsed = -1, memTotal = -1, rxPerSec = -1, txPerSec = -1, uptimeSeconds = -1; // bytes, bytes/s, seconds
    int diskPercent = -1;                                                                 // root filesystem
    QList<QPair<QString, int>> disks; // mount point -> percent used, local block devices only
    int clients = -1, processes = -1; // established TCP connections, running processes
    qint64 fileDescriptors = -1;      // open file handles (system-wide)
    // Set by RemoteMonitor: when the values were read (monitorClock(); -1: just now) and how often they are.
    qint64 readAt = -1;
    int refreshSeconds = MonitorPeriod;
};

// Steady clock in ms (MonitorStats::readAt).
qint64 monitorClock();

// POSIX sh script for Linux (no quotes): a loop that prints one sample every MonitorPeriod seconds, in "@@<name>"
// sections read by parseMonitor and closed by a line "@@end", until its output is closed.
QString monitorScript();
// Pure parser for one sample of monitorScript(); false if everything needed is missing.
bool parseMonitor(const QByteArray &output, MonitorStats &stats);
// Status bar texts in order: host, CPU, RAM, upload, download, uptime, user, disk, clients, processes,
// file descriptors (empty if unknown).
QStringList monitorTexts(const MonitorStats &stats);

// One long-lived "ssh -T ... -- host sh -c '<script>'" per monitor: it logs in once, then every sample the remote
// loop sends is reported by updated() as it arrives. When the process ends, or is killed because no complete sample
// came for about three periods, failed() is called and a new process starts intervalSeconds later (never two at the
// same time). After a rejected login the new processes back off to one per minute until one logs in; start()
// starts one at once at the normal pace again.
class RemoteMonitor : public QObject
{
public:
    RemoteMonitor(const Command &command, const QStringList &environment = {}, int intervalSeconds = 5,
                  QObject *parent = nullptr);
    ~RemoteMonitor() override;
    void start();
    void stop();
    bool isActive() const { return active; }
    // Actual program and arguments (for tests and diagnostics).
    QString program() const { return command.program; }
    QStringList arguments() const;
    std::function<void(const MonitorStats &)> updated;
    std::function<void(const QString &)> failed;

private:
    void launch();        // a new ssh process, unless one is running
    void read();          // the complete samples received so far
    void readErrors();    // what ssh printed as errors
    void ended(int code); // the ssh process is over
    void unavailable();   // ssh cannot be started
    Command command;
    QStringList environment;
    QProcess *process = nullptr;
    QTimer *retry;         // single shot: the next ssh process, after the last one ended
    QTimer *silence;       // single shot: the ssh process sent no complete sample for too long
    QByteArray output;     // the sample being received
    qsizetype scanned = 0; // start of the first line of output not looked at yet
    QByteArray errors;     // end of what ssh printed as errors since the last sample
    int interval;          // ms from the end of an ssh process to the next one
    bool active = false;   // between start() and stop()
    bool rejected = false; // the last login was rejected: backing off
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
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void applyVisibility();
    QString summary() const;
    void render();          // texts with the colours of their thresholds, dimmed when stale
    void refreshAge();      // stale or not, and the timer that tells
    QList<QWidget *> cells; // host, CPU, RAM, upload, download, uptime, user, disks
    QList<QLabel *> labels;
    Sparkline *cpuGraph = nullptr;
    QLabel *error = nullptr, *age = nullptr;
    QStringList texts;                      // monitorTexts() of the values on display
    int cpuLevel = 0, memLevel = 0;         // 0 normal, 1 amber (>= 75 %), 2 red (>= 90 %)
    QList<int> diskLevels;                  // one per disk on display
    qint64 readAt = -1, staleAfter = 15000; // ms: no update for 3 refresh intervals = stale
    bool stale = false;
    // Single shot until the values become stale, then every second for their age; stopped while hidden.
    QTimer *ageTimer;
};
