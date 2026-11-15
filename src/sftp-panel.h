#pragma once
// Sidebar SFTP browser: each operation is an "sftp -b -" batch of the OpenSSH client,
// with StrictHostKeyChecking=yes (the host key must first be confirmed in the SSH terminal).
#include "icons.h"
#include "i18n.h"
#include "profiles.h"
#include "askpass.h"
#include "ftp.h" // fileSizeText, linkIcon, RemoteFileItem, TransferRate: shared with the FTP browser
#include <QtWidgets>
#include <QDesktopServices>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <memory>
#include <vector>
#include <QFileSystemWatcher>
#include <QTemporaryFile>
#ifdef Q_OS_WIN
#include <shlobj.h> // SHOpenWithDialog
#endif
#ifdef Q_OS_MACOS
#include <libproc.h> // upload progress: offset of the sftp process's descriptor on the local file
#include <sys/stat.h>
#include <sys/xattr.h> // quarantine mark on files opened from the server
#include <xlocale.h>
#endif
#ifdef Q_OS_UNIX
#include <locale.h>
#include <unistd.h>
#endif


// A path for the shell prompt: inside double quotes a shell still expands $(...), `...` and !, so a file name
// holding them is single-quoted (POSIX).
static QString shellQuote(const QString &path)
{
    if (path.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))
        return {};
    if (!path.contains(QRegularExpression("[$`!\"\\\\]")))
        return "\"" + path + "\"";
    return "'" + QString(path).replace("'", "'\\''") + "'";
}

// A word for a remote POSIX shell, always single-quoted; empty for control characters.
static QString posixQuote(const QString &text)
{
    if (text.isEmpty() || text.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))
        return {};
    return "'" + QString(text).replace("'", "'\\''") + "'";
}

// Permissions dialog: read/write/execute checkboxes for owner, group and others, kept in step with the numeric
// mode (whose fourth digit, setuid/setgid/sticky, is kept as it is). The new mode, or empty when cancelled.
// With a shell on the server (shell): owner and group, and for a folder (dir) the same mode for its content too.
struct SftpPermissions
{
    QString mode, owner, group; // owner and group as they were when unchanged
    int recurse = 0;            // 0 the item alone; with its content: 1 files and folders, 2 files only, 3 folders only
};
static std::optional<SftpPermissions> sftpPermissionsDialog(QWidget *parent, const QString &path, const QString &start,
                                                            const QString &owner = {}, const QString &group = {},
                                                            bool shell = false, bool dir = false)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(L("Permissions"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *name = new QLabel(path);
    name->setTextFormat(Qt::PlainText);
    name->setWordWrap(true);
    layout->addWidget(name);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(18);
    const char *columns[] = {N("Read"), N("Write"), N("Execute")};
    const char *rows[] = {N("Owner"), N("Group"), N("Others")};
    for (int c = 0; c < 3; ++c)
        grid->addWidget(new QLabel(L(columns[c])), 0, c + 1, Qt::AlignHCenter);
    QCheckBox *boxes[3][3];
    for (int r = 0; r < 3; ++r)
    {
        grid->addWidget(new QLabel(L(rows[r])), r + 1, 0);
        for (int c = 0; c < 3; ++c)
        {
            boxes[r][c] = new QCheckBox;
            boxes[r][c]->setAccessibleName(L(rows[r]) + " " + L(columns[c]));
            grid->addWidget(boxes[r][c], r + 1, c + 1, Qt::AlignHCenter);
        }
    }
    layout->addLayout(grid);
    auto *numericRow = new QHBoxLayout;
    auto *numeric = new QLineEdit(start);
    numeric->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-7]{0,4}"), numeric));
    numeric->setMaximumWidth(80);
    numericRow->addWidget(new QLabel(L("Numeric value:")));
    numericRow->addWidget(numeric);
    numericRow->addStretch();
    layout->addLayout(numericRow);
    QLineEdit *ownerEdit = nullptr, *groupEdit = nullptr;
    QCheckBox *recurse = nullptr;
    QComboBox *scope = nullptr;
    static const QRegularExpression account("^[A-Za-z0-9._][A-Za-z0-9._-]*\\$?$");
    if (shell)
    {
        auto *form = new QFormLayout;
        ownerEdit = new QLineEdit(owner);
        groupEdit = new QLineEdit(group);
        for (auto *edit : {ownerEdit, groupEdit})
            edit->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9._$-]{0,64}"), edit));
        form->addRow(L("Owner") + ":", ownerEdit);
        form->addRow(L("Group") + ":", groupEdit);
        layout->addLayout(form);
        if (dir)
        {
            recurse = new QCheckBox(L("Apply to the folder's content too"));
            scope = new QComboBox;
            scope->addItems({L("Files and folders"), L("Files only"), L("Folders only")});
            scope->setEnabled(false);
            QObject::connect(recurse, &QCheckBox::toggled, scope, &QWidget::setEnabled);
            layout->addWidget(recurse);
            layout->addWidget(scope);
        }
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    static const QRegularExpression valid("^[0-7]{3,4}$");
    bool syncing = false;
    auto fromNumber = [&]
    {
        const auto mode = numeric->text();
        const bool ok = valid.match(mode).hasMatch();
        buttons->button(QDialogButtonBox::Ok)->setEnabled(ok);
        if (!ok || syncing)
            return;
        syncing = true;
        const auto digits = mode.right(3);
        for (int r = 0; r < 3; ++r)
        {
            const int v = digits[r].digitValue();
            for (int c = 0; c < 3; ++c)
                boxes[r][c]->setChecked(v & (4 >> c));
        }
        syncing = false;
    };
    auto fromBoxes = [&]
    {
        if (syncing)
            return;
        syncing = true;
        QString digits;
        for (int r = 0; r < 3; ++r)
        {
            int v = 0;
            for (int c = 0; c < 3; ++c)
                v |= boxes[r][c]->isChecked() ? 4 >> c : 0;
            digits += QString::number(v);
        }
        const auto current = numeric->text();
        const auto special = current.size() == 4 && current[0] != '0' ? current.left(1) : QString();
        numeric->setText(special + digits);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
        syncing = false;
    };
    QObject::connect(numeric, &QLineEdit::textChanged, &dialog, fromNumber);
    for (auto &row : boxes)
        for (auto *box : row)
            QObject::connect(box, &QCheckBox::toggled, &dialog, fromBoxes);
    fromNumber();
    if (dialog.exec() != QDialog::Accepted || !valid.match(numeric->text()).hasMatch())
        return std::nullopt;
    SftpPermissions result{numeric->text(), owner, group, 0};
    if (ownerEdit && account.match(ownerEdit->text()).hasMatch())
        result.owner = ownerEdit->text();
    if (groupEdit && account.match(groupEdit->text()).hasMatch())
        result.group = groupEdit->text();
    if (recurse && recurse->isChecked())
        result.recurse = scope->currentIndex() + 1;
    return result;
}

// Commands run on the server through sh for the operations that SFTP has no request for. Names are relative to
// the folder dir ("./" first: never read as an option); empty when a path cannot be quoted.
static QString sftpNames(const QStringList &names)
{
    QString words;
    for (const auto &name : names)
    {
        if (const auto quoted = posixQuote("./" + name); !quoted.isEmpty())
            words += " " + quoted;
    }
    return words;
}
// Extracts archive (zip, or a tar of any compression) into dir, replacing files already there (unzip's "A").
// zip: the first output line is the number of entries, then unzip writes one line per entry (progress); its exit
// code 1 is warnings only (names in another encoding), the files are extracted.
static QString sftpExtractScript(const QString &dir, const QString &archive, bool zip)
{
    const auto folder = posixQuote(dir), file = posixQuote("./" + archive);
    if (folder.isEmpty() || file.isEmpty())
        return {};
    if (zip)
        return "cd " + folder + " || exit 2; unzip -Z1 " + file + " | wc -l; unzip -o " + file + "; [ $? -le 1 ]";
    return "cd " + folder + " && tar -x -f " + file;
}
static QString sftpCompressScript(const QString &dir, const QString &archive, const QStringList &names, bool zip)
{
    const auto folder = posixQuote(dir), out = posixQuote("./" + archive), words = sftpNames(names);
    if (folder.isEmpty() || out.isEmpty() || words.isEmpty())
        return {};
    return "cd " + folder + " && " + (zip ? "zip -r -q -y " : "tar -czf ") + out + words;
}
static QString sftpSizeScript(const QString &dir, const QStringList &names)
{
    const auto folder = posixQuote(dir), words = sftpNames(names);
    return folder.isEmpty() || words.isEmpty() ? QString() : "cd " + folder + " && du -sk" + words;
}
// Copies each source path to its target path; fails (exit 1) when any copy fails, after trying them all.
static QString sftpCopyScript(const QList<std::pair<QString, QString>> &copies)
{
    QString script = "e=0";
    for (const auto &[from, to] : copies)
    {
        const auto source = posixQuote(from), target = posixQuote(to);
        if (!source.isEmpty() && !target.isEmpty())
            script += "; cp -R -p -- " + source + " " + target + " || e=1";
    }
    return script + "; exit $e";
}
// recurse: 0 the item alone; 1 its content too; 2 files only; 3 folders only. owner empty: no chown.
static QString sftpPermissionsScript(const QString &path, const QString &mode, const QString &owner, int recurse)
{
    // "--" before the mode and the owner: BSD chmod and chown end their options at the first operand.
    const auto file = posixQuote(path.startsWith('/') ? path : "./" + path);
    if (file.isEmpty() || !QRegularExpression("^[0-7]{3,4}$").match(mode).hasMatch())
        return {};
    QStringList steps;
    if (!owner.isEmpty())
        steps << QString("chown ") + (recurse ? "-R " : "") + "-- " + posixQuote(owner) + " " + file;
    if (recurse == 1)
        steps << "chmod -R -- " + mode + " " + file;
    else if (recurse)
        steps << "find " + file + (recurse == 2 ? " -type f" : " -type d") + " -exec chmod " + mode + " {} +";
    else
        steps << "chmod -- " + mode + " " + file;
    return steps.join(" && ");
}
// "name (copy).ext", "name (copy) 2.ext"…: a name not in taken (a folder keeps its dots).
static QString sftpCopyName(const QString &name, bool dir, const QStringList &taken)
{
    int dot = dir ? -1 : int(name.lastIndexOf('.'));
    if (dot <= 0)
        dot = int(name.size());
    const auto base = name.left(dot), extension = name.mid(dot);
    for (int n = 1;; ++n)
    {
        const auto candidate = L("%1 (copy)").arg(base) + (n > 1 ? " " + QString::number(n) : QString()) + extension;
        if (!taken.contains(candidate))
            return candidate;
    }
}
// "zip" or "tar" for an archive name, empty otherwise.
static QString sftpArchiveKind(const QString &name)
{
    const auto lower = name.toLower();
    if (lower.endsWith(".zip"))
        return "zip";
    for (const char *extension : {".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tbz", ".tar.xz", ".txz"})
    {
        if (lower.endsWith(extension))
            return "tar";
    }
    return {};
}

// A remote name usable as a local file name inside a folder (a name with '\' or ':' from a Unix server would
// leave the folder on Windows).
static bool safeLocalName(const QString &name)
{
#ifdef Q_OS_WIN
    if (name.contains('\\') || name.contains(':'))
        return false;
#endif
    return !name.isEmpty() && name != "." && name != ".." && !name.contains('/');
}

// CrossTerm's folder in the temporary directory. Where that directory is shared (/tmp on Linux) the folder must
// belong to this user and not be a link, and it is made private; empty otherwise.
static QString sftpTempFolder(const QString &name)
{
    const auto dir = QDir::temp().filePath(name);
    QDir().mkpath(dir);
#ifdef Q_OS_UNIX
    const QFileInfo info(dir);
    if (info.isSymLink() || !info.isDir() || info.ownerId() != uint(::getuid()))
        return {};
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
    return dir;
}

// A UTF-8 locale that exists on this system (empty if none). File names go to and come from sftp as UTF-8, and
// without a UTF-8 LC_CTYPE (an app started from the Finder has no LANG) sftp prints non-ASCII names as escapes.
static QString sftpUtf8Locale()
{
#ifdef Q_OS_UNIX
    static const QString found = []
    {
        for (const char *name : {"C.UTF-8", "en_US.UTF-8"})
        {
            if (auto probe = newlocale(LC_CTYPE_MASK, name, nullptr))
            {
                freelocale(probe);
                return QString(name);
            }
        }
        return QString();
    }();
    return found;
#else
    return {};
#endif
}

// rwxr-xr-x -> 755, rwsr-xr-x -> 4755 from the access column of the listing ('*' and unknown bits count as 0).
static QString sftpOctalMode(const QString &access)
{
    QString octal;
    int special = 0;
    for (int g = 0; g < 3; ++g)
    {
        int v = 0;
        for (int b = 0; b < 3; ++b)
        {
            const auto c = access.mid(1 + g * 3 + b, 1);
            v = v * 2 + (c == "r" || c == "w" || c == "x" || c == "s" || c == "t");
            if (b == 2 && (c == "s" || c == "S" || c == "t" || c == "T"))
                special |= 4 >> g;
        }
        octal += QString::number(v);
    }
    if (special)
        octal.prepend(QString::number(special));
    return octal;
}

