#pragma once
// FTP without external programs: asynchronous client (RFC 959 + EPSV/EPRT RFC 2428, passive or active, binary,
// explicit/implicit FTPS RFC 4217, REST/APPE resume, recursive folders), browser panel similar to the
// SFTP browser, and a small FTP(S) server confined to a folder.
#include "profiles.h" // RemoteEntry, parseListing
#include <QElapsedTimer>
#include <QHash>
#include <QIcon>
#include <QJsonObject>
#include <QTreeWidgetItem>
#include <QWidget>
#include <functional>
#include <memory>

class QFile;
class QIODevice;
class QLabel;
class QLineEdit;
class QSslCertificate;
class QSslConfiguration;
class QSslSocket;
class QTcpServer;
class QTcpSocket;
class QTemporaryDir;
class QTimer;
class QTreeWidget;

// ---- Shared by the SFTP and FTP browsers.
// "512 B", "12 KB", "1.2 MB", "3.4 GB" (1024 based; the units are not translated).
QString fileSizeText(qint64 bytes);
// "Replace existing files?" answers remembered until CrossTerm closes, per kind of transfer (the question's title), for
// every SFTP and FTP tab.
inline QHash<QString, bool> &rememberedReplaces()
{
    static QHash<QString, bool> answers;
    return answers;
}
// Column widths of a file list kept across restarts under key (one value for every SFTP or FTP tab).
void rememberColumnWidths(class QTreeView *view, const QString &key);
// The file or folder icon with a small link arrow at the bottom left (symbolic links); composed once per kind.
QIcon linkIcon(const QIcon &base, bool folder);
// Target of a symbolic link when its "ls -l" line carries it ("name -> target": parseListing keeps the name only).
QString listedLinkTarget(const QString &line, const RemoteEntry &entry);

// Row of a remote file list: the Size column sorts by the byte count kept in its Qt::UserRole (-1 for folders).
class RemoteFileItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override;
};

// Speed of a running transfer over the last 3 s and the time left: "3.4 MB/s — 0:42" (the speed alone when the
// total is unknown). Numbers, units and punctuation only: nothing to translate.
class TransferRate
{
public:
    bool add(qint64 done, qint64 total); // bytes so far; true when text() changed (about once a second)
    void reset();
    QString text() const { return shown; }

private:
    QElapsedTimer clock;
    QList<std::pair<qint64, qint64>> samples; // ms, bytes
    qint64 shownAt = 0;
    QString shown;
};

// FTP client: operations are queued and executed one at a time on the same session.
// Callbacks come from the event loop; do not destroy the client inside a callback (use deleteLater on the owner).
class FtpClient
{
public:
    using Done = std::function<void(bool ok, const QString &error)>; // error: "550 server text" or a local message
    using Progress = std::function<void(qint64 done, qint64 total)>; // total -1 if unknown
    using Listed = std::function<void(bool ok, const QString &error, const QList<RemoteEntry> &entries)>;

    // Download/upload options. Resume: continues what an earlier, interrupted call with the same local and remote
    // names left on this client (download: the "<local>.part" it wrote; upload: the remote file whose STOR it
    // started, REST+STOR or APPE, if the local file is unchanged; files it completed are skipped). Anything else
    // is transferred from the start: a file that merely has the same name is never continued.
    // Recursive: remote and local are folders, copied with all their contents; aggregate progress.
    enum Flag
    {
        Resume = 1,
        Recursive = 2
    };

    FtpClient();
    ~FtpClient();
    // Empty user = anonymous. The password is never written to the log. Port 0 = 21 (990 with implicit FTPS).
    void connectToHost(const QString &host, quint16 port, const QString &user, const QString &password, Done done);
    void list(const QString &path, Listed done); // empty path = current folder; MLSD if offered, otherwise LIST
    // CWD (unless path is empty), PWD and the listing of that folder as one operation: nothing else runs in between.
    void enter(const QString &path,
               std::function<void(bool ok, const QString &error, const QString &where, const QList<RemoteEntry> &entries)> done);
    void cwd(const QString &path, Done done);
    void pwd(std::function<void(bool ok, const QString &error, const QString &path)> done);
    // Downloads write to "<local>.part" and rename it over localFile when the transfer completes; if interrupted
    // (or shorter than the size the server announced) the .part file is kept (if not empty) for resuming with Resume.
    void download(const QString &remote, const QString &localFile, Progress progress, Done done, int flags = 0);
    void upload(const QString &localFile, const QString &remote, Progress progress, Done done, int flags = 0);
    void mkdir(const QString &path, Done done);
    void removeFile(const QString &path, Done done);
    void removeDirectory(const QString &path, bool recursive, Done done);
    void rename(const QString &from, const QString &to, Done done);
    // Aborts the current and queued operations (error "Aborted.") and closes the session.
    void abort();
    void disconnectFromHost();
    bool isConnected() const;
    bool busy() const;
    bool usesMlsd() const { return mlsd && preferMlsd; }
    // The last connectToHost failed because the server refused the user or the password (530 to USER or PASS).
    bool loginRejected() const { return rejected; }
    // Symbolic links of the last listing whose target the server told: name -> target.
    QHash<QString, QString> linkTargets() const { return targets; }
    static QString fingerprint(const QSslCertificate &certificate); // SHA-256 "AB:CD:..."

