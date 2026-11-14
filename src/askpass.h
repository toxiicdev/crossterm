#pragma once
// CrossTerm as the SSH_ASKPASS program. OpenSSH runs it with the prompt text as the only argument
// and reads the answer from stdout. The first attempt uses the vault password; if rejected (or missing)
// it asks the user. Yes/no questions (e.g. host key) become an explicit dialog, never an automatic answer.
#include "credentials.h"
#include "i18n.h"
#include "profiles.h"
#include <memory>
#include <QtWidgets>
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QRandomGenerator>
#include <QScopeGuard>
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

// Attempt counters of one ssh/sftp process (see runAskpass).
inline QString askpassAttemptsFile(const QString &token)
{
    return QDir::temp().filePath("crossterm-askpass-" + token);
}

// Host as OpenSSH prints it in its prompts: lower case, no IPv6 brackets.
inline QString askpassHost(const QString &host)
{
    const auto name = host.trimmed().toLower();
    return name.startsWith('[') && name.endsWith(']') ? name.mid(1, name.size() - 2) : name;
}

// Who an OpenSSH prompt asks for: "user@host's password:" or, keyboard-interactive, "(user@host) Password:".
struct PromptIdentity
{
    QString user, host;    // host empty: the prompt names nobody (passphrase, PIN, old keyboard-interactive)
    bool keyboard = false; // "(user@host) text": the text after the prefix is chosen by the server
};

inline PromptIdentity promptIdentity(const QString &prompt)
{
    // Anchored and not greedy: text of the server after the prefix cannot name another host.
    static const QRegularExpression keyboard("^\\(([^()\\s]*?)@([^@()\\s]+)\\) ");
    static const QRegularExpression password("^(\\S*)@([^@\\s']+)'s password:\\s*$");
    if (const auto match = keyboard.match(prompt); match.hasMatch())
        return {match.captured(1), askpassHost(match.captured(2)), true};
    if (const auto match = password.match(prompt); match.hasMatch())
        return {match.captured(1), askpassHost(match.captured(2)), false};
    return {};
}

// "user@host" or "host" against a prompt identity; without a user it stands for any user of that host.
inline bool askpassSameHost(const QString &spec, const PromptIdentity &who)
{
    const auto at = spec.lastIndexOf('@');
    return !who.host.isEmpty() && askpassHost(spec.mid(at + 1)) == who.host && (at <= 0 || spec.left(at) == who.user);
}

// Jump hosts of a profile ("jumps") as "user@host" or "host", the form of CROSSTERM_CREDENTIAL_HOPS.
inline QStringList askpassHops(const QJsonArray &jumps)
{
    QStringList hops;
    for (const auto &item : jumps)
    {
        const auto hop = item.toObject();
        const auto host = askpassHost(hop["host"].toString()), user = hop["user"].toString();
        if (!host.isEmpty() && !host.contains(',') && !user.contains(','))
            hops << (user.isEmpty() ? host : user + "@" + host);
    }
    return hops;
}

// "user@host" as OpenSSH names the destination of an ssh command line in its password prompts: after ssh_config
// (Host alias, HostName, User) and the options of the command, read from "ssh -G". Empty when ssh cannot tell
// (too old, a configuration error, no answer in 3 s).
inline QString sshPromptIdentity(const QString &program, const QStringList &arguments)
{
    QProcess ssh;
    hideConsoleWindow(&ssh);
    ssh.setStandardInputFile(QProcess::nullDevice());
    ssh.start(program, QStringList{"-G"} + arguments);
    if (!ssh.waitForFinished(3000))
    {
        ssh.kill();
        ssh.waitForFinished(1000);
        return {};
    }
    if (ssh.exitStatus() != QProcess::NormalExit || ssh.exitCode() != 0)
        return {};
    QString user, host;
    for (const auto &line : ssh.readAllStandardOutput().split('\n'))
    {
        const auto text = QString::fromUtf8(line).trimmed();
        if (text.startsWith("user "))
            user = text.mid(5);
        else if (text.startsWith("hostname "))
            host = text.mid(9);
    }
    return host.isEmpty() ? QString() : user + "@" + askpassHost(host);
}

