// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// AMBIENT_PAYLOAD_260128
import QtQuick 2.0
import "common"
import HMIEVENTS 1.0

/* A label + horizontal drag bar.  value is 0..1; the page converts it.
 * `moved` fires while dragging, `committed` on release (that is when the
 * page posts to the daemon, so a drag is one command, not fifty). */
Item {
    id: root
    width: 240
    height: 62
    property string label: ""
    property string valueText: ""
    property real value: 0
    property bool interactive: true

    signal moved(real v)
    signal committed(real v)

    QText {
        id: caption
        x: 0
        y: 0
        text: root.label
        font.pixelSize: ConstantModel.FONT_SIZE_21
        color: HomeThemeHelper.normalTextColor
    }

    QText {
        anchors.right: parent.right
        y: 0
        text: root.valueText
        font.pixelSize: ConstantModel.FONT_SIZE_21
        color: HomeThemeHelper.disableTextColor
    }

    Rectangle {
        id: track
        x: 0
        y: 32
        width: parent.width
        height: 18
        radius: 9
        color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
        border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
        border.width: 1

        Rectangle {
            x: 1
            y: 1
            width: Math.max(0, (track.width - 2) * root.value)
            height: track.height - 2
            radius: 8
            color: ConstantModel.GENERAL_COLOR_PROGRESS_BLUE
            opacity: root.interactive ? 1.0 : 0.5
        }

        Rectangle {
            x: Math.max(0, Math.min(track.width - 18, (track.width - 18) * root.value))
            y: -4
            width: 18
            height: track.height + 8
            radius: 9
            color: ConstantModel.GENERAL_COLOR_BRIGHT_GREY
            opacity: root.interactive ? 1.0 : 0.5
        }

        MouseArea {
            id: drag
            anchors.fill: parent
            anchors.margins: -10
            enabled: root.interactive
            onPressed: {
                root.value = Math.max(0, Math.min(1, (mouse.x - 10) / track.width))
                root.moved(root.value)
            }
            onPositionChanged: {
                if (!pressed) return
                root.value = Math.max(0, Math.min(1, (mouse.x - 10) / track.width))
                root.moved(root.value)
            }
            onReleased: root.committed(root.value)
        }
    }
}
