#pragma once
// Linux: no system title bar, CrossTerm draws close, minimize and full screen at the top left.
// The empty area of the menu bar or toolbar moves the window.
// macOS keeps the native window and its buttons, with a transparent title bar in the window color.
// Linux uses a frameless window, moved and resized through the window manager.
#include <QtWidgets>
#ifdef Q_OS_MACOS
#include <objc/message.h>
#include <objc/runtime.h>
#endif

class WindowButtons : public QWidget
{
    static constexpr int D = 12, Gap = 8, Margin = 10;
    int hover = -1;
    int pressed = -1; // index of the button held down, -1 when none

    int at(QPoint p) const
    {
        for (int i = 0; i < 3; ++i)
            if (circle(i).adjusted(-2, -2, 2, 2).contains(p))
                return i;
        return -1;
    }
    QRect circle(int i) const { return QRect(Margin + i * (D + Gap), (height() - D) / 2, D, D); }

public:
    explicit WindowButtons(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName("windowButtons");
        setMouseTracking(true);
        setFixedWidth(2 * Margin + 3 * D + 2 * Gap);
        setMinimumHeight(D + 8);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    }
    // 0 close, 1 minimize, 2 full screen
    void trigger(int i)
    {
        auto *w = window();
        if (i == 0)
            w->close();
        else if (i == 1)
            w->showMinimized();
        else if (i == 2)
            w->isFullScreen() ? w->showNormal() : w->showFullScreen();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool active = window()->isActiveWindow() || hover >= 0;
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        static const QColor fill[3] = {QColor("#ff5f57"), QColor("#febc2e"), QColor("#28c840")};
        static const QColor edge[3] = {QColor("#e0443e"), QColor("#dea123"), QColor("#1aab29")};
        for (int i = 0; i < 3; ++i)
        {
            const QRectF r = QRectF(circle(i)).adjusted(0.5, 0.5, -0.5, -0.5);
            const bool down = pressed == i;
            p.setPen(QPen(active ? edge[i] : (dark ? QColor("#3a3a3a") : QColor("#c8c8c8")), 1));
            p.setBrush(!active ? (dark ? QColor("#4a4a4a") : QColor("#dcdcdc")) : down ? fill[i].darker(115) : fill[i]);
            p.drawEllipse(r);
            if (hover < 0)
                continue;
            // Glyphs appear while the pointer is over the group, as on macOS.
            p.setPen(QPen(QColor(0, 0, 0, down ? 170 : 130), 1.3, Qt::SolidLine, Qt::RoundCap));
            const QPointF c = r.center();
            const qreal k = 2.6;
            if (i == 0)
            {
                p.drawLine(c + QPointF(-k, -k), c + QPointF(k, k));
                p.drawLine(c + QPointF(-k, k), c + QPointF(k, -k));
            }
            else if (i == 1)
                p.drawLine(c + QPointF(-k - 0.6, 0), c + QPointF(k + 0.6, 0));
            else
            {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, down ? 170 : 130));
                const qreal s = 3.4;
                p.drawPolygon(QPolygonF{c + QPointF(-s, -s), c + QPointF(s * 0.55, -s), c + QPointF(-s, s * 0.55)});
                p.drawPolygon(QPolygonF{c + QPointF(s, s), c + QPointF(-s * 0.55, s), c + QPointF(s, -s * 0.55)});
            }
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const int h = at(e->position().toPoint()) >= 0 ? 0 : -1;
        if (h != hover)
        {
            hover = h;
            update();
        }
    }
    void leaveEvent(QEvent *) override
    {
        hover = -1;
        pressed = -1;
        update();
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        pressed = e->button() == Qt::LeftButton ? at(e->position().toPoint()) : -1;
        update();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        const int was = pressed;
        pressed = -1;
        update();
        // Only the button that was pressed fires, and only when released over it.
        if (was >= 0 && e->button() == Qt::LeftButton && at(e->position().toPoint()) == was)
            trigger(was);
    }
    void changeEvent(QEvent *e) override
    {
        if (e->type() == QEvent::ActivationChange || e->type() == QEvent::PaletteChange)
            update();
        QWidget::changeEvent(e);
    }
};

// Drag on empty bar areas moves the window, double click maximizes; on Linux the window border resizes it.
class TitleBarFilter : public QObject
{
    QMainWindow *main;
    static constexpr int Border = 5;
#ifdef Q_OS_LINUX
    Qt::Edges cursorEdges; // edges the resize cursor is currently shown for
#endif

