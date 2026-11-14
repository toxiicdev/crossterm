#pragma once
// Sign-in with Dropbox, Microsoft (OneDrive) and Google (Drive) for synchronization: OAuth 2.0 authorization code
// with PKCE (RFC 7636) and a loopback redirect (RFC 8252). The browser opens the provider's page; the provider
// sends the browser back to a listener on 127.0.0.1 / ::1 that only this program holds, with a one-time code that
// is exchanged (with the PKCE verifier) for tokens. No server of ours is involved and no password passes through
// CrossTerm. Only the refresh token is kept (in the system vault, by the caller).
//
// App ids are public (desktop apps cannot keep secrets) and set at build time: CMake variables or environment
// variables CROSSTERM_DROPBOX_APP_KEY, CROSSTERM_MICROSOFT_CLIENT_ID, CROSSTERM_GOOGLE_CLIENT_ID and
// CROSSTERM_GOOGLE_CLIENT_SECRET (Google issues one to desktop clients and requires it, but it is not secret).
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QTcpServer;

namespace SyncOAuth
{
struct Provider
{
    QString name; // "dropbox" | "onedrive" | "gdrive"
    QString authorizeUrl, tokenUrl, clientId, clientSecret, scope;
    QString redirectHost;  // host written in the redirect address: "localhost" or "127.0.0.1"
    QList<quint16> ports;  // fixed ports registered with the provider (Dropbox); empty: any free port
    QList<std::pair<QString, QString>> extra; // extra authorization parameters
};

// Built-in provider ("dropbox", "onedrive", "gdrive") with the app ids of this build. testBase (http://127.0.0.1:port
// only) sends every request to a local test server instead.
Provider provider(const QString &name, const QString &testBase = {});
// The build has an app id for this provider (otherwise it cannot be offered).
bool available(const QString &name);
// url with the scheme, host and port of base when base is a local test server; url otherwise.
QString redirected(const QString &url, const QString &base);

struct Tokens
{
    QString access, refresh;
    qint64 expiresAt = 0; // ms since 1970
    QString error;        // empty on success
};

// One sign-in: listen, open the browser, wait for the redirect (5 minutes at most), exchange the code.
class Login : public QObject
{
public:
    explicit Login(QObject *parent = nullptr);
    ~Login() override;
    // openBrowser receives the provider's page (QDesktopServices::openUrl in the program, a fake in tests).
    void start(const Provider &provider, std::function<void(const QUrl &)> openBrowser, std::function<void(const Tokens &)> done);
    void cancel();

private:
    void finish(const Tokens &tokens);
    QList<QTcpServer *> listeners;
    QNetworkAccessManager *network;
    std::function<void(const Tokens &)> callback;
};

// New access token from a refresh token (the refresh token may come back rotated: Microsoft).
void refresh(QNetworkAccessManager *network, const Provider &provider, const QString &refreshToken,
             std::function<void(const Tokens &)> done);
} // namespace SyncOAuth