// A prompt of another host than the session's own: a jump host ("ssh -J" and ProxyJump run "ssh -W" for each hop
// with this same environment) or whatever a ProxyCommand logs in to. target: "<app>:<user>@<host>:<port>".
// identity: the session as ssh_config resolves it (sshPromptIdentity); with it, a prompt that names anyone else is
// foreign, also a jump host that only ssh_config knows. Without it only the listed hops are, and a name that is
// neither counts as the session (it may be a Host alias).
inline bool askpassForeign(const PromptIdentity &who, const QString &target, const QStringList &hops,
                           const QString &identity = {})
{
    if (who.host.isEmpty())
        return false; // the prompt names nobody
    const auto session = target.section(':', 1);
    if (askpassSameHost(session.left(session.lastIndexOf(':')), who))
        return false;
    if (!identity.isEmpty())
        return !askpassSameHost(identity, who);
    return std::any_of(hops.begin(), hops.end(), [&](const QString &hop) { return askpassSameHost(hop, who); });
}

// Session passwords: typed once (in the askpass dialog) and reused by the terminal, SFTP browser,
// Remote monitoring, tunnels and mosh of the same session. They live only in CrossTerm's memory and are
// reachable by the askpass process through a user-private local socket, with the key of its own launch.
class SessionSecrets
{
    // One per ssh/sftp launch (askpassEnvironment): all that the askpass helpers holding its key may ask for.
    struct Grant
    {
        QString target, terminal;
        QStringList hops;
        QString identity; // the session in OpenSSH's prompts (sshPromptIdentity), when known
        bool interactive = true;
        qint64 used = 0;
    };
    static constexpr qint64 GrantLifetime = 30 * 24 * 3600; // seconds without a request
    static constexpr qint64 RequestLimit = 256 * 1024;      // bytes of one request: a few lines, base64 of a prompt

    QLocalServer server;
    QHash<QString, QString> secrets;
    QHash<QString, Grant> grants;
    QHash<QString, QStringList> routes; // jump hosts per target, for the launches that do not pass them
    QHash<QString, QString> names;      // prompt identity per target, likewise

    static QByteArray encode(const QString &text) { return text.toUtf8().toBase64(); }
    static QString decode(const QByteArray &line) { return QString::fromUtf8(QByteArray::fromBase64(line.trimmed())); }

