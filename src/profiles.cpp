#include "profiles.h"
#include "proxy.h"
#include <algorithm>
#include <QCoreApplication>
#include <QCryptographicHash>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>
#endif
#include <array>
#include <cstring>
#include <string>
#include <thread>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUuid>
#include <QSet>
#include <QMap>

static const QStringList types{"ssh",    "telnet", "rsh",   "xdmcp",   "rdp",  "vnc",    "ftp", "sftp",
                               "serial", "file",   "shell", "browser", "mosh", "rlogin", "wsl"};

int defaultPort(const QString &protocol)
{
    const int ports[]{22, 23, 514, 177, 3389, 5900, 21, 22, 0, 0, 0, 0, 22, 513, 0};
    const int index = types.indexOf(protocol);
    return index < 0 ? 0 : ports[index];
}

bool validHost(const QString &host)
{
    static const QRegularExpression pattern("^(?:[a-zA-Z0-9_][a-zA-Z0-9_.-]{0,252}|(?=[0-9a-fA-F:.]*:)[0-9a-fA-F:.]+)$");
    return pattern.match(host).hasMatch();
}

// --- MobaXterm passwords protected by a master password: key = SHA-512(master)[0..32),
// IV = AES-256-ECB(key, zero block), cipher = AES-256-CFB8, value in base64.
namespace
{
uint8_t xtime(uint8_t x)
{
    return uint8_t((x << 1) ^ ((x >> 7) * 0x1b));
}

const uint8_t *aesSbox()
{
    static const auto table = []
    {
        std::array<uint8_t, 256> s{};
        for (int x = 0; x < 256; ++x)
        {
            uint8_t b = 0; // multiplicative inverse in GF(2^8), then the affine transform
            for (int y = 1; y < 256 && x; ++y)
            {
                uint8_t a = uint8_t(x), m = uint8_t(y), p = 0;
                for (; m; m >>= 1, a = xtime(a))
                    if (m & 1)
                        p ^= a;
                if (p == 1)
                {
                    b = uint8_t(y);
                    break;
                }
            }
            auto rot = [b](int n) { return uint8_t((b << n) | (b >> (8 - n))); };
            s[size_t(x)] = uint8_t(b ^ rot(1) ^ rot(2) ^ rot(3) ^ rot(4) ^ 0x63);
        }
        return s;
    }();
    return table.data();
}

// AES-256, encryption only (CFB uses only the forward cipher).
struct Aes256
{
    uint8_t rk[240];
    explicit Aes256(const uint8_t *key)
    {
        const auto *s = aesSbox();
        memcpy(rk, key, 32);
        uint8_t rcon = 1;
        for (int i = 32; i < 240; i += 4)
        {
            uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
            if (i % 32 == 0)
            {
                const uint8_t t0 = t[0];
                t[0] = uint8_t(s[t[1]] ^ rcon);
                t[1] = s[t[2]];
                t[2] = s[t[3]];
                t[3] = s[t0];
                rcon = xtime(rcon);
            }
            else if (i % 32 == 16)
            {
                for (auto &v : t)
                    v = s[v];
            }
            for (int j = 0; j < 4; ++j)
                rk[i + j] = rk[i + j - 32] ^ t[j];
        }
    }
    void encrypt(uint8_t *b) const
    {
        const auto *s = aesSbox();
        for (int i = 0; i < 16; ++i)
            b[i] ^= rk[i];
        for (int r = 1; r <= 14; ++r)
        {
            uint8_t t[16];
            for (int i = 0; i < 16; ++i) // SubBytes + ShiftRows (column-major state)
                t[i] = s[b[(i + 4 * (i % 4)) % 16]];
            if (r < 14)
            {
                for (int c = 0; c < 16; c += 4) // MixColumns
                {
                    const uint8_t a0 = t[c], a1 = t[c + 1], a2 = t[c + 2], a3 = t[c + 3], x = a0 ^ a1 ^ a2 ^ a3;
                    t[c] ^= x ^ xtime(a0 ^ a1);
                    t[c + 1] ^= x ^ xtime(a1 ^ a2);
                    t[c + 2] ^= x ^ xtime(a2 ^ a3);
                    t[c + 3] ^= x ^ xtime(a3 ^ a0);
                }
            }
            for (int i = 0; i < 16; ++i)
                b[i] = t[i] ^ rk[16 * r + i];
        }
    }
};

QByteArray mobaCfb8(const QByteArray &input, const QString &master, bool decrypt)
{
    auto digest = QCryptographicHash::hash(master.toUtf8(), QCryptographicHash::Sha512);
    const Aes256 aes(reinterpret_cast<const uint8_t *>(digest.constData()));
    digest.fill('\0');
    uint8_t shift[16]{};
    aes.encrypt(shift); // IV
    QByteArray output(input.size(), '\0');
    for (qsizetype i = 0; i < input.size(); ++i)
    {
        uint8_t block[16];
        memcpy(block, shift, 16);
        aes.encrypt(block);
        const auto in = uint8_t(input[i]), out = uint8_t(in ^ block[0]);
        output[i] = char(out);
        memmove(shift, shift + 1, 15);
        shift[15] = decrypt ? in : out; // CFB feeds back the ciphertext byte
    }
    return output;
}
} // namespace

