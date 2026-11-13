// Drag from the SFTP browser to the outside: formats, deferred rendering and cache.
// Network part against the test server 127.0.0.1:2222 (reads only); "--network" makes it mandatory.
#include <QtWidgets> // icons.h assumes QMap is already included
#include "../sftp-panel.h"
#include <QTcpSocket>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const QByteArray &what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what.constData());
    failures += !ok;
}

static bool wait(int ms, const std::function<bool()> &done)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms && !done())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return done();
}

// Counts downloads by wrapping the drag data download function.
static int *counted(SftpDragData *data)
{
    auto count = std::make_shared<int>(0);
    data->fetch = [count, inner = data->fetch]
    {
        ++*count;
        return inner();
    };
    return count.get(); // lives as long as data->fetch
}

int main(int argc, char **argv)
{
    // ssh may run this executable as SSH_ASKPASS (the panel points askpass at the running program):
    // answer "no password" instead of starting the test again, which would loop forever.
    if (isAskpassInvocation(argc))
        return 1;
    QApplication app(argc, argv);
    const bool force = app.arguments().contains("--network");

    // --- Without network: unreachable server.
    {
        SftpPanel panel(QJsonObject{{"protocol", "sftp"}, {"host", "127.0.0.1"}, {"port", 1}, {"user", "test"}}, "windows");
        panel.dragTimeout = 20000;
        check(!panel.createDragData({}), "no drag data without names");
        auto *mime = panel.createDragData({"a.txt", "b.txt"});
        auto *data = dynamic_cast<SftpDragData *>(mime);
        check(data != nullptr, "drag data created");
        if (data)
        {
            check(data->formats() == QStringList{"text/uri-list"}, "only text/uri-list advertised");
            check(data->hasUrls() && !data->hasFormat("text/plain"), "hasFormat matches formats");
            check(data->planned.size() == 2 && data->planned[0].isLocalFile() &&
                      data->planned[0].toLocalFile().contains("/CrossTerm-drag/") && data->planned[0].fileName() == "a.txt",
                  "planned paths in temporary drag folder");
            const int *fetches = counted(data);
            check(*fetches == 0, "nothing downloaded before the target asks");
            check(data->urls().isEmpty(), "failed download returns empty list");
            check(data->urls().isEmpty() && *fetches == 1, "failure cached, no second download");
            check(!panel.message().isEmpty(), "failure shown in status: " + panel.message().toUtf8());
            delete mime;
        }
        // Simulated successful download: the cache always returns the same paths.
        auto *fake = static_cast<SftpDragData *>(panel.createDragData({"x"}));
        int calls = 0;
        fake->fetch = [&calls] { return ++calls > 0; };
        check(fake->urls() == fake->planned && fake->urls() == fake->planned && calls == 1, "success cached");
        delete fake;
    }

    // --- With network: test server managed by others, reads only.
    QTcpSocket probe;
    probe.connectToHost("127.0.0.1", 2222);
    const bool reachable = probe.waitForConnected(1500);
    probe.abort();
    // Test server key from the CROSSTERM_TEST_DATA variable (folder with srv/client_key).
    const QString identity = qEnvironmentVariable("CROSSTERM_TEST_DATA") + "/srv/client_key";
    if ((!reachable || !QFileInfo::exists(identity)) && !force)
    {
        std::printf("SKIPPED: network part (127.0.0.1:2222 not reachable or CROSSTERM_TEST_DATA not set)\n");
        return failures ? 1 : 0;
    }
    SftpPanel panel(
        QJsonObject{{"protocol", "sftp"}, {"host", "127.0.0.1"}, {"port", 2222}, {"user", "test"}, {"identity", identity}},
        "windows");
    panel.launchEditor = false;
    auto names = [&]
    {
        QStringList list;
        for (int i = 0; i < panel.view()->topLevelItemCount(); ++i)
            list << panel.view()->topLevelItem(i)->text(0);
        return list;
    };
    wait(3000, [&] { return !panel.currentPath().isEmpty(); });
    wait(10000, [&] { return !panel.busy(); });
    panel.run("/", {}, false);
    const bool listed = wait(15000, [&] { return !panel.busy() && names().contains("README.md") && names().contains("docs"); });
    if (!listed && !force)
    {
        std::printf("SKIPPED: network part (listing failed: %s)\n", qPrintable(panel.message()));
        return failures ? 1 : 0;
    }
    check(listed, "remote listing");
    // Selection and hook of the QTreeWidget used by QDrag.
    for (int i = 0; i < panel.view()->topLevelItemCount(); ++i)
    {
        auto *item = panel.view()->topLevelItem(i);
        item->setSelected(item->text(0) == "README.md" || item->text(0) == "docs" || item->text(0) == "..");
    }
    auto *mime = static_cast<SftpTree *>(panel.view())->dragData();
    auto *data = dynamic_cast<SftpDragData *>(mime);
    check(data && data->planned.size() == 2, "drag data from selection (.. excluded)");
    if (!data)
        return 1;
    const int *fetches = counted(data);
    const auto urls = data->urls();
    check(urls.size() == 2 && *fetches == 1, "download on request");
    QString readme, docs; // folders are listed first
    for (const auto &url : urls)
        (url.fileName() == "docs" ? docs : readme) = url.toLocalFile();
    check(QFileInfo(readme).isFile() && QFileInfo(readme).size() > 0, "README.md downloaded");
    check(QFileInfo(docs).isDir() && QFileInfo::exists(docs + "/a.txt"), "docs folder downloaded recursively");
    check(data->urls() == urls && *fetches == 1, "second request served from cache");
    QDir(QFileInfo(readme).absolutePath()).removeRecursively();
    delete mime;
    return failures ? 1 : 0;
}
