// Sync file storages: settings checks, then read / conditional write / conflict on each of them. WebDAV and Gist run
// against a small local HTTP server that behaves like the real ones (ETags, If-Match, Gist versions and truncated
// content); FTPS against CrossTerm's own FTP server with TLS; SFTP against the test SSH server when it is there
// (127.0.0.1:2222, user "test", key in $CROSSTERM_TEST_DATA/srv/client_key).
#include "ftp.h"
#include "syncstorage.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSslCertificate>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <cstdio>

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

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // --- Settings that are refused.
    QString error;
    CHECK(!create(QJsonObject{{"type", "nothing"}}, &error) && !error.isEmpty())
    CHECK(!create(QJsonObject{{"type", "webdav"}, {"url", "http://example.com/x"}}, &error)) // password in clear text
    CHECK(create(QJsonObject{{"type", "webdav"}, {"url", "https://example.com/x"}}))
    CHECK(!create(QJsonObject{{"type", "ftps"}, {"host", "h"}, {"path", "p"}, {"security", "none"}}, &error))
    CHECK(!create(QJsonObject{{"type", "gist"}, {"token", "t"}, {"api", "https://evil.example"}}, &error)) // token stays at GitHub
    CHECK(!create(QJsonObject{{"type", "sftp"}, {"path", "x"}}, &error))
    CHECK(contentRevision("abc").startsWith("sha256:") && contentRevision("abc").size() == 7 + 64)

    // --- Folder of a sync client; a folder path gets the file name, a missing folder is an error.
    {
        QTemporaryDir dir;
        auto folder = create(QJsonObject{{"type", "folder"}, {"path", dir.path()}});
        CHECK(folder)
        CHECK(exercise(*folder, "folder") == 0)
        CHECK(QFile::exists(dir.filePath(FileName)))
        auto missing = create(QJsonObject{{"type", "folder"}, {"path", dir.filePath("no/such/file")}});
        CHECK(await([&](Done d) { missing->write("x", {}, d); }).status == Result::Failed)
    }

    // --- WebDAV: ETags and If-Match / If-None-Match, Basic authentication; then a server without ETags.
    {
        FakeHttp server;
        QByteArray stored;
        int version = 0;
        bool etags = true;
        server.handle = [&](const HttpRequest &r) -> HttpReply
        {
            if (r.headers.value("authorization") != "Basic " + QByteArray("me:secret").toBase64())
                return {401, {}, {}};
            const QByteArray etag = "\"v" + QByteArray::number(version) + "\"";
            const auto tag = [&](HttpReply reply)
            {
                if (etags)
                    reply.headers << std::pair<QByteArray, QByteArray>{"ETag", "\"v" + QByteArray::number(version) + "\""};
                return reply;
            };
            if (r.method == "GET")
                return version ? tag({200, stored, {}}) : HttpReply{404, {}, {}};
            if (r.method == "PUT")
            {
                if ((r.headers.contains("if-none-match") && version) ||
                    (r.headers.contains("if-match") && r.headers.value("if-match") != etag))
                    return {412, {}, {}};
                stored = r.body;
                ++version;
                return tag({version == 1 ? 201 : 204, {}, {}});
            }
            return {405, {}, {}};
        };
        auto wrong = create(QJsonObject{{"type", "webdav"}, {"url", server.base() + "/dav/f"}, {"user", "me"}, {"password", "no"}});
        const auto denied = await([&](Done d) { wrong->read(d); });
        CHECK(denied.status == Result::Failed && denied.error.contains("Access denied"))
        auto dav = create(QJsonObject{{"type", "webdav"}, {"url", server.base() + "/dav/f"}, {"user", "me"}, {"password", "secret"}});
        CHECK(exercise(*dav, "webdav") == 0)
        CHECK(stored == "version 2")
        stored.clear();
        version = 0;
        etags = false; // the client falls back to comparing the content
        CHECK(exercise(*dav, "webdav without ETags") == 0)
    }

    // --- GitHub Gist: created by the first write (its id comes back to be saved), versions, truncated content.
    {
        FakeHttp server;
        QByteArray content;
        int version = 0;
        bool exists = false, truncate = false;
        server.handle = [&](const HttpRequest &r) -> HttpReply
        {
            if (r.headers.value("authorization") != "Bearer tok")
                return {401, {}, {}};
            const auto gist = [&]
            {
                QJsonObject file{{"content", QString::fromUtf8(truncate ? content.left(3) : content)},
                                 {"truncated", truncate},
                                 {"raw_url", server.base() + "/raw/x"}};
                return QJsonDocument(QJsonObject{{"id", "g1"},
                                                 {"files", QJsonObject{{FileName, file}}},
                                                 {"history", QJsonArray{QJsonObject{{"version", QString("v%1").arg(version)}}}}})
                    .toJson();
            };
            if (r.path == "/raw/x")
                return {200, content, {}};
            if (r.method == "POST" && r.path == "/gists")
            {
                const auto body = QJsonDocument::fromJson(r.body).object();
                if (body["public"].toBool())
                    return {422, {}, {}};
                content = body["files"].toObject()[FileName].toObject()["content"].toString().toUtf8();
                exists = true;
                ++version;
                return {201, gist(), {}};
            }
            if (r.path != "/gists/g1" || !exists)
                return {404, {}, {}};
            if (r.method == "PATCH")
            {
                content = QJsonDocument::fromJson(r.body).object()["files"].toObject()[FileName].toObject()["content"].toString().toUtf8();
                ++version;
            }
            return {200, gist(), {}};
        };
        auto gist = create(QJsonObject{{"type", "gist"}, {"token", "tok"}, {"api", server.base()}});
        CHECK(await([&](Done d) { gist->read(d); }).status == Result::NotFound)
        const auto created = await([&](Done d) { gist->write("version 1", {}, d); });
        CHECK(created.status == Result::Ok && created.updated["gist"] == "g1")
        // From here as a device that saved the id.
        gist = create(QJsonObject{{"type", "gist"}, {"token", "tok"}, {"api", server.base()}, {"gist", "g1"}});
        const auto read = await([&](Done d) { gist->read(d); });
        CHECK(read.status == Result::Ok && read.data == "version 1" && read.revision == created.revision)
        const auto second = await([&](Done d) { gist->write("version 2", read.revision, d); });
        CHECK(second.status == Result::Ok && second.updated.isEmpty())
        CHECK(await([&](Done d) { gist->write("version 3", read.revision, d); }).status == Result::Conflict)
        truncate = true; // over 1 MB GitHub truncates "content": the file comes from raw_url
        const auto whole = await([&](Done d) { gist->read(d); });
        CHECK(whole.status == Result::Ok && whole.data == "version 2")
        auto bad = create(QJsonObject{{"type", "gist"}, {"token", "wrong"}, {"api", server.base()}, {"gist", "g1"}});
        CHECK(await([&](Done d) { bad->read(d); }).status == Result::Failed)
    }

    // --- FTPS against CrossTerm's FTP server with a self-signed certificate: refused until its fingerprint is
    // trusted, then the usual cycle.
    {
        QString openssl = QStandardPaths::findExecutable("openssl");
        if (openssl.isEmpty() && QFileInfo::exists("C:/Program Files/Git/mingw64/bin/openssl.exe"))
            openssl = "C:/Program Files/Git/mingw64/bin/openssl.exe";
        QTemporaryDir certDir, root;
        const QString cert = certDir.filePath("cert.pem"), key = certDir.filePath("key.pem");
        if (QSslSocket::supportsSsl() && !openssl.isEmpty())
        {
            QProcess gen;
            gen.start(openssl, {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=127.0.0.1",
                                "-keyout", key, "-out", cert});
            gen.waitForFinished(60000);
        }
        if (!QFile::exists(cert) || !QFile::exists(key))
            std::puts("SKIPPED: FTPS (openssl or TLS backend not available)");
        else
        {
            FtpServer server;
            server.certificateFile = cert;
            server.privateKeyFile = key;
            CHECK(server.start(root.path(), 0, false, "alice", "pw"))
            QJsonObject settings{{"type", "ftps"},  {"host", "127.0.0.1"},       {"port", int(server.port())},
                                 {"user", "alice"}, {"password", "pw"},          {"security", "explicit"},
                                 {"path", "/sync.ctvault"}};
            auto untrusted = create(settings);
            const auto refused = await([&](Done d) { untrusted->read(d); });
            const auto expected = FtpClient::fingerprint(QSslCertificate::fromPath(cert).value(0));
            CHECK(refused.status == Result::Failed && refused.fingerprint == expected)
            settings["fingerprint"] = expected;
            auto ftps = create(settings);
            CHECK(exercise(*ftps, "ftps") == 0)
            QFile onServer(root.filePath("sync.ctvault"));
            CHECK(onServer.open(QIODevice::ReadOnly) && onServer.readAll() == "version 2")
            CHECK(!QFile::exists(root.filePath("sync.ctvault.part")))
        }
    }

    // --- SFTP through the OpenSSH client, against the test server when available.
    {
        const QString key = qEnvironmentVariable("CROSSTERM_TEST_DATA") + "/srv/client_key";
        QTcpSocket probe;
        probe.connectToHost("127.0.0.1", 2222);
        if (!probe.waitForConnected(1500) || !QFile::exists(key) || QStandardPaths::findExecutable("sftp").isEmpty())
            std::puts("SKIPPED: SFTP (127.0.0.1:2222, CROSSTERM_TEST_DATA or sftp not available)");
        else
        {
            const QString path = QString("crossterm-sync-test-%1.ctvault").arg(QCoreApplication::applicationPid());
            const QJsonObject session{{"protocol", "ssh"}, {"host", "127.0.0.1"}, {"port", 2222}, {"user", "test"}, {"identity", key}};
            auto sftp = create(QJsonObject{{"type", "sftp"}, {"session", session}, {"path", path}});
            CHECK(exercise(*sftp, "sftp") == 0)
        }
    }
    return 0;
}
