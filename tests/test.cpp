#include "profiles.h"
#include "credentials.h"
#include "mobaimport.h"
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#define CHECK(expression)                                                                                                        \
    if (!(expression))                                                                                                           \
    {                                                                                                                            \
        qCritical() << #expression;                                                                                              \
        return 1;                                                                                                                \
    }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto imported = parseMoba("[Bookmarks]\r\nSubRep=Caf\xe9\r\n"
                                    "SSH=#100#0%server.example%2222%relay%%-1%-1#\r\n"
                                    "FTP=#99#6%files.example%21%user%password#\r\n"
                                    "[Passwords]\r\nignored=secret\r\n");
    CHECK(imported.warnings.isEmpty())
    CHECK(imported.profiles.size() == 2)
    const auto ssh = imported.profiles[0].toObject();
    CHECK(ssh["folder"].toString() == QString::fromUtf8("Café"))
    CHECK(ssh["x11"].toBool())
    CHECK(ssh["port"].toInt() == 2222)
    CHECK(!imported.profiles[1].toObject().contains("password"))
    CHECK(mergeProfiles(imported.profiles, imported.profiles).size() == 2)
    auto command = sessionCommand(ssh, "linux", true);
    CHECK(command.program == "ssh")
    CHECK(command.arguments.contains("-X"))
    CHECK(command.arguments.last() == "server.example")
    CHECK(command.arguments.contains("ServerAliveInterval=30"))
    {
        auto proxied = ssh;
        proxied["proxyType"] = "socks5";
        proxied["proxyHost"] = "proxy.example";
        proxied["proxyPort"] = 1080;
        const auto viaProxy = sessionCommand(proxied, "windows", false);
        CHECK(viaProxy.error.isEmpty() &&
              viaProxy.arguments.join(' ').contains("--proxy-connect socks5 proxy.example 1080 %h %p"))
        CHECK(viaProxy.arguments.indexOf("--") > viaProxy.arguments.indexOf("-o"))
        proxied["proxyHost"] = "-bad";
        CHECK(!sessionCommand(proxied, "windows", false).error.isEmpty())
        CHECK(sessionCommand(ssh, "windows", true).arguments.contains("-Y"))
        CHECK(sessionCommand(ssh, "macos", true).arguments.contains("-Y"))
    }
    auto invalid = ssh;
    invalid["host"] = "-oProxyCommand";
    CHECK(!sessionCommand(invalid, "linux", false).error.isEmpty())
    auto legacy = parseMoba("Legacy=#109#0%localhost%22%[login]#").profiles[0].toObject();
    CHECK(!legacy["issues"].toArray().isEmpty())
    auto named = parseMoba("[Credentials]\nLOGIN=alice:opaque-test-value\n"
                           "[Bookmarks]\nLegacy=#109#0%localhost%22%[login]#")
                     .profiles;
    CHECK(named[0].toObject()["user"].toString() == "alice")
    CHECK(named[0].toObject()["issues"].toArray().isEmpty())
    const auto repaired = mergeProfiles(QJsonArray{legacy}, named);
    CHECK(repaired.size() == 1)
    CHECK(repaired[0].toObject()["user"].toString() == "alice")
    CHECK(repaired[0].toObject()["id"] == legacy["id"])
    auto rdp = parseMoba("Desk=#91#4%[::1]%3389%user#").profiles[0].toObject();
    command = sessionCommand(rdp, "windows", false);
    CHECK(command.program == "mstsc.exe")
    CHECK(command.arguments.value(0) == "/v:[::1]:3389")
    // Window fit: larger than the screen, negative coordinates, monitor removed.
    const QRect area(0, 0, 1920, 1040);
    CHECK(fitToArea({100, 100, 800, 600}, area) == QRect(100, 100, 800, 600))
    CHECK(fitToArea({-50, -40, 3000, 2000}, area) == area)
    CHECK(fitToArea({3000, 200, 800, 600}, area) == QRect(1120, 200, 800, 600))
    CHECK(fitToArea({-900, 900, 800, 600}, area) == QRect(0, 440, 800, 600))
    CHECK(fitToArea({-2000, 10, 800, 600}, QRect(-1920, 0, 1920, 1080)) == QRect(-1920, 10, 800, 600))
    auto foreign = [](const QByteArray &data, const QString &file, const QString &protocol, int port)
    {
        const auto items = readForeign(data, file).profiles;
        return items.size() == 1 && items[0].toObject()["protocol"].toString() == protocol &&
               items[0].toObject()["port"].toInt() == port;
    };
    CHECK(foreign("[Bookmarks]\nOne=#109#0%host.test%22%user#", "config.mobaconf", "ssh", 22))
    CHECK(foreign("S:\"Hostname\"=host.test\nS:\"Protocol Name\"=SSH2\nD:\"[SSH2] Port\"=000008ae", "crt.ini", "ssh", 2222))
    CHECK(foreign("[Xstart]\nHost=host.test\nMethod=telnet\nUserID=operator", "exceed.xs", "telnet", 23))
    CHECK(foreign(
        "[HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions\\Demo]\n\"HostName\"=\"host.test\"\n\"PortNumber\"=dword:000008ae",
        "putty.reg", "ssh", 2222))
    CHECK(foreign(
        "<Connections><Node Type='Connection' Name='Demo' Hostname='host.test' Protocol='RDP' Port='3389'/></Connections>",
        "confCons.xml", "rdp", 3389))
    CHECK(foreign(
        "<configuration><connection><connection_info><name>Demo</name><hostname>host.test</hostname><protocol>SSH</protocol></connection_info></connection></configuration>",
        "puttycm.dat", "ssh", 22))
    CHECK(foreign(
        "<ArrayOfSessionData><SessionData SessionId='Lab/Demo' Host='host.test' Proto='SSH' Port='2222'/></ArrayOfSessionData>",
        "sessions.xml", "ssh", 2222))
    const auto protectedImport = parseMoba("[Credentials]\nlogin=alice:opaque\n[Passwords]\nssh:bob@other.test=opaque\n"
                                           "[Bookmarks]\nNamed=#109#0%host.test%22%[login]#\nDirect=#109#0%other.test%22%bob#\n"
                                           "Unrelated=#109#0%other.test%22%eve#\n");
    CHECK(protectedImport.profiles.size() == 3)
    CHECK(protectedImport.passwordProfiles.size() == 2)
    CHECK(protectedImport.passwordProfiles.contains(protectedImport.profiles[0].toObject()["id"].toString()))
    CHECK(protectedImport.passwordProfiles.contains(protectedImport.profiles[1].toObject()["id"].toString()))
    // Import: accounts and protected passwords counted, never imported in plaintext.
    const auto secured = parseMoba("[Credentials]\nLOGIN=alice:protected-value\n[Passwords]\nssh22@host=protected\n"
                                   "[Bookmarks]\nS=#109#0%h.example%22%[login]#");
    CHECK(secured.accounts == 1 && secured.secrets == 2)
    CHECK(!QString(QJsonDocument(secured.profiles).toJson()).contains("protected"))
    // MobaXterm master-password encryption: AES-256 (FIPS-197 C.3 vector), CFB8 round trip, wrong master password.
    CHECK(aes256EncryptBlock(QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"),
                             QByteArray::fromHex("00112233445566778899aabbccddeeff"))
              .toHex() == "8ea2b7ca516745bfeafc49904b496089")
    const auto sealed = encryptMobaPassword("s3cret-Pässword!", "master");
    CHECK(sealed != "s3cret-Pässword!" && decryptMobaPassword(sealed, "master") == QString("s3cret-Pässword!"))
    CHECK(!decryptMobaPassword(encryptMobaPassword("a-longer-password-123", "master"), "wrong"))
    // Real key formats: [Passwords] "ssh22:user@host" and [Credentials] "name=user:encrypted"; values stay in memory.
    const auto keyed = parseMoba("[Credentials]\nwork=carol:" + encryptMobaPassword("pw-carol", "m").toLatin1() +
                                 "\n[Passwords]\nssh22:root@10.0.0.5=" + encryptMobaPassword("pw-root", "m").toLatin1() +
                                 "\n[Bookmarks]\nA=#109#0%10.0.0.5%22%root#\nB=#109#0%h.example%22%[work]#\n");
    CHECK(keyed.profiles.size() == 2 && keyed.encryptedPasswords.size() == 2)
    CHECK(decryptMobaPassword(keyed.encryptedPasswords.value(keyed.profiles[0].toObject()["id"].toString()), "m") ==
          QString("pw-root"))
    CHECK(decryptMobaPassword(keyed.encryptedPasswords.value(keyed.profiles[1].toObject()["id"].toString()), "m") ==
          QString("pw-carol"))
    CHECK(!QString(QJsonDocument(keyed.profiles).toJson()).contains("pw-"))
    // Import policies.
    QJsonObject existing{{"id", "keep"},      {"name", "web"},         {"folder", "A"},
                         {"protocol", "ssh"}, {"host", "old.example"}, {"port", 22}};
    QJsonObject incoming{{"id", "new"},       {"name", "WEB"},         {"folder", "a"},
                         {"protocol", "ssh"}, {"host", "new.example"}, {"port", 22}};
    QJsonObject other{{"id", "x"}, {"name", "db"}, {"folder", "A"}, {"protocol", "ssh"}, {"host", "db.example"}, {"port", 22}};
    const auto skipped = applyImport({existing}, {incoming, other}, ImportPolicy::Skip);
    CHECK(skipped.size() == 2 && skipped[0].toObject()["host"] == "old.example")
    const auto replaced = applyImport({existing}, {incoming}, ImportPolicy::Replace);
    CHECK(replaced.size() == 1 && replaced[0].toObject()["host"] == "new.example" && replaced[0].toObject()["id"] == "keep")
    CHECK(applyImport({existing}, {incoming}, ImportPolicy::Merge).size() == 2)
    // Tunnels: correct arguments for each type, ports and destinations validated.
    {
        const QJsonObject server{{"protocol", "ssh"}, {"host", "gw.example"}, {"port", 22}, {"user", "ops"}};
        auto local =
            tunnelCommand(QJsonObject{{"type", "local"}, {"listenPort", 8080}, {"destHost", "db.internal"}, {"destPort", 5432}},
                          server, "linux");
        CHECK(local.error.isEmpty() && local.program == "ssh")
        CHECK(local.arguments.contains("-N") && local.arguments.contains("127.0.0.1:8080:db.internal:5432"))
        CHECK(local.arguments.indexOf("-L") < local.arguments.indexOf("--") && local.arguments.last() == "gw.example")
        auto remote = tunnelCommand(QJsonObject{{"type", "remote"}, {"listenPort", 9000}, {"destHost", "::1"}, {"destPort", 80}},
                                    server, "linux");
        CHECK(remote.arguments.contains("-R") && remote.arguments.contains("9000:[::1]:80"))
        auto dynamic = tunnelCommand(QJsonObject{{"type", "dynamic"}, {"listenPort", 1080}}, server, "linux");
        CHECK(dynamic.arguments.contains("-D") && dynamic.arguments.contains("127.0.0.1:1080"))
        CHECK(
            !tunnelCommand(QJsonObject{{"type", "local"}, {"listenPort", 0}, {"destHost", "x"}, {"destPort", 1}}, server, "linux")
                 .error.isEmpty())
        CHECK(
            !tunnelCommand(QJsonObject{{"type", "local"}, {"listenPort", 80}, {"destHost", "-oProxyCommand=x"}, {"destPort", 1}},
                           server, "linux")
                 .error.isEmpty())
    }
    // Vault: write, read and remove with a dedicated test entry.
    if (vaultAvailable())
    {
        const auto target =
            credentialTarget(QJsonObject{{"user", "unit"}, {"host", "vault.test"}, {"port", 22}}, "CrossTerm-UnitTest");
        CHECK(target == "CrossTerm-UnitTest:unit@vault.test:22")
        CHECK(storeSecret(target, "unit", QString::fromUtf8("pässwörd-✓")))
        CHECK(readSecret(target).value_or(QString()) == QString::fromUtf8("pässwörd-✓"))
        CHECK(removeSecret(target))
        CHECK(!readSecret(target).has_value())
    }
    // CSV: headers in any order, quotes, semicolons, entries to review.
    {
        const auto csv = parseCsv("\xef\xbb\xbfName;Host;Port;User;Protocol;Folder\r\n"
                                  "web;web.example;2200;deploy;ssh;Prod\\Web\r\n"
                                  "\"db; primary\";db.example;;;ssh;\r\n"
                                  "bad;-oProxyCommand=x;22;;ssh;\r\n");
        CHECK(csv.profiles.size() == 3)
        CHECK(csv.profiles[0].toObject()["port"].toInt() == 2200 && csv.profiles[0].toObject()["folder"] == "Prod\\Web")
        CHECK(csv.profiles[1].toObject()["name"] == "db; primary" && csv.profiles[1].toObject()["port"].toInt() == 22)
        CHECK(!csv.profiles[2].toObject()["issues"].toArray().isEmpty())
        CHECK(!parseCsv("host,port\n1.2.3.4,22\n").warnings.isEmpty())
    }
    // Portable mode.
    CHECK(portableRoot("C:/apps/ct", {"CrossTerm.exe", "--portable"}) == "C:/apps/ct/CrossTerm-data")
    CHECK(portableRoot(QString(CROSSTERM_SOURCE_DIR), {"CrossTerm.exe"}).isEmpty())
    RemoteEntry entry;
    CHECK(parseListing("-rw-r--r--    1 alice    staff        2048 Nov 13 12:00 my file.txt", entry))
    CHECK(entry.name == "my file.txt" && entry.size == 2048 && entry.owner == "alice" && !entry.dir)
    CHECK(parseListing("-rw-rw-rw-      0        0              16 Nov 13 12:52 README.md", entry))
    CHECK(entry.name == "README.md" && entry.size == 16 && entry.group == "0")
    CHECK(parseListing("drwxr-xr-x    2 root     root         4096 Jan  3  2025 docs", entry) && entry.dir)
    CHECK(parseListing("lrwxrwxrwx    1 root     root            7 Jan  3  2025 bin -> usr/bin", entry) && entry.name == "bin")
    CHECK(!parseListing("drwxr-xr-x    2 root     root         4096 Jan  3  2025 ..", entry))
    // Windows OpenSSH sftp against an OpenSSH server (Ubuntu 24.04): '*' permission bits and '?' link count.
    CHECK(parseListing("drwx******    ? root     root         4096 Nov 13 18:47 docs", entry) && entry.dir &&
          entry.name == "docs" && entry.owner == "root")
    CHECK(parseListing("-rw-******    ? root     root         3106 Apr 22  2024 .bashrc", entry) && !entry.dir &&
          entry.size == 3106 && entry.name == ".bashrc")
    CHECK(!parseListing("sftp> ls -la", entry))
    // Catalogs: 29 languages besides English, all keys translated, placeholders preserved.
    {
        QFile keysFile(QString(CROSSTERM_SOURCE_DIR) + "/i18n/keys.json");
        CHECK(keysFile.open(QIODevice::ReadOnly))
        const auto keys = QJsonDocument::fromJson(keysFile.readAll()).array();
        CHECK(keys.size() > 150)
        const QStringList codes{"zh_CN", "hi", "es", "ar", "fr", "bn",  "pt_BR", "ru", "ur", "id", "de", "ja", "mr", "te", "tr",
                                "ta",    "vi", "ko", "ha", "sw", "fil", "pa",    "fa", "jv", "it", "gu", "th", "kn", "am"};
        CHECK(codes.size() == 29)
        for (const auto &code : codes)
        {
            QFile file(QString(CROSSTERM_SOURCE_DIR) + "/i18n/" + code + ".json");
            if (!file.open(QIODevice::ReadOnly))
            {
                qCritical() << "missing catalog" << code;
                return 1;
            }
            const auto catalog = QJsonDocument::fromJson(file.readAll()).object();
            int same = 0;
            for (const auto &k : keys)
            {
                const auto key = k.toString(), value = catalog.value(key).toString();
                if (value.trimmed().isEmpty())
                {
                    qCritical() << code << "untranslated:" << key;
                    return 1;
                }
                for (const auto &mark : {"%1", "%2"})
                    if (key.contains(mark) && !value.contains(mark))
                    {
                        qCritical() << code << "placeholder lost:" << key;
                        return 1;
                    }
                if (value == key)
                    ++same;
            }
            if (same > keys.size() / 4)
            {
                qCritical() << code << "too many untranslated values:" << same;
                return 1;
            }
        }
    }
    if (argc == 2)
    {
        QFile local(QString::fromLocal8Bit(argv[1]));
        CHECK(local.open(QIODevice::ReadOnly))
        const auto actual = parseMoba(local.readAll(), "C");
        CHECK(actual.warnings.isEmpty())
        CHECK(actual.profiles.size() == 25)
        int review = 0;
        for (const auto &entry : actual.profiles)
        {
            CHECK(entry.toObject()["protocol"].toString() == "ssh")
            if (!entry.toObject()["issues"].toArray().isEmpty())
                ++review;
        }
        CHECK(review < 24)
        qInfo() << "Verified local MobaXterm 21.3:" << actual.profiles.size() << "SSH profiles," << review << "for review.";
    }
    return 0;
}
