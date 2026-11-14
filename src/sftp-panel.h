#pragma once
// Sidebar SFTP browser: each operation is an "sftp -b -" batch of the OpenSSH client,
// with StrictHostKeyChecking=yes (the host key must first be confirmed in the SSH terminal).
#include "icons.h"
#include "i18n.h"
#include "profiles.h"
#include "askpass.h"
#include <QtWidgets>
#include <QDesktopServices>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <memory>
#include <QFileSystemWatcher>
#ifdef Q_OS_WIN
#include <shlobj.h> // SHOpenWithDialog
#endif

static QString sftpQuote(QString text)
{
    if (text.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))
        return {};
    return "\"" + text.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
}

// Drag to File Explorer with deferred rendering: files are downloaded only when the
// drop target actually requests the data (on Windows: OLE IDataObject::GetData -> retrieveData).
class SftpDragData : public QMimeData
{
public:
    QList<QUrl> planned;                // final local paths (temp folder + name)
    std::function<bool()> fetch;        // blocking download; true on success
    mutable std::optional<bool> result; // cache: a single download per drag
    QStringList formats() const override { return {"text/uri-list"}; }
    bool hasFormat(const QString &mime) const override { return mime == "text/uri-list"; }

protected:
    QVariant retrieveData(const QString &mime, QMetaType) const override
    {
        if (mime != "text/uri-list")
            return {};
        // While hovering over the drop target (button still pressed) OLE queries the formats
        // (QueryGetData calls urls()): return the expected paths without downloading anything.
#ifdef Q_OS_WIN
        const bool held = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000;
#else
        const bool held = QGuiApplication::mouseButtons() != Qt::NoButton;
#endif
        if (!result && !held)
            result = fetch && fetch();
        QVariantList urls;
        if (!result || *result)
            for (const auto &url : planned)
                urls << url;
        return urls;
    }
};

// Starts the QDrag with the panel data instead of the models internal format.
class SftpTree : public QTreeWidget
{
public:
    std::function<QMimeData *()> dragData;

protected:
    void startDrag(Qt::DropActions) override
    {
        auto *data = dragData ? dragData() : nullptr;
        if (!data)
            return;
        auto *drag = new QDrag(this);
        drag->setMimeData(data);
        drag->setPixmap(icon("file").pixmap(32));
        drag->exec(Qt::CopyAction, Qt::CopyAction);
    }
};

class SftpPanel : public QWidget
{
    QLineEdit *path = new QLineEdit(".");
    SftpTree *files = new SftpTree;
    QLabel *status = new QLabel;
    QProcess *process = hideConsoleWindow(new QProcess(this));
    Command command;
    QString listed, openAfter, target;
    Texts texts;

    QStringList selectedNames(bool *anyDir = nullptr) const
    {
        QStringList names;
        for (auto *item : files->selectedItems())
        {
            if (item->text(0) == "..")
                continue;
            names << item->text(0);
            if (anyDir && item->data(0, Qt::UserRole).toBool())
                *anyDir = true;
        }
        return names;
    }
    bool isDir(const QString &name) const
    {
        for (int i = 0; i < files->topLevelItemCount(); ++i)
            if (files->topLevelItem(i)->text(0) == name)
                return files->topLevelItem(i)->data(0, Qt::UserRole).toBool();
        return false;
    }
    QString join(const QString &name) const { return listed.endsWith('/') ? listed + name : listed + "/" + name; }

    // --- Transfer queue: one sftp process per item, up to "sftp/parallel" at a time (Settings), with real progress.
    struct Transfer
    {
        QString directory, name, local, state; // state: queued, running, done, failed, cancelled
        bool upload = false, folder = false, resume = false, open = false, cancelled = false;
        int openMode = 0; // 0 default program, 1 "Open with" chooser, 2 text editor
        qint64 size = -1;
        QTreeWidgetItem *item = nullptr;
        QProgressBar *bar = nullptr;
        QProcess *process = nullptr; // while running
    };
    QList<std::shared_ptr<Transfer>> transfers;
    QTimer *ticker = new QTimer(this);
    static int parallelTransfers() { return std::clamp(QSettings().value("sftp/parallel", 5).toInt(), 1, 20); }
    int runningTransfers() const
    {
        return int(std::count_if(transfers.begin(), transfers.end(), [](const auto &t) { return t->state == "running"; }));
    }
    QWidget *queueBox = new QWidget;
    QTreeWidget *queueView = new QTreeWidget;
    // Files opened for editing: every local save is uploaded back to the server.
    QFileSystemWatcher *watcher = new QFileSystemWatcher(this);
    QHash<QString, std::pair<QString, QString>> edited; // local -> (remote folder, name)
    QSet<QString> pendingEdits;
    // Recursive delete: walk the folders, then a single rm/rmdir batch.
    QProcess *scanner = hideConsoleWindow(new QProcess(this));
    QStringList scanQueue, foundFiles, foundDirs;
    // Follow terminal folder.
    QCheckBox *follow = new QCheckBox;
    QTimer *followTimer = new QTimer(this);
    QString home, followed;

