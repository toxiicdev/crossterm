#include "terminal.h"
#include "mosh.h"
#include "transports.h"
#include <QAbstractSocket>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QUrl>
#include <deque>
#include <vector>
#include <QPainter>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <vterm.h>
#include <algorithm>
#include <cmath>

namespace
{
// The key that makes control characters (^C, ^D...). Qt reports the macOS Command key as ControlModifier and the
// Control key as MetaModifier: on macOS Control talks to the shell and Command runs CrossTerm commands.
#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifier ControlKey = Qt::MetaModifier;
#else
constexpr Qt::KeyboardModifier ControlKey = Qt::ControlModifier;
#endif
} // namespace
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#if defined(Q_OS_MACOS)
#include <util.h>
#else
#include <pty.h>
#include <sys/prctl.h>
#endif
#endif

// Empty cell with the default colors.
static VTermScreenCell blankCell()
{
    VTermScreenCell cell{};
    cell.width = 1;
    cell.fg.type = VTERM_COLOR_DEFAULT_FG;
    cell.bg.type = VTERM_COLOR_DEFAULT_BG;
    return cell;
}

// Appends the characters of a cell (base character and combining marks) to text.
static void appendCell(QString &text, const VTermScreenCell &cell)
{
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i)
    {
        const char32_t c = cell.chars[i] > 0x10FFFF ? char32_t(0xFFFD) : char32_t(cell.chars[i]);
        if (QChar::requiresSurrogates(c))
        {
            text += QChar(QChar::highSurrogate(c));
            text += QChar(QChar::lowSurrogate(c));
        }
        else
            text += QChar(char16_t(c));
    }
}

// Color a moved toward b: t = 0 gives a, 1 gives b.
static QColor blend(const QColor &a, const QColor &b, double t)
{
    auto mix = [t](int from, int to) { return int(std::lround(from + (to - from) * t)); };
    return QColor(mix(a.red(), b.red()), mix(a.green(), b.green()), mix(a.blue(), b.blue()));
}

// WCAG relative luminance (0 black, 1 white) and contrast ratio (1 to 21).
static double luminance(const QColor &color)
{
    auto channel = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
}

