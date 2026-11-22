// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// STORE_PAYLOAD_260128
//
// Marketplace overlay. It is only the UI: every action goes to the store_daemon
// on 127.0.0.1:8092 (packages/store), which owns pairing, the catalogue cache,
// the download/verify/install job and the report back to the hub.
import QtQuick 2.0
import HMIEVENTS 1.0

Item {
    id: root
    width: 1920
    height: 720
    visible: false
    z: 2000
    signal closed()

    property string apiBase: "http://127.0.0.1:8092"
    property bool paired: false
    property bool busy: false
    property string hub: ""
    property string firmware: ""
    property string statusText: "Contacting the store daemon"
    property string pairCode: ""
    property string pairUrl: ""
    property string jobState: "idle"
    property int jobPercent: 0
    property string jobDetail: ""
    property string jobPackage: ""
    property var packages: []

    function post(path, cb) {
        var x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState !== XMLHttpRequest.DONE)
                return
            var body = x.responseText
            var parsed = null
            try {
                parsed = JSON.parse(body)
            } catch (e) {
                parsed = null
            }
            if (cb)
                cb(x.status, parsed)
        }
        x.open("POST", apiBase + path, true)
        x.send()
    }

    function get(path, cb) {
        var x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState !== XMLHttpRequest.DONE)
                return
            var parsed = null
            try {
                parsed = JSON.parse(x.responseText)
            } catch (e) {
                parsed = null
            }
            if (cb)
                cb(x.status, parsed)
        }
        x.open("GET", apiBase + path, true)
        x.send()
    }

    function refreshStatus() {
        get("/status", function (code, d) {
            if (code !== 200 || !d) {
                statusText = "Store daemon not running (port 8092)"
                return
            }
            paired = d.paired === true
            hub = d.hub || ""
            firmware = d.firmware || ""
            statusText = paired ? ("Connected to " + hub) : "Not paired yet - scan the QR code"
            if (d.job) {
                jobState = d.job.state || "idle"
                jobPercent = d.job.percent || 0
                jobDetail = d.job.detail || ""
                jobPackage = d.job.package || ""
            }
        })
    }

    function refreshCatalog() {
        get("/catalog", function (code, d) {
            if (code !== 200 || !d)
                return
            packages = (d.packages !== undefined) ? d.packages : d.packages
        })
    }

    function refresh() {
        busy = true
        post("/refresh", function () {
            busy = false
            refreshStatus()
            refreshCatalog()
        })
    }

    function install(slug) {
        post("/install?pkg=" + encodeURIComponent(slug), function () {
            refreshStatus()
        })
    }

    property var localPackages: []
    property bool showLocal: false
    // Version history of the package whose "Versions" was tapped (from /versions).
    property var versionRows: []
    property string versionPkg: ""

    function loadVersions(pkg) {
        versionPkg = pkg
        versionRows = []
        get("/versions?pkg=" + encodeURIComponent(pkg), function (code, d) {
            if (code !== 200 || !d || d.versions === undefined)
                return
            versionRows = d.versions
        })
    }

    function refreshLocal() {
        get("/local", function (code, d) {
            if (code !== 200 || !d || d.packages === undefined) {
                localPackages = []
                return
            }
            localPackages = d.packages
        })
    }

    function installLocal(file) {
        post("/install-local?file=" + encodeURIComponent(file), function () {
            refreshStatus()
        })
    }

    function showPairing() {
        post("/refresh", function () {
            refreshStatus()
        })
    }

    onVisibleChanged: {
        if (visible) {
            refreshStatus()
            refreshCatalog()
            refreshLocal()
        }
    }

    Timer {
        interval: 2000
        running: root.visible
        repeat: true
        onTriggered: {
            refreshStatus()
            if (jobState === "installing" || jobState === "downloading" ||
                jobState === "verifying" || jobState === "extracting")
                jobTick.start()
        }
    }

    Timer {
        id: jobTick
        interval: 250
        running: false
        repeat: false
        onTriggered: refreshStatus()
    }

    Rectangle {
        anchors.fill: parent
        color: "#0E1216"

        Item {
            id: content
            width: 1180
            height: 630
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 90

            Image {
                id: headerIcon
                x: 0
                y: 4
                width: 56
                height: 56
                smooth: true
                source: "file:///app/share/AppHomeScreen/store/store_icon_n.png"
            }

            QText {
                id: title
                x: 72
                y: 0
                text: "Marketplace"
                font.pixelSize: 44
                textBold: true
                color: "#FFFFFF"
            }

            QText {
                id: subtitle
                x: 74
                y: 50
                text: root.statusText
                font.pixelSize: 20
                color: root.paired ? "#8FCB9B" : "#F0B35B"
            }

            QText {
                x: 74
                y: 74
                text: root.firmware
                font.pixelSize: 16
                color: "#6E7780"
            }

            Rectangle {
                id: closeButton
                width: 150
                height: 46
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 6
                radius: 10
                color: closeArea.pressed ? "#2B333B" : "#1B2228"

                QText {
                    anchors.centerIn: parent
                    text: "\u2039  Close"
                    font.pixelSize: 22
                    color: "#E8EDF2"
                }
                MouseArea {
                    id: closeArea
                    anchors.fill: parent
                    onClicked: {
                        root.visible = false
                        root.closed()
                    }
                }
            }

            // ---- pairing / refresh --------------------------------------
            Rectangle {
                id: actionRow
                x: 0
                y: 106
                width: parent.width
                height: 64
                radius: 12
                color: "#161C22"

                QText {
                    x: 20
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.paired ? "Unit paired - installs need your approval on this screen"
                                    : "Open the hub, scan the QR code from Tweaks > Store, or refresh the catalogue"
                    font.pixelSize: 18
                    color: "#C9D3DC"
                }

                Rectangle {
                    id: usbButton
                    width: 150
                    height: 44
                    anchors.right: refreshButton.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    radius: 10
                    color: root.showLocal ? "#1F6FEB" : "#232B33"

                    QText {
                        anchors.centerIn: parent
                        text: root.showLocal ? "USB packages" : "Store packages"
                        font.pixelSize: 18
                        color: "#FFFFFF"
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            root.showLocal = !root.showLocal
                            if (root.showLocal)
                                root.refreshLocal()
                        }
                    }
                }

                Rectangle {
                    id: refreshButton
                    width: 170
                    height: 44
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    radius: 10
                    color: root.busy ? "#2A323A" : "#1F6FEB"
                    opacity: root.busy ? 0.6 : 1

                    QText {
                        anchors.centerIn: parent
                        text: root.busy ? "Refreshing..." : "Refresh catalogue"
                        font.pixelSize: 19
                        color: "#FFFFFF"
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: !root.busy
                        onClicked: root.refresh()
                    }
                }
            }

            // ---- job progress -------------------------------------------
            Rectangle {
                id: progressCard
                x: 0
                y: 182
                width: parent.width
                height: 58
                radius: 12
                color: "#161C22"
                visible: root.jobState !== "idle"

                QText {
                    x: 20
                    y: 8
                    text: root.jobPackage + " - " + root.jobState +
                          (root.jobDetail !== "" ? "  \u00b7  " + root.jobDetail : "")
                    font.pixelSize: 17
                    color: root.jobState === "failed" ? "#F08A84"
                         : root.jobState === "installed" ? "#8FCB9B" : "#C9D3DC"
                }

                Rectangle {
                    x: 20
                    y: 34
                    width: parent.width - 40
                    height: 8
                    radius: 4
                    color: "#0F1418"

                    Rectangle {
                        width: parent.width * Math.max(0, Math.min(100, root.jobPercent)) / 100
                        height: parent.height
                        radius: 4
                        color: root.jobState === "failed" ? "#F08A84" : "#1F6FEB"
                    }
                }
            }

            // ---- catalogue ----------------------------------------------
            ListView {
                id: list
                x: 0
                y: 254
                width: parent.width
                height: 372
                clip: true
                model: root.showLocal ? root.localPackages : root.packages
                spacing: 10

                delegate: Rectangle {
                    width: list.width
                    height: 96
                    radius: 12
                    color: "#161C22"

                    QText {
                        x: 20
                        y: 14
                        text: root.showLocal
                              ? ((modelData.meta && modelData.meta.pkg) ? modelData.meta.pkg : modelData.file)
                              : modelData.pkg
                        font.pixelSize: 26
                        textBold: true
                        color: "#FFFFFF"
                    }
                    QText {
                        x: 20
                        y: 50
                        text: root.showLocal
                              ? (modelData.file + "  \u00b7  USB stick")
                              : ("v" + modelData.version + "  \u00b7  " + modelData.firmware +
                                 "  \u00b7  " + modelData.region + "  \u00b7  " + modelData.gui +
                                 (modelData.verified === true
                                  ? "  \u00b7  \u2713 " + (modelData.verified_devices || 0) + " car(s)"
                                  : "  \u00b7  not verified yet"))
                        font.pixelSize: 16
                        color: modelData.verified === true ? "#8FCB9B" : "#6E7780"
                    }
                    QText {
                        x: 20
                        y: 70
                        text: root.showLocal
                              ? (modelData.sha256_sidecar ? "sha256 sidecar verified" : "no sha256 sidecar")
                              : ((modelData.notes !== undefined && modelData.notes !== "")
                                 ? modelData.notes
                                 : "sha256 " + (modelData.artifact ? modelData.artifact.sha256.substring(0, 16) : ""))
                        font.pixelSize: 14
                        color: "#4C555E"
                        width: parent.width - 260
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        id: installButton
                        width: 190
                        height: 46
                        anchors.right: parent.right
                        anchors.rightMargin: 18
                        anchors.verticalCenter: parent.verticalCenter
                        radius: 10
                        color: installArea.pressed ? "#1A5FD0" : "#1F6FEB"

                        QText {
                            anchors.centerIn: parent
                            text: root.showLocal ? "Install from USB" : (root.paired ? "Install" : "Pair first")
                            font.pixelSize: 20
                            color: "#FFFFFF"
                        }
                        MouseArea {
                            id: installArea
                            anchors.fill: parent
                            enabled: root.showLocal ? true : root.paired
                            onClicked: {
                                if (root.showLocal)
                                    root.installLocal(modelData.file)
                                else
                                    root.install(modelData.pkg)
                            }
                            onPressAndHold: {
                                if (!root.showLocal)
                                    root.loadVersions(modelData.pkg)
                            }
                        }
                    }
                }
            }

            // Version history drawer: long-press Install on a row to open it.
            Rectangle {
                id: versionsDrawer
                x: 0
                y: 254
                width: parent.width
                height: 372
                radius: 12
                color: "#10161B"
                visible: root.versionPkg !== ""
                border.color: "#23303A"

                QText {
                    x: 20
                    y: 10
                    text: "Versions of " + root.versionPkg
                    font.pixelSize: 18
                    color: "#C9D3DC"
                }
                ListView {
                    x: 0
                    y: 44
                    width: parent.width
                    height: parent.height - 52
                    clip: true
                    model: root.versionRows
                    delegate: Item {
                        width: versionsDrawer.width
                        height: 54
                        QText {
                            x: 20
                            anchors.verticalCenter: parent.verticalCenter
                            text: "v" + modelData.version + "  \u00b7  " + modelData.size_mb + " MB" +
                                  "  \u00b7  \u2713 " + (modelData.verified_devices || 0) + " car(s)"
                            font.pixelSize: 17
                            color: "#E8EDF2"
                        }
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.versionPkg = ""
                }
            }

            QText {
                x: 0
                y: 300
                width: parent.width
                visible: root.packages.length === 0
                text: "No packages yet - press Refresh catalogue"
                font.pixelSize: 20
                color: "#6E7780"
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }
}