QByteArray aes256EncryptBlock(const QByteArray &key32, const QByteArray &block16)
{
    QByteArray out = block16;
    Aes256(reinterpret_cast<const uint8_t *>(key32.constData())).encrypt(reinterpret_cast<uint8_t *>(out.data()));
    return out;
}

QString encryptMobaPassword(const QString &plain, const QString &master)
{
    return QString::fromLatin1(mobaCfb8(plain.toUtf8(), master, false).toBase64());
}

std::optional<QString> decryptMobaPassword(const QString &base64, const QString &master)
{
    const auto bytes = mobaCfb8(QByteArray::fromBase64(base64.trimmed().toLatin1()), master, true);
    if (bytes.isEmpty())
        return std::nullopt;
    // A wrong master password yields random bytes: accept only printable text (UTF-8 or ANSI).
    auto text = QString::fromUtf8(bytes);
    if (text.contains(QChar::ReplacementCharacter))
        text = QString::fromLatin1(bytes);
    for (const auto c : text)
    {
        if (!c.isPrint())
            return std::nullopt;
    }
    return text;
}

QProcess *hideConsoleWindow(QProcess *process)
{
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *arguments)
        {
            arguments->flags |= CREATE_NEW_CONSOLE;
            arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            arguments->startupInfo->wShowWindow = SW_HIDE;
        });
#elif defined(Q_OS_LINUX)
    // The child ends with CrossTerm also when CrossTerm is killed (SIGKILL), which the process group cleanup cannot
    // catch. Only async-signal-safe calls: this runs in the child between fork and exec.
    const pid_t parent = getpid();
    process->setChildProcessModifier(
        [parent]
        {
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (getppid() != parent) // CrossTerm died before prctl
                _exit(1);
        });
#endif
    return process;
}

bool isHiddenShellInvocation(int argc)
{
    return argc >= 2 && qEnvironmentVariableIsSet("CROSSTERM_HIDDEN_SHELL");
}

int runHiddenShell()
{
#ifdef Q_OS_WIN
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring line = L"\"" + std::wstring(system) + L"\\cmd.exe\"";
    // Everything after our own program name goes to cmd unchanged (quotes and redirections included).
    const std::wstring self = GetCommandLineW();
    const size_t rest = !self.empty() && self[0] == L'"' ? self.find(L'"', 1) + 1 : self.find(L' ');
    if (rest != std::wstring::npos && rest < self.size())
        line += self.substr(rest);
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    HPCON console = nullptr;
    if (!CreatePipe(&inRead, &inWrite, nullptr, 0) || !CreatePipe(&outRead, &outWrite, nullptr, 0) ||
        FAILED(CreatePseudoConsole({120, 30}, inRead, outWrite, 0, &console)))
        return 1;
    // The pseudoconsole writes its screen to outRead: drain it so it never blocks.
    std::thread(
        [outRead]
        {
            char buffer[4096];
            DWORD n = 0;
            while (ReadFile(outRead, buffer, sizeof buffer, &n, nullptr) && n)
            {
            }
        })
        .detach();
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::string memory(size, '\0');
    STARTUPINFOEXW info{};
    info.StartupInfo.cb = sizeof info;
    info.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
    PROCESS_INFORMATION child{};
    if (!InitializeProcThreadAttributeList(info.lpAttributeList, 1, 0, &size) ||
        !UpdateProcThreadAttribute(info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof console, nullptr,
                                   nullptr) ||
        !CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                        &info.StartupInfo, &child))
        return 1;
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    DeleteProcThreadAttributeList(info.lpAttributeList);
    ClosePseudoConsole(console);
    return int(code);
#else
    return 1;
#endif
}

static QString decode(const QByteArray &bytes)
{
    if (bytes.startsWith("\xff\xfe") || bytes.startsWith("\xfe\xff"))
        return QStringDecoder(QStringDecoder::Utf16).decode(bytes);
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString value = utf8.decode(bytes);
    if (!utf8.hasError())
        return value;
    value = QString::fromLatin1(bytes);
    const QString substitutions = QString::fromUtf8("€\u0081‚ƒ„…†‡ˆ‰Š‹Œ\u008dŽ\u008f\u0090‘’“”•–—˜™š›œ\u009džŸ");
    for (auto &c : value)
    {
        if (c.unicode() >= 128 && c.unicode() <= 159)
            c = substitutions.at(c.unicode() - 128);
    }
    return value;
}

