#include "credentials.h"
// CROSSTERM_FORCE_SECRET_SERVICE compiles the Linux branch on Windows too: only as a syntax check.
#if defined(Q_OS_WIN) && !defined(CROSSTERM_FORCE_SECRET_SERVICE)
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
#elif defined(Q_OS_MACOS)
#include <QSettings>
#include <Security/Security.h>
#else
#include <QLibrary>
#include <QProcess>
#include <QStandardPaths>
#endif

QString credentialTarget(const QJsonObject &profile, const QString &application)
{
    return QString("%1:%2@%3:%4")
        .arg(application, profile["user"].toString(), profile["host"].toString())
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
// Base Keychain query: generic password, service "CrossTerm", account = entry name.
CFMutableDictionaryRef keychainQuery(const QString &target)
{
    CFMutableDictionaryRef query =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = QStringLiteral("CrossTerm").toCFString(), account = target.toCFString();
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFRelease(service);
    CFRelease(account);
    return query;
}
} // namespace

bool vaultAvailable()
{
    return true;
}

bool storeSecret(const QString &target, const QString &, const QString &secret)
{
    QByteArray bytes = secret.toUtf8();
    // NoCopy: the only copy of the secret stays in "bytes", which is zeroed at the end.
    CFDataRef data = CFDataCreateWithBytesNoCopy(nullptr, reinterpret_cast<const UInt8 *>(bytes.constData()),
                                                 CFIndex(bytes.size()), kCFAllocatorNull);
    CFMutableDictionaryRef query = keychainQuery(target);
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
    wipe(bytes);
    return status == errSecSuccess;
}

std::optional<QString> readSecret(const QString &target)
{
    CFMutableDictionaryRef query = keychainQuery(target);
    // Touch ID once per run, only when there is a saved password to read (the existence check reads no data).
    static bool confirmed = false;
    if (!confirmed && QSettings().value("vault/touchId", true).toBool())
    {
        if (SecItemCopyMatching(query, nullptr) != errSecSuccess)
        {
            CFRelease(query);
            return std::nullopt;
        }
        if (!confirmOwner(QStringLiteral("use saved passwords")))
        {
            CFRelease(query);
            return std::nullopt;
        }
        confirmed = true;
    }
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status != errSecSuccess || !result)
        return std::nullopt;
    const auto data = static_cast<CFDataRef>(result);
    const QString secret =
        QString::fromUtf8(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), qsizetype(CFDataGetLength(data)));
    CFRelease(result);
    return secret;
}

bool removeSecret(const QString &target)
{
    CFMutableDictionaryRef query = keychainQuery(target);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    return status == errSecSuccess || status == errSecItemNotFound;
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
        ok = lib.store(&schema, nullptr, label.constData(), bytes.constData(), nullptr, &error, "target", name.constData(),
                       nullptr) &&
             succeeded(error);
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