    void startSftp(QProcess *proc, const QByteArray &batch, bool interactive)
    {
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto &entry : askpassEnvironment(target, interactive))
            environment.insert(entry.section('=', 0, 0), entry.section('=', 1));
        proc->setProcessEnvironment(environment);
        proc->setProperty("batch", batch);
        proc->start(QStandardPaths::findExecutable(command.program),
                    QStringList{"-o", "BatchMode=no", "-b", "-", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=10"} +
                        command.arguments);
    }

    void scanNext()
    {
        if (scanQueue.isEmpty())
        {
            QStringList ops;
            for (const auto &file : foundFiles)
                ops << "rm " + sftpQuote(file);
            auto dirs = foundDirs;
            std::sort(dirs.begin(), dirs.end(), [](const QString &a, const QString &b) { return a.count('/') > b.count('/'); });
            for (const auto &dir : dirs)
                ops << "rmdir " + sftpQuote(dir);
            foundFiles.clear();
            foundDirs.clear();
            run(listed, ops);
            return;
        }
        const auto dir = scanQueue.takeFirst();
        scanner->setProperty("dir", dir);
        startSftp(scanner, ("cd " + sftpQuote(dir) + "\nls -la\n").toUtf8(), true);
    }

    void followTick()
    {
        if (!folderSource || busy() || listed.isEmpty())
            return;
        auto dir = folderSource();
        if (dir.startsWith('~'))
        {
            if (home.isEmpty())
                return;
            dir = home + "/" + dir.mid(1);
        }
        dir = QDir::cleanPath(dir);
        if (dir.isEmpty() || dir == followed || dir == QDir::cleanPath(listed))
            return;
        followed = dir;
        run(dir, {}, false);
    }

    // Bytes read by the sftp process (upload estimate: sftp reads the file while sending it).
    static qint64 bytesRead(qint64 pid)
    {
#ifdef Q_OS_WIN
        qint64 result = -1;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid)))
        {
            IO_COUNTERS counters{};
            if (GetProcessIoCounters(h, &counters))
                result = qint64(counters.ReadTransferCount);
            CloseHandle(h);
        }
        return result;
#elif defined(Q_OS_LINUX)
        QFile io(QString("/proc/%1/io").arg(pid));
        if (!io.open(QIODevice::ReadOnly))
            return -1;
        for (const auto &line : io.readAll().split('\n'))
        {
            if (line.startsWith("rchar:"))
                return line.mid(6).trimmed().toLongLong();
        }
        return -1;
#else
        Q_UNUSED(pid);
        return -1; // ponytail: no per-process I/O counter on macOS; indeterminate bar for uploads
#endif
    }

    void showState(const std::shared_ptr<Transfer> &t)
    {
        static const QHash<QString, const char *> labels{{"queued", N("Queued")},
                                                         {"running", N("Transferring")},
                                                         {"done", N("Completed")},
                                                         {"failed", N("Failed")},
                                                         {"cancelled", N("Cancelled")}};
        if (t->state != "failed")
            t->item->setText(3, L(labels.value(t->state, "")));
        t->item->setIcon(0, icon(t->upload ? "upload" : "download", 16));
        if (t->state == "done")
            t->bar->setRange(0, 100), t->bar->setValue(100);
    }

    void pump()
    {
        int running = runningTransfers();
        for (const auto &t : transfers)
        {
            if (running >= parallelTransfers())
                break;
            if (t->state != "queued")
                continue;
            const auto program = QStandardPaths::findExecutable(command.program);
            const auto directory = sftpQuote(t->directory), name = sftpQuote(t->name),
                       local = sftpQuote(QDir::fromNativeSeparators(t->local));
            if (program.isEmpty() || directory.isEmpty() || name.isEmpty() || local.isEmpty() || !command.error.isEmpty())
            {
                t->state = "failed";
                t->item->setText(3,
                                 L("Failed") + ": " + (command.error.isEmpty() ? L("Invalid path.") : describe(command.error)));
                continue;
            }
            // reget/reput resume an interrupted transfer from where it stopped.
            if (!t->upload && watcher->files().contains(t->local))
                watcher->removePath(t->local);
            const QString verb = t->upload ? (t->resume ? "reput" : "put") : (t->resume ? "reget" : "get");
            const QString flags = t->folder ? " -r" : "";
            const auto batch =
                "cd " + directory + "\n" + verb + flags + " " + (t->upload ? local + " " + name : name + " " + local) + "\n";
            auto environment = QProcessEnvironment::systemEnvironment();
            for (const auto &entry : askpassEnvironment(target, true))
                environment.insert(entry.section('=', 0, 0), entry.section('=', 1));
            auto *mover = hideConsoleWindow(new QProcess(this));
            mover->setProcessEnvironment(environment);
            mover->setProperty("batch", batch.toUtf8());
            t->process = mover;
            watchTransfer(t);
            ++running;
            t->state = "running";
            t->cancelled = false;
            if (t->folder || (t->upload && bytesRead(QCoreApplication::applicationPid()) < 0))
                t->bar->setRange(0, 0);
            else
                t->bar->setRange(0, 100);
            showState(t);
            mover->start(program, QStringList{"-o", "BatchMode=no", "-b", "-", "-o", "StrictHostKeyChecking=yes", "-o",
                                              "ConnectTimeout=10"} +
                                      command.arguments);
            ticker->start(150);
        }
    }

    void tick()
    {
        for (const auto &t : transfers)
        {
            if (t->state != "running" || !t->process || t->size <= 0 || t->folder)
                continue;
            qint64 done = -1;
            if (t->upload)
                done = t->process->state() == QProcess::Running ? bytesRead(t->process->processId()) : -1;
            else
                done = QFileInfo(t->local).size();
            if (done >= 0)
                t->bar->setValue(int(std::min<qint64>(99, done * 100 / t->size)));
        }
    }

    std::shared_ptr<Transfer> selectedTransfer() const
    {
        for (const auto &t : transfers)
        {
            if (t->item == queueView->currentItem())
                return t;
        }
        return nullptr;
    }

