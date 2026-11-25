#include "syncstorage.h"
#include "askpass.h"
#include "credentials.h"
#include "ftp.h"
#include "profiles.h"
#include "syncoauth.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QUrlQuery>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSaveFile>
#include <QSslCertificate>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

namespace SyncStorage
{
namespace
{
QString ui(const char *text)
{
    return QCoreApplication::translate("CrossTerm", text);
}

Result failed(const QString &error)
{
    Result r;
    r.error = error;
    return r;
}

Result withStatus(Result::Status status)
{
    Result r;
    r.status = status;
    return r;
}

bool localHost(const QUrl &url)
{
    const auto host = url.host();
    return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

QString platformName()
{
#ifdef Q_OS_WIN
    return "windows";
#elif defined(Q_OS_MACOS)
    return "macos";
#else
    return "linux";
#endif
}

// Storages without versions of their own: the revision is the content hash, checked again right before writing.
class HashStorage : public Storage
{
protected:
    virtual void fetch(Done done) = 0;                         // Ok with data, NotFound or Failed
    virtual void store(const QByteArray &data, Done done) = 0; // Ok or Failed

public:
    void read(Done done) override
    {
        fetch(
            [done](Result r)
            {
                if (r.status == Result::Ok)
                    r.revision = contentRevision(r.data);
                done(r);
            });
    }
    void write(const QByteArray &data, const QString &revision, Done done) override
    {
        fetch(
            [this, data, revision, done](const Result &current)
            {
                if (current.status == Result::Failed)
                    return done(current);
                const bool unchanged = current.status == Result::NotFound ? revision.isEmpty()
                                                                         : contentRevision(current.data) == revision;
                if (!unchanged)
                    return done(withStatus(Result::Conflict));
                store(data,
                      [data, done](Result r)
                      {
                          if (r.status == Result::Ok)
                              r.revision = contentRevision(data);
                          done(r);
                      });
            });
    }
};

// ---------------------------------------------------------------- folder of a sync client
class Folder : public HashStorage
{
    QString file;

public:
    explicit Folder(const QString &path)
        : file(QFileInfo(path).isDir() ? QDir(path).filePath(FileName) : path)
    {
    }

protected:
    void fetch(Done done) override
    {
        QFile f(file);
        // iCloud Drive with "Optimize Mac Storage" (macOS 13 and earlier) leaves ".<name>.icloud" in place of a file
        // not downloaded: the file is there, so it must not look absent (a new sync would replace the real one).
        const QFileInfo info(file);
        if (!f.exists() && QFileInfo::exists(info.dir().filePath("." + info.fileName() + ".icloud")))
            return done(failed(ui("Cannot read %1: %2").arg(QDir::toNativeSeparators(file), "not downloaded from iCloud yet")));
        if (!f.exists())
            return done(withStatus(Result::NotFound));
        if (!f.open(QIODevice::ReadOnly))
            return done(failed(ui("Cannot read %1: %2").arg(QDir::toNativeSeparators(file), f.errorString())));
        Result r = withStatus(Result::Ok);
        r.data = f.readAll();
        f.close(); // done() may write it: Windows cannot replace a file that is still open
        done(r);
    }
    void store(const QByteArray &data, Done done) override
    {
        if (!QFileInfo(QFileInfo(file).absolutePath()).isDir())
            return done(failed(ui("The folder %1 does not exist.").arg(QDir::toNativeSeparators(QFileInfo(file).absolutePath()))));
        QSaveFile f(file); // written beside and renamed: the sync client never uploads half a file
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
            return done(failed(ui("Cannot write %1: %2").arg(QDir::toNativeSeparators(file), f.errorString())));
        done(withStatus(Result::Ok));
    }
};

// ---------------------------------------------------------------- HTTP: WebDAV and GitHub Gist
QString httpError(QNetworkReply *reply)
{
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == 401 || code == 403)
        return ui("Access denied: check the user name, password or token.");
    if (code)
        return QString("HTTP %1 %2").arg(code).arg(reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString());
    return reply->errorString();
}

class Http : public Storage
{
protected:
    QObject context; // replies and their callbacks end with the storage
    QNetworkAccessManager *network = new QNetworkAccessManager(&context);

