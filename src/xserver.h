#pragma once
#include "profiles.h"
#include <memory>

class QProcess;

// empty executable = the systems native display (DISPLAY on Linux, XQuartz on macOS).
struct XServerInfo
{
    QString name, executable;
};

// Windows: the <appdir>/xserver/ folder first, then extraDirectories, standard install paths and PATH.
QList<XServerInfo> detectXServers(const QStringList &extraDirectories = {});
// mode: "multiwindow" (default), "rootless", "fullscreen", "xdmcp" (-query xdmcpHost) or "broadcast".
Command xserverCommand(const XServerInfo &server, int display, const QString &mode = "multiwindow",
                       const QString &xdmcpHost = {});
// First display number >= from whose TCP port 6000+n is free on 127.0.0.1; -1 if none.
int freeDisplay(int from = 0);

class XServerManager
{
public:
    XServerManager();
    ~XServerManager();
    bool start(const XServerInfo &server, const QString &mode = "multiwindow", const QString &xdmcpHost = {});
    void stop();
    bool isRunning() const;
    int display() const { return number; }
    QString error() const { return message; }
    // DISPLAY value for child processes ("127.0.0.1:<n>.0"); empty if the server is not running.
    QString displayVariable() const;

private:
    std::unique_ptr<QProcess> process;
    bool native = false;
    int number = -1;
    QString message;
};