    Qt::Edges edgesAt(QPoint p) const
    {
        Qt::Edges e;
        if (main->isMaximized() || main->isFullScreen())
            return e;
        if (p.x() < Border)
            e |= Qt::LeftEdge;
        if (p.x() >= main->width() - Border)
            e |= Qt::RightEdge;
        if (p.y() < Border)
            e |= Qt::TopEdge;
        if (p.y() >= main->height() - Border)
            e |= Qt::BottomEdge;
        return e;
    }

public:
    explicit TitleBarFilter(QMainWindow *w) : QObject(w), main(w) {}

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const auto type = event->type();
        if (watched == main)
        {
            if (type == QEvent::WindowStateChange)
            {
                const bool plain = !(main->isMaximized() || main->isFullScreen());
#ifdef Q_OS_LINUX
                main->setContentsMargins(plain ? QMargins(Border, Border, Border, Border) : QMargins());
#endif
                Q_UNUSED(plain);
                transparentTitleBar(main);
            }
#ifdef Q_OS_LINUX
            if (type == QEvent::MouseMove || type == QEvent::HoverMove)
            {
                const auto e = edgesAt(main->mapFromGlobal(QCursor::pos()));
                if (e != cursorEdges) // the cursor changes only when the pointer reaches or leaves an edge
                {
                    cursorEdges = e;
                    const bool diag = (e & Qt::LeftEdge && e & Qt::TopEdge) || (e & Qt::RightEdge && e & Qt::BottomEdge);
                    const bool anti = (e & Qt::RightEdge && e & Qt::TopEdge) || (e & Qt::LeftEdge && e & Qt::BottomEdge);
                    if (diag)
                        main->setCursor(Qt::SizeFDiagCursor);
                    else if (anti)
                        main->setCursor(Qt::SizeBDiagCursor);
                    else if (e & (Qt::LeftEdge | Qt::RightEdge))
                        main->setCursor(Qt::SizeHorCursor);
                    else if (e)
                        main->setCursor(Qt::SizeVerCursor);
                    else
                        main->unsetCursor();
                }
            }
            if (type == QEvent::MouseButtonPress && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
            {
                const auto e = edgesAt(static_cast<QMouseEvent *>(event)->position().toPoint());
                if (e && main->windowHandle())
                    return main->windowHandle()->startSystemResize(e);
            }
#endif
            return false;
        }
        auto *bar = qobject_cast<QWidget *>(watched);
        if (!bar || (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick))
            return false;
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton)
            return false;
        if (auto *menu = qobject_cast<QMenuBar *>(bar); menu && menu->actionAt(mouse->position().toPoint()))
            return false;
        if (type == QEvent::MouseButtonDblClick)
        {
            main->isMaximized() ? main->showNormal() : main->showMaximized();
            return true;
        }
        return main->windowHandle() && main->windowHandle()->startSystemMove();
    }

    // macOS: transparent, empty title bar in the window color; the system buttons stay.
    static void transparentTitleBar(QWidget *w)
    {
#ifdef Q_OS_MACOS
        using Get = void *(*)(void *, SEL);
        using GetMask = unsigned long (*)(void *, SEL);
        using SetMask = void (*)(void *, SEL, unsigned long);
        using SetBool = void (*)(void *, SEL, BOOL);
        using SetLong = void (*)(void *, SEL, long);
        void *nsWindow = reinterpret_cast<Get>(objc_msgSend)(reinterpret_cast<void *>(w->winId()), sel_registerName("window"));
        if (!nsWindow)
            return;
        const unsigned long fullSizeContentView = 1UL << 15;
        const unsigned long mask = reinterpret_cast<GetMask>(objc_msgSend)(nsWindow, sel_registerName("styleMask"));
        if (!(mask & fullSizeContentView))
            reinterpret_cast<SetMask>(objc_msgSend)(nsWindow, sel_registerName("setStyleMask:"), mask | fullSizeContentView);
        reinterpret_cast<SetBool>(objc_msgSend)(nsWindow, sel_registerName("setTitlebarAppearsTransparent:"), YES);
        reinterpret_cast<SetLong>(objc_msgSend)(nsWindow, sel_registerName("setTitleVisibility:"), 1); // hidden
#else
        Q_UNUSED(w);
#endif
    }
};

// Installs the custom title bar. Linux: buttons at the left of the in-window menu bar, or of the toolbar when the
// menu bar is global. macOS: transparent title bar only. Windows keeps its system title bar.
inline WindowButtons *installTitleBar(QMainWindow *main, QToolBar *tools)
{
#ifdef Q_OS_WIN
    Q_UNUSED(main);
    Q_UNUSED(tools);
    return nullptr;
#else
    if (QGuiApplication::platformName() == "offscreen")
        return nullptr;
    auto *filter = new TitleBarFilter(main);
    auto *menu = main->menuBar();
    for (QWidget *bar : {static_cast<QWidget *>(menu), static_cast<QWidget *>(tools)})
        bar->installEventFilter(filter);
    main->installEventFilter(filter);
#ifdef Q_OS_MACOS
    TitleBarFilter::transparentTitleBar(main);
    return nullptr;
#else
    auto *buttons = new WindowButtons;
    if (menu->isNativeMenuBar())
        tools->insertWidget(tools->actions().value(0), buttons);
    else
        menu->setCornerWidget(buttons, Qt::TopLeftCorner);
    main->setWindowFlag(Qt::FramelessWindowHint);
    main->setMouseTracking(true);
    main->setAttribute(Qt::WA_Hover);
    main->setContentsMargins(5, 5, 5, 5);
    return buttons;
#endif
#endif
}
