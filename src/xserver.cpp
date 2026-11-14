#include "xserver.h"
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>

static QString ui(const char *text)
{
    return QCoreApplication::translate("CrossTerm", text);
}

QList<XServerInfo> detectXServers(const QStringList &extraDirectories)
{
#ifndef Q_OS_WIN
    Q_UNUSED(extraDirectories)
#ifdef Q_OS_MACOS
    return {{"XQuartz", {}}};
#else
    return {{"DISPLAY " + qEnvironmentVariable("DISPLAY"), {}}};
#endif
#else
    QStringList dirs;
    if (QCoreApplication::instance())
        dirs << QCoreApplication::applicationDirPath() + "/xserver";
    dirs << extraDirectories;
    for (const auto *variable : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"})
    {
        const auto root = qEnvironmentVariable(variable);
        if (!root.isEmpty())
            dirs << root + "/VcXsrv" << root + "/Xming";
    }
    dirs << "C:/cygwin64/bin" << "C:/cygwin/bin" << qEnvironmentVariable("LOCALAPPDATA") + "/Microsoft/WindowsApps"; // X410
    dirs << qEnvironmentVariable("PATH").split(';', Qt::SkipEmptyParts);
    static const QList<XServerInfo> kinds{
        {"VcXsrv", "vcxsrv.exe"}, {"Xming", "Xming.exe"}, {"Cygwin/X", "XWin.exe"}, {"X410", "X410.exe"}};
    QList<XServerInfo> found;
    QSet<QString> seen;
    for (const auto &dir : dirs)
    {
        for (const auto &kind : kinds)
        {
            const QFileInfo file(QDir(dir).filePath(kind.executable));
            const auto path = QDir::toNativeSeparators(file.absoluteFilePath());
            if (file.isFile() && !seen.contains(path.toLower()))
            {
                seen.insert(path.toLower());
                found.append({kind.name, path});
            }
        }
    }
    return found;
#endif
}

Command xserverCommand(const XServerInfo &server, int display, const QString &mode, const QString &xdmcpHost)
{
    const auto exe = QFileInfo(server.executable).fileName().toLower();
    const bool xdmcp = mode == "xdmcp";
    if (server.executable.isEmpty())
        return {{}, {}, ui("The native X display is used: nothing to start.")};
    if (display < 0 || display > 99 || !QStringList{"multiwindow", "rootless", "fullscreen", "xdmcp", "broadcast"}.contains(mode))
        return {{}, {}, ui("Invalid X display or mode.")};
    if (xdmcp && !validHost(xdmcpHost))
        return {{}, {}, ui("Invalid XDMCP host.")};
    if (exe == "x410.exe")
    {
        // X410 (Microsoft Store) always uses :0 and does not support XDMCP.
        if (display != 0 || xdmcp || mode == "broadcast")
            return {{}, {}, ui("X410 supports only display :0 without XDMCP.")};
        return {server.executable, {mode == "fullscreen" ? "/desktop" : "/wm"}, {}};
    }
    if (exe != "vcxsrv.exe" && exe != "xming.exe" && exe != "xwin.exe")
        return {{}, {}, ui("Unknown X server.")};
    QStringList args{":" + QString::number(display)};
    if (xdmcp)
        args << "-query" << xdmcpHost;
    else
        args << "-" + mode;
    args << "-clipboard";
    if (exe == "vcxsrv.exe")
        args << "-wgl";
    if (exe == "xwin.exe")
        args << "-listen" << "tcp"; // Cygwin/X does not listen on TCP by default
    args << "-ac";                  // ssh for Windows has no xauth: cookie-less access, local only
    return {server.executable, args, {}};
}

int freeDisplay(int from)
{
    for (int n = qMax(0, from); n < 100; ++n)
    {
        QTcpServer probe;
        if (probe.listen(QHostAddress::Any, quint16(6000 + n)))
            return n;
    }
    return -1;
}

XServerManager::XServerManager() = default;

XServerManager::~XServerManager()
{
    stop();
}

bool XServerManager::start(const XServerInfo &server, const QString &mode, const QString &xdmcpHost)
{
    stop();
    message.clear();
    if (server.executable.isEmpty())
    {
        native = !qEnvironmentVariable("DISPLAY").isEmpty();
        if (!native)
            message = ui("DISPLAY is not set.");
        return native;
    }
    const bool x410 = QFileInfo(server.executable).fileName().compare("X410.exe", Qt::CaseInsensitive) == 0;
    const int candidate = x410 ? (freeDisplay(0) == 0 ? 0 : -1) : freeDisplay(0);
    if (candidate < 0)
    {
        message = ui("No free X display.");
        return false;
    }
    const auto command = xserverCommand(server, candidate, mode, xdmcpHost);
    if (!command.error.isEmpty())
    {
        message = command.error;
        return false;
    }
    process = std::make_unique<QProcess>();
#ifdef Q_OS_WIN
    // VcXsrv runs "%COMSPEC% /c xkbcomp ..." at startup; with Windows Terminal as the default terminal that console
    // flashes on screen. COMSPEC points to CrossTerm itself, which runs cmd in an invisible pseudoconsole.
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("COMSPEC", QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
    environment.insert("CROSSTERM_HIDDEN_SHELL", "1");
    process->setProcessEnvironment(environment);
#endif
    process->start(command.program, command.arguments);
    if (!process->waitForStarted(5000))
    {
        message = process->errorString();
        process.reset();
        return false;
    }
    // Ready once it accepts TCP connections on the display port.
    for (QDeadlineTimer deadline(15000); !deadline.hasExpired() && process->state() == QProcess::Running;)
    {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, quint16(6000 + candidate));
        if (socket.waitForConnected(500))
        {
            number = candidate;
            return true;
        }
        process->waitForFinished(200);
    }
    message = ui("The X server did not start.");
    stop();
    return false;
}

void XServerManager::stop()
{
    if (process)
    {
        process->kill();
        process->waitForFinished(3000);
        process.reset();
    }
    native = false;
    number = -1;
}

bool XServerManager::isRunning() const
{
    return native || (process && process->state() == QProcess::Running);
}

QString XServerManager::displayVariable() const
{
    if (native)
        return qEnvironmentVariable("DISPLAY");
    return isRunning() ? "127.0.0.1:" + QString::number(number) + ".0" : QString{};
}
