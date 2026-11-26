/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */
/* mdns_adv - a 300 line mDNS responder for the one name this unit needs.
 *
 * Why: the head unit has no avahi, and carplay_mdnsd is not a general resolver -
 * it advertises CarPlay (_airplay/_raop) and nothing else.  So "scourge.local"
 * could not be reached by name, and in client mode the address comes from DHCP
 * and can change.  The static address stays the primary way in; this is the
 * convenience layer, and it is honest about its limits: it needs multicast to
 * reach the client (AP mode with client isolation, or a router that drops
 * multicast, and the name will not resolve).
 *
 * What it advertises:
 *   <host>.local            A     the interface's IPv4 address
 *   _ssh._tcp.local         PTR   <host>._ssh._tcp.local   (+ SRV, TXT)
 *   _http._tcp.local        PTR   <host>._http._tcp.local  (only with --http-port)
 * It answers queries for those names and announces itself twice on start-up.
 *
 * Deliberate limits: no cache, no goodbye packets on exit, no probing for a
 * conflicting name (the host name carries two MAC bytes, so a collision on a home
 * network is not a real scenario), no IPv6.
 *
 * Usage:
 *   mdns_adv [-i IFACE] [--hostname NAME] [--ip ADDRESS] [--ssh-port PORT]
 *            [--http-port PORT] [--announce-interval SECONDS] [-v]
 *   mdns_adv --query-test '<name>/<type>'   (no socket: prints the answer size)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
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

#include "../../common/net_iface.inc"

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"
#define MDNS_TTL_HOST 120
#define MDNS_TTL_SERVICE 4500
#define MDNS_PKT_MAX 1500

enum {
	MDNS_TYPE_A = 1,
	MDNS_TYPE_PTR = 12,
	MDNS_TYPE_TXT = 16,
	MDNS_TYPE_ANY = 255,
	MDNS_TYPE_SRV = 33,
};

#define MDNS_CLASS_IN 1
#define MDNS_CLASS_FLUSH 0x8000
#define MDNS_CLASS_QU 0x8000		/* unicast-response bit in a question */

struct mdns_ctx {
	char host[64];			/* "scourge-ab" (labels, no .local) */
	char host_fqdn[80];		/* "scourge-ab.local" */
	char ssh_name[96];		/* "scourge-ab._ssh._tcp.local" */
	char http_name[96];		/* "scourge-ab._http._tcp.local" */
	uint32_t ip;			/* host order */
	uint16_t ssh_port;
	uint16_t http_port;		/* 0 = do not advertise */
	char txt_ssh[64];
};

struct mdns_question {
	char name[128];
	uint16_t type;
	uint16_t qclass;
};

/* ------------------------------------------------------------------ names -- */

static size_t mdns_encode_name(uint8_t *out, size_t cap, const char *name)
{
	size_t off = 0;
	const char *label = name;

	if (!name || !name[0])
		return 0;
	for (;;) {
		const char *dot = strchr(label, '.');
		size_t n = dot ? (size_t)(dot - label) : strlen(label);

		if (n == 0)				/* "a..b" or a trailing dot */
			return 0;
		if (n > 63 || off + 1 + n + 1 > cap)
			return 0;
		out[off++] = (uint8_t)n;
		memcpy(out + off, label, n);
		off += n;
		if (!dot)
			break;
		label = dot + 1;
		if (!*label)
			return 0;
	}
	out[off++] = 0;				/* root label */
	return off;
}

/* Decode a name, following compression pointers, into dotted form.  *end is set
 * past the bytes the name occupied in the message (not past a pointer target). */
