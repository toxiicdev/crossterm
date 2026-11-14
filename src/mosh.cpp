#include "mosh.h"
#include "terminal.h"
#include "vnc.h" // VncInflater: inflate with an output limit
#include <QElapsedTimer>
#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkDatagram>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

bool parseMoshConnect(const QByteArray &serverOutput, int &port, QByteArray &key)
{
    static const QRegularExpression re("MOSH CONNECT (\\d{1,5}) ([A-Za-z0-9/+]{22})(?![A-Za-z0-9/+=])");
    const auto m = re.match(QString::fromLatin1(serverOutput));
    if (!m.hasMatch() || m.captured(1).toInt() < 1 || m.captured(1).toInt() > 65535)
        return false;
    port = m.captured(1).toInt();
    key = m.captured(2).toLatin1();
    return true;
}

QStringList moshServerArguments(const QString &portRange)
{
    QStringList args{"mosh-server", "new", "-s", "-c", "256", "-l", "LANG=en_US.UTF-8"};
    if (!portRange.isEmpty())
        args << "-p" << portRange;
    return args;
}

namespace mosh
{
namespace
{
uint8_t xt(uint8_t x)
{
    return uint8_t((x << 1) ^ ((x >> 7) * 0x1b));
}

// Computed S-box: inverse in GF(2^8) followed by the affine transform.
struct Sbox
{
    uint8_t s[256], inv[256];
    Sbox()
    {
        for (int x = 0; x < 256; ++x)
        {
            uint8_t b = 0;
            for (int y = 1; y < 256 && x; ++y)
            {
                uint8_t a = uint8_t(x), m = uint8_t(y), p = 0;
                for (; m; m >>= 1, a = xt(a))
                    if (m & 1)
                        p ^= a;
                if (p == 1)
                {
                    b = uint8_t(y);
                    break;
                }
            }
            auto rot = [b](int n) { return uint8_t((b << n) | (b >> (8 - n))); };
            s[x] = uint8_t(b ^ rot(1) ^ rot(2) ^ rot(3) ^ rot(4) ^ 0x63);
            inv[s[x]] = uint8_t(x);
        }
    }
};
const Sbox &sbox()
{
    static const Sbox table;
    return table;
}

void mixColumns(Block &b)
{
    for (int c = 0; c < 16; c += 4)
    {
        const uint8_t a0 = b[c], a1 = b[c + 1], a2 = b[c + 2], a3 = b[c + 3], t = a0 ^ a1 ^ a2 ^ a3;
        b[c] ^= t ^ xt(a0 ^ a1);
        b[c + 1] ^= t ^ xt(a1 ^ a2);
        b[c + 2] ^= t ^ xt(a2 ^ a3);
        b[c + 3] ^= t ^ xt(a3 ^ a0);
    }
}

void addKey(Block &b, const uint8_t *k)
{
    for (int i = 0; i < 16; ++i)
        b[i] ^= k[i];
}

Block xorBlock(Block a, const Block &b)
{
    for (int i = 0; i < 16; ++i)
        a[i] ^= b[i];
    return a;
}

Block twice(const Block &b)
{
    Block r;
    for (int i = 0; i < 16; ++i)
        r[i] = uint8_t((b[i] << 1) | (i < 15 ? b[i + 1] >> 7 : 0));
    if (b[0] & 0x80)
        r[15] ^= 0x87;
    return r;
}

Block load(const char *p, int n = 16)
{
    Block b{};
    memcpy(b.data(), p, size_t(n));
    return b;
}

int ntz(quint64 i)
{
    int n = 0;
    for (; !(i & 1); i >>= 1)
        ++n;
    return n;
}
} // namespace

Aes128::Aes128(const uint8_t *key16)
{
    const auto &s = sbox().s;
    memcpy(rk, key16, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4)
    {
        uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 16 == 0)
        {
            const uint8_t t0 = t[0];
            t[0] = uint8_t(s[t[1]] ^ rcon);
            t[1] = s[t[2]];
            t[2] = s[t[3]];
            t[3] = s[t0];
            rcon = xt(rcon);
        }
        for (int j = 0; j < 4; ++j)
            rk[i + j] = rk[i + j - 16] ^ t[j];
    }
}