// File types that the system runs instead of showing: a file named by the server is never handed to them.
static bool sftpRunnable(QString name)
{
    static const QStringList everywhere{"lnk", "url",     "hta",     "scr", "msi", "reg", "jar",
                                        "ps1", "command", "desktop", "app", "pkg", "dmg", "sh"};
#ifdef Q_OS_WIN
    while (name.endsWith('.') || name.endsWith(' ')) // Windows ignores them: "x.exe." is x.exe
        name.chop(1);
    const auto suffix = QFileInfo(name).suffix().toLower();
    const auto runnable = qEnvironmentVariable("PATHEXT", ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC");
    if (!suffix.isEmpty() && runnable.split(';', Qt::SkipEmptyParts).contains("." + suffix, Qt::CaseInsensitive))
        return true;
#else
    const auto suffix = QFileInfo(name).suffix().toLower();
#endif
    return everywhere.contains(suffix);
}

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
// Shows a file in the user's text editor: the program that opens text/plain, started through its desktop entry
// (gtk-launch, else gio), else the first of the usual editors that is installed. Never the program of the file's
// own type, which may run a script. False when no editor was started.
static bool sftpOpenInTextEditor(const QString &file)
{
    // Runs a helper that ends at once (a launcher returns when the editor has started); true when it ended well.
    auto run = [](const QString &program, const QStringList &arguments, QByteArray *output = nullptr)
    {
        QProcess helper;
        helper.setStandardInputFile(QProcess::nullDevice());
        helper.setStandardErrorFile(QProcess::nullDevice());
        if (!output) // the editor inherits the launcher's output: not a pipe, which closes when this returns
            helper.setStandardOutputFile(QProcess::nullDevice());
        helper.start(program, arguments);
        if (!helper.waitForFinished(3000))
            return false;
        if (output)
            *output = helper.readAllStandardOutput();
        return helper.exitStatus() == QProcess::NormalExit && helper.exitCode() == 0;
    };
    static const QRegularExpression id("^[A-Za-z0-9_][A-Za-z0-9_.+-]*\\.desktop$");
    QByteArray answer;
    run("xdg-mime", {"query", "default", "text/plain"}, &answer);
    const auto entry = QString::fromUtf8(answer).section(QRegularExpression("[;\\r\\n]"), 0, 0).trimmed();
    if (id.match(entry).hasMatch())
    {
        if (run("gtk-launch", {entry, file}))
            return true;
        const auto desktop = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, entry);
        if (!desktop.isEmpty() && run("gio", {"launch", desktop, file}))
            return true;
    }
    for (const char *editor : {"gnome-text-editor", "gedit", "kate", "kwrite", "mousepad", "xed", "pluma", "featherpad"})
    {
        if (const auto program = QStandardPaths::findExecutable(editor); !program.isEmpty())
            return QProcess::startDetached(program, {file});
    }
    return false;
}
#endif

// Before a file named by the server is handed to a local program: not executable, and marked as downloaded
// (macOS quarantine, Windows zone of the Internet) so that the system asks before running it.
static void sftpMarkDownloaded(const QString &file)
{
    QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#ifdef Q_OS_MACOS
    const auto mark = "0081;" + QByteArray::number(QDateTime::currentSecsSinceEpoch(), 16) + ";CrossTerm;";
    ::setxattr(QFile::encodeName(file).constData(), "com.apple.quarantine", mark.constData(), size_t(mark.size()), 0, 0);
#elif defined(Q_OS_WIN)
    QFile zone(file + ":Zone.Identifier"); // NTFS alternate stream (not verified: no Windows build here)
    if (zone.open(QIODevice::WriteOnly))
        zone.write("[ZoneTransfer]\r\nZoneId=3\r\n");
#endif
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
        {
            // The download runs an event loop: the drag that owns this data may be gone when it returns.
            QPointer<const QMimeData> self(this);
            const bool ok = fetch && fetch();
            if (!self)
                return {};
            result = ok;
        }
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
    std::function<void()> dragEnded; // after a drag's exec() returned
    int dragging = 0;                // drags whose exec() is on the stack

protected:
    void startDrag(Qt::DropActions) override
    {
        auto *data = dragData ? dragData() : nullptr;
        if (!data)
            return;
        QPointer<SftpTree> self(this);
        auto *drag = new QDrag(this);
        drag->setMimeData(data);
        drag->setPixmap(icon("file").pixmap(32));
        ++dragging;
        drag->exec(Qt::CopyAction, Qt::CopyAction);
        if (!self)
            return; // deleted while the drag ran
        --dragging;
        if (dragEnded)
            dragEnded();
    }
};

// Buttons in a row that wraps onto more lines when the panel is too narrow for them, instead of widening it.
class SftpFlowLayout : public QLayout
{
    QList<QLayoutItem *> items;
    int arrange(const QRect &rect, bool apply) const
    {
        const int gap = spacing() < 0 ? 4 : spacing();
        int x = rect.x(), y = rect.y(), line = 0;
        for (auto *item : items)
        {
            const auto size = item->sizeHint();
            if (x > rect.x() && x + size.width() > rect.right() + 1)
            {
                x = rect.x();
                y += line + gap;
                line = 0;
            }
            if (apply)
                item->setGeometry(QRect(QPoint(x, y), size));
            x += size.width() + gap;
            line = std::max(line, size.height());
        }
        return y + line - rect.y();
    }

public:
    using QLayout::QLayout;
    ~SftpFlowLayout() override
    {
        while (auto *item = takeAt(0))
            delete item;
    }
    void addItem(QLayoutItem *item) override { items << item; }
    int count() const override { return int(items.size()); }
    QLayoutItem *itemAt(int i) const override { return items.value(i); }
    QLayoutItem *takeAt(int i) override { return i >= 0 && i < items.size() ? items.takeAt(i) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        arrange(rect, true);
    }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override
    {
        QSize size; // the widest button: the narrowest the row can get
        for (auto *item : items)
            size = size.expandedTo(item->minimumSize());
        return size;
    }
};

// Rename in place in the list (F2, "Rename"): the name is edited in its row, as in Finder or Explorer. The row keeps
// its old name until the server's listing shows the new one; renamed gets the old and the new name.
class SftpRenameDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    std::function<void(const QString &, const QString &)> renamed;

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        auto *edit = qobject_cast<QLineEdit *>(editor);
        if (!edit)
            return QStyledItemDelegate::setEditorData(editor, index);
        const auto name = index.data().toString();
        edit->setText(name);
        // The name without its extension selected, for a file ("report" of "report.pdf").
        const int dot = name.lastIndexOf('.');
        if (!index.data(Qt::UserRole).toBool() && dot > 0)
            QTimer::singleShot(0, edit, [edit, dot] { edit->setSelection(0, dot); });
        else
            edit->selectAll();
    }
    void setModelData(QWidget *editor, QAbstractItemModel *, const QModelIndex &index) const override
    {
        auto *edit = qobject_cast<QLineEdit *>(editor);
        const auto old = index.data().toString(), next = edit ? edit->text().trimmed() : QString();
        if (renamed && !next.isEmpty() && next != old)
            renamed(old, next);
    }
};

// Dot of the connection state in the path field: 0 grey (not connected yet), 1 green, 2 red.
static QPixmap sftpStateDot(int state)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(10, 10) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor fill(state == 1 ? "#2fb350" : state == 2 ? "#e5484d" : "#9a9aa0");
    p.setPen(QPen(fill.darker(125), 1));
    p.setBrush(fill);
    p.drawEllipse(QRectF(1.5, 1.5, 7, 7));
    return pixmap;
}

class SftpPanel : public QWidget
{
    QLineEdit *path = new QLineEdit(".");
    SftpTree *files = new SftpTree;
    QLabel *status = new QLabel;
    QLabel *dot = new QLabel(path); // connection state; its tooltip is the status text
    int link = 0;                   // 0 not connected yet or connecting, 1 last listing succeeded, 2 cannot connect
    QProcess *process = hideConsoleWindow(new QProcess(this));
    Command command;
    Command shell; // ssh to the same server: "rm -rf" in one go instead of walking the folders over sftp
    QProcess *remover = hideConsoleWindow(new QProcess(this));
    QStringList removing[2]; // folders and files of the "rm -rf" in progress (for the sftp fallback)
    // Programs found on the server (asked once after the first listing): the menu shows what they make possible.
    QSet<QString> tools;
    QString note; // shown as the status once the listing in progress (or the next one) has finished
    // History of the listed folders (Back/Forward) and the move waiting for its listing: -1 back, 1 forward.
    QStringList backStack, forwardStack;
    int historyMove = 0;
    QString historyTarget;
    // Cut or copied remote paths (this server only), pasted into the listed folder.
    QStringList clipboardPaths;
    bool clipboardCut = false;
    QSet<QString> clipboardDirs;
    QLineEdit *filter = new QLineEdit;
    QAction *hiddenToggle = nullptr;
    QAction *backAction = nullptr, *forwardAction = nullptr;
    QString listed, target;
    QString login; // user name of the session ("" when ssh picks it)
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
    QStringList listedNames() const
    {
        QStringList names;
        for (int i = 0; i < files->topLevelItemCount(); ++i)
        {
            if (files->topLevelItem(i)->text(0) != "..")
                names << files->topLevelItem(i)->text(0);
        }
        return names;
    }
    QString join(const QString &name) const { return listed.endsWith('/') ? listed + name : listed + "/" + name; }
    // A new name typed by the user: a name only ('/' or ".." would move the file to another folder).
    static bool plainName(const QString &name)
    {
        return !name.isEmpty() && name != "." && name != ".." && !name.contains('/') && !sftpQuote(name).isEmpty();
    }
    // True (with the reason as the status) when the name cannot be created in the folder: not a plain name, or
    // already in its listing, where creating or renaming onto it would replace that file.
    bool refused(const QString &directory, const QString &name)
    {
        const bool taken = directory == listed && listedNames().contains(name);
        if (taken || !plainName(name))
            status->setText(taken ? L("Already exists") + ": " + name : L("Invalid path."));
        return taken || !plainName(name);
    }

