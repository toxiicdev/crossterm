#pragma once
// RDP session inside a tab.
// Windows: Remote Desktop ActiveX control (mstscax.dll) hosted without ActiveQt through the system atl.dll and
// driven via IDispatch by name. Linux: xfreerdp embedded with /parent-window (password on stdin); the server
// certificate is read first and the user accepts it once (fingerprint kept in the profile).
// macOS: external client, the tab only shows an explanation.
// A disconnected session shows a card with the reason and a Reconnect button (connectToHost again).
#include <QString>
#include <QVariant>
#include <QWidget>
#include <functional>
#include <memory>

class RemoteCard;

// SHA-256 fingerprint of the TLS certificate an RDP server presents, as FtpClient::fingerprint() writes it; empty
// when it cannot be read (unreachable, or a server without TLS). done runs once, later, unless context is destroyed.
void rdpCertificateFingerprint(const QString &host, quint16 port, QObject *context,
                               std::function<void(const QString &fingerprint)> done);

class RdpView : public QWidget
{
public:
    struct Settings
    {
        QString host, user, domain;
        quint16 port = 3389;
        int width = 0, height = 0; // 0: widget size
        int colorDepth = 32;
        int authenticationLevel = 2; // 0 no verification, 1 required, 2 warn
        bool smartSizing = true, redirectClipboard = true, redirectDrives = false, redirectPrinters = false;
        QString fingerprint; // Linux: SHA-256 of the certificate already accepted for this session
    };

    RdpView(const Settings &settings, std::function<QString()> passwordProvider, QWidget *parent = nullptr);
    ~RdpView() override;

    std::function<void()> onConnected;
    std::function<void(const QString &reason)> onDisconnected;
    // Linux: certificate accepted by the user in the dialog, fingerprint to save in the profile ("rdpFingerprint").
    std::function<void(const QString &fingerprint)> onCertificateTrusted;

    // Also reconnects: a session still open is closed first.
    void connectToHost();
    void disconnectFromHost();
    void setFullScreen(bool on);
    void sendCtrlAltDel();

    // Control properties by name; "AdvancedSettings.X" uses the most recent available AdvancedSettingsN.
    // Windows only; elsewhere false / null QVariant().
    bool setControlProperty(const QString &path, const QVariant &value);
    QVariant controlProperty(const QString &path) const;

    // Test hooks.
    bool controlCreated() const;
    QString lastReason() const { return reason; }
    bool isConnected() const { return connected; }

protected:
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    friend struct RdpEvents;
    void connectedEvent();
    void disconnectedEvent(const QString &text);
    void updateCard();
    void launch(const QString &program, bool trusted); // Linux: starts xfreerdp

    struct Private;
    std::unique_ptr<Private> d;
    RemoteCard *card;
    Settings settings;
    std::function<QString()> passwordProvider;
    QString reason;
    bool connected = false;
};
