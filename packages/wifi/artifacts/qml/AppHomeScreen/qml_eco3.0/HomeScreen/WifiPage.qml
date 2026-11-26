// WIFI_PAYLOAD_260128
// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
import QtQuick 2.0
import "../component"
import "../component/common"
import HMIEVENTS 1.0
import AppHome_Enum 1.0

Item {
    id: root
    width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y

    signal closed()

    // ---- overlay / foreground ownership ------------------------------
    // The page owns its visibility: callers use open()/close(), never
    // .visible.  Never paint while another app owns the Wayland surface,
    // otherwise taps fall through to it (UI_CONTEXT.md Rule 2).
    property bool requested: false
    property bool atOtherApp: typeof AppHomeModel !== "undefined"
                               ? AppHomeModel.isAtOtherApp : false
    property int homeSeq: 0
    property double homeEpoch: 0
    property string homeSource: "none"
    property bool homeMonitor: false
    property bool homeFileSeen: false
    property double openedAt: 0
    property bool homeBaselineValid: false
    property double homeEpochBaseline: 0
    property bool homeFallback: false
    property int openSerial: 0
    property int homeReadSerial: 0
    property int homeAcceptedRead: 0

    // Do not bind `visible` to `requested`.  Older Tweaks/Battery host
    // variants assign wifiPage.visible directly; a direct assignment
    // destroys a QML binding, which made Exit set requested=false while the
    // page stayed painted.  Keep the two properties synchronized explicitly
    // so both open()/close() and legacy host assignments work.
    visible: false
    focus: visible
    enabled: !(typeof AppHomeModel !== "undefined"
               && AppHomeModel.sysPopupShowing)

    function open() {
        // A stale AppHomeModel.isAtOtherApp must not swallow the tap: the flag
        // is only refreshed by code we replace.  Open, then re-check (below).
        openSerial++
        requested = true
        visible = true
        openedAt = Date.now()
        homeBaselineValid = false
        refresh()
        refreshHome()
    }
    function close() {
        if (!requested && !visible) return
        requested = false
        visible = false
        openSerial++
        homeBaselineValid = false
        closed()
    }

    onVisibleChanged: {
        console.log("[WifiPage] visible=" + visible + " requested=" + requested
                    + " atOtherApp=" + atOtherApp + " serial=" + openSerial)
        if (visible && !requested) {
            // Compatibility for a host that assigns .visible = true instead
            // of calling open().  Re-arm the page exactly once.
            requested = true
            openSerial++
            openedAt = Date.now()
            homeBaselineValid = false
            refresh()
            refreshHome()
        } else if (!visible && requested) {
            requested = false
        }
    }

    onRequestedChanged: {
        if (requested && !visible)
            visible = true
        else if (!requested && visible)
            visible = false
    }

    onAtOtherAppChanged: if (atOtherApp) autoClose()

    // Auto-close events only count once the overlay has been up briefly.  The
    // home screen we replaced is what maintained AppHomeModel's flags, so some
    // of them are stale and fired a few hundred ms after open, shutting the
    // page immediately.  Genuine navigation still closes it.
    function autoClose() {
        if (Date.now() - openedAt < 1200) return
        close()
    }

    // Steering-wheel Home/Star is a session-bus event; the daemon tails it
    // and bumps home_seq.  Any navigation or foreground change also closes
    // the overlay.  AppHomeModel.onButtonBackClicked is only the home
    // screen's own back affordance, not the steering-wheel key.
    Connections {
        target: (typeof AppHomeModel !== "undefined") ? AppHomeModel : null
        onButtonBackClicked: root.autoClose()
        onSwitchToOtherApp: root.autoClose()
        onIsAtOtherAppChanged: if (AppHomeModel.isAtOtherApp) root.autoClose()
        onCurrentScreenChanged: root.autoClose()
        onCurrentHomeAdvancedPageIdxChanged: root.autoClose()
        onRequestHomeByPPChanged: if (AppHomeModel.requestHomeByPP) root.autoClose()
        // A system popup is not a stale host flag: it must take the page down at
        // once, so it does not wait for the autoClose() grace window.  Fighting a
        // popup for the surface is the one thing the stock pages never do
        // (styleguide 4.6).
        onSysPopupShowingChanged: if (AppHomeModel.sysPopupShowing) root.close()
    }

    function acceptHome(d) {
        var hs = (typeof d.seq === "number") ? d.seq : 0
        var ep = (typeof d.epoch === "number") ? d.epoch : 0
        // Snapshot the first *fresh* response after open. A new daemon epoch
        // is a new counter domain, not a Home press or an unreachable count.
        if (!homeBaselineValid || ep !== homeEpochBaseline) {
            homeSeqBaseline = hs
            homeEpochBaseline = ep
            homeBaselineValid = requested
        } else if (requested && hs > homeSeqBaseline) {
            close()
        }
        homeSeq = hs
        homeEpoch = ep
        if (d.source) homeSource = d.source
        homeMonitor = d.monitor === true
    }

    // Live log console on the right; only wide (1920) layouts have room.
    readonly property bool showConsole: width >= 1700
    property string consoleSource: "daemon"

    // ---- daemon state -------------------------------------------------
    property bool serviceOk: false
    // Last POST sent, shown while the daemon has not answered yet: a tap must be
    // visible even before the next status poll lands.
    property string pendingPost: ""
    property bool observationsFresh: false
    property string mode: "off"
    property bool wlanOn: false
    property bool driver: false
    property string ip: ""
    property string ssid: ""
    property int signalLevel: 0
    property bool apActive: false
    // How the AP is created and what it really is.  The daemon's vendor tether
    // path is refused on this unit (rc=0 rejected=1), so the method and the
    // actual band have to be visible instead of implied by the button state.
    property string apMethod: "static"
    property string apPurpose: "ssh"
    property string apDhcpPool: "10.42.0.1 .10-.200"
    property int apDhcpLeases: 0
    readonly property bool aaBandMismatch: apPurpose === "aa" && carplayBand !== 2
    function apMethodText() {
        return (apMethod === "tether" ? "ConnMan tether" : "static") +
               "   \u00b7   DHCP " + apDhcpPool +
               (apActive ? "   \u00b7   " + apDhcpLeases + " lease(s)" : "")
    }
    function apVerdictText() {
        if (apPurpose !== "aa") return ""
        if (aaBandMismatch) return "Android Auto needs 5 GHz - the phone will not join this AP"
        return apActive ? "Android Auto AP on 5 GHz" : "Android Auto AP is not running"
    }
    property string apSsid: ""
    property int apClients: 0
    property bool carplayReady: false
    // The wireless-CarPlay flag in VBTConfig.json changed and only a reboot applies
    // it: the daemon deliberately never restarts the running Bluetooth stack (that
    // destroys BluetoothHMI/controller state and exhausts the stack's CCBs).
    property bool vbtNeedsReboot: false
    property bool carplayIe: false
    property bool carplayIap2: false
    property bool carplayMdns: false
    property bool sshActive: false
    property bool sshEnabled: false
    property int sshPort: 2222
    property bool sshPortFallback: false
    property bool vendorSsh22: false
    property int secureConsole: -1
    property bool debugConsole: false
    property bool imported: false
    property string importedFrom: ""
    property string clientSsid: ""
    property string clientNetworks: ""
    property int clientNetCount: 1
    property string carplaySsid: ""
    property string carplayPsk: ""
    property bool carplayPskSet: false
    property int carplayBand: 2
    // 149, not 36: this firmware has no ch36..48 (its table is 1-13 + 149..165).
    property int carplayChannel: 149
    property string carplayBtaddr: ""
    property string carplayId: ""
    property string daemonMessage: ""
    // ---- live activity popup -------------------------------------------
    // The daemon reports what it is doing through /status (message + applying),
    // so the page can show it as an interactive "what is happening now" card
    // instead of making the user hunt for the log console.
    property bool applying: false
    property bool showActivity: false
    property bool activitySeen: false
    // The popup defaults to the INSTALLER log: after a stick run the question is
    // "where did it stop", and the daemon runtime log rarely answers it.
    // "root" is the bundle log the loader writes while it runs: that is the live
    // one, the stage logs are per package.
    property string logSource: "root"
    property var logSources: ["root", "install", "battery", "tweaks", "stages", "daemon", "wpa"]
    property bool btDiscoverable: false
    property bool btPairable: false
    property string pairMessage: ""

    property int currentTab: 0
    property var networks: []
    property bool scanning: false
    property string selectedSsid: ""
    property double statusUpdatedAt: 0
    property int homeSeqBaseline: 0

    readonly property string daemonBase: "http://127.0.0.1:8090"
    readonly property bool carplayMode: mode === "carplay"
    // The AP is started through the same tether path as CarPlay, so the mode alone
    // cannot tell them apart - the advertised SSID can.  Without this the Status
    // readout said "CarPlay AP" for an Android Auto access point.
    // The daemon reports which access point this is (aa_ap); the SSID is only a
    // fallback for an older daemon, so the AA button no longer lit up CarPlay state
    // and vice versa (2026-11-24).
    // Green only while the Android Auto access point is actually up, not merely
    // configured: the old SSID fallback matched the default SCOURGE-AA profile and
    // kept the button lit forever (2026-11-24).
    readonly property bool aaApActive: statusAaAp && apActive
    property bool statusAaAp: false
                                       || (carplayMode && carplaySsid === "SCOURGE-AA")

    function applyStatus(d) {
        // A parseable status file is not proof the daemon is alive: require a
        // fresh timestamp so a crashed daemon cannot leave stale controls.
        var upd = (typeof d.updated_ms === "number") ? d.updated_ms : 0
        serviceOk = upd > 0 && (Date.now() - upd) < 8000
        statusUpdatedAt = upd
        observationsFresh = d.observations_fresh === true
        mode = d.mode
        wlanOn = d.wlan_on
        driver = d.driver
        ip = d.ip
        ssid = d.ssid
        signalLevel = d.signal
        apActive = d.ap_active
        apSsid = d.ap_ssid
        statusAaAp = d.aa_ap === true
        apClients = d.ap_clients
        carplayReady = d.carplay_ready
        if (d.vbt_needs_reboot !== undefined) vbtNeedsReboot = d.vbt_needs_reboot === true
        carplayIe = d.carplay_ie === true
        carplayIap2 = d.carplay_iap2
        carplayMdns = d.carplay_mdns
        sshActive = d.ssh_active
        sshEnabled = d.ssh_enabled
        sshPort = d.ssh_port
        sshPortFallback = d.ssh_port_fallback
        vendorSsh22 = d.vendor_ssh_22
        secureConsole = d.secure_console
        debugConsole = d.debug_console
        imported = d.imported
        importedFrom = d.imported_from
        clientSsid = d.client_ssid
        if (d.client_networks !== undefined) clientNetworks = d.client_networks
        if (d.client_net_count !== undefined) clientNetCount = d.client_net_count
        carplaySsid = d.carplay_ssid
        carplayPsk = d.carplay_psk
        carplayPskSet = d.carplay_psk_set
        carplayBand = d.carplay_band
        carplayChannel = d.carplay_channel
        if (d.ap_method !== undefined) apMethod = "" + d.ap_method
        if (d.ap_purpose !== undefined) apPurpose = "" + d.ap_purpose
        if (d.ap_dhcp_pool !== undefined) apDhcpPool = "" + d.ap_dhcp_pool
        if (d.ap_dhcp_leases !== undefined) apDhcpLeases = d.ap_dhcp_leases
        carplayBtaddr = d.carplay_btaddr
        carplayId = d.carplay_id
        daemonMessage = d.message
        if (d.applying !== undefined) applying = d.applying === true
        // Show the card when work starts; keep it if the user opened it.
        btDiscoverable = d.bt_discoverable === true
        btPairable = d.bt_pairable === true
        pairMessage = d.pair_message || ""
        // home.json is the fast path; /status is only a fallback for a
        // daemon that predates it.  Once home.json has been read, ignore
        // the slower status snapshot so the two cannot flap.
        if (homeFallback && serviceOk)
            acceptHome({seq: d.home_seq, epoch: d.home_epoch,
                        source: d.home_source, monitor: d.home_monitor})
        homeSource = d.home_source || homeSource
        homeMonitor = d.home_monitor === true
    }

    function refreshFromFile() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 200 || xhr.status === 0) {
                    try {
                        applyStatus(JSON.parse(xhr.responseText))
                    } catch (e) {
                        serviceOk = false
                    }
                } else {
                    serviceOk = false
                }
            }
        }
        xhr.open("GET", "file:///rw_data/wifi/status.json")
        xhr.send()
    }

    function refresh() {
        // The daemon writes status.json every 2 s; reading the file avoids
        // running its expensive build_status() on every UI poll.  HTTP is
        // the fallback while the file is missing or unreadable.
        var fx = new XMLHttpRequest()
        fx.onreadystatechange = function() {
            if (fx.readyState === XMLHttpRequest.DONE) {
                if (fx.status === 200 || fx.status === 0) {
                    try {
                        applyStatus(JSON.parse(fx.responseText))
                        if (serviceOk) return
                    } catch (e) {
                        // fall through to HTTP
                    }
                }
                var xhr = new XMLHttpRequest()
                xhr.onreadystatechange = function() {
                    if (xhr.readyState === XMLHttpRequest.DONE &&
                        xhr.status === 200) {
                        try {
                            applyStatus(JSON.parse(xhr.responseText))
                        } catch (e) {
                            serviceOk = false
                        }
                    } else if (xhr.readyState === XMLHttpRequest.DONE) {
                        serviceOk = false
                    }
                }
                xhr.open("GET", daemonBase + "/status")
                xhr.send()
            }
        }
        fx.open("GET", "file:///rw_data/wifi/status.json")
        fx.send()
    }

    function post(path, done) {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                // Surface the daemon's answer.  A rejected request used to look like a
                // dead button: "/ap/start?channel=36" fails on this radio ("Bad
                // Channel"), the POST still returns, and nothing on screen changed
                // (measured 2026-11-26 - "the AA buttons do nothing").
                if (xhr.status !== 200 && xhr.status !== 0) {
                    daemonMessage = path + " failed: HTTP " + xhr.status
                    pendingPost = ""
                } else {
                    try {
                        var d = JSON.parse(xhr.responseText)
                        if (d && d.ok === false) {
                            daemonMessage = path + " failed: " + (d.error || "rejected")
                            pendingPost = ""
                        } else {
                            pendingPost = path
                        }
                    } catch (e) {
                        pendingPost = path
                    }
                }
                if (done) done()
                refresh()
            }
        }
        xhr.open("POST", daemonBase + path)
        xhr.send()
    }

    function get(path, done) {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (done) done(xhr)
                refresh()
            }
        }
        xhr.open("GET", daemonBase + path)
        xhr.send()
    }

    function refreshHome() {
        // The daemon writes this tiny file immediately on a Home/Star
        // event, so the overlay can close within ~0.5 s without forcing
        // the expensive full status build at 1 Hz.
        var xhr = new XMLHttpRequest()
        var serial = openSerial
        var readSerial = ++homeReadSerial
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (serial !== openSerial || readSerial < homeAcceptedRead) return
            if (xhr.status !== 200 && xhr.status !== 0) {
                homeFallback = true
                return
            }
            try {
                var d = JSON.parse(xhr.responseText)
                homeAcceptedRead = readSerial
                acceptHome(d)
                homeFileSeen = true
                homeFallback = false
            } catch (e) { homeFallback = true }
        }
        xhr.open("GET", "file:///rw_data/wifi/home.json")
        xhr.send()
    }

    function logUrl() {
        if (logSource === "install") return "/install?source=wifi"
        if (logSource === "stages") return "/install?source=stages"
        if (logSource === "battery" || logSource === "tweaks" || logSource === "root")
            return "/install?source=" + logSource
        return "/logs?source=" + logSource
    }
    function cycleLogSource() {
        var i = logSources.indexOf(logSource)
        logSource = logSources[(i + 1) % logSources.length]
        refreshConsole()
    }

    // Installer logs are plain files, and the loader writes /rw_data/combo/combo.log
    // from its first second - long before any of our daemons runs.  Reading them
    // through file:// is what makes the popup live from the very beginning.
    function fileLogPath() {
        if (logSource === "root") return "file:///rw_data/combo/combo.log"
        if (logSource === "stages") return "file:///rw_data/combo/stages.log"
        if (logSource === "battery") return "file:///rw_data/battery/install.log"
        if (logSource === "tweaks") return "file:///rw_data/tweaks/install.log"
        if (logSource === "install") return "file:///rw_data/wifi/install.log"
        return ""
    }

    function showLines(t) {
        // Keep the tail: QML cannot seek, so a big log is sliced after the read.
        if (t.length > 200000) t = t.substring(t.length - 200000)
        var lines = t.split("\n")
        var start = Math.max(0, lines.length - 40)
        consoleModel.clear()
        for (var i = start; i < lines.length; i++) {
            if (lines[i].length > 0)
                consoleModel.append({ line: lines[i] })
        }
    }

    function refreshConsole() {
        // The activity popup renders the same model, so refreshing must not depend
        // on the right-hand console being open.
        if (!showConsole) return
        var filePath = ""
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (xhr.status !== 200 && xhr.status !== 0) return
            var body = xhr.responseText || ""
            if (filePath !== "") { showLines(body); return }
            try {
                var d = JSON.parse(body)
                showLines(d.log || "")
            } catch (e) {}
        }
        if (filePath !== "") {
            xhr.open("GET", filePath)
        } else {
            xhr.open("GET", daemonBase + "/logs?source=" + consoleSource)
        }
        xhr.send()
    }

    function scan() {
        networks = []
        scanning = true
        get("/scan", function(xhr) {
            if (xhr.status !== 200 && xhr.status !== 202) { scanning = false; return }
            try {
                var d = JSON.parse(xhr.responseText)
                networks = d.networks || []
                scanning = d.scanning === true
            } catch (e) {
                networks = []
                scanning = false
            }
        })
    }

    function maskKey(s) {
        if (!s) return "(not imported)"
        return "\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022"
    }

    function bandText(b) {
        if (b === 1) return "2.4 GHz"
        if (b === 2) return "5 GHz"
        return "Auto"
    }

    function modeText() {
        if (!wlanOn || mode === "off") return "Off"
        if (mode === "carplay") return aaApActive ? "Android Auto AP" : "CarPlay AP"
        return "Client"
    }

    function ipText() {
        if (ip && ip.length) return ip
        return apActive ? "10.42.0.1" : "--"
    }

    function sshCommand() {
        return "ssh car@" + ipText() + " -p " + sshPort
    }

    Timer {
        interval: 500
        running: root.requested
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refreshHome()
    }

    Timer {
        interval: 750
        running: root.requested && root.scanning
        repeat: true
        onTriggered: root.get("/scan?poll=1", function(xhr) {
            if (xhr.status !== 200) { root.scanning = false; return }
            try {
                var d = JSON.parse(xhr.responseText)
                root.scanning = d.scanning === true
                root.networks = d.networks || []
            } catch (e) { root.scanning = false }
        })
    }

    Timer {
        interval: 2000
        running: true
        repeat: true
        triggeredOnStart: true
        // The AA tab reads its state from /aa/check, and it must be current while the
        // tab is open - otherwise the panel shows the values of the last press.
        onTriggered: { root.refresh(); if (root.currentTab === 4) root.aaCheck() }
    }

    // ---- background ---------------------------------------------------
    Rectangle {
        anchors.fill: parent
        color: "#0E0E0E"
    }

    // Full-screen input shield (APP_OVERLAY_CHECKLIST.md).  A bare
    // onClicked handler is not enough: presses, releases, drags and the
    // wheel must all be owned, and the real content must sit above it.
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

    // Local close control is above both content and console. It never waits
    // for a daemon request, association, scan or log operation to finish.
    Rectangle {
        id: closeButton
        objectName: "wifiExit"
        z: 10
        x: content.x + content.width - width
        y: 26
        width: 170
        height: 52
        radius: 26
        color: closeMouse.pressed ? "#4A4A4A" : "#2E2E2E"
        border.color: "#777777"
        border.width: 2
        QText {
            anchors.centerIn: parent
            text: "\u2715  Exit"
            font.pixelSize: 26
            textBold: true
            color: "#FFFFFF"
        }
        MouseArea {
            id: closeMouse
            anchors.fill: parent
            preventStealing: true
            onClicked: root.close()
        }
    }

    // Factory-style back chevron.  The stock Notifications page puts a thin "<"
    // in the top-left of its own header; our overlays are full-page, so they draw
    // their own and it does exactly what Exit does.
    Item {
        objectName: "wifiBack"
        z: 10
        // Sit in the launcher header band (the page starts at MENU_BAR_Y on screen),
        // in the same slot the stock Notifications page puts its back chevron, so it
        // reads as the header control instead of a second button below it.
        x: 132
        y: -40
        width: 52
        height: 56
        Rectangle {
            anchors.fill: parent
            radius: 26
            color: backMouse.pressed ? "#3A3A3A" : "transparent"
        }
        QText {
            anchors.centerIn: parent
            text: "\u2039"
            font.pixelSize: 54
            textBold: true
            color: "#D8D8D8"
        }
        MouseArea {
            id: backMouse
            anchors.fill: parent
            preventStealing: true
            onClicked: root.close()
        }
    }
    Item {
        id: content
        z: 2
        width: 1180
        height: root.height
        x: root.showConsole ? 16 : (root.width - width) / 2

        Image {
            id: headerIcon
            x: 62
            y: 24
            width: 56
            height: 56
            smooth: true
            source: "file:///app/share/AppHomeScreen/wifi/wifi_icon_n.png"
        }

        QText {
            id: title
            x: 136
            y: 18
            text: "WiFi"
            font.pixelSize: 46
            textBold: true
            color: "#FFFFFF"
        }

        QText {
            id: subtitle
            x: 138
            y: 72
            text: "WLAN \u00b7 Wireless CarPlay \u00b7 SSH"
            font.pixelSize: 22
            color: "#9A9A9A"
        }

        // ---- tabs -----------------------------------------------------
        Row {
            id: tabRow
            x: 0
            y: 110
            spacing: 10

            // Tab widths are sized so the five tabs AND the page-level action row
            // fit on one line: at 190/170 the fifth tab sat exactly under Import
            // USB, which is why "Android Auto" was unreachable on the car.
            // Widths: the five tabs end at 780 (130+130+180+110+190 + 4*10), the
            // page action row starts at 800 and ends at 1136 of the 1180 panel, so
            // nothing can slide under anything else.
            WifiButton {
                width: 130
                label: "Status"
                selected: root.currentTab === 0
                onClicked: root.currentTab = 0
            }
            WifiButton {
                width: 130
                label: "Client"
                selected: root.currentTab === 1
                onClicked: root.currentTab = 1
            }
            WifiButton {
                width: 180
                label: "CarPlay AP"
                selected: root.currentTab === 2
                onClicked: root.currentTab = 2
            }
            WifiButton {
                width: 110
                label: "SSH"
                selected: root.currentTab === 3
                onClicked: root.currentTab = 3
            }
            WifiButton {
                width: 190
                label: "Android Auto"
                selected: root.currentTab === 4
                onClicked: { root.currentTab = 4; root.aaCheck() }
            }
        }

        // One action row for the whole page: importing from USB and saving the
        // log bundle are page-level, not per-tab, and the three identical copies
        // that used to live in Status/Client/CarPlay AP were the same POST.
        Row {
            x: 800
            y: 110
            spacing: 8

            WifiButton {
                width: 104
                label: "Import USB"
                buttonEnabled: root.serviceOk
                onClicked: root.post("/import")
            }
            WifiButton {
                width: 96
                label: "Save logs"
                buttonEnabled: root.serviceOk
                onClicked: root.post("/logs/dump")
            }
            WifiButton {
                width: 120
                label: root.showKeypad ? "Hide keypad" : "Manual join"
                buttonEnabled: true
                onClicked: root.showKeypad = !root.showKeypad
            }

        }

        // ================= STATUS ====================================
        Item {
            id: statusTab
            x: 0
            y: 170
            width: 1180
            height: 450
            visible: root.currentTab === 0

            Rectangle {
                x: 0
                y: 0
                width: 570
                height: 130
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 28
                    y: 20
                    text: "WLAN interface"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 28
                    y: 56
                    text: root.wlanOn ? "On" : "Off"
                    font.pixelSize: 46
                    textBold: true
                    color: root.wlanOn ? "#4CAF50" : "#FFFFFF"
                }
                QText {
                    x: 28
                    y: 108
                    text: !root.observationsFresh ? "Refreshing interface state..."
                                      : root.driver ? "bcmdhd loaded \u00b7 wlan0 present"
                                      : "driver / wlan0 not detected"
                    font.pixelSize: 21
                    color: root.driver ? "#7A7A7A" : "#FFB300"
                }

                WifiSwitch {
                    anchors.right: parent.right
                    anchors.rightMargin: 24
                    anchors.verticalCenter: parent.verticalCenter
                    checked: root.wlanOn
                    switchEnabled: root.serviceOk
                    onToggled: root.post("/wlan?on=" + (root.wlanOn ? "0" : "1"))
                }
            }

            Rectangle {
                x: 610
                y: 0
                width: 570
                height: 130
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 28
                    y: 20
                    text: "Current mode"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 28
                    y: 56
                    text: root.modeText()
                    font.pixelSize: 46
                    textBold: true
                    color: "#FFFFFF"
                }
                QText {
                    x: 28
                    y: 108
                    text: "IP " + root.ipText()
                    font.pixelSize: 21
                    color: "#7A7A7A"
                }
            }

            Row {
                x: 0
                y: 140
                spacing: 12

                WifiButton {
                    width: 220
                    label: "Off"
                    selected: root.mode === "off" || !root.wlanOn
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/mode?value=off")
                }
                WifiButton {
                    width: 220
                    label: "Client"
                    selected: root.mode === "client" && root.wlanOn
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/mode?value=client")
                }
                WifiButton {
                    width: 240
                    label: "CarPlay AP"
                    // Mutually exclusive with the Android Auto button: both are the
                    // same "carplay" role on the radio, so keying this off mode alone
                    // lit CarPlay and AA at once and read as two active modes
                    // (2026-11-24 screenshot).
                    selected: root.mode === "carplay" && root.wlanOn && !root.aaApActive
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/mode?value=carplay")
                }
                // Same radio path, AA preset: the status surface used to offer only
                // Off/Client/CarPlay, so there was no way to start the AA AP there.
                WifiButton {
                    width: 240
                    label: root.aaApActive ? "Android Auto (on)" : "Android Auto"
                    selected: root.aaApActive
                    buttonEnabled: true
                    onClicked: {
                        // aa=1: serve Android Auto, not CarPlay, so the car does not
                        // announce CarPlay when the driver pressed Android Auto.
                        root.post("/ap/start?ssid=" + encodeURIComponent(root.carplaySsid) + "&channel=149&aa=1")
                        root.aaCheck()
                    }
                }
                WifiButton {
                    width: 200
                    label: "Reconnect"
                    // One tap instead of Off -> Client: a station that comes back as
                    // "managed but stuck in SCANNING" only recovers when the interface
                    // is torn down and rebuilt, and doing that by hand was the only way
                    // on the car (2026-11-25).  /restart does exactly that in one call,
                    // and it also re-imports nothing, so the credentials stay as they are.
                    // Deliberately NOT gated on serviceOk: that flag needs status.json
                    // to be younger than 8 s, and a daemon stuck in an apply (or one
                    // that just lost the link) is exactly when the button is needed -
                    // it was grey on the car in that state (2026-11-25).  The POST
                    // reports the real outcome on the message line.
                    buttonEnabled: true
                    onClicked: root.post("/restart")
                }
                // Import USB / Save logs live in the shared action row above.
            }

            Rectangle {
                x: 0
                y: 212
                width: 1180
                height: 96
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 14
                    text: "Imported credentials"
                    font.pixelSize: 24
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 48
                    text: root.imported
                          ? ("Client nets (" + root.clientNetCount + ", priority order): " +
                             (root.clientNetworks || root.clientSsid || "--") +
                             "   \u00b7   CarPlay: " + (root.carplaySsid || "--") +
                             "   \u00b7   " + root.importedFrom)
                          : "No wifi.txt imported yet. Put wifi.txt on the USB root, then press Import USB."
                    font.pixelSize: 21
                    color: root.imported ? "#FFFFFF" : "#FFB300"
                    width: 1130
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                x: 0
                y: 318
                width: 570
                height: 120
                radius: 14
                color: "#1B1B1B"
                border.color: root.apActive ? "#2E7D32" : "#333333"
                border.width: root.apActive ? 2 : 1

                QText {
                    x: 24
                    y: 14
                    text: "Access point"
                    font.pixelSize: 24
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 44
                    text: root.apActive
                          ? (root.apSsid + "  \u00b7  " + root.apClients + " client(s)")
                          : "Not running"
                    font.pixelSize: 24
                    color: root.apActive ? "#4CAF50" : "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 78
                    text: "Key: " + (root.carplayPsk ? root.carplayPsk : "(not set)")
                    font.pixelSize: 24
                    textBold: true
                    color: "#FFFFFF"
                    width: 520
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                x: 610
                y: 318
                width: 570
                height: 96
                radius: 14
                color: "#1B1B1B"
                border.color: root.sshActive ? "#2E7D32" : "#333333"
                border.width: root.sshActive ? 2 : 1

                QText {
                    x: 24
                    y: 14
                    text: "SSH"
                    font.pixelSize: 24
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 48
                    text: root.sshActive
                          ? ("Listening on " + root.sshPort + "  \u00b7  " + root.sshCommand())
                          : "Stopped"
                    font.pixelSize: root.sshActive ? 21 : 24
                    color: root.sshActive ? "#4CAF50" : "#FFFFFF"
                    width: 530
                    elide: Text.ElideRight
                }
            }

            QText {
                x: 4
                y: 428
                width: 1172
                text: root.daemonMessage
                font.pixelSize: 20
                color: "#7A7A7A"
                elide: Text.ElideRight
            }
        }

        // ================= CLIENT ====================================
        Item {
            id: clientTab
            x: 0
            y: 170
            width: 1180
            height: 450
            visible: root.currentTab === 1

            Rectangle {
                x: 0
                y: 0
                width: 570
                height: 180
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "Client network"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 58
                    text: root.clientSsid ? root.clientSsid : "(no SSID imported)"
                    font.pixelSize: 40
                    textBold: true
                    color: "#FFFFFF"
                    width: 520
                    elide: Text.ElideRight
                }
                QText {
                    x: 24
                    y: 112
                    text: (root.clientNetCount > 1 && root.clientNetworks
                           ? ("Priority: " + root.clientNetworks + "   \u00b7   ")
                           : "") +
                          "Key: " + root.maskKey(root.clientSsid)
                    font.pixelSize: 22
                    color: "#7A7A7A"
                }
                QText {
                    x: 24
                    y: 142
                    text: root.ssid ? ("Connected to " + root.ssid + "  \u00b7  " +
                                     root.signalLevel + " dBm  \u00b7  " + root.ip)
                                   : "Not connected"
                    font.pixelSize: 21
                    color: root.ssid ? "#4CAF50" : "#FFB300"
                    width: 520
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                x: 610
                y: 0
                width: 570
                height: 180
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "How it works"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 58
                    width: 520
                    text: "Put wifi.txt on the USB root with ssid= and psk=, then press Import USB. "
                          + "The client uses the imported SSID/key. Passwords are not typed on screen."
                    font.pixelSize: 22
                    color: "#B0B0B0"
                    wrapMode: Text.WordWrap
                }
            }

            Row {
                x: 0
                y: 192
                spacing: 12

                WifiButton {
                    width: 220
                    label: "Scan"
                    buttonEnabled: root.serviceOk
                    onClicked: root.scan()
                }
                WifiButton {
                    width: 260
                    label: "Connect imported"
                    buttonEnabled: root.serviceOk && root.clientSsid !== ""
                    onClicked: root.post("/client/connect")
                }
                WifiButton {
                    width: 220
                    label: "Disconnect"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/client/disconnect")
                }
                // Import USB is in the shared action row above.
            }

            Rectangle {
                x: 0
                y: 264
                width: 1180
                height: 186
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 10
                    text: "Scan results"
                    font.pixelSize: 24
                    color: "#9A9A9A"
                }

                ListView {
                    x: 18
                    y: 44
                    width: 1144
                    height: 132
                    clip: true
                    model: root.networks
                    delegate: Rectangle {
                      width: 1120
                      height: 44
                      color: "transparent"
                      QText {
                          x: 8
                          anchors.verticalCenter: parent.verticalCenter
                          width: 520
                          text: modelData.ssid
                          font.pixelSize: 24
                          color: "#FFFFFF"
                          elide: Text.ElideRight
                      }
                      QText {
                          x: 540
                          anchors.verticalCenter: parent.verticalCenter
                          width: 100
                          text: modelData.security
                          font.pixelSize: 22
                          color: "#9A9A9A"
                      }
                      // Band and channel come from the scan frequency: the whole
                      // point of listing networks is knowing which one is 5 GHz.
                      QText {
                          x: 650
                          anchors.verticalCenter: parent.verticalCenter
                          width: 92
                          text: (modelData.band !== undefined && modelData.band !== "?")
                                ? modelData.band + " GHz" : "\u2014"
                          font.pixelSize: 22
                          color: modelData.band === "5" ? "#4FC3F7" : "#9A9A9A"
                      }
                      QText {
                          x: 748
                          anchors.verticalCenter: parent.verticalCenter
                          width: 86
                          text: modelData.channel ? "ch " + modelData.channel : ""
                          font.pixelSize: 22
                          color: "#9A9A9A"
                      }
                      QText {
                          x: 840
                          anchors.verticalCenter: parent.verticalCenter
                          width: 270
                          text: (modelData.signal || 0) + " dBm"
                                + "  " + ((modelData.signal || -99) >= -60 ? "strong"
                                         : (modelData.signal || -99) >= -75 ? "ok" : "weak")
                          font.pixelSize: 22
                          color: (modelData.signal || -99) >= -70 ? "#8BC34A" : "#FFB300"
                      }
                  }
                }

                QText {
                    x: 24
                    y: 152
                    width: 1130
                    text: root.daemonMessage !== "" ? root.daemonMessage
                          : (root.networks.length === 0 ? "Press Scan to list nearby networks." : "")
                    font.pixelSize: 20
                    color: root.daemonMessage !== "" ? "#FFB300" : "#7A7A7A"
                    elide: Text.ElideRight
                }
            }
        }

        // ---- manual join (on-screen keyboard) ------------------------
        // The head unit has no hardware keyboard, so the entry field and the
          // keypad are the same widget: tap the field, then the keys.  The key
          // is held in memory only for the POST and is never written to a file.
          property string manualSsid: ""
          property string manualPsk: ""
          property string keyTarget: "ssid"
          // Declared on the page root: the Manual join button and the overlay both
          // bind to it.  Without the property the Item stayed visible for ever,
          // and the keypad covered the page on the car (2026-11-23).
          property bool shiftOn: false
          property bool showKeypad: false

          // ---- Android Auto readiness (same radio, second consumer) ------
          property string aaVerdict: ""
          property string aaReason: ""
          property string aaApSsid: ""
          property string aaApIp: ""
          property bool aaApReady: false
          property int aaBand: 0
          property int aaChannel: 0
          property bool aaBluez: false
          property bool aaVbtd: false
          // The vendor vbtd exports the SDP/RFCOMM entry points AA needs, so
          // "no BlueZ" alone is not a blocker (docs/18 §5c).
          property bool aaVendorApi: false

          function aaCheck() {
              get("/aa/check", function(xhr) {
                  if (xhr.status !== 200) return
                  try {
                      var d = JSON.parse(xhr.responseText)
                      if (d.message) daemonMessage = d.message
                    if (d.ap) {
                          aaApReady = d.ap.ready === true
                          aaApSsid = d.ap.ssid || ""
                          aaApIp = d.ap.ip || ""
                          aaBand = d.ap.band || 0
                          aaChannel = d.ap.channel || 0
                      }
                      if (d.bt) {
                aaBluez = d.bt.bluez === true
                aaVbtd = d.bt.vbtd === true
                aaVendorApi = d.bt.sdp_api === true && d.bt.rfcomm_api === true
            }
                      aaVerdict = d.verdict || ""
                  // Why the verdict says that: the daemon names the missing half.
                  aaReason = d.reason || ""
                  } catch (e) {}
              })
          }


          function keyAppend(c) {
              if (keyTarget === "psk") {
                if (manualPsk.length < 63) manualPsk += c
            } else {
                if (manualSsid.length < 32) manualSsid += c
            }
          }
          function keyBackspace() {
            if (keyTarget === "psk") manualPsk = manualPsk.substring(0, manualPsk.length - 1)
            else manualSsid = manualSsid.substring(0, manualSsid.length - 1)
          }
          function manualJoin() {
            if (manualSsid.length < 1) { root.message = "Type the network name first" ; return }
            root.post("/client/pair?ssid=" + encodeURIComponent(manualSsid)
                      + "&psk=" + encodeURIComponent(manualPsk))
            manualPsk = ""
          }

          // An overlay, not a tab section: the Client tab is already full, and a
          // keypad that covers the list while typing is what a phone does too.
          Item {
            x: 0
            y: 170
            width: 720
            height: 440
            z: 6
            visible: root.showKeypad === true

            Rectangle {
                width: 1180
                height: 60
                radius: 12
                color: "#1B1B1B"
                QText {
                    x: 16; anchors.verticalCenter: parent.verticalCenter
                    text: "Network"; font.pixelSize: 22; color: "#9A9A9A"
                }
                Rectangle {
                    x: 120; y: 10; width: 520; height: 40; radius: 8
                    color: root.keyTarget === "ssid" ? "#263238" : "#161616"
                    border.color: root.keyTarget === "ssid" ? "#4FC3F7" : "#333333"
                    border.width: 2
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.keyTarget = "ssid"
                    }
                    QText {
                        x: 10; anchors.verticalCenter: parent.verticalCenter
                        width: 490
                        text: root.manualSsid !== "" ? root.manualSsid : "tap to type the SSID"
                        font.pixelSize: 24
                        color: root.manualSsid !== "" ? "#FFFFFF" : "#666666"
                        elide: Text.ElideRight
                    }
                }
                QText {
                    x: 660; anchors.verticalCenter: parent.verticalCenter
                    text: "Key"; font.pixelSize: 22; color: "#9A9A9A"
                }
                Rectangle {
                    x: 730; y: 10; width: 300; height: 40; radius: 8
                    color: root.keyTarget === "psk" ? "#263238" : "#161616"
                    border.color: root.keyTarget === "psk" ? "#4FC3F7" : "#333333"
                    border.width: 2
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.keyTarget = "psk"
                    }
                    QText {
                        x: 10; anchors.verticalCenter: parent.verticalCenter
                        width: 270
                        text: root.manualPsk !== ""
                              ? new Array(root.manualPsk.length + 1).join("*")
                              : "(open network if empty)"
                        font.pixelSize: 24
                        color: root.manualPsk !== "" ? "#FFFFFF" : "#666666"
                        elide: Text.ElideRight
                    }
                }
                WifiButton {
                    x: 1040; y: 10; width: 130; height: 40
                    label: "Join"
                    buttonEnabled: root.serviceOk && root.manualSsid !== ""
                    onClicked: root.manualJoin()
                }
            }

            // QWERTY, three letter rows plus a control row.  Keys are plain
            // rectangles: no focus widgets, so nothing can steal the touch.
            // Nested Repeaters: the inner one must read the OUTER model's string
              // through a property on the row delegate.  Using modelData inside
              // the inner delegate drew the inner index instead of a character
              // (the keypad showed 0 1 2 3 … on the car).
              Repeater {
                  model: ["1234567890.", "qwertyuiop_", "asdfghjkl-", "zxcvbnm"]
                  delegate: Row {
                      property string keyRow: modelData
                      x: 0
                      y: 72 + index * 52
                      spacing: 6
                      Repeater {
                          model: keyRow.length
                          delegate: Rectangle {
                              property string keyChar: keyRow.charAt(index)
                              width: 60
                              height: 46
                              radius: 8
                            color: "#232323"
                            QText {
                                anchors.centerIn: parent
                                text: root.shiftOn ? keyChar.toUpperCase() : keyChar
                                  font.pixelSize: 24
                                  color: "#FFFFFF"
                              }
                              MouseArea {
                                anchors.fill: parent
                                  onClicked: root.keyAppend(root.shiftOn ? keyChar.toUpperCase() : keyChar)
                              }
                          }
                      }
                  }
            }

            Row {
                x: 0
                y: 300
                spacing: 6
                Rectangle {
                    width: 150; height: 46; radius: 8
                    color: root.shiftOn ? "#0E5C8A" : "#232323"
                    QText { anchors.centerIn: parent; text: "Shift"; font.pixelSize: 22; color: "#FFFFFF" }
                    MouseArea { anchors.fill: parent; onClicked: root.shiftOn = !root.shiftOn }
                }
                Rectangle {
                    width: 200; height: 46; radius: 8; color: "#232323"
                    QText { anchors.centerIn: parent; text: "Space"; font.pixelSize: 22; color: "#FFFFFF" }
                    MouseArea { anchors.fill: parent; onClicked: root.keyAppend(" ") }
                }
                Rectangle {
                    width: 160; height: 46; radius: 8; color: "#232323"
                    QText { anchors.centerIn: parent; text: "Delete"; font.pixelSize: 22; color: "#FFFFFF" }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.keyBackspace()
                        onPressAndHold: { root.manualPsk = ""; root.manualSsid = "" }
                    }
                }
                QText {
                    x: 540; anchors.verticalCenter: parent.verticalCenter
                    width: 620
                    text: root.keyTarget === "psk"
                          ? "Typing: key (8-63 characters, or the 64 hex digits)"
                          : "Typing: network name (1-32 bytes)"
                    font.pixelSize: 20
                    color: "#7A7A7A"
                    elide: Text.ElideRight
                }
            }
          }

          // ================= CARPLAY ===================================
          Item {
            id: carplayTab
            x: 0
            y: 170
            width: 1180
            height: 450
            visible: root.currentTab === 2

            Rectangle {
                x: 0
                y: 0
                width: 570
                height: 240
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "CarPlay access point"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 58
                    text: root.carplaySsid
                    font.pixelSize: 40
                    textBold: true
                    color: "#FFFFFF"
                    width: 520
                    elide: Text.ElideRight
                }
                QText {
                    x: 24
                    y: 112
                    text: "Key: " + (root.carplayPsk ? root.carplayPsk : "(not set)")
                    font.pixelSize: 22
                    color: "#7A7A7A"
                }
                QText {
                    x: 24
                    y: 142
                    text: "Band: " + root.bandText(root.carplayBand) +
                          "   \u00b7   Channel: " + root.carplayChannel +
                          "   \u00b7   Max clients: " + (root.apActive ? root.apClients : 2)
                    font.pixelSize: 21
                    color: "#B0B0B0"
                }
                QText {
                    x: 24
                    y: 172
                    text: root.apMethodText()
                    font.pixelSize: 19
                    color: "#7A7A7A"
                }
                QText {
                    x: 24
                    y: 198
                    text: root.apActive ? "AP running" : "AP stopped"
                    font.pixelSize: 24
                    textBold: true
                    color: root.apActive ? "#4CAF50" : "#FFB300"
                }
                QText {
                    x: 24
                    y: 226
                    visible: root.apPurpose === "aa"
                    text: root.apVerdictText()
                    font.pixelSize: 19
                    color: root.aaBandMismatch ? "#FF7043" : "#7A7A7A"
                }
            }

            Rectangle {
                x: 610
                y: 0
                width: 570
                height: 240
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "CarPlay transport"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 50
                    text: "iAP2: " + (root.carplayIap2 ? "running" : "stopped")
                    font.pixelSize: 24
                    color: root.carplayIap2 ? "#4CAF50" : "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 82
                    text: "mDNS: " + (root.carplayMdns ? "running" : "stopped")
                    font.pixelSize: 24
                    color: root.carplayMdns ? "#4CAF50" : "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 114
                    text: root.vbtNeedsReboot
                          ? "Reboot needed to apply the wireless CarPlay setting"
                          : "Ready: " + (!root.observationsFresh ? "checking" : (root.carplayReady ? "yes" : "no"))
                    font.pixelSize: 24
                    color: root.vbtNeedsReboot ? "#FFB300" : (root.carplayReady ? "#4CAF50" : "#FFB300")
                }
                QText {
                    x: 24
                    y: 140
                    width: 520
                    text: "Press Start CarPlay AP, then pick the car on the iPhone: "
                          + "Settings > General > CarPlay > Available Cars."
                    font.pixelSize: 19
                    color: "#B0B0B0"
                    wrapMode: Text.WordWrap
                }
                QText {
                    x: 24
                    y: 192
                    width: 520
                    text: "iPhone already paired via Bluetooth? Just pick the car "
                          + "under Settings > General > CarPlay once the AP is running."
                    font.pixelSize: 17
                    color: "#8FD18F"
                    wrapMode: Text.WordWrap
                }
            }

            Row {
                x: 0
                y: 252
                spacing: 10

                WifiButton {
                    width: 230
                    label: "Start CarPlay AP"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/carplay/pair")
                }
                WifiButton {
                    width: 210
                    label: "Stop pairing"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/carplay/pair/stop")
                }
                WifiButton {
                    width: 220
                    label: "Restart stack"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/carplay/restart")
                }
                WifiButton {
                    width: 170
                    label: "Stop AP"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/carplay/stop")
                }
                // Import USB is in the shared action row above.
            }

            QText {
                x: 0
                y: 308
                width: 1180
                text: (root.pairMessage !== "" && root.daemonMessage !== "")
                      ? (root.pairMessage + "   \u00b7   " + root.daemonMessage)
                      : (root.pairMessage !== "" ? root.pairMessage : root.daemonMessage)
                font.pixelSize: 18
                color: (root.pairMessage !== "" || root.daemonMessage !== "") ? "#FFB300" : "#7A7A7A"
                elide: Text.ElideRight
            }

            Rectangle {
                x: 0
                y: 326
                width: 1180
                height: 124
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 8
                    text: "Identity sent in the CarPlay information element"
                    font.pixelSize: 24
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 40
                    text: "Manufacturer: Hyundai   \u00b7   Model: Ioniq 5   \u00b7   Name: Ioniq 5"
                    font.pixelSize: 21
                    color: "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 68
                    text: "Bluetooth: " + (root.carplayBtaddr ? root.carplayBtaddr : "(not detected)")
                    font.pixelSize: 21
                    color: "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 96
                    text: "Device ID: " + (root.carplayId ? root.carplayId : "(not detected)")
                    font.pixelSize: 21
                    color: "#FFFFFF"
                }
            }
        }

        // ================= SSH =======================================
        Item {
            id: sshTab
            x: 0
            y: 170
            width: 1180
            height: 450
            visible: root.currentTab === 3

            Rectangle {
                x: 0
                y: 0
                width: 570
                height: 180
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "SSH server"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 56
                    text: root.sshActive ? "Running" : "Stopped"
                    font.pixelSize: 46
                    textBold: true
                    color: root.sshActive ? "#4CAF50" : "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 116
                    text: "Port " + root.sshPort + "  \u00b7  user car"
                    font.pixelSize: 22
                    color: "#B0B0B0"
                }
                QText {
                    x: 24
                    y: 146
                    text: root.sshCommand()
                    font.pixelSize: 21
                    color: "#7A7A7A"
                }

                WifiSwitch {
                    anchors.right: parent.right
                    anchors.rightMargin: 24
                    anchors.verticalCenter: parent.verticalCenter
                    checked: root.sshEnabled
                    switchEnabled: root.serviceOk
                    onToggled: root.post("/ssh?enabled=" + (root.sshEnabled ? "0" : "1") + "&port=" + root.sshPort)
                }
            }

            Rectangle {
                x: 610
                y: 0
                width: 570
                height: 180
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "Credentials"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 58
                    text: "User: car"
                    font.pixelSize: 24
                    color: "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 92
                    text: "Password: electric"
                    font.pixelSize: 24
                    color: "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 132
                    width: 520
                    text: "Password auth is enabled at your request. Change it after install. "
                          + "You can also import authorized_keys from the USB root."
                    font.pixelSize: 20
                    color: "#FFB300"
                    wrapMode: Text.WordWrap
                }
            }

            Row {
                x: 0
                y: 192
                spacing: 12

                WifiButton {
                    width: 260
                    label: "Import SSH keys"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/ssh/import")
                }
                WifiButton {
                    width: 220
                    label: "Restart SSH"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/ssh?enabled=1&port=" + root.sshPort)
                }
                WifiButton {
                    width: 220
                    label: "Stop SSH"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/ssh?enabled=0&port=" + root.sshPort)
                }
            }

            Rectangle {
                x: 0
                y: 264
                width: 1180
                height: 186
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 10
                    text: "Notes"
                    font.pixelSize: 24
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 44
                    width: 1120
                    text: "SSH is only reachable over the head unit's WLAN. In CarPlay AP mode "
                          + "the address is normally 10.42.0.1. In client mode use the IP shown "
                          + "on the Status tab. The app adds a firewall rule for wlan0 and the "
                          + "selected port. Dropbear is static and does not depend on the "
                          + "head unit's glibc."
                    font.pixelSize: 20
                    color: "#B0B0B0"
                    wrapMode: Text.WordWrap
                }
                QText {
                    x: 24
                    y: 108
                    width: 1120
                    visible: root.sshPortFallback || root.vendorSsh22
                    text: root.vendorSsh22
                          ? ("Port 22 is already in use (vendor/debug SSH). The app uses port " + root.sshPort + ".")
                          : ("The configured SSH port was busy. The app uses port " + root.sshPort + ".")
                    font.pixelSize: 18
                    color: "#FFB300"
                    wrapMode: Text.WordWrap
                }
                QText {
                    x: 24
                    y: 146
                    text: "Secure console: " + (root.debugConsole ? "debug (serial/root)" :
                          (root.secureConsole === 1 ? "secure" : "unknown"))
                    font.pixelSize: 18
                    color: root.debugConsole ? "#4CAF50" : "#7A7A7A"
                }
                QText {
                    x: 24
                    y: 164
                    text: "Authorized keys: " + (root.sshActive ? "loaded from /rw_data/wifi/ssh/authorized_keys" : "not loaded")
                    font.pixelSize: 18
                    color: "#7A7A7A"
                }
            }
        }
    }

    // ---- live log console (wide layouts) ---------------------------
    Rectangle {
        id: consolePanel
        visible: root.showConsole
        x: root.width - 436
        y: 20
        width: 420
        height: 600
        radius: 12
        color: "#0B0B0B"
        border.color: "#333333"
        border.width: 1
        z: 5

        QText {
            x: 16
            y: 18
            text: "Live log"
            font.pixelSize: 22
            color: "#9A9A9A"
        }

        Row {
            x: 16
            y: 56
            spacing: 8

            WifiButton {
                width: 130
                height: 42
                label: "Daemon"
                selected: root.consoleSource === "daemon"
                onClicked: { root.consoleSource = "daemon"; root.refreshConsole() }
            }
            WifiButton {
                width: 150
                height: 42
                label: "Supplicant"
                selected: root.consoleSource === "wpa"
                onClicked: { root.consoleSource = "wpa"; root.refreshConsole() }
            }
            WifiButton {
                width: 90
                height: 42
                label: "Dump"
                onClicked: root.post("/logs/dump")
            }
        }

        Rectangle {
            x: 12
            y: 104
            width: 396
            height: 484
            radius: 8
            color: "#050505"
            border.color: "#222222"
            border.width: 1
            clip: true

            ListView {
                id: consoleView
                anchors.fill: parent
                anchors.margins: 10
                clip: true
                model: consoleModel
                delegate: QText {
                    width: consoleView.width
                    text: line
                    font.pixelSize: 13
                    color: "#8FD18F"
                    wrapMode: Text.WrapAnywhere
                }
                onCountChanged: positionViewAtEnd()
                Component.onCompleted: positionViewAtEnd()
            }

            QText {
                anchors.centerIn: parent
                visible: consoleModel.count === 0
                text: "(no log lines yet)"
                font.pixelSize: 18
                color: "#555555"
            }
        }
    }

        // ================= ANDROID AUTO ==============================
        // Same radio, second consumer: wireless AA needs the 5 GHz AP + DHCP we
        // already own, plus a Bluetooth RFCOMM bootstrap this unit cannot do (no
        // BlueZ, only the vendor vbtd).  The tab starts the neutral AP and shows
        // the measured verdict, so both can be tested from one screen.
        Item {
            id: aaTab
            x: 0
            y: 170
            width: 1180
            height: 450
            visible: root.currentTab === 4

            // Same visual language as the CarPlay tab: two cards, the access-point
            // facts on the left, the transport state on the right, then the action
            // row, the daemon message and a full-width verdict card.
            Rectangle {
                x: 0
                y: 0
                width: 570
                height: 240
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "Android Auto access point"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 58
                    text: root.aaApSsid !== "" ? root.aaApSsid : root.carplaySsid
                    font.pixelSize: 40
                    textBold: true
                    color: "#FFFFFF"
                    width: 520
                    elide: Text.ElideRight
                }
                QText {
                    x: 24
                    y: 112
                    text: "Key: " + (root.carplayPsk ? root.carplayPsk : "(not set)")
                    font.pixelSize: 22
                    color: "#7A7A7A"
                }
                QText {
                    x: 24
                    y: 142
                    text: "Band: " + root.bandText(root.carplayBand) +
                          "   \u00b7   Channel: " + root.carplayChannel +
                          "   \u00b7   Max clients: " + (root.apActive ? root.apClients : 2)
                    font.pixelSize: 21
                    color: "#B0B0B0"
                }
                QText {
                    x: 24
                    y: 176
                    // "undefined" leaked into this line on the car (screenshot
                    // 2026-11-26): the status field can be absent, and a missing
                    // JSON member renders as that word, so test the value.
                    text: (root.apActive ? "AP running" : "AP stopped")
                          + (root.aaApIp && root.aaApIp !== "undefined"
                             ? "   \u00b7   " + root.aaApIp : "")
                    font.pixelSize: 24
                    textBold: true
                    color: root.apActive ? "#4CAF50" : "#FFB300"
                }
            }

            Rectangle {
                x: 610
                y: 0
                width: 570
                height: 240
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "Android Auto transport"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 50
                    text: "Bluetooth: " + (root.aaVbtd ? "vendor stack (vbtd) running"
                                                     : "no BT stack seen")
                    font.pixelSize: 24
                    color: root.aaVbtd ? "#4CAF50" : "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 82
                    // BlueZ is absent on this unit by design; the vendor stack is the
                    // API AA needs, so this line reports what is actually usable.
                    text: "SDP/RFCOMM API: " + (root.aaVendorApi ? "present" : "absent")
                          + (root.aaBluez ? "   \u00b7   BlueZ present" : "")
                    font.pixelSize: 24
                    color: root.aaVendorApi ? "#4CAF50" : "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 114
                    text: "Ready: " + (!root.observationsFresh ? "checking"
                                      : (root.aaApReady ? "yes" : "no"))
                    font.pixelSize: 24
                    color: root.aaApReady ? "#4CAF50" : "#FFB300"
                }
                QText {
                    x: 24
                    y: 148
                    width: 520
                    text: "Start AA AP, then pair the phone over Bluetooth: the car "
                          + "appears in Android Auto > Connection settings."
                    font.pixelSize: 19
                    color: "#B0B0B0"
                    wrapMode: Text.WordWrap
                }
                QText {
                    x: 24
                    y: 196
                    width: 520
                    text: "The CarPlay advertisement stays off in this mode, so the "
                          + "car does not answer as a CarPlay device."
                    font.pixelSize: 17
                    color: "#8FD18F"
                    wrapMode: Text.WordWrap
                }
            }

            Row {
                x: 0
                y: 252
                spacing: 10

                WifiButton {
                    width: 230
                    label: root.aaApReady ? "Restart AA AP" : "Start AA AP"
                    // Not gated on serviceOk: a grey, dead-looking button was exactly
                    // what the car showed, and the POST reports the real error through
                    // the daemon message line below.
                    buttonEnabled: true
                    onClicked: {
                        root.post("/ap/start?ssid=" + encodeURIComponent(root.carplaySsid) + "&channel=149&aa=1")
                        root.aaCheck()
                    }
                }
                WifiButton {
                    width: 170
                    label: "Stop AP"
                    buttonEnabled: true
                    // /ap/stop, not /carplay/stop: the latter switches the radio OFF
                    // (mode=off + wlan_on=0) and left the unit unreachable after an AA
                    // test (measured 2026-11-26: mode=carplay, wlan_on=0, no address,
                    // and the whole box looked dead).  Stopping the AP means "be a
                    // station again".
                    onClicked: { root.post("/ap/stop"); root.aaCheck() }
                }
                WifiButton {
                    width: 150
                    label: "Re-check"
                    buttonEnabled: true
                    onClicked: root.aaCheck()
                }
            }

            QText {
                x: 0
                y: 308
                width: 1180
                text: root.pendingPost !== ""
                      ? "sent: " + root.pendingPost + " (waiting for the daemon)"
                      : (root.daemonMessage !== "" ? root.daemonMessage : "no daemon message")
                font.pixelSize: 18
                color: root.daemonMessage !== "" ? "#FFB300" : "#7A7A7A"
                elide: Text.ElideRight
            }

            Rectangle {
                x: 0
                y: 326
                width: 1180
                height: 124
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 8
                    text: "Wireless Android Auto verdict"
                    font.pixelSize: 24
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 40
                    width: 700
                    text: root.aaVerdict !== "" ? root.aaVerdict : "press Re-check"
                    font.pixelSize: 24
                    textBold: true
                    color: root.aaVerdict === "aa-wireless-possible" ? "#4CAF50"
                           : root.aaVerdict === "" ? "#7A7A7A" : "#FFB300"
                    elide: Text.ElideRight
                }
                QText {
                    x: 24
                    y: 70
                    width: 1120
                    text: root.aaReason
                    font.pixelSize: 20
                    color: root.aaVerdict === "aa-wireless-possible" ? "#4CAF50" : "#B0B0B0"
                    elide: Text.ElideRight
                    visible: root.aaReason !== ""
                  }
                  QText {
                    x: 24
                    y: 100
                    width: 1120
                    text: "The AP half is what the phone joins; the Bluetooth bootstrap "
                          + "needs an RFCOMM channel with our UUID. If the vendor stack "
                          + "exposes no way to register one, bridge board (docs/18 route B)."
                    font.pixelSize: 19
                    color: "#7A7A7A"
                    wrapMode: Text.WordWrap
                }
            }
        }


    ListModel { id: consoleModel }

    Timer {
        // The right-hand console refreshes while it is open.
        interval: 2000
        running: root.visible && root.showConsole
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refreshConsole()
    }
}