static QString hostValue(QString host)
{
    host = host.trimmed();
    if (host.startsWith('[') && host.endsWith(']'))
        return host.mid(1, host.size() - 2);
    return host;
}

static QString bracket(const QString &host)
{
    return host.contains(':') ? "[" + host + "]" : host;
}

static bool flag(const QString &value)
{
    return value == "-1" || value == "1";
}
static bool option(const QString &value)
{
    return !value.isEmpty() && value != "0";
}
static QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
static bool safeUser(const QString &user)
{
    static const QRegularExpression pattern("^(?:[a-zA-Z0-9_][a-zA-Z0-9_.@\\\\-]{0,127})?$");
    return pattern.match(user).hasMatch();
}
static QString readUser(QString user, QJsonArray &issues, const QMap<QString, QString> &accounts)
{
    if (user.startsWith('[') && user.endsWith(']'))
    {
        const auto account = accounts.value(user.mid(1, user.size() - 2).toCaseFolded());
        if (!account.isEmpty())
            return account;
    }
    if (user.contains('['))
    {
        issues.append("Sostituisci il riferimento alla credenziale con il nome utente remoto.");
        return {};
    }
    if (user.compare("<default>", Qt::CaseInsensitive) == 0)
        return {};
    if (!safeUser(user))
        issues.append("Nome utente da verificare.");
    return user;
}