static double contrast(const QColor &a, const QColor &b)
{
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// Text color for a filled area: the scheme color (a or b) that stands out more, black or white when neither does.
static QColor readable(const QColor &fill, const QColor &a, const QColor &b)
{
    const QColor best = contrast(fill, a) >= contrast(fill, b) ? a : b;
    if (contrast(fill, best) >= 4.5)
        return best;
    return luminance(fill) > 0.18 ? QColor(Qt::black) : QColor(Qt::white);
}

// Box-drawing lines U+2500-257F as arms from the cell center: nibbles left, right, up, down with 1 = light, 2 = heavy,
// 3 = double. 0 = dashes, arcs and diagonals, drawn apart.
static const uint16_t boxArms[128] = {
    0x1100, 0x2200, 0x0011, 0x0022, 0,      0,      0,      0,      0,      0,      0,      0,      0x0101, 0x0201, 0x0102,
    0x0202, 0x1001, 0x2001, 0x1002, 0x2002, 0x0110, 0x0210, 0x0120, 0x0220, 0x1010, 0x2010, 0x1020, 0x2020, 0x0111, 0x0211,
    0x0121, 0x0112, 0x0122, 0x0221, 0x0212, 0x0222, 0x1011, 0x2011, 0x1021, 0x1012, 0x1022, 0x2021, 0x2012, 0x2022, 0x1101,
    0x2101, 0x1201, 0x2201, 0x1102, 0x2102, 0x1202, 0x2202, 0x1110, 0x2110, 0x1210, 0x2210, 0x1120, 0x2120, 0x1220, 0x2220,
    0x1111, 0x2111, 0x1211, 0x2211, 0x1121, 0x1112, 0x1122, 0x2121, 0x1221, 0x2112, 0x1212, 0x2221, 0x2212, 0x2122, 0x1222,
    0x2222, 0,      0,      0,      0,      0x3300, 0x0033, 0x0301, 0x0103, 0x0303, 0x3001, 0x1003, 0x3003, 0x0310, 0x0130,
    0x0330, 0x3010, 0x1030, 0x3030, 0x0311, 0x0133, 0x0333, 0x3011, 0x1033, 0x3033, 0x3301, 0x1103, 0x3303, 0x3310, 0x1130,
    0x3330, 0x3311, 0x1133, 0x3333, 0,      0,      0,      0,      0,      0,      0,      0x1000, 0x0010, 0x0100, 0x0001,
    0x2000, 0x0020, 0x0200, 0x0002, 0x1200, 0x0012, 0x2100, 0x0021};

// Paints a box-drawing or block character (U+2500-259F) over its whole cell, so the lines and blocks of neighbouring
// cells join without gaps whatever the font. bg = cell background (double lines are a band with the middle removed).
static void drawBlock(QPainter &painter, const QRect &box, uint32_t c, const QColor &fg, const QColor &bg)
{
    const int x = box.left(), y = box.top(), w = box.width(), h = box.height();
    if (c >= 0x2580)
    {
        // Block elements: eighths from the bottom or the left, shades, quadrants. Halves are complementary.
        auto rows = [h](int eighths) { return std::max(1, (h * eighths + 4) / 8); };
        auto cols = [w](int eighths) { return std::max(1, (w * eighths + 4) / 8); };
        const int left = cols(4), top = h - rows(4);
        if (c == 0x2580)
            painter.fillRect(x, y, w, top, fg);
        else if (c <= 0x2588)
            painter.fillRect(x, y + h - rows(int(c - 0x2580)), w, rows(int(c - 0x2580)), fg);
        else if (c <= 0x258F)
            painter.fillRect(x, y, cols(int(0x2590 - c)), h, fg);
        else if (c == 0x2590)
            painter.fillRect(x + left, y, w - left, h, fg);
        else if (c <= 0x2593)
        {
            QColor shade = fg;
            shade.setAlphaF(0.25f * float(c - 0x2590));
            painter.fillRect(box, shade);
        }
        else if (c == 0x2594)
            painter.fillRect(x, y, w, rows(1), fg);
        else if (c == 0x2595)
            painter.fillRect(x + w - cols(1), y, cols(1), h, fg);
        else
        {
            // Quadrants U+2596-259F: bit 0 upper left, 1 upper right, 2 lower left, 3 lower right.
            static const uint8_t quadrants[10] = {4, 8, 1, 13, 9, 7, 11, 2, 6, 14};
            const int mask = quadrants[c - 0x2596];
            if (mask & 1)
                painter.fillRect(x, y, left, top, fg);
            if (mask & 2)
                painter.fillRect(x + left, y, w - left, top, fg);
            if (mask & 4)
                painter.fillRect(x, y + top, left, h - top, fg);
            if (mask & 8)
                painter.fillRect(x + left, y + top, w - left, h - top, fg);
        }
        return;
    }
    const int light = std::max(1, (w + 3) / 7), heavy = 2 * light;
    const int midX = x + w / 2, midY = y + h / 2;
    if (const int arms = boxArms[c - 0x2500])
    {
        const int l = arms >> 12, r = (arms >> 8) & 15, u = (arms >> 4) & 15, d = arms & 15;
        auto thickness = [&](int weight) { return weight == 3 ? 3 * light : weight == 2 ? heavy : weight ? light : 0; };
        const int across = std::max(l, r), along = std::max(u, d); // weights of the horizontal and vertical strokes
        const int th = thickness(across), tv = thickness(along);   // their thickness
        const int hy = y + (h - th) / 2, vx = x + (w - tv) / 2;    // top of the horizontal, left of the vertical
        // Each arm runs from its edge over the stroke it meets, so corners are filled. A single arm ending on a double
        // stroke it does not cross stops at the nearer line when that stroke goes on both ways (tees), else at the farther.
        auto single = [](int arm, int crossed) { return arm && arm != 3 && crossed == 3; };
        if (l)
        {
            const int end = !tv ? midX : single(l, along) && !r && u && d ? vx + light : vx + tv;
            painter.fillRect(x, y + (h - thickness(l)) / 2, end - x, thickness(l), fg);
        }
        if (r)
        {
            const int start = !tv ? midX : single(r, along) && !l && u && d ? vx + 2 * light : vx;
            painter.fillRect(start, y + (h - thickness(r)) / 2, x + w - start, thickness(r), fg);
        }
        if (u)
        {
            const int end = !th ? midY : single(u, across) && !d && l && r ? hy + light : hy + th;
            painter.fillRect(x + (w - thickness(u)) / 2, y, thickness(u), end - y, fg);
        }
        if (d)
        {
            const int start = !th ? midY : single(d, across) && !u && l && r ? hy + 2 * light : hy;
            painter.fillRect(x + (w - thickness(d)) / 2, start, thickness(d), y + h - start, fg);
        }
        // Double arms: the middle third is removed, up to the center of a double crossing, up to a single stroke that
        // crosses or closes the arm, or through the cell when the arm continues on the other side.
        if (l == 3)
        {
            const int end = along == 3 ? vx + 2 * light : !tv ? midX : u && d ? vx : r ? midX : vx;
            painter.fillRect(x, hy + light, end - x, light, bg);
        }
        if (r == 3)
        {
            const int start = along == 3 ? vx + light : !tv ? midX : u && d ? vx + tv : l ? midX : vx + tv;
            painter.fillRect(start, hy + light, x + w - start, light, bg);
        }
        if (u == 3)
        {
            const int end = across == 3 ? hy + 2 * light : !th ? midY : l && r ? hy : d ? midY : hy;
            painter.fillRect(vx + light, y, light, end - y, bg);
        }
        if (d == 3)
        {
            const int start = across == 3 ? hy + light : !th ? midY : l && r ? hy + th : u ? midY : hy + th;
            painter.fillRect(vx + light, start, light, y + h - start, bg);
        }
        return;
    }
    if (c < 0x2550)
    {
        // Dashed lines: U+2504-250B three and four dashes, U+254C-254F two; bit 0 heavy, bit 1 vertical.
        const int count = c >= 0x254C ? 2 : c >= 0x2508 ? 4 : 3, thick = c & 1 ? heavy : light;
        const bool vertical = c & 2;
        const int length = vertical ? h : w;
        for (int i = 0; i < count; ++i)
        {
            const int from = length * i / count, to = length * (i + 1) / count;
            const int gap = std::max(1, (to - from) / 3), dash = std::max(1, to - from - gap);
            if (vertical)
                painter.fillRect(x + (w - thick) / 2, y + from + gap / 2, thick, dash, fg);
            else
                painter.fillRect(x + from + gap / 2, y + (h - thick) / 2, dash, thick, fg);
        }
        return;
    }
    // Rounded corners U+256D-2570 and diagonals U+2571-2573: antialiased strokes.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(fg, light, Qt::SolidLine, Qt::FlatCap));
    if (c <= 0x2570)
    {
        const bool right = c == 0x256D || c == 0x2570, down = c <= 0x256E;
        const qreal radius = std::min(w, h) / 2.0, cx = x + (w - light) / 2 + light / 2.0, cy = y + (h - light) / 2 + light / 2.0;
        // Quarter circle from the vertical line to the horizontal one, then the straight rest of each up to the edges.
        const QRectF circle(right ? cx : cx - 2 * radius, down ? cy : cy - 2 * radius, 2 * radius, 2 * radius);
        painter.drawArc(circle, (down ? (right ? 90 : 0) : (right ? 180 : 270)) * 16, 90 * 16);
        const int bendX = int(right ? std::floor(cx + radius) : std::ceil(cx - radius));
        const int bendY = int(down ? std::floor(cy + radius) : std::ceil(cy - radius));
        if (right ? bendX < x + w : bendX > x)
            painter.fillRect(right ? bendX : x, y + (h - light) / 2, right ? x + w - bendX : bendX - x, light, fg);
        if (down ? bendY < y + h : bendY > y)
            painter.fillRect(x + (w - light) / 2, down ? bendY : y, light, down ? y + h - bendY : bendY - y, fg);
    }
    else
    {
        if (c != 0x2572)
            painter.drawLine(QPointF(x, y + h), QPointF(x + w, y));
        if (c != 0x2571)
            painter.drawLine(QPointF(x, y), QPointF(x + w, y + h));
    }
    painter.restore();
}

#ifndef Q_OS_WIN
// Reaps a child sent SIGHUP without blocking the UI, so it does not stay a zombie until CrossTerm exits.
static void reap(pid_t pid, int attempt = 0)
{
    const pid_t result = waitpid(pid, nullptr, WNOHANG);
    if (result > 0 || (result < 0 && errno != EINTR) || !QCoreApplication::instance())
        return;
    QTimer::singleShot(attempt < 50 ? 100 : 5000, [pid, attempt] { reap(pid, attempt + 1); });
}
#endif

struct Terminal::State
{
    int rows = 24, cols = 80;
    VTerm *vt = vterm_new(rows, cols);
    VTermScreen *screen = vterm_obtain_screen(vt);
    bool cursor = true, ended = false, selecting = false;
    // Scrollback and selection: absolute rows, negative = scrollback (max 5000 lines).
    // continuation = the line continues the previous one (auto-wrap), used for reflow.
    struct Line
    {
        std::vector<VTermScreenCell> cells;
        bool continuation = false;
    };
    std::deque<Line> scrollback;
    int offset = 0;
    bool alternate = false, popping = true, joinTop = false;
    int pushed = 0;           // lines pushed to the scrollback during the last vterm_set_size
    std::vector<bool> popped; // continuation flags of lines popped back onto the screen during the last vterm_set_size
    int logged = 0;           // lines at the top of the screen already written to the log (popped from the scrollback)
    QIODevice *device = nullptr;
    // Local echo: predicted cells and predicted cursor (x < 0 = real cursor); feeding = server output in progress,
    // libvterm replies (e.g. cursor reports) are not keystrokes to predict.
    QList<PredictedCell> predictions;
    QPoint predictedCursor{-1, -1};
    bool predictionUnderline = true, feeding = false;
    // Inline askpass prompt in progress (promptDone set): typed text goes to promptBuffer, not to the session.
    std::function<void(std::optional<QString>)> promptDone;
    QList<QByteArray> hiddenOutput; // whole lines dropped from the session output (hideOutput)
    QString promptBuffer;
    bool promptEcho = false;
    // Keys typed before ssh has logged in (holdKeysUntilLogin): kept here, in order, instead of being sent.
    struct HeldKey
    {
        int key;
        Qt::KeyboardModifiers modifiers;
        QString text; // of the key, or the pasted text
        bool paste;
    };
    QList<HeldKey> heldKeys;
    bool holding = false;
    bool sizeWhenShown = false; // sizeWhenShown(): resizes ignored until the first show
    std::unique_ptr<MoshPredictor> predictor;
    QPoint anchor, extent;
    std::unique_ptr<QFile> log; // session log: lines scrolled off the screen + final screen
    // Log with times: when the cursor last left or entered each screen row, the moment its text was written.
    bool logTimes = false;
    std::deque<qint64> rowTimes;
    qint64 &rowTime(int row)
    {
        if (int(rowTimes.size()) < rows)
            rowTimes.resize(size_t(rows), QDateTime::currentMSecsSinceEpoch());
        return rowTimes[size_t(std::clamp(row, 0, int(rowTimes.size()) - 1))];
    }
    void writeLog(const QString &text, qint64 time)
    {
        const auto stamp = logTimes ? "[" + QDateTime::fromMSecsSinceEpoch(time).toString("yyyy-MM-dd HH:mm:ss.zzz") + "] "
                                    : QString();
        log->write((stamp + text + "\n").toUtf8());
    }
    QColor background{"#1c1c1c"}, foreground{"#dadada"}, cursorColor{"#c8c8c8"};
    // Derived from the scheme (applyScheme): selection, other search matches, character under the block cursor.
    QColor selection, selectionText, match, matchText, cursorText;
    std::array<QColor, 16> ansi;
    QString needle; // text of the last search: every occurrence on screen is highlighted
    Qt::CaseSensitivity needleCase = Qt::CaseInsensitive;
    // Link under the mouse while Cmd/Ctrl is held: cells from linkFrom to linkTo as the selection (equal = none);
    // hover = cell it was looked up for (x < 0 = none).
    QPoint linkFrom, linkTo, hover{-1, 0};
    QUrl link;
    MoshDevice *mosh = nullptr; // the device, when it is a mosh session ("last contact" banner)
    QTimer contact;             // refreshes the banner every second, only while the server is silent
    QTimer repaint;             // running = a repaint happened less than 16 ms ago
    bool stale = false;         // output arrived meanwhile: repaint when the timer fires
    QByteArray osc;    // incoming OSC 7 (file://host/path)
    QString directory; // last directory announced by the shell via OSC 7
    // Host of the first OSC 7 / first "user@host:path$" prompt: the session's own. elsewhere = the last OSC 7 came
    // from another host (nested ssh), so the SFTP browser must not follow its folders on this server.
    QString oscHost, promptHost;
    bool elsewhere = false;
    QString lineText(int line) const
    {
        QString result;
        for (int col = 0; col < cols; ++col)
        {
            VTermScreenCell c{};
            if (!cell(line, col, c) || c.chars[0] == UINT32_MAX)
                continue;
            if (c.chars[0])
                appendCell(result, c);
            else
                result += ' ';
        }
        while (result.endsWith(' '))
            result.chop(1);
        return result;
    }
    // Appends the text of a row and the column of each UTF-16 unit (wide, non-BMP and combining characters take more
    // than one); an empty cell and the second half of a wide character count as a space.
    void rowText(int line, QString &text, std::vector<int> &columns) const
    {
        for (int col = 0; col < cols; ++col)
        {
            VTermScreenCell c{};
            if (cell(line, col, c) && c.chars[0] && c.chars[0] != UINT32_MAX)
                appendCell(text, c);
            else
                text += ' ';
            columns.resize(size_t(text.size()), col);
        }
    }
    // The row continues the previous one (auto-wrap).
    bool continued(int line) const
    {
        if (line >= 0)
            return line < rows && vterm_state_get_lineinfo(vterm_obtain_state(vt), line)->continuation;
        const int index = int(scrollback.size()) + line;
        return index >= 0 && scrollback[size_t(index)].continuation;
    }
    bool cell(int line, int col, VTermScreenCell &out) const
    {
        if (line >= 0)
            return vterm_screen_get_cell(screen, {line, col}, &out);
        const int index = int(scrollback.size()) + line;
        if (index < 0)
            return false;
        const auto &row = scrollback[size_t(index)].cells;
        out = col < int(row.size()) ? row[size_t(col)] : blankCell();
        return true;
    }
    // Row 0 may continue a line already in the scrollback: libvterm's reflow would read it outside the buffer,
    // so it is detached and the continuation moves to the first line pushed to the scrollback (joinTop).
    void detachTop()
    {
        joinTop = false;
        if (alternate)
            return;
        auto *top = const_cast<VTermLineInfo *>(vterm_state_get_lineinfo(vterm_obtain_state(vt), 0));
        joinTop = top->continuation;
        top->continuation = 0;
    }
    void pushLine(Line line)
    {
        line.continuation = line.continuation || joinTop;
        joinTop = false;
        scrollback.push_back(std::move(line));
        ++pushed;
        if (logged > 0)
            --logged;
        else if (log)
            writeLog(lineText(-1), rowTime(0));
        if (logTimes) // the screen moved up one row
        {
            rowTime(0);
            rowTimes.pop_front();
            rowTimes.push_back(QDateTime::currentMSecsSinceEpoch());
        }
        if (scrollback.size() > 5000)
            scrollback.pop_front();
        // Every line moved up by one: a scrolled-back view and the selection stay on their text
        // (also with a full scrollback, where the oldest line has just been dropped).
        if (offset)
            offset = std::min(offset + 1, int(scrollback.size()));
        if (anchor != extent)
        {
            anchor.ry()--;
            extent.ry()--;
        }
    }
    // One libvterm resize. popping = lines may be pulled back from the scrollback.
    void setSize(int newRows, int newCols, bool popping)
    {
        detachTop();
        popped.clear();
        this->popping = popping;
        auto *state = vterm_obtain_state(vt);
        VTermPos cursor;
        vterm_state_get_cursorpos(state, &cursor);
        const bool lineStart = cursor.col == 0 && !vterm_state_get_lineinfo(state, cursor.row)->continuation;
        // If a logical line no longer fits, libvterm pushes only its first row to the scrollback and drops the rest:
        // keep a copy of the screen to append them back.
        std::vector<Line> snapshot;
        for (int row = 0; row < rows && !alternate; ++row)
        {
            Line line{std::vector<VTermScreenCell>(size_t(cols)), vterm_state_get_lineinfo(state, row)->continuation != 0};
            for (int col = 0; col < cols; ++col)
                vterm_screen_get_cell(screen, {row, col}, &line.cells[size_t(col)]);
            snapshot.push_back(std::move(line));
        }
        pushed = 0;
        // With the alternate screen active, row 0 of the main screen is unreachable: no reflow.
        vterm_screen_enable_reflow(screen, !alternate);
        vterm_set_size(vt, newRows, newCols);
        joinTop = false;
        this->popping = true;
        const int first = pushed;
        for (int row = first; first > 0 && row < int(snapshot.size()) && snapshot[size_t(row)].continuation; ++row)
            pushLine(std::move(snapshot[size_t(row)]));
        rows = newRows;
        cols = newCols;
        // libvterm moves a cursor at the start of a line to the end of the previous line when that line exactly fills
        // the columns: move it back to the start of the next line.
        vterm_state_get_cursorpos(state, &cursor);
        if (lineStart && cursor.col != 0 && cursor.row + 1 < newRows)
        {
            const auto move = QByteArray("\x1b[") + QByteArray::number(cursor.row + 2) + ";1H";
            vterm_input_write(vt, move.constData(), size_t(move.size()));
        }
        // Lines pulled back from the scrollback land at the top: restore their continuation flags.
        const int count = int(popped.size());
        if (!alternate)
        {
            for (int row = 0; row < count && row < newRows; ++row)
                const_cast<VTermLineInfo *>(vterm_state_get_lineinfo(state, row))->continuation = popped[size_t(count - 1 - row)];
        }
        logged = std::min(logged + count, newRows);
    }
    // Reflow: libvterm reflows the screen, the scrollback is reflowed here; then the freed rows are filled from the
    // scrollback, already at the new width.
    void resize(int newRows, int newCols)
    {
        auto *state = vterm_obtain_state(vt);
        // 0) if row 0 continues a scrollback line, the whole logical line moves back onto the screen
        if (!alternate && !scrollback.empty() && vterm_state_get_lineinfo(state, 0)->continuation)
        {
            int head = int(scrollback.size()) - 1;
            while (head > 0 && scrollback[size_t(head)].continuation)
                --head;
            head = int(scrollback.size()) - head; // rows of the part in the scrollback
            // Bounded: pulling back a huge logical line would need a screen that tall; it stays split at the boundary.
            if (rows + head <= 1000)
            {
                setSize(rows + head, cols, true);
                const_cast<VTermLineInfo *>(vterm_state_get_lineinfo(state, head))->continuation = 1;
            }
        }
        VTermPos cursor;
        vterm_state_get_cursorpos(state, &cursor);
        const int below = rows - 1 - cursor.row; // rows below the cursor before the resize
        // 1) new width and any smaller height, without pulling lines back
        const bool reflowed = newCols != cols;
        setSize(std::min(rows, newRows), newCols, false);
        if (reflowed)
            rewrap();
        // 2) if the content moved up leaving empty rows at the bottom, remove them...
        vterm_state_get_cursorpos(state, &cursor);
        const int freed = rows - 1 - cursor.row - below;
        if (freed > 0 && !alternate && !scrollback.empty())
            setSize(rows - freed, cols, false);
        // 3) ...then grow to the final height, pulling lines back from the scrollback (already at the new width).
        if (rows != newRows)
            setSize(newRows, cols, true);
    }
    // Rewraps the scrollback to the current width by joining continued lines.
    void rewrap()
    {
        std::deque<Line> result;
        std::vector<VTermScreenCell> logical;
        for (size_t i = 0; i < scrollback.size(); ++i)
        {
            auto &cells = scrollback[i].cells;
            logical.insert(logical.end(), cells.begin(), cells.end());
            if (i + 1 < scrollback.size() && scrollback[i + 1].continuation)
                continue;
            while (!logical.empty() && !logical.back().chars[0])
                logical.pop_back();
            size_t at = 0;
            do
            {
                size_t end = std::min(logical.size(), at + size_t(cols));
                if (end < logical.size() && end - at > 1 && logical[end - 1].width == 2)
                    --end; // a wide character wraps as a whole
                result.push_back({{logical.begin() + qsizetype(at), logical.begin() + qsizetype(end)}, at > 0});
                at = end;
            } while (at < logical.size());
            logical.clear();
        }
        while (result.size() > 5000)
            result.pop_front();
        scrollback = std::move(result);
    }
#ifdef Q_OS_WIN
    HANDLE input = nullptr, output = nullptr, process = nullptr;
    HPCON console = nullptr;
#else
    int fd = -1;
    pid_t pid = -1;
    std::unique_ptr<QSocketNotifier> readable; // PTY output: read when it arrives instead of polling
    // Input the PTY has not accepted yet (its buffer holds about 1 KB in raw mode): written when it becomes writable.
    QByteArray pending;
    std::unique_ptr<QSocketNotifier> writable;
    void flush()
    {
        while (!pending.isEmpty())
        {
            const auto count = ::write(fd, pending.constData(), size_t(pending.size()));
            if (count > 0)
                pending.remove(0, qsizetype(count));
            else if (count < 0 && errno == EINTR)
                continue;
            else
            {
                if (count == 0 || errno != EAGAIN)
                    pending.clear(); // the session is gone
                break;
            }
        }
        if (!pending.isEmpty() && !writable)
        {
            writable = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Write);
            QObject::connect(writable.get(), &QSocketNotifier::activated, writable.get(), [this] { flush(); });
        }
        if (writable)
            writable->setEnabled(!pending.isEmpty());
    }
#endif
    int wheelDelta = 0, zoomDelta = 0; // wheel angle not yet turned into lines / zoom steps (touchpads)
    ~State()
    {
#ifdef Q_OS_WIN
        if (process)
        {
            TerminateProcess(process, 0);
            CloseHandle(process);
        }
        if (input)
            CloseHandle(input);
        if (output)
            CloseHandle(output);
        if (console)
            ClosePseudoConsole(console);
#else
        writable.reset(); // before their descriptor is closed
        readable.reset();
        if (fd >= 0)
            ::close(fd); // global close(): QWidget::close would shadow it
        if (pid > 0)
        {
            kill(pid, SIGHUP);
            reap(pid);
        }
#endif
        if (log)
        {
            int last = rows - 1;
            while (last >= 0 && lineText(last).isEmpty())
                --last;
            for (int row = std::min(logged, last + 1); row <= last; ++row)
                writeLog(lineText(row), rowTime(row));
            log->close();
        }
        vterm_free(vt);
    }
};

