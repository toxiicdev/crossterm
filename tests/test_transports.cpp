// Telnet, rlogin and rsh against fake servers on 127.0.0.1; serial port enumeration.
#include "transports.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDeadlineTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <functional>
#include <memory>

#define CHECK(expression)                                                                                                        \
    if (!(expression))                                                                                                           \
    {                                                                                                                            \
        qCritical() << "line" << __LINE__ << #expression;                                                                        \
        return 1;                                                                                                                \
    }

static bool waitFor(const std::function<bool()> &condition, int ms = 5000)
{
    QDeadlineTimer deadline(ms);
    while (!condition())
    {
        if (deadline.hasExpired())
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return true;
}

static QByteArray bytes(std::initializer_list<int> values)
{
    QByteArray result;
    for (const int value : values)
        result += char(uchar(value));
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    {
        QTcpServer server;
        CHECK(server.listen(QHostAddress::LocalHost, 0))
        TelnetDevice telnet("127.0.0.1", server.serverPort());
        telnet.setWindowSize(80, 24);
        QByteArray data; // application data received by the client
        bool finished = false;
        QObject::connect(&telnet, &QIODevice::readyRead, [&] { data += telnet.readAll(); });
        QObject::connect(&telnet, &QIODevice::readChannelFinished, [&] { finished = true; });
        CHECK(server.waitForNewConnection(5000))
        QTcpSocket *peer = server.nextPendingConnection();
        QByteArray received; // bytes received by the server
        QObject::connect(peer, &QTcpSocket::readyRead, [&] { received += peer->readAll(); });
        // IAC DO NAWS, IAC DO TTYPE, IAC WILL ECHO, unknown option, data with IAC IAC and CR NUL, TTYPE SEND.
        peer->write(bytes({255, 253, 31, 255, 253, 24, 255, 251, 1, 255, 253, 99}) + "hello\r" + bytes({0, 255, 255}) + "x" +
                    bytes({255, 250, 24, 1, 255, 240}));
        const QByteArray ttype = bytes({255, 250, 24, 0}) + "xterm-256color" + bytes({255, 240});
        CHECK(waitFor([&] { return received.contains(ttype) && data.size() >= 8; }))
        CHECK(received.contains(bytes({255, 251, 31})))                         // WILL NAWS
        CHECK(received.contains(bytes({255, 251, 24})))                         // WILL TTYPE
        CHECK(received.contains(bytes({255, 253, 1})))                          // DO ECHO
        CHECK(received.contains(bytes({255, 252, 99})))                         // WONT 99
        CHECK(received.contains(bytes({255, 250, 31, 0, 80, 0, 24, 255, 240}))) // NAWS 80x24
        CHECK(data == "hello\r" + bytes({255}) + "x")
        // Size with a 0xFF byte: it must be doubled inside SB too.
        received.clear();
        telnet.setWindowSize(255, 30);
        CHECK(waitFor([&] { return received.contains(bytes({255, 250, 31, 0, 255, 255, 0, 30, 255, 240})); }))
        // IAC split across two packets.
        received.clear();
        peer->write(bytes({255}));
        peer->flush();
        QCoreApplication::processEvents();
        QThread::msleep(50);
        peer->write(bytes({251, 3}) + "ok");
        CHECK(waitFor([&] { return received.contains(bytes({255, 253, 3})) && data.endsWith("ok"); })) // DO SGA
        CHECK(!data.contains(char(251)))
        // Outgoing 0xFF doubled, lone CR followed by NUL.
        received.clear();
        telnet.write("a" + bytes({255}) + "b\r");
        CHECK(waitFor([&] { return received == "a" + bytes({255, 255}) + "b\r" + bytes({0}); }))
        peer->close();
        CHECK(waitFor([&] { return finished; }))
    }
    {
        // Rlogin: handshake string, server "\0", window size message.
        QTcpServer server;
        CHECK(server.listen(QHostAddress::LocalHost, 0))
        RloginDevice rlogin("127.0.0.1", "bob", server.serverPort(), "alice");
        rlogin.setWindowSize(100, 40);
        QByteArray data;
        QObject::connect(&rlogin, &QIODevice::readyRead, [&] { data += rlogin.readAll(); });
        CHECK(server.waitForNewConnection(5000))
        QTcpSocket *peer = server.nextPendingConnection();
        QByteArray received;
        QObject::connect(peer, &QTcpSocket::readyRead, [&] { received += peer->readAll(); });
        const QByteArray handshake = QByteArray("\0alice\0bob\0xterm-256color/38400\0", 32);
        CHECK(waitFor([&] { return received.size() >= handshake.size(); }))
        CHECK(received == handshake)
        received.clear();
        peer->write(QByteArray("\0welcome", 8));
        const QByteArray window = bytes({255, 255, 's', 's', 0, 40, 0, 100, 0, 0, 0, 0});
        CHECK(waitFor([&] { return received == window && data == "welcome"; }))
        rlogin.write("ls\r");
        CHECK(waitFor([&] { return received.endsWith("ls\r"); }))
    }
    {
        // Rsh: handshake, server "\0", forwarded output, remote stdin, close.
        QTcpServer server;
        CHECK(server.listen(QHostAddress::LocalHost, 0))
        RshDevice rsh("127.0.0.1", "bob", "uname -a", server.serverPort(), "alice");
        QByteArray data;
        bool finished = false;
        QObject::connect(&rsh, &QIODevice::readyRead, [&] { data += rsh.readAll(); });
        QObject::connect(&rsh, &QIODevice::readChannelFinished, [&] { finished = true; });
        CHECK(server.waitForNewConnection(5000))
        QTcpSocket *peer = server.nextPendingConnection();
        QByteArray received;
        QObject::connect(peer, &QTcpSocket::readyRead, [&] { received += peer->readAll(); });
        const QByteArray handshake = QByteArray("0\0alice\0bob\0uname -a\0", 21);
        CHECK(waitFor([&] { return received.size() >= handshake.size(); }))
        CHECK(received == handshake)
        qInfo() << "rsh local port:" << peer->peerPort();
#ifdef Q_OS_WIN
        CHECK(peer->peerPort() >= 512 && peer->peerPort() <= 1023) // on Windows reserved ports need no privileges
#endif
        received.clear();
        peer->write(QByteArray("\0Linux box 6.1\n", 15));
        CHECK(waitFor([&] { return data == "Linux box 6.1\n"; }))
        rsh.write("input\n");
        CHECK(waitFor([&] { return received == "input\n"; }))
        peer->close();
        CHECK(waitFor([&] { return finished; }))
    }
    {
        // Rsh refused: "\1message\n" becomes errorString and stays visible in the output.
        QTcpServer server;
        CHECK(server.listen(QHostAddress::LocalHost, 0))
        RshDevice rsh("127.0.0.1", "bob", "id", server.serverPort(), "alice");
        QByteArray data;
        bool finished = false;
        QObject::connect(&rsh, &QIODevice::readyRead, [&] { data += rsh.readAll(); });
        QObject::connect(&rsh, &QIODevice::readChannelFinished, [&] { finished = true; });
        CHECK(server.waitForNewConnection(5000))
        QTcpSocket *peer = server.nextPendingConnection();
        CHECK(waitFor([&] { return peer->bytesAvailable() >= 14; }))
        peer->write("\1Permission ");
        peer->flush();
        CHECK(waitFor([&] { return data == "Permission "; }))
        peer->write("denied.\n");
        peer->flush();
        peer->close();
        CHECK(waitFor([&] { return finished && data == "Permission denied.\n"; }))
        CHECK(rsh.errorString() == "Permission denied.")
    }
    {
        // Without a command rsh becomes rlogin.
        QTcpServer server;
        CHECK(server.listen(QHostAddress::LocalHost, 0))
        std::unique_ptr<QIODevice> device(RshDevice::create("127.0.0.1", "bob", " ", server.serverPort(), "alice"));
        CHECK(dynamic_cast<RloginDevice *>(device.get()) != nullptr)
        CHECK(server.waitForNewConnection(5000))
        QTcpSocket *peer = server.nextPendingConnection();
        CHECK(waitFor([&] { return peer->bytesAvailable() >= 32; }))
        CHECK(peer->readAll() == QByteArray("\0alice\0bob\0xterm-256color/38400\0", 32))
    }
    {
        // Serial ports: listing must not fail; a nonexistent port does not open.
        const auto ports = serialPorts();
        qInfo() << "serial ports:" << ports;
        SerialDevice::Settings settings;
        settings.portName = "COM_CROSSTERM_MISSING";
        SerialDevice serial(settings);
        CHECK(!serial.open(QIODevice::ReadWrite))
        CHECK(!serial.errorString().isEmpty())
    }
    return 0;
}
