#include "monitor.h"
#include "proxy.h"
#include "xserver.h"
#include <QApplication>
#include <QDebug>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <functional>
#include <memory>

static int failures = 0;
#define CHECK(expression)                                                                                                        \
    if (!(expression))                                                                                                           \
    {                                                                                                                            \
        qCritical() << __LINE__ << #expression;                                                                                  \
        ++failures;                                                                                                              \
    }

// Test Linux server data (key and known_hosts) from the CROSSTERM_TEST_DATA variable; without it, the parts
// that use it are skipped. The bundled X server is looked up in dist/xserver of the source tree.
static const QString scratch = qEnvironmentVariable("CROSSTERM_TEST_DATA", "missing-test-data");
static const QString sshKey = scratch + "/srv/client_key";
static const QString knownHosts = scratch + "/linux-test/known_hosts";
static const QString bundledX = QString(CROSSTERM_SOURCE_DIR) + "/dist/xserver";

static bool spin(const std::function<bool()> &done, int milliseconds)
{
    for (QDeadlineTimer deadline(milliseconds); !done();)
    {
        if (deadline.hasExpired())
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

static bool reachable(quint16 port)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    return socket.waitForConnected(1500);
}

// ---------- in-process test proxy (HTTP CONNECT, SOCKS5, SOCKS4/4a) ----------

static void pipeTo(QTcpSocket *from, QTcpSocket *to)
{
    QObject::connect(from, &QTcpSocket::readyRead, to, [from, to] { to->write(from->readAll()); });
    QObject::connect(from, &QTcpSocket::disconnected, to, [to] { to->disconnectFromHost(); });
}

static QString ipv4(const QByteArray &b, int at)
{
    return QHostAddress(quint32(uchar(b[at])) << 24 | uchar(b[at + 1]) << 16 | uchar(b[at + 2]) << 8 | uchar(b[at + 3]))
        .toString();
}

static QTcpServer *proxyServer(const QString &kind)
{
    auto *server = new QTcpServer;
    server->listen(QHostAddress::LocalHost, 0);
    QObject::connect(
        server, &QTcpServer::newConnection, server,
        [server, kind]
        {
            while (auto *client = server->nextPendingConnection())
            {
                auto buffer = std::make_shared<QByteArray>();
                auto greeted = std::make_shared<bool>(kind != "socks5");
                auto reading = std::make_shared<QMetaObject::Connection>();
                *reading = QObject::connect(
                    client, &QTcpSocket::readyRead, client,
                    [=]
                    {
                        *buffer += client->readAll();
                        auto &b = *buffer;
                        QString host;
                        int port = 0, used = -1;
                        if (kind == "http")
                        {
                            used = b.indexOf("\r\n\r\n");
                            if (used < 0)
                                return;
                            used += 4;
                            const auto target = QString::fromLatin1(b.split(' ').value(1));
                            host = target.left(target.lastIndexOf(':')).remove('[').remove(']');
                            port = target.mid(target.lastIndexOf(':') + 1).toInt();
                        }
                        else if (kind == "socks5")
                        {
                            if (!*greeted)
                            {
                                if (b.size() < 3)
                                    return;
                                b.remove(0, 3);
                                client->write(QByteArray("\x05\x00", 2));
                                *greeted = true;
                            }
                            if (b.size() < 5)
                                return;
                            const int atyp = b[3];
                            const int len = atyp == 1 ? 4 : atyp == 4 ? 16 : 1 + uchar(b[4]);
                            if (b.size() < 4 + len + 2)
                                return;
                            host = atyp == 1   ? ipv4(b, 4)
                                   : atyp == 4 ? QHostAddress(reinterpret_cast<const quint8 *>(b.constData() + 4)).toString()
                                               : QString::fromLatin1(b.mid(5, len - 1));
                            port = uchar(b[4 + len]) << 8 | uchar(b[5 + len]);
                            used = 4 + len + 2;
                        }
                        else
                        {
                            const int user = b.indexOf('\0', 8);
                            if (user < 0)
                                return;
                            port = uchar(b[2]) << 8 | uchar(b[3]);
                            host = ipv4(b, 4);
                            used = user + 1;
                            if (host.startsWith("0.0.0."))
                            {
                                const int end = b.indexOf('\0', used);
                                if (end < 0)
                                    return;
                                host = QString::fromLatin1(b.mid(used, end - used));
                                used = end + 1;
                            }
                        }
                        QObject::disconnect(*reading);
                        b.remove(0, used);
                        auto *upstream = new QTcpSocket(client);
                        auto established = std::make_shared<bool>(false);
                        QObject::connect(upstream, &QTcpSocket::connected, client,
                                         [=]
                                         {
                                             *established = true;
                                             client->write(kind == "http"
                                                               ? QByteArray("HTTP/1.1 200 Connection established\r\n\r\n")
                                                           : kind == "socks5" ? QByteArray("\x05\x00\x00\x01\0\0\0\0\0\0", 10)
                                                                              : QByteArray("\x00\x5a\0\0\0\0\0\0", 8));
                                             upstream->write(*buffer + client->readAll());
                                             pipeTo(client, upstream);
                                             pipeTo(upstream, client);
                                         });
                        QObject::connect(upstream, &QTcpSocket::errorOccurred, client,
                                         [=]
                                         {
                                             if (*established)
                                                 return;
                                             client->write(kind == "http"     ? QByteArray("HTTP/1.1 502 Bad Gateway\r\n\r\n")
                                                           : kind == "socks5" ? QByteArray("\x05\x05\x00\x01\0\0\0\0\0\0", 10)
                                                                              : QByteArray("\x00\x5b\0\0\0\0\0\0", 8));
                                             client->disconnectFromHost();
                                         });
                        upstream->connectToHost(host, quint16(port));
                    });
            }
        });
    return server;
}

static QTcpServer *echoServer()
{
    auto *server = new QTcpServer;
    server->listen(QHostAddress::Any, 0);
    QObject::connect(server, &QTcpServer::newConnection, server,
                     [server]
                     {
                         while (auto *socket = server->nextPendingConnection())
                         {
                             QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                              [socket] { socket->write(socket->readAll()); });
                             QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                         }
                     });
    return server;
}