    // One request; done(reply) runs once, with the reply deleted afterwards.
    void send(QNetworkRequest request, const QByteArray &verb, const QByteArray &body,
              std::function<void(QNetworkReply *)> done)
    {
        request.setTransferTimeout(30000);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        auto *reply = network->sendCustomRequest(request, verb, body);
        QObject::connect(reply, &QNetworkReply::finished, &context,
                         [reply, done]
                         {
                             reply->deleteLater();
                             done(reply);
                         });
    }
    static int status(QNetworkReply *reply) { return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); }
};

class WebDav : public Http
{
    QUrl url;
    QByteArray authorization;

    QNetworkRequest request() const
    {
        QNetworkRequest r(url);
        if (!authorization.isEmpty())
            r.setRawHeader("Authorization", authorization);
        r.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
        return r;
    }
    static QString revisionOf(QNetworkReply *reply, const QByteArray &data)
    {
        const auto etag = reply->rawHeader("ETag");
        return etag.isEmpty() ? contentRevision(data) : "etag:" + QString::fromLatin1(etag);
    }

public:
    WebDav(const QUrl &url, const QString &user, const QString &password) : url(url)
    {
        if (!user.isEmpty())
            authorization = "Basic " + (user + ":" + password).toUtf8().toBase64();
    }
    void read(Done done) override
    {
        send(request(), "GET", {},
             [done](QNetworkReply *reply)
             {
                 if (status(reply) == 404)
                     return done(withStatus(Result::NotFound));
                 if (reply->error() != QNetworkReply::NoError)
                     return done(failed(httpError(reply)));
                 Result r = withStatus(Result::Ok);
                 r.data = reply->readAll();
                 r.revision = revisionOf(reply, r.data);
                 done(r);
             });
    }
    void write(const QByteArray &data, const QString &revision, Done done) override
    {
        const auto put = [this, data, done](QNetworkRequest request)
        {
            send(request, "PUT", data,
                 [data, done](QNetworkReply *reply)
                 {
                     if (status(reply) == 412)
                         return done(withStatus(Result::Conflict));
                     if (reply->error() != QNetworkReply::NoError)
                         return done(failed(httpError(reply)));
                     Result r = withStatus(Result::Ok);
                     r.revision = revisionOf(reply, data);
                     done(r);
                 });
        };
        auto conditional = request();
        if (revision.startsWith("etag:"))
            conditional.setRawHeader("If-Match", revision.mid(5).toLatin1());
        else if (revision.isEmpty())
            conditional.setRawHeader("If-None-Match", "*");
        else // a server without ETags: compare the content right before writing
            return read(
                [put, revision, done, plain = request()](const Result &current)
                {
                    if (current.status == Result::Failed)
                        return done(current);
                    if (current.status != Result::Ok || contentRevision(current.data) != revision)
                        return done(withStatus(Result::Conflict));
                    put(plain);
                });
        put(conditional);
    }
};

class Gist : public Http
{
    QString api, token, id;