    std::function<void(const QString &line)> log; // control channel, PASS masked
    int timeout = 30000;                          // ms of inactivity before "Timed out."
    bool preferMlsd = true;                       // false: always use LIST (Unix/DOS format)
    QString security = "none";                    // "none" | "explicit" (AUTH TLS) | "implicit" (TLS immediately, 990)
    bool passive = true;                          // false: active mode (EPRT/PORT, the server connects to us)
    // Unverifiable certificate (self-signed, name mismatch...): true = trust it for this host (the client
    // remembers it for later connections). Without a callback, or with false, the connection fails with the fingerprint.
    std::function<bool(const QSslCertificate &certificate)> trustCertificate;

private:
    struct Job
    {
        Done done;
        std::function<void()> start;
    };
    struct Plan;
    struct Trace;
    QObject *ctx;
    QSslSocket *control, *data = nullptr;
    QTcpServer *listener = nullptr; // active mode
    QTimer *watchdog;
    std::shared_ptr<QFile> sinkFile;      // download in progress (closed, and removed if empty, on failure)
    std::function<void()> afterHandshake; // AUTH TLS in progress
    QString hostName, tlsError;
    QHash<QString, QByteArray> trusted; // host -> SHA-256 of the accepted certificate
    QHash<QString, QString> targets;    // linkTargets()
    // Interrupted transfers, for Resume: "D"/"U", local and remote names -> what they left behind.
    QHash<QString, std::shared_ptr<Trace>> traces;
    QByteArray input;
    int multiline = 0;
    QString multiText;
    std::function<void(int, const QString &)> pending;
    QList<Job> queue;
    Done current;
    bool running = false, loggedIn = false, mlsd = false, epsv = true, rejected = false;

    void enqueue(Done done, std::function<void()> start);
    void next();
    void complete(bool ok, const QString &error = {});
    void fail(const QString &error);
    void readReplies();
    void dispatch(int code, const QString &text);
    void send(const QString &command, std::function<void(int, const QString &)> handler);
    void simple(const QString &command, std::function<void()> then);
    void afterLogin();
    void openPassive(std::function<void(quint16 port)> then);
    void openActive(std::function<void(QTcpServer *server)> then);
    void closeListener();
    void secureData(QSslSocket *socket, std::function<void()> ready);
    // restart > 0: REST right before the command (after PASV/PORT); resumed(REST accepted) returns the command to send.
    // began: the server accepted the command (1xx reply).
    void transfer(const QString &command, std::shared_ptr<QIODevice> source, std::function<bool(const QByteArray &)> sink,
                  Progress progress, qint64 total, std::function<void()> then, qint64 restart = 0,
                  std::function<QString(bool accepted)> resumed = {}, std::function<void()> began = {});
    std::shared_ptr<Trace> traced(const QString &key, bool resume);
    void fetch(const QString &remote, const QString &local, std::shared_ptr<Trace> trace, Progress progress,
               std::function<void()> then);
    void store(const QString &local, const QString &remote, std::shared_ptr<Trace> trace, Progress progress,
               std::function<void()> then);
    void scanRemote(std::shared_ptr<Plan> plan, std::function<void()> then);
    void makeDirs(std::shared_ptr<Plan> plan, int index, std::function<void()> then);
    void runPlan(std::shared_ptr<Plan> plan, int index, qint64 base, bool upload, std::shared_ptr<Trace> trace,
                 Progress progress);
    // strict: lines in an unknown format fail the listing even when other lines were understood (recursive operations).
    void listing(const QString &path, std::function<void(const QList<RemoteEntry> &)> then, bool strict = false);
    void removeTree(const QString &path, int depth, std::function<void()> then);
    void removeEntries(const QString &dir, std::shared_ptr<const QList<RemoteEntry>> entries, int index, int depth,
                       std::function<void()> then);
};

// Sidebar FTP browser, with the same look as the SFTP panel.
// profile: host, port (21; 990 if implicit), user (empty = anonymous), ftpSecurity ("none" | "explicit" | "implicit"),
// ftpPassive (true), ftpFingerprint (SHA-256 of the already accepted certificate). passwordProvider is called only when needed.
class FtpPanel : public QWidget
{
public:
    FtpPanel(const QJsonObject &profile, std::function<QString()> passwordProvider, QWidget *parent = nullptr);
    ~FtpPanel() override;
    void browse(const QString &path); // absolute path or relative to the current folder
    void refresh();
    // Existing remote names (upload) or local files (download): one question before anything is queued.
    void upload(const QStringList &localFiles);                     // into the current folder; folders too
    void download(const QStringList &names, const QString &folder); // from the current folder; folders too
    void resumeTransfer(int i);                                     // only if failed or cancelled
    void makeDirectory(const QString &name);
    void renameEntry(const QString &from, const QString &to);
    void removeEntries(const QStringList &names); // without confirmation; folders with all their contents
    void retranslate();

