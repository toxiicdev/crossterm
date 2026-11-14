#include "servers.h"
#include <algorithm>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHostInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeDatabase>
#include <QNetworkDatagram>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QStorageInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QToolButton>
#include <QUdpSocket>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
QString ui(const char *text)
{
    return QCoreApplication::translate("CrossTerm", text);
}
} // namespace

QString jailedPath(const QString &root, const QString &relative)
{
    const QString base = QFileInfo(root).canonicalFilePath();
    if (base.isEmpty() || relative.contains(QChar(0)))
        return {};
    QStringList parts;
    for (const QString &part : QString(relative).replace('\\', '/').split('/', Qt::SkipEmptyParts))
    {
        if (part == ".")
            continue;
        // "..", hidden names (".ssh", ".env": not listed, so not served or written either), drives ("C:"), alternate
        // streams and names made only of dots/spaces (normalized by Windows) are rejected.
        if (part.startsWith('.') || part.contains(':') || QString(part).remove('.').trimmed().isEmpty())
            return {};
        parts << part;
    }
    const QString path = parts.isEmpty() ? base : base + '/' + parts.join('/');
    const QFileInfo info(path);
    if (info.isSymLink() && !info.exists())
        return {};
    // The real path (symlinks resolved) must stay under the root; for a new file the parent folder is checked.
    const QString real = info.exists() ? info.canonicalFilePath() : QFileInfo(info.absolutePath()).canonicalFilePath();
    if (real.isEmpty() || (real != base && !real.startsWith(base.endsWith('/') ? base : base + '/')))
        return {};
    // A link inside the root must not lead to a hidden name of the root either.
    if (('/' + real.mid(base.size())).contains("/."))
        return {};
    return path;
}

// ---------------------------------------------------------------- HTTP

namespace
{
QByteArray reason(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 206:
        return "Partial Content";
    case 301:
        return "Moved Permanently";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 416:
        return "Range Not Satisfiable";
    case 431:
        return "Request Header Fields Too Large";
    }
    return "Error";
}

QByteArray statusLine(int status, const QByteArray &type, qint64 length, const QByteArray &extra)
{
    return "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason(status) + "\r\nConnection: close\r\nContent-Type: " + type +
           "\r\nContent-Length: " + QByteArray::number(length) + "\r\n" + extra + "\r\n";
}

void respond(QTcpSocket *socket, int status, bool head, const QByteArray &extra = {}, QByteArray body = {},
             const QByteArray &type = "text/plain; charset=utf-8")
{
    if (body.isEmpty() && status >= 300)
        body = QByteArray::number(status) + ' ' + reason(status) + '\n';
    socket->write(statusLine(status, type, body.size(), extra));
    if (!head)
        socket->write(body);
    socket->disconnectFromHost();
}

QByteArray listing(const QString &path, const QString &urlPath)
{
    const QByteArray title = ("Index of " + urlPath).toHtmlEscaped().toUtf8();
    QByteArray html = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>" + title + "</title></head><body><h1>" +
                      title + "</h1><ul>\n";
    if (urlPath != "/")
        html += "<li><a href=\"../\">../</a></li>\n";
    const auto entries =
        QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name | QDir::DirsFirst | QDir::IgnoreCase);
    for (const QFileInfo &entry : entries)
    {
        const QByteArray slash = entry.isDir() ? "/" : "";
        html += "<li><a href=\"" + QUrl::toPercentEncoding(entry.fileName()) + slash + "\">" +
                entry.fileName().toHtmlEscaped().toUtf8() + slash + "</a></li>\n";
    }
    return html + "</ul></body></html>\n";
}

