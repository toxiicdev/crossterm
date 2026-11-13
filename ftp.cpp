#include "ftp.h"
#include "icons.h"
#include <QtWidgets>
#include <QDesktopServices>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimeZone>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

static QString ui(const char *text)
{
    return QCoreApplication::translate("CrossTerm", text);
}

static QString message(int code, const QString &text)
{
    return QString::number(code) + ' ' + text.section('\n', 0, 0).trimmed();
}

// Unix permissions (octal) of a local file, for the server's LIST and MLSD.
static int unixMode(const QFileInfo &info)
{
    static const QFileDevice::Permission bits[9] = {QFileDevice::ReadOwner, QFileDevice::WriteOwner, QFileDevice::ExeOwner,
                                                    QFileDevice::ReadGroup, QFileDevice::WriteGroup, QFileDevice::ExeGroup,
                                                    QFileDevice::ReadOther, QFileDevice::WriteOther, QFileDevice::ExeOther};
    int mode = 0;
    for (int i = 0; i < 9; ++i)
        mode = mode << 1 | (info.permissions() & bits[i] ? 1 : 0);
    return mode;
}

static QString modeString(int mode, QChar type)
{
    QString text(type);
    for (int i = 8; i >= 0; --i)
        text += (mode >> i) & 1 ? QChar("rwx"[(8 - i) % 3]) : QChar('-');
    return text;
}

// MLSD lines (facts;name) or LIST in Unix or DOS/IIS format.
static QList<RemoteEntry> parseEntries(const QByteArray &bytes, bool machine)
{
    static const QRegularExpression dos("^(\\d{2}-\\d{2}-\\d{2,4})\\s+(\\d{1,2}:\\d{2}[AaPp][Mm])\\s+(<DIR>|\\d+)\\s+(.+)$");
    QList<RemoteEntry> entries;
    for (auto line : QString::fromUtf8(bytes).split('\n', Qt::SkipEmptyParts))
    {
        if (line.endsWith('\r'))
            line.chop(1);
        RemoteEntry e;
        if (machine)
        {
            const int space = line.indexOf(' ');
            if (space < 0)
                continue;
            e.name = line.mid(space + 1);
            int mode = -1;
            for (const auto &fact : line.left(space).split(';', Qt::SkipEmptyParts))
            {
                const auto key = fact.section('=', 0, 0).toLower(), value = fact.mid(fact.indexOf('=') + 1);
                if (key == "type")
                {
                    const auto type = value.toLower();
                    if (type == "cdir" || type == "pdir")
                        e.name.clear();
                    e.dir = type == "dir";
                    e.link = type.startsWith("os.unix=sl") || type.startsWith("os.unix=symlink");
                }
                else if (key == "size")
                    e.size = value.toLongLong();
                else if (key == "modify")
                {
                    auto when = QDateTime::fromString(value.left(14), "yyyyMMddHHmmss");
                    when.setTimeZone(QTimeZone(QTimeZone::UTC));
                    e.modified = when.toLocalTime().toString("yyyy-MM-dd HH:mm");
                }
                else if (key == "unix.mode")
                    mode = value.toInt(nullptr, 8);
                else if (key == "perm")
                    e.access = value;
            }
            if (mode >= 0)
                e.access = modeString(mode, e.link ? 'l' : e.dir ? 'd' : '-');
        }
        else if (!parseListing(line, e))
        {
            const auto m = dos.match(line);
            if (!m.hasMatch())
                continue;
            e.dir = m.captured(3) == "<DIR>";
            e.size = e.dir ? 0 : m.captured(3).toLongLong();
            e.modified = m.captured(1) + ' ' + m.captured(2);
            e.name = m.captured(4);
        }
        if (!e.name.isEmpty() && e.name != "." && e.name != "..")
            entries << e;
    }
    return entries;
}

// ---------------------------------------------------------------- client

// QTcpServer that accepts QSslSocket (plaintext until TLS is needed): FTP server and client active mode.
class SslListener : public QTcpServer
{
public:
    using QTcpServer::QTcpServer;

protected:
    void incomingConnection(qintptr handle) override
    {
        auto *socket = new QSslSocket(this);
        if (socket->setSocketDescriptor(handle))
            addPendingConnection(socket);
        else
            delete socket;
    }
};

// Names received from the server in downloaded folders: no separators, drives or ".." (they would escape the local folder).
static bool safeName(const QString &name)
{
    return !name.isEmpty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\\') && !name.contains(':') &&
           !name.contains(QChar(0));
}

static QString child(const QString &dir, const QString &name)
{
    return (dir.endsWith('/') ? dir : dir + '/') + name;
}

// Folder copy: destinations to create (parents before children) and files with their expected size.
struct FtpClient::Plan
{
    struct File
    {
        QString source, target;
        qint64 size;
    };
    QStringList dirs;
    QList<File> files;
    qint64 total = 0;
};

QString FtpClient::fingerprint(const QSslCertificate &certificate)
{
    return QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex(':').toUpper());
}

FtpClient::FtpClient() : ctx(new QObject), control(new QSslSocket(ctx)), watchdog(new QTimer(ctx))
{
    watchdog->setSingleShot(true);
    // Reusable TLS session for data channels, where the backend supports it.
    auto tls = control->sslConfiguration();
    tls.setSslOption(QSsl::SslOptionDisableSessionPersistence, false);
    control->setSslConfiguration(tls);
    QObject::connect(control, &QSslSocket::readyRead, ctx, [this] { readReplies(); });
    QObject::connect(control, &QSslSocket::disconnected, ctx,
                     [this]
                     {
                         loggedIn = false;
                         if (pending || afterHandshake)
                             fail(tlsError.isEmpty() ? ui("Connection closed by the server.") : tlsError);
                     });
    QObject::connect(control, &QSslSocket::errorOccurred, ctx,
                     [this](QAbstractSocket::SocketError e)
                     {
                         if (e == QAbstractSocket::RemoteHostClosedError)
                             return; // disconnected follows
                         loggedIn = false;
                         if (pending || afterHandshake)
                             fail(tlsError.isEmpty() ? control->errorString() : tlsError);
                     });
    QObject::connect(control, &QSslSocket::encrypted, ctx,
                     [this]
                     {
                         if (auto next = std::exchange(afterHandshake, nullptr))
                             next();
                     });
    // TLS errors are never silently ignored: either the certificate was already accepted for this host,
    // or trustCertificate accepts it explicitly; otherwise the connection fails and shows the fingerprint.
    QObject::connect(control, &QSslSocket::sslErrors, ctx,
                     [this](const QList<QSslError> &errors)
                     {
                         const auto certificate = control->peerCertificate();
                         const auto digest = certificate.digest(QCryptographicHash::Sha256);
                         watchdog->stop(); // the callback may open a modal dialog
                         const bool accepted = !certificate.isNull() && (trusted.value(hostName) == digest ||
                                                                         (trustCertificate && trustCertificate(certificate)));
                         if (running)
                             watchdog->start(timeout);
                         if (accepted)
                         {
                             trusted[hostName] = digest;
                             return control->ignoreSslErrors(errors);
                         }
                         QStringList reasons;
                         for (const auto &e : errors)
                             reasons << e.errorString();
                         tlsError = ui("The server certificate is not trusted (%1). SHA-256 fingerprint: %2")
                                        .arg(reasons.join("; "), fingerprint(certificate));
                     });
    QObject::connect(watchdog, &QTimer::timeout, ctx,
                     [this]
                     {
                         fail(ui("Timed out."));
                         loggedIn = false;
                         control->abort();
                     });
}

FtpClient::~FtpClient()
{
    delete ctx; // ~QObject disconnects signals before destroying the child sockets
}

void FtpClient::enqueue(Done done, std::function<void()> start)
{
    queue.append({std::move(done), std::move(start)});
    QTimer::singleShot(0, ctx, [this] { next(); }); // never synchronous callbacks inside the public call
}

void FtpClient::next()
{
    if (running || queue.isEmpty())
        return;
    auto job = queue.takeFirst();
    running = true;
    current = std::move(job.done);
    job.start();
}

void FtpClient::complete(bool ok, const QString &error)
{
    if (!running)
        return;
    running = false;
    pending = nullptr;
    afterHandshake = nullptr;
    watchdog->stop();
    if (data)
    {
        QObject::disconnect(data, nullptr, ctx, nullptr);
        data->abort();
        data->deleteLater();
        data = nullptr;
    }
    closeListener();
    if (auto file = std::exchange(sinkFile, nullptr))
    {
        // Failed download: the .part file is kept for resuming only if it is not empty.
        file->close();
        if (file->fileName().endsWith(".part") && QFileInfo(file->fileName()).size() == 0)
            QFile::remove(file->fileName());
    }
    auto done = std::move(current);
    current = nullptr;
    if (!queue.isEmpty())
        QTimer::singleShot(0, ctx, [this] { next(); });
    if (done)
        done(ok, error);
}

void FtpClient::fail(const QString &error)
{
    complete(false, error);
}

