#pragma once
// CrossTerm as the SSH_ASKPASS program. OpenSSH runs it with the prompt text as the only argument
// and reads the answer from stdout. The first attempt uses the vault password; if rejected (or missing)
// it asks the user. Yes/no questions (e.g. host key) become an explicit dialog, never an automatic answer.
#include "credentials.h"
#include "i18n.h"
#include "profiles.h"
#include <memory>
#include <QtWidgets>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QUuid>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef Q_OS_MACOS
#include <objc/message.h>
#include <objc/runtime.h>
#endif

// Session passwords: typed once (in the askpass dialog) and reused by the terminal, SFTP browser,
// Remote monitoring, tunnels and mosh of the same session. They live only in CrossTerm's memory and are
// reachable by the askpass process through a user-private local socket, with a random token.
class SessionSecrets
{
    QLocalServer server;
    QHash<QString, QString> secrets;

    static QByteArray encode(const QString &text) { return text.toUtf8().toBase64(); }
    static QString decode(const QByteArray &line) { return QString::fromUtf8(QByteArray::fromBase64(line.trimmed())); }

public:
    QString name, key;

    static SessionSecrets &instance()
    {
        static SessionSecrets secrets;
        return secrets;
    }
    ~SessionSecrets() { clear(); }

    bool start()
    {
        if (server.isListening())
            return true;
        key = QString::number(QRandomGenerator::system()->generate64(), 16) +
              QString::number(QRandomGenerator::system()->generate64(), 16);
        name = "crossterm-secrets-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server.listen(name))
            return false;
        QObject::connect(&server, &QLocalServer::newConnection, &server,
                         [this]
                         {
                             while (auto *socket = server.nextPendingConnection())
                             {
                                 QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                                 QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, socket] { serve(socket); });
                             }
                         });
        return true;
    }

    // Request: token, command (GET/PUT/DEL), name, [secret]; name and secret in base64, one line each.
    void serve(QLocalSocket *socket)
    {
        if (!socket->canReadLine())
            return;
        const auto all = socket->peek(64 * 1024);
        const auto lines = all.split('\n');
        const bool withValue = lines.size() > 1 && (lines[1].trimmed() == "PUT" || lines[1].trimmed() == "ASK" ||
                                                    lines[1].trimmed() == "VPUT");
        if (lines.size() < 4 || (withValue && lines.size() < 5))
            return; // incomplete request: wait for more data
        socket->readAll();
        QByteArray reply = "\n";
        if (lines[0].trimmed() == key.toUtf8())
        {
            const auto command = lines[1].trimmed();
            const auto entry = decode(lines[2]);
            if (command == "ASK")
            {
                // entry = terminal id; value = echo flag ('0'/'1') + prompt. Reply "+answer", "-" (cancelled)
                // or "!" (no such terminal: the askpass process falls back to a dialog).
                const auto text = decode(lines[3]);
                QPointer<QLocalSocket> peer(socket);
                auto answer = [peer](std::optional<QString> value)
                {
                    if (!peer)
                        return;
                    QByteArray line = value ? "+" + encode(*value) + "\n" : QByteArray("-\n");
                    peer->write(line);
                    line.fill('\0');
                    peer->flush();
                    peer->disconnectFromServer();
                };
                if (ask && ask(entry, text.mid(1), text.startsWith('1'), answer))
                    return; // answered when the user presses Enter in the terminal
                reply = "!\n";
            }
            else if (command == "GET" && secrets.contains(entry))
                reply = encode(secrets.value(entry)) + "\n";
            else if (command == "PUT")
                put(entry, decode(lines[3]));
            else if (command == "DEL")
                forget(entry);
            // System vault, read and written by this process for the askpass helpers: macOS then asks for
            // Keychain access once, in the foreground app, instead of once per background helper.
            else if (command == "VGET")
            {
                auto value = readSecret(entry);
                reply = value ? "+" + encode(*value) + "\n" : QByteArray("-\n");
                if (value)
                    value->fill(QChar(0));
            }
            else if (command == "VPUT")
                reply = storeSecret(entry, entry.section(':', 1).section('@', 0, 0), decode(lines[3])) ? "+\n" : "-\n";
            else if (command == "VDEL")
                reply = removeSecret(entry) ? "+\n" : "-\n";
        }
        socket->write(reply);
        socket->flush();
        socket->disconnectFromServer();
    }

    // Called (queued) after a secret is cached: SFTP and monitoring that failed before login retry.
    std::function<void(const QString &)> stored;
    // Shows the prompt inside the terminal with the given id; false if that terminal does not exist.
    std::function<bool(const QString &terminal, const QString &prompt, bool echo,
                       std::function<void(std::optional<QString>)> answer)>
        ask;

    void put(const QString &entry, const QString &secret)
    {
        secrets.insert(entry, secret);
        QTimer::singleShot(0, &server,
                           [this, entry]
                           {
                               if (stored)
                                   stored(entry);
                           });
    }
    std::optional<QString> get(const QString &entry) const
    {
        return secrets.contains(entry) ? std::optional<QString>(secrets.value(entry)) : std::nullopt;
    }
    void forget(const QString &entry)
    {
        auto it = secrets.find(entry);
        if (it == secrets.end())
            return;
        it->fill(QChar(0));
        secrets.erase(it);
    }
    void clear()
    {
        for (auto &secret : secrets)
            secret.fill(QChar(0));
        secrets.clear();
    }
};