Import parseMoba(const QByteArray &bytes, const QString &drive)
{
    static const QRegularExpression lines("[\r\n]+");
    static const QRegularExpression section("^\\[([^\\]]+)\\]\\s*(?:;.*)?$");
    static const QRegularExpression bookmarkSection("^Bookmarks(?:_\\d+)?$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bookmark("^(?:;\\s*logout\\s*)?#-?\\d+#(\\d+(?:%[^#]*)?)(?:#|$)");
    const auto rows = decode(bytes).split(lines);
    bool inside = true;
    for (const auto &line : rows)
    {
        if (section.match(line.trimmed()).hasMatch())
        {
            inside = false;
            break;
        }
    }
    QMap<QString, QString> accounts;
    QHash<QString, QString> protectedAliases, protectedEndpoints; // name -> encrypted password
    bool credentials = false, passwords = false;
    int accountCount = 0, secretCount = 0;
    for (const auto &row : rows)
    {
        const auto heading = section.match(row.trimmed());
        if (heading.hasMatch())
        {
            credentials = heading.captured(1).compare("Credentials", Qt::CaseInsensitive) == 0;
            passwords = heading.captured(1).compare("Passwords", Qt::CaseInsensitive) == 0;
            continue;
        }
        const int eq = row.indexOf('=');
        if (passwords && eq > 0 && !row.mid(eq + 1).trimmed().isEmpty())
        {
            ++secretCount;
            protectedEndpoints.insert(row.left(eq).trimmed().toCaseFolded(), row.mid(eq + 1).trimmed());
        }
        if (!credentials)
            continue;
        const int colon = row.indexOf(':', eq + 1);
        if (eq <= 0 || colon <= eq)
            continue;
        ++accountCount;
        if (!row.mid(colon + 1).trimmed().isEmpty())
        {
            ++secretCount;
            protectedAliases.insert(row.left(eq).trimmed().toCaseFolded(), row.mid(colon + 1).trimmed());
        }
        const auto user = row.mid(eq + 1, colon - eq - 1).trimmed();
        if (!user.isEmpty() && safeUser(user))
            accounts.insert(row.left(eq).trimmed().toCaseFolded(), user);
    }
    Import result;
    result.accounts = accountCount;
    result.secrets = secretCount;
    QString folder;
    QJsonArray pending;
    auto flush = [&]
    {
        for (const auto &entry : pending)
        {
            auto profile = entry.toObject();
            profile["folder"] = folder;
            result.profiles.append(profile);
        }
        pending = {};
        folder.clear();
    };
    for (const auto &row : rows)
    {
        const auto line = row.trimmed();
        if (line.isEmpty() || line.startsWith(';') || line.startsWith('#'))
            continue;
        const auto heading = section.match(line);
        if (heading.hasMatch())
        {
            flush();
            inside = bookmarkSection.match(heading.captured(1)).hasMatch();
            continue;
        }
        if (!inside)
            continue;
        const int equals = line.indexOf('=');
        const auto name = line.left(equals).trimmed();
        const auto value = line.mid(equals + 1).trimmed();
        if (name.compare("SubRep", Qt::CaseInsensitive) == 0 && equals > 0)
        {
            folder = value;
            continue;
        }
        if (name.compare("ImgNum", Qt::CaseInsensitive) == 0 && equals > 0)
            continue;
        const auto match = bookmark.match(value);
        if (!match.hasMatch() || equals <= 0)
        {
            result.warnings.append("Una voce di sessione ha un formato non riconosciuto.");
            continue;
        }
        const auto fields = match.captured(1).split('%');
        bool validType = false;
        const auto type = fields[0].toInt(&validType);
        const auto protocol = validType ? types.value(type, "unsupported") : QString("unsupported");
        const auto host = hostValue(fields.value(1));
        const auto rawPort = fields.value(2);
        const int port = rawPort.isEmpty() ? defaultPort(protocol) : rawPort.toInt();
        QJsonArray issues, jumps;
        QString user, identity;
        if (protocol == "ssh" || protocol == "sftp" || protocol == "rdp" || protocol == "ftp")
            user = readUser(protocol == "ssh" && flag(fields.value(12)) ? QString{} : fields.value(3), issues, accounts);
        const bool ssh = protocol == "ssh";
        const bool sftp = protocol == "sftp";
        if (ssh || sftp)
        {
            identity = fields.value(ssh ? 14 : 9);
            if (identity.startsWith("_CurrentDrive_") && !drive.isEmpty())
            {
                auto tail = identity.mid(14);
                if (tail.startsWith(':'))
                    tail.remove(0, 1);
                identity = drive + ":" + tail;
            }
            if (identity.startsWith("_CurrentDrive_"))
                issues.append("Seleziona la chiave privata locale.");
        }
        if (ssh && !fields.value(8).isEmpty())
        {
            const auto hosts = fields.value(8).split("__PIPE__");
            const auto ports = fields.value(9).split("__PIPE__");
            const auto users = fields.value(10).split("__PIPE__");
            for (int i = 0; i < hosts.size(); ++i)
            {
                jumps.append(QJsonObject{{"host", hostValue(hosts[i])},
                                         {"port", ports.value(i, "22").isEmpty() ? 22 : ports.value(i, "22").toInt()},
                                         {"user", readUser(users.value(i), issues, accounts)}});
            }
        }
        if ((ssh &&
             (option(fields.value(19)) || !fields.value(7).isEmpty() || option(fields.value(13)) || !fields.value(15).isEmpty() ||
              fields.value(27) == "2" || !fields.value(28).isEmpty() || !fields.value(29).isEmpty() ||
              !fields.value(30).isEmpty() || flag(fields.value(31)) || fields.value(33) == "0" || flag(fields.value(34)) ||
              !fields.value(35).isEmpty() || !fields.value(36).isEmpty() || flag(fields.value(37)))) ||
            (sftp && (option(fields.value(10)) || fields.value(4) == "0" || !fields.value(6).isEmpty() || flag(fields.value(7)) ||
                      !fields.value(15).isEmpty())) ||
            (protocol == "rdp" && (!fields.value(12).isEmpty() || !fields.value(13).isEmpty() || !fields.value(20).isEmpty() ||
                                   option(fields.value(30)))) ||
            (protocol == "vnc" && (flag(fields.value(4)) || !fields.value(5).isEmpty() || flag(fields.value(11)) ||
                                   !fields.value(12).isEmpty() || option(fields.value(13)))) ||
            (protocol == "telnet" && (!fields.value(6).isEmpty() || option(fields.value(11)))))
            issues.append("Rivedi le opzioni avanzate di connessione nel client.");
        if (defaultPort(protocol) && (!validHost(host) || port < 1 || port > 65535))
            issues.append("Indirizzo o porta da verificare.");
        const auto id = uuid();
        const auto reference = fields.value(3).trimmed();
        const auto endpoint = (user + "@" + host).toCaseFolded();
        if ((ssh || sftp) && !user.isEmpty())
        {
            // [Credentials] alias referenced as "[name]", or [Passwords] keyed by endpoint: "user@host",
            // "ssh22:user@host" (protocol + port) and the older "ssh:user@host[:port]".
            QString secret;
            if (reference.startsWith('[') && reference.endsWith(']'))
                secret = protectedAliases.value(reference.mid(1, reference.size() - 2).toCaseFolded());
            for (const auto &key : {protocol + QString::number(port) + ":" + endpoint, protocol + ":" + endpoint,
                                    protocol + ":" + endpoint + ":" + QString::number(port), endpoint})
            {
                if (secret.isEmpty())
                    secret = protectedEndpoints.value(key);
            }
            if (!secret.isEmpty())
            {
                result.passwordProfiles.append(id);
                result.encryptedPasswords.insert(id, secret);
            }
        }
        pending.append(QJsonObject{{"id", id},
                                   {"name", name},
                                   {"protocol", protocol},
                                   {"host", defaultPort(protocol) ? host : QString{}},
                                   {"port", port},
                                   {"user", user},
                                   {"identity", identity},
                                   {"jumps", jumps},
                                   {"compression", flag(fields.value(ssh    ? 6
                                                                     : sftp ? 5
                                                                            : 99))},
                                   {"x11", ssh && flag(fields.value(5))},
                                   {"issues", issues}});
    }
    flush();
    if (result.profiles.isEmpty())
        result.warnings.append("Nessuna sessione MobaXterm riconosciuta.");
    return result;
}

QJsonArray mergeProfiles(QJsonArray current, const QJsonArray &incoming)
{
    auto key = [](const QJsonValue &value)
    {
        const auto p = value.toObject();
        return QJsonDocument(QJsonArray{p["name"], p["folder"], p["protocol"], p["host"], p["port"], p["user"]})
            .toJson(QJsonDocument::Compact);
    };
    // Same key without the user: sessions with no user that an imported one with a user can complete.
    auto endpoint = [](const QJsonObject &p)
    {
        return QJsonDocument(QJsonArray{p["name"], p["folder"], p["protocol"], p["host"], p["port"]})
            .toJson(QJsonDocument::Compact);
    };
    QSet<QByteArray> seen;
    QHash<QByteArray, QList<int>> unnamed; // indexes in current, in order: no full scan per imported session
    for (int i = 0; i < current.size(); ++i)
    {
        seen.insert(key(current[i]));
        if (const auto p = current[i].toObject(); p["user"].toString().isEmpty())
            unnamed[endpoint(p)].append(i);
    }
    for (const auto &item : incoming)
    {
        const auto imported = item.toObject();
        bool repaired = false;
        auto candidates = imported["user"].toString().isEmpty() ? unnamed.end() : unnamed.find(endpoint(imported));
        if (candidates != unnamed.end())
        {
            for (int k = 0; k < candidates->size(); ++k)
            {
                const int i = candidates->at(k);
                auto previous = current[i].toObject();
                QJsonArray issues;
                for (const auto &issue : previous["issues"].toArray())
                {
                    if (issue.toString() != "Sostituisci il riferimento alla credenziale con il nome utente remoto.")
                        issues.append(issue);
                }
                if (issues.size() == previous["issues"].toArray().size())
                    continue;
                previous["user"] = imported["user"];
                previous["issues"] = issues;
                current.replace(i, previous);
                seen.insert(key(previous));
                candidates->removeAt(k); // it has a user now
                repaired = true;
                break;
            }
        }
        if (repaired)
            continue;
        const auto id = key(item);
        if (!seen.contains(id))
        {
            current.append(item);
            seen.insert(id);
            if (imported["user"].toString().isEmpty())
                unnamed[endpoint(imported)].append(int(current.size() - 1));
        }
    }
    return current;
}

Command sessionCommand(const QJsonObject &profile, const QString &platform, bool display)
{
    const auto protocol = profile["protocol"].toString();
    if (protocol == "wsl")
    {
        const auto distro = profile["host"].toString().trimmed();
        if (platform != "windows" || distro.isEmpty() || distro.startsWith('-') || distro.contains(QChar(0)))
            return {{}, {}, "Distribuzione WSL non valida o non disponibile su questa piattaforma."};
        return {"wsl.exe", {"--distribution", distro}, {}};
    }
    const auto host = hostValue(profile["host"].toString());
    const auto user = profile["user"].toString();
    const auto key = profile["identity"].toString();
    const int port = profile["port"].toInt();
    if (!profile["issues"].toArray().isEmpty())
        return {{}, {}, "Rivedi le impostazioni importate."};
    // A key on a network share (a UNC path) makes Windows log in to that server just to read it: a session file
    // from someone else could name their server. A mapped drive letter works.
    const bool remoteKey = platform == "windows" && (key.startsWith("\\\\") || key.startsWith("//"));
    if (!validHost(host) || port < 1 || port > 65535 || !safeUser(user) || key.startsWith('-') || remoteKey ||
        key.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))
        return {{}, {}, "Host, porta, utente o percorso della chiave non valido."};
    const auto destination = bracket(host) + ":" + QString::number(port);
    if (protocol != "ssh" && protocol != "sftp" &&
        (!profile["jumps"].toArray().isEmpty() || !key.isEmpty() || profile["x11"].toBool() || profile["compression"].toBool()))
        return {{}, {}, "Le opzioni SSH richiedono un profilo SSH o SFTP."};
    if (protocol == "rdp")
    {
        if (platform == "windows")
            return {"mstsc.exe", {"/v:" + destination}, {}};
        QStringList args{"/v:" + destination};
        if (!user.isEmpty())
            args << "/u:" + user;
        return {"xfreerdp", args, {}};
    }
    if (protocol == "vnc")
        return {"vncviewer", {bracket(host) + "::" + QString::number(port)}, {}};
    if (protocol == "telnet")
        return {"telnet", {host, QString::number(port)}, {}};
    if (protocol == "ftp")
    {
        const auto endpoint = "ftp://" + destination;
        QStringList args;
        if (!user.isEmpty())
            args << "-u" << user;
        return {"lftp", args << endpoint, {}};
    }
    if (protocol != "ssh" && protocol != "sftp")
        return {{}, {}, "Protocollo ancora da implementare."};
    const bool ssh = protocol == "ssh";
    QStringList args{ssh ? "-p" : "-P", QString::number(port)};
    if (!user.isEmpty())
        args << (ssh ? "-l" : "-o") << (ssh ? user : "User=" + user);
    if (!key.isEmpty())
        args << "-i" << key;
    QStringList hops;
    for (const auto &item : profile["jumps"].toArray())
    {
        const auto hop = item.toObject();
        const auto hopHost = hop["host"].toString();
        const auto hopUser = hop["user"].toString();
        const int hopPort = hop["port"].toInt();
        if (!validHost(hopHost) || !safeUser(hopUser) || hopUser.contains('@') || hopUser.contains('\\') || hopPort < 1 ||
            hopPort > 65535)
            return {{}, {}, "Gateway SSH non valido."};
        hops << (hopUser.isEmpty() ? QString{} : hopUser + "@") + bracket(hopHost) + ":" + QString::number(hopPort);
    }
    if (!hops.isEmpty())
        args << "-J" << hops.join(',');
    if (profile["compression"].toBool())
        args << "-C";
    // Keepalive for SSH sessions, the SFTP browser and monitoring: a crashed server or a lost network ends the session
    // within about 30 s ("Timeout, server not responding") instead of leaving it frozen. A host that does not answer
    // fails after 20 s instead of the system TCP timeout (over a minute).
    args << "-o" << "ServerAliveInterval=10" << "-o" << "ServerAliveCountMax=3" << "-o" << "ConnectTimeout=20";
    // X11: on Windows ssh has no xauth for "untrusted" cookies, so use -Y (trusted) with the local X server.
    // On macOS -X makes ssh generate a cookie by connecting to the display, which starts XQuartz at every login;
    // -Y only reads the existing cookie, so XQuartz starts when a remote X program actually opens a window.
    if (ssh && display && profile["x11"].toBool())
        args << (platform == "linux" ? "-X" : "-Y");
    // HTTP/SOCKS proxy: CrossTerm itself acts as the ProxyCommand (ssh ignores it when jump hosts are set).
    const auto proxyType = profile["proxyType"].toString();
    if (!proxyType.isEmpty() && proxyType != "none")
    {
        const auto proxy = proxyArguments(proxyType, profile["proxyHost"].toString(), profile["proxyPort"].toInt(),
                                          QCoreApplication::applicationFilePath());
        if (proxy.isEmpty())
            return {{}, {}, "Proxy non valido."};
        args << proxy;
    }
    args << "--" << (ssh ? host : bracket(host));
    return {protocol, args, {}};
}

