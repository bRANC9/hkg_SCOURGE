/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors */
/* SPDX-License-Identifier: AGPL-3.0-only */
/* Client-mode decisions the car depends on, tested with command mocks:
 * no network blocks in the supplicant config (ConnMan owns the networks),
 * radio-level association truth, "no wifi technology" must not be fatal, and
 * the static address fallback for a unit with no DHCP client at all. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int fast_usleep(useconds_t);
#define usleep fast_usleep
#define WIFI_TEST 1
#define main daemon_main
#include "../src/wifi_daemon.c"
#undef main
#undef usleep

static int fast_usleep(useconds_t us) { (void)us; return 0; }

static char seen[96][512];
static int seen_n;
static bool routed, have_ip;
static const char *tech_reply, *iw_link_reply;

static int mock(char *out, size_t n, const char *cmd)
{
    if (seen_n < 96) snprintf(seen[seen_n++], 512, "%s", cmd);
    if (out && n) out[0] = 0;
    if (strstr(cmd, "connmanctl technologies")) {
        if (out && n) snprintf(out, n, "%s", tech_reply ? tech_reply : "");
        return 0;
    }
    if (strstr(cmd, "wpa_cli") && strstr(cmd, "ping")) return 0;
    if (strstr(cmd, "iw dev wlan0 link")) {
        if (out && n) snprintf(out, n, "%s", iw_link_reply ? iw_link_reply : "Not connected.\n");
        return 0;
    }
    if (strstr(cmd, "iw dev wlan0 info")) {
        if (out && n) snprintf(out, n, "Interface wlan0\n\ttype managed\n");
        return 0;
    }
    if (strstr(cmd, "ifconfig wlan0")) {
        if (out && n)
            snprintf(out, n, have_ip
                     ? "wlan0 Link encap:Ethernet\n          inet addr:192.168.1.60  Mask:255.255.255.0\n"
                     : "wlan0 Link encap:Ethernet\n");
        return 0;
    }
    if (strstr(cmd, "ip addr add")) {
        if (strstr(cmd, "192.168.1.60/24")) have_ip = true;
        return 0;
    }
    if (strstr(cmd, "scan_results")) {
        if (out && n)
            snprintf(out, n,
                     "bssid / frequency / signal level / flags / ssid\n"
                     "f8:64:b8:c9:99:88\t2427\t-74\t[WPA2-PSK-CCMP][ESS]\tFranci_wifi\n"
                     "a4:2b:b0:11:22:33\t5180\t-61\t[WPA2-PSK-CCMP][ESS]\tFranci_wifi-ext_5G\n");
        return 0;
    }
    if (strstr(cmd, "ip route replace")) { routed = true; return 0; }
    return 0;
}

static bool ran(const char *needle)
{
    for (int i = 0; i < seen_n; i++) if (strstr(seen[i], needle)) return true;
    return false;
}

int main(void)
{
    mkdir(APP_DIR, 0755);
    test_command = mock;
    test_iface = 1;
    test_owned_wpa = 1;
    config_defaults();
    g_cfg.mode = MODE_CLIENT;
    g_cfg.wlan_on = true;
    set_net_ssid(0, "Franci_wifi");
    set_net_psk(0, "fixture-key-123");

    /* 1. Client mode: no network blocks - ConnMan owns the networks there, and a
     * local block made it answer with Interface.Disconnect. */
    unlink(WPA_CONF);
    ensure_wpa_conf();
    char conf[4096] = "";
    read_file(WPA_CONF, conf, sizeof(conf));
    assert(strstr(conf, "ctrl_interface="));
    assert(!strstr(conf, "network={"));
    /* AP mode keeps the profile: there ConnMan must keep out. */
    g_cfg.mode = MODE_CARPLAY;
    ensure_wpa_conf();
    read_file(WPA_CONF, conf, sizeof(conf));
    assert(strstr(conf, "network={") && strstr(conf, "Franci_wifi"));

    /* 2. Whitespace-tolerant SSID compare plus radio-level truth: the link was
     * up ("iw ... SSID: Franci_wifi") while the daemon claimed none was visible. */
    assert(ssid_same("Franci_wifi", "Franci_wifi "));
    assert(ssid_same(" Franci_wifi", "Franci_wifi"));
    assert(!ssid_same("Franci_wifi", "Franci_wifi-ext_5G"));
    g_cfg.mode = MODE_CLIENT;
    iw_link_reply = "Connected to f8:64:b8:c9:99:88 (on wlan0)\n\tSSID: Franci_wifi\n\tsignal: -74 dBm\n";
    assert(iw_link_any_configured());
    assert(client_associated());

    /* 3. No wifi technology (ConnMan lists cellular only) must not be fatal -
     * that was the "WiFi technology did not power on" loop - while a technology
     * that exists and stays off is still a real failure. */
    tech_reply = "/net/connman/technology/cellular\n  Name = Cellular\n  Powered = True\n";
    assert(ensure_wifi_ready());
    /* Client mode never demands Powered=True either: the technology is ConnMan's
     * business, the supplicant answering is ours. */
    tech_reply = "/net/connman/technology/wifi\n  Name = WiFi\n  Powered = False\n";
    assert(ensure_wifi_ready());
    /* AP mode still refuses to start on an unpowered technology. */
    g_cfg.mode = MODE_CARPLAY;
    assert(!ensure_wifi_ready());
    g_cfg.mode = MODE_CLIENT;

    /* 4. Static fallback for a unit with no udhcpc/dhcpcd/dhclient at all. */
    have_ip = false;
    routed = false;
    write_file_atomic(APP_DIR "/static_ip", "192.168.1.60/24\n", 0600);
    write_file_atomic(APP_DIR "/static_gw", "192.168.1.1\n", 0600);
    assert(dhcp_acquire());
    assert(ran("ip addr add 192.168.1.60/24"));
    assert(ran("ip route replace default via 192.168.1.1") && routed);


    /* 5. The scan list carries band/channel so the UI can label a network
     *    2.4 vs 5 GHz next to its RSSI: the split comes from the frequency. */
    assert(!strcmp(band_label(5180), "5") && band_channel(5180) == 36);
    assert(!strcmp(band_label(5500), "5") && band_channel(5500) == 100);
    assert(!strcmp(band_label(2427), "2.4") && band_channel(2427) == 4);
    assert(!strcmp(band_label(2412), "2.4") && band_channel(2412) == 1);
    assert(!strcmp(band_label(0), "?") && band_channel(0) == 0);
    printf("PASS: client path (no network blocks, radio truth, no-technology, static fallback, scan bands)\n");
    return 0;
}
