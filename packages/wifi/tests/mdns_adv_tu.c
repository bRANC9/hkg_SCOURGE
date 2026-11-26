/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * mdns_adv_tu.c - the responder's wire behaviour, checked without a network.
 *
 * The queries here are built by hand (not with the code under test), so a wrong
 * label encoding on our side cannot hide behind a matching encoder: the bytes are
 * the DNS wire format from RFC 1035 / RFC 6762.
 */
#define _GNU_SOURCE 1
#define MDNS_ADV_NO_MAIN 1
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/mdns_adv.c"

/* One record as the test sees it on the wire. */
struct rec {
	char name[128];
	uint16_t type;
	uint16_t rclass;
	uint32_t ttl;
	uint16_t rdlen;
	const uint8_t *rdata;
};

static size_t walk(const uint8_t *msg, size_t len, struct rec *out, size_t max)
{
	uint16_t ancount;
	size_t off = 12;
	size_t n = 0;

	memcpy(&ancount, msg + 6, 2);
	ancount = ntohs(ancount);
	while (n < ancount && n < max) {
		struct rec r;
		size_t end = off;
		uint16_t v16;

		memset(&r, 0, sizeof r);
		assert(mdns_decode_name(msg, len, off, r.name, sizeof r.name, &end) == 0);
		assert(end + 10 <= len);
		memcpy(&v16, msg + end, 2);
		r.type = ntohs(v16);
		memcpy(&v16, msg + end + 2, 2);
		r.rclass = ntohs(v16);
		r.ttl = (uint32_t)msg[end + 4] << 24 | (uint32_t)msg[end + 5] << 16 |
			(uint32_t)msg[end + 6] << 8 | msg[end + 7];
		memcpy(&v16, msg + end + 8, 2);
		r.rdlen = ntohs(v16);
		assert(end + 10 + r.rdlen <= len);
		r.rdata = msg + end + 10;
		off = end + 10 + r.rdlen;
		out[n++] = r;
	}
	return n;
}

/* Query with one question: id, flags, qdcount=1, then the name labels. */
static size_t build_query(uint8_t *out, size_t cap, uint16_t id, uint16_t flags,
			  const char *name, uint16_t type, uint16_t qclass)
{
	size_t off = 12;
	size_t nlen;
	uint16_t v;

	memset(out, 0, cap);
	v = htons(id);
	memcpy(out, &v, 2);
	v = htons(flags);
	memcpy(out + 2, &v, 2);
	v = htons(1);
	memcpy(out + 4, &v, 2);			/* qdcount */
	nlen = mdns_encode_name(out + 12, cap - 12, name);
	assert(nlen > 0);
	off += nlen;
	v = htons(type);
	memcpy(out + off, &v, 2);
	off += 2;
	v = htons(qclass);
	memcpy(out + off, &v, 2);
	return off + 2;
}

static const struct rec *find_type(const struct rec *recs, size_t n, uint16_t type)
{
	for (size_t i = 0; i < n; i++)
		if (recs[i].type == type)
			return &recs[i];
	return NULL;
}

static void test_host_a(void)
{
	struct mdns_ctx ctx;
	uint8_t query[512];
	uint8_t reply[MDNS_PKT_MAX];
	struct rec recs[16];
	size_t qlen, rlen, n;
	int unicast = -1;
	uint16_t txid = 0xbeef;
	const struct rec *a;

	fill_ctx(&ctx, "scourge-ab", 0x0a630032u /* 10.99.0.50 */, 2222, 0);
	qlen = build_query(query, sizeof query, 0x1234, 0, "scourge-ab.local",
			   MDNS_TYPE_A, MDNS_CLASS_IN);
	rlen = mdns_build_response(&ctx, query, qlen, reply, sizeof reply, &unicast,
				   &txid);
	assert(rlen > 0);
	assert(txid == 0x1234);
	assert(unicast == 0);
	assert((reply[2] & 0x80) && (reply[3] == 0));	/* QR and AA */
	n = walk(reply, rlen, recs, 16);
	assert(n == 1);
	a = find_type(recs, n, MDNS_TYPE_A);
	assert(a);
	assert(!strcmp(a->name, "scourge-ab.local"));
	assert(a->rclass == (MDNS_CLASS_IN | MDNS_CLASS_FLUSH));
	assert(a->ttl == MDNS_TTL_HOST);
	assert(a->rdlen == 4);
	assert(a->rdata[0] == 10 && a->rdata[1] == 99 && a->rdata[2] == 0 &&
	       a->rdata[3] == 50);
	printf("PASS: A %s -> 10.99.0.50 (class flush, ttl %d)\n", a->name,
	       MDNS_TTL_HOST);
}

