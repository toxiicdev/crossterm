#pragma once
// Operating system credential vault. On Windows it uses Credential Manager (CredWrite/CredRead,
// per-user DPAPI encryption). Passwords never appear on a command line or in logs: they reach OpenSSH
// via SSH_ASKPASS (CrossTerm itself in askpass mode) and the environment holds only the entry name.
#include <QJsonObject>
#include <QString>
#include <optional>

bool vaultAvailable();
// Vault entry name: "<app>:<user>@<host>:<port>", shared by SSH, SFTP and tunnels.
QString credentialTarget(const QJsonObject &profile, const QString &application);
bool storeSecret(const QString &target, const QString &user, const QString &secret);
std::optional<QString> readSecret(const QString &target);
bool removeSecret(const QString &target);
#ifdef Q_OS_MACOS
// Touch ID (or the Mac password) before the first saved password is read; "vault/touchId" setting, on by default.
bool confirmOwner(const QString &reason);
// Reads all saved passwords (one Keychain item) once; called when CrossTerm opens. False if access was refused.
bool unlockVault();
#endif
