#pragma once
class QAbstractSocket;

// TCP keepalive with short timings on a TCP socket (now if connected, otherwise once it connects): a peer that
// vanished without closing the connection (server crash, network lost, sleep) is detected in about 30 s and the
// socket reports an error instead of waiting forever. Probes are sent only while the connection is idle.
void enableKeepAlive(QAbstractSocket *socket);