    QNetworkRequest request(const QString &path) const
    {
        QNetworkRequest r(QUrl(api + path));
        r.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        r.setRawHeader("Accept", "application/vnd.github+json");
        r.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
        r.setRawHeader("User-Agent", "CrossTerm");
        r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        return r;
    }
    // Version id of the gist; empty for an answer without history (never trusted to have one).
    static QString revisionOf(const QJsonObject &gist)
    {
        const auto history = gist["history"].toArray();
        const auto version = history.isEmpty() ? QString() : history[0].toObject()["version"].toString();
        return version.isEmpty() ? QString() : "gist:" + version;
    }
    // The gist and the sync file in it (NotFound: no gist yet, or no such file in it).
    void fetchGist(std::function<void(const Result &, const QJsonObject &gist)> done)
    {
        if (id.isEmpty())
            return done(withStatus(Result::NotFound), {});
        send(request("/gists/" + id), "GET", {},
             [this, done](QNetworkReply *reply)
             {
                 if (status(reply) == 404)
                     return done(withStatus(Result::NotFound), {});
                 if (reply->error() != QNetworkReply::NoError)
                     return done(failed(httpError(reply)), {});
                 const auto gist = QJsonDocument::fromJson(reply->readAll()).object();
                 const auto file = gist["files"].toObject()[FileName].toObject();
                 if (file.isEmpty())
                     return done(withStatus(Result::NotFound), gist);
                 Result r = withStatus(Result::Ok);
                 r.revision = revisionOf(gist);
                 if (!file["truncated"].toBool())
                 {
                     r.data = file["content"].toString().toUtf8();
                     return done(r, gist);
                 }
                 // Over 1 MB the API truncates the content: the whole file comes from its raw URL.
                 auto raw = request({});
                 raw.setUrl(QUrl(file["raw_url"].toString()));
                 send(raw, "GET", {},
                      [r, gist, done](QNetworkReply *rawReply) mutable
                      {
                          if (rawReply->error() != QNetworkReply::NoError)
                              return done(failed(httpError(rawReply)), gist);
                          r.data = rawReply->readAll();
                          done(r, gist);
                      });
             });
    }

public:
    Gist(const QString &api, const QString &token, const QString &id) : api(api), token(token), id(id) {}
    void read(Done done) override
    {
        fetchGist([done](const Result &r, const QJsonObject &) { done(r); });
    }
    void write(const QByteArray &data, const QString &revision, Done done) override
    {
        const QJsonObject files{{FileName, QJsonObject{{"content", QString::fromUtf8(data)}}}};
        const auto answer = [this, done](QNetworkReply *reply)
        {
            if (reply->error() != QNetworkReply::NoError)
                return done(failed(httpError(reply)));
            const auto gist = QJsonDocument::fromJson(reply->readAll()).object();
            Result r = withStatus(Result::Ok);
            r.revision = revisionOf(gist);
            if (gist["id"].toString() != id)
            {
                id = gist["id"].toString();
                r.updated = QJsonObject{{"gist", id}};
            }
            done(r);
        };
        fetchGist(
            [this, files, revision, answer, done](const Result &current, const QJsonObject &gist)
            {
                if (current.status == Result::Failed)
                    return done(current);
                if ((current.status == Result::Ok ? current.revision : QString()) != revision)
                    return done(withStatus(Result::Conflict));
                const auto body = [&](QJsonObject o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); };
                if (gist.isEmpty()) // first write: a new secret gist
                    send(request("/gists"), "POST",
                         body({{"description", "CrossTerm sync (end-to-end encrypted)"}, {"public", false}, {"files", files}}),
                         answer);
                else
                    send(request("/gists/" + id), "PATCH", body({{"files", files}}), answer);
            });
    }
};

// ---------------------------------------------------------------- SFTP through the OpenSSH client
class Sftp : public HashStorage
{
    QObject context;
    QJsonObject profile;
    QString path;

