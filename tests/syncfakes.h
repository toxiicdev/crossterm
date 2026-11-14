#pragma once
// Shared by the sync tests: CHECK, await() for asynchronous storages, the read / write / conflict cycle every
// storage must pass, and a minimal local HTTP server that plays the role of WebDAV, GitHub, Dropbox, Microsoft
// Graph and Google Drive.
#include "syncstorage.h"
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <cstdio>
#include <functional>
#include <optional>

#define CHECK(expression)                                                                                                        \
    if (!(expression))                                                                                                           \
    {                                                                                                                            \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #expression);                                                            \
        return 1;                                                                                                                \
    }

using namespace SyncStorage;

// Runs the event loop until the storage answers.
static Result await(const std::function<void(Done)> &call)
{
    std::optional<Result> result;
    call([&](const Result &r) { result = r; });
    QDeadlineTimer deadline(60000);
    while (!result && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return result.value_or(Result{});
}

// Read, first write, conflict with a stale revision, write with the current one: the same on every storage.
static int exercise(Storage &s, const char *name)
{
    std::printf("%s\n", name);
    const auto missing = await([&](Done d) { s.read(d); });
    CHECK(missing.status == Result::NotFound)
    const auto first = await([&](Done d) { s.write("version 1", {}, d); });
    CHECK(first.status == Result::Ok && !first.revision.isEmpty())
    const auto again = await([&](Done d) { s.write("other device", {}, d); }); // "no file yet" is no longer true
    CHECK(again.status == Result::Conflict)
    const auto read = await([&](Done d) { s.read(d); });
    CHECK(read.status == Result::Ok && read.data == "version 1" && read.revision == first.revision)
    const auto second = await([&](Done d) { s.write("version 2", read.revision, d); });
    CHECK(second.status == Result::Ok && second.revision != first.revision)
    const auto stale = await([&](Done d) { s.write("version 3", read.revision, d); });
    CHECK(stale.status == Result::Conflict)
    const auto last = await([&](Done d) { s.read(d); });
    CHECK(last.status == Result::Ok && last.data == "version 2" && last.revision == second.revision)
    return 0;
}

// Minimal HTTP/1.1 server: one request per connection, handled by a callback.
struct HttpRequest
{
    QByteArray method, path, body;
    QMap<QByteArray, QByteArray> headers; // lower-case names
};
struct HttpReply
{
    int code = 200;
    QByteArray body;
    QList<std::pair<QByteArray, QByteArray>> headers;
};
class FakeHttp : public QTcpServer
{
public:
    std::function<HttpReply(const HttpRequest &)> handle;
    FakeHttp()
    {
        listen(QHostAddress::LocalHost, 0);
        connect(this, &QTcpServer::newConnection, this,
                [this]
                {
                    while (auto *socket = nextPendingConnection())
                        connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { serve(socket); });
                });
    }
    QString base() const { return QString("http://127.0.0.1:%1").arg(serverPort()); }

private:
    void serve(QTcpSocket *socket)
    {
        auto buffer = socket->property("buffer").toByteArray() + socket->readAll();
        socket->setProperty("buffer", buffer);
        const int end = buffer.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        HttpRequest request;
        const auto lines = buffer.left(end).split('\n');
        const auto first = lines[0].trimmed().split(' ');
        request.method = first.value(0);
        request.path = first.value(1);
        for (int i = 1; i < lines.size(); ++i)
        {
            const int colon = lines[i].indexOf(':');
            if (colon > 0)
                request.headers[lines[i].left(colon).trimmed().toLower()] = lines[i].mid(colon + 1).trimmed();
        }
        const int length = request.headers.value("content-length").toInt();
        if (buffer.size() < end + 4 + length)
            return;
        request.body = buffer.mid(end + 4, length);
        const auto reply = handle(request);
        QByteArray out = "HTTP/1.1 " + QByteArray::number(reply.code) + " X\r\nConnection: close\r\nContent-Length: " +
                         QByteArray::number(reply.body.size()) + "\r\n";
        for (const auto &[name, value] : reply.headers)
            out += name + ": " + value + "\r\n";
        socket->write(out + "\r\n" + reply.body);
        socket->disconnectFromHost();
    }
};