static int mdns_decode_name(const uint8_t *msg, size_t len, size_t off,
			    char *out, size_t cap, size_t *end)
{
	size_t used = 0;
	size_t pos = off;
	int hops = 0;
	int jumped = 0;

	if (cap)
		out[0] = '\0';
	for (;;) {
		uint8_t label;

		if (pos >= len)
			return -1;
		label = msg[pos];
		if ((label & 0xc0) == 0xc0) {
			size_t target;

			if (pos + 1 >= len || ++hops > 8)
				return -1;
			target = (size_t)(label & 0x3f) << 8 | msg[pos + 1];
			if (!jumped && end)
				*end = pos + 2;
			jumped = 1;
			if (target >= len)
				return -1;
			pos = target;
			continue;
		}
		if (label & 0xc0)			/* reserved form */
			return -1;
		if (label == 0) {
			if (!jumped && end)
				*end = pos + 1;
			if (used)
				out[used - 1] = '\0';	/* drop the trailing dot */
			return 0;
		}
		pos++;
		if (pos + label > len)
			return -1;
		if (used + label + 2 > cap)
			return -1;
		memcpy(out + used, msg + pos, label);
		used += label;
		out[used++] = '.';
		pos += label;
	}
	return 0;
}

static int mdns_parse_question(const uint8_t *msg, size_t len, size_t off,
			       struct mdns_question *q, size_t *next)
{
	size_t end = off;
	uint16_t v;

	if (mdns_decode_name(msg, len, off, q->name, sizeof q->name, &end) < 0)
		return -1;
	if (end + 4 > len)
		return -1;
	memcpy(&v, msg + end, 2);
	q->type = ntohs(v);
	memcpy(&v, msg + end + 2, 2);
	q->qclass = ntohs(v);
	if (next)
		*next = end + 4;
	return 0;
}

/* ---------------------------------------------------------------- answers -- */

static int mdns_name_eq(const char *a, const char *b)
{
	while (*a && *b) {
		char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;

		if (ca != cb)
			return 0;
		a++;
		b++;
	}
	return *a == *b;
}

static size_t mdns_put_record(uint8_t *out, size_t off, size_t cap,
			      const char *name, uint16_t type, uint16_t rclass,
			      uint32_t ttl, const uint8_t *rdata, size_t rdlen)
{
	uint8_t tmp[512];
	size_t nlen = mdns_encode_name(tmp, sizeof tmp, name);
	uint32_t be32;
	uint16_t be16;

	if (!nlen || off + nlen + 10 + rdlen > cap)
		return 0;
	memcpy(out + off, tmp, nlen);
	off += nlen;
	be16 = htons(type);
	memcpy(out + off, &be16, 2);
	off += 2;
	be16 = htons(rclass);
	memcpy(out + off, &be16, 2);
	off += 2;
	be32 = htonl(ttl);
	memcpy(out + off, &be32, 4);
	off += 4;
	be16 = htons((uint16_t)rdlen);
	memcpy(out + off, &be16, 2);
	off += 2;
	if (rdlen)
		memcpy(out + off, rdata, rdlen);
	return off + rdlen;
}

static size_t mdns_put_ptr(uint8_t *out, size_t off, size_t cap,
			   const char *name, const char *target, uint32_t ttl)
{
	uint8_t rdata[256];
	size_t nlen = mdns_encode_name(rdata, sizeof rdata, target);

	if (!nlen)
		return 0;
	return mdns_put_record(out, off, cap, name, MDNS_TYPE_PTR, MDNS_CLASS_IN,
			       ttl, rdata, nlen);
}

static size_t mdns_put_srv(uint8_t *out, size_t off, size_t cap,
			   const char *name, const char *target, uint16_t port,
			   uint32_t ttl)
{
	uint8_t rdata[260];
	size_t nlen;
	uint16_t be16;

	memset(rdata, 0, 6);			/* priority, weight */
	be16 = htons(port);
	memcpy(rdata + 4, &be16, 2);
	nlen = mdns_encode_name(rdata + 6, sizeof rdata - 6, target);
	if (!nlen)
		return 0;
	return mdns_put_record(out, off, cap, name, MDNS_TYPE_SRV,
			       MDNS_CLASS_IN | MDNS_CLASS_FLUSH, ttl, rdata,
			       6 + nlen);
}

