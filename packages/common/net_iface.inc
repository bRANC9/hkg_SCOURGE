/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * net_iface.inc - the handful of interface ioctls a userspace network tool needs
 * on this unit.
 *
 * Why an include and not shell-outs: the head unit has no DHCP client at all
 * (measured 2026-11-25: udhcpc/dhcpcd/dhclient/dhcp6c/pump/netctl absent, and
 * busybox has no udhcpc applet), and a tool that shells out cannot be tested
 * without root.  These calls are plain ioctl()s: no libnl, no fork, no /proc
 * parsing.
 *
 * Everything is IPv4 because that is what the unit's networks are; errors are
 * returned, never logged, so the caller owns the wording.
 */
#ifndef NET_IFACE_INC
#define NET_IFACE_INC

/* struct ifreq, IFNAMSIZ and IFF_UP sit behind the misc/default feature set,
 * so an includer that already pulled in a system header has to define
 * _DEFAULT_SOURCE (or _GNU_SOURCE) itself before doing so.  This guard covers
 * the includer that starts with this file. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/* An includer uses a subset of these helpers; silence -Wunused-function the
 * same way packages/common/ap_dhcpd.inc does. */
#if defined(__GNUC__)
#define NETI_UNUSED __attribute__((unused))
#else
#define NETI_UNUSED
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

/* A name that does not fit in ifr_name is a caller bug, not something to
 * truncate silently: truncation would address a different interface. */
static NETI_UNUSED int neti_name(struct ifreq *ifr, const char *ifname)
{
	size_t n = strlen(ifname);

	if (n == 0 || n >= IFNAMSIZ)
		return -1;
	memset(ifr, 0, sizeof *ifr);
	memcpy(ifr->ifr_name, ifname, n);
	return 0;
}

static NETI_UNUSED int neti_socket(void)
{
	return socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
}

static NETI_UNUSED int neti_mac(const char *ifname, uint8_t mac[6])
{
	struct ifreq ifr;
	int fd = neti_socket();
	int rc;

	if (fd < 0)
		return -1;
	if (neti_name(&ifr, ifname) < 0) {
		close(fd);
		return -1;
	}
	rc = ioctl(fd, SIOCGIFHWADDR, &ifr);
	close(fd);
	if (rc < 0)
		return -1;
	memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
	return 0;
}

static NETI_UNUSED int neti_ifindex(const char *ifname)
{
	struct ifreq ifr;
	int fd = neti_socket();
	int rc;

	if (fd < 0)
		return -1;
	if (neti_name(&ifr, ifname) < 0) {
		close(fd);
		return -1;
	}
	rc = ioctl(fd, SIOCGIFINDEX, &ifr);
	close(fd);
	return rc < 0 ? -1 : ifr.ifr_ifindex;
}

/* Host-order IPv4 address of the interface, 0 when it has none. */
static NETI_UNUSED int neti_ipv4(const char *ifname, uint32_t *ip)
{
	struct ifreq ifr;
	struct sockaddr_in *sin;
	int fd = neti_socket();
	int rc;

	if (fd < 0)
		return -1;
	if (neti_name(&ifr, ifname) < 0) {
		close(fd);
		return -1;
	}
	rc = ioctl(fd, SIOCGIFADDR, &ifr);
	close(fd);
	if (rc < 0)
		return -1;
	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	*ip = ntohl(sin->sin_addr.s_addr);
	return 0;
}

static NETI_UNUSED int neti_set_ipv4(const char *ifname, uint32_t addr,
				     uint32_t mask)
{
	struct ifreq ifr;
	struct sockaddr_in *sin;
	int fd = neti_socket();
	int rc = -1;

	if (fd < 0)
		return -1;
	if (neti_name(&ifr, ifname) < 0) {
		close(fd);
		return -1;
	}
	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(addr);
	if (ioctl(fd, SIOCSIFADDR, &ifr) < 0)
		goto out;
	if (neti_name(&ifr, ifname) < 0)
		goto out;
	sin = (struct sockaddr_in *)&ifr.ifr_netmask;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(mask);
	if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0)
		goto out;
	rc = 0;
out:
	close(fd);
	return rc;
}

static NETI_UNUSED int neti_up(const char *ifname)
{
	struct ifreq ifr;
	int fd = neti_socket();
	int rc;

	if (fd < 0)
		return -1;
	if (neti_name(&ifr, ifname) < 0) {
		close(fd);
		return -1;
	}
	if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
		close(fd);
		return -1;
	}
	ifr.ifr_flags = (short)(ifr.ifr_flags | IFF_UP | IFF_RUNNING);
	rc = ioctl(fd, SIOCSIFFLAGS, &ifr);
	close(fd);
	return rc;
}

/* Default route through gw on this interface.  rt_dev is a plain char *, so the
 * name needs storage that outlives the ioctl. */
static NETI_UNUSED int neti_default_route(const char *ifname, uint32_t gw)
{
	struct {
		struct rtentry rt;
		char dev[IFNAMSIZ];
	} route;
	struct sockaddr_in *sin;
	size_t n = strlen(ifname);
	int fd = neti_socket();
	int rc;

	if (fd < 0)
		return -1;
	if (n == 0 || n >= IFNAMSIZ) {
		close(fd);
		return -1;
	}
	memset(&route, 0, sizeof route);
	memcpy(route.dev, ifname, n);
	route.rt.rt_dev = route.dev;

	sin = (struct sockaddr_in *)&route.rt.rt_dst;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(INADDR_ANY);
	sin = (struct sockaddr_in *)&route.rt.rt_genmask;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(INADDR_ANY);
	sin = (struct sockaddr_in *)&route.rt.rt_gateway;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(gw);
	route.rt.rt_flags = RTF_UP | RTF_GATEWAY;

	rc = ioctl(fd, SIOCADDRT, &route.rt);
	close(fd);
	return rc;
}

/* Dotted quad, or 0 with *ok = 0 on a malformed string. */
static NETI_UNUSED uint32_t neti_ipv4_parse(const char *s, int *ok)
{
	struct in_addr a;
	uint32_t v = 0;

	if (ok)
		*ok = 0;
	if (!s || inet_pton(AF_INET, s, &a) != 1)
		return 0;
	v = ntohl(a.s_addr);
	if (ok)
		*ok = 1;
	return v;
}

static NETI_UNUSED void neti_ipv4_str(uint32_t ip, char *out, size_t n)
{
	struct in_addr a;

	a.s_addr = htonl(ip);
	if (!inet_ntop(AF_INET, &a, out, n))
		snprintf(out, n, "?");
}

#endif /* NET_IFACE_INC */
