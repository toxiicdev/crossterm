#pragma once
// Built-in VNC viewer (RFB 3.3/3.7/3.8, RFC 6143): the remote desktop is drawn inside a tab.
#include <QHash>
#include <QImage>
#include <QWidget>
#include <functional>

class QPushButton;
class QTcpSocket;

// DES (FIPS 46-3) of one 8-byte block with an 8-byte key (parity bits ignored).
QByteArray desEncrypt(const QByteArray &key, const QByteArray &block);
// VNC Authentication response: password truncated/padded to 8 bytes, bits of each byte reversed, DES-ECB over the 16 bytes.
QByteArray vncAuthResponse(const QByteArray &password, const QByteArray &challenge);

// Inflate (RFC 1950/1951) on a persistent stream: ZRLE uses a single zlib stream for the whole connection.
class VncInflater
{
public:
    // Queues compressed data; bytes of complete blocks end up in out. false if the stream is corrupt
    // or if out would exceed maxOutput bytes (>= 0).
    bool feed(const QByteArray &data, QByteArray &out, qsizetype maxOutput = -1);
    bool atEnd() const { return finished; } // the final block was decoded
    // Input kept for the next call: a block that is not complete yet (what follows the stream, once at its end).
    qsizetype unread() const { return pending.size(); }

private:
    QByteArray pending, window;
    quint32 bitbuf = 0;
    int bitcnt = 0;
    bool header = false, finished = false;
};

// Centred card over a remote desktop view (VNC, RDP): a message, a dimmer detail line and, once disconnected,
// a Reconnect button. Colours follow the theme; it stays centred when its parent is resized.
class RemoteCard : public QWidget
{
public:
    explicit RemoteCard(QWidget *parent);

    std::function<void()> onReconnect;

    void present(const QString &text, const QString &detail, bool reconnect);
    bool offersReconnect() const;

protected:
    bool eventFilter(QObject *watched, QEvent *e) override;
    void changeEvent(QEvent *e) override;
    void paintEvent(QPaintEvent *) override;

private:
    void place();

    QString text, detail;
    QPushButton *button;
};

class VncView : public QWidget
{
public:
    VncView(const QString &host, quint16 port, std::function<QString()> passwordProvider, QWidget *parent = nullptr);
    ~VncView() override;

    std::function<void(const QString &reason)> onDisconnected;
    std::function<QString()> onPasswordNeeded; // initialized by the constructor; empty string = cancel

    void setScaled(bool on);
    bool isScaled() const { return scaled; }
    void setViewOnly(bool on) { viewOnly = on; }
    bool isViewOnly() const { return viewOnly; }
    void sendCtrlAltDel();
    // Drops the current connection, if any, and connects again; the last frame stays until the new one arrives.
    void reconnect();

    QImage framebuffer() const { return fb; }
    bool isConnected() const { return connected; }
    QString desktopName() const { return name; }
    QSize sizeHint() const override;

protected:
    bool event(QEvent *e) override;
    bool focusNextPrevChild(bool) override { return false; }
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    enum State
    {
        Version,
        Security,
        Challenge,
        Result,
        Init,
        Message,
        Rects
    };

    bool step();
    bool decodeRect();
    bool zrle(const QByteArray &data);
    bool failWithReason(int skip, const char *fallback);
    void fail(const QString &reason);
    void send(const QByteArray &data);
    void sendClipboard(); // ClientCutText, only while focused and if the text differs from serverText
    void requestUpdate(bool incremental);
    void sendKey(quint32 keysym, bool down);
    void sendPointer(const QPointF &position);
    void putPixels(int x, int y, int w, int h, const uchar *p);
    void fill(int x, int y, int w, int h, quint32 colour);
    QRectF target() const;
    QString address() const;
    bool has(qint64 n) const { return in.size() - rd >= n; }
    const uchar *at() const { return reinterpret_cast<const uchar *>(in.constData()) + rd; }

    QTcpSocket *socket;
    RemoteCard *card;
    QString host;
    quint16 port;
    int session = 0; // incremented by reconnect()
    QByteArray in;
    qsizetype rd = 0;
    qint64 skip = 0; // bytes of a refused ServerCutText still to drop
    State state = Version;
    int minor = 8;
    QImage fb;
    QRect dirty;  // framebuffer area changed by the update being decoded
    QPoint wheel; // angleDelta not yet sent as wheel clicks
    QString name, serverText;
    bool connected = false, closed = false, busy = false, scaled = true, viewOnly = false, fullNext = false;
    int rects = 0, rx = 0, ry = 0, rw = 0, rh = 0, tile = 0, buttons = 0;
    qint32 encoding = 0;
    bool inRect = false;
    quint32 bg = 0, fg = 0;
    QHash<int, quint32> pressed;
    VncInflater zlib;
};