static size_t mdns_put_txt(uint8_t *out, size_t off, size_t cap,
			   const char *name, const char *text, uint32_t ttl)
{
	uint8_t rdata[256];
	size_t n = strlen(text);
	size_t rdlen;

	if (n > 200)
		n = 200;
	rdata[0] = (uint8_t)n;
	memcpy(rdata + 1, text, n);
	rdlen = n + 1;
	return mdns_put_record(out, off, cap, name, MDNS_TYPE_TXT,
			       MDNS_CLASS_IN | MDNS_CLASS_FLUSH, ttl, rdata, rdlen);
}

static size_t mdns_put_a(uint8_t *out, size_t off, size_t cap, const char *name,
			 uint32_t ip, uint32_t ttl)
{
	uint8_t rdata[4];
	uint32_t be = htonl(ip);

	memcpy(rdata, &be, 4);
	return mdns_put_record(out, off, cap, name, MDNS_TYPE_A,
			       MDNS_CLASS_IN | MDNS_CLASS_FLUSH, ttl, rdata, 4);
}

/* Everything this responder knows, appended for one service instance. */
static size_t mdns_put_service(uint8_t *out, size_t off, size_t cap,
			       const struct mdns_ctx *c, const char *service,
			       const char *instance, uint16_t port,
			       const char *txt)
{
	off = mdns_put_ptr(out, off, cap, service, instance, MDNS_TTL_SERVICE);
	if (!off)
		return 0;
	off = mdns_put_srv(out, off, cap, instance, c->host_fqdn, port,
			   MDNS_TTL_HOST);
	if (!off)
		return 0;
	off = mdns_put_txt(out, off, cap, instance, txt, MDNS_TTL_HOST);
	if (!off)
		return 0;
	return off;
}

static size_t mdns_put_host(uint8_t *out, size_t off, size_t cap,
			    const struct mdns_ctx *c)
{
	return mdns_put_a(out, off, cap, c->host_fqdn, c->ip, MDNS_TTL_HOST);
}

/* Answer the questions in *query.  Returns the length of a response, or 0 when
 * the packet is not a question for us (nothing is sent in that case).
 * *unicast is set when the asker asked for a unicast answer (QU bit). */
static size_t mdns_build_response(const struct mdns_ctx *c, const uint8_t *query,
				  size_t qlen, uint8_t *out, size_t cap,
				  int *unicast, uint16_t *txid)
{
	uint16_t qdcount, flags;
	size_t off = 12;
	size_t answers = 0;
	size_t body;
	int qu = 0;

	if (unicast)
		*unicast = 0;
	if (qlen < 12)
		return 0;
	memcpy(&flags, query + 2, 2);
	flags = ntohs(flags);
	if (flags & 0x8000)			/* a response, not a query */
		return 0;
	memcpy(&qdcount, query + 4, 2);
	qdcount = ntohs(qdcount);
	if (!qdcount)
		return 0;
	if (txid)			/* host order, so the caller can write it back */
		*txid = (uint16_t)((query[0] << 8) | query[1]);

	/* Header first; ancount is patched once the records are in. */
	memset(out, 0, 12);
	memcpy(out, query, 2);			/* same id (0 for multicast asks) */
	out[2] = 0x84;				/* QR = 1, AA = 1 */
	out[3] = 0x00;
	body = 12;

	for (uint16_t i = 0; i < qdcount; i++) {
		struct mdns_question q;
		size_t next;

		if (mdns_parse_question(query, qlen, off, &q, &next) < 0)
			return 0;
		off = next;
		if (q.qclass & MDNS_CLASS_QU)
			qu = 1;
		if (q.type == MDNS_TYPE_A || q.type == MDNS_TYPE_ANY) {
			if (mdns_name_eq(q.name, c->host_fqdn)) {
				body = mdns_put_host(out, body, cap, c);
				if (!body)
					return 0;
				answers++;
			}
		}
		if (q.type == MDNS_TYPE_PTR || q.type == MDNS_TYPE_ANY) {
			if (mdns_name_eq(q.name, "_ssh._tcp.local")) {
				body = mdns_put_service(out, body, cap, c,
							"_ssh._tcp.local", c->ssh_name,
							c->ssh_port, c->txt_ssh);
				if (!body)
					return 0;
				body = mdns_put_host(out, body, cap, c);
				if (!body)
					return 0;
				answers += 4;		/* PTR + SRV + TXT + A */
			}
			if (c->http_port &&
			    mdns_name_eq(q.name, "_http._tcp.local")) {
				body = mdns_put_service(out, body, cap, c,
							"_http._tcp.local", c->http_name,
							c->http_port, "path=/");
				if (!body)
					return 0;
				body = mdns_put_host(out, body, cap, c);
				if (!body)
					return 0;
				answers += 4;		/* PTR + SRV + TXT + A */
			}
		}
		if (q.type == MDNS_TYPE_SRV || q.type == MDNS_TYPE_ANY) {
			if (mdns_name_eq(q.name, c->ssh_name)) {
				size_t n = mdns_put_srv(out, body, cap, c->ssh_name,
							c->host_fqdn, c->ssh_port,
							MDNS_TTL_HOST);

				if (!n)
					return 0;
				body = mdns_put_host(out, n, cap, c);
				if (!body)
					return 0;
				answers += 2;
			}
		}
	}
	if (!answers)
		return 0;
	out[6] = (uint8_t)(answers >> 8);
	out[7] = (uint8_t)answers;
	if (unicast && qu)
		*unicast = 1;
	return body;
}