QString sshExitError(int code, const QString &screen, const QString &host)
{
    if (code >= 0 && code != 255)
        return {};
    const auto lines = screen.split('\n', Qt::SkipEmptyParts);
    const auto tail = lines.mid(std::max<qsizetype>(0, lines.size() - 4)).join('\n');
    // OpenSSH messages of a connection that never came up (resolution, refused, unreachable, authentication).
    static const char *const notConnected[]{"connect to host",
                                            "Could not resolve hostname",
                                            "Permission denied",
                                            "Host key verification failed",
                                            "Too many authentication failures",
                                            "kex_exchange_identification",
                                            "Unable to negotiate",
                                            "banner exchange"};
    for (const auto *text : notConnected)
    {
        if (tail.contains(QLatin1String(text), Qt::CaseInsensitive))
            return QCoreApplication::translate("CrossTerm", "Could not connect to %1").arg(host);
    }
    if (tail.contains("closed by remote host") || tail.contains("Connection closed by") || tail.contains("Received disconnect"))
        return QCoreApplication::translate("CrossTerm", "Connection closed by the server");
    return QCoreApplication::translate("CrossTerm", "Connection to %1 lost").arg(host); // timeout, broken pipe, reset
}

bool parseListing(const QString &line, RemoteEntry &entry)
{
    // The link count is optional: some SFTP servers omit it from the longname. Windows OpenSSH formats the
    // listing itself against an OpenSSH server: "drwx******    ? root root 4096 Nov 13 18:47 docs"
    // ('*' for group/other bits it does not show, '?' for the unknown link count).
    static const QRegularExpression pattern(
        "^([-dlbcps][-rwxsStT*]{9})\\S*(?:\\s+[\\d?]+)?\\s+(\\S+)\\s+(\\S+)\\s+(\\d+)\\s+(\\S+\\s+\\d+\\s+[\\d:]+)\\s(.+)$");
    const auto m = pattern.match(line);
    if (!m.hasMatch())
        return false;
    entry.access = m.captured(1);
    entry.owner = m.captured(2);
    entry.group = m.captured(3);
    entry.size = m.captured(4).toLongLong();
    entry.modified = m.captured(5).simplified();
    entry.name = m.captured(6);
    entry.dir = entry.access.startsWith('d');
    entry.link = entry.access.startsWith('l');
    if (entry.link && entry.name.contains(" -> "))
        entry.name = entry.name.section(" -> ", 0, 0);
    return entry.name != "." && entry.name != "..";
}