// Host header of a request: an IP literal, "localhost" or this machine's own name, on the listening port. Any other
// name is a page of another site whose DNS now points here (DNS rebinding) and would read the shared files.
bool ownHost(QByteArray value, quint16 listenPort)
{
    QByteArray port;
    const int colon = value.lastIndexOf(':');
    if (colon > value.lastIndexOf(']'))
    {
        port = value.mid(colon + 1);
        value.truncate(colon);
        bool ok = false;
        if (port.toUShort(&ok) != listenPort || !ok ||
            !std::all_of(port.cbegin(), port.cend(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
    }
    QString host = QString::fromLatin1(value).toLower();
    if (host.startsWith('[') && host.endsWith(']'))
        return QHostAddress(host.mid(1, host.size() - 2).section('%', 0, 0)).protocol() == QAbstractSocket::IPv6Protocol;
    if (QHostAddress(host).protocol() == QAbstractSocket::IPv4Protocol)
        return true;
    if (host.endsWith('.'))
        host.chop(1);
    if (host.isEmpty())
        return false;
    // Own name with or without its domain; ".local" is the name other machines of the network use (mDNS).
    const QString name = QHostInfo::localHostName().toLower(), domain = QHostInfo::localDomainName().toLower();
    QStringList own{"localhost", name, name.section('.', 0, 0), name.section('.', 0, 0) + ".local"};
    if (!domain.isEmpty())
        own << name + '.' + domain << name.section('.', 0, 0) + '.' + domain;
    own.removeAll(QString());
    own.removeAll(".local");
    return own.contains(host);
}
} // namespace

HttpFileServer::HttpFileServer() = default;
HttpFileServer::~HttpFileServer() = default;

bool HttpFileServer::start(const QString &rootDirectory, quint16 listenPort, bool bindAll)
{
    stop();
    root = QFileInfo(rootDirectory).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir())
    {
        error = ui("Folder not found: %1").arg(rootDirectory);
        return false;
    }
    server = std::make_unique<QTcpServer>();
    if (!server->listen(bindAll ? QHostAddress::Any : QHostAddress::LocalHost, listenPort))
    {
        error = server->errorString();
        server.reset();
        return false;
    }
    error.clear();
    requests = 0;
    QObject::connect(server.get(), &QTcpServer::newConnection, server.get(),
                     [this]
                     {
                         while (QTcpSocket *socket = server->nextPendingConnection())
                         {
                             // Limited number of connections (sockets are children of the server): extra clients are dropped.
                             if (server->findChildren<QTcpSocket *>(Qt::FindDirectChildrenOnly).size() > 64)
                             {
                                 socket->abort();
                                 socket->deleteLater();
                                 continue;
                             }
                             // Closed after 60 s without a response starting: idle clients, slow request headers. Reads
                             // do not extend it. Once data flows, a download may stall for 10 minutes (a browser download
                             // paused by the user) before it is dropped.
                             auto *idle = new QTimer(socket);
                             idle->setSingleShot(true);
                             QObject::connect(idle, &QTimer::timeout, socket,
                                              [socket]
                                              {
                                                  socket->abort();
                                                  socket->deleteLater();
                                              });
                             QObject::connect(socket, &QTcpSocket::bytesWritten, idle, [idle] { idle->start(600000); });
                             idle->start(60000);
                             QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { serve(socket); });
                             QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                         }
                     });
    return true;
}

void HttpFileServer::stop()
{
    server.reset(); // also closes the child sockets
}

bool HttpFileServer::isRunning() const
{
    return server && server->isListening();
}

quint16 HttpFileServer::port() const
{
    return server ? server->serverPort() : 0;
}

