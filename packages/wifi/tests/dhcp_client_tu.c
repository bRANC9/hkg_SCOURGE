/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * dhcp_client_tu.c - the DHCP client against a scripted server.
 *
 * The unit has no DHCP client and no way to test one: this file plays the server
 * through dhcp_client's injected I/O, so the whole DISCOVER/OFFER/REQUEST/ACK
 * exchange, the NAK restart, the timeout and the wire encoding are checked
 * without root, without a network and without a lease from anybody.
 *
 * The server side is built here, independently of dcli_build(): a test that
 * reuses the code under test would agree with it about a wrong format.
 */
#define _GNU_SOURCE 1
#define DHCP_CLIENT_NO_MAIN 1
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/dhcp_client.c"

/* ----------------------------------------------------------- fake server -- */

struct srv {
	int mac_replies;	/* 1 = answer, 0 = stay silent */
	int nak_once;		/* NAK the first REQUEST, then behave */
	int naks;
	int nak_reply_type;
	uint8_t mac[6];
	uint32_t xid;
	uint32_t offer_ip;
	uint32_t server_ip;
	uint32_t gw;
	uint32_t dns;
	uint32_t mask;
	uint8_t last_sent[DCLI_MAX_LEN];
	size_t last_len;
	int seen_discover;
	int seen_request;
};

static size_t srv_build(uint8_t *out, size_t cap, int type, uint32_t xid,
			const uint8_t mac[6], uint32_t yiaddr, uint32_t server,
			uint32_t mask, uint32_t gw, uint32_t dns)
{
	size_t off = DCLI_HEADER_LEN;

	memset(out, 0, cap);
	out[0] = DCLI_BOOTREPLY;
	out[1] = DCLI_HTYPE_ETHER;
	out[2] = DCLI_HLEN_ETHER;
	out[4] = (uint8_t)(xid >> 24);
	out[5] = (uint8_t)(xid >> 16);
	out[6] = (uint8_t)(xid >> 8);
	out[7] = (uint8_t)xid;
	out[16] = (uint8_t)(yiaddr >> 24);
	out[17] = (uint8_t)(yiaddr >> 16);
	out[18] = (uint8_t)(yiaddr >> 8);
	out[19] = (uint8_t)yiaddr;
	memcpy(out + 28, mac, 6);
	out[off++] = 0x63;
	out[off++] = 0x82;
	out[off++] = 0x53;
	out[off++] = 0x63;
	out[off++] = DCLI_OPT_MSG_TYPE;
	out[off++] = 1;
	out[off++] = (uint8_t)type;
#define SRV_U32(code, value)					\
	out[off++] = (code);					\
	out[off++] = 4;						\
	out[off++] = (uint8_t)((value) >> 24);			\
	out[off++] = (uint8_t)((value) >> 16);			\
	out[off++] = (uint8_t)((value) >> 8);			\
	out[off++] = (uint8_t)(value)
	SRV_U32(DCLI_OPT_SERVER_ID, server);
	SRV_U32(DCLI_OPT_MASK, mask);
	SRV_U32(DCLI_OPT_ROUTER, gw);
	SRV_U32(DCLI_OPT_DNS, dns);
	SRV_U32(DCLI_OPT_LEASE_TIME, 43200);
	SRV_U32(DCLI_OPT_T1, 21600);
	SRV_U32(DCLI_OPT_T2, 37800);
#undef SRV_U32
	out[off++] = DCLI_OPT_END;
	while (off < 300)
		out[off++] = 0;
	return off;
}

static int srv_send(void *ctx, const void *buf, size_t len, uint32_t dst_ip)
{
	struct srv *s = ctx;
	int type = 0;

	assert(dst_ip == INADDR_BROADCAST);
	assert(len <= sizeof s->last_sent);
	memcpy(s->last_sent, buf, len);
	s->last_len = len;
	/* Read the message type straight out of the message under test. */
	{
		size_t off = DCLI_HEADER_LEN + 4;
		const uint8_t *p = buf;

		while (off + 1 < len) {
			uint8_t code = p[off++];
			uint8_t olen;

			if (code == DCLI_OPT_END)
				break;
			if (code == 0)
				continue;
			olen = p[off++];
			if (code == DCLI_OPT_MSG_TYPE && olen == 1)
				type = p[off];
			off += olen;
		}
	}
	if (type == DCLI_DISCOVER)
		s->seen_discover++;
	if (type == DCLI_REQUEST) {
		s->seen_request++;
		if (s->nak_once && s->naks == 0) {
			s->naks++;
			memcpy(s->mac, s->last_sent + 28, 6);
			s->xid = dcli_be32(s->last_sent + 4);
			s->nak_reply_type = DCLI_NAK;
		} else {
			s->nak_reply_type = DCLI_ACK;
		}
	}
	return 0;
}

