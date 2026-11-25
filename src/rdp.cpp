#include "rdp.h"
#include "ftp.h" // FtpClient::fingerprint
#include "profiles.h"
#include "vnc.h" // RemoteCard
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <algorithm>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <ocidl.h>
#include <ole2.h>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#else
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUrl>
#include <QVBoxLayout>
#endif

static QString ui(const char *text)
{
    return QCoreApplication::translate("CrossTerm", text);
}

static QString destination(const RdpView::Settings &settings)
{
    return (settings.host.contains(':') ? "[" + settings.host + "]" : settings.host) + ":" + QString::number(settings.port);
}

// User callbacks run from the event loop: they may even destroy the view without re-entering the control.
void RdpView::connectedEvent()
{
    connected = true;
    QTimer::singleShot(0, this,
                       [this]
                       {
                           if (onConnected)
                               onConnected();
                       });
}

void RdpView::disconnectedEvent(const QString &text)
{
    connected = false;
    reason = text;
    QTimer::singleShot(0, this,
                       [this, text]
                       {
                           updateCard();
                           if (onDisconnected)
                               onDisconnected(text);
                       });
}

void RdpView::setFullScreen(bool on)
{
#ifdef Q_OS_WIN
    setControlProperty("FullScreen", on);
#else
    // embedded xfreerdp follows its containing window (/dynamic-resolution).
    window()->setWindowState(on ? window()->windowState() | Qt::WindowFullScreen
                                : window()->windowState() & ~Qt::WindowFullScreen);
#endif
}

#ifdef Q_OS_WIN
// ---------------------------------------------------------------------------------------------------------------
// Windows: ActiveX control hosted by atl.dll ("AtlAxWin" window class), without ActiveQt.

static const IID DIID_IMsTscAxEvents = {0x336d5562, 0xefa8, 0x482e, {0x8c, 0xb3, 0xc5, 0xc0, 0xfc, 0x7a, 0x7d, 0xb6}};
static const IID IID_IMsTscNonScriptable = {0xc1e6743a, 0x41c1, 0x4a74, {0x83, 0x2a, 0x0d, 0xd0, 0x6c, 0x1c, 0x7a, 0x0e}};
static const IID IID_IMsRdpClientNonScriptable = {0x2f079c4c, 0x87b2, 0x4afd, {0x97, 0xab, 0x20, 0xcd, 0xb4, 0x30, 0x38, 0xae}};

