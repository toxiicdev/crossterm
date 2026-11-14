#include "keepalive.h"
#include <QAbstractSocket>
#include <QVariant>
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <mstcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

namespace
{
constexpr int Idle = 10, Interval = 5, Count = 4; // first probe after 10 s of silence, then 4 probes 5 s apart

void configure(QAbstractSocket *socket)
{
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    const auto fd = socket->socketDescriptor();
    if (fd == -1)
        return;
#ifdef Q_OS_WIN
    tcp_keepalive values{1, ULONG(Idle * 1000), ULONG(Interval * 1000)}; // Windows always sends 10 probes
    DWORD bytes = 0;
    WSAIoctl(SOCKET(fd), SIO_KEEPALIVE_VALS, &values, sizeof(values), nullptr, 0, &bytes, nullptr, nullptr);
#else
    const int idle = Idle, interval = Interval, count = Count;
#ifdef TCP_KEEPIDLE
    setsockopt(int(fd), IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#elif defined(TCP_KEEPALIVE)
    setsockopt(int(fd), IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle)); // macOS name for the idle time
#endif
#ifdef TCP_KEEPINTVL
    setsockopt(int(fd), IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
#endif
#ifdef TCP_KEEPCNT
    setsockopt(int(fd), IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
#endif
}
} // namespace

void enableKeepAlive(QAbstractSocket *socket)
{
    if (socket->state() == QAbstractSocket::ConnectedState)
        configure(socket);
    else
        QObject::connect(socket, &QAbstractSocket::connected, socket, [socket] { configure(socket); });
}