Block Aes128::encrypt(Block b) const
{
    const auto &s = sbox().s;
    addKey(b, rk);
    for (int r = 1; r <= 10; ++r)
    {
        Block t;
        for (int i = 0; i < 16; ++i) // SubBytes + ShiftRows (column-major state)
            t[i] = s[b[(i + 4 * (i % 4)) % 16]];
        b = t;
        if (r < 10)
            mixColumns(b);
        addKey(b, rk + 16 * r);
    }
    return b;
}

Block Aes128::decrypt(Block b) const
{
    const auto &inv = sbox().inv;
    for (int r = 10; r >= 1; --r)
    {
        addKey(b, rk + 16 * r);
        if (r < 10) // InvMixColumns = preprocessing + MixColumns
        {
            for (int c = 0; c < 16; c += 4)
            {
                const uint8_t u = xt(xt(b[c] ^ b[c + 2])), v = xt(xt(b[c + 1] ^ b[c + 3]));
                b[c] ^= u;
                b[c + 1] ^= v;
                b[c + 2] ^= u;
                b[c + 3] ^= v;
            }
            mixColumns(b);
        }
        Block t;
        for (int i = 0; i < 16; ++i) // InvShiftRows + InvSubBytes
            t[(i + 4 * (i % 4)) % 16] = inv[b[i]];
        b = t;
    }
    addKey(b, rk);
    return b;
}

Ocb::Ocb(const QByteArray &key16) : aes(reinterpret_cast<const uint8_t *>(key16.leftJustified(16, '\0').constData()))
{
    lStar = aes.encrypt(Block{});
    lDollar = twice(lStar);
    l[0] = twice(lDollar);
    for (int i = 1; i < 32; ++i)
        l[i] = twice(l[i - 1]);
}

Block Ocb::offset0(const QByteArray &nonce12) const
{
    Block n{0, 0, 0, 1}; // TAGLEN 128 and 96-bit nonce
    memcpy(n.data() + 4, nonce12.constData(), 12);
    const int bottom = n[15] & 63;
    n[15] &= 0xC0;
    const Block top = aes.encrypt(n);
    uint8_t stretch[24];
    memcpy(stretch, top.data(), 16);
    for (int i = 0; i < 8; ++i)
        stretch[16 + i] = top[i] ^ top[i + 1];
    Block off;
    const int by = bottom / 8, bit = bottom % 8;
    for (int i = 0; i < 16; ++i)
        off[i] = uint8_t((stretch[i + by] << bit) | (bit ? stretch[i + by + 1] >> (8 - bit) : 0));
    return off;
}

Block Ocb::hash(const QByteArray &ad) const
{
    Block sum{}, off{};
    const qsizetype full = ad.size() / 16;
    for (qsizetype i = 0; i < full; ++i)
    {
        off = xorBlock(off, l[ntz(quint64(i + 1))]);
        sum = xorBlock(sum, aes.encrypt(xorBlock(load(ad.constData() + 16 * i), off)));
    }
    if (const int rest = int(ad.size() % 16))
    {
        Block last = load(ad.constData() + 16 * full, rest);
        last[rest] = 0x80;
        sum = xorBlock(sum, aes.encrypt(xorBlock(last, xorBlock(off, lStar))));
    }
    return sum;
}

QByteArray Ocb::encrypt(const QByteArray &nonce12, const QByteArray &plain, const QByteArray &ad) const
{
    Block off = offset0(nonce12), sum{};
    QByteArray out(plain.size() + 16, Qt::Uninitialized);
    const qsizetype full = plain.size() / 16;
    for (qsizetype i = 0; i < full; ++i)
    {
        off = xorBlock(off, l[ntz(quint64(i + 1))]);
        const Block p = load(plain.constData() + 16 * i);
        sum = xorBlock(sum, p);
        const Block c = xorBlock(aes.encrypt(xorBlock(p, off)), off);
        memcpy(out.data() + 16 * i, c.data(), 16);
    }
    if (const int rest = int(plain.size() % 16))
    {
        off = xorBlock(off, lStar);
        const Block pad = aes.encrypt(off);
        Block p = load(plain.constData() + 16 * full, rest);
        p[rest] = 0x80;
        sum = xorBlock(sum, p);
        for (int j = 0; j < rest; ++j)
            out[16 * full + j] = char(p[j] ^ pad[j]);
    }
    const Block tag = xorBlock(aes.encrypt(xorBlock(xorBlock(sum, off), lDollar)), hash(ad));
    memcpy(out.data() + plain.size(), tag.data(), 16);
    return out;
}