#ifdef Q_OS_WIN
static QString quote(const QString &arg)
{
    QString result = "\"";
    int slashes = 0;
    for (const auto c : arg)
    {
        if (c == '\\')
        {
            ++slashes;
            continue;
        }
        result += QString(slashes * (c == '"' ? 2 : 1) + (c == '"' ? 1 : 0), '\\');
        slashes = 0;
        result += c;
    }
    return result + QString(slashes * 2, '\\') + '"';
}
#endif

static QString platformFont()
{
#ifdef Q_OS_WIN
    return "Consolas";
#elif defined(Q_OS_MACOS)
    return "Menlo";
#else
    return "DejaVu Sans Mono";
#endif
}

void Terminal::setup()
{
    auto &s = *state;
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true); // links are looked up under the mouse while Cmd/Ctrl is held
    s.repaint.setSingleShot(true);
    connect(&s.repaint, &QTimer::timeout, this,
            [this]
            {
                if (!state->stale)
                    return;
                state->stale = false;
                update();
                state->repaint.start(16);
            });
    QSettings settings;
    QFont face = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const auto family = settings.value("terminal/font", platformFont()).toString();
    if (QFontDatabase::hasFamily(family))
        face.setFamily(family);
    face.setPointSize(settings.value("terminal/size", 10).toInt());
    face.setStyleHint(QFont::Monospace);
    setFont(face);
    s.predictionUnderline = settings.value("terminal/predictionUnderline", true).toBool();
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled); // without it Qt composes no dead keys and passes no IME text
    setAccessibleName("Terminal");
    vterm_set_utf8(s.vt, 1);
    vterm_screen_enable_altscreen(s.screen, 1);
    vterm_screen_reset(s.screen, 1);
    vterm_output_set_callback(
        s.vt, [](const char *p, size_t n, void *self) { static_cast<Terminal *>(self)->write(QByteArray(p, qsizetype(n))); },
        this);
    static const VTermScreenCallbacks callbacks = []
    {
        VTermScreenCallbacks cb{};
        cb.movecursor = [](VTermPos pos, VTermPos old, int visible, void *context)
        {
            auto &s = *static_cast<State *>(context);
            if (s.logTimes && pos.row != old.row)
                s.rowTime(old.row) = s.rowTime(pos.row) = QDateTime::currentMSecsSinceEpoch();
            s.cursor = visible;
            return 1;
        };
        cb.sb_pushline4 = [](int cols, const VTermScreenCell *cells, bool continuation, void *context)
        {
            // Trailing empty cells are not stored (cell() returns them as blanks): much less scrollback memory.
            while (cols > 0 && !cells[cols - 1].chars[0] && !cells[cols - 1].attrs.reverse &&
                   VTERM_COLOR_IS_DEFAULT_BG(&cells[cols - 1].bg))
                --cols;
            static_cast<State *>(context)->pushLine({{cells, cells + cols}, continuation});
            return 1;
        };
        // The screen grows: libvterm asks back for the last scrollback lines (cols cells).
        cb.sb_popline = [](int cols, VTermScreenCell *cells, void *context)
        {
            auto &s = *static_cast<State *>(context);
            if (!s.popping || s.scrollback.empty())
                return 0;
            const auto &line = s.scrollback.back();
            for (int col = 0; col < cols; ++col)
                cells[col] = col < int(line.cells.size()) ? line.cells[size_t(col)] : blankCell();
            s.popped.push_back(line.continuation);
            s.scrollback.pop_back();
            return 1;
        };
        cb.sb_clear = [](void *context)
        {
            auto &s = *static_cast<State *>(context);
            s.scrollback.clear();
            s.offset = 0;
            s.anchor = s.extent;
            return 1;
        };
        cb.settermprop = [](VTermProp prop, VTermValue *value, void *context)
        {
            if (prop == VTERM_PROP_ALTSCREEN)
                static_cast<State *>(context)->alternate = value->boolean;
            // Shown again without moving (nano and vim hide it, redraw, place it, then show it): movecursor alone
            // would leave it hidden until the next move.
            if (prop == VTERM_PROP_CURSORVISIBLE)
                static_cast<State *>(context)->cursor = value->boolean;
            return 1;
        };
        return cb;
    }();
    vterm_screen_set_callbacks(s.screen, &callbacks, &s);
    vterm_screen_callbacks_has_pushline4(s.screen);
    vterm_screen_enable_reflow(s.screen, true);
    static const VTermStateFallbacks fallbacks = []
    {
        VTermStateFallbacks fb{};
        fb.osc = [](int command, VTermStringFragment frag, void *context)
        {
            if (command != 7)
                return 0;
            auto &s = *static_cast<State *>(context);
            if (frag.initial)
                s.osc.clear();
            if (s.osc.size() + qsizetype(frag.len) <= 4096) // a path; the session cannot grow it without limit
                s.osc.append(frag.str, qsizetype(frag.len));
            if (frag.final)
            {
                const QUrl url(QString::fromUtf8(s.osc));
                if (url.scheme() == "file" && !url.path().isEmpty())
                {
                    // The first host announced is the session's. A different one later is a shell on another
                    // machine (ssh typed inside the session, a container): its folders are not this server's.
                    if (s.oscHost.isEmpty())
                        s.oscHost = url.host();
                    s.elsewhere = !url.host().isEmpty() && !s.oscHost.isEmpty() && url.host() != s.oscHost;
                    s.directory = s.elsewhere ? QString() : url.path();
                }
            }
            return 1;
        };
        return fb;
    }();
    vterm_screen_set_unrecognised_fallbacks(s.screen, &fallbacks, &s);
    applyScheme();
}