    // --- Transfer queue: one sftp process per item, up to "sftp/parallel" at a time (Settings), with real progress.
    struct Transfer
    {
        QString directory, name, local, state; // state: queued, running, done, failed, cancelled
        bool upload = false, folder = false, resume = false, open = false, cancelled = false;
        int openMode = 0; // 0 default program, 1 "Open with" chooser, 2 text editor
        qint64 size = -1;
        // Upload of a saved edit copy. stage 0: to a temp name, then renamed over the file (copy: copied onto it by
        // the server instead, see replaceMode); 1: the same swap for servers whose rename does not replace; 2: straight
        // onto the file; 3: the same, after the temp file of a copy that did not work is removed.
        bool edit = false, copy = false;
        int stage = 2;
        QString mode; // octal mode of the remote file when a save may replace it through a temp name (see replaceMode)
        // Where a save starts. No mode with copy still set: a copy did not work and its temp file is still there.
        int firstStage() const { return !mode.isEmpty() ? 0 : copy ? 3 : 2; }
        // What the last attempt left, for Resume: an upload that sent bytes and its source then; for a download
        // its start and the "ls -l" line of the remote file.
        bool sent = false;
        qint64 started = 0, sourceSize = -1;
        QDateTime sourceTime;
        QString stamp;
        QString note; // warnings of a completed transfer (entries that sftp skipped)
        bool temporary = false; // "Download as zip": the remote file is removed once downloaded (or the row cleared)
        QTreeWidgetItem *item = nullptr;
        QProgressBar *bar = nullptr;
        QProcess *process = nullptr; // while running
        TransferRate rate;           // speed and time left, shown as the status while running
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
    // Size, time and (small files) hash of a local copy: tells a save from a touch or an unfinished download.
    struct Stamp
    {
        qint64 size = -1; // -1: nothing the server has
        QDateTime time;
        QByteArray hash;
        bool operator==(const Stamp &o) const
        {
            return size >= 0 && size == o.size && (hash.isEmpty() || o.hash.isEmpty() ? time == o.time : hash == o.hash);
        }
    };
    static Stamp stampOf(const QString &file)
    {
        Stamp stamp;
        const QFileInfo info(file);
        if (!info.isFile())
            return stamp;
        stamp.size = info.size();
        stamp.time = info.lastModified();
        QFile content(file);
        if (stamp.size <= 4 * 1024 * 1024 && content.open(QIODevice::ReadOnly))
            stamp.hash = QCryptographicHash::hash(content.readAll(), QCryptographicHash::Sha1);
        return stamp;
    }
    struct Edit
    {
        QString directory, name, mode; // remote folder and name; mode and copy as in Transfer
        bool copy = false;
        Stamp stamp; // the local copy as the server has it (downloaded, or last upload started)
    };
    QHash<QString, Edit> edited; // local copy -> remote file
    QSet<QString> pendingEdits;
    // Edit copies live in folders of this panel alone, one per remote folder: a path is never shared with another
    // panel, server or run (an editor left open on such a copy would upload it to the wrong file).
    const QString editId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QHash<QString, QString> editFolders; // remote folder -> local folder
    QString editFolder(const QString &directory)
    {
        if (const auto known = editFolders.value(directory); !known.isEmpty())
            return known;
        const auto base = sftpTempFolder("CrossTerm-sftp");
        const auto dir = QDir(base).filePath(editId + "/" + QString::number(editFolders.size() + 1));
        if (base.isEmpty() || QFileInfo::exists(dir) || !QDir().mkpath(dir))
            return {};
        return editFolders[directory] = dir;
    }
    // Octal mode of a listed file when a save may replace it through a temp name without changing who can use it:
    // a plain file owned by the session's user, no special bits, the same rights for its group as for everyone else
    // (the new file gets the user's own group) and no second name (a link count above 1: a rename would leave the
    // other names with the old content). Empty otherwise: saves then write straight into the file, which keeps its
    // owner, group, other names and ACL. So do names that sftp would read as a pattern, or too long for the temp name.
    // copy: the listing does not tell the link count (sftp prints '?' for servers that send user names apart from
    // the listing, OpenSSH 9.1 and later). The temp file is then copied onto the file by the server ("cp", on
    // those same servers) instead of renamed: the file stays the same one for all its names.
    QString replaceMode(const QString &directory, const QString &name, bool &copy) const
    {
        copy = false;
        static const QRegularExpression plain("^-([r-][w-][x-]){3}$"), pattern("[*?\\[\\\\]");
        if (directory != listed || login.isEmpty() || name.contains(pattern) || name.toUtf8().size() > 230)
            return {};
        for (int i = 0; i < files->topLevelItemCount(); ++i)
        {
            const auto *item = files->topLevelItem(i);
            const auto access = item->text(5);
            if (item->text(0) != name)
                continue;
            const int links = item->data(0, Qt::UserRole + 2).toInt();
            if (!plain.match(access).hasMatch() || access.mid(4, 3) != access.mid(7, 3) || item->text(3) != login || links > 1)
                return {};
            copy = links < 1;
            return sftpOctalMode(access);
        }
        return {};
    }
    // The server has the content of this edit copy: the same as when it was downloaded or last uploaded, and no
    // upload of it waiting or under way.
    bool uploaded(const QString &file) const
    {
        const auto edit = edited.constFind(file);
        return edit != edited.constEnd() && stampOf(file) == edit->stamp &&
               std::none_of(transfers.begin(), transfers.end(), [&file](const auto &t)
                            { return t->upload && t->local == file && (t->state == "queued" || t->state == "running"); });
    }
    // A watched copy changed, or may have: uploaded when it differs from what the server has.
    void syncEdit(const QString &file, int retries = 5)
    {
        if (!edited.contains(file))
            return;
        const auto edit = edited.value(file);
        bool queued = false;
        for (const auto &t : transfers)
        {
            if (t->local != file || (t->state != "queued" && t->state != "running"))
                continue;
            if (!t->upload)
                return; // being downloaded again: compared with that copy from then on
            queued = queued || t->state == "queued";
        }
        if (!QFileInfo::exists(file))
        {
            // An editor that saves by replacing the file may be half way: look again shortly, a few times.
            if (retries > 0)
                QTimer::singleShot(600, this, [this, file, retries] { syncEdit(file, retries - 1); });
            return;
        }
        watcher->addPath(file); // editors often replace the file, which ends the watch
        // An upload that has not started yet sends this content; while one runs, exactly one more is queued.
        if (queued || stampOf(file) == edit.stamp)
            return;
        status->setText(L("Uploading changes of %1…").arg(edit.name));
        const auto size = QFileInfo(file).size();
        // One row per edited file for its saves, not one per save: the row of an earlier save that ended is
        // queued again. A second row exists only while one upload runs and the next save waits.
        std::shared_ptr<Transfer> row;
        for (int i = int(transfers.size()) - 1; i >= 0; --i)
        {
            const auto t = transfers[i];
            if (!t->edit || !t->upload || t->local != file || t->state == "running" || t->process)
                continue;
            if (row)
            {
                delete row->item;
                transfers.removeOne(row);
            }
            row = t;
        }
        if (row)
        {
            row->size = size;
            row->mode = edit.mode;
            row->copy = edit.copy;
            row->stage = row->firstStage();
            row->resume = row->cancelled = false;
            row->note.clear();
            row->state = "queued";
            row->item->setText(1, fileSizeText(size));
            row->bar->setToolTip({});
            row->bar->setRange(0, 100);
            row->bar->setValue(0);
            showState(row);
            queueBox->show();
            pump();
            return;
        }
        enqueue(edit.directory, edit.name, file, true, false, size, false,
                [&](Transfer &t)
                {
                    t.edit = true;
                    t.mode = edit.mode;
                    t.copy = edit.copy;
                    t.stage = t.firstStage();
                });
    }
    // Recursive delete: walk the folders, then a single rm/rmdir batch.
    QProcess *scanner = hideConsoleWindow(new QProcess(this));
    QStringList scanQueue, foundFiles, foundDirs;
    // Work that waits for the listing in progress: the delete batch, a refresh after an upload.
    bool deletePending = false, relist = false;
    // Follow terminal folder.
    QCheckBox *follow = new QCheckBox;
    QTimer *followTimer = new QTimer(this);
    QString home, followed;
    QTemporaryFile *emptyFile = nullptr; // "Create new file": an empty local file uploaded under the new name
    // Drag out: the sftp processes of fetchBlocking calls on the stack, and whether the window cancelled the drag.
    QList<QProcess *> fetchers;
    bool dragCancelled = false;

    // A drag or its download returned: the window is told once neither is on the stack any more.
    void dragFinished()
    {
        if (dragActive())
            return;
        dragCancelled = false;
        QTimer::singleShot(0, this,
                           [this]
                           {
                               if (const auto ended = onDragEnded; ended && !dragActive())
                                   ended();
                           });
    }

    void setLink(int state)
    {
        if (link != state)
            dot->setPixmap(sftpStateDot(link = state));
        // After a failed connection the list is what the server had: shown faded until the next listing.
        if (state == 1)
            files->setGraphicsEffect(nullptr);
        else if (state == 2 && !files->graphicsEffect())
        {
            auto *fade = new QGraphicsOpacityEffect(files);
            fade->setOpacity(0.45);
            files->setGraphicsEffect(fade);
        }
    }
    // sftp echoes the commands of its batch once it is connected: no echo, no connection.
    static bool reached(const QString &output) { return output.contains("sftp> "); }

