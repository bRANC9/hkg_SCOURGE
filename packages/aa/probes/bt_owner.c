/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * bt_owner — one question, asked on the head unit: can *our* process own the
 * Bluetooth sockets that the Android Auto wireless bootstrap needs, or does the
 * vendor stack (`vbtd`) hold them?
 *
 * Why this is the question that decides the route
 * -----------------------------------------------
 * The AA bootstrap needs two things on the Bluetooth side (docs/22 §5b):
 *
 *   1. an SDP record that contains only the AA UUID, so the phone can find the
 *      RFCOMM channel;
 *   2. an RFCOMM listener on channel 8 that speaks the five-stage codec in
 *      `aa_btboot`.
 *
 * There is no BlueZ on the unit, but BlueZ is not the only way to get those:
 * SDP and RFCOMM are *kernel* sockets.  The SDP server is an L2CAP socket bound
 * to PSM 0x0001 and the RFCOMM listener is a socket bound to channel 8.  If
 * those binds succeed, we can serve both from our own process and `vbtd` is
 * only needed for discoverability/pairing — which its CommonAPI adapter
 * already exposes (docs/22 §5b).  If they fail with EADDRINUSE, the vendor
 * daemon owns them and the on-unit route needs vendor ABI work.
 *
 * No BlueZ headers are used: the two `sockaddr` layouts below are the kernel
 * uapi ones, and they are static-asserted so a wrong size cannot silently
 * turn into a wrong bind.
 *
 * Build for the unit: `make target-bt-owner` (musl, baseline ISA, static-pie).
 * Host self-test:     `make bt-owner-selftest`.
 *
 * RESULT (2026-11-23, docs/22 §5b): on this head unit the answer is already
 * known and it is "unavailable".  The kernel loads no Bluetooth module at all
 * (the dmesg module list has no bluetooth/l2cap/rfcomm/hci_uart) and
 * /sys/class/bluetooth never appears, because the vendor daemon `vbtd` *is* the
 * stack on /dev/ttyS0.  So the expectation here is EAFNOSUPPORT or ENODEV, not
 * a free socket, and the real route is the vendor's exported SDP/RFCOMM API
 * reached through the LD_PRELOAD shim in tools/bttap.  This probe stays as a
 * diagnostic: it records the exact errno and the adapter/module/fd evidence.
 */

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif

#define BTPROTO_L2CAP 0
#define BTPROTO_RFCOMM 3

/* The SDP server PSM is a protocol constant, not a choice. */
#define SDP_PSM 0x0001u
/* The channel Android Auto's SDP record announces (docs/22 §5b). */
#define AA_RFCOMM_CHANNEL 8u
/* RFCOMM has 30 usable channels; the sweep stops below that to stay readable. */
#define RFCOMM_SWEEP_MAX 14u

/* Kernel uapi layouts (linux/bluetooth/l2cap.h, rfcomm.h). */
struct sockaddr_l2 {
    uint16_t l2_family;
    uint16_t l2_psm;        /* little-endian on the wire */
    uint8_t  l2_bdaddr[6];
    uint16_t l2_cid;
    uint8_t  l2_bdaddr_type;
};

struct sockaddr_rc {
    uint16_t rc_family;
    uint8_t  rc_bdaddr[6];
    uint8_t  rc_channel;
};

_Static_assert(sizeof(struct sockaddr_l2) == 14u, "L2CAP bind needs the 14-byte uapi layout");
_Static_assert(sizeof(struct sockaddr_rc) == 10u, "RFCOMM bind needs the 10-byte uapi layout");

enum bt_owner {
    BT_OWNER_FREE = 0,
    BT_OWNER_TAKEN,
    BT_OWNER_UNAVAILABLE,
    BT_OWNER_DENIED,
    BT_OWNER_OTHER
};

/* One line per outcome, because the reader of this output is deciding a route,
 * not debugging a socket call. */
static const char *classify(int err)
{
    switch (err) {
    case 0:
        return "free — our process can bind it";
    case EADDRINUSE:
        return "EADDRINUSE — already owned (vendor daemon)";
    case EACCES:
    case EPERM:
        return "EPERM — not root, or the capability is missing";
    case EAFNOSUPPORT:
        return "EAFNOSUPPORT — kernel Bluetooth support is not loaded";
    case EPROTONOSUPPORT:
        return "EPROTONOSUPPORT — the l2cap/rfcomm module is not loaded";
    case ENODEV:
        return "ENODEV — no Bluetooth adapter is registered";
    case EBUSY:
        return "EBUSY — in use by another owner";
    case EINVAL:
        return "EINVAL — the kernel rejected the address layout";
    default:
        return "unexpected errno";
    }
}

