// SPDX-License-Identifier: GPL-2.0-or-later
// LinuxRunner local Plasma capture, EIS input and authenticated RFB bridge.
#include "lr-plasma-krfb.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusUnixFileDescriptor>
#include <QDebug>
#include <QSocketNotifier>
#include <algorithm>
#include <libei.h>
#include <linux/input-event-codes.h>
#include <unistd.h>

class LREisHandler final : public EventHandler {
    ei *context = nullptr;
    ei_device *pointer = nullptr, *text = nullptr;
    QSocketNotifier *notifier = nullptr;
    int previousButtons = 0;
    uint32_t sequence = 0;
    int cookie = 0;
    bool connected = false, keyboardWarned = false, mappingWarned = false;
    void clear(ei_device *device) {
        if (pointer == device) pointer = ei_device_unref(pointer);
        if (text == device) text = ei_device_unref(text);
    }
    void dispatch() {
        ei_dispatch(context);
        while (auto *event = ei_get_event(context)) {
            auto *device = ei_event_get_device(event);
            switch (ei_event_get_type(event)) {
            case EI_EVENT_CONNECT:
                connected = true;
                qInfo("LR_KWIN_EIS: connected");
                break;
            case EI_EVENT_SEAT_ADDED:
                ei_seat_bind_capabilities(ei_event_get_seat(event),
                    EI_DEVICE_CAP_POINTER_ABSOLUTE, EI_DEVICE_CAP_BUTTON,
                    EI_DEVICE_CAP_SCROLL, EI_DEVICE_CAP_KEYBOARD, EI_DEVICE_CAP_TEXT, nullptr);
                break;
            case EI_EVENT_DEVICE_RESUMED:
                ei_device_start_emulating(device, ++sequence);
                if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
                    if (pointer) ei_device_unref(pointer);
                    pointer = ei_device_ref(device);
                    previousButtons = 0;
                    mappingWarned = false;
                    qInfo("LR_KWIN_EIS: absolute pointer ready");
                }
                if (ei_device_has_capability(device, EI_DEVICE_CAP_TEXT)) {
                    if (text) ei_device_unref(text);
                    text = ei_device_ref(device);
                    keyboardWarned = false;
                    qInfo("LR_KWIN_EIS: keysym input ready");
                }
                break;
            case EI_EVENT_DEVICE_PAUSED:
            case EI_EVENT_DEVICE_REMOVED:
                clear(device);
                break;
            case EI_EVENT_DISCONNECT:
                connected = false;
                if (pointer) clear(pointer);
                if (text) clear(text);
                notifier->setEnabled(false);
                qWarning("LR_KWIN_EIS: compositor input disconnected");
                break;
            default: break;
            }
            ei_event_unref(event);
        }
    }
