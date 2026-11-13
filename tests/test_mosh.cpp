// Mosh client tests: AES/OCB vectors, protobuf, zlib, bootstrap, loopback with a fake mosh-server,
// local echo (predictions) on the Terminal and, if MOSH_INTEROP_SSH is set ("-i key -p port user@host"), interoperability
// with a real mosh-server.
#include "mosh.h"
#include "terminal.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QNetworkDatagram>
#include <QProcess>
#include <QRandomGenerator>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>

static int failures = 0;
#define CHECK(cond)                                                                                                              \
    do                                                                                                                           \
    {                                                                                                                            \
        if (!(cond))                                                                                                             \
        {                                                                                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                          \
            ++failures;                                                                                                          \
        }                                                                                                                        \
    } while (0)

static QByteArray hex(const char *s)
{
    return QByteArray::fromHex(s);
}

static bool waitFor(const std::function<bool()> &done, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
    {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return done();
}

static void testAes()
{
    auto check = [](const char *key, const char *plain, const char *cipher)
    {
        const QByteArray k = hex(key), p = hex(plain), c = hex(cipher);
        mosh::Aes128 aes(reinterpret_cast<const uint8_t *>(k.constData()));
        mosh::Block b;
        memcpy(b.data(), p.constData(), 16);
        const mosh::Block e = aes.encrypt(b);
        CHECK(QByteArray(reinterpret_cast<const char *>(e.data()), 16) == c);
        CHECK(aes.decrypt(e) == b);
    };
    // FIPS-197 appendix C.1 and appendix B
    check("000102030405060708090a0b0c0d0e0f", "00112233445566778899aabbccddeeff", "69c4e0d86a7b0430d8cdb78070b4c55a");
    check("2b7e151628aed2a6abf7158809cf4f3c", "3243f6a8885a308d313198a2e0370734", "3925841d02dc09fbdc118597196a0b32");
}

static void testOcb()
{
    // RFC 7253 appendix A, AES-128, TAGLEN 128
    const mosh::Ocb ocb(hex("000102030405060708090A0B0C0D0E0F"));
    const struct
    {
        const char *nonce, *ad, *plain, *cipher;
    } vectors[] = {
        {"BBAA99887766554433221100", "", "", "785407BFFFC8AD9EDCC5520AC9111EE6"},
        {"BBAA99887766554433221101", "0001020304050607", "0001020304050607", "6820B3657B6F615A5725BDA0D3B4EB3A257C9AF1F8F03009"},
        {"BBAA99887766554433221102", "0001020304050607", "", "81017F8203F081277152FADE694A0A00"},
        {"BBAA99887766554433221103", "", "0001020304050607", "45DD69F8F5AAE72414054CD1F35D82760B2CD00D2F99BFA9"},
        {"BBAA99887766554433221104", "000102030405060708090A0B0C0D0E0F", "000102030405060708090A0B0C0D0E0F",
         "571D535B60B277188BE5147170A9A22C3AD7A4FF3835B8C5701C1CCEC8FC3358"},
        {"BBAA99887766554433221105", "000102030405060708090A0B0C0D0E0F", "", "8CF761B6902EF764462AD86498CA6B97"},
        {"BBAA99887766554433221106", "", "000102030405060708090A0B0C0D0E0F",
         "5CE88EC2E0692706A915C00AEB8B2396F40E1C743F52436BDF06D8FA1ECA343D"},
        {"BBAA99887766554433221107", "000102030405060708090A0B0C0D0E0F1011121314151617",
         "000102030405060708090A0B0C0D0E0F1011121314151617",
         "1CA2207308C87C010756104D8840CE1952F09673A448A122C92C62241051F57356D7F3C90BB0E07F"},
        {"BBAA99887766554433221108", "000102030405060708090A0B0C0D0E0F1011121314151617", "", "6DC225A071FC1B9F7C69F93B0F1E10DE"},
        {"BBAA99887766554433221109", "", "000102030405060708090A0B0C0D0E0F1011121314151617",
         "221BD0DE7FA6FE993ECCD769460A0AF2D6CDED0C395B1C3CE725F32494B9F914D85C0B1EB38357FF"},
        {"BBAA9988776655443322110A", "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F",
         "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F",
         "BD6F6C496201C69296C11EFD138A467ABD3C707924B964DEAFFC40319AF5A48540FBBA186C5553C68AD9F592A79A4240"},
        {"BBAA9988776655443322110B", "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "",
         "FE80690BEE8A485D11F32965BC9D2A32"},
        {"BBAA9988776655443322110C", "", "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F",
         "2942BFC773BDA23CABC6ACFD9BFD5835BD300F0973792EF46040C53F1432BCDFB5E1DDE3BC18A5F840B52E653444D5DF"},
    };
    for (const auto &v : vectors)
    {
        const QByteArray sealed = ocb.encrypt(hex(v.nonce), hex(v.plain), hex(v.ad));
        CHECK(sealed == hex(v.cipher));
        QByteArray plain;
        CHECK(ocb.decrypt(hex(v.nonce), sealed, plain, hex(v.ad)) && plain == hex(v.plain));
        QByteArray tampered = sealed;
        tampered[0] = char(tampered[0] ^ 1);
        CHECK(!ocb.decrypt(hex(v.nonce), tampered, plain, hex(v.ad)));
    }
    // Iterative test from appendix A: covers lengths 0..127 bytes.
    const mosh::Ocb k(QByteArray(15, '\0') + char(128));
    QByteArray c;
    auto nonce = [](quint32 n)
    {
        QByteArray b(12, '\0');
        b[8] = char(n >> 24), b[9] = char(n >> 16), b[10] = char(n >> 8), b[11] = char(n);
        return b;
    };
    for (quint32 i = 0; i < 128; ++i)
    {
        const QByteArray s(int(i), '\0');
        c += k.encrypt(nonce(3 * i + 1), s, s);
        c += k.encrypt(nonce(3 * i + 2), s, {});
        c += k.encrypt(nonce(3 * i + 3), {}, s);
    }
    CHECK(k.encrypt(nonce(385), {}, c) == hex("67E944D23256C5E0B6C61FA22FDF1EA2"));
}

static void testCodecs()
{
    mosh::Instruction in;
    in.oldNum = 7;
    in.newNum = ~0ULL;
    in.ackNum = 300;
    in.throwawayNum = 5;
    in.diff = "diff";
    mosh::Instruction out;
    CHECK(mosh::decodeInstruction(mosh::encodeInstruction(in), out));
    CHECK(out.protocolVersion == 2 && out.oldNum == 7 && out.newNum == ~0ULL && out.ackNum == 300 && out.throwawayNum == 5 &&
          out.diff == "diff");
    // Keystroke "a": UserMessage{instruction{[keystroke]{keys:"a"}}}
    CHECK(mosh::encodeActions({mosh::Action{"a"}}) == hex("0a051203220161"));
    QList<mosh::Action> actions{mosh::Action{"ls\r"}, mosh::Action{{}, 132, 43}, mosh::Action{{}, -1, -1, 99}}, back;
    CHECK(mosh::decodeActions(mosh::encodeActions(actions), back) && back.size() == 3);
    CHECK(back.value(0).bytes == "ls\r" && back.value(0).width == -1);
    CHECK(back.value(1).width == 132 && back.value(1).height == 43);
    CHECK(back.value(2).echoAck == 99);
    CHECK(!mosh::decodeActions(hex("0a05120322"), back)); // truncated
    // zlib: known stream (zlib.compress(b"hello")) and a message much larger than the length hint
    CHECK(mosh::zlibUncompress(hex("789ccb48cdc9c90700062c0215")) == "hello");
    const QByteArray big = QByteArray("mosh ").repeated(60000);
    CHECK(mosh::zlibCompress(big).startsWith('\x78'));
    CHECK(mosh::zlibUncompress(mosh::zlibCompress(big)) == big);
    // Fragments: large instruction split and reassembled, even out of order
    QByteArray noise(5000, Qt::Uninitialized);
    for (char &ch : noise)
        ch = char(QRandomGenerator::global()->generate());
    in.diff = noise;
    QList<QByteArray> frags = mosh::makeFragments(42, in, 462);
    CHECK(frags.size() > 1);
    std::swap(frags.first(), frags.last());
    mosh::Assembler as;
    int complete = 0;
    for (const QByteArray &f : frags)
        complete += as.add(f, out);
    CHECK(complete == 1 && out.diff == noise);
    // Encrypted datagram
    const mosh::Ocb ocb(hex("00112233445566778899aabbccddeeff"));
    mosh::Packet p, q;
    p.toClient = true;
    p.seq = 12345;
    p.timestamp = 100;
    p.payload = "payload";
    const QByteArray wire = mosh::seal(ocb, p);
    CHECK(wire.left(8) == hex("8000000000003039"));
    CHECK(mosh::open(ocb, wire, q) && q.toClient && q.seq == 12345 && q.timestamp == 100 && q.timestampReply == 0xFFFF &&
          q.payload == "payload");
    QByteArray bad = wire;
    bad[3] = char(bad[3] ^ 1); // altered sequence: the nonce no longer matches
    CHECK(!mosh::open(ocb, bad, q));
}

static void testBootstrap()
{
    int port = 0;
    QByteArray key;
    CHECK(parseMoshConnect("\r\nMOSH CONNECT 60001 4NeCCgvZFe2RnPgrcU1PQw\r\n\r\nmosh-server (mosh 1.4.0) [build mosh 1.4.0]\r\n",
                           port, key));
    CHECK(port == 60001 && key == "4NeCCgvZFe2RnPgrcU1PQw");
    CHECK(!parseMoshConnect("MOSH CONNECT 70000 4NeCCgvZFe2RnPgrcU1PQw\n", port, key));
    CHECK(!parseMoshConnect("MOSH CONNECT 60001 short\n", port, key));
    CHECK(!parseMoshConnect("mosh-server: command not found\n", port, key));
    CHECK(moshServerArguments().join(' ') == "mosh-server new -s -c 256 -l LANG=en_US.UTF-8");
    CHECK(moshServerArguments("60001:60010").mid(7) == QStringList({"-p", "60001:60010"}));
    MoshDevice bad("127.0.0.1", 60001, "not-a-key");
    CHECK(!bad.isOpen() && !bad.errorString().isEmpty());
}

// Fake mosh-server: same crypto, same fragments and same "apply only from a known old_num" rule.
struct FakeServer
{
    QUdpSocket socket;
    mosh::Ocb ocb;
    mosh::Assembler assembler;
    QHostAddress peer;
    quint16 peerPort = 0;
    quint64 seq = 0, fragmentId = 0, clientNum = 0, lastAck = 0;
    QByteArray keys;
    QList<QPair<int, int>> resizes;
    std::function<bool(const QList<mosh::Action> &)> drop; // drops the first matching packet once
    bool dropped = false, clientShutdown = false;
    // Echo like a shell: echo(keys) = HostBytes to send back (empty = no echo). delay = network delay in ms.
    std::function<QByteArray(const QByteArray &)> echo;
    int delay = 0, timestamp = -1;
    quint64 hostNum = 0;

    explicit FakeServer(const QByteArray &key) : ocb(key)
    {
        socket.bind(QHostAddress::LocalHost, 0);
        QObject::connect(&socket, &QUdpSocket::readyRead, [this] { read(); });
    }
    void host(const QByteArray &bytes)
    {
        send(hostNum, hostNum + 1, bytes);
        ++hostNum;
    }
    void read()
    {
        while (socket.hasPendingDatagrams())
        {
            const QNetworkDatagram dg = socket.receiveDatagram();
            if (delay > 0)
                QTimer::singleShot(delay, &socket, [this, dg] { process(dg); });
            else
                process(dg);
        }
    }
    void process(const QNetworkDatagram &dg)
    {
        mosh::Packet p;
        mosh::Instruction inst;
        if (!mosh::open(ocb, dg.data(), p) || p.toClient)
            return;
        timestamp = p.timestamp == 0xFFFF ? timestamp : p.timestamp; // sent back immediately: measured RTT = delay
        if (!assembler.add(p.payload, inst))
            return;
        peer = dg.senderAddress();
        peerPort = quint16(dg.senderPort());
        QList<mosh::Action> actions;
        mosh::decodeActions(inst.diff, actions);
        if (drop && drop(actions))
        {
            drop = nullptr;
            dropped = true;
            return;
        }
        lastAck = inst.ackNum;
        clientShutdown |= inst.newNum == ~0ULL;
        QByteArray typed;
        if (inst.oldNum == clientNum && inst.newNum > clientNum)
        {
            clientNum = inst.newNum;
            for (const mosh::Action &a : actions)
                if (a.width >= 0)
                    resizes << qMakePair(a.width, a.height);
                else
                    typed += a.bytes;
            keys += typed;
        }
        const QByteArray reply = echo && !typed.isEmpty() ? echo(typed) : QByteArray();
        if (!reply.isEmpty())
            host(reply);
        else
            send(0, 0, {}); // ack only: new_num already known to the client
    }
    void send(quint64 oldNum, quint64 newNum, const QByteArray &hostBytes)
    {
        mosh::Instruction inst;
        inst.oldNum = oldNum;
        inst.newNum = newNum;
        inst.ackNum = clientNum;
        if (!hostBytes.isEmpty())
            inst.diff = mosh::encodeActions({mosh::Action{hostBytes}});
        for (const QByteArray &f : mosh::makeFragments(fragmentId++, inst))
        {
            mosh::Packet p;
            p.toClient = true;
            p.seq = seq++;
            p.timestampReply = quint16(timestamp);
            p.payload = f;
            socket.writeDatagram(mosh::seal(ocb, p), peer, peerPort);
        }
    }
};

static QByteArray randomKey()
{
    QByteArray key(16, Qt::Uninitialized);
    for (char &c : key)
        c = char(QRandomGenerator::global()->generate());
    return key;
}

static void testLoopback()
{
    const QByteArray key = randomKey();
    FakeServer server(key);
    MoshDevice dev("127.0.0.1", server.socket.localPort(), key.toBase64().left(22));
    CHECK(dev.isOpen());
    QByteArray output;
    bool finished = false;
    QObject::connect(&dev, &QIODevice::readyRead, [&] { output += dev.readAll(); });
    QObject::connect(&dev, &QIODevice::readChannelFinished, [&] { finished = true; });
    CHECK(waitFor([&] { return server.resizes.contains(qMakePair(80, 24)); })); // first contact
    dev.write("A");
    CHECK(waitFor([&] { return server.keys == "A"; }));
    // The packet with "B" is lost: it must arrive with the retransmission when the RTO expires.
    server.drop = [](const QList<mosh::Action> &a) { return !a.isEmpty() && a.last().bytes == "B"; };
    dev.write("B");
    CHECK(waitFor([&] { return server.keys == "AB"; }, 5000));
    CHECK(server.dropped);
    dev.setWindowSize(100, 30);
    CHECK(waitFor([&] { return !server.resizes.isEmpty() && server.resizes.last() == qMakePair(100, 30); }));
    server.send(0, 1, "hello");
    CHECK(waitFor([&] { return output == "hello"; }));
    CHECK(waitFor([&] { return server.lastAck == 1; })); // ack within ACK_DELAY
    server.send(5, 6, "bogus");                          // unknown base: must be ignored
    server.send(1, 2, " world");
    CHECK(waitFor([&] { return output == "hello world"; }));
    server.send(2, ~0ULL, "\r\nbye"); // close from the server
    CHECK(waitFor([&] { return finished; }));
    CHECK(output == "hello world\r\nbye" && !dev.isOpen());
    CHECK(waitFor([&] { return server.lastAck == ~0ULL; }));

    // Close from the client: the server receives the final state with new_num = -1.
    FakeServer server2(key);
    MoshDevice dev2("localhost", server2.socket.localPort(), key.toBase64().left(22));
    CHECK(waitFor([&] { return server2.clientNum > 0; }));
    dev2.close();
    CHECK(waitFor([&] { return server2.clientShutdown; }));
}

// Terminal on MoshDevice with a fake server that echoes like a shell. Enter prints a prompt and sets echoOn = promptEcho.
struct Session
{
    FakeServer server;
    MoshDevice *device;
    Terminal terminal;
    bool echoOn = true, upper = false, promptEcho = true;
    QByteArray prompt = "$ ";

    Session(const QByteArray &key, const char *mode, int delay)
        : server(key), device(new MoshDevice("127.0.0.1", server.socket.localPort(), key.toBase64().left(22))),
          terminal((QSettings().setValue("mosh/prediction", mode), device))
    {
        server.delay = delay;
        server.echo = [this](const QByteArray &keys)
        {
            QByteArray out;
            for (const char c : keys)
                if (c == '\r')
                {
                    out += "\r\n" + prompt;
                    echoOn = promptEcho;
                }
                else if (c == 0x7f)
                    out += "\b \b";
                else if (echoOn)
                    out += upper ? char(toupper(c)) : c;
            return out;
        };
        waitFor([&] { return server.clientNum > 0; });
        server.host(prompt);
        waitFor([&] { return row(0) == "$"; });
    }
    QString row(int r) const
    {
        QString s;
        for (int c = 0; c < 40; ++c)
        {
            const char32_t ch = terminal.cellChar(r, c);
            s += QString::fromUcs4(&ch, 1);
        }
        return s.trimmed();
    }
    void key(int k, const QString &text = {})
    {
        QKeyEvent event(QEvent::KeyPress, k, Qt::NoModifier, text);
        QApplication::sendEvent(&terminal, &event);
    }
    void type(const QString &text)
    {
        for (const QChar c : text)
            key(0, c);
    }
    // Types and waits for the server echo (and for the predictions to be confirmed).
    bool typeEchoed(const QString &text, const QString &expected)
    {
        type(text);
        return waitFor([&] { return row(0) == expected && terminal.predictions().isEmpty(); });
    }
};

static void testPrediction()
{
    const QByteArray key = randomKey();
    using P = QList<PredictedCell>;
    {
        Session s(key, "always", 150);
        CHECK(s.row(0) == "$");
        s.type("a");
        CHECK(s.terminal.predictions().isEmpty()); // first epoch: hidden until the server confirms one
        CHECK(waitFor([&] { return s.row(0) == "$ a" && s.terminal.predictions().isEmpty(); }));
        // Predictions shown immediately, then confirmed by the echo and removed.
        s.type("bc");
        CHECK(s.terminal.predictions() == P({{0, 3, U'b'}, {0, 4, U'c'}}));
        CHECK(s.row(0) == "$ a");
        CHECK(waitFor([&] { return s.row(0) == "$ abc" && s.terminal.predictions().isEmpty(); }));
        // Backspace: predicted cell empty.
        s.key(Qt::Key_Backspace);
        CHECK(s.terminal.predictions() == P({{0, 4, U' '}}));
        CHECK(waitFor([&] { return s.row(0) == "$ ab" && s.terminal.predictions().isEmpty(); }));
        // Mismatch: the server shows something else, predictions disappear and stay hidden until a new confirmation.
        s.upper = true;
        s.type("x");
        CHECK(s.terminal.predictions() == P({{0, 4, U'x'}}));
        CHECK(waitFor([&] { return s.row(0) == "$ abX"; }));
        CHECK(s.terminal.predictions().isEmpty());
        s.type("y");
        CHECK(s.terminal.predictions().isEmpty());
        CHECK(waitFor([&] { return s.row(0) == "$ abXY"; }));
        s.upper = false;
        CHECK(s.typeEchoed("z", "$ abXYz"));
        s.type("w");
        CHECK(s.terminal.predictions() == P({{0, 7, U'w'}}));
        CHECK(waitFor([&] { return s.row(0) == "$ abXYzw" && s.terminal.predictions().isEmpty(); }));
        // Password: after Enter the epoch is new and the server does not echo, nothing is ever shown.
        s.prompt = "Password: ";
        s.promptEcho = false;
        s.key(Qt::Key_Return, "\r");
        CHECK(waitFor([&] { return s.row(1) == "Password:"; }));
        s.type("sec");
        bool shown = false;
        waitFor([&] { return (shown = shown || !s.terminal.predictions().isEmpty()), false; }, 1200);
        CHECK(!shown && s.row(1) == "Password:");
        // Echo that stops in a confirmed epoch: the prediction expires and later ones stay hidden.
        s.prompt = "$ ";
        s.promptEcho = true;
        s.key(Qt::Key_Return, "\r");
        CHECK(waitFor([&] { return s.row(2) == "$"; }));
        s.type("q");
        CHECK(waitFor([&] { return s.row(2) == "$ q" && s.terminal.predictions().isEmpty(); }));
        s.echoOn = false;
        s.type("r");
        CHECK(s.terminal.predictions() == P({{2, 3, U'r'}}));
        CHECK(waitFor([&] { return s.terminal.predictions().isEmpty(); }, 2000));
        CHECK(s.row(2) == "$ q");
        s.type("t");
        shown = false;
        waitFor([&] { return (shown = shown || !s.terminal.predictions().isEmpty()), false; }, 1000);
        CHECK(!shown);
    }
    {
        Session s(key, "never", 150);
        CHECK(s.typeEchoed("a", "$ a"));
        s.type("bc");
        CHECK(s.terminal.predictions().isEmpty());
        CHECK(waitFor([&] { return s.row(0) == "$ abc"; }));
        CHECK(s.terminal.predictions().isEmpty());
    }
    {
        // Adaptive: hidden on a fast link, then predictions appear with high RTT.
        Session s(key, "adaptive", 0);
        CHECK(waitFor([&] { return s.device->srtt() < 30; }));
        CHECK(s.typeEchoed("a", "$ a"));
        bool shown = false;
        s.type("b");
        waitFor([&] { return (shown = shown || !s.terminal.predictions().isEmpty()), s.row(0) == "$ ab"; });
        CHECK(!shown && s.row(0) == "$ ab");
        s.server.delay = 120;
        QString expected = "$ ab";
        for (int i = 0; i < 20 && s.device->srtt() <= 30; ++i)
            CHECK(s.typeEchoed("c", expected += 'c'));
        CHECK(s.device->srtt() > 30);
        s.type("d");
        CHECK(s.terminal.predictions() == P({{0, int(expected.size()), U'd'}}));
        CHECK(waitFor([&] { return s.row(0) == expected + 'd' && s.terminal.predictions().isEmpty(); }));
    }
}

static void testInterop()
{
    const QString ssh = qEnvironmentVariable("MOSH_INTEROP_SSH");
    if (ssh.isEmpty())
    {
        std::printf("SKIPPED interop: MOSH_INTEROP_SSH not set\n");
        return;
    }
    QProcess proc;
    proc.start("ssh", QStringList{"-tt", "-o", "BatchMode=yes"} + QProcess::splitCommand(ssh) +
                          QStringList{"--", moshServerArguments("60001:60010").join(' ')});
    if (!proc.waitForFinished(20000))
    {
        std::printf("SKIPPED interop: ssh did not finish\n");
        proc.kill();
        return;
    }
    int port = 0;
    QByteArray key;
    if (!parseMoshConnect(proc.readAll(), port, key)) // the output contains the key: do not print it
    {
        std::printf("SKIPPED interop: no MOSH CONNECT line (ssh exit %d)\n", proc.exitCode());
        return;
    }
    MoshDevice dev("127.0.0.1", quint16(port), key);
    key.fill('\0');
    QByteArray output;
    bool finished = false;
    QObject::connect(&dev, &QIODevice::readyRead, [&] { output += dev.readAll(); });
    QObject::connect(&dev, &QIODevice::readChannelFinished, [&] { finished = true; });
    CHECK(waitFor([&] { return !output.isEmpty(); }, 10000)); // first screen (prompt)
    dev.write("echo MOSH-INTEROP-$((40+2))\r");
    CHECK(waitFor([&] { return output.contains("MOSH-INTEROP-42"); }, 10000));
    dev.setWindowSize(100, 30);
    dev.write("stty size | tr ' ' x\r");
    CHECK(waitFor([&] { return output.contains("30x100"); }, 10000));
    dev.write("exit\r");
    CHECK(waitFor([&] { return finished; }, 15000));
    std::printf("interop: %s, %lld bytes of host output\n", finished ? "session closed by server" : "no shutdown",
                qint64(output.size()));
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir settings; // "mosh/prediction" in a temporary file, not in the user's settings
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    app.setOrganizationName("CrossTermTest");
    app.setApplicationName("test_mosh");
    testAes();
    testOcb();
    testCodecs();
    testBootstrap();
    testLoopback();
    testPrediction();
    testInterop();
    std::printf(failures ? "%d failure(s)\n" : "all mosh tests passed\n", failures);
    return failures ? 1 : 0;
}