    // Environment of every sftp process: askpass, and a UTF-8 LC_CTYPE when the user's locale is not one.
    QProcessEnvironment sftpEnvironment(bool interactive) const
    {
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto &entry : askpassEnvironment(target, interactive))
            environment.insert(entry.section('=', 0, 0), entry.section('=', 1));
        QString ctype; // the first of these that is set decides LC_CTYPE
        for (const auto &name : {"LC_ALL", "LC_CTYPE", "LANG"})
        {
            if (ctype.isEmpty())
                ctype = environment.value(name);
        }
        const auto utf8 = sftpUtf8Locale();
        if (!utf8.isEmpty() && !ctype.contains(QRegularExpression("utf-?8", QRegularExpression::CaseInsensitiveOption)))
            environment.insert(environment.value("LC_ALL").isEmpty() ? "LC_CTYPE" : "LC_ALL", utf8);
        return environment;
    }
    QStringList sftpArguments(bool interactive) const
    {
        // BatchMode=no before -b: otherwise sftp disables password authentication (askpass).
        QStringList arguments{"-o", "BatchMode=no", "-b", "-", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=10"};
        // Askpass cannot ask the user: one password attempt (the saved or session one, if any) instead of three,
        // which without a password are three empty ones (lockout by fail2ban).
        if (!interactive)
            arguments << "-o" << "NumberOfPasswordPrompts=1";
        return arguments + command.arguments;
    }

    void startSftp(QProcess *proc, const QByteArray &batch, bool interactive)
    {
        proc->setProcessEnvironment(sftpEnvironment(interactive));
        proc->setProperty("batch", batch);
        proc->start(QStandardPaths::findExecutable(command.program), sftpArguments(interactive));
    }

    void scanNext()
    {
        // A folder whose path cannot be quoted is skipped: "cd" without a path would list the home folder.
        scanQueue.erase(
            std::remove_if(scanQueue.begin(), scanQueue.end(), [](const QString &dir) { return sftpQuote(dir).isEmpty(); }),
            scanQueue.end());
        if (scanQueue.isEmpty())
        {
            // A listing in progress would refuse the batch: it runs when that listing has finished.
            deletePending = process->state() != QProcess::NotRunning;
            if (deletePending)
                return;
            QStringList ops;
            for (const auto &file : foundFiles)
            {
                if (const auto quoted = sftpQuote(file); !quoted.isEmpty())
                    ops << "rm " + quoted;
            }
            auto dirs = foundDirs;
            std::sort(dirs.begin(), dirs.end(), [](const QString &a, const QString &b) { return a.count('/') > b.count('/'); });
            for (const auto &dir : dirs)
            {
                if (const auto quoted = sftpQuote(dir); !quoted.isEmpty())
                    ops << "rmdir " + quoted;
            }
            foundFiles.clear();
            foundDirs.clear();
            run(listed, ops);
            return;
        }
        // Up to 50 folders per sftp connection instead of one each; their listings are told apart by the
        // "sftp> cd" lines that sftp echoes in batch mode.
        const auto dirs = scanQueue.mid(0, 50);
        scanQueue.remove(0, dirs.size());
        QByteArray batch;
        for (const auto &dir : dirs)
            batch += ("cd " + sftpQuote(dir) + "\nls -la\n").toUtf8();
        scanner->setProperty("dirs", dirs);
        startSftp(scanner, batch, true);
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
    static qint64 bytesRead(qint64 pid, const QString &local)
    {
#ifdef Q_OS_WIN
        Q_UNUSED(local);
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
        Q_UNUSED(local);
        QFile io(QString("/proc/%1/io").arg(pid));
        if (!io.open(QIODevice::ReadOnly))
            return -1;
        for (const auto &line : io.readAll().split('\n'))
        {
            if (line.startsWith("rchar:"))
                return line.mid(6).trimmed().toLongLong();
        }
        return -1;
#elif defined(Q_OS_MACOS)
        // No I/O counters per process: the offset of its open descriptor on the file (same device and inode).
        struct stat info;
        if (::stat(QFile::encodeName(local).constData(), &info) != 0)
            return -1;
        const int bytes = proc_pidinfo(int(pid), PROC_PIDLISTFDS, 0, nullptr, 0);
        if (bytes <= 0)
            return -1;
        std::vector<proc_fdinfo> fds(size_t(bytes) / sizeof(proc_fdinfo));
        const int count = proc_pidinfo(int(pid), PROC_PIDLISTFDS, 0, fds.data(), int(fds.size() * sizeof(proc_fdinfo))) /
                          int(sizeof(proc_fdinfo));
        for (int i = 0; i < count; ++i)
        {
            vnode_fdinfo vnode{};
            if (fds[size_t(i)].proc_fdtype == PROX_FDTYPE_VNODE &&
                proc_pidfdinfo(int(pid), fds[size_t(i)].proc_fd, PROC_PIDFDVNODEINFO, &vnode, PROC_PIDFDVNODEINFO_SIZE) ==
                    PROC_PIDFDVNODEINFO_SIZE &&
                vnode.pvi.vi_stat.vst_dev == uint32_t(info.st_dev) && vnode.pvi.vi_stat.vst_ino == info.st_ino)
                return vnode.pfi.fi_offset;
        }
        return -1;
#else
        Q_UNUSED(pid);
        Q_UNUSED(local);
        return -1;
#endif
    }
    // Whether bytesRead() can follow uploads here (the counters of this process tell on Windows and Linux).
    static bool uploadProgress()
    {
#ifdef Q_OS_MACOS
        return true;
#else
        return bytesRead(QCoreApplication::applicationPid(), {}) >= 0;
#endif
    }

    // The status of a row is written inside its progress bar (the Status column stays hidden, so the queue needs
    // no horizontal scrolling): the percentage with the speed while running, the state otherwise.
    void setStatus(Transfer &t, const QString &text)
    {
        t.item->setText(3, text);
        // A narrow column clipped the centred text on both sides: drop the time left, then the speed, so what is
        // shown fits ("100%" measures the widest percentage); other states are elided on the right.
        const auto metrics = t.bar->fontMetrics();
        const int room = t.bar->width() - 8;
        if (t.state == "running")
        {
            const auto rate = t.rate.text();
            QString format = "%p%";
            if (!rate.isEmpty())
                for (const QString &c : {"%p% · " + rate, "%p% · " + rate.section(" — ", 0, 0)})
                    if (metrics.horizontalAdvance(QString(c).replace("%p", "100")) <= room)
                    {
                        format = c;
                        break;
                    }
            t.bar->setFormat(format);
        }
        else
            t.bar->setFormat(text.isEmpty() ? "%p%" : metrics.elidedText(text, Qt::ElideRight, room));
    }
    void showState(const std::shared_ptr<Transfer> &t)
    {
        static const QHash<QString, const char *> labels{{"queued", N("Queued")},
                                                         {"running", N("Transferring")},
                                                         {"done", N("Completed")},
                                                         {"failed", N("Failed")},
                                                         {"cancelled", N("Cancelled")}};
        if (t->state == "running" && !t->rate.text().isEmpty())
            setStatus(*t, t->rate.text()); // speed and time left, once known
        else if (t->state == "done" && !t->note.isEmpty())
            setStatus(*t, L("Completed") + ": " + t->note.section('\n', 0, 0)); // all of it in the tooltip
        else if (t->state != "failed")
            setStatus(*t, L(labels.value(t->state, "")));
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
            // One at a time onto the same remote file or local path: two saves of an edited file uploaded together
            // would mix their content on the server.
            if (std::any_of(transfers.begin(), transfers.end(),
                            [&t](const auto &o)
                            {
                                return o->state == "running" &&
                                       (o->local == t->local ||
                                        (o->upload && t->upload && o->directory == t->directory && o->name == t->name));
                            }))
                continue;
            const auto program = QStandardPaths::findExecutable(command.program);
            const auto directory = sftpQuote(t->directory), name = sftpQuote(t->name),
                       local = sftpQuote(QDir::fromNativeSeparators(t->local));
            if (program.isEmpty() || directory.isEmpty() || name.isEmpty() || local.isEmpty() || !command.error.isEmpty())
            {
                t->state = "failed";
                setStatus(*t,
                                 L("Failed") + ": " + (command.error.isEmpty() ? L("Invalid path.") : describe(command.error)));
                continue;
            }
            // No upload of a copy while it is downloaded again: watched again when the download has finished.
            if (!t->upload && watcher->files().contains(t->local))
                watcher->removePath(t->local);
            // reget/reput resume an interrupted transfer from where it stopped (see resumeTransfer).
            const QString verb = t->upload ? (t->resume ? "reput" : "put") : (t->resume ? "reget" : "get");
            const QString flags = t->folder ? " -r" : "";
            if (!t->resume)
            {
                // A new start: what Resume may later rely on is recorded from this attempt on.
                const QFileInfo source(t->local);
                t->sent = false;
                t->sourceSize = source.size();
                t->sourceTime = source.lastModified();
                t->stamp.clear();
                t->started = QDateTime::currentSecsSinceEpoch() * 1000; // whole seconds, as coarse file times are
            }
            QString batch = "cd " + directory + "\n";
            const auto part = sftpQuote("." + t->name + ".crossterm-part"), old = sftpQuote("." + t->name + ".crossterm-old");
            if (t->edit && t->stage < 2)
            {
                if (t->stage == 0)
                    // The file is complete under a temp name before it takes the place of the original (with its
                    // mode): an interrupted upload leaves the original as it was. copy: the server writes the temp
                    // file into the original, which stays the same file, and the temp file is removed.
                    batch += "put " + local + " " + part + "\nchmod " + t->mode + " " + part + "\n" +
                             (t->copy ? "cp " + part + " " + name + "\n-rm " + part : "rename " + part + " " + name) + "\n";
                else
                    // The server's rename does not replace: the original steps aside first ("-l" never replaces,
                    // "ls" stops the batch when the original or the new file is not there), so one of the two names
                    // always holds a complete file. A leftover of an earlier swap is removed only while both exist.
                    batch += "ls " + name + "\nls " + part + "\n-rm " + old + "\nrename -l " + name + " " + old + "\nrename -l " +
                             part + " " + name + "\n-rm " + old + "\n";
            }
            else
            {
                if (!t->upload && !t->folder)
                    batch += "-ls -l " + name + "\n"; // its line tells a resume whether the remote file changed
                if (t->edit && t->stage == 3)
                    batch += "-rm " + part + "\n"; // what a copy that did not work left
                batch += verb + flags + " " + (t->upload ? local + " " + name : name + " " + local) + "\n";
            }
            if (auto edit = edited.find(t->local); t->edit && t->stage != 1 && edit != edited.end())
                edit->stamp = stampOf(t->local); // what this upload sends: a later save differs from it
            auto *mover = hideConsoleWindow(new QProcess(this));
            mover->setProcessEnvironment(sftpEnvironment(true));
            mover->setProperty("batch", batch.toUtf8());
            t->process = mover;
            watchTransfer(t);
            ++running;
            t->state = "running";
            t->cancelled = false;
            t->rate.reset();
            if (t->folder || (t->upload && !uploadProgress()))
                t->bar->setRange(0, 0);
            else
                t->bar->setRange(0, 100);
            showState(t);
            mover->start(program, sftpArguments(true));
            ticker->start(150);
        }
    }

    void tick()
    {
        for (const auto &t : transfers)
        {
            if (t->state != "running" || !t->process || t->folder || (t->upload && t->size <= 0))
                continue;
            qint64 done = -1;
            if (t->upload)
                done = t->process->state() == QProcess::Running ? bytesRead(t->process->processId(), t->local) : -1;
            else
                done = QFileInfo(t->local).size();
            if (done < 0)
                continue;
            if (t->upload && done > 0)
                t->sent = true; // sftp reads the file only once the remote one is open (and emptied)
            if (t->size > 0)
                t->bar->setValue(int(std::min<qint64>(99, done * 100 / t->size)));
            if (t->rate.add(done, t->size))
                setStatus(*t, t->rate.text());
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
    // prepare: sets more of the item before it may start.
    void enqueue(const QString &directory, const QString &name, const QString &local, bool upload, bool folder, qint64 size,
                 bool open = false, const std::function<void(Transfer &)> &prepare = nullptr)
    {
        auto t = std::make_shared<Transfer>();
        t->directory = directory;
        t->name = name;
        t->local = local;
        t->upload = upload;
        t->folder = folder;
        t->size = size;
        t->open = open;
        if (prepare)
            prepare(*t);
        t->state = "queued";
        t->item = new QTreeWidgetItem(queueView, {name, folder ? QString() : fileSizeText(size)});
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
        // reput/reget append to the file that is there: only to the partial file that the last attempt of this item
        // left (an upload that sent bytes; a download that reached the remote file, local file written since), and
        // only from a source that is the same as then. Otherwise the transfer starts over.
        const QFileInfo local(t->local);
        if (t->upload)
            t->resume = t->sent && local.size() == t->sourceSize && local.lastModified() == t->sourceTime;
        else
            t->resume = !t->stamp.isEmpty() && local.isFile() && local.lastModified().toMSecsSinceEpoch() >= t->started;
        t->resume = t->resume && !t->folder && !t->edit;
        if (t->edit)
            t->stage = t->firstStage();
        t->note.clear();
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
    // Nothing queued or running (an upload waiting for another one of the same file counts as queued).
    bool transfersIdle() const
    {
        return std::none_of(transfers.begin(), transfers.end(),
                            [](const auto &t) { return t->state == "queued" || t->state == "running"; });
    }
    // Local copies opened for editing that still exist: their saves are uploaded only while this panel lives.
    int openEdits() const
    {
        return int(
            std::count_if(edited.keyBegin(), edited.keyEnd(), [](const QString &file) { return QFileInfo::exists(file); }));
    }
    // Of those, the copies whose content the server does not have: saved a moment ago and not queued yet, being
    // uploaded, or their upload failed or was cancelled.
    int unsavedEdits() const
    {
        return int(std::count_if(edited.keyBegin(), edited.keyEnd(),
                                 [this](const QString &file) { return QFileInfo::exists(file) && !uploaded(file); }));
    }
    // Local copy of a remote file of the given folder (the listed one by default) once openRemote was called for
    // a file of that folder; empty before.
    QString editPath(const QString &name, const QString &directory = {}) const
    {
        const auto folder = editFolders.value(directory.isEmpty() ? listed : directory);
        return folder.isEmpty() ? QString() : QDir(folder).filePath(name);
    }
    std::function<bool(const QString &)> askReplace; // tests: answers the "Replace?" question instead of the dialog

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
        // As wide as the panel, never wider: name and progress share the width, the size fits its text.
        queueView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        queueView->setColumnHidden(3, true);
        queueView->header()->setStretchLastSection(false);
        queueView->header()->setMinimumSectionSize(30);
        queueView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        queueView->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        queueView->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        box->addWidget(queueView);
        auto *row = new SftpFlowLayout;
        row->setSpacing(4);
        auto *cancel = texts.text(new QPushButton, N("Cancel"));
        auto *resume = texts.text(new QPushButton, N("Resume"));
        auto *clear = texts.text(new QPushButton, N("Clear"));
        texts.tip(clear, N("Clear finished"));
        for (auto *b : {cancel, resume, clear})
        {
            b->setStyleSheet("padding: 2px 8px;");
            row->addWidget(b);
        }
        box->addLayout(row);
        connect(cancel, &QPushButton::clicked, this, [this] { cancelTransfer(selectedTransfer()); });
        connect(resume, &QPushButton::clicked, this, [this] { resumeTransfer(selectedTransfer()); });
        connect(clear, &QPushButton::clicked, this,
                [this]
                {
                    for (int i = int(transfers.size()) - 1; i >= 0; --i)
                    {
                        // Ended ones, failed and cancelled included (a cancelled one counts as running until
                        // its process has gone).
                        const auto &state = transfers[i]->state;
                        if ((state == "done" || state == "failed" || state == "cancelled") && !transfers[i]->process)
                        {
                            removeTemporary(*transfers[i]);
                            delete transfers[i]->item;
                            transfers.removeAt(i);
                        }
                    }
                    if (transfers.isEmpty())
                        queueBox->hide();
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
                    const auto lines = QString::fromUtf8(mover->readAllStandardOutput()).split(QRegularExpression("[\\r\\n]+"));
                    const bool connected = reached(lines.join('\n'));
                    forgetAskpassAttempts(mover);
                    mover->deleteLater();
                    t->process = nullptr;
                    // The echo of a command of the batch (file names cannot hold a line break).
                    auto echoed = [&lines](const QString &command)
                    { return int(lines.indexOf(QRegularExpression("^sftp> " + QRegularExpression::escape(command) + ".*"))); };
                    t->note.clear();
                    if (t->cancelled)
                        t->state = "cancelled";
                    else if (code == 0 && exit == QProcess::NormalExit)
                    {
                        t->state = "done";
                        // "put -r" and "get -r" skip what is not a file or a folder (links) and still end well.
                        t->note = error.split(QRegularExpression("[\\r\\n]+")).filter("not a regular file").join('\n');
                        t->bar->setToolTip(t->note);
                    }
                    else
                    {
                        t->state = "failed";
                        setStatus(*t,
                                         L("Failed") + ": " + (error.isEmpty() ? L("SFTP operation failed.") : error.left(200)));
                        t->bar->setToolTip(error);
                        if (!connected)
                            setLink(2);
                    }
                    if (!t->upload && !t->folder)
                    {
                        // The "ls -l" line of the remote file, when this attempt got that far.
                        const int at = echoed("-ls -l ");
                        const auto stamp =
                            at >= 0 && !lines.value(at + 1).startsWith("sftp> ") ? lines.value(at + 1).simplified() : QString();
                        if (!t->resume)
                            t->stamp = stamp;
                        else if (!stamp.isEmpty() && stamp != t->stamp)
                        {
                            // Resumed onto a remote file that is not the one of the first attempt: start over.
                            t->stamp.clear();
                            t->resume = false;
                            if (t->state == "done")
                                t->state = "queued";
                        }
                    }
                    if (t->edit && t->state == "failed" && connected && t->stage == 0)
                    {
                        // Not replaced through the temp name: the server's rename refuses an existing name (swap),
                        // or the folder takes no new file (straight onto the file, as before), or the server did
                        // not copy the temp file onto the file (straight onto it, now and for the later saves).
                        const bool swap = echoed("rename ") >= 0, copy = echoed("cp ") >= 0;
                        if (copy)
                        {
                            t->mode.clear();
                            if (auto edit = edited.find(t->local); edit != edited.end())
                                edit->mode.clear();
                        }
                        if (swap || copy || (echoed("chmod ") < 0 && error.contains("Permission denied")))
                        {
                            t->stage = swap ? 1 : copy ? 3 : 2;
                            t->state = "queued";
                        }
                    }
                    else if (t->edit && t->stage == 3 && echoed("-rm ") >= 0)
                    {
                        // The temp file is gone: plainly straight onto the file from now on.
                        t->copy = false;
                        if (auto edit = edited.find(t->local); edit != edited.end())
                            edit->copy = false;
                    }
                    if (auto edit = edited.find(t->local); edit != edited.end())
                    {
                        if (!t->upload)
                            edit->stamp = stampOf(t->local); // downloaded again (or not all of it: not the user's change)
                        else if (t->edit && t->state != "done" && t->state != "queued")
                            edit->stamp = {}; // not on the server: the next save, or opening it again, counts as a change
                    }
                    if (t->bar->maximum() == 0)
                        t->bar->setRange(0, 100);
                    showState(t);
                    // A failed or cancelled zip download stays on the server for Resume until its row is cleared.
                    if (t->state == "done")
                        removeTemporary(*t);
                    if (t->state == "done" && t->upload && t->directory == listed)
                    {
                        if (process->state() == QProcess::NotRunning)
                            refresh();
                        else
                            relist = true; // the listing in progress may miss this file: list again after it
                    }
                    if (t->state == "done" && t->open)
                    {
                        sftpMarkDownloaded(t->local);
                        edited[t->local] = {t->directory, t->name, t->mode, t->copy, stampOf(t->local)};
                        if (launchEditor && !openLocal(t->local, t->openMode))
                            status->setText(L("Unsafe file name from the server: %1").arg(t->name));
                    }
                    // Watched only now that no download writes to it; an upload that ended looks for a save it missed.
                    if (!t->upload && t->state != "queued" && edited.contains(t->local) && QFileInfo::exists(t->local))
                        watcher->addPath(t->local);
                    else if (t->edit && t->state == "done")
                        QTimer::singleShot(0, this, [this, file = t->local] { syncEdit(file); });
                    if (!runningTransfers())
                        ticker->stop();
                    pump();
                    QTimer::singleShot(2000, this, [this] { hideQueueIfIdle(); });
                });
        connect(mover, &QProcess::errorOccurred, this,
                [this, t, mover](QProcess::ProcessError e)
                {
                    if (e != QProcess::FailedToStart)
                        return;
                    t->state = "failed";
                    setStatus(*t, L("Failed") + ": " + mover->errorString());
                    if (auto edit = edited.find(t->local); t->edit && edit != edited.end())
                        edit->stamp = {}; // not on the server
                    setLink(2);
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
            setLink(2);
            return;
        }
        const auto program = QStandardPaths::findExecutable(command.program);
        if (program.isEmpty())
        {
            status->setText(L("OpenSSH sftp client not found in PATH."));
            setLink(2);
            return;
        }
        const auto remote = sftpQuote(directory);
        if (remote.isEmpty())
        {
            status->setText(L("Invalid path."));
            return;
        }
        // "pwd" before "cd" gives the folder sftp starts in, the remote home (used to resolve "~").
        QString batch = "pwd\ncd " + remote + "\n";
        for (const auto &op : operations)
            batch += "-" + op + "\n";
        batch += "pwd\nls -la\n";
        process->setProperty("batch", batch.toUtf8());
        process->setProcessEnvironment(sftpEnvironment(interactive));
        status->setText(L("Listing %1…").arg(directory));
        if (link == 2)
            setLink(0); // connecting again
        files->setCursor(Qt::BusyCursor);
        process->start(program, sftpArguments(interactive));
    }
    void retranslate()
    {
        texts.apply();
        files->setHeaderLabels({L("Name"), L("Size"), L("Last modified"), L("Owner"), L("Group"), L("Access")});
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
        return process->state() != QProcess::NotRunning || scanner->state() != QProcess::NotRunning || !scanQueue.isEmpty() ||
               deletePending || remover->state() != QProcess::NotRunning;
    }
    std::function<QString()> folderSource;
    std::function<void(const QString &)> pasteToTerminal; // "Copy file path to terminal"
    std::function<void(bool)> onMonitoring;               // the window starts/stops monitoring the session
    QPushButton *monitoring = new QPushButton;            // Remote monitoring toggle
    QProgressBar *jobBar = new QProgressBar;              // progress of an extraction, above Remote monitoring
    bool launchEditor = true;                             // false in tests: no external editor
    void setFollow(bool on) { follow->setChecked(on); }
    QCheckBox *followBox() const { return follow; }
    // Downloads to a temporary folder and opens it (with the "Open with" chooser when chooseApp);
    // every save of the local copy is uploaded back to the server. directory: the folder of the file when the
    // user chose it (the listed one by default).
    void openRemote(const QString &name, qint64 size, int openMode = 0, QString directory = {})
    {
        if (directory.isEmpty())
            directory = listed;
        const auto dir = editFolder(directory);
        if (dir.isEmpty() || !safeLocalName(name))
        {
            status->setText(L("Invalid path."));
            return;
        }
        const auto local = QDir(dir).filePath(name);
        // Already on its way (a second double click) or being uploaded: downloading over the copy now would have
        // the half-written file uploaded as a save.
        for (const auto &t : transfers)
        {
            if (t->local == local && (t->state == "queued" || t->state == "running"))
            {
                queueView->setCurrentItem(t->item);
                return;
            }
        }
        // A copy with changes that are not on the server (their upload failed): opened as it is, not downloaded over.
        if (const auto edit = edited.constFind(local);
            edit != edited.constEnd() && QFileInfo::exists(local) && !(stampOf(local) == edit->stamp))
        {
            if (launchEditor && !openLocal(local, openMode))
                status->setText(L("Unsafe file name from the server: %1").arg(name));
            return;
        }
        enqueue(directory, name, local, false, false, size, true,
                [&](Transfer &t)
                {
                    t.openMode = openMode;
                    t.mode = replaceMode(directory, name, t.copy);
                });
    }
    // Opens a downloaded copy; false when it is of a type that the system would run and there is no text editor
    // to show it with instead.
    static bool openLocal(const QString &file, int openMode)
    {
        const bool runnable = sftpRunnable(QFileInfo(file).fileName());
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(file);
        if (openMode == 1 && !runnable)
        {
            const auto wide = native.toStdWString();
            OPENASINFO info{wide.c_str(), nullptr, OAIF_ALLOW_REGISTRATION | OAIF_EXEC};
            if (SUCCEEDED(SHOpenWithDialog(nullptr, &info)))
                return true;
        }
        if ((openMode == 2 || runnable) && QProcess::startDetached("notepad.exe", {native}))
            return true;
#elif defined(Q_OS_MACOS)
        if ((openMode == 2 || runnable) && QProcess::startDetached("/usr/bin/open", {"-t", file})) // the default text editor
            return true;
#else
        if ((openMode == 2 || runnable) && sftpOpenInTextEditor(file))
            return true;
#endif
        if (!runnable)
            QDesktopServices::openUrl(QUrl::fromLocalFile(file));
        return !runnable;
    }
    // Rows shown: those whose name holds the filter text, hidden files ("." names) only when they are shown.
    void applyFilter()
    {
        const auto text = filter->text().trimmed();
        const bool hidden = !hiddenToggle || hiddenToggle->isChecked();
        for (int i = 0; i < files->topLevelItemCount(); ++i)
        {
            auto *item = files->topLevelItem(i);
            const auto name = item->text(0);
            const bool hide = name != ".." && ((!hidden && name.startsWith('.')) ||
                                               (!text.isEmpty() && !name.contains(text, Qt::CaseInsensitive)));
            if (hide)
                item->setSelected(false); // a hidden row must not be downloaded or deleted with the selection
            item->setHidden(hide);
        }
    }
    void goHistory(int direction)
    {
        auto &from = direction < 0 ? backStack : forwardStack;
        if (from.isEmpty() || process->state() != QProcess::NotRunning)
            return;
        historyMove = direction;
        historyTarget = from.last();
        run(historyTarget);
    }
    // After a listing: the folder before it goes into the history (or moves between Back and Forward).
    void recordHistory(const QString &before)
    {
        const int move = std::exchange(historyMove, 0);
        if (before.isEmpty() || before == listed)
            return;
        filter->clear(); // the filter was for the folder left
        if (move && listed == historyTarget)
        {
            (move < 0 ? backStack : forwardStack).removeLast();
            (move < 0 ? forwardStack : backStack) << before;
        }
        else
        {
            backStack << before;
            forwardStack.clear();
            if (backStack.size() > 50)
                backStack.removeFirst();
        }
        backAction->setEnabled(!backStack.isEmpty());
        forwardAction->setEnabled(!forwardStack.isEmpty());
    }
    // Favourite folders of this server (its credential target: user, host and port).
    QString bookmarksKey() const { return "sftp/bookmarks/" + QString::fromLatin1(target.toUtf8().toHex()); }
    QStringList bookmarks() const { return QSettings().value(bookmarksKey()).toStringList(); }
    void fillBookmarks(QMenu *menu)
    {
        menu->clear();
        auto saved = bookmarks();
        const auto here = listed;
        if (!here.isEmpty() && !saved.contains(here))
            menu->addAction(icon("plus", 16), L("Add current folder"),
                            [this, here]
                            {
                                auto list = bookmarks();
                                list << here;
                                QSettings().setValue(bookmarksKey(), list);
                            });
        else if (!here.isEmpty())
            menu->addAction(icon("delete", 16), L("Remove current folder"),
                            [this, here]
                            {
                                auto list = bookmarks();
                                list.removeAll(here);
                                QSettings().setValue(bookmarksKey(), list);
                            });
        if (!saved.isEmpty())
            menu->addSeparator();
        for (const auto &folder : saved)
            menu->addAction(icon("folder", 16), folder, [this, folder] { run(folder); });
    }
    // Lists the current folder again, after the listing in progress if there is one.
    void relistNow()
    {
        if (process->state() != QProcess::NotRunning)
            relist = true;
        else
            refresh();
    }
    // The queue is hidden once nothing is waiting or running: completed rows go after a moment; failed and
    // cancelled ones stay for Resume.
    void hideQueueIfIdle()
    {
        for (const auto &t : std::as_const(transfers))
        {
            if (t->state == "queued" || t->state == "running" || t->process)
                return;
        }
        for (int i = int(transfers.size()) - 1; i >= 0; --i)
        {
            if (transfers[i]->state == "done")
            {
                removeTemporary(*transfers[i]);
                delete transfers[i]->item;
                transfers.removeAt(i);
            }
        }
        if (transfers.isEmpty())
            queueBox->hide();
    }
    // Runs script in the remote shell over ssh (shell of the session's server); done gets its exit code (-1: ssh
    // could not start or crashed), standard output and standard error. false: no ssh for this session.
    // output: standard output as it arrives (then not passed to done).
    bool runShell(const QString &script, bool interactive,
                  const std::function<void(int, const QString &, const QString &)> &done,
                  const std::function<void(const QByteArray &)> &output = nullptr)
    {
        const auto program = QStandardPaths::findExecutable(shell.program);
        if (!shell.error.isEmpty() || shell.program != "ssh" || !shell.arguments.contains("--") || program.isEmpty())
            return false;
        auto *proc = hideConsoleWindow(new QProcess(this));
        proc->setStandardInputFile(QProcess::nullDevice());
        proc->setProcessEnvironment(sftpEnvironment(interactive));
        auto finish = [this, proc, done](int code)
        {
            if (proc->property("ended").toBool())
                return;
            proc->setProperty("ended", true);
            forgetAskpassAttempts(proc);
            const auto out = QString::fromUtf8(proc->readAllStandardOutput());
            const auto err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
            proc->deleteLater();
            if (done)
                done(code, out, err);
        };
        if (output)
            connect(proc, &QProcess::readyReadStandardOutput, this, [proc, output] { output(proc->readAllStandardOutput()); });
        connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [finish](int code, QProcess::ExitStatus exit) { finish(exit == QProcess::NormalExit ? code : -1); });
        connect(proc, &QProcess::errorOccurred, this,
                [finish](QProcess::ProcessError error)
                {
                    if (error == QProcess::FailedToStart)
                        finish(-1);
                });
        QStringList arguments{"-T", "-o", "BatchMode=no", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=10"};
        if (!interactive)
            arguments << "-o" << "NumberOfPasswordPrompts=1";
        // Through sh: the same syntax whatever the login shell of the account (fish, csh).
        proc->start(program, arguments + shell.arguments + QStringList{"sh -c " + posixQuote(script)});
        return true;
    }
    bool has(const QString &tool) const { return tools.contains(tool); }
    // Which programs the server has, asked once after the first listing (none without a shell: SFTP-only account).
    void checkTools()
    {
        runShell("for t in zip unzip tar du sha256sum shasum cp chown find; do command -v $t >/dev/null 2>&1 && echo "
                 "$t; done; exit 0",
                 false,
                 [this](int code, const QString &out, const QString &)
                 {
                     if (code != 0)
                         return;
                     for (const auto &line : out.split('\n', Qt::SkipEmptyParts))
                         tools.insert(line.trimmed());
                 });
    }
    // A command run on the server for an operation on the listed folder, which is listed again afterwards; its
    // error (if any) stays as the status.
    void shellJob(const QString &doing, const QString &script,
                  const std::function<void(const QByteArray &)> &output = nullptr)
    {
        status->setText(doing);
        files->setCursor(Qt::BusyCursor);
        const bool started = runShell(script, true,
                                      [this](int code, const QString &, const QString &error)
                                      {
                                          files->unsetCursor();
                                          jobBar->hide();
                                          note = code == 0 ? QString()
                                                           : (error.isEmpty() ? L("SFTP operation failed.") : error.left(400));
                                          relistNow();
                                      });
        if (!started)
        {
            files->unsetCursor();
            jobBar->hide();
            status->setText(L("SFTP operation failed."));
        }
    }
    void removeTemporary(Transfer &t)
    {
        if (!t.temporary)
            return;
        t.temporary = false;
        if (const auto quoted = posixQuote(t.directory + "/" + t.name); !quoted.isEmpty())
            runShell("rm -f -- " + quoted, false, nullptr);
    }
    // "zip" or "tar" for an archive the server can extract, empty otherwise.
    QString archiveKind(const QString &name) const
    {
        const auto kind = sftpArchiveKind(name);
        return has(kind == "zip" ? "unzip" : "tar") ? kind : QString();
    }
    void extractHere(const QString &name)
    {
        const bool zip = archiveKind(name) == "zip";
        const auto script = sftpExtractScript(listed, name, zip);
        if (script.isEmpty())
            return;
        // zip: entries extracted out of the count on the first line; tar: no count, a busy bar.
        jobBar->setRange(0, zip ? 100 : 0);
        jobBar->setValue(0);
        jobBar->show();
        auto lines = std::make_shared<qint64>(-1), total = std::make_shared<qint64>(0);
        auto pending = std::make_shared<QByteArray>();
        shellJob(L("Extracting…"), script,
                 !zip ? std::function<void(const QByteArray &)>()
                      : [this, lines, total, pending](const QByteArray &chunk)
                        {
                            *pending += chunk;
                            for (int end; (end = pending->indexOf('\n')) >= 0; pending->remove(0, end + 1))
                            {
                                if (*lines < 0)
                                    *total = pending->left(end).trimmed().toLongLong();
                                ++*lines;
                            }
                            if (*total > 0)
                                jobBar->setValue(int(std::min<qint64>(99, *lines * 100 / *total)));
                        });
    }
    // An archive of the selection in the listed folder: zip when the server has it, otherwise tar.gz.
    void compress(const QStringList &names)
    {
        const bool zip = has("zip");
        const QString extension = zip ? ".zip" : ".tar.gz";
        const auto taken = listedNames();
        const auto base = names.size() == 1 ? names.value(0) : L("Archive");
        auto out = base + extension;
        for (int n = 2; taken.contains(out); ++n)
            out = base + " " + QString::number(n) + extension;
        if (const auto script = sftpCompressScript(listed, out, names, zip); !script.isEmpty())
            shellJob(L("Compressing…"), script);
    }
    // Total size of folders (du), shown in their Size column.
    void folderSizes(const QStringList &names)
    {
        const auto directory = listed;
        const auto script = sftpSizeScript(directory, names);
        if (script.isEmpty())
            return;
        status->setText(L("Calculating size…"));
        runShell(script, true,
                 [this, directory](int, const QString &out, const QString &error)
                 {
                     QStringList shown;
                     for (const auto &line : out.split('\n', Qt::SkipEmptyParts))
                     {
                         const auto name = line.section('\t', 1).mid(2); // after "./"
                         bool ok = false;
                         const auto bytes = line.section('\t', 0, 0).toLongLong(&ok) * 1024;
                         if (!ok || directory != listed)
                             continue;
                         for (int i = 0; i < files->topLevelItemCount(); ++i)
                         {
                             if (auto *item = files->topLevelItem(i); item->text(0) == name)
                             {
                                 item->setText(1, fileSizeText(bytes));
                                 item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
                             }
                         }
                         shown << name + ": " + fileSizeText(bytes);
                     }
                     status->setText(!error.isEmpty() ? error.left(400) : shown.join('\n'));
                 });
    }
    void checksum(const QString &name)
    {
        const auto file = posixQuote(join(name));
        if (file.isEmpty())
            return;
        status->setText(L("Calculating checksum…"));
        const auto script = (has("sha256sum") ? "sha256sum -- " : "shasum -a 256 -- ") + file;
        runShell(script, true,
                 [this, name](int code, const QString &out, const QString &error)
                 {
                     const auto hash = out.section(' ', 0, 0).trimmed();
                     if (code != 0 || !QRegularExpression("^[0-9a-fA-F]{64}$").match(hash).hasMatch())
                     {
                         status->setText(error.isEmpty() ? L("SFTP operation failed.") : error.left(400));
                         return;
                     }
                     status->clear();
                     QApplication::clipboard()->setText(hash);
                     QMessageBox box(QMessageBox::Information, L("Checksum (SHA-256)"),
                                     name + "\n\n" + hash + "\n\n" + L("Copied to the clipboard."), QMessageBox::Ok, this);
                     box.setTextInteractionFlags(Qt::TextSelectableByMouse);
                     box.exec();
                 });
    }
    void cutOrCopy(bool cut)
    {
        const auto names = selectedNames();
        if (names.isEmpty() || listed.isEmpty())
            return;
        clipboardPaths.clear();
        clipboardDirs.clear();
        for (const auto &name : names)
        {
            clipboardPaths << join(name);
            if (isDir(name))
                clipboardDirs.insert(join(name));
        }
        clipboardCut = cut;
        status->setText((cut ? L("%1 item(s) cut") : L("%1 item(s) copied")).arg(names.size()));
    }
    bool canPaste() const { return !clipboardPaths.isEmpty() && (clipboardCut || has("cp")); }
    // Into the listed folder: what was cut is moved there (an sftp rename, which never replaces), what was copied
    // is copied there by the server, under a "(copy)" name when the name is taken.
    void paste()
    {
        if (!canPaste() || listed.isEmpty())
            return;
        if (!clipboardCut)
        {
            copyInto(clipboardPaths);
            return;
        }
        QStringList ops;
        for (const auto &source : std::as_const(clipboardPaths))
        {
            const auto name = source.section('/', -1);
            const auto from = source.left(source.size() - name.size());
            if (QDir::cleanPath(from) == QDir::cleanPath(listed) || !plainName(name) || sftpQuote(source).isEmpty())
                continue;
            ops << "rename -l " + sftpQuote(source) + " " + sftpQuote(name);
        }
        clipboardPaths.clear();
        clipboardDirs.clear();
        if (!ops.isEmpty())
            run(listed, ops);
    }
    void copyInto(const QStringList &sources)
    {
        auto taken = listedNames();
        QList<std::pair<QString, QString>> copies;
        for (const auto &source : sources)
        {
            const auto name = source.section('/', -1);
            if (!plainName(name))
                continue;
            const bool dir = clipboardDirs.contains(source) || (source == join(name) && isDir(name));
            const auto target = taken.contains(name) ? sftpCopyName(name, dir, taken) : name;
            taken << target;
            copies.append({source, join(target)});
        }
        if (!copies.isEmpty())
            shellJob(L("Copying…"), sftpCopyScript(copies));
    }
    void duplicate()
    {
        QStringList sources;
        for (const auto &name : selectedNames())
            sources << join(name);
        if (!sources.isEmpty() && has("cp"))
            copyInto(sources);
    }
    // chmod (and chown) of one item; through the shell when owner, group or the folder's content change.
    void applyPermissions(const QString &remotePath, const SftpPermissions &p, const QString &owner, const QString &group)
    {
        const bool chown = p.owner != owner || p.group != group;
        if (!chown && !p.recurse)
        {
            run(listed, {"chmod " + p.mode + " " + sftpQuote(remotePath)});
            return;
        }
        const auto script = sftpPermissionsScript(remotePath, p.mode, chown ? p.owner + ":" + p.group : QString(), p.recurse);
        if (!script.isEmpty())
            shellJob(L("Changing permissions…"), script);
    }
    // Zips the selected items on the server into /tmp/download-<date time>.zip, downloads it to a file the user
    // chooses and removes it from the server.
    void downloadZip()
    {
        const auto names = selectedNames();
        const auto directory = listed; // before the dialog: a listing may finish while it is open
        const auto dir = posixQuote(directory);
        if (names.isEmpty() || dir.isEmpty())
            return;
        const auto name = "download-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".zip";
        const auto local = QFileDialog::getSaveFileName(
            this, L("Download as zip"),
            QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).filePath(name), "Zip (*.zip)");
        if (local.isEmpty())
            return;
        const auto remote = "/tmp/" + name;
        // "./": a name starting with '-' is not read as an option of zip. -y keeps links as links.
        QString script = "cd " + dir + " && zip -r -q -y " + posixQuote(remote);
        for (const auto &item : names)
        {
            if (const auto quoted = posixQuote("./" + item); !quoted.isEmpty())
                script += " " + quoted;
        }
        // 18: some files could not be read; the zip holds the others.
        script += "; code=$?; [ $code -eq 0 ] || [ $code -eq 18 ] && wc -c < " + posixQuote(remote) + "; exit $code";
        status->setText(L("Compressing…"));
        const bool started = runShell(
            script, true,
            [this, name, local](int code, const QString &out, const QString &error)
            {
                if (code != 0 && code != 18)
                {
                    status->setText(error.isEmpty() ? L("SFTP operation failed.") : error.left(400));
                    if (code > 0 && code != 255) // zip may have left a partial file
                        runShell("rm -f -- " + posixQuote("/tmp/" + name), false, nullptr);
                    return;
                }
                status->setText(code == 18 ? error.left(400) : QString());
                const auto size = out.trimmed().section('\n', -1).trimmed().toLongLong();
                enqueue("/tmp", name, local, false, false, size > 0 ? size : -1, false,
                        [](Transfer &t) { t.temporary = true; });
            });
        if (!started)
            status->setText(L("SFTP operation failed."));
    }
    // One "rm -rf" over ssh; servers without a POSIX shell (SFTP only, Windows) get the walk over sftp instead.
    void removeRecursive(const QStringList &directories, const QStringList &filesToRemove)
    {
        if (remover->state() != QProcess::NotRunning)
        {
            status->setText(L("Operation in progress…"));
            return;
        }
        const auto program = QStandardPaths::findExecutable(shell.program);
        QString script = "rm -rf --";
        for (const auto &item : directories + filesToRemove)
        {
            if (const auto quoted = posixQuote(item); !quoted.isEmpty())
                script += " " + quoted;
        }
        if (!shell.error.isEmpty() || shell.program != "ssh" || !shell.arguments.contains("--") || program.isEmpty())
        {
            removeBySftp(directories, filesToRemove);
            return;
        }
        removing[0] = directories;
        removing[1] = filesToRemove;
        status->setText(L("Deleting…"));
        files->setCursor(Qt::BusyCursor);
        remover->setProcessEnvironment(sftpEnvironment(true));
        remover->start(program, QStringList{"-T", "-o", "BatchMode=no", "-o", "StrictHostKeyChecking=yes", "-o",
                                            "ConnectTimeout=10"} +
                                    shell.arguments + QStringList{"sh -c " + posixQuote(script)});
    }
    void removeBySftp(const QStringList &directories, const QStringList &filesToRemove)
    {
        if (scanner->state() != QProcess::NotRunning || !scanQueue.isEmpty() || deletePending)
        {
            status->setText(L("Operation in progress…")); // a deletion is still walking its folders
            return;
        }
        scanQueue = directories;
        foundDirs = directories;
        foundFiles = filesToRemove;
        status->setText(L("Deleting…"));
        scanNext();
    }
    QString message() const { return status->text(); }
    int dragTimeout = 10 * 60 * 1000; // ms allowed for a drag download
    // A drag out of the list is under way: its exec(), or the download it asked for (a nested event loop), is on
    // the stack and uses this panel when it returns. The panel must not be deleted meanwhile: the window calls
    // cancelDrag() and closes the tab from onDragEnded.
    bool dragActive() const { return files->dragging > 0 || !fetchers.isEmpty(); }
    // Ends the download of the drag under way at once (nothing is delivered) and starts no other one for it.
    void cancelDrag()
    {
        if (!dragActive())
            return;
        dragCancelled = true;
        for (auto *fetcher : std::as_const(fetchers))
            fetcher->kill();
    }
    std::function<void()> onDragEnded; // from the event loop, once the drag and its download have returned

    // Data for dragging the given names of the current folder out (nullptr if there is nothing).
    QMimeData *createDragData(const QStringList &names)
    {
        const auto directory = listed.isEmpty() ? path->text() : listed;
        const auto base = sftpTempFolder("CrossTerm-drag");
        if (names.isEmpty() || directory.isEmpty() || base.isEmpty() || !std::all_of(names.begin(), names.end(), safeLocalName))
            return nullptr;
        // Folders of earlier drags: the drop target may copy (or keep using) the files after the drop, so only
        // CrossTerm's own UUID-named folders older than a day are removed.
        const auto stale = QDateTime::currentDateTime().addDays(-1);
        for (const auto &old : QDir(base).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden))
        {
            if (QUuid::fromString(old.fileName()).toString(QUuid::WithoutBraces) == old.fileName() && old.lastModified() < stale)
                QDir(old.filePath()).removeRecursively();
        }
        const auto local = QDir(base).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSet<QString> folders;
        auto *data = new SftpDragData;
        for (const auto &name : names)
        {
            data->planned << QUrl::fromLocalFile(QDir(local).filePath(name));
            if (isDir(name))
                folders << name;
        }
        data->fetch = [self = QPointer<SftpPanel>(this), directory, names, folders, local]
        { return self && self->fetchBlocking(directory, names, folders, local); };
        return data;
    }

    // Downloads the names into a local folder with an sftp batch; local event loop, so the UI stays responsive.
    bool fetchBlocking(const QString &directory, const QStringList &names, const QSet<QString> &folders, const QString &local)
    {
        if (dragCancelled)
            return false; // the tab is closing
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
        fetchers << &proc; // dragActive() until this call returns
        if (proc.state() != QProcess::NotRunning)
            loop.exec();
        const bool timedOut = proc.state() != QProcess::NotRunning;
        if (timedOut)
        {
            proc.kill();
            proc.waitForFinished(3000);
        }
        forgetAskpassAttempts(&proc);
        const auto error = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        const bool ok = !timedOut && proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0 &&
                        proc.error() != QProcess::FailedToStart;
        if (!self)
            return false; // panel closed during the download
        fetchers.removeOne(&proc);
        dragFinished();
        status->setText(ok         ? QString()
                        : timedOut ? L("SFTP operation timed out.")
                                   : (error.isEmpty() ? L("SFTP operation failed.") : error.left(400)));
        return ok;
    }

    SftpPanel(QJsonObject profile, const QString &platform, QWidget *parent = nullptr) : QWidget(parent)
    {
        profile["protocol"] = "sftp";
        profile["x11"] = false;
        auto sshProfile = profile;
        sshProfile["protocol"] = "ssh";
        shell = sessionCommand(sshProfile, platform, false);
        command = sessionCommand(profile, platform, false);
        login = profile["user"].toString();
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
        backAction = action("back", N("Back"), [this] { goHistory(-1); }, QKeySequence(QKeySequence::Back));
        forwardAction = action("forward", N("Forward"), [this] { goHistory(1); }, QKeySequence(QKeySequence::Forward));
        backAction->setEnabled(false);
        forwardAction->setEnabled(false);
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
                   // The folder and its names as they are now: a listing may finish while the dialog is open.
                   const auto directory = listed;
                   const auto present = listedNames();
                   const auto names = QFileDialog::getOpenFileNames(this, L("Upload files"));
                   upload(names, directory, &present);
               });
        action("refresh", N("Refresh current folder"), [this] { refresh(); }, QKeySequence(Qt::Key_F5));
        action("newfolder", N("Create new directory"),
               [this]
               {
                   const auto directory = listed; // the folder may be listed again while the dialog is open
                   const auto name = QInputDialog::getText(this, L("New directory"), L("Directory name:")).trimmed();
                   if (!name.isEmpty() && !sftpQuote(name).isEmpty())
                       run(directory, {"mkdir " + sftpQuote(name)});
               });
        action("newfile", N("Create new file"),
               [this]
               {
                   const auto directory = listed; // the folder may be listed again while the dialog is open
                   const auto name = QInputDialog::getText(this, L("New file"), L("File name:")).trimmed();
                   if (name.isEmpty() || refused(directory, name))
                       return;
                   // A unique private file (a fixed name in a shared /tmp could be planted by another user);
                   // removed with the panel.
                   if (!emptyFile)
                   {
                       emptyFile = new QTemporaryFile(QDir::temp().filePath("crossterm-empty-XXXXXX"), this);
                       if (!emptyFile->open())
                       {
                           status->setText(emptyFile->errorString());
                           delete std::exchange(emptyFile, nullptr);
                           return;
                       }
                       emptyFile->close();
                   }
                   // Under a temp name first, then a rename that fails on an existing name ("-l"): a file that the
                   // listing does not show is not emptied. The temp name is uploaded once more so that removing it
                   // cannot fail, whether the rename took it or not.
                   const auto empty = sftpQuote(QDir::fromNativeSeparators(emptyFile->fileName())),
                              temp = sftpQuote("." + name + ".crossterm-new");
                   run(directory, {"put " + empty + " " + temp, "rename -l " + temp + " " + sftpQuote(name),
                                   "put " + empty + " " + temp, "rm " + temp});
               });
        action("delete", N("Delete selected files"), [this] { remove(); }, QKeySequence(Qt::Key_Delete));
        auto *renameAction = action(
            "rename", N("Rename"),
            [this]
            {
                const auto names = selectedNames();
                if (names.size() != 1 || busy())
                    return;
                for (auto *item : files->selectedItems())
                {
                    if (item->text(0) != names[0])
                        continue;
                    item->setFlags(item->flags() | Qt::ItemIsEditable);
                    files->scrollToItem(item);
                    files->editItem(item, 0);
                }
            },
            QKeySequence(Qt::Key_F2));

        // Cut, copy and paste of remote files (Cmd/Ctrl+X, C, V in the list).
        for (const auto &[key, slot] : std::initializer_list<std::pair<QKeySequence::StandardKey, std::function<void()>>>{
                 {QKeySequence::Cut, [this] { cutOrCopy(true); }},
                 {QKeySequence::Copy, [this] { if (has("cp")) cutOrCopy(false); }},
                 {QKeySequence::Paste, [this] { paste(); }}})
        {
            auto *a = new QAction(files);
            a->setShortcut(key);
            a->setShortcutContext(Qt::WidgetShortcut);
            connect(a, &QAction::triggered, this, slot);
            files->addAction(a);
        }
        texts.bind(path, &QWidget::setAccessibleName, N("Remote SFTP path"));
        path->setObjectName("sftpPath");
        auto *go = path->addAction(icon("ok"), QLineEdit::TrailingPosition);
        texts.tip(go, N("Go to this folder"));
        // Favourite folders: a menu under the star at the end of the path field.
        {
            auto *favourites = path->addAction(icon("star", 16), QLineEdit::TrailingPosition);
            texts.tip(favourites, N("Favourite folders"));
            auto *menu = new QMenu(this);
            connect(menu, &QMenu::aboutToShow, this, [this, menu] { fillBookmarks(menu); });
            connect(favourites, &QAction::triggered, this,
                    [this, menu] { menu->popup(path->mapToGlobal(QPoint(path->width() - 40, path->height()))); });
        }
        connect(go, &QAction::triggered, this, [this] { run(path->text()); });
        connect(path, &QLineEdit::returnPressed, this, [this] { run(path->text()); });
        dot->setPixmap(sftpStateDot(link));
        dot->setFixedSize(10, 10);
        dot->setCursor(Qt::ArrowCursor);
        dot->installEventFilter(this); // tooltip: the status text of the moment
        path->setTextMargins(14, 0, 0, 0);
        path->installEventFilter(this); // keeps the dot centred vertically
        layout->addWidget(path);
        texts.bind(filter, &QLineEdit::setPlaceholderText, N("Filter"));
        texts.bind(filter, &QWidget::setAccessibleName, N("Filter"));
        filter->setClearButtonEnabled(true);
        filter->setObjectName("sftpFilter");
        // The eye is bright while hidden files are shown, faded while they are not (the field's buttons do not
        // draw a checked state).
        const QIcon eyeOn = icon("eye", 16), eyeOff(toggleIcon("eye", 16).pixmap(QSize(16, 16), QIcon::Normal, QIcon::Off));
        hiddenToggle = filter->addAction(eyeOn, QLineEdit::TrailingPosition);
        hiddenToggle->setCheckable(true);
        texts.tip(hiddenToggle, N("Show hidden files"));
        connect(hiddenToggle, &QAction::toggled, this,
                [this, eyeOn, eyeOff](bool on)
                {
                    hiddenToggle->setIcon(on ? eyeOn : eyeOff);
                    QSettings().setValue("sftp/showHidden", on);
                    applyFilter();
                });
        {
            const bool shown = QSettings().value("sftp/showHidden", true).toBool();
            hiddenToggle->setChecked(shown);
            hiddenToggle->setIcon(shown ? eyeOn : eyeOff);
        }
        connect(filter, &QLineEdit::textChanged, this, [this] { applyFilter(); });
        // Escape empties the filter; Down moves into the list.
        filter->installEventFilter(this);
        layout->addWidget(filter);
        {
            auto *find = new QAction(this);
            find->setShortcut(QKeySequence::Find);
            find->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            connect(find, &QAction::triggered, this,
                    [this]
                    {
                        filter->setFocus();
                        filter->selectAll();
                    });
            addAction(find);
        }

        retranslate();
        files->setRootIsDecorated(false);
        // Only F2 and "Rename" edit a name: a double click opens.
        files->setEditTriggers(QAbstractItemView::NoEditTriggers);
        auto *renamer = new SftpRenameDelegate(files);
        renamer->renamed = [this](const QString &old, const QString &next)
        {
            const auto directory = listed;
            if (sftpQuote(old).isEmpty() || refused(directory, next))
                return;
            // "-l": the plain rename request, which fails on an existing name (sftp's default one replaces it on
            // OpenSSH servers). Except for a change of case alone, the same file on a case-insensitive server.
            const bool sameFile = next.compare(old, Qt::CaseInsensitive) == 0;
            // After the editor has closed: the listing that follows replaces the rows.
            QTimer::singleShot(0, this,
                               [this, directory, sameFile, old, next]
                               {
                                   run(directory, {QString(sameFile ? "rename " : "rename -l ") + sftpQuote(old) + " " +
                                                   sftpQuote(next)});
                               });
        };
        files->setItemDelegateForColumn(0, renamer);
        files->setSelectionMode(QAbstractItemView::ExtendedSelection);
        files->setUniformRowHeights(true);
        files->setIconSize(QSize(16, 16));
        files->setColumnWidth(0, 150);
        files->setAcceptDrops(true);
        files->viewport()->setAcceptDrops(true);
        files->viewport()->installEventFilter(this);
        files->setDragEnabled(true); // drag out (download), see SftpTree
        files->dragData = [this] { return createDragData(selectedNames()); };
        files->dragEnded = [this] { dragFinished(); };
        // Right click on a file: open, edit and download / delete and rename / path, properties, permissions.
        files->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(files, &QWidget::customContextMenuRequested, this,
                [this, renameAction](const QPoint &at)
                {
                    auto *item = files->itemAt(at);
                    if (!item || item->text(0) == "..")
                    {
                        // Empty space: what applies to the folder itself.
                        if (listed.isEmpty())
                            return;
                        files->clearSelection();
                        QMenu menu(this);
                        if (canPaste())
                            menu.addAction(QIcon(), L("Paste"), [this] { paste(); });
                        menu.addAction(icon("refresh", 16), L("Refresh current folder"), [this] { refresh(); });
                        menu.exec(files->viewport()->mapToGlobal(at));
                        return;
                    }
                    const auto name = item->text(0);
                    const bool dir = item->data(0, Qt::UserRole).toBool();
                    const auto size = item->data(0, Qt::UserRole + 1).toLongLong();
                    const auto remotePath = join(name);
                    const auto directory = listed;
                    // Copies: a listing finishing while the menu or a dialog is open deletes the item.
                    QStringList columns;
                    for (int i = 0; i < 6; ++i)
                        columns << item->text(i);
                    QMenu menu(this);
                    if (!dir)
                    {
                        menu.addAction(icon("open", 16), L("Open"), [=] { openRemote(name, size, 0, directory); });
                        menu.addAction(icon("edit", 16), L("Open with default text editor"),
                                       [=] { openRemote(name, size, 2, directory); });
                        menu.addAction(icon("openwith", 16), L("Open with..."), [=] { openRemote(name, size, 1, directory); });
                    }
                    else
                        menu.addAction(icon("folder", 16), L("Open"), [=] { run(remotePath); });
                    menu.addAction(icon("download", 16), L("Download"), [this] { download(); });
                    if (has("zip"))
                        menu.addAction(icon("download", 16), L("Download as zip"), [this] { downloadZip(); });
                    if (!dir && !archiveKind(name).isEmpty())
                        menu.addAction(icon("packages", 16), L("Extract here"), [this, name] { extractHere(name); });
                    if (has("zip") || has("tar"))
                        menu.addAction(icon("packages", 16), L("Compress"), [this] { compress(selectedNames()); });
                    menu.addSeparator();
                    menu.addAction(QIcon(), L("Cut"), [this] { cutOrCopy(true); });
                    if (has("cp"))
                        menu.addAction(icon("copy", 16), L("Copy"), [this] { cutOrCopy(false); });
                    if (canPaste())
                        menu.addAction(QIcon(), L("Paste"), [this] { paste(); });
                    if (has("cp"))
                        menu.addAction(icon("copy", 16), L("Duplicate"), [this] { duplicate(); });
                    menu.addSeparator();
                    menu.addAction(icon("delete", 16), L("Delete"), [this] { remove(); });
                    menu.addAction(icon("rename", 16), L("Rename"), renameAction, &QAction::trigger);
                    menu.addSeparator();
                    menu.addAction(icon("copy", 16), L("Copy file path"),
                                   [remotePath] { QApplication::clipboard()->setText(remotePath); });
                    if (pasteToTerminal)
                        menu.addAction(icon("terminal", 16), L("Copy file path to terminal"),
                                       [this, remotePath]
                                       {
                                           if (pasteToTerminal)
                                               pasteToTerminal(shellQuote(remotePath));
                                       });
                    if (dir && has("du"))
                        menu.addAction(icon("info", 16), L("Calculate size"),
                                       [this]
                                       {
                                           QStringList dirs;
                                           for (const auto &selected : selectedNames())
                                               if (isDir(selected))
                                                   dirs << selected;
                                           folderSizes(dirs);
                                       });
                    if (!dir && (has("sha256sum") || has("shasum")))
                        menu.addAction(icon("lock", 16), L("Checksum (SHA-256)"), [this, name] { checksum(name); });
                    menu.addAction(icon("info", 16), L("Properties"),
                                   [this, remotePath, columns]
                                   {
                                       QMessageBox::information(this, L("Properties"),
                                                                QString("%1\n\n%2: %3\n%4: %5\n%6: %7 / %8\n%9: %10")
                                                                    .arg(remotePath, L("Size"), columns[1], L("Last modified"),
                                                                         columns[2], L("Owner"), columns[3], columns[4],
                                                                         L("Access"))
                                                                    .arg(columns[5]));
                                   });
                    menu.addAction(icon("lock", 16), L("Permissions"),
                                   [this, remotePath, dir, access = columns[5], owner = columns[3], group = columns[4]]
                                   {
                                       // The current mode as the starting value: a confirmed default must not drop
                                       // setuid, setgid or sticky.
                                       const bool shellTools = has("chown") && has("find");
                                       const auto chosen = sftpPermissionsDialog(this, remotePath, sftpOctalMode(access), owner,
                                                                                 group, shellTools, dir);
                                       if (chosen)
                                           applyPermissions(remotePath, *chosen, owner, group);
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
                                           syncEdit(file);
                                       });
                });
        remover->setStandardInputFile(QProcess::nullDevice());
        connect(remover, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError error)
                {
                    if (error != QProcess::FailedToStart)
                        return;
                    files->unsetCursor();
                    QTimer::singleShot(0, this, [this] { removeBySftp(removing[0], removing[1]); }); // no "finished"
                });
        connect(remover, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus exit)
                {
                    files->unsetCursor();
                    forgetAskpassAttempts(remover);
                    const auto error = QString::fromUtf8(remover->readAllStandardError()).trimmed();
                    if (code == 0 && exit == QProcess::NormalExit)
                    {
                        // Gone from the list at once; the listing that follows confirms it.
                        for (const auto &path : removing[0] + removing[1])
                        {
                            for (int i = files->topLevelItemCount() - 1; i >= 0; --i)
                            {
                                if (join(files->topLevelItem(i)->text(0)) == path)
                                    delete files->topLevelItem(i);
                            }
                        }
                        status->clear();
                        relistNow();
                        return;
                    }
                    // ssh could not log in: the sftp walk would fail the same way.
                    if (code == 255 || exit != QProcess::NormalExit)
                    {
                        note = error.isEmpty() ? L("SFTP operation failed.") : error.left(400);
                        relistNow();
                        return;
                    }
                    // rm ran and refused some items (permissions): what it could delete is gone.
                    const auto lines = error.split('\n', Qt::SkipEmptyParts);
                    if (!lines.isEmpty() && std::all_of(lines.begin(), lines.end(),
                                                         [](const QString &line) { return line.startsWith("rm: "); }))
                    {
                        note = error.left(400);
                        relistNow();
                        return;
                    }
                    // No POSIX shell or no rm (SFTP-only account, Windows server): file by file over sftp.
                    removeBySftp(removing[0], removing[1]);
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
                    forgetAskpassAttempts(scanner);
                    if (code || exit != QProcess::NormalExit)
                    {
                        scanQueue.clear();
                        foundFiles.clear();
                        foundDirs.clear();
                        if (reached(output))
                            relistNow(); // what a partial walk could see is unchanged, but the list may be stale
                        status->setText(error.isEmpty() ? L("SFTP operation failed.") : error.left(400));
                        if (!reached(output))
                            setLink(2);
                        return;
                    }
                    // Each "sftp> cd" echo starts the listing of the next folder of the batch. Names never hold '/',
                    // so whatever a file name with a line break fakes, the paths stay inside these folders.
                    const auto dirs = scanner->property("dirs").toStringList();
                    int index = -1;
                    for (const auto &line : output.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts))
                    {
                        if (line.startsWith("sftp> cd "))
                        {
                            ++index;
                            continue;
                        }
                        RemoteEntry entry;
                        if (index < 0 || index >= dirs.size() || !parseListing(line, entry))
                            continue;
                        const auto full = dirs[index] + "/" + entry.name;
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
        jobBar->setMaximumHeight(14);
        jobBar->hide();
        bottom->addWidget(jobBar);
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
                [this](QProcess::ProcessError error)
                {
                    files->unsetCursor();
                    status->setText(process->errorString());
                    if (error == QProcess::FailedToStart)
                        setLink(2);
                    if (error == QProcess::FailedToStart && deletePending)
                        QTimer::singleShot(0, this, [this] { scanNext(); }); // no "finished" will follow
                });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus exit)
                {
                    files->unsetCursor();
                    forgetAskpassAttempts(process);
                    // Then the work that waited for this listing: the delete batch, or a refresh after uploads.
                    QTimer::singleShot(0, this,
                                       [this]
                                       {
                                           if (deletePending)
                                               scanNext();
                                           else if (relist && process->state() == QProcess::NotRunning)
                                           {
                                               relist = false;
                                               refresh();
                                           }
                                       });
                    const auto output = QString::fromUtf8(process->readAllStandardOutput());
                    const auto error = QString::fromUtf8(process->readAllStandardError()).trimmed();
                    if (code || exit != QProcess::NormalExit)
                    {
                        if (error.contains("Permission denied"))
                            status->setText(
                                L("Authentication failed. Click Refresh to enter the password, or check the user name and key.") +
                                "\n" + error.left(300));
                        else if (error.contains("Host key verification failed"))
                            status->setText(L("Unknown host key: accept it in the SSH terminal first, then click Refresh."));
                        else
                            status->setText(error.isEmpty() ? L("SFTP operation failed.") : error.left(400));
                        if (!reached(output))
                            setLink(2);
                        return;
                    }
                    QList<RemoteEntry> entries;
                    // Link name -> target, when the listing tells it.
                    QHash<QString, QString> targets;
                    QHash<QString, int> links; // name -> link count of the file (0 when the listing shows '?')
                    QStringList folders;       // the two "pwd" answers: start folder (remote home), then the listed one
                    for (const auto &line : output.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts))
                    {
                        // Only those two: a file name with a line break can fake this line inside the listing.
                        if (folders.size() < 2 && line.startsWith("Remote working directory: "))
                        {
                            folders << line.mid(26).trimmed();
                            continue;
                        }
                        RemoteEntry entry;
                        if (!parseListing(line, entry))
                            continue;
                        entries << entry;
                        if (const auto target = listedLinkTarget(line, entry); !target.isEmpty())
                            targets.insert(entry.name, target);
                        links.insert(entry.name, line.simplified().section(' ', 1, 1).toInt());
                    }
                    const auto before = listed;
                    if (folders.size() == 2)
                        listed = folders[1];
                    recordHistory(before);
                    std::sort(entries.begin(), entries.end(), [](const RemoteEntry &a, const RemoteEntry &b)
                              { return a.dir != b.dir ? a.dir : a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
                    const bool first = home.isEmpty();
                    if (first)
                        home = folders.value(0, listed); // used to resolve "~" of the terminal folder
                    path->setText(listed.endsWith('/') ? listed : listed + "/");
                    files->clear();
                    auto *up = new RemoteFileItem(files, {".."});
                    up->setIcon(0, icon("parent"));
                    up->setData(0, Qt::UserRole, true);
                    up->setData(1, Qt::UserRole, qint64(-2)); // first when sorted by size
                    const auto folderIcon = fileIcon({}, true);
                    for (const auto &e : entries)
                    {
                        auto *item = new RemoteFileItem(
                            files, {e.name, e.dir ? QString() : fileSizeText(e.size), e.modified, e.owner, e.group, e.access});
                        item->setData(0, Qt::UserRole, e.dir);
                        item->setData(0, Qt::UserRole + 1, e.size);
                        item->setData(0, Qt::UserRole + 2, links.value(e.name));
                        item->setData(1, Qt::UserRole, e.dir ? qint64(-1) : e.size);
                        item->setIcon(0,
                                      !e.link ? (e.dir ? folderIcon : fileIcon(e.name)) : linkIcon(e.dir ? folderIcon : fileIcon(e.name), e.dir));
                        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
                        if (const auto target = targets.value(e.name); !target.isEmpty())
                            item->setToolTip(0, e.name + " → " + target);
                    }
                    applyFilter();
                    setLink(1);
                    const auto remark = std::exchange(note, {});
                    status->setText(!error.isEmpty() ? error.left(400) : remark);
                    if (first)
                        checkTools();
                    if (first && onConnected)
                        onConnected();
                });
        // First automatic listing when the SSH session opens.
        QTimer::singleShot(1500, this, [this] { run(startFolder(), {}, false); });
    }

    ~SftpPanel() override
    {
        // QWidget deletes its children after the members of this class are gone: a running sftp process would then
        // be killed by ~QProcess and its finished handler would use this half-destroyed panel (closing a tab during
        // a listing or a transfer).
        for (auto *child : findChildren<QProcess *>(Qt::FindDirectChildrenOnly))
        {
            child->disconnect(this);
            if (child->state() != QProcess::NotRunning)
            {
                child->kill();
                child->waitForFinished(1000);
            }
            forgetAskpassAttempts(child);
        }
        // Deleted during a drag download after all (the window defers that, see dragActive): its nested loop,
        // still on the stack, returns at once and finds the panel gone.
        for (auto *fetcher : std::as_const(fetchers))
            fetcher->kill();
        // Edit copies that the server has go with the panel; one with changes that were not uploaded stays.
        for (auto it = edited.cbegin(); it != edited.cend(); ++it)
        {
            if (uploaded(it.key()))
                QFile::remove(it.key());
        }
        for (const auto &folder : std::as_const(editFolders))
            QDir().rmdir(folder);
        if (!editFolders.isEmpty())
            QDir(QDir::temp().filePath("CrossTerm-sftp")).rmdir(editId);
    }

    void download()
    {
        const auto names = selectedNames();
        if (names.isEmpty() || listed.isEmpty())
            return;
        const auto picked = pick(names); // before the dialog: a listing may finish while it is open
        const auto folder = QFileDialog::getExistingDirectory(this, L("Download to folder"),
                                                              QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
        if (!folder.isEmpty())
            download(picked, folder, true);
    }

    void download(const QStringList &names, const QString &folder) { download(pick(names), folder); }

    // Remote folder, name, folder flag and size of names of the current listing.
    struct Picked
    {
        QString directory, name;
        bool dir = false;
        qint64 size = -1;
    };
    QList<Picked> pick(const QStringList &names) const
    {
        QList<Picked> picked;
        for (const auto &name : names)
        {
            Picked p{listed, name, isDir(name), -1};
            for (int i = 0; i < files->topLevelItemCount(); ++i)
            {
                if (files->topLevelItem(i)->text(0) == name)
                    p.size = files->topLevelItem(i)->data(0, Qt::UserRole + 1).toLongLong();
            }
            picked << p;
        }
        return picked;
    }
    // ask: the user chose the folder in a dialog and is asked before files in it are replaced.
    void download(const QList<Picked> &picked, const QString &folder, bool ask = false)
    {
        QStringList existing;
        for (const auto &p : picked)
        {
            if (ask && safeLocalName(p.name) && QFileInfo::exists(QDir(folder).filePath(p.name)))
                existing << p.name;
        }
        const bool replace = existing.isEmpty() || confirmReplace(existing, N("Download"));
        for (const auto &p : picked)
        {
            if (!safeLocalName(p.name))
                status->setText(L("Invalid path."));
            else if (replace || !existing.contains(p.name))
                enqueue(p.directory, p.name, QDir(folder).filePath(p.name), false, p.dir, p.size);
        }
    }

    // One question for all the names that would be replaced; "No" leaves those out, the others are transferred.
    bool confirmReplace(const QStringList &names, const char *title)
    {
        const auto text = names.size() == 1 ? L("%1 already exists. Replace it?").arg(names[0])
                                            : L("%1 files already exist. Replace them?").arg(names.size());
        return askReplace ? askReplace(text) : QMessageBox::question(this, L(title), text) == QMessageBox::Yes;
    }

    void upload(const QStringList &locals) { upload(locals, listed, nullptr); }
    // directory, present: the remote folder and its names when the user chose the files; with present the user is
    // asked before files of the folder are replaced.
    void upload(const QStringList &locals, const QString &directory, const QStringList *present)
    {
        if (locals.isEmpty() || directory.isEmpty())
            return;
        QList<QFileInfo> infos;
        QStringList existing;
        for (const auto &local : locals)
        {
            // A folder dropped from a file manager ends with '/': its name would be empty and its content would
            // land in the remote folder itself.
            const QFileInfo info(QDir::cleanPath(local));
            if (info.fileName().isEmpty())
                continue;
            infos << info;
            if (present && present->contains(info.fileName()))
                existing << info.fileName();
        }
        const bool replace = existing.isEmpty() || confirmReplace(existing, N("Upload files"));
        for (const auto &info : std::as_const(infos))
        {
            if (replace || !existing.contains(info.fileName()))
                enqueue(directory, info.fileName(), info.filePath(), true, info.isDir(), info.isDir() ? -1 : info.size());
        }
    }

    void remove()
    {
        const auto names = selectedNames();
        if (names.isEmpty() || listed.isEmpty())
            return;
        // Full paths before the question: a listing of another folder finishing meanwhile must not change them.
        QStringList dirs, plain;
        for (const auto &name : names)
            (isDir(name) ? dirs : plain) << join(name);
        if (QMessageBox::question(this, L("Delete"),
                                  L("Delete %1 selected item(s)? Folders are deleted with all their contents.")
                                      .append("\n%2")
                                      .arg(names.size())
                                      .arg(names.mid(0, 10).join('\n'))) != QMessageBox::Yes)
            return;
        removeRecursive(dirs, plain);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == filter && event->type() == QEvent::KeyPress)
        {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Escape && !filter->text().isEmpty())
            {
                filter->clear();
                return true;
            }
            if (key == Qt::Key_Down)
            {
                files->setFocus();
                return true;
            }
        }
        if (watched == path && event->type() == QEvent::Resize)
            dot->move(7, (path->height() - dot->height()) / 2);
        if (watched == dot && event->type() == QEvent::ToolTip)
        {
            // Empty once listed without a remark: the folder on display then.
            const auto text = !status->text().isEmpty() ? status->text() : link == 1 ? listed : QString();
            QToolTip::showText(static_cast<QHelpEvent *>(event)->globalPos(), text, dot);
            return true;
        }
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
            // After the drop has returned to the program the files come from: the question must not hold it.
            QTimer::singleShot(0, this, [this, locals, directory = listed, present = listedNames()]
                               { upload(locals, directory, &present); });
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }
};
