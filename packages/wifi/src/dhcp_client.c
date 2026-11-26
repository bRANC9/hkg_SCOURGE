/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */
/* dhcp_client - the DHCP client this unit does not have.
 *
 * Measured on the head unit (2026-11-25): there is no DHCP client at all -
 * udhcpc, dhcpcd, dhclient, dhcp6c, pump and netctl are absent, busybox has no
 * udhcpc applet, and ConnMan refuses the wifi technology ("Method SetProperty
 * ... doesn't exist").  So client mode could associate and still be unreachable:
 * wifi_daemon's dhcp_acquire() tried four shell clients that cannot exist on
 * this image and then fell back to the static address in /rw_data/wifi/static_ip.
 *
 * This tool is the missing first element of that chain.  It speaks the same wire
 * format as packages/common/ap_dhcpd.inc (our own AP server): BOOTP header, magic
 * cookie 0x63825363, and only the options we actually consume.
 *
 * Design notes:
 *   - the protocol state machine takes an injected I/O object, so the whole
 *     DISCOVER/OFFER/REQUEST/ACK exchange is testable without root or a network
 *     (tests/dhcp_client_tu.c plays the server);
 *   - the lease is applied with packages/common/net_iface.inc ioctls, never by
 *     shelling out to a tool the image does not have;
 *   - it never touches an interface that already has an address (the caller
 *     checks that first); flushing somebody else's address is not its job.
 *
 * Usage:
 *   dhcp_client [-i IFACE] [-t SECONDS] [--lease-file PATH] [--hostname NAME]
 *               [--no-config] [--release] [--quiet]
 * Exit: 0 lease obtained (and applied), 1 usage, 2 no answer, 3 apply failed.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#include "../../common/net_iface.inc"

#define DCLI_SERVER_PORT 67
#define DCLI_CLIENT_PORT 68
#define DCLI_MAGIC 0x63825363u
#define DCLI_BOOTREQUEST 1
#define DCLI_BOOTREPLY 2
#define DCLI_HTYPE_ETHER 1
#define DCLI_HLEN_ETHER 6
#define DCLI_FLAG_BROADCAST 0x8000
#define DCLI_HEADER_LEN 236
#define DCLI_MIN_LEN 300
#define DCLI_MAX_LEN 576
#define DCLI_LEASE_DEFAULT 3600
#define DCLI_OPTIONS_MAX 256

enum dcli_msg {
	DCLI_DISCOVER = 1,
	DCLI_OFFER = 2,
	DCLI_REQUEST = 3,
	DCLI_ACK = 5,
	DCLI_NAK = 6,
	DCLI_RELEASE = 7,
};

enum dcli_opt {
	DCLI_OPT_MASK = 1,
	DCLI_OPT_ROUTER = 3,
	DCLI_OPT_DNS = 6,
	DCLI_OPT_HOSTNAME = 12,
	DCLI_OPT_MTU = 26,
	DCLI_OPT_BROADCAST = 28,
	DCLI_OPT_REQUESTED_IP = 50,
	DCLI_OPT_LEASE_TIME = 51,
	DCLI_OPT_MSG_TYPE = 53,
	DCLI_OPT_SERVER_ID = 54,
	DCLI_OPT_PARAM_LIST = 55,
	DCLI_OPT_MAX_SIZE = 57,
	DCLI_OPT_T1 = 58,
	DCLI_OPT_T2 = 59,
	DCLI_OPT_CLIENT_ID = 61,
	DCLI_OPT_END = 255,
};

struct dcli_lease {
	uint32_t ip;		/* yiaddr, host order */
	uint32_t mask;
	uint32_t router;
	uint32_t dns;
	uint32_t server;
	uint32_t lease_seconds;
	uint32_t t1;
	uint32_t t2;
	int have_router;
	int have_dns;
};

struct dcli_io {
	/* Send one DHCP payload.  dst_ip is INADDR_BROADCAST until we have a lease. */
	int (*send)(void *ctx, const void *buf, size_t len, uint32_t dst_ip);
	/* Wait up to timeout_ms for one frame: length, 0 on timeout, -1 on error. */
	int (*recv)(void *ctx, void *buf, size_t cap, int timeout_ms);
};

