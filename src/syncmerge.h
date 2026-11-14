#pragma once
// Synchronization data and how two copies of it are merged.
//
// Payload (the plaintext inside the encrypted sync file, and the same shape for the local data):
//   {"sessions":[...], "tunnels":[...], "macros":[...],   records with a UUID "id" and "modified" (ms since 1970)
//    "passwords":{"<vault target>":{"user":..,"secret":..,"modified":..}},
//    "keys":{"<session id>":{"name":"id_ed25519","data":<b64>,"modified":..}},
//    "deleted":{"sessions/<id>":<ms>, "passwords/<target>":<ms>, ...}}       when each record was deleted
//
// Merging works record by record: the newer modification wins, a deletion wins over modifications older than it,
// and a record edited on both devices since the last synchronization keeps both versions (the older one as a copy
// with a new id and "conflictOf"). The "identity" of a session (its key file path) belongs to each device: it
// never makes two copies differ and a local one is never replaced.
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace SyncMerge
{
// Deletion records older than this are dropped: a device offline for longer may bring those records back.
constexpr qint64 TombstoneLifetimeMs = 180LL * 24 * 3600 * 1000;

qint64 nowMs();

// Stamps after against before (both payloads): "modified" = now on every new or changed record, the old time on
// unchanged ones, and a deletion time for every record that disappeared. after["deleted"] keeps before's records,
// minus those of records that came back.
void stamp(const QJsonObject &before, QJsonObject &after, qint64 now);

struct Result
{
    QJsonObject payload;   // merged data, for this device and for the sync file
    QJsonObject base;      // "<kind>/<id>" -> modified of every merged record: the base of the next merge
    QStringList conflicts; // names of the records kept twice
};

// Merges this device's payload with the one read from the sync file. base: Result::base of the previous merge
// ({} the first time: no conflicts, the newer copy wins).
Result merge(const QJsonObject &local, const QJsonObject &remote, const QJsonObject &base, qint64 now);

// True when a and b hold the same records (ignoring order, "modified" and session key paths).
bool sameData(const QJsonObject &a, const QJsonObject &b);
} // namespace SyncMerge