// Askpass side: one request to the CrossTerm window that launched ssh; nullopt when it cannot be reached.
inline std::optional<QByteArray> requestSessionSecrets(const QByteArray &command, const QString &entry,
                                                       const QString &secret = {}, int timeout = 2000)
{
    const auto server = qEnvironmentVariable("CROSSTERM_ASKPASS_SERVER");
    const auto key = qEnvironmentVariable("CROSSTERM_ASKPASS_KEY");
    if (server.isEmpty() || key.isEmpty())
        return std::nullopt;
    QLocalSocket socket;
    socket.connectToServer(server);
    if (!socket.waitForConnected(2000))
        return std::nullopt;
    QByteArray request = key.toUtf8() + "\n" + command + "\n" + entry.toUtf8().toBase64() + "\n" +
                         (command == "PUT" || command == "VPUT" ? secret.toUtf8().toBase64() : QByteArray()) + "\n";
    socket.write(request);
    request.fill('\0');
    socket.flush();
    QByteArray reply;
    while (!reply.contains('\n') && socket.waitForReadyRead(timeout))
        reply += socket.readAll();
    if (!reply.contains('\n'))
        return std::nullopt;
    return reply.trimmed();
}

// Askpass side: queries the cache of the CrossTerm window that launched ssh.
inline QString askSessionSecrets(const QByteArray &command, const QString &entry, const QString &secret = {})
{
    auto reply = requestSessionSecrets(command, entry, secret).value_or(QByteArray());
    auto value = QString::fromUtf8(QByteArray::fromBase64(reply));
    reply.fill('\0');
    return value;
}

// Askpass side: the system vault through the CrossTerm window (which may wait for a Keychain prompt), or directly
// when there is no window to ask.
inline std::optional<QString> vaultRead(const QString &target)
{
    auto reply = requestSessionSecrets("VGET", target, {}, 300000);
    if (!reply)
        return readSecret(target);
    std::optional<QString> value;
    if (reply->startsWith('+'))
        value = QString::fromUtf8(QByteArray::fromBase64(reply->mid(1)));
    reply->fill('\0');
    return value;
}
inline void vaultStore(const QString &target, const QString &secret)
{
    if (!requestSessionSecrets("VPUT", target, secret, 300000))
        storeSecret(target, target.section(':', 1).section('@', 0, 0), secret);
}
inline void vaultRemove(const QString &target)
{
    if (!requestSessionSecrets("VDEL", target, {}, 300000))
        removeSecret(target);
}

// Askpass side: asks the CrossTerm window to read the answer inside the session's terminal.
// handled = false when there is no such terminal (use a dialog); value = nullopt when the user cancelled.
struct TerminalAnswer
{
    bool handled = false;
    std::optional<QString> value;
};

