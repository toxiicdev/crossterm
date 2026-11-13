#pragma once
// RDP session inside a tab.
// Windows: Remote Desktop ActiveX control (mstscax.dll) hosted without ActiveQt through the system atl.dll and
// driven via IDispatch by name. Linux: xfreerdp embedded with /parent-window (password on stdin).
// macOS: external client, the tab only shows an explanation.
#include <QString>
#include <QVariant>
#include <QWidget>
#include <functional>
#include <memory>

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
    };

    RdpView(const Settings &settings, std::function<QString()> passwordProvider, QWidget *parent = nullptr);
    ~RdpView() override;

    std::function<void()> onConnected;
    std::function<void(const QString &reason)> onDisconnected;

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

private:
    friend struct RdpEvents;
    void connectedEvent();
    void disconnectedEvent(const QString &text);

    struct Private;
    std::unique_ptr<Private> d;
    Settings settings;
    std::function<QString()> passwordProvider;
    QString reason;
    bool connected = false;
};