/* Unsolicited announcement: every record, no questions. */
static size_t mdns_build_announce(const struct mdns_ctx *c, uint8_t *out,
				  size_t cap)
{
	size_t body = 12;
	size_t answers = 0;

	memset(out, 0, 12);
	out[2] = 0x84;				/* QR = 1, AA = 1 */
	body = mdns_put_service(out, body, cap, c, "_ssh._tcp.local", c->ssh_name,
				c->ssh_port, c->txt_ssh);
	if (!body)
		return 0;
	answers += 3;
	if (c->http_port) {
		body = mdns_put_service(out, body, cap, c, "_http._tcp.local",
					c->http_name, c->http_port, "path=/");
		if (!body)
			return 0;
		answers += 3;
	}
	body = mdns_put_host(out, body, cap, c);
	if (!body)
		return 0;
	answers++;
	out[6] = (uint8_t)(answers >> 8);
	out[7] = (uint8_t)answers;
	return body;
}

/* ------------------------------------------------------------ transport -- */

struct mdns_sock {
	int fd;
	int ifindex;
	int port;
};

/* The port is a knob for one reason: a test must not fight avahi for 5353 on a
 * developer machine (packages/common/ap_dhcpd.inc does the same). */
static int mdns_port(void)
{
	const char *e = getenv("SCOURGE_MDNS_TEST_PORT");
	int p = e ? atoi(e) : MDNS_PORT;

	return p > 0 && p < 65536 ? p : MDNS_PORT;
}

static int64_t mdns_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int mdns_open(struct mdns_sock *s, const char *ifname)
{
	struct sockaddr_in addr;
	struct ip_mreqn mreq;
	int one = 1;
	int ttl = 255;
	int loop = 0;
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	/* SO_REUSEADDR only, on purpose: this socket has to coexist with
	 * carplay_mdnsd, which also answers mDNS on 5353.  Both then receive the
	 * multicast questions (we answer only our own names), but we stay out of
	 * SO_REUSEPORT's unicast load balancing - a unicast CarPlay question must
	 * never be hashed onto this socket and dropped here. */
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	s->port = mdns_port();
	addr.sin_port = htons((uint16_t)s->port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return -1;
	}
	s->ifindex = neti_ifindex(ifname);
	if (s->ifindex <= 0) {
		close(fd);
		return -1;
	}
	memset(&mreq, 0, sizeof mreq);
	inet_pton(AF_INET, MDNS_GROUP, &mreq.imr_multiaddr);
	mreq.imr_ifindex = s->ifindex;
	if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) < 0) {
		close(fd);
		return -1;
	}
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &mreq, sizeof mreq);
	s->fd = fd;
	return 0;
}