static int srv_recv(void *ctx, void *buf, size_t cap, int timeout_ms)
{
	struct srv *s = ctx;
	uint32_t xid;
	uint8_t mac[6];
	int type = 0;

	(void)timeout_ms;
	if (!s->mac_replies)
		return 0;
	if (!s->last_len)
		return 0;
	{
		size_t off = DCLI_HEADER_LEN + 4;
		const uint8_t *p = s->last_sent;

		while (off + 1 < s->last_len) {
			uint8_t code = p[off++];
			uint8_t olen;

			if (code == DCLI_OPT_END)
				break;
			if (code == 0)
				continue;
			olen = p[off++];
			if (code == DCLI_OPT_MSG_TYPE && olen == 1)
				type = p[off];
			off += olen;
		}
	}
	xid = dcli_be32(s->last_sent + 4);
	memcpy(mac, s->last_sent + 28, 6);
	s->last_len = 0;			/* one reply per message */
	if (type == DCLI_DISCOVER)
		return (int)srv_build(buf, cap, DCLI_OFFER, xid, mac, s->offer_ip,
				      s->server_ip, s->mask, s->gw, s->dns);
	if (type == DCLI_REQUEST)
		return (int)srv_build(buf, cap, s->nak_reply_type, xid, mac,
				      s->nak_reply_type == DCLI_NAK ? 0 : s->offer_ip,
				      s->server_ip, s->mask, s->gw, s->dns);
	return 0;
}

/* ------------------------------------------------------------------ tests -- */

