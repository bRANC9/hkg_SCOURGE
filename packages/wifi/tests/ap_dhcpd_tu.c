/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors */
/* SPDX-License-Identifier: AGPL-3.0-only */
/* standalone-TU check for packages/common/ap_dhcpd.inc: the WiFi1b includer
 * reported that errno/stdbool/usleep needed extra defines and that the /proc path
 * snprintf tripped format-truncation.  This file proves none of that is needed. */
#define APP_DIR "/tmp/apd-probe"
#define AP_DHCP_IP            "10.42.0.1"
#define AP_DHCP_CIDR          "10.42.0.1/24"
#define AP_DHCP_NETMASK       "255.255.255.0"
#define AP_DHCP_BROADCAST     "10.42.0.255"
#define AP_DHCP_POOL_FIRST    10
#define AP_DHCP_POOL_LAST     200
#define AP_DHCP_LEASE_FILE    APP_DIR "/dhcp.leases"
#define AP_DHCP_PID_FILE      APP_DIR "/dhcpd.pid"
#define AP_DHCP_LOG_FILE      APP_DIR "/dhcpd.log"
#define AP_DHCP_PROC_NAME     "apd_probe"
#include "../../common/ap_dhcpd.inc"   /* packages/common, from packages/wifi/tests */

static void probe_log(const char *line) { (void)line; }
static int probe_run(const char *cmd) { (void)cmd; return 0; }
static bool probe_stop(void) { return true; }

int main(void)
{
    ap_dhcp_ops_t ops = { probe_log, probe_run, probe_stop };
    (void)apd_dhcpd_running();
    (void)apd_leases_count();
    (void)apd_iface_set_static(&ops);
    apd_iface_flush(&ops);
    apd_dhcpd_stop();
    return apd_dhcpd_start(&ops) ? 0 : 0;
}