// Starts this executable in --proxy-connect mode and passes the payload through the tunnel.
static int relay(const QString &kind, quint16 proxyPort, const QString &target, quint16 targetPort, const QByteArray &payload,
                 QByteArray &received)
{
    QProcess helper;
    helper.start(QCoreApplication::applicationFilePath(),
                 {"--proxy-connect", kind, "127.0.0.1", QString::number(proxyPort), target, QString::number(targetPort)});
    if (!helper.waitForStarted(5000))
        return -1;
    helper.write(payload);
    received.clear();
    spin(
        [&]
        {
            received += helper.readAllStandardOutput();
            return received.size() >= payload.size() || helper.state() == QProcess::NotRunning;
        },
        20000);
    helper.closeWriteChannel(); // EOF on stdin -> half-close towards the server
    spin([&] { return helper.state() == QProcess::NotRunning; }, 20000);
    received += helper.readAllStandardOutput();
    if (helper.state() != QProcess::NotRunning)
    {
        helper.kill();
        helper.waitForFinished();
        return -2;
    }
    return helper.exitCode();
}

static QStringList sshTestOptions()
{
    return {
        "-F", "none", "-i", sshKey, "-o", "UserKnownHostsFile=" + knownHosts, "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes"};
}

static void testProxy()
{
    // ssh arguments
    const auto args = proxyArguments("http", "proxy.example", 3128, "C:/Program Files/CrossTerm/CrossTerm.exe");
#ifdef Q_OS_WIN
    CHECK(
        args ==
        QStringList(
            {"-o", "ProxyCommand=\"C:\\Program Files\\CrossTerm\\CrossTerm.exe\" --proxy-connect http proxy.example 3128 %h %p"}))
    CHECK(proxyArguments("socks5", "10.0.0.1", 1080, "C:/100%/a.exe").value(1).startsWith("ProxyCommand=\"C:\\100%%\\a.exe\""))
    CHECK(proxyArguments("socks5", "10.0.0.1", 1080, "C:/a\"b.exe").isEmpty())
#else
    CHECK(proxyArguments("socks4", "::1", 1080, "/opt/it's/ct").value(1) ==
          "ProxyCommand='/opt/it'\\''s/ct' --proxy-connect socks4 ::1 1080 %h %p")
#endif
    CHECK(proxyArguments("ftp", "proxy.example", 3128, "x").isEmpty())
    CHECK(proxyArguments("http", "-oEvil", 3128, "x").isEmpty())
    CHECK(proxyArguments("http", "proxy example", 3128, "x").isEmpty())
    CHECK(proxyArguments("http", "proxy.example", 0, "x").isEmpty())
    CHECK(proxyArguments("http", "proxy.example", 70000, "x").isEmpty())
    CHECK(proxyArguments("http", "proxy.example", 3128, "").isEmpty())

    // Binary relay through the three proxy types to a local echo server
    std::unique_ptr<QTcpServer> echo(echoServer());
    QByteArray payload;
    for (int i = 0; i < (1 << 20); ++i)
        payload += char((i * 7919 + i / 251) & 0xff); // includes \0, \r\n, ^Z
    QByteArray received;
    for (const QString kind : {"http", "socks5", "socks4"})
    {
        std::unique_ptr<QTcpServer> proxy(proxyServer(kind));
        for (const QString target : {"127.0.0.1", "localhost"})
        {
            const int code = relay(kind, proxy->serverPort(), target, echo->serverPort(), payload, received);
            CHECK(code == 0)
            CHECK(received == payload)
            if (code != 0 || received != payload)
                qCritical() << kind << target << code << received.size();
        }
        // Closed destination: the proxy refuses and the helper exits with 2
        QTcpServer closed;
        closed.listen(QHostAddress::LocalHost, 0);
        const auto closedPort = closed.serverPort();
        closed.close();
        CHECK(relay(kind, proxy->serverPort(), "127.0.0.1", closedPort, "x", received) == 2)
    }
    CHECK(relay("gopher", 1, "127.0.0.1", 1, "x", received) == 1)

    // real ssh with ProxyCommand to the test container
    if (!reachable(2223) || !QFile::exists(sshKey))
    {
        qInfo() << "SKIPPED ssh ProxyCommand: 127.0.0.1:2223 unreachable";
        return;
    }
    for (const QString kind : {"http", "socks5", "socks4"})
    {
        std::unique_ptr<QTcpServer> proxy(proxyServer(kind));
        QProcess ssh;
        ssh.setStandardInputFile(QProcess::nullDevice()); // ssh for Windows sometimes hangs with stdin open and idle
        ssh.start("ssh", proxyArguments(kind, "127.0.0.1", proxy->serverPort(), QCoreApplication::applicationFilePath()) +
                             sshTestOptions() + QStringList{"-p", "2223", "-l", "linuxtest", "--", "127.0.0.1", "echo PROXY-OK"});
        const bool ended = spin([&] { return ssh.state() == QProcess::NotRunning; }, 30000);
        const auto out = ssh.readAllStandardOutput();
        CHECK(out.trimmed() == "PROXY-OK")
        if (out.trimmed() != "PROXY-OK")
            qCritical() << kind << ended << ssh.exitCode() << out << ssh.readAllStandardError();
        else
            qInfo() << "ssh via" << kind << "proxy: PROXY-OK";
        ssh.kill();
        ssh.waitForFinished();
    }
}