static void test_wire(void)
{
	uint8_t mac[6] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01 };
	uint8_t msg[DCLI_MAX_LEN];
	size_t n;

	n = dcli_build(msg, sizeof msg, DCLI_DISCOVER, 0x11223344u, mac, 0, 0,
		       "scourge", 0);
	assert(n >= DCLI_MIN_LEN && n <= sizeof msg);
	assert(msg[0] == DCLI_BOOTREQUEST);
	assert(msg[1] == DCLI_HTYPE_ETHER);
	assert(msg[2] == DCLI_HLEN_ETHER);
	assert(msg[4] == 0x11 && msg[5] == 0x22 && msg[6] == 0x33 && msg[7] == 0x44);
	assert(msg[10] == 0x80 && msg[11] == 0x00);	/* broadcast flag */
	assert(!memcmp(msg + 28, mac, 6));
	assert(msg[DCLI_HEADER_LEN] == 0x63 && msg[DCLI_HEADER_LEN + 3] == 0x63);
	/* option 53 = DISCOVER right after the cookie (this client never pads) */
	assert(msg[DCLI_HEADER_LEN + 4] == DCLI_OPT_MSG_TYPE);
	assert(msg[DCLI_HEADER_LEN + 5] == 1);
	assert(msg[DCLI_HEADER_LEN + 6] == DCLI_DISCOVER);
	/* option 61 = htype + mac */
	assert(msg[DCLI_HEADER_LEN + 7] == DCLI_OPT_CLIENT_ID);
	assert(msg[DCLI_HEADER_LEN + 8] == 7);
	assert(msg[DCLI_HEADER_LEN + 9] == DCLI_HTYPE_ETHER);
	assert(!memcmp(msg + DCLI_HEADER_LEN + 10, mac, 6));
	/* the parameter request list starts with the netmask and includes the router */
	{
		size_t off = DCLI_HEADER_LEN + 4;
		int saw_mask = 0, saw_router = 0, saw_end = 0, saw_hostname = 0;

		while (off + 1 < n) {
			uint8_t code = msg[off++];
			uint8_t olen;

			if (code == DCLI_OPT_END) {
				saw_end = 1;
				break;
			}
			if (code == 0)
				continue;
			olen = msg[off++];
			if (code == DCLI_OPT_PARAM_LIST) {
				for (uint8_t k = 0; k < olen; k++) {
					if (msg[off + k] == DCLI_OPT_MASK)
						saw_mask = 1;
					if (msg[off + k] == DCLI_OPT_ROUTER)
						saw_router = 1;
				}
			}
			if (code == DCLI_OPT_HOSTNAME && olen == 7 &&
			    !memcmp(msg + off, "scourge", 7))
				saw_hostname = 1;
			off += olen;
		}
		assert(saw_mask && saw_router && saw_hostname && saw_end);
	}

	/* A REQUEST must name the offered address and the server it came from. */
	n = dcli_build(msg, sizeof msg, DCLI_REQUEST, 0x11223344u, mac,
		       0xc0a80114u /* 192.168.1.20 */, 0xc0a80101u,
		       "scourge", 0);
	assert(n > 0);
	{
		size_t off = DCLI_HEADER_LEN + 4;
		int saw_req = 0, saw_srv = 0;

		while (off + 1 < n) {
			uint8_t code = msg[off++];
			uint8_t olen;

			if (code == DCLI_OPT_END)
				break;
			if (code == 0)
				continue;
			olen = msg[off++];
			if (code == DCLI_OPT_REQUESTED_IP && olen == 4) {
				assert(dcli_be32(msg + off) == 0xc0a80114u);
				saw_req = 1;
			}
			if (code == DCLI_OPT_SERVER_ID && olen == 4) {
				assert(dcli_be32(msg + off) == 0xc0a80101u);
				saw_srv = 1;
			}
			off += olen;
		}
		assert(saw_req && saw_srv);
	}

	/* A frame small enough to be safe must be refused rather than truncated. */
	assert(dcli_build(msg, 100, DCLI_DISCOVER, 1, mac, 0, 0, NULL, 0) == 0);
	printf("PASS: DISCOVER/REQUEST encoding (flags, chaddr, options 53/61/55/50/54)\n");
}

static void test_parse(void)
{
	uint8_t mac[6] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01 };
	uint8_t other[6] = { 0, 0, 0, 0, 0, 2 };
	uint8_t frame[600];
	struct dcli_lease lease;
	size_t n;

	n = srv_build(frame, sizeof frame, DCLI_OFFER, 0xabcdu, mac, 0xc0a80114u,
		      0xc0a80101u, 0xffffff00u, 0xc0a80101u, 0x08080808u);
	assert(dcli_parse(frame, n, 0xabcdu, mac, &lease) == DCLI_OFFER);
	assert(lease.ip == 0xc0a80114u);
	assert(lease.mask == 0xffffff00u);
	assert(lease.server == 0xc0a80101u);
	assert(lease.have_router && lease.router == 0xc0a80101u);
	assert(lease.have_dns && lease.dns == 0x08080808u);
	assert(lease.lease_seconds == 43200u && lease.t1 == 21600u);
	assert(lease.t2 == 37800u);

	/* Other transaction, other client, a request instead of a reply, bad cookie:
	 * all of them are "not ours", not errors. */
	assert(dcli_parse(frame, n, 0xabce, mac, NULL) == 0);
	assert(dcli_parse(frame, n, 0xabcdu, other, NULL) == 0);
	frame[0] = DCLI_BOOTREQUEST;
	assert(dcli_parse(frame, n, 0xabcdu, mac, NULL) == 0);
	frame[0] = DCLI_BOOTREPLY;
	frame[DCLI_HEADER_LEN] = 0x00;
	assert(dcli_parse(frame, n, 0xabcdu, mac, NULL) == 0);

	/* Truncated option area is malformed, and says so. */
	frame[DCLI_HEADER_LEN] = 0x63;
	assert(dcli_parse(frame, DCLI_HEADER_LEN + 5, 0xabcdu, mac, NULL) < 0);
	printf("PASS: reply parsing (lease fields) and foreign/malformed frames\n");
}

