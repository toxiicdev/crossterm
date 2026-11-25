#include "servers.h"
#include <QApplication>
#include <QCheckBox>
#include <QDebug>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkDatagram>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <functional>

static int failures = 0;
#define CHECK(expression)                                                                                                        \
    if (!(expression))                                                                                                           \
    {                                                                                                                            \
        qCritical() << __LINE__ << #expression;                                                                                  \
        ++failures;                                                                                                              \
    }

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

static void write(const QString &path, const QByteArray &data)
{
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(data);
}

static QByteArray read(const QString &path)
{
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    return file.readAll();
}

static quint16 freeTcpPort()
{
    QTcpServer server;
    server.listen(QHostAddress::LocalHost, 0);
    return server.serverPort();
}

static QByteArray pattern(int size)
{
    QByteArray data(size, 0);
    for (int i = 0; i < size; ++i)
        data[i] = char((i * 7 + i / 251) & 0xff);
    return data;
}

// ---------- HTTP: raw requests, full response until the server closes ----------

static QByteArray http(quint16 port, const QByteArray &request)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    QByteArray response;
    QObject::connect(&socket, &QTcpSocket::readyRead, [&] { response += socket.readAll(); });
    if (!spin([&] { return socket.state() == QAbstractSocket::ConnectedState; }, 3000))
        return {};
    socket.write(request);
    spin([&] { return socket.state() == QAbstractSocket::UnconnectedState; }, 5000);
    return response + socket.readAll();
}

static QByteArray body(const QByteArray &response)
{
    return response.mid(response.indexOf("\r\n\r\n") + 4);
}

