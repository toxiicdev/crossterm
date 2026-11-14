#pragma once
// Transports for Terminal(QIODevice *): telnet (RFC 854), rlogin (RFC 1282) and native serial port.
#include <QByteArray>
#include <QIODevice>
#include <QSet>
#include <QStringList>

class QTcpSocket;
class QTimer;

// Implemented by devices that can report the terminal size to the server.
struct WindowSizeAware
{
    virtual void setWindowSize(int cols, int rows) = 0;
    virtual ~WindowSizeAware() = default;
};

// Telnet client: the connection is asynchronous; readyRead carries only application data (no IAC sequences).
class TelnetDevice : public QIODevice, public WindowSizeAware
{
public:
    TelnetDevice(const QString &host, quint16 port, QObject *parent = nullptr);
    void setWindowSize(int cols, int rows) override;
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return input.size() + QIODevice::bytesAvailable(); }
    void close() override;

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 size) override;

private:
    void receive();
    void command(uchar verb, uchar option);
    void sendNaws();
    QTcpSocket *socket;
    QByteArray input, subnegotiation;
    QSet<uchar> local, remote, offered; // options active on our side / the server's; WILLs sent awaiting DO
    enum
    {
        Data,
        Iac,
        Verb,
        Sub,
        SubIac
    } parse = Data;
    uchar verb = 0;
    bool carriageReturn = false;
    int cols = 80, rows = 24;
};

// rlogin client. The urgent control byte (0x80 = window size request) arrives as TCP out-of-band data, which Qt
// does not expose: the size is therefore sent right after the server's "\0" and on every resize.
class RloginDevice : public QIODevice, public WindowSizeAware
{
public:
    // Empty localUser: the system user (USERNAME/USER).
    RloginDevice(const QString &host, const QString &remoteUser, quint16 port = 513, const QString &localUser = {},
                 QObject *parent = nullptr);
    void setWindowSize(int cols, int rows) override;
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return input.size() + QIODevice::bytesAvailable(); }
    void close() override;

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 size) override;

private:
    void receive();
    void sendWindow();
    QTcpSocket *socket;
    QByteArray input;
    bool accepted = false; // server's initial "\0" received
    int cols = 80, rows = 24;
};

// BSD rsh client (port 514, no RFC): from the highest free reserved local port between 1023 and 512 (if none is
// available, e.g. without privileges on POSIX, any port with a warning: modern servers refuse it), sends
// "0\0local-user\0remote-user\0command\0" (stderr merged into stdout), waits for "\0" or "\1message\n" and then
// forwards the command output; writes go to the remote stdin.
class RshDevice : public QIODevice
{
public:
    // Empty localUser: the system user (USERNAME/USER).
    RshDevice(const QString &host, const QString &remoteUser, const QString &command, quint16 port = 514,
              const QString &localUser = {}, QObject *parent = nullptr);
    // Like classic rsh: without a command rsh runs rlogin, so this returns an RloginDevice (port 513).
    static QIODevice *create(const QString &host, const QString &remoteUser, const QString &command, quint16 port = 514,
                             const QString &localUser = {}, QObject *parent = nullptr);
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return input.size() + QIODevice::bytesAvailable(); }
    void close() override;
    quint16 localPort() const;

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 size) override;

private:
    void receive();
    QTcpSocket *socket;
    QByteArray input, errorText;
    bool accepted = false, failed = false; // first server byte received; error "\1"
};

// Serial port without Qt SerialPort: Win32 (COMx) or termios; reads via a polling QTimer.
class SerialDevice : public QIODevice
{
public:
    struct Settings
    {
        QString portName; // "COM3" or "/dev/ttyUSB0"
        int baud = 9600, dataBits = 8, stopBits = 1;
        enum Parity
        {
            NoParity,
            EvenParity,
            OddParity
        } parity = NoParity;
        enum FlowControl
        {
            NoFlowControl,
            HardwareFlowControl,
            SoftwareFlowControl
        } flowControl = NoFlowControl;
    };
    explicit SerialDevice(const Settings &settings, QObject *parent = nullptr);
    ~SerialDevice() override;
    bool open(OpenMode mode) override;
    void close() override;
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return input.size() + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 size) override;

private:
    void poll();
    bool send();
    void fail(const QString &message);
    void release(); // closes the port
    Settings settings;
    QTimer *timer;
    QByteArray input, output; // output: written data the port has not accepted yet
    qintptr handle = -1;      // HANDLE on Windows, file descriptor on POSIX
    int held = 0;             // Windows: polls to skip after the port accepted nothing
};

// Available serial ports (Windows: HKLM\HARDWARE\DEVICEMAP\SERIALCOMM; POSIX: /dev/ttyS*, ttyUSB*, ttyACM*, cu.*).
QStringList serialPorts();
