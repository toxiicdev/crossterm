#include "syncui.h"
#include "credentials.h"
#include "syncoauth.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageAuthenticationCode>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QVBoxLayout>

namespace
{
QString ui(const char *text)
{
    return QCoreApplication::translate("CrossTerm", text);
}

// ---------------------------------------------------------------- secrets in the system vault
QString secretTarget(const QString &name)
{
    return QCoreApplication::applicationName() + "-sync/" + name;
}
QString secret(const QString &name)
{
    return readSecret(secretTarget(name)).value_or(QString());
}
void setSecret(const QString &name, const QString &value)
{
    if (value.isEmpty())
        removeSecret(secretTarget(name));
    else
        storeSecret(secretTarget(name), "CrossTerm", value);
}

QString encodeKeys(const SyncVault::Keys &k)
{
    const auto b64 = [](const QByteArray &b) { return QString::fromLatin1(b.toBase64()); };
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"dataKey", b64(k.dataKey)},
                                                      {"memoryKiB", k.kdf.memoryKiB},
                                                      {"passes", k.kdf.passes},
                                                      {"salt", b64(k.kdf.salt)},
                                                      {"passwordNonce", b64(k.passwordNonce)},
                                                      {"passwordWrapped", b64(k.passwordWrapped)},
                                                      {"recoveryNonce", b64(k.recoveryNonce)},
                                                      {"recoveryWrapped", b64(k.recoveryWrapped)}})
                                 .toJson(QJsonDocument::Compact));
}

std::optional<SyncVault::Keys> decodeKeys(const QString &text)
{
    const auto o = QJsonDocument::fromJson(text.toUtf8()).object();
    const auto b = [&](const char *name) { return QByteArray::fromBase64(o[name].toString().toLatin1()); };
    SyncVault::Keys k;
    k.dataKey = b("dataKey");
    k.kdf.memoryKiB = o["memoryKiB"].toInt();
    k.kdf.passes = o["passes"].toInt();
    k.kdf.salt = b("salt");
    k.passwordNonce = b("passwordNonce");
    k.passwordWrapped = b("passwordWrapped");
    k.recoveryNonce = b("recoveryNonce");
    k.recoveryWrapped = b("recoveryWrapped");
    if (k.dataKey.size() != 32)
        return std::nullopt;
    return k;
}

QStringList kinds()
{
    return QSettings().value("sync/kinds", QStringList{"sessions", "tunnels", "macros", "passwords"}).toStringList();
}

struct Service
{
    QString type, name;
};
QList<Service> services()
{
    QList<Service> list{{"folder", ui("Synced folder (iCloud Drive, Proton Drive, Syncthing...)")}};
    for (const auto &[type, name] : QList<std::pair<QString, QString>>{{"dropbox", "Dropbox"}, {"onedrive", "OneDrive"}, {"gdrive", "Google Drive"}})
        if (SyncOAuth::available(type))
            list << Service{type, name};
    list << Service{"webdav", ui("WebDAV (Nextcloud, ownCloud, Synology...)")} << Service{"sftp", ui("SFTP (SSH server)")}
         << Service{"ftps", ui("FTPS (FTP over TLS)")} << Service{"gist", "GitHub Gist"};
    return list;
}

// Runs one storage call to its end with a local event loop (the dialog stays responsive).
SyncStorage::Result wait(const std::function<void(SyncStorage::Done)> &call)
{
    std::optional<SyncStorage::Result> result;
    QEventLoop loop;
    call(
        [&](const SyncStorage::Result &r)
        {
            result = r;
            loop.quit();
        });
    if (!result)
        loop.exec();
    return *result;
}

// New sync password, typed twice; empty when cancelled.
QString askNewPassword(QWidget *parent, const QString &title, const QString &intro)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QLabel(intro);
    text->setWordWrap(true);
    layout->addWidget(text);
    auto *form = new QFormLayout;
    auto *first = new QLineEdit, *second = new QLineEdit;
    first->setEchoMode(QLineEdit::Password);
    second->setEchoMode(QLineEdit::Password);
    form->addRow(ui("Password"), first);
    form->addRow(ui("Repeat password"), second);
    layout->addLayout(form);
    auto *hint = new QLabel;
    layout->addWidget(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    const auto check = [&]
    {
        const bool longEnough = first->text().size() >= 10, same = first->text() == second->text();
        hint->setText(!longEnough ? ui("At least 10 characters.") : !same ? ui("The two passwords differ.") : QString());
        buttons->button(QDialogButtonBox::Ok)->setEnabled(longEnough && same);
    };
    QObject::connect(first, &QLineEdit::textChanged, &dialog, check);
    QObject::connect(second, &QLineEdit::textChanged, &dialog, check);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    check();
    return dialog.exec() == QDialog::Accepted ? first->text() : QString();
}

