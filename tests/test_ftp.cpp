// FTP: CrossTerm server on 127.0.0.1 with a temporary root, asynchronous client and panel.
// Login (wrong password rejected), MLSD and LIST listings, folders, 5 MB upload/download with progress,
// rename, recursive delete, attempts to escape the root, read-only anonymous access.
// Active mode (EPRT) and "bounce" rejection, resume of interrupted downloads/uploads, recursive folders
// (client and panel, Resume in the panel), explicit and implicit FTPS with a self-signed certificate.
#include "ftp.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QSslSocket>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>

static int failures = 0;

static void check(bool ok, const char *step)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAILED", step);
    if (!ok)
        ++failures;
}

template <typename F> static bool wait(F done, int ms = 20000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return done();
}

struct Result
{
    bool done = false, ok = false;
    QString error;
    QList<RemoteEntry> entries;
};

// Runs a client operation and waits for it to finish.
template <typename Start> static Result call(Start start)
{
    Result r;
    start(
        [&r](bool ok, const QString &error)
        {
            r.done = true;
            r.ok = ok;
            r.error = error;
        });
    wait([&] { return r.done; });
    return r;
}

static Result list(FtpClient &client, const QString &path)
{
    Result r;
    client.list(path,
                [&r](bool ok, const QString &error, const QList<RemoteEntry> &entries)
                {
                    r.done = true;
                    r.ok = ok;
                    r.error = error;
                    r.entries = entries;
                });
    wait([&] { return r.done; });
    return r;
}

static const RemoteEntry *entry(const Result &r, const QString &name)
{
    for (const auto &e : r.entries)
    {
        if (e.name == name)
            return &e;
    }
    return nullptr;
}

static bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

static QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir temp;
    const QString root = temp.filePath("root"), outside = temp.filePath("outside");
    QDir().mkpath(root + "/sub");
    QDir().mkpath(outside);
    writeFile(root + "/hello.txt", "hello ftp\n");
    writeFile(outside + "/secret.txt", "top secret");

    FtpServer server;
    QStringList serverLog;
    server.log = [&](const QString &line) { serverLog << line; };
    check(!server.start(temp.filePath("missing"), 0, false, "alice", "x"), "nonexistent root folder rejected");
    check(server.start(root, 0, false, "alice", "s3cret") && server.isRunning() && server.port() > 0, "server started");
    const quint16 port = server.port();

    {
        FtpClient bad;
        auto r = call([&](auto done) { bad.connectToHost("127.0.0.1", port, "alice", "wrong", done); });
        check(!r.ok && r.error.startsWith("530"), "wrong password rejected (530)");
        r = call([&](auto done) { bad.connectToHost("127.0.0.1", port, "", "", done); });
        check(!r.ok && r.error.startsWith("530"), "anonymous rejected when not enabled");
    }

    FtpClient client;
    QStringList clientLog;
    client.log = [&](const QString &line) { clientLog << line; };
    auto r = call([&](auto done) { client.connectToHost("127.0.0.1", port, "alice", "s3cret", done); });
    check(r.ok && client.isConnected(), "login");
    check(client.usesMlsd(), "FEAT offre MLSD");
    check(!clientLog.join('\n').contains("s3cret") && !serverLog.join('\n').contains("s3cret"), "password mai nel log");

    auto listed = list(client, "/");
    check(listed.ok && entry(listed, "hello.txt") && entry(listed, "hello.txt")->size == 10 && entry(listed, "sub") &&
              entry(listed, "sub")->dir,
          "MLSD listing of the root");
    client.preferMlsd = false;
    listed = list(client, "/");
    check(listed.ok && entry(listed, "hello.txt") && entry(listed, "hello.txt")->size == 10 && entry(listed, "sub") &&
              entry(listed, "sub")->dir && entry(listed, "sub")->access.startsWith('d'),
          "LIST listing (Unix format) of the root");
    client.preferMlsd = true;

    r = call([&](auto done) { client.mkdir("/newdir", done); });
    check(r.ok && QFileInfo(root + "/newdir").isDir(), "mkdir");
    r = call([&](auto done) { client.mkdir("/newdir", done); });
    check(!r.ok && r.error.startsWith("550"), "mkdir of an existing folder: error 550");

    QByteArray big(5 * 1024 * 1024, Qt::Uninitialized);
    QRandomGenerator(42).fillRange(reinterpret_cast<quint32 *>(big.data()), big.size() / 4);
    const QString bigLocal = temp.filePath("big.bin");
    writeFile(bigLocal, big);
    QList<qint64> upSteps;
    qint64 upTotal = 0;
    r = call(
        [&](auto done)
        {
            client.upload(
                bigLocal, "/newdir/big.bin",
                [&](qint64 n, qint64 total)
                {
                    upSteps << n;
                    upTotal = total;
                },
                done);
        });
    const bool upMiddle = std::any_of(upSteps.begin(), upSteps.end(), [&](qint64 n) { return n > 0 && n < big.size(); });
    check(r.ok && readFile(root + "/newdir/big.bin") == big, "5 MB upload");
    check(upTotal == big.size() && upMiddle && upSteps.last() == big.size(), "avanzamento upload tra 0 e 100%");

    const QString back = temp.filePath("back.bin");
    QList<qint64> downSteps;
    r = call([&](auto done) { client.download("/newdir/big.bin", back, [&](qint64 n, qint64) { downSteps << n; }, done); });
    const bool downMiddle = std::any_of(downSteps.begin(), downSteps.end(), [&](qint64 n) { return n > 0 && n < big.size(); });
    check(r.ok && readFile(back) == big, "5 MB download, identical bytes");
    check(downMiddle && downSteps.last() == big.size(), "avanzamento download tra 0 e 100%");

    r = call([&](auto done) { client.download("/nothing.bin", temp.filePath("nothing.bin"), {}, done); });
    check(!r.ok && r.error.startsWith("550") && !QFile::exists(temp.filePath("nothing.bin")),
          "download of a nonexistent file: 550, no local file");

    r = call([&](auto done) { client.rename("/newdir/big.bin", "/newdir/renamed.bin", done); });
    check(r.ok && QFile::exists(root + "/newdir/renamed.bin") && !QFile::exists(root + "/newdir/big.bin"), "rename");
    r = call([&](auto done) { client.removeFile("/newdir/renamed.bin", done); });
    check(r.ok && !QFile::exists(root + "/newdir/renamed.bin"), "file deletion");
    r = call([&](auto done) { client.removeDirectory("/newdir", false, done); });
    check(r.ok && !QFileInfo::exists(root + "/newdir"), "empty folder deletion");

    QDir().mkpath(root + "/tree/a/b");
    writeFile(root + "/tree/x.txt", "x");
    writeFile(root + "/tree/a/b/y.txt", "y");
    r = call([&](auto done) { client.removeDirectory("/tree", false, done); });
    check(!r.ok && QFileInfo::exists(root + "/tree"), "RMD of a non-empty folder rejected");
    r = call([&](auto done) { client.removeDirectory("/tree", true, done); });
    check(r.ok && !QFileInfo::exists(root + "/tree"), "recursive deletion");

    r = call([&](auto done) { client.cwd("/sub", done); });
    QString where;
    client.pwd([&](bool, const QString &, const QString &path) { where = path; });
    wait([&] { return !where.isEmpty(); });
    check(r.ok && where == "/sub", "cwd e pwd");
    r = call([&](auto done) { client.upload(temp.filePath("hello-local.txt"), "x.txt", {}, done); });
    check(!r.ok, "upload of a nonexistent local file: error");
    r = call([&](auto done) { client.mkdir("bad\r\nDELE /hello.txt", done); });
    check(!r.ok && QFile::exists(root + "/hello.txt"), "CR/LF in names rejected by the client");

    // Attempts to escape the root.
    r = call([&](auto done) { client.download("../../outside/secret.txt", temp.filePath("s1"), {}, done); });
    check(!r.ok && r.error.startsWith("550") && !QFile::exists(temp.filePath("s1")), "RETR with .. rejected");
    r = call([&](auto done) { client.download("/../outside/secret.txt", temp.filePath("s2"), {}, done); });
    check(!r.ok && r.error.startsWith("550"), "RETR /.. rejected");
    r = call([&](auto done) { client.download("..\\..\\outside\\secret.txt", temp.filePath("s3"), {}, done); });
    check(!r.ok && r.error.startsWith("550"), "RETR with backslash rejected");
    r = call([&](auto done)
             { client.download(QDir::fromNativeSeparators(outside) + "/secret.txt", temp.filePath("s4"), {}, done); });
    check(!r.ok && r.error.startsWith("550"), "RETR with absolute local path rejected");
    r = call([&](auto done) { client.cwd("/..", done); });
    check(!r.ok, "CWD above the root rejected");
    r = call([&](auto done) { client.upload(temp.filePath("big.bin"), "/../evil.bin", {}, done); });
    check(!r.ok && !QFile::exists(temp.filePath("evil.bin")), "STOR outside the root rejected");
    check(!list(client, "/../outside").ok, "LIST outside the root rejected");
    r = call([&](auto done) { client.rename("/hello.txt", "/../hello.txt", done); });
    check(!r.ok && QFile::exists(root + "/hello.txt"), "RNTO outside the root rejected");

    // Link pointing outside the root (a junction on Windows: needs no privileges).