// ---------- monitoring ----------

static const char *fixture =
    "@@host\nserver01\n@@user\nroot\n@@uptime\n1213325.47 4785123.10\n"
    "@@stat1\ncpu  10000 0 5000 85000 0 0 0 0 0 0\n"
    "@@net1\nInter-|   Receive                                                |  Transmit\n"
    " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
    "    lo: 9000000   12000    0    0    0     0          0         0  9000000   12000    0    0    0     0       0          0\n"
    "  eth0: 1000000    5000    0    0    0     0          0         0   500000    4000    0    0    0     0       0          0\n"
    "@@stat2\ncpu  10010 0 5004 85186 0 0 0 0 0 0\n"
    "@@net2\nInter-|   Receive                                                |  Transmit\n"
    " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
    "    lo: 9900000   12500    0    0    0     0          0         0  9900000   12500    0    0    0     0       0          0\n"
    "  eth0: 1000100    5001    0    0    0     0          0         0   502500    4003    0    0    0     0       0          0\n"
    "@@mem\nMemTotal:        4005484 kB\nMemFree:          812344 kB\nMemAvailable:    3271481 kB\n"
    "@@df\nFilesystem     1024-blocks     Used Available Capacity Mounted on\n"
    "/dev/sda1         20511312 10156340   9290400      52% /\n"
    "tmpfs               401000      1200    399800       1% /run\n"
    "/dev/loop3           65536     65536         0     100% /snap/core/1\n"
    "/dev/sdd          1055762868 38231956 963827440      4% /etc/hosts\n"
    "/dev/sda2           999320     58200    872308       6% /boot\n"
    "@@procs\n145\n@@fds\n2048\t0\t9223372036854775807\n@@tcp\n3\n1\n@@end\n";

