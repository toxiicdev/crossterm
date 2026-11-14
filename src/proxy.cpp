#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32")
using Socket = SOCKET;
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
static const Socket INVALID_SOCKET = -1;
static int closesocket(Socket s)
{
    return close(s);
}
#define SD_SEND SHUT_WR
#endif
#include "proxy.h"
#include "profiles.h"
#include <QHostAddress>
#include <QRegularExpression>
#include <thread>

// The relay uses native sockets and handles in blocking mode: no event loop or QCoreApplication, so it works
// even when ssh.exe launches the GUI executable with stdin/stdout connected to pipes.

static bool writeStd(int which, const char *data, qint64 size)
{
    while (size > 0)
    {
#ifdef _WIN32
        DWORD written = 0;
        const auto handle = GetStdHandle(which == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
        if (!WriteFile(handle, data, DWORD(std::min<qint64>(size, 1 << 20)), &written, nullptr) || written == 0)
            return false;
#else
        const auto written = write(which, data, size_t(size));
        if (written <= 0)
            return false;
#endif
        data += written;
        size -= written;
    }
    return true;
}

static qint64 readStdin(char *buffer, int size)
{
#ifdef _WIN32
    DWORD count = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, DWORD(size), &count, nullptr))
        return 0;
    return count;
#else
    const auto count = read(0, buffer, size_t(size));
    return count < 0 ? 0 : count;
#endif
}

static int fail(const QString &message)
{
    const auto text = ("CrossTerm proxy: " + message + "\n").toUtf8();
    writeStd(2, text.constData(), text.size());
    return 2;
}

static bool sendAll(Socket s, const QByteArray &data)
{
    for (qint64 done = 0; done < data.size();)
    {
        const int sent = send(s, data.constData() + done, int(data.size() - done), 0);
        if (sent <= 0)
            return false;
        done += sent;
    }
    return true;
}

static bool receive(Socket s, int count, QByteArray &out)
{
    out.resize(count);
    for (int done = 0; done < count;)
    {
        const int got = recv(s, out.data() + done, count - done, 0);
        if (got <= 0)
            return false;
        done += got;
    }
    return true;
}