QJsonArray applyImport(QJsonArray current, const QJsonArray &incoming, ImportPolicy policy)
{
    if (policy == ImportPolicy::Merge)
        return mergeProfiles(current, incoming);
    auto identity = [](const QJsonObject &p)
    { return (p["name"].toString() + "\n" + p["folder"].toString() + "\n" + p["protocol"].toString()).toCaseFolded(); };
    QHash<QString, int> first; // identity -> first index in current
    for (int i = int(current.size()) - 1; i >= 0; --i)
        first.insert(identity(current[i].toObject()), i);
    for (const auto &value : incoming)
    {
        const auto item = value.toObject();
        const auto id = identity(item);
        const int found = first.value(id, -1);
        if (found < 0)
        {
            first.insert(id, int(current.size()));
            current.append(item);
        }
        else if (policy == ImportPolicy::Replace)
        {
            auto replaced = item;
            replaced["id"] = current[found].toObject()["id"];
            current.replace(found, replaced);
        }
    }
    return current;
}

QRect fitToArea(QRect window, const QRect &area)
{
    if (area.isEmpty())
        return window;
    window.setWidth(std::min(window.width(), area.width()));
    window.setHeight(std::min(window.height(), area.height()));
    window.moveLeft(std::clamp(window.left(), area.left(), area.right() - window.width() + 1));
    window.moveTop(std::clamp(window.top(), area.top(), area.bottom() - window.height() + 1));
    return window;
}

