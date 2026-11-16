#pragma once
// Built-in servers (static HTTP, TFTP) and network tools (port scanner).
// No moc: connections use lambdas and events are delivered through std::function.
#include <QDialog>
#include <QHostAddress>
#include <QList>
#include <QString>
#include <functional>
#include <memory>

class QTcpServer;
class QTcpSocket;
class QUdpSocket;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QProgressBar;
class QListWidget;

// Real path of `relative` inside `root`, empty if it escapes the root (.., drives, external symlinks) or names a
// hidden file or folder (a component starting with a dot).
QString jailedPath(const QString &root, const QString &relative);

// Wake On LAN magic packet: 6 bytes 0xFF, then the MAC address 16 times. Empty for an invalid address (accepted:
// 12 hex digits, optionally separated by ':', '-' or '.').
QByteArray wakeOnLanPacket(const QString &mac);
// Sends the packet as a UDP broadcast (or to the given address); false with the reason in error.
bool sendWakeOnLan(const QString &mac, const QHostAddress &to, quint16 port, QString *error = nullptr);

// Read-only HTTP/1.1 server (GET/HEAD, Range, directory listing) confined to a folder.
class HttpFileServer
{
public:
    HttpFileServer();
    ~HttpFileServer();
    // port 0 = free port chosen by the system; bindAll = listen on all interfaces.
    bool start(const QString &rootDirectory, quint16 port, bool bindAll = false);
    void stop();
    bool isRunning() const;
    quint16 port() const;
    int requestCount() const { return requests; }
    QString errorString() const { return error; }

private:
    void serve(QTcpSocket *socket);
    std::unique_ptr<QTcpServer> server;
    QString root, error;
    int requests = 0;
};

// TFTP server (RFC 1350, blksize/tsize/timeout options RFC 2347-2349) over UDP.
class TftpServer
{
public:
    TftpServer();
    ~TftpServer();
    bool start(const QString &rootDirectory, quint16 port, bool allowWrite = false, bool bindAll = false);
    void stop();
    bool isRunning() const;
    quint16 port() const;
    QString errorString() const { return error; }

private:
    void request(const QByteArray &packet, const QHostAddress &peer, quint16 peerPort);
    std::unique_ptr<QUdpSocket> socket;
    std::unique_ptr<QObject> transfers; // parent of the active transfers
    QString root, error;
    bool writable = false;
};

// "22,80,8000-8010" -> sorted, unique ports; empty list if any element is invalid.
QList<int> parsePortList(const QString &text);

// TCP connect scan of a host (or a /24../32 network) with a limited number of concurrent connections.
class PortScanner
{
public:
    ~PortScanner();
    std::function<void(const QString &host, int port)> found;
    std::function<void(int done, int total)> progress;
    std::function<void()> finished; // only at the end of the scan, not after cancel()
    // target: "host", "10.0.0.5" or "10.0.0.0/24" (minimum prefix 24).
    bool start(const QString &target, const QList<int> &ports, int concurrency = 64, int timeoutMs = 1500);
    void cancel();
    bool isRunning() const { return holder != nullptr; }

private:
    void launch();
    QObject *holder = nullptr; // parent of the sockets in progress
    QStringList hosts;
    QList<int> ports;
    int next = 0, done = 0, total = 0, timeout = 1500;
    QObject queue; // context of the queued launch() calls: they are dropped with the scanner
};

// Start/stop of the HTTP, TFTP and FTP servers (the latter injected by the caller).
class ServersDialog : public QDialog
{
public:
    explicit ServersDialog(QWidget *parent = nullptr);
    HttpFileServer &http() { return httpServer; }
    TftpServer &tftp() { return tftpServer; }
    // Attaches an external FTP server; without this call the FTP row stays disabled.
    void setFtpControl(std::function<bool(const QString &root, quint16 port, bool allowWrite, bool bindAll)> start,
                       std::function<void()> stop, std::function<bool()> isRunning);
    void refresh();

private:
    struct Row
    {
        QLineEdit *root;
        QSpinBox *port;
        QCheckBox *write;
        QPushButton *start, *stop;
        QLabel *status;
    };
    Row addRow(int line, const QString &name, const QString &key, quint16 port, bool canWrite);
    HttpFileServer httpServer;
    TftpServer tftpServer;
    Row httpRow, tftpRow, ftpRow;
    QCheckBox *bindAll;
    std::function<bool(const QString &, quint16, bool, bool)> ftpStart;
    std::function<void()> ftpStop;
    std::function<bool()> ftpRunning;
};

class PortScannerDialog : public QDialog
{
public:
    explicit PortScannerDialog(QWidget *parent = nullptr);

private:
    void finish();
    PortScanner scanner;
    QLineEdit *host, *ports;
    QPushButton *startButton, *stopButton;
    QProgressBar *bar;
    QListWidget *results;
};
