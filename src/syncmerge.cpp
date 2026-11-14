#include "syncmerge.h"
#include <QDateTime>
#include <QMap>
#include <QSet>
#include <QUuid>

namespace SyncMerge
{
namespace
{
const QStringList Lists{"sessions", "tunnels", "macros"}; // arrays of records with "id"
const QStringList Maps{"passwords", "keys"};              // objects keyed by vault target / session id

// Every record of a payload under "<kind>/<id>", in payload order.
struct Records
{
    QMap<QString, QJsonObject> byKey;
    QStringList order;
};

Records flatten(const QJsonObject &payload)
{
    Records r;
    for (const auto &kind : Lists)
        for (const auto &v : payload[kind].toArray())
        {
            const auto o = v.toObject();
            const auto id = o["id"].toString();
            if (id.isEmpty())
                continue;
            const auto key = kind + "/" + id;
            if (!r.byKey.contains(key))
                r.order << key;
            r.byKey[key] = o;
        }
    for (const auto &kind : Maps)
    {
        const auto map = payload[kind].toObject();
        for (auto it = map.begin(); it != map.end(); ++it)
        {
            const auto key = kind + "/" + it.key();
            r.order << key;
            r.byKey[key] = it.value().toObject();
        }
    }
    return r;
}

// Back to a payload: records in the given order, deletion records as given.
QJsonObject unflatten(const QStringList &order, const QMap<QString, QJsonObject> &records, const QJsonObject &deleted)
{
    QJsonObject payload;
    QMap<QString, QJsonArray> lists;
    QMap<QString, QJsonObject> maps;
    for (const auto &key : order)
    {
        const auto kind = key.section('/', 0, 0), id = key.section('/', 1);
        if (Lists.contains(kind))
            lists[kind].append(records[key]);
        else
            maps[kind][id] = records[key];
    }
    for (const auto &kind : Lists)
        payload[kind] = lists.value(kind);
    for (const auto &kind : Maps)
        payload[kind] = maps.value(kind);
    payload["deleted"] = deleted;
    return payload;
}

qint64 modified(const QJsonObject &record)
{
    return qint64(record["modified"].toDouble());
}

// Content that is compared: everything but the modification time and the device's own key path.
QJsonObject content(QJsonObject record)
{
    record.remove("modified");
    record.remove("identity");
    return record;
}

QJsonObject withModified(QJsonObject record, qint64 time)
{
    record["modified"] = double(time);
    return record;
}
} // namespace

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

void stamp(const QJsonObject &before, QJsonObject &after, qint64 now)
{
    const auto old = flatten(before);
    auto current = flatten(after);
    QJsonObject deleted = before["deleted"].toObject();
    for (const auto &key : current.order)
    {
        auto &record = current.byKey[key];
        const auto previous = old.byKey.find(key);
        if (previous == old.byKey.end() || content(*previous) != content(record))
            record = withModified(record, now);
        else if (previous->contains("modified"))
            record = withModified(record, modified(*previous));
        deleted.remove(key); // back after an undo or an import
    }
    for (const auto &key : old.order)
        if (!current.byKey.contains(key))
            deleted[key] = double(now);
    for (const auto &kind : Lists + Maps)
        after.remove(kind);
    const auto rebuilt = unflatten(current.order, current.byKey, deleted);
    for (auto it = rebuilt.begin(); it != rebuilt.end(); ++it)
        after[it.key()] = it.value();
}

Result merge(const QJsonObject &local, const QJsonObject &remote, const QJsonObject &base, qint64 now)
{
    const auto mine = flatten(local), theirs = flatten(remote);
    // Deletions known on either side; the later time wins. Old ones are forgotten.
    QJsonObject deleted;
    for (const auto &side : {local["deleted"].toObject(), remote["deleted"].toObject()})
        for (auto it = side.begin(); it != side.end(); ++it)
        {
            const double time = it.value().toDouble();
            if (now - qint64(time) <= TombstoneLifetimeMs && time > deleted[it.key()].toDouble())
                deleted[it.key()] = time;
        }
    const auto deletedAt = [&](const QString &key) { return qint64(deleted[key].toDouble()); };

    Result result;
    QMap<QString, QJsonObject> out;
    QStringList order;
    QStringList copies; // conflict copies, listed after everything else
    QStringList keys = mine.order;
    for (const auto &key : theirs.order)
        if (!mine.byKey.contains(key))
            keys << key;
    for (const auto &key : keys)
    {
        const bool here = mine.byKey.contains(key), there = theirs.byKey.contains(key);
        const auto a = mine.byKey.value(key), b = theirs.byKey.value(key);
        const qint64 newest = std::max(here ? modified(a) : 0, there ? modified(b) : 0);
        if (deleted.contains(key) && deletedAt(key) >= newest)
            continue; // deleted after its last modification
        deleted.remove(key); // modified after it was deleted elsewhere: it stays
        if (!there || !here)
        {
            out[key] = here ? a : b;
            order << key;
            continue;
        }
        // On both sides.
        const auto lastSync = qint64(base[key].toDouble());
        const bool bothEdited = content(a) != content(b) && lastSync > 0 && modified(a) > lastSync &&
                                modified(b) > lastSync && Lists.contains(key.section('/', 0, 0));
        QJsonObject winner = modified(b) > modified(a) ? b : a;
        if (a.contains("identity"))
            winner["identity"] = a["identity"]; // the key path of this device
        else
            winner.remove("identity");
        out[key] = winner;
        order << key;
        if (bothEdited)
        {
            // The older edit survives as a copy, so nobody's change is lost.
            QJsonObject copy = modified(b) > modified(a) ? a : b;
            const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            copy["conflictOf"] = key.section('/', 1);
            copy["id"] = id;
            copy["modified"] = double(now);
            const auto copyKey = key.section('/', 0, 0) + "/" + id;
            out[copyKey] = copy;
            copies << copyKey;
            result.conflicts << winner["name"].toString();
        }
    }
    order << copies;
    result.payload = unflatten(order, out, deleted);
    for (const auto &key : order)
        result.base[key] = double(modified(out[key]));
    return result;
}

bool sameData(const QJsonObject &a, const QJsonObject &b)
{
    const auto x = flatten(a), y = flatten(b);
    if (x.byKey.size() != y.byKey.size())
        return false;
    for (auto it = x.byKey.begin(); it != x.byKey.end(); ++it)
        if (!y.byKey.contains(it.key()) || content(it.value()) != content(y.byKey[it.key()]))
            return false;
    return true;
}
} // namespace SyncMerge