// Shows the recovery key once; true when the user confirmed keeping it.
bool showRecoveryKey(QWidget *parent, const QString &key)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(ui("Recovery key"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QLabel(ui("The recovery key is the only way to open the synchronized data if you forget the sync "
                               "password. Keep it somewhere safe: it is not shown again."));
    text->setWordWrap(true);
    layout->addWidget(text);
    auto *shown = new QLabel(key.left(34) + "\n" + key.mid(35));
    shown->setTextInteractionFlags(Qt::TextSelectableByMouse);
    shown->setAlignment(Qt::AlignCenter);
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSizeF(shown->font().pointSizeF() * 1.15);
    shown->setFont(mono);
    shown->setStyleSheet("padding: 8px; border: 1px solid palette(mid);");
    layout->addWidget(shown);
    auto *row = new QHBoxLayout;
    auto *copy = new QPushButton(ui("Copy"));
    auto *save = new QPushButton(ui("Save to file..."));
    row->addWidget(copy);
    row->addWidget(save);
    row->addStretch(1);
    layout->addLayout(row);
    auto *kept = new QCheckBox(ui("I saved the recovery key"));
    layout->addWidget(kept);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);
    QObject::connect(copy, &QPushButton::clicked, &dialog, [key] { QApplication::clipboard()->setText(key); });
    QObject::connect(save, &QPushButton::clicked, &dialog,
                     [&dialog, key]
                     {
                         const auto file = QFileDialog::getSaveFileName(&dialog, ui("Recovery key"), "CrossTerm-recovery-key.txt",
                                                                        ui("Text files (*.txt)"));
                         QSaveFile out(file);
                         if (!file.isEmpty() && out.open(QIODevice::WriteOnly))
                         {
                             out.write(("CrossTerm sync recovery key\n" + key + "\n").toUtf8());
                             out.commit();
                         }
                     });
    QObject::connect(kept, &QCheckBox::toggled, buttons->button(QDialogButtonBox::Ok), &QWidget::setEnabled);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted;
}
} // namespace

SyncController::SyncController(Host h, const QString &base, QObject *parent)
    : QObject(parent), host(std::move(h)), baseFile(base)
{
    debounce.setSingleShot(true);
    debounce.setInterval(5000);
    connect(&debounce, &QTimer::timeout, this, [this] { syncNow(); });
    periodic.setInterval(10 * 60 * 1000);
    connect(&periodic, &QTimer::timeout, this, [this] { syncNow(); });
    periodic.start();
    if (enabled())
        QTimer::singleShot(3000, this, [this] { syncNow(); });
}

bool SyncController::enabled() const
{
    return QSettings().value("sync/enabled").toBool();
}

void SyncController::schedule()
{
    if (enabled())
        debounce.start();
}

QJsonObject SyncController::storageSettings() const
{
    QSettings s;
    const auto type = s.value("sync/type", "folder").toString();
    QJsonObject o{{"type", type}};
    if (type == "folder")
        o["path"] = s.value("sync/path").toString();
    else if (type == "webdav")
    {
        o["url"] = s.value("sync/url").toString();
        o["user"] = s.value("sync/user").toString();
        o["password"] = secret("password");
    }
    else if (type == "sftp")
    {
        for (const auto &v : host.sessions())
            if (v.toObject()["id"].toString() == s.value("sync/session").toString())
                o["session"] = v.toObject();
        o["path"] = s.value("sync/remotePath", SyncStorage::FileName).toString();
    }
    else if (type == "ftps")
    {
        o["host"] = s.value("sync/host").toString();
        o["port"] = s.value("sync/port", 21).toInt();
        o["user"] = s.value("sync/user").toString();
        o["password"] = secret("password");
        o["security"] = s.value("sync/security", "explicit").toString();
        o["path"] = s.value("sync/remotePath", QString("/") + SyncStorage::FileName).toString();
        o["fingerprint"] = s.value("sync/fingerprint").toString();
    }
    else if (type == "gist")
    {
        o["token"] = secret("token");
        o["gist"] = s.value("sync/gist").toString();
    }
    else
        o["refresh"] = secret("refresh");
    return o;
}

void SyncController::saveStorageUpdates(const QJsonObject &updated)
{
    if (updated.contains("gist"))
        QSettings().setValue("sync/gist", updated["gist"].toString());
    if (updated.contains("refresh"))
        setSecret("refresh", updated["refresh"].toString());
}

std::optional<SyncVault::Keys> SyncController::keys() const
{
    return decodeKeys(secret("keys"));
}

void SyncController::storeKeys(const SyncVault::Keys &k)
{
    setSecret("keys", encodeKeys(k));
}

void SyncController::setStatus(const QString &text)
{
    statusText = text;
    if (statusChanged)
        statusChanged();
}

