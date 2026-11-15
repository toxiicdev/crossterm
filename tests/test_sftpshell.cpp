// Commands that the SFTP browser runs on the server through sh (extract, compress, folder size, copy, permissions),
// run here on local files with the local sh and tools. Skipped where there is no sh (Windows).
#include <QtWidgets> // icons.h assumes QMap is already included
#include "sftp-panel.h"
#include <cstdio>

static int failures = 0;
static void check(bool ok, const QByteArray &what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what.constData());
    failures += !ok;
}

// Runs script as the panel does (sh -c, the script as one word); exit code and standard output.
static std::pair<int, QString> sh(const QString &script)
{
    QProcess p;
    p.start(QStandardPaths::findExecutable("sh"), {"-c", script});
    p.waitForFinished(30000);
    return {p.exitCode(), QString::fromUtf8(p.readAllStandardOutput())};
}

static void write(const QString &file, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QFile f(file);
    f.open(QIODevice::WriteOnly);
    f.write(content);
}
static QByteArray read(const QString &file)
{
    QFile f(file);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // Names: pure functions, everywhere.
    check(sftpCopyName("report.pdf", false, {}) == "report (copy).pdf", "copy name keeps the extension");
    check(sftpCopyName("report.pdf", false, {"report (copy).pdf"}) == "report (copy) 2.pdf", "second copy numbered");
    check(sftpCopyName("site.v2", true, {}) == "site.v2 (copy)", "a folder keeps its dots");
    check(sftpCopyName(".bashrc", false, {}) == ".bashrc (copy)", "a dot file has no extension");
    check(sftpArchiveKind("a.ZIP") == "zip" && sftpArchiveKind("b.tar.gz") == "tar" && sftpArchiveKind("c.tgz") == "tar" &&
              sftpArchiveKind("e.RAR") == "rar" && sftpArchiveKind("f.7z") == "7z" && sftpArchiveKind("d.txt").isEmpty(),
          "archive kinds");
    check(sftpPermissionsScript("/x", "64", {}, 0).isEmpty(), "invalid mode refused");
    check(sftpExtractScript("/x", "a\nb.zip", "zip").isEmpty() && sftpExtractScript("/x", "a.txt", {}).isEmpty(), "control characters refused");

    if (QStandardPaths::findExecutable("sh").isEmpty())
    {
        std::printf("no sh: command tests skipped\n%d failure(s)\n", failures);
        return failures ? 1 : 0;
    }
    QTemporaryDir root;
    // Awkward names: spaces, a quote, $, a leading dash.
    const QString dir = root.filePath("it's $HOME dir");
    write(dir + "/site/index.html", "<h1>hi</h1>");
    write(dir + "/site/css/a.css", "body{}");
    write(dir + "/-notes.txt", "notes");
    const QStringList names{"site", "-notes.txt"};

    // Folder size.
    {
        const auto [code, out] = sh(sftpSizeScript(dir, names));
        check(code == 0 && out.contains("\t./site") && out.contains("\t./-notes.txt"), "du lists each item");
    }
    // Copy: a folder and a file under new names, in the same folder.
    {
        const auto [code, out] = sh(sftpCopyScript({{dir + "/site", dir + "/site (copy)"}, {dir + "/-notes.txt", dir + "/-notes (copy).txt"}}));
        check(code == 0 && read(dir + "/site (copy)/css/a.css") == "body{}" && read(dir + "/-notes (copy).txt") == "notes",
              "cp copies folders and files");
        const auto [failed, _] = sh(sftpCopyScript({{dir + "/missing", dir + "/x"}, {dir + "/-notes.txt", dir + "/y.txt"}}));
        check(failed == 1 && read(dir + "/y.txt") == "notes", "a failed copy is reported, the others still done");
    }
    // Compress and extract, zip and tar.gz.
    for (const bool zip : {true, false})
    {
        const auto label = QByteArray(zip ? "zip" : "tar.gz");
        if (QStandardPaths::findExecutable(zip ? "zip" : "tar").isEmpty() ||
            (zip && QStandardPaths::findExecutable("unzip").isEmpty()))
        {
            std::printf("SKIP: %s not installed\n", label.constData());
            continue;
        }
        const QString archive = zip ? "pack 'x'.zip" : "pack 'x'.tar.gz";
        const auto [code, out] = sh(sftpCompressScript(dir, archive, names, zip));
        check(code == 0 && QFileInfo::exists(dir + "/" + archive), label + ": archive created");
        const QString target = root.filePath(QString("out ") + (zip ? "zip" : "tar"));
        QDir().mkpath(target);
        QFile::copy(dir + "/" + archive, target + "/" + archive);
        write(target + "/-notes.txt", "mine"); // already there: replaced
        const auto [extracted, listing] = sh(sftpExtractScript(target, archive, zip ? "zip" : "tar"));
        check(extracted == 0 && read(target + "/site/css/a.css") == "body{}", label + ": folders extracted");
        check(read(target + "/-notes.txt") == "notes", label + ": existing file replaced");
        if (zip)
            check(listing.section('\n', 0, 0).trimmed().toInt() >= 4, "zip: entry count on the first line");
        check(QFileInfo::exists(target + "/" + archive), label + ": archive kept by Extract here");
        // Extract and delete: the archive goes only after a successful extraction.
        QFile::remove(target + "/site/css/a.css");
        const auto [removed, _] = sh(sftpExtractScript(target, archive, zip ? "zip" : "tar", true));
        check(removed == 0 && read(target + "/site/css/a.css") == "body{}" && !QFileInfo::exists(target + "/" + archive),
              label + ": extract and delete");
        const QString broken = zip ? "broken.zip" : "broken.tar.gz";
        write(target + "/" + broken, "not an archive");
        sh(sftpExtractScript(target, broken, zip ? "zip" : "tar", true));
        check(QFileInfo::exists(target + "/" + broken), label + ": failed extraction keeps the archive");
    }
    // Permissions: files only, folders only, everything.
    {
        // QFile::Permissions: owner in the fourth hex digit, group in the second, others in the first.
        auto mode = [](const QString &file)
        {
            const int p = int(QFileInfo(file).permissions());
            return QString::number((p >> 12) & 7) + QString::number((p >> 4) & 7) + QString::number(p & 7);
        };
        check(sh(sftpPermissionsScript(dir + "/site", "600", {}, 2)).first == 0, "chmod of the files");
        check(mode(dir + "/site/css/a.css") == "600" && mode(dir + "/site/css") != "600", "files only: folders untouched");
        check(sh(sftpPermissionsScript(dir + "/site", "750", {}, 3)).first == 0 && mode(dir + "/site/css") == "750" &&
                  mode(dir + "/site/index.html") == "600",
              "folders only: files untouched");
        check(sh(sftpPermissionsScript(dir + "/site", "755", {}, 1)).first == 0 && mode(dir + "/site/index.html") == "755",
              "content too: files and folders");
        check(sh(sftpPermissionsScript(dir + "/-notes.txt", "640", {}, 0)).first == 0 && mode(dir + "/-notes.txt") == "640",
              "single file, name starting with a dash");
        // chown to the owner it already has: allowed for a normal user.
        const QFileInfo info(dir + "/-notes.txt");
        const auto owner = info.owner() + ":" + info.group();
        check(sh(sftpPermissionsScript(dir + "/site", "755", owner, 1)).first == 0, "chown -R with owner:group");
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
