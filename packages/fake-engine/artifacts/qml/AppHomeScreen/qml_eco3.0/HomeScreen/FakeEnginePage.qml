// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// FAKE_ENGINE_PAYLOAD_260128
import QtQuick 2.0
import "../component"
import "../component/common"
import HMIEVENTS 1.0
import AppHome_Enum 1.0

// Fake engine sound (parked).  One switch: on or off.  Everything else follows
// the car: the daemon reads /rw_data/fake-engine/{speed,pedal} and shifts a
// six-speed model by itself, so the pitch rises, drops at every shift and holds
// a gear under load.  This page only shows that state.
//
//   POST /sound?action=start|stop   GET /status -> playing, gear, rpm, file
//
// The car's own power is untouched: this is the soundtrack, not the drivetrain.
Item {
    id: root
    width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y

    signal goHome()

    property bool requested: false
    property bool atOtherApp: typeof AppHomeModel !== "undefined"
                               ? AppHomeModel.isAtOtherApp : false
    visible: requested && !atOtherApp
    focus: visible
    enabled: !(typeof AppHomeModel !== "undefined"
               && AppHomeModel.sysPopupShowing)

    property double openedAt: 0
    property bool playing: false
    property int gear: 1
    property real rpm: 0
    property string engineFile: ""
    property string lastError: ""

    function autoClose() {
        if (Date.now() - openedAt < 1200) return
        close()
    }
    function open() {
        requested = true
        openedAt = Date.now()
        refresh()
    }
    function close() {
        requested = false
        goHome()
    }

    Connections {
        target: typeof AppHomeModel !== "undefined" ? AppHomeModel : null
        onButtonBackClicked: root.autoClose()
        onIsAtOtherAppChanged: if (AppHomeModel.isAtOtherApp) root.autoClose()
        onCurrentScreenChanged: root.autoClose()
        onCurrentHomeAdvancedPageIdxChanged: root.autoClose()
    }

    function refresh() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (xhr.status === 200) {
                try {
                    var d = JSON.parse(xhr.responseText)
                    playing = d.playing === true
                    gear = typeof d.gear === "number" ? d.gear : gear
                    rpm = typeof d.rpm === "number" ? d.rpm : rpm
                    engineFile = d.file || ""
                    lastError = ""
                } catch (e) { lastError = "bad answer" }
            } else {
                lastError = "http " + xhr.status
            }
        }
        xhr.open("GET", "http://127.0.0.1:8095/status")
        xhr.send()
    }

    function toggle() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status !== 200) lastError = "http " + xhr.status
                refresh()
            }
        }
        xhr.open("POST", "http://127.0.0.1:8095/sound?action=" + (root.playing ? "stop" : "start"))
        xhr.send()
    }

    Timer {
        interval: 500
        running: root.visible
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }

    Rectangle {
        anchors.fill: parent
        color: "#0B0B0B"
    }

    QText {
        x: 28
        y: 10
        text: "Engine sound"
        font.pixelSize: ConstantModel.FONT_SIZE_24
        color: "#FFFFFF"
    }
    QText {
        x: 28
        y: 40
        width: parent.width - 300
        text: root.lastError.length ? ("daemon: " + root.lastError)
              : (root.playing ? ("Gear " + root.gear + "  \u00b7  " + Math.round(root.rpm) + " rpm")
                              : "Off")
        font.pixelSize: ConstantModel.FONT_SIZE_18
        color: root.lastError.length ? "#FFB300" : "#9A9A9A"
        elide: Text.ElideRight
    }

    Rectangle {
        id: homeButton
        anchors.right: parent.right
        anchors.rightMargin: 24
        y: 8
        width: 140
        height: 50
        radius: 25
        color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
        border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
        border.width: 1
        QText {
            anchors.centerIn: parent
            text: "\u2039  Home"
            font.pixelSize: ConstantModel.FONT_SIZE_21
            color: HomeThemeHelper.normalTextColor
        }
        MouseArea {
            anchors.fill: parent
            onReleased: root.close()
        }
    }

    Rectangle {
        x: 0
        y: 110
        width: parent.width - 48
        height: 220
        radius: 14
        color: "#141414"
        border.color: "#3A3A3A"
        border.width: 1

        QText {
            x: 28
            y: 26
            text: root.playing ? "Engine is running" : "Engine is off"
            font.pixelSize: 34
            color: "#FFFFFF"
        }
        QText {
            x: 28
            y: 74
            width: parent.width - 340
            text: root.playing
                  ? "Following speed and pedal: the gear model shifts by itself, and the pitch drops at every shift."
                  : "Switch it on: from then on it follows speed and pedal by itself."
            font.pixelSize: ConstantModel.FONT_SIZE_18
            color: "#9A9A9A"
            wrapMode: Text.WordWrap
        }
        QText {
            x: 28
            y: 150
            text: root.engineFile
            font.pixelSize: ConstantModel.FONT_SIZE_15
            color: "#5A5A5A"
            elide: Text.ElideRight
            width: parent.width - 340
        }

        Rectangle {
            id: engineSwitch
            anchors.right: parent.right
            anchors.rightMargin: 28
            anchors.verticalCenter: parent.verticalCenter
            width: 150
            height: 84
            radius: 42
            color: root.playing ? "#2E7D32" : ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            Rectangle {
                width: parent.height - 12
                height: width
                radius: width / 2
                y: 6
                x: root.playing ? parent.width - width - 6 : 6
                color: "#FFFFFF"
            }
            QText {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: root.playing ? -26 : 26
                text: root.playing ? "ON" : "OFF"
                font.pixelSize: 20
                color: root.playing ? "#FFFFFF" : "#DADADA"
            }
            MouseArea {
                anchors.fill: parent
                onReleased: root.toggle()
            }
        }
    }
}
