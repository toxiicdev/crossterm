#include "transports.h"
#include "keepalive.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTcpSocket>
#include <QTimer>
#include <algorithm>
#include <cstring>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#ifdef Q_OS_MACOS
#include <IOKit/serial/ioss.h> // IOSSIOSPEED: rates above 230400
#endif
#endif

// Moves at most maxSize bytes from the internal buffer to the caller.
static qint64 take(QByteArray &buffer, char *data, qint64 maxSize)
{
    const qint64 count = std::min<qint64>(maxSize, buffer.size());
    memcpy(data, buffer.constData(), size_t(count));
    buffer.remove(0, qsizetype(count));
    return count;
}

static QString translated(const char *text)
{
    return QCoreApplication::translate("CrossTerm", text);
}

// Telnet commands and options (RFC 854, 856, 857, 858, 1073, 1091).
enum : uchar
{
    TnSE = 240,
    TnSB = 250,
    TnWILL = 251,
    TnWONT = 252,
    TnDO = 253,
    TnDONT = 254,
    TnIAC = 255,
    TnBINARY = 0,
    TnECHO = 1,
    TnSGA = 3,
    TnTTYPE = 24,
    TnNAWS = 31
};

TelnetDevice::TelnetDevice(const QString &host, quint16 port, QObject *parent) : QIODevice(parent), socket(new QTcpSocket(this))
{
    QIODevice::open(ReadWrite | Unbuffered);
    connect(socket, &QTcpSocket::connected, this,
            [this]
            {
                // Offer NAWS and TERMINAL-TYPE; the server replies DO or DONT.
                offered = {TnNAWS, TnTTYPE};
                socket->write(QByteArray::fromRawData("\xff\xfb\x1f\xff\xfb\x18", 6));
            });
    connect(socket, &QTcpSocket::readyRead, this, [this] { receive(); });
    connect(socket, &QTcpSocket::disconnected, this, &QIODevice::readChannelFinished);
    connect(socket, &QAbstractSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError error)
            {
                if (error == QAbstractSocket::RemoteHostClosedError)
                    return; // disconnected follows
                setErrorString(socket->errorString());
                emit readChannelFinished();
            });
    enableKeepAlive(socket); // a server that vanished ends the session instead of freezing it
    socket->connectToHost(host, port);
}

void TelnetDevice::receive()
{
    const QByteArray chunk = socket->readAll();
    const qsizetype before = input.size();
    for (const char ch : chunk)
    {
        const uchar c = uchar(ch);
        switch (parse)
        {
        case Data:
            if (c == TnIAC)
                parse = Iac;
            else if (!(carriageReturn && c == 0 && !remote.contains(TnBINARY))) // CR NUL in NVT means CR
                input += ch;
            carriageReturn = c == '\r';
            break;
        case Iac:
            carriageReturn = false;
            if (c == TnIAC)
                input += ch;
            if (c == TnSB)
            {
                subnegotiation.clear();
                parse = Sub;
            }
            else if (c >= TnWILL && c <= TnDONT)
            {
                verb = c;
                parse = Verb;
            }
            else
                parse = Data; // IAC IAC, NOP, GA and other commands without arguments
            break;
        case Verb:
            command(verb, c);
            parse = Data;
            break;
        case Sub:
            if (c == TnIAC)
                parse = SubIac;
            else if (subnegotiation.size() < 1024)
                subnegotiation += ch;
            break;
        case SubIac:
            if (c == TnSE)
            {
                // TERMINAL-TYPE SEND: reply IS xterm-256color.
                if (subnegotiation.size() >= 2 && uchar(subnegotiation[0]) == TnTTYPE && subnegotiation[1] == 1)
                    socket->write(QByteArray("\xff\xfa\x18\x00xterm-256color\xff\xf0", 20));
                parse = Data;
            }
            else
            {
                if (subnegotiation.size() < 1024) // as in Sub: what is beyond the limit is dropped
                    subnegotiation += ch;         // IAC IAC inside the subnegotiation
                parse = Sub;
            }
            break;
        }
    }
    if (input.size() > before)
        emit readyRead();
}