inline TerminalAnswer askInTerminal(const QString &prompt, bool echo)
{
    const auto terminal = qEnvironmentVariable("CROSSTERM_ASKPASS_TERMINAL");
    const auto server = qEnvironmentVariable("CROSSTERM_ASKPASS_SERVER");
    const auto key = qEnvironmentVariable("CROSSTERM_ASKPASS_KEY");
    if (terminal.isEmpty() || server.isEmpty() || key.isEmpty())
        return {};
    QLocalSocket socket;
    socket.connectToServer(server);
    if (!socket.waitForConnected(2000))
        return {};
    socket.write(key.toUtf8() + "\nASK\n" + terminal.toUtf8().toBase64() + "\n" +
                 (QString(echo ? "1" : "0") + prompt).toUtf8().toBase64() + "\n");
    socket.flush();
    QByteArray reply;
    while (!reply.contains('\n') && socket.waitForReadyRead(-1)) // the user may take a while to type
        reply += socket.readAll();
    reply += socket.readAll();
    TerminalAnswer result;
    if (reply.startsWith('+'))
        result = {true, QString::fromUtf8(QByteArray::fromBase64(reply.mid(1).trimmed()))};
    else if (reply.startsWith('-'))
        result.handled = true;
    reply.fill('\0');
    return result;
}

// Variables for the ssh/sftp process: entry name, attempt token and CrossTerm path.
// terminal: id of the terminal tab that shows the prompts inline instead of a dialog.
inline QStringList askpassEnvironment(const QString &target, bool interactive, const QString &terminal = {})
{
    QStringList environment{"SSH_ASKPASS=" + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()),
                            "SSH_ASKPASS_REQUIRE=force", "CROSSTERM_CREDENTIAL=" + target,
                            "CROSSTERM_ASKPASS_TOKEN=" + QUuid::createUuid().toString(QUuid::WithoutBraces)};
    if (!interactive)
        environment << "CROSSTERM_ASKPASS_NONINTERACTIVE=1";
    if (!terminal.isEmpty())
        environment << "CROSSTERM_ASKPASS_TERMINAL=" + terminal;
    auto &secrets = SessionSecrets::instance();
    if (!secrets.name.isEmpty())
        environment << "CROSSTERM_ASKPASS_SERVER=" + secrets.name << "CROSSTERM_ASKPASS_KEY=" + secrets.key;
    return environment;
}

inline bool isAskpassInvocation(int argc)
{
    return argc == 2 && qEnvironmentVariableIsSet("CROSSTERM_ASKPASS_TOKEN");
}

static void writeAnswer(QString answer)
{
    QByteArray bytes = answer.toUtf8() + "\n";
#ifdef Q_OS_WIN
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.constData(), DWORD(bytes.size()), &written, nullptr);
    SecureZeroMemory(bytes.data(), size_t(bytes.size()));
#else
    ::write(1, bytes.constData(), size_t(bytes.size()));
    bytes.fill('\0');
#endif
    answer.fill(QChar(0));
}

