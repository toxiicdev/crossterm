#pragma once
#include "profiles.h"
#include <QColor>
#include <QWidget>
#include <array>
#include <functional>
#include <memory>
#include <optional>
class QIODevice;
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
    bool finished() const;
    // Searches the visible text and the scrollback starting from the last match; selects and reveals the result.
    bool find(const QString &text, bool backward, Qt::CaseSensitivity cs = Qt::CaseInsensitive);
    QString selection() const;
    QString allText() const;              // scrollback + screen, one line per terminal row
    bool setLogFile(const QString &path); // records the session output to a text file
    // Current shell directory: from OSC 7 if the shell sends it, otherwise from the prompt (may be "~/...").
    QString workingDirectory() const;
    // Called after "Session stopped": Enter closes the tab, R restarts the session.
    std::function<void()> onClose, onRestart;
    // Ctrl+wheel zoom: new font size in points (the window saves it per session).
    std::function<void(int)> onZoom;
    void setFontPointSize(int points);
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
    // Writes text (with ANSI sequences) to the screen locally, without sending it to the session: welcome banner.
    void printLocal(const QString &text);

protected:
    bool event(QEvent *) override;
    void changeEvent(QEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    struct State;
    std::unique_ptr<State> state;
    void setup();
    void stop(const QString &reason = {}); // shows "Session stopped"
    void readDevice();
    void write(const QByteArray &data);
    void finishPrompt(std::optional<QString> answer);
    QPoint cellAt(const QPoint &pixel) const;
    QString selectedText() const;
};
