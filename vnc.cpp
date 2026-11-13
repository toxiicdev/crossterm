#include "vnc.h"
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QStringDecoder>
#include <QTcpSocket>
#include <QtEndian>
#include <algorithm>

static QString ui(const char *text)
{
    return QCoreApplication::translate("CrossTerm", text);
}

// ---------------------------------------------------------------------------------------------------------------
// DES (FIPS 46-3): tables with bits numbered from 1, most significant first.

static const quint8 IP[64] = {58, 50, 42, 34, 26, 18, 10, 2,  60, 52, 44, 36, 28, 20, 12, 4,  62, 54, 46, 38, 30, 22,
                              14, 6,  64, 56, 48, 40, 32, 24, 16, 8,  57, 49, 41, 33, 25, 17, 9,  1,  59, 51, 43, 35,
                              27, 19, 11, 3,  61, 53, 45, 37, 29, 21, 13, 5,  63, 55, 47, 39, 31, 23, 15, 7};
static const quint8 FP[64] = {40, 8,  48, 16, 56, 24, 64, 32, 39, 7,  47, 15, 55, 23, 63, 31, 38, 6,  46, 14, 54, 22,
                              62, 30, 37, 5,  45, 13, 53, 21, 61, 29, 36, 4,  44, 12, 52, 20, 60, 28, 35, 3,  43, 11,
                              51, 19, 59, 27, 34, 2,  42, 10, 50, 18, 58, 26, 33, 1,  41, 9,  49, 17, 57, 25};
