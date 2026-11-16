// Terminal on a QIODevice: reflow on resize, scrollback, writing and end of session.
#include "terminal.h"
#include "transports.h"
#include <QApplication>
#include <QDebug>
#include <QFontMetrics>
#include <QImage>
#include <QPixmap>
#include <QWheelEvent>

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

    // A tab created hidden gets a placeholder size of a few pixels (2 rows): with sizeWhenShown() the welcome banner
    // is still written at the top of a full grid, and the session starts below it; shown, the banner stays on top.
    {
        // As in the window: a child widget, which Qt creates 100x30 pixels; its constructor sets the font and so
        // the grid for that size, 2 rows.
        QWidget host;
        auto *tabDevice = new FakeDevice;
        Terminal tab(tabDevice, &host);
        tab.sizeWhenShown();
        tab.setFontPointSize(11); // a font change recomputes the grid: still not while hidden
        QString banner;
        for (int i = 1; i <= 13; ++i)
            banner += QString("banner %1\r\n").arg(i);
        tab.printLocal(banner);
        CHECK(tab.cursorPosition().y() == 13 && tab.contents().startsWith("banner 1"))
        host.setAttribute(Qt::WA_ShowWithoutActivating);
        host.resize(1200, 900);
        host.show();
        setGrid(tab, 80, 30);
        tabDevice->feed("Linux motd\r\n");
        QApplication::processEvents();
        CHECK(tab.contents().startsWith("banner 1") && tab.contents().contains("Linux motd") && tabDevice->rows == 30)
    }

    // Mouse wheel: scrollback on the normal screen, arrow keys in full-screen programs (alternate screen).
    {
        auto *wheelDevice = new FakeDevice;
        Terminal wheel(wheelDevice);
        wheel.setAttribute(Qt::WA_ShowWithoutActivating);
        wheel.show();
        setGrid(wheel, 40, 10);
        auto spin = [&](int delta)
        {
            QWheelEvent event(QPointF(10, 10), wheel.mapToGlobal(QPointF(10, 10)), {}, {0, delta}, Qt::NoButton,
                              Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(&wheel, &event);
        };
        wheelDevice->written.clear();
        spin(120);
        CHECK(wheelDevice->written.isEmpty())
        wheelDevice->feed("\x1b[?1049h"); // nano, vim, less: alternate screen
        spin(120);
        CHECK(wheelDevice->written == "\x1b[A\x1b[A\x1b[A")
        wheelDevice->written.clear();
        wheelDevice->feed("\x1b[?1h"); // application cursor keys
        spin(-120);
        CHECK(wheelDevice->written == "\x1bOB\x1bOB\x1bOB")
        // nano's redraw after a scroll: cursor hidden, placed, then shown where it is: drawn again.
        wheelDevice->feed("\x1b[?25l\x1b[5;3H");
        QApplication::processEvents();
        const QImage hidden = wheel.grab().toImage();
        wheelDevice->feed("\x1b[?25h");
        QApplication::processEvents();
        CHECK(wheel.grab().toImage() != hidden)
        wheelDevice->feed("\x1b[?1049l");
        wheelDevice->written.clear();
        spin(-120);
        CHECK(wheelDevice->written.isEmpty())
    }

    // Process on a pseudo terminal: TERM describes this emulator (Windows sets none, so ssh sent none to the server).
#ifdef Q_OS_WIN
    Terminal process(Command{"powershell.exe", {"-NoProfile", "-Command", "'term=' + $env:TERM"}, {}, {}});
#else
    Terminal process(Command{"/bin/sh", {"-c", "echo term=$TERM"}, {}, {}});
#endif
    QDeadlineTimer deadline(10000);
    while (!process.allText().contains("term=xterm-256color") && !deadline.hasExpired())
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    CHECK(process.allText().contains("term=xterm-256color"))
    return 0;
}