static int mdns_send(struct mdns_sock *s, const void *buf, size_t len,
		     const struct sockaddr_in *to)
{
	if (sendto(s->fd, buf, len, 0, (const struct sockaddr *)to, sizeof *to) < 0)
		return -1;
	return 0;
}

static int mdns_send_multicast(struct mdns_sock *s, const void *buf, size_t len)
{
	struct sockaddr_in to;

	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port = htons((uint16_t)s->port);
	inet_pton(AF_INET, MDNS_GROUP, &to.sin_addr);
	return mdns_send(s, buf, len, &to);
}

/* ------------------------------------------------------------------ main -- */

static void fill_ctx(struct mdns_ctx *c, const char *host, uint32_t ip,
		     uint16_t ssh_port, uint16_t http_port)
{
	snprintf(c->host, sizeof c->host, "%s", host);
	snprintf(c->host_fqdn, sizeof c->host_fqdn, "%s.local", host);
	snprintf(c->ssh_name, sizeof c->ssh_name, "%s._ssh._tcp.local", host);
	snprintf(c->http_name, sizeof c->http_name, "%s._http._tcp.local", host);
	c->ip = ip;
	c->ssh_port = ssh_port;
	c->http_port = http_port;
	snprintf(c->txt_ssh, sizeof c->txt_ssh, "u=car");
}

static void usage(void)
{
	fprintf(stderr,
		"mdns_adv - advertise <host>.local, _ssh._tcp and _http._tcp over mDNS\n"
		"\n"
		"  mdns_adv [-i IFACE] [--hostname NAME] [--ip ADDRESS]\n"
		"           [--ssh-port PORT] [--http-port PORT] [--interval SECONDS] [-v]\n"
		"\n"
		"  -i IFACE       interface to advertise on, default wlan0\n"
		"  --hostname     label before .local; default scourge-<last two MAC bytes>\n"
		"  --ip           address to advertise; default: the interface's own\n"
		"  --ssh-port     default 2222, 0 disables the SSH service\n"
		"  --http-port    daemon port (e.g. 8090); 0 disables that service\n"
		"  --interval     re-announce period in seconds, default 60\n"
		"Exit: 0 on SIGTERM/SIGINT, 1 usage, 3 socket failure.\n");
}

