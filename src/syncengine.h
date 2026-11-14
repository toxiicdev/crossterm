#pragma once
// One synchronization: read the encrypted file from the storage, merge it with this device's data, apply the result
// here and write it back when it changed. A write refused because another device wrote meanwhile (Conflict) starts
// over, up to three times.
//
// Only the chosen kinds take part ("sessions", "tunnels", "macros", "passwords", "keys"); the others are left as
// they are on both sides, so two devices may sync different subsets of the same file.
#include "syncmerge.h"
#include "syncstorage.h"
#include "syncvault.h"
#include <QStringList>
#include <functional>
#include <memory>

namespace SyncEngine
{
inline const QStringList AllKinds{"sessions", "tunnels", "macros", "passwords", "keys"};

struct Outcome
{
    enum Status
    {
        Ok,
        Failed,       // storage or network error (error, signIn, fingerprint tell more)
        NeedPassword, // the file is encrypted with another key (sync set up again elsewhere): unlock it again
    } status = Failed;
    QString error;
    bool signIn = false;     // cloud account to sign in again
    QString fingerprint;     // FTPS certificate to confirm
    QJsonObject base;        // to keep for the next run
    QJsonObject updated;     // storage settings to save (Gist id, rotated refresh token)
    SyncVault::Keys keys;    // with the wrapped copies of the file just written or read (password changes)
    QStringList conflicts;   // records kept twice
    bool changedHere = false; // this device's data was updated
    bool uploaded = false;
};

struct Device
{
    std::function<QJsonObject()> collect;               // this device's payload (syncmerge.h)
    std::function<void(const QJsonObject &merged)> apply; // called only when the merged data differs from it
};

class Engine
{
public:
    Engine(std::unique_ptr<SyncStorage::Storage> storage, const SyncVault::Keys &keys, const QJsonObject &base,
           const QStringList &kinds = AllKinds);
    void run(const Device &device, std::function<void(const Outcome &)> done);

private:
    void attempt(int left);
    void finish(Outcome outcome);
    std::unique_ptr<SyncStorage::Storage> storage;
    SyncVault::Keys keys;
    QJsonObject base;
    QStringList kinds;
    Device device;
    QJsonObject updated;
    std::function<void(const Outcome &)> done;
};

// First run on a device: the sync file's keys unlocked with the password, or new keys (and a new recovery key in
// *recovery) when the storage has no file yet. A wrong password gives NeedPassword with nothing else.
void setUp(SyncStorage::Storage &storage, const QString &password, std::function<void(const Outcome &, const QString &recovery)> done);
// Same with the recovery key, for a forgotten password.
void recover(SyncStorage::Storage &storage, const QString &recoveryKey, std::function<void(const Outcome &)> done);
// New sync password, written to the file at once (same data key, same content): runs on every device adopt the
// key copies they find in the file, so a change kept only locally would be undone by the next run.
void changePassword(SyncStorage::Storage &storage, const SyncVault::Keys &keys, const QString &password,
                    std::function<void(const Outcome &)> done);
} // namespace SyncEngine