Terminal::Terminal(QIODevice *device, QWidget *parent) : QWidget(parent), state(std::make_unique<State>())
{
    setup();
    state->device = device;
    device->setParent(this);
    if (auto *mosh = dynamic_cast<MoshDevice *>(device))
    {
        const auto mode = MoshPredictor::modeFromSettings();
        if (mode != MoshPredictor::Never)
            state->predictor = std::make_unique<MoshPredictor>(*this, *mosh, mode);
        // "Last contact" banner on the top row: the device tells when the server goes silent and when it is back; the
        // timer runs only in between, for the seconds counter.
        state->mosh = mosh;
        auto refresh = [this]
        {
            auto &s = *state;
            if (!s.ended && s.mosh->silence() >= 4000)
            {
                if (!s.contact.isActive())
                    s.contact.start(1000);
            }
            else
                s.contact.stop();
            update(0, 0, width(), 4 + QFontMetrics(font()).height());
        };
        connect(&state->contact, &QTimer::timeout, this, refresh);
        mosh->onSilence = refresh;
    }
    connect(device, &QIODevice::readyRead, this, [this] { readDevice(); });
    for (const auto signal : {&QIODevice::readChannelFinished, &QIODevice::aboutToClose})
    {
        connect(device, signal, this,
                [this]
                {
                    // "Mosh session ended" (exit typed in the session) is a normal end, not an error.
                    const auto *mosh = dynamic_cast<MoshDevice *>(state->device);
                    stop(state->device->errorString(), !(mosh && mosh->endedNormally()));
                });
    }
    if (auto *socket = qobject_cast<QAbstractSocket *>(device))
    {
        connect(socket, &QAbstractSocket::disconnected, this, [this] { stop(); });
        connect(socket, &QAbstractSocket::errorOccurred, this, [this] { stop(state->device->errorString()); });
    }
    if (!device->isOpen() && !device->open(QIODevice::ReadWrite))
        stop(device->errorString());
    else
        readDevice();
}

void Terminal::readDevice()
{
    auto &s = *state;
    const auto data = s.device->readAll();
    if (data.isEmpty())
        return;
    s.feeding = true;
    feed(data);
    s.feeding = false;
    vterm_screen_flush_damage(s.screen);
    if (s.predictor)
        s.predictor->output();
    outputChanged();
}

void Terminal::outputChanged()
{
    // The first chunk is painted at once (typing echo), the ones arriving within 16 ms together: a program writing
    // many small chunks does not repaint the whole grid for each of them.
    auto &s = *state;
    if (s.repaint.isActive())
    {
        s.stale = true;
        return;
    }
    update();
    s.repaint.start(16);
}

void Terminal::stop(const QString &reason, bool error)
{
    auto &s = *state;
    if (s.ended)
        return;
    if (s.device && s.device->isOpen())
        readDevice(); // last data still in the buffer
    s.ended = true;
    if (s.promptDone)
        finishPrompt(std::nullopt);
    s.holding = false;
    dropHeldKeys();
    s.predictor.reset();
    clearPredictions();
    s.contact.stop();
    auto tr = [](const char *text) { return QCoreApplication::translate("CrossTerm", text); };
    const QString newline = QString::fromLatin1("\r\n");
    // Without an error set, QIODevice reports "Unknown error": don't show it.
    static const QString unknown = QBuffer().errorString();
    // Errors (connection lost, refused...) in bold red, so a dropped session stands out from a normal logout.
    const QString detail = reason.isEmpty() || reason == unknown ? QString()
                           : error ? newline + QString::fromLatin1("\x1b[1;31m") + reason + QString::fromLatin1("\x1b[0m")
                                   : newline + reason;
    // A rule as in MobaXterm, in a grey between the scheme background and text: visible on every scheme.
    const QColor grey = blend(s.background, s.foreground, 0.4);
    const QString rule = QString::fromLatin1("\x1b[38;2;%1;%2;%3m").arg(grey.red()).arg(grey.green()).arg(grey.blue()) +
                         QString(std::min(s.cols, 80), QChar(0x2500)) + QString::fromLatin1("\x1b[0m");
    // A session lost inside a full-screen program (vim, top...): back to the normal screen and a soft reset (scroll
    // region, cursor shown, colours), so the notice goes below the shell output instead of over the program's screen.
    const auto notice =
        (QString::fromLatin1("\x1b[?1049l\x1b[!p") + newline + QString::fromLatin1("\x1b[0m") + detail + newline + rule + newline + QString::fromLatin1("\x1b[1m") +
         tr("Session stopped") + QString::fromLatin1("\x1b[0m") + newline + "    - " + tr("Press <Return> to exit tab") +
         newline + "    - " + tr("Press R to restart session") + newline)
            .toUtf8();
    vterm_input_write(s.vt, notice.constData(), size_t(notice.size()));
    vterm_screen_flush_damage(s.screen);
    update();
    if (onEnded)
        onEnded(error && !detail.isEmpty());
}

Terminal::Terminal(const Command &command, QWidget *parent) : QWidget(parent), state(std::make_unique<State>())
{
    setup();
    auto &s = *state;
    // A command with an error is never run, only shown as "Cannot start" (a session whose connection failed).
    const QString program = !command.error.isEmpty()                ? QString()
                            : QDir::isAbsolutePath(command.program) ? command.program
                                                                    : QStandardPaths::findExecutable(command.program);
    bool started = false;
    if (!program.isEmpty())
    {
#ifdef Q_OS_WIN
        HANDLE childInput = nullptr, childOutput = nullptr;
        // ConPTY starts at our cursor (asked with DSR) instead of clearing the screen: the banner stays visible.
        const DWORD inheritCursor = 1; // PSEUDOCONSOLE_INHERIT_CURSOR
        if (CreatePipe(&childInput, &s.input, nullptr, 0) && CreatePipe(&s.output, &childOutput, nullptr, 0) &&
            SUCCEEDED(CreatePseudoConsole({80, 24}, childInput, childOutput, inheritCursor, &s.console)))
        {
            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            QByteArray memory(qsizetype(size), '\0');
            STARTUPINFOEXW info{};
            info.StartupInfo.cb = sizeof(info);
            // Null standard handles: otherwise a child of a CrossTerm whose own output is redirected (a launcher, a
            // script, ctest) writes to those pipes instead of the pseudo console.
            info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            info.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
            if (InitializeProcThreadAttributeList(info.lpAttributeList, 1, 0, &size))
            {
                QString line = quote(QDir::toNativeSeparators(program));
                for (const auto &arg : command.arguments)
                    line += " " + quote(arg);
                PROCESS_INFORMATION child{};
                // Current process environment plus TERM and the command variables (UTF-16 block terminated by a double
                // NUL). Windows sets no TERM: without it ssh announces none and clear, top, vim fail on the server.
                QString environment;
                auto variables = QProcessEnvironment::systemEnvironment();
                variables.insert("TERM", "xterm-256color");
                for (const auto &entry : command.environment)
                    variables.insert(entry.section('=', 0, 0), entry.section('=', 1));
                for (const auto &entry : variables.toStringList())
                    environment += entry + QChar(0);
                environment += QChar(0);
                if (UpdateProcThreadAttribute(info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, s.console,
                                              sizeof(HPCON), nullptr, nullptr) &&
                    CreateProcessW(reinterpret_cast<LPCWSTR>(program.utf16()), reinterpret_cast<LPWSTR>(line.data()), nullptr,
                                   nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                                   environment.data(),
                                   reinterpret_cast<LPCWSTR>(QDir::homePath().utf16()), &info.StartupInfo, &child))
                {
                    s.process = child.hProcess;
                    CloseHandle(child.hThread);
                    started = true;
                }
                DeleteProcThreadAttributeList(info.lpAttributeList);
            }
        }
        if (childInput)
            CloseHandle(childInput);
        if (childOutput)
            CloseHandle(childOutput);
#else
        QList<QByteArray> encoded;
        encoded << QFile::encodeName(program);
        for (const auto &arg : command.arguments)
            encoded << arg.toLocal8Bit();
        QList<char *> argv;
        for (auto &arg : encoded)
            argv << arg.data();
        argv << nullptr;
        // The whole environment is built before fork: between fork and exec only async-signal-safe calls.
        // TERM describes this emulator, not the one CrossTerm was started from (unset when started from a desktop).
        auto variables = QProcessEnvironment::systemEnvironment();
        variables.insert("TERM", "xterm-256color");
        for (const auto &entry : command.environment)
            variables.insert(entry.section('=', 0, 0), entry.section('=', 1));
        QList<QByteArray> environment;
        for (const auto &entry : variables.toStringList())
            environment << entry.toLocal8Bit();
        QList<char *> envp;
        for (auto &entry : environment)
            envp << entry.data();
        envp << nullptr;
        const auto home = QFile::encodeName(QDir::homePath());
        winsize size{24, 80, 0, 0};
        s.pid = forkpty(&s.fd, nullptr, nullptr, &size);
        if (s.pid == 0)
        {
#ifdef Q_OS_LINUX
            prctl(PR_SET_PDEATHSIG, SIGHUP); // the shell dies with CrossTerm, also on a crash
#endif
            chdir(home.constData());
            // Default signals for the session: Qt ignores SIGPIPE in this process once a QProcess is used, and an
            // ignored signal survives exec (`yes | head -1` would then never end in tabs opened later).
            for (int number = 1; number < NSIG; ++number)
                signal(number, SIG_DFL);
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, nullptr);
            execve(argv[0], argv.data(), envp.data());
            _exit(127);
        }
        started = s.pid > 0;
        if (started)
        {
            fcntl(s.fd, F_SETFL, O_NONBLOCK);
            // Not inherited by the shells and ssh of other tabs: closing this tab really hangs up its session.
            fcntl(s.fd, F_SETFD, FD_CLOEXEC);
        }
#endif
    }
    if (!started)
    {
        const auto message = QCoreApplication::translate("CrossTerm", "Cannot start: %1").arg(command.program).toUtf8();
        vterm_input_write(s.vt, message.constData(), size_t(message.size()));
        return;
    }
    auto *timer = new QTimer(this);
#ifdef Q_OS_WIN
    // ConPTY pipes cannot be watched by the event loop: poll them.
    connect(timer, &QTimer::timeout, this,
            [this, timer]
            {
                readPty();
                checkExit();
                if (state->ended)
                    timer->stop();
            });
    timer->start(16);
#else
    // Output is read as soon as it arrives; the timer only notices the exit of a process whose PTY is still held
    // open (by a background job), the common case being caught when the PTY closes.
    s.readable = std::make_unique<QSocketNotifier>(s.fd, QSocketNotifier::Read);
    connect(s.readable.get(), &QSocketNotifier::activated, this,
            [this]
            {
                if (!readPty())
                {
                    state->readable->setEnabled(false); // closed: it would fire again and again
                    checkExit();
                }
            });
    connect(timer, &QTimer::timeout, this,
            [this, timer]
            {
                checkExit();
                if (state->ended)
                    timer->stop();
            });
    timer->start(250);
#endif
}

bool Terminal::readPty()
{
    auto &s = *state;
    char buffer[16384];
    bool open = true, changed = false;
    // Bounded per call, so a flood of output still lets the window repaint and take keys in between.
    for (int i = 0; i < 16; ++i)
    {
#ifdef Q_OS_WIN
        DWORD available = 0, count = 0;
        if (!PeekNamedPipe(s.output, nullptr, 0, nullptr, &available, nullptr))
        {
            open = false;
            break;
        }
        if (!available || !ReadFile(s.output, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr))
            break;
#else
        if (s.fd < 0)
            return false;
        const auto count = ::read(s.fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            open = count < 0 && errno == EAGAIN; // 0 or EIO: the process side of the PTY is closed
            break;
        }
#endif
        feed(QByteArray(buffer, qsizetype(count)));
        changed = true;
    }
    if (changed)
    {
        vterm_screen_flush_damage(s.screen);
        outputChanged();
    }
    releaseHeldKeys(); // the first output of the session usually follows the login
    return open;
}

