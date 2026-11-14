#include "terminal.h"
#include "mosh.h"
#include "transports.h"
#include <QAbstractSocket>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QRegularExpression>
#include <QUrl>
#include <deque>
#include <vector>
#include <QPainter>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <vterm.h>
#include <algorithm>

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
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
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
    std::unique_ptr<MoshPredictor> predictor;
    QPoint anchor, extent;
    std::unique_ptr<QFile> log; // session log: lines scrolled off the screen + final screen
    QColor background{"#1c1c1c"}, foreground{"#dadada"}, cursorColor{"#c8c8c8"};
    QByteArray osc;    // incoming OSC 7 (file://host/path)
    QString directory; // last directory announced by the shell via OSC 7
    QString lineText(int line) const
    {
        QString result;
        for (int col = 0; col < cols; ++col)
        {
            VTermScreenCell c{};
            if (!cell(line, col, c) || c.chars[0] == UINT32_MAX)
                continue;
            const char32_t point = c.chars[0] ? char32_t(c.chars[0]) : U' ';
            result += QString::fromUcs4(&point, 1);
        }
        while (result.endsWith(' '))
            result.chop(1);
        return result;
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
            log->write((lineText(-1) + "\n").toUtf8());
        if (scrollback.size() > 5000)
            scrollback.pop_front();
        else if (offset)
            ++offset;
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
            if (rows + head <= 1000)              // ponytail: huge logical lines stay split at the boundary
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
#endif
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
        if (fd >= 0)
            ::close(fd); // global close(): QWidget::close would shadow it
        if (pid > 0)
        {
            kill(pid, SIGHUP);
            waitpid(pid, nullptr, WNOHANG);
        }
#endif
        if (log)
        {
            int last = rows - 1;
            while (last >= 0 && lineText(last).isEmpty())
                --last;
            for (int row = std::min(logged, last + 1); row <= last; ++row)
                log->write((lineText(row) + "\n").toUtf8());
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
        cb.movecursor = [](VTermPos, VTermPos, int visible, void *context)
        {
            static_cast<State *>(context)->cursor = visible;
            return 1;
        };
        cb.sb_pushline4 = [](int cols, const VTermScreenCell *cells, bool continuation, void *context)
        {
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
            s.osc.append(frag.str, qsizetype(frag.len));
            if (frag.final)
            {
                const QUrl url(QString::fromUtf8(s.osc));
                if (url.scheme() == "file" && !url.path().isEmpty())
                    s.directory = url.path();
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
    }
    connect(device, &QIODevice::readyRead, this, [this] { readDevice(); });
    for (const auto signal : {&QIODevice::readChannelFinished, &QIODevice::aboutToClose})
        connect(device, signal, this, [this] { stop(state->device->errorString()); });
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
    update();
}

void Terminal::stop(const QString &reason)
{
    auto &s = *state;
    if (s.ended)
        return;
    if (s.device && s.device->isOpen())
        readDevice(); // last data still in the buffer
    s.ended = true;
    if (s.promptDone)
        finishPrompt(std::nullopt);
    s.predictor.reset();
    clearPredictions();
    auto tr = [](const char *text) { return QCoreApplication::translate("CrossTerm", text); };
    const QString newline = QString::fromLatin1("\r\n");
    // Without an error set, QIODevice reports "Unknown error": don't show it.
    static const QString unknown = QBuffer().errorString();
    const QString detail = reason.isEmpty() || reason == unknown ? QString() : newline + reason;
    const auto notice =
        (newline + QString::fromLatin1("\x1b[0m") + detail + newline + tr("Session stopped") + newline + "    - " +
         tr("Press <Return> to exit tab") + newline + "    - " + tr("Press R to restart session") + newline)
            .toUtf8();
    vterm_input_write(s.vt, notice.constData(), size_t(notice.size()));
    vterm_screen_flush_damage(s.screen);
    update();
}

Terminal::Terminal(const Command &command, QWidget *parent) : QWidget(parent), state(std::make_unique<State>())
{
    setup();
    auto &s = *state;
    const QString program =
        QDir::isAbsolutePath(command.program) ? command.program : QStandardPaths::findExecutable(command.program);
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
            info.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
            if (InitializeProcThreadAttributeList(info.lpAttributeList, 1, 0, &size))
            {
                QString line = quote(QDir::toNativeSeparators(program));
                for (const auto &arg : command.arguments)
                    line += " " + quote(arg);
                PROCESS_INFORMATION child{};
                // Current process environment plus the command variables (UTF-16 block terminated by a double NUL).
                QString environment;
                if (!command.environment.isEmpty())
                {
                    auto variables = QProcessEnvironment::systemEnvironment();
                    for (const auto &entry : command.environment)
                        variables.insert(entry.section('=', 0, 0), entry.section('=', 1));
                    for (const auto &entry : variables.toStringList())
                        environment += entry + QChar(0);
                    environment += QChar(0);
                }
                if (UpdateProcThreadAttribute(info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, s.console,
                                              sizeof(HPCON), nullptr, nullptr) &&
                    CreateProcessW(reinterpret_cast<LPCWSTR>(program.utf16()), reinterpret_cast<LPWSTR>(line.data()), nullptr,
                                   nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                                   command.environment.isEmpty() ? nullptr : environment.data(),
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
        const auto home = QFile::encodeName(QDir::homePath());
        winsize size{24, 80, 0, 0};
        s.pid = forkpty(&s.fd, nullptr, nullptr, &size);
        if (s.pid == 0)
        {
#ifdef Q_OS_LINUX
            prctl(PR_SET_PDEATHSIG, SIGHUP); // the shell dies with CrossTerm, also on a crash
#endif
            chdir(home.constData());
            for (const auto &entry : command.environment)
                setenv(entry.section('=', 0, 0).toLocal8Bit().constData(), entry.section('=', 1).toLocal8Bit().constData(), 1);
            execv(argv[0], argv.data());
            _exit(127);
        }
        started = s.pid > 0;
        if (started)
            fcntl(s.fd, F_SETFL, O_NONBLOCK);
#endif
    }
    if (!started)
    {
        const auto message = QCoreApplication::translate("CrossTerm", "Cannot start: %1").arg(command.program).toUtf8();
        vterm_input_write(s.vt, message.constData(), size_t(message.size()));
        return;
    }
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this,
            [this, timer]
            {
                auto &s = *state;
                char buffer[8192];
                bool changed = false, ended = false;
                for (int i = 0; i < 16; ++i)
                {
#ifdef Q_OS_WIN
                    DWORD available = 0, count = 0;
                    if (!PeekNamedPipe(s.output, nullptr, 0, nullptr, &available, nullptr))
                        break;
                    if (!available || !ReadFile(s.output, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr))
                        break;
#else
            const auto count = read(s.fd, buffer, sizeof(buffer));
            if (count <= 0) break;
#endif
                    feed(QByteArray(buffer, qsizetype(count)));
                    changed = true;
                }
#ifdef Q_OS_WIN
                ended = WaitForSingleObject(s.process, 0) == WAIT_OBJECT_0;
#else
        if (waitpid(s.pid, nullptr, WNOHANG) == s.pid) { s.pid = -1; ended = true; }
#endif
                if (ended)
                {
                    timer->stop();
                    stop();
                }
                if (changed)
                {
                    vterm_screen_flush_damage(s.screen);
                    update();
                }
            });
    timer->start(16);
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
        delete state->device;
    }
}

void Terminal::hideOutput(const QList<QByteArray> &lines)
{
    state->hiddenOutput = lines;
}

void Terminal::feed(QByteArray data)
{
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
        ::write(state->fd, data.constData(), size_t(data.size()));
#endif
}

QString Terminal::contents() const
{
    QByteArray result(state->cols * state->rows * 24 + 1, '\0');
    auto size = vterm_screen_get_text(state->screen, result.data(), size_t(result.size()), {0, state->rows, 0, state->cols});
    return QString::fromUtf8(result.constData(), qsizetype(size));
}

void Terminal::resizeEvent(QResizeEvent *)
{
    const QFontMetrics fm(font());
    auto &s = *state;
    const int cols = std::clamp((width() - 8) / std::max(1, fm.horizontalAdvance('M')), 2, 500);
    const int rows = std::clamp((height() - 8) / std::max(1, fm.height()), 2, 250);
    if (cols == s.cols && rows == s.rows)
        return;
    s.resize(rows, cols);
    if (s.predictor) // predicted cells do not follow the reflow
        s.predictor->reset();
    s.offset = std::clamp(s.offset, 0, int(s.scrollback.size()));
    s.anchor = s.extent; // the selection does not follow the reflow
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
    painter.fillRect(rect(), background);
    VTermPos cursor;
    vterm_state_get_cursorpos(vterm_obtain_state(s.vt), &cursor);
    if (s.predictedCursor.x() >= 0)
        cursor = {s.predictedCursor.y(), s.predictedCursor.x()};
    for (int row = 0; row < s.rows; ++row)
    {
        const int line = row - s.offset;
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
            if (inside(s.anchor, s.extent, line, col))
            {
                bg = QColor("#3a6ea8");
                fg = Qt::white;
            }
            const QRect box(4 + w * col, 4 + h * row, w * std::max(1, int(cell.width)), h);
            if (bg != background)
                painter.fillRect(box, bg);
            if (cell.chars[0] && !cell.attrs.conceal)
            {
                QString text;
                for (uint32_t c : cell.chars)
                {
                    if (!c)
                        break;
                    const char32_t point = c;
                    text += QString::fromUcs4(&point, 1);
                }
                auto face = font();
                face.setBold(cell.attrs.bold);
                face.setItalic(cell.attrs.italic);
                face.setUnderline(cell.attrs.underline);
                painter.setFont(face);
                painter.setPen(fg);
                painter.drawText(box.topLeft() + QPoint(0, fm.ascent()), text);
            }
            for (const auto &p : s.predictions) // local echo over the real cell
            {
                if (p.row != line || p.col != col)
                    continue;
                const QRect predicted(box.topLeft(), QSize(w, h));
                painter.fillRect(predicted, bg);
                auto face = font();
                face.setUnderline(s.predictionUnderline);
                painter.setFont(face);
                painter.setPen(fg);
                painter.drawText(predicted.topLeft() + QPoint(0, fm.ascent()), QString::fromUcs4(&p.ch, 1));
            }
            if (s.cursor && !s.ended && line == cursor.row && col == cursor.col)
                painter.fillRect(hasFocus() ? box : box.adjusted(0, h - 2, 0, 0), hasFocus() ? s.cursorColor : QColor("#808080"));
        }
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
    QStringList lines;
    for (int line = a.y(); line <= b.y(); ++line)
    {
        QString text;
        const int from = line == a.y() ? a.x() : 0, to = line == b.y() ? b.x() : state->cols;
        for (int col = from; col < to; ++col)
        {
            VTermScreenCell cell{};
            if (!state->cell(line, col, cell) || cell.chars[0] == UINT32_MAX)
                continue;
            const char32_t point = cell.chars[0] ? char32_t(cell.chars[0]) : U' ';
            text += QString::fromUcs4(&point, 1);
        }
        while (text.endsWith(' '))
            text.chop(1);
        lines << text;
    }
    return lines.join('\n');
}

void Terminal::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    if (event->button() == Qt::LeftButton)
    {
        state->anchor = state->extent = cellAt(event->position().toPoint());
        state->selecting = true;
        update();
    }
    else if (event->button() == Qt::MiddleButton ||
             (event->button() == Qt::RightButton && QSettings().value("terminal/rightPaste", true).toBool()))
    {
        paste(QApplication::clipboard()->text());
    }
}

void Terminal::mouseMoveEvent(QMouseEvent *event)
{
    if (!state->selecting)
        return;
    state->extent = cellAt(event->position().toPoint());
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
        return state->cell(at.y(), col, cell) && cell.chars[0] && cell.chars[0] != ' ' && cell.chars[0] != UINT32_MAX;
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
        static int pending = 0;
        pending += event->angleDelta().y();
        const int steps = pending / 120;
        pending %= 120;
        if (steps)
            zoom(steps);
        event->accept();
        return;
    }
    const int lines = event->angleDelta().y() / 40;
    state->offset = std::clamp(state->offset + lines, 0, int(state->scrollback.size()));
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
    vterm_keyboard_start_paste(state->vt);
    for (const auto c : text.toUcs4())
        vterm_keyboard_unichar(state->vt, c, VTERM_MOD_NONE);
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
    return QWidget::event(event);
}

