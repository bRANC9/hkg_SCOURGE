// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// AMBIENT_PAYLOAD_260128
import QtQuick 2.0
import "../component"
import "../component/common"
import HMIEVENTS 1.0
import AppHome_Enum 1.0

Item {
    id: root
    width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y

    signal goHome()

    // ---- foreground / home-key handling --------------------------------
    // Same contract as the Battery overlay: only visible while the home
    // screen owns the foreground, and closed by every navigation event and
    // by the session-bus Home/Star watcher in the daemon (home_seq).
    property bool requested: false
    property bool atOtherApp: typeof AppHomeModel !== "undefined"
                               ? AppHomeModel.isAtOtherApp : false
    property int homeSeq: 0
    property int homeSeqBaseline: -1
    property string homeSource: "none"

    visible: requested && !atOtherApp
    focus: visible
    enabled: !(typeof AppHomeModel !== "undefined"
               && AppHomeModel.sysPopupShowing)

    property double openedAt: 0
    // Stale AppHomeModel flags from the replaced home screen used to close the
    // overlay right after open; only honour auto-close after a short grace.
    function autoClose() {
        if (Date.now() - openedAt < 1200) return
        close()
    }
    function open() {
        requested = true
        homeSeqBaseline = -1
        openedAt = Date.now()
        refresh()
    }
    function close() {
        requested = false
    }

    Connections {
        target: (typeof AppHomeModel !== "undefined") ? AppHomeModel : null
        onButtonBackClicked: root.autoClose()
        onIsAtOtherAppChanged: if (AppHomeModel.isAtOtherApp) root.autoClose()
        onCurrentScreenChanged: root.autoClose()
        onCurrentHomeAdvancedPageIdxChanged: root.autoClose()
        onRequestHomeByPPChanged: if (AppHomeModel.requestHomeByPP) root.autoClose()
    }
    // Close only when home_seq changes after open (see the parse site).

    // Factory-style back chevron, same place and weight as the stock Notifications
    // page: thin "<" top-left, closes the overlay exactly like Exit.
    Item {
        objectName: "ambientBack"
        z: 10
        // Launcher header slot: the page starts at MENU_BAR_Y, so y:-40 puts the
        // chevron on the header row the stock Notifications page uses.
        x: 132
        y: -40
        width: 52
        height: 52
        Rectangle { anchors.fill: parent; radius: 26; color: bm2.pressed ? "#3A3A3A" : "transparent" }
        QText {
            anchors.centerIn: parent
            text: "\u2039"
            font.pixelSize: 54
            textBold: true
            color: "#D8D8D8"
        }
        MouseArea { id: bm2; anchors.fill: parent; preventStealing: true; onClicked: root.close() }
    }
    // ---- daemon state --------------------------------------------------
    property bool serviceOk: false
    // Result of the on-demand factory-ambient capture, straight from the daemon:
    // "did the round find anything" must be visible, otherwise a broken filter
    // looks exactly like "the light is not on D-Bus".
    property string captureState: "idle"
    property int captureEvents: 0
    property int captureLight: 0
    function captureText() {
        if (captureState === "running") return "capture: running - toggle a light now"
        if (captureState === "done")
            return "capture: " + captureEvents + " events, " + captureLight + " light-ish"
        return "capture: idle"
    }
    property bool power: true
    property string effect: "breathe"
    property real hue: 0.62
    property real sat: 0.83
    property real val: 1.0
    property var rgb: [43, 108, 255]
    property int brightness: 70
    property int speed: 45
    property int intensity: 50
    property int transitionMs: 800
    property string transitionStyle: "crossfade"
    property bool reactiveOn: false
    property int audioGain: 110
    property bool audioEnabled: false
    property string audioSource: "none"
    property real audioLevel: 0
    property real audioBass: 0
    property real audioMid: 0
    property real audioHigh: 0
    property int beats: 0
    property string backend: "none"
    property string target: ""
    property string lastError: ""
    property string daemonVersion: ""
    property var presetNames: []


    function applyStatus(d) {
        power = d.power === true
        effect = d.effect || "breathe"
        var c = d.color
        if (c && c.length === 3) rgb = [c[0], c[1], c[2]]
        var hsv = d.color_hsv
        if (hsv && hsv.length === 3) {
            hue = hsv[0]
            sat = hsv[1]
            val = hsv[2]
        }
        brightness = typeof d.brightness === "number" ? d.brightness : 70
        speed = typeof d.speed === "number" ? d.speed : 45
        intensity = typeof d.intensity === "number" ? d.intensity : 50
        transitionMs = typeof d.transition_ms === "number" ? d.transition_ms : 800
        transitionStyle = d.transition_style || "crossfade"
        reactiveOn = d.reactive === true
        audioGain = typeof d.audio_gain === "number" ? d.audio_gain : 110
        daemonVersion = d.version || ""
        var _seq = typeof d.home_seq === "number" ? d.home_seq : 0
        if (homeSeqBaseline < 0 || _seq < homeSeqBaseline) homeSeqBaseline = _seq
        else if (_seq > homeSeqBaseline) {
            homeSeqBaseline = _seq
            if (requested) close()
        }
        homeSeq = _seq
        homeSource = d.home_source || "none"
        backend = d.backend || "none"
        target = d.target || ""
        lastError = d.last_error || ""
        presetNames = d.presets || []
        if (d.capture !== undefined) captureState = "" + d.capture
        if (d.capture_events !== undefined) captureEvents = d.capture_events
        if (d.capture_light_hits !== undefined) captureLight = d.capture_light_hits
        var a = d.audio
        if (a) {
            audioEnabled = a.enabled === true
            audioSource = a.source || "none"
            audioLevel = typeof a.level === "number" ? a.level : 0
            audioBass = typeof a.bass === "number" ? a.bass : 0
            audioMid = typeof a.mid === "number" ? a.mid : 0
            audioHigh = typeof a.high === "number" ? a.high : 0
            beats = typeof a.beats === "number" ? a.beats : 0
        }
    }

    function refreshFromFile() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (xhr.status === 200 || xhr.status === 0) {
                try {
                    applyStatus(JSON.parse(xhr.responseText))
                    serviceOk = true
                } catch (e) {
                    serviceOk = false
                }
            } else {
                serviceOk = false
            }
        }
        xhr.open("GET", "file:///rw_data/ambient/status.json")
        xhr.send()
    }

    function refresh() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (xhr.status === 200) {
                try {
                    applyStatus(JSON.parse(xhr.responseText))
                    serviceOk = true
                    return
                } catch (e) {
                    // fall through to the status mirror
                }
            }
            refreshFromFile()
        }
        xhr.open("GET", "http://127.0.0.1:8094/status")
        xhr.send()
    }

    function post(path) {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status !== 200) {
                    try {
                        var r = JSON.parse(xhr.responseText)
                        root.lastError = r.error || ("http " + xhr.status)
                    } catch (e) {
                        root.lastError = "http " + xhr.status
                    }
                }
                refresh()
            }
        }
        xhr.open("POST", "http://127.0.0.1:8094" + path)
        xhr.send()
    }

    function rgbCss(c) {
        if (!c || c.length !== 3) return "#000000"
        return "rgb(" + c[0] + "," + c[1] + "," + c[2] + ")"
    }

    function effectLabel(name) {
        var map = {
            "off": "Off", "solid": "Solid", "breathe": "Breathe",
            "pulse": "Beat", "rainbow": "Rainbow", "wave": "Wave",
            "comet": "Comet", "sparkle": "Sparkle", "cycle": "Cycle"
        }
        return map[name] ? map[name] : name
    }

    Timer {
        interval: 700
        running: root.visible
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }

    // ---- full-screen input shield --------------------------------------
    Rectangle {
        anchors.fill: parent
        color: "#0E0E0E"
    }

    MouseArea {
        anchors.fill: parent
        z: 1
        preventStealing: true
        acceptedButtons: Qt.AllButtons
        onPressed: mouse.accepted = true
        onReleased: mouse.accepted = true
        onClicked: mouse.accepted = true
        onWheel: wheel.accepted = true
    }

    Item {
        id: content
        z: 2
        width: Math.min(parent.width - 40, 1180)
        height: 630
        x: (parent.width - width) / 2

        // ---- header ----------------------------------------------------
        Image {
            id: headerIcon
            x: 62
            y: 8
            width: 52
            height: 52
            smooth: true
            source: "file:///app/share/AppHomeScreen/ambient/ambient_icon_n.png"
        }

        QText {
            x: 124
            y: 2
            text: "Ambience"
            font.pixelSize: ConstantModel.FONT_SIZE_42
            textBold: true
            color: HomeThemeHelper.normalTextColor
        }

        QText {
            x: 66
            y: 54
            text: {
                var svc = root.serviceOk ? "connected" : "unavailable"
                var out = root.backend === "none" ? "no output"
                        : (root.backend + (root.target ? " \u2192 " + root.target : ""))
                return "Ambient lighting   \u00b7   Service " + svc
                       + "   \u00b7   Output " + out
            }
            font.pixelSize: ConstantModel.FONT_SIZE_21
            color: HomeThemeHelper.normalTextColor
        }

        Rectangle {
            id: powerButton
            property bool pressActive: false
            anchors.right: homeButton.left
            anchors.rightMargin: 12
            y: 8
            width: 190
            height: 50
            radius: 25
            color: root.power ? ConstantModel.GENERAL_COLOR_PROGRESS_BLUE
                              : ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1
            opacity: root.serviceOk ? 1.0 : 0.55

            QText {
                anchors.centerIn: parent
                text: root.power ? "Lights on" : "Lights off"
                font.pixelSize: ConstantModel.FONT_SIZE_21
                color: root.power ? ConstantModel.GENERAL_COLOR_WHITE
                                  : HomeThemeHelper.normalTextColor
            }

            MouseArea {
                id: powerMouse
                anchors.fill: parent
                onPressed: powerButton.pressActive = true
                onPositionChanged: if (!containsMouse) powerButton.pressActive = false
                onReleased: {
                    if (powerButton.pressActive && root.serviceOk)
                        root.post(root.power ? "/off" : "/on")
                    powerButton.pressActive = false
                }
            }
        }

        Rectangle {
            id: homeButton
            property bool pressActive: false
            anchors.right: parent.right
            y: 8
            width: 140
            height: 50
            radius: 25
            color: homeMouse.pressed && pressActive
                   ? ConstantModel.GENERAL_COLOR_FOCUS_GREY
                   : ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            QText {
                anchors.centerIn: parent
                text: "\u2039  Home"
                font.pixelSize: ConstantModel.FONT_SIZE_24
                color: HomeThemeHelper.normalTextColor
            }

            MouseArea {
                id: homeMouse
                anchors.fill: parent
                onPressed: homeButton.pressActive = true
                onPositionChanged: if (!containsMouse) homeButton.pressActive = false
                onReleased: {
                    if (homeButton.pressActive) {
                        QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
                        root.goHome()
                    }
                    homeButton.pressActive = false
                }
            }
        }


          // On-demand factory-ambient capture: baseline, then an audible
          // "toggle now" beep, then the 15 s window.  Result: hw_capture.txt.
          Rectangle {
              id: captureButton
              property bool pressActive: false
              anchors.right: homeButton.left
              anchors.rightMargin: 12
              y: 8
              width: 190
              height: 50
              radius: 25
              color: captureMouse.pressed && pressActive
                     ? ConstantModel.GENERAL_COLOR_FOCUS_GREY
                     : ConstantModel.GENERAL_COLOR_BUTTON_GREY
              border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
              border.width: 1
              opacity: root.serviceOk ? 1.0 : 0.55

              QText {
                  anchors.centerIn: parent
                  text: "Capture frames"
                  font.pixelSize: ConstantModel.FONT_SIZE_21
                  color: HomeThemeHelper.normalTextColor
              }

              QText {
                  anchors.left: parent.left
                  anchors.leftMargin: 6
                  anchors.top: parent.bottom
                  anchors.topMargin: 4
                  text: root.captureText()
                  font.pixelSize: ConstantModel.FONT_SIZE_17
                  color: root.captureLight > 0 ? ConstantModel.GENERAL_COLOR_PROGRESS_BLUE
                                               : (root.captureState === "done" ? "#FFB300"
                                                                              : HomeThemeHelper.disableTextColor)
              }

              MouseArea {
                  id: captureMouse
                  anchors.fill: parent
                  onPressed: captureButton.pressActive = true
                  onPositionChanged: if (!containsMouse) captureButton.pressActive = false
                  onReleased: {
                      if (captureButton.pressActive && root.serviceOk) {
                          root.post("/capture")
                          root.captureTimer.restart()
                          capturePollTimer.restart()
                      }
                      captureButton.pressActive = false
                  }
              }
          }

          // Beep exactly when the capture enters the "toggle a light" window.
          Timer {
              id: captureTimer
              interval: 8000
              repeat: false
              onTriggered: {
                  QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
              }
          }

          // Follow the capture until the daemon reports it finished (baseline 8 s
          // + window 15 s + margin), then the label carries the counts.
          Timer {
              id: capturePollTimer
              interval: 4000
              repeat: true
              running: false
              onTriggered: {
                  root.refresh()
                  if (root.captureState === "done") stop()
              }
          }

        // ---- effects ---------------------------------------------------
        Rectangle {
            x: 0
            y: 92
            width: 570
            height: 300
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            QText {
                x: 24
                y: 14
                text: "Effect"
                font.pixelSize: ConstantModel.FONT_SIZE_27
                color: HomeThemeHelper.normalTextColor
            }

            ListModel {
                id: effectModel
                ListElement { fx: "off" }
                ListElement { fx: "solid" }
                ListElement { fx: "breathe" }
                ListElement { fx: "pulse" }
                ListElement { fx: "rainbow" }
                ListElement { fx: "wave" }
                ListElement { fx: "comet" }
                ListElement { fx: "sparkle" }
                ListElement { fx: "cycle" }
            }

            Item {
                id: effectGrid
                x: 24
                y: 58
                width: 522
                height: 216

                Repeater {
                    model: effectModel
                    delegate: Rectangle {
                        x: (index % 3) * 176
                        y: Math.floor(index / 3) * 70
                        width: 164
                        height: 58
                        radius: 12
                        color: root.effect === fx
                               ? ConstantModel.GENERAL_COLOR_PROGRESS_BLUE
                               : "#262626"
                        border.color: root.effect === fx
                                      ? ConstantModel.GENERAL_COLOR_PROGRESS_BLUE
                                      : ConstantModel.GENERAL_COLOR_FOCUS_GREY
                        border.width: 1
                        opacity: root.serviceOk ? 1.0 : 0.55

                        QText {
                            anchors.centerIn: parent
                            text: root.effectLabel(fx)
                            font.pixelSize: ConstantModel.FONT_SIZE_24
                            color: root.effect === fx
                                   ? ConstantModel.GENERAL_COLOR_WHITE
                                   : HomeThemeHelper.normalTextColor
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (!root.serviceOk) return
                                if (root.effect !== fx)
                                    root.post("/effect?name=" + fx)
                            }
                        }
                    }
                }
            }
        }

        // ---- colour ----------------------------------------------------
        Rectangle {
            x: 0
            y: 404
            width: 570
            height: 140
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            QText {
                x: 24
                y: 12
                text: "Colour"
                font.pixelSize: ConstantModel.FONT_SIZE_27
                color: HomeThemeHelper.normalTextColor
            }

            Rectangle {
                id: swatchPreview
                anchors.right: parent.right
                anchors.rightMargin: 24
                y: 12
                width: 66
                height: 30
                radius: 8
                color: root.rgbCss(root.rgb)
                border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
                border.width: 1
            }

            property var swatches: [
                [255, 0, 0], [255, 122, 0], [255, 210, 0], [60, 255, 120],
                [0, 210, 255], [43, 108, 255], [170, 80, 255], [255, 255, 255]
            ]

            Repeater {
                model: 8
                delegate: Rectangle {
                    x: 24 + index * 46
                    y: 52
                    width: 36
                    height: 36
                    radius: 8
                    color: {
                        var s = colourCard.swatches[index]
                        return "rgb(" + s[0] + "," + s[1] + "," + s[2] + ")"
                    }
                    border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
                    border.width: 1
                    opacity: root.serviceOk ? 1.0 : 0.55

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (!root.serviceOk) return
                            var s = colourCard.swatches[index]
                            root.post("/color?r=" + s[0] + "&g=" + s[1] + "&b=" + s[2])
                        }
                    }
                }
            }

            AmbientSlider {
                x: 24
                y: 92
                width: 522
                height: 40
                label: "Hue"
                valueText: Math.round(root.hue * 360) + "\u00b0"
                value: root.hue
                interactive: root.serviceOk
                onCommitted: root.post("/color?h=" + v.toFixed(3) + "&s="
                                       + Math.max(0.35, root.sat).toFixed(2)
                                       + "&v=" + root.val.toFixed(2))
            }
        }

        // ---- brightness / speed / intensity ----------------------------
        Rectangle {
            x: 0
            y: 556
            width: 570
            height: 74
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            AmbientSlider {
                x: 20
                y: 8
                width: 250
                height: 58
                label: "Brightness"
                valueText: root.brightness + "%"
                value: root.brightness / 100
                interactive: root.serviceOk
                onCommitted: root.post("/brightness?value=" + Math.round(v * 100))
            }

            AmbientSlider {
                x: 300
                y: 8
                width: 250
                height: 58
                label: "Speed"
                valueText: root.speed + "%"
                value: root.speed / 100
                interactive: root.serviceOk
                onCommitted: root.post("/effect?name=" + root.effect
                                       + "&speed=" + Math.round(v * 100)
                                       + "&intensity=" + root.intensity)
            }
        }

        // ---- presets ---------------------------------------------------
        Rectangle {
            x: 610
            y: 92
            width: 570
            height: 150
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            QText {
                x: 24
                y: 12
                text: "Presets"
                font.pixelSize: ConstantModel.FONT_SIZE_27
                color: HomeThemeHelper.normalTextColor
            }

            Rectangle {
                id: saveButton
                property bool pressActive: false
                anchors.right: parent.right
                anchors.rightMargin: 24
                y: 10
                width: 120
                height: 34
                radius: 17
                color: saveMouse.pressed && pressActive
                       ? ConstantModel.GENERAL_COLOR_FOCUS_GREY
                       : "#262626"
                border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
                border.width: 1
                opacity: root.serviceOk ? 1.0 : 0.55

                QText {
                    anchors.centerIn: parent
                    text: "Save current"
                    font.pixelSize: ConstantModel.FONT_SIZE_21
                    color: HomeThemeHelper.normalTextColor
                }

                MouseArea {
                    id: saveMouse
                    anchors.fill: parent
                    onPressed: saveButton.pressActive = true
                    onPositionChanged: if (!containsMouse) saveButton.pressActive = false
                    onReleased: {
                        if (saveButton.pressActive && root.serviceOk)
                            root.post("/preset?name=" + encodeURIComponent("Custom "
                                      + (root.presetNames.length + 1))
                                      + "&action=save")
                        saveButton.pressActive = false
                    }
                }
            }

            Repeater {
                model: root.presetNames
                delegate: Rectangle {
                    x: 24 + (index % 4) * 136
                    y: 58 + Math.floor(index / 4) * 44
                    width: 128
                    height: 36
                    radius: 10
                    color: "#262626"
                    border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
                    border.width: 1
                    opacity: root.serviceOk ? 1.0 : 0.55

                    QText {
                        anchors.centerIn: parent
                        text: modelData
                        font.pixelSize: ConstantModel.FONT_SIZE_21
                        color: HomeThemeHelper.normalTextColor
                        elide: Text.ElideRight
                        width: parent.width - 16
                        horizontalAlignment: Text.AlignHCenter
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: if (root.serviceOk)
                                       root.post("/preset?name="
                                                 + encodeURIComponent(modelData))
                    }
                }
            }
        }

        // ---- transitions ----------------------------------------------
        Rectangle {
            x: 610
            y: 254
            width: 570
            height: 140
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            QText {
                x: 24
                y: 12
                text: "Transition"
                font.pixelSize: ConstantModel.FONT_SIZE_27
                color: HomeThemeHelper.normalTextColor
            }

            ListModel {
                id: transitionModel
                ListElement { style: "crossfade"; label: "Crossfade" }
                ListElement { style: "black"; label: "Through black" }
                ListElement { style: "cut"; label: "Cut" }
            }

            Row {
                x: 24
                y: 48
                spacing: 10

                Repeater {
                    model: transitionModel
                    delegate: Rectangle {
                        width: 160
                        height: 34
                        radius: 10
                        color: root.transitionStyle === style
                               ? ConstantModel.GENERAL_COLOR_PROGRESS_BLUE
                               : "#262626"
                        border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
                        border.width: 1
                        opacity: root.serviceOk ? 1.0 : 0.55

                        QText {
                            anchors.centerIn: parent
                            text: label
                            font.pixelSize: ConstantModel.FONT_SIZE_21
                            color: root.transitionStyle === style
                                   ? ConstantModel.GENERAL_COLOR_WHITE
                                   : HomeThemeHelper.normalTextColor
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: if (root.serviceOk)
                                           root.post("/transition?ms="
                                                     + root.transitionMs
                                                     + "&style=" + style)
                        }
                    }
                }
            }

            AmbientSlider {
                x: 24
                y: 88
                width: 522
                height: 44
                label: "Duration"
                valueText: (root.transitionMs / 1000).toFixed(1) + " s"
                value: Math.min(1, root.transitionMs / 3000)
                interactive: root.serviceOk
                onCommitted: root.post("/transition?ms=" + Math.round(v * 3000)
                                       + "&style=" + root.transitionStyle)
            }
        }

        // ---- music reactive -------------------------------------------
        Rectangle {
            x: 610
            y: 404
            width: 570
            height: 140
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            QText {
                x: 24
                y: 12
                text: "Music reactive"
                font.pixelSize: ConstantModel.FONT_SIZE_27
                color: HomeThemeHelper.normalTextColor
            }

            Rectangle {
                id: reactiveButton
                property bool pressActive: false
                x: 24
                y: 48
                width: 190
                height: 36
                radius: 18
                color: root.reactiveOn ? ConstantModel.GENERAL_COLOR_PROGRESS_BLUE
                                       : "#262626"
                border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
                border.width: 1
                opacity: (root.serviceOk && root.audioEnabled) ? 1.0 : 0.55

                QText {
                    anchors.centerIn: parent
                    text: root.reactiveOn ? "Reactive on" : "Reactive off"
                    font.pixelSize: ConstantModel.FONT_SIZE_21
                    color: root.reactiveOn ? ConstantModel.GENERAL_COLOR_WHITE
                                           : HomeThemeHelper.normalTextColor
                }

                MouseArea {
                    id: reactiveMouse
                    anchors.fill: parent
                    onPressed: reactiveButton.pressActive = true
                    onPositionChanged: if (!containsMouse) reactiveButton.pressActive = false
                    onReleased: {
                        if (reactiveButton.pressActive && root.serviceOk
                            && root.audioEnabled)
                            root.post("/reactive?enabled=" + (root.reactiveOn ? 0 : 1)
                                      + "&gain=" + root.audioGain)
                        reactiveButton.pressActive = false
                    }
                }
            }

            QText {
                x: 230
                y: 56
                text: root.audioEnabled
                      ? ("Audio: " + root.audioSource + "   \u00b7   beats "
                         + root.beats)
                      : "No audio source configured"
                font.pixelSize: ConstantModel.FONT_SIZE_21
                color: HomeThemeHelper.disableTextColor
                elide: Text.ElideRight
                width: 320
            }

            // Level meters: overall + three bands.
            Item {
                x: 24
                y: 92
                width: 522
                height: 40

                Repeater {
                    model: 4
                    delegate: Item {
                        id: meter
                        x: index * 132
                        width: 124
                        height: 40

                        property real level: index === 0 ? root.audioLevel
                                          : index === 1 ? root.audioBass
                                          : index === 2 ? root.audioMid
                                          : root.audioHigh
                        property string name: index === 0 ? "level"
                                            : index === 1 ? "bass"
                                            : index === 2 ? "mid" : "high"

                        Rectangle {
                            x: 0
                            y: 0
                            width: parent.width
                            height: 14
                            radius: 7
                            color: "#1c1c1c"
                            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
                            border.width: 1

                            Rectangle {
                                x: 1
                                y: 1
                                width: Math.max(0, (parent.width - 2) * meter.level)
                                height: parent.height - 2
                                radius: 6
                                color: ConstantModel.GENERAL_COLOR_PROGRESS_BLUE
                            }
                        }

                        QText {
                            x: 0
                            y: 18
                            text: parent.name
                            font.pixelSize: ConstantModel.FONT_SIZE_21
                            color: HomeThemeHelper.disableTextColor
                        }
                    }
                }
            }
        }

        // ---- output / footer -------------------------------------------
        Rectangle {
            x: 610
            y: 556
            width: 570
            height: 74
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            QText {
                x: 24
                y: 10
                width: 522
                text: "Output: " + root.backend
                      + (root.target ? ("  \u2192  " + root.target) : "")
                      + (root.lastError ? ("   \u00b7   " + root.lastError) : "")
                font.pixelSize: ConstantModel.FONT_SIZE_21
                color: root.lastError ? "#FFB300"
                                      : HomeThemeHelper.normalTextColor
                elide: Text.ElideRight
            }

            QText {
                x: 24
                y: 40
                width: 522
                text: "ambient_daemon " + root.daemonVersion
                      + "   \u00b7   Home key: " + root.homeSource
                      + (root.serviceOk ? "" : "   \u00b7   service unavailable")
                font.pixelSize: ConstantModel.FONT_SIZE_21
                color: HomeThemeHelper.disableTextColor
                elide: Text.ElideRight
            }
        }
    }
}