// Messages stay stored in profiles as Italian identifiers (compatibility with existing data);
// here they become the English source text, translated into the current language.
QString describe(const QString &message)
{
    static const QHash<QString, const char *> sources{
        {"Sostituisci il riferimento alla credenziale con il nome utente remoto.",
         "Replace the credential reference with the remote user name."},
        {"Nome utente da verificare.", "Check the user name."},
        {"Una voce di sessione ha un formato non riconosciuto.", "A session entry has an unrecognized format."},
        {"Seleziona la chiave privata locale.", "Select the local private key."},
        {"Rivedi le opzioni avanzate di connessione nel client.", "Review the advanced connection options."},
        {"Indirizzo o porta da verificare.", "Check the address or port."},
        {"Nessuna sessione MobaXterm riconosciuta.", "No MobaXterm session recognized."},
        {"Rivedi le impostazioni importate.", "Review the imported settings."},
        {"Host, porta, utente o percorso della chiave non valido.", "Invalid host, port, user or key path."},
        {"Le opzioni SSH richiedono un profilo SSH o SFTP.", "SSH options require an SSH or SFTP session."},
        {"Protocollo ancora da implementare.", "This protocol is not supported yet."},
        {"Gateway SSH non valido.", "Invalid SSH gateway."},
        {"Proxy non valido.", "Invalid proxy: check the type, host and port."},
        {"Tunnel non valido.", "Invalid tunnel: check the ports and the destination host."},
    };
    const auto source = sources.value(message);
    return source ? QCoreApplication::translate("CrossTerm", source) : message;
}