bool Ocb::decrypt(const QByteArray &nonce12, const QByteArray &sealed, QByteArray &plain, const QByteArray &ad) const
{
    if (sealed.size() < 16)
        return false;
    const qsizetype len = sealed.size() - 16, full = len / 16;
    Block off = offset0(nonce12), sum{};
    QByteArray out(len, Qt::Uninitialized);
    for (qsizetype i = 0; i < full; ++i)
    {
        off = xorBlock(off, l[ntz(quint64(i + 1))]);
        const Block p = xorBlock(aes.decrypt(xorBlock(load(sealed.constData() + 16 * i), off)), off);
        sum = xorBlock(sum, p);
        memcpy(out.data() + 16 * i, p.data(), 16);
    }
    if (const int rest = int(len % 16))
    {
        off = xorBlock(off, lStar);
        const Block pad = aes.encrypt(off);
        Block p{};
        for (int j = 0; j < rest; ++j)
            out[16 * full + j] = char(p[j] = uint8_t(sealed[16 * full + j]) ^ pad[j]);
        p[rest] = 0x80;
        sum = xorBlock(sum, p);
    }
    const Block tag = xorBlock(aes.encrypt(xorBlock(xorBlock(sum, off), lDollar)), hash(ad));
    uint8_t diff = 0; // constant-time comparison
    for (int i = 0; i < 16; ++i)
        diff |= tag[i] ^ uint8_t(sealed[len + i]);
    if (diff)
        return false;
    plain = out;
    return true;
}

// --- minimal protobuf ---
namespace
{
void putVarint(QByteArray &o, quint64 v)
{
    for (; v >= 0x80; v >>= 7)
        o += char(v | 0x80);
    o += char(v);
}
void putVarintField(QByteArray &o, int field, quint64 v)
{
    putVarint(o, quint64(field) << 3);
    putVarint(o, v);
}
void putBytesField(QByteArray &o, int field, const QByteArray &b)
{
    putVarint(o, quint64(field) << 3 | 2);
    putVarint(o, quint64(b.size()));
    o += b;
}

// Reads one field at a time; fixed-length types are skipped.
struct Reader
{
    const QByteArray &d;
    qsizetype pos = 0;
    bool ok = true;
    quint64 varint()
    {
        quint64 v = 0;
        for (int shift = 0; shift < 64; shift += 7)
        {
            if (pos >= d.size())
                break;
            const uint8_t b = uint8_t(d[pos++]);
            v |= quint64(b & 0x7f) << shift;
            if (!(b & 0x80))
                return v;
        }
        ok = false;
        return 0;
    }
    bool next(int &field, quint64 &value, QByteArray &bytes)
    {
        if (!ok || pos >= d.size())
            return false;
        const quint64 tag = varint();
        field = int(tag >> 3);
        switch (tag & 7)
        {
        case 0:
            value = varint();
            break;
        case 1:
        case 5:
            pos += (tag & 7) == 1 ? 8 : 4;
            break;
        case 2:
            value = varint();
            if (value > quint64(d.size() - pos))
                ok = false;
            else
                bytes = d.mid(pos, qsizetype(value)), pos += qsizetype(value);
            break;
        default:
            ok = false;
        }
        ok = ok && pos <= d.size();
        return ok;
    }
};
} // namespace

QByteArray encodeInstruction(const Instruction &inst)
{
    QByteArray o;
    putVarintField(o, 1, inst.protocolVersion);
    putVarintField(o, 2, inst.oldNum);
    putVarintField(o, 3, inst.newNum);
    putVarintField(o, 4, inst.ackNum);
    putVarintField(o, 5, inst.throwawayNum);
    putBytesField(o, 6, inst.diff);
    if (!inst.chaff.isEmpty())
        putBytesField(o, 7, inst.chaff);
    return o;
}

bool decodeInstruction(const QByteArray &data, Instruction &inst)
{
    inst = Instruction{};
    inst.protocolVersion = 0;
    Reader r{data};
    int f;
    quint64 v = 0;
    QByteArray b;
    while (r.next(f, v, b))
    {
        switch (f)
        {
        case 1:
            inst.protocolVersion = quint32(v);
            break;
        case 2:
            inst.oldNum = v;
            break;
        case 3:
            inst.newNum = v;
            break;
        case 4:
            inst.ackNum = v;
            break;
        case 5:
            inst.throwawayNum = v;
            break;
        case 6:
            inst.diff = b;
            break;
        case 7:
            inst.chaff = b;
            break;
        }
    }
    return r.ok;
}

