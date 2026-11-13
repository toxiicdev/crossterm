#pragma once
#include <QStringList>

// OpenSSH ProxyCommand helper: arguments contains (anywhere) "--proxy-connect <http|socks5|socks4> <proxyHost>
// <proxyPort> <targetHost> <targetPort>". Connects to the proxy and relays stdin/stdout <-> socket until EOF.
// Returns 0 when the session ends, 1 for invalid arguments, 2 if the proxy or destination refuses the connection.
int runProxyConnect(const QStringList &arguments);

// ssh options {"-o", "ProxyCommand=..."} that use helperExecutable as ProxyCommand; empty if the data is invalid.
QStringList proxyArguments(const QString &type, const QString &host, int port, const QString &helperExecutable);
