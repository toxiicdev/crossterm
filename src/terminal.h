#pragma once
#include "profiles.h"
#include <QColor>
#include <QWidget>
#include <array>
#include <functional>
#include <memory>
#include <optional>
class QIODevice;
class QKeyEvent;
class QInputMethodEvent;
// Predicted character (local echo) in a screen cell, drawn over the real content.
struct PredictedCell
{
    int row, col;
    char32_t ch;
    bool operator==(const PredictedCell &o) const { return row == o.row && col == o.col && ch == o.ch; }
};

struct ColorScheme
{
    const char *name;
    QColor background, foreground, cursor;
    std::array<QColor, 16> ansi;
};

class Terminal : public QWidget
{
public:
    static const QList<ColorScheme> &schemes();
    // Applies the scheme saved in "terminal/scheme" (background, text, cursor and ANSI palette).
    void applyScheme();
    QColor background() const;
    explicit Terminal(const Command &command, QWidget *parent = nullptr);
    // Session on an already created device (telnet, rlogin, serial...): the terminal takes ownership of it.
    // If the device implements WindowSizeAware (transports.h) it receives the size on every resize.
    explicit Terminal(QIODevice *device, QWidget *parent = nullptr);
    ~Terminal() override;
    QString contents() const;
    void paste(const QString &text);
    // The paste shortcut of this platform (Cmd+V; Ctrl+Shift+V or Shift+Insert elsewhere).
    static bool isPasteKey(const QKeyEvent *event);
    // A paste the user made (shortcut or mouse) into the running session, not at a password prompt: called instead of
    // paste(), so the window can confirm risky text, then paste it, replicate it with MultiExec and record it.
    std::function<void(const QString &text)> onUserPaste;
    bool finished() const;
    // Searches the visible text and the scrollback starting from the last match; selects and reveals the result.
    // Every other occurrence on screen gets a soft highlight until clearFind() (or an empty text, or typing).
    bool find(const QString &text, bool backward, Qt::CaseSensitivity cs = Qt::CaseInsensitive);
    void clearFind(); // search closed: removes the highlights of the matches
    QString selection() const;
    QString allText() const;              // scrollback + screen, one line per terminal row
    bool setLogFile(const QString &path, bool timestamps = false); // records the session output to a text file
    // Current shell directory: from OSC 7 if the shell sends it, otherwise from the prompt (may be "~/...").
    QString workingDirectory() const;
    // Called after "Session stopped": Enter closes the tab, R restarts the session.
    std::function<void()> onClose, onRestart;
    // Local process sessions (ssh...): turns the exit code (-1 = killed by a signal) into an error shown in red above
    // "Session stopped", e.g. "Connection to host lost". Empty result, or no function: a normal end, nothing shown.
    std::function<QString(int exitCode)> describeExit;
    // Called once when the session ends, after the notice is written: error = it ended with an error shown in red
    // (connection lost, refused...), false for a normal end.
    std::function<void(bool error)> onEnded;
    // Ctrl+wheel zoom: new font size in points (the window saves it per session).
    std::function<void(int)> onZoom;
    void setFontPointSize(int points);
    // Font zoom by whole points (Ctrl/Cmd + wheel, Ctrl/Cmd +/-); 0 = back to the size in the settings.
    void zoom(int steps);
    // Predictions drawn over the screen (underlined if "terminal/predictionUnderline", on by default);
    // cursor.x() >= 0 shows the cursor there. The caller refreshes them after every output: real output wins.
    // With a MoshDevice the terminal runs MoshPredictor (mosh.h) on its own, according to "mosh/prediction".
    void setPredictions(const QList<PredictedCell> &cells, QPoint cursor = {-1, -1});
    void clearPredictions();
    QList<PredictedCell> predictions() const;
    QPoint cursorPosition() const;             // real cursor: x = column, y = screen row
    char32_t cellChar(int row, int col) const; // U' ' for an empty cell, 0 outside the screen
    // Inline prompt (askpass): shows text and reads one line typed in the terminal without sending it to the
    // session. done(nullopt) on Ctrl+C, Escape or when the session ends.
    void prompt(const QString &text, bool echo, std::function<void(std::optional<QString>)> done);
    bool prompting() const;
    // SSH session: keys typed before ssh has logged in are kept instead of sent. Sent at once they would wait in
    // the terminal and reach the remote shell as a command, a password typed before its prompt included. They go
    // to the password prompt if one opens, to the session once it is logged in. Only for a session that gets a
    // remote terminal (ssh makes its own raw at login: that is the signal); nothing is kept on Windows.
    void holdKeysUntilLogin();
    bool holdingKeys() const; // a key typed now would be kept
    // Writes text (with ANSI sequences) to the screen locally, without sending it to the session: welcome banner.
    void printLocal(const QString &text);
    // Keeps the initial 80x24 grid until the terminal is first shown: the sizes a tab gets while it is still hidden
    // (a few pixels, 2 rows) would push the welcome banner into the scrollback before the session starts writing.
    void sizeWhenShown();
    // Output lines removed before they reach the screen (harmless client warnings, e.g. X11 without xauth).
    void hideOutput(const QList<QByteArray> &lines);

protected:
    bool event(QEvent *) override;
    void changeEvent(QEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void setGrid(int rows, int cols);
    void keyPressEvent(QKeyEvent *) override;
    // Dead keys and compose (Linux, macOS) and input methods (Chinese, Japanese...): the committed text is typed.
    void inputMethodEvent(QInputMethodEvent *) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    void userPaste(const QString &text);
    bool mouseReport(const QPointF &position, Qt::KeyboardModifiers modifiers, int button, bool pressed);
    struct State;
    std::unique_ptr<State> state;
    void setup();
    // Shows "Session stopped", after the reason (in red when error is set).
    void stop(const QString &reason = {}, bool error = true);
    void readDevice();
    bool readPty();   // reads the local process output available now; false once its side is closed
    void checkExit(); // local process: ends the session once it exited
    void write(const QByteArray &data);
    void finishPrompt(std::optional<QString> answer);
    void dropHeldKeys();
    void replayHeldKeys();
    void releaseHeldKeys(); // once logged in
    void feed(QByteArray data);
    void outputChanged(); // repaints after session output, at most every 16 ms
    // Link under the mouse (active = Cmd/Ctrl held): underlined, with a pointing hand cursor.
    void hoverLink(const QPoint &pixel, bool active);
    QPoint cellAt(const QPoint &pixel) const;
    QString selectedText() const;
};