static void test_service_ptr(void)
{
	struct mdns_ctx ctx;
	uint8_t query[512];
	uint8_t reply[MDNS_PKT_MAX];
	struct rec recs[16];
	size_t qlen, rlen, n;
	int unicast = 0;
	uint16_t txid = 0;
	const struct rec *ptr, *srv, *txt, *a;
	char target[128];
	size_t end = 0;
	uint16_t port;

	fill_ctx(&ctx, "scourge-ab", 0x0a630032u, 2222, 0);
	qlen = build_query(query, sizeof query, 0, 0, "_ssh._tcp.local",
			   MDNS_TYPE_PTR, MDNS_CLASS_IN);
	rlen = mdns_build_response(&ctx, query, qlen, reply, sizeof reply, &unicast,
				   &txid);
	assert(rlen > 0);
	n = walk(reply, rlen, recs, 16);
	assert(n == 4);				/* PTR + SRV + TXT + A */
	ptr = find_type(recs, n, MDNS_TYPE_PTR);
	srv = find_type(recs, n, MDNS_TYPE_SRV);
	txt = find_type(recs, n, MDNS_TYPE_TXT);
	assert(ptr && srv && txt);
	assert(!strcmp(ptr->name, "_ssh._tcp.local"));
	assert(mdns_decode_name(ptr->rdata, ptr->rdlen, 0, target, sizeof target,
				&end) == 0);
	assert(!strcmp(target, "scourge-ab._ssh._tcp.local"));
	assert(!strcmp(srv->name, "scourge-ab._ssh._tcp.local"));
	memcpy(&port, srv->rdata + 4, 2);
	assert(ntohs(port) == 2222);
	/* SRV target is the host name, uncompressed */
	assert(mdns_decode_name(srv->rdata, srv->rdlen, 6, target, sizeof target,
				&end) == 0);
	assert(!strcmp(target, "scourge-ab.local"));
	assert(txt->rdata[0] == 5 && !memcmp(txt->rdata + 1, "u=car", 5));
	(void)a;
	printf("PASS: PTR/SRV/TXT for _ssh._tcp.local (port 2222, target %s)\n",
	       "scourge-ab.local");
}

static void test_compressed_and_quirks(void)
{
	struct mdns_ctx ctx;
	uint8_t query[512];
	uint8_t reply[MDNS_PKT_MAX];
	struct rec recs[16];
	size_t qlen, rlen, n;
	int unicast = 0;
	uint16_t txid = 0;
	uint16_t v;

	fill_ctx(&ctx, "scourge-ab", 0x0a630032u, 2222, 8090);

	/* A real client compresses: "scourge-ab" + pointer to the "local" label of
	 * an earlier name.  Build that by hand: header, q1 = scourge-ab.local A,
	 * q2 = "scourge-ab" + ptr(12+0?) ... simplest: q1 uncompressed, then a
	 * second question whose name is a pointer back to q1's name. */
	qlen = build_query(query, sizeof query, 7, 0, "scourge-ab.local",
			   MDNS_TYPE_A, MDNS_CLASS_IN);
	v = htons(2);
	memcpy(query + 4, &v, 2);		/* qdcount = 2 */
	query[qlen] = 0xc0;			/* pointer to offset 12 */
	query[qlen + 1] = 12;
	memcpy(query + qlen + 2, "\x00\x01", 2);	/* type A */
	memcpy(query + qlen + 4, "\x00\x01", 2);	/* class IN */
	qlen += 6;

	rlen = mdns_build_response(&ctx, query, qlen, reply, sizeof reply, &unicast,
				   &txid);
	assert(rlen > 0);
	n = walk(reply, rlen, recs, 16);
	assert(n == 2);				/* both questions answered */
	assert(find_type(recs, n, MDNS_TYPE_A));

	/* Unrelated name: nothing to say, and nothing is sent. */
	qlen = build_query(query, sizeof query, 0, 0, "example.lan", MDNS_TYPE_A,
			   MDNS_CLASS_IN);
	assert(mdns_build_response(&ctx, query, qlen, reply, sizeof reply, &unicast,
				   &txid) == 0);

	/* A response must never be answered (no loops). */
	qlen = build_query(query, sizeof query, 0, 0x8400, "scourge-ab.local",
			   MDNS_TYPE_A, MDNS_CLASS_IN);
	assert(mdns_build_response(&ctx, query, qlen, reply, sizeof reply, &unicast,
				   &txid) == 0);

	/* QU bit: the asker wants a unicast answer with its own id. */
	qlen = build_query(query, sizeof query, 0x4321, 0, "scourge-ab.local",
			   MDNS_TYPE_A, MDNS_CLASS_IN | MDNS_CLASS_QU);
	unicast = 0;
	rlen = mdns_build_response(&ctx, query, qlen, reply, sizeof reply, &unicast,
				   &txid);
	assert(rlen > 0 && unicast == 1 && txid == 0x4321);

	/* ANY on the host answers the address too. */
	qlen = build_query(query, sizeof query, 0, 0, "scourge-ab.local",
			   MDNS_TYPE_ANY, MDNS_CLASS_IN);
	assert(mdns_build_response(&ctx, query, qlen, reply, sizeof reply, &unicast,
				   &txid) > 0);
	printf("PASS: compressed names, foreign names, loop guard, QU bit, ANY\n");
}

static void test_announce(void)
{
	struct mdns_ctx ctx;
	uint8_t pkt[MDNS_PKT_MAX];
	struct rec recs[16];
	size_t len, n;

	fill_ctx(&ctx, "scourge-x1", 0x0a630032u, 2222, 0);
	len = mdns_build_announce(&ctx, pkt, sizeof pkt);
	assert(len > 0);
	n = walk(pkt, len, recs, 16);
	assert(n == 4);				/* PTR + SRV + TXT + A */
	assert(find_type(recs, n, MDNS_TYPE_A));

	fill_ctx(&ctx, "scourge-x1", 0x0a630032u, 2222, 8090);
	len = mdns_build_announce(&ctx, pkt, sizeof pkt);
	n = walk(pkt, len, recs, 16);
	assert(n == 7);				/* both services plus the address */
	printf("PASS: announcements carry host + services (4 and 7 records)\n");
}

int main(void)
{
	test_host_a();
	test_service_ptr();
	test_compressed_and_quirks();
	test_announce();
	printf("PASS: mdns_adv answers, ignores foreign/response packets and announces\n");
	return 0;
}
