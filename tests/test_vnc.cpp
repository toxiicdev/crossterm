// VNC viewer tests: DES/VNC auth, inflate and a fake in-process RFB server on 127.0.0.1.
#include "vnc.h"
#include <QApplication>
#include <QClipboard>
#include <QDeadlineTimer>
#include <QDebug>
#include <QKeyEvent>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtEndian>
#include <functional>
#include <memory>

static int failures = 0;
#define CHECK(expression)                                                                                                        \
    if (!(expression))                                                                                                           \
    {                                                                                                                            \
        qCritical() << __LINE__ << #expression;                                                                                  \
        ++failures;                                                                                                              \
    }

static bool spin(const std::function<bool()> &done, int milliseconds = 5000)
{
    for (QDeadlineTimer deadline(milliseconds); !done();)
    {
        if (deadline.hasExpired())
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

// Big-endian message builder; pixels are 32-bit little-endian (the format requested by the client).
struct Out
{
    QByteArray b;
    Out &u8(int v) { return b += char(v), *this; }
    Out &u16(int v) { return u8(v >> 8).u8(v); }
    Out &u32(quint32 v) { return u16(int(v >> 16)).u16(int(v & 0xFFFF)); }
    Out &pixel(quint32 c) { return u8(int(c)).u8(int(c >> 8)).u8(int(c >> 16)).u8(0); }
    Out &cpixel(quint32 c) { return u8(int(c)).u8(int(c >> 8)).u8(int(c >> 16)); }
    Out &rect(int x, int y, int w, int h, qint32 encoding) { return u16(x).u16(y).u16(w).u16(h).u32(quint32(encoding)); }
};

static QByteArray readN(QTcpSocket *s, qint64 n)
{
    spin([&] { return s->bytesAvailable() >= n; });
    return s->read(n);
}

static quint32 be32(const QByteArray &b, int offset)
{
    return qFromBigEndian<quint32>(b.constData() + offset);
}

static quint16 be16(const QByteArray &b, int offset)
{
    return qFromBigEndian<quint16>(b.constData() + offset);
}

// Next client message (whole, including type).
static QByteArray next(QTcpSocket *s)
{
    QByteArray m = readN(s, 1);
    if (m.isEmpty())
        return m;
    switch (m[0])
    {
    case 0:
        return m + readN(s, 19);
    case 2:
        m += readN(s, 3);
        return m + readN(s, 4 * be16(m, 2));
    case 3:
        return m + readN(s, 9);
    case 4:
        return m + readN(s, 7);
    case 5:
        return m + readN(s, 5);
    case 6:
        m += readN(s, 7);
        return m + readN(s, be32(m, 4));
    }
    return m;
}

// Next message that is not a FramebufferUpdateRequest.
static QByteArray event(QTcpSocket *s)
{
    QByteArray m;
    do
        m = next(s);
    while (!m.isEmpty() && m[0] == 3);
    return m;
}

static QByteArray hex(const char *text)
{
    return QByteArray::fromHex(text);
}

static quint32 colour(int x, int y)
{
    return 0xFF000000u | quint32((x * 7) & 255) << 16 | quint32((y * 13) & 255) << 8 | quint32((x ^ y) & 255);
}

static quint32 colour2(int x, int y)
{
    return 0xFF000000u | quint32((x * 3 + y) & 255) << 16 | quint32((x + y * 5) & 255) << 8 | 0x40;
}

// Minimal deflate compressor (fixed Huffman blocks, naive LZ77) with sync flush: one stream for the whole session.
struct Deflater
{
    QByteArray history, out;
    quint32 acc = 0;
    int count = 0, crossRefs = 0;
    bool started = false;

    void bit(int b)
    {
        acc |= quint32(b & 1) << count;
        if (++count == 8)
        {
            out += char(acc);
            acc = 0;
            count = 0;
        }
    }
    void bits(quint32 v, int n)
    {
        for (int i = 0; i < n; ++i)
            bit(int(v >> i));
    }
    void huff(quint32 code, int len)
    {
        for (int i = len - 1; i >= 0; --i)
            bit(int(code >> i));
    }
    void symbol(int s)
    {
        if (s < 144)
            huff(0x30 + s, 8);
        else if (s < 256)
            huff(0x190 + s - 144, 9);
        else if (s < 280)
            huff(s - 256, 7);
        else
            huff(0xC0 + s - 280, 8);
    }
    QByteArray compress(const QByteArray &data)
    {
        static const int lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const int lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const int dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const int dext[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        out.clear();
        if (!started)
            out += "\x78\x01";
        started = true;
        bits(0, 1); // not final
        bits(1, 2); // fixed Huffman
        const int start = int(history.size());
        history += data;
        for (int i = start; i < history.size();)
        {
            int best = 0, distance = 0;
            for (int j = qMax(0, i - 32768); j < i; ++j)
            {
                int l = 0;
                while (l < 258 && i + l < history.size() && history[j + l] == history[i + l])
                    ++l;
                if (l > best)
                    best = l, distance = i - j;
            }
            if (best < 3)
            {
                symbol(uchar(history[i++]));
                continue;
            }
            int li = 28, di = 29;
            while (lbase[li] > best)
                --li;
            while (dbase[di] > distance)
                --di;
            symbol(257 + li);
            bits(quint32(best - lbase[li]), lext[li]);
            huff(quint32(di), 5);
            bits(quint32(distance - dbase[di]), dext[di]);
            if (i - distance < start)
                ++crossRefs;
            i += best;
        }
        symbol(256);
        bits(0, 3); // sync flush: empty stored block
        if (count)
            bits(0, 8 - count);
        out += QByteArray("\0\0\xFF\xFF", 4);
        return out;
    }
};

static QTcpSocket *accept(QTcpServer &server)
{
    if (!spin([&] { return server.hasPendingConnections(); }))
        return nullptr;
    return server.nextPendingConnection();
}

static void testCrypto()
{
    CHECK(desEncrypt(hex("133457799BBCDFF1"), hex("0123456789ABCDEF")) == hex("85E813540F0AB405"));
    CHECK(desEncrypt(hex("0E329232EA6D0D73"), hex("8787878787878787")) == hex("0000000000000000"));
    CHECK(desEncrypt(hex("0000000000000000"), hex("0000000000000000")) == hex("8CA64DE9C1B123A7"));
    // Password whose bytes, with bits reversed, give the key 133457799BBCDFF1; beyond 8 bytes it is truncated.
    const QByteArray challenge = hex("0123456789ABCDEF0123456789ABCDEF");
    CHECK(vncAuthResponse(hex("C82CEA9ED93DFB8F"), challenge) == hex("85E813540F0AB40585E813540F0AB405"));
    CHECK(vncAuthResponse(hex("C82CEA9ED93DFB8F") + "extra", challenge) == hex("85E813540F0AB40585E813540F0AB405"));
    // Short password: zero-padded -> null key.
    CHECK(vncAuthResponse(QByteArray(), hex("00000000000000000000000000000000")) == hex("8CA64DE9C1B123A78CA64DE9C1B123A7"));
}

static void testInflate()
{
    QByteArray data;
    for (int i = 0; i < 20000; ++i)
        data += char("abcdefghij"[(i * i / 7) % 10] + (i % 97 == 0));
    const QByteArray zlib = qCompress(data, 9).mid(4); // dynamic Huffman
    QByteArray all, part;
    // Feeding in irregular chunks, including mid-block.
    VncInflater pieces;
    for (int i = 0; i < zlib.size(); i += 37)
    {
        CHECK(pieces.feed(zlib.mid(i, 37), part));
        all += part;
    }
    CHECK(all == data);
    VncInflater bad;
    CHECK(!bad.feed(QByteArray("\x78\x01\x07", 3), part));
}

static void testWrongPassword()
{
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost));
    QString reason;
    bool asked = false;
    VncView view("127.0.0.1", server.serverPort(), [&] { return asked = true, QString("wrong"); });
    view.onDisconnected = [&](const QString &r) { reason = r; };
    QTcpSocket *s = accept(server);
    CHECK(s);
    if (!s)
        return;
    s->write("RFB 003.008\n");
    readN(s, 12);
    s->write(QByteArray("\x01\x02", 2));
    CHECK(readN(s, 1) == QByteArray(1, 2));
    const QByteArray challenge = hex("00112233445566778899AABBCCDDEEFF");
    s->write(challenge);
    CHECK(readN(s, 16) == vncAuthResponse("wrong", challenge));
    const QByteArray text = "Authentication failed: wrong password";
    s->write(Out().u32(1).u32(quint32(text.size())).b + text);
    s->flush();
    CHECK(spin([&] { return !reason.isEmpty(); }));
    CHECK(asked);
    CHECK(reason == text);
    CHECK(!view.isConnected());
    delete s;
}

static void testVersion33()
{
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost));
    VncView view("127.0.0.1", server.serverPort(), [] { return QString(); });
    QTcpSocket *s = accept(server);
    CHECK(s);
    if (!s)
        return;
    s->write("RFB 003.003\n");
    CHECK(readN(s, 12) == "RFB 003.003\n");
    s->write(Out().u32(1).b); // None, no SecurityResult in 3.3
    CHECK(readN(s, 1) == QByteArray(1, 1));
    s->write(Out().u16(32).u16(16).u32(0).u32(0).u32(0).u32(0).u32(3).b + "old");
    CHECK(spin([&] { return view.isConnected(); }));
    CHECK(view.desktopName() == "old");
    CHECK(view.framebuffer().size() == QSize(32, 16));
    delete s;
}

