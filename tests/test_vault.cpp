// System vault: write, read, update and removal of an entry dedicated to tests.
// Secrets are never printed: on error only the failed step is reported.
#include "credentials.h"
#include <QCoreApplication>
#include <QUuid>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (!vaultAvailable())
    {
        std::puts("vault not available: test skipped");
        return 0;
    }
    // CI runners (GitHub Actions sets CI): the macOS Keychain can wait for an unlock dialog nobody answers.
    if (!qEnvironmentVariableIsEmpty("CI"))
    {
        std::puts("CI: system vault not tested (it may wait for a dialog)");
        return 0;
    }
    const QString target = "CrossTerm-VaultTest:" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString first = QString(u"p\u00e0ss-") + QUuid::createUuid().toString(),
                  second = QString(u"secondo-\u20ac-") + QUuid::createUuid().toString();
    int failures = 0;
    const auto check = [&](bool ok, const char *step)
    {
        if (!ok)
        {
            std::printf("FAILED: %s\n", step);
            ++failures;
        }
    };
    check(!readSecret(target).has_value(), "entry absent before writing");
    check(storeSecret(target, "tester", first), "write");
    check(readSecret(target) == first, "read");
    check(storeSecret(target, "tester", second), "aggiornamento");
    check(readSecret(target) == second, "read after update");
    // A long OAuth refresh token (work accounts): about 2000 characters, beyond 2560 bytes as UTF-16.
    const QString token = QString(2000, QChar('t')) + QString(u"€");
    check(storeSecret(target, "tester", token), "write of a long secret");
    check(readSecret(target) == token, "read of a long secret");
    check(removeSecret(target), "rimozione");
    check(!readSecret(target).has_value(), "entry absent after removal");
    check(removeSecret(target), "removal of an absent entry");
    removeSecret(target); // cleanup even if an intermediate step failed
    std::printf("%s\n", failures ? "test_vault: errori" : "test_vault: ok");
    return failures ? 1 : 0;
}
