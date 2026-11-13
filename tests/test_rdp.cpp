// Remote Desktop ActiveX control: creation, properties via IDispatch, connection to a closed port on 127.0.0.1
// (no real RDP server), repeated destruction.
#include "rdp.h"
#include <QApplication>
#include <QDebug>
#include <QDeadlineTimer>
#include <QTcpServer>
#include <QThread>
#include <functional>

#define CHECK(expression)                                                                                                        \
    if (!(expression))                                                                                                           \
    {                                                                                                                            \
        qCritical() << "line" << __LINE__ << #expression;                                                                        \
        return 1;                                                                                                                \
    }

static bool waitFor(const std::function<bool()> &condition, int ms)
{
    QDeadlineTimer deadline(ms);
    while (!condition())
    {
        if (deadline.hasExpired())
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return true;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
#ifndef Q_OS_WIN
    qInfo("SKIPPED: the RDP ActiveX control exists only on Windows");
    return 0;
#else
    // Port guaranteed closed: opened and immediately released.
    quint16 closedPort = 0;
    {
        QTcpServer probe;
        CHECK(probe.listen(QHostAddress::LocalHost, 0))
        closedPort = probe.serverPort();
    }
    for (int round = 0; round < 5; ++round)
    {
        RdpView::Settings settings;
        settings.host = "127.0.0.1";
        settings.port = closedPort;
        settings.user = "crossterm-test";
        int passwordRequests = 0;
        auto *view = new RdpView(settings, [&] { return ++passwordRequests, QString("not-a-real-password"); });
        view->resize(800, 600);
        view->show();
        CHECK(view->controlCreated())
        CHECK(!view->isConnected())
        if (round == 0)
        {
            CHECK(view->setControlProperty("Server", "example.invalid"))
            CHECK(view->controlProperty("Server").toString() == "example.invalid")
            CHECK(view->setControlProperty("DesktopWidth", 1024))
            CHECK(view->controlProperty("DesktopWidth").toInt() == 1024)
            CHECK(view->setControlProperty("AdvancedSettings.RDPPort", 3390))
            CHECK(view->controlProperty("AdvancedSettings.RDPPort").toInt() == 3390)
            CHECK(view->setControlProperty("AdvancedSettings.SmartSizing", true))
            CHECK(view->controlProperty("AdvancedSettings.SmartSizing").toBool())
            CHECK(!view->setControlProperty("NoSuchProperty", 1))
            view->sendCtrlAltDel(); // no session: no effect, no crash
            view->setFullScreen(false);
            view->resize(1024, 700);
            qInfo() << "control version:" << view->controlProperty("Version").toString();
        }
        if (round < 2)
        {
            QString reason;
            bool connected = false;
            view->onConnected = [&] { connected = true; };
            view->onDisconnected = [&](const QString &text) { reason = text; };
            view->connectToHost();
            CHECK(passwordRequests == 1)
            CHECK(waitFor([&] { return !reason.isEmpty(); }, 60000))
            qInfo() << "disconnect reason:" << reason;
            CHECK(!connected)
            CHECK(!view->isConnected())
            CHECK(view->lastReason() == reason)
            CHECK(view->controlProperty("Server").toString() == "127.0.0.1")
            CHECK(view->controlProperty("AdvancedSettings.RDPPort").toInt() == closedPort)
        }
        delete view;
        QCoreApplication::processEvents();
    }
    qInfo("rdp: OK");
    return 0;
#endif
}