static void test_exchange(int nak_once)
{
	uint8_t mac[6] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01 };
	struct srv srv;
	struct dcli_io io;
	struct dcli_state st;
	struct dcli_lease lease;
	int rc;

	memset(&srv, 0, sizeof srv);
	srv.mac_replies = 1;
	srv.nak_once = nak_once;
	srv.offer_ip = 0xc0a80114u;
	srv.server_ip = 0xc0a80101u;
	srv.mask = 0xffffff00u;
	srv.gw = 0xc0a80101u;
	srv.dns = 0x08080808u;

	memset(&io, 0, sizeof io);
	io.send = srv_send;
	io.recv = srv_recv;
	memset(&st, 0, sizeof st);
	st.io = &io;
	st.io_ctx = &srv;
	memcpy(st.mac, mac, 6);
	st.xid = 0x0badc0deu;

	memset(&lease, 0, sizeof lease);
	rc = dcli_acquire(&st, "scourge", 200, &lease);
	assert(rc == 0);
	assert(lease.ip == 0xc0a80114u);
	assert(lease.mask == 0xffffff00u);
	assert(lease.have_router && lease.router == 0xc0a80101u);
	assert(lease.lease_seconds == 43200u);
	assert(srv.seen_discover >= 1);
	assert(srv.seen_request >= 1);
	if (nak_once) {
		assert(srv.naks == 1);
		assert(srv.seen_discover >= 2);	/* a NAK restarts with a new DISCOVER */
		assert(st.xid != 0x0badc0deu);
		printf("PASS: NAK restarts the exchange with a fresh transaction\n");
	} else {
		assert(srv.naks == 0);
		printf("PASS: DISCOVER/OFFER/REQUEST/ACK end to end (lease applied data)\n");
	}
}

static void test_timeout(void)
{
	uint8_t mac[6] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01 };
	struct srv srv;
	struct dcli_io io;
	struct dcli_state st;
	struct dcli_lease lease;

	memset(&srv, 0, sizeof srv);
	srv.mac_replies = 0;			/* a silent network */
	memset(&io, 0, sizeof io);
	io.send = srv_send;
	io.recv = srv_recv;
	memset(&st, 0, sizeof st);
	st.io = &io;
	st.io_ctx = &srv;
	memcpy(st.mac, mac, 6);
	st.xid = 0x1234u;
	memset(&lease, 0, sizeof lease);

	assert(dcli_acquire(&st, "scourge", 50, &lease) == 1);
	assert(srv.seen_discover == 3);		/* three tries, then give up */
	assert(lease.ip == 0);
	printf("PASS: a silent network gives up after three DISCOVERs (no fake lease)\n");
}

static void test_lease_file(void)
{
	struct dcli_lease lease;
	char line[256];
	FILE *f;
	int seen_ip = 0, seen_prefix = 0, seen_gw = 0, seen_dns = 0;
	const char *path = "dhcp_client_test.lease";

	memset(&lease, 0, sizeof lease);
	lease.ip = 0x0a630032u;			/* 10.99.0.50 */
	lease.mask = 0xffffff00u;
	lease.router = 0x0a630001u;
	lease.dns = 0x08080808u;
	lease.server = 0x0a630001u;
	lease.have_router = lease.have_dns = 1;
	lease.lease_seconds = 43200;
	assert(prefix_len(0xffffff00u) == 24);
	assert(prefix_len(0xffff0000u) == 16);
	assert(lease_write(path, "wlan0", &lease) == 0);
	f = fopen(path, "r");
	assert(f);
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "ip=10.99.0.50", 13)) seen_ip = 1;
		if (!strncmp(line, "prefix=24", 9)) seen_prefix = 1;
		if (!strncmp(line, "router=10.99.0.1", 16)) seen_gw = 1;
		if (!strncmp(line, "dns=8.8.8.8", 11)) seen_dns = 1;
	}
	fclose(f);
	unlink(path);
	assert(seen_ip && seen_prefix && seen_gw && seen_dns);
	printf("PASS: the lease file carries ip/mask/prefix/gw/dns for the daemon\n");
}

int main(void)
{
	test_wire();
	test_parse();
	test_exchange(0);
	test_exchange(1);
	test_timeout();
	test_lease_file();
	printf("PASS: dhcp_client exchange, NAK, timeout and lease file\n");
	return 0;
}