static Socket connectTcp(const QString &host, int port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *list = nullptr;
    if (getaddrinfo(host.toUtf8().constData(), QByteArray::number(port).constData(), &hints, &list) != 0)
        return INVALID_SOCKET;
    Socket s = INVALID_SOCKET;
    for (auto *item = list; item && s == INVALID_SOCKET; item = item->ai_next)
    {
        s = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (s != INVALID_SOCKET && ::connect(s, item->ai_addr, int(item->ai_addrlen)) != 0)
        {
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }
    freeaddrinfo(list);
    return s;
}

static QByteArray portBytes(int port)
{
    return QByteArray(1, char(port >> 8)) + char(port & 0xff);
}

// Proxy negotiation; returns an error message, or an empty string once the tunnel is ready.
static QString handshake(Socket s, const QString &type, const QString &host, int port)
{
    const QHostAddress address(host);
    QByteArray reply;
    if (type == "http")
    {
        const auto target = (host.contains(':') ? "[" + host + "]" : host) + ":" + QString::number(port);
        if (!sendAll(s, ("CONNECT " + target + " HTTP/1.1\r\nHost: " + target + "\r\n\r\n").toUtf8()))
            return "invio CONNECT fallito";
        // Byte by byte: anything after the header already belongs to the tunnel (e.g. the SSH banner).
        QByteArray header, byte;
        while (!header.endsWith("\r\n\r\n"))
        {
            if (header.size() > 16384 || !receive(s, 1, byte))
                return "risposta HTTP non valida";
            header += byte;
        }
        const auto status = QString::fromLatin1(header.left(header.indexOf('\r')));
        if (!QRegularExpression("^HTTP/1\\.[01] 2\\d\\d").match(status).hasMatch())
            return "il proxy ha risposto " + status;
        return {};
    }
    if (type == "socks5")
    {
        if (!sendAll(s, QByteArray("\x05\x01\x00", 3)) || !receive(s, 2, reply) || reply != QByteArray("\x05\x00", 2))
            return "il proxy SOCKS5 richiede un'autenticazione non supportata";
        QByteArray request("\x05\x01\x00", 3);
        if (address.protocol() == QAbstractSocket::IPv4Protocol)
        {
            const quint32 ip = address.toIPv4Address();
            request += char(1) + QByteArray(1, char(ip >> 24)) + char(ip >> 16) + char(ip >> 8) + char(ip);
        }
        else if (address.protocol() == QAbstractSocket::IPv6Protocol)
            request += char(4) + QByteArray(reinterpret_cast<const char *>(address.toIPv6Address().c), 16);
        else
            request += char(3) + QByteArray(1, char(host.toUtf8().size())) + host.toUtf8();
        if (!sendAll(s, request + portBytes(port)) || !receive(s, 4, reply))
            return "risposta SOCKS5 non valida";
        if (reply[1] != 0)
            return "il proxy SOCKS5 ha rifiutato la connessione (codice " + QString::number(uchar(reply[1])) + ")";
        QByteArray rest;
        const int atyp = reply[3];
        int skip = atyp == 1 ? 4 : atyp == 4 ? 16 : -1;
        if (atyp == 3 && receive(s, 1, rest))
            skip = uchar(rest[0]);
        if (skip < 0 || !receive(s, skip + 2, rest))
            return "risposta SOCKS5 non valida";
        return {};
    }
    // SOCKS4 with an IPv4 address, otherwise SOCKS4a (name resolved by the proxy).
    if (address.protocol() == QAbstractSocket::IPv6Protocol)
        return "SOCKS4 non supporta IPv6";
    const bool ipv4 = address.protocol() == QAbstractSocket::IPv4Protocol;
    const quint32 ip = ipv4 ? address.toIPv4Address() : 1;
    QByteArray request =
        QByteArray("\x04\x01", 2) + portBytes(port) + char(ip >> 24) + char(ip >> 16) + char(ip >> 8) + char(ip) + char(0);
    if (!ipv4)
        request += host.toUtf8() + char(0);
    if (!sendAll(s, request) || !receive(s, 8, reply))
        return "risposta SOCKS4 non valida";
    if (reply[1] != 0x5a)
        return "il proxy SOCKS4 ha rifiutato la connessione (codice " + QString::number(uchar(reply[1])) + ")";
    return {};
}

int runProxyConnect(const QStringList &arguments)
{
    const int at = arguments.indexOf("--proxy-connect");
    bool proxyOk = false, targetOk = false;
    const int proxyPort = arguments.value(at + 3).toInt(&proxyOk);
    const int targetPort = arguments.value(at + 5).toInt(&targetOk);
    const auto type = arguments.value(at + 1), proxyHost = arguments.value(at + 2), target = arguments.value(at + 4);
    if (at < 0 || !QStringList{"http", "socks5", "socks4"}.contains(type) || !validHost(proxyHost) || !validHost(target) ||
        !proxyOk || !targetOk || proxyPort < 1 || proxyPort > 65535 || targetPort < 1 || targetPort > 65535)
    {
        fail("uso: --proxy-connect <http|socks5|socks4> <proxyHost> <proxyPort> <targetHost> <targetPort>");
        return 1;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const Socket s = connectTcp(proxyHost, proxyPort);
    if (s == INVALID_SOCKET)
        return fail("connessione al proxy " + proxyHost + ":" + QString::number(proxyPort) + " fallita");
    const auto error = handshake(s, type, target, targetPort);
    if (!error.isEmpty())
    {
        closesocket(s);
        return fail(error);
    }
    // stdin -> socket in a thread; on EOF only the sending direction is closed (half-close).
    std::thread(
        [s]
        {
            char buffer[65536];
            for (qint64 n; (n = readStdin(buffer, sizeof buffer)) > 0;)
            {
                if (!sendAll(s, QByteArray::fromRawData(buffer, n)))
                    return;
            }
            shutdown(s, SD_SEND);
        })
        .detach();
    char buffer[65536];
    for (int n; (n = recv(s, buffer, sizeof buffer, 0)) > 0;)
    {
        if (!writeStd(1, buffer, n))
            break;
    }
    // The detached stdin thread may still be blocked on read: main() returns right after, ending the process.
    closesocket(s);
    return 0;
}

QStringList proxyArguments(const QString &type, const QString &host, int port, const QString &helperExecutable)
{
    if (!QStringList{"http", "socks5", "socks4"}.contains(type) || !validHost(host) || port < 1 || port > 65535 ||
        helperExecutable.isEmpty() || helperExecutable.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))
        return {};
#ifdef _WIN32
    // Win32-OpenSSH runs the command as is: wrapping the path in double quotes is enough.
    if (helperExecutable.contains('"'))
        return {};
    auto quoted = '"' + QString(helperExecutable).replace('/', '\\') + '"';
#else
    // OpenSSH runs the command via "$SHELL -c": use single quotes.
    auto quoted = "'" + QString(helperExecutable).replace("'", "'\\''") + "'";
#endif
    quoted.replace("%", "%%"); // % tokens are expanded by ssh
    return {"-o", "ProxyCommand=" + quoted + " --proxy-connect " + type + " " + host + " " + QString::number(port) + " %h %p"};
}
