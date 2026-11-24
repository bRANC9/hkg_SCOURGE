import QtQuick 2.3
import "../component"
import "../component/common"
import HMIEVENTS 1.0
import AppHome_Enum 1.0

QRootScreen {
    id: root
    width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: DEVICE_HEIGHT
    onStateChanged: {
        right_left_animation.startAnimation()
        if(root.state == "customize") {
            enableClickOutside.start()
        }
        else {
            isLoadedMenuDone = false
        }
    }
    property bool customizeMode: AppHomeModel.settingMenuRearrangeState
    readonly property int _DURATION_DRAG: 200
    readonly property int _HOME_VIEW_AT_WIDGET: 0
    property bool customizeModeHK_PAGE1: false
    property bool customizeModeHK_PAGE2: false
    property bool customizeModeHK_PAGE3: false
    property bool hkPressd: false
    property bool mouseAreaPressed: false
    property bool isPendingConstructModel: false
    property bool isLoadedMenuDone: false
    isEnable: !AppHomeModel.sysPopupShowing

    Rectangle {
        anchors.fill: root
        color: ConstantModel.GENERAL_COLOR_BLACK
    }

    Q_ThemeImage {
        anchors.bottom: parent.bottom
        id: home_bg_image
        themeSource: AppHomeModel.isWideScr? defineModel.home_main_bg_01 : defineModel.home_main_bg_02
        anchors.horizontalCenter: parent.horizontalCenter
    }

    MouseArea {
        property bool _isClickOutSide: false
        property real posDownX: 0
        property real posUpX: 0
        property real posDownY: 0
        property real posUpY: 0

        width: parent.width
        height: 630
        y: 90
        anchors.fill: parent
        onPressed: {
            posDownX = mouse.x
            posDownY = mouse.y
            posUpX = mouse.x
            posUpY = mouse.y
            _isClickOutSide = true
        }
        onPositionChanged: {
            posUpX = mouse.x
            posUpY = mouse.y
        }
        onCanceled: {
            _isClickOutSide = false
        }

        onReleased: {
            if(root.customizeMode)
            {
                var deltaX = Math.abs(posUpX - posDownX)
                var deltaY = Math.abs(posUpY - posDownY)
                if(_isClickOutSide && (deltaX < 20 && deltaY < 20))
                {
                    QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
                    if(isLoadedMenuDone) {
                        reqPageCustomize(false)
                    }
                }
            }
        }
    }

    QMenuBarSubmenu{
        id: headerMenu
        y: ConstantModel.MENU_BAR_Y
        title: customizeMode ? TranslationsModel.STR_HOME_TITLE_EDIT_SETUP : TranslationsModel.STR_HOME_TITLE_SETTING
        displayOffEnale: customizeMode? false : true
        searchEnable: true //AppHomeModel.vehicleVariant !== AppHome_Enum.E_VEHICLE_HCR_PE
        onButtonDisplayOffCicked: {
            console.log("Display off Button Clicked")
            QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_CLICK_DISP_OFF)
        }
        defaultEnale: customizeMode? true : false
        fullModeForTopBar: false
        z: 100
        onButtonDefaultClicked: {
            console.log("[3] Default Button Click")
            QML_Handler.qmlSendEvtKey(HMIEVENTS.KEY_EVT_SHOW_POPUP_SETTING_RESET_MENU)
//            AppHomeModel.defaultEditHomeicon(AppHome_Enum.E_LIST_SETUP_MAIN)
//            menuList.isReady = false
//            menuList.requestResetRerrange()
//            menuList.updateHistoryPageReset()
        }
//        onButtonEditClicked: {
//            console.log("Edit Button Clicked")
//            reqPageCustomize(true)
//        }
    }

    Connections {
        target: AppHomeModel
        onSetDefaultSettingMenuChanged: {
            menuList.requestResetRerrange()
        }
    }

    QMainScreenAnimation {
        id: right_left_animation
        positionTarget: contentArea
        opacityTarget: contentArea
    }

    Item {
        id: contentArea
        anchors.fill: parent

        property bool isAnimating: false

        onOpacityChanged: {

            if (opacity < 1 && contentArea.isAnimating == false)
            {
                if (contentArea.isAnimating == false)
                {
                    console.log("[NTH] isAnimating true")
                    contentArea.isAnimating = true
                }
            }
            else
            {
                if (contentArea.isAnimating)
                {
                    console.log("[NTH] isAnimating false")
                    contentArea.isAnimating = false
                }
            }
        }

        QText{
            id: customizeGuide
            anchors.verticalCenter: parent.top
            anchors.verticalCenterOffset: 217
            anchors.horizontalCenter: parent.horizontalCenter
            font.pixelSize: ConstantModel.FONT_SIZE_33
            text: TranslationsModel.STR_HOME_EDIT_DRAG_HELP_ADV
            visible: customizeMode
        }
        QCustomizedMenuListAllowEmpty{
            id: menuList
            listType: AppHome_Enum.E_LIST_SETUP_MAIN
            customizeMode: root.customizeMode
            lstModel: AppHomeModel.listSubmenuSetup
            currentPageIndex: AppHomeModel.currentSetupPageIdx
            maxNumPages: AppHomeModel.isWideScr? 2 : 3
            isAllMenuScreen: false
            isFullMode: true
            listPosY: ConstantModel.SETUP_PAGE_Y
            onPageIndexChanged: {
                AppHomeModel.currentSetupPageIdx = index
            }
            onMenuItemClicked: {
                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_SUBMENU_SETUP_ITEMCLICKED, _itemID)
            }
            onLstModelChanged: {
                console.log("PDT >> constructmodel again")
                //menuList.constructMenuModel()
            }
            onSwapMenuItem: {
                AppHomeModel.editAllMenus(2, dragItemIdx, dropItemIdx) // 2 is the enum for All Menu list, refer to AppHome_Enum::E_LIST_SETUP_MAIN
            }
            onClickOutSide: {
//                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_BACK_TO_PREV_SCREEN)
//                root.customizeMode = false
                if(isLoadedMenuDone) {
                    reqPageCustomize(false)
                }
            }
            onUpdateMenuItem: {
                AppHomeModel.updateAllMenus(2, dragItemPage, dragItemIndex, dropItemPage, dropItemIndex)
            }
            onActiveDragAndDrop: {
                menuList.lockCurrentIndexChanged()
                reqPageCustomize(true)
            }
        }
        Row{
            id: indicatorGroup
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            width: ((menuList.totalPagesDisplay - 1) * 9) + (menuList.totalPagesDisplay * 39)
            height: 26
            spacing: 9
            visible: menuList.totalPagesDisplay > 1
            Repeater {
                model: menuList.totalPagesDisplay
                Q_ThemeImage {
                    themeSource: AppHomeModel.currentSetupPageIdx === index? defineModel.home_ic_indicator_w_s : defineModel.home_ic_indicator_w_n
                }
            }
        }
    }
    Connections {
        target: AppHomeModel
        onMenuListUpdated: {
            menuList.updateItemTonedown(itemIds, itemCount, lstTarget, itemToneDown)
        }
//        onMenuListVisibleUpdated: {
//            menuList.updateItemVisible(itemId, visible);
//        }

        onButtonBackClicked:{
            console.log("onBackButtonClicked, customizeMode: ", customizeMode)
            if(customizeMode){
                finishCustomization()
            }
            else{
                // reset page index to 0 when back
                AppHomeModel.currentSetupPageIdx = 0
                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_BACK_TO_PREV_SCREEN);
            }
        }
        onUpdateMenuSetup:{
            console.log("Update Menu setup")
            if (!AllMenuPage1.allMenuScreenOwner)
            {
                menuList.constructMenuModel()
            }
            else
            {
                console.log("[3] Allmenu owner -> ignore.")
            }
        }
        onRequestBackPageCustomize: {
            reqPageCustomize(true)
        }
        onRequestPageCustomize: {
            reqPageCustomize(true)
            AppHomeModel.optMenuAnimation = false
        }
        onIsAtOtherAppChanged: {
            if (!AppHomeModel.isAtOtherApp && isPendingConstructModel)
            {
                if (root.visible)
                {
                    console.log("[3] isPendingConstructModel = true. start construct list")
                    isPendingConstructModel = false;
                    if (!AllMenuPage1.allMenuScreenOwner)
                    {
                        menuList.constructMenuModel()
                    }
                    else
                    {
                        console.log("[3] Allmenu owner -> ignore.")
                    }
                }
                else
                {
                    console.log("[3] isPendingConstructModel = true. But visible false ")
                }
            }
        }
    }
    states: [
        State {
            name: "normal"
//            PropertyChanges {
//                target: root
//                customizeMode: false
//                customizeModeHK_PAGE1: false
//                customizeModeHK_PAGE2: false
//                customizeModeHK_PAGE3: false
//            }
//            PropertyChanges {
//                target: headerMenu
//                editVisible: true
//                title: TranslationsModel.STR_HOME_TITLE_SETTING
//            }
        },
        State {
            name: "customize"

//            PropertyChanges {
//                target: root
//                customizeMode: true
//            }
//            PropertyChanges {
//                target: headerMenu
//                editVisible: false
//                title: TranslationsModel.STR_HOME_TITLE_EDIT_SETUP
//            }
//            PropertyChanges {
//                target: menuList
//                listPosY: 255 - ConstantModel.MENU_BAR_Y
//            }
        }
    ]

    Timer {
        id: enableClickOutside
        interval: 100
        repeat: false
        onTriggered: {
            isLoadedMenuDone = true
        }
    }

    Component.onCompleted: {
//            AppHomeModel.isAtSetupMenuScreen = true
        right_left_animation.startAnimation()
        AppHomeModel.screenNotSupportSS = true
        if(AppHomeModel.isOnScrShowing)
        {
            AppHomeModel.isOnScrShowing = false
        }
        AppHomeModel.listOptMenu = Qt.binding(function(){return (!customizeMode) ? [
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_DISPLAY_OFF,  label: TranslationsModel.STR_HOME_TITLE_DIS_OFF, isItemSelect: false, isEnabled: true},
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_SEARCH_FOR_SETTINGS,  label: TranslationsModel.STR_HOME_SEARCH_FOR_SETTINGS, isItemSelect: false, isEnabled: true},
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_REARRANGE_ICON,   label: TranslationsModel.STR_HOME_REARRANGE_ICONS,         isItemSelect: false,    isEnabled: true},
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_MANUAL,  label: TranslationsModel.STR_HOME_MANUAL + " > " + TranslationsModel.STR_HOME_SETUP, isItemSelect: false, isEnabled: true},
                                                                                       //{indexOptItem: AppHome_Enum.E_OPT_SPLIT_SCREEN,     label: TranslationsModel.STR_HOME_SPLIT_SCREEN, isItemSelect: true,     isEnabled: true},
                                                                                   ] :
                                                                                   [
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_DISPLAY_OFF,  label: TranslationsModel.STR_HOME_TITLE_DIS_OFF, isItemSelect: false, isEnabled: true},
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_SEARCH_FOR_SETTINGS,  label: TranslationsModel.STR_HOME_SEARCH_FOR_SETTINGS, isItemSelect: false, isEnabled: true},
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_MANUAL,  label: TranslationsModel.STR_HOME_MANUAL + " > " + TranslationsModel.STR_HOME_SETUP, isItemSelect: false, isEnabled: true},
                                                                                       //{indexOptItem: AppHome_Enum.E_OPT_MANUAL,  label: TranslationsModel.STR_HOME_MANUAL, isItemSelect: false, isEnabled: true},
                                                                                       //{indexOptItem: AppHome_Enum.E_OPT_SPLIT_SCREEN,     label: TranslationsModel.STR_HOME_SPLIT_SCREEN, isItemSelect: true,     isEnabled: true},
                                                                                   ]})
    }

    onCustomizeModeChanged: {
        AppHomeModel.listOptMenu = Qt.binding(function(){return (!customizeMode) ? [
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_DISPLAY_OFF,  label: TranslationsModel.STR_HOME_TITLE_DIS_OFF, isItemSelect: false, isEnabled: true},
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_SEARCH_FOR_SETTINGS,  label: TranslationsModel.STR_HOME_SEARCH_FOR_SETTINGS, isItemSelect: false, isEnabled: true},
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_REARRANGE_ICON,   label: TranslationsModel.STR_HOME_REARRANGE_ICONS,         isItemSelect: false,    isEnabled: true},
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_MANUAL,  label: TranslationsModel.STR_HOME_MANUAL + " > " + TranslationsModel.STR_HOME_SETUP, isItemSelect: false, isEnabled: true},
                                                                                       //{indexOptItem: AppHome_Enum.E_OPT_SPLIT_SCREEN,     label: TranslationsModel.STR_HOME_SPLIT_SCREEN, isItemSelect: true,     isEnabled: true},
                                                                                   ] :
                                                                                   [
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_DISPLAY_OFF,  label: TranslationsModel.STR_HOME_TITLE_DIS_OFF, isItemSelect: false, isEnabled: true},
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_SEARCH_FOR_SETTINGS,  label: TranslationsModel.STR_HOME_SEARCH_FOR_SETTINGS, isItemSelect: false, isEnabled: true},
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_MANUAL,  label: TranslationsModel.STR_HOME_MANUAL + " > " + TranslationsModel.STR_HOME_SETUP, isItemSelect: false, isEnabled: true},
                                                                                       //{indexOptItem: AppHome_Enum.E_OPT_MANUAL,  label: TranslationsModel.STR_HOME_MANUAL, isItemSelect: false, isEnabled: true},
                                                                                       //{indexOptItem: AppHome_Enum.E_OPT_SPLIT_SCREEN,     label: TranslationsModel.STR_HOME_SPLIT_SCREEN, isItemSelect: true,     isEnabled: true},
                                                                                   ]})
    }

    function finishCustomization(){
        reqPageCustomize(false)
//        menuList.finishCustomization()
    }
    function reqPageCustomize(_customize){
        if(_customize){
            menuList.lockCurrentIndexChanged()
            AppHomeModel.settingMenuRearrangeState = true
            root.state = "customize"
            menuList.updateHistoryPage()
        }
        else{
            menuList.lockCurrentIndexChanged()
            AppHomeModel.settingMenuRearrangeState = false
            root.state = "normal"
            menuList.updateHistoryPage()
        }
    }
}