void HttpFileServer::serve(QTcpSocket *socket)
{
    // One request per connection (Connection: close): anything else is ignored.
    if (socket->property("served").toBool())
    {
        socket->readAll();
        return;
    }
    const int end = socket->peek(16384).indexOf("\r\n\r\n");
    if (end < 0)
    {
        if (socket->bytesAvailable() >= 16384)
        {
            socket->setProperty("served", true);
            respond(socket, 431, false);
        }
        return;
    }
    socket->setProperty("served", true);
    ++requests;
    const QList<QByteArray> lines = socket->read(end + 4).split('\n');
    const QList<QByteArray> request = lines.first().trimmed().split(' ');
    if (request.size() != 3 || !request[2].startsWith("HTTP/1."))
        return respond(socket, 400, false);
    const bool head = request[0] == "HEAD";
    if (!head && request[0] != "GET")
        return respond(socket, 405, head, "Allow: GET, HEAD\r\n");
    QByteArray target = request[1];
    target.truncate(qMin(uint(target.indexOf('?')), uint(target.indexOf('#'))));
    // No control characters: the target is echoed in the Location header.
    if (!target.startsWith('/') ||
        std::any_of(target.cbegin(), target.cend(), [](char c) { return uchar(c) < 0x20 || c == 0x7f; }))
        return respond(socket, 400, head);
    QByteArray range;
    QList<QByteArray> hosts;
    for (const QByteArray &line : lines)
    {
        if (line.toLower().startsWith("range:"))
            range = line.mid(6).trimmed();
        else if (line.toLower().startsWith("host:"))
            hosts << line.mid(5).trimmed();
    }
    // Browsers always send Host: a request without one (HTTP/1.0, simple clients) cannot come from a web page.
    if (hosts.size() > 1)
        return respond(socket, 400, head);
    if (!hosts.isEmpty() && !ownHost(hosts.first(), socket->localPort()))
        return respond(socket, 403, head);

    const QString urlPath = QString::fromUtf8(QByteArray::fromPercentEncoding(target));
    const QString path = jailedPath(root, urlPath);
    if (path.isEmpty())
        return respond(socket, 403, head);
    const QFileInfo info(path);
    if (!info.exists())
        return respond(socket, 404, head);
    if (info.isDir())
    {
        if (!target.endsWith('/'))
        {
            // A single leading slash: "//name" or "/\name" would be read by browsers as another host.
            int slashes = 0;
            while (slashes < target.size() && (target[slashes] == '/' || target[slashes] == '\\'))
                ++slashes;
            QByteArray location = '/' + target.mid(slashes);
            if (!location.endsWith('/'))
                location += '/';
            return respond(socket, 301, head, "Location: " + location + "\r\n");
        }
        return respond(socket, 200, head, {}, listing(path, urlPath), "text/html; charset=utf-8");
    }
    // Regular files only: opening a FIFO would block the event loop, devices have no size.
    if (!info.isFile())
        return respond(socket, 403, head);
    auto *file = new QFile(path, socket);
    if (!file->open(QIODevice::ReadOnly))
        return respond(socket, 403, head);

    // Range: a single "a-b", "a-" or "-n" range; multiple ranges or unknown syntax = whole file.
    const qint64 size = file->size();
    qint64 from = 0, to = size - 1;
    int status = 200;
    if (range.startsWith("bytes=") && !range.contains(','))
    {
        const QByteArray spec = range.mid(6).trimmed();
        const int dash = spec.indexOf('-');
        bool okFrom = false, okTo = false;
        const qint64 a = spec.left(dash).toLongLong(&okFrom), b = spec.mid(dash + 1).toLongLong(&okTo);
        if (dash == 0 && okTo && b > 0)
        {
            from = qMax<qint64>(0, size - b);
            status = 206;
        }
        else if (dash > 0 && okFrom && a >= 0 && (dash == spec.size() - 1 || (okTo && b >= a)))
        {
            from = a;
            to = dash == spec.size() - 1 ? size - 1 : qMin(b, size - 1);
            status = 206;
        }
        if (status == 206 && from > to)
            return respond(socket, 416, head, "Content-Range: bytes */" + QByteArray::number(size) + "\r\n");
    }
    static const QMimeDatabase mimes;
    QByteArray extra = "Accept-Ranges: bytes\r\n";
    if (status == 206)
        extra += "Content-Range: bytes " + QByteArray::number(from) + '-' + QByteArray::number(to) + '/' +
                 QByteArray::number(size) + "\r\n";
    socket->write(
        statusLine(status, mimes.mimeTypeForFile(info, QMimeDatabase::MatchExtension).name().toUtf8(), to - from + 1, extra));
    if (head || !file->seek(from))
        return socket->disconnectFromHost();

    // Chunked sending: the socket buffer is topped up on every bytesWritten.
    const qint64 last = to + 1;
    auto pump = [socket, file, last]
    {
        while (file->isOpen() && file->pos() < last && socket->bytesToWrite() < (1 << 18))
        {
            const QByteArray chunk = file->read(qMin<qint64>(1 << 16, last - file->pos()));
            if (chunk.isEmpty())
                break;
            socket->write(chunk);
        }
        if (file->isOpen() && (file->pos() >= last || file->atEnd()))
        {
            file->close();
            socket->disconnectFromHost();
        }
    };
    QObject::connect(socket, &QTcpSocket::bytesWritten, file, pump);
    pump();
}

// ---------------------------------------------------------------- TFTP