void Terminal::checkExit()
{
    auto &s = *state;
    if (s.ended)
        return;
    int code = 0; // -1: killed by a signal
#ifdef Q_OS_WIN
    if (!s.process || WaitForSingleObject(s.process, 0) != WAIT_OBJECT_0)
        return;
    DWORD status = 0;
    code = GetExitCodeProcess(s.process, &status) ? int(status) : -1;
#else
    if (s.pid <= 0)
        return;
    releaseHeldKeys();
    int status = 0;
    pid_t result;
    do
        result = waitpid(s.pid, &status, WNOHANG);
    while (result < 0 && errno == EINTR);
    if (result == 0 || (result < 0 && errno != ECHILD))
        return; // still running
    s.pid = -1;
    if (result > 0)
        code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    // What the process wrote just before exiting (ssh: "Connection to ... closed", "Broken pipe") is still in the
    // PTY: show it before "Session stopped".
    readPty();
#ifndef Q_OS_WIN
    if (s.readable)
        s.readable->setEnabled(false);
#endif
    stop(describeExit ? describeExit(code) : QString());
}

Terminal::~Terminal()
{
    if (state->promptDone)
    {
        state->ended = true;
        finishPrompt(std::nullopt);
    }
    // The device must be destroyed before the state: on close it emits signals that would use it.
    state->predictor.reset(); // uses the device
    if (state->device)
    {
        state->device->disconnect(this);
        if (state->mosh)
            state->mosh->onSilence = nullptr;
        delete state->device;
    }
}

void Terminal::hideOutput(const QList<QByteArray> &lines)
{
    state->hiddenOutput = lines;
}

void Terminal::feed(QByteArray data)
{
    if (state->hover.x() >= 0)
        hoverLink({-1, -1}, false); // the rows move: the link is looked up again on the next mouse move
    for (const auto &line : state->hiddenOutput)
    {
        for (qsizetype at = data.indexOf(line); at >= 0; at = data.indexOf(line, at))
        {
            qsizetype end = at + line.size();
            while (end < data.size() && (data[end] == '\r' || data[end] == '\n'))
                ++end;
            data.remove(at, end - at);
        }
    }
    vterm_input_write(state->vt, data.constData(), size_t(data.size()));
}

void Terminal::printLocal(const QString &text)
{
    const auto bytes = text.toUtf8();
    vterm_input_write(state->vt, bytes.constData(), size_t(bytes.size()));
    vterm_screen_flush_damage(state->screen);
    update();
}

void Terminal::prompt(const QString &text, bool echo, std::function<void(std::optional<QString>)> done)
{
    auto &s = *state;
    if (s.promptDone)
        finishPrompt(std::nullopt);
    if (s.ended)
    {
        done(std::nullopt);
        return;
    }
    s.promptDone = std::move(done);
    s.promptEcho = echo;
    s.promptBuffer.clear();
    printLocal(QString(text).replace("\r\n", "\n").replace('\n', "\r\n"));
    // What was typed while ssh was connecting was meant for this prompt when it asks for a secret (a password typed
    // early); before a question shown in clear (host key: yes/no) it is dropped, not taken as the answer.
    if (echo)
        dropHeldKeys();
    else
        replayHeldKeys();
}

void Terminal::holdKeysUntilLogin()
{
#ifndef Q_OS_WIN
    state->holding = state->fd >= 0 && !state->ended;
#endif
}

// ssh leaves its terminal in line mode until it has logged in and the remote session starts, then makes it raw.
bool Terminal::holdingKeys() const
{
#ifndef Q_OS_WIN
    termios mode{};
    return state->holding && state->fd >= 0 && tcgetattr(state->fd, &mode) == 0 && (mode.c_lflag & ICANON);
#else
    return false;
#endif
}

void Terminal::dropHeldKeys()
{
    for (auto &held : state->heldKeys)
        held.text.fill(QChar(0));
    state->heldKeys.clear();
}

// Types the kept keys again, in order: into the prompt in progress, or into the session.
void Terminal::replayHeldKeys()
{
    auto keys = std::move(state->heldKeys);
    state->heldKeys.clear();
    for (auto &held : keys)
    {
        if (held.paste)
            paste(held.text);
        else
        {
            QKeyEvent event(QEvent::KeyPress, held.key, held.modifiers, held.text);
            keyPressEvent(&event);
        }
        held.text.fill(QChar(0));
    }
}

// Logged in: the kept keys go to the session and nothing is kept from now on.
void Terminal::releaseHeldKeys()
{
    if (!state->holding || holdingKeys())
        return;
    state->holding = false;
    replayHeldKeys();
}

bool Terminal::prompting() const
{
    return bool(state->promptDone);
}

void Terminal::finishPrompt(std::optional<QString> answer)
{
    auto &s = *state;
    auto done = std::move(s.promptDone);
    s.promptDone = nullptr;
    s.promptBuffer.fill(QChar(0));
    s.promptBuffer.clear();
    if (!s.ended)
        printLocal(answer ? "\r\n" : "^C\r\n");
    if (done)
        done(std::move(answer));
}

void Terminal::write(const QByteArray &data)
{
    if (state->device)
    {
        if (state->predictor && !state->feeding)
            state->predictor->input(data);
        if (state->device->isOpen())
            state->device->write(data);
        return;
    }
#ifdef Q_OS_WIN
    if (state->input)
    {
        DWORD count;
        WriteFile(state->input, data.constData(), DWORD(data.size()), &count, nullptr);
    }
#else
    if (state->fd >= 0)
    {
        state->pending += data; // a partial write must not lose the rest (long pastes)
        state->flush();
    }
#endif
}

QString Terminal::contents() const
{
    // Up to 6 code points of 4 UTF-8 bytes per cell, plus a line end per row. The function returns the size it
    // needed, which can exceed what it wrote: never read past the buffer.
    QByteArray result(state->cols * state->rows * 24 + state->rows + 1, '\0');
    const auto size =
        vterm_screen_get_text(state->screen, result.data(), size_t(result.size()), {0, state->rows, 0, state->cols});
    return QString::fromUtf8(result.constData(), std::min(qsizetype(size), result.size()));
}

void Terminal::sizeWhenShown()
{
    state->sizeWhenShown = true;
    // A widget gets a placeholder size as soon as it is created (here already applied by the constructor): back to
    // the initial grid until it is shown.
    if (!isVisible())
        setGrid(24, 80);
}

void Terminal::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (std::exchange(state->sizeWhenShown, false))
        resizeEvent(nullptr); // the size it has now that it is laid out and visible
}

void Terminal::resizeEvent(QResizeEvent *)
{
    if (state->sizeWhenShown && !isVisible())
        return; // a placeholder size of a tab not shown yet (showEvent applies the real one)
    const QFontMetrics fm(font());
    setGrid(std::clamp((height() - 8) / std::max(1, fm.height()), 2, 250),
            std::clamp((width() - 8) / std::max(1, fm.horizontalAdvance('M')), 2, 500));
}

// Rows and columns of the screen, for libvterm (with reflow), the session and the pseudo terminal.
void Terminal::setGrid(int rows, int cols)
{
    auto &s = *state;
    if (cols == s.cols && rows == s.rows)
        return;
    s.resize(rows, cols);
    if (s.predictor) // predicted cells do not follow the reflow
        s.predictor->reset();
    s.offset = std::clamp(s.offset, 0, int(s.scrollback.size()));
    s.anchor = s.extent; // the selection does not follow the reflow
    hoverLink({-1, -1}, false);
    if (auto *aware = dynamic_cast<WindowSizeAware *>(s.device))
        aware->setWindowSize(s.cols, s.rows);
#ifdef Q_OS_WIN
    if (s.console)
        ResizePseudoConsole(s.console, {short(s.cols), short(s.rows)});
#else
    if (s.fd >= 0)
    {
        winsize size{static_cast<unsigned short>(s.rows), static_cast<unsigned short>(s.cols), 0, 0};
        ioctl(s.fd, TIOCSWINSZ, &size);
    }
#endif
}

static bool inside(QPoint a, QPoint b, int line, int col)
{
    if (a == b)
        return false;
    if (std::make_pair(a.y(), a.x()) > std::make_pair(b.y(), b.x()))
        std::swap(a, b);
    const auto here = std::make_pair(line, col);
    return here >= std::make_pair(a.y(), a.x()) && here < std::make_pair(b.y(), b.x());
}

