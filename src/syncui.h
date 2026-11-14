#pragma once
// Synchronization in the program: settings, secrets, automatic runs and the "Synchronization" dialog.
//
// Settings (QSettings "sync/..."): enabled, storage settings without secrets, kinds, FTPS fingerprint.
// Secrets (system vault, entry "<app>-sync"): the vault keys (data key and wrapped copies), the storage password /
// token / refresh token. The merge base is a file next to the sessions ("sync-base.json": ids and times only).
// Saved passwords travel with their modification time; to tell whether a password changed since the last run
// without keeping anything that reveals it, the settings hold an HMAC of it keyed with the data key.
#include "syncengine.h"
#include <QJsonArray>
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>

class QWidget;

class SyncController : public QObject
{
public:
    struct Host
    {
        std::function<QJsonObject()> local;                 // {"sessions","tunnels","macros","deleted"} as saved
        std::function<void(const QJsonObject &merged)> apply; // same shape, only the kinds that are synced
        std::function<QJsonArray()> sessions;               // saved sessions (SFTP storage choice, passwords, keys)
    };

    SyncController(Host host, const QString &baseFile, QObject *parent = nullptr);
    bool enabled() const;
    QString status() const { return statusText; }
    std::function<void()> statusChanged;

    // After a local save (debounced), at start and every 10 minutes; now: right away.
    void schedule();
    void syncNow();
    void dialog(QWidget *parent);

private:
    QJsonObject storageSettings() const; // with the secrets from the vault
    void saveStorageUpdates(const QJsonObject &updated);
    std::optional<SyncVault::Keys> keys() const;
    void storeKeys(const SyncVault::Keys &keys);
    QJsonObject collect(const SyncVault::Keys &keys);
    void apply(const QJsonObject &merged, const SyncVault::Keys &keys);
    void setStatus(const QString &text);
    void finished(const SyncEngine::Outcome &outcome);

    Host host;
    QString baseFile;
    QString statusText;
    QTimer debounce, periodic;
    std::unique_ptr<SyncEngine::Engine> running;
    bool pending = false;
};