static enum bt_owner grade(int err)
{
    if (err == 0)
        return BT_OWNER_FREE;
    if (err == EADDRINUSE || err == EBUSY)
        return BT_OWNER_TAKEN;
    if (err == EACCES || err == EPERM)
        return BT_OWNER_DENIED;
    if (err == EAFNOSUPPORT || err == EPROTONOSUPPORT || err == ENODEV)
        return BT_OWNER_UNAVAILABLE;
    return BT_OWNER_OTHER;
}

static const char *grade_name(enum bt_owner owner)
{
    switch (owner) {
    case BT_OWNER_FREE:
        return "ours-possible";
    case BT_OWNER_TAKEN:
        return "vendor-owned";
    case BT_OWNER_UNAVAILABLE:
        return "kernel-unavailable";
    case BT_OWNER_DENIED:
        return "permission-denied";
    default:
        return "other-error";
    }
}

/* Try to become the SDP server: bind L2CAP PSM 1 and listen.  Returns 0 on
 * success, else the errno of the failing step. */
#ifndef AA_BT_OWNER_SELFTEST
static int try_sdp_psm(uint16_t psm)
{
    const int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (fd < 0)
        return errno;

    struct sockaddr_l2 addr;
    memset(&addr, 0, sizeof addr);
    addr.l2_family = (uint16_t)AF_BLUETOOTH;
    addr.l2_psm = psm;                  /* x86 unit: little-endian already */

    int err = 0;
    if (bind(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof addr) != 0)
        err = errno;
    else if (listen(fd, 1) != 0)
        err = errno;
    (void)close(fd);
    return err;
}

/* Try to become the AA RFCOMM listener on one channel. */
static int try_rfcomm(uint8_t channel)
{
    const int fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (fd < 0)
        return errno;

    struct sockaddr_rc addr;
    memset(&addr, 0, sizeof addr);
    addr.rc_family = (uint16_t)AF_BLUETOOTH;
    addr.rc_channel = channel;

    int err = 0;
    if (bind(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof addr) != 0)
        err = errno;
    else if (listen(fd, 1) != 0)
        err = errno;
    (void)close(fd);
    return err;
}

/* The adapter address is what the phone will pair against; it comes from sysfs
 * because reading it over HCI would need the device handle `vbtd` owns. */
static void print_adapter(void)
{
    const char *path = "/sys/class/bluetooth/hci0/address";
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        printf("adapter: no %s (kernel Bluetooth not registered, or another name)\n", path);
        DIR *dir = opendir("/sys/class/bluetooth");
        if (dir == NULL) {
            printf("         /sys/class/bluetooth does not exist\n");
            return;
        }
        printf("         /sys/class/bluetooth entries:");
        const struct dirent *entry = NULL;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] != '.')
                printf(" %s", entry->d_name);
        }
        printf("\n");
        (void)closedir(dir);
        return;
    }
    char text[32];
    if (fgets(text, (int)sizeof text, file) == NULL) {
        printf("adapter: %s unreadable\n", path);
        (void)fclose(file);
        return;
    }
    (void)fclose(file);
    text[strcspn(text, "\n")] = '\0';
    printf("adapter: hci0 %s (this is what the phone pairs with)\n", text);
}

/* Which Bluetooth pieces the kernel actually has loaded.  A missing module is a
 * different answer from a bound port: `modprobe` fixes one, nothing fixes the
 * other. */
static void print_modules(void)
{
    const char *wanted[] = {"bluetooth", "bluetooth_l2cap", "l2cap", "rfcomm", "hci_uart"};
    FILE *file = fopen("/proc/modules", "r");
    if (file == NULL) {
        printf("modules: /proc/modules unreadable\n");
        return;
    }
    char line[512];
    while (fgets(line, (int)sizeof line, file) != NULL) {
        for (size_t i = 0u; i < sizeof wanted / sizeof wanted[0]; i++) {
            const size_t len = strlen(wanted[i]);
            if (strncmp(line, wanted[i], len) == 0 && line[len] == ' ')
                printf("modules: %s loaded\n", wanted[i]);
        }
    }
    (void)fclose(file);
}

