// Cloud drive sign-in and storages against local fakes of the providers:
// - sign-in: PKCE (the token endpoint checks the verifier against the challenge), state, loopback redirect, a
//   forged redirect with another state ignored, cancellation;
// - Dropbox, OneDrive, Google Drive: the read / write / conflict cycle with their own revision mechanisms,
//   access token refreshed after a 401, rotated refresh token reported, revoked refresh token = sign in again,
//   OneDrive download address used without the token.
#include "syncfakes.h"
#include "syncoauth.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrlQuery>

using namespace SyncOAuth;

// Token endpoint shared by the fakes: refresh tokens "R1" (rotated to "R2" by the first refresh when rotate is
// set) and "R2" give access tokens A1, A2...; anything else is revoked.
struct Tokens_
{
    int issued = 0;
    bool rotate = false;
    QByteArray current() const { return "A" + QByteArray::number(issued); }
    HttpReply refresh(const HttpRequest &r)
    {
        const QUrlQuery form(QString::fromUtf8(r.body));
        const auto token = form.queryItemValue("refresh_token");
        if (form.queryItemValue("grant_type") != "refresh_token" || (token != "R1" && token != "R2"))
            return {400, R"({"error":"invalid_grant","error_description":"revoked"})", {}};
        ++issued;
        QJsonObject o{{"access_token", QString::fromLatin1(current())}, {"expires_in", 3600}};
        if (rotate && token == "R1")
            o["refresh_token"] = "R2";
        return {200, QJsonDocument(o).toJson(), {}};
    }
    bool authorized(const HttpRequest &r) const { return r.headers.value("authorization") == "Bearer " + current(); }
};