void Terminal::keyPressEvent(QKeyEvent *event)
{
    // Copy and paste: Cmd+C / Cmd+V on macOS; Ctrl+Shift+C / Ctrl+Shift+V, Ctrl+Insert / Shift+Insert elsewhere
    // (plain Ctrl+C stays the interrupt).
    const auto held = event->modifiers() & ~Qt::KeypadModifier;
#ifdef Q_OS_MACOS
    const bool copy = held == Qt::ControlModifier && event->key() == Qt::Key_C;
    const bool pasteKey = held == Qt::ControlModifier && event->key() == Qt::Key_V;
#else
    const bool copy = (held == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_C) ||
                      (held == Qt::ControlModifier && event->key() == Qt::Key_Insert);
    const bool pasteKey = (held == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_V) ||
                          (held == Qt::ShiftModifier && event->key() == Qt::Key_Insert);
#endif
    if (copy)
    {
        const auto text = selectedText();
        QApplication::clipboard()->setText(text.isEmpty() ? contents() : text);
        return;
    }
    if (pasteKey)
    {
        paste(QApplication::clipboard()->text());
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
    if (keys.contains(event->key()))
        vterm_keyboard_key(state->vt, keys.value(event->key()), VTermModifier(mods));
    else if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F12)
        vterm_keyboard_key(state->vt, VTermKey(VTERM_KEY_FUNCTION(event->key() - Qt::Key_F1 + 1)), VTermModifier(mods));
    else if ((mods & VTERM_MOD_CTRL) && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z)
        vterm_keyboard_unichar(state->vt, 'a' + event->key() - Qt::Key_A, VTermModifier(mods));
    else
        for (auto c : event->text().toUcs4())
            vterm_keyboard_unichar(state->vt, c, VTermModifier(mods));
}