typedef void (*dcli_log_fn)(void *ctx, const char *line);

struct dcli_state {
	const struct dcli_io *io;
	void *io_ctx;
	dcli_log_fn log;
	void *log_ctx;
	uint8_t mac[6];
	uint32_t xid;
};

static void dcli_log(struct dcli_state *st, const char *fmt, ...)
{
	char line[256];
	va_list ap;

	if (!st->log)
		return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	st->log(st->log_ctx, line);
}

static uint32_t dcli_rand32(void)
{
	struct timespec ts;
	uint32_t v;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	v = (uint32_t)ts.tv_nsec ^ ((uint32_t)ts.tv_sec << 16) ^ (uint32_t)getpid();
	v ^= v << 13;
	v ^= v >> 17;
	v ^= v << 5;
	return v;
}

static uint32_t dcli_be32(const uint8_t *p)
{
	uint32_t be;

	memcpy(&be, p, 4);
	return ntohl(be);
}

/* ------------------------------------------------------------ wire format -- */

static size_t dcli_put_opt(uint8_t *buf, size_t off, size_t cap,
			   uint8_t code, const void *data, size_t len)
{
	if (len > 255 || off + 2 + len > cap)
		return off;
	buf[off++] = code;
	buf[off++] = (uint8_t)len;
	if (len)
		memcpy(buf + off, data, len);
	return off + len;
}

static size_t dcli_put_opt_u32(uint8_t *buf, size_t off, size_t cap,
			       uint8_t code, uint32_t value)
{
	uint32_t be = htonl(value);

	return dcli_put_opt(buf, off, cap, code, &be, sizeof be);
}

static size_t dcli_put_opt_u8(uint8_t *buf, size_t off, size_t cap,
			      uint8_t code, uint8_t value)
{
	return dcli_put_opt(buf, off, cap, code, &value, 1);
}

/* One BOOTP + DHCP message.  Returns its length, 0 when it cannot be built. */
static size_t dcli_build(uint8_t *buf, size_t cap, enum dcli_msg type,
			 uint32_t xid, const uint8_t mac[6],
			 uint32_t requested_ip, uint32_t server_id,
			 const char *hostname, uint32_t client_ip)
{
	static const uint8_t params[] = {
		DCLI_OPT_MASK, DCLI_OPT_ROUTER, DCLI_OPT_DNS, DCLI_OPT_BROADCAST,
		DCLI_OPT_LEASE_TIME, DCLI_OPT_SERVER_ID, DCLI_OPT_T1, DCLI_OPT_T2,
	};
	static const uint8_t cookie[4] = { 0x63, 0x82, 0x53, 0x63 };
	uint8_t cid[DCLI_HLEN_ETHER + 1];
	uint16_t flags = htons(DCLI_FLAG_BROADCAST);
	uint32_t be32;
	size_t off;

	if (cap < DCLI_MIN_LEN)
		return 0;
	memset(buf, 0, cap);
	buf[0] = DCLI_BOOTREQUEST;
	buf[1] = DCLI_HTYPE_ETHER;
	buf[2] = DCLI_HLEN_ETHER;
	/* hops stays 0 */
	be32 = htonl(xid);
	memcpy(buf + 4, &be32, 4);
	memcpy(buf + 10, &flags, 2);	/* broadcast: we have no address yet */
	be32 = htonl(client_ip);	/* ciaddr, only for release/renew */
	memcpy(buf + 12, &be32, 4);
	memcpy(buf + 28, mac, DCLI_HLEN_ETHER);

	off = DCLI_HEADER_LEN;
	memcpy(buf + off, cookie, sizeof cookie);
	off += sizeof cookie;
	off = dcli_put_opt_u8(buf, off, cap, DCLI_OPT_MSG_TYPE, (uint8_t)type);
	cid[0] = DCLI_HTYPE_ETHER;
	memcpy(cid + 1, mac, DCLI_HLEN_ETHER);
	off = dcli_put_opt(buf, off, cap, DCLI_OPT_CLIENT_ID, cid, sizeof cid);
	off = dcli_put_opt(buf, off, cap, DCLI_OPT_PARAM_LIST, params, sizeof params);
	off = dcli_put_opt_u32(buf, off, cap, DCLI_OPT_MAX_SIZE, DCLI_MAX_LEN);
	if (hostname && hostname[0])
		off = dcli_put_opt(buf, off, cap, DCLI_OPT_HOSTNAME, hostname,
				   strlen(hostname));
	if (type == DCLI_REQUEST) {
		off = dcli_put_opt_u32(buf, off, cap, DCLI_OPT_REQUESTED_IP, requested_ip);
		off = dcli_put_opt_u32(buf, off, cap, DCLI_OPT_SERVER_ID, server_id);
	} else if (type == DCLI_RELEASE) {
		off = dcli_put_opt_u32(buf, off, cap, DCLI_OPT_SERVER_ID, server_id);
	}
	if (off + 1 > cap)
		return 0;
	buf[off++] = DCLI_OPT_END;
	while (off < DCLI_MIN_LEN)
		buf[off++] = 0;
	return off;
}

