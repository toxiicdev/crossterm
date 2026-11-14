#pragma once
// End-to-end encrypted file for synchronization: everything is encrypted on this device before it reaches the
// storage (cloud drive, WebDAV, SFTP...), which only ever sees ciphertext.
//
// A random 32-byte data key encrypts the content (XChaCha20-Poly1305, Monocypher). The data key is stored twice,
// wrapped: with a key derived from the sync password (Argon2id) and with the recovery key (32 random bytes shown to
// the user once). Changing the password rewraps the data key only; a device that kept the data key reads new
// versions without asking for the password again.
//
// File: compact JSON
//   {"format":"crossterm-sync","version":1,
//    "kdf":{"algorithm":"argon2id","memoryKiB":65536,"passes":3,"lanes":1,"salt":<b64>},
//    "keys":{"password":{"nonce":<b64>,"key":<b64>},"recovery":{"nonce":<b64>,"key":<b64>}},
//    "cipher":"xchacha20-poly1305","nonce":<b64>,"data":<b64 ciphertext + 16-byte tag>}
#include <QByteArray>
#include <QString>
#include <optional>

namespace SyncVault
{
enum class Error
{
    None,
    NotAVault,     // not a CrossTerm sync file (or a damaged header)
    NewerVersion,  // written by a newer CrossTerm
    WrongPassword, // password or recovery key does not open it
    Corrupt,       // header fine, content modified or damaged (authentication failed)
};

struct Kdf
{
    int memoryKiB = 65536; // 64 MiB
    int passes = 3;
    QByteArray salt; // 16 bytes
};

// Open vault: the data key plus the wrapped copies, carried over unchanged when saving new content.
struct Keys
{
    QByteArray dataKey; // 32 bytes; secret
    Kdf kdf;
    QByteArray passwordNonce, passwordWrapped, recoveryNonce, recoveryWrapped;
};

// New vault keys protected by password and recoveryKey (from newRecoveryKey()).
Keys create(const QString &password, const QString &recoveryKey, const Kdf &kdf = {});
// Same data key, new password (new salt): earlier copies of the file keep opening with the old one.
Keys changePassword(const Keys &keys, const QString &password);
// Recovery key for create(): 32 random bytes as 13 groups of 4 base32 characters ("ABCD-EFGH-...").
QString newRecoveryKey();

// Encrypts plain into a sync file with a fresh nonce.
QByteArray seal(const Keys &keys, const QByteArray &plain);
// Unlocks the data key of a sync file with the password, the recovery key or a data key kept from before.
std::optional<Keys> unlock(const QByteArray &file, const QString &password, Error *error = nullptr);
std::optional<Keys> unlockWithRecovery(const QByteArray &file, const QString &recoveryKey, Error *error = nullptr);
std::optional<Keys> unlockWithDataKey(const QByteArray &file, const QByteArray &dataKey, Error *error = nullptr);
// Decrypts the content of a sync file whose data key is keys.dataKey.
std::optional<QByteArray> open(const Keys &keys, const QByteArray &file, Error *error = nullptr);
} // namespace SyncVault