namespace
{
enum Opcode
{
    RRQ = 1,
    WRQ,
    DATA,
    ACK,
    ERR,
    OACK
};

QByteArray header(int opcode, quint16 number)
{
    QByteArray packet(4, 0);
    packet[1] = char(opcode);
    packet[2] = char(number >> 8);
    packet[3] = char(number);
    return packet;
}

QByteArray tftpError(int code, const QByteArray &message)
{
    return header(ERR, quint16(code)) + message + '\0';
}

quint16 word(const QByteArray &packet, int at)
{
    return packet.size() < at + 2 ? 0 : quint16((uchar(packet[at]) << 8) | uchar(packet[at + 1]));
}

// Uploads: at most 1 GiB each, and the volume keeps 64 MiB free (anyone who reaches the port can write).
const qint64 uploadCap = 1LL << 30, uploadMargin = 64LL << 20;
const char diskFull[] = "Disk full or allocation exceeded";

// Bytes an upload into `folder` may still write: 0 when the volume is (nearly) full; the cap alone if it is unknown.
qint64 uploadRoom(const QString &folder)
{
    const qint64 available = QStorageInfo(folder).bytesAvailable();
    return available < 0 ? uploadCap : qBound<qint64>(0, available - uploadMargin, uploadCap);
}

// One transfer: ephemeral socket (new TID), the last packet is retransmitted on every timeout.
struct Transfer : QObject
{
    QUdpSocket udp{this};
    QTimer timer{this};
    QHostAddress peer;
    quint16 peerPort = 0;
    bool writing = false, netascii = false, final = false;
    int blksize = 512, seconds = 2, retries = 0;
    quint16 block = 0;
    qint64 received = 0, room = 0, checkedAt = 0; // upload: bytes written, bytes allowed, last free space check
    QByteArray last, pending;                     // netascii: converted bytes not sent yet, or a CR waiting for the next DATA
    QFile file;
    QSaveFile save;

    Transfer(QObject *parent) : QObject(parent)
    {
        timer.setSingleShot(true);
        connect(&udp, &QUdpSocket::readyRead, this, [this] { incoming(); });
        connect(&timer, &QTimer::timeout, this, [this] { expired(); });
    }
    void send(const QByteArray &packet)
    {
        last = packet;
        retries = 0;
        udp.writeDatagram(packet, peer, peerPort);
        timer.start(seconds * 1000);
    }
    void sendBlock()
    {
        QByteArray chunk;
        if (netascii)
        {
            // Converted while sending: LF -> CR LF, CR -> CR NUL.
            while (pending.size() < blksize && !file.atEnd())
            {
                const QByteArray raw = file.read(blksize);
                if (raw.isEmpty())
                    break;
                for (const char c : raw)
                {
                    if (c == '\n')
                        pending += '\r';
                    pending += c;
                    if (c == '\r')
                        pending += '\0';
                }
            }
            chunk = pending.left(blksize);
            pending.remove(0, chunk.size());
        }
        else
            chunk = file.read(blksize);
        final = chunk.size() < blksize;
        send(header(DATA, block) + chunk);
    }
    void close(int code = -1, const QByteArray &message = {})
    {
        if (code >= 0)
            udp.writeDatagram(tftpError(code, message), peer, peerPort);
        timer.stop();
        udp.close();
        save.cancelWriting(); // a partial upload is discarded at once (no effect after commit())
        deleteLater();
    }
    void expired()
    {
        if (writing && final)
            return close(); // end of the wait for duplicate final DATA
        if (++retries > 5)
            return close(0, "Timeout");
        udp.writeDatagram(last, peer, peerPort);
        timer.start(seconds * 1000);
    }
    void incoming()
    {
        while (udp.hasPendingDatagrams())
        {
            const QNetworkDatagram datagram = udp.receiveDatagram();
            if (datagram.senderAddress() != peer || datagram.senderPort() != peerPort)
            {
                udp.writeDatagram(tftpError(5, "Unknown transfer ID"), datagram.senderAddress(), quint16(datagram.senderPort()));
                continue;
            }
            const QByteArray packet = datagram.data();
            const int opcode = word(packet, 0);
            const quint16 number = word(packet, 2);
            if (opcode == ERR)
                return close();
            if (!writing && opcode == ACK && number == block)
            {
                if (final)
                    return close();
                ++block;
                sendBlock();
            }
            else if (writing && opcode == DATA && number == quint16(block + 1) && !final)
            {
                QByteArray payload = packet.mid(4);
                if (payload.size() > blksize)
                    return close(4, "Block too large");
                ++block;
                const bool end = payload.size() < blksize;
                if (netascii)
                {
                    // CR LF -> LF, CR NUL -> CR, block by block; a trailing CR waits for the next block.
                    payload.prepend(pending);
                    pending.clear();
                    if (!end && payload.endsWith('\r'))
                    {
                        pending = "\r";
                        payload.chop(1);
                    }
                    payload.replace("\r\n", "\n").replace(QByteArray("\r\0", 2), "\r");
                }
                // Size cap; the free space is read again every 4 MiB (other transfers and programs fill the disk too).
                received += payload.size();
                if (received - checkedAt >= (4 << 20))
                {
                    checkedAt = received;
                    room = qMin(room, received + uploadRoom(QFileInfo(save.fileName()).absolutePath()));
                }
                if (received > room || save.write(payload) != payload.size())
                    return close(3, diskFull);
                if (end)
                {
                    if (!save.commit())
                        return close(3, diskFull);
                    final = true;
                }
                send(header(ACK, block));
            }
            else if (writing && opcode == DATA && number == block)
                udp.writeDatagram(last, peer, peerPort); // ACK lost: retransmit
        }
    }
};
} // namespace