// Askpass runs as a plain QCoreApplication (no windowing system: on macOS no Dock icon, no focus change). Only
// when a dialog is really needed it is replaced by a QApplication, which on macOS is an "accessory" app.
inline int runAskpass(int &argc, char **argv)
{
    std::unique_ptr<QCoreApplication> app = std::make_unique<QCoreApplication>(argc, argv);
    auto identity = [&]
    {
        app->setOrganizationName("CrossTerm");
        app->setApplicationName("CrossTerm");
    };
    identity();
    // Portable mode: the same settings (language) as the CrossTerm window that launched ssh.
    if (const auto portable = portableRoot(QCoreApplication::applicationDirPath(), app->arguments()); !portable.isEmpty())
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, portable);
    }
    auto gui = [&]
    {
        app.reset();
        app = std::make_unique<QApplication>(argc, argv);
        identity();
#ifdef Q_OS_MACOS
        using SharedApplication = void *(*)(void *, SEL);
        using SetPolicy = signed char (*)(void *, SEL, long);
        void *nsApp = reinterpret_cast<SharedApplication>(objc_msgSend)(reinterpret_cast<void *>(objc_getClass("NSApplication")),
                                                                        sel_registerName("sharedApplication"));
        reinterpret_cast<SetPolicy>(objc_msgSend)(nsApp, sel_registerName("setActivationPolicy:"),
                                                  1); // NSApplicationActivationPolicyAccessory: no Dock icon
#endif
        applyLanguage(currentLanguage());
    };
    const auto prompt = app->arguments().value(1);
    const auto target = qEnvironmentVariable("CROSSTERM_CREDENTIAL");
    const bool interactive = !qEnvironmentVariableIsSet("CROSSTERM_ASKPASS_NONINTERACTIVE");
    applyLanguage(currentLanguage());
    const bool secretPrompt = prompt.contains("password", Qt::CaseInsensitive) ||
                              prompt.contains("passphrase", Qt::CaseInsensitive) || prompt.contains("PIN", Qt::CaseSensitive);
    if (!secretPrompt)
    {
        if (!interactive)
            return 1;
        if (const auto inline_ = askInTerminal(prompt, true); inline_.handled)
        {
            if (!inline_.value)
                return 1;
            writeAnswer(*inline_.value);
            return 0;
        }
        gui();
        const auto answer = QMessageBox::question(nullptr, "CrossTerm", prompt.trimmed());
        writeAnswer(answer == QMessageBox::Yes ? "yes" : "no");
        return 0;
    }
    // Attempt counter for this ssh process (the file holds just a number). Only secret prompts count:
    // a host key question answered first must not turn the first password prompt into a "retry".
    const auto counter = QDir::temp().filePath("crossterm-askpass-" + qEnvironmentVariable("CROSSTERM_ASKPASS_TOKEN"));
    QFile file(counter);
    int attempt = file.open(QIODevice::ReadOnly) ? file.readAll().toInt() + 1 : 1;
    file.close();
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QByteArray::number(attempt));
    file.close();
    // Cache entry: the session password, or the passphrase of the key named in the prompt.
    const bool passphrase = prompt.contains("passphrase", Qt::CaseInsensitive);
    const auto entry = passphrase ? "passphrase:" + prompt.trimmed() : target;
    const auto saved = target.isEmpty() || passphrase ? std::nullopt : vaultRead(target);
    if (attempt == 1 && saved)
    {
        writeAnswer(*saved);
        return 0;
    }
    auto cached = entry.isEmpty() ? QString() : askSessionSecrets("GET", entry);
    if (attempt <= 2 && !cached.isEmpty() && !(attempt == 2 && !saved))
    {
        writeAnswer(cached); // password already typed in this CrossTerm session
        return 0;
    }
    if (attempt > 1 && !cached.isEmpty())
        askSessionSecrets("DEL", entry); // rejected by the server: do not offer it again
    cached.fill(QChar(0));
    if (!interactive || attempt > 3)
        return 1;
    // SSH terminal: the password is typed in the terminal itself, then cached for SFTP, monitoring and tunnels;
    // a vault password rejected by the server is replaced by the new one.
    if (auto typed = askInTerminal(prompt, false); typed.handled)
    {
        if (!typed.value)
            return 1;
        if (saved && !passphrase)
            vaultStore(target, *typed.value);
        if (!entry.isEmpty())
            askSessionSecrets("PUT", entry, *typed.value);
        writeAnswer(*typed.value);
        typed.value->fill(QChar(0));
        return 0;
    }
    gui();
    QDialog dialog;
    dialog.setWindowTitle(L("Authentication"));
    auto *layout = new QVBoxLayout(&dialog);
    if (attempt > 1)
    {
        auto *rejected = new QLabel(L("The saved password was rejected by the server."));
        rejected->setStyleSheet("color: #c0392b;");
        layout->addWidget(rejected);
    }
    auto *label = new QLabel(prompt.trimmed());
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto *secret = new QLineEdit;
    secret->setEchoMode(QLineEdit::Password);
    layout->addWidget(secret);
    auto *remember = new QCheckBox(L("Save password in the system vault"));
    remember->setVisible(vaultAvailable() && !target.isEmpty() && !prompt.contains("passphrase", Qt::CaseInsensitive));
    remember->setChecked(saved.has_value());
    layout->addWidget(remember);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.setWindowFlag(Qt::WindowStaysOnTopHint);
    dialog.resize(420, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted)
        return 1;
    auto value = secret->text();
    if (remember->isVisible())
    {
        if (remember->isChecked())
            vaultStore(target, value);
        else if (saved)
            vaultRemove(target);
    }
    if (!entry.isEmpty())
        askSessionSecrets("PUT", entry, value); // reused by the session's SFTP, monitoring and tunnels
    writeAnswer(value);
    value.fill(QChar(0));
    secret->clear();
    return 0;
}