// Non-dispatch interfaces of mstscax.dll: only the vtable order matters.
struct IMsTscNonScriptable : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE put_ClearTextPassword(BSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PortablePassword(BSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PortablePassword(BSTR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PortableSalt(BSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PortableSalt(BSTR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BinaryPassword(BSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BinaryPassword(BSTR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BinarySalt(BSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BinarySalt(BSTR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetPassword() = 0;
};
struct IMsRdpClientNonScriptable : IMsTscNonScriptable
{
    virtual HRESULT STDMETHODCALLTYPE NotifyRedirectDeviceChange(UINT_PTR, LONG_PTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SendKeys(long numKeys, VARIANT_BOOL *keyUp, long *keyData) = 0;
};

// DISPIDs of IMsTscAxEvents (mstsc.idl).
enum
{
    EvConnected = 2,
    EvLoginComplete = 3,
    EvDisconnected = 4,
    EvFatalError = 10
};

static VARIANT toVariant(const QVariant &value)
{
    VARIANT v;
    VariantInit(&v);
    switch (value.typeId())
    {
    case QMetaType::Bool:
        v.vt = VT_BOOL;
        v.boolVal = value.toBool() ? VARIANT_TRUE : VARIANT_FALSE;
        break;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        v.vt = VT_I4;
        v.lVal = value.toInt();
        break;
    default:
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(reinterpret_cast<const OLECHAR *>(value.toString().utf16()));
    }
    return v;
}

static QVariant fromVariant(const VARIANT &v)
{
    if (v.vt == VT_BSTR)
        return QString::fromWCharArray(v.bstrVal ? v.bstrVal : L"");
    if (v.vt == VT_BOOL)
        return v.boolVal != VARIANT_FALSE;
    VARIANT n;
    VariantInit(&n);
    if (FAILED(VariantChangeType(&n, &v, 0, VT_I4)))
        return {};
    return int(n.lVal);
}

// Invoke by name; args in natural order (DISPPARAMS wants them reversed). The args are freed.
static HRESULT invoke(IDispatch *object, const QString &name, WORD flags, std::vector<VARIANT> args = {},
                      VARIANT *result = nullptr)
{
    HRESULT hr = E_POINTER;
    if (object)
    {
        std::wstring wide = name.toStdWString();
        LPOLESTR names = wide.data();
        DISPID id = 0;
        hr = object->GetIDsOfNames(IID_NULL, &names, 1, LOCALE_USER_DEFAULT, &id);
        if (SUCCEEDED(hr))
        {
            std::reverse(args.begin(), args.end());
            DISPID put = DISPID_PROPERTYPUT;
            DISPPARAMS params{args.data(), nullptr, UINT(args.size()), 0};
            if (flags & DISPATCH_PROPERTYPUT)
            {
                params.rgdispidNamedArgs = &put;
                params.cNamedArgs = 1;
            }
            hr = object->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &params, result, nullptr, nullptr);
        }
    }
    for (auto &arg : args)
        VariantClear(&arg);
    return hr;
}

static ComPtr<IDispatch> child(IDispatch *object, const QString &name)
{
    // "AdvancedSettings" = the most recent version exposed by the control (later ones inherit the earlier ones).
    QStringList candidates{name};
    if (name == "AdvancedSettings")
        candidates = {"AdvancedSettings9", "AdvancedSettings8", "AdvancedSettings7", "AdvancedSettings6", "AdvancedSettings5",
                      "AdvancedSettings4", "AdvancedSettings3", "AdvancedSettings2", "AdvancedSettings"};
    for (const auto &candidate : candidates)
    {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(invoke(object, candidate, DISPATCH_PROPERTYGET, {}, &v)) && v.vt == VT_DISPATCH && v.pdispVal)
        {
            ComPtr<IDispatch> result;
            result.Attach(v.pdispVal); // the reference passes to result
            return result;
        }
        VariantClear(&v);
    }
    return {};
}

// Resolves "A.B.C": returns the object for "A.B" and "C" in leaf.
static ComPtr<IDispatch> resolve(IDispatch *root, const QString &path, QString &leaf)
{
    auto parts = path.split('.');
    leaf = parts.takeLast();
    ComPtr<IDispatch> object(root);
    for (const auto &part : parts)
    {
        if (!object)
            break;
        object = child(object.Get(), part);
    }
    return object;
}

struct RdpView::Private
{
    bool ole = false;
    HWND host = nullptr; // AtlAxWin window that contains the control
    bool embedded = false;
    bool restart = false; // connectToHost() on an open session: connect again once it has disconnected
    ComPtr<IUnknown> control;
    ComPtr<IDispatch> dispatch;
    ComPtr<IConnectionPoint> point;
    DWORD cookie = 0;
    struct RdpEvents *sink = nullptr;
    QTimer resize;
};

// IMsTscAxEvents sink.
struct RdpEvents final : IDispatch
{
    ULONG refs = 1;
    RdpView *view;
    explicit RdpEvents(RdpView *view) : view(view) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (iid == IID_IUnknown || iid == IID_IDispatch || iid == DIID_IMsTscAxEvents)
        {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --refs;
        if (!left)
            delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *count) override
    {
        *count = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR *, UINT, LCID, DISPID *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS *params, VARIANT *, EXCEPINFO *, UINT *) override
    {
        if (!view)
            return S_OK;
        const QVariant first = params && params->cArgs ? fromVariant(params->rgvarg[params->cArgs - 1]) : QVariant();
        switch (id)
        {
        case EvConnected:
            view->connectedEvent();
            break;
        case EvLoginComplete:
            qInfo("RDP: login complete");
            break;
        case EvDisconnected:
        {
            if (view->d->restart)
            {
                // Requested by connectToHost(): not reported, the new connection starts from the event loop.
                view->d->restart = false;
                view->connected = false;
                QTimer::singleShot(0, view, [v = view] { v->connectToHost(); });
                break;
            }
            const int code = first.toInt();
            QVariant extended;
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(invoke(view->d->dispatch.Get(), "ExtendedDisconnectReason", DISPATCH_PROPERTYGET, {}, &v)))
                extended = fromVariant(v);
            VariantClear(&v);
            QString text;
            if (SUCCEEDED(invoke(view->d->dispatch.Get(), "GetErrorDescription", DISPATCH_METHOD,
                                 {toVariant(code), toVariant(extended.toInt())}, &v)))
                text = fromVariant(v).toString().trimmed();
            VariantClear(&v);
            if (text.isEmpty())
                text = ui("Disconnected (code %1)").arg(code);
            view->disconnectedEvent(text);
            break;
        }
        case EvFatalError:
            view->disconnectedEvent(ui("Fatal error in the Remote Desktop control (code %1)").arg(first.toInt()));
            break;
        }
        return S_OK;
    }
};

// "...NotSafeForScripting" CLSIDs, from newest (MsRdpClient10) to oldest.
static const wchar_t *const clsids[] = {L"{A0C63C30-F08D-4AB4-907C-34905D770C7D}", L"{8B918B82-7985-4C24-89DF-C33AD2BBFBCD}",
                                        L"{A3BC03A0-041D-42E3-AD22-882B7865C9C5}", L"{54D38BF7-B1EF-4479-9674-1BD6EA465258}",
                                        L"{D2EA46A7-C2BF-426B-AF24-E19C44456399}", L"{4EB89FF4-7F78-4A0F-8B8D-2BF02E94E4B2}"};

RdpView::RdpView(const Settings &settings, std::function<QString()> passwordProvider, QWidget *parent)
    : QWidget(parent), d(new Private), card(new RemoteCard(this)), settings(settings),
      passwordProvider(std::move(passwordProvider))
{
    card->onReconnect = [this] { connectToHost(); };
    setAttribute(Qt::WA_NativeWindow);
    setFocusPolicy(Qt::StrongFocus);
    d->ole = SUCCEEDED(OleInitialize(nullptr));
    // Only from the system folder: no same-named DLLs next to the executable.
    static HMODULE atl = LoadLibraryExW(L"atl.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto init = atl ? reinterpret_cast<BOOL(WINAPI *)()>(GetProcAddress(atl, "AtlAxWinInit")) : nullptr;
    auto getControl =
        atl ? reinterpret_cast<HRESULT(WINAPI *)(HWND, IUnknown **)>(GetProcAddress(atl, "AtlAxGetControl")) : nullptr;
    if (!init || !getControl || !init())
    {
        reason = ui("The Remote Desktop ActiveX host (atl.dll) is not available");
        return;
    }
    // With the "offscreen" platform winId() is not an HWND: the control lives in a hidden window.
    d->embedded = QGuiApplication::platformName() == "windows";
    const HWND parentWindow = d->embedded ? reinterpret_cast<HWND>(winId()) : nullptr;
    const qreal ratio = devicePixelRatioF();
    for (const wchar_t *clsid : clsids)
    {
        d->host = CreateWindowExW(0, L"AtlAxWin", clsid,
                                  d->embedded ? WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS : WS_POPUP, 0, 0,
                                  std::max(1, int(width() * ratio)), std::max(1, int(height() * ratio)), parentWindow, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
        IUnknown *unknown = nullptr;
        if (d->host && SUCCEEDED(getControl(d->host, &unknown)) && unknown)
        {
            d->control.Attach(unknown);
            if (SUCCEEDED(d->control.As(&d->dispatch)))
                break;
            d->control.Reset();
        }
        if (d->host)
            DestroyWindow(d->host);
        d->host = nullptr;
    }
    if (!d->dispatch)
    {
        reason = ui("The Remote Desktop ActiveX control (mstscax.dll) is not available");
        return;
    }
    ComPtr<IConnectionPointContainer> container;
    if (SUCCEEDED(d->control.As(&container)) && SUCCEEDED(container->FindConnectionPoint(DIID_IMsTscAxEvents, &d->point)))
    {
        d->sink = new RdpEvents(this);
        if (FAILED(d->point->Advise(d->sink, &d->cookie)))
            d->cookie = 0;
    }
    d->resize.setSingleShot(true);
    d->resize.setInterval(400);
    QObject::connect(
        &d->resize, &QTimer::timeout, this,
        [this]
        {
            // Resize the session without reconnecting (IMsRdpClient9); otherwise SmartSizing.
            const qreal ratio = devicePixelRatioF();
            const int w = std::max(200, int(width() * ratio)) & ~1, h = std::max(200, int(height() * ratio));
            // Only with the widget's size ("0" in the profile): a fixed resolution stays as chosen.
            if (connected && this->settings.width <= 0 && this->settings.height <= 0)
                invoke(d->dispatch.Get(), "UpdateSessionDisplaySettings", DISPATCH_METHOD,
                       {toVariant(w), toVariant(h), toVariant(w), toVariant(h), toVariant(0), toVariant(100), toVariant(100)});
        });
}

RdpView::~RdpView()
{
    if (d->point && d->cookie)
        d->point->Unadvise(d->cookie);
    if (d->sink)
    {
        d->sink->view = nullptr;
        d->sink->Release();
    }
    if (connected)
        invoke(d->dispatch.Get(), "Disconnect", DISPATCH_METHOD);
    d->point.Reset();
    d->dispatch.Reset();
    d->control.Reset();
    if (d->host)
        DestroyWindow(d->host); // AtlAxWin releases the control
    if (d->ole)
        OleUninitialize();
}

bool RdpView::controlCreated() const
{
    return bool(d->dispatch);
}

bool RdpView::setControlProperty(const QString &path, const QVariant &value)
{
    QString leaf;
    const auto object = resolve(d->dispatch.Get(), path, leaf);
    const HRESULT hr = invoke(object.Get(), leaf, DISPATCH_PROPERTYPUT, {toVariant(value)});
    if (FAILED(hr))
        qDebug("RDP: property %s not set (0x%08lx)", qPrintable(path), ulong(hr)); // never the value
    return SUCCEEDED(hr);
}

QVariant RdpView::controlProperty(const QString &path) const
{
    QString leaf;
    const auto object = resolve(d->dispatch.Get(), path, leaf);
    VARIANT v;
    VariantInit(&v);
    QVariant result;
    if (SUCCEEDED(invoke(object.Get(), leaf, DISPATCH_PROPERTYGET, {}, &v)))
        result = fromVariant(v);
    VariantClear(&v);
    return result;
}

void RdpView::connectToHost()
{
    if (!d->dispatch)
    {
        disconnectedEvent(reason);
        return;
    }
    // Session still open or connecting (Connected: 1 or 2): Connect would fail, so it is closed first and the
    // connection starts again from its Disconnected event.
    if (controlProperty("Connected").toInt() != 0)
    {
        d->restart = true;
        if (SUCCEEDED(invoke(d->dispatch.Get(), "Disconnect", DISPATCH_METHOD)))
            return;
        d->restart = false;
    }
    const qreal ratio = devicePixelRatioF();
    const int w = settings.width > 0 ? settings.width : std::max(640, int(width() * ratio));
    const int h = settings.height > 0 ? settings.height : std::max(480, int(height() * ratio));
    setControlProperty("Server", settings.host);
    setControlProperty("UserName", settings.user);
    setControlProperty("Domain", settings.domain);
    setControlProperty("DesktopWidth", std::min(w, 8192) & ~3);
    setControlProperty("DesktopHeight", std::min(h, 8192));
    setControlProperty("ColorDepth", settings.colorDepth);
    setControlProperty("AdvancedSettings.RDPPort", int(settings.port));
    setControlProperty("AdvancedSettings.EnableCredSspSupport", true);
    setControlProperty("AdvancedSettings.SmartSizing", settings.smartSizing);
    setControlProperty("AdvancedSettings.RedirectClipboard", settings.redirectClipboard);
    setControlProperty("AdvancedSettings.RedirectDrives", settings.redirectDrives);
    setControlProperty("AdvancedSettings.RedirectPrinters", settings.redirectPrinters);
    setControlProperty("AdvancedSettings.AuthenticationLevel", settings.authenticationLevel);
    // The connection bar shows FullScreenTitle; ConnectionBarText is only in IMsRdpClientNonScriptable3 (non-dispatch).
    setControlProperty("FullScreenTitle", ui("Remote Desktop: %1").arg(settings.host));
    if (passwordProvider)
    {
        QString password = passwordProvider();
        if (!password.isEmpty())
        {
            // Only to the control: BSTR zeroed before freeing it, never in logs.
            BSTR secret = SysAllocString(reinterpret_cast<const OLECHAR *>(password.utf16()));
            QString leaf;
            const auto advanced = resolve(d->dispatch.Get(), "AdvancedSettings.ClearTextPassword", leaf);
            VARIANT v; // not owned: the BSTR is freed below, after zeroing it
            VariantInit(&v);
            v.vt = VT_BSTR;
            v.bstrVal = secret;
            DISPID put = DISPID_PROPERTYPUT, id = 0;
            LPOLESTR name = const_cast<LPOLESTR>(L"ClearTextPassword");
            DISPPARAMS params{&v, &put, 1, 1};
            const bool done = advanced && SUCCEEDED(advanced->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &id)) &&
                              SUCCEEDED(advanced->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &params,
                                                         nullptr, nullptr, nullptr));
            ComPtr<IMsTscNonScriptable> nonScriptable;
            if (!done && SUCCEEDED(d->control->QueryInterface(IID_IMsTscNonScriptable, &nonScriptable)))
                nonScriptable->put_ClearTextPassword(secret);
            SecureZeroMemory(secret, SysStringByteLen(secret));
            SysFreeString(secret);
            password.fill(QChar(0));
        }
    }
    reason.clear();
    updateCard();
    const HRESULT hr = invoke(d->dispatch.Get(), "Connect", DISPATCH_METHOD);
    if (FAILED(hr))
        disconnectedEvent(ui("Remote Desktop connection could not start (0x%1)").arg(ulong(hr), 8, 16, QChar('0')));
}

void RdpView::disconnectFromHost()
{
    invoke(d->dispatch.Get(), "Disconnect", DISPATCH_METHOD);
}

void RdpView::sendCtrlAltDel()
{
    ComPtr<IMsRdpClientNonScriptable> keys;
    if (!d->control || FAILED(d->control->QueryInterface(IID_IMsRdpClientNonScriptable, &keys)))
        return;
    // Scancodes: Ctrl 0x1d, Alt 0x38, Del 0x53; pressed, then released in reverse order.
    VARIANT_BOOL up[] = {VARIANT_FALSE, VARIANT_FALSE, VARIANT_FALSE, VARIANT_TRUE, VARIANT_TRUE, VARIANT_TRUE};
    long data[] = {0x1d, 0x38, 0x53, 0x53, 0x38, 0x1d};
    if (d->host)
        SetFocus(d->host);
    keys->SendKeys(6, up, data);
}

void RdpView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (d->host && d->embedded)
    {
        const qreal ratio = devicePixelRatioF();
        MoveWindow(d->host, 0, 0, int(width() * ratio), int(height() * ratio), TRUE);
    }
    if (d->dispatch)
        d->resize.start();
}

#else
// ---------------------------------------------------------------------------------------------------------------
// Linux: xfreerdp embedded in the widget's native window; macOS: external client.

struct RdpView::Private
{
    QProcess *process = nullptr;
    QByteArray prompt;       // last bytes printed by xfreerdp, to recognize prompts
    int attempt = 0;         // Linux: connection attempts so far; an older certificate check is dropped
    QLabel *label = nullptr; // macOS: explanation, hidden while the card is shown
};

RdpView::RdpView(const Settings &settings, std::function<QString()> passwordProvider, QWidget *parent)
    : QWidget(parent), d(new Private), card(new RemoteCard(this)), settings(settings),
      passwordProvider(std::move(passwordProvider))
{
    card->onReconnect = [this] { connectToHost(); };
#ifdef Q_OS_MACOS
    setFocusPolicy(Qt::StrongFocus); // Return/Enter on the card
    auto *layout = new QVBoxLayout(this);
    // One literal: i18n/extract.py does not join adjacent string literals.
    auto *label = new QLabel(
        ui("On macOS the Remote Desktop session cannot be embedded: it opens in the external Microsoft Remote Desktop (Windows App) client."),
        this);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
    d->label = label;
#else
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setObjectName("rdpView");
    setStyleSheet("#rdpView { background: black; }"); // by name: the card must not inherit it
#endif
}

RdpView::~RdpView()
{
    if (d->process)
    {
        d->process->disconnect();
        d->process->kill();
        d->process->waitForFinished(2000);
    }
}

bool RdpView::controlCreated() const
{
    return false;
}

bool RdpView::setControlProperty(const QString &, const QVariant &)
{
    return false;
}

QVariant RdpView::controlProperty(const QString &) const
{
    return {};
}

void RdpView::connectToHost()
{
    reason.clear();
    updateCard();
#ifdef Q_OS_MACOS
    // .rdp file opened by the Microsoft client (rdp://full%20address=s:host:port is not a valid QUrl).
    // The password is not passed; control characters would add lines (settings) to the file.
    const auto field = [](QString text) { return text.remove(QRegularExpression("[\\x00-\\x1f\\x7f]")).toUtf8(); };
    QByteArray content = "full address:s:" + field(destination(settings)) + "\r\n";
    if (!settings.user.isEmpty())
        content += "username:s:" + field(settings.user) + "\r\n";
    // Own files left behind (quit before the removal below) go at a later connection, once a day old.
    static const QRegularExpression own("^CrossTerm-[A-Za-z0-9]{6}\\.rdp$");
    const QDateTime stale = QDateTime::currentDateTime().addDays(-1);
    for (const QFileInfo &old : QDir(QDir::tempPath()).entryInfoList({"CrossTerm-*.rdp"}, QDir::Files | QDir::NoSymLinks))
    {
        if (own.match(old.fileName()).hasMatch() && old.lastModified() < stale)
            QFile::remove(old.absoluteFilePath());
    }
    // openUrl() returns at once and the client reads the file later: it is removed after a minute, by a timer
    // that outlives the tab, not when the tab closes or reconnects.
    QTemporaryFile file(QDir::tempPath() + "/CrossTerm-XXXXXX.rdp");
    file.setAutoRemove(false);
    const bool written = file.open() && file.write(content) == content.size();
    const QString path = file.fileName();
    file.close();
    const bool opened = written && QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    if (opened)
        QTimer::singleShot(60000, qApp, [path] { QFile::remove(path); });
    else if (!path.isEmpty())
        QFile::remove(path);
    if (opened)
        connectedEvent();
    else
        disconnectedEvent(ui("No Remote Desktop client is installed"));
#else
    // Reconnect: the previous xfreerdp is stopped first, without reporting its exit.
    if (d->process)
    {
        d->process->disconnect();
        d->process->kill();
        d->process->waitForFinished(2000);
        d->process->deleteLater();
        d->process = nullptr;
        d->prompt.clear();
        connected = false;
    }
    QString program = QStandardPaths::findExecutable("xfreerdp");
    if (program.isEmpty())
        program = QStandardPaths::findExecutable("xfreerdp3");
    if (program.isEmpty())
    {
        disconnectedEvent(ui("xfreerdp was not found: install FreeRDP"));
        return;
    }
    // xfreerdp cannot ask about a certificate while it reads the password from stdin: with /cert:tofu it accepts an
    // unknown one by itself. So the certificate is read here first and the user decides, once per certificate; a
    // server whose certificate was not accepted this way gets /cert:deny (only what xfreerdp already knows passes).
    const int attempt = ++d->attempt;
    rdpCertificateFingerprint(
        settings.host, settings.port, this,
        [this, program, attempt](const QString &print)
        {
            if (attempt != d->attempt || d->process)
                return;
            bool trusted = !print.isEmpty() && print.compare(settings.fingerprint, Qt::CaseInsensitive) == 0;
            if (!trusted && !print.isEmpty())
            {
                const auto answer = QMessageBox::warning(
                    this, ui("Untrusted certificate"),
                    ui("The certificate of %1 cannot be verified.\n\nSHA-256 fingerprint:\n%2\n\nTrust this certificate for this host?")
                        .arg(settings.host, print),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (attempt != d->attempt || d->process)
                    return;
                if (answer != QMessageBox::Yes)
                {
                    disconnectedEvent(ui("Untrusted certificate"));
                    return;
                }
                settings.fingerprint = print;
                if (onCertificateTrusted)
                    onCertificateTrusted(print);
                trusted = true;
            }
            launch(program, trusted);
        });
#endif
}

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
void RdpView::launch(const QString &program, bool trusted)
{
    QStringList args{"/v:" + destination(settings), "/from-stdin:force", trusted ? "/cert:tofu" : "/cert:deny",
                     "/bpp:" + QString::number(settings.colorDepth)};
    // Embedded only under X11: on Wayland winId() is no X window and xfreerdp (XWayland) could not use it.
    if (QGuiApplication::platformName() == "xcb")
        args << "/parent-window:" + QString::number(winId());
    // The widget's size follows the tab; a fixed resolution stays as chosen (scaled with smart sizing).
    if (settings.width <= 0 || settings.height <= 0)
        args << "/dynamic-resolution";
    else if (settings.smartSizing)
        args << "/smart-sizing";
    if (!settings.user.isEmpty())
        args << "/u:" + settings.user;
    if (!settings.domain.isEmpty())
        args << "/d:" + settings.domain;
    if (settings.width > 0 && settings.height > 0)
        args << QString("/size:%1x%2").arg(settings.width).arg(settings.height);
    else
        args << QString("/size:%1x%2").arg(std::max(640, width())).arg(std::max(480, height()));
    args << (settings.redirectClipboard ? "+clipboard" : "-clipboard");
    if (settings.redirectDrives)
        args << "/home-drive"; // the home folder only, not every mount point (/drives)
    if (settings.redirectPrinters)
        args << "/printer";
    d->process = hideConsoleWindow(new QProcess(this)); // Linux: ends with CrossTerm also on a crash
    d->process->setProcessChannelMode(QProcess::MergedChannels);
    // Missing credentials are prompted on stdin: answer each prompt, the password never goes into argv.
    QObject::connect(d->process, &QProcess::readyReadStandardOutput, this,
                     [this]
                     {
                         d->prompt = (d->prompt + d->process->readAllStandardOutput()).right(256);
                         const QByteArray tail = d->prompt.trimmed().toLower();
                         QByteArray answer;
                         bool matched = true;
                         if (tail.endsWith("username:"))
                             answer = settings.user.toUtf8();
                         else if (tail.endsWith("domain:"))
                             answer = settings.domain.toUtf8();
                         else if (tail.endsWith("password:"))
                         {
                             QString password = passwordProvider ? passwordProvider() : QString();
                             answer = password.toUtf8();
                             password.fill(QChar(0));
                         }
                         else
                             matched = false;
                         if (!matched)
                             return;
                         d->prompt.clear();
                         d->process->write(answer + "\n");
                         answer.fill('\0');
                     });
    QObject::connect(d->process, &QProcess::started, this, [this] { connectedEvent(); });
    QObject::connect(d->process, &QProcess::finished, this,
                     [this](int code, QProcess::ExitStatus status)
                     {
                         disconnectedEvent(status == QProcess::CrashExit ? ui("xfreerdp crashed")
                                                                         : ui("xfreerdp exited with code %1").arg(code));
                     });
    QObject::connect(d->process, &QProcess::errorOccurred, this,
                     [this](QProcess::ProcessError error)
                     {
                         if (error == QProcess::FailedToStart)
                             disconnectedEvent(d->process->errorString());
                     });
    d->process->start(program, args);
}
#endif

void RdpView::disconnectFromHost()
{
    ++d->attempt; // a certificate check still running starts nothing
    if (d->process)
        d->process->terminate();
}

void RdpView::sendCtrlAltDel()
{
    // Nothing to do: xfreerdp has no external command for SAS (inside the session Ctrl+Alt+End sends it).
}

void RdpView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e); // the xfreerdp child window follows with /dynamic-resolution
}
#endif

// The card (reason and Reconnect) is shown from a disconnection until the next connection attempt.
void RdpView::updateCard()
{
    const bool show = !connected && !reason.isEmpty();
#ifdef Q_OS_WIN
    // The control is a native child window, always above what Qt draws: hidden while the card is shown.
    if (d->host && d->embedded)
        ShowWindow(d->host, show ? SW_HIDE : SW_SHOWNA);
#elif defined(Q_OS_MACOS)
    d->label->setVisible(!show);
#endif
    if (show)
        card->present(reason, destination(settings), true);
    else
        card->hide();
}

void RdpView::keyPressEvent(QKeyEvent *e)
{
    if (card->offersReconnect() && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter))
        connectToHost();
    else
        QWidget::keyPressEvent(e);
}

void rdpCertificateFingerprint(const QString &host, quint16 port, QObject *context,
                               std::function<void(const QString &fingerprint)> done)
{
    auto *socket = new QSslSocket(context);
    // Reports once, outside the socket's own signals (the caller may open a dialog), and drops the connection.
    const auto report = std::make_shared<std::function<void(const QString &)>>(std::move(done));
    const auto finish = [socket, context, report](const QString &print)
    {
        if (!*report)
            return;
        const auto call = *report;
        *report = nullptr;
        socket->abort();
        socket->deleteLater();
        QTimer::singleShot(0, context, [call, print] { call(print); });
    };
    auto configuration = socket->sslConfiguration();
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone); // nothing is sent: the certificate is only read
    configuration.setProtocol(QSsl::AnyProtocol);
    socket->setSslConfiguration(configuration);
    QObject::connect(socket, &QSslSocket::connected, socket,
                     [socket]
                     {
                         // X.224 Connection Request with an RDP negotiation request for TLS or NLA (both start with TLS).
                         static const char request[] =
                             "\x03\x00\x00\x13\x0e\xe0\x00\x00\x00\x00\x00\x01\x00\x08\x00\x03\x00\x00\x00";
                         socket->write(request, 19);
                     });
    QObject::connect(socket, &QSslSocket::readyRead, socket,
                     [socket, finish]
                     {
                         if (socket->mode() != QSslSocket::UnencryptedMode)
                             return;
                         const QByteArray head = socket->peek(4);
                         if (head.size() < 4)
                             return;
                         const int length = (uchar(head[2]) << 8) | uchar(head[3]);
                         if (head[0] != 3 || length < 11 || length > 512)
                             return finish({});
                         if (socket->bytesAvailable() < length)
                             return;
                         // Connection Confirm: a negotiation response (type 2) selecting TLS (1), NLA (2), RDSTLS (4) or NLA
                         // extended (8); anything else is a server without TLS.
                         const QByteArray reply = socket->read(length);
                         if (length < 19 || reply[11] != 2 || !(uchar(reply[15]) & 0x0f))
                             return finish({});
                         socket->startClientEncryption();
                     });
    QObject::connect(socket, &QSslSocket::encrypted, socket,
                     [socket, finish]
                     {
                         const auto certificate = socket->peerCertificate();
                         finish(certificate.isNull() ? QString() : FtpClient::fingerprint(certificate));
                     });
    QObject::connect(socket, &QSslSocket::errorOccurred, socket, [finish] { finish({}); });
    QObject::connect(socket, &QSslSocket::disconnected, socket, [finish] { finish({}); });
    QTimer::singleShot(10000, socket, [finish] { finish({}); });
    socket->connectToHost(host, port);
}