TftpServer::TftpServer() = default;
TftpServer::~TftpServer() = default;

bool TftpServer::start(const QString &rootDirectory, quint16 listenPort, bool allowWrite, bool bindAll)
{
    stop();
    root = QFileInfo(rootDirectory).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir())
    {
        error = ui("Folder not found: %1").arg(rootDirectory);
        return false;
    }
    socket = std::make_unique<QUdpSocket>();
    if (!socket->bind(bindAll ? QHostAddress::Any : QHostAddress::LocalHost, listenPort))
    {
        error = socket->errorString();
        socket.reset();
        return false;
    }
    error.clear();
    writable = allowWrite;
    transfers = std::make_unique<QObject>();
    QObject::connect(socket.get(), &QUdpSocket::readyRead, socket.get(),
                     [this]
                     {
                         while (socket && socket->hasPendingDatagrams())
                         {
                             const QNetworkDatagram datagram = socket->receiveDatagram();
                             request(datagram.data(), datagram.senderAddress(), quint16(datagram.senderPort()));
                         }
                     });
    return true;
}

void TftpServer::stop()
{
    socket.reset();
    transfers.reset();
}

bool TftpServer::isRunning() const
{
    return socket != nullptr;
}

quint16 TftpServer::port() const
{
    return socket ? socket->localPort() : 0;
}

void TftpServer::request(const QByteArray &packet, const QHostAddress &peer, quint16 peerPort)
{
    const auto fail = [&](int code, const QByteArray &message)
    { socket->writeDatagram(tftpError(code, message), peer, peerPort); };
    const int opcode = word(packet, 0);
    const QList<QByteArray> fields = packet.mid(2).split('\0');
    if ((opcode != RRQ && opcode != WRQ) || fields.size() < 3 || !packet.endsWith('\0'))
        return fail(4, "Illegal TFTP operation");
    // Each transfer holds a socket and a file until it ends or times out: their number is limited.
    if (transfers->children().size() >= 64)
        return fail(0, "Too many transfers in progress");
    const QByteArray mode = fields[1].toLower();
    if (mode != "octet" && mode != "netascii")
        return fail(4, "Unsupported transfer mode");
    if (opcode == WRQ && !writable)
        return fail(2, "Access violation: uploads disabled");
    const QString path = jailedPath(root, QString::fromUtf8(fields[0]));
    if (path.isEmpty() || QFileInfo(path).isDir())
        return fail(2, "Access violation");

    auto transfer = std::make_unique<Transfer>(nullptr);
    transfer->peer = peer;
    transfer->peerPort = peerPort;
    transfer->writing = opcode == WRQ;
    transfer->netascii = mode == "netascii";
    if (transfer->writing)
    {
        transfer->room = uploadRoom(QFileInfo(path).absolutePath());
        if (transfer->room <= 0)
            return fail(3, diskFull);
        transfer->save.setFileName(path);
        if (!transfer->save.open(QIODevice::WriteOnly))
            return fail(2, "Access violation: cannot create file");
    }
    else
    {
        if (!QFileInfo(path).isFile())
            return fail(1, "File not found");
        transfer->file.setFileName(path);
        if (!transfer->file.open(QIODevice::ReadOnly))
            return fail(2, "Access violation: cannot read file");
    }

    // RFC 2347 options: blksize (8..65464), timeout (1..255 s) and tsize are accepted; tsize is not acknowledged
    // for netascii downloads, whose converted size is known only at the end.
    QByteArray acknowledged;
    for (int i = 2; i + 1 < fields.size(); i += 2)
    {
        const QByteArray name = fields[i].toLower();
        bool ok = false;
        const qint64 value = fields[i + 1].toLongLong(&ok);
        if (!ok)
            continue;
        if (name == "blksize" && value >= 8)
        {
            transfer->blksize = int(qMin<qint64>(value, 65464));
            acknowledged += name + '\0' + QByteArray::number(transfer->blksize) + '\0';
        }
        else if (name == "timeout" && value >= 1 && value <= 255)
        {
            transfer->seconds = int(value);
            acknowledged += name + '\0' + QByteArray::number(value) + '\0';
        }
        else if (name == "tsize" && value >= 0 && (transfer->writing || !transfer->netascii))
        {
            // Announced upload size: refused now rather than after the data (the QSaveFile goes with the transfer).
            if (transfer->writing && value > transfer->room)
                return fail(3, diskFull);
            acknowledged += name + '\0' + QByteArray::number(transfer->writing ? value : transfer->file.size()) + '\0';
        }
    }

    if (!transfer->udp.bind(socket->localAddress(), 0))
        return fail(0, "Cannot open transfer socket");
    Transfer *active = transfer.release();
    active->setParent(transfers.get());
    if (!acknowledged.isEmpty())
        active->send(QByteArray(1, 0) + char(OACK) + acknowledged); // the client replies with ACK 0 or DATA 1
    else if (active->writing)
        active->send(header(ACK, 0));
    else
    {
        active->block = 1;
        active->sendBlock();
    }
}