void TelnetDevice::command(uchar verb, uchar option)
{
    static const QSet<uchar> ours{TnBINARY, TnSGA, TnNAWS, TnTTYPE}, theirs{TnBINARY, TnECHO, TnSGA};
    auto send = [this, option](uchar reply)
    {
        const char bytes[] = {char(TnIAC), char(reply), char(option)};
        socket->write(bytes, 3);
    };
    // Reply only to state changes so negotiations do not loop (RFC 1143).
    switch (verb)
    {
    case TnDO:
        if (offered.remove(option) || local.contains(option))
            local.insert(option);
        else if (ours.contains(option))
        {
            local.insert(option);
            send(TnWILL);
        }
        else
        {
            send(TnWONT);
            return;
        }
        if (option == TnNAWS)
            sendNaws();
        break;
    case TnDONT:
        offered.remove(option);
        if (local.remove(option))
            send(TnWONT);
        break;
    case TnWILL:
        if (remote.contains(option))
            break;
        if (theirs.contains(option))
        {
            remote.insert(option);
            send(TnDO);
        }
        else
            send(TnDONT);
        break;
    case TnWONT:
        if (remote.remove(option))
            send(TnDONT);
        break;
    }
}

void TelnetDevice::sendNaws()
{
    if (!local.contains(TnNAWS))
        return;
    QByteArray message("\xff\xfa\x1f", 3);
    for (const int value : {cols, rows})
    {
        for (const uchar byte : {uchar(value >> 8), uchar(value)})
        {
            message += char(byte);
            if (byte == TnIAC)
                message += char(TnIAC);
        }
    }
    socket->write(message + "\xff\xf0");
}

void TelnetDevice::setWindowSize(int cols, int rows)
{
    this->cols = std::clamp(cols, 1, 65535);
    this->rows = std::clamp(rows, 1, 65535);
    sendNaws();
}

qint64 TelnetDevice::readData(char *data, qint64 maxSize)
{
    return take(input, data, maxSize);
}

qint64 TelnetDevice::writeData(const char *data, qint64 size)
{
    QByteArray out;
    out.reserve(qsizetype(size) + 8);
    for (qint64 i = 0; i < size; ++i)
    {
        out += data[i];
        if (uchar(data[i]) == TnIAC)
            out += data[i];
        else if (data[i] == '\r' && !local.contains(TnBINARY) && (i + 1 == size || data[i + 1] != '\n'))
            out += '\0'; // a bare CR becomes CR NUL (RFC 854)
    }
    return socket->write(out) < 0 ? -1 : size;
}

void TelnetDevice::close()
{
    socket->close();
    QIODevice::close();
}

RloginDevice::RloginDevice(const QString &host, const QString &remoteUser, quint16 port, const QString &localUser,
                           QObject *parent)
    : QIODevice(parent), socket(new QTcpSocket(this))
{
    QIODevice::open(ReadWrite | Unbuffered);
    const QString user = localUser.isEmpty() ? qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER")) : localUser;
    connect(socket, &QTcpSocket::readyRead, this, [this] { receive(); });
    connect(socket, &QTcpSocket::disconnected, this, &QIODevice::readChannelFinished);
    // rlogind, like rshd, refuses unprivileged source ports ("illegal port"): a reserved one (512..1023) if any.
    // Before errorOccurred is connected: a failed bind must not end the session or stay as its reason.
    bool reserved = false;
    for (quint16 local = 1023; local >= 512 && !reserved; --local)
        reserved = socket->bind(QHostAddress::Any, local);
    if (!reserved)
        socket->bind(QHostAddress::Any, 0);
    connect(socket, &QAbstractSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError error)
            {
                if (error == QAbstractSocket::RemoteHostClosedError)
                    return;
                setErrorString(socket->errorString());
                emit readChannelFinished();
            });
    enableKeepAlive(socket); // a server that vanished ends the session instead of freezing it
    socket->connectToHost(host, port);
    // RFC 1282: "\0local-user\0remote-user\0terminal-type/speed\0" (queued until connected).
    const QByteArray zero(1, '\0');
    socket->write(zero + user.toUtf8() + zero + remoteUser.toUtf8() + zero + "xterm-256color/38400" + zero);
}

void RloginDevice::receive()
{
    QByteArray chunk = socket->readAll();
    if (!accepted && !chunk.isEmpty())
    {
        accepted = true;
        if (chunk[0] == '\0')
            chunk.remove(0, 1);
        sendWindow();
    }
    if (chunk.isEmpty())
        return;
    input += chunk;
    emit readyRead();
}

void RloginDevice::sendWindow()
{
    if (!accepted)
        return;
    // 12-byte message: 0xff 0xff 's' 's', rows, columns, x pixels, y pixels (16 bit, big endian).
    QByteArray message("\xff\xffss", 4);
    for (const int value : {rows, cols, 0, 0})
    {
        message += char(uchar(value >> 8));
        message += char(uchar(value));
    }
    socket->write(message);
}

