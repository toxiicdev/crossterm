#include "syncoauth.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

// App ids of this build (public; see syncoauth.h). Empty: that provider is not offered.
#ifndef CROSSTERM_DROPBOX_APP_KEY
#define CROSSTERM_DROPBOX_APP_KEY ""
#endif
#ifndef CROSSTERM_MICROSOFT_CLIENT_ID
#define CROSSTERM_MICROSOFT_CLIENT_ID ""
#endif
#ifndef CROSSTERM_GOOGLE_CLIENT_ID
#define CROSSTERM_GOOGLE_CLIENT_ID ""
#endif
#ifndef CROSSTERM_GOOGLE_CLIENT_SECRET
#define CROSSTERM_GOOGLE_CLIENT_SECRET ""
#endif

namespace SyncOAuth
{
namespace
{
QString ui(const char *text)
{
    return QCoreApplication::translate("CrossTerm", text);
}

QByteArray randomUrlSafe(int bytes)
{
    QList<quint32> words((bytes + 3) / 4);
    QRandomGenerator::system()->fillRange(words.data(), words.size());
    return QByteArray(reinterpret_cast<const char *>(words.constData()), bytes)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

bool local(const QUrl &url)
{
    return url.scheme() == "http" && (url.host() == "127.0.0.1" || url.host() == "localhost" || url.host() == "::1");
}

Tokens parseTokens(QNetworkReply *reply)
{
    Tokens t;
    const auto o = QJsonDocument::fromJson(reply->readAll()).object();
    t.access = o["access_token"].toString();
    t.refresh = o["refresh_token"].toString();
    if (o.contains("expires_in"))
        t.expiresAt = QDateTime::currentMSecsSinceEpoch() + qint64(o["expires_in"].toDouble()) * 1000;
    if (t.access.isEmpty())
    {
        t.error = o["error_description"].toString(o["error"].toString());
        if (t.error.isEmpty())
            t.error = reply->errorString();
    }
    return t;
}

void postForm(QNetworkAccessManager *network, const QString &url, const QUrlQuery &form, QObject *context,
              std::function<void(const Tokens &)> done)
{
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setTransferTimeout(30000);
    auto *reply = network->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    QObject::connect(reply, &QNetworkReply::finished, context,
                     [reply, done]
                     {
                         reply->deleteLater();
                         done(parseTokens(reply));
                     });
}

// The page the browser shows after the redirect. Plain text, no data from the request.
QByteArray page(const QString &message)
{
    const QByteArray html = "<!doctype html><meta charset=utf-8><title>CrossTerm</title>"
                            "<body style=\"font:16px system-ui,sans-serif;margin:3em\"><p>" +
                            message.toHtmlEscaped().toUtf8() + "</p></body>";
    return "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
           QByteArray::number(html.size()) + "\r\n\r\n" + html;
}
} // namespace

QString redirected(const QString &url, const QString &base)
{
    const QUrl target(base);
    if (base.isEmpty() || !local(target))
        return url;
    QUrl u(url);
    u.setScheme(target.scheme());
    u.setHost(target.host());
    u.setPort(target.port());
    return u.toString();
}

Provider provider(const QString &name, const QString &testBase)
{
    Provider p;
    p.name = name;
    if (name == "dropbox")
    {
        p.authorizeUrl = "https://www.dropbox.com/oauth2/authorize";
        p.tokenUrl = "https://api.dropboxapi.com/oauth2/token";
        p.clientId = CROSSTERM_DROPBOX_APP_KEY;
        p.scope = "files.content.read files.content.write";
        // Dropbox requires the exact redirect address: these three are registered with the app.
        p.redirectHost = "localhost";
        p.ports = {53682, 53683, 53684};
        p.extra = {{"token_access_type", "offline"}};
    }
    else if (name == "onedrive")
    {
        p.authorizeUrl = "https://login.microsoftonline.com/common/oauth2/v2.0/authorize";
        p.tokenUrl = "https://login.microsoftonline.com/common/oauth2/v2.0/token";
        p.clientId = CROSSTERM_MICROSOFT_CLIENT_ID;
        p.scope = "Files.ReadWrite.AppFolder offline_access";
        p.redirectHost = "localhost"; // any port (Microsoft ignores it for http://localhost)
        p.extra = {{"prompt", "select_account"}};
    }
    else if (name == "gdrive")
    {
        p.authorizeUrl = "https://accounts.google.com/o/oauth2/v2/auth";
        p.tokenUrl = "https://oauth2.googleapis.com/token";
        p.clientId = CROSSTERM_GOOGLE_CLIENT_ID;
        p.clientSecret = CROSSTERM_GOOGLE_CLIENT_SECRET;
        p.scope = "https://www.googleapis.com/auth/drive.appdata"; // the app's own hidden folder only
        p.redirectHost = "127.0.0.1";
        p.extra = {{"access_type", "offline"}, {"prompt", "consent"}};
    }
    p.authorizeUrl = redirected(p.authorizeUrl, testBase);
    p.tokenUrl = redirected(p.tokenUrl, testBase);
    return p;
}

bool available(const QString &name)
{
    return !provider(name).clientId.isEmpty();
}

Login::Login(QObject *parent) : QObject(parent), network(new QNetworkAccessManager(this)) {}

Login::~Login()
{
    callback = nullptr;
}

void Login::start(const Provider &p, std::function<void(const QUrl &)> openBrowser, std::function<void(const Tokens &)> done)
{
    callback = std::move(done);
    // Listener on IPv4 and, on the same port, IPv6 loopback: "localhost" may resolve to either.
    quint16 port = 0;
    for (const quint16 candidate : p.ports.isEmpty() ? QList<quint16>{0} : p.ports)
    {
        auto *v4 = new QTcpServer(this);
        if (v4->listen(QHostAddress::LocalHost, candidate))
        {
            port = v4->serverPort();
            listeners << v4;
            auto *v6 = new QTcpServer(this);
            if (v6->listen(QHostAddress::LocalHostIPv6, port))
                listeners << v6;
            else
                delete v6;
            break;
        }
        delete v4;
    }
    if (!port)
        return finish(Tokens{{}, {}, 0, ui("No local port is free for the sign-in.")});

    const QString redirect = QString("http://%1:%2/").arg(p.redirectHost).arg(port);
    const QByteArray verifier = randomUrlSafe(32), state = randomUrlSafe(16);
    const QByteArray challenge = QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    QUrlQuery query;
    query.addQueryItem("client_id", p.clientId);
    query.addQueryItem("response_type", "code");
    query.addQueryItem("redirect_uri", redirect);
    query.addQueryItem("scope", p.scope);
    query.addQueryItem("state", QString::fromLatin1(state));
    query.addQueryItem("code_challenge", QString::fromLatin1(challenge));
    query.addQueryItem("code_challenge_method", "S256");
    for (const auto &[key, value] : p.extra)
        query.addQueryItem(key, value);
    QUrl authorize(p.authorizeUrl);
    authorize.setQuery(query);

    auto *timeout = new QTimer(this);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, this, [this] { finish(Tokens{{}, {}, 0, ui("The sign-in timed out.")}); });
    timeout->start(5 * 60 * 1000);