static void testMonitor()
{
    const auto script = monitorScript();
    CHECK(!script.contains('\'') && !script.contains('"') && !script.contains('\\'))
    MonitorStats stats;
    CHECK(parseMonitor(QByteArray(fixture).replace("\n", "\r\n"), stats)) // with CRLF too
    CHECK(parseMonitor(fixture, stats))
    CHECK(stats.host == "server01" && stats.user == "root")
    CHECK(qAbs(stats.cpu - 7.0) < 1e-9)
    CHECK(stats.memTotal == 4005484LL * 1024 && stats.memUsed == (4005484LL - 3271481) * 1024)
    CHECK(stats.rxPerSec == 100 && stats.txPerSec == 2500) // lo excluded
    CHECK(stats.uptimeSeconds == 1213325 && stats.diskPercent == 52)
    const QStringList expected{"server01",
                               "7%",
                               "0.70 GB / 3.82 GB",
                               "0.02 Mb/s",
                               "0.00 Mb/s",
                               "14 days",
                               "root",
                               "/: 52%   /boot: 6%", // tmpfs and snap loops left out
                               "4 clients",
                               "145 processes",
                               "2048 files open"};
    CHECK(monitorTexts(stats) == expected)
    MonitorStats partial;
    partial.uptimeSeconds = 3725;
    CHECK(monitorTexts(partial).value(5) == "1 h 2 min")
    CHECK(monitorTexts(partial).value(1).isEmpty())
    partial.uptimeSeconds = 90000;
    CHECK(monitorTexts(partial).value(5) == "1 day")
    // Counters reset and kernel without MemAvailable
    auto reset = QByteArray(fixture).replace("1000100    5001", "     10    5001").replace("MemAvailable:    3271481 kB\n", "");
    CHECK(parseMonitor(reset, stats) && stats.rxPerSec == 0 && stats.memUsed == (4005484LL - 812344) * 1024)
    CHECK(!parseMonitor("garbage\n", stats) && stats.cpu < 0)

    MonitorBar bar;
    bar.show(parseMonitor(fixture, stats) ? stats : MonitorStats{});
    QStringList shown;
    for (auto *label : bar.findChildren<QLabel *>("monitorText"))
        shown << label->text();
    QStringList displayOrder; // bar order: the three optional values sit after the network ones
    for (const int i : {0, 1, 2, 3, 4, 8, 9, 10, 5, 6, 7})
        displayOrder << expected[i];
    CHECK(shown == displayOrder)
    bar.showError("connection lost");
    CHECK(bar.findChild<QLabel *>("monitorError")->text() == "connection lost")

    // Command: -T first, script as the remote command after "-- host"
    QJsonObject profile{{"protocol", "ssh"}, {"host", "127.0.0.1"}, {"port", 2223}, {"user", "linuxtest"}, {"identity", sshKey}};
    auto command = sessionCommand(profile, "windows", false);
    CHECK(command.error.isEmpty())
    command.arguments = sshTestOptions() + command.arguments;
    RemoteMonitor monitor(command, {"CROSSTERM_TEST=1"}, 2);
    const auto margs = monitor.arguments();
    CHECK(margs.first() == "-T" && margs.value(margs.size() - 2) == "127.0.0.1" && margs.last() == "sh -c '" + script + "'")
    QString error;
    RemoteMonitor wrong(Command{"telnet", {"host"}, {}, {}});
    wrong.failed = [&](const QString &message) { error = message; };
    wrong.start();
    CHECK(!error.isEmpty() && !wrong.isActive())

    if (!reachable(2223) || !QFile::exists(sshKey))
    {
        qInfo() << "SKIPPED real monitoring: 127.0.0.1:2223 unreachable";
        return;
    }
    int updates = 0;
    MonitorStats live;
    error.clear();
    monitor.updated = [&](const MonitorStats &s)
    {
        live = s;
        ++updates;
    };
    monitor.failed = [&](const QString &message) { error = message; };
    monitor.start();
    CHECK(spin([&] { return updates >= 2 || !error.isEmpty(); }, 40000)) // two rounds: also checks the timer
    monitor.stop();
    CHECK(error.isEmpty())
    if (!error.isEmpty())
        qCritical() << error;
    CHECK(live.user == "linuxtest" && !live.host.isEmpty())
    CHECK(live.cpu >= 0 && live.cpu <= 100)
    CHECK(live.memTotal > 0 && live.memUsed > 0 && live.memUsed <= live.memTotal)
    CHECK(live.rxPerSec >= 0 && live.txPerSec >= 0 && live.uptimeSeconds > 0)
    CHECK(live.diskPercent >= 0 && live.diskPercent <= 100)
    qInfo() << "real monitor:" << monitorTexts(live);
}

