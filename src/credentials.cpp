#include "credentials.h"
// CROSSTERM_FORCE_SECRET_SERVICE compiles the Linux branch on Windows too: only as a syntax check.
#if defined(Q_OS_WIN) && !defined(CROSSTERM_FORCE_SECRET_SERVICE)
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
#elif defined(Q_OS_MACOS)
#include <QDir>
#include <QJsonDocument>
#include <QLockFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <Security/Security.h>
#include <functional>
#else
#include <QLibrary>
#include <QProcess>
#include <QStandardPaths>
#endif

QString credentialTarget(const QJsonObject &profile, const QString &application)
{
    // SSH, SFTP, mosh and tunnels log in to the same sshd and share one entry. Any other protocol has its own, so
    // a password saved for SSH is never sent to an FTP, RDP or VNC server at the same user, host and port.
    const auto protocol = profile["protocol"].toString();
    const bool ssh = protocol.isEmpty() || protocol == "ssh" || protocol == "sftp" || protocol == "mosh";
    return QString("%1%2:%3@%4:%5")
        .arg(application, ssh ? QString() : "/" + protocol, profile["user"].toString(), profile["host"].toString())
        .arg(profile["port"].toInt(22));
}

#if defined(Q_OS_WIN) && !defined(CROSSTERM_FORCE_SECRET_SERVICE)
bool vaultAvailable()
{
    return true;
}

bool storeSecret(const QString &target, const QString &user, const QString &secret)
{
    QByteArray blob(reinterpret_cast<const char *>(secret.utf16()), secret.size() * 2);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    auto targetName = target.toStdWString(), userName = user.toStdWString();
    credential.TargetName = targetName.data();
    credential.UserName = userName.data();
    credential.CredentialBlobSize = DWORD(blob.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    const bool ok = CredWriteW(&credential, 0);
    SecureZeroMemory(blob.data(), size_t(blob.size()));
    return ok;
}

std::optional<QString> readSecret(const QString &target)
{
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0, &credential))
        return std::nullopt;
    const QString secret = QString::fromUtf16(reinterpret_cast<const char16_t *>(credential->CredentialBlob),
                                              qsizetype(credential->CredentialBlobSize / 2));
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    return secret;
}

bool removeSecret(const QString &target)
{
    return CredDeleteW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
}
#else
namespace
{
// Zeroes a buffer holding the secret; volatile keeps the compiler from eliding the writes.
void wipe(QByteArray &bytes)
{
    volatile char *p = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i)
        p[i] = 0;
}
} // namespace

#if defined(Q_OS_MACOS)
namespace
{
// All passwords live in one Keychain item, read once per launch: macOS asks for Keychain access (and Touch ID)
// once when CrossTerm opens, not at every SSH, SFTP or monitoring connection.
const QString Bundle = QStringLiteral("CrossTerm vault");

// Base Keychain query: generic password, service "CrossTerm", account = item name.
CFMutableDictionaryRef keychainQuery(const QString &account)
{
    CFMutableDictionaryRef query =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = QStringLiteral("CrossTerm").toCFString();
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFRelease(service);
    if (!account.isEmpty())
    {
        CFStringRef name = account.toCFString();
        CFDictionarySetValue(query, kSecAttrAccount, name);
        CFRelease(name);
    }
    return query;
}

// errSecSuccess, errSecItemNotFound or an error (e.g. locked Keychain, unlock cancelled).
OSStatus itemStatus(const QString &account)
{
    CFMutableDictionaryRef query = keychainQuery(account);
    const OSStatus status = SecItemCopyMatching(query, nullptr); // attributes only: no prompt
    CFRelease(query);
    return status;
}

std::optional<QByteArray> itemRead(const QString &account)
{
    CFMutableDictionaryRef query = keychainQuery(account);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status != errSecSuccess || !result)
        return std::nullopt;
    const auto data = static_cast<CFDataRef>(result);
    QByteArray bytes(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), qsizetype(CFDataGetLength(data)));
    CFRelease(result);
    return bytes;
}