QByteArray encodeActions(const QList<Action> &actions)
{
    QByteArray o;
    for (const Action &a : actions)
    {
        QByteArray inner, ext;
        if (a.width >= 0)
        {
            putVarintField(inner, 5, quint64(a.width));
            putVarintField(inner, 6, quint64(a.height));
            putBytesField(ext, 3, inner);
        }
        else if (a.echoAck >= 0)
        {
            putVarintField(inner, 8, quint64(a.echoAck));
            putBytesField(ext, 7, inner);
        }
        else
        {
            putBytesField(inner, 4, a.bytes);
            putBytesField(ext, 2, inner);
        }
        putBytesField(o, 1, ext);
    }
    return o;
}

bool decodeActions(const QByteArray &data, QList<Action> &actions)
{
    actions.clear();
    Reader r{data};
    int f, g;
    quint64 v = 0, w = 0;
    QByteArray ext, sub, inner;
    while (r.next(f, v, ext))
    {
        if (f != 1)
            continue;
        Action a;
        Reader re{ext};
        while (re.next(g, v, sub))
        {
            if (g != 2 && g != 3 && g != 7)
                continue;
            Reader ri{sub};
            int h;
            while (ri.next(h, w, inner))
            {
                if (g == 2 && h == 4)
                    a.bytes = inner;
                else if (g == 3 && h == 5)
                    a.width = int(w);
                else if (g == 3 && h == 6)
                    a.height = int(w);
                else if (g == 7 && h == 8)
                    a.echoAck = qint64(w & 0x7fffffffffffffffULL);
            }
            if (!ri.ok)
                return false;
            if (g == 3 && a.height < 0)
                a.height = 0;
            if (g == 3 && a.width < 0)
                a.width = 0;
        }
        if (!re.ok)
            return false;
        actions << a;
    }
    return r.ok;
}

QByteArray zlibCompress(const QByteArray &data)
{
    return qCompress(data).mid(4); // strips the big-endian length prepended by Qt
}

QByteArray zlibUncompress(const QByteArray &data)
{
    // Inflated with a hard limit (qUncompress has none: a few kilobytes of zeros expand to gigabytes). mosh never
    // sends more than 4 MiB; a larger, corrupt or cut stream gives nothing, which is dropped like any bad packet.
    VncInflater inflater;
    QByteArray plain;
    if (!inflater.feed(data, plain, 4 << 20) || !inflater.atEnd() || inflater.unread() < 4)
        return {};
    quint32 a = 1, b = 0; // Adler-32 of the content, which follows the stream
    for (const char ch : plain)
    {
        a = (a + uchar(ch)) % 65521;
        b = (b + a) % 65521;
    }
    if (qFromBigEndian<quint32>(data.constData() + data.size() - inflater.unread()) != (b << 16 | a))
        return {};
    return plain;
}

QByteArray seal(const Ocb &ocb, const Packet &packet)
{
    QByteArray nonce(12, '\0'), plain(4, '\0');
    qToBigEndian(quint64(packet.toClient) << 63 | (packet.seq & 0x7fffffffffffffffULL), nonce.data() + 4);
    qToBigEndian(packet.timestamp, plain.data());
    qToBigEndian(packet.timestampReply, plain.data() + 2);
    return nonce.mid(4) + ocb.encrypt(nonce, plain + packet.payload);
}

bool open(const Ocb &ocb, const QByteArray &datagram, Packet &packet)
{
    QByteArray plain;
    if (datagram.size() < 8 + 16 + 4 || !ocb.decrypt(QByteArray(4, '\0') + datagram.left(8), datagram.mid(8), plain))
        return false;
    const quint64 n = qFromBigEndian<quint64>(datagram.constData());
    packet.toClient = n >> 63;
    packet.seq = n & 0x7fffffffffffffffULL;
    packet.timestamp = qFromBigEndian<quint16>(plain.constData());
    packet.timestampReply = qFromBigEndian<quint16>(plain.constData() + 2);
    packet.payload = plain.mid(4);
    return true;
}

QList<QByteArray> makeFragments(quint64 id, const Instruction &inst, int maxContent)
{
    const QByteArray payload = zlibCompress(encodeInstruction(inst));
    QList<QByteArray> out;
    for (qsizetype pos = 0; pos < payload.size() || out.isEmpty(); pos += maxContent)
    {
        QByteArray f(10, '\0');
        const bool final = pos + maxContent >= payload.size();
        qToBigEndian(id, f.data());
        qToBigEndian(quint16((final ? 0x8000 : 0) | out.size()), f.data() + 8);
        out << f + payload.mid(pos, maxContent);
    }
    return out;
}