public:
    LREisHandler() {
        auto call = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
            QStringLiteral("/org/kde/KWin/EIS/RemoteDesktop"),
            QStringLiteral("org.kde.KWin.EIS.RemoteDesktop"), QStringLiteral("connectToEIS"));
        call << 3; // KWin's keyboard + pointer capabilities, not device access.
        auto reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 5000);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 2) {
            qWarning() << "LR_KWIN_EIS: connect failed" << reply.errorName();
            return;
        }
        auto fd = qvariant_cast<QDBusUnixFileDescriptor>(reply.arguments()[0]);
        cookie = reply.arguments()[1].toInt();
        int owned = fd.isValid() ? dup(fd.fileDescriptor()) : -1;
        if (owned < 0) return;
        context = ei_new_sender(this);
        if (!context) { close(owned); return; }
        ei_configure_name(context, "LinuxRunner desktop controller");
        if (ei_setup_backend_fd(context, owned) != 0) {
            qWarning("LR_KWIN_EIS: backend setup failed");
            return;
        }
        notifier = new QSocketNotifier(ei_get_fd(context), QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this] { dispatch(); });
        dispatch();
    }
    bool valid() const { return notifier != nullptr; }
    ~LREisHandler() override {
        if (notifier) notifier->setEnabled(false);
        if (pointer) ei_device_unref(pointer);
        if (text) ei_device_unref(text);
        if (context) ei_unref(context);
        if (cookie) {
            auto call = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
                QStringLiteral("/org/kde/KWin/EIS/RemoteDesktop"),
                QStringLiteral("org.kde.KWin.EIS.RemoteDesktop"), QStringLiteral("disconnect"));
            call << cookie;
            QDBusConnection::sessionBus().asyncCall(call);
        }
    }
    void handleKeyboard(bool down, rfbKeySym key) override {
        if (!connected || !text) {
            if (!keyboardWarned) qWarning("LR_KWIN_EIS: keyboard not ready");
            keyboardWarned = true; return;
        }
        ei_device_text_keysym(text, key, down);
        ei_device_frame(text, ei_now(context));
    }
    // Frame-paced pointer motion: a viewer sends up to ~60 motions/s, and each
    // one makes KWin and the Plasma shell redo hover handling in software.
    // Motion is held and only the newest position is sent, once per captured
    // frame (or every 120 ms), so the desktop renders the current position
    // instead of working through a backlog. Button and wheel changes are sent
    // at once, preceded by the newest position.
    bool motionPending = false; int pendingX = 0, pendingY = 0;
    qint64 lastMotionMs = 0;
    void handlePointer(int buttons, int x, int y) override {
        if (!connected || !pointer) return;
        if (buttons == previousButtons) { motionPending = true; pendingX = x; pendingY = y; return; }
        motionPending = false;
        sendPointer(buttons, x, y);
    }
    void flushMotion(bool framePresented, qint64 nowMs) {
        if (!motionPending || !connected || !pointer) return;
        if (!framePresented && nowMs - lastMotionMs < 120) return;
        motionPending = false; lastMotionMs = nowMs;
        sendPointer(previousButtons, pendingX, pendingY);
    }
    void sendPointer(int buttons, int x, int y) {
        // EIS coordinates use the compositor's logical desktop, while RFB uses
        // pixels of one monitor. Match its actual KWin output name when given.
        auto *region = ei_device_get_region(pointer, 0);
        const QByteArray output = qgetenv("LR_KWIN_EIS_OUTPUT");
        if (!output.isEmpty()) {
            region = nullptr;
            for (size_t i=0; auto *candidate = ei_device_get_region(pointer, i); ++i) {
                const char *id = ei_region_get_mapping_id(candidate);
                if (id && output == id) { region = candidate; break; }
            }
        } else if (ei_device_get_region(pointer, 1)) region = nullptr;
        if (!region) {
            if (!mappingWarned) qWarning("LR_KWIN_EIS: no unambiguous monitor mapping");
            mappingWarned = true; return;
        }
        const auto fb = frameBuffer();
        if (!fb || fb->width() <= 0 || fb->height() <= 0) return;
        const double px = ei_region_get_x(region) + std::clamp(x, 0, fb->width()-1) * double(ei_region_get_width(region)) / fb->width();
        const double py = ei_region_get_y(region) + std::clamp(y, 0, fb->height()-1) * double(ei_region_get_height(region)) / fb->height();
        ei_device_pointer_motion_absolute(pointer, px, py);
        const uint32_t codes[] = {BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, 0, 0, 0, 0, BTN_SIDE, BTN_EXTRA};
        const int changed = buttons ^ previousButtons;
        for (unsigned i=0; i<9; i++) if (changed & (1 << i)) {
            const bool down = buttons & (1 << i);
            if (codes[i]) ei_device_button_button(pointer, codes[i], down);
            else if (down) {
                const int step = (i == 3 || i == 5) ? -120 : 120;
                ei_device_scroll_discrete(pointer, i >= 5 ? step : 0, i < 5 ? step : 0);
            }
        }
        previousButtons = buttons;
        ei_device_frame(pointer, ei_now(context));
    }
};


#include <KPluginFactory>
#include <KPluginMetaData>
#include <memory>
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <cstring>
#include <vector>
#include <QDateTime>
#include <arpa/inet.h>
#include <signal.h>