    for (auto *listener : listeners)
        connect(listener, &QTcpServer::newConnection, this,
                [this, listener, p, redirect, verifier, state]
                {
                    while (auto *socket = listener->nextPendingConnection())
                    {
                        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                        connect(socket, &QTcpSocket::readyRead, this,
                                [this, socket, p, redirect, verifier, state]
                                {
                                    auto request = socket->property("request").toByteArray() + socket->readAll();
                                    socket->setProperty("request", request);
                                    if (!request.contains("\r\n\r\n"))
                                    {
                                        if (request.size() > 16384)
                                            socket->abort();
                                        return;
                                    }
                                    const auto target = request.left(request.indexOf("\r\n")).split(' ').value(1);
                                    const QUrl url("http://local" + QString::fromLatin1(target));
                                    if (url.path() != "/")
                                    {
                                        socket->write("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                                        socket->disconnectFromHost();
                                        return;
                                    }
                                    const QUrlQuery answer(url);
                                    // A page that did not come from this sign-in (wrong state) changes nothing.
                                    if (answer.queryItemValue("state").toLatin1() != state)
                                    {
                                        socket->write(page(ui("This sign-in is not the one CrossTerm started.")));
                                        socket->disconnectFromHost();
                                        return;
                                    }
                                    const auto code = answer.queryItemValue("code", QUrl::FullyDecoded);
                                    socket->write(page(code.isEmpty() ? ui("Sign-in cancelled. You can close this page.")
                                                                      : ui("Signed in. You can close this page and go back to CrossTerm.")));
                                    socket->disconnectFromHost();
                                    for (auto *l : listeners)
                                        l->close();
                                    if (code.isEmpty())
                                        return finish(Tokens{{}, {}, 0, ui("Sign-in cancelled.")});
                                    QUrlQuery form;
                                    form.addQueryItem("grant_type", "authorization_code");
                                    form.addQueryItem("code", code);
                                    form.addQueryItem("redirect_uri", redirect);
                                    form.addQueryItem("client_id", p.clientId);
                                    form.addQueryItem("code_verifier", QString::fromLatin1(verifier));
                                    if (!p.clientSecret.isEmpty())
                                        form.addQueryItem("client_secret", p.clientSecret);
                                    postForm(network, p.tokenUrl, form, this, [this](const Tokens &t) { finish(t); });
                                });
                    }
                });
    openBrowser(authorize);
}

void Login::cancel()
{
    finish(Tokens{{}, {}, 0, ui("Sign-in cancelled.")});
}

void Login::finish(const Tokens &tokens)
{
    for (auto *listener : listeners)
        listener->close();
    if (auto done = std::exchange(callback, nullptr)) // once
        done(tokens);
}

void refresh(QNetworkAccessManager *network, const Provider &p, const QString &refreshToken,
             std::function<void(const Tokens &)> done)
{
    QUrlQuery form;
    form.addQueryItem("grant_type", "refresh_token");
    form.addQueryItem("refresh_token", refreshToken);
    form.addQueryItem("client_id", p.clientId);
    if (!p.clientSecret.isEmpty())
        form.addQueryItem("client_secret", p.clientSecret);
    if (p.name == "onedrive")
        form.addQueryItem("scope", p.scope);
    postForm(network, p.tokenUrl, form, network,
             [refreshToken, done](Tokens t)
             {
                 if (t.error.isEmpty() && t.refresh.isEmpty())
                     t.refresh = refreshToken; // not rotated
                 done(t);
             });
}
} // namespace SyncOAuth