public:
    void enqueue(const QString &directory, const QString &name, const QString &local, bool upload, bool folder, qint64 size,
                 bool open = false)
    {
        auto t = std::make_shared<Transfer>();
        t->directory = directory;
        t->name = name;
        t->local = local;
        t->upload = upload;
        t->folder = folder;
        t->size = size;
        t->open = open;
        t->state = "queued";
        t->item = new QTreeWidgetItem(queueView, {name, size >= 0 && !folder ? QLocale().formattedDataSize(size) : QString()});
        t->item->setToolTip(0, QDir::toNativeSeparators(local));
        t->bar = new QProgressBar;
        t->bar->setRange(0, 100);
        t->bar->setValue(0);
        t->bar->setTextVisible(true);
        t->bar->setMaximumHeight(14);
        queueView->setItemWidget(t->item, 2, t->bar);
        transfers << t;
        showState(t);
        queueBox->show();
        pump();
    }
    void cancelTransfer(const std::shared_ptr<Transfer> &t)
    {
        if (!t || (t->state != "queued" && t->state != "running"))
            return;
        if (t->state == "running")
        {
            t->cancelled = true;
            if (t->process)
                t->process->kill();
        }
        else
        {
            t->state = "cancelled";
            showState(t);
        }
    }
    void resumeTransfer(const std::shared_ptr<Transfer> &t)
    {
        if (!t || (t->state != "failed" && t->state != "cancelled"))
            return;
        t->resume = !t->folder;
        t->state = "queued";
        showState(t);
        pump();
    }
    // For tests: state and progress of the i-th queue item.
    QString transferState(int i) const { return i < transfers.size() ? transfers[i]->state : QString(); }
    int transferProgress(int i) const { return i < transfers.size() ? transfers[i]->bar->value() : -1; }
    int transferCount() const { return int(transfers.size()); }
    void cancelTransfer(int i) { cancelTransfer(transfers.value(i)); }
    void resumeTransfer(int i) { resumeTransfer(transfers.value(i)); }
    bool transfersIdle() const
    {
        return std::none_of(transfers.begin(), transfers.end(),
                            [](const auto &t) { return t->state == "queued" || t->state == "running"; });
    }

private:
    void buildQueue(QVBoxLayout *layout)
    {
        auto *box = new QVBoxLayout(queueBox);
        box->setContentsMargins(0, 4, 0, 0);
        box->setSpacing(2);
        auto *title = texts.text(new QLabel, N("Transfers"));
        title->setObjectName("empty");
        box->addWidget(title);
        queueView->setRootIsDecorated(false);
        queueView->setColumnCount(4);
        queueView->setMaximumHeight(150);
        queueView->setColumnWidth(0, 110);
        queueView->setColumnWidth(1, 60);
        queueView->setColumnWidth(2, 90);
        box->addWidget(queueView);
        auto *row = new QHBoxLayout;
        auto *cancel = texts.text(new QPushButton, N("Cancel"));
        auto *resume = texts.text(new QPushButton, N("Resume"));
        auto *clear = texts.text(new QPushButton, N("Clear finished"));
        for (auto *b : {cancel, resume, clear})
        {
            b->setStyleSheet("padding: 2px 8px;");
            row->addWidget(b);
        }
        row->addStretch();
        box->addLayout(row);
        connect(cancel, &QPushButton::clicked, this, [this] { cancelTransfer(selectedTransfer()); });
        connect(resume, &QPushButton::clicked, this, [this] { resumeTransfer(selectedTransfer()); });
        connect(clear, &QPushButton::clicked, this,
                [this]
                {
                    for (int i = int(transfers.size()) - 1; i >= 0; --i)
                    {
                        if (transfers[i]->state == "done" || transfers[i]->state == "cancelled")
                        {
                            delete transfers[i]->item;
                            transfers.removeAt(i);
                        }
                    }
                });
        queueBox->hide();
        layout->addWidget(queueBox);
        connect(ticker, &QTimer::timeout, this, [this] { tick(); });
    }

    // Signals of one transfer's sftp process: send the batch, then record the result and start the next ones.
    void watchTransfer(const std::shared_ptr<Transfer> &t)
    {
        auto *mover = t->process;
        connect(mover, &QProcess::started, this,
                [mover]
                {
                    mover->write(mover->property("batch").toByteArray());
                    mover->closeWriteChannel();
                });
        connect(mover, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, t, mover](int code, QProcess::ExitStatus exit)
                {
                    const auto error = QString::fromUtf8(mover->readAllStandardError()).trimmed();
                    mover->readAllStandardOutput();
                    mover->deleteLater();
                    t->process = nullptr;
                    if (t->cancelled)
                        t->state = "cancelled";
                    else if (code == 0 && exit == QProcess::NormalExit)
                        t->state = "done";
                    else
                    {
                        t->state = "failed";
                        t->item->setText(3,
                                         L("Failed") + ": " + (error.isEmpty() ? L("SFTP operation failed.") : error.left(200)));
                        t->item->setToolTip(3, error);
                    }
                    if (t->bar->maximum() == 0)
                        t->bar->setRange(0, 100);
                    showState(t);
                    if (t->state == "done" && t->upload && t->directory == listed)
                        refresh();
                    if (t->state == "done" && t->open)
                    {
                        edited[t->local] = {t->directory, t->name};
                        if (launchEditor)
                            openLocal(t->local, t->openMode);
                    }
                    if (!t->upload && edited.contains(t->local) && QFileInfo::exists(t->local))
                        watcher->addPath(t->local);
                    if (!runningTransfers())
                        ticker->stop();
                    pump();
                });
        connect(mover, &QProcess::errorOccurred, this,
                [this, t, mover](QProcess::ProcessError e)
                {
                    if (e != QProcess::FailedToStart)
                        return;
                    t->state = "failed";
                    t->item->setText(3, L("Failed") + ": " + mover->errorString());
                    t->process = nullptr;
                    mover->deleteLater();
                    if (!runningTransfers())
                        ticker->stop();
                    pump();
                });
    }

    void retranslateQueue()
    {
        queueView->setHeaderLabels({L("File"), L("Size"), L("Progress"), L("Status")});
        for (const auto &t : transfers)
            showState(t);
    }