bool Assembler::add(const QByteArray &fragment, Instruction &inst)
{
    if (fragment.size() < 10)
        return false;
    const quint64 fid = qFromBigEndian<quint64>(fragment.constData());
    const quint16 n = qFromBigEndian<quint16>(fragment.constData() + 8);
    const int num = n & 0x7fff;
    if (fid != id)
        id = fid, total = -1, bytes = 0, parts.clear();
    if (parts.size() <= num)
        parts.resize(num + 1);
    bytes += fragment.size() - 10 - parts[num].size();
    parts[num] = fragment.mid(10);
    if (bytes > (8 << 20)) // mosh never sends more than 4 MB: drop it instead of buffering without limit
    {
        total = -1, bytes = 0, parts.clear();
        return false;
    }
    if (n & 0x8000)
        total = num + 1;
    if (total < 0 || parts.size() != total)
        return false;
    QByteArray whole;
    for (const QByteArray &p : parts)
    {
        if (p.isNull())
            return false; // missing fragment
        whole += p;
    }
    parts.clear();
    total = -1;
    bytes = 0;
    const QByteArray plain = zlibUncompress(whole);
    return !plain.isEmpty() && decodeInstruction(plain, inst); // empty: corrupt or oversized
}
} // namespace mosh

// --- session ---
namespace
{
constexpr int AckInterval = 3000, AckDelay = 100;    // heartbeat and maximum ack delay (same values as mosh)
constexpr int FirstContact = 15000, Silence = 60000; // with no reply from the server the session ends
constexpr int ActiveRetry = 10000;                   // longer silence: no more retransmissions, only heartbeats (as mosh)
constexpr int Quiet = 4000;                          // silence reported to onSilence (the server heartbeat is 3 s)
constexpr quint64 Shutdown = ~0ULL;
} // namespace

struct MoshDevice::State
{
    QUdpSocket socket;
    QHostAddress address;
    quint16 port = 0;
    std::unique_ptr<mosh::Ocb> ocb;
    QTimer timer;
    QElapsedTimer clock;
    bool ended = false;
    // Sending: the user state is the event list; a sent state is identified by its event count.
    struct Sent
    {
        quint64 num;
        qint64 time;
        qint64 count;
    };
    QList<Sent> sent{{0, -1000000, 0}}; // the first one is the last state acked by the server
    QList<mosh::Action> events;
    qint64 eventBase = 0; // events already acked and discarded
    quint64 seq = 0, fragmentId = 0;
    qint64 nextAck = 0;
    int lastCols = 80, lastRows = 24;
    // Receiving
    quint64 remoteNum = 0, expectedSeq = 0;
    mosh::Assembler assembler;
    int savedTimestamp = -1;
    qint64 savedAt = 0, lastHeard = -1;
    double srtt = 1000, rttvar = 500;
    bool rttHit = false;
    bool quiet = false; // silent for Quiet ms: onSilence called
    QByteArray readBuffer;

    int rto() const { return qBound(50, int(std::ceil(srtt + 4 * rttvar)), 1000); }
    int sendInterval() const { return qBound(20, int(std::ceil(srtt / 2)), 250); }
    qint64 current() const { return eventBase + events.size(); }

    void sendInstruction(quint64 oldNum, quint64 newNum, const QByteArray &diff)
    {
        const qint64 now = clock.elapsed();
        mosh::Instruction inst;
        inst.oldNum = oldNum;
        inst.newNum = newNum;
        inst.ackNum = remoteNum;
        inst.throwawayNum = sent.first().num;
        inst.diff = diff;
        for (const QByteArray &f : mosh::makeFragments(fragmentId++, inst))
        {
            mosh::Packet p;
            p.seq = seq++;
            p.timestamp = quint16(now) == 0xFFFF ? 0 : quint16(now); // 0xFFFF = no timestamp
            if (savedTimestamp >= 0 && now - savedAt < 1000)         // reply adjusted for the hold time
                p.timestampReply = quint16(savedTimestamp + (now - savedAt));
            savedTimestamp = -1;
            p.payload = f;
            socket.writeDatagram(mosh::seal(*ocb, p), address, port);
        }
        nextAck = now + AckInterval;
    }
    QByteArray diffFrom(qint64 count) const { return mosh::encodeActions(events.mid(count - eventBase)); }
};