static int run_probe(void)
{
    printf("aa bt owner probe — can this process own the AA Bluetooth sockets?\n");
    print_adapter();
    print_modules();

    const int sdp = try_sdp_psm(SDP_PSM);
    printf("sdp   PSM 0x%04x  %-58s [%s]\n", (unsigned)SDP_PSM, classify(sdp),
           grade_name(grade(sdp)));

    const int aa = try_rfcomm((uint8_t)AA_RFCOMM_CHANNEL);
    printf("rfcomm ch %-3u   %-58s [%s]\n", (unsigned)AA_RFCOMM_CHANNEL, classify(aa),
           grade_name(grade(aa)));

    unsigned free_channels = 0u;
    char free_list[64];
    size_t used = 0u;
    free_list[0] = '\0';
    for (unsigned channel = 1u; channel <= RFCOMM_SWEEP_MAX; channel++) {
        if (try_rfcomm((uint8_t)channel) != 0)
            continue;
        free_channels++;
        const int written = snprintf(free_list + used, sizeof free_list - used, "%s%u",
                                     used == 0u ? "" : ",", channel);
        if (written > 0 && (size_t)written < sizeof free_list - used)
            used += (size_t)written;
    }
    printf("rfcomm free 1..%u: %u channel(s)%s%s\n", (unsigned)RFCOMM_SWEEP_MAX, free_channels,
           free_channels > 0u ? " — " : "", free_list);

    /* The verdict, in the terms the route decision uses. */
    printf("verdict: sdp=%s rfcomm%u=%s\n", grade_name(grade(sdp)), (unsigned)AA_RFCOMM_CHANNEL,
           grade_name(grade(aa)));
    if (grade(sdp) == BT_OWNER_FREE && grade(aa) == BT_OWNER_FREE) {
        printf("verdict: the unit can serve the AA bootstrap itself — vendor API only for"
               " pairing/discoverability, no vendor RFCOMM/SDP ABI needed\n");
        return 0;
    }
    if (grade(sdp) == BT_OWNER_TAKEN || grade(aa) == BT_OWNER_TAKEN) {
        printf("verdict: the vendor daemon holds the socket — the on-unit route needs its"
               " ABI, or the bridge board\n");
        return 2;
    }
    if (grade(sdp) == BT_OWNER_DENIED || grade(aa) == BT_OWNER_DENIED) {
        printf("verdict: the bind needs privileges — re-run as root (on the unit we are)\n");
        return 4;
    }
    printf("verdict: the kernel side is not ready — load bluetooth/l2cap/rfcomm first, then"
           " re-run\n");
    return 3;
}
#endif /* AA_BT_OWNER_SELFTEST */

#ifdef AA_BT_OWNER_SELFTEST
static unsigned failures;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        failures++;
}

/* The classification is the part that has to be right on the unit: the rest is
 * a socket call whose errno speaks for itself. */
static void selftest(void)
{
    check("size l2cap", sizeof(struct sockaddr_l2) == 14u);
    check("size rfcomm", sizeof(struct sockaddr_rc) == 10u);
    check("free", grade(0) == BT_OWNER_FREE);
    check("taken", grade(EADDRINUSE) == BT_OWNER_TAKEN && grade(EBUSY) == BT_OWNER_TAKEN);
    check("denied", grade(EACCES) == BT_OWNER_DENIED && grade(EPERM) == BT_OWNER_DENIED);
    check("unavailable", grade(EAFNOSUPPORT) == BT_OWNER_UNAVAILABLE &&
                             grade(EPROTONOSUPPORT) == BT_OWNER_UNAVAILABLE &&
                             grade(ENODEV) == BT_OWNER_UNAVAILABLE);
    check("other", grade(EINVAL) == BT_OWNER_OTHER);
    check("names", strcmp(grade_name(BT_OWNER_FREE), "ours-possible") == 0 &&
                       strcmp(grade_name(BT_OWNER_TAKEN), "vendor-owned") == 0);
    check("text free", strstr(classify(0), "can bind") != NULL);
    check("text taken", strstr(classify(EADDRINUSE), "vendor daemon") != NULL);
    check("sdp psm constant", SDP_PSM == 0x0001u);
    check("aa channel constant", AA_RFCOMM_CHANNEL == 8u);

    printf(failures == 0u ? "bt_owner: all checks passed\n" : "bt_owner: %u check(s) failed\n",
           failures);
}

int main(void)
{
    selftest();
    return failures == 0u ? 0 : 1;
}
#else
int main(void)
{
    return run_probe();
}
#endif
