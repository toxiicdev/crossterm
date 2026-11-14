#include "syncengine.h"
#include <QCoreApplication>
#include <QJsonDocument>

namespace SyncEngine
{
namespace
{
QString ui(const char *text)
{
    return QCoreApplication::translate("CrossTerm", text);
}

bool chosen(const QString &key, const QStringList &kinds)
{
    return kinds.contains(key.section('/', 0, 0));
}

// The chosen kinds of a payload (only=true) or all the others (only=false), deletion records included.
QJsonObject part(const QJsonObject &payload, const QStringList &kinds, bool only)
{
    QJsonObject out;
    for (const auto &kind : AllKinds)
        if (kinds.contains(kind) == only && payload.contains(kind))
            out[kind] = payload[kind];
    QJsonObject deleted;
    const auto all = payload["deleted"].toObject();
    for (auto it = all.begin(); it != all.end(); ++it)
        if (chosen(it.key(), kinds) == only)
            deleted[it.key()] = it.value();
    out["deleted"] = deleted;
    return out;
}

QByteArray encode(const QJsonObject &payload)
{
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

Outcome failure(const SyncStorage::Result &r)
{
    Outcome o;
    o.error = r.error;
    o.signIn = r.signIn;
    o.fingerprint = r.fingerprint;
    return o;
}

QString vaultError(SyncVault::Error e)
{
    switch (e)
    {
    case SyncVault::Error::NewerVersion:
        return ui("The sync file was written by a newer CrossTerm: update this one.");
    case SyncVault::Error::NotAVault:
        return ui("The file in the storage is not a CrossTerm sync file.");
    case SyncVault::Error::Corrupt:
        return ui("The sync file is damaged or was modified.");
    default:
        return ui("The password or recovery key is not correct.");
    }
}
} // namespace

Engine::Engine(std::unique_ptr<SyncStorage::Storage> storage, const SyncVault::Keys &keys, const QJsonObject &base,
               const QStringList &kinds)
    : storage(std::move(storage)), keys(keys), base(base), kinds(kinds)
{
}

void Engine::run(const Device &d, std::function<void(const Outcome &)> callback)
{
    device = d;
    done = std::move(callback);
    attempt(3);
}

void Engine::finish(Outcome outcome)
{
    for (auto it = updated.begin(); it != updated.end(); ++it)
        outcome.updated[it.key()] = it.value();
    if (outcome.status == Outcome::Ok)
        outcome.keys = keys;
    if (auto callback = std::exchange(done, nullptr))
        callback(outcome);
}

void Engine::attempt(int left)
{
    if (left == 0)
    {
        Outcome o;
        o.error = ui("The sync file keeps changing on another device: try again in a moment.");
        return finish(o);
    }
    storage->read(
        [this, left](const SyncStorage::Result &read)
        {
            for (auto it = read.updated.begin(); it != read.updated.end(); ++it)
                updated[it.key()] = it.value();
            if (read.status == SyncStorage::Result::Failed)
                return finish(failure(read));
            const QJsonObject local = part(device.collect(), kinds, true);
            QJsonObject remoteAll;
            if (read.status == SyncStorage::Result::Ok)
            {
                // The wrapped key copies come from the file: a password changed on another device stays changed.
                SyncVault::Error error = SyncVault::Error::None;
                auto current = SyncVault::unlockWithDataKey(read.data, keys.dataKey, &error);
                if (!current)
                {
                    Outcome o;
                    o.status = error == SyncVault::Error::WrongPassword ? Outcome::NeedPassword : Outcome::Failed;
                    o.error = vaultError(error);
                    return finish(o);
                }
                keys = *current;
                const auto plain = SyncVault::open(keys, read.data, &error);
                if (!plain)
                {
                    Outcome o;
                    o.error = vaultError(error);
                    return finish(o);
                }
                remoteAll = QJsonDocument::fromJson(*plain).object();
            }
            const auto remote = part(remoteAll, kinds, true);
            const auto merged = SyncMerge::merge(local, remote, base, SyncMerge::nowMs());
            Outcome o;
            o.status = Outcome::Ok;
            o.base = merged.base;
            o.conflicts = merged.conflicts;
            o.changedHere = !SyncMerge::sameData(merged.payload, local);
            if (o.changedHere) // only the chosen kinds: the merge result has (empty) lists for all of them
                device.apply(part(merged.payload, kinds, true));
            const bool upload = read.status == SyncStorage::Result::NotFound ||
                                !SyncMerge::sameData(merged.payload, remote) ||
                                merged.payload["deleted"] != remote["deleted"];
            if (!upload)
                return finish(o);
            // The kinds this device does not sync go back unchanged.
            QJsonObject out = merged.payload;
            const auto others = part(remoteAll, kinds, false);
            for (auto it = others.begin(); it != others.end(); ++it)
                if (it.key() != "deleted")
                    out[it.key()] = it.value();
            QJsonObject deleted = out["deleted"].toObject();
            const auto otherDeleted = others["deleted"].toObject();
            for (auto it = otherDeleted.begin(); it != otherDeleted.end(); ++it)
                deleted[it.key()] = it.value();
            out["deleted"] = deleted;
            storage->write(SyncVault::seal(keys, encode(out)),
                           read.status == SyncStorage::Result::Ok ? read.revision : QString(),
                           [this, left, o](const SyncStorage::Result &written) mutable
                           {
                               for (auto it = written.updated.begin(); it != written.updated.end(); ++it)
                                   updated[it.key()] = it.value();
                               if (written.status == SyncStorage::Result::Conflict)
                                   return attempt(left - 1); // another device wrote meanwhile: read and merge again
                               if (written.status != SyncStorage::Result::Ok)
                                   return finish(failure(written));
                               o.uploaded = true;
                               finish(o);
                           });
        });
}

void setUp(SyncStorage::Storage &storage, const QString &password,
           std::function<void(const Outcome &, const QString &recovery)> done)
{
    storage.read(
        [password, done](const SyncStorage::Result &read)
        {
            Outcome o = failure(read);
            o.updated = read.updated;
            if (read.status == SyncStorage::Result::Failed)
                return done(o, {});
            if (read.status == SyncStorage::Result::NotFound)
            {
                // First device: new keys; the first synchronization writes the file.
                const auto recovery = SyncVault::newRecoveryKey();
                o.status = Outcome::Ok;
                o.keys = SyncVault::create(password, recovery);
                return done(o, recovery);
            }
            SyncVault::Error error = SyncVault::Error::None;
            const auto keys = SyncVault::unlock(read.data, password, &error);
            o.error = keys ? QString() : vaultError(error);
            o.status = keys ? Outcome::Ok : error == SyncVault::Error::WrongPassword ? Outcome::NeedPassword : Outcome::Failed;
            if (keys)
                o.keys = *keys;
            done(o, {});
        });
}

void changePassword(SyncStorage::Storage &storage, const SyncVault::Keys &keys, const QString &password,
                    std::function<void(const Outcome &)> done)
{
    // Up to three tries when another device writes in between.
    auto step = std::make_shared<std::function<void(int)>>();
    // The function refers to itself weakly; the pending callbacks keep it alive and it goes with the last one.
    *step = [&storage, keys, password, done, weak = std::weak_ptr(step)](int left)
    {
        auto self = weak.lock();
        storage.read(
            [&storage, keys, password, done, self, left](const SyncStorage::Result &read)
            {
                if (read.status == SyncStorage::Result::Failed)
                    return done(failure(read));
                if (read.status == SyncStorage::Result::NotFound) // nothing written yet: the first run writes it
                {
                    Outcome o;
                    o.status = Outcome::Ok;
                    o.keys = SyncVault::changePassword(keys, password);
                    return done(o);
                }
                SyncVault::Error error = SyncVault::Error::None;
                const auto current = SyncVault::unlockWithDataKey(read.data, keys.dataKey, &error);
                const auto plain = current ? SyncVault::open(*current, read.data, &error) : std::nullopt;
                if (!plain)
                {
                    Outcome o;
                    o.status = error == SyncVault::Error::WrongPassword ? Outcome::NeedPassword : Outcome::Failed;
                    o.error = vaultError(error);
                    return done(o);
                }
                const auto changed = SyncVault::changePassword(*current, password);
                storage.write(SyncVault::seal(changed, *plain), read.revision,
                              [changed, done, self, left](const SyncStorage::Result &written)
                              {
                                  if (written.status == SyncStorage::Result::Conflict && left > 1)
                                      return (*self)(left - 1);
                                  Outcome o = failure(written);
                                  if (written.status == SyncStorage::Result::Ok)
                                  {
                                      o.status = Outcome::Ok;
                                      o.keys = changed;
                                  }
                                  else if (written.status == SyncStorage::Result::Conflict)
                                      o.error = ui("The sync file keeps changing on another device: try again in a moment.");
                                  done(o);
                              });
            });
    };
    (*step)(3);
}

void recover(SyncStorage::Storage &storage, const QString &recoveryKey, std::function<void(const Outcome &)> done)
{
    storage.read(
        [recoveryKey, done](const SyncStorage::Result &read)
        {
            Outcome o = failure(read);
            if (read.status == SyncStorage::Result::NotFound)
                o.error = ui("There is no sync file in this storage yet.");
            if (read.status != SyncStorage::Result::Ok)
                return done(o);
            SyncVault::Error error = SyncVault::Error::None;
            const auto keys = SyncVault::unlockWithRecovery(read.data, recoveryKey, &error);
            o.error = keys ? QString() : vaultError(error);
            o.status = keys ? Outcome::Ok : error == SyncVault::Error::WrongPassword ? Outcome::NeedPassword : Outcome::Failed;
            if (keys)
                o.keys = *keys;
            done(o);
        });
}
} // namespace SyncEngine