    // Runs one sftp batch; done(ok, output) with output the messages of sftp and ssh.
    void run(const QStringList &lines, std::function<void(bool, const QString &)> done)
    {
        const auto command = sessionCommand(profile, platformName(), false);
        const auto program = QStandardPaths::findExecutable(command.program);
        if (!command.error.isEmpty() || program.isEmpty())
            return done(false, !command.error.isEmpty() ? describe(command.error) : ui("OpenSSH sftp client not found in PATH."));
        auto *process = new QProcess(&context);
        hideConsoleWindow(process);
        auto environment = QProcessEnvironment::systemEnvironment();
        // Saved password or key only: a synchronization never asks.
        for (const auto &entry : askpassEnvironment(credentialTarget(profile, QCoreApplication::applicationName()), false,
                                                    {}, profile["jumps"].toArray()))
            environment.insert(entry.section('=', 0, 0), entry.section('=', 1));
        process->setProcessEnvironment(environment);
        process->setProcessChannelMode(QProcess::MergedChannels);
        const auto batch = (lines.join('\n') + '\n').toUtf8();
        auto *timer = new QTimer(process);
        timer->setSingleShot(true);
        QObject::connect(timer, &QTimer::timeout, process, [process] { process->kill(); });
        QObject::connect(process, &QProcess::started, process,
                         [process, batch, timer]
                         {
                             process->write(batch);
                             process->closeWriteChannel();
                             timer->start(60000);
                         });
        QObject::connect(process, &QProcess::finished, &context,
                         [process, done](int code, QProcess::ExitStatus status)
                         {
                             process->deleteLater();
                             done(status == QProcess::NormalExit && code == 0, QString::fromUtf8(process->readAll()).trimmed());
                         });
        QObject::connect(process, &QProcess::errorOccurred, &context,
                         [process, done](QProcess::ProcessError e)
                         {
                             if (e != QProcess::FailedToStart)
                                 return;
                             process->deleteLater();
                             done(false, process->errorString());
                         });
        process->start(program, QStringList{"-o", "BatchMode=no", "-b", "-", "-o", "StrictHostKeyChecking=yes", "-o",
                                            "ConnectTimeout=10", "-o", "NumberOfPasswordPrompts=1"} +
                                    command.arguments);
    }
    static QString lastLine(const QString &output)
    {
        const auto lines = output.split('\n', Qt::SkipEmptyParts);
        return lines.isEmpty() ? ui("sftp failed.") : lines.last().trimmed();
    }

public:
    Sftp(QJsonObject session, const QString &path) : profile(std::move(session)), path(path)
    {
        profile["protocol"] = "sftp";
        profile["x11"] = false;
    }

protected:
    void fetch(Done done) override
    {
        auto folder = std::make_shared<QTemporaryDir>();
        const auto local = QDir::fromNativeSeparators(folder->filePath(FileName));
        const auto remote = sftpQuote(path);
        if (!folder->isValid() || remote.isEmpty())
            return done(failed(ui("Invalid path.")));
        run({"get " + remote + " " + sftpQuote(local)},
            [folder, local, done](bool ok, const QString &output)
            {
                if (!ok)
                    return done(output.contains("not found", Qt::CaseInsensitive) ||
                                        output.contains("No such file", Qt::CaseInsensitive)
                                    ? withStatus(Result::NotFound)
                                    : failed(lastLine(output)));
                QFile f(local);
                if (!f.open(QIODevice::ReadOnly))
                    return done(failed(f.errorString()));
                Result r = withStatus(Result::Ok);
                r.data = f.readAll();
                done(r);
            });
    }
    void store(const QByteArray &data, Done done) override
    {
        auto folder = std::make_shared<QTemporaryDir>();
        const auto local = QDir::fromNativeSeparators(folder->filePath(FileName));
        QFile f(local);
        if (!folder->isValid() || !f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
            return done(failed(ui("Cannot write a temporary file.")));
        f.close();
        const auto remote = sftpQuote(path), partial = sftpQuote(path + ".part"), source = sftpQuote(local);
        // Uploaded beside and renamed over it (posix-rename): readers never see half a file. Servers without that
        // extension refuse to rename over an existing file: then it is uploaded in place.
        run({"put " + source + " " + partial, "rename " + partial + " " + remote},
            [this, folder, source, remote, partial, done](bool ok, const QString &)
            {
                if (ok)
                    return done(withStatus(Result::Ok));
                run({"put " + source + " " + remote, "-rm " + partial},
                    [folder, done](bool ok, const QString &output)
                    { done(ok ? withStatus(Result::Ok) : failed(lastLine(output))); });
            });
    }
};

// ---------------------------------------------------------------- FTPS
class Ftps : public HashStorage
{
    QObject context;
    QJsonObject settings;