bool itemWrite(const QString &account, const QByteArray &bytes)
{
    // NoCopy: the only copy of the secret stays in "bytes", which the caller zeroes.
    CFDataRef data = CFDataCreateWithBytesNoCopy(nullptr, reinterpret_cast<const UInt8 *>(bytes.constData()),
                                                 CFIndex(bytes.size()), kCFAllocatorNull);
    CFMutableDictionaryRef query = keychainQuery(account);
    CFMutableDictionaryRef changes =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(changes, kSecValueData, data);
    OSStatus status = SecItemUpdate(query, changes);
    if (status == errSecItemNotFound)
    {
        CFDictionarySetValue(query, kSecValueData, data);
        status = SecItemAdd(query, nullptr);
    }
    CFRelease(changes);
    CFRelease(query);
    CFRelease(data);
    return status == errSecSuccess;
}

bool itemDelete(const QString &account)
{
    CFMutableDictionaryRef query = keychainQuery(account);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    return status == errSecSuccess || status == errSecItemNotFound;
}

// One item per password, as written by earlier versions: names only, no prompt.
QStringList legacyAccounts()
{
    CFMutableDictionaryRef query = keychainQuery({});
    CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitAll);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    QStringList names;
    if (status != errSecSuccess || !result)
        return names;
    const auto items = static_cast<CFArrayRef>(result);
    for (CFIndex i = 0; i < CFArrayGetCount(items); ++i)
    {
        const auto item = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(items, i));
        if (const auto account = static_cast<CFStringRef>(CFDictionaryGetValue(item, kSecAttrAccount)))
            if (const auto name = QString::fromCFString(account); name != Bundle)
                names << name;
    }
    CFRelease(result);
    return names;
}

// Copy of the item in this process, as of its last read or write here.
QJsonObject &entries()
{
    static QJsonObject vault;
    return vault;
}

// The item as stored now; nullopt on a Keychain error, which is not an empty vault (no item yet).
std::optional<QJsonObject> loadVault()
{
    const OSStatus status = itemStatus(Bundle);
    if (status == errSecItemNotFound)
        return QJsonObject();
    auto bytes = status == errSecSuccess ? itemRead(Bundle) : std::nullopt;
    if (!bytes)
        return std::nullopt;
    const auto vault = QJsonDocument::fromJson(*bytes).object();
    wipe(*bytes);
    return vault;
}

bool saveVault(const QJsonObject &vault)
{
    QByteArray bytes = QJsonDocument(vault).toJson(QJsonDocument::Compact);
    const bool ok = itemWrite(Bundle, bytes);
    wipe(bytes);
    return ok;
}

// The item is rewritten as a whole and other CrossTerm processes (a second window, an askpass helper without a
// window) change it too: every change is "read the item, apply it, write" under this lock, never a write of the
// copy read at launch. Same file for all of them, whatever their settings folder (portable mode, tests).
struct VaultLock
{
    QLockFile file;
    bool ok = false;

    static QString path()
    {
        const auto folder = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/CrossTerm";
        QDir().mkpath(folder);
        return folder + "/vault.lock";
    }
    VaultLock() : file(path())
    {
        file.setStaleLockTime(0); // a holder may be waiting for a Keychain prompt; one that died is detected anyway
        // Held by another process for this long: give up. A lock file that cannot be created does not block saving.
        ok = file.tryLock(20000) || file.error() != QLockFile::LockFailedError;
    }
};

// One change of the stored item; "change" edits the entries just read and tells whether they must be written.
bool changeVault(const std::function<bool(QJsonObject &)> &change)
{
    VaultLock lock;
    auto vault = lock.ok ? loadVault() : std::nullopt;
    if (!vault)
        return false;
    if (change(*vault) && !saveVault(*vault))
        return false;
    entries() = *vault;
    return true;
}
} // namespace

