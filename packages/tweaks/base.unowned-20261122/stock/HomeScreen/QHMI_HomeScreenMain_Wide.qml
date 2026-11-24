import QtQuick 2.0
import "../component"
import "../component/common"
import HMIEVENTS 1.0
import AppHome_Enum 1.0

QRootScreen {
    property bool customizeMode: false
    property string _prefixImg: defineModel.prefixImg99
    property string _prefixImg72: defineModel.prefixImg72
    property string _prefixOtherImg: defineModel.prefixImgHome
    property bool customizeModeHK_PAGE0: false
    property bool customizeModeHK_PAGE1: false
    property bool customizeModeHK_PAGE2: false
    property bool customizeModeHK_PAGE3: false
    property bool customizeModeHK_PAGE4: false
    property bool mouseAreaPressed:false
    property bool hkPressd: false
    property var optMenuModelData: AppHomeModel.listWidgetAdvVisibleCount() > 3? [
        {indexOptItem: AppHome_Enum.E_OPT_EDIT_HOME_WIDGET, label: TranslationsModel.STR_HOME_EDIT_WIDGET, isItemSelect: false, isEnabled: true},
        {indexOptItem: AppHome_Enum.E_OPT_EDIT_HOME_MENU,   label: TranslationsModel.STR_HOME_EDIT_MENUS, isItemSelect: false, isEnabled: true},
        {indexOptItem: AppHome_Enum.E_OPT_MANUAL,  label: TranslationsModel.STR_HOME_MANUAL + " > " +TranslationsModel.STR_HOME_SCREEN, isItemSelect: false, isEnabled: true}
    ] : [
        {indexOptItem: AppHome_Enum.E_OPT_EDIT_HOME_MENU,   label: TranslationsModel.STR_HOME_EDIT_MENUS, isItemSelect: false, isEnabled: true},
        {indexOptItem: AppHome_Enum.E_OPT_MANUAL,  label: TranslationsModel.STR_HOME_MANUAL + " > " +TranslationsModel.STR_HOME_SCREEN, isItemSelect: false, isEnabled: true}
    ]
    id: root
    isEnable: !AppHomeModel.sysPopupShowing
    width: DEVICE_WIDTH
    height: DEVICE_HEIGHT
    focus: true
    state : "normal"
    states: [
        State {
            name: "normal"
            PropertyChanges {
                target: root
                customizeMode: false
                customizeModeHK_PAGE0: false
                customizeModeHK_PAGE1: false
                customizeModeHK_PAGE2: false
                customizeModeHK_PAGE3: false
                customizeModeHK_PAGE4: false
            }
        }
    ]
    Rectangle{
        anchors.fill: root
        color: "black"
    }

    MouseArea {
        anchors.fill: parent
        onClicked: {
            console.log("[3] Click out area")
        }
    }

    Q_ThemeImage {
        anchors.bottom: parent.bottom
        id: home_bg_image
        themeSource: AppHomeModel.isWideScr? defineModel.home_main_bg_01 : defineModel.home_main_bg_02
        anchors.horizontalCenter: parent.horizontalCenter
    }

    /*__________________________________________Component Menu on home screen______________________________*/
    // Component menu on Home Screen
    Item {
        id: itemMainMenu
        width: AppHomeModel.isWideScr ? ConstantModel.HOME_1920_MENU_W : ConstantModel.HOME_1280_MENU_W //(627 + 627 + 12)
        height: ConstantModel.HOME_1920_MENU_H //(183)
        x: ConstantModel.HOME_1920_MENU_X //(12 + 618 + 12)
        y: ConstantModel.HOME_1920_MENU_Y //(102 + 405 + 15)

        // All Menu
        QHomeItem {
            id: itemAllMenu
            property bool atPressState: true
            width: AppHomeModel.isWideScr ? 250 : 207
            height: 183
            x_icon: AppHomeModel.isWideScr ? 76 : 55
            x: AppHomeModel.isWideScr ? 4 : 1
            icoPressed: gridAreaitemAllMenu.pressed && gridAreaitemAllMenu.containsMouse && itemAllMenu.atPressState
            iconSource: [defineModel.home_allmenu_99_n, defineModel.home_bg_icon_p,"", defineModel.home_allmenu_99_d]
            itemName: TranslationsModel.STR_HOME_ALL_MENU
            MouseArea {
                id: gridAreaitemAllMenu
                width: parent.width
                height: parent.height
                anchors.verticalCenter: parent.verticalCenter
                hoverEnabled: true
                preventStealing : false
                onPressedChanged: {
                }
                onPressed: {
                    timerLongPress.start()
                    itemAllMenu.atPressState = true
                }

                onPositionChanged: {
                    if(!containsMouse && itemAllMenu.atPressState) {
                        itemAllMenu.atPressState = false
                    }
                }

//                onPressAndHold: {
//                    if(itemAllMenu.atPressState) {
//                        QML_Handler.qmlChangeTopMsg(HMIEVENTS.EVT_GOTO_SCREEN_SUBMENU_EDIT_MENUS)
//                        QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
//                    }
//                }

                onCanceled: {
                    timerLongPress.stop()
                }

                onExited: {
                    timerLongPress.stop()
                }

                function onLongPress(){
                    if(itemAllMenu.atPressState) {
                        QML_Handler.qmlChangeTopMsg(HMIEVENTS.EVT_GOTO_SCREEN_SUBMENU_EDIT_MENUS)
                        QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
                    }
                    timerLongPress.stop()
                }

                Timer {
                    id: timerLongPress
                    interval: 650
                    running: false
                    repeat: false
                    onTriggered: {
                        gridAreaitemAllMenu.onLongPress()
                    }
                }

                onReleased: {
                    timerLongPress.stop()
                    if(itemAllMenu.atPressState)
                    {
                        AppHomeModel.allMenuRearrangeState = false
                        QML_Handler.qmlSendEvtKey(HMIEVENTS.KEY_EVT_REQUEST_GOTO_SCREEN_ALL_MENUS)
                        QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
                        itemAllMenu.atPressState = false
                        root.isEnable = false
                    }
                }
//                onPositionChanged: {
//                    if (activeId != -1 && index != -1 && index != activeIndex) {
//                        var _data = [activeIndex,  index]
//                        QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_LIST_PAGE1_MOVING, _data)
//                        mainGridItemMenu.model.move(activeIndex, activeIndex = index, 1)
//                        AppHomeModel.homePage1CurrentIdx = activeIndex
//                    }
//                }
            }

        }

        GridView {
            id: mainGridItemMenu
            height: itemMainMenu.height
            width: itemMainMenu.width
            anchors.top: parent.top
            anchors.left: itemAllMenu.right
            cellWidth: AppHomeModel.isWideScr ? 250 : 207
            cellHeight: 183
            model: AppHomeModel.listMenuHomeItem
            currentIndex : AppHomeModel.homePage1CurrentIdx
            interactive: false
            delegate: Item {
                id: item
                width: mainGridItemMenu.cellWidth
                height: mainGridItemMenu.cellHeight
                visible: model.modelData.visible
                QHomeItem {
                    id: gridItemMenuDelegate
                    property bool atPressState: true
                    width: item.width
                    height: item.height
                    parent: mainGridItemMenu
                    x_icon: AppHomeModel.isWideScr ? 78 : 55
                    x: item.x
                    y: item.y
                    icoPressed: gridAreaMenu.containsMouse && gridAreaMenu.pressed && gridItemMenuDelegate.atPressState && !model.modelData.tonedown//gridAreaMenu.index === index || AppHomeModel.homeCurrentIdx == 1 && AppHomeModel.homePage1CurrentIdx == index
                    iconSource: [_prefixImg + model.modelData.iconN, defineModel.home_bg_icon_p,"", _prefixImg + model.modelData.iconD]
                    icoDim: model.modelData.tonedown
                    fontColor: model.modelData.tonedown ? "Gray" : "White"
                    itemName: TranslationsModel.getDefineLocalize(model.modelData.label) + AppHomeModel.textUpdate
                    lineRVisible: index != mainGridItemMenu.count - 1
                    property int uid: (index >= 0) ?  model.modelData.pos : -1
                    states: [
                        State {
                            name: "active"
                            when: (gridAreaMenu.activeId === page1Delegate.uid)
                            PropertyChanges {
                                target: gridItemMenuDelegate
                                x: gridAreaMenu.mouseX - page1Delegate.width/2
                                z: 10
                            }
                        }
                    ]
                    MouseArea {
                        id: gridAreaMenu
                        anchors.fill: parent
                        hoverEnabled: true
                        preventStealing : false
                        visible: !model.modelData.tonedown
                        onPressed: {
                            timerLongPress2.start()
                            AppHomeModel.homePage1CurrentIdx =  index
                            gridItemMenuDelegate.atPressState = true
                        }
                        onPositionChanged: {
                            if(!containsMouse && gridItemMenuDelegate.atPressState) {
                                gridItemMenuDelegate.atPressState = false
                            }
                        }
//                        onPressAndHold: {
//                            if(gridItemMenuDelegate.atPressState) {
//                                QML_Handler.qmlChangeTopMsg(HMIEVENTS.EVT_GOTO_SCREEN_SUBMENU_EDIT_MENUS)
//                                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
//                            }
//                        }

                        onCanceled: {
                            timerLongPress2.stop()
                        }

                        onExited: {
                            timerLongPress2.stop()
                        }

                        function onLongPress(){
                            if(gridItemMenuDelegate.atPressState) {
                                QML_Handler.qmlChangeTopMsg(HMIEVENTS.EVT_GOTO_SCREEN_SUBMENU_EDIT_MENUS)
                                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
                            }
                            timerLongPress2.stop()
                        }

                        Timer {
                            id: timerLongPress2
                            interval: 650
                            running: false
                            repeat: false
                            onTriggered: {
                                gridAreaMenu.onLongPress()
                            }
                        }

                        onReleased: {
                            timerLongPress2.stop()
                            if(gridItemMenuDelegate.atPressState) {
                                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_MENU_LIST, modelData.itemID)
                                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
                            }
                            gridItemMenuDelegate.atPressState = true
                        }
                    }
                }
            }
        }

    }

    Component.onCompleted: {
        console.log("[3] QHMI_HomeScreenMain_Wide.qml onCompleted")
        AppHomeModel.reqLoadWidget()
        AppHomeModel.listOptMenu = Qt.binding(function(){return optMenuModelData})
        // reset list index when back to home screen
        AppHomeModel.screenNotSupportSS = true
        MyMenuModel.currentPageIdx = 0
        AppHomeModel.currentSetupPageIdx = 0
        AppHomeModel.currentMenuPageIdx = 0
        AppHomeModel.currentMenuMediaGatePageIdx = 0
        if(AppHomeModel.isOnScrShowing)
        {
            AppHomeModel.isOnScrShowing = false
        }
    }
    Component.onDestruction: {
        console.log("[3] QHMI_HomeScreenMain_Wide.qml onDestruction")
    }

}
