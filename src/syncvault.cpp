#include "syncvault.h"
extern "C" {
#include "monocypher.h"
}
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <cstring>
#include <memory>

namespace SyncVault
{
namespace
{
const char *const FormatName = "crossterm-sync";
constexpr int Version = 1;
constexpr int KeySize = 32, SaltSize = 16, NonceSize = 24, TagSize = 16;
// Accepted Argon2id cost: what this version writes is 64 MiB x 3; a file asking for more than 1 GiB or 10 passes
// is refused rather than freezing the program.
constexpr int MinMemoryKiB = 8192, MaxMemoryKiB = 1 << 20, MaxPasses = 10;
const char *const Base32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

uint8_t *bytes(QByteArray &b)
{
    return reinterpret_cast<uint8_t *>(b.data());
}
const uint8_t *bytes(const QByteArray &b)
{
    return reinterpret_cast<const uint8_t *>(b.constData());
}

void wipe(QByteArray &b)
{
    if (!b.isEmpty())
        crypto_wipe(b.data(), size_t(b.size()));
}

QByteArray random(int size)
{
    QList<quint32> words((size + 3) / 4);
    QRandomGenerator::system()->fillRange(words.data(), words.size()); // OS CSPRNG
    QByteArray out(reinterpret_cast<const char *>(words.constData()), size);
    crypto_wipe(words.data(), size_t(words.size()) * sizeof(quint32));
    return out;
}

// Argon2id(password, salt). The password is normalized (NFC) so that the same text typed on different systems
// gives the same key.
QByteArray derive(const QString &password, const Kdf &kdf)
{
    QByteArray pass = password.normalized(QString::NormalizationForm_C).toUtf8();
    QByteArray key(KeySize, '\0');
    const crypto_argon2_config config{CRYPTO_ARGON2_ID, uint32_t(kdf.memoryKiB), uint32_t(kdf.passes), 1};
    const crypto_argon2_inputs inputs{bytes(pass), bytes(kdf.salt), uint32_t(pass.size()), uint32_t(kdf.salt.size())};
    const size_t area = size_t(kdf.memoryKiB) * 1024;
    std::unique_ptr<uint8_t[]> work(new uint8_t[area]);
    crypto_argon2(bytes(key), KeySize, work.get(), config, inputs, crypto_argon2_no_extras);
    crypto_wipe(work.get(), area);
    wipe(pass);
    return key;
}

// Additional authenticated data: binds each ciphertext to its role (and the password copy to its KDF settings).
QByteArray dataAd()
{
    return QByteArray(FormatName) + "/1/data";
}
QByteArray recoveryAd()
{
    return QByteArray(FormatName) + "/1/recovery";
}
QByteArray passwordAd(const Kdf &kdf)
{
    return QByteArray(FormatName) + "/1/password/argon2id/" + QByteArray::number(kdf.memoryKiB) + "/" +
           QByteArray::number(kdf.passes) + "/1/" + kdf.salt.toBase64();
}

// XChaCha20-Poly1305: ciphertext followed by the 16-byte tag.
QByteArray lock(const QByteArray &key, const QByteArray &nonce, const QByteArray &ad, const QByteArray &plain)
{
    QByteArray out(plain.size() + TagSize, '\0');
    crypto_aead_lock(bytes(out), bytes(out) + plain.size(), bytes(key), bytes(nonce), bytes(ad), size_t(ad.size()),
                     bytes(plain), size_t(plain.size()));
    return out;
}

std::optional<QByteArray> unlockBytes(const QByteArray &key, const QByteArray &nonce, const QByteArray &ad,
                                      const QByteArray &sealed)
{
    if (key.size() != KeySize || sealed.size() < TagSize)
        return std::nullopt;
    const qsizetype size = sealed.size() - TagSize;
    QByteArray plain(size, '\0');
    if (crypto_aead_unlock(bytes(plain), bytes(sealed) + size, bytes(key), bytes(nonce), bytes(ad), size_t(ad.size()),
                           bytes(sealed), size_t(size)) != 0)
        return std::nullopt;
    return plain;
}

// Recovery key text to its 32 bytes. Case, spaces and dashes are ignored; 0 and 1 are read as O and I.
std::optional<QByteArray> decodeRecovery(const QString &text)
{
    QByteArray digits;
    for (QChar c : text.toUpper())
    {
        if (c == '-' || c.isSpace())
            continue;
        if (c == '0')
            c = 'O';
        else if (c == '1')
            c = 'I';
        const char *at = c.unicode() < 128 ? std::strchr(Base32, char(c.unicode())) : nullptr;
        if (!at || !*at)
            return std::nullopt;
        digits += char(at - Base32);
    }
    if (digits.size() != 52) // 256 bits in 52 digits of 5 bits, the last 4 bits zero
        return std::nullopt;
    QByteArray out;
    quint32 buffer = 0;
    int bits = 0;
    for (const char d : digits)
    {
        buffer = (buffer << 5) | quint32(d);
        bits += 5;
        if (bits >= 8)
        {
            bits -= 8;
            out += char((buffer >> bits) & 0xff);
        }
    }
    if (out.size() != KeySize || (buffer & ((1u << bits) - 1)) != 0)
        return std::nullopt;
    return out;
}

struct Parsed
{
    Kdf kdf;
    QByteArray passwordNonce, passwordWrapped, recoveryNonce, recoveryWrapped, nonce, data;
};

std::optional<Parsed> parse(const QByteArray &file, Error *error)
{
    const auto fail = [error](Error e) -> std::optional<Parsed>
    {
        if (error)
            *error = e;
        return std::nullopt;
    };
    const auto doc = QJsonDocument::fromJson(file);
    const auto o = doc.object();
    if (!doc.isObject() || o["format"].toString() != FormatName || !o["version"].isDouble())
        return fail(Error::NotAVault);
    if (o["version"].toInt() > Version)
        return fail(Error::NewerVersion);
    const auto b64 = [](const QJsonValue &v)
    {
        const auto r = QByteArray::fromBase64Encoding(v.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        return r ? *r : QByteArray();
    };
    const auto kdf = o["kdf"].toObject();
    const auto keys = o["keys"].toObject();
    const auto password = keys["password"].toObject(), recovery = keys["recovery"].toObject();
    Parsed p;
    p.kdf.memoryKiB = kdf["memoryKiB"].toInt();
    p.kdf.passes = kdf["passes"].toInt();
    p.kdf.salt = b64(kdf["salt"]);
    p.passwordNonce = b64(password["nonce"]);
    p.passwordWrapped = b64(password["key"]);
    p.recoveryNonce = b64(recovery["nonce"]);
    p.recoveryWrapped = b64(recovery["key"]);
    p.nonce = b64(o["nonce"]);
    p.data = b64(o["data"]);
    if (o["version"].toInt() != Version || kdf["algorithm"].toString() != "argon2id" || kdf["lanes"].toInt() != 1 ||
        o["cipher"].toString() != "xchacha20-poly1305" || p.kdf.memoryKiB < MinMemoryKiB ||
        p.kdf.memoryKiB > MaxMemoryKiB || p.kdf.passes < 1 || p.kdf.passes > MaxPasses ||
        p.kdf.salt.size() != SaltSize || p.passwordNonce.size() != NonceSize ||
        p.passwordWrapped.size() != KeySize + TagSize || p.recoveryNonce.size() != NonceSize ||
        p.recoveryWrapped.size() != KeySize + TagSize || p.nonce.size() != NonceSize || p.data.size() < TagSize)
        return fail(Error::NotAVault);
    return p;
}

Keys keysOf(const Parsed &p, const QByteArray &dataKey)
{
    return Keys{dataKey, p.kdf, p.passwordNonce, p.passwordWrapped, p.recoveryNonce, p.recoveryWrapped};
}

void setError(Error *error, Error value)
{
    if (error)
        *error = value;
}

void wrapPassword(Keys &keys, const QString &password)
{
    keys.kdf.salt = random(SaltSize);
    QByteArray kek = derive(password, keys.kdf);
    keys.passwordNonce = random(NonceSize);
    keys.passwordWrapped = lock(kek, keys.passwordNonce, passwordAd(keys.kdf), keys.dataKey);
    wipe(kek);
}
} // namespace

QString newRecoveryKey()
{
    QByteArray key = random(KeySize);
    QString text;
    quint32 buffer = 0;
    int bits = 0;
    for (const char byte : key)
    {
        buffer = (buffer << 8) | quint8(byte);
        bits += 8;
        while (bits >= 5)
        {
            bits -= 5;
            text += QChar(Base32[(buffer >> bits) & 31]);
        }
    }
    text += QChar(Base32[(buffer << (5 - bits)) & 31]); // the last 1 bit padded with zeros
    wipe(key);
    for (int at = text.size() - 4; at > 0; at -= 4)
        text.insert(at, '-');
    return text;
}

Keys create(const QString &password, const QString &recoveryKey, const Kdf &kdf)
{
    Keys keys;
    keys.kdf = kdf;
    keys.dataKey = random(KeySize);
    wrapPassword(keys, password);
    auto recovery = decodeRecovery(recoveryKey);
    Q_ASSERT(recovery); // always a key from newRecoveryKey()
    keys.recoveryNonce = random(NonceSize);
    keys.recoveryWrapped = lock(recovery.value_or(random(KeySize)), keys.recoveryNonce, recoveryAd(), keys.dataKey);
    if (recovery)
        wipe(*recovery);
    return keys;
}

Keys changePassword(const Keys &keys, const QString &password)
{
    Keys changed = keys;
    wrapPassword(changed, password);
    return changed;
}

QByteArray seal(const Keys &keys, const QByteArray &plain)
{
    const QByteArray nonce = random(NonceSize);
    const QJsonObject file{
        {"format", FormatName},
        {"version", Version},
        {"kdf", QJsonObject{{"algorithm", "argon2id"},
                            {"memoryKiB", keys.kdf.memoryKiB},
                            {"passes", keys.kdf.passes},
                            {"lanes", 1},
                            {"salt", QString::fromLatin1(keys.kdf.salt.toBase64())}}},
        {"keys", QJsonObject{{"password", QJsonObject{{"nonce", QString::fromLatin1(keys.passwordNonce.toBase64())},
                                                      {"key", QString::fromLatin1(keys.passwordWrapped.toBase64())}}},
                             {"recovery", QJsonObject{{"nonce", QString::fromLatin1(keys.recoveryNonce.toBase64())},
                                                      {"key", QString::fromLatin1(keys.recoveryWrapped.toBase64())}}}}},
        {"cipher", "xchacha20-poly1305"},
        {"nonce", QString::fromLatin1(nonce.toBase64())},
        {"data", QString::fromLatin1(lock(keys.dataKey, nonce, dataAd(), plain).toBase64())}};
    return QJsonDocument(file).toJson(QJsonDocument::Compact);
}

std::optional<Keys> unlock(const QByteArray &file, const QString &password, Error *error)
{
    const auto p = parse(file, error);
    if (!p)
        return std::nullopt;
    QByteArray kek = derive(password, p->kdf);
    auto dataKey = unlockBytes(kek, p->passwordNonce, passwordAd(p->kdf), p->passwordWrapped);
    wipe(kek);
    if (!dataKey)
    {
        setError(error, Error::WrongPassword);
        return std::nullopt;
    }
    setError(error, Error::None);
    return keysOf(*p, *dataKey);
}

std::optional<Keys> unlockWithRecovery(const QByteArray &file, const QString &recoveryKey, Error *error)
{
    const auto p = parse(file, error);
    if (!p)
        return std::nullopt;
    auto kek = decodeRecovery(recoveryKey);
    auto dataKey = kek ? unlockBytes(*kek, p->recoveryNonce, recoveryAd(), p->recoveryWrapped) : std::nullopt;
    if (kek)
        wipe(*kek);
    if (!dataKey)
    {
        setError(error, Error::WrongPassword);
        return std::nullopt;
    }
    setError(error, Error::None);
    return keysOf(*p, *dataKey);
}

std::optional<Keys> unlockWithDataKey(const QByteArray &file, const QByteArray &dataKey, Error *error)
{
    const auto p = parse(file, error);
    if (!p)
        return std::nullopt;
    // A kept key no longer opens the file when the sync was set up again from scratch: ask for the password.
    auto plain = unlockBytes(dataKey, p->nonce, dataAd(), p->data);
    if (!plain)
    {
        setError(error, Error::WrongPassword);
        return std::nullopt;
    }
    wipe(*plain);
    setError(error, Error::None);
    return keysOf(*p, dataKey);
}

std::optional<QByteArray> open(const Keys &keys, const QByteArray &file, Error *error)
{
    const auto p = parse(file, error);
    if (!p)
        return std::nullopt;
    auto plain = unlockBytes(keys.dataKey, p->nonce, dataAd(), p->data);
    setError(error, plain ? Error::None : Error::Corrupt);
    return plain;
}
} // namespace SyncVault