static const quint8 E[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
                             16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
static const quint8 P[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                             2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};
static const quint8 PC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43,
                               35, 27, 19, 11, 3,  60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,  62, 54,
                               46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
static const quint8 PC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,  26, 8,  16, 7,  27, 20, 13, 2,
                               41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
static const quint8 SHIFTS[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
static const quint8 SBOX[8][64] = {
    {14, 4, 13, 1, 2,  15, 11, 8,  3,  10, 6, 12, 5, 9,  0, 7, 0,  15, 7, 4, 14, 2, 13, 1, 10, 6,  12, 11, 9,  5, 3, 8,
     4,  1, 14, 8, 13, 6,  2,  11, 15, 12, 9, 7,  3, 10, 5, 0, 15, 12, 8, 2, 4,  9, 1,  7, 5,  11, 3,  14, 10, 0, 6, 13},
    {15, 1,  8, 14, 6,  11, 3,  4, 9, 7, 2,  13, 12, 0, 5, 10, 3,  13, 4,  7, 15, 2,  8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
     0,  14, 7, 11, 10, 4,  13, 1, 5, 8, 12, 6,  9,  3, 2, 15, 13, 8,  10, 1, 3,  15, 4, 2,  11, 6, 7, 12, 0, 5, 14, 9},
    {10, 0, 9, 14, 6, 3,  15, 5, 1,  13, 12, 7,  11, 4,  2,  8, 13, 7,  0,  9, 3, 4, 6, 10, 2, 8,  5,  14, 12, 11, 15, 1,
     13, 6, 4, 9,  8, 15, 3,  0, 11, 1,  2,  12, 5,  10, 14, 7, 1,  10, 13, 0, 6, 9, 8, 7,  4, 15, 14, 3,  11, 5,  2,  12},
    {7,  13, 14, 3, 0,  6,  9, 10, 1,  2, 8, 5,  11, 12, 4, 15, 13, 8,  11, 5, 6,  15, 0,  3, 4, 7, 2, 12, 1,  10, 14, 9,
     10, 6,  9,  0, 12, 11, 7, 13, 15, 1, 3, 14, 5,  2,  8, 4,  3,  15, 0,  6, 10, 1,  13, 8, 9, 4, 5, 11, 12, 7,  2,  14},
    {2, 12, 4, 1,  7,  10, 11, 6, 8,  5, 3,  15, 13, 0, 14, 9,  14, 11, 2,  12, 4, 7,  13, 1,  5, 0,  15, 10, 3,  9, 8, 6,
     4, 2,  1, 11, 10, 13, 7,  8, 15, 9, 12, 5,  6,  3, 0,  14, 11, 8,  12, 7,  1, 14, 2,  13, 6, 15, 0,  9,  10, 4, 5, 3},
    {12, 1,  10, 15, 9, 2, 6,  8, 0, 13, 3, 4,  14, 7,  5,  11, 10, 15, 4, 2,  7, 12, 9,  5,  6,  1,  13, 14, 0, 11, 3, 8,
     9,  14, 15, 5,  2, 8, 12, 3, 7, 0,  4, 10, 1,  13, 11, 6,  4,  3,  2, 12, 9, 5,  15, 10, 11, 14, 1,  7,  6, 0,  8, 13},
    {4, 11, 2,  14, 15, 0, 8, 13, 3,  12, 9, 7, 5, 10, 6, 1, 13, 0,  11, 7, 4, 9, 1,  10, 14, 3, 5, 12, 2,  15, 8, 6,
     1, 4,  11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5,  9, 2, 6,  11, 13, 8, 1, 4, 10, 7,  9,  5, 0, 15, 14, 2,  3, 12},
    {13, 2,  8, 4, 6, 15, 11, 1, 10, 9, 3,  14, 5,  0, 12, 7, 1, 15, 13, 8, 10, 3,  7, 4,  12, 5,  6, 11, 0, 14, 9, 2,
     7,  11, 4, 1, 9, 12, 14, 2, 0,  6, 10, 13, 15, 3, 5,  8, 2, 1,  14, 7, 4,  10, 8, 13, 15, 12, 9, 0,  3, 5,  6, 11}};

static quint64 permute(quint64 input, const quint8 *table, int n, int inBits)
{
    quint64 out = 0;
    for (int i = 0; i < n; ++i)
        out = (out << 1) | ((input >> (inBits - table[i])) & 1);
    return out;
}

QByteArray desEncrypt(const QByteArray &key, const QByteArray &block)
{
    if (key.size() != 8 || block.size() != 8)
        return {};
    const quint64 cd = permute(qFromBigEndian<quint64>(key.constData()), PC1, 56, 64);
    quint32 c = quint32(cd >> 28), d = quint32(cd & 0xFFFFFFF);
    quint64 sub[16];
    for (int i = 0; i < 16; ++i)
    {
        const int s = SHIFTS[i];
        c = ((c << s) | (c >> (28 - s))) & 0xFFFFFFF;
        d = ((d << s) | (d >> (28 - s))) & 0xFFFFFFF;
        sub[i] = permute((quint64(c) << 28) | d, PC2, 48, 56);
    }
    const quint64 ip = permute(qFromBigEndian<quint64>(block.constData()), IP, 64, 64);
    quint32 l = quint32(ip >> 32), r = quint32(ip);
    for (int i = 0; i < 16; ++i)
    {
        const quint64 e = permute(r, E, 48, 32) ^ sub[i];
        quint64 s = 0;
        for (int j = 0; j < 8; ++j)
        {
            const int six = int(e >> (42 - 6 * j)) & 0x3F;
            s = (s << 4) | SBOX[j][(six & 0x20) | (six & 1) << 4 | ((six >> 1) & 0xF)];
        }
        const quint32 next = l ^ quint32(permute(s, P, 32, 32));
        l = r;
        r = next;
    }
    QByteArray out(8, 0);
    qToBigEndian(permute((quint64(r) << 32) | l, FP, 64, 64), out.data());
    return out;
}

QByteArray vncAuthResponse(const QByteArray &password, const QByteArray &challenge)
{
    // VNC convention: each key byte has its bit order reversed.
    QByteArray key(8, 0);
    for (int i = 0; i < 8 && i < password.size(); ++i)
    {
        uchar b = uchar(password[i]), reversed = 0;
        for (int bit = 0; bit < 8; ++bit)
            reversed |= ((b >> bit) & 1) << (7 - bit);
        key[i] = char(reversed);
    }
    return desEncrypt(key, challenge.mid(0, 8)) + desEncrypt(key, challenge.mid(8, 8));
}

// ---------------------------------------------------------------------------------------------------------------
// "puff"-style inflate: one block at a time; if the data ends mid-block, restart from the beginning of the block.

namespace
{
struct Underflow
{
};

struct Huffman
{
    short count[16];
    short symbol[288];
};

struct Bits
{
    const uchar *data;
    qsizetype size, pos;
    quint32 buf;
    int cnt;

    int need(int n)
    {
        while (cnt < n)
        {
            if (pos == size)
                throw Underflow();
            buf |= quint32(data[pos++]) << cnt;
            cnt += 8;
        }
        const int value = int(buf & ((1u << n) - 1));
        buf >>= n;
        cnt -= n;
        return value;
    }
};
} // namespace

static const short LBASE[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short DBASE[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static bool build(Huffman &h, const quint8 *lengths, int n)
{
    std::fill_n(h.count, 16, short(0));
    for (int i = 0; i < n; ++i)
        ++h.count[lengths[i]];
    int left = 1;
    for (int len = 1; len < 16; ++len)
    {
        left = (left << 1) - h.count[len];
        if (left < 0)
            return false; // over-subscribed code
    }
    short offsets[16] = {};
    for (int len = 1; len < 15; ++len)
        offsets[len + 1] = short(offsets[len] + h.count[len]);
    for (int i = 0; i < n; ++i)
    {
        if (lengths[i])
            h.symbol[offsets[lengths[i]]++] = short(i);
    }
    return true;
}

static int decode(Bits &b, const Huffman &h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len)
    {
        code |= b.need(1);
        const int count = h.count[len];
        if (code - count < first)
            return h.symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static bool codes(Bits &b, QByteArray &window, const Huffman &lengths, const Huffman &distances)
{
    for (;;)
    {
        int symbol = decode(b, lengths);
        if (symbol < 0)
            return false;
        if (symbol < 256)
            window.append(char(symbol));
        else if (symbol == 256)
            return true;
        else
        {
            symbol -= 257;
            if (symbol >= 29)
                return false;
            int len = LBASE[symbol] + b.need(LEXT[symbol]);
            const int d = decode(b, distances);
            if (d < 0 || d >= 30)
                return false;
            const int distance = DBASE[d] + b.need(DEXT[d]);
            if (distance > window.size())
                return false;
            for (; len; --len)
                window.append(window.at(window.size() - distance));
        }
    }
}

static bool dynamic(Bits &b, QByteArray &window)
{
    static const quint8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int nlen = b.need(5) + 257, ndist = b.need(5) + 1, ncode = b.need(4) + 4;
    if (nlen > 286 || ndist > 30)
        return false;
    quint8 cl[19] = {}, lengths[316] = {};
    for (int i = 0; i < ncode; ++i)
        cl[order[i]] = quint8(b.need(3));
    Huffman table, distances;
    if (!build(table, cl, 19))
        return false;
    for (int i = 0; i < nlen + ndist;)
    {
        const int symbol = decode(b, table);
        if (symbol < 0)
            return false;
        if (symbol < 16)
        {
            lengths[i++] = quint8(symbol);
            continue;
        }
        int value = 0, repeat;
        if (symbol == 16)
        {
            if (i == 0)
                return false;
            value = lengths[i - 1];
            repeat = 3 + b.need(2);
        }
        else
            repeat = symbol == 17 ? 3 + b.need(3) : 11 + b.need(7);
        if (i + repeat > nlen + ndist)
            return false;
        while (repeat--)
            lengths[i++] = quint8(value);
    }
    if (!lengths[256] || !build(table, lengths, nlen) || !build(distances, lengths + nlen, ndist))
        return false;
    return codes(b, window, table, distances);
}

bool VncInflater::feed(const QByteArray &data, QByteArray &out)
{
    static Huffman fixedLengths, fixedDistances;
    static const bool ready = []
    {
        quint8 l[288];
        std::fill_n(l, 144, quint8(8));
        std::fill_n(l + 144, 112, quint8(9));
        std::fill_n(l + 256, 24, quint8(7));
        std::fill_n(l + 280, 8, quint8(8));
        build(fixedLengths, l, 288);
        std::fill_n(l, 30, quint8(5));
        return build(fixedDistances, l, 30);
    }();
    Q_UNUSED(ready);

    out.clear();
    if (finished)
        return true; // ignore the Adler-32 checksum and anything after it
    pending += data;
    if (!header)
    {
        if (pending.size() < 2)
            return true;
        const int cmf = uchar(pending[0]), flg = uchar(pending[1]);
        if ((cmf & 0x0F) != 8 || (cmf * 256 + flg) % 31 || (flg & 0x20))
            return false;
        pending.remove(0, 2);
        header = true;
    }
    const qsizetype start = window.size();
    Bits b{reinterpret_cast<const uchar *>(pending.constData()), pending.size(), 0, bitbuf, bitcnt};
    while (!finished)
    {
        const Bits saved = b;
        const qsizetype mark = window.size();
        try
        {
            const bool last = b.need(1);
            const int type = b.need(2);
            bool ok = true;
            if (type == 0)
            {
                b.buf = 0; // byte alignment: need() never reads more than necessary
                b.cnt = 0;
                if (b.size - b.pos < 4)
                    throw Underflow();
                const int len = b.data[b.pos] | b.data[b.pos + 1] << 8;
                const int nlen = b.data[b.pos + 2] | b.data[b.pos + 3] << 8;
                if (len != (~nlen & 0xFFFF))
                    return false;
                if (b.size - b.pos - 4 < len)
                    throw Underflow();
                window.append(reinterpret_cast<const char *>(b.data + b.pos + 4), len);
                b.pos += 4 + len;
            }
            else if (type == 1)
                ok = codes(b, window, fixedLengths, fixedDistances);
            else if (type == 2)
                ok = dynamic(b, window);
            else
                ok = false;
            if (!ok)
                return false;
            finished = last;
        }
        catch (Underflow)
        {
            b = saved;
            window.truncate(mark);
            break;
        }
    }
    bitbuf = b.buf;
    bitcnt = b.cnt;
    pending.remove(0, b.pos);
    out = window.mid(start);
    if (window.size() > 32768)
        window = window.right(32768);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// RFB view.

static quint16 u16(const uchar *p)
{
    return qFromBigEndian<quint16>(p);
}

static quint32 u32(const uchar *p)
{
    return qFromBigEndian<quint32>(p);
}

// Requested pixel format: 32-bit little-endian, red<<16 green<<8 blue, i.e. QImage::Format_RGB32.
static quint32 pixel(const uchar *p)
{
    return qFromLittleEndian<quint32>(p) | 0xFF000000u;
}

static QByteArray be16(quint16 v)
{
    QByteArray b(2, 0);
    qToBigEndian(v, b.data());
    return b;
}

static QByteArray be32(quint32 v)
{
    QByteArray b(4, 0);
    qToBigEndian(v, b.data());
    return b;
}

VncView::VncView(const QString &host, quint16 port, std::function<QString()> passwordProvider, QWidget *parent)
    : QWidget(parent), onPasswordNeeded(std::move(passwordProvider)), socket(new QTcpSocket(this))
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    connect(socket, &QTcpSocket::readyRead, this,
            [this]
            {
                in += socket->readAll();
                if (busy)
                    return; // the password prompt opened a nested event loop
                busy = true;
                while (!closed && step())
                {
                }
                in.remove(0, rd);
                rd = 0;
                busy = false;
            });
    connect(socket, &QTcpSocket::disconnected, this, [this] { fail(ui("Connection closed by the server")); });
    connect(socket, &QTcpSocket::errorOccurred, this, [this] { fail(socket->errorString()); });
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this,
            [this]
            {
                const QString text = QGuiApplication::clipboard()->text();
                if (!connected || viewOnly || text == serverText)
                    return;
                serverText = text;
                const QByteArray latin = text.toLatin1(); // RFB: ClientCutText is Latin-1
                send(QByteArray("\x06\0\0\0", 4) + be32(quint32(latin.size())) + latin);
            });
    socket->connectToHost(host, port);
}

VncView::~VncView()
{
    closed = true;
    socket->disconnect(this);
    socket->abort();
}

void VncView::fail(const QString &reason)
{
    if (closed)
        return;
    closed = true;
    connected = false;
    lastError = reason;
    socket->abort();
    update();
    if (onDisconnected)
        onDisconnected(reason);
}

void VncView::send(const QByteArray &data)
{
    if (!closed)
        socket->write(data);
}

bool VncView::failWithReason(int skip, const char *fallback)
{
    if (!has(skip + 4))
        return false;
    const quint32 len = u32(at() + skip);
    if (len > (1u << 16))
        fail(ui(fallback));
    else if (has(skip + 4 + qint64(len)))
    {
        const QString text = QString::fromUtf8(in.mid(rd + skip + 4, len));
        fail(text.isEmpty() ? ui(fallback) : text);
    }
    return false;
}

void VncView::requestUpdate(bool incremental)
{
    send(QByteArray(1, 3) + char(incremental) + be16(0) + be16(0) + be16(quint16(fb.width())) + be16(quint16(fb.height())));
}

bool VncView::step()
{
    switch (state)
    {
    case Version:
    {
        if (!has(12))
            return false;
        const QByteArray v = in.mid(rd, 12);
        rd += 12;
        const int major = v.mid(4, 3).toInt(), m = v.mid(8, 3).toInt();
        if (!v.startsWith("RFB ") || v[11] != '\n' || major < 3)
        {
            fail(ui("The server is not a VNC server"));
            return false;
        }
        minor = major > 3 || m >= 8 ? 8 : m == 7 ? 7 : 3;
        send("RFB 003.00" + QByteArray::number(minor) + "\n");
        state = Security;
        return true;
    }
    case Security:
    {
        int type;
        if (minor == 3)
        {
            if (!has(4))
                return false;
            type = int(u32(at()));
            if (type == 0)
                return failWithReason(4, "Connection refused by the server");
            rd += 4;
        }
        else
        {
            if (!has(1))
                return false;
            const int n = at()[0];
            if (n == 0)
                return failWithReason(1, "Connection refused by the server");
            if (!has(1 + n))
                return false;
            const QByteArray types = in.mid(rd + 1, n);
            rd += 1 + n;
            type = types.contains(char(1)) ? 1 : types.contains(char(2)) ? 2 : 0;
            if (type)
                send(QByteArray(1, char(type)));
        }
        if (type != 1 && type != 2)
        {
            fail(ui("The server requires an unsupported security type"));
            return false;
        }
        if (type == 2)
            state = Challenge;
        else if (minor == 8)
            state = Result;
        else
        {
            send(QByteArray(1, 1)); // ClientInit: shared session
            state = Init;
        }
        return true;
    }
    case Challenge:
    {
        if (!has(16))
            return false;
        const QByteArray challenge = in.mid(rd, 16);
        rd += 16;
        const QString password = onPasswordNeeded ? onPasswordNeeded() : QString();
        if (closed)
            return false;
        if (password.isEmpty())
        {
            fail(ui("Authentication cancelled"));
            return false;
        }
        send(vncAuthResponse(password.toLatin1(), challenge));
        state = Result;
        return true;
    }
    case Result:
        if (!has(4))
            return false;
        if (u32(at()) != 0)
        {
            if (minor == 8)
                return failWithReason(4, "Authentication failed");
            fail(ui("Authentication failed"));
            return false;
        }
        rd += 4;
        send(QByteArray(1, 1));
        state = Init;
        return true;
    case Init:
    {
        if (!has(24) || !has(24 + qint64(u32(at() + 20))))
            return false;
        const int w = u16(at()), h = u16(at() + 2);
        const quint32 len = u32(at() + 20);
        name = QString::fromUtf8(in.mid(rd + 24, len));
        rd += 24 + len;
        fb = QImage(w, h, QImage::Format_RGB32);
        if (fb.isNull())
        {
            fail(ui("Desktop too large"));
            return false;
        }
        fb.fill(Qt::black);
        // SetPixelFormat: 32 bpp, depth 24, little-endian, true colour, max 255, shift 16/8/0.
        send(QByteArray("\0\0\0\0\x20\x18\0\x01\0\xff\0\xff\0\xff\x10\x08\0\0\0\0", 20));
        const qint32 encodings[] = {16, 5, 2, 1, 0, -223}; // ZRLE, Hextile, RRE, CopyRect, Raw, DesktopSize
        QByteArray message = QByteArray(" ", 2) + be16(quint16(std::size(encodings)));
        for (qint32 e : encodings)
            message += be32(quint32(e));
        send(message);
        requestUpdate(false);
        connected = true;
        state = Message;
        updateGeometry();
        update();
        return true;
    }
    case Message:
    {
        if (!has(1))
            return false;
        switch (at()[0])
        {
        case 0:
            if (!has(4))
                return false;
            rects = u16(at() + 2);
            rd += 4;
            inRect = false;
            state = Rects;
            return true;
        case 1:
            if (!has(6) || !has(6 + 6 * qint64(u16(at() + 4))))
                return false;
            rd += 6 + 6 * qint64(u16(at() + 4));
            return true;
        case 2:
            rd += 1; // bell
            return true;
        case 3:
        {
            if (!has(8))
                return false;
            const quint32 len = u32(at() + 4);
            if (len > (64u << 20))
            {
                fail(ui("Invalid data from the VNC server"));
                return false;
            }
            if (!has(8 + qint64(len)))
                return false;
            const QByteArray text = in.mid(rd + 8, len);
            rd += 8 + len;
            // ServerCutText is Latin-1 by spec, but many servers send UTF-8.
            QStringDecoder utf8(QStringDecoder::Utf8);
            serverText = utf8(text);
            if (utf8.hasError())
                serverText = QString::fromLatin1(text);
            QGuiApplication::clipboard()->setText(serverText);
            return true;
        }
        default:
            fail(ui("Invalid data from the VNC server"));
            return false;
        }
    }
    case Rects:
        if (rects == 0)
        {
            state = Message;
            requestUpdate(!fullNext);
            fullNext = false;
            update();
            return true;
        }
        if (!inRect)
        {
            if (!has(12))
                return false;
            rx = u16(at());
            ry = u16(at() + 2);
            rw = u16(at() + 4);
            rh = u16(at() + 6);
            encoding = qint32(u32(at() + 8));
            rd += 12;
            if (encoding != -223 && (rx + rw > fb.width() || ry + rh > fb.height()))
            {
                fail(ui("Invalid data from the VNC server"));
                return false;
            }
            inRect = true;
            tile = 0;
            bg = fg = 0xFF000000u;
        }
        if (!decodeRect())
            return false;
        inRect = false;
        --rects;
        return true;
    }
    return false;
}

void VncView::putPixels(int x, int y, int w, int h, const uchar *p)
{
    for (int j = 0; j < h; ++j)
    {
        auto *line = reinterpret_cast<quint32 *>(fb.scanLine(y + j)) + x;
        for (int i = 0; i < w; ++i, p += 4)
            line[i] = pixel(p);
    }
}

void VncView::fill(int x, int y, int w, int h, quint32 colour)
{
    for (int j = 0; j < h; ++j)
        std::fill_n(reinterpret_cast<quint32 *>(fb.scanLine(y + j)) + x, w, colour);
}

// Decodes the current rectangle; false if more data is needed (or if the connection was closed).
bool VncView::decodeRect()
{
    switch (encoding)
    {
    case 0: // Raw
    {
        const qint64 n = qint64(rw) * rh * 4;
        if (!has(n))
            return false;
        putPixels(rx, ry, rw, rh, at());
        rd += n;
        return true;
    }
    case 1: // CopyRect
    {
        if (!has(4))
            return false;
        const int sx = u16(at()), sy = u16(at() + 2);
        rd += 4;
        if (sx + rw > fb.width() || sy + rh > fb.height())
        {
            fail(ui("Invalid data from the VNC server"));
            return false;
        }
        const QImage source = fb.copy(sx, sy, rw, rh);
        for (int j = 0; j < rh; ++j)
            std::copy_n(reinterpret_cast<const quint32 *>(source.constScanLine(j)), rw,
                        reinterpret_cast<quint32 *>(fb.scanLine(ry + j)) + rx);
        return true;
    }
    case 2: // RRE
    {
        if (!has(8))
            return false;
        const qint64 n = u32(at()), size = 8 + n * 12;
        if (!has(size))
            return false;
        fill(rx, ry, rw, rh, pixel(at() + 4));
        for (const uchar *p = at() + 8; p < at() + size; p += 12)
        {
            const int x = u16(p + 4), y = u16(p + 6), w = u16(p + 8), h = u16(p + 10);
            if (x + w > rw || y + h > rh)
            {
                fail(ui("Invalid data from the VNC server"));
                return false;
            }
            fill(rx + x, ry + y, w, h, pixel(p));
        }
        rd += size;
        return true;
    }
    case 5: // Hextile: 16x16 tiles consumed one at a time, background/foreground persist within the rectangle
    {
        const int across = (rw + 15) / 16, count = across * ((rh + 15) / 16);
        for (; tile < count; ++tile)
        {
            const int tx = rx + tile % across * 16, ty = ry + tile / across * 16;
            const int tw = qMin(16, rx + rw - tx), th = qMin(16, ry + rh - ty);
            if (!has(1))
                return false;
            const int mask = at()[0];
            qint64 need = 1;
            if (mask & 1)
                need += tw * th * 4;
            else
            {
                need += (mask & 2 ? 4 : 0) + (mask & 4 ? 4 : 0);
                if (mask & 8)
                {
                    if (!has(need + 1))
                        return false;
                    need += 1 + at()[need] * (mask & 16 ? 6 : 2);
                }
            }
            if (!has(need))
                return false;
            const uchar *p = at() + 1;
            if (mask & 1)
                putPixels(tx, ty, tw, th, p);
            else
            {
                if (mask & 2)
                    bg = pixel(p), p += 4;
                if (mask & 4)
                    fg = pixel(p), p += 4;
                fill(tx, ty, tw, th, bg);
                for (int n = mask & 8 ? *p++ : 0; n > 0; --n)
                {
                    quint32 colour = fg;
                    if (mask & 16)
                        colour = pixel(p), p += 4;
                    const int sx = p[0] >> 4, sy = p[0] & 15, sw = (p[1] >> 4) + 1, sh = (p[1] & 15) + 1;
                    p += 2;
                    if (sx < tw && sy < th)
                        fill(tx + sx, ty + sy, qMin(sw, tw - sx), qMin(sh, th - sy), colour);
                }
            }
            rd += need;
        }
        return true;
    }
    case 16: // ZRLE
    {
        if (!has(4) || !has(4 + qint64(u32(at()))))
            return false;
        const QByteArray data = in.mid(rd + 4, u32(at()));
        rd += 4 + data.size();
        QByteArray plain;
        if (!zlib.feed(data, plain) || !zrle(plain))
        {
            fail(ui("Invalid data from the VNC server"));
            return false;
        }
        return true;
    }
    case -223: // DesktopSize
        fb = QImage(rw, rh, QImage::Format_RGB32);
        if (fb.isNull())
        {
            fail(ui("Desktop too large"));
            return false;
        }
        fb.fill(Qt::black);
        fullNext = true;
        updateGeometry();
        return true;
    default:
        fail(ui("Unsupported VNC encoding"));
        return false;
    }
}

// ZRLE 64x64 tiles; CPIXEL = 3 bytes (the least significant bytes of the 32-bit little-endian pixel).
bool VncView::zrle(const QByteArray &data)
{
    const uchar *p = reinterpret_cast<const uchar *>(data.constData()), *end = p + data.size();
    auto cpixel = [&](quint32 &c)
    {
        if (end - p < 3)
            return false;
        c = 0xFF000000u | p[0] | p[1] << 8 | p[2] << 16;
        p += 3;
        return true;
    };
    for (int ty = ry; ty < ry + rh; ty += 64)
    {
        for (int tx = rx; tx < rx + rw; tx += 64)
        {
            const int tw = qMin(64, rx + rw - tx), th = qMin(64, ry + rh - ty), total = tw * th;
            auto px = [&](int i) -> quint32 & { return reinterpret_cast<quint32 *>(fb.scanLine(ty + i / tw))[tx + i % tw]; };
            if (p == end)
                return false;
            const int sub = *p++, colours = sub & 127;
            quint32 palette[128], c;
            if (sub == 1)
            {
                if (!cpixel(c))
                    return false;
                fill(tx, ty, tw, th, c);
                continue;
            }
            if (sub == 0)
            {
                for (int i = 0; i < total; ++i)
                {
                    if (!cpixel(px(i)))
                        return false;
                }
                continue;
            }
            if (sub == 129 || (sub > 16 && sub < 128))
                return false;
            for (int i = 0; sub != 128 && i < colours; ++i)
            {
                if (!cpixel(palette[i]))
                    return false;
            }
            if (sub <= 16) // packed palette, rows byte-aligned
            {
                const int bits = colours <= 2 ? 1 : colours <= 4 ? 2 : 4;
                for (int j = 0; j < th; ++j)
                {
                    int shift = 0, byte = 0;
                    for (int i = 0; i < tw; ++i)
                    {
                        if (shift == 0)
                        {
                            if (p == end)
                                return false;
                            byte = *p++;
                            shift = 8;
                        }
                        shift -= bits;
                        const int index = (byte >> shift) & ((1 << bits) - 1);
                        if (index >= colours)
                            return false;
                        px(j * tw + i) = palette[index];
                    }
                }
                continue;
            }
            for (int i = 0; i < total;) // plain RLE (128) or palette RLE (130-255)
            {
                if (sub == 128)
                {
                    if (!cpixel(c))
                        return false;
                }
                else
                {
                    if (p == end)
                        return false;
                    const int index = *p++;
                    if ((index & 127) >= colours)
                        return false;
                    c = palette[index & 127];
                    if (!(index & 128))
                    {
                        px(i++) = c;
                        continue;
                    }
                }
                int run = 1;
                do
                {
                    if (p == end)
                        return false;
                    run += *p;
                } while (*p++ == 255);
                if (run > total - i)
                    return false;
                while (run--)
                    px(i++) = c;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Input and painting.

QSize VncView::sizeHint() const
{
    return fb.isNull() ? QSize(800, 600) : fb.size();
}

void VncView::setScaled(bool on)
{
    scaled = on;
    update();
}

QRect VncView::target() const
{
    if (!scaled || fb.isNull())
        return QRect(QPoint(0, 0), fb.size());
    const QSize s = fb.size().scaled(size(), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - s.width()) / 2, (height() - s.height()) / 2), s);
}

void VncView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (!fb.isNull())
    {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, scaled);
        painter.drawImage(target(), fb);
    }
    if (!connected)
    {
        painter.setPen(Qt::white);
        painter.drawText(QWidget::rect(), Qt::AlignCenter | Qt::TextWordWrap,
                         lastError.isEmpty() ? ui("Connecting…") : lastError);
    }
}

bool VncView::event(QEvent *e)
{
    // Application shortcuts must not steal keys meant for the remote desktop.
    if (e->type() == QEvent::ShortcutOverride && connected && !viewOnly)
    {
        e->accept();
        return true;
    }
    return QWidget::event(e);
}

void VncView::sendKey(quint32 keysym, bool down)
{
    if (connected && !viewOnly && keysym)
        send(QByteArray(1, 4) + char(down) + be16(0) + be32(keysym));
}

void VncView::sendCtrlAltDel()
{
    for (quint32 k : {0xFFE3u, 0xFFE9u, 0xFFFFu})
        sendKey(k, true);
    for (quint32 k : {0xFFFFu, 0xFFE9u, 0xFFE3u})
        sendKey(k, false);
}

// Qt key -> X11 keysym: special keys from a table, otherwise the Unicode character (Latin-1 directly, beyond that 0x01000000|U).
static quint32 keysym(const QKeyEvent *e)
{
    static const QHash<int, quint32> special{
        {Qt::Key_Backspace, 0xFF08}, {Qt::Key_Tab, 0xFF09},        {Qt::Key_Backtab, 0xFF09}, {Qt::Key_Return, 0xFF0D},
        {Qt::Key_Enter, 0xFF8D},     {Qt::Key_Escape, 0xFF1B},     {Qt::Key_Insert, 0xFF63},  {Qt::Key_Delete, 0xFFFF},
        {Qt::Key_Home, 0xFF50},      {Qt::Key_End, 0xFF57},        {Qt::Key_PageUp, 0xFF55},  {Qt::Key_PageDown, 0xFF56},
        {Qt::Key_Left, 0xFF51},      {Qt::Key_Up, 0xFF52},         {Qt::Key_Right, 0xFF53},   {Qt::Key_Down, 0xFF54},
        {Qt::Key_Shift, 0xFFE1},     {Qt::Key_Control, 0xFFE3},    {Qt::Key_Alt, 0xFFE9},     {Qt::Key_AltGr, 0xFE03},
        {Qt::Key_Meta, 0xFFEB},      {Qt::Key_Super_L, 0xFFEB},    {Qt::Key_Super_R, 0xFFEC}, {Qt::Key_CapsLock, 0xFFE5},
        {Qt::Key_NumLock, 0xFF7F},   {Qt::Key_ScrollLock, 0xFF14}, {Qt::Key_Print, 0xFF61},   {Qt::Key_Pause, 0xFF13},
        {Qt::Key_Menu, 0xFF67}};
    const int key = e->key();
    if (special.contains(key))
        return special.value(key);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
        return 0xFFBE + (key - Qt::Key_F1);
    const QList<uint> text = e->text().toUcs4();
    if (text.size() == 1 && text[0] >= 0x20 && text[0] != 0x7F)
        return text[0] < 0x100 ? text[0] : 0x01000000 | text[0];
    // With Ctrl the text is a control character: derive the keysym from the key.
    if (key >= 0x20 && key <= 0xFF)
        return key >= 'A' && key <= 'Z' && !(e->modifiers() & Qt::ShiftModifier) ? key + 32 : key;
    return 0;
}

void VncView::keyPressEvent(QKeyEvent *e)
{
    const quint32 sym = keysym(e);
    if (!sym)
        return QWidget::keyPressEvent(e);
    pressed.insert(e->key(), sym);
    sendKey(sym, true);
}

void VncView::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat())
        return;
    // Release the same keysym that was pressed, even if the modifiers changed in the meantime.
    sendKey(pressed.contains(e->key()) ? pressed.take(e->key()) : keysym(e), false);
}

void VncView::focusOutEvent(QFocusEvent *e)
{
    for (quint32 sym : std::as_const(pressed))
        sendKey(sym, false);
    pressed.clear();
    QWidget::focusOutEvent(e);
}

void VncView::sendPointer(const QPointF &position)
{
    const QRect t = target();
    if (!connected || viewOnly || t.isEmpty())
        return;
    const int x = qBound(0, int((position.x() - t.x()) * fb.width() / t.width()), fb.width() - 1);
    const int y = qBound(0, int((position.y() - t.y()) * fb.height() / t.height()), fb.height() - 1);
    send(QByteArray(1, 5) + char(buttons) + be16(quint16(x)) + be16(quint16(y)));
}

static int buttonMask(Qt::MouseButton b)
{
    return b == Qt::LeftButton ? 1 : b == Qt::MiddleButton ? 2 : b == Qt::RightButton ? 4 : 0;
}

void VncView::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    buttons |= buttonMask(e->button());
    sendPointer(e->position());
}

void VncView::mouseReleaseEvent(QMouseEvent *e)
{
    buttons &= ~buttonMask(e->button());
    sendPointer(e->position());
}

void VncView::mouseMoveEvent(QMouseEvent *e)
{
    sendPointer(e->position());
}

void VncView::wheelEvent(QWheelEvent *e)
{
    // ponytail: one click per event; accumulate angleDelta if high-resolution touchpads scroll too much.
    const int bit = e->angleDelta().y() > 0 ? 8 : e->angleDelta().y() < 0 ? 16 : 0;
    if (!bit)
        return;
    buttons |= bit;
    sendPointer(e->position());
    buttons &= ~bit;
    sendPointer(e->position());
}
