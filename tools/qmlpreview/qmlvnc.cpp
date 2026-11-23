// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// Interactive emulation through Qt's VNC platform plugin: the real page renders
// into the plugin's framebuffer, so any VNC client can look at it and tap it at
// head-unit resolution.  Unlike the offscreen plugin, this one keeps a real
// window, which is why grabWindow()/clicks behave.
#include <QGuiApplication>
#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickView>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <cstdio>

/* Find an item by its visible text, so a probe click lands on the widget instead
 * of a guessed pixel: the pages position their controls with explicit x/y and a
 * wrong guess silently hits dead space. */
static QQuickItem *findByText(QQuickItem *item, const QString &text)
{
    if (!item) return nullptr;
    /* Vendor buttons carry their caption in `label`, not `text`. */
    if (item->property("text").toString() == text) return item;
    if (item->property("label").toString() == text) return item;
    for (QQuickItem *c : item->childItems()) {
        if (QQuickItem *hit = findByText(c, text)) return hit;
    }
    return nullptr;
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    const QString pageUrl = args.value(1);
    const int width = args.value(2, "1280").toInt();
    const int height = args.value(3, "720").toInt();
    const QString dataDir = args.value(4, "");
    const QString probePath = args.value(5, "");   /* optional: grab once, save, quit */
    const QString click = args.value(6, "");       /* optional: x,y tapped before the second grab */
    const QString tab = args.value(7, "");         /* optional: switch to this tab for the second grab */
    const QString clickText = args.value(8, "");   /* optional: click the widget with this text */
    if (pageUrl.isEmpty()) {
        fprintf(stderr, "usage: qmlvnc <page.qml> [w] [h] [dataDir] [probe.png]\n");
        return 2;
    }

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

    QQuickView view;
    view.setColor(QColor("#000000"));
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(width, height);
    view.engine()->addImportPath(dataDir + "/imports");
    view.engine()->rootContext()->setContextProperty("AppHomeModel", &appModel);
    view.engine()->rootContext()->setContextProperty("ConstantModel", &constModel);
    view.engine()->rootContext()->setContextProperty("DEVICE_WIDTH", width);
    view.engine()->rootContext()->setContextProperty("DEVICE_HEIGHT", height);
    view.setSource(QUrl::fromLocalFile(pageUrl));
    if (view.status() != QQuickView::Ready) {
        fprintf(stderr, "page failed to load\n");
        for (const QQmlError &e : view.errors()) fprintf(stderr, "  %s\n", qPrintable(e.toString()));
        return 3;
    }
    if (QQuickItem *rootItem = view.rootObject()) {
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
        rootItem->setProperty("networks", networks);
        /* visible: requested - the page hides itself until it is opened. */
        rootItem->setProperty("requested", true);
        rootItem->setProperty("visible", true);
    }
    view.show();

    if (!probePath.isEmpty()) {
        QTimer::singleShot(1500, [&view, probePath, width, height, click, tab, clickText] {
            const QImage before = view.grabWindow();
            const bool ok = !before.isNull() && before.save(probePath);
            int diff = -1;
            if (!click.isEmpty() && !before.isNull()) {
                const QStringList xy = click.split(',');
                QPointF p(xy.value(0).toInt(), xy.value(1).toInt());
                if (QQuickItem *hit = findByText(view.rootObject(), clickText)) {
                    const QPointF c = hit->mapToScene(QPointF(hit->width() / 2, hit->height() / 2));
                    p = c;
                    fprintf(stderr, "clicking '%s' at %.0f,%.0f (item %.0fx%.0f)\n",
                            qPrintable(clickText), p.x(), p.y(), hit->width(), hit->height());
                }
                QMouseEvent press(QEvent::MouseButtonPress, p, view.mapToGlobal(p.toPoint()),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(&view, &press);
                QMouseEvent rel(QEvent::MouseButtonRelease, p, view.mapToGlobal(p.toPoint()),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(&view, &rel);
                /* Give the scene a moment to re-render, then compare pixel-wise. */
                QElapsedTimer t;
                t.start();
                while (t.elapsed() < 400) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                /* Optional state change for the probe (tab index), so a render
                 * can be proven to update without depending on input delivery. */
                if (!tab.isEmpty()) {
                    if (QQuickItem *ri = view.rootObject())
                        ri->setProperty("currentTab", tab.toInt());
                    QElapsedTimer t2;
                    t2.start();
                    while (t2.elapsed() < 400) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                }
                const QImage after = view.grabWindow();
                if (!after.isNull()) {
                    after.save(probePath + ".after.png");
                    diff = 0;
                    const QImage a = before.convertToFormat(QImage::Format_ARGB32);
                    const QImage b = after.convertToFormat(QImage::Format_ARGB32);
                    for (int y = 0; y < a.height(); y++) {
                        const QRgb *ra = reinterpret_cast<const QRgb *>(a.constScanLine(y));
                        const QRgb *rb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
                        for (int x = 0; x < a.width(); x++)
                            if (ra[x] != rb[x]) diff++;
                    }
                }
            }
            printf("probe %s %dx%d saved=%d changed-pixels=%d\n", qPrintable(probePath),
                   width, height, int(ok), diff);
            fflush(stdout);
            QCoreApplication::quit();
        });
    } else {
        printf("vnc emulator running (%dx%d); connect a VNC client to the plugin's port\n",
               width, height);
        fflush(stdout);
    }
    return app.exec();
}
