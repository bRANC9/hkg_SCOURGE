// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// Self-test for the preview's overflow detector: one text that must be flagged
// as clipped, one wrapped text that must not be, and one wrapped text that is
// taller than its box and must be flagged as a vertical clip.
import QtQuick 2.0

Item {
    width: 400
    height: 300
    Text {
        objectName: "clipped"
        x: 10; y: 10; width: 120; height: 30
        text: "This sentence is definitely wider than its box"
        wrapMode: Text.NoWrap
    }
    Text {
        objectName: "wrapped-fine"
        x: 10; y: 60; width: 120; height: 120
        text: "This sentence wraps and has plenty of room vertically"
        wrapMode: Text.WordWrap
    }
    Text {
        objectName: "wrapped-clipped"
        x: 10; y: 190; width: 120; height: 20
        text: "This wrapped sentence needs several lines but the box is only one line tall"
        wrapMode: Text.WordWrap
    }
}