    // A connected client; done(client) or done(nullptr, failure). The client is released after the callback.
    void connect(std::function<void(std::shared_ptr<FtpClient>, const Result &)> done)
    {
        auto client = std::make_shared<FtpClient>();
        client->security = settings["security"].toString();
        auto seen = std::make_shared<QString>();
        client->trustCertificate = [trusted = settings["fingerprint"].toString(), seen](const QSslCertificate &c)
        {
            *seen = FtpClient::fingerprint(c);
            return !trusted.isEmpty() && *seen == trusted;
        };
        client->connectToHost(settings["host"].toString(), quint16(settings["port"].toInt()), settings["user"].toString(),
                              settings["password"].toString(),
                              [this, client, seen, done](bool ok, const QString &error)
                              {
                                  Result r = failed(error);
                                  if (!ok && !seen->isEmpty() && *seen != settings["fingerprint"].toString())
                                  {
                                      r.error = ui("The server certificate is not the trusted one.");
                                      r.fingerprint = *seen;
                                  }
                                  if (!ok)
                                      release(client); // not destroyed inside its own callback
                                  done(ok ? client : nullptr, r);
                              });
    }
    // Ends the session outside the client's own callback (it must not be destroyed there).
    void release(const std::shared_ptr<FtpClient> &client)
    {
        QTimer::singleShot(0, &context, [client] { client->disconnectFromHost(); });
    }

public:
    explicit Ftps(const QJsonObject &settings) : settings(settings) {}

protected:
    void fetch(Done done) override
    {
        connect(
            [this, done](std::shared_ptr<FtpClient> client, const Result &failure)
            {
                if (!client)
                    return done(failure);
                auto folder = std::make_shared<QTemporaryDir>();
                const auto local = folder->filePath(FileName);
                client->download(settings["path"].toString(), local, nullptr,
                                 [this, client, folder, local, done](bool ok, const QString &error)
                                 {
                                     release(client);
                                     if (!ok)
                                         return done(error.startsWith("550") ? withStatus(Result::NotFound) : failed(error));
                                     QFile f(local);
                                     if (!f.open(QIODevice::ReadOnly))
                                         return done(failed(f.errorString()));
                                     Result r = withStatus(Result::Ok);
                                     r.data = f.readAll();
                                     done(r);
                                 });
            });
    }
    void store(const QByteArray &data, Done done) override
    {
        auto folder = std::make_shared<QTemporaryDir>();
        const auto local = folder->filePath(FileName);
        QFile f(local);
        if (!folder->isValid() || !f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
            return done(failed(ui("Cannot write a temporary file.")));
        f.close();
        connect(
            [this, folder, local, done](std::shared_ptr<FtpClient> client, const Result &failure)
            {
                if (!client)
                    return done(failure);
                const auto path = settings["path"].toString(), partial = path + ".part";
                // Uploaded beside, then put in place: readers never see half a file.
                client->upload(local, partial, nullptr,
                               [this, client, folder, path, partial, done](bool ok, const QString &error)
                               {
                                   if (!ok)
                                   {
                                       release(client);
                                       return done(failed(error));
                                   }
                                   client->removeFile(path, [](bool, const QString &) {}); // may not exist yet
                                   client->rename(partial, path,
                                                  [this, client, done](bool ok, const QString &error)
                                                  {
                                                      release(client);
                                                      done(ok ? withStatus(Result::Ok) : failed(error));
                                                  });
                               });
            });
    }
};

// ---------------------------------------------------------------- Dropbox, OneDrive, Google Drive
// Requests with an access token obtained from the saved refresh token: refreshed when it expires and once more
// after a 401. A refresh token that no longer works means signing in again (Result::signIn).
class Cloud : public Http
{
protected:
    SyncOAuth::Provider provider;
    QString base; // local test server, or empty
    QString refreshToken, access;
    qint64 expiresAt = 0;
    QJsonObject updates; // rotated refresh token, reported with the next result

    QString url(const QString &address) const { return SyncOAuth::redirected(address, base); }