// This device's payload: sessions, tunnels, macros as saved, plus saved passwords and key files when chosen.
QJsonObject SyncController::collect(const SyncVault::Keys &k)
{
    auto payload = host.local();
    const auto chosen = kinds();
    const auto sessions = host.sessions();
    if (chosen.contains("passwords"))
    {
        QSettings settings;
        auto stamps = QJsonDocument::fromJson(settings.value("sync/stamps").toByteArray()).object();
        QJsonObject passwords;
        for (const auto &v : sessions)
        {
            const auto profile = v.toObject();
            const auto target = credentialTarget(profile, QCoreApplication::applicationName());
            if (passwords.contains(target))
                continue;
            const auto value = readSecret(target);
            if (!value || value->isEmpty())
                continue;
            // Changed since the last run? Compared through an HMAC keyed with the data key: nothing in the settings
            // reveals the password.
            const auto mac = QString::fromLatin1(
                QMessageAuthenticationCode::hash((target + '\n' + *value).toUtf8(), k.dataKey, QCryptographicHash::Sha256).toBase64());
            auto stamp = stamps[target].toObject();
            if (stamp["mac"].toString() != mac)
                stamp = QJsonObject{{"mac", mac}, {"modified", double(SyncMerge::nowMs())}};
            stamps[target] = stamp;
            passwords[target] = QJsonObject{{"user", profile["user"].toString()}, {"secret", *value}, {"modified", stamp["modified"]}};
        }
        // Passwords synchronized before: still in the vault (their session may be gone) they keep travelling; gone
        // from the vault they were deleted here, and the deletion goes to the other devices.
        auto deleted = payload["deleted"].toObject();
        const auto now = SyncMerge::nowMs();
        for (const auto &target : stamps.keys())
        {
            if (passwords.contains(target))
                continue;
            auto stamp = stamps[target].toObject();
            if (const auto value = readSecret(target); value && !value->isEmpty())
            {
                if (stamp.contains("deleted")) // saved again after its deletion: a new change
                {
                    stamp = QJsonObject{{"mac", QString::fromLatin1(QMessageAuthenticationCode::hash(
                                                                         (target + '\n' + *value).toUtf8(), k.dataKey,
                                                                         QCryptographicHash::Sha256)
                                                                         .toBase64())},
                                        {"modified", double(now)}};
                    stamps[target] = stamp;
                }
                passwords[target] = QJsonObject{
                    {"user", target.section(':', 1).section('@', 0, 0)}, {"secret", *value}, {"modified", stamp["modified"]}};
                continue;
            }
            if (!stamp.contains("deleted"))
                stamp = QJsonObject{{"deleted", double(now)}};
            if (now - qint64(stamp["deleted"].toDouble()) > SyncMerge::TombstoneLifetimeMs)
            {
                stamps.remove(target);
                continue;
            }
            stamps[target] = stamp;
            deleted["passwords/" + target] = stamp["deleted"];
        }
        settings.setValue("sync/stamps", QJsonDocument(stamps).toJson(QJsonDocument::Compact));
        payload["passwords"] = passwords;
        payload["deleted"] = deleted;
    }
    if (chosen.contains("keys"))
    {
        QJsonObject files;
        for (const auto &v : sessions)
        {
            const auto profile = v.toObject();
            QFile key(profile["identity"].toString());
            if (profile["identity"].toString().isEmpty() || key.size() > 65536 || !key.open(QIODevice::ReadOnly))
                continue;
            files[profile["id"].toString()] = QJsonObject{
                {"name", QFileInfo(key).fileName()},
                {"data", QString::fromLatin1(key.readAll().toBase64())},
                {"modified", double(QFileInfo(key).lastModified().toMSecsSinceEpoch())}};
        }
        payload["keys"] = files;
    }
    return payload;
}

