// Synchronization merge: modification stamps on save, union, newer wins, deletions, conflicts kept twice, device key
// paths, saved passwords, old deletion records, and two devices converging on the same data.
#include "syncmerge.h"
#include <QCoreApplication>
#include <QDebug>

#define CHECK(expression)                                                                                                        \
    if (!(expression))                                                                                                           \
    {                                                                                                                            \
        qCritical() << "line" << __LINE__ << #expression;                                                                        \
        return 1;                                                                                                                \
    }

using namespace SyncMerge;

static QJsonObject session(const QString &id, const QString &host, qint64 modified = -1)
{
    QJsonObject s{{"id", id}, {"name", id}, {"protocol", "ssh"}, {"host", host}};
    if (modified >= 0)
        s["modified"] = double(modified);
    return s;
}

static QJsonObject payload(const QJsonArray &sessions, const QJsonObject &deleted = {})
{
    return QJsonObject{{"sessions", sessions}, {"deleted", deleted}};
}

static QJsonObject find(const QJsonObject &p, const QString &id)
{
    for (const auto &v : p["sessions"].toArray())
        if (v.toObject()["id"].toString() == id)
            return v.toObject();
    return {};
}

static qint64 modifiedOf(const QJsonObject &p, const QString &id)
{
    return qint64(find(p, id)["modified"].toDouble());
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const qint64 day = 24LL * 3600 * 1000, t0 = 1'760'000'000'000LL;

    // --- stamp: what save() records.
    {
        QJsonObject before = payload({session("a", "h1", t0), session("b", "h2", t0), session("c", "h3", t0)});
        QJsonObject after = payload({session("a", "h1"), session("b", "changed"), session("d", "new")});
        stamp(before, after, t0 + 5);
        CHECK(modifiedOf(after, "a") == t0)     // unchanged: keeps its time
        CHECK(modifiedOf(after, "b") == t0 + 5) // changed
        CHECK(modifiedOf(after, "d") == t0 + 5) // new
        CHECK(after["deleted"].toObject()["sessions/c"].toDouble() == t0 + 5)
        // Only the device's key path changed: not a modification.
        QJsonObject keyPath = after;
        auto list = keyPath["sessions"].toArray();
        auto a = list[0].toObject();
        a["identity"] = "C:/keys/id";
        list[0] = a;
        keyPath["sessions"] = list;
        stamp(after, keyPath, t0 + 9);
        CHECK(modifiedOf(keyPath, "a") == t0)
        // c comes back (undo): its deletion record goes.
        QJsonObject undo = payload({session("a", "h1"), session("c", "h3")});
        stamp(after, undo, t0 + 10);
        CHECK(!undo["deleted"].toObject().contains("sessions/c") && undo["deleted"].toObject().contains("sessions/b"))
    }

    // --- first synchronization: union; same id, newer copy wins; local order kept, new ones after.
    {
        const auto local = payload({session("a", "local", t0 + 2), session("x", "only here", t0)});
        const auto remote = payload({session("y", "only there", t0), session("a", "remote", t0 + 1)});
        const auto r = merge(local, remote, {}, t0 + 10);
        const auto list = r.payload["sessions"].toArray();
        CHECK(list.size() == 3 && r.conflicts.isEmpty())
        CHECK(list[0].toObject()["id"] == "a" && list[1].toObject()["id"] == "x" && list[2].toObject()["id"] == "y")
        CHECK(find(r.payload, "a")["host"] == "local")
        CHECK(r.base["sessions/a"].toDouble() == t0 + 2 && r.base["sessions/y"].toDouble() == t0)
    }

    // --- the remote copy is newer: it wins, but this device's key path stays.
    {
        auto mine = session("a", "old", t0);
        mine["identity"] = "C:/Users/me/.ssh/id_ed25519";
        auto theirs = session("a", "new", t0 + 5);
        theirs["identity"] = "/home/me/.ssh/id_ed25519";
        const auto r = merge(payload({mine}), payload({theirs}), {}, t0 + 10);
        CHECK(find(r.payload, "a")["host"] == "new")
        CHECK(find(r.payload, "a")["identity"] == "C:/Users/me/.ssh/id_ed25519")
    }

    // --- deletions: a later deletion removes the record; an edit after the deletion keeps it.
    {
        const auto local = payload({session("a", "h", t0), session("b", "edited later", t0 + 20)});
        const auto remote = payload({}, QJsonObject{{"sessions/a", double(t0 + 10)}, {"sessions/b", double(t0 + 10)}});
        const auto r = merge(local, remote, {}, t0 + 30);
        CHECK(find(r.payload, "a").isEmpty() && r.payload["deleted"].toObject().contains("sessions/a"))
        CHECK(!find(r.payload, "b").isEmpty() && !r.payload["deleted"].toObject().contains("sessions/b"))
    }

    // --- both devices edited the same session since the last synchronization: both versions kept.
    {
        const QJsonObject base{{"sessions/a", double(t0)}};
        const auto local = payload({session("a", "edited here", t0 + 5)});
        const auto remote = payload({session("a", "edited there", t0 + 7)});
        const auto r = merge(local, remote, base, t0 + 10);
        const auto list = r.payload["sessions"].toArray();
        CHECK(list.size() == 2 && r.conflicts == QStringList{"a"})
        CHECK(find(r.payload, "a")["host"] == "edited there") // the newer one keeps the id
        const auto copy = list[1].toObject();
        CHECK(copy["host"] == "edited here" && copy["conflictOf"] == "a" && copy["id"] != "a")
        // Only one side edited since the last sync: no conflict, the edit wins.
        const auto quiet = merge(payload({session("a", "same", t0)}), remote, base, t0 + 10);
        CHECK(quiet.conflicts.isEmpty() && quiet.payload["sessions"].toArray().size() == 1 &&
              find(quiet.payload, "a")["host"] == "edited there")
    }

    // --- saved passwords and keys: maps keyed by vault target / session id, newer wins, deletions apply.
    {
        const QJsonObject local{{"passwords", QJsonObject{{"CrossTerm:root@h:22", QJsonObject{{"user", "root"}, {"secret", "old"}, {"modified", double(t0)}}},
                                                          {"CrossTerm:me@x:22", QJsonObject{{"user", "me"}, {"secret", "s"}, {"modified", double(t0)}}}}}};
        const QJsonObject remote{{"passwords", QJsonObject{{"CrossTerm:root@h:22", QJsonObject{{"user", "root"}, {"secret", "new"}, {"modified", double(t0 + 1)}}}}},
                                 {"deleted", QJsonObject{{"passwords/CrossTerm:me@x:22", double(t0 + 2)}}}};
        const auto r = merge(local, remote, {}, t0 + 10);
        const auto passwords = r.payload["passwords"].toObject();
        CHECK(passwords["CrossTerm:root@h:22"].toObject()["secret"] == "new")
        CHECK(!passwords.contains("CrossTerm:me@x:22"))
    }

    // --- deletion records older than their lifetime are forgotten.
    {
        const auto r = merge(payload({}, QJsonObject{{"sessions/old", double(t0)}, {"sessions/recent", double(t0 + 179 * day)}}),
                             payload({}), {}, t0 + 181 * day);
        CHECK(!r.payload["deleted"].toObject().contains("sessions/old") &&
              r.payload["deleted"].toObject().contains("sessions/recent"))
    }

    // --- two devices converge: B merges A's result and both end with the same data; merging again changes nothing.
    {
        const auto a = payload({session("1", "a1", t0 + 1), session("2", "a2", t0 + 9)}, QJsonObject{{"sessions/3", double(t0 + 4)}});
        const auto b = payload({session("2", "b2", t0 + 5), session("3", "b3", t0 + 2), session("4", "b4", t0)});
        const auto onA = merge(a, b, {}, t0 + 10);
        const auto onB = merge(b, onA.payload, {}, t0 + 11);
        CHECK(sameData(onA.payload, onB.payload))
        CHECK(find(onB.payload, "2")["host"] == "a2" && find(onB.payload, "3").isEmpty() && !find(onB.payload, "4").isEmpty())
        const auto again = merge(onA.payload, onB.payload, onA.base, t0 + 12);
        CHECK(sameData(again.payload, onA.payload) && again.conflicts.isEmpty())
    }

    // --- sameData ignores order, modification times and key paths.
    {
        auto withKey = session("a", "h", t0 + 3);
        withKey["identity"] = "/k";
        CHECK(sameData(payload({session("a", "h", t0), session("b", "h")}), payload({session("b", "h", 7), withKey})))
        CHECK(!sameData(payload({session("a", "h")}), payload({session("a", "other")})))
    }
    return 0;
}