// ---------------------------------------------------------------- Port scanner

QList<int> parsePortList(const QString &text)
{
    QList<int> ports;
    for (const QString &item : text.split(',', Qt::SkipEmptyParts))
    {
        const QStringList bounds = item.trimmed().split('-');
        bool okLow = false, okHigh = false;
        const int low = bounds.first().trimmed().toInt(&okLow);
        const int high = bounds.size() == 2 ? bounds.last().trimmed().toInt(&okHigh) : low;
        if (!okLow || (bounds.size() == 2 && !okHigh) || bounds.size() > 2 || low < 1 || high > 65535 || low > high)
            return {};
        for (int port = low; port <= high; ++port)
            ports << port;
    }
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    return ports;
}

PortScanner::~PortScanner()
{
    cancel();
}

bool PortScanner::start(const QString &target, const QList<int> &portList, int concurrency, int timeoutMs)
{
    cancel();
    hosts.clear();
    const QString spec = target.trimmed();
    if (spec.contains('/'))
    {
        const auto subnet = QHostAddress::parseSubnet(spec);
        if (subnet.first.protocol() != QAbstractSocket::IPv4Protocol || subnet.second < 24)
            return false;
        const quint32 count = 1u << (32 - subnet.second);
        const quint32 base = subnet.first.toIPv4Address() & ~(count - 1);
        for (quint32 i = 0; i < count; ++i)
            if (count <= 2 || (i != 0 && i != count - 1)) // skip network and broadcast addresses
                hosts << QHostAddress(base + i).toString();
    }
    else if (!spec.isEmpty())
        hosts << spec;
    if (hosts.isEmpty() || portList.isEmpty())
        return false;
    ports = portList;
    next = done = 0;
    total = int(hosts.size() * ports.size());
    timeout = timeoutMs;
    holder = new QObject;
    for (int i = 0; i < qMax(1, concurrency) && holder && next < total; ++i) // holder: cancel() from a callback
        launch();
    return true;
}

void PortScanner::cancel()
{
    if (!holder)
        return;
    for (QObject *child : holder->findChildren<QObject *>())
        child->disconnect();
    holder->deleteLater();
    holder = nullptr;
}

void PortScanner::launch()
{
    const QString host = hosts[next / ports.size()];
    const int port = ports[next % ports.size()];
    ++next;
    auto *socket = new QTcpSocket(holder);
    auto *timer = new QTimer(socket);
    timer->setSingleShot(true);
    const auto complete = [this, socket, host, port](bool open)
    {
        if (socket->property("done").toBool())
            return;
        socket->setProperty("done", true);
        socket->disconnect();
        socket->abort();
        socket->deleteLater();
        ++done;
        if (open && found)
            found(host, port);
        if (holder && progress)
            progress(done, total);
        if (!holder) // cancel() called from a callback
            return;
        if (done == total)
        {
            holder->deleteLater();
            holder = nullptr;
            if (finished)
                finished();
        }
        else if (next < total)
        {
            // Queued, not recursive: a connection that fails at once (unreachable host) completes inside
            // connectToHost(), and thousands of them in a row overflowed the stack. Skipped after cancel()/start().
            QMetaObject::invokeMethod(
                &queue,
                [this, current = holder]
                {
                    if (holder == current && next < total)
                        launch();
                },
                Qt::QueuedConnection);
        }
    };
    QObject::connect(socket, &QTcpSocket::connected, socket, [complete] { complete(true); });
    QObject::connect(socket, &QTcpSocket::errorOccurred, socket, [complete] { complete(false); });
    QObject::connect(timer, &QTimer::timeout, socket, [complete] { complete(false); });
    timer->start(timeout);
    socket->connectToHost(host, quint16(port));
}