void RloginDevice::setWindowSize(int cols, int rows)
{
    this->cols = std::clamp(cols, 1, 65535);
    this->rows = std::clamp(rows, 1, 65535);
    sendWindow();
}

qint64 RloginDevice::readData(char *data, qint64 maxSize)
{
    return take(input, data, maxSize);
}

qint64 RloginDevice::writeData(const char *data, qint64 size)
{
    return socket->write(data, size);
}

void RloginDevice::close()
{
    socket->close();
    QIODevice::close();
}

RshDevice::RshDevice(const QString &host, const QString &remoteUser, const QString &command, quint16 port,
                     const QString &localUser, QObject *parent)
    : QIODevice(parent), socket(new QTcpSocket(this))
{
    QIODevice::open(ReadWrite | Unbuffered);
    const QString user = localUser.isEmpty() ? qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER")) : localUser;
    connect(socket, &QTcpSocket::readyRead, this, [this] { receive(); });
    connect(socket, &QTcpSocket::disconnected, this, &QIODevice::readChannelFinished);
    // rshd accepts only reserved source ports (512..1023): try from the highest.
    bool reserved = false;
    for (quint16 local = 1023; local >= 512 && !reserved; --local)
        reserved = socket->bind(QHostAddress::Any, local);
    if (!reserved)
    {
        socket->bind(QHostAddress::Any, 0);
        qWarning("rsh: no reserved local port available, the server will probably refuse the connection");
        input += translated(
                     "Warning: no reserved local port (512-1023) available; the rsh server will probably refuse the connection.")
                     .toUtf8() +
                 "\r\n";
    }
    // Connected only now: each failed bind above emits errorOccurred, and its message would stay as errorString
    // (shown as the reason when the session ends normally).
    connect(socket, &QAbstractSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError error)
            {
                if (error == QAbstractSocket::RemoteHostClosedError)
                    return;
                setErrorString(socket->errorString());
                emit readChannelFinished();
            });
    enableKeepAlive(socket); // a server that vanished ends the session instead of freezing it
    socket->connectToHost(host, port);
    const QByteArray zero(1, '\0');
    socket->write("0" + zero + user.toUtf8() + zero + remoteUser.toUtf8() + zero + command.toUtf8() + zero);
}

QIODevice *RshDevice::create(const QString &host, const QString &remoteUser, const QString &command, quint16 port,
                             const QString &localUser, QObject *parent)
{
    if (command.trimmed().isEmpty())
        return new RloginDevice(host, remoteUser, port == 514 ? 513 : port, localUser, parent);
    return new RshDevice(host, remoteUser, command, port, localUser, parent);
}

quint16 RshDevice::localPort() const
{
    return socket->localPort();
}

void RshDevice::receive()
{
    QByteArray chunk = socket->readAll();
    if (!accepted && !chunk.isEmpty())
    {
        accepted = true;
        failed = chunk[0] == '\1';
        if (chunk[0] == '\0' || failed)
            chunk.remove(0, 1);
    }
    if (failed && !errorText.contains('\n') && errorText.size() < 4096)
    {
        // Error message up to "\n" (4096 bytes at most); the server then closes. It is also shown in the terminal.
        errorText += chunk.left(4096 - errorText.size());
        setErrorString(QString::fromUtf8(errorText).section('\n', 0, 0).trimmed());
    }
    if (chunk.isEmpty())
        return;
    input += chunk;
    emit readyRead();
}

qint64 RshDevice::readData(char *data, qint64 maxSize)
{
    return take(input, data, maxSize);
}

qint64 RshDevice::writeData(const char *data, qint64 size)
{
    return socket->write(data, size);
}

void RshDevice::close()
{
    socket->close();
    QIODevice::close();
}

SerialDevice::SerialDevice(const Settings &settings, QObject *parent)
    : QIODevice(parent), settings(settings), timer(new QTimer(this))
{
    connect(timer, &QTimer::timeout, this, [this] { poll(); });
}

SerialDevice::~SerialDevice()
{
    if (handle != -1)
        close();
}

