// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// Interactive emulation of a head-unit page, rendering through QQuickRenderControl
// instead of a window: no platform exposure is needed, so the frames are real even
// under the offscreen/minimal platform plugin.  The HTTP side streams frames and
// feeds clicks back into the same scene.
#include <QGuiApplication>
#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QSGRendererInterface>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#endif
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <cstdio>

static QQuickRenderControl *g_rc = nullptr;
static QQuickWindow *g_win = nullptr;
static QQuickItem *g_rootItem = nullptr;
static QSize g_size(1280, 720);

static void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

/* One frame, rendered synchronously: polish, sync, render, then read back. */
static QImage renderFrame()
{
    if (!g_rc || !g_win) return QImage();
    g_rc->polishItems();
    g_rc->sync();
    g_rc->render();
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    /* Qt 5 renders into an FBO and hands the pixels over directly; this is the
     * path that works headless, unlike QQuickWindow::grabWindow() on the
     * offscreen platform plugin of Qt 6. */
    return g_rc->grab();
#else
    fprintf(stderr, "this build needs Qt 5 for the render-control read-back\n");
    return QImage();
#endif
}

static QByteArray g_lastPng;

static QByteArray grabPng()
{
    QImage img = renderFrame();
    if (img.isNull()) return QByteArray();
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return png;
}

static const char *kIndexHtml = R"HTML(<!doctype html><meta charset=utf-8>
<title>SCOURGE UI emulator</title>
<style>body{margin:0;background:#111;color:#ddd;font:14px sans-serif}
#c{display:block;cursor:crosshair}#hud{padding:6px 10px}</style>
<div id=hud>Tap inside the picture. Tabs, buttons and the keypad work.
<span id=st>ready</span></div>
<canvas id=c width=1280 height=720></canvas>
<script>
const c=document.getElementById('c'),x=c.getContext('2d'),st=document.getElementById('st');
let busy=false;
async function frame(){
  if(busy) return; busy=true;
  try{const r=await fetch('/frame.png?t='+Date.now()); if(r.ok){
    const b=await r.blob(),u=URL.createObjectURL(b),i=new Image();
    i.onload=()=>{x.drawImage(i,0,0,c.width,c.height);URL.revokeObjectURL(u);};i.src=u;}}catch(e){}
  busy=false;
}
setInterval(frame,350);frame();
c.addEventListener('mousedown',async ev=>{
  const r=c.getBoundingClientRect();
  const px=Math.round((ev.clientX-r.left)*c.width/r.width);
  const py=Math.round((ev.clientY-r.top)*c.height/r.height);
  st.textContent='tap '+px+','+py;
  await fetch('/click?x='+px+'&y='+py,{method:'POST'});
  setTimeout(frame,120);
});
window.addEventListener('keydown',async ev=>{
  await fetch('/key?k='+encodeURIComponent(ev.key),{method:'POST'});
  setTimeout(frame,120);
});
</script>)HTML";

static void respond(QTcpSocket *s, const QByteArray &body, const char *type)
{
    QByteArray head = "HTTP/1.1 200 OK\r\nContent-Type: ";
    head += type;
    head += "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nCache-Control: no-store\r\n\r\n";
    s->write(head);
    s->write(body);
    s->disconnectFromHost();
}