    // Sends request with the token; answer(reply) or answer(nullptr, failure) when no token could be had.
    void call(QNetworkRequest request, const QByteArray &verb, const QByteArray &body,
              std::function<void(QNetworkReply *, const Result &failure)> answer, bool retried = false)
    {
        withToken(
            [this, request, verb, body, answer, retried](const Result &failure) mutable
            {
                if (failure.status == Result::Failed)
                    return answer(nullptr, failure);
                request.setRawHeader("Authorization", "Bearer " + access.toUtf8());
                send(request, verb, body,
                     [this, request, verb, body, answer, retried](QNetworkReply *reply)
                     {
                         if (status(reply) == 401 && !retried)
                         {
                             access.clear(); // revoked or expired early: one more try with a fresh one
                             return call(request, verb, body, answer, true);
                         }
                         answer(reply, {});
                     });
            });
    }
    void withToken(std::function<void(const Result &)> ready)
    {
        if (!access.isEmpty() && QDateTime::currentMSecsSinceEpoch() < expiresAt - 60000)
            return ready(withStatus(Result::Ok));
        SyncOAuth::refresh(network, provider, refreshToken,
                           [this, ready](const SyncOAuth::Tokens &t)
                           {
                               if (!t.error.isEmpty())
                               {
                                   // Sign in again only when the provider refused the token; offline it is not.
                                   if (!t.rejected)
                                       return ready(failed(t.error));
                                   Result r = failed(ui("Sign in again: %1").arg(t.error));
                                   r.signIn = true;
                                   return ready(r);
                               }
                               access = t.access;
                               expiresAt = t.expiresAt ? t.expiresAt : QDateTime::currentMSecsSinceEpoch() + 3600000;
                               if (t.refresh != refreshToken)
                               {
                                   refreshToken = t.refresh;
                                   updates["refresh"] = refreshToken;
                               }
                               ready(withStatus(Result::Ok));
                           });
    }
    // Delivers r with any settings to save (rotated refresh token).
    void finish(Result r, const Done &done)
    {
        for (auto it = updates.begin(); it != updates.end(); ++it)
            r.updated[it.key()] = it.value();
        updates = {};
        done(r);
    }

public:
    Cloud(const QString &name, const QString &refresh, const QString &testBase)
        : provider(SyncOAuth::provider(name, testBase)), base(testBase), refreshToken(refresh)
    {
    }
};

class Dropbox : public Cloud
{
    // Path inside the app folder (Apps/CrossTerm): the app sees nothing else of the account.
    static QByteArray argument(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
    QNetworkRequest request(const QString &endpoint, const QJsonObject &arg) const
    {
        QNetworkRequest r{QUrl(url("https://content.dropboxapi.com/2/files/" + endpoint))};
        r.setRawHeader("Dropbox-API-Arg", argument(arg));
        return r;
    }
    static bool conflict(QNetworkReply *reply, const QByteArray &body, const char *kind)
    {
        return status(reply) == 409 && body.contains(kind);
    }

public:
    using Cloud::Cloud;
    void read(Done done) override
    {
        call(request("download", {{"path", QString("/") + FileName}}), "POST", {},
             [this, done](QNetworkReply *reply, const Result &failure)
             {
                 if (!reply)
                     return finish(failure, done);
                 const auto body = reply->readAll();
                 if (conflict(reply, body, "not_found"))
                     return finish(withStatus(Result::NotFound), done);
                 if (reply->error() != QNetworkReply::NoError)
                     return finish(failed(httpError(reply)), done);
                 Result r = withStatus(Result::Ok);
                 r.data = body;
                 r.revision = "rev:" + QJsonDocument::fromJson(reply->rawHeader("Dropbox-API-Result")).object()["rev"].toString();
                 finish(r, done);
             });
    }
    void write(const QByteArray &data, const QString &revision, Done done) override
    {
        // "add" fails if the file exists; "update" fails unless it is still at that revision.
        const QJsonValue mode = revision.isEmpty() ? QJsonValue("add")
                                                   : QJsonValue(QJsonObject{{".tag", "update"}, {"update", revision.mid(4)}});
        auto r = request("upload", {{"path", QString("/") + FileName},
                                    {"mode", mode},
                                    {"autorename", false},
                                    {"mute", true},
                                    {"strict_conflict", true}});
        r.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
        call(r, "POST", data,
             [this, done](QNetworkReply *reply, const Result &failure)
             {
                 if (!reply)
                     return finish(failure, done);
                 const auto body = reply->readAll();
                 if (conflict(reply, body, "conflict"))
                     return finish(withStatus(Result::Conflict), done);
                 if (reply->error() != QNetworkReply::NoError)
                     return finish(failed(httpError(reply)), done);
                 Result ok = withStatus(Result::Ok);
                 ok.revision = "rev:" + QJsonDocument::fromJson(body).object()["rev"].toString();
                 finish(ok, done);
             });
    }
};

class OneDrive : public Cloud
{
    // The app folder (Apps/CrossTerm) of the signed-in account.
    QString item() const { return url(QString("https://graph.microsoft.com/v1.0/me/drive/special/approot:/") + FileName); }

public:
    using Cloud::Cloud;
    void read(Done done) override
    {
        call(QNetworkRequest(QUrl(item())), "GET", {},
             [this, done](QNetworkReply *reply, const Result &failure)
             {
                 if (!reply)
                     return finish(failure, done);
                 if (status(reply) == 404)
                     return finish(withStatus(Result::NotFound), done);
                 if (reply->error() != QNetworkReply::NoError)
                     return finish(failed(httpError(reply)), done);
                 const auto meta = QJsonDocument::fromJson(reply->readAll()).object();
                 const auto revision = "etag:" + meta["eTag"].toString();
                 // Short-lived pre-authorized address: no token goes with it.
                 send(QNetworkRequest(QUrl(meta["@microsoft.graph.downloadUrl"].toString())), "GET", {},
                      [this, revision, done](QNetworkReply *content)
                      {
                          if (content->error() != QNetworkReply::NoError)
                              return finish(failed(httpError(content)), done);
                          Result r = withStatus(Result::Ok);
                          r.data = content->readAll();
                          r.revision = revision;
                          finish(r, done);
                      });
             });
    }
    void write(const QByteArray &data, const QString &revision, Done done) override
    {
        QUrl target(item() + ":/content");
        QNetworkRequest r;
        if (revision.isEmpty())
            target.setQuery("@microsoft.graph.conflictBehavior=fail"); // 409 if it exists
        r.setUrl(target);
        if (!revision.isEmpty())
            r.setRawHeader("If-Match", revision.mid(5).toUtf8()); // 412 if it changed
        r.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
        call(r, "PUT", data,
             [this, done](QNetworkReply *reply, const Result &failure)
             {
                 if (!reply)
                     return finish(failure, done);
                 if (status(reply) == 409 || status(reply) == 412)
                     return finish(withStatus(Result::Conflict), done);
                 if (reply->error() != QNetworkReply::NoError)
                     return finish(failed(httpError(reply)), done);
                 Result ok = withStatus(Result::Ok);
                 ok.revision = "etag:" + QJsonDocument::fromJson(reply->readAll()).object()["eTag"].toString();
                 finish(ok, done);
             });
    }
};

class GoogleDrive : public Cloud
{
    QString files() const { return url("https://www.googleapis.com/drive/v3/files"); }
    QString uploads() const { return url("https://www.googleapis.com/upload/drive/v3/files"); }

