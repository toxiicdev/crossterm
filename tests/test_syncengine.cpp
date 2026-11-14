// Synchronization runs between simulated devices sharing one storage (a folder): first device creates the file,
// the second joins with the password, both converge; kinds a device does not sync are left alone; a write refused
// because another device wrote meanwhile is retried; a password changed on one device stays changed; a file
// recreated elsewhere asks for the password again; the recovery key opens it.
#include "syncengine.h"
#include "syncfakes.h"
#include <QJsonArray>
#include <QTemporaryDir>

using namespace SyncEngine;

// A device: its data, collected and replaced by the engine.
struct FakeDevice
{
    QJsonObject data;
    Device device()
    {
        return Device{[this] { return data; },
                      [this](const QJsonObject &merged)
                      {
                          for (auto it = merged.begin(); it != merged.end(); ++it)
                              data[it.key()] = it.value();
                      }};
    }
    QStringList names(const QString &kind = "sessions") const
    {
        QStringList list;
        for (const auto &v : data[kind].toArray())
            list << v.toObject()["name"].toString();
        list.sort();
        return list;
    }
};

static QJsonObject record(const QString &id, qint64 modified)
{
    return QJsonObject{{"id", id}, {"name", id}, {"modified", double(modified)}};
}

static Outcome sync(const QJsonObject &storage, const SyncVault::Keys &keys, QJsonObject &base, FakeDevice &d,
                    const QStringList &kinds = AllKinds)
{
    Engine engine(SyncStorage::create(storage), keys, base, kinds);
    std::optional<Outcome> result;
    engine.run(d.device(), [&](const Outcome &o) { result = o; });
    QDeadlineTimer deadline(30000);
    while (!result && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (result && result->status == Outcome::Ok)
        base = result->base;
    return result.value_or(Outcome{});
}

static std::pair<Outcome, QString> setUpWith(const QJsonObject &storage, const QString &password)
{
    auto s = SyncStorage::create(storage);
    std::pair<Outcome, QString> result;
    setUp(*s, password, [&](const Outcome &o, const QString &recovery) { result = {o, recovery}; });
    return result; // the folder storage answers at once
}

// The storage of the test, with one more trick: the first write first lets "another device" write.
class Racing : public SyncStorage::Storage
{
public:
    std::unique_ptr<SyncStorage::Storage> inner;
    std::function<void()> before;
    void read(SyncStorage::Done done) override { inner->read(done); }
    void write(const QByteArray &data, const QString &revision, SyncStorage::Done done) override
    {
        if (auto race = std::exchange(before, nullptr))
            race();
        inner->write(data, revision, done);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    const QJsonObject storage{{"type", "folder"}, {"path", dir.path()}};
    const qint64 t = SyncMerge::nowMs();

    // --- First device: new keys, its data uploaded.
    auto [first, recovery] = setUpWith(storage, "sync password");
    CHECK(first.status == Outcome::Ok && !recovery.isEmpty() && first.keys.dataKey.size() == 32)
    FakeDevice a, b;
    a.data = QJsonObject{{"sessions", QJsonArray{record("web-01", t)}}, {"macros", QJsonArray{record("deploy", t)}},
                         {"passwords", QJsonObject{{"CrossTerm:root@web:22", QJsonObject{{"user", "root"}, {"secret", "s3cret"}, {"modified", double(t)}}}}}};
    QJsonObject baseA, baseB;
    auto o = sync(storage, first.keys, baseA, a);
    CHECK(o.status == Outcome::Ok && o.uploaded && !o.changedHere)
    QFile file(dir.filePath(SyncStorage::FileName));
    CHECK(file.open(QIODevice::ReadOnly) && !file.readAll().contains("s3cret")) // encrypted on this device
    file.close();

    // --- Second device joins with the password; it only syncs sessions and passwords.
    CHECK(setUpWith(storage, "wrong").first.status == Outcome::NeedPassword)
    auto [joined, noRecovery] = setUpWith(storage, "sync password");
    CHECK(joined.status == Outcome::Ok && noRecovery.isEmpty() && joined.keys.dataKey == first.keys.dataKey)
    b.data = QJsonObject{{"sessions", QJsonArray{record("db-01", t + 1)}}};
    const QStringList bKinds{"sessions", "passwords"};
    o = sync(storage, joined.keys, baseB, b, bKinds);
    CHECK(o.status == Outcome::Ok && o.changedHere && o.uploaded)
    CHECK(b.names() == (QStringList{"db-01", "web-01"}) && !b.data.contains("macros"))
    CHECK(b.data["passwords"].toObject()["CrossTerm:root@web:22"].toObject()["secret"] == "s3cret")
    // A gets B's session and keeps its macro (B did not touch macros in the file).
    o = sync(storage, first.keys, baseA, a);
    CHECK(o.status == Outcome::Ok && a.names() == (QStringList{"db-01", "web-01"}) && a.names("macros") == QStringList{"deploy"})
    // Nothing new: no write.
    o = sync(storage, first.keys, baseA, a);
    CHECK(o.status == Outcome::Ok && !o.uploaded && !o.changedHere)

    // --- Another device writes between this device's read and write: refused, read again, merged, written.
    {
        a.data["sessions"] = QJsonArray{record("web-01", t), record("db-01", t + 1), record("from-a", t + 5)};
        Racing racing;
        racing.inner = SyncStorage::create(storage);
        racing.before = [&]
        {
            b.data["sessions"] = QJsonArray{record("web-01", t), record("db-01", t + 1), record("from-b", t + 6)};
            sync(storage, joined.keys, baseB, b, bKinds);
        };
        Engine engine(std::unique_ptr<SyncStorage::Storage>(new Racing(std::move(racing))), first.keys, baseA);
        std::optional<Outcome> result;
        engine.run(a.device(), [&](const Outcome &r) { result = r; });
        CHECK(result && result->status == Outcome::Ok && result->uploaded)
        baseA = result->base;
        CHECK(a.names() == (QStringList{"db-01", "from-a", "from-b", "web-01"}))
        sync(storage, joined.keys, baseB, b, bKinds);
        CHECK(b.names() == a.names())
    }

    // --- Password changed on A: B (which kept the data key) still syncs and does not undo the change.
    {
        auto s = SyncStorage::create(storage);
        std::optional<Outcome> changed;
        SyncEngine::changePassword(*s, first.keys, "new password", [&](const Outcome &r) { changed = r; });
        CHECK(changed && changed->status == Outcome::Ok && changed->keys.dataKey == first.keys.dataKey)
        // A syncs with its new keys, B with the ones it kept: neither undoes the change.
        auto edited = record("web-01", t + 20);
        edited["host"] = "10.0.0.9"; // a real change, not only a newer time
        a.data["sessions"] = QJsonArray{edited};
        o = sync(storage, changed->keys, baseA, a);
        CHECK(o.status == Outcome::Ok && o.uploaded)
        b.data["sessions"] = QJsonArray{edited, record("db-01", t + 1), record("b-only", t + 30)};
        o = sync(storage, joined.keys, baseB, b, bKinds);
        CHECK(o.status == Outcome::Ok && o.uploaded)
        CHECK(setUpWith(storage, "new password").first.status == Outcome::Ok)
        CHECK(setUpWith(storage, "sync password").first.status == Outcome::NeedPassword)
    }

    // --- Recovery key opens the file (forgotten password).
    {
        auto s = SyncStorage::create(storage);
        std::optional<Outcome> r;
        SyncEngine::recover(*s, recovery, [&](const Outcome &x) { r = x; });
        CHECK(r && r->status == Outcome::Ok && r->keys.dataKey == first.keys.dataKey)
    }

    // --- Set up again elsewhere from scratch (file removed, new keys): the old key no longer opens it.
    {
        QFile::remove(dir.filePath(SyncStorage::FileName));
        auto [fresh, freshRecovery] = setUpWith(storage, "another");
        FakeDevice c;
        QJsonObject baseC;
        CHECK(sync(storage, fresh.keys, baseC, c).status == Outcome::Ok)
        o = sync(storage, first.keys, baseA, a);
        CHECK(o.status == Outcome::NeedPassword)
    }
    return 0;
}