void Terminal::paintEvent(QPaintEvent *)
{
    const auto &s = *state;
    QPainter painter(this);
    const QFontMetrics fm(font());
    const int w = fm.horizontalAdvance('M'), h = fm.height();
    const QColor background = s.background, foreground = s.foreground;
    // Font variants (bit 0 bold, bit 1 italic, bit 2 underline) built once per paint; the painter font and pen
    // change only when a cell needs different ones.
    std::array<QFont, 8> faces;
    for (size_t i = 0; i < faces.size(); ++i)
    {
        faces[i] = font();
        faces[i].setBold(i & 1);
        faces[i].setItalic(i & 2);
        faces[i].setUnderline(i & 4);
    }
    size_t face = faces.size();
    QColor pen;
    auto style = [&](size_t index, const QColor &color)
    {
        if (index != face)
            painter.setFont(faces[face = index]);
        if (color != pen)
            painter.setPen(pen = color);
    };
    const bool focused = hasFocus();
    QString text;
    painter.fillRect(rect(), background);
    VTermPos cursor;
    vterm_state_get_cursorpos(vterm_obtain_state(s.vt), &cursor);
    if (s.predictedCursor.x() >= 0)
        cursor = {s.predictedCursor.y(), s.predictedCursor.x()};
    QRect hollow; // cursor of a terminal without focus: outlined once the cells are drawn
    // Underlines and strikethrough are drawn here, not by the font: they join between cells and exist in all styles.
    const int stroke = std::max(1, fm.lineWidth());
    const int underlineY = std::min(fm.ascent() + std::max(1, fm.underlinePos()), h - stroke);
    const int strikeY = std::clamp(fm.ascent() - fm.strikeOutPos(), 0, h - stroke);
    const bool searching = !s.needle.isEmpty(), linked = s.linkFrom != s.linkTo;
    std::vector<int> columns;  // search: column of each character of the row
    std::vector<char> matched; // search: cells of the row inside an occurrence
    for (int row = 0; row < s.rows; ++row)
    {
        const int line = row - s.offset;
        if (searching)
        {
            matched.assign(size_t(s.cols), 0);
            text.truncate(0);
            columns.clear();
            s.rowText(line, text, columns);
            for (auto at = text.indexOf(s.needle, 0, s.needleCase); at >= 0;
                 at = text.indexOf(s.needle, at + s.needle.size(), s.needleCase))
            {
                for (int col = columns[size_t(at)]; col <= columns[size_t(at + s.needle.size() - 1)]; ++col)
                    matched[size_t(col)] = 1;
            }
        }
        for (int col = 0; col < s.cols; ++col)
        {
            VTermScreenCell cell{};
            if (!s.cell(line, col, cell) || cell.chars[0] == UINT32_MAX)
                continue;
            auto color = [&](VTermColor raw, QColor fallback, bool fg)
            {
                if (fg ? VTERM_COLOR_IS_DEFAULT_FG(&raw) : VTERM_COLOR_IS_DEFAULT_BG(&raw))
                    return fallback;
                vterm_screen_convert_color_to_rgb(s.screen, &raw);
                return QColor(raw.rgb.red, raw.rgb.green, raw.rgb.blue);
            };
            auto fg = color(cell.fg, foreground, true), bg = color(cell.bg, background, false);
            if (cell.attrs.reverse)
                std::swap(fg, bg);
            // Faint (SGR 2): the text halfway to its background.
            if (cell.attrs.dim)
                fg = blend(fg, bg, 0.5);
            const int span = std::max(1, int(cell.width)); // a wide character takes the cursor on either half
            const bool atCursor = s.cursor && !s.ended && line == cursor.row && cursor.col >= col && cursor.col < col + span;
            if (atCursor && focused)
            {
                // Block cursor: the character stays readable, in the inverse colors.
                bg = s.cursorColor;
                fg = s.cursorText;
            }
            else if (inside(s.anchor, s.extent, line, col))
            {
                bg = s.selection;
                fg = s.selectionText;
            }
            else if (searching && matched[size_t(col)])
            {
                bg = s.match;
                fg = s.matchText;
            }
            const QRect box(4 + w * col, 4 + h * row, w * span, h);
            if (bg != background)
                painter.fillRect(box, bg);
            const uint32_t first = cell.chars[0];
            const bool visible = first && !cell.attrs.conceal;
            // A plain space draws nothing: no text layout for it.
            if (visible && (first != ' ' || cell.chars[1]))
            {
                if (first >= 0x2500 && first <= 0x259F && !cell.chars[1])
                    drawBlock(painter, box, first, fg, bg);
                else
                {
                    text.truncate(0);
                    appendCell(text, cell);
                    style((cell.attrs.bold ? 1 : 0) | (cell.attrs.italic ? 2 : 0), fg);
                    painter.drawText(box.topLeft() + QPoint(0, fm.ascent()), text);
                }
            }
            const bool link = linked && inside(s.linkFrom, s.linkTo, line, col);
            const int underline = visible ? cell.attrs.underline : 0;
            if (underline == VTERM_UNDERLINE_CURLY)
            {
                // A small wave, three strokes high, continuous from cell to cell.
                const int top = box.top() + std::min(underlineY, h - 3 * stroke);
                for (int x = box.left(); x <= box.right(); x += stroke)
                {
                    const int phase = (x - 4) / stroke % 4;
                    painter.fillRect(x, top + (phase == 3 ? 1 : phase) * stroke, stroke, stroke, fg);
                }
            }
            else if (underline == VTERM_UNDERLINE_DOUBLE)
            {
                const int lower = box.top() + std::min(underlineY + 2 * stroke, h - stroke);
                painter.fillRect(box.left(), lower - 2 * stroke, box.width(), stroke, fg);
                painter.fillRect(box.left(), lower, box.width(), stroke, fg);
            }
            else if (underline || link)
                painter.fillRect(box.left(), box.top() + underlineY, box.width(), stroke, fg);
            if (visible && cell.attrs.strike)
                painter.fillRect(box.left(), box.top() + strikeY, box.width(), stroke, fg);
            for (const auto &p : s.predictions) // local echo over the real cell
            {
                if (p.row != line || p.col != col)
                    continue;
                const QRect predicted(box.topLeft(), QSize(w, h));
                painter.fillRect(predicted, bg);
                style(s.predictionUnderline ? 4 : 0, fg);
                painter.drawText(predicted.topLeft() + QPoint(0, fm.ascent()), QString::fromUcs4(&p.ch, 1));
            }
            if (atCursor && !focused)
                hollow = box;
        }
    }
    if (!hollow.isNull())
    {
        painter.fillRect(hollow.left(), hollow.top(), hollow.width(), 1, s.cursorColor);
        painter.fillRect(hollow.left(), hollow.bottom(), hollow.width(), 1, s.cursorColor);
        painter.fillRect(hollow.left(), hollow.top(), 1, hollow.height(), s.cursorColor);
        painter.fillRect(hollow.right(), hollow.top(), 1, hollow.height(), s.cursorColor);
    }
    int top = 0; // below the banner
    // Mosh: the server has been silent for more than 5 s (as the blue bar of the mosh client).
    if (const qint64 silence = s.mosh && !s.ended ? s.mosh->silence() : -1; silence >= 5000)
    {
        top = 4 + h;
        const QRect bar(0, 0, width(), top);
        painter.fillRect(bar, s.ansi[4]);
        style(1, readable(s.ansi[4], background, foreground));
        painter.drawText(bar.adjusted(0, 4, 0, 0), Qt::AlignCenter,
                         QCoreApplication::translate("CrossTerm", "Last contact %1 s ago").arg(silence / 1000));
    }
    // Scrolled back: number of lines above the live screen, in a badge at the top right.
    if (s.offset > 0)
    {
        text = QString::fromUtf8("\u2191 ") + QString::number(s.offset);
        const QRect badge(width() - fm.horizontalAdvance(text) - 2 * w - 8, top + 6, fm.horizontalAdvance(text) + 2 * w, h + 4);
        QColor fill = foreground;
        fill.setAlpha(200);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(badge, badge.height() / 2.0, badge.height() / 2.0);
        painter.setFont(faces[0]);
        painter.setPen(background);
        painter.drawText(badge, Qt::AlignCenter, text);
    }
}

QPoint Terminal::cellAt(const QPoint &pixel) const
{
    const QFontMetrics fm(font());
    const int w = std::max(1, fm.horizontalAdvance('M'));
    const int col = std::clamp((pixel.x() - 4 + w / 2) / w, 0, state->cols);
    const int row = std::clamp((pixel.y() - 4) / std::max(1, fm.height()), 0, state->rows - 1);
    return {col, row - state->offset};
}

QString Terminal::selectedText() const
{
    auto a = state->anchor, b = state->extent;
    if (a == b)
        return {};
    if (std::make_pair(a.y(), a.x()) > std::make_pair(b.y(), b.x()))
        std::swap(a, b);
    // A row that the next one continues (wrapped at the window width) is one line: no break between them, and its
    // last columns are kept even when they are spaces, or a long command pasted elsewhere would be cut in two.
    QString result;
    for (int line = a.y(); line <= b.y(); ++line)
    {
        QString text;
        const int from = line == a.y() ? a.x() : 0, to = line == b.y() ? b.x() : state->cols;
        for (int col = from; col < to; ++col)
        {
            VTermScreenCell cell{};
            if (!state->cell(line, col, cell) || cell.chars[0] == UINT32_MAX)
                continue;
            if (cell.chars[0])
                appendCell(text, cell);
            else
                text += ' ';
        }
        const bool wraps = line < b.y() && state->continued(line + 1);
        if (!wraps)
            while (text.endsWith(' '))
                text.chop(1);
        result += text;
        if (line < b.y() && !wraps)
            result += '\n';
    }
    return result;
}

void Terminal::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    if (event->button() == Qt::LeftButton)
    {
        // Cmd/Ctrl + click on a link opens it; anywhere else the click starts a selection as usual.
        if (event->modifiers() & Qt::ControlModifier)
        {
            hoverLink(event->position().toPoint(), true);
            if (state->linkFrom != state->linkTo)
            {
                openForUser(state->link); // the browser must not close with CrossTerm
                return;
            }
        }
        state->anchor = state->extent = cellAt(event->position().toPoint());
        state->selecting = true;
        update();
    }
    else if (event->button() == Qt::MiddleButton ||
             (event->button() == Qt::RightButton && QSettings().value("terminal/rightPaste", true).toBool()))
    {
        userPaste(QApplication::clipboard()->text());
    }
}

void Terminal::mouseMoveEvent(QMouseEvent *event)
{
    if (!state->selecting)
    {
        hoverLink(event->position().toPoint(), event->modifiers() & Qt::ControlModifier);
        return;
    }
    state->extent = cellAt(event->position().toPoint());
    update();
}

void Terminal::hoverLink(const QPoint &pixel, bool active)
{
    auto &s = *state;
    QPoint from, to;
    QUrl url;
    if (active && !s.selecting && rect().contains(pixel))
    {
        const int w = std::max(1, QFontMetrics(font()).horizontalAdvance('M'));
        const QPoint at(std::clamp((pixel.x() - 4) / w, 0, s.cols - 1), cellAt(pixel).y());
        if (at == s.hover)
            return; // same cell: same answer
        s.hover = at;
        // The logical line: the row and the rows it wraps from and onto (a long URL), at most 20 each way. A URL ends
        // at a space, a quote or the border of a text-mode window.
        int first = at.y(), last = at.y();
        while (s.continued(first) && at.y() - first < 20)
            --first;
        while (s.continued(last + 1) && last - at.y() < 20)
            ++last;
        QString text;
        std::vector<int> columns, starts; // column of each character, first character of each row
        for (int line = first; line <= last; ++line)
        {
            starts.push_back(int(text.size()));
            s.rowText(line, text, columns);
        }
        auto row = [&](qsizetype index)
        { return int(std::upper_bound(starts.begin(), starts.end(), int(index)) - starts.begin()) - 1; };
        const int begin = starts[size_t(at.y() - first)];
        const auto end = at.y() < last ? columns.begin() + starts[size_t(at.y() - first + 1)] : columns.end();
        const auto here = std::lower_bound(columns.begin() + begin, end, at.x()) - columns.begin();
        static const QRegularExpression pattern(QStringLiteral("\\b(?:https?|ftp)://[^\\s<>\"'`\\x{2500}-\\x{259F}]+"),
                                                QRegularExpression::CaseInsensitiveOption);
        for (auto found = pattern.globalMatch(text); found.hasNext();)
        {
            const auto match = found.next();
            const auto start = match.capturedStart();
            auto stop = match.capturedEnd();
            // Punctuation after the URL is not part of it; a closing bracket only when the URL has no opening one.
            for (; stop > start; --stop)
            {
                const QChar c = text[stop - 1];
                const auto part = QStringView(text).mid(start, stop - start);
                const bool unpaired = (c == ')' && part.count('(') < part.count(')')) ||
                                      (c == ']' && part.count('[') < part.count(']')) ||
                                      (c == '}' && part.count('{') < part.count('}'));
                if (!unpaired && !QStringView(u".,;:!?").contains(c))
                    break;
            }
            if (here < start || here >= stop)
                continue;
            // Only web and FTP addresses are ever opened: never a local file or another scheme.
            const QUrl candidate(text.mid(start, stop - start));
            static const QStringList schemes{"http", "https", "ftp"};
            if (candidate.isValid() && schemes.contains(candidate.scheme()) && !candidate.host().isEmpty())
            {
                url = candidate;
                from = {columns[size_t(start)], first + row(start)};
                to = {columns[size_t(stop - 1)] + 1, first + row(stop - 1)};
            }
            break;
        }
    }
    else
        s.hover = {-1, 0};
    if (from == s.linkFrom && to == s.linkTo && url == s.link)
        return;
    s.linkFrom = from;
    s.linkTo = to;
    s.link = url;
    if (from != to)
        setCursor(Qt::PointingHandCursor);
    else
        unsetCursor();
    update();
}

void Terminal::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !state->selecting)
        return;
    state->selecting = false;
    const auto text = selectedText(); // copy on select
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