    // The sync file in the hidden app data folder: {id, version}, or {} if there is none. If two devices created
    // it at the same moment, every device uses the oldest one.
    void find(std::function<void(const QJsonObject &file, const Result &failure)> done)
    {
        QUrl list(files());
        QUrlQuery query;
        query.addQueryItem("spaces", "appDataFolder");
        query.addQueryItem("q", QString("name = '%1' and trashed = false").arg(FileName));
        query.addQueryItem("orderBy", "createdTime");
        query.addQueryItem("fields", "files(id,version)");
        list.setQuery(query);
        call(QNetworkRequest(list), "GET", {},
             [done](QNetworkReply *reply, const Result &failure)
             {
                 if (!reply)
                     return done({}, failure);
                 if (reply->error() != QNetworkReply::NoError)
                     return done({}, failed(httpError(reply)));
                 const auto found = QJsonDocument::fromJson(reply->readAll()).object()["files"].toArray();
                 done(found.isEmpty() ? QJsonObject{} : found[0].toObject(), withStatus(Result::Ok));
             });
    }
    static QString revisionOf(const QJsonObject &file) { return "version:" + file["version"].toString(); }

public:
    using Cloud::Cloud;
    void read(Done done) override
    {
        find(
            [this, done](const QJsonObject &file, const Result &failure)
            {
                if (failure.status == Result::Failed)
                    return finish(failure, done);
                if (file.isEmpty())
                    return finish(withStatus(Result::NotFound), done);
                call(QNetworkRequest(QUrl(files() + "/" + file["id"].toString() + "?alt=media")), "GET", {},
                     [this, file, done](QNetworkReply *reply, const Result &failure)
                     {
                         if (!reply)
                             return finish(failure, done);
                         if (reply->error() != QNetworkReply::NoError)
                             return finish(failed(httpError(reply)), done);
                         Result r = withStatus(Result::Ok);
                         r.data = reply->readAll();
                         r.revision = revisionOf(file);
                         finish(r, done);
                     });
            });
    }
    // Drive has no conditional upload: the version is compared right before writing.
    // ponytail: a write from another device in the instant between the check and the upload is overwritten;
    // its next synchronization reads this version and merges again, so only that instant is at risk.
    void write(const QByteArray &data, const QString &revision, Done done) override
    {
        find(
            [this, data, revision, done](const QJsonObject &file, const Result &failure)
            {
                if (failure.status == Result::Failed)
                    return finish(failure, done);
                if ((file.isEmpty() ? QString() : revisionOf(file)) != revision)
                    return finish(withStatus(Result::Conflict), done);
                const auto answer = [this, done](QNetworkReply *reply, const Result &failure)
                {
                    if (!reply)
                        return finish(failure, done);
                    if (reply->error() != QNetworkReply::NoError)
                        return finish(failed(httpError(reply)), done);
                    Result ok = withStatus(Result::Ok);
                    ok.revision = revisionOf(QJsonDocument::fromJson(reply->readAll()).object());
                    finish(ok, done);
                };
                if (!file.isEmpty())
                {
                    QNetworkRequest r(QUrl(uploads() + "/" + file["id"].toString() + "?uploadType=media&fields=id,version"));
                    r.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
                    return call(r, "PATCH", data, answer);
                }
                // New file: metadata (name, app data folder) and content in one multipart request.
                const QByteArray boundary = "crossterm-" + QByteArray::number(QDateTime::currentMSecsSinceEpoch());
                const auto metadata = QJsonDocument(QJsonObject{{"name", FileName}, {"parents", QJsonArray{"appDataFolder"}}})
                                          .toJson(QJsonDocument::Compact);
                const QByteArray body = "--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" +
                                        metadata + "\r\n--" + boundary +
                                        "\r\nContent-Type: application/octet-stream\r\n\r\n" + data + "\r\n--" + boundary +
                                        "--\r\n";
                QNetworkRequest r(QUrl(uploads() + "?uploadType=multipart&fields=id,version"));
                r.setHeader(QNetworkRequest::ContentTypeHeader, "multipart/related; boundary=" + boundary);
                call(r, "POST", body, answer);
            });
    }
};
} // namespace

QString contentRevision(const QByteArray &data)
{
    return "sha256:" + QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

std::unique_ptr<Storage> create(const QJsonObject &s, QString *error)
{
    const auto fail = [error](const QString &message) -> std::unique_ptr<Storage>
    {
        if (error)
            *error = message;
        return nullptr;
    };
    const auto type = s["type"].toString();
    if (type == "folder")
    {
        if (s["path"].toString().isEmpty())
            return fail(ui("Choose the folder."));
        return std::make_unique<Folder>(s["path"].toString());
    }
    if (type == "webdav")
    {
        const QUrl url(s["url"].toString());
        // Plain HTTP would send the password in clear text (the file itself is encrypted either way).
        if (!url.isValid() || !(url.scheme() == "https" || (url.scheme() == "http" && localHost(url))))
            return fail(ui("Enter an https:// address."));
        return std::make_unique<WebDav>(url, s["user"].toString(), s["password"].toString());
    }
    if (type == "gist")
    {
        if (s["token"].toString().isEmpty())
            return fail(ui("Enter a GitHub token."));
        const QUrl api(s["api"].toString("https://api.github.com"));
        if (api.toString() != "https://api.github.com" && !localHost(api)) // the token only goes to GitHub
            return fail(ui("Enter an https:// address."));
        return std::make_unique<Gist>(api.toString(), s["token"].toString(), s["gist"].toString());
    }
    if (type == "sftp")
    {
        if (s["session"].toObject()["host"].toString().isEmpty() || s["path"].toString().isEmpty())
            return fail(ui("Choose the SSH session and the file path."));
        return std::make_unique<Sftp>(s["session"].toObject(), s["path"].toString());
    }
    if (type == "ftps")
    {
        // Plain FTP would send the password in clear text.
        if (s["security"].toString() != "explicit" && s["security"].toString() != "implicit")
            return fail(ui("Only FTP over TLS (explicit or implicit) can be used."));
        if (s["host"].toString().isEmpty() || s["path"].toString().isEmpty())
            return fail(ui("Enter the server and the file path."));
        return std::make_unique<Ftps>(s);
    }
    if (type == "dropbox" || type == "onedrive" || type == "gdrive")
    {
        const QUrl api(s["api"].toString());
        if (!s["api"].toString().isEmpty() && !localHost(api)) // tokens only go to the provider
            return fail(ui("Enter an https:// address."));
        if (!SyncOAuth::available(type) && s["api"].toString().isEmpty())
            return fail(ui("This build of CrossTerm cannot sign in to this service."));
        if (s["refresh"].toString().isEmpty())
            return fail(ui("Sign in to the service first."));
        if (type == "dropbox")
            return std::make_unique<Dropbox>(type, s["refresh"].toString(), s["api"].toString());
        if (type == "onedrive")
            return std::make_unique<OneDrive>(type, s["refresh"].toString(), s["api"].toString());
        return std::make_unique<GoogleDrive>(type, s["refresh"].toString(), s["api"].toString());
    }
    return fail(ui("Unknown storage type."));
}
} // namespace SyncStorage