MoshDevice::MoshDevice(const QString &host, quint16 udpPort, const QByteArray &keyBase64, QObject *parent)
    : QIODevice(parent), d(std::make_unique<State>())
{
    const auto key = QByteArray::fromBase64Encoding(keyBase64 + "==", QByteArray::AbortOnBase64DecodingErrors);
    if (keyBase64.size() != 22 || !key || key.decoded.size() != 16)
    {
        setErrorString(tr("Invalid mosh session key"));
        return;
    }
    d->ocb = std::make_unique<mosh::Ocb>(key.decoded);
    d->port = udpPort;
    d->clock.start();
    d->socket.bind(QHostAddress(QHostAddress::Any), 0);
    QObject::connect(&d->socket, &QUdpSocket::readyRead, this, [this] { receive(); });
    QObject::connect(&d->timer, &QTimer::timeout, this, [this] { tick(); });
    d->timer.setSingleShot(true); // re-armed by tick() to the next deadline
    open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    d->events << mosh::Action{{}, d->lastCols, d->lastRows}; // the server learns the client from the first packet
    if (!d->address.setAddress(host))
        QHostInfo::lookupHost(host, this,
                              [this](const QHostInfo &info)
                              {
                                  if (info.addresses().isEmpty())
                                      return finish(tr("Cannot resolve mosh host: %1").arg(info.errorString()));
                                  // Prefer IPv4: with -s the server listens on the address ssh used, usually IPv4.
                                  d->address = info.addresses().first();
                                  for (const QHostAddress &a : info.addresses())
                                      if (a.protocol() == QAbstractSocket::IPv4Protocol)
                                      {
                                          d->address = a;
                                          break;
                                      }
                                  tick();
                              });
    tick();
}

MoshDevice::~MoshDevice()
{
    MoshDevice::close();
}

void MoshDevice::setWindowSize(int cols, int rows)
{
    if (d->ended || !d->ocb || (cols == d->lastCols && rows == d->lastRows))
        return;
    d->lastCols = cols;
    d->lastRows = rows;
    d->events << mosh::Action{{}, cols, rows};
    tick();
}

qint64 MoshDevice::bytesAvailable() const
{
    return d->readBuffer.size() + QIODevice::bytesAvailable();
}

qint64 MoshDevice::readData(char *data, qint64 maxSize)
{
    const qint64 n = qMin<qint64>(maxSize, d->readBuffer.size());
    memcpy(data, d->readBuffer.constData(), size_t(n));
    d->readBuffer.remove(0, n);
    return n;
}

qint64 MoshDevice::writeData(const char *data, qint64 size)
{
    if (d->ended)
        return -1;
    d->events << mosh::Action{QByteArray(data, size)};
    tick();
    return size;
}

void MoshDevice::close()
{
    if (!d->ended && d->ocb && !d->address.isNull())
    {
        // Client-side close: "infinite" state with the unacked events; does not wait for an ack.
        const QByteArray diff = d->diffFrom(d->sent.first().count);
        for (int i = 0; i < 3; ++i)
            d->sendInstruction(d->sent.first().num, Shutdown, diff);
    }
    d->ended = true;
    d->timer.stop();
    d->socket.close();
    if (isOpen())
        QIODevice::close();
}

void MoshDevice::finish(const QString &reason)
{
    if (d->ended)
        return;
    d->ended = true;
    setErrorString(reason);
    emit readChannelFinished();
    close();
}