#ifdef Q_OS_WIN
    QProcess::execute("cmd",
                      {"/c", "mklink", "/J", QDir::toNativeSeparators(root + "/escape"), QDir::toNativeSeparators(outside)});
#else
    QFile::link(outside, root + "/escape");
#endif
    if (QFileInfo(root + "/escape/secret.txt").exists())
    {
        r = call([&](auto done) { client.download("/escape/secret.txt", temp.filePath("s5"), {}, done); });
        check(!r.ok && !QFile::exists(temp.filePath("s5")), "RETR through an external link rejected");
        check(!list(client, "/escape").ok, "LIST through an external link rejected");
        r = call([&](auto done) { client.upload(bigLocal, "/escape/new.bin", {}, done); });
        check(!r.ok && !QFile::exists(outside + "/new.bin"), "STOR through an external link rejected");
        QDir().rmdir(root + "/escape"); // removes the junction, not its contents
        QFile::remove(root + "/escape");
    }
    else
        std::puts("link not created: link check skipped");

    client.disconnectFromHost();
    check(!client.isConnected(), "disconnessione");

    // Panel: first listing, navigation, upload from the desktop and rename.
    {
        int asked = 0;
        FtpPanel panel(QJsonObject{{"host", "127.0.0.1"}, {"port", int(port)}, {"user", "alice"}},
                       [&]
                       {
                           ++asked;
                           return QString("s3cret");
                       });
        panel.resize(400, 600);
        panel.show();
        auto names = [&]
        {
            QStringList list;
            for (const auto &e : panel.entries())
                list << e.name;
            return list;
        };
        check(wait([&] { return !panel.busy() && panel.currentPath() == "/"; }) && names().contains("hello.txt") &&
                  names().contains("sub"),
              "panel: root listing");
        check(asked == 1, "panel: password asked once");
        panel.browse("sub");
        check(wait([&] { return !panel.busy() && panel.currentPath() == "/sub"; }), "panel: enters a folder");
        panel.upload({bigLocal});
        check(wait([&] { return panel.transfersIdle() && !panel.busy() && names().contains("big.bin"); }) &&
                  panel.transferState(0) == "done" && panel.transferProgress(0) == 100 && readFile(root + "/sub/big.bin") == big,
              "panel: upload through the transfer queue");
        panel.renameEntry("big.bin", "moved.bin");
        check(wait([&] { return !panel.busy() && names().contains("moved.bin"); }), "panel: rename");
        panel.removeEntries({"moved.bin"});
        check(wait([&] { return !panel.busy() && !names().contains("moved.bin"); }) && !QFile::exists(root + "/sub/moved.bin"),
              "panel: deletion");
        panel.browse("/");
        check(wait([&] { return !panel.busy() && panel.currentPath() == "/"; }), "panel: back to the root");
        check(asked == 1, "panel: no new password prompt");
    }

    // Read-only anonymous access and passive ports from a range.
    server.stop();
    check(!server.isRunning(), "server stopped");
    server.passivePortMin = 50710;
    server.passivePortMax = 50740;
    check(server.start(root, 0, true), "anonymous server started");
    FtpClient anon;
    clientLog.clear();
    anon.log = [&](const QString &line) { clientLog << line; };
    r = call([&](auto done) { anon.connectToHost("127.0.0.1", server.port(), "", "", done); });
    check(r.ok, "login anonimo");
    listed = list(anon, "/");
    check(listed.ok && entry(listed, "hello.txt"), "anonymous listing");
    static const QRegularExpression epsv("\\(\\|\\|\\|(\\d+)\\|\\)");
    const auto m = epsv.match(clientLog.join('\n'));
    check(m.hasMatch() && m.captured(1).toInt() >= 50710 && m.captured(1).toInt() <= 50740,
          "passive port within the configured range");
    r = call([&](auto done) { anon.download("/hello.txt", temp.filePath("anon.txt"), {}, done); });
    check(r.ok && readFile(temp.filePath("anon.txt")) == "hello ftp\n", "download anonimo");
    r = call([&](auto done) { anon.upload(bigLocal, "/anon.bin", {}, done); });
    check(!r.ok && r.error.startsWith("550") && !QFile::exists(root + "/anon.bin"), "anonymous upload rejected");
    r = call([&](auto done) { anon.removeFile("/hello.txt", done); });
    check(!r.ok && QFile::exists(root + "/hello.txt"), "anonymous deletion rejected");

    // ------------------------------------------------ active mode, resume, folders, FTPS
    server.stop();
    server.passivePortMin = server.passivePortMax = 0;
    check(server.start(root, 0, false, "alice", "s3cret"), "server restarted with a user");
    const quint16 port2 = server.port();
    auto login = [&](FtpClient &c, quint16 p)
    { return call([&](auto done) { c.connectToHost("127.0.0.1", p, "alice", "s3cret", done); }); };

    {
        FtpClient active;
        QStringList log;
        active.log = [&](const QString &line) { log << line; };
        active.passive = false;
        r = login(active, port2);
        listed = list(active, "/");
        check(r.ok && listed.ok && entry(listed, "hello.txt") && log.join('\n').contains("> EPRT |1|127.0.0.1|"),
              "active mode: EPRT and listing");
        r = call([&](auto done) { active.upload(bigLocal, "/active.bin", {}, done); });
        check(r.ok && readFile(root + "/active.bin") == big, "active mode: 5 MB upload");
        r = call([&](auto done) { active.download("/active.bin", temp.filePath("active-back.bin"), {}, done); });
        check(r.ok && readFile(temp.filePath("active-back.bin")) == big, "active mode: 5 MB download");
        QFile::remove(root + "/active.bin");
    }

    {
        // Raw commands: no "FTP bounce" to other hosts or privileged ports.
        QTcpSocket raw;
        QByteArray buffer;
        auto ask = [&](const QString &line)
        {
            if (!line.isEmpty())
                raw.write(line.toUtf8() + "\r\n");
            wait(
                [&]
                {
                    buffer += raw.readAll();
                    return buffer.contains('\n');
                },
                5000);
            const int nl = int(buffer.indexOf('\n'));
            const auto reply = QString::fromUtf8(buffer.left(nl)).trimmed();
            buffer.remove(0, nl + 1);
            return reply;
        };
        raw.connectToHost("127.0.0.1", port2);
        check(ask({}).startsWith("220") && ask("USER alice").startsWith("331") && ask("PASS s3cret").startsWith("230"),
              "connessione grezza autenticata");
        check(ask("PORT 10,1,2,3,19,136").startsWith("500"), "PORT to another IP rejected (bounce)");
        check(ask("EPRT |1|10.1.2.3|5000|").startsWith("500"), "EPRT to another IP rejected");
        check(ask("PORT 127,0,0,1,0,25").startsWith("500"), "PORT to a privileged port rejected");
        check(ask("PORT 127,0,0,1,19,136").startsWith("200"), "PORT to the client accepted");
        check(ask("REST 10").startsWith("350"), "REST accepted");
    }

    {
        FtpClient rc;
        QStringList log;
        rc.log = [&](const QString &line) { log << line; };
        writeFile(root + "/resume.bin", big);
        const QString local = temp.filePath("resume.bin"), part = local + ".part";
        bool stopped = false;
        login(rc, port2);
        r = call(
            [&](auto done)
            {
                rc.download(
                    "/resume.bin", local,
                    [&](qint64 n, qint64)
                    {
                        if (!stopped && n >= (1 << 20))
                        {
                            stopped = true;
                            rc.abort();
                        }
                    },
                    done);
            });
        const qint64 partial = QFileInfo(part).size();
        check(!r.ok && stopped && partial > 0 && partial < big.size() && !QFile::exists(local),
              "interrupted download: the .part file remains");
        login(rc, port2);
        qint64 first = -1;
        r = call(
            [&](auto done)
            {
                rc.download(
                    "/resume.bin", local,
                    [&](qint64 n, qint64)
                    {
                        if (first < 0)
                            first = n;
                    },
                    done, FtpClient::Resume);
            });
        check(r.ok && readFile(local) == big && !QFile::exists(part) && first == partial &&
                  log.join('\n').contains("> REST " + QString::number(partial)),
              "download resumed with REST, identical bytes");

        stopped = false;
        r = call(
            [&](auto done)
            {
                rc.upload(
                    bigLocal, "/resume-up.bin",
                    [&](qint64 n, qint64)
                    {
                        if (!stopped && n >= (2 << 20))
                        {
                            stopped = true;
                            rc.abort();
                        }
                    },
                    done);
            });
        wait([&] { return QFileInfo(root + "/resume-up.bin").size() > 0; }, 5000);
        login(rc, port2); // meanwhile the server closes the partial file
        const qint64 have = QFileInfo(root + "/resume-up.bin").size();
        check(!r.ok && stopped && have > 0 && have < big.size(), "upload interrotto: file remoto parziale");
        r = call([&](auto done) { rc.upload(bigLocal, "/resume-up.bin", {}, done, FtpClient::Resume); });
        check(r.ok && readFile(root + "/resume-up.bin") == big && log.join('\n').contains("> REST " + QString::number(have)),
              "upload resumed with REST+STOR, identical bytes");
        r = call([&](auto done) { rc.upload(bigLocal, "/resume-up.bin", {}, done, FtpClient::Resume); });
        check(r.ok && readFile(root + "/resume-up.bin") == big, "upload already complete: no transfer");
        QFile::remove(root + "/resume.bin");
        QFile::remove(root + "/resume-up.bin");
    }

    // 3-level local tree with files at every level and an empty folder.
    const QString tree = temp.filePath("tree");
    const QByteArray deep = big.left(300000);
    QDir().mkpath(tree + "/a/b");
    QDir().mkpath(tree + "/empty");
    writeFile(tree + "/top.txt", "top");
    writeFile(tree + "/a/mid.txt", "middle");
    writeFile(tree + "/a/b/deep.bin", deep);
    auto sameTree = [&](const QString &dir)
    {
        return readFile(dir + "/top.txt") == "top" && readFile(dir + "/a/mid.txt") == "middle" &&
               readFile(dir + "/a/b/deep.bin") == deep && QFileInfo(dir + "/empty").isDir();
    };
    {
        FtpClient tc;
        login(tc, port2);
        qint64 lastDone = -1, lastTotal = -1;
        auto track = [&](qint64 n, qint64 total)
        {
            lastDone = n;
            lastTotal = total;
        };
        r = call([&](auto done) { tc.upload(tree, "/tree-up", track, done, FtpClient::Recursive); });
        check(r.ok && sameTree(root + "/tree-up"), "3-level recursive upload");
        check(lastTotal == 3 + 6 + deep.size() && lastDone == lastTotal, "upload ricorsivo: avanzamento aggregato");
        lastDone = lastTotal = -1;
        r = call([&](auto done) { tc.download("/tree-up", temp.filePath("tree-down"), track, done, FtpClient::Recursive); });
        check(r.ok && sameTree(temp.filePath("tree-down")), "3-level recursive download");
        check(lastTotal == 3 + 6 + deep.size() && lastDone == lastTotal, "download ricorsivo: avanzamento aggregato");
        r = call([&](auto done) { tc.removeDirectory("/tree-up", true, done); });
    }

    {
        FtpPanel panel(QJsonObject{{"host", "127.0.0.1"}, {"port", int(port2)}, {"user", "alice"}, {"ftpPassive", false}},
                       [] { return QString("s3cret"); });
        panel.show();
        auto names = [&]
        {
            QStringList list;
            for (const auto &e : panel.entries())
                list << e.name;
            return list;
        };
        check(wait([&] { return !panel.busy() && panel.currentPath() == "/"; }), "panel in active mode: listing");
        panel.upload({tree});
        check(wait([&] { return panel.transfersIdle() && !panel.busy() && names().contains("tree"); }) &&
                  panel.transferState(0) == "done" && sameTree(root + "/tree"),
              "panel: folder upload");
        panel.download({"tree"}, temp.filePath("panel-down"));
        check(wait([&] { return panel.transfersIdle() && panel.transferState(1) == "done"; }) &&
                  sameTree(temp.filePath("panel-down/tree")),
              "panel: folder download");
        // Transfer fails with the server stopped, then resumed with Resume after restarting on the same port.
        server.stop();
        wait([&] { return !panel.client().isConnected(); }, 5000);
        panel.download({"hello.txt"}, temp.filePath("panel-down"));
        check(wait([&] { return panel.transferState(2) == "failed"; }), "panel: download failed with the server stopped");
        check(server.start(root, port2, false, "alice", "s3cret"), "server restarted on the same port");
        panel.resumeTransfer(2);
        check(wait([&] { return panel.transferState(2) == "done"; }) &&
                  readFile(temp.filePath("panel-down/hello.txt")) == "hello ftp\n",
              "panel: Resume completes the transfer");
    }

    // FTPS with a self-signed certificate generated on the fly.
    const QString openssl = "C:/Program Files/Git/mingw64/bin/openssl.exe";
    QTemporaryDir certDir;
    const QString certFile = certDir.filePath("cert.pem"), keyFile = certDir.filePath("key.pem");
    QProcess gen;
    gen.setProcessChannelMode(QProcess::MergedChannels);
    if (QSslSocket::supportsSsl() && QFileInfo::exists(openssl))
    {
        gen.start(openssl, {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=127.0.0.1", "-keyout",
                            keyFile, "-out", certFile});
        gen.waitForFinished(60000);
    }
    if (!QFileInfo::exists(certFile) || !QFileInfo::exists(keyFile))
        std::puts("SKIPPED: openssl or TLS backend not available, FTPS tests skipped");
    else
    {
        const auto expected = FtpClient::fingerprint(QSslCertificate::fromPath(certFile).value(0));
        FtpServer tlsServer;
        tlsServer.certificateFile = certFile;
        tlsServer.privateKeyFile = keyFile;
        check(tlsServer.start(root, 0, false, "alice", "s3cret"), "explicit FTPS server started");
        {
            FtpClient untrusted;
            untrusted.security = "explicit";
            r = login(untrusted, tlsServer.port());
            check(!r.ok && !expected.isEmpty() && r.error.contains(expected),
                  "explicit FTPS: self-signed certificate rejected without callback, error with fingerprint");
            untrusted.trustCertificate = [](const QSslCertificate &) { return false; };
            r = login(untrusted, tlsServer.port());
            check(!r.ok && r.error.contains(expected), "FTPS esplicito: rifiutato se il callback dice no");
        }
        FtpClient secure;
        QStringList log;
        secure.log = [&](const QString &line) { log << line; };
        secure.security = "explicit";
        int asked = 0;
        secure.trustCertificate = [&](const QSslCertificate &c)
        {
            ++asked;
            return FtpClient::fingerprint(c) == expected;
        };
        r = login(secure, tlsServer.port());
        check(r.ok && asked == 1 && log.join('\n').contains("> AUTH TLS") && log.join('\n').contains("> PROT P"),
              "explicit FTPS: login after AUTH TLS, PBSZ, PROT P");
        listed = list(secure, "/");
        check(listed.ok && entry(listed, "hello.txt"), "explicit FTPS: listing over TLS data channel");
        r = call([&](auto done) { secure.upload(bigLocal, "/tls.bin", {}, done); });
        check(r.ok && readFile(root + "/tls.bin") == big, "explicit FTPS: 5 MB upload");
        r = call([&](auto done) { secure.download("/tls.bin", temp.filePath("tls-back.bin"), {}, done); });
        check(r.ok && readFile(temp.filePath("tls-back.bin")) == big, "explicit FTPS: 5 MB download");
        secure.passive = false;
        listed = list(secure, "/");
        r = call([&](auto done) { secure.download("/hello.txt", temp.filePath("tls-active.txt"), {}, done); });
        check(listed.ok && r.ok && readFile(temp.filePath("tls-active.txt")) == "hello ftp\n",
              "explicit FTPS in active mode: listing and download");
        secure.disconnectFromHost();
        r = login(secure, tlsServer.port());
        check(r.ok && asked == 1, "FTPS: certificate remembered for the host, no new prompt");
        QFile::remove(root + "/tls.bin");

        FtpServer implicitServer;
        implicitServer.certificateFile = certFile;
        implicitServer.privateKeyFile = keyFile;
        implicitServer.implicitTls = true;
        check(implicitServer.start(root, 0, false, "alice", "s3cret"), "implicit FTPS server started");
        {
            FtpClient untrusted;
            untrusted.security = "implicit";
            r = login(untrusted, implicitServer.port());
            check(!r.ok && r.error.contains(expected), "implicit FTPS: certificate rejected without callback");
        }
        FtpClient imp;
        imp.security = "implicit";
        imp.trustCertificate = [&](const QSslCertificate &c) { return FtpClient::fingerprint(c) == expected; };
        r = login(imp, implicitServer.port());
        listed = list(imp, "/");
        check(r.ok && listed.ok && entry(listed, "hello.txt"), "implicit FTPS: login and listing");
        r = call([&](auto done) { imp.upload(bigLocal, "/implicit.bin", {}, done); });
        check(r.ok && readFile(root + "/implicit.bin") == big, "implicit FTPS: 5 MB upload");
        r = call([&](auto done) { imp.download("/implicit.bin", temp.filePath("implicit-back.bin"), {}, done); });
        check(r.ok && readFile(temp.filePath("implicit-back.bin")) == big, "implicit FTPS: 5 MB download");
        QFile::remove(root + "/implicit.bin");
        FtpServer noKey;
        noKey.implicitTls = true;
        check(!noKey.start(root, 0, false, "alice", "s3cret"), "implicit FTPS without certificate rejected");
    }

    std::printf("%s\n", failures ? "test_ftp: errori" : "test_ftp: ok");
    return failures ? 1 : 0;
}