    Grant *granted(const QByteArray &key)
    {
        const auto it = grants.find(QString::fromUtf8(key.trimmed()));
        if (it == grants.end())
            return nullptr;
        const auto now = QDateTime::currentSecsSinceEpoch();
        if (now - it->used > GrantLifetime)
        {
            grants.erase(it);
            return nullptr;
        }
        it->used = now;
        return &*it;
    }
    // Cache entries of a launch: its own password, those of its jump hosts ("hop:user@host") and key passphrases.
    static bool allowed(const Grant &grant, const QString &entry)
    {
        if (entry.startsWith("passphrase:"))
            return true;
        if (!entry.startsWith("hop:"))
            return !entry.isEmpty() && entry == grant.target;
        const auto name = entry.mid(4);
        const auto at = name.lastIndexOf('@');
        const PromptIdentity who{name.left(qMax(at, 0)), askpassHost(name.mid(at + 1))};
        // A launch that knows its own name treats every other host as a hop (askpassForeign).
        if (!grant.identity.isEmpty())
            return !who.host.isEmpty() && !askpassSameHost(grant.identity, who);
        return std::any_of(grant.hops.begin(), grant.hops.end(), [&](const QString &hop) { return askpassSameHost(hop, who); });
    }

public:
    QString name;

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
        name = "crossterm-secrets-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server.listen(name))
            return false;
        // Attempt counters of terminals and tunnels (or of a crash) left over from earlier runs.
        const auto stale = QDateTime::currentDateTime().addDays(-1);
        for (const auto &file : QDir::temp().entryInfoList({"crossterm-askpass-*"}, QDir::Files | QDir::Hidden))
        {
            if (file.lastModified() < stale)
                QFile::remove(file.filePath());
        }
        QObject::connect(&server, &QLocalServer::newConnection, &server,
                         [this]
                         {
                             while (auto *socket = server.nextPendingConnection())
                             {
                                 socket->setReadBufferSize(RequestLimit); // see serve()
                                 QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                                 QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, socket] { serve(socket); });
                             }
                         });
        return true;
    }

    // Key of a new launch, limited to its target, its jump hosts ("user@host" or "host") and its terminal tab.
    QString issue(const QString &target, bool interactive, const QString &terminal, const QStringList &hops,
                  const QString &identity = {})
    {
        const auto now = QDateTime::currentSecsSinceEpoch();
        for (auto it = grants.begin(); it != grants.end();)
            it = now - it->used > GrantLifetime ? grants.erase(it) : std::next(it);
        const auto key = QString::number(QRandomGenerator::system()->generate64(), 16) +
                         QString::number(QRandomGenerator::system()->generate64(), 16);
        grants.insert(key, {target, terminal, hops, identity, interactive, now});
        return key;
    }
    void revoke(const QString &key) { grants.remove(key); }
    // Jump hosts of a target, remembered from the launches that passed them (the terminal) for those that do not.
    QStringList route(const QString &target, const QStringList &hops)
    {
        if (target.isEmpty())
            return hops;
        auto &known = routes[target];
        for (const auto &hop : hops)
        {
            if (!known.contains(hop))
                known << hop;
        }
        return known;
    }
    // Prompt identity of a target, remembered from the launches that resolved it for those that do not.
    QString identity(const QString &target, const QString &resolved)
    {
        if (target.isEmpty() || resolved.isEmpty())
            return target.isEmpty() ? resolved : names.value(target);
        names.insert(target, resolved);
        return resolved;
    }

    // Request: key of the launch, command (GET/PUT/DEL, VGET/VPUT/VDEL, ASK), name, [value]; name and value in
    // base64, one line each. A launch reaches only its own entries and its own terminal tab.
    void serve(QLocalSocket *socket)
    {
        // The read buffer is full and the request still incomplete: it never will be, the client is dropped.
        const bool full = socket->bytesAvailable() >= RequestLimit;
        if (!socket->canReadLine())
            return full ? socket->abort() : void();
        auto all = socket->peek(RequestLimit);
        auto lines = all.split('\n');
        // The request and the reply may carry a secret (base64): wiped once handled.
        auto wipe = qScopeGuard(
            [&]
            {
                all.fill('\0');
                for (auto &line : lines)
                    line.fill('\0');
            });
        const bool withValue = lines.size() > 1 && (lines[1].trimmed() == "PUT" || lines[1].trimmed() == "ASK" ||
                                                    lines[1].trimmed() == "VPUT");
        if (lines.size() < 4 || (withValue && lines.size() < 5))
            return full ? socket->abort() : void(); // incomplete request: wait for more data
        socket->readAll().fill('\0');
        QByteArray reply = "\n";
        if (const auto *grant = granted(lines[0]))
        {
            const auto command = lines[1].trimmed();
            const auto entry = decode(lines[2]);
            const auto target = grant->target;
            if (command == "ASK")
            {
                // entry = terminal id; value = echo flag ('0'/'1') + prompt. Reply "+answer", "-" (cancelled)
                // or "!" (no such terminal: the askpass process falls back to a dialog).
                // Only the tab of this launch; a duplicated tab has a new id that no launch has registered.
                const bool own = grant->interactive && !grant->terminal.isEmpty() &&
                                 (entry == grant->terminal || std::none_of(grants.cbegin(), grants.cend(), [&](const Grant &other)
                                                                           { return other.terminal == entry; }));
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
                if (own && ask && ask(entry, text.mid(1), text.startsWith('1'), answer))
                    return; // answered when the user presses Enter in the terminal
                reply = "!\n";
            }
            else if ((command == "GET" || command == "PUT" || command == "DEL") && !allowed(*grant, entry))
                reply = "\n"; // not an entry of this launch
            else if (command == "GET" && secrets.contains(entry))
                reply = encode(secrets.value(entry)) + "\n";
            else if (command == "PUT")
                put(entry, decode(lines[3]));
            else if (command == "DEL")
                forget(entry);
            else if (command.startsWith('V') && (target.isEmpty() || entry != target))
                reply = "-\n"; // the vault entry of this launch only
            // System vault, read and written by this process for the askpass helpers: macOS then asks for
            // Keychain access once, in the foreground app, instead of once per background helper.
            else if (command == "VGET")
            {
                auto value = readSecret(target);
                reply = value ? "+" + encode(*value) + "\n" : QByteArray("-\n");
                if (value)
                    value->fill(QChar(0));
            }
            else if (command == "VPUT")
            {
                auto secret = decode(lines[3]);
                reply = storeSecret(target, target.section(':', 1).section('@', 0, 0), secret) ? "+\n" : "-\n";
                secret.fill(QChar(0));
            }
            else if (command == "VDEL")
                reply = removeSecret(target) ? "+\n" : "-\n";
        }
        socket->write(reply);
        reply.fill('\0');
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
        if (auto it = secrets.find(entry); it != secrets.end() && *it != secret)
            it->fill(QChar(0)); // replaced: the old password does not stay in memory
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

// Called by the launcher once the process has exited: SFTP starts a new one (new token and key) per operation.
inline void forgetAskpassAttempts(const QProcess *process)
{
    const auto environment = process->processEnvironment();
    const auto token = environment.value("CROSSTERM_ASKPASS_TOKEN");
    if (!token.isEmpty())
        QFile::remove(askpassAttemptsFile(token));
    SessionSecrets::instance().revoke(environment.value("CROSSTERM_ASKPASS_KEY"));
}

// A launch that is over for good (its tab closed, its tunnel or monitor ended), given its environment: the key of
// the launch is revoked and its attempt counter removed.
inline void releaseAskpass(const QStringList &environment)
{
    for (const auto &entry : environment)
    {
        const auto variable = entry.section('=', 0, 0), value = entry.section('=', 1);
        if (variable == "CROSSTERM_ASKPASS_KEY")
            SessionSecrets::instance().revoke(value);
        else if (variable == "CROSSTERM_ASKPASS_TOKEN" && !value.isEmpty())
            QFile::remove(askpassAttemptsFile(value));
    }
}

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

// Askpass side: the system vault, only through the CrossTerm window that launched ssh (which checks the key of the
// launch and may wait for a Keychain prompt). Started by anything else, this program gives out no saved password.
inline std::optional<QString> vaultRead(const QString &target)
{
    auto reply = requestSessionSecrets("VGET", target, {}, 300000);
    if (!reply)
        return std::nullopt;
    std::optional<QString> value;
    if (reply->startsWith('+'))
        value = QString::fromUtf8(QByteArray::fromBase64(reply->mid(1)));
    reply->fill('\0');
    return value;
}
// Writes go through that window too: if it does not answer they fail.
inline bool vaultStore(const QString &target, const QString &secret)
{
    const auto reply = requestSessionSecrets("VPUT", target, secret, 300000);
    return reply && reply->startsWith('+');
}
inline bool vaultRemove(const QString &target)
{
    const auto reply = requestSessionSecrets("VDEL", target, {}, 300000);
    return reply && reply->startsWith('+');
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

// Variables for one ssh/sftp launch: entry name, attempt token, CrossTerm path and the key of this launch.
inline QStringList askpassVariables(const QString &target, bool interactive, const QString &terminal, const QStringList &hops,
                                    const QString &identity = {})
{
    QStringList environment{"SSH_ASKPASS=" + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()),
                            "SSH_ASKPASS_REQUIRE=force", "CROSSTERM_CREDENTIAL=" + target,
                            "CROSSTERM_ASKPASS_TOKEN=" + QUuid::createUuid().toString(QUuid::WithoutBraces)};
    if (!interactive)
        environment << "CROSSTERM_ASKPASS_NONINTERACTIVE=1";
    if (!terminal.isEmpty())
        environment << "CROSSTERM_ASKPASS_TERMINAL=" + terminal;
    if (!hops.isEmpty())
        environment << "CROSSTERM_CREDENTIAL_HOPS=" + hops.join(',');
    if (!identity.isEmpty())
        environment << "CROSSTERM_CREDENTIAL_IDENTITY=" + identity;
    auto &secrets = SessionSecrets::instance();
    if (!secrets.name.isEmpty())
        environment << "CROSSTERM_ASKPASS_SERVER=" + secrets.name
                    << "CROSSTERM_ASKPASS_KEY=" + secrets.issue(target, interactive, terminal, hops, identity);
    return environment;
}

// terminal: id of the terminal tab that shows the prompts inline instead of a dialog.
// jumps: the profile's "jumps"; their password prompts are kept apart from the session's own password.
// identity: sshPromptIdentity() of the launch, when the caller resolved it.
inline QStringList askpassEnvironment(const QString &target, bool interactive, const QString &terminal = {},
                                      const QJsonArray &jumps = {}, const QString &identity = {})
{
    auto &secrets = SessionSecrets::instance();
    return askpassVariables(target, interactive, terminal, secrets.route(target, askpassHops(jumps)),
                            secrets.identity(target, identity));
}

// Restart or copy ("Duplicate tab") of a stored ssh command: a fresh attempt token and key, and for a copy its own
// terminal id. The key of a restarted command is revoked; the original of a copy keeps its own.
inline void renewAskpass(QStringList &environment, bool copy = false)
{
    QString target, terminal, key, identity;
    QStringList hops, others;
    bool interactive = true, found = false;
    for (const auto &entry : environment)
    {
        const auto variable = entry.section('=', 0, 0), value = entry.section('=', 1);
        if (variable == "CROSSTERM_ASKPASS_TOKEN")
            found = true;
        else if (variable == "CROSSTERM_CREDENTIAL")
            target = value;
        else if (variable == "CROSSTERM_CREDENTIAL_HOPS")
            hops = value.split(',', Qt::SkipEmptyParts);
        else if (variable == "CROSSTERM_ASKPASS_TERMINAL")
            terminal = value;
        else if (variable == "CROSSTERM_ASKPASS_KEY")
            key = value;
        else if (variable == "CROSSTERM_CREDENTIAL_IDENTITY")
            identity = value;
        else if (variable == "CROSSTERM_ASKPASS_NONINTERACTIVE")
            interactive = false;
        else if (variable != "SSH_ASKPASS" && variable != "SSH_ASKPASS_REQUIRE" && variable != "CROSSTERM_ASKPASS_SERVER")
            others << entry;
    }
    if (!found)
        return; // no askpass: local shell, telnet, serial
    if (!copy)
        SessionSecrets::instance().revoke(key);
    else if (!terminal.isEmpty())
        terminal = QUuid::createUuid().toString(QUuid::WithoutBraces);
    environment = others + askpassVariables(target, interactive, terminal, hops, identity);
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
        QMessageBox question(QMessageBox::Question, "CrossTerm", prompt.trimmed(), QMessageBox::Yes | QMessageBox::No);
        question.setTextFormat(Qt::PlainText); // the text comes from ssh and, in part, from the server
        writeAnswer(question.exec() == QMessageBox::Yes ? "yes" : "no");
        return 0;
    }
    // A jump host asks for its own password: never the session's saved or typed one, and what is typed for it is
    // cached under its own name (asked once per run) and never saved as the session's password.
    const auto who = promptIdentity(prompt);
    const bool foreign =
        askpassForeign(who, target, qEnvironmentVariable("CROSSTERM_CREDENTIAL_HOPS").split(',', Qt::SkipEmptyParts),
                       qEnvironmentVariable("CROSSTERM_CREDENTIAL_IDENTITY"));
    // Cache entry: the session password, the passphrase of the key named in the prompt (a prompt relayed from the
    // server, "(user@host) ...", is never one) or the jump host's password.
    const bool passphrase = !who.keyboard && prompt.contains("passphrase", Qt::CaseInsensitive);
    const char kind = passphrase ? 'p' : 'c';
    const auto entry = passphrase ? "passphrase:" + prompt.trimmed() : foreign ? "hop:" + who.user + "@" + who.host : target;
    // Attempt counter for this ssh process. Only secret prompts count: a host key question answered first must
    // not turn the first password prompt into a "retry". The first line of the file holds the number, then 'c'
    // (session password) and/or 'p' (key passphrase) once that cached or typed value has been answered to this
    // process; each jump host that asked has its own line, "<name in base64> <number>[c]".
    const auto counter = askpassAttemptsFile(qEnvironmentVariable("CROSSTERM_ASKPASS_TOKEN"));
    QFile file(counter);
    auto states = file.open(QIODevice::ReadOnly) ? file.readAll().split('\n') : QByteArrayList{QByteArray()};
    file.close();
    const auto tag = foreign ? entry.toUtf8().toBase64() + ' ' : QByteArray();
    qsizetype line = 0;
    if (foreign)
    {
        line = 1;
        while (line < states.size() && !states[line].startsWith(tag))
            ++line;
        if (line == states.size())
            states << tag;
    }
    const auto state = states[line].mid(tag.size()).trimmed();
    const int attempt = QByteArray(state).replace('c', "").replace('p', "").toInt() + 1;
    QByteArray answered = state.contains('c') ? "c" : "";
    if (state.contains('p'))
        answered += 'p';
    auto record = [&]
    {
        states[line] = tag + QByteArray::number(attempt) + answered;
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(states.join('\n'));
        file.close();
    };
    record();
    const auto saved = target.isEmpty() || passphrase || foreign ? std::nullopt : vaultRead(target);
    if (attempt == 1 && saved)
    {
        writeAnswer(*saved);
        return 0;
    }
    auto cached = entry.isEmpty() ? QString() : askSessionSecrets("GET", entry);
    // Not answered to this process yet, also when it was typed in the terminal while this process was already
    // asking (SFTP and monitoring start without a password): judging by the attempt number alone discarded it.
    if (!cached.isEmpty() && !answered.contains(kind))
    {
        answered += kind;
        record();
        writeAnswer(cached); // password already typed in this CrossTerm session
        return 0;
    }
    if (!cached.isEmpty())
        askSessionSecrets("DEL", entry); // answered and asked again: rejected by the server, do not offer it again
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
        {
            askSessionSecrets("PUT", entry, *typed.value);
            answered += kind;
            record();
        }
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
    // The vault belongs to the window that launched ssh: nothing to save into without it.
    remember->setVisible(vaultAvailable() && !target.isEmpty() && !passphrase && !foreign &&
                         qEnvironmentVariableIsSet("CROSSTERM_ASKPASS_SERVER"));
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
    {
        askSessionSecrets("PUT", entry, value); // reused by the session's SFTP, monitoring and tunnels
        answered += kind;
        record();
    }
    writeAnswer(value);
    value.fill(QChar(0));
    secret->clear();
    return 0;
}