void MoshDevice::tick()
{
    State &s = *d;
    const qint64 now = s.clock.elapsed();
    if (s.ended || s.address.isNull())
        return;
    if (s.lastHeard < 0 ? now > FirstContact : now - s.lastHeard > Silence)
        return finish(s.lastHeard < 0 ? tr("No response from mosh-server (is the UDP port reachable?)")
                                      : tr("mosh-server stopped responding"));
    if (!s.quiet && s.lastHeard >= 0 && now - s.lastHeard >= Quiet)
    {
        s.quiet = true;
        if (onSilence)
            onSilence();
    }
    // State the server has probably received: those sent less than an RTO ago count as received.
    const auto assumedState = [&s, now]
    {
        int i = 0;
        while (i + 1 < s.sent.size() && now - s.sent[i + 1].time < s.rto() + AckDelay)
            ++i;
        return i;
    };
    int assumed = assumedState();
    const qint64 cur = s.current();
    bool pending = cur != s.sent[assumed].count;
    // New states always go out; retransmissions only while the server answers (heartbeats carry the diff).
    const bool retrying = now - qMax<qint64>(s.lastHeard, 0) < ActiveRetry;
    const bool sendDue = pending && (retrying || cur != s.sent.last().count) && now - s.sent.last().time >= s.sendInterval();
    const bool ackDue = now >= s.nextAck || s.remoteNum == Shutdown;
    if (sendDue || ackDue)
    {
        const quint64 oldNum = s.sent[assumed].num;
        const QByteArray diff = pending ? s.diffFrom(s.sent[assumed].count) : QByteArray();
        if (pending && cur == s.sent.last().count)
            s.sent.last().time = now; // retransmission of the same state
        else
            s.sent << State::Sent{s.sent.last().num + 1, now, cur};
        if (s.sent.size() > 32)
            s.sent.removeAt(s.sent.size() - 16);
        s.sendInstruction(oldNum, s.sent.last().num, diff);
        assumed = assumedState();
        pending = cur != s.sent[assumed].count;
    }
    // Next wakeup, as mosh's wait_time(): ack/heartbeat, (re)send, silence timeout. Every deadline is after now.
    qint64 next = qMin(s.nextAck, s.lastHeard < 0 ? FirstContact + 1 : s.lastHeard + Silence + 1);
    if (!s.quiet && s.lastHeard >= 0) // never reached while the server heartbeats arrive: no extra wakeup
        next = qMin(next, s.lastHeard + Quiet);
    if (pending && (retrying || cur != s.sent.last().count))
        next = qMin(next, s.sent.last().time + s.sendInterval());
    else if (retrying && cur != s.sent.first().count) // unacked: retransmit when the oldest stops counting as received
        next = qMin(next, s.sent[1].time + s.rto() + AckDelay);
    s.timer.start(int(qBound(qint64(1), next - now, qint64(Silence))));
}

void MoshDevice::receive()
{
    State &s = *d;
    while (s.socket.hasPendingDatagrams())
    {
        const QByteArray datagram = s.socket.receiveDatagram().data();
        mosh::Packet p;
        mosh::Instruction inst;
        if (s.ended || !mosh::open(*s.ocb, datagram, p) || !p.toClient)
            continue; // unauthenticated or reflected packets are ignored
        const qint64 now = s.clock.elapsed();
        if (p.seq >= s.expectedSeq)
        {
            s.expectedSeq = p.seq + 1;
            if (p.timestamp != 0xFFFF)
                s.savedTimestamp = p.timestamp, s.savedAt = now;
            if (p.timestampReply != 0xFFFF)
            {
                const double r = quint16(quint16(now) - p.timestampReply);
                if (r < 5000)
                {
                    if (!s.rttHit)
                        s.srtt = r, s.rttvar = r / 2, s.rttHit = true;
                    else
                    {
                        s.rttvar = 0.75 * s.rttvar + 0.25 * std::fabs(s.srtt - r);
                        s.srtt = 0.875 * s.srtt + 0.125 * r;
                    }
                }
            }
            s.lastHeard = now; // as mosh: replayed or duplicated packets do not keep a dead session alive
            if (s.quiet)
            {
                s.quiet = false;
                if (onSilence)
                    onSilence();
            }
        }
        if (!s.assembler.add(p.payload, inst))
            continue;
        if (inst.protocolVersion != 2)
            return finish(tr("Incompatible mosh-server protocol version"));
        for (int i = 0; i < s.sent.size(); ++i)
            if (s.sent[i].num == inst.ackNum)
            {
                s.sent.remove(0, i);
                const qint64 drop = s.sent.first().count - s.eventBase;
                s.events.remove(0, drop);
                s.eventBase += drop;
                break;
            }
        // Only diffs computed against the screen we actually have: the server resends the others.
        if (inst.oldNum != s.remoteNum || inst.newNum <= s.remoteNum)
            continue;
        QList<mosh::Action> actions;
        if (!mosh::decodeActions(inst.diff, actions))
            continue;
        s.remoteNum = inst.newNum;
        for (const mosh::Action &a : actions)
            s.readBuffer += a.bytes; // ResizeMessage and EchoAck ignored: predictions are verified against the screen
        if (!inst.diff.isEmpty())
            s.nextAck = qMin(s.nextAck, now + AckDelay);
        if (!s.readBuffer.isEmpty())
            emit readyRead();
        if (s.remoteNum == Shutdown)
        {
            tick(); // ack the server's close immediately
            normalEnd = true;
            return finish(tr("Mosh session ended"));
        }
    }
    tick(); // acks, RTT and acked states changed: re-arm the timer
}

