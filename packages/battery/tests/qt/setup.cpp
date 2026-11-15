/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors */
/* SPDX-License-Identifier: AGPL-3.0-only */
#include <QtQuickTest/quicktest.h>
#include <QQmlEngine>
#include <QQmlContext>
#include <qqml.h>

class Model : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isWideScr MEMBER wide NOTIFY isWideScrChanged)
    Q_PROPERTY(bool isAtOtherApp MEMBER other NOTIFY isAtOtherAppChanged)
    Q_PROPERTY(bool sysPopupShowing MEMBER popup NOTIFY sysPopupShowingChanged)
    Q_PROPERTY(bool requestHomeByPP MEMBER home NOTIFY requestHomeByPPChanged)
    Q_PROPERTY(int currentScreen MEMBER screen NOTIFY currentScreenChanged)
    Q_PROPERTY(int currentHomeAdvancedPageIdx MEMBER page NOTIFY currentHomeAdvancedPageIdxChanged)
public:
    bool wide = true, other = false, popup = false, home = false;
    int screen = 0, page = 0;
signals:
    void isWideScrChanged(); void isAtOtherAppChanged(); void sysPopupShowingChanged();
    void requestHomeByPPChanged(); void currentScreenChanged(); void currentHomeAdvancedPageIdxChanged();
    void buttonBackClicked(); void switchToOtherApp();
};

class Constants : public QObject {
    Q_OBJECT
    Q_PROPERTY(int SCREEN_WIDTH MEMBER width CONSTANT)
    Q_PROPERTY(int MENU_BAR_Y MEMBER top CONSTANT)
    Q_PROPERTY(QString GENERAL_COLOR_BUTTON_GREY MEMBER buttonGrey CONSTANT)
    Q_PROPERTY(QString GENERAL_COLOR_FOCUS_GREY MEMBER focusGrey CONSTANT)
    Q_PROPERTY(int FONT_SIZE_15 MEMBER f15 CONSTANT)
    Q_PROPERTY(int FONT_SIZE_18 MEMBER f18 CONSTANT)
    Q_PROPERTY(int FONT_SIZE_21 MEMBER f21 CONSTANT)
    Q_PROPERTY(int FONT_SIZE_24 MEMBER f24 CONSTANT)
    Q_PROPERTY(int FONT_SIZE_27 MEMBER f27 CONSTANT)
    Q_PROPERTY(int FONT_SIZE_42 MEMBER f42 CONSTANT)
    Q_PROPERTY(int FONT_SIZE_44 MEMBER f44 CONSTANT)
public:
    int width = 1280, top = 90;
    QString buttonGrey = "#222222", focusGrey = "#555555";
    int f15 = 15, f18 = 18, f21 = 21, f24 = 24, f27 = 27, f42 = 42, f44 = 44;
};

class Phev : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool vcuEvReady MEMBER ready CONSTANT)
    Q_PROPERTY(int utilityMode MEMBER utility CONSTANT)
    Q_PROPERTY(double batLevel MEMBER level CONSTANT)
    Q_PROPERTY(int heatBatteryPercent MEMBER heat NOTIFY heatPercentChanged)
    Q_PROPERTY(int pwrInstantHeat MEMBER heatW NOTIFY instantPowerChanged)
    Q_PROPERTY(int pwrInstantBattery MEMBER batteryW NOTIFY instantPowerChanged)
    Q_PROPERTY(int batteryConditionMode MEMBER cond CONSTANT)
public:
    bool ready = true;
    int utility = 0;
    double level = 78.5;
    int heat = -1;
    int heatW = -1;
    int batteryW = -1;
    int cond = 0;

    Q_INVOKABLE void setInstantPower(int heat, int battery)
    {
        heatW = heat;
        batteryW = battery;
        emit instantPowerChanged();
    }
    Q_INVOKABLE void setHeatPercent(int percent)
    {
        heat = percent;
        emit heatPercentChanged();
    }
signals:
    void instantPowerChanged();
    void heatPercentChanged();
};

class NMode : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString gearPosRightTxtImg MEMBER right CONSTANT)
    Q_PROPERTY(QString gearPosLeftTxtImg MEMBER left CONSTANT)
public:
    QString right = "P", left = "";
signals:
    void signalGEAR_TYPE(int value);
};

class Climate : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString ambienTemp MEMBER temp CONSTANT)
public:
    QString temp = "14";
};

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString normalTextColor MEMBER normal CONSTANT)
    Q_PROPERTY(QString disableTextColor MEMBER disabled CONSTANT)
public:
    QString normal = "#FFFFFF", disabled = "#888888";
};

class Define : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString home_main_bg_01 MEMBER bg1 CONSTANT)
    Q_PROPERTY(QString home_main_bg_02 MEMBER bg2 CONSTANT)
public:
    QString bg1 = "", bg2 = "";
};

class Handler : public QObject {
    Q_OBJECT
public slots:
    void qmlSendEvtKey(int) {}
};

// The vendor HMIEVENTS module is a C++ singleton whose enum values are
// addressed as HMIEVENTS.EVT_*. QML-declared properties cannot start with
// an upper-case letter, so model it as a registered C++ singleton.
class HmiEvents : public QObject {
    Q_OBJECT
public:
    enum Event { EVT_HOME_SOUND_BEEP_PLAY = 1 };
    Q_ENUM(Event)
};

class AppHomeEnum : public QObject {
    Q_OBJECT
};

class Setup : public QObject {
    Q_OBJECT
    Model model;
    Constants constants;
    Phev phev;
    NMode nmode;
    Climate climate;
    Theme theme;
    Define define;
    Handler handler;
    HmiEvents hmi;
    AppHomeEnum appHomeEnum;
public slots:
    void qmlEngineAvailable(QQmlEngine *engine) {
        static bool registered = false;
        if (!registered) {
            qmlRegisterSingletonInstance("HMIEVENTS", 1, 0, "HMIEVENTS", &hmi);
            qmlRegisterSingletonInstance("AppHome_Enum", 1, 0, "AppHome_Enum", &appHomeEnum);
            registered = true;
        }
        engine->rootContext()->setContextProperty("AppHomeModel", &model);
        engine->rootContext()->setContextProperty("ConstantModel", &constants);
        engine->rootContext()->setContextProperty("AppPHEVModel", &phev);
        engine->rootContext()->setContextProperty("AppNModeModel", &nmode);
        engine->rootContext()->setContextProperty("appClimateModel", &climate);
        engine->rootContext()->setContextProperty("HomeThemeHelper", &theme);
        engine->rootContext()->setContextProperty("defineModel", &define);
        engine->rootContext()->setContextProperty("QML_Handler", &handler);
        engine->rootContext()->setContextProperty("DEVICE_WIDTH", 1920);
        engine->rootContext()->setContextProperty("DEVICE_HEIGHT", 720);
    }
};
QUICK_TEST_MAIN_WITH_SETUP(battery_preheat, Setup)
#include "setup.moc"