QString Terminal::selection() const
{
    return selectedText();
}

bool Terminal::find(const QString &text, bool backward, Qt::CaseSensitivity cs)
{
    auto &s = *state;
    if (text.isEmpty())
        return false;
    auto lineText = [&](int line)
    {
        QString result;
        for (int col = 0; col < s.cols; ++col)
        {
            VTermScreenCell cell{};
            const char32_t point =
                s.cell(line, col, cell) && cell.chars[0] && cell.chars[0] != UINT32_MAX ? char32_t(cell.chars[0]) : U' ';
            result += QString::fromUcs4(&point, 1); // one cell = one character: columns stay aligned
        }
        return result;
    };
    const int first = -int(s.scrollback.size()), last = s.rows - 1;
    // Starting point: after (or before) the current selection, otherwise the bottom/top of the buffer.
    const bool selected = s.anchor != s.extent;
    int line = selected ? s.anchor.y() : (backward ? last : first);
    int column = selected ? s.anchor.x() + (backward ? -1 : 1) : (backward ? s.cols : 0);
    for (int step = 0; step <= last - first + 1; ++step)
    {
        const auto content = lineText(line);
        const int at =
            backward ? (column < 0 ? -1 : int(content.lastIndexOf(text, std::min<qsizetype>(column, content.size() - 1), cs)))
                     : int(content.indexOf(text, std::max(0, column), cs));
        if (at >= 0)
        {
            s.anchor = {at, line};
            s.extent = {at + int(text.size()), line};
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

bool Terminal::setLogFile(const QString &path)
{
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;
    state->log = std::move(file);
    return true;
}

QString Terminal::workingDirectory() const
{
    if (!state->directory.isEmpty())
        return state->directory;
    // Without OSC 7: a "user@host:path$" or "user@host:path#" style prompt on the cursor line.
    VTermPos cursor;
    vterm_state_get_cursorpos(vterm_obtain_state(state->vt), &cursor);
    static const QRegularExpression prompt("[\\w.-]+@[\\w.-]+:\\s*([~/][^\\s]*)\\s*[#$]\\s*$");
    const auto match = prompt.match(state->lineText(cursor.row).left(cursor.col + 1));
    return match.hasMatch() ? match.captured(1) : QString();
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