public:
    // Runs "cd <path>", the operations ("-" prefix: errors do not abort the list) and then lists again.
    // interactive=false: askpass opens no dialogs (used for the first automatic listing).
    void run(const QString &directory, const QStringList &operations = {}, bool interactive = true)
    {
        if (process->state() != QProcess::NotRunning)
        {
            status->setText(L("Operation in progress…"));
            return;
        }
        if (!command.error.isEmpty())
        {
            status->setText(describe(command.error));
            return;
        }
        const auto program = QStandardPaths::findExecutable(command.program);
        if (program.isEmpty())
        {
            status->setText(L("OpenSSH sftp client not found in PATH."));
            return;
        }
        const auto remote = sftpQuote(directory);
        if (remote.isEmpty())
        {
            status->setText(L("Invalid path."));
            return;
        }
        QString batch = "cd " + remote + "\n";
        for (const auto &op : operations)
            batch += "-" + op + "\n";
        batch += "pwd\nls -la\n";
        process->setProperty("batch", batch.toUtf8());
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto &entry : askpassEnvironment(target, interactive))
            environment.insert(entry.section('=', 0, 0), entry.section('=', 1));
        process->setProcessEnvironment(environment);
        status->setText(L("Listing %1…").arg(directory));
        files->setCursor(Qt::BusyCursor);
        process->start(
            program,
            // BatchMode=no before -b: otherwise sftp disables password authentication (askpass).
            QStringList{"-o", "BatchMode=no", "-b", "-", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=10"} +
                command.arguments);
    }
    void retranslate()
    {
        texts.apply();
        files->setHeaderLabels({L("Name"), L("Size (KB)"), L("Last modified"), L("Owner"), L("Group"), L("Access")});
        retranslateQueue();
    }
    void refresh() { run(listed.isEmpty() ? path->text() : listed); }
    // The first automatic listing failed (login not finished yet): retry it without dialogs.
    void retryAutomatic()
    {
        if (listed.isEmpty() && !busy())
            run(startFolder(), {}, false);
    }
    // The terminal's folder right after login (sftp starts in the remote home, so "~/x" is just "x").
    QString startFolder() const
    {
        const auto dir = folderSource ? folderSource() : QString();
        if (dir.isEmpty() || dir == "~")
            return path->text();
        return dir.startsWith("~/") ? dir.mid(2) : dir;
    }
    // True after the first successful listing: the server is reachable and speaks SFTP.
    bool connected() const { return !home.isEmpty(); }
    std::function<void()> onConnected;
    QString currentPath() const { return listed; }
    QTreeWidget *view() const { return files; }
    bool busy() const
    {
        return process->state() != QProcess::NotRunning || scanner->state() != QProcess::NotRunning || !scanQueue.isEmpty();
    }
    std::function<QString()> folderSource;
    std::function<void(const QString &)> pasteToTerminal; // "Copy file path to terminal"
    std::function<void(bool)> onMonitoring;               // the window starts/stops monitoring the session
    QPushButton *monitoring = new QPushButton;            // Remote monitoring toggle
    bool launchEditor = true;                             // false in tests: no external editor
    void setFollow(bool on) { follow->setChecked(on); }
    QCheckBox *followBox() const { return follow; }
    // Downloads to a temporary folder and opens it (with the "Open with" chooser when chooseApp);
    // every save of the local copy is uploaded back to the server.
    void openRemote(const QString &name, qint64 size, int openMode = 0)
    {
        const auto dir = QDir::temp().filePath("CrossTerm-sftp");
        QDir().mkpath(dir);
        enqueue(listed, name, QDir(dir).filePath(name), false, false, size, true);
        transfers.last()->openMode = openMode;
    }
    static void openLocal(const QString &file, int openMode)
    {
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(file);
        if (openMode == 1)
        {
            const auto wide = native.toStdWString();
            OPENASINFO info{wide.c_str(), nullptr, OAIF_ALLOW_REGISTRATION | OAIF_EXEC};
            if (SUCCEEDED(SHOpenWithDialog(nullptr, &info)))
                return;
        }
        if (openMode == 2 && QProcess::startDetached("notepad.exe", {native}))
            return;
#else
        Q_UNUSED(openMode);
#endif
        QDesktopServices::openUrl(QUrl::fromLocalFile(file));
    }
    void removeRecursive(const QStringList &directories, const QStringList &filesToRemove)
    {
        scanQueue = directories;
        foundDirs = directories;
        foundFiles = filesToRemove;
        status->setText(L("Deleting…"));
        scanNext();
    }
    QString message() const { return status->text(); }
    int dragTimeout = 10 * 60 * 1000; // ms allowed for a drag download

    // Data for dragging the given names of the current folder out (nullptr if there is nothing).
    QMimeData *createDragData(const QStringList &names)
    {
        const auto directory = listed.isEmpty() ? path->text() : listed;
        if (names.isEmpty() || directory.isEmpty())
            return nullptr;
        const auto local = QDir::temp().filePath("CrossTerm-drag/" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSet<QString> folders;
        auto *data = new SftpDragData;
        for (const auto &name : names)
        {
            data->planned << QUrl::fromLocalFile(QDir(local).filePath(name));
            if (isDir(name))
                folders << name;
        }
        // ponytail: temp folders are left behind (the drop target may copy them asynchronously);
        // clean up at startup if they grow too large.
        data->fetch = [self = QPointer<SftpPanel>(this), directory, names, folders, local]
        { return self && self->fetchBlocking(directory, names, folders, local); };
        return data;
    }

    // Downloads the names into a local folder with an sftp batch; local event loop, so the UI stays responsive.
    bool fetchBlocking(const QString &directory, const QStringList &names, const QSet<QString> &folders, const QString &local)
    {
        const auto program = QStandardPaths::findExecutable(command.program);
        QString batch = "cd " + sftpQuote(directory) + "\nlcd " + sftpQuote(QDir::fromNativeSeparators(local)) + "\n";
        bool valid = !sftpQuote(directory).isEmpty() && !sftpQuote(QDir::fromNativeSeparators(local)).isEmpty();
        for (const auto &name : names)
        {
            valid = valid && !sftpQuote(name).isEmpty();
            batch += (folders.contains(name) ? "get -r " : "get ") + sftpQuote(name) + "\n";
        }
        if (!command.error.isEmpty() || program.isEmpty() || !valid || !QDir().mkpath(local))
        {
            status->setText(!command.error.isEmpty() ? describe(command.error)
                            : program.isEmpty()      ? L("OpenSSH sftp client not found in PATH.")
                                                     : L("Invalid path."));
            return false;
        }
        QPointer<SftpPanel> self(this);
        QProcess proc;
        hideConsoleWindow(&proc);
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        connect(&proc, &QProcess::started, &loop,
                [&]
                {
                    proc.write(proc.property("batch").toByteArray());
                    proc.closeWriteChannel();
                });
        connect(&proc, &QProcess::finished, &loop, &QEventLoop::quit);
        connect(&proc, &QProcess::errorOccurred, &loop,
                [&](QProcess::ProcessError e)
                {
                    if (e == QProcess::FailedToStart)
                        loop.quit();
                });
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        status->setText(L("Downloading %1 item(s) for drag and drop…").arg(names.size()));
        startSftp(&proc, batch.toUtf8(), true);
        timer.start(dragTimeout);
        if (proc.state() != QProcess::NotRunning)
            loop.exec();
        const bool timedOut = proc.state() != QProcess::NotRunning;
        if (timedOut)
        {
            proc.kill();
            proc.waitForFinished(3000);
        }
        const auto error = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        const bool ok = !timedOut && proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0 &&
                        proc.error() != QProcess::FailedToStart;
        if (!self)
            return false; // panel closed during the download
        status->setText(ok         ? QString()
                        : timedOut ? L("SFTP operation timed out.")
                                   : (error.isEmpty() ? L("SFTP operation failed.") : error.left(400)));
        return ok;
    }

    SftpPanel(QJsonObject profile, const QString &platform, QWidget *parent = nullptr) : QWidget(parent)
    {
        profile["protocol"] = "sftp";
        profile["x11"] = false;
        command = sessionCommand(profile, platform, false);
        target = credentialTarget(profile, QCoreApplication::applicationName());
        texts.bind(static_cast<QWidget *>(this), &QWidget::setAccessibleName, N("SFTP browser"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        auto *bar = new QToolBar;
        bar->setIconSize(QSize(18, 18));
        bar->setObjectName("sftpTools");
        layout->addWidget(bar);
        auto action = [&](const QString &name, const char *tip, auto slot, QKeySequence key = {})
        {
            auto *a = bar->addAction(icon(name), QString());
            texts.text(a, tip);
            texts.tip(a, tip);
            if (!key.isEmpty())
            {
                a->setShortcut(key);
#ifdef Q_OS_MACOS
                if (key == QKeySequence(Qt::Key_Delete)) // Cmd+Backspace deletes on macOS, like Del
                    a->setShortcuts({key, QKeySequence(Qt::CTRL | Qt::Key_Backspace)});
#endif
                a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            }
            connect(a, &QAction::triggered, this, slot);
            files->addAction(a);
            return a;
        };
        action(
            "parent", N("Parent directory"),
            [this]
            {
                if (!listed.isEmpty())
                    run(join(".."));
            },
            QKeySequence(Qt::Key_Backspace));
        action("download", N("Download selected files"), [this] { download(); });
        action("upload", N("Upload to current folder"),
               [this]
               {
                   const auto names = QFileDialog::getOpenFileNames(this, L("Upload files"));
                   upload(names);
               });
        action("refresh", N("Refresh current folder"), [this] { refresh(); }, QKeySequence(Qt::Key_F5));
        action("newfolder", N("Create new directory"),
               [this]
               {
                   const auto name = QInputDialog::getText(this, L("New directory"), L("Directory name:")).trimmed();
                   if (!name.isEmpty() && !sftpQuote(name).isEmpty())
                       run(listed, {"mkdir " + sftpQuote(name)});
               });
        action("newfile", N("Create new file"),
               [this]
               {
                   const auto name = QInputDialog::getText(this, L("New file"), L("File name:")).trimmed();
                   if (name.isEmpty() || sftpQuote(name).isEmpty())
                       return;
                   const auto empty = QDir::temp().filePath("crossterm-empty-file");
                   QFile file(empty);
                   if (!file.open(QIODevice::WriteOnly))
                   {
                       status->setText(file.errorString());
                       return;
                   }
                   file.close();
                   run(listed, {"put " + sftpQuote(QDir::fromNativeSeparators(empty)) + " " + sftpQuote(name)});
               });
        action("delete", N("Delete selected files"), [this] { remove(); }, QKeySequence(Qt::Key_Delete));
        auto *renameAction = action(
            "rename", N("Rename"),
            [this]
            {
                const auto names = selectedNames();
                if (names.size() != 1)
                    return;
                const auto next = QInputDialog::getText(this, L("Rename"), L("New name:"), QLineEdit::Normal, names[0]).trimmed();
                if (!next.isEmpty() && next != names[0] && !sftpQuote(next).isEmpty())
                    run(listed, {"rename " + sftpQuote(names[0]) + " " + sftpQuote(next)});
            },
            QKeySequence(Qt::Key_F2));

        texts.bind(path, &QWidget::setAccessibleName, N("Remote SFTP path"));
        path->setObjectName("sftpPath");
        auto *go = path->addAction(icon("ok"), QLineEdit::TrailingPosition);
        texts.tip(go, N("Go to this folder"));
        connect(go, &QAction::triggered, this, [this] { run(path->text()); });
        connect(path, &QLineEdit::returnPressed, this, [this] { run(path->text()); });
        layout->addWidget(path);

        retranslate();
        files->setRootIsDecorated(false);
        files->setSelectionMode(QAbstractItemView::ExtendedSelection);
        files->setUniformRowHeights(true);
        files->setIconSize(QSize(16, 16));
        files->setColumnWidth(0, 150);
        files->setAcceptDrops(true);
        files->viewport()->setAcceptDrops(true);
        files->viewport()->installEventFilter(this);
        files->setDragEnabled(true); // drag out (download), see SftpTree
        files->dragData = [this] { return createDragData(selectedNames()); };
        // Right click on a file: open, edit and download / delete and rename / path, properties, permissions.
        files->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(files, &QWidget::customContextMenuRequested, this,
                [this, renameAction](const QPoint &at)
                {
                    auto *item = files->itemAt(at);
                    if (!item || item->text(0) == "..")
                        return;
                    const auto name = item->text(0);
                    const bool dir = item->data(0, Qt::UserRole).toBool();
                    const auto size = item->data(0, Qt::UserRole + 1).toLongLong();
                    const auto remotePath = join(name);
                    QMenu menu(this);
                    if (!dir)
                    {
                        menu.addAction(icon("open", 16), L("Open"), [=] { openRemote(name, size, 0); });
                        menu.addAction(icon("edit", 16), L("Open with default text editor"), [=] { openRemote(name, size, 2); });
                        menu.addAction(icon("openwith", 16), L("Open with..."), [=] { openRemote(name, size, 1); });
                    }
                    else
                        menu.addAction(icon("folder", 16), L("Open"), [=] { run(remotePath); });
                    menu.addAction(icon("download", 16), L("Download"), [this] { download(); });
                    menu.addSeparator();
                    menu.addAction(icon("delete", 16), L("Delete"), [this] { remove(); });
                    menu.addAction(icon("rename", 16), L("Rename"), renameAction, &QAction::trigger);
                    menu.addSeparator();
                    menu.addAction(icon("copy", 16), L("Copy file path"),
                                   [remotePath] { QApplication::clipboard()->setText(remotePath); });
                    if (pasteToTerminal)
                        menu.addAction(icon("terminal", 16), L("Copy file path to terminal"),
                                       [this, remotePath] { pasteToTerminal(sftpQuote(remotePath)); });
                    menu.addAction(icon("info", 16), L("Properties"),
                                   [this, item]
                                   {
                                       QMessageBox::information(this, L("Properties"),
                                                                QString("%1\n\n%2: %3\n%4: %5\n%6: %7 / %8\n%9: %10")
                                                                    .arg(join(item->text(0)), L("Size (KB)"), item->text(1),
                                                                         L("Last modified"), item->text(2), L("Owner"),
                                                                         item->text(3), item->text(4), L("Access"))
                                                                    .arg(item->text(5)));
                                   });
                    menu.addAction(icon("lock", 16), L("Permissions"),
                                   [this, item]
                                   {
                                       // rwxr-xr-x -> 755 as the starting value ('*' and unknown bits count as 0).
                                       const auto access = item->text(5);
                                       QString octal;
                                       for (int g = 0; g < 3; ++g)
                                       {
                                           int v = 0;
                                           for (int b = 0; b < 3; ++b)
                                               v = v * 2 + (QString("rwx").contains(access.mid(1 + g * 3 + b, 1)) &&
                                                            access.mid(1 + g * 3 + b, 1) != "-");
                                           octal += QString::number(v);
                                       }
                                       bool ok = false;
                                       const auto mode =
                                           QInputDialog::getText(this, L("Permissions"), L("Octal mode (e.g. 644, 755):"),
                                                                 QLineEdit::Normal, octal, &ok)
                                               .trimmed();
                                       if (ok && QRegularExpression("^[0-7]{3,4}$").match(mode).hasMatch())
                                           run(listed, {"chmod " + mode + " " + sftpQuote(item->text(0))});
                                   });
                    menu.exec(files->viewport()->mapToGlobal(at));
                });
        layout->addWidget(files, 1);
        buildQueue(layout);
        connect(files, &QTreeWidget::itemActivated, this,
                [this](QTreeWidgetItem *item)
                {
                    const auto name = item->text(0);
                    if (name == ".." || item->data(0, Qt::UserRole).toBool())
                    {
                        run(join(name));
                        return;
                    }
                    // Open with the default editor; saves are uploaded back to the server.
                    openRemote(name, item->data(0, Qt::UserRole + 1).toLongLong());
                });

        texts.text(monitoring, N("Remote monitoring"));
        monitoring->setIcon(icon("monitor"));
        monitoring->setCheckable(true);
        texts.tip(monitoring, N("Show CPU, memory, network, uptime and disk of the remote Linux server"));
        connect(monitoring, &QPushButton::toggled, this,
                [this](bool on)
                {
                    if (onMonitoring)
                        onMonitoring(on);
                });
        texts.text(follow, N("Follow terminal folder"));
        texts.tip(follow, N("Show in the SFTP browser the folder of the terminal (OSC 7 or a user@host:path prompt)"));
        connect(follow, &QCheckBox::toggled, this,
                [this](bool on)
                {
                    followed.clear();
                    on ? followTimer->start(800) : followTimer->stop();
                });
        connect(followTimer, &QTimer::timeout, this, [this] { followTick(); });
        connect(watcher, &QFileSystemWatcher::fileChanged, this,
                [this](const QString &file)
                {
                    if (!edited.contains(file) || pendingEdits.contains(file))
                        return;
                    pendingEdits << file;
                    QTimer::singleShot(600, this,
                                       [this, file]
                                       {
                                           pendingEdits.remove(file);
                                           if (!QFileInfo::exists(file))
                                               return;
                                           watcher->addPath(file); // editors often replace the file
                                           const auto [dir, name] = edited.value(file);
                                           status->setText(L("Uploading changes of %1…").arg(name));
                                           enqueue(dir, name, file, true, false, QFileInfo(file).size());
                                       });
                });
        connect(scanner, &QProcess::started, this,
                [this]
                {
                    scanner->write(scanner->property("batch").toByteArray());
                    scanner->closeWriteChannel();
                });
        connect(scanner, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus exit)
                {
                    const auto output = QString::fromUtf8(scanner->readAllStandardOutput());
                    const auto error = QString::fromUtf8(scanner->readAllStandardError()).trimmed();
                    if (code || exit != QProcess::NormalExit)
                    {
                        scanQueue.clear();
                        foundFiles.clear();
                        foundDirs.clear();
                        status->setText(error.isEmpty() ? L("SFTP operation failed.") : error.left(400));
                        return;
                    }
                    const auto dir = scanner->property("dir").toString();
                    for (const auto &line : output.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts))
                    {
                        RemoteEntry entry;
                        if (!parseListing(line, entry))
                            continue;
                        const auto full = dir + "/" + entry.name;
                        if (entry.dir && !entry.link)
                        {
                            scanQueue << full;
                            foundDirs << full;
                        }
                        else
                            foundFiles << full;
                    }
                    scanNext();
                });
        status->setWordWrap(true);
        status->setTextFormat(Qt::PlainText);
        status->setObjectName("sftpStatus");
        layout->addWidget(status);
        // Bottom: Remote monitoring (1px side margin) with Follow terminal folder centered under it, 2px above the edge.
        auto *bottom = new QVBoxLayout;
        bottom->setContentsMargins(1, 0, 1, 2);
        bottom->setSpacing(2);
        bottom->addWidget(monitoring);
        bottom->addWidget(follow, 0, Qt::AlignHCenter);
        layout->addLayout(bottom);

        connect(process, &QProcess::started, this,
                [this]
                {
                    process->write(process->property("batch").toByteArray());
                    process->closeWriteChannel();
                });
        connect(process, &QProcess::errorOccurred, this,
                [this]
                {
                    files->unsetCursor();
                    status->setText(process->errorString());
                });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus exit)
                {
                    files->unsetCursor();
                    const auto output = QString::fromUtf8(process->readAllStandardOutput());
                    const auto error = QString::fromUtf8(process->readAllStandardError()).trimmed();
                    if (code || exit != QProcess::NormalExit)
                    {
                        openAfter.clear();
                        if (error.contains("Permission denied"))
                            status->setText(
                                L("Authentication failed. Click Refresh to enter the password, or check the user name and key.") +
                                "\n" + error.left(300));
                        else if (error.contains("Host key verification failed"))
                            status->setText(L("Unknown host key: accept it in the SSH terminal first, then click Refresh."));
                        else
                            status->setText(error.isEmpty() ? L("SFTP operation failed.") : error.left(400));
                        return;
                    }
                    QList<RemoteEntry> entries;
                    for (const auto &line : output.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts))
                    {
                        if (line.startsWith("Remote working directory: "))
                        {
                            listed = line.mid(26).trimmed();
                            continue;
                        }
                        RemoteEntry entry;
                        if (parseListing(line, entry))
                            entries << entry;
                    }
                    std::sort(entries.begin(), entries.end(), [](const RemoteEntry &a, const RemoteEntry &b)
                              { return a.dir != b.dir ? a.dir : a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
                    const bool first = home.isEmpty();
                    if (first)
                        home = listed; // first listed folder = remote home, used to resolve "~"
                    path->setText(listed.endsWith('/') ? listed : listed + "/");
                    files->clear();
                    auto *up = new QTreeWidgetItem(files, {".."});
                    up->setIcon(0, icon("parent"));
                    up->setData(0, Qt::UserRole, true);
                    for (const auto &e : entries)
                    {
                        auto *item =
                            new QTreeWidgetItem(files, {e.name, e.dir ? QString() : QString::number((e.size + 1023) / 1024),
                                                        e.modified, e.owner, e.group, e.access});
                        item->setData(0, Qt::UserRole, e.dir);
                        item->setData(0, Qt::UserRole + 1, e.size);
                        item->setIcon(0, icon(e.dir ? "folder" : "file"));
                        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
                    }
                    status->setText(error.isEmpty() ? QString() : error.left(400));
                    if (first && onConnected)
                        onConnected();
                    if (!openAfter.isEmpty())
                    {
                        QDesktopServices::openUrl(QUrl::fromLocalFile(openAfter));
                        openAfter.clear();
                    }
                });
        // First automatic listing when the SSH session opens.
        QTimer::singleShot(1500, this, [this] { run(startFolder(), {}, false); });
    }

    void download()
    {
        const auto names = selectedNames();
        if (names.isEmpty() || listed.isEmpty())
            return;
        const auto folder = QFileDialog::getExistingDirectory(this, L("Download to folder"),
                                                              QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
        if (!folder.isEmpty())
            download(names, folder);
    }

    void download(const QStringList &names, const QString &folder)
    {
        for (const auto &name : names)
        {
            qint64 size = -1;
            for (int i = 0; i < files->topLevelItemCount(); ++i)
            {
                if (files->topLevelItem(i)->text(0) == name)
                    size = files->topLevelItem(i)->data(0, Qt::UserRole + 1).toLongLong();
            }
            enqueue(listed, name, QDir(folder).filePath(name), false, isDir(name), size);
        }
    }

    void upload(const QStringList &locals)
    {
        if (locals.isEmpty() || listed.isEmpty())
            return;
        for (const auto &local : locals)
        {
            const QFileInfo info(local);
            enqueue(listed, info.fileName(), local, true, info.isDir(), info.isDir() ? -1 : info.size());
        }
    }

    void remove()
    {
        const auto names = selectedNames();
        if (names.isEmpty() || listed.isEmpty())
            return;
        if (QMessageBox::question(this, L("Delete"),
                                  L("Delete %1 selected item(s)? Folders are deleted with all their contents.")
                                      .append("\n%2")
                                      .arg(names.size())
                                      .arg(names.mid(0, 10).join('\n'))) != QMessageBox::Yes)
            return;
        QStringList dirs, plain;
        for (const auto &name : names)
            (isDir(name) ? dirs : plain) << join(name);
        removeRecursive(dirs, plain);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const bool dragging =
            event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop;
        // A drag that started from this panel must not be uploaded back to the server.
        if (watched == files->viewport() && dragging && static_cast<QDropEvent *>(event)->source() == files)
        {
            event->ignore();
            return true;
        }
        if (watched == files->viewport() && (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove))
        {
            auto *drag = static_cast<QDropEvent *>(event);
            if (drag->mimeData()->hasUrls() && !listed.isEmpty())
            {
                drag->acceptProposedAction();
                return true;
            }
        }
        if (watched == files->viewport() && event->type() == QEvent::Drop)
        {
            QStringList locals;
            for (const auto &url : static_cast<QDropEvent *>(event)->mimeData()->urls())
                if (url.isLocalFile())
                    locals << url.toLocalFile();
            upload(locals);
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }
};