/* Returns the DHCP message type, 0 when the frame is not ours (other xid, other
 * chaddr, not a reply) and -1 when it is ours but malformed.  A lease is filled
 * from the options the reply carries. */
static int dcli_parse(const uint8_t *buf, size_t len, uint32_t xid,
		      const uint8_t mac[6], struct dcli_lease *lease)
{
	size_t off;
	int type = 0;

	if (len < DCLI_HEADER_LEN + 4 || buf[0] != DCLI_BOOTREPLY ||
	    buf[2] != DCLI_HLEN_ETHER)
		return 0;
	if (dcli_be32(buf + 4) != xid)
		return 0;
	if (memcmp(buf + 28, mac, DCLI_HLEN_ETHER))
		return 0;
	if (memcmp(buf + DCLI_HEADER_LEN, "\x63\x82\x53\x63", 4))
		return 0;
	if (lease) {
		memset(lease, 0, sizeof *lease);
		lease->ip = dcli_be32(buf + 16);	/* yiaddr */
	}
	off = DCLI_HEADER_LEN + 4;
	while (off < len) {
		uint8_t code = buf[off++];
		uint8_t olen;
		const uint8_t *data;

		if (code == DCLI_OPT_END)
			break;
		if (code == 0)			/* pad */
			continue;
		if (off >= len)
			return -1;
		olen = buf[off++];
		if (off + olen > len)
			return -1;
		data = buf + off;
		off += olen;
		switch (code) {
		case DCLI_OPT_MSG_TYPE:
			if (olen != 1)
				return -1;
			type = data[0];
			break;
		case DCLI_OPT_MASK:
			if (olen == 4 && lease)
				lease->mask = dcli_be32(data);
			break;
		case DCLI_OPT_ROUTER:
			if (olen >= 4 && lease) {
				lease->router = dcli_be32(data);
				lease->have_router = 1;
			}
			break;
		case DCLI_OPT_DNS:
			if (olen >= 4 && lease) {
				lease->dns = dcli_be32(data);
				lease->have_dns = 1;
			}
			break;
		case DCLI_OPT_LEASE_TIME:
			if (olen == 4 && lease)
				lease->lease_seconds = dcli_be32(data);
			break;
		case DCLI_OPT_SERVER_ID:
			if (olen == 4 && lease)
				lease->server = dcli_be32(data);
			break;
		case DCLI_OPT_T1:
			if (olen == 4 && lease)
				lease->t1 = dcli_be32(data);
			break;
		case DCLI_OPT_T2:
			if (olen == 4 && lease)
				lease->t2 = dcli_be32(data);
			break;
		default:
			break;
		}
	}
	if (!type)
		return -1;
	return type;
}

/* -------------------------------------------------------- state machine -- */

static int64_t dcli_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Wait for one message of the wanted type.  Fills *lease; returns the type seen
 * (0 on timeout, -1 on a transport error). */
static int dcli_wait(struct dcli_state *st, int want, int timeout_ms,
		     struct dcli_lease *lease)
{
	uint8_t rx[DCLI_MAX_LEN];
	int64_t deadline = dcli_now_ms() + timeout_ms;

