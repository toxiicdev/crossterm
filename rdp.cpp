#include "rdp.h"
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QResizeEvent>
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
#include <QDesktopServices>
#include <QLabel>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#endif

static QString ui(const char *text)
{
    return QCoreApplication::translate("CrossTerm", text);
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
    : QWidget(parent), d(new Private), settings(settings), passwordProvider(std::move(passwordProvider))
{
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
            if (connected)
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
    QByteArray prompt; // last bytes printed by xfreerdp, to recognize prompts
};

RdpView::RdpView(const Settings &settings, std::function<QString()> passwordProvider, QWidget *parent)
    : QWidget(parent), d(new Private), settings(settings), passwordProvider(std::move(passwordProvider))
{
#ifdef Q_OS_MACOS
    auto *layout = new QVBoxLayout(this);
    // One literal: i18n/extract.py does not join adjacent string literals.
    auto *label = new QLabel(
        ui("On macOS the Remote Desktop session cannot be embedded: it opens in the external Microsoft Remote Desktop (Windows App) client."),
        this);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
#else
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setStyleSheet("background: black");
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
    const QString destination =
        (settings.host.contains(':') ? "[" + settings.host + "]" : settings.host) + ":" + QString::number(settings.port);
#ifdef Q_OS_MACOS
    // rdp:// scheme of the Microsoft client; the password is not passed.
    QString url = "rdp://full%20address=s:" + destination;
    if (!settings.user.isEmpty())
        url += "&username=s:" + QUrl::toPercentEncoding(settings.user);
    if (QDesktopServices::openUrl(QUrl(url, QUrl::StrictMode)))
        connectedEvent();
    else
        disconnectedEvent(ui("No Remote Desktop client is installed"));
#else
    QString program = QStandardPaths::findExecutable("xfreerdp");
    if (program.isEmpty())
        program = QStandardPaths::findExecutable("xfreerdp3");
    if (program.isEmpty())
    {
        disconnectedEvent(ui("xfreerdp was not found: install FreeRDP"));
        return;
    }
    QStringList args{"/parent-window:" + QString::number(winId()),
                     "/v:" + destination,
                     "/from-stdin:force",
                     "/cert:tofu",
                     "/dynamic-resolution",
                     "/bpp:" + QString::number(settings.colorDepth)};
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
        args << "/home-drive"; // ponytail: home folder only; user-chosen folders if needed
    if (settings.redirectPrinters)
        args << "/printer";
    d->process = new QProcess(this);
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
#endif
}

void RdpView::disconnectFromHost()
{
    if (d->process)
        d->process->terminate();
}

void RdpView::sendCtrlAltDel()
{
    // ponytail: xfreerdp has no external command for SAS; inside the session use Ctrl+Alt+End.
}

void RdpView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e); // the xfreerdp child window follows with /dynamic-resolution
}
#endif
