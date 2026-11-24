import QtQuick 2.3
import QtGraphicalEffects 1.0

import "../"
import "../component/MyMenuComponent"
import "../component"
import "../component/common"

import HMIEVENTS 1.0
import AppHome_Enum 1.0

Item{
    id: root
    readonly property int k_nbrOfRows: 2
    property string textNormalColor: HomeThemeHelper.normalTextColor
    property string prefixImg: defineModel.prefixImg99
    property bool isFullMode: AppHomeModel.isFullMode
    property bool isWideScreen: AppHomeModel.isWideScr
    property int k_itemWidth: 306  // item width
    property int k_itemHeight: 249 // item height
    property int k_itemWidth_custo: 272
    property int k_itemWidth_custo_s: 250
    property int k_itemHeight_custo: 207
    property int k_menuPageWidth : (isFullMode && isWideScreen) ? k_itemWidth * 6: k_itemWidth * 4
    property int k_menuPageHeight : k_itemHeight*2
    property int k_nbrOfColumns: (isFullMode && isWideScreen) ? 6 : 4
    property int nbrItemsPerPage: k_nbrOfRows*k_nbrOfColumns
    property var lstModel                            // model for menu list
    property bool  customizeMode: false          // TRUE is the list is being re-arranged
    property alias listPosY: menuList.y           // position Y of the list
    property int totalPages: 0    // total menu pages
    property int totalPagesNormalMode: 0
    property int totalPagesDisplay: (customizeMode ? listModel.count : listModelDisplay.count)
    property int listType         // list type: allmenus, setup main, media,...
    property bool custoBGVisible: true
    property int currentPageIndex: 0
    property int draggingIndex
    property int draggingPage
    property int menuDummyItemIdx: -1
    property int srcPageIndex: 0
    property int dstPageIndex: 0
    property bool isImagePSingleImg: true
    property int maxNumPages: -1
    property bool isAllMenuScreen: true // Is used for not conflict the logic of constructMenuModel()
    property bool isGridMoving: menuList.contentX != (menuList.width + menuList.spacing) * menuList.currentIndex
    property string imageEditBG:{ // image background for Customize state
        if( listType === AppHome_Enum.E_LIST_ALL_MENUS ||
                listType === AppHome_Enum.E_LIST_SETUP_MAIN){
            return isFullMode ? defineModel.home_bg_icon_edit_p : defineModel.home_bg_icon_edit_s_p
        }
        else if( listType === AppHome_Enum.E_LIST_MEDIA_GATE ){
            return defineModel.home_bg_icon_main_p
        }
    }
    property string imageP:{ // image background for PRESSED state
        if( listType === AppHome_Enum.E_LIST_ALL_MENUS ||
                listType === AppHome_Enum.E_LIST_SETUP_MAIN){
            return !customizeMode? defineModel.home_bg_icon_p : isFullMode ? defineModel.home_bg_icon_edit_p : defineModel.home_bg_icon_edit_s_p
        }
        else if( listType === AppHome_Enum.E_LIST_MEDIA_GATE ){
            if(isFullMode){
                return defineModel.me_bg_m_p
            }else{
                return defineModel.me_bg_m_p_s
            }
        }
    }

    property string imageIconN:{ // image background for NORMAL state
        if( listType === AppHome_Enum.E_LIST_ALL_MENUS ||
                listType === AppHome_Enum.E_LIST_SETUP_MAIN){
            return !customizeMode? defineModel.home_bg_icon_n : isFullMode ? defineModel.home_bg_icon_edit_n : defineModel.home_bg_icon_edit_s_n
        }
        else if( listType === AppHome_Enum.E_LIST_MEDIA_GATE ){
            if(isFullMode){
                return defineModel.me_bg_m_n
            }else{
                return defineModel.me_bg_m_n_s
            }
        }
    }
    property alias isReady: menuList.isAready
    property bool isCheck: false
    property int countWidthChange: 0
    property bool isListFlicking: false
    property bool isOverLap: false
    property bool isXLessThanZero: false
    property bool isDragging: false

    signal menuItemClicked(int _itemID, var _tonedown)
    signal pageIndexChanged(int index)
    signal swapMenuItem(int dragItemIdx, int dropItemIdx)
    signal updateMenuItem(int dragItemPage, int dragItemIndex, int dropItemPage,int dropItemIndex)
    signal clickOutSide();
    signal indexChange();
    signal activeDragAndDrop();

    property int firstDragIndex: -1
    property int currentDropIndex: -1


    onCurrentDropIndexChanged: {
        console.log("|onCurrentDropIndexChanged:", currentDropIndex)
    }

    property int lastDragIndex: -1
    property int lastDropIndex: -1
    property int lastEmptyIndex: -1

    property int lastDropPage: -1

    property int currentDragItemID: -1
    property int lastDragItemID: -1     //  For ignore animation
    property int currentDragItemPageIndex: -1

    ListModel{
        id: listModelDisplay
    }
    ListModel{
        id: listModel
        Component.onCompleted: {
            constructMenuModel()
            menuList.isAready = true
//            AppHomeModel.currentMenuPageIdx = 0;
            menuList.positionViewAtIndex(currentPageIndex, ListView.Beginning)
//            console.log("[3] List model "+ AppHomeModel.currentMenuPageIdx)
        }

    }

    onMaxNumPagesChanged: {
        //        console.log("Max num pages: ", maxNumPages)
    }

    onCustomizeModeChanged: {
        if (customizeMode == false)
        {
            if(anim.running)
            {
                console.log("[3] Cancel Animation of Page")
                anim.complete()
            }
            if (currentDragItemID != -1)
            {
                console.log("Exit customize while dragging.")
                return;
            }
            AppHomeModel.updateDisplayList(nbrItemsPerPage)
            updateDisplayList()
            menuList.positionViewAtIndex(currentPageIndex, ListView.Beginning)
            menuList.isArrowVisible = true
        }
        if (customizeMode == true && rearrangeLoader.sourceComponent == undefined)
        {
            console.log("QCustomizedMenuListAllowEmpty.qml: Load one grid view for customize mode.")
            rearrangeLoader.sourceComponent = oneGridComponent
        }
    }
    function updateHistoryPage(){
        console.log("updateHistoryPage");
        if(currentPageIndex === 2 && totalPagesDisplay === 2)
        {
            console.log("updateHistoryPage invalid currentPageIndex -> reset to 1")
            root.pageIndexChanged(1)
            menuList.currentIndex = 1
        } else if (currentPageIndex >= totalPagesDisplay && totalPagesDisplay === 1)
        {
            console.log("updateHistoryPage invalid currentPageIndex -> reset to 0")
            root.pageIndexChanged(0)
            menuList.currentIndex = 0
        } else if(menuList.currentIndex !== currentPageIndex){
           menuList.currentIndex = currentPageIndex
        }
        menuList.isAready = true
        menuList.positionViewAtIndex(currentPageIndex, ListView.Beginning)
    }
//    function updateHistoryPageReset(){
//        console.log("updateHistoryPageReset");
//        menuList.isAready = true
//        menuList.positionViewAtIndex(currentPageIndex, ListView.Beginning)
//    }

    function lockCurrentIndexChanged(){
        menuList.isAready = false
    }

    function updateDisplayList()
    {
        // Update display list model for non-customize mode
        listModelDisplay.clear()
        var index = 0;
        var list;
        do {
            list = getPageListModel(index, false)
            listModelDisplay.append({"pageData" : list})
            index++
        }
        while (!list.isLastPage())

        totalPagesNormalMode = listModelDisplay.count
    }

Component {
    id: oneGridComponent
    // Use GridView of total items for easy drag/drop animation. Its delegates receive DropArea and trigger handle drag/drop logic.
    QMenuPageTotal {
        id: oneGrid
        isFullMode: root.isFullMode
        isWideScreen: root.isWideScreen
        currentPageIdx: currentPageIndex
        itemCountPerPage: nbrItemsPerPage
        imgPath: prefixImg
        pageIndex: 0
        pageModel: AllMenuTotal
        width: k_itemWidth * k_nbrOfColumns
        height: 417*4
        parentView: menuList
        rootView: root
        cellWidth: isFullMode && isWideScreen? k_itemWidth_custo : k_itemWidth_custo_s
        cellHeight: 208.5
        customizeMode: root.customizeMode
        interactive: false
        visible: customizeMode && menuList.isAready ? true : false
        enabled: true
        x: (isFullMode && isWideScreen ? 144: 140)
        y: - currentPageIdx * 417
        clip: false

        onInterDrop: {
            console.log("Interdrop from ", firstDragIndex, " to ", currentDropIndex)
            var hasChanges = doInterDrop(AllMenuTotal)
            if (hasChanges)
            {
                syncListFromAllMenuTotal()
            }

        }
    }
}

ListView{
    id: idBG
    y: menuList.y
    currentIndex: menuList.currentIndex
    spacing: !customizeMode? 0 : (isFullMode && isWideScreen? -191 : -189)
    width:isFullMode && isWideScreen? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: 540
    orientation: ListView.Horizontal
    flickDeceleration: 50000
    boundsBehavior: Flickable.StopAtBounds
    snapMode: ListView.SnapOneItem
    highlightRangeMode: ListView.StrictlyEnforceRange
    preferredHighlightBegin: 0
    preferredHighlightEnd: 0
    cacheBuffer : menuList.width*(totalPagesDisplay + 1) + 1
    displayMarginBeginning: menuList.width*(totalPagesDisplay+1)
    model: menuList.count

    contentX: menuList.contentX

    delegate:
        Item {
        id: gridDelegateBG
        width: isFullMode && isWideScreen? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
        height: 540

        Q_ThemeImage {
            id: custoBgDumy
            themeSource: isFullMode && isWideScreen? defineModel.home_bg_horizontal : defineModel.home_bg_horizontal_c
            visible: custoBGVisible && /*index === 0 &&*/ customizeMode
            anchors{
                top: parent.top
                topMargin: 255 - ConstantModel.MENU_BAR_Y * 2
                left: parent.left
                leftMargin: isFullMode && isWideScreen? 142 : 141
            }
            width: sourceSize.width
            height: sourceSize.height
            MouseArea {
                anchors.fill: parent
            }
        }
    }
}

    Loader {
        id: rearrangeLoader
        sourceComponent: undefined
        asynchronous: false
        width: k_itemWidth * k_nbrOfColumns
        height: 417 * totalPagesDisplay * 2
        anchors.top: menuList.top
        anchors.topMargin: (255 - ConstantModel.MENU_BAR_Y * 2)
        visible: customizeMode
    }

    function proceedItemDropped(colorModel)
    {
        console.log("firstDragIndex: " + firstDragIndex)
        console.log("currentDropIndex: " + currentDropIndex)
        var dragItem = colorModel.getItem(firstDragIndex);
        var dropItem = colorModel.getItem(currentDropIndex);

        var ret = false;

        if (firstDragIndex != currentDropIndex)
        {

            // Revert last drop if any
            if (lastDragIndex != -1 && lastDropIndex != -1)
            {
                revertDrop(colorModel, lastDragIndex, lastDropIndex, lastEmptyIndex)
                firstDragIndex = lastDragIndex
                lastDragIndex = -1
                lastDropIndex = -1
                lastEmptyIndex = -1
            }

            // Do drop

            if ((firstDragIndex != currentDropIndex) && colorModel.getItem(currentDropIndex).isEditingVisible() == false)
            {
                console.log("[3] white instance drop.")
                console.log("[3] swap between ", firstDragIndex ," and ", currentDropIndex)
                if (firstDragIndex < currentDropIndex)
                {
                    colorModel.moveIcon(firstDragIndex, currentDropIndex, 1)
                    colorModel.moveIcon(currentDropIndex -1 , firstDragIndex, 1)
                }
                else
                {
                    colorModel.moveIcon(currentDropIndex, firstDragIndex, 1)
                    colorModel.moveIcon(firstDragIndex -1 , currentDropIndex, 1)

                }
            }
            else if (firstDragIndex != currentDropIndex)
            {
                proceedDropInPage(colorModel, firstDragIndex, currentDropIndex)
                firstDragIndex = currentDropIndex
            }

            // has changes
            ret = true
        }

        console.log("FINISH DROP")
        resetDragDropStates()

        return ret;
    }

    // Use current listView with opacity = 0 to accept flick/change Page/pressing/start drag action.
    QListViewHomePage {
        id: menuList
        property bool isArrowVisible: true
        y: ConstantModel.MENU_BAR_Y
        spacing: !customizeMode? 0 : (isFullMode && isWideScreen? -191 : -189)
        width:isFullMode && isWideScreen? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
        height: 540
        orientation: ListView.Horizontal
        flickDeceleration: 50000
        boundsBehavior: Flickable.StopAtBounds
        snapMode: ListView.SnapOneItem
        highlightRangeMode: ListView.StrictlyEnforceRange
        preferredHighlightBegin: 0
        preferredHighlightEnd: 0
        cacheBuffer : menuList.width*(totalPagesDisplay + 1) + 1
        displayMarginBeginning: menuList.width*(totalPagesDisplay+1)
//      spacing: (customizeMode && !isFullMode && totalPagesDisplay > 1) ? (-4) : 0 // Adjust spacing between grids to map background rectangle
        model: AllMenuPage1.allMenuScreenOwner != isAllMenuScreen ? undefined : customizeMode ? listModel : listModelDisplay
        interactive: totalPagesDisplay > 1
        pressDelay: totalPagesDisplay > 1 ? 100 : 0
        delegate:
            Item {
            id: gridDelegate
            width: isFullMode && isWideScreen? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
            height: 540
            MouseArea {
                property bool _isClickOutSide: false
                property real posDownX: 0
                property real posUpX: 0
                property real posDownY: 0
                property real posUpY: 0

                anchors.fill: parent
                visible: index === menuList.currentIndex && root.customizeMode
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
                    var deltaX = Math.abs(posUpX - posDownX)
                    var deltaY = Math.abs(posUpY - posDownY)
                   if(_isClickOutSide && (deltaX < 20 && deltaY < 20))
                    {
                        QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
                        clickOutSide()
                    }
                }
            }

            Q_ThemeImage {
                id: custoBg
//                themeSource: isFullMode && isWideScreen? defineModel.home_bg_horizontal : defineModel.home_bg_horizontal_c
                source: ""
                visible: custoBGVisible && /*index === 0 &&*/ customizeMode
                anchors{
                    top: parent.top
                    topMargin: 255 - ConstantModel.MENU_BAR_Y * 2
                    left: parent.left
                    leftMargin: isFullMode && isWideScreen? 142 : 141
                }
                width: isWideScreen? 1634 : 998 //sourceSize.width
                height: 416 //sourceSize.height
//                MouseArea {
//                    anchors.fill: parent
//                }
            }

            Q_ThemeImage {
                id: arrow_left
                anchors {
                    verticalCenter: custoBg.verticalCenter
                    right: custoBg.left
                    rightMargin: 26
                }
                themeSource: defineModel.home_bg_horizontal_arrow_l
                visible: !isDragging && customizeMode && ((currentPageIndex === 1 && index !== 2 && index !== 0) || currentPageIndex === 2) /*&& menuList.isArrowVisible && (menuList.contentX === (menuList.width + menuList.spacing) * menuList.currentIndex)*/
                MouseArea{
                    anchors.fill: parent
                    onClicked: {
                        console.log("press left !!!!")
                    }
                }
            }

            MouseArea{
                width: arrow_left.visible ? (isWideScreen? 51 : 48) : 0
                height: 415
                anchors {
                    verticalCenter: custoBg.verticalCenter
                    right: arrow_left.left
                    rightMargin: 27
                }
                onClicked: {
                    console.log("press left area !!!!")
                }
            }

            Q_ThemeImage {
                id: arrow_right
                anchors {
                    verticalCenter: custoBg.verticalCenter
                    left: custoBg.right
                    leftMargin: 26
                }
                themeSource: defineModel.home_bg_horizontal_arrow_r
                visible: !isDragging && customizeMode && ((totalPagesDisplay === 2 && currentPageIndex === 0) || (totalPagesDisplay === 3 && ((currentPageIndex === 1 && index !== 0) || currentPageIndex == 0))) /*&& menuList.isArrowVisible && (menuList.contentX === (menuList.width + menuList.spacing) * menuList.currentIndex)*/
                MouseArea{
                    anchors.fill: parent
                    onClicked: {
                        console.log("press right !!!!")
                    }
                }
            }
            MouseArea{
                width: arrow_right.visible ? (isWideScreen? 51 : 48) : 0
                height: 415
                anchors {
                    verticalCenter: custoBg.verticalCenter
                    left: arrow_right.right
                    leftMargin: 27
                }
                onClicked: {
                    console.log("press right area !!!!")
                }
            }
            QItem {
                 id: img_hl_right
                 width: isWideScreen? 51 : 45
                 height: isWideScreen? 417 : 416
                 visible: !isListFlicking && customizeMode && ((totalPagesDisplay === 2 && currentPageIndex === 0) || (totalPagesDisplay === 3 && ((currentPageIndex === 1 && index !== 0) || currentPageIndex == 0))) /*&& menuList.isArrowVisible && (menuList.contentX === (menuList.width + menuList.spacing) * menuList.currentIndex)*/
                 anchors {
                     verticalCenter: custoBg.verticalCenter
                     left: arrow_right.right
                     leftMargin: isWideScreen? 29 : 32
                 }

                 Q_ThemeImage {
                     width: sourceSize.width
                     height: sourceSize.height
                     anchors.centerIn: parent
                     themeSource: isFullMode && isWideScreen? defineModel.home_bg_horizontal_highlight_r : defineModel.home_bg_horizontal_highlight_r_1280
                     visible: (isFullMode && isWideScreen) ? isOverLap : (isOverLap && !isXLessThanZero)
                 }
             }

            QItem {
                id: img_hl_left
                width: isWideScreen? 51 : 45
                height: isWideScreen? 417 : 416
                visible: !isListFlicking && customizeMode && ((currentPageIndex === 1 && index !== 2 && index !== 0) || currentPageIndex === 2) /*&& menuList.isArrowVisible && (menuList.contentX === (menuList.width + menuList.spacing) * menuList.currentIndex)*/
                anchors {
                    verticalCenter: custoBg.verticalCenter
                    right: arrow_left.left
                    rightMargin: isWideScreen? 29 : 32
                }

                Q_ThemeImage {
                    width: sourceSize.width
                    height: sourceSize.height
                    anchors.centerIn: parent
                    themeSource: isFullMode && isWideScreen? defineModel.home_bg_horizontal_highlight_l : defineModel.home_bg_horizontal_highlight_l_1280
                    visible: (isFullMode && isWideScreen) ? isOverLap : (isOverLap && isXLessThanZero)
                }
            }

            QMenuPageAllowEmpty{
                id: menuPage
                isFullMode: root.isFullMode
                isWideScreen: root.isWideScreen
                imgPath: prefixImg
                pageIndex: index
                pageModel: pageData
                width: k_menuPageWidth
                height: k_menuPageHeight
                parentView: menuList
                rootView: root
                cellWidth: k_itemWidth
                cellHeight: k_itemHeight
                customizeMode: root.customizeMode
                anchors{
                    top: parent.top
                    topMargin: !customizeMode? 0 : (255 - ConstantModel.MENU_BAR_Y * 2)
                    horizontalCenter: !customizeMode? parent.horizontalCenter : custoBg.horizontalCenter
                    horizontalCenterOffset: !customizeMode? (!isFullScreen? 14 : 0) : (!isFullScreen? -1 : 0)
                }
                interactive: false
                visible: true
                enabled: index === menuList.currentIndex || !customizeMode

                MouseArea {
                    id: underScreen
                    anchors.fill: parent
                    z: -9999
                    onPressed: {
                        enabled = false;
                    }
                    onReleased: {
                        enabled = true;
                    }
                }

                onItemDropped: {
//                  console.log("dragItemID: " + dragItemIndex, "of page ", dragItemPage, "  drop  " + dropItemIndex, "of page ", dropItemPage)
                    idNext.enabled = false
                    idBack.enabled = false
                    idEnableDropAreaTimer.start()
                    proceedItemDropped(AllMenuTotal)
                    updateListFromAllMenuTotal();
                    AppHomeModel.saveAllMenuDB(isAllMenuScreen);
//                    AppHomeModel.setIsEditedAllMenu(true) /* to send VRCM */
                    tmrReleaseDropArea.start()
                    currentDragItemID = -1
                }
                onInterDrop: {
                    console.log("Moving Interdrop from ", firstDragIndex, " to ", currentDropIndex)
                    var hasChanges = doInterDrop(AllMenuTotal)
                    if (hasChanges)
                    {
                        syncListFromAllMenuTotal()
                    }
                }

                onItemClicked: {
                    if( !customizeMode ){
                        root.menuItemClicked(_itemID, _tonedown)
                    }
                }
                onDraggingChanged: {
                    // prevent list interaction while dragging
//                    menuList.interactive = !dragging
                }
                onItemPressAndHold: {
                    activeDragAndDrop()
                }

                onDragStarted: {
                    console.log("onDragStarted dragItemId: ", dragItemId)
                    console.log("onDragStarted pageIndex: ", dragPageIndex)
                    currentDragItemID = dragItemId
                    lastDragItemID = dragItemId
                    currentDragItemPageIndex = dragPageIndex
                    console.log("onDragStarted end! ")
                }

                states: [
                    State {
                        name: "customize"
                        when: customizeMode
                        PropertyChanges {
                            target: menuPage
                            width: isFullMode && isWideScreen? k_itemWidth_custo*6 : k_itemWidth_custo_s*4
                            height: 417 //209 * 2
                            cellWidth: isFullMode && isWideScreen? k_itemWidth_custo : k_itemWidth_custo_s
                            cellHeight: 208.5
                        }
                    }
                ]
            }
            Timer{
                id: backPageTimer
                interval: 200
                running: false
                repeat: false
                onTriggered: nextPageRequested(menuPage.pageIndex, menuPage.pageIndex-1, draggingIndex)
                onRunningChanged: {
                    console.log("[3] backPageTimer.running = ", running)
                }
            }
            DropArea{
                id: idBack
                anchors.left: img_hl_left.left
                anchors.leftMargin: 10
                anchors.top: img_hl_left.top
                width: 70

                height: menuPage.height
                visible: customizeMode && ((currentPageIndex === 1 && index !== 2 && index !== 0) || currentPageIndex === 2) && currentDragItemID != -1

                onEntered: {
//                    console.log("[3] idBack DropAreaonEntered")
                    backPageTimer.restart()
                }
                onExited: {
//                    console.log("[3] idBack DropAreaonExited")
                    backPageTimer.running = false
                }
            }

            Timer{
                id: nextPageTimer
                interval: 200
                running: false
                repeat: false
                onTriggered: nextPageRequested(menuPage.pageIndex, menuPage.pageIndex+1, draggingIndex)
                onRunningChanged: {
//                  console.log("[3] nextPageTimer.running = ", running)
                }
            }

            // Timer to fix issue of DropArea auto enter after icon is changed.
            Timer {
                id: idEnableDropAreaTimer
                interval: 100
                onTriggered: {
                    idNext.enabled = true
                    idBack.enabled = true
                }
            }

            DropArea{
                id: idNext
                anchors.left: img_hl_right.left
                anchors.leftMargin: -10
                anchors.top: img_hl_right.top
                width: 70
                height: menuPage.height
                visible: customizeMode && ((totalPagesDisplay === 2 && currentPageIndex === 0) || (totalPagesDisplay === 3 && ((currentPageIndex === 1 && index !== 0) || currentPageIndex == 0))) && currentDragItemID != -1

                onEntered: {
                    console.log("[3] idNext DropAreaonEntered")
                    nextPageTimer.restart()
                }
                onExited: {
                    console.log("[3] idNext DropAreaonExited")
                    nextPageTimer.running = false
                }
            }
        }

        property bool isAready : false

        onMovementStarted: {
            isListFlicking = true
            if(root.customizeMode) {
                menuList.isArrowVisible = false
            }
        }
        onMovementEnded: {
            isListFlicking = false
            console.log("[3] Check postion: isFullMode: " + isFullMode + "  menuList.contentX: " + menuList.contentX + "  width: " + menuList.width + " Spacing: " + menuList.spacing)
            if(root.customizeMode) {
                menuList.isArrowVisible = true
            }
        }

        onDragStarted: {
            isDragging = true
        }

        onDragEnded: {
            isDragging = false
        }

        Component.onCompleted: {
//            AppHomeModel.currentMenuPageIdx = 0
            console.log("[3] Listview Component.onCompleted")
        }
        onCurrentIndexChanged: {
          console.log("onCurrentIndexChanged currentIndex: ",menuList.currentIndex, ", isAready: ", isAready)
            if(isAready == true){
//                AppHomeModel.currentMenuPageIdx = menuList.currentIndex
                console.log("[3] AppHomeModel.currentIndex onPageIndexChanged" + AppHomeModel.currentMenuPageIdx)
                root.pageIndexChanged(menuList.currentIndex)
                console.log("[3] onCurrentIndexChanged" + menuList.currentIndex)
            }
        }
        onWidthChanged: {
            countWidthChange++;
            if(countWidthChange >=2 ){
                isCheck = true
            }
        }
        Connections {
            target: AppHomeModel
            onIsAtOtherAppChanged: {
                console.log("[3] ThanhDV10 returnToBounds: " + menuList.currentIndex)
                menuList.positionViewAtIndex(menuList.currentIndex, ListView.Beginning)
                if(isListFlicking)
                {
                    isListFlicking = false
                    menuList.isArrowVisible = true
                }
            }

            onSysPopupShowingChanged:
            {
                console.log("[3] onSysPopupShowingChanged: ")
                if(AppHomeModel.sysPopupShowing)
                {
                    menuList.returnToBounds()
                    isListFlicking = false
                    menuList.isArrowVisible = true
                }
            }
        }
    }

    Timer {
        id: tmrReleaseDropArea
        interval: 10
        repeat: false
        onTriggered: {
            console.log("tmrReleaseDropArea timeout")
            rearrangeLoader.item.notifyReleaseDrop()
        }
    }

    onIndexChange: {
        console.log("[3] onIndexChange")
        if((!isFullMode || !isWideScreen) && currentPageIndex === 2){
            root.pageIndexChanged(2)
            menuList.positionViewAtIndex(currentPageIndex, ListView.Beginning)
            menuList.currentIndex = currentPageIndex;
        }else if(currentPageIndex >= 1){
            root.pageIndexChanged(1)
            menuList.positionViewAtIndex(currentPageIndex, ListView.Beginning)
            menuList.currentIndex = currentPageIndex;
            console.log("[3] menuList.currentIndex1" + menuList.currentIndex)
        }else{
            root.pageIndexChanged(0)
            menuList.positionViewAtIndex(currentPageIndex, ListView.Beginning)
            menuList.currentIndex = currentPageIndex;
            console.log("[3] menuList.currentIndex0" + menuList.currentIndex)
        }
    }

    onIsFullModeChanged: {
        if(menuList.moving /*&& menuList.flicking*/)
        {
            menuList.cancelFlick()
        }
    }

    function doInterDrop(colorModel)
    {

        console.log("firstDragIndex: " + firstDragIndex)
        console.log("currentDropIndex: " + currentDropIndex)

        var modelChanged = false

        if (firstDragIndex != currentDropIndex)
        {
            // Revert last drop if any
            if (lastDragIndex != -1 && lastDropIndex != -1)
            {
                revertDrop(colorModel, lastDragIndex, lastDropIndex, lastEmptyIndex)
                firstDragIndex = lastDragIndex
                lastDragIndex = -1
                lastDropIndex = -1
                lastEmptyIndex = -1

                modelChanged = true
            }

            // Do drop
            if (colorModel.getItem(currentDropIndex).isEditingVisible() == false)
            {
                console.log("[3] white instance drop -> ignore")

            }
            else if (firstDragIndex != currentDropIndex)
            {
                if (lastDragIndex != -1 && lastDropIndex != -1)
                {
                    revertDrop(colorModel, lastDragIndex, lastDropIndex, lastEmptyIndex)
                    firstDragIndex = lastDropIndex
                    lastDragIndex = -1
                    lastDropIndex = -1
                    lastEmptyIndex = -1
                }

                proceedDropInPage(colorModel, firstDragIndex, currentDropIndex)
                firstDragIndex = currentDropIndex
                modelChanged = true
            }
        }

        return modelChanged;
    }
    function findEmptyIndex(page, fromIndex, dragIndex)
    {
        var index = -1;
        if (!dragIndex)
        {
            dragIndex = -1
        }

        for (var i = fromIndex; i < page.getCount(); i++)
        {
            var item = page.getItem(i);
            console.log("check item:", item.label)
            if (!item.isEditingVisible() || i == dragIndex)
            {
                console.log("[3] findEmptyIndex: found forward emptyIndex = ", i);
                index = i;
                break;
            }
        }
        return index;
    }

    function proceedDropInPage(modelList, dragIndex, dropIndex)
    {
        if (dragIndex == dropIndex)
            return;

        console.log("---------------START proceedDrop-----------------")

        var itmEmpty = -1;
        for (var i = dropIndex -1; i >= dragIndex + 1; i--)
        {
            var item = modelList.getItem(i);
            if (item.isEditingVisible() == false || i == dragIndex)
            {
                console.log("[3] TanBT: emptyIndex = ", i);
                itmEmpty = i;
                break;
            }
        }


        console.log("[3] itmEmpty:", itmEmpty);
        console.log("[3] proceedDrop: action");

        if (dragIndex < dropIndex)
        {
            console.log("[3] TANBT 3 edit: dragIndex: " + dragIndex + " dropIndex: " + dropIndex + "itmEmpty: " + itmEmpty)
            modelList.moveIcon(dragIndex, dropIndex, 1)

            //move item to new posiontion and between old possition and new possition have empty item
            if(itmEmpty != -1)
            {
                console.log("[3] TANBT 32 edit: dragIndex: " + dragIndex + " dropIndex: " + dropIndex + "itmEmpty: " + itmEmpty)
                modelList.moveIcon(itmEmpty-1, dragIndex, 1)
            }

        }
        else
        {

            for (var i = dropIndex; i < dragIndex; i++)
            {
                var item = modelList.getItem(i);
                if (item.isEditingVisible() == false || i == dragIndex)
                {
                    console.log("[3] TanBT: forward emptyIndex = ", i);
                    itmEmpty = i;
                    break;
                }
            }

            for (var i = dropIndex; i >= 0; i--)
            {
                var item = modelList.getItem(i);
                if (item.isEditingVisible() == false || i == dragIndex)
                {
                    console.log("[3] TanBT: backward emptyIndex = ", i);
                    itmEmpty = i;
                    break;
                }
            }

            if(itmEmpty < dropIndex)
            {

                console.log("[3] TANBT 5 edit: dragIndex: " + dragIndex + " dropIndex: " + dropIndex + " itmEmpty: " + itmEmpty)
                console.log("[3] TANBT 62 edit: dragIndex: " + dragIndex + " dropIndex: " + dropIndex + "itmEmpty: " + itmEmpty)

                if(dragIndex > dropIndex)
                {
                    if (itmEmpty != -1)
                    {
                        console.log("[3] TANBT 61 edit: dragIndex: " + dragIndex + " dropIndex: " + dropIndex + " itmEmpty: " + itmEmpty)
                        modelList.moveIcon(itmEmpty, dragIndex , 1);
                        modelList.moveIcon(dragIndex-1 , dropIndex, 1)
                    }else{
                        console.log("[3] TANBT 612 edit: dragIndex: " + dragIndex + " dropIndex: " + dropIndex + " itmEmpty: " + itmEmpty)
                        modelList.moveIcon(dragIndex, dropIndex, 1)
                    }
                }else{
                    console.log("[3] TANBT 613 edit: dragIndex: " + dragIndex + " dropIndex: " + dropIndex + " itmEmpty: " + itmEmpty)
                    modelList.moveIcon(dragIndex, dropIndex+1, 1)

                    if (itmEmpty != -1)
                    {
                        console.log("[3] TANBT 614 edit: dragIndex: " + dragIndex + " dropIndex: " + dropIndex + " itmEmpty: " + itmEmpty)
                        modelList.moveIcon(itmEmpty, dragIndex, 1);
                    }
                }
            }else{
                console.log("[3] TANBT 63 edit: dragIndex: " + dragIndex + " dropIndex: " + dropIndex + " itmEmpty: " + itmEmpty)
                modelList.moveIcon(dragIndex, dropIndex, 1)
                if (itmEmpty != -1)
                {
                    console.log("[3] TANBT 64 edit: dragIndex: " + dragIndex + " dropIndex: " + dropIndex + " itmEmpty: " + itmEmpty)
                    modelList.moveIcon(itmEmpty+1, dragIndex, 1);
                }
            }
        }
        lastDragIndex = dragIndex
        lastDropIndex = dropIndex
        lastEmptyIndex = itmEmpty //iEmptyIndex
    }



    function revertDrop(modelList, dragIndex, dropIndex, emptyIndex)
    {
        console.log("---------------START revertDrop(", dragIndex, "|", dropIndex, "|", emptyIndex, ")-----------------")
        console.log("[3] TANBT 14 edit: dropIndex: " + dropIndex + " dragIndex: " + dragIndex + " emptyIndex: " + emptyIndex)
        if (emptyIndex > dragIndex)
        {
            if (emptyIndex > dropIndex)
            {
                console.log("[3] TANBT 8 edit: dragIndex: " + dragIndex + " emptyIndex: " + emptyIndex)
                modelList.moveIcon(dragIndex, emptyIndex, 1)
                console.log("[3] TANBT 9 edit: dropIndex: " + dropIndex + " dragIndex: " + dragIndex)
                modelList.moveIcon(dropIndex - 1, dragIndex, 1)
            }else{
                console.log("[3] TANBT 10 edit: dragIndex: " + dragIndex + " emptyIndex - 1: " + (emptyIndex - 1))
                modelList.moveIcon(dragIndex, emptyIndex - 1, 1)
                console.log("[3] TANBT 11 edit: dropIndex: " + dropIndex + " dragIndex: " + dragIndex)
                modelList.moveIcon(dropIndex, dragIndex, 1)
            }
        }else{
//            modelList.move(dragIndex, emptyIndex + 1, 1)
//            modelList.move(dropIndex, dragIndex, 1)
            if (emptyIndex > dropIndex)
            {
                console.log("[3] TANBT 81 edit: dragIndex: " + dragIndex + " emptyIndex: " + emptyIndex)
                modelList.moveIcon(dragIndex, emptyIndex + 1, 1)
                console.log("[3] TANBT 91 edit: dropIndex: " + dropIndex + " dragIndex: " + dragIndex)
                modelList.moveIcon(dropIndex, dragIndex, 1)
            }else{
                if(emptyIndex != -1)
                {
                    console.log("[3] TANBT 101 edit: dragIndex: " + dragIndex + " emptyIndex: " + emptyIndex)
                    modelList.moveIcon(dragIndex, emptyIndex, 1)
                    console.log("[3] TANBT 111 edit: dropIndex: " + dropIndex + " dragIndex: " + dragIndex)
                    modelList.moveIcon(dropIndex + 1 , dragIndex, 1)
                }else{
                    console.log("[3] TANBT 112 edit: dropIndex: " + dropIndex + " dragIndex: " + dragIndex)
                    modelList.moveIcon(dropIndex , dragIndex, 1)
                }
            }
        }
    }

    NumberAnimation { id: anim; target: menuList; property: "contentX"; duration: 200 }
    function nextPageRequested(srcPage,dstPage,itemIndex){
        var curPageIdx = menuList.currentIndex
        console.log("[3] nextPageRequested, curPageIdx: ",curPageIdx, ", srcPage: ", srcPage, " - dstPage: ", dstPage)
        var nextPage =  curPageIdx + (dstPage-srcPage)
        if( nextPage < 0 || nextPage >= totalPages )
        {
            return
        }
        if( nextPage != curPageIdx ){
            var pos = menuList.contentX;
            var destPos;
            menuList.positionViewAtIndex(nextPage, ListView.Beginning)
            destPos = menuList.contentX;
            anim.from = pos;
            anim.to = destPos;
            anim.running = true;

        }
    }
    function requestResetRerrange()
    {
        console.log("requestResetRerrange")

        // Unload Rearrange Page GUI
        rearrangeLoader.sourceComponent = undefined

        // Reset drag/drop states
        resetDragDropStates()

        // Rebuild Menu model
        constructMenuModel()

        // Load new Rearrange Page GUI
        rearrangeLoader.sourceComponent = oneGridComponent
    }

    function resetDragDropStates()
    {
        console.log("resetDragDropStates")
        firstDragIndex = -1
        currentDropIndex = -1
        lastDragIndex = -1
        lastDropIndex = -1
        lastEmptyIndex = -1
        currentDragItemID = -1
    }

    function constructMenuModel(){
        console.log("[3] call constructMenuModel")
        resetAllPages();
        totalPages = 0
        totalPagesNormalMode = 0
        listModel.clear()
        listModelDisplay.clear()
        var listMenus = lstModel
        var i=0
        while( i < listMenus.length)
        {
            var list = [], itemIdx = 0
            while( itemIdx < nbrItemsPerPage && i < listMenus.length)
            {
                var item = listMenus[i]
                item["pos"] = i
                item["isdummy"] = false
//              if( item.visible ){
                list.push(item)
                itemIdx++
//                }
                i++
            }
            if( list.length > 0 ){
                var customizePageListModel = getPageListModel(totalPages, true);
                customizePageListModel.allMenuScreenOwner = isAllMenuScreen;
                customizePageListModel.updateData(list);
                listModel.append({"pageData" : customizePageListModel})
                totalPages++
                customizePageListModel.setIsLastPage(totalPages == maxNumPages)
                customizePageListModel.setIsFirstPage(totalPages == 1)
            }
            console.log("nbrItemsPerPage: ",nbrItemsPerPage,", totalPage: ", totalPages)
        }

        // Fill empty pages
        while (totalPages < maxNumPages)
        {
            list = []
            customizePageListModel = getPageListModel(totalPages, true);
            customizePageListModel.updateData(list);
            listModel.append({"pageData" : customizePageListModel})
            totalPages++
            customizePageListModel.setIsLastPage(totalPages == maxNumPages)
            customizePageListModel.setIsFirstPage(totalPages == 1)
        }
        if (customizeMode == false)
        {
            AppHomeModel.updateDisplayList(nbrItemsPerPage);
            updateDisplayList()
        }

        AppHomeModel.updateAllMenuTotal();

        indexChange()
    }

    function updateListFromAllMenuTotal(){
        console.log("[3] call updateListFromAllMenuTotal")
        resetAllPages();
        var pageCount = 0;
        listModelDisplay.clear()
        var listMenus = AllMenuTotal
        var i=0
        var totalPage = []
        var listTotal = []
        while( i < listMenus.getCount())
        {
            var list = [], itemIdx = 0
            while( itemIdx < nbrItemsPerPage && i < listMenus.getCount())
            {
                var item = listMenus.getItem(i)
                item["pos"] = i
                item["isdummy"] = false
                list.push(item)
                listTotal.push(item)
                itemIdx++
                i++
            }
            if( list.length > 0 ){
                var customizePageListModel = listModel.get(pageCount).pageData;
                customizePageListModel.allMenuScreenOwner = isAllMenuScreen;
                customizePageListModel.updateData(list);
                pageCount++
                customizePageListModel.setIsLastPage(pageCount == maxNumPages)
                customizePageListModel.setIsFirstPage(pageCount == 1)
            }
        }

        AppHomeModel.updateDisplayList(nbrItemsPerPage);
        updateDisplayList()

        indexChange()
    }

    function removeDragItemInList(checkList, checkListIndex)
    {
        if (checkListIndex == currentDragItemPageIndex)
        {
            return;
        }
        for (var i = 0; i < checkList.getCount(); i++)
        {
            if (checkList.getItem(i).itemID == currentDragItemID)
            {
                checkList.removeIcon(i)
                break;
            }
        }
    }

    function syncListFromAllMenuTotal(){

        console.log("[3] call syncListFromAllMenuTotal")

        var page1 = getPageListModel(0, true)
        var page2 = getPageListModel(1, true)
        var page3 = getPageListModel(2, true)

        removeDragItemInList(page1, 0)
        removeDragItemInList(page2, 1)
        if (totalPages == 3)
        {
            removeDragItemInList(page3, 2)
        }

        moveItemToList(page1, 0, page2, page3)
        moveItemToList(page2, 1, page1, page3)
        if (totalPages == 3)
        {
            moveItemToList(page3, 2, page1, page2)
        }


        console.log("syncListFromAllMenuTotal step2")

        syncPosition(page1, 0)
        syncPosition(page2, 1)
        if (totalPages == 3)
        {
            syncPosition(page3, 2)
        }
    }

    function syncPosition(checkList, checkListIndex)
    {
        for (var i = (checkListIndex+1) * nbrItemsPerPage - 1; i >= checkListIndex * nbrItemsPerPage; i--)
        {
            var found = false
            var item = AllMenuTotal.getItem(i);
            for (var j = 0; j < checkList.getCount(); j++)
            {
                if (checkList.getItem(j) == item)
                {
                    if (j != i)
                    {
                        checkList.moveIcon(j, i - checkListIndex * nbrItemsPerPage, 1)
                        break;
                    }
                }
            }
        }
    }

    function moveItemToList(checkList, checkListIndex, otherList1, otherList2)
    {
        console.log("moveItemToList: for page index ", checkListIndex)
        for (var i = checkListIndex * nbrItemsPerPage; i < (checkListIndex+1) * nbrItemsPerPage; i++)
        {
            var found = false
            var item = AllMenuTotal.getItem(i);

            for (var j = 0; j < checkList.getCount(); j++)
            {
                if (checkList.getItem(j) == item)
                {
                    found = true
                    break;
                }
            }
            if (!found)
            {
                for (var j = 0; j < otherList1.getCount(); j++)
                {
                    if (otherList1.getItem(j) == item)
                    {
                        checkList.insert(checkList.getCount(), item)
                        if (currentDragItemID != item.itemID || getPageListModel(currentDragItemPageIndex, true) != otherList1)
                        {
                            otherList1.removeIcon(j)
                        }
                        else
                        {
                            console.log("Hide current dragging to last pos of the list")
                            otherList1.moveIcon(j, otherList1.getCount() - 1, 1)
                        }

                        found = true
                        break;
                    }
                }
                if (!found)
                {
                    for (var j = 0; j < otherList2.getCount(); j++)
                    {
                        if (otherList2.getItem(j) == item)
                        {
                            checkList.insert(checkList.getCount(), item)
                            if (currentDragItemID != item.itemID || getPageListModel(currentDragItemPageIndex, true) != otherList2)
                            {
                                otherList2.removeIcon(j)
                            }
                            else
                            {
                                console.log("Hide current dragging to last pos of the list")
                                otherList2.moveIcon(j, otherList2.getCount() - 1, 1)
                            }

                            break;
                        }
                    }

                }

            }
        }
    }


    function getPageListModel(index, isEdit)
    {
        if (isEdit)
        {
            if (index === 0)
            {
                return AllMenuPage1;
            }
            else if (index === 1)
            {
                return AllMenuPage2;
            }
            else if (index === 2)
            {
                return AllMenuPage3;
            }
        }
        else
        {
            if (index === 0)
            {
                return AllMenuPage1Display;
            }
            else if (index === 1)
            {
                return AllMenuPage2Display;
            }
            else if (index === 2)
            {
                return AllMenuPage3Display;
            }
        }
        return undefined

    }

    function resetAllPages()
    {
        AllMenuPage1.resetData();
        AllMenuPage2.resetData();
        AllMenuPage3.resetData();
    }

    function updateItemTonedown(itemIds, itemCount, lstTarget, itemTonedown){
        if( lstTarget !== listType ){
            return;
        }
        var listItem = []
        for (var i = 0; i < itemCount; i++)
        {
            listItem.push(itemIds[i])
        }
        AppHomeModel.updateItemTonedown(listItem, itemCount, itemTonedown)
    }
    function updateItemVisible(itemId, visible)
    {
        console.log("itemId: ", itemId, " visible: ",visible);
        AppHomeModel.updateItemVisible(itemId, visible)
    }
}