	for (;;) {
		int left = (int)(deadline - dcli_now_ms());
		int got;
		int type;

		if (left <= 0)
			return 0;
		got = st->io->recv(st->io_ctx, rx, sizeof rx, left);
		if (got == 0)
			return 0;
		if (got < 0)
			return -1;
		type = dcli_parse(rx, (size_t)got, st->xid, st->mac, lease);
		if (type <= 0)
			continue;		/* not ours, or malformed: ignore */
		if (type == want || type == DCLI_NAK)
			return type;
	}
}

/* DISCOVER -> OFFER -> REQUEST -> ACK.  Returns 0 with a lease, 1 when no server
 * answered within the time budget, -1 on a transport error. */
static int dcli_acquire(struct dcli_state *st, const char *hostname,
			int timeout_ms, struct dcli_lease *out)
{
	uint8_t tx[DCLI_MAX_LEN];
	int attempt;

	for (attempt = 0; attempt < 3; attempt++) {
		struct dcli_lease offer;
		struct dcli_lease ack;
		size_t n;
		int type;

		if (attempt)
			st->xid = dcli_rand32();
		n = dcli_build(tx, sizeof tx, DCLI_DISCOVER, st->xid, st->mac,
			       0, 0, hostname, 0);
		if (!n)
			return -1;
		if (st->io->send(st->io_ctx, tx, n, INADDR_BROADCAST) < 0) {
			dcli_log(st, "DISCOVER send failed: %s", strerror(errno));
			continue;
		}
		dcli_log(st, "DISCOVER sent (xid 0x%08x)", st->xid);

		memset(&offer, 0, sizeof offer);
		type = dcli_wait(st, DCLI_OFFER, timeout_ms, &offer);
		if (type < 0)
			return -1;
		if (type != DCLI_OFFER || !offer.ip) {
			dcli_log(st, "no OFFER within %d ms (attempt %d)",
				 timeout_ms, attempt + 1);
			continue;
		}
		{
			char ip[32], srv[32];

			neti_ipv4_str(offer.ip, ip, sizeof ip);
			neti_ipv4_str(offer.server, srv, sizeof srv);
			dcli_log(st, "OFFER %s from server %s", ip, srv);
		}
		if (!offer.server) {
			/* Without a server id the request cannot be addressed; the
			 * server's own address is the value we have no other use for. */
			dcli_log(st, "OFFER carries no server id; ignoring it");
			continue;
		}

		n = dcli_build(tx, sizeof tx, DCLI_REQUEST, st->xid, st->mac,
			       offer.ip, offer.server, hostname, 0);
		if (!n || st->io->send(st->io_ctx, tx, n, INADDR_BROADCAST) < 0) {
			dcli_log(st, "REQUEST send failed: %s", strerror(errno));
			continue;
		}
		dcli_log(st, "REQUEST sent for the offered address");

		memset(&ack, 0, sizeof ack);
		type = dcli_wait(st, DCLI_ACK, timeout_ms, &ack);
		if (type < 0)
			return -1;
		if (type == DCLI_NAK) {
			dcli_log(st, "server NAKed the request; restarting");
			continue;
		}
		if (type != DCLI_ACK || !ack.ip) {
			dcli_log(st, "no ACK within %d ms (attempt %d)",
				 timeout_ms, attempt + 1);
			continue;
		}
		if (!ack.mask)		/* a lease without a netmask is unusable */
			ack.mask = 0xffffff00u;
		if (!ack.server)
			ack.server = offer.server;
		if (!ack.lease_seconds)
			ack.lease_seconds = DCLI_LEASE_DEFAULT;
		*out = ack;
		return 0;
	}
	return 1;
}

/* ----------------------------------------------------- packet transport -- */

struct dcli_pkt {
	int fd;
	int ifindex;
	uint8_t mac[6];
};

#define DCLI_ETH_HDR 14
#define DCLI_IP_HDR 20
#define DCLI_UDP_HDR 8

