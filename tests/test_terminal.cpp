// Terminal on a QIODevice: reflow on resize, scrollback, writing and end of session.
#include "terminal.h"
#include "transports.h"
#include <QApplication>
#include <QDebug>
#include <QFontMetrics>
#include <QImage>
#include <QPixmap>

#define CHECK(expression)                                                                                                        \
    if (!(expression))                                                                                                           \
    {                                                                                                                            \
        qCritical() << "line" << __LINE__ << #expression;                                                                        \
        return 1;                                                                                                                \
    }

// Fake device: feed() simulates server data, written collects what the terminal sent.
class FakeDevice : public QIODevice, public WindowSizeAware
{
public:
    QByteArray input, written;
    int cols = 0, rows = 0;
    FakeDevice() { open(ReadWrite | Unbuffered); }
    bool isSequential() const override { return true; }
    void feed(const QByteArray &data)
    {
        input += data;
        emit readyRead();
    }
    void setWindowSize(int c, int r) override
    {
        cols = c;
        rows = r;
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 count = std::min<qint64>(maxSize, input.size());
        memcpy(data, input.constData(), size_t(count));
        input.remove(0, qsizetype(count));
        return count;
    }
    qint64 writeData(const char *data, qint64 size) override
    {
        written.append(data, qsizetype(size));
        return size;
    }
};

static void setGrid(Terminal &terminal, int cols, int rows)
{
    const QFontMetrics fm(terminal.font());
    terminal.resize(8 + cols * fm.horizontalAdvance('M') + 1, 8 + rows * fm.height() + 1);
    QApplication::processEvents();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    auto *device = new FakeDevice;
    Terminal terminal(device);
    terminal.setAttribute(Qt::WA_ShowWithoutActivating);
    terminal.show();
    setGrid(terminal, 40, 10);
    CHECK(device->cols == 40 && device->rows == 10)

    // 100-character line: 40 + 40 + 20, then 20 x 5, then 50 + 50.
    QByteArray line;
    for (int i = 0; i < 100; ++i)
        line += char('a' + i % 26);
    device->feed(line + "\r\n");
    auto lines = terminal.allText().split('\n');
    CHECK(lines.size() >= 3 && lines[0] == line.left(40) && lines[1] == line.mid(40, 40) && lines[2] == line.mid(80))
    setGrid(terminal, 20, 10);
    lines = terminal.allText().split('\n');
    CHECK(lines.size() >= 5 && lines[0] == line.left(20) && lines[4] == line.mid(80, 20))
    setGrid(terminal, 50, 10);
    lines = terminal.allText().split('\n');
    CHECK(lines.size() >= 2 && lines[0] == line.left(50) && lines[1] == line.mid(50))
    CHECK(terminal.contents().startsWith(QString(line.left(50))))

    // Scrollback: 30 numbered lines in a 5-line screen, the long line ends up in the scrollback.
    setGrid(terminal, 50, 5);
    for (int i = 0; i < 30; ++i)
        device->feed("row " + QByteArray::number(i) + "\r\n");
    const QString before = terminal.allText();
    CHECK(before.contains("row 0\nrow 1\n") && before.contains("row 29"))
    // Narrower: the long line in the scrollback is rewrapped at 20 columns.
    setGrid(terminal, 20, 5);
    lines = terminal.allText().split('\n');
    CHECK(lines[0] == line.left(20) && lines[4] == line.mid(80, 20) && lines[5] == "row 0")
    // Wider and taller: lines restored from the scrollback, identical text without duplicates.
    setGrid(terminal, 50, 12);
    CHECK(terminal.allText() == before)
    CHECK(terminal.contents().contains("row 20"))
    setGrid(terminal, 50, 5);
    setGrid(terminal, 100, 20);
    lines = terminal.allText().split('\n');
    CHECK(lines[0] == QString(line) && lines[1] == "row 0" && terminal.allText().count("row 29") == 1)

    // Long lines straddling scrollback and screen, many different sizes: no character lost or duplicated.
    for (int i = 0; i < 40; ++i)
        device->feed(QByteArray(17 + i * 7 % 90, char('A' + i % 26)) + "\r\n");
    const QString flat = terminal.allText().remove('\n');
    unsigned seed = 12345;
    for (int i = 0; i < 60; ++i)
    {
        seed = seed * 1103515245 + 12345;
        setGrid(terminal, 3 + int(seed >> 8) % 130, 2 + int(seed >> 20) % 30);
        CHECK(terminal.allText().remove('\n') == flat)
    }
    // Wrapping consistent with the final width.
    setGrid(terminal, 100, 8);
    CHECK(terminal.allText().contains('\n' + QString(100, 'M') + "\nMMM\nNNNNNNNNNNNNNNNNNNNN"))
    CHECK(terminal.allText().contains('\n' + QString(96, 'L') + '\n'))

    // Alternate screen (vim, less...): resizing must not touch the scrollback.
    const auto scrollbackLines = terminal.allText().count('\n');
    device->feed("\x1b[?1049h\x1b[Hfull screen app");
    setGrid(terminal, 20, 5);
    setGrid(terminal, 90, 12);
    CHECK(terminal.contents().startsWith("full screen app"))
    device->feed("\x1b[?1049l");
    CHECK(terminal.allText().count('\n') >= scrollbackLines - 1 && terminal.allText().contains(QString(20, 'N')))

    // Keyboard and paste go to the device.
    terminal.paste("ls -l");
    CHECK(device->written == "ls -l")

    // Predictions: drawn over the cell, the real cursor does not move, clearPredictions returns to the real screen.
    device->feed("\x1b[2J\x1b[Hxy");
    CHECK(terminal.cellChar(0, 1) == U'y' && terminal.cellChar(0, 5) == U' ' && terminal.cellChar(-1, 0) == 0 &&
          terminal.cellChar(0, 1000) == 0)
    CHECK(terminal.cursorPosition() == QPoint(2, 0))
    const QImage plain = terminal.grab().toImage();
    terminal.setPredictions({{0, 2, U'Z'}, {0, 3, U'W'}}, QPoint(4, 0));
    CHECK(terminal.predictions().size() == 2 && terminal.cursorPosition() == QPoint(2, 0))
    const QImage predicted = terminal.grab().toImage();
    const QFontMetrics fm(terminal.font());
    const QRect cell(4 + 3 * fm.horizontalAdvance('M'), 4, fm.horizontalAdvance('M'), fm.height());
    CHECK(predicted.copy(cell) != plain.copy(cell)) // the predicted "W" is drawn
    CHECK(terminal.cellChar(0, 3) == U' ')          // but the real screen does not change
    terminal.clearPredictions();
    CHECK(terminal.predictions().isEmpty() && terminal.grab().toImage() == plain)

    // Channel close: "Session stopped" notice and finished().
    CHECK(!terminal.finished())
    device->feed("bye\r\n");
    emit device->readChannelFinished();
    CHECK(terminal.finished())
    CHECK(terminal.allText().contains("Session stopped"))
    CHECK(terminal.allText().contains("bye"))
    return 0;
}