#ifndef MDNS_ADV_NO_MAIN   /* tests/mdns_adv_tu.c includes this file */
int main(int argc, char **argv)
{
	const char *ifname = "wlan0";
	const char *hostname = NULL;
	uint32_t fixed_ip = 0;
	int ssh_port = 2222;
	int http_port = 0;
	int interval = 60;
	int verbose = 0;
	char host[64] = "";
	struct mdns_ctx ctx;
	struct mdns_sock sock;
	int64_t next_announce;
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "-i") && i + 1 < argc) {
			ifname = argv[++i];
		} else if (!strcmp(a, "--hostname") && i + 1 < argc) {
			hostname = argv[++i];
		} else if (!strcmp(a, "--ip") && i + 1 < argc) {
			int ok = 0;

			fixed_ip = neti_ipv4_parse(argv[++i], &ok);
			if (!ok) {
				fprintf(stderr, "mdns_adv: --ip needs a dotted quad\n");
				return 1;
			}
		} else if (!strcmp(a, "--ssh-port") && i + 1 < argc) {
			ssh_port = atoi(argv[++i]);
		} else if (!strcmp(a, "--http-port") && i + 1 < argc) {
			http_port = atoi(argv[++i]);
		} else if (!strcmp(a, "--interval") && i + 1 < argc) {
			interval = atoi(argv[++i]);
		} else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
			verbose = 1;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage();
			return 0;
		} else {
			fprintf(stderr, "mdns_adv: unknown argument '%s'\n", a);
			usage();
			return 1;
		}
	}
	if (interval < 5)
		interval = 60;

	if (!hostname || !hostname[0]) {
		uint8_t mac[6] = { 0 };

		if (neti_mac(ifname, mac) < 0) {
			fprintf(stderr, "mdns_adv: %s: %s\n", ifname, strerror(errno));
			return 3;
		}
		/* Two MAC bytes: unique enough on a home network, and stable across
		 * reboots of the same unit (a lease change does not rename the box). */
		snprintf(host, sizeof host, "scourge-%02x%02x", mac[4], mac[5]);
		hostname = host;
	}
	fill_ctx(&ctx, hostname, fixed_ip, (uint16_t)ssh_port, (uint16_t)http_port);

	if (mdns_open(&sock, ifname) < 0) {
		fprintf(stderr, "mdns_adv: %s: %s\n", ifname, strerror(errno));
		return 3;
	}

	if (!ctx.ip && neti_ipv4(ifname, &ctx.ip) < 0) {
		fprintf(stderr, "mdns_adv: %s has no IPv4 address yet; "
			"advertising once it has one\n", ifname);
	}
	if (verbose)
		printf("mdns: %s -> %08x (%s), _ssh._tcp port %u%s\n", ctx.host_fqdn,
		       ctx.ip, ifname, ctx.ssh_port,
		       ctx.http_port ? ", _http._tcp" : "");

	next_announce = mdns_now_ms();
	for (;;) {
		uint8_t pkt[MDNS_PKT_MAX];
		struct pollfd pfd = { .fd = sock.fd, .events = POLLIN };
		int64_t now = mdns_now_ms();
		int timeout = (int)(next_announce - now);
		int rc;

		if (timeout < 0)
			timeout = 0;
		rc = poll(&pfd, 1, timeout > 1000 ? 1000 : timeout);
		if (rc > 0 && (pfd.revents & POLLIN)) {
			struct sockaddr_in from;
			socklen_t fromlen = sizeof from;
			ssize_t n = recvfrom(sock.fd, pkt, sizeof pkt, 0,
					     (struct sockaddr *)&from, &fromlen);

			if (n > 0) {
				uint8_t reply[MDNS_PKT_MAX];
				uint16_t txid = 0;
				int unicast = 0;
				size_t rlen;

				if (!ctx.ip)
					neti_ipv4(ifname, &ctx.ip);
				rlen = mdns_build_response(&ctx, pkt, (size_t)n, reply,
							   sizeof reply, &unicast,
							   &txid);
				if (rlen) {
					if (unicast) {
						reply[0] = (uint8_t)(txid >> 8);
						reply[1] = (uint8_t)txid;
						mdns_send(&sock, reply, rlen, &from);
					} else {
						reply[0] = 0;
						reply[1] = 0;
						mdns_send_multicast(&sock, reply, rlen);
					}
					if (verbose)
						printf("mdns: answered %zd byte query "
						       "from %s (%zu byte reply%s)\n",
						       n, inet_ntoa(from.sin_addr), rlen,
						       unicast ? ", unicast" : "");
				}
			}
		}
		if (mdns_now_ms() >= next_announce) {
			uint8_t pkt[MDNS_PKT_MAX];
			size_t len;

			if (!ctx.ip)
				neti_ipv4(ifname, &ctx.ip);
			len = mdns_build_announce(&ctx, pkt, sizeof pkt);
			if (len)
				mdns_send_multicast(&sock, pkt, len);
			if (verbose && len)
				printf("mdns: announced %s (%zu bytes)\n",
				       ctx.host_fqdn, len);
			next_announce = mdns_now_ms() + (int64_t)interval * 1000;
		}
	}
}
#endif /* MDNS_ADV_NO_MAIN */