Command tunnelCommand(const QJsonObject &tunnel, const QJsonObject &profile, const QString &platform)
{
    auto ssh = profile;
    ssh["protocol"] = "ssh";
    ssh["x11"] = false;
    auto command = sessionCommand(ssh, platform, false);
    if (!command.error.isEmpty())
        return command;
    const auto type = tunnel["type"].toString();
    const int listen = tunnel["listenPort"].toInt(), port = tunnel["destPort"].toInt();
    const auto host = tunnel["destHost"].toString().trimmed();
    if (listen < 1 || listen > 65535 || (type != "dynamic" && (port < 1 || port > 65535 || !validHost(host))))
        return {{}, {}, "Tunnel non valido."};
    const auto target = (host.contains(':') ? "[" + host + "]" : host) + ":" + QString::number(port);
    QStringList forward;
    if (type == "local")
        forward = {"-L", "127.0.0.1:" + QString::number(listen) + ":" + target};
    else if (type == "remote")
        forward = {"-R", QString::number(listen) + ":" + target};
    else if (type == "dynamic")
        forward = {"-D", "127.0.0.1:" + QString::number(listen)};
    else
        return {{}, {}, "Tunnel non valido."};
    // -N: no shell; ExitOnForwardFailure: fail immediately if the port is not available.
    const int at = int(command.arguments.indexOf("--"));
    command.arguments = command.arguments.mid(0, at) +
                        QStringList{"-N", "-o", "ExitOnForwardFailure=yes", "-o", "StrictHostKeyChecking=yes"} + forward +
                        command.arguments.mid(at);
    return command;
}

static QStringList csvFields(const QString &line, QChar separator)
{
    QStringList fields;
    QString field;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i)
    {
        const QChar c = line[i];
        if (quoted)
        {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"')
                field += '"', ++i;
            else if (c == '"')
                quoted = false;
            else
                field += c;
        }
        else if (c == '"')
            quoted = true;
        else if (c == separator)
            fields << field.trimmed(), field.clear();
        else
            field += c;
    }
    fields << field.trimmed();
    return fields;
}

Import parseCsv(const QByteArray &bytes)
{
    Import result;
    auto text = decode(bytes); // also UTF-16 and ANSI (Excel exports)
    if (text.startsWith(QChar(0xfeff)))
        text.remove(0, 1);
    const auto lines = text.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts);
    if (lines.isEmpty())
    {
        result.warnings << "Nessuna sessione MobaXterm riconosciuta.";
        return result;
    }
    const QChar separator = lines[0].count(';') > lines[0].count(',') ? ';' : ',';
    const auto header = csvFields(lines[0].toLower(), separator);
    const int name = int(header.indexOf("name")), host = int(header.indexOf("host"));
    if (name < 0 || host < 0)
    {
        result.warnings << "Una voce di sessione ha un formato non riconosciuto.";
        return result;
    }
    const int protocol = int(header.indexOf("protocol")), port = int(header.indexOf("port")), user = int(header.indexOf("user")),
              folder = int(header.indexOf("folder"));
    for (int i = 1; i < lines.size(); ++i)
    {
        const auto f = csvFields(lines[i], separator);
        const auto kind = (protocol >= 0 ? f.value(protocol) : QString("ssh")).toLower();
        const auto address = f.value(host).trimmed();
        const int number = port >= 0 && !f.value(port).isEmpty() ? f.value(port).toInt() : defaultPort(kind);
        QJsonArray issues;
        if (!defaultPort(kind))
            issues.append("Protocollo ancora da implementare.");
        else if (!validHost(address) || number < 1 || number > 65535)
            issues.append("Indirizzo o porta da verificare.");
        if (f.value(name).isEmpty())
        {
            result.warnings << "Una voce di sessione ha un formato non riconosciuto.";
            continue;
        }
        result.profiles.append(QJsonObject{{"id", uuid()},
                                           {"name", f.value(name)},
                                           {"folder", folder >= 0 ? f.value(folder) : QString()},
                                           {"protocol", kind},
                                           {"host", address},
                                           {"port", number},
                                           {"user", user >= 0 ? f.value(user) : QString()},
                                           {"identity", QString()},
                                           {"jumps", QJsonArray{}},
                                           {"compression", false},
                                           {"x11", false},
                                           {"issues", issues}});
    }
    return result;
}

QString portableRoot(const QString &executableDir, const QStringList &arguments)
{
    if (arguments.contains("--portable") || QFileInfo::exists(QDir(executableDir).filePath("CrossTerm.portable")))
        return QDir(executableDir).filePath("CrossTerm-data");
    return {};
}

QString sftpQuote(QString text)
{
    if (text.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))
        return {};
    if (text.startsWith('-')) // otherwise sftp reads the name as flags ("rm -a": invalid flag, "get -r x": recursive)
        text.prepend("./");
    return "\"" + text.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
}
