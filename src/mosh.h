#pragma once
// Native Mosh client: SSP over UDP, AES-128-OCB3, zlib-compressed protobuf instructions.
// Bootstrap goes through ssh: "ssh -tt host -- <moshServerArguments()>" and parseMoshConnect() on its output.
#include <QByteArray>
#include <QElapsedTimer>
#include <QIODevice>
#include <QPoint>
#include <QTimer>
#include "transports.h"
#include <QList>
#include <QStringList>
#include <array>
#include <cstdint>
#include <memory>
class Terminal;

bool parseMoshConnect(const QByteArray &serverOutput, int &port, QByteArray &key);
// Empty portRange = port chosen by the server; otherwise "60001" or "60001:60010".
QStringList moshServerArguments(const QString &portRange = {});

namespace mosh
{
using Block = std::array<uint8_t, 16>;

// Software AES-128 (FIPS-197). The tables are not constant-time.
class Aes128
{
public:
    explicit Aes128(const uint8_t *key16);
    Block encrypt(Block b) const;
    Block decrypt(Block b) const;

private:
    uint8_t rk[176];
};

// OCB3 (RFC 7253) with a 12-byte nonce and a 16-byte tag.
class Ocb
{
public:
    explicit Ocb(const QByteArray &key16);
    QByteArray encrypt(const QByteArray &nonce12, const QByteArray &plain, const QByteArray &ad = {}) const; // ciphertext||tag
    bool decrypt(const QByteArray &nonce12, const QByteArray &sealed, QByteArray &plain, const QByteArray &ad = {}) const;

private:
    Aes128 aes;
    Block lStar, lDollar, l[32];
    Block offset0(const QByteArray &nonce12) const;
    Block hash(const QByteArray &ad) const;
};

// TransportBuffers.Instruction (transportinstruction.proto).
struct Instruction
{
    quint32 protocolVersion = 2;
    quint64 oldNum = 0, newNum = 0, ackNum = 0, throwawayNum = 0;
    QByteArray diff, chaff;
};
QByteArray encodeInstruction(const Instruction &inst);
bool decodeInstruction(const QByteArray &data, Instruction &inst);

// One UserMessage/HostMessage entry: field numbers match in the two protos
// (Keystroke.keys/HostBytes.hoststring = ext 2 field 4, ResizeMessage = ext 3 fields 5/6, EchoAck = ext 7 field 8).
struct Action
{
    QByteArray bytes;
    int width = -1, height = -1; // >= 0: ResizeMessage
    qint64 echoAck = -1;         // >= 0: EchoAck
};
QByteArray encodeActions(const QList<Action> &actions);
bool decodeActions(const QByteArray &data, QList<Action> &actions);

// Raw zlib streams (as mosh's compress()/uncompress()) via qCompress/qUncompress.
QByteArray zlibCompress(const QByteArray &data);
QByteArray zlibUncompress(const QByteArray &data);

// Datagram: 8-byte nonce (direction bit + 63-bit sequence) + OCB(timestamp, reply, payload).
struct Packet
{
    bool toClient = false;
    quint64 seq = 0;
    quint16 timestamp = 0xFFFF, timestampReply = 0xFFFF;
    QByteArray payload;
};
QByteArray seal(const Ocb &ocb, const Packet &packet);
bool open(const Ocb &ocb, const QByteArray &datagram, Packet &packet);

// Fragments: 64-bit id, 15-bit number + "final" bit, content = compressed instruction.
QList<QByteArray> makeFragments(quint64 id, const Instruction &inst, int maxContent = 462);
struct Assembler
{
    quint64 id = ~0ULL;
    int total = -1;
    QList<QByteArray> parts;
    bool add(const QByteArray &fragment, Instruction &inst); // true when the instruction is complete
};
} // namespace mosh

// Mosh session as a QIODevice: write() = keystrokes, read() = HostBytes to feed to the emulator.
// mosh-server HostBytes are screen diffs against the client's old_num state: only instructions whose old_num
// equals the last applied state are applied, so the terminal (libvterm) always starts from the screen
// the server computed the sequences against. Session end: readChannelFinished() + close(), reason in errorString().
class MoshDevice : public QIODevice, public WindowSizeAware
{
public:
    MoshDevice(const QString &host, quint16 udpPort, const QByteArray &keyBase64, QObject *parent = nullptr);
    ~MoshDevice() override;
    void setWindowSize(int cols, int rows) override;
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;
    void close() override;
    double srtt() const; // smoothed RTT in ms (1000 until a sample exists)

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 size) override;

private:
    struct State;
    std::unique_ptr<State> d;
    void tick();
    void receive();
    void finish(const QString &reason);
};

// Mosh-style local echo: printable characters typed on the cursor line are shown immediately
// (Terminal::setPredictions) and confirmed when the server screen has the same character in that cell.
// Every control key/sequence opens a new "epoch": its predictions stay hidden until the server
// confirms one of them. An error or a prediction not echoed within the timeout (password) discards everything and opens a new
// epoch.
class MoshPredictor
{
public:
    enum Mode
    {
        Never,
        Adaptive, // only with SRTT > 30 ms or a prediction pending for more than 100 ms
        Always
    };
    static Mode modeFromSettings(); // "mosh/prediction": "adaptive" (default), "always", "never"
    MoshPredictor(Terminal &terminal, const MoshDevice &device, Mode mode);
    void input(const QByteArray &keys); // outgoing keystrokes to the server
    void output();                      // after the server output has been applied to the screen
    void reset();

private:
    struct Cell
    {
        int row, col;
        char32_t ch, original, previous; // previous = replaced prediction (e.g. a character later erased)
        int epoch;
        qint64 time;
    };
    Terminal &terminal;
    const MoshDevice &device;
    Mode mode;
    QList<Cell> cells;
    QPoint cursor{-1, -1}; // predicted cursor; x < 0 = restart from the real cursor
    int epoch = 1, confirmedEpoch = 0;
    qint64 lastInput = 0;
    QElapsedTimer clock;
    QTimer timer;
    int timeout() const;
    void discard();
    void publish();
    void tick();
};