static void testHttp(const QString &root)
{
    HttpFileServer server;
    CHECK(!server.start(root + "/missing", 0));
    CHECK(server.start(root, 0));
    CHECK(server.isRunning() && server.port() != 0);
    const quint16 port = server.port();

    QByteArray r = http(port, "GET /hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(r.startsWith("HTTP/1.1 200 OK\r\n"));
    CHECK(r.contains("Content-Type: text/plain"));
    CHECK(r.contains("Content-Length: 11\r\n"));
    CHECK(body(r) == "hello world");

    r = http(port, "HEAD /hello.txt HTTP/1.1\r\n\r\n");
    CHECK(r.startsWith("HTTP/1.1 200") && r.contains("Content-Length: 11\r\n") && body(r).isEmpty());

    r = http(port, "GET /hello.txt HTTP/1.1\r\nRange: bytes=2-5\r\n\r\n");
    CHECK(r.startsWith("HTTP/1.1 206") && r.contains("Content-Range: bytes 2-5/11\r\n") && body(r) == "llo ");
    r = http(port, "GET /hello.txt HTTP/1.1\r\nrange: bytes=-5\r\n\r\n");
    CHECK(r.startsWith("HTTP/1.1 206") && body(r) == "world");
    r = http(port, "GET /hello.txt HTTP/1.1\r\nRange: bytes=6-\r\n\r\n");
    CHECK(body(r) == "world");
    r = http(port, "GET /hello.txt HTTP/1.1\r\nRange: bytes=100-\r\n\r\n");
    CHECK(r.startsWith("HTTP/1.1 416") && r.contains("Content-Range: bytes */11"));

    const QByteArray big = pattern(1 << 20);
    r = http(port, "GET /sub/big.bin HTTP/1.1\r\n\r\n");
    CHECK(r.contains("Content-Length: 1048576\r\n") && body(r) == big);
    r = http(port, "GET /sub/big.bin HTTP/1.1\r\nRange: bytes=1000-1999\r\n\r\n");
    CHECK(body(r) == big.mid(1000, 1000));

    r = http(port, "GET / HTTP/1.1\r\n\r\n");
    CHECK(r.startsWith("HTTP/1.1 200") && r.contains("text/html"));
    const QByteArray list = body(r);
    CHECK(list.contains(">R&amp;D #1.txt<") && !list.contains("R&D"));
    CHECK(list.contains("href=\"sub/\"") && list.contains("href=\"R%26D%20%231.txt\""));
    CHECK(list.indexOf("sub/") < list.indexOf("a.txt") && list.indexOf("a.txt") < list.indexOf("hello.txt")); // folders first
    r = http(port, "GET /sub HTTP/1.1\r\n\r\n");
    CHECK(r.startsWith("HTTP/1.1 301") && r.contains("Location: /sub/\r\n"));
    r = http(port, "GET /sub/ HTTP/1.1\r\n\r\n");
    CHECK(body(r).contains("big.bin") && body(r).contains("href=\"../\""));
    r = http(port, "GET /R%26D%20%231.txt?q=1 HTTP/1.1\r\n\r\n");
    CHECK(body(r) == "x");

    CHECK(http(port, "GET /nothing.txt HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 404"));
    CHECK(http(port, "GET /../secret.txt HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 403"));
    CHECK(http(port, "GET /sub/../../secret.txt HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 403"));
    CHECK(http(port, "GET /%2e%2e/secret.txt HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 403"));
    CHECK(http(port, "GET /%2E%2E%2Fsecret.txt HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 403"));
    CHECK(http(port, "GET /..%5csecret.txt HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 403"));
    CHECK(http(port, "GET /C:/Windows/win.ini HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 403"));
    CHECK(http(port, "GET http://x/hello.txt HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 400"));
    CHECK(http(port, "POST /hello.txt HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 405"));
    CHECK(http(port, "garbage\r\n\r\n").startsWith("HTTP/1.1 400"));
    CHECK(server.requestCount() >= 20);

    server.stop();
    CHECK(!server.isRunning());
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    // A connection can only be TCP "self-connect" (source port == closed server port, seen on macOS/Linux).
    CHECK(!socket.waitForConnected(1000) || socket.localPort() == socket.peerPort());
}

static void testJail(const QString &root)
{
    const QString base = QFileInfo(root).canonicalFilePath();
    CHECK(jailedPath(root, "/hello.txt") == base + "/hello.txt");
    CHECK(jailedPath(root, "sub\\big.bin") == base + "/sub/big.bin");
    CHECK(jailedPath(root, "/") == base);
    CHECK(jailedPath(root, "new.txt") == base + "/new.txt");
    CHECK(jailedPath(root, "..").isEmpty());
    CHECK(jailedPath(root, "sub/../../x").isEmpty());
    CHECK(jailedPath(root, "...").isEmpty());
    CHECK(jailedPath(root, "C:/Windows/win.ini").isEmpty());
    CHECK(jailedPath(root, "hello.txt:stream").isEmpty());
    CHECK(jailedPath(root, "nodir/x.txt").isEmpty()); // nonexistent parent folder
    // A link to a folder outside the root (a junction on Windows: needs no privileges, and canonicalFilePath does
    // not resolve it): neither what is in it nor a new file in it.
    QTemporaryDir outside;
    QFile secret(outside.filePath("secret.txt"));
    secret.open(QIODevice::WriteOnly);
    secret.close();
#ifdef Q_OS_WIN
    QProcess::execute("cmd", {"/c", "mklink", "/J", QDir::toNativeSeparators(root + "/escape"),
                              QDir::toNativeSeparators(outside.path())});
#else
    QFile::link(outside.path(), root + "/escape");
#endif
    CHECK(QFileInfo::exists(root + "/escape/secret.txt"));
    CHECK(jailedPath(root, "escape/secret.txt").isEmpty());
    CHECK(jailedPath(root, "escape/new.txt").isEmpty());
    QDir().rmdir(root + "/escape"); // the junction, not its contents
    QFile::remove(root + "/escape");
}

// ---------- TFTP: minimal client ----------

struct TftpClient
{
    QUdpSocket socket;
    quint16 serverPort = 0, tid = 0;
    TftpClient() { socket.bind(QHostAddress::LocalHost, 0); }
    void send(const QByteArray &packet, quint16 port) { socket.writeDatagram(packet, QHostAddress::LocalHost, port); }
    QByteArray receive()
    {
        if (!spin([&] { return socket.hasPendingDatagrams(); }, 5000))
            return {};
        const QNetworkDatagram datagram = socket.receiveDatagram();
        tid = quint16(datagram.senderPort());
        return datagram.data();
    }
};

static QByteArray packet(int opcode, quint16 number)
{
    QByteArray p(4, 0);
    p[1] = char(opcode);
    p[2] = char(number >> 8);
    p[3] = char(number);
    return p;
}

static QByteArray requestPacket(int opcode, const QByteArray &file, const QByteArray &mode, const QByteArray &options = {})
{
    return QByteArray(1, 0) + char(opcode) + file + '\0' + mode + '\0' + options;
}

static int opcode(const QByteArray &p)
{
    return p.size() >= 2 ? uchar(p[1]) : -1;
}

static int number(const QByteArray &p)
{
    return p.size() >= 4 ? (uchar(p[2]) << 8 | uchar(p[3])) : -1;
}

// Downloads a file; with blksize > 0 negotiates options. Returns the data or "ERROR".
static QByteArray tftpGet(quint16 port, const QByteArray &file, const QByteArray &mode, int blksize, QByteArray *oack = nullptr)
{
    TftpClient client;
    const QByteArray options = blksize ? QByteArray("blksize\0", 8) + QByteArray::number(blksize) +
                                             QByteArray("\0tsize\0"
                                                        "0\0",
                                                        9)
                                       : QByteArray();
    client.send(requestPacket(1, file, mode, options), port);
    QByteArray p = client.receive(), data;
    if (opcode(p) == 6)
    {
        if (oack)
            *oack = p;
        client.send(packet(4, 0), client.tid);
        p = client.receive();
    }
    const int size = blksize ? blksize : 512;
    for (quint16 expected = 1;; ++expected)
    {
        if (opcode(p) != 3 || number(p) != expected)
            return "ERROR";
        data += p.mid(4);
        client.send(packet(4, expected), client.tid);
        if (p.size() - 4 < size)
            return data;
        p = client.receive();
    }
}

// Uploads a file in 512-byte blocks; returns false on any error.
static bool tftpPut(quint16 port, const QByteArray &file, const QByteArray &data, int *errorCode = nullptr)
{
    TftpClient client;
    client.send(requestPacket(2, file, "octet"), port);
    QByteArray p = client.receive();
    if (opcode(p) == 5 && errorCode)
        *errorCode = number(p);
    if (opcode(p) != 4 || number(p) != 0)
        return false;
    for (quint16 block = 1;; ++block)
    {
        const QByteArray chunk = data.mid((block - 1) * 512, 512);
        client.send(packet(3, block) + chunk, client.tid);
        p = client.receive();
        if (opcode(p) != 4 || number(p) != block)
            return false;
        if (chunk.size() < 512)
            return true;
    }
}

static void testTftp(const QString &root)
{
    TftpServer server;
    CHECK(server.start(root, 0));
    const quint16 port = server.port();
    CHECK(server.isRunning() && port != 0);

    const QByteArray big = pattern(200 * 1024);
    write(root + "/tftp.bin", big);
    QByteArray oack;
    CHECK(tftpGet(port, "tftp.bin", "octet", 1428, &oack) == big);
    CHECK(oack.contains(QByteArray("blksize\0"
                                   "1428\0",
                                   13)) &&
          oack.contains(QByteArray("tsize\0"
                                   "204800\0",
                                   13)));
    CHECK(tftpGet(port, "/tftp.bin", "octet", 0) == big); // no options, 512-byte blocks
    {
        // retransmission: without an ACK the server repeats the same block after the negotiated timeout (1 s)
        TftpClient client;
        client.send(requestPacket(1, "hello.txt", "octet",
                                  QByteArray("timeout\0"
                                             "1\0",
                                             10)),
                    port);
        CHECK(opcode(client.receive()) == 6);
        client.send(packet(4, 0), client.tid);
        const QByteArray first = client.receive(), again = client.receive();
        CHECK(opcode(first) == 3 && first == again);
        client.send(packet(4, 1), client.tid);
    }
    CHECK(tftpGet(port, "hello.txt", "OCTET", 0) == "hello world");
    write(root + "/lines.txt", "a\nb\rc");
    CHECK(tftpGet(port, "lines.txt", "netascii", 0) == QByteArray("a\r\nb\r\0c", 7));
    write(root + "/exact.bin", pattern(1024)); // exact multiple: empty final block
    CHECK(tftpGet(port, "exact.bin", "octet", 0) == pattern(1024));

    CHECK(tftpGet(port, "../secret.txt", "octet", 0) == "ERROR");
    CHECK(tftpGet(port, "..\\secret.txt", "octet", 0) == "ERROR");
    CHECK(tftpGet(port, "C:/Windows/win.ini", "octet", 0) == "ERROR");
    CHECK(tftpGet(port, "missing.bin", "octet", 0) == "ERROR");
    CHECK(tftpGet(port, "hello.txt", "mail", 0) == "ERROR");

    int code = -1;
    CHECK(!tftpPut(port, "upload.bin", "data", &code) && code == 2);
    CHECK(!QFile::exists(root + "/upload.bin"));

    CHECK(server.start(root, port, true));
    const QByteArray upload = pattern(1500);
    CHECK(tftpPut(port, "upload.bin", upload));
    CHECK(read(root + "/upload.bin") == upload);
    CHECK(tftpPut(port, "sub/empty.bin", {}) && QFile::exists(root + "/sub/empty.bin"));
    CHECK(!tftpPut(port, "../escape.bin", "x"));
    CHECK(!QFile::exists(root + "/../escape.bin"));

    server.stop();
    CHECK(!server.isRunning());
}

// ---------- Port scanner ----------

static void testScanner()
{
    CHECK(parsePortList("22,80,8000-8003") == (QList<int>{22, 80, 8000, 8001, 8002, 8003}));
    CHECK(parsePortList(" 80 , 22,22,21-23") == (QList<int>{21, 22, 23, 80}));
    CHECK(parsePortList("0").isEmpty() && parsePortList("70000").isEmpty() && parsePortList("10-5").isEmpty());
    CHECK(parsePortList("abc").isEmpty() && parsePortList("1-2-3").isEmpty() && parsePortList("").isEmpty());

    QTcpServer open1, open2;
    open1.listen(QHostAddress::LocalHost, 0);
    open2.listen(QHostAddress::LocalHost, 0);
    QList<int> ports{open1.serverPort(), open2.serverPort()};
    for (int i = 0; i < 4; ++i)
        ports << freeTcpPort();

    PortScanner scanner;
    QList<int> found;
    int lastDone = 0, lastTotal = 0;
    bool finished = false;
    scanner.found = [&](const QString &host, int port)
    {
        CHECK(host == "127.0.0.1");
        found << port;
    };
    scanner.progress = [&](int done, int total) { lastDone = done, lastTotal = total; };
    scanner.finished = [&] { finished = true; };
    CHECK(!scanner.start("10.0.0.0/16", ports));
    CHECK(!scanner.start("127.0.0.1", {}));
    CHECK(scanner.start("127.0.0.1", ports, 3, 800));
    CHECK(scanner.isRunning());
    CHECK(spin([&] { return finished; }, 15000));
    std::sort(found.begin(), found.end());
    QList<int> expected{open1.serverPort(), open2.serverPort()};
    std::sort(expected.begin(), expected.end());
    CHECK(found == expected);
    CHECK(lastDone == 6 && lastTotal == 6 && !scanner.isRunning());

    // cancel: no more callbacks after cancellation
    QList<int> many;
    for (int i = 0; i < 40; ++i)
        many << freeTcpPort();
    finished = false;
    int calls = 0;
    // Cancel from the first callback: refused loopback connects are instant on Linux, so cancelling after
    // spin() would race with the whole scan finishing.
    scanner.progress = [&](int, int)
    {
        if (++calls == 1)
            scanner.cancel();
    };
    CHECK(scanner.start("127.0.0.1", many, 2, 300));
    CHECK(spin([&] { return calls > 0; }, 5000));
    spin([] { return false; }, 1000);
    CHECK(!scanner.isRunning() && calls == 1 && !finished);

    // /30 network: two usable hosts
    found.clear();
    finished = false;
    scanner.progress = nullptr;
    CHECK(scanner.start("127.0.0.0/30", {int(open1.serverPort())}, 8, 800));
    CHECK(spin([&] { return finished; }, 10000));
    CHECK(found == QList<int>{open1.serverPort()});
}

// ---------- Dialogs ----------

static void testDialogs(const QString &root)
{
    ServersDialog dialog;
    dialog.findChild<QLineEdit *>("httpRoot")->setText(root);
    dialog.findChild<QSpinBox *>("httpPort")->setValue(freeTcpPort());
    CHECK(!dialog.findChild<QPushButton *>("ftpStart")->isEnabled()); // FTP not attached
    dialog.findChild<QPushButton *>("httpStart")->click();
    CHECK(dialog.http().isRunning());
    CHECK(dialog.findChild<QLabel *>("httpStatus")->text().contains("http://127.0.0.1:"));
    CHECK(!dialog.findChild<QPushButton *>("httpStart")->isEnabled() && dialog.findChild<QPushButton *>("httpStop")->isEnabled());
    CHECK(http(dialog.http().port(), "GET /hello.txt HTTP/1.1\r\n\r\n").endsWith("hello world"));
    dialog.findChild<QPushButton *>("httpStop")->click();
    CHECK(!dialog.http().isRunning());

    dialog.findChild<QLineEdit *>("tftpRoot")->setText(root);
    dialog.findChild<QSpinBox *>("tftpPort")->setValue(freeTcpPort());
    dialog.findChild<QCheckBox *>("tftpWrite")->setChecked(true);
    dialog.findChild<QPushButton *>("tftpStart")->click();
    CHECK(dialog.tftp().isRunning());
    CHECK(tftpPut(dialog.tftp().port(), "dialog.bin", "via dialog"));
    dialog.findChild<QPushButton *>("tftpStop")->click();
    CHECK(!dialog.tftp().isRunning());

    dialog.findChild<QLineEdit *>("httpRoot")->setText(root + "/missing");
    dialog.findChild<QPushButton *>("httpStart")->click();
    CHECK(!dialog.http().isRunning() && dialog.findChild<QLabel *>("httpStatus")->text().startsWith("Error"));

    bool ftpUp = false, gotWrite = false;
    QString ftpRoot;
    dialog.setFtpControl(
        [&](const QString &folder, quint16, bool allowWrite, bool)
        {
            ftpRoot = folder, gotWrite = allowWrite;
            return ftpUp = true;
        },
        [&] { ftpUp = false; }, [&] { return ftpUp; });
    dialog.findChild<QLineEdit *>("ftpRoot")->setText(root);
    dialog.findChild<QCheckBox *>("ftpWrite")->setChecked(true);
    dialog.findChild<QPushButton *>("ftpStart")->click();
    CHECK(ftpUp && ftpRoot == root && gotWrite && dialog.findChild<QPushButton *>("ftpStop")->isEnabled());
    dialog.findChild<QPushButton *>("ftpStop")->click();
    CHECK(!ftpUp && dialog.findChild<QPushButton *>("ftpStart")->isEnabled());

    QTcpServer listener;
    listener.listen(QHostAddress::LocalHost, 0);
    PortScannerDialog scan;
    scan.findChild<QLineEdit *>("scanPorts")->setText(QString("%1,%2").arg(listener.serverPort()).arg(freeTcpPort()));
    auto *start = scan.findChild<QPushButton *>("scanStart");
    auto *results = scan.findChild<QListWidget *>("scanResults");
    start->click();
    CHECK(!start->isEnabled() && scan.findChild<QPushButton *>("scanStop")->isEnabled());
    CHECK(spin([&] { return start->isEnabled(); }, 10000));
    CHECK(results->count() == 1 && results->item(0)->text() == QString("127.0.0.1:%1").arg(listener.serverPort()));
    scan.findChild<QLineEdit *>("scanPorts")->setText("1-100000");
    start->click();
    CHECK(start->isEnabled() && results->count() == 1 && results->item(0)->text().contains("Invalid"));
    scan.findChild<QLineEdit *>("scanPorts")->setText("1-2000");
    start->click();
    scan.findChild<QPushButton *>("scanStop")->click();
    CHECK(start->isEnabled());
}

static void testWakeOnLan()
{
    const auto good = wakeOnLanPacket("00-1A-2B-3C-4D-5E");
    CHECK(wakeOnLanPacket("00:1a:2B-3c.4D5E") == good) // separators and case as typed
    CHECK(good.size() == 102 && good.startsWith(QByteArray(6, char(0xFF))) &&
          good.mid(6, 6) == QByteArray::fromHex("001A2B3C4D5E") && good.endsWith(QByteArray::fromHex("001A2B3C4D5E")))
    CHECK(wakeOnLanPacket("001A2B3C4D5E") == good)
    CHECK(wakeOnLanPacket("001a.2b3c.4d5e") == good)
    CHECK(wakeOnLanPacket("00:1A:2B:3C:4D").isEmpty())
    CHECK(wakeOnLanPacket("zz:1A:2B:3C:4D:5E").isEmpty())
    QUdpSocket receiver;
    CHECK(receiver.bind(QHostAddress::LocalHost, 0))
    QString error;
    CHECK(sendWakeOnLan("00:1A:2B:3C:4D:5E", QHostAddress::LocalHost, receiver.localPort(), &error))
    CHECK(spin([&] { return receiver.hasPendingDatagrams(); }, 3000))
    CHECK(receiver.receiveDatagram().data() == good)
    CHECK(!sendWakeOnLan("nope", QHostAddress::LocalHost, receiver.localPort(), &error) && error == "MAC")
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir temp;
    const QString root = temp.path() + "/root";
    QDir().mkpath(root + "/sub");
    write(temp.path() + "/secret.txt", "secret");
    write(root + "/hello.txt", "hello world");
    write(root + "/a.txt", "a");
    write(root + "/R&D #1.txt", "x"); // name to escape in HTML and in the URL
    write(root + "/sub/big.bin", pattern(1 << 20));
    testJail(root);
    testHttp(root);
    testTftp(root);
    testScanner();
    testWakeOnLan();
    testDialogs(root);
    qInfo() << (failures ? "FAILED" : "OK") << failures;
    return failures ? 1 : 0;
}