static void handle(QTcpSocket *s)
{
    s->waitForReadyRead(150);
    const QByteArray req = s->readAll();
    const QByteArray line = req.left(req.indexOf("\r\n"));
    const QList<QByteArray> parts = line.split(' ');
    if (parts.size() < 2) { s->disconnectFromHost(); return; }
    const QByteArray path = parts.at(1);

    if (path.startsWith("/frame.png")) {
        QByteArray png = grabPng();
        if (!png.isEmpty()) g_lastPng = png;
        if (png.isEmpty()) png = g_lastPng;
        if (png.isEmpty()) { respond(s, "no frame", "text/plain"); return; }
        respond(s, png, "image/png");
        return;
    }
    if (path.startsWith("/click")) {
        int x = 0, y = 0;
        const QByteArray q = path.mid(path.indexOf('?') + 1);
        for (const QByteArray &kv : q.split('&')) {
            const int eq = kv.indexOf('=');
            if (eq < 0) continue;
            if (kv.left(eq) == "x") x = kv.mid(eq + 1).toInt();
            if (kv.left(eq) == "y") y = kv.mid(eq + 1).toInt();
        }
        const QPointF p(x, y);
        QMouseEvent press(QEvent::MouseButtonPress, p, p, p, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(g_win, &press);
        QMouseEvent rel(QEvent::MouseButtonRelease, p, p, p, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(g_win, &rel);
        pump(60);
        respond(s, "ok", "text/plain");
        return;
    }
    if (path.startsWith("/key")) {
        QByteArray k = path.mid(path.indexOf("k=") + 2);
        k = QUrl::fromPercentEncoding(k).toUtf8();
        QKeyEvent ev(QEvent::KeyPress, 0, Qt::NoModifier, QString::fromUtf8(k));
        QCoreApplication::sendEvent(g_win, &ev);
        pump(40);
        respond(s, "ok", "text/plain");
        return;
    }
    respond(s, kIndexHtml, "text/html; charset=utf-8");
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    const QString pageUrl = args.value(1);
    const int width = args.value(2, "1280").toInt();
    const int height = args.value(3, "720").toInt();
    const QString dataDir = args.value(4, "");
    const int port = args.value(5, "8099").toInt();
    const QString probePath = args.value(6, "");   /* optional: render one PNG and exit */
    if (pageUrl.isEmpty()) {
        fprintf(stderr, "usage: qmluiserve <page.qml> [w] [h] [dataDir] [port] [probe.png]\n");
        return 2;
    }
    g_size = QSize(width, height);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
#endif

    QQmlEngine engine;
    engine.addImportPath(dataDir + "/imports");
    QQmlPropertyMap appModel, constModel;
    appModel.insert("isWideScr", width >= 1700);
    appModel.insert("isAtOtherApp", false);
    appModel.insert("sysPopupShowing", false);
    appModel.insert("requestHomeByPP", false);
    appModel.insert("currentScreen", 0);
    appModel.insert("currentHomeAdvancedPageIdx", 0);
    appModel.insert("currentSetupPageIdx", 0);
    appModel.insert("currentMenuPageIdx", 0);
    appModel.insert("currentMenuMediaGatePageIdx", 0);
    appModel.insert("screenNotSupportSS", false);
    appModel.insert("preventScreenAnimation", true);
    appModel.insert("homeMainContentX", 0);
    constModel.insert("MENU_BAR_Y", 60);
    constModel.insert("SCREEN_WIDTH", width);
    constModel.insert("SCREEN_HEIGHT", height);
    engine.rootContext()->setContextProperty("AppHomeModel", &appModel);
    engine.rootContext()->setContextProperty("ConstantModel", &constModel);
    engine.rootContext()->setContextProperty("DEVICE_WIDTH", width);
    engine.rootContext()->setContextProperty("DEVICE_HEIGHT", height);

    QQuickRenderControl rc;
    g_rc = &rc;
    QQuickWindow win(&rc);
    g_win = &win;
    win.setColor(QColor("#000000"));
    win.resize(g_size);

    QQmlComponent component(&engine, QUrl::fromLocalFile(pageUrl));
    QObject *obj = component.create(engine.rootContext());
    if (!obj) {
        fprintf(stderr, "page failed to load\n");
        for (const QQmlError &e : component.errors()) fprintf(stderr, "  %s\n", qPrintable(e.toString()));
        return 3;
    }
    g_rootItem = qobject_cast<QQuickItem *>(obj);
    if (!g_rootItem) { fprintf(stderr, "root is not an item\n"); return 3; }
    g_rootItem->setParentItem(win.contentItem());
    g_rootItem->setSize(QSizeF(width, height));

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QSurfaceFormat fmt;
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8);
    QOpenGLContext glctx;
    glctx.setFormat(fmt);
    if (!glctx.create()) { fprintf(stderr, "no GL context\n"); return 4; }
    QOffscreenSurface surface;
    surface.setFormat(glctx.format());
    surface.create();
    if (!surface.isValid()) { fprintf(stderr, "no offscreen surface\n"); return 4; }
    if (!glctx.makeCurrent(&surface)) { fprintf(stderr, "makeCurrent failed\n"); return 4; }
    rc.initialize(&glctx);   /* Qt 5 returns void; a failed init shows up as a null grab */
#else
    if (!rc.initialize()) { fprintf(stderr, "render control init failed\n"); return 4; }
#endif

    /* Mock scan rows so the list has content without a daemon. */
    QVariantList networks;
    for (int i = 0; i < 8; i++) {
        QVariantMap n;
        const bool five = (i % 3) == 1;
        n.insert("ssid", i == 0 ? "Franci_wifi"
                                : (five ? QString("Franci_wifi-ext_5G_%1").arg(i)
                                        : QString("Neighbour network %1").arg(i)));
        n.insert("signal", -50 - i * 6);
        n.insert("security", i == 2 ? "Open" : "WPA2");
        n.insert("freq", five ? 5180 + i * 20 : 2412 + i * 5);
        n.insert("band", five ? "5" : "2.4");
        n.insert("channel", five ? 36 + i * 4 : 1 + i * 2);
        networks.append(n);
    }
    g_rootItem->setProperty("networks", networks);
    g_rootItem->setProperty("requested", true);
    g_rootItem->setProperty("visible", true);

    QImage first = renderFrame();
    if (!probePath.isEmpty()) {
        first.save(probePath);
        printf("probe %s %dx%d bytes-null=%d\n", qPrintable(probePath),
               first.width(), first.height(), int(first.isNull()));
        return first.isNull() ? 5 : 0;
    }
    if (first.isNull()) { fprintf(stderr, "first frame is null\n"); return 5; }
    { QByteArray png; QBuffer b(&png); b.open(QIODevice::WriteOnly); first.save(&b, "PNG"); g_lastPng = png; }

    QTimer capture;
    QObject::connect(&capture, &QTimer::timeout, [] {
        QImage img = renderFrame();
        if (img.isNull()) return;
        QByteArray png; QBuffer b(&png); b.open(QIODevice::WriteOnly);
        img.save(&b, "PNG");
        if (!png.isEmpty()) g_lastPng = png;
    });
    capture.start(300);

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, quint16(port))) {
        fprintf(stderr, "cannot listen on %d\n", port);
        return 6;
    }
    QObject::connect(&server, &QTcpServer::newConnection, [&] {
        while (QTcpSocket *s = server.nextPendingConnection()) handle(s);
    });
    printf("emulator ready: http://127.0.0.1:%d/ (%dx%d) first-frame=%d bytes\n",
           port, width, height, int(g_lastPng.size()));
    fflush(stdout);
    return app.exec();
}