void FtpClient::readReplies()
{
    input += control->readAll();
    for (int nl; (nl = input.indexOf('\n')) >= 0;)
    {
        auto line = QString::fromUtf8(input.left(nl));
        input.remove(0, nl + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        if (log)
            log(line);
        const bool numbered = line.size() >= 3 && line[0].isDigit() && line[1].isDigit() && line[2].isDigit();
        const int code = numbered ? line.left(3).toInt() : 0;
        if (multiline)
        {
            multiText += '\n' + line;
            if (code == multiline && (line.size() == 3 || line[3] == ' '))
            {
                multiline = 0;
                dispatch(code, multiText);
            }
            continue;
        }
        if (!numbered)
            continue;
        if (line.size() > 3 && line[3] == '-')
        {
            multiline = code;
            multiText = line.mid(4);
            continue;
        }
        dispatch(code, line.mid(4));
    }
}

void FtpClient::dispatch(int code, const QString &text)
{
    if (code < 200)
    {
        watchdog->start(timeout);
        auto handler = pending; // copy: the handler may replace or clear pending
        if (handler)
            handler(code, text);
        return;
    }
    watchdog->stop();
    auto handler = std::move(pending);
    pending = nullptr;
    if (handler)
        handler(code, text);
    else if (code == 421)
        control->abort(); // close announced by the server outside a command
}

void FtpClient::send(const QString &command, std::function<void(int, const QString &)> handler)
{
    if (command.contains('\r') || command.contains('\n') || command.contains(QChar(0)))
        return fail(ui("Invalid file name."));
    if (control->state() != QAbstractSocket::ConnectedState)
        return fail(ui("Not connected."));
    pending = std::move(handler);
    if (log)
        log("> " + (command.startsWith("PASS ") ? QString("PASS ****") : command));
    control->write(command.toUtf8() + "\r\n");
    watchdog->start(timeout);
}

// Command without data: 2xx or 3xx (RNFR replies 350) = success, otherwise a job error.
void FtpClient::simple(const QString &command, std::function<void()> then)
{
    send(command,
         [this, then](int code, const QString &text)
         {
             if (code < 200)
                 return;
             code < 400 ? then() : fail(message(code, text));
         });
}

void FtpClient::connectToHost(const QString &host, quint16 port, const QString &user, const QString &password, Done done)
{
    enqueue(std::move(done),
            [=]
            {
                loggedIn = mlsd = false;
                epsv = true;
                control->abort();
                input.clear();
                multiline = 0;
                tlsError.clear();
                hostName = host;
                const bool implicit = security == "implicit", explicitTls = security == "explicit";
                if (!implicit && !explicitTls && security != "none" && !security.isEmpty())
                    return fail(ui("Unknown FTP security mode: %1").arg(security));
                if ((implicit || explicitTls) && !QSslSocket::supportsSsl())
                    return fail(ui("TLS is not available on this system."));
                const auto login = user.isEmpty() ? QString("anonymous") : user;
                const auto secret = user.isEmpty() && password.isEmpty() ? QString("anonymous@") : password;
                auto sendUser = [=]
                {
                    send("USER " + login,
                         [=](int code, const QString &text)
                         {
                             if (code == 230)
                                 return afterLogin();
                             if (code != 331)
                                 return code < 200 ? void() : fail(message(code, text));
                             send("PASS " + secret,
                                  [=](int code, const QString &text)
                                  {
                                      if (code < 200)
                                          return;
                                      code == 230 || code == 202 ? afterLogin() : fail(message(code, text));
                                  });
                         });
                };
                pending = [=](int code, const QString &text)
                {
                    if (code < 200)
                        return;
                    if (code != 220)
                        return fail(message(code, text));
                    if (!explicitTls)
                        return sendUser();
                    // RFC 4217: AUTH TLS before the credentials, which are then sent encrypted.
                    send("AUTH TLS",
                         [=](int code, const QString &text)
                         {
                             if (code < 200)
                                 return;
                             if (code != 234)
                                 return fail(message(code, text));
                             afterHandshake = sendUser;
                             watchdog->start(timeout);
                             control->startClientEncryption();
                         });
                };
                watchdog->start(timeout);
                if (implicit)
                    control->connectToHostEncrypted(host, port ? port : 990);
                else
                    control->connectToHost(host, port ? port : 21);
            });
}

void FtpClient::afterLogin()
{
    auto features = [this]
    {
        send("FEAT",
             [this](int code, const QString &text)
             {
                 bool utf8 = false;
                 mlsd = false;
                 if (code == 211)
                 {
                     for (const auto &line : text.split('\n'))
                     {
                         const auto feature = line.trimmed().toUpper();
                         mlsd = mlsd || feature.startsWith("MLST");
                         utf8 = utf8 || feature == "UTF8";
                     }
                 }
                 auto binary = [this]
                 {
                     send("TYPE I",
                          [this](int code, const QString &text)
                          {
                              if (code < 200)
                                  return;
                              if (code >= 300)
                                  return fail(message(code, text));
                              loggedIn = true;
                              complete(true);
                          });
                 };
                 if (!utf8)
                     return binary();
                 send("OPTS UTF8 ON",
                      [binary](int code, const QString &)
                      {
                          if (code >= 200)
                              binary();
                      });
             });
    };
    if (!control->isEncrypted())
        return features();
    // Data channels in TLS too (PROT P); PBSZ 0 is mandatory before PROT.
    simple("PBSZ 0", [this, features] { simple("PROT P", features); });
}

void FtpClient::openPassive(std::function<void(quint16)> then)
{
    auto pasv = [this, then]
    {
        send("PASV",
             [this, then](int code, const QString &text)
             {
                 static const QRegularExpression numbers("(\\d+),(\\d+),(\\d+),(\\d+),(\\d+),(\\d+)");
                 const auto m = numbers.match(text);
                 if (code < 200)
                     return;
                 if (code != 227 || !m.hasMatch())
                     return fail(message(code, text));
                 // Like most common clients, ignore the advertised address (NAT) and use the control connection's.
                 then(quint16(m.captured(5).toUInt() * 256 + m.captured(6).toUInt()));
             });
    };
    if (!epsv)
        return pasv();
    send("EPSV",
         [this, then, pasv](int code, const QString &text)
         {
             static const QRegularExpression port("\\((.)\\1\\1(\\d+)\\1\\)");
             const auto m = port.match(text);
             if (code < 200)
                 return;
             if (code == 229 && m.hasMatch())
                 return then(quint16(m.captured(2).toUInt()));
             if (code >= 500)
             {
                 epsv = false; // server without RFC 2428
                 return pasv();
             }
             fail(message(code, text));
         });
}

// Active mode: listen on the local address of the control connection; EPRT, or PORT if the server does not support it.
void FtpClient::openActive(std::function<void(QTcpServer *)> then)
{
    auto address = control->localAddress();
    bool v4 = false;
    const quint32 ip = address.toIPv4Address(&v4);
    if (v4)
        address = QHostAddress(ip);
    auto *server = new SslListener(ctx);
    if (!server->listen(address, 0))
    {
        const auto error = server->errorString();
        delete server;
        return fail(error);
    }
    listener = server;
    const quint16 port = server->serverPort();
    auto legacy = [=]
    {
        simple(QString("PORT %1,%2,%3,%4,%5,%6")
                   .arg(ip >> 24)
                   .arg((ip >> 16) & 255)
                   .arg((ip >> 8) & 255)
                   .arg(ip & 255)
                   .arg(port >> 8)
                   .arg(port & 255),
               [=] { then(server); });
    };
    if (!epsv && v4)
        return legacy();
    send(QString("EPRT |%1|%2|%3|").arg(v4 ? 1 : 2).arg(address.toString()).arg(port),
         [=](int code, const QString &text)
         {
             if (code < 200)
                 return;
             if (code < 300)
                 return then(server);
             if (code >= 500 && v4)
             {
                 epsv = false; // server without RFC 2428
                 return legacy();
             }
             fail(message(code, text));
         });
}

void FtpClient::closeListener()
{
    if (!listener)
        return;
    listener->close();
    listener->deleteLater();
    listener = nullptr;
}

// TLS data channel when the control channel is encrypted (PROT P); the client is always the TLS client side (RFC 4217).
void FtpClient::secureData(QSslSocket *socket, std::function<void()> ready)
{
    if (!control->isEncrypted())
        return ready();
    QObject::connect(socket, &QSslSocket::encrypted, ctx, ready);
    QObject::connect(socket, &QSslSocket::sslErrors, ctx,
                     [this, socket](const QList<QSslError> &errors)
                     {
                         // Same certificate as the control channel (already verified or accepted): no new prompt.
                         const auto certificate = socket->peerCertificate();
                         if (!certificate.isNull() && certificate == control->peerCertificate())
                             socket->ignoreSslErrors(errors);
                     });
    socket->setSslConfiguration(control->sslConfiguration()); // with the session to resume, if any
    socket->setPeerVerifyName(hostName);
    socket->startClientEncryption();
}

// The job ends only after the server's final reply and with the data channel closed.
void FtpClient::transfer(const QString &command, std::shared_ptr<QIODevice> source, std::function<bool(const QByteArray &)> sink,
                         Progress progress, qint64 total, std::function<void()> then)
{
    struct State
    {
        bool started = false, ready = false, replied = false, closed = false, closing = false, settled = false;
        qint64 done = 0;
        QString error;
        QSslSocket *sock = nullptr;
    };
    auto st = std::make_shared<State>();
    auto settle = [=]
    {
        if (st->settled || !st->replied || !st->closed)
            return;
        st->settled = true;
        if (auto *sock = st->sock)
        {
            QObject::disconnect(sock, nullptr, ctx, nullptr);
            if (data == sock)
                data = nullptr;
            sock->deleteLater();
        }
        closeListener();
        st->error.isEmpty() ? then() : fail(st->error);
    };
    auto drop = [=](const QString &error)
    {
        if (st->error.isEmpty())
            st->error = error;
        if (st->closed)
            return;
        st->closed = true;
        if (st->sock)
        {
            QObject::disconnect(st->sock, nullptr, ctx, nullptr);
            st->sock->abort();
        }
        settle();
    };
    auto report = [=](qint64 n)
    {
        st->done += n;
        watchdog->start(timeout);
        if (progress)
            progress(st->done, total);
    };
    auto pump = [=]
    {
        auto *sock = st->sock;
        if (!source || !sock || !st->started || !st->ready || st->closing || sock->state() != QAbstractSocket::ConnectedState)
            return;
        while (sock->bytesToWrite() < 256 * 1024 && !source->atEnd())
        {
            const auto chunk = source->read(64 * 1024);
            if (chunk.isEmpty())
                return drop(source->errorString());
            sock->write(chunk);
        }
        if (source->atEnd())
        {
            st->closing = true;
            sock->disconnectFromHost(); // send pending data first
        }
    };
    auto drain = [=]
    {
        const auto bytes = st->sock->readAll();
        if (bytes.isEmpty())
            return;
        if (sink && !sink(bytes))
            return drop(ui("Cannot write the local file."));
        report(bytes.size());
    };
    auto attach = [=](QSslSocket *sock)
    {
        st->sock = sock;
        data = sock;
        QObject::connect(sock, &QSslSocket::readyRead, ctx, drain);
        QObject::connect(sock, &QSslSocket::bytesWritten, ctx,
                         [=](qint64 n)
                         {
                             report(n);
                             pump();
                         });
        QObject::connect(sock, &QSslSocket::disconnected, ctx,
                         [=]
                         {
                             drain();
                             st->closed = true;
                             settle();
                         });
        QObject::connect(sock, &QSslSocket::errorOccurred, ctx,
                         [=](QAbstractSocket::SocketError e)
                         {
                             if (e != QAbstractSocket::RemoteHostClosedError)
                                 drop(sock->errorString());
                         });
        auto ready = [=]
        {
            st->ready = true;
            pump();
        };
        if (sock->state() == QAbstractSocket::ConnectedState)
            secureData(sock, ready);
        else
            QObject::connect(sock, &QSslSocket::connected, ctx, [=] { secureData(sock, ready); });
    };
    auto replied = [=](int code, const QString &text)
    {
        if (code < 200)
        {
            st->started = true;
            return pump();
        }
        st->replied = true;
        if (code >= 300)
            return drop(message(code, text)), settle();
        settle();
    };
    if (passive)
    {
        return openPassive(
            [=](quint16 port)
            {
                auto *sock = new QSslSocket(ctx);
                attach(sock);
                sock->connectToHost(control->peerAddress(), port);
                send(command, replied);
            });
    }
    openActive(
        [=](QTcpServer *server)
        {
            QObject::connect(server, &QTcpServer::newConnection, ctx,
                             [=]
                             {
                                 while (server->hasPendingConnections())
                                 {
                                     auto *sock = static_cast<QSslSocket *>(server->nextPendingConnection());
                                     // A single connection, and only from the control channel's host.
                                     if (st->sock || st->closed ||
                                         !sock->peerAddress().isEqual(control->peerAddress(), QHostAddress::TolerantConversion))
                                     {
                                         sock->abort();
                                         sock->deleteLater();
                                         continue;
                                     }
                                     sock->setParent(ctx);
                                     attach(sock);
                                 }
                             });
            send(command, replied);
        });
}

void FtpClient::listing(const QString &path, std::function<void(const QList<RemoteEntry> &)> then)
{
    const bool machine = usesMlsd();
    auto buffer = std::make_shared<QByteArray>();
    transfer((machine ? "MLSD" : "LIST") + (path.isEmpty() ? QString() : ' ' + path), nullptr,
             [buffer](const QByteArray &bytes)
             {
                 buffer->append(bytes);
                 return true;
             },
             {}, -1, [=] { then(parseEntries(*buffer, machine)); });
}

void FtpClient::list(const QString &path, Listed done)
{
    auto result = std::make_shared<QList<RemoteEntry>>();
    enqueue([=](bool ok, const QString &error) { done(ok, error, *result); },
            [=]
            {
                listing(path,
                        [=](const QList<RemoteEntry> &entries)
                        {
                            *result = entries;
                            complete(true);
                        });
            });
}

void FtpClient::cwd(const QString &path, Done done)
{
    enqueue(std::move(done), [=] { simple("CWD " + path, [this] { complete(true); }); });
}

void FtpClient::pwd(std::function<void(bool, const QString &, const QString &)> done)
{
    auto result = std::make_shared<QString>();
    enqueue([=](bool ok, const QString &error) { done(ok, error, *result); },
            [=]
            {
                send("PWD",
                     [=](int code, const QString &text)
                     {
                         const int a = text.indexOf('"'), b = text.lastIndexOf('"');
                         if (code < 200)
                             return;
                         if (code != 257 || a < 0 || b <= a)
                             return fail(message(code, text));
                         *result = text.mid(a + 1, b - a - 1).replace("\"\"", "\"");
                         complete(true);
                     });
            });
}

// Single file: writes to "<local>.part" (or resumes from it, or from the local file itself) and renames at the end.
void FtpClient::fetch(const QString &remote, const QString &local, bool resume, Progress progress, std::function<void()> then)
{
    send("SIZE " + remote,
         [=](int code, const QString &text)
         {
             if (code < 200)
                 return;
             const qint64 total = code == 213 ? text.trimmed().toLongLong() : -1;
             const auto part = local + ".part";
             const auto target = resume && !QFileInfo::exists(part) && QFileInfo(local).isFile() ? local : part;
             qint64 offset = resume && QFileInfo(target).isFile() ? QFileInfo(target).size() : 0;
             if (total >= 0 && offset > total)
                 offset = 0; // local file longer than the remote one: restart from zero
             auto file = std::make_shared<QFile>(target);
             if (!file->open(offset ? QIODevice::OpenMode(QIODevice::Append) : QIODevice::WriteOnly | QIODevice::Truncate))
                 return fail(file->errorString());
             sinkFile = file;
             auto finish = [=]
             {
                 const bool flushed = file->flush();
                 file->close();
                 sinkFile.reset();
                 if (!flushed)
                     return fail(file->errorString());
                 if (target != local)
                 {
                     QFile::remove(local);
                     if (!QFile::rename(target, local))
                         return fail(ui("Cannot write the local file."));
                 }
                 then();
             };
             if (total >= 0 && offset == total && offset > 0)
             {
                 if (progress)
                     progress(total, total);
                 return finish(); // already complete
             }
             auto go = [=](qint64 from)
             {
                 if (!from)
                     file->resize(0);
                 if (progress)
                     progress(from, total);
                 transfer(
                     "RETR " + remote, nullptr, [file](const QByteArray &bytes) { return file->write(bytes) == bytes.size(); },
                     progress ? Progress([=](qint64 n, qint64) { progress(from + n, total); }) : Progress(), total, finish);
             };
             if (!offset)
                 return go(0);
             send("REST " + QString::number(offset),
                  [=](int code, const QString &)
                  {
                      if (code >= 200)
                          go(code == 350 ? offset : 0); // REST not supported: full download
                  });
         });
}

// Single file: with resume, continues from the remote size (REST+STOR, APPE if REST is rejected).
void FtpClient::store(const QString &local, const QString &remote, bool resume, Progress progress, std::function<void()> then)
{
    auto file = std::make_shared<QFile>(local);
    if (!file->open(QIODevice::ReadOnly))
        return fail(file->errorString());
    const qint64 size = file->size();
    auto go = [=](qint64 from, const QString &verb)
    {
        if (!file->seek(from))
            return fail(file->errorString());
        if (progress)
            progress(from, size);
        transfer(verb + ' ' + remote, file, {},
                 progress ? Progress([=](qint64 n, qint64) { progress(from + n, size); }) : Progress(), size, then);
    };
    if (!resume)
        return go(0, "STOR");
    send("SIZE " + remote,
         [=](int code, const QString &text)
         {
             if (code < 200)
                 return;
             const qint64 have = code == 213 ? text.trimmed().toLongLong() : 0;
             if (code == 213 && have == size)
             {
                 if (progress)
                     progress(size, size);
                 return then(); // already complete
             }
             if (have <= 0 || have > size)
                 return go(0, "STOR");
             send("REST " + QString::number(have),
                  [=](int code, const QString &)
                  {
                      if (code >= 200)
                          code == 350 ? go(have, "STOR") : go(have, "APPE");
                  });
         });
}

// Breadth-first walk of remote folders: todo = (remote folder, local folder).
void FtpClient::scanRemote(std::shared_ptr<Plan> plan, QList<std::pair<QString, QString>> todo, std::function<void()> then)
{
    if (todo.isEmpty())
        return then();
    const auto next = todo.takeFirst();
    const QString remote = next.first, local = next.second;
    plan->dirs << local;
    listing(remote,
            [=](const QList<RemoteEntry> &entries) mutable
            {
                for (const auto &e : entries)
                {
                    if (!safeName(e.name))
                        return fail(ui("Unsafe file name from the server: %1").arg(e.name));
                    const auto from = child(remote, e.name), to = QDir(local).filePath(e.name);
                    if (e.dir && !e.link)
                        todo.append({from, to});
                    else
                    {
                        plan->files.append({from, to, e.size});
                        plan->total += e.size;
                    }
                }
                scanRemote(plan, todo, then);
            });
}

void FtpClient::makeDirs(std::shared_ptr<Plan> plan, int index, std::function<void()> then)
{
    if (index == plan->dirs.size())
        return then();
    // An existing folder replies 550: keep going, real errors surface with STOR.
    send("MKD " + plan->dirs[index],
         [=](int code, const QString &)
         {
             if (code >= 200)
                 makeDirs(plan, index + 1, then);
         });
}

void FtpClient::runPlan(std::shared_ptr<Plan> plan, int index, qint64 base, bool upload, bool resume, Progress progress)
{
    if (index == plan->files.size())
    {
        if (progress)
            progress(plan->total, plan->total);
        return complete(true);
    }
    const auto file = plan->files[index];
    auto step = progress ? Progress([=](qint64 n, qint64) { progress(base + n, plan->total); }) : Progress();
    auto next = [=] { runPlan(plan, index + 1, base + file.size, upload, resume, progress); };
    upload ? store(file.source, file.target, resume, step, next) : fetch(file.source, file.target, resume, step, next);
}

void FtpClient::download(const QString &remote, const QString &localFile, Progress progress, Done done, int flags)
{
    enqueue(std::move(done),
            [=]
            {
                const bool resume = flags & Resume;
                if (!(flags & Recursive))
                    return fetch(remote, localFile, resume, progress, [this] { complete(true); });
                auto plan = std::make_shared<Plan>();
                scanRemote(plan, {{remote, localFile}},
                           [=]
                           {
                               for (const auto &dir : plan->dirs)
                               {
                                   if (!QDir().mkpath(dir))
                                       return fail(ui("Cannot create the local folder %1.").arg(QDir::toNativeSeparators(dir)));
                               }
                               runPlan(plan, 0, 0, false, resume, progress);
                           });
            });
}

void FtpClient::upload(const QString &localFile, const QString &remote, Progress progress, Done done, int flags)
{
    enqueue(std::move(done),
            [=]
            {
                const bool resume = flags & Resume;
                if (!(flags & Recursive))
                    return store(localFile, remote, resume, progress, [this] { complete(true); });
                if (!QFileInfo(localFile).isDir())
                    return fail(ui("Not a folder: %1").arg(QDir::toNativeSeparators(localFile)));
                auto plan = std::make_shared<Plan>();
                const QDir base(localFile);
                // ponytail: local links are skipped (no cycles); follow them only if requested.
                QDirIterator it(localFile,
                                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System | QDir::NoSymLinks,
                                QDirIterator::Subdirectories);
                while (it.hasNext())
                {
                    const auto info = it.nextFileInfo();
                    const auto target = child(remote, base.relativeFilePath(info.filePath()));
                    if (info.isDir())
                        plan->dirs << target;
                    else
                    {
                        plan->files.append({info.filePath(), target, info.size()});
                        plan->total += info.size();
                    }
                }
                plan->dirs.sort(); // the parent is a prefix: it sorts before its children
                plan->dirs.prepend(remote);
                makeDirs(plan, 0, [=] { runPlan(plan, 0, 0, true, resume, progress); });
            });
}

void FtpClient::mkdir(const QString &path, Done done)
{
    enqueue(std::move(done), [=] { simple("MKD " + path, [this] { complete(true); }); });
}

void FtpClient::removeFile(const QString &path, Done done)
{
    enqueue(std::move(done), [=] { simple("DELE " + path, [this] { complete(true); }); });
}

void FtpClient::removeDirectory(const QString &path, bool recursive, Done done)
{
    enqueue(std::move(done),
            [=]
            {
                if (recursive)
                    removeTree(path, [this] { complete(true); });
                else
                    simple("RMD " + path, [this] { complete(true); });
            });
}

void FtpClient::removeTree(const QString &path, std::function<void()> then)
{
    listing(path, [=](const QList<RemoteEntry> &entries) { removeEntries(path, entries, then); });
}

void FtpClient::removeEntries(const QString &dir, QList<RemoteEntry> rest, std::function<void()> then)
{
    if (rest.isEmpty())
        return simple("RMD " + dir, then);
    const auto entry = rest.takeFirst();
    const auto path = child(dir, entry.name);
    auto next = [=] { removeEntries(dir, rest, then); };
    // Links are deleted, not followed.
    entry.dir && !entry.link ? removeTree(path, next) : simple("DELE " + path, next);
}

void FtpClient::rename(const QString &from, const QString &to, Done done)
{
    enqueue(std::move(done), [=] { simple("RNFR " + from, [=] { simple("RNTO " + to, [this] { complete(true); }); }); });
}

void FtpClient::abort()
{
    const auto jobs = queue;
    queue.clear();
    fail(ui("Aborted."));
    for (const auto &job : jobs)
    {
        if (job.done)
            job.done(false, ui("Aborted."));
    }
    loggedIn = false;
    control->abort();
}

void FtpClient::disconnectFromHost()
{
    abort();
}

bool FtpClient::isConnected() const
{
    return loggedIn && control->state() == QAbstractSocket::ConnectedState;
}

bool FtpClient::busy() const
{
    return running || !queue.isEmpty();
}

// ---------------------------------------------------------------- panel

struct FtpPanel::Transfer
{
    QString state, error, local, remote;
    bool upload = false, folder = false;
    QTreeWidgetItem *item = nullptr;
    QProgressBar *bar = nullptr;
};

FtpPanel::FtpPanel(const QJsonObject &profile, std::function<QString()> passwordProvider, QWidget *parent)
    : QWidget(parent), profile(profile), passwordProvider(std::move(passwordProvider)), path(new QLineEdit),
      files(new QTreeWidget), queueView(new QTreeWidget), status(new QLabel), queueBox(new QWidget)
{
    texts << [this] { setAccessibleName(ui("FTP browser")); };
    // Unverifiable certificate: fingerprint already accepted in the profile, otherwise the user decides.
    ftp->trustCertificate = [this](const QSslCertificate &certificate)
    {
        const auto print = FtpClient::fingerprint(certificate);
        if (print.compare(this->profile["ftpFingerprint"].toString(), Qt::CaseInsensitive) == 0)
            return true;
        const auto answer = QMessageBox::warning(
            this, ui("Untrusted certificate"),
            ui("The certificate of %1 cannot be verified.\n\nSHA-256 fingerprint:\n%2\n\nTrust this certificate for this host?")
                .arg(this->profile["host"].toString(), print),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return false;
        if (certificateTrusted)
            certificateTrusted(print);
        return true;
    };
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto *bar = new QToolBar;
    bar->setIconSize(QSize(18, 18));
    bar->setObjectName("sftpTools");
    layout->addWidget(bar);
    auto action = [&](const QString &name, const char *tip, auto slot, QKeySequence key = {})
    {
        auto *a = bar->addAction(icon(name), QString());
        texts << [a, tip]
        {
            a->setText(ui(tip));
            a->setToolTip(ui(tip));
        };
        if (!key.isEmpty())
        {
            a->setShortcut(key);
            a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        }
        connect(a, &QAction::triggered, this, slot);
        files->addAction(a);
    };
    auto up = [this]
    {
        if (!listed.isEmpty() && listed != "/")
            browse(listed.section('/', 0, -2).isEmpty() ? "/" : listed.section('/', 0, -2));
    };
    action("parent", "Parent directory", up, QKeySequence(Qt::Key_Backspace));
    action("download", "Download selected files and folders",
           [this]
           {
               const auto names = selectedNames();
               if (names.isEmpty())
                   return;
               const auto folder = QFileDialog::getExistingDirectory(
                   this, ui("Download to folder"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
               if (!folder.isEmpty())
                   download(names, folder);
           });
    action("upload", "Upload to current folder", [this] { upload(QFileDialog::getOpenFileNames(this, ui("Upload files"))); });
    action("refresh", "Refresh current folder", [this] { refresh(); }, QKeySequence(Qt::Key_F5));
    action("newfolder", "Create new directory",
           [this]
           {
               const auto name = QInputDialog::getText(this, ui("New directory"), ui("Directory name:")).trimmed();
               if (!name.isEmpty())
                   makeDirectory(name);
           });
    action(
        "delete", "Delete selected files",
        [this]
        {
            const auto names = selectedNames();
            if (!names.isEmpty() &&
                QMessageBox::question(this, ui("Delete"),
                                      ui("Delete %1 selected item(s)? Folders are deleted with all their contents.")
                                          .append("\n%2")
                                          .arg(names.size())
                                          .arg(names.mid(0, 10).join('\n'))) == QMessageBox::Yes)
                removeEntries(names);
        },
        QKeySequence(Qt::Key_Delete));
    action(
        "rename", "Rename",
        [this]
        {
            const auto names = selectedNames();
            if (names.size() != 1)
                return;
            const auto next = QInputDialog::getText(this, ui("Rename"), ui("New name:"), QLineEdit::Normal, names[0]).trimmed();
            if (!next.isEmpty() && next != names[0])
                renameEntry(names[0], next);
        },
        QKeySequence(Qt::Key_F2));

    path->setObjectName("sftpPath");
    texts << [this] { path->setAccessibleName(ui("Remote FTP path")); };
    connect(path, &QLineEdit::returnPressed, this, [this] { browse(path->text()); });
    layout->addWidget(path);

    files->setRootIsDecorated(false);
    files->setSelectionMode(QAbstractItemView::ExtendedSelection);
    files->setUniformRowHeights(true);
    files->setIconSize(QSize(16, 16));
    files->setColumnCount(4);
    files->setColumnWidth(0, 150);
    files->setAcceptDrops(true);
    files->viewport()->setAcceptDrops(true);
    files->viewport()->installEventFilter(this);
    files->setContextMenuPolicy(Qt::ActionsContextMenu);
    layout->addWidget(files, 1);
    connect(files, &QTreeWidget::itemActivated, this,
            [this, up](QTreeWidgetItem *item)
            {
                const auto name = item->text(0);
                const auto *entry = find(name);
                if (name == "..")
                    return up();
                if (entry && (entry->dir || entry->link))
                    return browse(join(name));
                if (!entry)
                    return;
                // Open with the default application from a temporary copy (no reload).
                const auto local = QDir(QDir::temp().filePath("CrossTerm-ftp")).filePath(name);
                QDir().mkpath(QFileInfo(local).path());
                ftp->download(join(name), local, {},
                              [this, local](bool ok, const QString &error)
                              {
                                  if (!ok)
                                      status->setText(error);
                                  else
                                      QDesktopServices::openUrl(QUrl::fromLocalFile(local));
                              });
            });

    // Transfer queue.
    auto *box = new QVBoxLayout(queueBox);
    box->setContentsMargins(0, 4, 0, 0);
    box->setSpacing(2);
    auto *title = new QLabel;
    title->setObjectName("empty");
    texts << [title] { title->setText(ui("Transfers")); };
    box->addWidget(title);
    queueView->setRootIsDecorated(false);
    queueView->setColumnCount(4);
    queueView->setMaximumHeight(150);
    queueView->setColumnWidth(0, 110);
    queueView->setColumnWidth(1, 60);
    queueView->setColumnWidth(2, 90);
    box->addWidget(queueView);
    auto *row = new QHBoxLayout;
    auto *cancel = new QPushButton, *resume = new QPushButton, *clear = new QPushButton;
    texts << [cancel, resume, clear]
    {
        cancel->setText(ui("Cancel"));
        resume->setText(ui("Resume"));
        resume->setToolTip(ui("Resume the selected failed or cancelled transfers"));
        clear->setText(ui("Clear finished"));
    };
    queueView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (auto *b : {cancel, resume, clear})
    {
        b->setStyleSheet("padding: 2px 8px;");
        row->addWidget(b);
    }
    row->addStretch();
    box->addLayout(row);
    connect(cancel, &QPushButton::clicked, this, [this] { ftp->abort(); }); // ponytail: cancels the whole FTP queue
    connect(resume, &QPushButton::clicked, this,
            [this]
            {
                for (int i = 0; i < transfers.size(); ++i)
                {
                    if (transfers[i]->item->isSelected())
                        resumeTransfer(i);
                }
            });
    connect(clear, &QPushButton::clicked, this,
            [this]
            {
                for (int i = int(transfers.size()) - 1; i >= 0; --i)
                {
                    if (transfers[i]->state == "done" || transfers[i]->state == "cancelled")
                    {
                        delete transfers[i]->item;
                        transfers.removeAt(i);
                    }
                }
            });
    queueBox->hide();
    layout->addWidget(queueBox);

    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    status->setObjectName("sftpStatus");
    layout->addWidget(status);
    retranslate();
    QTimer::singleShot(0, this, [this] { browse(this->profile["path"].toString()); });
}

FtpPanel::~FtpPanel() = default;

void FtpPanel::retranslate()
{
    for (const auto &apply : texts)
        apply();
    files->setHeaderLabels({ui("Name"), ui("Size"), ui("Last modified"), ui("Access")});
    queueView->setHeaderLabels({ui("File"), ui("Size"), ui("Progress"), ui("Status")});
    for (const auto &t : transfers)
        setState(*t, t->state, t->error);
}

QString FtpPanel::message() const
{
    return status->text();
}

// Runs then() once the session is open; login happens only once for all pending requests.
void FtpPanel::ensure(std::function<void()> then, std::function<void(const QString &)> failed)
{
    if (ftp->isConnected())
        return then();
    waiting.append({std::move(then), std::move(failed)});
    if (connecting)
        return;
    connecting = true;
    auto user = profile["user"].toString();
    if (user.compare("anonymous", Qt::CaseInsensitive) == 0)
        user.clear();
    if (!user.isEmpty() && password.isEmpty() && passwordProvider)
        password = passwordProvider();
    const auto host = profile["host"].toString();
    ftp->security = profile["ftpSecurity"].toString("none");
    ftp->passive = profile["ftpPassive"].toBool(true);
    status->setText(ui("Connecting to %1…").arg(host));
    ftp->connectToHost(host, quint16(profile["port"].toInt(ftp->security == "implicit" ? 990 : 21)), user, password,
                       [this](bool ok, const QString &error)
                       {
                           connecting = false;
                           if (!ok && error.startsWith("530"))
                               password.clear(); // the password is asked again on the next attempt
                           const auto jobs = std::exchange(waiting, {});
                           for (const auto &[then, failed] : jobs)
                               ok ? then() : failed(error);
                       });
}

void FtpPanel::browse(const QString &dir)
{
    ++operations;
    status->setText(ui("Listing %1…").arg(dir));
    files->setCursor(Qt::BusyCursor);
    auto finish = [this](const QString &error)
    {
        --operations;
        files->unsetCursor();
        status->setText(error);
    };
    ensure(
        [=]
        {
            auto show = [=]
            {
                ftp->pwd(
                    [=](bool ok, const QString &error, const QString &where)
                    {
                        if (!ok)
                            return finish(error);
                        ftp->list({},
                                  [=](bool ok, const QString &error, const QList<RemoteEntry> &entries)
                                  {
                                      if (ok)
                                      {
                                          listed = where;
                                          fill(entries);
                                      }
                                      finish(ok ? QString() : error);
                                  });
                    });
            };
            if (dir.isEmpty())
                return show();
            ftp->cwd(dir, [=](bool ok, const QString &error) { ok ? show() : finish(error); });
        },
        finish);
}

void FtpPanel::refresh()
{
    browse(listed);
}

void FtpPanel::fill(const QList<RemoteEntry> &entries)
{
    shown = entries;
    std::sort(shown.begin(), shown.end(), [](const RemoteEntry &a, const RemoteEntry &b)
              { return a.dir != b.dir ? a.dir : a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
    path->setText(listed);
    files->clear();
    auto *up = new QTreeWidgetItem(files, {".."});
    up->setIcon(0, icon("parent"));
    for (const auto &e : shown)
    {
        auto *item =
            new QTreeWidgetItem(files, {e.name, e.dir ? QString() : QLocale().formattedDataSize(e.size), e.modified, e.access});
        item->setIcon(0, icon(e.dir ? "folder" : "file"));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    }
}

const RemoteEntry *FtpPanel::find(const QString &name) const
{
    for (const auto &e : shown)
    {
        if (e.name == name)
            return &e;
    }
    return nullptr;
}

QStringList FtpPanel::selectedNames() const
{
    QStringList names;
    for (auto *item : files->selectedItems())
    {
        if (item->text(0) != "..")
            names << item->text(0);
    }
    return names;
}

QString FtpPanel::join(const QString &name) const
{
    return (listed.endsWith('/') ? listed : listed + '/') + name;
}

// Server operation followed by a fresh listing of the current folder.
void FtpPanel::operate(std::function<void(FtpClient::Done)> op)
{
    ++operations;
    auto failed = [this](const QString &error)
    {
        --operations;
        status->setText(error);
    };
    ensure(
        [=]
        {
            op(
                [=](bool ok, const QString &error)
                {
                    if (!ok)
                        return failed(error);
                    browse(listed);
                    --operations;
                });
        },
        failed);
}

void FtpPanel::makeDirectory(const QString &name)
{
    const auto target = join(name);
    operate([=](FtpClient::Done done) { ftp->mkdir(target, done); });
}

void FtpPanel::renameEntry(const QString &from, const QString &to)
{
    const auto source = join(from), target = to.startsWith('/') ? to : join(to);
    operate([=](FtpClient::Done done) { ftp->rename(source, target, done); });
}

void FtpPanel::removeEntries(const QStringList &names)
{
    QList<std::pair<QString, bool>> targets; // path, folder
    for (const auto &name : names)
    {
        const auto *entry = find(name);
        targets.append({join(name), entry && entry->dir && !entry->link});
    }
    if (targets.isEmpty())
        return;
    status->setText(ui("Deleting…"));
    operate(
        [=](FtpClient::Done done)
        {
            auto left = std::make_shared<int>(int(targets.size()));
            auto firstError = std::make_shared<QString>();
            for (const auto &[target, folder] : targets)
            {
                auto one = [=](bool ok, const QString &error)
                {
                    if (!ok && firstError->isEmpty())
                        *firstError = error;
                    if (--*left == 0)
                        done(firstError->isEmpty(), *firstError);
                };
                folder ? ftp->removeDirectory(target, true, one) : ftp->removeFile(target, one);
            }
        });
}

void FtpPanel::upload(const QStringList &localFiles)
{
    if (listed.isEmpty())
        return;
    for (const auto &local : localFiles)
    {
        const QFileInfo info(local);
        enqueue(info.fileName(), local, join(info.fileName()), true, info.isDir() ? -1 : info.size(), info.isDir());
    }
}

void FtpPanel::download(const QStringList &names, const QString &folder)
{
    for (const auto &name : names)
    {
        const auto *entry = find(name);
        const bool dir = entry && entry->dir;
        enqueue(name, QDir(folder).filePath(name), join(name), false, entry && !dir ? entry->size : -1, dir);
    }
}

void FtpPanel::resumeTransfer(int i)
{
    if (i >= 0 && i < transfers.size() && (transfers[i]->state == "failed" || transfers[i]->state == "cancelled"))
        start(transfers[i], true);
}

void FtpPanel::setState(Transfer &t, const QString &state, const QString &error)
{
    t.state = state;
    t.error = error;
    const auto label = state == "queued"      ? ui("Queued")
                       : state == "running"   ? ui("Transferring")
                       : state == "done"      ? ui("Completed")
                       : state == "cancelled" ? ui("Cancelled")
                                              : ui("Failed");
    t.item->setText(3, state == "failed" && !error.isEmpty() ? label + ": " + error : label);
    t.item->setToolTip(3, error);
}

void FtpPanel::enqueue(const QString &name, const QString &local, const QString &remote, bool upload, qint64 size, bool folder)
{
    auto t = std::make_shared<Transfer>();
    t->local = local;
    t->remote = remote;
    t->upload = upload;
    t->folder = folder;
    t->item = new QTreeWidgetItem(queueView, {name, size >= 0 ? QLocale().formattedDataSize(size) : QString()});
    t->item->setToolTip(0, QDir::toNativeSeparators(local));
    t->item->setIcon(0, folder ? icon("folder", 16) : icon(upload ? "upload" : "download", 16));
    t->bar = new QProgressBar;
    t->bar->setRange(0, 100);
    t->bar->setValue(0);
    t->bar->setMaximumHeight(14);
    queueView->setItemWidget(t->item, 2, t->bar);
    transfers << t;
    queueBox->show();
    start(t, false);
}

// Starts (or resumes) a queued transfer; folders report aggregate progress.
void FtpPanel::start(const std::shared_ptr<Transfer> &t, bool resume)
{
    setState(*t, "queued");
    const auto parent = t->remote.section('/', 0, -2);
    const auto directory = parent.isEmpty() ? QString("/") : parent;
    auto progress = [this, t](qint64 done, qint64 total)
    {
        if (t->state == "queued")
            setState(*t, "running");
        if (total > 0)
        {
            t->bar->setValue(int(done * 100 / total));
            if (t->folder)
                t->item->setText(1, QLocale().formattedDataSize(total));
        }
    };
    auto finished = [this, t, directory](bool ok, const QString &error)
    {
        if (ok)
            t->bar->setValue(100);
        setState(*t, ok ? "done" : error == ui("Aborted.") ? "cancelled" : "failed", ok ? QString() : error);
        if (ok && t->upload && directory == listed && transfersIdle())
            refresh();
    };
    const int flags = (resume ? FtpClient::Resume : 0) | (t->folder ? FtpClient::Recursive : 0);
    ensure(
        [=]
        {
            t->upload ? ftp->upload(t->local, t->remote, progress, finished, flags)
                      : ftp->download(t->remote, t->local, progress, finished, flags);
        },
        [=](const QString &error) { finished(false, error); });
}

QString FtpPanel::transferState(int i) const
{
    return i >= 0 && i < transfers.size() ? transfers[i]->state : QString();
}

int FtpPanel::transferProgress(int i) const
{
    return i >= 0 && i < transfers.size() ? transfers[i]->bar->value() : -1;
}

bool FtpPanel::transfersIdle() const
{
    return std::none_of(transfers.begin(), transfers.end(),
                        [](const auto &t) { return t->state == "queued" || t->state == "running"; });
}

bool FtpPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == files->viewport() && (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove))
    {
        auto *drag = static_cast<QDropEvent *>(event);
        if (drag->mimeData()->hasUrls() && !listed.isEmpty())
        {
            drag->acceptProposedAction();
            return true;
        }
    }
    if (watched == files->viewport() && event->type() == QEvent::Drop)
    {
        QStringList locals;
        for (const auto &url : static_cast<QDropEvent *>(event)->mimeData()->urls())
        {
            if (url.isLocalFile())
                locals << url.toLocalFile();
        }
        upload(locals);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

// ---------------------------------------------------------------- server

class FtpSession : public QObject
{
public:
    FtpSession(FtpServer *server, QSslSocket *socket);

private:
    FtpServer *server;
    QSslSocket *control;
    QTcpServer *passive = nullptr;
    QSslSocket *data = nullptr;
    std::function<void()> waiting; // transfer waiting for the data connection
    QByteArray input;
    QString cwd = "/", user, renameFrom;
    QHostAddress activeHost; // active mode (PORT/EPRT); activePort 0 = not requested
    quint16 activePort = 0;
    qint64 restart = 0;                                        // REST for the next RETR/STOR
    bool loggedIn = false, anonymous = false, protect = false; // protect: PROT P, data over TLS
    bool dataReady = false;                                    // data connection open (and encrypted if protect)
    int failures = 0;

    void reply(int code, const QString &text) { control->write(QString("%1 %2\r\n").arg(code).arg(text).toUtf8()); }
    void note(const QString &text) const
    {
        if (server->log)
            server->log(control->peerAddress().toString() + ": " + text);
    }
    void handle(const QString &line);
    QString virtualPath(const QString &arg) const;
    QString localPath(const QString &virt) const;
    QString resolve(const QString &arg) const
    {
        const auto virt = virtualPath(arg);
        return virt.isEmpty() ? QString() : localPath(virt);
    }
    void openPassive(bool extended);
    void openActive(const QString &command, const QString &arg);
    void attachData(QSslSocket *socket);
    void closeData();
    void withData(std::function<void()> action);
    void finish(QSslSocket *socket, bool ok, int failCode = 426);
    void sendFrom(std::shared_ptr<QIODevice> source);
    void storeTo(std::shared_ptr<QFile> file);
};

// Real path with all links resolved. On Windows QFileInfo::canonicalFilePath does not follow
// junctions: ask the system for the final path of the open handle.
static QString realPath(const QString &path)
{
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    HANDLE h = CreateFileW(native.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    std::wstring buffer(32768, L' ');
    const DWORD n = GetFinalPathNameByHandleW(h, buffer.data(), DWORD(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!n || n >= buffer.size())
        return {};
    auto result = QString::fromWCharArray(buffer.data(), int(n));
    if (result.startsWith(R"(\\?\UNC\)"))
        result = R"(\\)" + result.mid(8);
    else if (result.startsWith(R"(\\?\)"))
        result = result.mid(4);
    return QDir::fromNativeSeparators(result);
#else
    return QFileInfo(path).canonicalFilePath();
#endif
}

static bool isLink(const QFileInfo &info)
{
    return info.isSymbolicLink() || info.isJunction();
}

static QString facts(const QFileInfo &info)
{
    return QString("type=%1;size=%2;modify=%3;UNIX.mode=%4;")
        .arg(isLink(info)   ? "OS.unix=symlink"
             : info.isDir() ? "dir"
                            : "file")
        .arg(info.isDir() ? 0 : info.size())
        .arg(info.lastModified().toUTC().toString("yyyyMMddHHmmss"))
        .arg(QString::number(unixMode(info), 8).rightJustified(4, '0'));
}

static QByteArray listing(const QString &local, const QString &kind)
{
    const QFileInfo info(local);
    const auto items = info.isDir()
                           ? QDir(local).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                                       QDir::DirsFirst | QDir::Name | QDir::IgnoreCase)
                           : QFileInfoList{info};
    QByteArray out;
    for (const auto &fi : items)
    {
        QString line = fi.fileName();
        if (kind == "MLSD")
            line = facts(fi) + ' ' + fi.fileName();
        else if (kind == "LIST")
        {
            const auto when = fi.lastModified();
            const auto date =
                QLocale::c().toString(when, when.date().year() == QDate::currentDate().year() ? "MMM dd HH:mm" : "MMM dd  yyyy");
            line = QString("%1 1 ftp ftp %2 %3 %4")
                       .arg(modeString(unixMode(fi), isLink(fi)   ? 'l'
                                                     : fi.isDir() ? 'd'
                                                                  : '-'))
                       .arg(fi.isDir() ? 0 : fi.size(), 12)
                       .arg(date, fi.fileName());
        }
        out += line.toUtf8() + "\r\n";
    }
    return out;
}

FtpSession::FtpSession(FtpServer *server, QSslSocket *socket) : QObject(server->sessions), server(server), control(socket)
{
    socket->setParent(this);
    connect(socket, &QTcpSocket::disconnected, this,
            [this]
            {
                note("disconnected");
                deleteLater();
            });
    connect(socket, &QTcpSocket::readyRead, this,
            [this]
            {
                input += control->readAll();
                for (int nl; control->state() == QAbstractSocket::ConnectedState && (nl = input.indexOf('\n')) >= 0;)
                {
                    auto line = QString::fromUtf8(input.left(nl));
                    input.remove(0, nl + 1);
                    if (line.endsWith('\r'))
                        line.chop(1);
                    handle(line);
                }
                if (input.size() > 8192) // abnormally long command line
                {
                    input.clear();
                    control->abort();
                }
            });
    note("connected");
    if (!server->implicitTls)
    {
        reply(220, "CrossTerm FTP server ready.");
        return;
    }
    // Implicit FTPS: greeting only once the channel is encrypted; a failed handshake closes the connection.
    connect(socket, &QSslSocket::encrypted, this, [this] { reply(220, "CrossTerm FTPS server ready."); });
    connect(socket, &QSslSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError e)
            {
                if (e != QAbstractSocket::RemoteHostClosedError && !control->isEncrypted())
                    control->abort();
            });
    socket->setSslConfiguration(*server->tls);
    socket->startServerEncryption();
}

// Normalized virtual path ("/a/b"), empty if it escapes the root or contains disallowed names.
QString FtpSession::virtualPath(const QString &arg) const
{
    static const QRegularExpression forbidden("^(con|prn|aux|nul|com\\d|lpt\\d)(\\..*)?$|^[. ]+$",
                                              QRegularExpression::CaseInsensitiveOption);
    auto text = arg;
    text.replace('\\', '/');
    if (text.contains(QChar(0)))
        return {};
    auto parts = text.startsWith('/') ? QStringList() : cwd.split('/', Qt::SkipEmptyParts);
    for (const auto &part : text.split('/', Qt::SkipEmptyParts))
    {
        if (part == ".")
            continue;
        if (part == "..")
        {
            if (parts.isEmpty())
                return {}; // above the root
            parts.removeLast();
            continue;
        }
        // ":" = drive or NTFS stream; Windows device names and names made only of dots/spaces.
        if (part.contains(':') || forbidden.match(part).hasMatch())
            return {};
        parts << part;
    }
    return '/' + parts.join('/');
}

// Local path, empty if the deepest existing ancestor (links resolved) is outside the real root.
// ponytail: check and use are not atomic (TOCTOU) if other processes create links inside the root.
QString FtpSession::localPath(const QString &virt) const
{
    const auto local = QDir::cleanPath(server->canonicalRoot + virt);
    QFileInfo probe(local);
    while (!probe.exists() && !probe.isSymLink())
    {
        const QFileInfo up(probe.absolutePath());
        if (up.absoluteFilePath() == probe.absoluteFilePath())
            return {};
        probe = up;
    }
    const auto real = realPath(probe.absoluteFilePath());
    const auto &root = server->canonicalRoot;
    if (real.isEmpty() || (real != root && !real.startsWith(root.endsWith('/') ? root : root + '/')))
        return {};
    return local;
}

void FtpSession::closeData()
{
    waiting = nullptr;
    dataReady = false;
    activePort = 0;
    if (data)
    {
        QObject::disconnect(data, nullptr, this, nullptr);
        data->abort();
        data->deleteLater();
        data = nullptr;
    }
    if (passive)
    {
        passive->close();
        passive->deleteLater();
        passive = nullptr;
    }
}

void FtpSession::openPassive(bool extended)
{
    closeData();
    auto local = control->localAddress();
    bool v4 = false;
    const quint32 ip = local.toIPv4Address(&v4);
    if (v4)
        local = QHostAddress(ip);
    else if (!extended)
        return reply(425, "Use EPSV over IPv6.");
    passive = new SslListener(this);
    const int low = server->passivePortMin, high = server->passivePortMax;
    bool ok = low <= 0 || high < low ? passive->listen(local, 0) : false;
    for (int i = 0; !ok && low > 0 && high >= low && i <= high - low; ++i)
        ok = passive->listen(local, quint16(low + (server->nextPassive++ & 0xffff) % (high - low + 1)));
    if (!ok)
    {
        delete passive;
        passive = nullptr;
        return reply(425, "Cannot open a passive data port.");
    }
    connect(passive, &QTcpServer::newConnection, this,
            [this]
            {
                while (passive && passive->hasPendingConnections())
                {
                    auto *socket = static_cast<QSslSocket *>(passive->nextPendingConnection());
                    // Only from the same host as the control connection (no data connection theft).
                    if (data || !socket->peerAddress().isEqual(control->peerAddress(), QHostAddress::TolerantConversion))
                    {
                        socket->abort();
                        socket->deleteLater();
                        continue;
                    }
                    socket->setParent(this);
                    data = socket;
                }
                if (!data || !passive)
                    return;
                passive->close();
                passive->deleteLater();
                passive = nullptr;
                attachData(data);
            });
    const quint16 port = passive->serverPort();
    if (extended)
        return reply(229, QString("Entering Extended Passive Mode (|||%1|)").arg(port));
    reply(227, QString("Entering Passive Mode (%1,%2,%3,%4,%5,%6)")
                   .arg(ip >> 24)
                   .arg((ip >> 16) & 255)
                   .arg((ip >> 8) & 255)
                   .arg(ip & 255)
                   .arg(port >> 8)
                   .arg(port & 255));
}

// PORT/EPRT. No "FTP bounce": only to the control client's IP and non-privileged ports.
void FtpSession::openActive(const QString &command, const QString &arg)
{
    QHostAddress host;
    int port = -1;
    if (command == "PORT")
    {
        const auto parts = arg.split(',');
        QList<int> n;
        for (const auto &p : parts)
        {
            bool ok = false;
            const int v = p.trimmed().toInt(&ok);
            if (ok && v >= 0 && v <= 255)
                n << v;
        }
        if (n.size() == 6 && parts.size() == 6)
        {
            host = QHostAddress(quint32(n[0]) << 24 | n[1] << 16 | n[2] << 8 | n[3]);
            port = n[4] * 256 + n[5];
        }
    }
    else if (arg.size() > 1) // EPRT |1|address|port|
    {
        const auto fields = arg.split(arg[0]);
        if (fields.size() == 5 && (fields[1] == "1" || fields[1] == "2"))
        {
            host = QHostAddress(fields[2]);
            port = fields[3].toInt();
        }
    }
    if (host.isNull() || !host.isEqual(control->peerAddress(), QHostAddress::TolerantConversion) || port < 1024 || port > 65535)
        return reply(500, "Illegal " + command + " command.");
    closeData();
    activeHost = host;
    activePort = quint16(port);
    reply(200, command + " command successful.");
}

// Data connection just opened (passive accepted or active in progress): TLS if PROT P, then the pending transfer.
void FtpSession::attachData(QSslSocket *socket)
{
    connect(socket, &QAbstractSocket::errorOccurred, this,
            [this, socket]
            {
                if (data != socket || dataReady)
                    return; // after opening, errors are handled by the transfer
                const bool expected = bool(waiting);
                closeData();
                if (expected)
                    reply(425, "Cannot open data connection.");
            });
    auto ready = [this, socket]
    {
        if (data != socket || dataReady)
            return;
        dataReady = true;
        if (auto action = std::exchange(waiting, nullptr))
            action();
    };
    auto secure = [this, socket, ready]
    {
        if (!protect)
            return ready();
        // RFC 4217: on the data channel too the server is the TLS server side, in either direction.
        connect(socket, &QSslSocket::encrypted, this, ready);
        socket->setSslConfiguration(*server->tls);
        socket->startServerEncryption();
    };
    if (socket->state() == QAbstractSocket::ConnectedState)
        secure();
    else
        connect(socket, &QAbstractSocket::connected, this, secure);
}

void FtpSession::withData(std::function<void()> action)
{
    auto start = [this, action]
    {
        reply(150, "Opening data connection.");
        action();
    };
    if (data && dataReady)
        return start();
    if (data || passive)
    {
        waiting = start;
        return;
    }
    if (!activePort)
        return reply(425, "Use PORT, EPRT, PASV or EPSV first.");
    waiting = start;
    data = new QSslSocket(this);
    attachData(data);
    data->connectToHost(activeHost, std::exchange(activePort, 0));
}

void FtpSession::finish(QSslSocket *socket, bool ok, int failCode)
{
    if (data != socket)
        return;
    QObject::disconnect(socket, nullptr, this, nullptr);
    data = nullptr;
    dataReady = false;
    socket->deleteLater();
    if (ok)
        reply(226, "Transfer complete.");
    else
        reply(failCode, failCode == 451 ? "Local error while writing the file." : "Connection closed; transfer aborted.");
}

void FtpSession::sendFrom(std::shared_ptr<QIODevice> source)
{
    withData(
        [this, source]
        {
            auto *socket = data;
            auto eof = std::make_shared<bool>(false), broken = std::make_shared<bool>(false);
            auto pump = [=]
            {
                while (!*eof && socket->state() == QAbstractSocket::ConnectedState && socket->bytesToWrite() < (1 << 20))
                {
                    const auto chunk = source->read(256 * 1024);
                    if (chunk.isEmpty())
                    {
                        *eof = true;
                        *broken = !source->atEnd();
                        socket->disconnectFromHost(); // after sending pending data
                        return;
                    }
                    socket->write(chunk);
                }
            };
            connect(socket, &QTcpSocket::bytesWritten, this, pump);
            connect(socket, &QTcpSocket::errorOccurred, this,
                    [=]
                    {
                        if (!*eof)
                            *broken = true;
                    });
            connect(socket, &QTcpSocket::disconnected, this, [=] { finish(socket, *eof && !*broken); });
            if (socket->state() != QAbstractSocket::ConnectedState)
                return finish(socket, false);
            pump();
        });
}

void FtpSession::storeTo(std::shared_ptr<QFile> file)
{
    withData(
        [this, file]
        {
            auto *socket = data;
            auto writeError = std::make_shared<bool>(false), reset = std::make_shared<bool>(false);
            auto drain = [=]
            {
                const auto bytes = socket->readAll();
                if (!bytes.isEmpty() && file->write(bytes) != bytes.size())
                    *writeError = true;
            };
            auto done = [=]
            {
                drain();
                file->close();
                finish(socket, !*writeError && !*reset, *writeError ? 451 : 426);
            };
            connect(socket, &QTcpSocket::readyRead, this, drain);
            connect(socket, &QTcpSocket::errorOccurred, this,
                    [=](QAbstractSocket::SocketError e)
                    {
                        if (e != QAbstractSocket::RemoteHostClosedError)
                            *reset = true;
                    });
            connect(socket, &QTcpSocket::disconnected, this, done);
            socket->state() == QAbstractSocket::ConnectedState ? drain() : done();
        });
}

void FtpSession::handle(const QString &line)
{
    const int space = line.indexOf(' ');
    const auto command = (space < 0 ? line : line.left(space)).toUpper();
    const auto arg = space < 0 ? QString() : line.mid(space + 1);
    note(command == "PASS" ? QString("PASS ****") : line);
    static const QStringList writes{"STOR", "APPE", "DELE", "MKD", "XMKD", "RMD", "XRMD", "RNFR", "RNTO"};

    if (command == "USER")
    {
        user = arg;
        loggedIn = false;
        anonymous = server->anonymous &&
                    (arg.compare("anonymous", Qt::CaseInsensitive) == 0 || arg.compare("ftp", Qt::CaseInsensitive) == 0);
        reply(331, anonymous ? "Anonymous login ok, send your e-mail address as password." : "Password required.");
    }
    else if (command == "PASS")
    {
        if (user.isEmpty())
            return reply(503, "Login with USER first.");
        loggedIn = anonymous || (!server->userName.isEmpty() && user == server->userName && arg == server->secret);
        if (loggedIn)
        {
            cwd = "/";
            return reply(230, "Login successful.");
        }
        reply(530, "Login incorrect.");
        if (++failures >= 3)
            control->disconnectFromHost();
    }
    else if (command == "QUIT")
    {
        reply(221, "Goodbye.");
        control->disconnectFromHost();
    }
    else if (command == "SYST")
        reply(215, "UNIX Type: L8");
    else if (command == "FEAT")
        control->write(QByteArray("211-Features:\r\n EPSV\r\n EPRT\r\n PASV\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n UTF8\r\n") +
                       (server->tls ? " AUTH TLS\r\n PBSZ\r\n PROT\r\n" : "") +
                       " MLST type*;size*;modify*;UNIX.mode*;\r\n211 End\r\n");
    else if (command == "OPTS")
        arg.toUpper().startsWith("UTF8") ? reply(200, "UTF8 mode enabled.") : reply(501, "Option not supported.");
    else if (command == "NOOP")
        reply(200, "OK.");
    else if (command == "AUTH")
    {
        const auto mechanism = arg.toUpper();
        if (!server->tls)
            return reply(502, "TLS is not configured.");
        if (control->isEncrypted())
            return reply(503, "Already using TLS.");
        if (mechanism != "TLS" && mechanism != "TLS-C" && mechanism != "SSL")
            return reply(504, "Unsupported security mechanism.");
        reply(234, "Proceed with negotiation.");
        user.clear(); // RFC 4217: AUTH resets the login
        loggedIn = false;
        control->setSslConfiguration(*server->tls);
        control->startServerEncryption(); // the plaintext reply goes out before the handshake
    }
    else if (command == "PBSZ")
        control->isEncrypted() ? reply(200, "PBSZ=0") : reply(503, "Use AUTH TLS first.");
    else if (command == "PROT")
    {
        const auto level = arg.toUpper();
        if (!control->isEncrypted())
            return reply(503, "Use AUTH TLS first.");
        if (level != "P" && level != "C")
            return reply(536, "Only C and P are supported.");
        protect = level == "P";
        reply(200, protect ? "Data channel protected." : "Data channel in clear.");
    }
    else if (!loggedIn)
        reply(530, "Please log in with USER and PASS.");
    else if (command == "PWD" || command == "XPWD")
        reply(257, '"' + QString(cwd).replace('"', "\"\"") + "\" is the current directory.");
    else if (command == "CWD" || command == "CDUP" || command == "XCUP")
    {
        const auto virt = virtualPath(command == "CWD" ? arg : "..");
        const auto local = virt.isEmpty() ? QString() : localPath(virt);
        if (local.isEmpty() || !QFileInfo(local).isDir())
            return reply(550, "No such directory.");
        cwd = virt;
        reply(250, "Directory changed to " + virt);
    }
    else if (command == "TYPE") // ponytail: ASCII treated as binary (no line ending conversion)
        !arg.isEmpty() && QString("AIL").contains(arg.left(1).toUpper()) ? reply(200, "Type set.")
                                                                         : reply(504, "Type not supported.");
    else if (command == "MODE" || command == "STRU")
        arg.toUpper() == (command == "MODE" ? "S" : "F") ? reply(200, "OK.") : reply(504, "Not supported.");
    else if (command == "PASV")
        openPassive(false);
    else if (command == "EPSV")
        arg.toUpper() == "ALL" ? reply(200, "EPSV ALL ok.") : openPassive(true);
    else if (command == "PORT" || command == "EPRT")
        openActive(command, arg);
    else if (command == "REST")
    {
        bool ok = false;
        const qint64 at = arg.toLongLong(&ok);
        if (!ok || at < 0)
            return reply(501, "Invalid restart position.");
        restart = at;
        reply(350, QString("Restarting at %1.").arg(at));
    }
    else if (command == "LIST" || command == "NLST" || command == "MLSD")
    {
        auto target = arg;
        while (command != "MLSD" && target.startsWith('-')) // ls-style options ("-la")
            target = target.section(' ', 1);
        const auto local = resolve(target);
        if (local.isEmpty() || !QFileInfo::exists(local) || (command == "MLSD" && !QFileInfo(local).isDir()))
            return reply(550, "No such file or directory.");
        auto buffer = std::make_shared<QBuffer>();
        buffer->setData(listing(local, command));
        buffer->open(QIODevice::ReadOnly);
        sendFrom(buffer);
    }
    else if (command == "RETR")
    {
        const qint64 from = std::exchange(restart, 0);
        const auto local = resolve(arg);
        auto file = std::make_shared<QFile>(local);
        if (local.isEmpty() || !QFileInfo(local).isFile() || !file->open(QIODevice::ReadOnly))
            return reply(550, "No such file or not readable.");
        if (from > file->size() || !file->seek(from))
            return reply(554, "Invalid restart position.");
        sendFrom(file);
    }
    else if (command == "SIZE" || command == "MDTM" || command == "MLST")
    {
        const auto local = resolve(arg);
        const QFileInfo info(local);
        if (local.isEmpty() || !info.exists() || (command == "SIZE" && !info.isFile()))
            return reply(550, "No such file.");
        if (command == "SIZE")
            return reply(213, QString::number(info.size()));
        if (command == "MDTM")
            return reply(213, info.lastModified().toUTC().toString("yyyyMMddHHmmss"));
        control->write(("250-Listing\r\n " + facts(info) + ' ' + info.fileName() + "\r\n250 End\r\n").toUtf8());
    }
    else if (writes.contains(command) && anonymous)
        reply(550, "Permission denied: anonymous access is read-only.");
    else if (command == "STOR" || command == "APPE")
    {
        // REST+STOR writes from the given offset (the rest is truncated); APPE appends.
        const qint64 from = std::exchange(restart, 0);
        const auto local = arg.isEmpty() ? QString() : resolve(arg);
        const QFileInfo info(local);
        auto file = std::make_shared<QFile>(local);
        QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Truncate;
        if (command == "APPE")
            mode = QIODevice::Append;
        else if (from)
            mode = QIODevice::ReadWrite;
        if (local.isEmpty() || info.isDir() || !QFileInfo(info.absolutePath()).isDir() || !file->open(mode))
            return reply(550, "Cannot create the file.");
        if (command == "STOR" && from && (from > file->size() || !file->resize(from) || !file->seek(from)))
            return reply(554, "Invalid restart position.");
        storeTo(file);
    }
    else if (command == "DELE")
    {
        const auto local = resolve(arg);
        const QFileInfo info(local);
        if (local.isEmpty() || (info.isDir() && !isLink(info)) || !(QFile::remove(local) || QDir().rmdir(local)))
            return reply(550, "Cannot delete the file.");
        reply(250, "File deleted.");
    }
    else if (command == "MKD" || command == "XMKD")
    {
        const auto virt = virtualPath(arg);
        const auto local = virt.isEmpty() ? QString() : localPath(virt);
        if (local.isEmpty() || QFileInfo::exists(local) || !QDir().mkdir(local))
            return reply(550, "Cannot create the directory.");
        reply(257, '"' + QString(virt).replace('"', "\"\"") + "\" created.");
    }
    else if (command == "RMD" || command == "XRMD")
    {
        const auto virt = virtualPath(arg);
        const auto local = virt.isEmpty() || virt == "/" ? QString() : localPath(virt);
        if (local.isEmpty() || !QFileInfo(local).isDir() || !QDir().rmdir(local))
            return reply(550, "Cannot remove the directory (missing or not empty).");
        reply(250, "Directory removed.");
    }
    else if (command == "RNFR")
    {
        const auto virt = virtualPath(arg);
        renameFrom = virt.isEmpty() || virt == "/" ? QString() : localPath(virt);
        if (renameFrom.isEmpty() || !QFileInfo::exists(renameFrom))
        {
            renameFrom.clear();
            return reply(550, "No such file or directory.");
        }
        reply(350, "Ready for RNTO.");
    }
    else if (command == "RNTO")
    {
        const auto from = std::exchange(renameFrom, QString());
        const auto to = resolve(arg);
        if (from.isEmpty())
            return reply(503, "Send RNFR first.");
        if (to.isEmpty() || QFileInfo::exists(to) || !QDir().rename(from, to))
            return reply(550, "Cannot rename.");
        reply(250, "Renamed.");
    }
    else if (command == "ABOR")
    {
        if (data || waiting)
            reply(426, "Transfer aborted.");
        closeData();
        reply(226, "ABOR successful.");
    }
    else
        reply(502, "Command not implemented.");
}

FtpServer::FtpServer() : listener(new SslListener), sessions(new QObject)
{
    QObject::connect(listener, &QTcpServer::newConnection, listener,
                     [this]
                     {
                         while (listener->hasPendingConnections())
                             new FtpSession(this, static_cast<QSslSocket *>(listener->nextPendingConnection()));
                     });
}

// Certificate (with optional chain) and PEM key; the key is never written to the log.
static std::shared_ptr<QSslConfiguration> loadTls(const QString &certificateFile, const QString &keyFile, QString &error)
{
    if (!QSslSocket::supportsSsl())
    {
        error = ui("TLS is not available on this system.");
        return nullptr;
    }
    const auto chain = QSslCertificate::fromPath(certificateFile, QSsl::Pem, QSslCertificate::PatternSyntax::FixedString);
    QFile file(keyFile.isEmpty() ? certificateFile : keyFile);
    const auto pem = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    QSslKey key;
    for (auto algorithm : {QSsl::Rsa, QSsl::Ec, QSsl::Dsa})
    {
        if (key.isNull())
            key = QSslKey(pem, algorithm, QSsl::Pem);
    }
    if (chain.isEmpty() || key.isNull())
    {
        error = ui("Cannot load the TLS certificate or its private key (PEM, without passphrase).");
        return nullptr;
    }
    auto tls = std::make_shared<QSslConfiguration>(QSslConfiguration::defaultConfiguration());
    tls->setLocalCertificateChain(chain);
    tls->setPrivateKey(key);
    tls->setPeerVerifyMode(QSslSocket::VerifyNone); // no certificate requested from clients
    return tls;
}

FtpServer::~FtpServer()
{
    stop();
    delete sessions;
    delete listener;
}

bool FtpServer::start(const QString &rootDirectory, quint16 port, bool allowAnonymous, const QString &user,
                      const QString &password)
{
    stop();
    canonicalRoot = rootDirectory.isEmpty() ? QString() : realPath(QFileInfo(rootDirectory).absoluteFilePath());
    if (canonicalRoot.isEmpty() || !QFileInfo(canonicalRoot).isDir())
    {
        error = ui("The root folder does not exist.");
        return false;
    }
    if (!allowAnonymous && user.isEmpty())
    {
        error = ui("Set a user name or allow anonymous access.");
        return false;
    }
    tls.reset();
    if (!certificateFile.isEmpty() && !(tls = loadTls(certificateFile, privateKeyFile, error)))
        return false;
    if (implicitTls && !tls)
    {
        error = ui("Implicit FTPS requires a certificate and a private key.");
        return false;
    }
    anonymous = allowAnonymous;
    userName = user;
    secret = password;
    if (!listener->listen(bindAllInterfaces ? QHostAddress::Any : QHostAddress::LocalHost, port))
    {
        error = listener->errorString();
        return false;
    }
    error.clear();
    return true;
}

void FtpServer::stop()
{
    listener->close();
    const auto open = sessions->children();
    qDeleteAll(open);
}

bool FtpServer::isRunning() const
{
    return listener->isListening();
}

quint16 FtpServer::port() const
{
    return isRunning() ? listener->serverPort() : 0;
}