    // For tests.
    QString currentPath() const { return listed; }
    QList<RemoteEntry> entries() const { return shown; }
    bool busy() const { return operations > 0 || connecting; }
    QString message() const;
    int transferCount() const { return int(transfers.size()); }
    QString transferState(int i) const; // queued, running, done, failed, cancelled
    int transferProgress(int i) const;
    bool transfersIdle() const;
    FtpClient &client() { return *ftp; }

    // Certificate accepted by the user in the dialog: fingerprint to save in the profile ("ftpFingerprint").
    std::function<void(const QString &fingerprint)> certificateTrusted;
    // Result of a login, told before the requests that waited for it go on or fail (so before the password can be
    // asked again). onLoginRejected: the server refused the user or the password (530); the panel has dropped its
    // copy and calls the password provider again on the next attempt. onLoggedIn: the session is open.
    std::function<void()> onLoginRejected, onLoggedIn;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Transfer;
    struct Item;
    std::unique_ptr<QTemporaryDir> scratch; // files opened with a double click; removed with the panel (after ftp closed them)
    int opened = 0;
    std::unique_ptr<FtpClient> ftp = std::make_unique<FtpClient>();
    QJsonObject profile;
    std::function<QString()> passwordProvider;
    QString password, listed;
    QList<RemoteEntry> shown;
    QHash<QString, QString> shownTargets; // link name -> target, when the listing tells it
    QList<std::shared_ptr<Transfer>> transfers;
    QList<std::pair<std::function<void()>, std::function<void(const QString &)>>> waiting; // waiting for login
    int operations = 0;
    bool connecting = false;
    QLineEdit *path;
    QTreeWidget *files, *queueView;
    QLabel *status;
    QWidget *queueBox;
    QList<std::function<void()>> texts; // re-apply the translated texts

    void ensure(std::function<void()> then, std::function<void(const QString &)> failed);
    void operate(std::function<void(FtpClient::Done)> op);
    void fill(const QList<RemoteEntry> &entries);
    void enqueue(const QString &name, const QString &local, const QString &remote, bool upload, qint64 size, bool folder);
    void start(const std::shared_ptr<Transfer> &t, bool resume);
    void setState(Transfer &t, const QString &state, const QString &error = {});
    QStringList selectedNames() const;
    QString join(const QString &name) const;
    const RemoteEntry *find(const QString &name) const;
    // Listed entries with their absolute paths, taken before a dialog opens: the folder shown may change meanwhile.
    QList<Item> picked(const QStringList &names) const;
    void fetchItems(const QList<Item> &items, const QString &folder);
    void removeItems(const QList<Item> &items);
    void renameItem(const QString &source, const QString &folder, const QString &to, const QStringList &siblings);
    bool replaceConfirmed(const QStringList &existing, const char *title);
};

// Minimal built-in FTP server: one user with a password and/or read-only anonymous access,
// confined to rootDirectory (no "..", drives, device names or links that escape it).
// Optional TLS (AUTH TLS/PBSZ/PROT or implicit), active mode only to the client's IP, REST and APPE.
// With a certificate, the user logs in only over TLS and transfers only with PROT P (no password or data in clear);
// anonymous access stays possible without TLS. Anonymous clients neither see nor reach hidden (dot) names.
class FtpServer
{
public:
    FtpServer();
    ~FtpServer();
    // port 0 = free port chosen by the system (see port()).
    bool start(const QString &rootDirectory, quint16 port, bool allowAnonymous, const QString &user = {},
               const QString &password = {});
    void stop();
    bool isRunning() const;
    quint16 port() const;
    QString errorString() const { return error; }

    bool bindAllInterfaces = false;                 // false: 127.0.0.1 only
    quint16 passivePortMin = 0, passivePortMax = 0; // 0: passive ports chosen by the system
    std::function<void(const QString &line)> log;   // connections and commands, PASS masked
    // TLS (read by start): certificate and unencrypted PEM key; empty = no TLS (AUTH rejected).
    QString certificateFile, privateKeyFile;
    bool implicitTls = false; // implicit FTPS (usually port 990): TLS immediately on connect

private:
    friend class FtpSession;
    QTcpServer *listener;
    std::shared_ptr<QSslConfiguration> tls;
    QObject *sessions;
    QString canonicalRoot, userName, secret, error;
    bool anonymous = false;
    int nextPassive = 0;
};