void SyncController::apply(const QJsonObject &merged, const SyncVault::Keys &k)
{
    QJsonObject data = merged;
    if (merged.contains("passwords"))
    {
        QSettings settings;
        auto stamps = QJsonDocument::fromJson(settings.value("sync/stamps").toByteArray()).object();
        const auto passwords = merged["passwords"].toObject();
        for (auto it = passwords.begin(); it != passwords.end(); ++it)
        {
            const auto p = it.value().toObject();
            const auto value = p["secret"].toString();
            if (value.isEmpty())
                continue;
            if (readSecret(it.key()).value_or(QString()) != value)
                storeSecret(it.key(), p["user"].toString(), value);
            stamps[it.key()] = QJsonObject{
                {"mac", QString::fromLatin1(QMessageAuthenticationCode::hash((it.key() + '\n' + value).toUtf8(), k.dataKey,
                                                                              QCryptographicHash::Sha256)
                                                .toBase64())},
                {"modified", p["modified"]}};
        }
        // Deleted on another device after its last change (the merge keeps a later change instead): removed here.
        const auto deleted = merged["deleted"].toObject();
        for (auto it = deleted.begin(); it != deleted.end(); ++it)
        {
            if (!it.key().startsWith("passwords/"))
                continue;
            const auto target = it.key().mid(10);
            if (passwords.contains(target))
                continue;
            if (readSecret(target))
                removeSecret(target);
            stamps[target] = QJsonObject{{"deleted", it.value()}};
        }
        settings.setValue("sync/stamps", QJsonDocument(stamps).toJson(QJsonDocument::Compact));
        data.remove("passwords");
    }
    if (merged.contains("keys") && merged.contains("sessions"))
    {
        // Key files from other devices go to a folder of CrossTerm (owner only) and become the session's key here
        // when the session has none, or the one it has is missing or is one of these files. A key file the user
        // chose elsewhere is never replaced.
        const auto folder = QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath("sync-keys");
        const auto files = merged["keys"].toObject();
        auto sessions = merged["sessions"].toArray();
        for (int i = 0; i < sessions.size(); ++i)
        {
            auto profile = sessions[i].toObject();
            const auto key = files[profile["id"].toString()].toObject();
            const auto current = profile["identity"].toString();
            if (key.isEmpty() || !(current.isEmpty() || !QFile::exists(current) || current.startsWith(folder)))
                continue;
            const auto name = QFileInfo(key["name"].toString()).fileName().remove(QRegularExpression("[^A-Za-z0-9._-]"));
            const auto path = QDir(folder).filePath(profile["id"].toString() + "-" + (name.isEmpty() ? "key" : name));
            const auto bytes = QByteArray::fromBase64(key["data"].toString().toLatin1());
            QFile existing(path);
            if (!existing.open(QIODevice::ReadOnly) || existing.readAll() != bytes)
            {
                existing.close();
                QDir().mkpath(folder);
                QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
                QSaveFile out(path);
                if (!out.open(QIODevice::WriteOnly))
                    continue;
                out.write(bytes);
                if (!out.commit())
                    continue;
                QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner); // ssh refuses keys others can read
            }
            profile["identity"] = QDir::toNativeSeparators(path);
            sessions[i] = profile;
        }
        data["sessions"] = sessions;
    }
    data.remove("keys");
    host.apply(data);
}

void SyncController::syncNow()
{
    if (!enabled())
        return;
    if (running)
    {
        pending = true; // once more when this run ends
        return;
    }
    QString error;
    auto storage = SyncStorage::create(storageSettings(), &error);
    const auto k = keys();
    if (!storage || !k)
        return setStatus(storage ? ui("Set up synchronization again (open Synchronization).") : error);
    QFile file(baseFile);
    const auto base = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
    setStatus(ui("Synchronizing..."));
    running = std::make_unique<SyncEngine::Engine>(std::move(storage), *k, base, kinds());
    running->run(SyncEngine::Device{[this, k] { return collect(*k); }, [this, k](const QJsonObject &m) { apply(m, *k); }},
                 [this](const SyncEngine::Outcome &o) { finished(o); });
}

void SyncController::finished(const SyncEngine::Outcome &o)
{
    // The engine may answer from inside run(): it is deleted afterwards, not here.
    auto *engine = running.release();
    QTimer::singleShot(0, this, [engine] { delete engine; });
    saveStorageUpdates(o.updated);
    if (o.status == SyncEngine::Outcome::Ok)
    {
        storeKeys(o.keys);
        QSaveFile out(baseFile);
        if (out.open(QIODevice::WriteOnly))
        {
            out.write(QJsonDocument(o.base).toJson(QJsonDocument::Compact));
            out.commit();
        }
        QSettings().setValue("sync/last", QDateTime::currentDateTime());
        auto text = ui("Synchronized at %1.").arg(QLocale().toString(QTime::currentTime(), QLocale::ShortFormat));
        if (!o.conflicts.isEmpty())
            text += " " + ui("Changed on two devices, both versions kept: %1").arg(o.conflicts.join(", "));
        setStatus(text);
    }
    else if (o.status == SyncEngine::Outcome::NeedPassword)
        setStatus(ui("The sync password is needed again (open Synchronization)."));
    else if (o.signIn)
        setStatus(ui("Sign in to the service again (open Synchronization)."));
    else if (!o.fingerprint.isEmpty())
        setStatus(ui("The FTPS server certificate changed (open Synchronization to check it)."));
    else
        setStatus(ui("Synchronization failed: %1").arg(o.error));
    if (std::exchange(pending, false))
        QTimer::singleShot(0, this, [this] { syncNow(); });
}