bool SerialDevice::open(OpenMode mode)
{
    if (handle != -1)
        return false;
    const auto &s = settings;
#ifdef Q_OS_WIN
    QString name = s.portName;
    if (!name.startsWith("\\\\.\\"))
        name.prepend("\\\\.\\"); // required from COM10 onwards
    HANDLE port =
        CreateFileW(reinterpret_cast<LPCWSTR>(name.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (port == INVALID_HANDLE_VALUE)
    {
        setErrorString(translated("Cannot open %1: %2").arg(s.portName, qt_error_string(int(GetLastError()))));
        return false;
    }
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(port, &dcb);
    dcb.BaudRate = DWORD(s.baud);
    dcb.ByteSize = BYTE(s.dataBits);
    dcb.Parity = s.parity == Settings::EvenParity ? EVENPARITY : s.parity == Settings::OddParity ? ODDPARITY : NOPARITY;
    dcb.fParity = s.parity != Settings::NoParity;
    dcb.StopBits = s.stopBits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    const bool hardware = s.flowControl == Settings::HardwareFlowControl;
    dcb.fOutxCtsFlow = hardware;
    dcb.fRtsControl = hardware ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
    dcb.fOutX = dcb.fInX = s.flowControl == Settings::SoftwareFlowControl;
    dcb.XonChar = 0x11;
    dcb.XoffChar = 0x13;
    // Non-blocking read (returns immediately with whatever is available); a write gives up after 50 ms and send()
    // keeps what was not sent.
    COMMTIMEOUTS timeouts{MAXDWORD, 0, 0, 0, 50};
    if (!SetCommState(port, &dcb) || !SetCommTimeouts(port, &timeouts))
    {
        setErrorString(translated("Cannot configure %1: %2").arg(s.portName, qt_error_string(int(GetLastError()))));
        CloseHandle(port);
        return false;
    }
    handle = qintptr(port);
#else
    // O_CLOEXEC: shells and other programs started later must not keep the port open.
    const int fd = ::open(QFile::encodeName(s.portName).constData(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        setErrorString(translated("Cannot open %1: %2").arg(s.portName, qt_error_string(errno)));
        return false;
    }
    // Exclusive: a second session on the same port (duplicated tab) fails with "busy" instead of stealing half of
    // the incoming bytes from this one.
    ioctl(fd, TIOCEXCL);
#ifdef Q_OS_MACOS
    // On BSD/macOS the speed is the numeric value. Above 230400 (460800, 921600) the drivers refuse it in
    // tcsetattr: 230400 there, then the real rate with IOSSIOSPEED below (as pyserial and screen do).
    const speed_t real = speed_t(s.baud);
    const speed_t speed = real > 230400 ? speed_t(230400) : real;
#else
    static const std::pair<int, speed_t> speeds[] = {{1200, B1200},     {2400, B2400},     {4800, B4800},    {9600, B9600},
                                                     {19200, B19200},   {38400, B38400},   {57600, B57600},  {115200, B115200},
                                                     {230400, B230400}, {460800, B460800}, {921600, B921600}};
    speed_t speed = 0;
    for (const auto &[baud, value] : speeds)
    {
        if (baud == s.baud)
            speed = value;
    }
#endif
    termios tio{};
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);
    tio.c_cflag &= ~tcflag_t(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag |= s.dataBits == 5 ? CS5 : s.dataBits == 6 ? CS6 : s.dataBits == 7 ? CS7 : CS8;
    if (s.parity != Settings::NoParity)
        tio.c_cflag |= PARENB | (s.parity == Settings::OddParity ? PARODD : 0);
    if (s.stopBits == 2)
        tio.c_cflag |= CSTOPB;
    if (s.flowControl == Settings::HardwareFlowControl)
        tio.c_cflag |= CRTSCTS;
    if (s.flowControl == Settings::SoftwareFlowControl)
        tio.c_iflag |= IXON | IXOFF;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (!speed || cfsetispeed(&tio, speed) != 0 || cfsetospeed(&tio, speed) != 0 || tcsetattr(fd, TCSANOW, &tio) != 0
#ifdef Q_OS_MACOS
        || (real != speed && ioctl(fd, IOSSIOSPEED, &real) != 0)
#endif
    )
    {
        setErrorString(translated("Cannot configure %1: %2")
                           .arg(s.portName, speed ? qt_error_string(errno) : translated("unsupported speed")));
        ::close(fd);
        return false;
    }
    handle = fd;
#endif
    timer->start(10);
    return QIODevice::open(mode | Unbuffered);
}

#ifndef Q_OS_WIN
// Data or end of file waiting (select(): unlike poll(), it supports devices on macOS).
static bool readable(int fd)
{
    if (fd >= FD_SETSIZE)
        return false;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval now{0, 0};
    return select(fd + 1, &set, nullptr, nullptr, &now) > 0;
}
#endif

void SerialDevice::poll()
{
    char buffer[4096];
    const qsizetype before = input.size();
    for (;;)
    {
#ifdef Q_OS_WIN
        DWORD count = 0;
        if (!ReadFile(HANDLE(handle), buffer, sizeof(buffer), &count, nullptr))
            return fail(qt_error_string(int(GetLastError())));
#else
        auto count = ::read(int(handle), buffer, sizeof(buffer));
        // VMIN = VTIME = 0: read() returns 0 both without data and after a hangup (USB adapter unplugged, other end of
        // a pty closed). Readable for select() and still nothing to read is the hangup.
        if (count == 0 && readable(int(handle)) && (count = ::read(int(handle), buffer, sizeof(buffer))) == 0)
            return fail(qt_error_string(EIO));
        if (count < 0 && errno != EAGAIN && errno != EINTR)
            return fail(qt_error_string(errno));
#endif
        if (count <= 0)
            break;
        input.append(buffer, qsizetype(count));
    }
    if (input.size() > before)
        emit readyRead();
    if (!send())
    {
#ifdef Q_OS_WIN
        fail(qt_error_string(int(GetLastError())));
#else
        fail(qt_error_string(errno));
#endif
    }
}

#ifdef Q_OS_WIN
// WriteFile() blocks until the bytes are on the line or the write timeout ends: a whole paste at a slow baud rate
// would freeze the window and lose what did not fit in the timeout. So one slice at a time, what the line carries
// in about 20 ms; poll() sends the rest. False on an error (GetLastError()).
bool SerialDevice::send()
{
    if (output.isEmpty())
        return true;
    if (held > 0)
    {
        --held;
        return true;
    }
    const DWORD slice = DWORD(qBound(16, settings.baud / 500, 4096));
    DWORD count = 0;
    if (!WriteFile(HANDLE(handle), output.constData(), qMin(DWORD(output.size()), slice), &count, nullptr))
    {
        const DWORD error = GetLastError();
        output.clear();
        SetLastError(error);
        return false;
    }
    output.remove(0, qsizetype(count));
    held = count == 0 ? 20 : 0; // nothing accepted (flow control holds the line): next try in 200 ms
    return true;
}
#else
// Writes what the port accepts now (slow baud rate, flow control); poll() retries the rest. False on an error (errno).
bool SerialDevice::send()
{
    while (!output.isEmpty())
    {
        const auto count = ::write(int(handle), output.constData(), size_t(output.size()));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && errno != EAGAIN)
        {
            const int error = errno;
            output.clear();
            errno = error;
            return false;
        }
        if (count <= 0)
            break;
        output.remove(0, qsizetype(count));
    }
    return true;
}
#endif

void SerialDevice::fail(const QString &message)
{
    timer->stop();
    setErrorString(message);
    emit readChannelFinished();
    release(); // the port is free again: restarting the session opens it while this device still exists
}

void SerialDevice::release()
{
    output.clear();
    held = 0;
    if (handle != -1)
    {
#ifdef Q_OS_WIN
        CloseHandle(HANDLE(handle));
#else
        ::close(int(handle));
#endif
        handle = -1;
    }
}

qint64 SerialDevice::readData(char *data, qint64 maxSize)
{
    return take(input, data, maxSize);
}

qint64 SerialDevice::writeData(const char *data, qint64 size)
{
    if (handle == -1)
        return -1;
    // Queued: the port takes only part of a long write at once (e.g. a paste at 9600 baud).
    output.append(data, qsizetype(size));
    return send() ? size : -1;
}

void SerialDevice::close()
{
    timer->stop();
    release();
    QIODevice::close();
}

QStringList serialPorts()
{
    QStringList ports;
#ifdef Q_OS_WIN
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        for (DWORD i = 0;; ++i)
        {
            wchar_t name[256], data[257] = {};
            DWORD nameSize = 256, dataSize = sizeof(data) - sizeof(wchar_t), type = 0;
            if (RegEnumValueW(key, i, name, &nameSize, nullptr, &type, reinterpret_cast<LPBYTE>(data), &dataSize) !=
                ERROR_SUCCESS)
                break;
            if (type == REG_SZ)
                ports << QString::fromWCharArray(data);
        }
        RegCloseKey(key);
    }
#else
    for (const auto &name : QDir("/dev").entryList({"ttyS*", "ttyUSB*", "ttyACM*", "cu.*"}, QDir::System))
        ports << "/dev/" + name;
#endif
    ports.sort();
    return ports;
}