static QByteArray pathOf(const HttpRequest &r)
{
    return r.path.left(r.path.indexOf('?') < 0 ? r.path.size() : r.path.indexOf('?'));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QNetworkAccessManager browser; // plays the user's browser

    // --- Providers: Dropbox needs fixed registered ports; test servers only replace local addresses.
    CHECK(provider("dropbox").ports == (QList<quint16>{53682, 53683, 53684}) && provider("dropbox").redirectHost == "localhost")
    CHECK(provider("gdrive").scope == "https://www.googleapis.com/auth/drive.appdata")
    CHECK(redirected("https://api.dropboxapi.com/oauth2/token", "http://127.0.0.1:9") == "http://127.0.0.1:9/oauth2/token")
    CHECK(redirected("https://api.dropboxapi.com/oauth2/token", "https://evil.example") == "https://api.dropboxapi.com/oauth2/token")

    // --- Sign-in.
    {
        FakeHttp server;
        QByteArray challenge;
        server.handle = [&](const HttpRequest &r) -> HttpReply
        {
            const QUrlQuery form(QString::fromUtf8(r.body));
            const auto verifier = form.queryItemValue("code_verifier").toLatin1();
            const auto computed = QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
                                      .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
            if (pathOf(r) != "/token" || form.queryItemValue("code") != "the-code" || computed != challenge ||
                form.queryItemValue("grant_type") != "authorization_code")
                return {400, R"({"error":"invalid_grant"})", {}};
            return {200, R"({"access_token":"A","refresh_token":"R","expires_in":3600})", {}};
        };
        const auto p = provider("gdrive", server.base());
        CHECK(p.tokenUrl == server.base() + "/token")
        std::optional<Tokens> result;
        QUrl page;
        Login login;
        login.start(p, [&](const QUrl &url) { page = url; }, [&](const Tokens &t) { result = t; });
        const QUrlQuery q(page);
        challenge = q.queryItemValue("code_challenge").toLatin1();
        const QUrl redirect(q.queryItemValue("redirect_uri"));
        CHECK(q.queryItemValue("code_challenge_method") == "S256" && challenge.size() == 43)
        CHECK(q.queryItemValue("state").size() >= 20 && q.queryItemValue("access_type") == "offline")
        CHECK(redirect.host() == "127.0.0.1" && redirect.port() > 0 && redirect.path() == "/")
        // A redirect carrying another state (another page, an attacker) changes nothing.
        auto *forged = browser.get(QNetworkRequest(QUrl(redirect.toString() + "?state=forged&code=evil")));
        QDeadlineTimer wait(10000);
        while (!forged->isFinished() && !wait.hasExpired())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        CHECK(forged->readAll().contains("not the one") && !result)
        // The real one: code exchanged with the verifier.
        QUrl back(redirect);
        back.setQuery("state=" + q.queryItemValue("state") + "&code=the-code");
        auto *reply = browser.get(QNetworkRequest(back));
        wait = QDeadlineTimer(10000);
        while ((!result || !reply->isFinished()) && !wait.hasExpired())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        CHECK(result && result->error.isEmpty() && result->access == "A" && result->refresh == "R")
        CHECK(reply->readAll().contains("Signed in"))

        // Cancelled in the browser (no code): an error, once.
        std::optional<Tokens> cancelled;
        Login second;
        QUrl page2;
        second.start(p, [&](const QUrl &url) { page2 = url; }, [&](const Tokens &t) { cancelled = t; });
        const QUrlQuery q2(page2);
        browser.get(QNetworkRequest(QUrl(q2.queryItemValue("redirect_uri") + "?state=" + q2.queryItemValue("state") +
                                         "&error=access_denied")));
        wait = QDeadlineTimer(10000);
        while (!cancelled && !wait.hasExpired())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        CHECK(cancelled && !cancelled->error.isEmpty() && cancelled->access.isEmpty())
    }

    // --- Dropbox: rev in Dropbox-API-Result, "add" / "update" modes, path/not_found and path/conflict.
    {
        FakeHttp server;
        Tokens_ tokens;
        tokens.rotate = true;
        QByteArray stored;
        int rev = 0;
        bool expireOnce = false;
        server.handle = [&](const HttpRequest &r) -> HttpReply
        {
            if (pathOf(r) == "/oauth2/token")
                return tokens.refresh(r);
            if (!tokens.authorized(r) || std::exchange(expireOnce, false))
                return {401, {}, {}};
            const auto arg = QJsonDocument::fromJson(r.headers.value("dropbox-api-arg")).object();
            if (arg["path"] != "/crossterm-sync.ctvault")
                return {400, {}, {}};
            const QByteArray current = "r" + QByteArray::number(rev);
            if (pathOf(r) == "/2/files/download")
            {
                if (!rev)
                    return {409, R"({"error_summary":"path/not_found/.."})", {}};
                return {200, stored, {{"Dropbox-API-Result", R"({"rev":")" + current + R"("})"}}};
            }
            if (pathOf(r) == "/2/files/upload")
            {
                const auto mode = arg["mode"];
                const bool ok = mode.isString() ? (mode.toString() == "add" && !rev)
                                                : (rev && mode.toObject()["update"].toString().toLatin1() == current);
                if (!ok)
                    return {409, R"({"error_summary":"path/conflict/file/.."})", {}};
                stored = r.body;
                ++rev;
                return {200, R"({"rev":"r)" + QByteArray::number(rev) + R"("})", {}};
            }
            return {404, {}, {}};
        };
        auto dropbox = SyncStorage::create(QJsonObject{{"type", "dropbox"}, {"refresh", "R1"}, {"api", server.base()}});
        CHECK(dropbox)
        const auto first = await([&](Done d) { dropbox->read(d); });
        CHECK(first.status == Result::NotFound && first.updated["refresh"] == "R2") // rotated: to be saved
        CHECK(exercise(*dropbox, "dropbox") == 0)
        expireOnce = true; // the access token stops working early: refreshed and retried once
        CHECK(await([&](Done d) { dropbox->read(d); }).status == Result::Ok && tokens.issued == 2)
        auto revoked = SyncStorage::create(QJsonObject{{"type", "dropbox"}, {"refresh", "gone"}, {"api", server.base()}});
        const auto again = await([&](Done d) { revoked->read(d); });
        CHECK(again.status == Result::Failed && again.signIn)
    }

    // --- OneDrive (Microsoft Graph): eTag and If-Match, conflictBehavior=fail, download address without token.
    {
        FakeHttp server;
        Tokens_ tokens;
        QByteArray stored;
        int version = 0;
        bool tokenOnDownload = false;
        const QByteArray item = "/v1.0/me/drive/special/approot:/crossterm-sync.ctvault";
        server.handle = [&](const HttpRequest &r) -> HttpReply
        {
            const auto path = pathOf(r);
            if (path == "/common/oauth2/v2.0/token")
                return tokens.refresh(r);
            if (path == "/download")
            {
                tokenOnDownload = r.headers.contains("authorization");
                return {200, stored, {}};
            }
            if (!tokens.authorized(r))
                return {401, {}, {}};
            const QByteArray etag = "\"{E}," + QByteArray::number(version) + "\"";
            if (r.method == "GET" && path == item)
                return version ? HttpReply{200,
                                           QJsonDocument(QJsonObject{{"eTag", QString::fromLatin1(etag)},
                                                                     {"@microsoft.graph.downloadUrl", server.base() + "/download"}})
                                               .toJson(),
                                           {}}
                               : HttpReply{404, {}, {}};
            if (r.method == "PUT" && path == item + ":/content")
            {
                if (r.path.contains("conflictBehavior=fail") && version)
                    return {409, R"({"error":{"code":"nameAlreadyExists"}})", {}};
                if (r.headers.contains("if-match") && r.headers.value("if-match") != etag)
                    return {412, {}, {}};
                stored = r.body;
                ++version;
                return {200, QJsonDocument(QJsonObject{{"eTag", QString("\"{E},%1\"").arg(version)}}).toJson(), {}};
            }
            return {404, {}, {}};
        };
        auto onedrive = SyncStorage::create(QJsonObject{{"type", "onedrive"}, {"refresh", "R1"}, {"api", server.base()}});
        CHECK(onedrive && exercise(*onedrive, "onedrive") == 0)
        CHECK(!tokenOnDownload)
    }

    // --- Google Drive: hidden app data folder, multipart creation, media update, version compared before writing.
    {
        FakeHttp server;
        Tokens_ tokens;
        QByteArray stored;
        int version = 0;
        bool appData = false;
        server.handle = [&](const HttpRequest &r) -> HttpReply
        {
            const auto path = pathOf(r);
            const QUrlQuery query(QUrl("http://x" + QString::fromLatin1(r.path)));
            if (path == "/token")
                return tokens.refresh(r);
            if (!tokens.authorized(r))
                return {401, {}, {}};
            const QJsonObject file{{"id", "F1"}, {"version", QString::number(version)}};
            if (r.method == "GET" && path == "/drive/v3/files")
            {
                if (query.queryItemValue("spaces") != "appDataFolder")
                    return {400, {}, {}};
                return {200, QJsonDocument(QJsonObject{{"files", version ? QJsonArray{file} : QJsonArray{}}}).toJson(), {}};
            }
            if (r.method == "GET" && path == "/drive/v3/files/F1" && query.queryItemValue("alt") == "media")
                return {200, stored, {}};
            if (r.method == "POST" && path == "/upload/drive/v3/files" && query.queryItemValue("uploadType") == "multipart")
            {
                const auto type = r.headers.value("content-type");
                const auto boundary = "--" + type.mid(type.indexOf("boundary=") + 9);
                appData = r.body.contains(R"("parents":["appDataFolder"])");
                const int start = r.body.indexOf("application/octet-stream\r\n\r\n") + 28;
                stored = r.body.mid(start, r.body.indexOf("\r\n" + boundary + "--") - start);
                ++version;
                return {200, QJsonDocument(QJsonObject{{"id", "F1"}, {"version", QString::number(version)}}).toJson(), {}};
            }
            if (r.method == "PATCH" && path == "/upload/drive/v3/files/F1")
            {
                stored = r.body;
                ++version;
                return {200, QJsonDocument(QJsonObject{{"id", "F1"}, {"version", QString::number(version)}}).toJson(), {}};
            }
            return {404, {}, {}};
        };
        auto drive = SyncStorage::create(QJsonObject{{"type", "gdrive"}, {"refresh", "R1"}, {"api", server.base()}});
        CHECK(drive && exercise(*drive, "google drive") == 0)
        CHECK(appData)
    }

    // --- Settings: tokens never go to another server; no refresh token = sign in first.
    QString error;
    CHECK(!SyncStorage::create(QJsonObject{{"type", "dropbox"}, {"refresh", "R"}, {"api", "https://evil.example"}}, &error))
    CHECK(!SyncStorage::create(QJsonObject{{"type", "onedrive"}, {"api", "http://127.0.0.1:1"}}, &error))
    return 0;
}