void SyncController::dialog(QWidget *parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(ui("Synchronization"));
    QSettings settings;
    auto *layout = new QVBoxLayout(&dialog);

    auto *top = new QHBoxLayout;
    auto *on = new QCheckBox(ui("Synchronization on"));
    on->setChecked(enabled());
    auto *now = new QPushButton(ui("Synchronize now"));
    top->addWidget(on);
    top->addStretch(1);
    top->addWidget(now);
    layout->addLayout(top);
    auto *status = new QLabel(statusText.isEmpty() ? ui("Not synchronized yet.") : statusText);
    status->setWordWrap(true);
    layout->addWidget(status);
    const auto previous = statusChanged;
    QPointer<QLabel> statusLabel(status);
    statusChanged = [this, statusLabel, previous]
    {
        if (statusLabel)
            statusLabel->setText(statusText);
        if (previous)
            previous();
    };

    // --- Storage
    auto *storageBox = new QGroupBox(ui("Storage"));
    auto *storageLayout = new QFormLayout(storageBox);
    auto *service = new QComboBox;
    for (const auto &s : services())
        service->addItem(s.name, s.type);
    service->setCurrentIndex(std::max(0, service->findData(settings.value("sync/type", "folder").toString())));
    storageLayout->addRow(ui("Service"), service);
    // One page per service, only the chosen one visible (hidden pages take no room).
    auto *pages = new QWidget;
    auto *pagesLayout = new QVBoxLayout(pages);
    pagesLayout->setContentsMargins(0, 0, 0, 0);
    storageLayout->addRow(pages);
    QMap<QString, QWidget *> pageOf;
    auto newPage = [&](const QString &type)
    {
        auto *page = new QWidget;
        auto *form = new QFormLayout(page);
        form->setContentsMargins(0, 0, 0, 0);
        pagesLayout->addWidget(page);
        pageOf[type] = page;
        return form;
    };
    // Folder
    auto *folderForm = newPage("folder");
    auto *folder = new QLineEdit(QDir::toNativeSeparators(settings.value("sync/path").toString()));
    auto *pick = folder->addAction(QApplication::style()->standardIcon(QStyle::SP_DirOpenIcon), QLineEdit::TrailingPosition);
    connect(pick, &QAction::triggered, &dialog,
            [&dialog, folder]
            {
                const auto chosen = QFileDialog::getExistingDirectory(&dialog, ui("Synced folder"), folder->text());
                if (!chosen.isEmpty())
                    folder->setText(QDir::toNativeSeparators(chosen));
            });
    folderForm->addRow(ui("Folder"), folder);
    auto *folderHint = new QLabel(ui("A folder kept in sync by its own client: Dropbox, OneDrive, iCloud Drive, Google Drive, "
                                     "Proton Drive, Syncthing..."));
    folderHint->setWordWrap(true);
    folderForm->addRow(folderHint);
    // Cloud drives: sign-in
    QMap<QString, QLabel *> accountLabels;
    for (const auto &type : {QString("dropbox"), QString("onedrive"), QString("gdrive")})
    {
        auto *form = newPage(type);
        auto *row = new QHBoxLayout;
        auto *account = new QLabel;
        auto *signIn = new QPushButton;
        row->addWidget(account, 1);
        row->addWidget(signIn);
        form->addRow(ui("Account"), row);
        auto refresh = [account, signIn]
        {
            const bool connected = !secret("refresh").isEmpty();
            account->setText(connected ? ui("Signed in") : ui("Not signed in"));
            signIn->setText(connected ? ui("Sign out") : ui("Sign in..."));
        };
        refresh();
        connect(signIn, &QPushButton::clicked, &dialog,
                [&dialog, type, refresh, account]
                {
                    if (!secret("refresh").isEmpty())
                    {
                        setSecret("refresh", {});
                        return refresh();
                    }
                    account->setText(ui("Waiting for the browser..."));
                    auto *login = new SyncOAuth::Login(&dialog);
                    login->start(SyncOAuth::provider(type), [](const QUrl &url) { QDesktopServices::openUrl(url); },
                                 [&dialog, login, refresh](const SyncOAuth::Tokens &t)
                                 {
                                     login->deleteLater();
                                     if (!t.error.isEmpty())
                                         QMessageBox::warning(&dialog, ui("Synchronization"), t.error);
                                     else
                                         setSecret("refresh", t.refresh);
                                     refresh();
                                 });
                });
        auto *where = new QLabel(type == "gdrive" ? ui("The file goes to a hidden folder of the app: nothing else of the account is visible to CrossTerm.")
                                                  : ui("The file goes to Apps/CrossTerm: nothing else of the account is visible to CrossTerm."));
        where->setWordWrap(true);
        form->addRow(where);
    }
    // WebDAV
    auto *webdavForm = newPage("webdav");
    auto *url = new QLineEdit(settings.value("sync/url").toString());
    url->setPlaceholderText("https://cloud.example.com/remote.php/dav/files/me/crossterm-sync.ctvault");
    auto *webdavUser = new QLineEdit(settings.value("sync/user").toString());
    auto *webdavPassword = new QLineEdit(secret("password"));
    webdavPassword->setEchoMode(QLineEdit::Password);
    webdavForm->addRow(ui("File address"), url);
    webdavForm->addRow(ui("User"), webdavUser);
    webdavForm->addRow(ui("Password"), webdavPassword);
    // SFTP
    auto *sftpForm = newPage("sftp");
    auto *session = new QComboBox;
    for (const auto &v : host.sessions())
    {
        const auto p = v.toObject();
        if (p["protocol"].toString() == "ssh" || p["protocol"].toString() == "sftp")
            session->addItem(QString("%1 (%2@%3)").arg(p["name"].toString(), p["user"].toString(), p["host"].toString()), p["id"]);
    }
    session->setCurrentIndex(std::max(0, session->findData(settings.value("sync/session").toString())));
    auto *sftpPath = new QLineEdit(settings.value("sync/remotePath", SyncStorage::FileName).toString());
    sftpForm->addRow(ui("Session"), session);
    sftpForm->addRow(ui("File path"), sftpPath);
    auto *sftpHint = new QLabel(ui("It uses the key or the saved password of the session: it never asks while synchronizing."));
    sftpHint->setWordWrap(true);
    sftpForm->addRow(sftpHint);
    // FTPS
    auto *ftpsForm = newPage("ftps");
    auto *ftpsHost = new QLineEdit(settings.value("sync/host").toString());
    auto *ftpsPort = new QSpinBox;
    ftpsPort->setRange(1, 65535);
    ftpsPort->setValue(settings.value("sync/port", 21).toInt());
    auto *ftpsUser = new QLineEdit(settings.value("sync/user").toString());
    auto *ftpsPassword = new QLineEdit(secret("password"));
    ftpsPassword->setEchoMode(QLineEdit::Password);
    auto *security = new QComboBox;
    security->addItem(ui("Explicit (AUTH TLS)"), "explicit");
    security->addItem(ui("Implicit (TLS from the start)"), "implicit");
    security->setCurrentIndex(std::max(0, security->findData(settings.value("sync/security", "explicit").toString())));
    auto *ftpsPath = new QLineEdit(settings.value("sync/remotePath", QString("/") + SyncStorage::FileName).toString());
    ftpsForm->addRow(ui("Server"), ftpsHost);
    ftpsForm->addRow(ui("Port"), ftpsPort);
    ftpsForm->addRow(ui("User"), ftpsUser);
    ftpsForm->addRow(ui("Password"), ftpsPassword);
    ftpsForm->addRow(ui("TLS"), security);
    ftpsForm->addRow(ui("File path"), ftpsPath);
    // Gist
    auto *gistForm = newPage("gist");
    auto *token = new QLineEdit(secret("token"));
    token->setEchoMode(QLineEdit::Password);
    gistForm->addRow(ui("Token"), token);
    auto *gistHint = new QLabel(ui("A GitHub token with the \"gist\" permission. The file goes to a secret gist created by "
                                   "the first synchronization."));
    gistHint->setWordWrap(true);
    gistForm->addRow(gistHint);
    const auto showPage = [service, pageOf, &dialog]
    {
        for (auto it = pageOf.begin(); it != pageOf.end(); ++it)
            it.value()->setVisible(it.key() == service->currentData().toString());
        if (dialog.isVisible())
            dialog.adjustSize();
    };
    connect(service, &QComboBox::currentIndexChanged, &dialog, showPage);
    showPage();
    layout->addWidget(storageBox);

    // --- What
    auto *whatBox = new QGroupBox(ui("What to synchronize"));
    auto *whatLayout = new QVBoxLayout(whatBox);
    const auto chosen = kinds();
    QMap<QString, QCheckBox *> boxes;
    for (const auto &[kind, label] : QList<std::pair<QString, QString>>{{"sessions", ui("Sessions, folders and favorites")},
                                                                         {"tunnels", ui("Tunnels")},
                                                                         {"macros", ui("Macros")},
                                                                         {"passwords", ui("Saved passwords")},
                                                                         {"keys", ui("SSH private keys")}})
    {
        boxes[kind] = new QCheckBox(label);
        boxes[kind]->setChecked(chosen.contains(kind));
        whatLayout->addWidget(boxes[kind]);
    }
    auto *keysHint = new QLabel(ui("Keys travel only inside the encrypted file and need the sessions to be synchronized."));
    keysHint->setWordWrap(true);
    keysHint->setIndent(22);
    whatLayout->addWidget(keysHint);
    const auto keysAllowed = [boxes] { boxes["keys"]->setEnabled(boxes["sessions"]->isChecked()); };
    connect(boxes["sessions"], &QCheckBox::toggled, &dialog, keysAllowed);
    keysAllowed();
    layout->addWidget(whatBox);

    // --- Security
    auto *securityBox = new QGroupBox(ui("Security"));
    auto *securityLayout = new QFormLayout(securityBox);
    auto *passwordRow = new QHBoxLayout;
    auto *passwordState = new QLabel;
    auto *change = new QPushButton(ui("Change..."));
    passwordRow->addWidget(passwordState, 1);
    passwordRow->addWidget(change);
    securityLayout->addRow(ui("Sync password"), passwordRow);
    auto *recoverButton = new QPushButton(ui("Forgot the password: use the recovery key..."));
    securityLayout->addRow(recoverButton);
    auto *cipher = new QLabel(ui("Encrypted on this device: XChaCha20-Poly1305, key from the password with Argon2id."));
    cipher->setWordWrap(true);
    securityLayout->addRow(cipher);
    const auto showKeys = [this, passwordState, change]
    {
        const bool set = keys().has_value();
        passwordState->setText(set ? ui("Set") : ui("Not set"));
        change->setEnabled(set);
    };
    showKeys();
    layout->addWidget(securityBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    // Fields saved as they are (secrets to the vault), before anything uses the storage.
    const auto saveFields = [=]
    {
        QSettings s;
        const auto type = service->currentData().toString();
        s.setValue("sync/type", type);
        s.setValue("sync/path", QDir::fromNativeSeparators(folder->text().trimmed()));
        if (type == "webdav")
        {
            s.setValue("sync/url", url->text().trimmed());
            s.setValue("sync/user", webdavUser->text().trimmed());
            setSecret("password", webdavPassword->text());
        }
        else if (type == "sftp")
        {
            s.setValue("sync/session", session->currentData().toString());
            s.setValue("sync/remotePath", sftpPath->text().trimmed());
        }
        else if (type == "ftps")
        {
            s.setValue("sync/host", ftpsHost->text().trimmed());
            s.setValue("sync/port", ftpsPort->value());
            s.setValue("sync/user", ftpsUser->text().trimmed());
            setSecret("password", ftpsPassword->text());
            s.setValue("sync/security", security->currentData().toString());
            s.setValue("sync/remotePath", ftpsPath->text().trimmed());
        }
        else if (type == "gist")
            setSecret("token", token->text().trimmed());
        QStringList list;
        for (auto it = boxes.begin(); it != boxes.end(); ++it)
            if (it.value()->isChecked() && it.value()->isEnabled())
                list << it.key();
        s.setValue("sync/kinds", list);
    };

    // The storage file: read once, asking to trust an FTPS certificate when needed.
    const auto readStorage = [this, &dialog](std::unique_ptr<SyncStorage::Storage> &storage) -> std::optional<SyncStorage::Result>
    {
        for (;;)
        {
            QString error;
            storage = SyncStorage::create(storageSettings(), &error);
            if (!storage)
            {
                QMessageBox::warning(&dialog, ui("Synchronization"), error);
                return std::nullopt;
            }
            auto read = wait([&](SyncStorage::Done d) { storage->read(d); });
            saveStorageUpdates(read.updated);
            if (read.status != SyncStorage::Result::Failed)
                return read;
            if (!read.fingerprint.isEmpty() &&
                QMessageBox::question(&dialog, ui("Synchronization"),
                                      ui("The FTPS server presents a certificate that is not trusted yet.\n\nSHA-256: %1\n\n"
                                         "Trust it? Compare the fingerprint with the one of the server.")
                                          .arg(read.fingerprint)) == QMessageBox::Yes)
            {
                QSettings().setValue("sync/fingerprint", read.fingerprint);
                continue;
            }
            QMessageBox::warning(&dialog, ui("Synchronization"), read.error);
            return std::nullopt;
        }
    };

    // Turning it on: the first device creates the password and the recovery key; the others unlock the file.
    const auto turnOn = [=, &dialog]() -> bool
    {
        saveFields();
        std::unique_ptr<SyncStorage::Storage> storage;
        const auto read = readStorage(storage);
        if (!read)
            return false;
        if (read->status == SyncStorage::Result::NotFound)
        {
            const auto password = askNewPassword(&dialog, ui("New synchronization"),
                                                  ui("The storage has no CrossTerm data yet. Choose the sync password: it "
                                                     "encrypts everything on this device and is needed on the others."));
            if (password.isEmpty())
                return false;
            const auto recovery = SyncVault::newRecoveryKey();
            if (!showRecoveryKey(&dialog, recovery))
                return false;
            storeKeys(SyncVault::create(password, recovery));
        }
        else
        {
            for (;;)
            {
                bool ok = false;
                const auto password = QInputDialog::getText(
                    &dialog, ui("Connect this device"),
                    ui("The storage already has CrossTerm data from another device. Enter the sync password chosen "
                       "there.\nThe sessions of this device are merged with them: nothing is overwritten."),
                    QLineEdit::Password, {}, &ok);
                if (!ok)
                    return false;
                SyncVault::Error error = SyncVault::Error::None;
                if (const auto unlocked = SyncVault::unlock(read->data, password, &error))
                {
                    storeKeys(*unlocked);
                    break;
                }
                if (error != SyncVault::Error::WrongPassword)
                {
                    QMessageBox::warning(&dialog, ui("Synchronization"),
                                         error == SyncVault::Error::NewerVersion
                                             ? ui("The sync file was written by a newer CrossTerm: update this one.")
                                             : ui("The file in the storage is not a CrossTerm sync file."));
                    return false;
                }
                QMessageBox::warning(&dialog, ui("Synchronization"), ui("The password or recovery key is not correct."));
            }
        }
        QFile::remove(baseFile); // a new partner: merge without a base (nothing is taken as deleted)
        QSettings().setValue("sync/enabled", true);
        return true;
    };

    connect(on, &QCheckBox::toggled, &dialog,
            [=](bool checked)
            {
                if (!checked)
                {
                    QSettings().setValue("sync/enabled", false);
                    setStatus(ui("Synchronization off."));
                    return;
                }
                const QSignalBlocker block(on);
                on->setChecked(turnOn());
                showKeys();
                if (on->isChecked())
                    syncNow();
            });
    connect(now, &QPushButton::clicked, &dialog,
            [=]
            {
                saveFields();
                syncNow();
            });
    connect(change, &QPushButton::clicked, &dialog,
            [=, &dialog]
            {
                const auto current = keys();
                const auto password = askNewPassword(&dialog, ui("Change the sync password"),
                                                     ui("The new password is needed on the other devices only when they "
                                                        "are set up again; the ones already connected keep working."));
                if (password.isEmpty() || !current)
                    return;
                saveFields();
                QString error;
                auto storage = SyncStorage::create(storageSettings(), &error);
                if (!storage)
                    return (void)QMessageBox::warning(&dialog, ui("Synchronization"), error);
                std::optional<SyncEngine::Outcome> result;
                QEventLoop loop;
                SyncEngine::changePassword(*storage, *current, password,
                                           [&](const SyncEngine::Outcome &o)
                                           {
                                               result = o;
                                               loop.quit();
                                           });
                if (!result)
                    loop.exec();
                if (result->status != SyncEngine::Outcome::Ok)
                    return (void)QMessageBox::warning(&dialog, ui("Synchronization"), result->error);
                storeKeys(result->keys);
                QMessageBox::information(&dialog, ui("Synchronization"), ui("Sync password changed."));
            });
    connect(recoverButton, &QPushButton::clicked, &dialog,
            [=, &dialog]
            {
                bool ok = false;
                const auto key = QInputDialog::getText(&dialog, ui("Recovery key"), ui("Recovery key"), QLineEdit::Normal, {}, &ok);
                if (!ok || key.trimmed().isEmpty())
                    return;
                saveFields();
                std::unique_ptr<SyncStorage::Storage> storage;
                const auto read = readStorage(storage);
                if (!read)
                    return;
                const auto unlocked = read->status == SyncStorage::Result::Ok ? SyncVault::unlockWithRecovery(read->data, key)
                                                                               : std::nullopt;
                if (!unlocked)
                    return (void)QMessageBox::warning(&dialog, ui("Synchronization"),
                                                      read->status == SyncStorage::Result::Ok
                                                          ? ui("The password or recovery key is not correct.")
                                                          : ui("There is no sync file in this storage yet."));
                storeKeys(*unlocked);
                showKeys();
                // A new password right away: the forgotten one should not stay in use.
                const auto password = askNewPassword(&dialog, ui("Change the sync password"),
                                                     ui("Choose a new sync password."));
                if (!password.isEmpty())
                {
                    std::optional<SyncEngine::Outcome> result;
                    QEventLoop loop;
                    SyncEngine::changePassword(*storage, *unlocked, password,
                                               [&](const SyncEngine::Outcome &o)
                                               {
                                                   result = o;
                                                   loop.quit();
                                               });
                    if (!result)
                        loop.exec();
                    if (result->status == SyncEngine::Outcome::Ok)
                        storeKeys(result->keys);
                }
                QSettings().setValue("sync/enabled", true);
                const QSignalBlocker block(on); // already set up: not the "turn on" flow again
                on->setChecked(true);
                syncNow();
            });
    dialog.resize(dialog.sizeHint().width() + 80, dialog.sizeHint().height());
    dialog.exec();
    saveFields();
    statusChanged = previous;
}