// ---------------------------------------------------------------- Dialogs

ServersDialog::ServersDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(ui("Servers"));
    auto *grid = new QGridLayout(this);
    const QStringList titles{ui("Server"), ui("Root folder"), {}, ui("Port"), ui("Allow uploads"), {}, {}, ui("Status")};
    for (int column = 0; column < titles.size(); ++column)
        grid->addWidget(new QLabel("<b>" + titles[column] + "</b>"), 0, column);
    httpRow = addRow(1, "HTTP", "http", 8080, false);
    tftpRow = addRow(2, "TFTP", "tftp", 69, true);
    ftpRow = addRow(3, "FTP", "ftp", 21, true);
    bindAll = new QCheckBox(ui("Listen on all network interfaces (otherwise only 127.0.0.1)"));
    bindAll->setObjectName("bindAll");
    grid->addWidget(bindAll, 4, 0, 1, 8);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    grid->addWidget(buttons, 5, 0, 1, 8);
    grid->setColumnStretch(1, 1);

    const auto run = [this](Row &row, const std::function<bool()> &action, const std::function<QString()> &error)
    {
        // No default folder: nothing is shared (least of all the home folder) before the user picks one.
        if (row.root->text().trimmed().isEmpty())
            return row.status->setText(ui("Choose a folder to share first."));
        const bool ok = action();
        refresh();
        if (!ok)
            row.status->setText(ui("Error: %1").arg(error()));
    };
    connect(httpRow.start, &QPushButton::clicked, this,
            [this, run]
            {
                run(
                    httpRow, [this]
                    { return httpServer.start(httpRow.root->text(), quint16(httpRow.port->value()), bindAll->isChecked()); },
                    [this] { return httpServer.errorString(); });
            });
    connect(tftpRow.start, &QPushButton::clicked, this,
            [this, run]
            {
                run(
                    tftpRow,
                    [this]
                    {
                        return tftpServer.start(tftpRow.root->text(), quint16(tftpRow.port->value()), tftpRow.write->isChecked(),
                                                bindAll->isChecked());
                    },
                    [this] { return tftpServer.errorString(); });
            });
    connect(ftpRow.start, &QPushButton::clicked, this,
            [this, run]
            {
                run(
                    ftpRow,
                    [this]
                    {
                        return ftpStart && ftpStart(ftpRow.root->text(), quint16(ftpRow.port->value()), ftpRow.write->isChecked(),
                                                    bindAll->isChecked());
                    },
                    [] { return ui("FTP server did not start"); });
            });
    connect(httpRow.stop, &QPushButton::clicked, this, [this] { httpServer.stop(), refresh(); });
    connect(tftpRow.stop, &QPushButton::clicked, this, [this] { tftpServer.stop(), refresh(); });
    connect(ftpRow.stop, &QPushButton::clicked, this,
            [this]
            {
                if (ftpStop)
                    ftpStop();
                refresh();
            });
    refresh();
}

ServersDialog::Row ServersDialog::addRow(int line, const QString &name, const QString &key, quint16 port, bool canWrite)
{
    auto *grid = static_cast<QGridLayout *>(layout());
    Row row{new QLineEdit,
            new QSpinBox,
            canWrite ? new QCheckBox : nullptr,
            new QPushButton(ui("Start")),
            new QPushButton(ui("Stop")),
            new QLabel};
    auto *browse = new QToolButton;
    browse->setText("...");
    row.port->setRange(1, 65535);
    row.port->setValue(port);
    row.root->setPlaceholderText(ui("Root folder"));
    row.root->setObjectName(key + "Root");
    row.port->setObjectName(key + "Port");
    row.start->setObjectName(key + "Start");
    row.stop->setObjectName(key + "Stop");
    row.status->setObjectName(key + "Status");
    row.status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(new QLabel(name), line, 0);
    grid->addWidget(row.root, line, 1);
    grid->addWidget(browse, line, 2);
    grid->addWidget(row.port, line, 3);
    if (row.write)
    {
        row.write->setObjectName(key + "Write");
        grid->addWidget(row.write, line, 4, Qt::AlignCenter);
    }
    grid->addWidget(row.start, line, 5);
    grid->addWidget(row.stop, line, 6);
    grid->addWidget(row.status, line, 7);
    QLineEdit *edit = row.root;
    connect(browse, &QToolButton::clicked, this,
            [this, edit]
            {
                const QString folder = QFileDialog::getExistingDirectory(
                    this, ui("Root folder"), edit->text().isEmpty() ? QDir::homePath() : edit->text());
                if (!folder.isEmpty())
                    edit->setText(QDir::toNativeSeparators(folder));
            });
    return row;
}