// ---------- X server ----------

static void testXServer()
{
    const XServerInfo vcxsrv{"VcXsrv", "C:\\X\\vcxsrv.exe"}, xming{"Xming", "C:\\X\\Xming.exe"},
        xwin{"Cygwin/X", "C:\\cygwin64\\bin\\XWin.exe"}, x410{"X410", "C:\\X\\X410.exe"};
    CHECK(xserverCommand(vcxsrv, 0, "multiwindow").arguments == QStringList({":0", "-multiwindow", "-clipboard", "-wgl", "-ac"}))
    CHECK(xserverCommand(vcxsrv, 3, "rootless").arguments == QStringList({":3", "-rootless", "-clipboard", "-wgl", "-ac"}))
    CHECK(xserverCommand(vcxsrv, 1, "xdmcp", "unix.example").arguments ==
          QStringList({":1", "-query", "unix.example", "-clipboard", "-wgl", "-ac"}))
    CHECK(xserverCommand(xming, 2, "fullscreen").arguments == QStringList({":2", "-fullscreen", "-clipboard", "-ac"}))
    CHECK(xserverCommand(xming, 0, "broadcast").arguments == QStringList({":0", "-broadcast", "-clipboard", "-ac"}))
    CHECK(xserverCommand(xwin, 0, "multiwindow").arguments ==
          QStringList({":0", "-multiwindow", "-clipboard", "-listen", "tcp", "-ac"}))
    CHECK(xserverCommand(xwin, 0, "multiwindow").program == xwin.executable)
    CHECK(xserverCommand(x410, 0, "multiwindow").arguments == QStringList({"/wm"}))
    CHECK(xserverCommand(x410, 0, "fullscreen").arguments == QStringList({"/desktop"}))
    CHECK(!xserverCommand(x410, 1, "multiwindow").error.isEmpty())
    CHECK(!xserverCommand(x410, 0, "xdmcp", "unix.example").error.isEmpty())
    CHECK(!xserverCommand(vcxsrv, 0, "xdmcp", "-evil").error.isEmpty())
    CHECK(!xserverCommand(vcxsrv, 0, "xdmcp", "").error.isEmpty())
    CHECK(!xserverCommand(vcxsrv, 0, "xdmcp", "a;b").error.isEmpty())
    CHECK(!xserverCommand(vcxsrv, 0, "weird").error.isEmpty())
    CHECK(!xserverCommand(vcxsrv, -1, "multiwindow").error.isEmpty())
    CHECK(!xserverCommand({"native", {}}, 0, "multiwindow").error.isEmpty())
    CHECK(!xserverCommand({"x", "C:\\X\\notx.exe"}, 0, "multiwindow").error.isEmpty())
    const int free = freeDisplay(10);
    CHECK(free >= 10)
    {
        QTcpServer busy;
        busy.listen(QHostAddress::Any, quint16(6000 + free));
        CHECK(freeDisplay(free) > free)
    }

#ifdef Q_OS_WIN
    // Detection with a fake "bundled" folder (the file is not executed)
    QTemporaryDir fake;
    QFile dummy(fake.filePath("vcxsrv.exe"));
    CHECK(dummy.open(QIODevice::WriteOnly) && dummy.write("MZ") == 2)
    dummy.close();
    const auto found = detectXServers({fake.path(), fake.path()});
    CHECK(!found.isEmpty() && found.first().name == "VcXsrv" &&
          found.first().executable == QDir::toNativeSeparators(fake.filePath("vcxsrv.exe")))
    CHECK(std::count_if(found.begin(), found.end(),
                        [&](const XServerInfo &x) { return x.executable == found.first().executable; }) == 1)

    // VcXsrv bundled with the distribution: real start on a free display
    const auto bundled = detectXServers({bundledX});
    if (bundled.isEmpty() || !QFile::exists(bundledX + "/vcxsrv.exe"))
    {
        qInfo() << "SKIPPED real X server: VcXsrv not found in" << bundledX;
        return;
    }
    CHECK(bundled.first().name == "VcXsrv")
    XServerManager manager;
    CHECK(manager.start(bundled.first()))
    if (!manager.isRunning())
    {
        qCritical() << manager.error();
        return;
    }
    CHECK(manager.display() >= 0 && manager.displayVariable() == "127.0.0.1:" + QString::number(manager.display()) + ".0")
    qInfo() << "VcXsrv started, DISPLAY =" << manager.displayVariable();
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("DISPLAY", manager.displayVariable());
    QProcess xhost;
    xhost.setProcessEnvironment(env);
    xhost.start(bundledX + "/xhost.exe");
    CHECK(xhost.waitForFinished(15000) && xhost.exitStatus() == QProcess::NormalExit && xhost.exitCode() == 0)
    if (reachable(2223) && QFile::exists(sshKey))
    {
        // Forwarded X11: xeyes must stay alive for 2 s (if it cannot find the display it exits immediately and kill fails).
        QProcess ssh;
        ssh.setProcessEnvironment(env);
        ssh.start("ssh", sshTestOptions() + QStringList{"-Y", "-p", "2223", "-l", "linuxtest", "--", "127.0.0.1",
                                                        "xeyes & p=$!; sleep 2; kill $p && echo XEYES-RAN"});
        spin([&] { return ssh.state() == QProcess::NotRunning; }, 30000);
        const auto out = ssh.readAllStandardOutput(), err = ssh.readAllStandardError();
        if (err.contains("X11 forwarding request failed")) // rejected by sshd: server configuration, not the client's
            qInfo() << "SKIPPED xeyes: the server refuses X11 forwarding";
        else
        {
            CHECK(out.contains("XEYES-RAN"))
            if (!out.contains("XEYES-RAN"))
                qCritical() << out << err;
        }
    }
    else
        qInfo() << "SKIPPED ssh -Y: 127.0.0.1:2223 unreachable";
    manager.stop();
    CHECK(!manager.isRunning() && manager.displayVariable().isEmpty())
#endif
}

int main(int argc, char **argv)
{
    QStringList arguments;
    for (int i = 0; i < argc; ++i)
        arguments << QString::fromLocal8Bit(argv[i]);
    if (arguments.contains("--proxy-connect"))
        return runProxyConnect(arguments); // helper mode, like CrossTerm.exe
    QApplication app(argc, argv);
    testProxy();
    testMonitor();
    testXServer();
    qInfo() << (failures ? "FAILED" : "OK") << failures;
    return failures ? 1 : 0;
}