static void testSession()
{
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost));
    QString reason;
    VncView view("127.0.0.1", server.serverPort(), [] { return QString("secret"); });
    view.onDisconnected = [&](const QString &r) { reason = r; };
    std::unique_ptr<QTcpSocket> s(accept(server));
    CHECK(s);
    if (!s)
        return;
    s->write("RFB 003.008\n");
    CHECK(readN(s.get(), 12) == "RFB 003.008\n");
    s->write(QByteArray("\x02\x02\x10", 3)); // VNC auth and an unknown type
    CHECK(readN(s.get(), 1) == QByteArray(1, 2));
    const QByteArray challenge = hex("F0E1D2C3B4A5968778695A4B3C2D1E0F");
    s->write(challenge);
    CHECK(readN(s.get(), 16) == vncAuthResponse("secret", challenge));
    s->write(Out().u32(0).b);
    CHECK(readN(s.get(), 1) == QByteArray(1, 1)); // shared ClientInit
    const int W = 200, H = 100;
    s->write(Out().u16(W).u16(H).u32(0x20180001).u32(0x00FF00FF).u32(0x00FF1008).u32(0).u32(12).b + "Fake Desktop");

    const QByteArray format = next(s.get());
    CHECK(format.size() == 20 && format[0] == 0 && format[4] == 32 && format[7] == 1 && format[14] == 16 && format[15] == 8);
    const QByteArray encodings = next(s.get());
    CHECK(encodings[0] == 2);
    QList<qint32> list;
    for (int i = 0; i < be16(encodings, 2); ++i)
        list << qint32(be32(encodings, 4 + 4 * i));
    CHECK(list.contains(16) && list.contains(5) && list.contains(2) && list.contains(1) && list.contains(0) &&
          list.contains(-223));
    const QByteArray request = next(s.get());
    CHECK(request[0] == 3 && request[1] == 0 && be16(request, 6) == W && be16(request, 8) == H);
    CHECK(view.isConnected());
    CHECK(view.desktopName() == "Fake Desktop");

    QImage expect(W, H, QImage::Format_RGB32);
    auto set = [&](int x, int y, quint32 c) { reinterpret_cast<quint32 *>(expect.scanLine(y))[x] = c; };
    auto fill = [&](int x, int y, int w, int h, quint32 c)
    {
        for (int j = y; j < y + h; ++j)
            for (int i = x; i < x + w; ++i)
                set(i, j, c);
    };

    Out update;
    update.u8(0).u8(0).u16(6);
    // Full-screen Raw.
    update.rect(0, 0, W, H, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            update.pixel(colour(x, y)), set(x, y, colour(x, y));
    // CopyRect.
    update.rect(100, 0, 40, 30, 1).u16(10).u16(5);
    expect = [&]
    {
        QImage e = expect;
        const QImage src = expect.copy(10, 5, 40, 30);
        for (int y = 0; y < 30; ++y)
            for (int x = 0; x < 40; ++x)
                reinterpret_cast<quint32 *>(e.scanLine(y))[100 + x] = src.pixel(x, y);
        return e;
    }();
    // RRE.
    update.rect(0, 40, 50, 20, 2).u32(2).pixel(0xFF112233);
    update.pixel(0xFFAA0000).u16(2).u16(3).u16(10).u16(5);
    update.pixel(0xFF00BB00).u16(20).u16(10).u16(30).u16(10);
    fill(0, 40, 50, 20, 0xFF112233);
    fill(2, 43, 10, 5, 0xFFAA0000);
    fill(20, 50, 30, 10, 0xFF00BB00);
    // Hextile 20x20: raw tile, background+foreground+subrect, inherited background, coloured subrects.
    update.rect(60, 40, 20, 20, 5);
    update.u8(1);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            update.pixel(colour2(x, y)), set(60 + x, 40 + y, colour2(x, y));
    update.u8(2 | 4 | 8).pixel(0xFF0000AA).pixel(0xFFFFFF00).u8(2).u8(0x00).u8(0x11).u8(0x15).u8(0x23);
    fill(76, 40, 4, 16, 0xFF0000AA);
    fill(76, 40, 2, 2, 0xFFFFFF00);
    fill(77, 45, 3, 4, 0xFFFFFF00);
    update.u8(0);
    fill(60, 56, 16, 4, 0xFF0000AA);
    update.u8(8 | 16).u8(1).pixel(0xFF123456).u8(0x11).u8(0x11);
    fill(76, 56, 4, 4, 0xFF0000AA);
    fill(77, 57, 2, 2, 0xFF123456);
    // ZRLE 1 (0,60,70,10): raw tile + solid-colour tile.
    Deflater deflater;
    Out z1;
    z1.u8(0);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 64; ++x)
            z1.cpixel(colour(x * 5, y)), set(x, 60 + y, colour(x * 5, y));
    z1.u8(1).cpixel(0xFF445566);
    fill(64, 60, 6, 10, 0xFF445566);
    const QByteArray c1 = deflater.compress(z1.b);
    update.rect(0, 60, 70, 10, 16).u32(quint32(c1.size())).b += c1;
    // ZRLE 2 (0,70,140,10): packed palette, plain RLE, palette RLE.
    Out z2;
    const quint32 p0 = 0xFF445566, p1 = 0xFF998877;
    z2.u8(2).cpixel(p0).cpixel(p1);
    for (int y = 0; y < 10; ++y)
        for (int byte = 0; byte < 8; ++byte)
        {
            int v = 0;
            for (int bit = 0; bit < 8; ++bit)
            {
                const int x = byte * 8 + bit, index = (x / 3 + y) & 1;
                v |= index << (7 - bit);
                set(x, 70 + y, index ? p1 : p0);
            }
            z2.u8(v);
        }
    z2.u8(128);
    const int runs[3] = {300, 100, 240};
    const quint32 runColours[3] = {0xFF010203, 0xFFA0B0C0, 0xFF0F0F0F};
    for (int r = 0, i = 0; r < 3; ++r)
    {
        z2.cpixel(runColours[r]);
        for (int left = runs[r] - 1;; left -= 255)
        {
            z2.u8(qMin(left, 255));
            if (left < 255)
                break;
        }
        for (int k = 0; k < runs[r]; ++k, ++i)
            set(64 + i % 64, 70 + i / 64, runColours[r]);
    }
    const quint32 pal[3] = {0xFFDD0000, 0xFF00DD00, 0xFF0000DD};
    z2.u8(128 + 3).cpixel(pal[0]).cpixel(pal[1]).cpixel(pal[2]);
    z2.u8(128 | 0).u8(49).u8(1).u8(128 | 2).u8(68);
    for (int i = 0; i < 120; ++i)
        set(128 + i % 12, 70 + i / 12, i < 50 ? pal[0] : i == 50 ? pal[1] : pal[2]);
    const QByteArray c2 = deflater.compress(z2.b);
    CHECK(deflater.crossRefs > 0); // back-references to the previous rectangle
    update.rect(0, 70, 140, 10, 16).u32(quint32(c2.size())).b += c2;
    s->write(update.b);

    QByteArray incremental;
    do
        incremental = next(s.get());
    while (!incremental.isEmpty() && incremental[0] != 3);
    CHECK(incremental.size() == 10 && incremental[1] == 1);
    if (!reason.isEmpty())
        qCritical() << "disconnected:" << reason;
    const QImage got = view.framebuffer();
    CHECK(got.size() == expect.size());
    int wrong = 0;
    for (int y = 0; y < H && got.size() == expect.size(); ++y)
        for (int x = 0; x < W; ++x)
            if ((got.pixel(x, y) & 0xFFFFFF) != (expect.pixel(x, y) & 0xFFFFFF) && wrong++ < 5)
                qCritical() << "pixel" << x << y << Qt::hex << got.pixel(x, y) << expect.pixel(x, y);
    CHECK(wrong == 0);

    // DesktopSize followed by Raw on the new framebuffer; then a non-incremental request.
    const int W2 = 120, H2 = 90;
    Out resize;
    resize.u8(0).u8(0).u16(2).rect(0, 0, W2, H2, -223).rect(0, 0, W2, H2, 0);
    for (int y = 0; y < H2; ++y)
        for (int x = 0; x < W2; ++x)
            resize.pixel(colour2(x, y));
    s->write(resize.b);
    QByteArray full;
    do
        full = next(s.get());
    while (!full.isEmpty() && full[0] != 3);
    CHECK(full.size() == 10 && full[1] == 0 && be16(full, 6) == W2 && be16(full, 8) == H2);
    CHECK(view.framebuffer().size() == QSize(W2, H2));
    CHECK((view.framebuffer().pixel(37, 81) & 0xFFFFFF) == (colour2(37, 81) & 0xFFFFFF));
    CHECK((view.framebuffer().pixel(119, 89) & 0xFFFFFF) == (colour2(119, 89) & 0xFFFFFF));

    // Clipboard: ServerCutText UTF-8 -> QClipboard, QClipboard -> ClientCutText. Only while the view has the focus.
    view.show();
    view.activateWindow();
    view.setFocus();
    CHECK(spin([&] { return view.hasFocus(); }));
    const QString clip = QString::fromUtf8("h\xC3\xA9llo w\xC3\xB6rld");
    s->write(Out().u8(3).u8(0).u16(0).u32(quint32(clip.toUtf8().size())).b + clip.toUtf8());
    CHECK(spin([&] { return QGuiApplication::clipboard()->text() == clip; }));
    QGuiApplication::clipboard()->setText("copy me");
    const QByteArray cut = event(s.get());
    CHECK(cut.size() == 15 && cut[0] == 6 && cut.mid(8) == "copy me");

    // Keyboard.
    auto key = [&](QEvent::Type type, int k, Qt::KeyboardModifiers mods, const QString &text)
    {
        QKeyEvent e(type, k, mods, text);
        QCoreApplication::sendEvent(&view, &e);
    };
    auto expectKey = [&](bool down, quint32 sym)
    {
        const QByteArray m = event(s.get());
        const bool ok = m.size() == 8 && m[0] == 4 && bool(m[1]) == down && be32(m, 4) == sym;
        if (!ok)
            qCritical() << "key" << m.toHex() << down << Qt::hex << sym;
        CHECK(ok);
    };
    key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a");
    key(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, "a");
    expectKey(true, 'a');
    expectKey(false, 'a');
    key(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, "");
    key(QEvent::KeyPress, Qt::Key_A, Qt::ShiftModifier, "A");
    key(QEvent::KeyRelease, Qt::Key_A, Qt::ShiftModifier, "A");
    key(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier, "");
    expectKey(true, 0xFFE1);
    expectKey(true, 'A');
    expectKey(false, 'A');
    expectKey(false, 0xFFE1);
    key(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier, "");
    key(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, QString(QChar(3)));
    expectKey(true, 0xFFE3);
    expectKey(true, 'c');
    key(QEvent::KeyPress, Qt::Key_F5, Qt::NoModifier, "");
    key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
    key(QEvent::KeyPress, 0x20AC, Qt::NoModifier, QString(QChar(0x20AC)));
    key(QEvent::KeyPress, 0xE9, Qt::NoModifier, QString(QChar(0xE9)));
    expectKey(true, 0xFFC2);
    expectKey(true, 0xFF0D);
    expectKey(true, 0x010020AC);
    expectKey(true, 0xE9);
    // View only: no events; then Ctrl+Alt+Del.
    view.setViewOnly(true);
    key(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "x");
    view.setViewOnly(false);
    view.sendCtrlAltDel();
    for (quint32 sym : {0xFFE3u, 0xFFE9u, 0xFFFFu})
        expectKey(true, sym);
    for (quint32 sym : {0xFFFFu, 0xFFE9u, 0xFFE3u})
        expectKey(false, sym);

    // Pointer: without scaling coordinates match; with scaling the aspect ratio is kept.
    auto mouse = [&](QEvent::Type type, QPointF at, Qt::MouseButton b, Qt::MouseButtons bs)
    {
        QMouseEvent e(type, at, at, b, bs, Qt::NoModifier);
        QCoreApplication::sendEvent(&view, &e);
    };
    auto expectPointer = [&](int mask, int x, int y)
    {
        const QByteArray m = event(s.get());
        const bool ok = m.size() == 6 && m[0] == 5 && uchar(m[1]) == mask && be16(m, 2) == x && be16(m, 4) == y;
        if (!ok)
            qCritical() << "pointer" << m.toHex() << mask << x << y;
        CHECK(ok);
    };
    view.setScaled(false);
    mouse(QEvent::MouseButtonPress, {10, 20}, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {10, 20}, Qt::LeftButton, Qt::NoButton);
    mouse(QEvent::MouseButtonPress, {30, 40}, Qt::RightButton, Qt::RightButton);
    mouse(QEvent::MouseMove, {31, 41}, Qt::NoButton, Qt::RightButton);
    mouse(QEvent::MouseButtonRelease, {31, 41}, Qt::RightButton, Qt::NoButton);
    mouse(QEvent::MouseButtonPress, {5, 6}, Qt::MiddleButton, Qt::MiddleButton);
    expectPointer(1, 10, 20);
    expectPointer(0, 10, 20);
    expectPointer(4, 30, 40);
    expectPointer(4, 31, 41);
    expectPointer(0, 31, 41);
    expectPointer(2, 5, 6);
    mouse(QEvent::MouseButtonRelease, {5, 6}, Qt::MiddleButton, Qt::NoButton);
    expectPointer(0, 5, 6);
    for (int delta : {120, -120})
    {
        QWheelEvent e({7, 8}, {7, 8}, {}, {0, delta}, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&view, &e);
        expectPointer(delta > 0 ? 8 : 16, 7, 8);
        expectPointer(0, 7, 8);
    }
    view.setScaled(true);
    view.resize(400, 200); // 120x90 -> 266x200 centered at x=67
    mouse(QEvent::MouseButtonPress, {200, 100}, Qt::LeftButton, Qt::LeftButton);
    expectPointer(1, 60, 45);

    CHECK(reason.isEmpty());
    s->disconnectFromHost();
    CHECK(spin([&] { return !reason.isEmpty(); }));
    CHECK(!view.isConnected());
}

int main(int argc, char **argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    testCrypto();
    testInflate();
    testWrongPassword();
    testVersion33();
    testSession();
    if (failures)
        qCritical() << failures << "failures";
    return failures ? 1 : 0;
}