void ServersDialog::setFtpControl(std::function<bool(const QString &, quint16, bool, bool)> start, std::function<void()> stop,
                                  std::function<bool()> isRunning)
{
    ftpStart = std::move(start);
    ftpStop = std::move(stop);
    ftpRunning = std::move(isRunning);
    refresh();
}

void ServersDialog::refresh()
{
    const auto show = [this](Row &row, bool available, bool running, const QString &address)
    {
        for (QWidget *input : std::initializer_list<QWidget *>{row.root, row.port, row.write})
            if (input)
                input->setEnabled(available && !running);
        row.start->setEnabled(available && !running);
        row.stop->setEnabled(available && running);
        row.status->setText(!available ? ui("Not available") : running ? ui("Running: %1").arg(address) : ui("Stopped"));
    };
    const QString host = bindAll->isChecked() ? "0.0.0.0" : "127.0.0.1";
    show(httpRow, true, httpServer.isRunning(), QString("http://%1:%2/").arg(host).arg(httpServer.port()));
    show(tftpRow, true, tftpServer.isRunning(), QString("tftp://%1:%2").arg(host).arg(tftpServer.port()));
    const bool ftpUp = ftpRunning && ftpRunning();
    show(ftpRow, bool(ftpStart), ftpUp, QString("ftp://%1:%2").arg(host).arg(ftpRow.port->value()));
    bindAll->setEnabled(!httpServer.isRunning() && !tftpServer.isRunning() && !ftpUp);
}

PortScannerDialog::PortScannerDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(ui("Port scanner"));
    host = new QLineEdit("127.0.0.1");
    host->setPlaceholderText(ui("Host name, IP address or network (e.g. 192.168.1.0/24)"));
    ports = new QLineEdit("21,22,23,25,53,80,110,135,139,143,443,445,993,995,1433,3306,3389,5432,5900,8080");
    startButton = new QPushButton(ui("Start"));
    stopButton = new QPushButton(ui("Stop"));
    bar = new QProgressBar;
    results = new QListWidget;
    host->setObjectName("scanHost");
    ports->setObjectName("scanPorts");
    startButton->setObjectName("scanStart");
    stopButton->setObjectName("scanStop");
    bar->setObjectName("scanProgress");
    results->setObjectName("scanResults");
    stopButton->setEnabled(false);

    auto *form = new QFormLayout;
    form->addRow(ui("Host:"), host);
    form->addRow(ui("Ports:"), ports);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(startButton);
    buttons->addWidget(stopButton);
    buttons->addWidget(bar, 1);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addLayout(buttons);
    layout->addWidget(new QLabel(ui("Open ports:")));
    layout->addWidget(results);

    scanner.found = [this](const QString &address, int port) { results->addItem(QString("%1:%2").arg(address).arg(port)); };
    scanner.progress = [this](int done, int total)
    {
        bar->setMaximum(total);
        bar->setValue(done);
    };
    scanner.finished = [this] { finish(); };
    connect(startButton, &QPushButton::clicked, this,
            [this]
            {
                results->clear();
                bar->setValue(0);
                const QList<int> list = parsePortList(ports->text());
                if (list.isEmpty())
                    return results->addItem(ui("Invalid port list"));
                if (!scanner.start(host->text(), list))
                    return results->addItem(ui("Invalid host or network (a network needs a prefix of at least /24)"));
                startButton->setEnabled(false);
                stopButton->setEnabled(true);
            });
    connect(stopButton, &QPushButton::clicked, this,
            [this]
            {
                scanner.cancel();
                finish();
            });
}

void PortScannerDialog::finish()
{
    startButton->setEnabled(true);
    stopButton->setEnabled(false);
}