static uint16_t dcli_csum(const void *data, size_t len)
{
	const uint16_t *p = data;
	uint32_t sum = 0;

	while (len > 1) {
		sum += *p++;
		len -= 2;
	}
	if (len)
		sum += *(const uint8_t *)p;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

static int pkt_send(void *ctx, const void *payload, size_t len, uint32_t dst_ip)
{
	struct dcli_pkt *p = ctx;
	uint8_t frame[DCLI_ETH_HDR + DCLI_IP_HDR + DCLI_UDP_HDR + DCLI_MAX_LEN];
	struct sockaddr_ll to;
	size_t l4 = DCLI_UDP_HDR + len;
	size_t total;

	if (len > DCLI_MAX_LEN)
		return -1;
	memset(frame, 0, DCLI_ETH_HDR);
	memset(frame, 0xff, 6);			/* broadcast MAC: we have no ARP yet */
	memcpy(frame + 6, p->mac, 6);
	frame[12] = 0x08;			/* ethertype IPv4 */
	frame[13] = 0x00;

	frame[DCLI_ETH_HDR + 0] = 0x45;		/* version 4, 20 byte header */
	frame[DCLI_ETH_HDR + 9] = 17;		/* UDP */
	frame[DCLI_ETH_HDR + 8] = 64;		/* ttl */
	total = DCLI_IP_HDR + l4;
	frame[DCLI_ETH_HDR + 2] = (uint8_t)(total >> 8);
	frame[DCLI_ETH_HDR + 3] = (uint8_t)total;
	{
		uint32_t d = htonl(dst_ip);
		uint16_t csum;

		memcpy(frame + DCLI_ETH_HDR + 16, &d, 4);	/* daddr, saddr stays 0 */
		csum = dcli_csum(frame + DCLI_ETH_HDR, DCLI_IP_HDR);
		memcpy(frame + DCLI_ETH_HDR + 10, &csum, 2);
	}
	{
		uint16_t sport = htons(DCLI_CLIENT_PORT);
		uint16_t dport = htons(DCLI_SERVER_PORT);
		uint16_t ulen = htons((uint16_t)l4);

		memcpy(frame + DCLI_ETH_HDR + DCLI_IP_HDR + 0, &sport, 2);
		memcpy(frame + DCLI_ETH_HDR + DCLI_IP_HDR + 2, &dport, 2);
		memcpy(frame + DCLI_ETH_HDR + DCLI_IP_HDR + 4, &ulen, 2);
		/* UDP checksum 0 is legal for IPv4 and is what most DHCP clients send */
	}
	memcpy(frame + DCLI_ETH_HDR + DCLI_IP_HDR + DCLI_UDP_HDR, payload, len);

	memset(&to, 0, sizeof to);
	to.sll_family = AF_PACKET;
	to.sll_ifindex = p->ifindex;
	to.sll_halen = 6;
	memset(to.sll_addr, 0xff, 6);
	if (sendto(p->fd, frame, DCLI_ETH_HDR + total, 0,
		   (struct sockaddr *)&to, sizeof to) < 0)
		return -1;
	return 0;
}

static int pkt_recv(void *ctx, void *buf, size_t cap, int timeout_ms)
{
	struct dcli_pkt *p = ctx;
	uint8_t frame[DCLI_ETH_HDR + DCLI_IP_HDR + DCLI_UDP_HDR + DCLI_MAX_LEN];
	struct pollfd pfd = { .fd = p->fd, .events = POLLIN };
	size_t hdr;
	int ihl;
	uint16_t sport, dport;
	ssize_t n;

	for (;;) {
		int rc = poll(&pfd, 1, timeout_ms);

		if (rc == 0)
			return 0;
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		n = recv(p->fd, frame, sizeof frame, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n < DCLI_ETH_HDR + DCLI_IP_HDR + DCLI_UDP_HDR)
			continue;
		if (frame[12] != 0x08 || frame[13] != 0x00)
			continue;
		if ((frame[DCLI_ETH_HDR] >> 4) != 4 || frame[DCLI_ETH_HDR + 9] != 17)
			continue;
		ihl = (frame[DCLI_ETH_HDR] & 0x0f) * 4;
		hdr = DCLI_ETH_HDR + (size_t)ihl;
		if (ihl < DCLI_IP_HDR || (size_t)n < hdr + DCLI_UDP_HDR)
			continue;
		memcpy(&sport, frame + hdr, 2);
		memcpy(&dport, frame + hdr + 2, 2);
		if (ntohs(sport) != DCLI_SERVER_PORT ||
		    ntohs(dport) != DCLI_CLIENT_PORT)
			continue;
		{
			size_t payload = (size_t)n - hdr - DCLI_UDP_HDR;
			uint16_t ulen;

			memcpy(&ulen, frame + hdr + 4, 2);
			if (ntohs(ulen) >= DCLI_UDP_HDR)
				payload = (size_t)ntohs(ulen) - DCLI_UDP_HDR;
			if (payload > (size_t)n - hdr - DCLI_UDP_HDR)
				payload = (size_t)n - hdr - DCLI_UDP_HDR;
			if (payload > cap)
				payload = cap;
			memcpy(buf, frame + hdr + DCLI_UDP_HDR, payload);
			return (int)payload;
		}
	}
}

static int pkt_open(struct dcli_pkt *p, const char *ifname)
{
	int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_IP));

	if (fd < 0)
		return -1;
	p->fd = fd;
	p->ifindex = neti_ifindex(ifname);
	if (p->ifindex <= 0 || neti_mac(ifname, p->mac) < 0) {
		close(fd);
		p->fd = -1;
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ main -- */

static void log_stdout(void *ctx, const char *line)
{
	(void)ctx;
	printf("dhcp: %s\n", line);
	fflush(stdout);
}

static int prefix_len(uint32_t mask)
{
	int bits = 0;

	while (mask) {
		bits += (int)(mask & 1u);
		mask >>= 1;
	}
	return bits;
}

/* key=value lines, so the daemon (or a human over ssh) can read the lease back
 * without a parser.  Written atomically: a half written lease is worse than a
 * stale one. */
static int lease_write(const char *path, const char *ifname,
		       const struct dcli_lease *l)
{
	char tmp[256];
	char ip[32], mask[32], gw[32], dns[32], srv[32];
	FILE *f;
	int rc = -1;

	neti_ipv4_str(l->ip, ip, sizeof ip);
	neti_ipv4_str(l->mask, mask, sizeof mask);
	neti_ipv4_str(l->router, gw, sizeof gw);
	neti_ipv4_str(l->dns, dns, sizeof dns);
	neti_ipv4_str(l->server, srv, sizeof srv);
	snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
	f = fopen(tmp, "w");
	if (!f)
		return -1;
	fprintf(f, "# written by dhcp_client\n");
	fprintf(f, "iface=%s\n", ifname);
	fprintf(f, "ip=%s\n", ip);
	fprintf(f, "mask=%s\n", mask);
	fprintf(f, "prefix=%d\n", prefix_len(l->mask));
	fprintf(f, "router=%s\n", l->have_router ? gw : "");
	fprintf(f, "dns=%s\n", l->have_dns ? dns : "");
	fprintf(f, "server=%s\n", srv);
	fprintf(f, "lease=%u\n", l->lease_seconds);
	fprintf(f, "t1=%u\nt2=%u\n", l->t1, l->t2);
	fprintf(f, "acquired_unix=%lld\n", (long long)time(NULL));
	if (fclose(f) == 0) {
		chmod(tmp, 0600);
		rc = rename(tmp, path);
	}
	if (rc != 0)
		unlink(tmp);
	return rc;
}

static void usage(void)
{
	fprintf(stderr,
		"dhcp_client - the DHCP client this unit does not have\n"
		"\n"
		"  dhcp_client [-i IFACE] [-t SECONDS] [--lease-file PATH]\n"
		"              [--hostname NAME] [--no-config] [--release] [--quiet]\n"
		"\n"
		"  -i IFACE        interface, default wlan0\n"
		"  -t SECONDS      per-attempt wait for OFFER/ACK, default 4 (x3 tries)\n"
		"  --lease-file    where to record the lease, default %s\n"
		"  --no-config     ask for a lease but do not touch the interface\n"
		"  --release       release the current lease (needs the lease file)\n"
		"Exit: 0 lease (applied), 1 usage, 2 no answer, 3 apply failed.\n",
		"/rw_data/wifi/dhcp.lease");
}

#ifndef DHCP_CLIENT_NO_MAIN   /* tests/dhcp_client_tu.c includes this file */
int main(int argc, char **argv)
{
	const char *ifname = "wlan0";
	const char *lease_file = "/rw_data/wifi/dhcp.lease";
	const char *hostname = "scourge";
	int timeout_s = 4;
	int do_config = 1;
	int release = 0;
	int quiet = 0;
	struct dcli_pkt pkt = { .fd = -1 };
	struct dcli_state st;
	struct dcli_io io;
	struct dcli_lease lease;
	char ip[32], mask[32], gw[32], dns[32], srv[32];
	int i, rc;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "-i") && i + 1 < argc) {
			ifname = argv[++i];
		} else if (!strcmp(a, "-t") && i + 1 < argc) {
			timeout_s = atoi(argv[++i]);
		} else if (!strcmp(a, "--lease-file") && i + 1 < argc) {
			lease_file = argv[++i];
		} else if (!strcmp(a, "--hostname") && i + 1 < argc) {
			hostname = argv[++i];
		} else if (!strcmp(a, "--no-config")) {
			do_config = 0;
		} else if (!strcmp(a, "--release")) {
			release = 1;
		} else if (!strcmp(a, "--quiet")) {
			quiet = 1;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage();
			return 0;
		} else {
			fprintf(stderr, "dhcp_client: unknown argument '%s'\n", a);
			usage();
			return 1;
		}
	}
	if (timeout_s <= 0 || timeout_s > 60)
		timeout_s = 4;

	if (pkt_open(&pkt, ifname) < 0) {
		fprintf(stderr, "dhcp_client: %s: %s\n", ifname, strerror(errno));
		return 3;
	}

	memset(&io, 0, sizeof io);
	io.send = pkt_send;
	io.recv = pkt_recv;
	memset(&st, 0, sizeof st);
	st.io = &io;
	st.io_ctx = &pkt;
	st.log = quiet ? NULL : log_stdout;
	st.mac[0] = pkt.mac[0];
	memcpy(st.mac, pkt.mac, sizeof st.mac);
	st.xid = dcli_rand32();

	if (release) {
		uint8_t tx[DCLI_MAX_LEN];
		uint32_t have = 0;
		uint32_t server = 0;
		size_t n;

		neti_ipv4(ifname, &have);
		if (!have) {
			fprintf(stderr, "dhcp_client: %s has no address to release\n",
				ifname);
			close(pkt.fd);
			return 2;
		}
		n = dcli_build(tx, sizeof tx, DCLI_RELEASE, st.xid, st.mac, 0, server,
			       hostname, have);
		if (n && pkt_send(&pkt, tx, n, INADDR_BROADCAST) == 0)
			printf("dhcp: RELEASE sent for %s\n", ifname);
		close(pkt.fd);
		return 0;
	}

	memset(&lease, 0, sizeof lease);
	rc = dcli_acquire(&st, hostname, timeout_s * 1000, &lease);
	close(pkt.fd);
	if (rc < 0) {
		fprintf(stderr, "dhcp_client: transport error on %s\n", ifname);
		return 2;
	}
	if (rc > 0) {
		fprintf(stderr, "dhcp_client: no lease on %s\n", ifname);
		return 2;
	}

	neti_ipv4_str(lease.ip, ip, sizeof ip);
	neti_ipv4_str(lease.mask, mask, sizeof mask);
	neti_ipv4_str(lease.router, gw, sizeof gw);
	neti_ipv4_str(lease.dns, dns, sizeof dns);
	neti_ipv4_str(lease.server, srv, sizeof srv);

	if (do_config) {
		if (neti_set_ipv4(ifname, lease.ip, lease.mask) < 0) {
			fprintf(stderr, "dhcp_client: cannot set %s/%d on %s: %s\n",
				ip, prefix_len(lease.mask), ifname, strerror(errno));
			return 3;
		}
		neti_up(ifname);
		if (lease.have_router &&
		    neti_default_route(ifname, lease.router) < 0)
			fprintf(stderr, "dhcp_client: default route via %s failed: %s\n",
				gw, strerror(errno));
	}
	if (lease_write(lease_file, ifname, &lease) < 0)
		fprintf(stderr, "dhcp_client: cannot write %s: %s\n", lease_file,
			strerror(errno));

	printf("dhcp: lease %s/%d gw=%s dns=%s from %s ttl=%us iface=%s%s\n",
	       ip, prefix_len(lease.mask), lease.have_router ? gw : "none",
	       lease.have_dns ? dns : "none", srv, lease.lease_seconds, ifname,
	       do_config ? "" : " (not applied)");
	return 0;
}
#endif /* DHCP_CLIENT_NO_MAIN */
