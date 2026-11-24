import QtQuick 2.3
import QtGraphicalEffects 1.0
import HMIEVENTS 1.0
import AppHome_Enum 1.0

import "../"
import "../component/MyMenuComponent"
import "../component"
import "../component/common"

QRootScreen {
    property bool mouseAreaPressed:false
    property bool customizeMode: AppHomeModel.allMenuRearrangeState
    property bool isFullMode : AppHomeModel.isFullMode
    readonly property int _DURATION_SCALE: 200
    readonly property int _DURATION_DRAG: 200
    readonly property real _SCALE_CUSTOMIZE: 0.75
    property bool hkPressd: false
    property bool isPendingConstructModel: false

    id: root
    isEnable: !AppHomeModel.sysPopupShowing
    anchors.leftMargin: (AppHomeModel.usingRHD) ? (640): (0)
    width: isFullMode && AppHomeModel.isWideScr ? ConstantModel.SCREEN_WIDE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: DEVICE_HEIGHT
    onStateChanged: right_left_animation.startAnimation()

    Rectangle{
        color: "black"
        anchors.fill: parent
        z: -1
    }

    Q_ThemeImage {
        anchors.bottom: parent.bottom
        id: home_bg_image
        themeSource: AppHomeModel.isWideScr? defineModel.home_main_bg_01 : defineModel.home_main_bg_02
        anchors.horizontalCenter: parent.horizontalCenter
        visible: !root.customizeMode
    }

    MouseArea {
        property bool _isClickOutSide: false
        property real posDownX: 0
        property real posUpX: 0
        property real posDownY: 0
        property real posUpY: 0

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
                    reqPageCustomize(false)
                }
            }
        }
    }

    QMenuBarSubmenu{
        id: headerMenu
        y: ConstantModel.MENU_BAR_Y
        title: customizeMode ? TranslationsModel.STR_HOME_EDIT_ALL_MENUS : TranslationsModel.STR_HOME_ALL_MENU
        displayOffEnale: false
        defaultEnale: customizeMode? true : false
        onButtonDefaultClicked: {
            console.log("[3] Default Button Click")
            AppHomeModel.defaultEditHomeicon(AppHome_Enum.E_LIST_ALL_MENUS)
//            menuList.isReady = false
            menuList.constructMenuModel()
//            menuList.updateHistoryPageReset()
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

        QCustomizedMenuListAllowEmpty{
            id: menuList
            listType: AppHome_Enum.E_LIST_ALL_MENUS
            prefixImg: defineModel.prefixImg99
            customizeMode: root.customizeMode
            lstModel: AppHomeModel.listHomePage
            currentPageIndex: AppHomeModel.currentMenuPageIdx
            maxNumPages: (isFullMode && AppHomeModel.isWideScr) ? 2 : 3
            isAllMenuScreen: true
            listPosY: ConstantModel.SETUP_PAGE_Y
            onPageIndexChanged: {
                AppHomeModel.currentMenuPageIdx = index
            }
            onMenuItemClicked: {
                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_MAIN_LIST_ITEMCLICKED, _itemID)
            }
            onSwapMenuItem: {
                AppHomeModel.editAllMenus(1, dragItemIdx, dropItemIdx) // 1 is the enum for All Menu list, refer to AppHome_Enum::E_LIST_ALL_MENUS
            }
            onUpdateMenuItem: {
                AppHomeModel.updateAllMenus(1, dragItemPage, dragItemIndex, dropItemPage, dropItemIndex)
            }

            onClickOutSide: {
//                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_BACK_TO_PREV_SCREEN)
//                root.customizeMode = false
//                AppHomeModel.currentMenuPageIdx = 0;
                reqPageCustomize(false)
            }
            onActiveDragAndDrop: {
                menuList.lockCurrentIndexChanged()
                reqPageCustomize(true)
                menuList.updateHistoryPage()
            }
        }
        Row{
            id: indicatorGroup
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
//            anchors.topMargin: -20
            width: (menuList.totalPagesDisplay - 1) * 85 + 12
            height: 26
            spacing: 10
            visible: menuList.totalPagesDisplay > 1
            Repeater {
                model: menuList.totalPagesDisplay
                Q_ThemeImage {
                    themeSource:{
                        var imgSelected = defineModel.home_ic_indicator_w_s
                        var imgNormal = defineModel.home_ic_indicator_w_n
                        return (index === AppHomeModel.currentMenuPageIdx ? imgSelected : imgNormal)
                    }
                }
            }
        }
        QText{
            id: customizeHeader
            visible: customizeMode
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.top
            anchors.verticalCenterOffset: 217
            font.pixelSize: 33
            color: "#FAFAFA"
            text: TranslationsModel.STR_HOME_EDIT_DRAG_HELP_ADV
        }
    }

    states: [
        State {
            name: "normal"
//            PropertyChanges {
//                target: root
//                customizeMode: false
//            }
        },
        State {
            name: "customize"
//            PropertyChanges {
//                target: root
//                customizeMode: true
//            }
//            PropertyChanges {
//                target: menuList
//                listPosY: 255 - ConstantModel.MENU_BAR_Y
//            }
        }
    ]

    onIsFullModeChanged: {
        if (!AppHomeModel.isAtOtherApp && menuList.isReady)
        {
            console.log("[3] isAtOtherApp:",AppHomeModel.isAtOtherApp)
            console.log("[3] menuList.isReady:",menuList.isReady)
            if (AllMenuPage1.allMenuScreenOwner)
            {
                menuList.lockCurrentIndexChanged()
                menuList.constructMenuModel()
                menuList.updateHistoryPage()
            }
            else
            {
                console.log("[3] Not allmenu owner -> ignore.")
            }
        }
        else if (AppHomeModel.isAtOtherApp && menuList.isReady)
        {
            console.log("[3] isPendingConstructModel = true ")
            isPendingConstructModel = true;
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
        onAllMenuEnableUpdated: {
            menuList.updateItemTonedown(itemIds, itemCount, lstTarget, itemToneDown)
        }
        onRequestUpdateAllMenu: {
            if (AllMenuPage1.allMenuScreenOwner)
            {
                menuList.constructMenuModel()
            }
            else
            {
                console.log("[3] Not allmenu owner -> ignore.")
            }
        }

        onButtonBackClicked:{
            if(customizeMode){
                finishCustomization()
            }
            else{
                AppHomeModel.currentMenuPageIdx = 0
                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_GOTO_ALL_SCREEN_ITEMS);
            }
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
                    if (AllMenuPage1.allMenuScreenOwner)
                    {
                        menuList.constructMenuModel()
                    }
                    else
                    {
                        console.log("[3] Not allmenu owner -> ignore.")
                    }
                }
                else
                {
                    console.log("[3] isPendingConstructModel = true. But visible false ")
                }

            }
        }
    }
    function finishCustomization(){
        reqPageCustomize(false)
//        menuList.finishCustomization()
    }
    Component.onCompleted: {
        right_left_animation.startAnimation()
        if(AppHomeModel.isOnScrShowing)
        {
            AppHomeModel.isOnScrShowing = false
        }
        AppHomeModel.listOptMenu  = Qt.binding(function(){return (!customizeMode) ? [
                                                                                         {indexOptItem: AppHome_Enum.E_OPT_REARRANGE_ICON,  label: TranslationsModel.STR_HOME_REARRANGE_ICONS,         isItemSelect: false,    isEnabled: true},
                                                                                         {indexOptItem: AppHome_Enum.E_OPT_MANUAL,  label: TranslationsModel.STR_HOME_MANUAL, isItemSelect: false, isEnabled: true},
                                                                                         {indexOptItem: AppHome_Enum.E_OPT_SPLIT_SCREEN,    label: TranslationsModel.STR_HOME_SPLIT_SCREEN, isItemSelect: true,     isEnabled: true},
                                                                                     ] :
                                                                                     [
                                                                                         //{indexOptItem: AppHome_Enum.E_OPT_MANUAL,  label: TranslationsModel.STR_HOME_MANUAL, isItemSelect: false, isEnabled: true},
                                                                                         {indexOptItem: AppHome_Enum.E_OPT_SPLIT_SCREEN,    label: TranslationsModel.STR_HOME_SPLIT_SCREEN, isItemSelect: true, isEnabled: true},
                                                                                     ]})
    }

    onCustomizeModeChanged: {
//        AppHomeModel.optMenuAnimation = false
        AppHomeModel.listOptMenu = Qt.binding(function(){return (!customizeMode) ? [
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_REARRANGE_ICON,  label: TranslationsModel.STR_HOME_REARRANGE_ICONS,         isItemSelect: false,    isEnabled: true},
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_MANUAL,  label: TranslationsModel.STR_HOME_MANUAL, isItemSelect: false, isEnabled: true},
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_SPLIT_SCREEN,    label: TranslationsModel.STR_HOME_SPLIT_SCREEN, isItemSelect: true,     isEnabled: true},
                                                                                   ] :
                                                                                   [
                                                                                       //{indexOptItem: AppHome_Enum.E_OPT_MANUAL,  label: TranslationsModel.STR_HOME_MANUAL, isItemSelect: false, isEnabled: true},
                                                                                       {indexOptItem: AppHome_Enum.E_OPT_SPLIT_SCREEN,    label: TranslationsModel.STR_HOME_SPLIT_SCREEN, isItemSelect: true, isEnabled: true},
                                                                                   ]})
    }

    function reqPageCustomize(_customize){
        if(_customize){
            menuList.lockCurrentIndexChanged()
            AppHomeModel.allMenuRearrangeState = true
            root.state = "customize"
            menuList.updateHistoryPage()
        }
        else{
            menuList.lockCurrentIndexChanged()
            AppHomeModel.allMenuRearrangeState = false
            root.state = "normal"
            menuList.updateHistoryPage()
        }
    }
}