bool unlockVault()
{
    // Asked once per run: a refusal (Touch ID or Keychain cancelled) is not asked again at every connection.
    static bool unlocked = false, refused = false;
    if (unlocked || refused)
        return unlocked;
    refused = true;
    const OSStatus status = itemStatus(Bundle);
    // Not "not found": starting from an empty vault would overwrite all saved passwords at the next save.
    if (status != errSecSuccess && status != errSecItemNotFound)
        return false;
    const QStringList legacy = legacyAccounts();
    const bool bundle = status == errSecSuccess;
    if (bundle || !legacy.isEmpty())
    {
        // Touch ID (or the Mac password) once; not in offscreen runs (tests), where nobody can answer.
        if (QSettings().value("vault/touchId", true).toBool() && qEnvironmentVariable("QT_QPA_PLATFORM") != "offscreen" &&
            !confirmOwner(QStringLiteral("use saved passwords")))
            return false;
        auto vault = loadVault();
        if (!vault)
            return false; // Keychain access denied: asked again at the next launch
        entries() = *vault;
        // Earlier versions: copy the separate items into the single one (one last prompt each, then never again).
        // They are left in place and ignored afterwards: deleting them would ask for the password once more each.
        QJsonObject copied;
        for (const auto &name : legacy)
            if (!entries().contains(name))
                if (auto bytes = itemRead(name))
                {
                    copied[name] = QString::fromUtf8(*bytes);
                    wipe(*bytes);
                }
        const auto merge = [&](QJsonObject &stored)
        {
            bool moved = false;
            for (auto it = copied.constBegin(); it != copied.constEnd(); ++it)
                if (!stored.contains(it.key()))
                {
                    stored[it.key()] = it.value();
                    moved = true;
                }
            return moved;
        };
        if (!copied.isEmpty() && !changeVault(merge))
            merge(entries()); // not written (copied again at the next launch), usable in this run
    }
    unlocked = true;
    refused = false;
    return true;
}

bool vaultAvailable()
{
    return true;
}

bool storeSecret(const QString &target, const QString &, const QString &secret)
{
    return unlockVault() && changeVault(
                                [&](QJsonObject &vault)
                                {
                                    vault[target] = secret;
                                    return true;
                                });
}

std::optional<QString> readSecret(const QString &target)
{
    if (!unlockVault() || !entries().contains(target))
        return std::nullopt;
    return entries().value(target).toString();
}

