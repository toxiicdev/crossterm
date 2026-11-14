// Encrypted sync file: round trip, wrong password, recovery key, tampering, password change, kept data key, and a
// file written once (fixture) that must open identically on every platform.
#include "syncvault.h"
#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

#define CHECK(expression)                                                                                                        \
    if (!(expression))                                                                                                           \
    {                                                                                                                            \
        qCritical() << "line" << __LINE__ << #expression;                                                                        \
        return 1;                                                                                                                \
    }

using namespace SyncVault;

// Changes one field of the sync file's JSON.
static QByteArray edited(const QByteArray &file, const std::function<void(QJsonObject &)> &change)
{
    auto o = QJsonDocument::fromJson(file).object();
    change(o);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// Flips one bit of a base64 field.
static QString flipped(const QString &base64, int byte = 0)
{
    auto raw = QByteArray::fromBase64(base64.toLatin1());
    raw[byte] = char(raw[byte] ^ 1);
    return QString::fromLatin1(raw.toBase64());
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const Kdf fast{8192, 1, {}}; // the minimum accepted cost, to keep the test quick
    const QByteArray secret = R"({"sessions":[{"name":"web-01","password":"pàss"}]})";

    // Recovery key: 13 groups of 4 base32 characters, a new one every time.
    const QString recovery = newRecoveryKey();
    CHECK(recovery.size() == 13 * 4 + 12 && recovery.count('-') == 12)
    CHECK(newRecoveryKey() != recovery)

    // Round trip with the password; a fresh nonce makes every copy different.
    const Keys keys = create("correct horse", recovery, fast);
    CHECK(keys.dataKey.size() == 32 && keys.kdf.salt.size() == 16)
    const QByteArray file = seal(keys, secret);
    CHECK(!file.contains("web-01") && !file.contains("correct horse"))
    CHECK(seal(keys, secret) != file)
    Error error = Error::None;
    auto opened = unlock(file, "correct horse", &error);
    CHECK(opened && error == Error::None && opened->dataKey == keys.dataKey)
    CHECK(open(*opened, file, &error) == secret && error == Error::None)

    // Wrong password, and the same text in another Unicode form (NFD) still opens: input is normalized.
    CHECK(!unlock(file, "Correct horse", &error) && error == Error::WrongPassword)
    const Keys accented = create(QString::fromUtf8("caf\xC3\xA9"), recovery, fast);
    CHECK(unlock(seal(accented, secret), QString::fromUtf8("cafe\xCC\x81")))

    // Recovery key: case, spaces and dashes ignored; a wrong one is refused.
    auto recovered = unlockWithRecovery(file, recovery.toLower().remove('-'), &error);
    CHECK(recovered && recovered->dataKey == keys.dataKey && error == Error::None)
    CHECK(!unlockWithRecovery(file, newRecoveryKey(), &error) && error == Error::WrongPassword)
    CHECK(!unlockWithRecovery(file, "not a key", &error) && error == Error::WrongPassword)

    // A device that kept the data key opens new versions without the password; a stale key does not.
    CHECK(unlockWithDataKey(file, keys.dataKey, &error) && error == Error::None)
    CHECK(!unlockWithDataKey(file, QByteArray(32, 'x'), &error) && error == Error::WrongPassword)

    // Tampering: content, nonce, the wrapped key or the KDF settings.
    CHECK(!open(keys, edited(file, [](QJsonObject &o) { o["data"] = flipped(o["data"].toString(), 3); }), &error) &&
          error == Error::Corrupt)
    CHECK(!open(keys, edited(file, [](QJsonObject &o) { o["nonce"] = flipped(o["nonce"].toString()); }), &error) &&
          error == Error::Corrupt)
    CHECK(!unlock(edited(file,
                         [](QJsonObject &o)
                         {
                             auto k = o["keys"].toObject();
                             auto p = k["password"].toObject();
                             p["key"] = flipped(p["key"].toString(), 40);
                             k["password"] = p;
                             o["keys"] = k;
                         }),
                  "correct horse", &error) &&
          error == Error::WrongPassword)
    CHECK(!unlock(edited(file,
                         [](QJsonObject &o)
                         {
                             auto k = o["kdf"].toObject();
                             k["passes"] = 2;
                             o["kdf"] = k;
                         }),
                  "correct horse", &error) &&
          error == Error::WrongPassword)

    // Not a sync file, a newer format, or a cost that would freeze the program.
    CHECK(!unlock("{}", "x", &error) && error == Error::NotAVault)
    CHECK(!unlock("not json", "x", &error) && error == Error::NotAVault)
    CHECK(!unlock(edited(file, [](QJsonObject &o) { o["version"] = 2; }), "x", &error) && error == Error::NewerVersion)
    CHECK(!unlock(edited(file,
                         [](QJsonObject &o)
                         {
                             auto k = o["kdf"].toObject();
                             k["memoryKiB"] = 1 << 22;
                             o["kdf"] = k;
                         }),
                  "correct horse", &error) &&
          error == Error::NotAVault)

    // Password change: same data key, new salt; the new file opens with the new password only, the old file still
    // with the old one, the recovery key with both.
    const Keys changed = changePassword(keys, "battery staple");
    CHECK(changed.dataKey == keys.dataKey && changed.kdf.salt != keys.kdf.salt)
    const QByteArray next = seal(changed, secret);
    CHECK(unlock(next, "battery staple") && !unlock(next, "correct horse"))
    CHECK(unlock(file, "correct horse"))
    CHECK(unlockWithRecovery(next, recovery))

    // Default cost (64 MiB, 3 passes) works too.
    const Keys strong = create("correct horse", recovery);
    CHECK(strong.kdf.memoryKiB == 65536 && strong.kdf.passes == 3)
    CHECK(open(*unlock(seal(strong, secret), "correct horse"), seal(strong, secret)) == secret)

    // Fixture written once on Windows (CROSSTERM_PRINT_SYNC_FIXTURE=1 prints a new one): the same bytes must open on
    // every platform, so a file synced from one system reads on the others.
    const QString zeroRecovery = "AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAA";
    if (!qgetenv("CROSSTERM_PRINT_SYNC_FIXTURE").isEmpty())
    {
        std::printf("%s\n", seal(create("fixture password", zeroRecovery, fast), "CrossTerm sync fixture").constData());
        return 0;
    }
    const QByteArray fixture =
        R"({"cipher":"xchacha20-poly1305","data":"gPErFj5udg6mj6MlCj4rOfngcT/J5BioVuJ2lyB7+8OOu7vYajw=",)"
        R"("format":"crossterm-sync","kdf":{"algorithm":"argon2id","lanes":1,"memoryKiB":8192,"passes":1,)"
        R"("salt":"q7nKF6992RKLIyvN3LTjYQ=="},"keys":{"password":{"key":)"
        R"("kxnjyFe+uLIQIVPFccLPwyr9q2ENPQNooMEYCFYeIuK370UV9QMHYY+w00TkLrcl","nonce":)"
        R"("nmdPu8S3dDpTXyxQF4v9JX34F58x5yRy"},"recovery":{"key":)"
        R"("RUq3T7P0DmsksnK9T4gDgU3UcX6+M7hmYip218pg3reYJs2l5tMYy4QD5jmgW1gB","nonce":)"
        R"("BA0R84TPlYvf5zdEIhXJ0mdikhYSfSuQ"}},"nonce":"aIuc4KWqQQ7j4iAVazbooQf8iD0oMIX2","version":1})";
    auto fixtureKeys = unlock(fixture, "fixture password", &error);
    CHECK(fixtureKeys && open(*fixtureKeys, fixture) == "CrossTerm sync fixture")
    CHECK(unlockWithRecovery(fixture, zeroRecovery))
    return 0;
}