double MoshDevice::srtt() const
{
    return d->srtt;
}

qint64 MoshDevice::silence() const
{
    return d->lastHeard < 0 ? -1 : d->clock.elapsed() - d->lastHeard;
}

// --- local echo ---
MoshPredictor::Mode MoshPredictor::modeFromSettings()
{
    const QString mode = QSettings().value("mosh/prediction", "adaptive").toString();
    return mode == "never" ? Never : mode == "always" ? Always : Adaptive;
}

MoshPredictor::MoshPredictor(Terminal &terminal, const MoshDevice &device, Mode mode)
    : terminal(terminal), device(device), mode(mode)
{
    clock.start();
    QObject::connect(&timer, &QTimer::timeout, [this] { tick(); });
}

int MoshPredictor::timeout() const
{
    return 300 + 2 * int(device.srtt()); // beyond this: no echo (password) or wrong prediction
}

void MoshPredictor::discard()
{
    cells.clear();
    cursor = {-1, -1};
    ++epoch;
}

void MoshPredictor::reset()
{
    discard();
    publish();
}

void MoshPredictor::input(const QByteArray &keys)
{
    const qint64 now = clock.elapsed();
    lastInput = now;
    for (const char32_t c : QString::fromUtf8(keys).toUcs4())
    {
        if (cursor.x() < 0)
            cursor = terminal.cursorPosition();
        const bool erase = c == 0x7f || c == 0x08;
        const QPoint at = erase ? cursor - QPoint(1, 0) : cursor;
        const char32_t original = terminal.cellChar(at.y(), at.x());
        // Control keys, sequences, wide characters and the screen edge: no prediction, new epoch.
        // Overwrite only: unlike mosh, no predicted line wrap or insert-with-shift.
        if ((c < 0x20 && !erase) || c >= 0x1100 || !original)
        {
            cursor = {-1, -1};
            ++epoch;
            if (c == 0x1b)
                break; // the rest is the escape sequence
            continue;
        }
        const char32_t ch = erase ? U' ' : c;
        auto same = std::find_if(cells.begin(), cells.end(), [&](const Cell &p) { return p.row == at.y() && p.col == at.x(); });
        if (same != cells.end())
            *same = Cell{at.y(), at.x(), ch, same->original, same->ch, epoch, now};
        else
            cells << Cell{at.y(), at.x(), ch, original, original, epoch, now};
        cursor = erase ? at : at + QPoint(1, 0);
    }
    publish();
}

void MoshPredictor::output()
{
    for (int i = 0; i < cells.size();)
    {
        const Cell &p = cells[i];
        const char32_t shown = terminal.cellChar(p.row, p.col);
        if (shown == p.ch)
        {
            if (p.ch != p.original) // the server really wrote the character: the epoch is trustworthy
                confirmedEpoch = std::max(confirmedEpoch, p.epoch);
            cells.removeAt(i);
        }
        else if (shown != p.original && shown != p.previous)
        {
            discard(); // the server shows something else: stop showing until a new epoch is confirmed
            break;
        }
        else
            ++i;
    }
    if (cells.isEmpty() && cursor == terminal.cursorPosition())
        cursor = {-1, -1};
    publish();
}

void MoshPredictor::tick()
{
    const qint64 now = clock.elapsed();
    if (std::any_of(cells.begin(), cells.end(), [&](const Cell &p) { return now - p.time > timeout(); }))
        discard();
    else if (cells.isEmpty() && now - lastInput > timeout())
        cursor = {-1, -1};
    publish();
}

void MoshPredictor::publish()
{
    const qint64 now = clock.elapsed();
    bool show = mode == Always || device.srtt() > 30;
    for (const Cell &p : cells)
        show = show || now - p.time > 100;
    QList<PredictedCell> visible;
    for (const Cell &p : cells)
        if (show && p.epoch <= confirmedEpoch)
            visible << PredictedCell{p.row, p.col, p.ch};
    const bool moved = show && cursor.x() >= 0 && epoch <= confirmedEpoch;
    terminal.setPredictions(visible, moved ? cursor : QPoint(-1, -1));
    if (cells.isEmpty() && cursor.x() < 0)
        timer.stop();
    else if (!timer.isActive())
        timer.start(50);
}
