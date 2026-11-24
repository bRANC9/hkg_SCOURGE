// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// Offscreen preview of a head-unit QML page: fixed resolution, mock context
// properties, mock scan data, and a debug overlay that marks every item whose
// text is wider than its box (overflow) plus the column guides, so "which field
// is how wide, does it overflow" is answerable from a screenshot.
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QImage>
#include <QPainter>
#include <QEventLoop>
#include <QThread>
#include <QElapsedTimer>
#include <QRectF>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

static QQuickItem *findPage(QQuickItem *root, const QString &objectName)
{
    if (!root) return nullptr;
    if (root->objectName() == objectName) return root;
    for (QQuickItem *c : root->childItems()) {
        QQuickItem *hit = findPage(c, objectName);
        if (hit) return hit;
    }
    return nullptr;
}

static int overflowCount = 0;

/* Two different defects, and they must not be confused:
 *  - CLIP: single-line text (wrapMode NoWrap) wider than its box - it is cut.
 *  - VCLIP: wrapped text taller than its box - the tail is lost downwards.
 * A wrapping text legitimately has implicitWidth > width (that is what wrapping
 * means), which is why the first version over-reported. */
static void markOverflow(QQuickItem *item, QList<QQuickItem *> *marks)
{
    const QMetaObject *mo = item->metaObject();
    if (mo->indexOfProperty("implicitWidth") >= 0) {
        const qreal iw = item->property("implicitWidth").toReal();
        const qreal ih = mo->indexOfProperty("implicitHeight") >= 0
                             ? item->property("implicitHeight").toReal() : 0;
        const qreal w = item->width();
        const qreal h = item->height();
        const int wrap = mo->indexOfProperty("wrapMode") >= 0
                             ? item->property("wrapMode").toInt() : 0;
        const bool clip = (wrap == 0) && iw > w + 0.5 && iw > 1;
        const bool vclip = (wrap != 0) && ih > h + 0.5 && h > 1;
        if (clip || vclip) {
            marks->append(item);
            overflowCount++;
            if (vclip)
                fprintf(stderr, "  VCLIP h=%.0f implicitH=%.0f text=%s\n", h, ih,
                        qPrintable(item->property("text").toString().left(48)));
        }
    }
    for (QQuickItem *c : item->childItems()) markOverflow(c, marks);
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    const QString pageUrl = args.value(1);
    const int width = args.value(2, "1280").toInt();
    const int height = args.value(3, "720").toInt();
    const QString outPath = args.value(4, "preview.png");
    const QString dataDir = args.value(5, "");
    if (pageUrl.isEmpty()) {
        fprintf(stderr, "usage: qmlpreview <page.qml> [width] [height] [out.png] [dataDir]\n");
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
    appModel.insert("currentScreenName", "home");
    constModel.insert("MENU_BAR_Y", 60);
    constModel.insert("SCREEN_WIDTH", width);
    constModel.insert("SCREEN_HEIGHT", height);

    QVariantList networks;
    const char *bands[] = {"2.4", "5"};
    for (int i = 0; i < 8; i++) {
        QVariantMap n;
        const bool five = (i % 3) == 1;
        n.insert("ssid", i == 0 ? "Franci_wifi" : (five ? QString("Franci_wifi-ext_5G_%1").arg(i) : QString("Neighbour network %1").arg(i)));
        n.insert("signal", -50 - i * 6);
        n.insert("security", i == 2 ? "Open" : "WPA2");
        n.insert("freq", five ? 5180 + i * 20 : 2412 + i * 5);
        n.insert("band", bands[five ? 1 : 0]);
        n.insert("channel", five ? 36 + i * 4 : 1 + i * 2);
        networks.append(n);
    }

    QQuickView view;
    view.engine()->addImportPath(dataDir + "/imports");
    view.engine()->rootContext()->setContextProperty("AppHomeModel", &appModel);
    view.engine()->rootContext()->setContextProperty("ConstantModel", &constModel);
    view.engine()->rootContext()->setContextProperty("DEVICE_WIDTH", width);
    view.engine()->rootContext()->setContextProperty("DEVICE_HEIGHT", height);
    view.setColor(QColor("#000000"));
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(width, height);
    view.setSource(QUrl::fromLocalFile(pageUrl));
    if (view.status() != QQuickView::Ready) {
        fprintf(stderr, "page failed to load: %s\n", qPrintable(pageUrl));
        for (const QQmlError &err : view.errors())
            fprintf(stderr, "  %s\n", qPrintable(err.toString()));
        return 3;
    }
    QQuickItem *rootItem = view.rootObject();
    QList<QQuickItem *> marks;
    if (rootItem) {
        // Inject the scan rows: the page normally fills them from /scan.
        QQuickItem *list = findPage(rootItem, "wifiNetworks");
        if (list) list->setProperty("model", networks);
        rootItem->setProperty("networks", networks);
        /* The page owns its own visibility (visible: requested); without this the
         * render is an empty window and only the overlay guides show up. */
        rootItem->setProperty("requested", true);
        rootItem->setProperty("visible", true);
        // Optional state flags so a preview can show an overlay/panel that is
        // normally closed: qmlpreview … [showKeypad]
        if (args.size() > 6) {
            const QString flag = args.value(6);
            if (flag == "showKeypad") rootItem->setProperty("showKeypad", true);
            else if (flag.startsWith("tab=")) rootItem->setProperty("currentTab", flag.mid(4).toInt());
        }
        markOverflow(rootItem, &marks);
        fprintf(stderr, "overflow items: %d\n", overflowCount);
        for (QQuickItem *m : marks)
            fprintf(stderr, "  overflow %s x=%.0f w=%.0f implicit=%.0f text=%s\n",
                    m->metaObject()->className(), m->x(), m->width(),
                    m->property("implicitWidth").toReal(),
                    qPrintable(m->property("text").toString().left(40)));
    }
    view.show();
    /* The offscreen platform needs the frame to be produced before a grab
     * returns pixels; spin the event loop instead of grabbing immediately. */
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 800) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
    }
    QImage shot = view.grabWindow();
    if (shot.isNull()) {
        fprintf(stderr, "grab failed (null image)\n");
        return 4;
    }

    /* Overlay: 100 px grid, the column guides a reviewer needs to judge widths,
     * and a red box + measurement on every field whose text overflows its box. */
    {
        QPainter painter(&shot);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(QColor(255, 255, 255, 40), 1, Qt::DotLine));
        for (int x = 0; x < shot.width(); x += 100) painter.drawLine(x, 0, x, shot.height());
        for (int y = 0; y < shot.height(); y += 100) painter.drawLine(0, y, shot.width(), y);
        painter.setPen(QPen(QColor(0, 200, 255, 120), 1));
        const int guides[] = {8, 540, 650, 748, 840, 1110, 1120, 1144};
        for (int gx : guides) painter.drawLine(gx, 0, gx, shot.height());
        painter.setPen(QPen(QColor(255, 60, 60), 2));
        for (QQuickItem *m : marks) {
            const QRectF r = m->mapRectToScene(QRectF(0, 0, m->width(), qMax(m->height(), (qreal)12)));
            painter.drawRect(r);
            painter.setPen(QPen(QColor(255, 60, 60), 1));
            painter.drawText(QPointF(r.left() + 2, r.top() - 2),
                             QString("%1 > %2").arg(m->property("implicitWidth").toReal(), 0, 'f', 0)
                                 .arg(m->width(), 0, 'f', 0));
            painter.setPen(QPen(QColor(255, 60, 60), 2));
        }
    }
    shot.save(outPath);
    printf("wrote %s (%dx%d, overflow=%d)\n", qPrintable(outPath), width, height, overflowCount);
    return 0;
}