void Terminal::mouseDoubleClickEvent(QMouseEvent *event)
{
    const auto at = cellAt(event->position().toPoint());
    auto word = [&](int col)
    {
        VTermScreenCell cell{};
        // The second half of a wide character (UINT32_MAX) belongs to the word too.
        return state->cell(at.y(), col, cell) && cell.chars[0] && cell.chars[0] != ' ';
    };
    int from = at.x(), to = at.x();
    while (from > 0 && word(from - 1))
        --from;
    while (to < state->cols && word(to))
        ++to;
    state->anchor = {from, at.y()};
    state->extent = {to, at.y()};
    const auto text = selectedText();
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
    update();
}

void Terminal::setFontPointSize(int points)
{
    QFont face = font();
    face.setPointSize(std::clamp(points, 6, 48));
    setFont(face);
}

void Terminal::zoom(int steps)
{
    QFont face = font();
    face.setPointSize(steps ? std::clamp(face.pointSize() + steps, 6, 48) : QSettings().value("terminal/size", 10).toInt());
    setFont(face);
    if (onZoom)
        onZoom(face.pointSize());
}

void Terminal::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier)
    {
        // Ctrl+wheel (Cmd+wheel on macOS): font zoom for this terminal (one point per notch; touchpads accumulate).
        auto &pending = state->zoomDelta;
        pending += event->angleDelta().y();
        const int steps = pending / 120;
        pending %= 120;
        if (steps)
            zoom(steps);
        event->accept();
        return;
    }
    // Three lines per notch; the small deltas of touchpads accumulate instead of being lost.
    auto &pending = state->wheelDelta;
    pending += event->angleDelta().y();
    const int lines = pending / 40;
    pending %= 40;
    // Full-screen programs (nano, vim, less, htop: the alternate screen, which has no scrollback): the wheel sends
    // arrow keys instead, as xterm's alternate scroll does. vterm_keyboard_key follows the cursor key mode.
    if (state->alternate)
    {
        for (int i = 0; i < std::abs(lines); ++i)
            vterm_keyboard_key(state->vt, lines > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
        event->accept();
        return;
    }
    state->offset = std::clamp(state->offset + lines, 0, int(state->scrollback.size()));
    hoverLink({-1, -1}, false); // another row is under the mouse now
    update();
}

void Terminal::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::FontChange)
    {
        resizeEvent(nullptr);
        update();
    }
    QWidget::changeEvent(event);
}

void Terminal::setPredictions(const QList<PredictedCell> &cells, QPoint cursor)
{
    auto &s = *state;
    if (cells == s.predictions && cursor == s.predictedCursor)
        return;
    s.predictions = cells;
    s.predictedCursor = cursor;
    update();
}

void Terminal::clearPredictions()
{
    setPredictions({});
}

QList<PredictedCell> Terminal::predictions() const
{
    return state->predictions;
}

QPoint Terminal::cursorPosition() const
{
    VTermPos cursor;
    vterm_state_get_cursorpos(vterm_obtain_state(state->vt), &cursor);
    return {cursor.col, cursor.row};
}

char32_t Terminal::cellChar(int row, int col) const
{
    VTermScreenCell cell{};
    if (row < 0 || col < 0 || row >= state->rows || col >= state->cols || !state->cell(row, col, cell))
        return 0;
    return cell.chars[0] && cell.chars[0] != UINT32_MAX ? char32_t(cell.chars[0]) : U' ';
}

bool Terminal::finished() const
{
    return state->ended;
}

void Terminal::userPaste(const QString &text)
{
    // Not a password prompt answer, nor text kept for one.
    if (onUserPaste && !text.isEmpty() && !state->ended && !state->promptDone && !holdingKeys())
        onUserPaste(text);
    else
        paste(text);
}

void Terminal::paste(const QString &text)
{
    if (text.isEmpty() || state->ended)
        return;
    if (state->promptDone)
    {
        const auto line = text.section('\n', 0, 0).remove('\r');
        state->promptBuffer += line;
        if (state->promptEcho)
            printLocal(line);
        return;
    }
    releaseHeldKeys();
    if (state->holding)
    {
        state->heldKeys.append({0, Qt::NoModifier, text, true});
        return;
    }
    // Line ends are sent as Return, like every terminal: a raw LF after the CR of a Windows line end would answer
    // the next prompt ([Y/n]) of the command just started, and raw-mode programs do not take LF as Enter.
    QString clean = text;
    clean.replace(QStringLiteral("\r\n"), QStringLiteral("\r")).replace(QChar('\n'), QChar('\r'));
    // An end-of-paste marker inside the text must not end the bracketed paste early and run the rest as typed
    // input. Removed until none is left: one pass can splice a new marker together from the pieces around it.
    const QString marker = QStringLiteral("\x1b[201~");
    while (clean.contains(marker))
        clean.remove(marker);
    // One write for the whole text (as UTF-8, like vterm_keyboard_unichar without modifiers) instead of one per
    // character.
    vterm_keyboard_start_paste(state->vt);
    write(clean.toUtf8());
    vterm_keyboard_end_paste(state->vt);
}

bool Terminal::event(QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
        auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)))
        {
            keyPressEvent(key);
            key->accept();
            return true;
        }
    }
    // Links follow the Cmd/Ctrl key (Qt::Key_Control is Command on macOS) even when the mouse does not move.
    if ((event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) &&
        static_cast<QKeyEvent *>(event)->key() == Qt::Key_Control)
        hoverLink(mapFromGlobal(QCursor::pos()), event->type() == QEvent::KeyPress);
    else if (event->type() == QEvent::Leave || event->type() == QEvent::WindowDeactivate)
        hoverLink({-1, -1}, false);
    return QWidget::event(event);
}

bool Terminal::isPasteKey(const QKeyEvent *event)
{
    const auto held = event->modifiers() & ~Qt::KeypadModifier;
#ifdef Q_OS_MACOS
    return held == Qt::ControlModifier && event->key() == Qt::Key_V;
#else
    return (held == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_V) ||
           (held == Qt::ShiftModifier && event->key() == Qt::Key_Insert);
#endif
}

void Terminal::inputMethodEvent(QInputMethodEvent *event)
{
    // Typed like the keys that produce it (prompt, keys held during login, session), as one text key.
    if (const auto text = event->commitString(); !text.isEmpty())
    {
        QKeyEvent key(QEvent::KeyPress, 0, Qt::NoModifier, text);
        keyPressEvent(&key);
    }
    event->accept();
}

QVariant Terminal::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled)
        return true;
    if (query == Qt::ImCursorRectangle) // where the input method's candidate window goes: at the cursor
    {
        const QFontMetrics fm(font());
        const auto cursor = cursorPosition();
        return QRect(4 + cursor.x() * fm.horizontalAdvance('M'), 4 + (cursor.y() + state->offset) * fm.height(),
                     fm.horizontalAdvance('M'), fm.height());
    }
    return QWidget::inputMethodQuery(query);
}

void Terminal::keyPressEvent(QKeyEvent *event)
{
    // Copy and paste: Cmd+C / Cmd+V on macOS; Ctrl+Shift+C / Ctrl+Shift+V, Ctrl+Insert / Shift+Insert elsewhere
    // (plain Ctrl+C stays the interrupt).
    const auto held = event->modifiers() & ~Qt::KeypadModifier;
#ifdef Q_OS_MACOS
    const bool copy = held == Qt::ControlModifier && event->key() == Qt::Key_C;
#else
    const bool copy = (held == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_C) ||
                      (held == Qt::ControlModifier && event->key() == Qt::Key_Insert);
#endif
    const bool pasteKey = isPasteKey(event);
    if (copy)
    {
        const auto text = selectedText();
        QApplication::clipboard()->setText(text.isEmpty() ? contents() : text);
        return;
    }
    if (pasteKey)
    {
        userPaste(QApplication::clipboard()->text());
        return;
    }
    if (state->ended)
    {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && onClose)
            onClose();
        else if (event->key() == Qt::Key_R && onRestart)
            onRestart();
        return;
    }
    if (state->promptDone)
    {
        auto &s = *state;
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            finishPrompt(s.promptBuffer);
        else if (event->key() == Qt::Key_Escape ||
                 ((event->modifiers() & ControlKey) && (event->key() == Qt::Key_C || event->key() == Qt::Key_D)))
            finishPrompt(std::nullopt);
        else if (event->key() == Qt::Key_Backspace)
        {
            if (!s.promptBuffer.isEmpty())
            {
                s.promptBuffer.chop(1);
                if (s.promptEcho)
                    printLocal("\b \b");
            }
        }
        else if (!event->text().isEmpty() && event->text().at(0).isPrint())
        {
            s.promptBuffer += event->text();
            if (s.promptEcho)
                printLocal(event->text());
        }
        return;
    }
    // ssh still connecting: the key is kept (see holdKeysUntilLogin). Ctrl+C and Ctrl+D pass, to give up.
    releaseHeldKeys();
    if (state->holding && !((event->modifiers() & ControlKey) && (event->key() == Qt::Key_C || event->key() == Qt::Key_D)))
    {
        if (!event->text().isEmpty() || event->key() < Qt::Key_Shift || event->key() > Qt::Key_Alt)
            state->heldKeys.append({event->key(), event->modifiers(), event->text(), false});
        return;
    }
    if (event->modifiers() == Qt::ShiftModifier && (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown))
    {
        const int step = event->key() == Qt::Key_PageUp ? state->rows - 1 : 1 - state->rows;
        state->offset = std::clamp(state->offset + step, 0, int(state->scrollback.size()));
        update();
        return;
    }
    if (!event->text().isEmpty() || event->key() < Qt::Key_Shift || event->key() > Qt::Key_Alt)
    {
        state->offset = 0;
        state->anchor = state->extent;
        state->needle.clear(); // typing in the session ends the search
        update();
    }
#ifdef Q_OS_MACOS
    // macOS line editing, as in Terminal.app: Option+arrows move by word, Cmd+arrows to the line ends,
    // Cmd+Backspace deletes the line, Option+Backspace the previous word. Other Command keys belong to the app.
    if (held & Qt::ControlModifier)
    {
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)
            vterm_keyboard_unichar(state->vt, event->key() == Qt::Key_Left ? 'a' : 'e', VTERM_MOD_CTRL);
        else if (event->key() == Qt::Key_Backspace)
            vterm_keyboard_unichar(state->vt, 'u', VTERM_MOD_CTRL);
        return;
    }
    if ((held & Qt::AltModifier) && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right))
    {
        vterm_keyboard_unichar(state->vt, event->key() == Qt::Key_Left ? 'b' : 'f', VTERM_MOD_ALT);
        return;
    }
    // Option composes characters (@ [ ] { } | ~ on many layouts, å ∫ on the US one): they are typed as they are, not
    // as Alt+character. Keys without printable text (Option+Backspace...) keep Alt below.
    if ((held & Qt::AltModifier) && !(held & Qt::MetaModifier) && !event->text().isEmpty() && event->text().at(0).isPrint())
    {
        write(event->text().toUtf8());
        return;
    }