bool removeSecret(const QString &target)
{
    if (!unlockVault())
        return false;
    itemDelete(target); // separate item from an earlier version, if any
    return changeVault(
        [&](QJsonObject &vault)
        {
            const bool found = vault.contains(target);
            vault.remove(target);
            return found;
        });
}
#else
namespace
{
// Documented glib/libsecret ABI, declared here so the headers are not needed at build time.
struct GError
{
    quint32 domain;
    int code;
    char *message;
};
struct SecretSchemaAttribute
{
    const char *name;
    int type; // SECRET_SCHEMA_ATTRIBUTE_STRING = 0
};
struct SecretSchema
{
    const char *name;
    int flags; // SECRET_SCHEMA_NONE = 0
    SecretSchemaAttribute attributes[32];
    int reserved;
    void *reserved1, *reserved2, *reserved3, *reserved4, *reserved5, *reserved6, *reserved7;
};
const SecretSchema schema{"org.crossterm.Credential", 0, {{"target", 0}}};

struct LibSecret
{
    int (*store)(const SecretSchema *, const char *collection, const char *label, const char *password, void *cancellable,
                 GError **, ...) = nullptr;
    char *(*lookup)(const SecretSchema *, void *cancellable, GError **, ...) = nullptr;
    int (*clear)(const SecretSchema *, void *cancellable, GError **, ...) = nullptr;
    void (*free)(char *) = nullptr;
    void (*errorFree)(GError *) = nullptr;
    bool loaded = false;
};

// libsecret loaded at runtime (dlopen of libsecret-1.so.0): no build-time dependency.
const LibSecret &libsecret()
{
    static const LibSecret lib = []
    {
        LibSecret l;
        QLibrary library("secret-1", 0);
        if (!library.load())
            return l;
        l.store = reinterpret_cast<decltype(l.store)>(library.resolve("secret_password_store_sync"));
        l.lookup = reinterpret_cast<decltype(l.lookup)>(library.resolve("secret_password_lookup_sync"));
        l.clear = reinterpret_cast<decltype(l.clear)>(library.resolve("secret_password_clear_sync"));
        l.free = reinterpret_cast<decltype(l.free)>(library.resolve("secret_password_free"));
        l.errorFree = reinterpret_cast<decltype(l.errorFree)>(library.resolve("g_error_free")); // from glib
        l.loaded = l.store && l.lookup && l.clear && l.free && l.errorFree;
        return l;
    }();
    return lib;
}

bool succeeded(GError *error)
{
    if (!error)
        return true;
    libsecret().errorFree(error);
    return false;
}

// Fallback without libsecret: secret-tool, with the secret only on stdin and never on the command line.
QString secretTool()
{
    static const QString path = QStandardPaths::findExecutable("secret-tool");
    return path;
}

bool runSecretTool(QStringList arguments, const QString &target, const QByteArray &input, QByteArray *output)
{
    // Explicit xdg:schema: entries stay compatible with those written via libsecret.
    arguments << "xdg:schema" << schema.name << "target" << target;
    QProcess process;
    process.start(secretTool(), arguments);
    if (!process.waitForStarted())
        return false;
    process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(60000))
    {
        process.kill();
        process.waitForFinished();
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
} // namespace

bool vaultAvailable()
{
    static const bool available = []
    {
        const LibSecret &lib = libsecret();
        if (!lib.loaded)
            return !secretTool().isEmpty();
        // The Secret Service is available if any lookup completes without error.
        GError *error = nullptr;
        if (char *found = lib.lookup(&schema, nullptr, &error, "target", "CrossTerm-probe", nullptr))
            lib.free(found);
        return succeeded(error);
    }();
    return available;
}

bool storeSecret(const QString &target, const QString &, const QString &secret)
{
    QByteArray bytes = secret.toUtf8();
    const QByteArray name = target.toUtf8(), label = ("CrossTerm " + target).toUtf8();
    bool ok = false;
    if (const LibSecret &lib = libsecret(); lib.loaded)
    {
        GError *error = nullptr;
        const bool stored = lib.store(&schema, nullptr, label.constData(), bytes.constData(), nullptr, &error, "target",
                                      name.constData(), nullptr);
        ok = succeeded(error) && stored; // succeeded() first: it frees the error set on failure
    }
    else if (!secretTool().isEmpty())
        ok = runSecretTool({"store", "--label=" + QString::fromUtf8(label)}, target, bytes, nullptr);
    wipe(bytes);
    return ok;
}

std::optional<QString> readSecret(const QString &target)
{
    const QByteArray name = target.toUtf8();
    if (const LibSecret &lib = libsecret(); lib.loaded)
    {
        GError *error = nullptr;
        char *found = lib.lookup(&schema, nullptr, &error, "target", name.constData(), nullptr);
        if (!succeeded(error) || !found)
        {
            if (found)
                lib.free(found);
            return std::nullopt;
        }
        const QString secret = QString::fromUtf8(found);
        lib.free(found); // secret_password_free zeroes the memory before freeing it
        return secret;
    }
    QByteArray bytes;
    if (secretTool().isEmpty() || !runSecretTool({"lookup"}, target, {}, &bytes))
    {
        wipe(bytes);
        return std::nullopt;
    }
    const QString secret = QString::fromUtf8(bytes);
    wipe(bytes);
    return secret;
}

bool removeSecret(const QString &target)
{
    const QByteArray name = target.toUtf8();
    if (const LibSecret &lib = libsecret(); lib.loaded)
    {
        // FALSE without an error means "no entry": as on Windows, it counts as removed.
        GError *error = nullptr;
        lib.clear(&schema, nullptr, &error, "target", name.constData(), nullptr);
        return succeeded(error);
    }
    return !secretTool().isEmpty() && runSecretTool({"clear"}, target, {}, nullptr);
}
#endif
#endif