static QSharedPointer<FrameBuffer> framebuffer;
static volatile sig_atomic_t stopping;
static void requestStop(int) { stopping = 1; }
static void keyboard(rfbBool down, rfbKeySym key, rfbClientPtr client) {
    static_cast<LREisHandler *>(client->clientData)->handleKeyboard(down, key);
}
static void pointer(int buttons, int x, int y, rfbClientPtr client) {
    static_cast<LREisHandler *>(client->clientData)->handlePointer(buttons, x, y);
    rfbDefaultPtrAddEvent(buttons, x, y, client);
}
static rfbNewClientAction clientConnected(rfbClientPtr client) {
    auto *input = new LREisHandler;
    if (!input->valid()) { delete input; return RFB_CLIENT_REFUSE; }
    input->setFrameBufferPlugin(framebuffer);
    client->clientData = input;
    client->clientGoneHook = [](rfbClientPtr c) { delete static_cast<LREisHandler *>(c->clientData); c->clientData = nullptr; };
    return RFB_CLIENT_ACCEPT;
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName("linuxrunner-plasma-bridge");
    app.setDesktopFileName("org.linuxrunner.plasma-bridge");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption(QCommandLineOption(QStringLiteral("name"), QStringLiteral("Actual KWin virtual monitor name"), QStringLiteral("name"), QStringLiteral("LinuxRunner")));
    parser.addOption(QCommandLineOption(QStringLiteral("resolution"), QStringLiteral("Landscape framebuffer size"), QStringLiteral("WxH"), QStringLiteral("1280x800")));
    parser.addOption(QCommandLineOption(QStringLiteral("port"), QStringLiteral("Loopback RFB port"), QStringLiteral("port")));
    parser.addOption(QCommandLineOption(QStringLiteral("password-file"), QStringLiteral("Private eight-byte session password file"), QStringLiteral("path")));
    parser.process(app);
    const auto dimensions = parser.value("resolution").split('x');
    bool portOk=false, widthOk=false, heightOk=false;
    const int port=parser.value("port").toInt(&portOk);
    const int width=dimensions.value(0).toInt(&widthOk), height=dimensions.value(1).toInt(&heightOk);
    if (getuid()==0 || dimensions.size()!=2 || !widthOk || !heightOk || width<320 || height<240 || width<height || width>2560 || height>1600 || !portOk || port<1024 || port>65535) {
        qCritical("PLASMA_BRIDGE: invalid identity, landscape geometry or port"); return 2;
    }
    QFile passwordFile(parser.value("password-file"));
    const QFileInfo passwordInfo(passwordFile);
    const auto unsafe=QFileDevice::ReadGroup|QFileDevice::WriteGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::WriteOther|QFileDevice::ExeOther;
    if (passwordInfo.isSymLink() || passwordInfo.ownerId()!=getuid() || (passwordInfo.permissions()&unsafe) || !passwordFile.open(QIODevice::ReadOnly)) {
        qCritical("PLASMA_BRIDGE: session password file must be private and owned by this user"); return 2;
    }
    QByteArray password=passwordFile.read(9);
    if (password.size()!=8 || password.contains('\0')) { qCritical("PLASMA_BRIDGE: expected eight-byte session password"); return 2; }
    qputenv("LR_KWIN_EIS_OUTPUT", QByteArray("Virtual-")+parser.value("name").toUtf8());
    QVariantMap parameters{{"name",parser.value("name")},{"resolution",QSize(width,height)},{"scale",1.0}};
    const auto metadata=KPluginMetaData::findPluginById(QStringLiteral("krfb/framebuffer"),QStringLiteral("pw"));
    const auto loaded=KPluginFactory::instantiatePlugin<FrameBufferPlugin>(metadata);
    std::unique_ptr<FrameBufferPlugin> capture(loaded.plugin);
    if (!capture) { qCritical() << "PLASMA_BRIDGE: capture plugin load failed" << loaded.errorString; return 3; }
    framebuffer.reset(capture->frameBuffer(parameters));
    if (!framebuffer) { qCritical("PLASMA_BRIDGE: capture plugin unavailable"); return 3; }
    rfbScreenInfoPtr server=nullptr;
    QTimer updates;
    bool framePresented=false;
    QObject::connect(&updates,&QTimer::timeout,&app,[&] {
        if (!server) return;
        const auto rectangles=framebuffer->modifiedTiles();
        {
            const qint64 nowMs=QDateTime::currentMSecsSinceEpoch();
            auto iterator=rfbGetClientIterator(server);
            while (auto *client=rfbClientIteratorNext(iterator))
                if (client->clientData) static_cast<LREisHandler *>(client->clientData)->flushMotion(!rectangles.isEmpty(), nowMs);
            rfbReleaseClientIterator(iterator);
        }
        if(!rectangles.isEmpty()&&!framePresented){framePresented=true;qInfo("PLASMA_BRIDGE: frame-presented");}
        // Capture rate, once a minute, for diagnosing smoothness reports.
        static int capturedFrames=0; static qint64 rateSince=QDateTime::currentMSecsSinceEpoch();
        if(!rectangles.isEmpty())capturedFrames++;
        if(QDateTime::currentMSecsSinceEpoch()-rateSince>=60000){qInfo("PLASMA_BRIDGE: captured-frames=%d per-minute",capturedFrames);capturedFrames=0;rateSince=QDateTime::currentMSecsSinceEpoch();}
        // The capture reports the whole output as modified for every frame.
        // Encoding a full frame per update made small changes (a hover
        // highlight, a blinking cursor) cost a full-screen encode. Compare the
        // new frame with the last one sent and mark only the tiles that differ.
        if (!rectangles.isEmpty()) {
            static std::vector<char> previous;
            const char *current=framebuffer->data();
            const int stride=framebuffer->paddedWidth(), bytesPerPixel=std::max(1,framebuffer->depth()/8);
            const int w=server->width, h=server->height;
            const size_t size=size_t(stride)*h;
            if (!current || stride<w*bytesPerPixel) {
                rfbMarkRectAsModified(server,0,0,w,h);
            } else if (previous.size()!=size) {
                previous.assign(current,current+size);
                rfbMarkRectAsModified(server,0,0,w,h);
            } else {
                constexpr int tileW=64, tileH=32;
                for (int ty=0; ty<h; ty+=tileH) for (int tx=0; tx<w; tx+=tileW) {
                    const int tw=std::min(tileW,w-tx), th=std::min(tileH,h-ty);
                    bool differs=false;
                    for (int y=ty; y<ty+th && !differs; y++)
                        differs=memcmp(current+size_t(y)*stride+size_t(tx)*bytesPerPixel,previous.data()+size_t(y)*stride+size_t(tx)*bytesPerPixel,size_t(tw)*bytesPerPixel)!=0;
                    if (!differs) continue;
                    for (int y=ty; y<ty+th; y++)
                        memcpy(previous.data()+size_t(y)*stride+size_t(tx)*bytesPerPixel,current+size_t(y)*stride+size_t(tx)*bytesPerPixel,size_t(tw)*bytesPerPixel);
                    rfbMarkRectAsModified(server,tx,ty,tx+tw,ty+th);
                }
            }
        }
        // Forward the pointer position only when it moved: re-sending it on
        // every tick kept the viewer and this process busy while idle.
        const QPoint cursor=framebuffer->cursorPosition();
        static QPoint sentCursor(-1,-1);
        if (cursor!=sentCursor && cursor.x()>=0 && cursor.y()>=0 && cursor.x()<server->width && cursor.y()<server->height)
            {
            sentCursor=cursor;
            auto iterator=rfbGetClientIterator(server);
            if (auto *client=rfbClientIteratorNext(iterator))
                rfbDefaultPtrAddEvent(0,cursor.x(),cursor.y(),client);
            rfbReleaseClientIterator(iterator);
        }
        rfbProcessEvents(server,0);
    });
    auto ready=[&] {
        if (!framebuffer->data() || framebuffer->width()<=0 || framebuffer->height()<=0) return;
        if (server) {
            rfbNewFramebuffer(server,framebuffer->data(),framebuffer->width(),framebuffer->height(),8,3,4);
            return;
        }
        server=rfbGetScreen(nullptr,nullptr,framebuffer->width(),framebuffer->height(),8,3,4);
        if (!server) { app.exit(4); return; }
        server->frameBuffer=framebuffer->data();
        server->paddedWidthInBytes=framebuffer->paddedWidth();
        framebuffer->getServerFormat(server->serverFormat);
        server->desktopName="LinuxRunner Plasma";
        server->port=port; server->ipv6port=-1; server->listenInterface=htonl(INADDR_LOOPBACK);
        static char *passwords[2]; passwords[0]=password.data(); passwords[1]=nullptr;
        server->authPasswdData=passwords; server->passwordCheck=rfbCheckPasswordByList;
        server->newClientHook=clientConnected; server->kbdAddEvent=keyboard; server->ptrAddEvent=pointer;
        rfbInitServer(server);
        if (!rfbIsActive(server)) { qCritical("PLASMA_BRIDGE: listener failed"); app.exit(4); return; }
        qInfo("PLASMA_BRIDGE: framebuffer-ready=%dx%d listener=loopback input=EIS",server->width,server->height);
        updates.start(25);
    };
    QObject::connect(framebuffer.data(),&FrameBuffer::frameBufferChanged,&app,ready);
    QTimer::singleShot(60000,&app,[&] { if(!server) { qCritical("PLASMA_BRIDGE: no real framebuffer before startup deadline"); app.exit(5); } });
    signal(SIGPIPE,SIG_IGN);
    signal(SIGTERM,requestStop); signal(SIGINT,requestStop);
    QTimer stopPoll;
    QObject::connect(&stopPoll,&QTimer::timeout,&app,[&] { if(stopping) app.quit(); });
    stopPoll.start(100);
    framebuffer->startMonitor();
    ready();
    const int result=app.exec();
    updates.stop();
    if(server) { rfbShutdownServer(server,true); rfbScreenCleanup(server); }
    framebuffer->stopMonitor(); framebuffer.clear();
    return result;
}