#endif
    int mods = 0;
    if (event->modifiers() & ControlKey)
        mods |= VTERM_MOD_CTRL;
    if ((event->modifiers() & Qt::ShiftModifier) || event->key() == Qt::Key_Backtab)
        mods |= VTERM_MOD_SHIFT;
    if (event->modifiers() & Qt::AltModifier)
        mods |= VTERM_MOD_ALT;
    static const QHash<int, VTermKey> keys{
        {Qt::Key_Return, VTERM_KEY_ENTER},     {Qt::Key_Enter, VTERM_KEY_ENTER}, {Qt::Key_Backspace, VTERM_KEY_BACKSPACE},
        {Qt::Key_Tab, VTERM_KEY_TAB},          {Qt::Key_Backtab, VTERM_KEY_TAB}, {Qt::Key_Escape, VTERM_KEY_ESCAPE},
        {Qt::Key_Left, VTERM_KEY_LEFT},        {Qt::Key_Right, VTERM_KEY_RIGHT}, {Qt::Key_Up, VTERM_KEY_UP},
        {Qt::Key_Down, VTERM_KEY_DOWN},        {Qt::Key_Home, VTERM_KEY_HOME},   {Qt::Key_End, VTERM_KEY_END},
        {Qt::Key_Delete, VTERM_KEY_DEL},       {Qt::Key_Insert, VTERM_KEY_INS},  {Qt::Key_PageUp, VTERM_KEY_PAGEUP},
        {Qt::Key_PageDown, VTERM_KEY_PAGEDOWN}};
    // Control characters go out as the plain byte (ESC first with Alt), as in every terminal. libvterm would encode
    // many of these keys as CSI-u sequences ("ESC[32;2u" for Shift+Space, "ESC[13;2u" for Shift+Return, Ctrl+[,
    // Ctrl+I/J/M, Ctrl+Space...), which shells that do not know them run as text: "2u" became a command.
    const int k = event->key();
    const QByteArray escape = (mods & VTERM_MOD_ALT) ? QByteArray("\x1b") : QByteArray();
#ifdef Q_OS_WIN
    // AltGr arrives as Ctrl+Alt with the composed character (@ [ ] { } on many layouts): typed as it is.
    if ((mods & VTERM_MOD_CTRL) && (mods & VTERM_MOD_ALT) && !event->text().isEmpty() && event->text().at(0).isPrint())
    {
        write(event->text().toUtf8());
        return;
    }
#endif
    if (mods & VTERM_MOD_CTRL)
    {
        int control = -1;
        if (k >= Qt::Key_A && k <= Qt::Key_Z)
            control = k - Qt::Key_A + 1;
        else if (k == Qt::Key_Space || k == Qt::Key_At || k == Qt::Key_2)
            control = 0;
        else if (k == Qt::Key_BracketLeft || k == Qt::Key_3)
            control = 0x1b;
        else if (k == Qt::Key_Backslash || k == Qt::Key_4)
            control = 0x1c;
        else if (k == Qt::Key_BracketRight || k == Qt::Key_5)
            control = 0x1d;
        else if (k == Qt::Key_AsciiCircum || k == Qt::Key_6)
            control = 0x1e;
        else if (k == Qt::Key_Underscore || k == Qt::Key_Slash || k == Qt::Key_7)
            control = 0x1f;
        else if (k == Qt::Key_Backspace)
            control = 0x08;
        if (control >= 0)
        {
            write(escape + char(control));
            return;
        }
    }
    if (k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Backspace || k == Qt::Key_Escape)
        mods &= VTERM_MOD_ALT; // Shift and Ctrl never change what these keys send
    if (keys.contains(event->key()))
        vterm_keyboard_key(state->vt, keys.value(event->key()), VTermModifier(mods));
    else if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F12)
        vterm_keyboard_key(state->vt, VTermKey(VTERM_KEY_FUNCTION(event->key() - Qt::Key_F1 + 1)), VTermModifier(mods));
    else
    {
        // Only Alt is left to libvterm for text (ESC plus the character): Shift is already in the character, and a
        // Ctrl combination without a control character above types the character itself.
        const auto alt = VTermModifier(mods & VTERM_MOD_ALT);
        for (char32_t c : event->text().toUcs4())
        {
            if (c == U'\n')
                c = U'\r'; // text steps of macros: a line end is Return
            if (c < 0x20 || c == 0x7f)
                write(escape + char(c));
            // libvterm sends Alt+character as ESC plus one byte, wrong beyond ASCII: ESC plus UTF-8 here.
            else if (alt && c > 0x7f)
                write("\x1b" + QString::fromUcs4(&c, 1).toUtf8());
            else
                vterm_keyboard_unichar(state->vt, c, alt);
        }
    }
}

QString Terminal::selection() const
{
    return selectedText();
}

void Terminal::clearFind()
{
    if (state->needle.isEmpty())
        return;
    state->needle.clear();
    update();
}

bool Terminal::find(const QString &text, bool backward, Qt::CaseSensitivity cs)
{
    auto &s = *state;
    // Every occurrence on screen is highlighted while painting; the one found here is selected.
    s.needle = text;
    s.needleCase = cs;
    update();
    if (text.isEmpty())
        return false;
    QString content;
    std::vector<int> columns; // column of each UTF-16 unit of the line
    // First text position at or after a column.
    auto position = [&](int col) { return int(std::lower_bound(columns.begin(), columns.end(), col) - columns.begin()); };
    const int first = -int(s.scrollback.size()), last = s.rows - 1;
    // Starting point: after (or before) the current selection, otherwise the bottom/top of the buffer.
    const bool selected = s.anchor != s.extent;
    int line = selected ? s.anchor.y() : (backward ? last : first);
    int column = selected ? s.anchor.x() + (backward ? -1 : 1) : (backward ? s.cols : 0);
    for (int step = 0; step <= last - first + 1; ++step)
    {
        content.truncate(0);
        columns.clear();
        s.rowText(line, content, columns);
        const int at = backward ? (column < 0 ? -1 : int(content.lastIndexOf(text, position(column + 1) - 1, cs)))
                                : int(content.indexOf(text, position(std::max(0, column)), cs));
        if (at >= 0)
        {
            s.anchor = {columns[size_t(at)], line};
            s.extent = {columns[size_t(at + text.size() - 1)] + 1, line};
            // Scroll so the matched line is visible.
            if (line < -s.offset || line >= s.rows - s.offset)
                s.offset = std::clamp(s.rows / 2 - line, 0, int(s.scrollback.size()));
            update();
            return true;
        }
        line += backward ? -1 : 1;
        if (line < first)
            line = last;
        if (line > last)
            line = first;
        column = backward ? s.cols : 0;
    }
    return false;
}

QString Terminal::allText() const
{
    QStringList lines;
    for (int line = -int(state->scrollback.size()); line < state->rows; ++line)
        lines << state->lineText(line);
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines.join('\n');
}

bool Terminal::setLogFile(const QString &path, bool timestamps)
{
    state->logTimes = timestamps;
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;
    file->setPermissions(QFile::ReadOwner | QFile::WriteOwner); // session output: for the owner only
    state->log = std::move(file);
    return true;
}

QString Terminal::workingDirectory() const
{
    if (state->elsewhere)
        return {};
    if (!state->directory.isEmpty())
        return state->directory;
    // Without OSC 7: a "user@host:path$" or "user@host:path#" style prompt on the cursor line.
    VTermPos cursor;
    vterm_state_get_cursorpos(vterm_obtain_state(state->vt), &cursor);
    static const QRegularExpression prompt("[\\w.-]+@([\\w.-]+):\\s*([~/][^\\s]*)\\s*[#$]\\s*$");
    const auto match = prompt.match(state->lineText(cursor.row).left(cursor.col + 1));
    if (!match.hasMatch())
        return {};
    // The first prompt's host is the session's: the prompt of another host (ssh typed inside the session) shows
    // folders of that machine, not of the server the SFTP browser is connected to.
    if (state->promptHost.isEmpty())
        state->promptHost = match.captured(1);
    return match.captured(1) == state->promptHost ? match.captured(2) : QString();
}

const QList<ColorScheme> &Terminal::schemes()
{
    // Well-known public palettes (xterm/VS Code for the defaults, Solarized by Ethan Schoonover).
    static const std::array<QColor, 16> standard{QColor("#000000"), QColor("#cd3131"), QColor("#0dbc79"), QColor("#e5e510"),
                                                 QColor("#2472c8"), QColor("#bc3fbc"), QColor("#11a8cd"), QColor("#e5e5e5"),
                                                 QColor("#666666"), QColor("#f14c4c"), QColor("#23d18b"), QColor("#f5f543"),
                                                 QColor("#3b8eea"), QColor("#d670d6"), QColor("#29b8db"), QColor("#ffffff")};
    static const std::array<QColor, 16> light{QColor("#000000"), QColor("#cd3131"), QColor("#00bc00"), QColor("#949800"),
                                              QColor("#0451a5"), QColor("#bc05bc"), QColor("#0598bc"), QColor("#555555"),
                                              QColor("#666666"), QColor("#cd3131"), QColor("#14ce14"), QColor("#b5ba00"),
                                              QColor("#0451a5"), QColor("#bc05bc"), QColor("#0598bc"), QColor("#a5a5a5")};
    static const std::array<QColor, 16> solarized{QColor("#073642"), QColor("#dc322f"), QColor("#859900"), QColor("#b58900"),
                                                  QColor("#268bd2"), QColor("#d33682"), QColor("#2aa198"), QColor("#eee8d5"),
                                                  QColor("#002b36"), QColor("#cb4b16"), QColor("#586e75"), QColor("#657b83"),
                                                  QColor("#839496"), QColor("#6c71c4"), QColor("#93a1a1"), QColor("#fdf6e3")};
    static const QList<ColorScheme> list{
        {"CrossTerm Dark", QColor("#1c1c1c"), QColor("#dadada"), QColor("#c8c8c8"), standard},
        {"CrossTerm Light", QColor("#ffffff"), QColor("#1e1e1e"), QColor("#404040"), light},
        {"Solarized Dark", QColor("#002b36"), QColor("#839496"), QColor("#93a1a1"), solarized},
        {"Solarized Light", QColor("#fdf6e3"), QColor("#657b83"), QColor("#586e75"), solarized},
    };
    return list;
}

void Terminal::applyScheme()
{
    const auto name = QSettings().value("terminal/scheme", "CrossTerm Dark").toString();
    auto scheme = schemes().first();
    for (const auto &candidate : schemes())
    {
        if (name == candidate.name)
            scheme = candidate;
    }
    auto &s = *state;
    s.background = scheme.background;
    s.foreground = scheme.foreground;
    s.cursorColor = scheme.cursor;
    s.ansi = scheme.ansi;
    // Selection from the scheme blue, other search matches from its yellow, softer on a light background; their text
    // and the character under the cursor in whichever color reads best.
    const bool dark = luminance(s.background) < 0.18;
    s.selection = blend(s.background, s.ansi[4], dark ? 0.7 : 0.35);
    s.selectionText = readable(s.selection, s.foreground, s.background);
    s.match = blend(s.background, s.ansi[3], dark ? 0.4 : 0.35);
    s.matchText = readable(s.match, s.foreground, s.background);
    s.cursorText = readable(s.cursorColor, s.background, s.foreground);
    auto *vtState = vterm_obtain_state(s.vt);
    for (int i = 0; i < 16; ++i)
    {
        VTermColor color;
        vterm_color_rgb(&color, uint8_t(scheme.ansi[size_t(i)].red()), uint8_t(scheme.ansi[size_t(i)].green()),
                        uint8_t(scheme.ansi[size_t(i)].blue()));
        vterm_state_set_palette_color(vtState, i, &color);
    }
    update();
}

QColor Terminal::background() const
{
    return state->background;
}
