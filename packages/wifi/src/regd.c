/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */
/* regd - the regulatory daemon this unit does not ship.
 *
 * Why this exists: cfg80211 only learned to read the signed firmware database
 * (regulatory.db / regulatory.db.p7s) in 4.15.  This unit runs 4.1.27-abl,
 * where every regulatory hint ends in
 *
 *     cfg80211: Calling CRDA to update world regulatory domain
 *
 * and CRDA - the userspace daemon that answered that uevent with
 * NL80211_CMD_SET_REG - is absent (no binary, no udev rule, no /lib/crda/).
 * So "iw reg set HU" and the ieee80211_regdom module parameter both leave
 * cfg80211 on country 00 (world), every 5 GHz channel stays passive, and the
 * 5 GHz AP - the only band wireless Android Auto accepts - cannot beacon.
 *
 * regd fills that hole without libnl and without RSA: it parses the database we
 * already ship (the pinned upstream-signed regulatory.db that
 * scripts/fetch_regdb.py downloads), converts the rules into the
 * NL80211_CMD_SET_REG message CRDA used to send, and then reads the regdomain
 * back so the caller learns whether it actually stuck.
 *
 * The signature is not re-checked here: 4.1 cannot verify it either, any root
 * process can write the file, and the pinned upstream pair in
 * config/third_party.lock is what makes the data trustworthy.  What regd does
 * NOT do is pretend: if cfg80211 keeps another country (a driver that manages
 * its own regdomain), it says so and exits non-zero.
 *
 * regdb file layout (wireless-regdb db2fw.py, version 20):
 *   struct regdb_file_header { __be32 magic; __be32 version; }
 *   country table: { char alpha2[2]; __be16 collection_offset >> 2; } ...
 *                  terminated by { 0, 0 }
 *   rule: { __u8 len; __u8 flags; __be16 max_eirp_cbm;
 *           __be32 freq_start_khz; __be32 freq_end_khz; __be32 max_bw_khz; }
 *         (len > 16 carries an optional u16 CAC timeout, > 18 a WMM pointer)
 *   collection: { __u8 len(=3); __u8 num_rules; __u8 dfs_region; __u8 pad;
 *                 __be16 rule_offset >> 2 ... }
 * The file's rule flags are db2fw's compact bits and are NOT the kernel's
 * NL80211_RRF_* numbering - see map_flags() for the conversion db2fw applied.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>

#define REGDB_MAGIC	0x52474442u
#define REGDB_VERSION	20
/* A country entry never carries more than a handful of rules; a larger count
 * means the file is not what we think it is, and silently sending a truncated
 * regdomain would look like a fix. */
#define REGDB_MAX_RULES	64
#define REGDB_MAX_BYTES	(4u * 1024u * 1024u)

/* Where the database can be: the installer keeps a copy next to the daemon so
 * the tool works even when /lib/firmware is not writable, and the firmware copy
 * is what a newer kernel would read. */
static const char *const g_db_candidates[] = {
	"/rw_data/wifi/regulatory.db",
	"/lib/firmware/regulatory.db",
	"/usr/lib/firmware/regulatory.db",
	NULL,
};

enum {
	EXIT_OK = 0,
	EXIT_USAGE = 1,
	EXIT_NODB = 2,
	EXIT_NOCOUNTRY = 3,
	EXIT_NETLINK = 4,
	EXIT_SETREG = 5,
	EXIT_NOTAPPLIED = 6,
};

struct regd_rule {
	uint32_t start_khz;
	uint32_t end_khz;
	uint32_t max_bw_khz;
	uint32_t eirp_cbm;
	uint32_t kernel_flags;
};

struct regd_domain {
	char alpha2[3];
	unsigned dfs_region;
	unsigned n_rules;
	unsigned unknown_flags;
	struct regd_rule rule[REGDB_MAX_RULES];
};

struct buf {
	uint8_t *data;
	size_t len;
	size_t cap;
};

struct opts {
	const char *db;
	const char *country;
	const char *status_file;
	const char *mode;
	int family_id;
	int timeout_ms;
	int trigger;
	int quiet;
};

static volatile sig_atomic_t g_stop;
static int g_verbose;

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	fputs("regd: ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fputc('\n', stdout);
	fflush(stdout);
}

static void errmsg(const char *fmt, ...)
{
	va_list ap;

	fputs("regd: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* ---------------------------------------------------------------- buffers -- */

static void buf_need(struct buf *b, size_t extra)
{
	size_t want = b->len + extra;

	if (want <= b->cap)
		return;
	if (b->cap == 0)
		b->cap = 512;
	while (b->cap < want)
		b->cap *= 2;
	b->data = realloc(b->data, b->cap);
	if (!b->data) {
		errmsg("out of memory");
		exit(EXIT_NODB);
	}
}

static void buf_add(struct buf *b, const void *data, size_t len)
{
	buf_need(b, len);
	if (len)
		memcpy(b->data + b->len, data, len);
	b->len += len;
}

static void buf_zero(struct buf *b, size_t len)
{
	buf_need(b, len);
	memset(b->data + b->len, 0, len);
	b->len += len;
}

static void buf_free(struct buf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}

/* Read-side attribute walkers.  <linux/netlink.h> ships struct nlattr and
 * NLA_ALIGN but not the kernel's nla_ok_()/nla_next_()/nla_data_() helpers, and
 * libnl is not available on this image. */
static int nla_ok_(const struct nlattr *a, int rem)
{
	return rem >= (int)sizeof(*a) && a->nla_len >= sizeof(*a) &&
	       (int)a->nla_len <= rem;
}

static struct nlattr *nla_next_(const struct nlattr *a, int *rem)
{
	int step = NLA_ALIGN(a->nla_len);

	*rem -= step;
	return (struct nlattr *)((uint8_t *)a + step);
}

static void *nla_data_(const struct nlattr *a)
{
	return (uint8_t *)a + NLA_HDRLEN;
}

/* --------------------------------------------------------------- netlink -- */

/* Attribute writers mirror libnl's nla_put*: header, payload, zero padding to
 * a 4 byte boundary.  Nested attributes are opened, written and then patched. */
static size_t nla_begin(struct buf *b, unsigned type)
{
	struct nlattr attr;
	size_t off = b->len;

	attr.nla_len = 0;
	attr.nla_type = (uint16_t)(type | NLA_F_NESTED);
	buf_add(b, &attr, sizeof attr);
	return off;
}

static void nla_end(struct buf *b, size_t off)
{
	struct nlattr *attr = (struct nlattr *)(b->data + off);

	attr->nla_len = (uint16_t)(b->len - off);
	buf_need(b, NLA_ALIGN(b->len) - b->len);
	buf_zero(b, NLA_ALIGN(b->len) - b->len);
}

static void nla_add(struct buf *b, unsigned type, const void *data, size_t len)
{
	struct nlattr attr;

	attr.nla_len = (uint16_t)(sizeof attr + len);
	attr.nla_type = (uint16_t)type;
	buf_add(b, &attr, sizeof attr);
	buf_add(b, data, len);
	buf_need(b, NLA_ALIGN(b->len) - b->len);
	buf_zero(b, NLA_ALIGN(b->len) - b->len);
}

static void nla_u32(struct buf *b, unsigned type, uint32_t value)
{
	uint32_t v = value;

	nla_add(b, type, &v, sizeof v);
}

static void nla_u8(struct buf *b, unsigned type, uint8_t value)
{
	nla_add(b, type, &value, 1);
}

/* ---------------------------------------------------------------- regdb -- */

static uint32_t be32_at(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static unsigned be16_at(const uint8_t *p)
{
	return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static int read_whole(const char *path, uint8_t **out, size_t *out_len,
		      char *err, size_t errlen)
{
	struct stat st;
	uint8_t *data;
	size_t got = 0;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) < 0 || st.st_size <= 0 ||
	    (unsigned long)st.st_size > REGDB_MAX_BYTES) {
		snprintf(err, errlen, "cannot stat %s or size out of range", path);
		close(fd);
		return -1;
	}
	data = malloc((size_t)st.st_size);
	if (!data) {
		snprintf(err, errlen, "out of memory reading %s", path);
		close(fd);
		return -1;
	}
	while (got < (size_t)st.st_size) {
		ssize_t n = read(fd, data + got, (size_t)st.st_size - got);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			snprintf(err, errlen, "short read on %s", path);
			free(data);
			close(fd);
			return -1;
		}
		got += (size_t)n;
	}
	close(fd);
	*out = data;
	*out_len = got;
	return 0;
}

/* db2fw writes compact 8 bit rule flags; the kernel wants NL80211_RRF_* bits.
 * Conversion applied by db2fw (wireless-regdb db2fw.py):
 *   db bit 0  (NO-OFDM)    -> file 1<<0
 *   db bit 3  (NO-OUTDOOR) -> file 1<<1
 *   db bit 4  (DFS)        -> file 1<<2
 *   db bit 7  (NO-IR)      -> file 1<<3
 *   db bit 11 (AUTO-BW)    -> file 1<<4
 * Verified against the shipped database: world "00" 5490-5730 MHz carries
 * NO-IR|DFS (file 0x0c), 5250-5330 adds AUTO-BW (file 0x1c), HU 5945-6425 is
 * NO-OUTDOOR (file 0x02).  Every one of these bits exists in 4.1's
 * nl80211_reg_rule_flags, so nothing has to be dropped for this kernel. */
static uint32_t map_flags(unsigned file_flags, unsigned *unknown)
{
	uint32_t flags = 0;

	if (file_flags & (1u << 0))
		flags |= NL80211_RRF_NO_OFDM;
	if (file_flags & (1u << 1))
		flags |= NL80211_RRF_NO_OUTDOOR;
	if (file_flags & (1u << 2))
		flags |= NL80211_RRF_DFS;
	if (file_flags & (1u << 3))
		flags |= NL80211_RRF_NO_IR;
	if (file_flags & (1u << 4))
		flags |= NL80211_RRF_AUTO_BW;
	if (unknown)
		*unknown |= file_flags & ~0x1fu;
	return flags;
}

static void format_kernel_flags(uint32_t flags, char *out, size_t outlen)
{
	static const struct {
		uint32_t bit;
		const char *name;
	} names[] = {
		{ NL80211_RRF_NO_OFDM, "NO-OFDM" },
		{ NL80211_RRF_NO_CCK, "NO-CCK" },
		{ NL80211_RRF_NO_INDOOR, "NO-INDOOR" },
		{ NL80211_RRF_NO_OUTDOOR, "NO-OUTDOOR" },
		{ NL80211_RRF_DFS, "DFS" },
		{ NL80211_RRF_PTP_ONLY, "PTP-ONLY" },
		{ NL80211_RRF_PTMP_ONLY, "PTMP-ONLY" },
		{ NL80211_RRF_NO_IR, "NO-IR" },
		{ NL80211_RRF_AUTO_BW, "AUTO-BW" },
		{ NL80211_RRF_IR_CONCURRENT, "IR-CONCURRENT" },
	};
	size_t used = 0;
	size_t i;

	out[0] = '\0';
	for (i = 0; i < sizeof names / sizeof names[0]; i++) {
		if (!(flags & names[i].bit))
			continue;
		used += (size_t)snprintf(out + used, outlen - used, "%s%s",
					 used ? "," : "", names[i].name);
		if (used >= outlen)
			return;
	}
	if (!used)
		snprintf(out, outlen, "-");
}

/* Returns 0 on success, 1 when the country is not in the database. */
static int regdb_lookup(const uint8_t *db, size_t len, const char *alpha2,
			struct regd_domain *dom, char *err, size_t errlen)
{
	unsigned coll;
	unsigned count;
	int terminated;
	size_t off;
	unsigned i;

	memset(dom, 0, sizeof *dom);
	if (len < 12 || be32_at(db) != REGDB_MAGIC) {
		snprintf(err, errlen, "not a regdb (magic mismatch)");
		return -1;
	}
	if (be32_at(db + 4) != REGDB_VERSION) {
		snprintf(err, errlen, "unsupported regdb version %u (expected %u)",
			 be32_at(db + 4), REGDB_VERSION);
		return -1;
	}

	/* Country table: 4 byte entries of { alpha2[2], offset >> 2 }, closed by an
	 * all-zero entry (db2fw writes 4 zero bytes after the table). */
	coll = 0;
	terminated = 0;
	for (off = 8; off + 4 <= len; off += 4) {
		if (db[off] == 0 && db[off + 1] == 0) {
			terminated = 1;
			break;
		}
		if (db[off] != (uint8_t)alpha2[0] || db[off + 1] != (uint8_t)alpha2[1])
			continue;
		coll = be16_at(db + off + 2) * 4u;
		break;
	}
	if (!coll && !terminated) {
		/* db2fw closes the country table with four zero bytes; without that
		 * marker the file is truncated and no lookup can be trusted. */
		snprintf(err, errlen, "country table is not terminated (truncated db?)");
		return -1;
	}
	if (!coll) {
		snprintf(err, errlen, "country %s is not in the database", alpha2);
		return 1;
	}
	if (coll + 4u > len || db[coll] != 3) {
		snprintf(err, errlen, "collection offset %u out of range", coll);
		return -1;
	}
	count = db[coll + 1];
	if (count == 0 || count > REGDB_MAX_RULES) {
		snprintf(err, errlen, "implausible rule count %u for %s", count, alpha2);
		return -1;
	}
	if (coll + 4u + 2u * count > len) {
		snprintf(err, errlen, "rule table for %s runs past the file", alpha2);
		return -1;
	}
	memcpy(dom->alpha2, alpha2, 2);
	dom->alpha2[2] = '\0';
	dom->dfs_region = db[coll + 2];
	dom->n_rules = count;
	for (i = 0; i < count; i++) {
		unsigned base = be16_at(db + coll + 4 + 2 * i) * 4u;
		struct regd_rule *rule = &dom->rule[i];
		unsigned rule_len;
		unsigned file_flags;

		if (base + 16u > len) {
			snprintf(err, errlen, "rule %u of %s out of range", i, alpha2);
			return -1;
		}
		rule_len = db[base];
		if (rule_len < 16u || base + rule_len > len) {
			snprintf(err, errlen, "rule %u of %s has length %u", i, alpha2,
				 rule_len);
			return -1;
		}
		file_flags = db[base + 1];
		rule->eirp_cbm = be16_at(db + base + 2);
		rule->start_khz = be32_at(db + base + 4);
		rule->end_khz = be32_at(db + base + 8);
		rule->max_bw_khz = be32_at(db + base + 12);
		rule->kernel_flags = map_flags(file_flags, &dom->unknown_flags);
		if (rule->start_khz >= rule->end_khz || rule->max_bw_khz == 0) {
			snprintf(err, errlen, "rule %u of %s is corrupt", i, alpha2);
			return -1;
		}
	}
	return 0;
}

static void print_domain(const struct regd_domain *dom)
{
	unsigned i;

	printf("country %s dfs_region=%u rules=%u\n", dom->alpha2, dom->dfs_region,
	       dom->n_rules);
	for (i = 0; i < dom->n_rules; i++) {
		char flags[96];

		format_kernel_flags(dom->rule[i].kernel_flags, flags, sizeof flags);
		printf("  %8u-%8u kHz bw=%7u kHz eirp=%4u cBm flags=%s\n",
		       dom->rule[i].start_khz, dom->rule[i].end_khz,
		       dom->rule[i].max_bw_khz, dom->rule[i].eirp_cbm, flags);
	}
}

/* ------------------------------------------------------------------ genl -- */

static uint32_t g_seq;

static int nl_send(int fd, const struct buf *msg, char *err, size_t errlen)
{
	struct sockaddr_nl addr;

	memset(&addr, 0, sizeof addr);
	addr.nl_family = AF_NETLINK;
	if (sendto(fd, msg->data, msg->len, 0, (struct sockaddr *)&addr,
		   sizeof addr) < 0) {
		snprintf(err, errlen, "sendto(netlink): %s", strerror(errno));
		return -1;
	}
	return 0;
}

/* Send one message and collect the reply payloads (genlmsghdr + attrs).
 * Returns 0 when at least one frame arrived, 1 when the socket timed out
 * (no answer), -1 on a kernel error reply (errno set, err filled). */
static int nl_talk(int fd, const struct buf *msg, struct buf *reply,
		   char *err, size_t errlen)
{
	uint8_t raw[16384];
	int saw_ack = 0;
	int saw_frame = 0;

	if (nl_send(fd, msg, err, errlen) < 0)
		return -1;
	for (;;) {
		struct nlmsghdr *nlh;
		ssize_t n;
		int len;

		n = recv(fd, raw, sizeof raw, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return saw_frame ? 0 : 1;
			snprintf(err, errlen, "recv(netlink): %s", strerror(errno));
			return -1;
		}
		saw_frame = 1;
		len = (int)n;
		for (nlh = (struct nlmsghdr *)raw; NLMSG_OK(nlh, (unsigned)len);
		     nlh = NLMSG_NEXT(nlh, len)) {
			if (nlh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = NLMSG_DATA(nlh);

				if (e->error) {
					errno = -e->error;
					snprintf(err, errlen, "kernel: %s",
						 strerror(errno));
					return -1;
				}
				saw_ack = 1;
				continue;
			}
			if (nlh->nlmsg_type == NLMSG_DONE)
				continue;
			if (nlh->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN)
				continue;
			if (reply)
				buf_add(reply, NLMSG_DATA(nlh),
					nlh->nlmsg_len - NLMSG_HDRLEN);
		}
		/* An ACK closes a request; a payload-only answer waits for the
		 * receive timeout, which is set on the socket. */
		if (saw_ack)
			return 0;
	}
}

static int genl_open(int timeout_ms, char *err, size_t errlen)
{
	struct sockaddr_nl addr;
	struct timeval tv;
	int fd;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (fd < 0) {
		snprintf(err, errlen, "socket(NETLINK_GENERIC): %s", strerror(errno));
		return -1;
	}
	memset(&addr, 0, sizeof addr);
	addr.nl_family = AF_NETLINK;
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		snprintf(err, errlen, "bind(netlink): %s", strerror(errno));
		close(fd);
		return -1;
	}
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	return fd;
}

static void put_headers(struct buf *msg, unsigned type, unsigned flags,
			unsigned cmd, uint8_t version)
{
	struct buf body = { 0 };
	struct nlmsghdr nlh;
	struct genlmsghdr genl;

	memset(&nlh, 0, sizeof nlh);
	memset(&genl, 0, sizeof genl);
	genl.cmd = (uint8_t)cmd;
	genl.version = version;
	buf_add(&body, &genl, sizeof genl);
	buf_add(&body, msg->data, msg->len);

	nlh.nlmsg_len = (uint32_t)(NLMSG_HDRLEN + body.len);
	nlh.nlmsg_type = (uint16_t)type;
	nlh.nlmsg_flags = (uint16_t)flags;
	nlh.nlmsg_seq = ++g_seq;
	nlh.nlmsg_pid = 0;

	buf_free(msg);
	buf_add(msg, &nlh, sizeof nlh);
	buf_add(msg, body.data, body.len);
	buf_free(&body);
}

/* One SET_REG frame, byte-for-byte what CRDA's reglib/nl80211.c sent: the
 * alpha2, the DFS region and a nested array of rules, each nested rule indexed
 * from 1 and carrying the six attributes cfg80211 validates. */
static void build_set_reg(struct buf *msg, const struct regd_domain *dom,
			  unsigned family, unsigned cmd, uint8_t version,
			  uint32_t fixed_seq)
{
	struct buf attrs = { 0 };
	size_t rules;
	unsigned i;

	nla_add(&attrs, NL80211_ATTR_REG_ALPHA2, dom->alpha2, 2);
	nla_u8(&attrs, NL80211_ATTR_DFS_REGION, (uint8_t)dom->dfs_region);
	rules = nla_begin(&attrs, NL80211_ATTR_REG_RULES);
	for (i = 0; i < dom->n_rules; i++) {
		size_t one = nla_begin(&attrs, i + 1);

		nla_u32(&attrs, NL80211_ATTR_REG_RULE_FLAGS, dom->rule[i].kernel_flags);
		nla_u32(&attrs, NL80211_ATTR_FREQ_RANGE_START, dom->rule[i].start_khz);
		nla_u32(&attrs, NL80211_ATTR_FREQ_RANGE_END, dom->rule[i].end_khz);
		nla_u32(&attrs, NL80211_ATTR_FREQ_RANGE_MAX_BW, dom->rule[i].max_bw_khz);
		/* db2fw asserts max_ant_gain == 0: the database never carries one, and
		 * cfg80211 requires the attribute to be present. */
		nla_u32(&attrs, NL80211_ATTR_POWER_RULE_MAX_ANT_GAIN, 0);
		nla_u32(&attrs, NL80211_ATTR_POWER_RULE_MAX_EIRP, dom->rule[i].eirp_cbm);
		nla_end(&attrs, one);
	}
	nla_end(&attrs, rules);

	buf_free(msg);
	buf_add(msg, attrs.data, attrs.len);
	buf_free(&attrs);
	put_headers(msg, family, NLM_F_REQUEST | NLM_F_ACK, cmd, version);
	if (fixed_seq)
		((struct nlmsghdr *)msg->data)->nlmsg_seq = fixed_seq;
}

static int resolve_family(int fd, const char *name, unsigned *family,
			  uint8_t *version, char *err, size_t errlen)
{
	struct buf msg = { 0 };
	struct buf reply = { 0 };
	uint8_t *p;
	int rem;
	int rc;
	int found = 0;

	nla_add(&msg, CTRL_ATTR_FAMILY_NAME, name, strlen(name) + 1);
	put_headers(&msg, GENL_ID_CTRL, NLM_F_REQUEST, CTRL_CMD_GETFAMILY, 1);
	rc = nl_talk(fd, &msg, &reply, err, errlen);
	if (rc != 0) {
		if (rc > 0)
			snprintf(err, errlen, "no answer to CTRL_CMD_GETFAMILY");
		buf_free(&msg);
		buf_free(&reply);
		return -1;
	}
	if (reply.len < GENL_HDRLEN) {
		snprintf(err, errlen, "short CTRL_CMD_GETFAMILY reply");
		buf_free(&msg);
		buf_free(&reply);
		return -1;
	}
	p = reply.data + GENL_HDRLEN;
	rem = (int)(reply.len - GENL_HDRLEN);
	for (; nla_ok_((struct nlattr *)p, rem); p = (uint8_t *)nla_next_((struct nlattr *)p, &rem)) {
		struct nlattr *a = (struct nlattr *)p;
		unsigned type = a->nla_type & NLA_TYPE_MASK;

		if (type == CTRL_ATTR_FAMILY_ID && a->nla_len >= NLA_HDRLEN + 2) {
			*family = *(uint16_t *)nla_data_(a);
			found |= 1;
		} else if (type == CTRL_ATTR_VERSION && a->nla_len >= NLA_HDRLEN + 4) {
			*version = (uint8_t)(*(uint32_t *)nla_data_(a));
			found |= 2;
		}
	}
	buf_free(&msg);
	buf_free(&reply);
	if (!found) {
		snprintf(err, errlen, "no family id in the nl80211 answer");
		return -1;
	}
	if (!(found & 1))
		*version = 1;
	return 0;
}

/* Read the regdomain back.  cfg80211 reports its own view, which is the only
 * evidence that matters: the rules we just sent are intersected with what the
 * driver claims, so "the file was accepted" is not "the radio moved". */
static int read_regdomain(int fd, unsigned family, uint8_t version,
			  struct regd_domain *dom, char *err, size_t errlen)
{
	struct buf msg = { 0 };
	struct buf reply = { 0 };
	uint8_t *p;
	int rem;
	int rc;

	memset(dom, 0, sizeof *dom);
	put_headers(&msg, family, NLM_F_REQUEST, NL80211_CMD_GET_REG, version);
	rc = nl_talk(fd, &msg, &reply, err, errlen);
	if (rc != 0) {
		if (rc > 0)
			snprintf(err, errlen, "no answer to NL80211_CMD_GET_REG");
		buf_free(&msg);
		buf_free(&reply);
		return -1;
	}
	if (reply.len < GENL_HDRLEN) {
		snprintf(err, errlen, "short GET_REG reply");
		buf_free(&msg);
		buf_free(&reply);
		return -1;
	}
	p = reply.data + GENL_HDRLEN;
	rem = (int)(reply.len - GENL_HDRLEN);
	for (; nla_ok_((struct nlattr *)p, rem); p = (uint8_t *)nla_next_((struct nlattr *)p, &rem)) {
		struct nlattr *a = (struct nlattr *)p;
		unsigned type = a->nla_type & NLA_TYPE_MASK;

		if (type == NL80211_ATTR_REG_ALPHA2) {
			size_t n = a->nla_len - NLA_HDRLEN;

			if (n >= 2) {
				memcpy(dom->alpha2, nla_data_(a), 2);
				dom->alpha2[2] = '\0';
			}
		} else if (type == NL80211_ATTR_DFS_REGION &&
			   a->nla_len >= NLA_HDRLEN + 1) {
			dom->dfs_region = *(uint8_t *)nla_data_(a);
		} else if (type == NL80211_ATTR_REG_RULES) {
			uint8_t *rp = nla_data_(a);
			int rrem = (int)(a->nla_len - NLA_HDRLEN);

			for (; nla_ok_((struct nlattr *)rp, rrem);
			     rp = (uint8_t *)nla_next_((struct nlattr *)rp, &rrem)) {
				struct nlattr *one = (struct nlattr *)rp;
				uint8_t *fp = nla_data_(one);
				int frem = (int)(one->nla_len - NLA_HDRLEN);
				struct regd_rule rule;

				if (dom->n_rules >= REGDB_MAX_RULES)
					break;
				memset(&rule, 0, sizeof rule);
				for (; nla_ok_((struct nlattr *)fp, frem);
				     fp = (uint8_t *)nla_next_((struct nlattr *)fp, &frem)) {
					struct nlattr *f = (struct nlattr *)fp;
					unsigned ftype = f->nla_type & NLA_TYPE_MASK;
					uint32_t *v;

					if (f->nla_len < NLA_HDRLEN + 4)
						continue;
					v = nla_data_(f);
					switch (ftype) {
					case NL80211_ATTR_REG_RULE_FLAGS:
						rule.kernel_flags = *v;
						break;
					case NL80211_ATTR_FREQ_RANGE_START:
						rule.start_khz = *v;
						break;
					case NL80211_ATTR_FREQ_RANGE_END:
						rule.end_khz = *v;
						break;
					case NL80211_ATTR_FREQ_RANGE_MAX_BW:
						rule.max_bw_khz = *v;
						break;
					case NL80211_ATTR_POWER_RULE_MAX_EIRP:
						rule.eirp_cbm = *v;
						break;
					default:
						break;
					}
				}
				dom->rule[dom->n_rules++] = rule;
			}
		}
	}
	buf_free(&msg);
	buf_free(&reply);
	return dom->alpha2[0] ? 0 : -1;
}

/* ------------------------------------------------------------- uevent -- */

static int uevent_open(void)
{
	struct sockaddr_nl addr;
	int fd;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof addr);
	addr.nl_family = AF_NETLINK;
	/* Group 1 is the kobject broadcast udev listens on; subscribing needs
	 * CAP_NET_ADMIN, which the daemon has when it runs as root. */
	addr.nl_groups = 1;
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* A kobject uevent is NUL separated KEY=VALUE strings; cfg80211 sends
 * "ACTION=change" ... "COUNTRY=HU".  Returns the alpha2 through *out. */
static int uevent_country(const char *payload, size_t len, char *out)
{
	size_t pos = 0;

	while (pos < len) {
		const char *line = payload + pos;
		size_t n = strnlen(line, len - pos);

		if (n > 8 && !strncmp(line, "COUNTRY=", 8)) {
			out[0] = line[8];
			out[1] = line[9] ? line[9] : '0';
			out[2] = '\0';
			return 1;
		}
		pos += n + 1;
	}
	return 0;
}

/* Wait up to timeout_ms for a regulatory uevent and report its country code. */
static int wait_uevent(int fd, int timeout_ms, char *alpha2, size_t a2len)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	char payload[2048];
	struct timespec start;
	int rc;

	if (fd < 0)
		return 0;
	if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
		memset(&start, 0, sizeof start);
	for (;;) {
		int waited;
		ssize_t n;

		rc = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 0);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc <= 0)
			return 0;
		n = recv(fd, payload, sizeof payload - 1, 0);
		if (n <= 0)
			return 0;
		payload[n] = '\0';
		if (g_verbose)
			errmsg("uevent: %s", payload);
		if (uevent_country(payload, (size_t)n, alpha2)) {
			alpha2[a2len - 1] = '\0';
			return 1;
		}
		/* unrelated hotplug event: keep waiting, but never forever */
		waited = timeout_ms;
		if (waited <= 0)
			return 0;
		timeout_ms = waited / 2;
	}
}

static int trigger_hint(int fd, unsigned family, uint8_t version,
			const char *alpha2, char *err, size_t errlen)
{
	struct buf msg = { 0 };
	struct buf reply = { 0 };
	int rc;

	nla_add(&msg, NL80211_ATTR_REG_ALPHA2, alpha2, 2);
	put_headers(&msg, family, NLM_F_REQUEST | NLM_F_ACK,
		    NL80211_CMD_REQ_SET_REG, version);
	rc = nl_talk(fd, &msg, &reply, err, errlen);
	buf_free(&msg);
	buf_free(&reply);
	return rc;
}

static int send_set_reg(int fd, unsigned family, uint8_t version,
			const struct regd_domain *dom, char *err, size_t errlen)
{
	struct buf msg = { 0 };
	struct buf reply = { 0 };
	int rc;

	build_set_reg(&msg, dom, family, NL80211_CMD_SET_REG, version, 0);
	rc = nl_talk(fd, &msg, &reply, err, errlen);
	buf_free(&msg);
	buf_free(&reply);
	if (rc > 0) {
		snprintf(err, errlen, "no answer to NL80211_CMD_SET_REG");
		return -1;
	}
	return rc;
}

/* --------------------------------------------------------------- modes -- */

static const char *env_country(void)
{
	const char *v = getenv("SCOURGE_REGDOM");

	if (v && strlen(v) >= 2 && v[0] && v[1])
		return v;
	return NULL;
}

/* The installer writes WIFI_COUNTRY=<alpha2> into /rw_data/wifi/config.conf;
 * honouring it keeps one knob for the whole package. */
static const char *config_country(void)
{
	static char code[4];
	FILE *f = fopen("/rw_data/wifi/config.conf", "r");
	char line[256];

	if (!f)
		return NULL;
	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, "WIFI_COUNTRY=", 13))
			continue;
		if (strlen(line + 13) < 2)
			break;
		code[0] = line[13];
		code[1] = line[14];
		code[2] = '\0';
		fclose(f);
		return code;
	}
	fclose(f);
	return NULL;
}

static const char *pick_db(const struct opts *o)
{
	size_t i;

	if (o->db)
		return o->db;
	for (i = 0; g_db_candidates[i]; i++) {
		if (access(g_db_candidates[i], R_OK) == 0)
			return g_db_candidates[i];
	}
	return g_db_candidates[0];
}

static const char *default_country(void)
{
	const char *c = env_country();

	if (c)
		return c;
	c = config_country();
	if (c)
		return c;
	return "HU";
}

static int valid_alpha2(const char *a)
{
	if (!a || strlen(a) != 2)
		return 0;
	return a[0] >= 'A' && a[0] <= 'Z' && a[1] >= 'A' && a[1] <= 'Z';
}

static int load_domain(const struct opts *o, const char *country,
		       struct regd_domain *dom, char *err, size_t errlen)
{
	uint8_t *db = NULL;
	size_t len = 0;
	int rc;

	if (!valid_alpha2(country)) {
		snprintf(err, errlen, "country code '%s' is not ISO-3166 alpha2",
			 country ? country : "");
		return EXIT_USAGE;
	}
	if (read_whole(pick_db(o), &db, &len, err, errlen) < 0)
		return EXIT_NODB;
	rc = regdb_lookup(db, len, country, dom, err, errlen);
	free(db);
	if (rc > 0)
		return EXIT_NOCOUNTRY;
	if (rc < 0)
		return EXIT_NODB;
	if (dom->unknown_flags)
		errmsg("warning: %s carries rule flags this build does not map "
		       "(0x%x); they are dropped", country, dom->unknown_flags);
	return EXIT_OK;
}

/* Test hook: hand a synthetic uevent payload to the real parser, with '|' in
 * place of the NUL separators (argv cannot carry NUL bytes). */
static int cmd_parse_uevent(const char *arg)
{
	char payload[1024];
	char country[4];
	size_t i;
	size_t n;

	n = strlen(arg);
	if (n >= sizeof payload) {
		errmsg("uevent payload too long");
		return EXIT_USAGE;
	}
	for (i = 0; i < n; i++)
		payload[i] = arg[i] == '|' ? '\0' : arg[i];
	payload[n] = '\0';
	if (!uevent_country(payload, n, country)) {
		errmsg("no COUNTRY= in that uevent");
		return EXIT_USAGE;
	}
	printf("%s\n", country);
	return EXIT_OK;
}

static int cmd_dump(const struct opts *o, int as_message)
{
	struct regd_domain dom;
	char err[256] = "";
	const char *country = o->country ? o->country : default_country();
	int rc;

	rc = load_domain(o, country, &dom, err, sizeof err);
	if (rc != EXIT_OK) {
		errmsg("%s", err);
		return rc;
	}
	if (!as_message) {
		print_domain(&dom);
		return EXIT_OK;
	}

	if (o->family_id > 0) {
		struct buf msg = { 0 };
		size_t i;

		build_set_reg(&msg, &dom, (unsigned)o->family_id,
			      NL80211_CMD_SET_REG, 1, 1);
		for (i = 0; i < msg.len; i++)
			printf("%02x%s", msg.data[i], (i % 32 == 31) ? "\n" : "");
		if (msg.len % 32)
			putchar('\n');
		buf_free(&msg);
		return EXIT_OK;
	}

	{
		int fd = genl_open(o->timeout_ms, err, sizeof err);
		unsigned family = 0;
		uint8_t version = 1;
		struct buf msg = { 0 };
		size_t i;

		if (fd < 0) {
			errmsg("%s (pass --family-id to dump without a socket)", err);
			return EXIT_NETLINK;
		}
		if (resolve_family(fd, "nl80211", &family, &version, err, sizeof err) < 0) {
			errmsg("%s", err);
			close(fd);
			return EXIT_NETLINK;
		}
		close(fd);
		build_set_reg(&msg, &dom, family, NL80211_CMD_SET_REG, version, 1);
		for (i = 0; i < msg.len; i++)
			printf("%02x%s", msg.data[i], (i % 32 == 31) ? "\n" : "");
		if (msg.len % 32)
			putchar('\n');
		buf_free(&msg);
	}
	return EXIT_OK;
}

static void write_status(const struct opts *o, const struct regd_domain *req,
			 const struct regd_domain *eff, int set_ok,
			 const char *detail)
{
	FILE *f;
	time_t now;

	if (!o->status_file)
		return;
	f = fopen(o->status_file, "w");
	if (!f) {
		errmsg("cannot write %s: %s", o->status_file, strerror(errno));
		return;
	}
	now = time(NULL);
	fprintf(f, "{\"ts\":%ld,\"requested\":\"%s\",\"effective\":\"%s\","
		"\"dfs_region\":%u,\"requested_rules\":%u,\"effective_rules\":%u,"
		"\"set_reg_ok\":%s,\"applied\":%s,\"detail\":\"%s\","
		"\"db\":\"%s\"}\n",
		(long)now, req->alpha2, eff->alpha2, req->dfs_region,
		req->n_rules, eff->n_rules, set_ok ? "true" : "false",
		(set_ok && !strcmp(req->alpha2, eff->alpha2)) ? "true" : "false",
		detail ? detail : "", pick_db(o));
	fclose(f);
}

/* Read-only: what cfg80211 thinks right now.  Needs no privileges, so the
 * diagnostics and tests can use it on any kernel. */
static int cmd_read(struct opts *o)
{
	struct regd_domain dom;
	char err[256] = "";
	unsigned family = 0;
	uint8_t version = 1;
	int fd;
	int rc;

	fd = genl_open(o->timeout_ms, err, sizeof err);
	if (fd < 0) {
		errmsg("%s", err);
		return EXIT_NETLINK;
	}
	if (o->family_id > 0) {
		family = (unsigned)o->family_id;
	} else if (resolve_family(fd, "nl80211", &family, &version, err,
				  sizeof err) < 0) {
		errmsg("%s", err);
		close(fd);
		return EXIT_NETLINK;
	}
	rc = read_regdomain(fd, family, version, &dom, err, sizeof err);
	close(fd);
	if (rc < 0) {
		errmsg("cannot read the regdomain: %s", err);
		return EXIT_NETLINK;
	}
	print_domain(&dom);
	return EXIT_OK;
}

static int cmd_once(struct opts *o)
{
	struct regd_domain dom;
	struct regd_domain eff;
	char err[256] = "";
	char seen[4] = "";
	char detail[160] = "";
	const char *country;
	int ufd;
	int fd;
	unsigned family = 0;
	uint8_t version = 1;
	int rc_set, rc_get, rc_all;

	/* Listen first: cfg80211 answers a hint by asking CRDA, and the answer has
	 * to be ready before the question is asked. */
	ufd = uevent_open();
	if (ufd < 0)
		errmsg("warning: no kobject uevent socket (%s); answering blind",
		       strerror(errno));

	fd = genl_open(o->timeout_ms, err, sizeof err);
	if (fd < 0) {
		errmsg("%s", err);
		if (ufd >= 0)
			close(ufd);
		return EXIT_NETLINK;
	}
	if (o->family_id > 0) {
		family = (unsigned)o->family_id;
	} else if (resolve_family(fd, "nl80211", &family, &version, err,
				  sizeof err) < 0) {
		errmsg("%s", err);
		close(fd);
		if (ufd >= 0)
			close(ufd);
		return EXIT_NETLINK;
	}

	country = o->country ? o->country : default_country();
	if (o->trigger) {
		if (trigger_hint(fd, family, version, country, err, sizeof err) < 0)
			logmsg("regulatory hint %s not queued (%s); trying SET_REG "
			       "directly", country, err);
		else
			logmsg("regulatory hint %s queued (cfg80211 now asks CRDA)",
			       country);
		if (wait_uevent(ufd, o->timeout_ms, seen, sizeof seen)) {
			logmsg("answered the regulatory uevent for %s", seen);
			if (strcmp(seen, country))
				errmsg("warning: cfg80211 asked for %s, this run "
				       "installs %s", seen, country);
		} else {
			logmsg("no regulatory uevent within %d ms; sending SET_REG "
			       "anyway", o->timeout_ms);
		}
	}
	if (ufd >= 0)
		close(ufd);

	rc_all = load_domain(o, country, &dom, err, sizeof err);
	if (rc_all != EXIT_OK) {
		errmsg("%s", err);
		close(fd);
		return rc_all;
	}
	if (g_verbose)
		print_domain(&dom);

	rc_set = send_set_reg(fd, family, version, &dom, err, sizeof err);
	if (rc_set < 0) {
		snprintf(detail, sizeof detail, "SET_REG: %s", err);
		errmsg("NL80211_CMD_SET_REG for %s rejected: %s", dom.alpha2, err);
	} else {
		logmsg("NL80211_CMD_SET_REG for %s accepted (%u rules, dfs_region %u)",
		       dom.alpha2, dom.n_rules, dom.dfs_region);
	}

	memset(&eff, 0, sizeof eff);
	err[0] = '\0';
	rc_get = read_regdomain(fd, family, version, &eff, err, sizeof err);
	close(fd);
	if (rc_get < 0) {
		errmsg("cannot read the regdomain back: %s", err);
		write_status(o, &dom, &eff, rc_set == 0, "readback failed");
		return rc_set < 0 ? EXIT_SETREG : EXIT_NETLINK;
	}
	logmsg("cfg80211 now reports country %s with %u rules (dfs_region %u)",
	       eff.alpha2, eff.n_rules, eff.dfs_region);
	if (g_verbose)
		print_domain(&eff);

	if (rc_set < 0) {
		write_status(o, &dom, &eff, 0, detail);
		return EXIT_SETREG;
	}
	if (strcmp(dom.alpha2, eff.alpha2)) {
		snprintf(detail, sizeof detail,
			 "cfg80211 kept %s: the driver manages its own regdomain",
			 eff.alpha2);
		errmsg("%s; CRDA cannot override that - the vendor country lever "
		       "(wl -i wlan0 country %s) is the only remaining knob",
		       detail, dom.alpha2);
		write_status(o, &dom, &eff, 1, detail);
		return EXIT_NOTAPPLIED;
	}
	write_status(o, &dom, &eff, 1, "ok");
	return EXIT_OK;
}

static int cmd_listen(struct opts *o)
{
	char err[256] = "";
	struct regd_domain dom;
	struct regd_domain eff;
	char seen[4];
	int ufd;
	int fd;
	unsigned family = 0;
	uint8_t version = 1;

	ufd = uevent_open();
	if (ufd < 0) {
		errmsg("cannot subscribe to kobject uevents: %s", strerror(errno));
		return EXIT_NETLINK;
	}
	fd = genl_open(o->timeout_ms, err, sizeof err);
	if (fd < 0) {
		errmsg("%s", err);
		close(ufd);
		return EXIT_NETLINK;
	}
	if (o->family_id > 0) {
		family = (unsigned)o->family_id;
	} else if (resolve_family(fd, "nl80211", &family, &version, err,
				  sizeof err) < 0) {
		errmsg("%s", err);
		close(fd);
		close(ufd);
		return EXIT_NETLINK;
	}
	logmsg("listening for cfg80211 regulatory uevents (nl80211 family %u)",
	       family);
	if (o->trigger) {
		const char *country = o->country ? o->country : default_country();

		if (trigger_hint(fd, family, version, country, err, sizeof err) < 0)
			errmsg("hint %s not queued: %s", country, err);
	}

	while (!g_stop) {
		if (!wait_uevent(ufd, 1000, seen, sizeof seen))
			continue;
		logmsg("regulatory uevent for %s", seen);
		memset(&err, 0, sizeof err);
		if (load_domain(o, seen, &dom, err, sizeof err) != EXIT_OK) {
			errmsg("%s", err);
			continue;
		}
		if (send_set_reg(fd, family, version, &dom, err, sizeof err) < 0) {
			errmsg("SET_REG for %s rejected: %s", seen, err);
			continue;
		}
		memset(&eff, 0, sizeof eff);
		if (read_regdomain(fd, family, version, &eff, err, sizeof err) == 0)
			logmsg("installed %s: cfg80211 reports %s with %u rules",
			       seen, eff.alpha2, eff.n_rules);
	}
	close(fd);
	close(ufd);
	return EXIT_OK;
}

static void usage(void)
{
	fprintf(stderr,
		"regd - install a regulatory domain on kernel 4.1 (CRDA stand-in)\n"
		"\n"
		"  regd --once [--country XX] [--db PATH] [--no-trigger]\n"
		"        [--status-file PATH] [--timeout MS] [--family-id N] [-v]\n"
		"      Trigger a regulatory hint, answer it with SET_REG and verify.\n"
		"  regd --listen [--country XX] [--db PATH] [--no-trigger]\n"
		"        [--timeout MS] [--family-id N] [-v]\n"
		"      Stay resident and answer every cfg80211 regulatory uevent.\n"
		"  regd --dump [--country XX] [--db PATH]\n"
		"      Print the rules the database holds for a country.\n"
		"  regd --dump-message [--country XX] [--db PATH] [--family-id N]\n"
		"      Print the NL80211_CMD_SET_REG frame as hex (no privileges).\n"
		"  regd --read [--family-id N]\n"
		"      Print the regdomain cfg80211 reports right now (no privileges).\n"
		"\n"
		"Exit: 0 applied, 1 usage, 2 database unusable, 3 country unknown,\n"
		"      4 netlink failure, 5 SET_REG rejected, 6 cfg80211 kept another\n"
		"      country (driver manages its own regdomain).\n"
		"Default country: --country, else SCOURGE_REGDOM, else WIFI_COUNTRY\n"
		"from /rw_data/wifi/config.conf, else HU.\n"
		"Database: --db, else /rw_data/wifi/regulatory.db, "
		"/lib/firmware/regulatory.db, /usr/lib/firmware/regulatory.db.\n");
}

int main(int argc, char **argv)
{
	struct opts o;
	struct sigaction sa;
	int i;

	memset(&o, 0, sizeof o);
	o.timeout_ms = 3000;
	o.trigger = 1;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "--once") || !strcmp(a, "--listen") ||
		    !strcmp(a, "--read") || !strcmp(a, "--dump") ||
		    !strcmp(a, "--dump-message")) {
			o.mode = a + 2;
		} else if (!strcmp(a, "--parse-uevent") && i + 1 < argc) {
			o.mode = "parse-uevent";
			o.country = argv[++i];
		} else if (!strcmp(a, "--db") && i + 1 < argc) {
			o.db = argv[++i];
		} else if (!strcmp(a, "--country") && i + 1 < argc) {
			o.country = argv[++i];
		} else if (!strcmp(a, "--status-file") && i + 1 < argc) {
			o.status_file = argv[++i];
		} else if (!strcmp(a, "--family-id") && i + 1 < argc) {
			o.family_id = atoi(argv[++i]);
		} else if (!strcmp(a, "--timeout") && i + 1 < argc) {
			o.timeout_ms = atoi(argv[++i]);
		} else if (!strcmp(a, "--trigger")) {
			o.trigger = 1;
		} else if (!strcmp(a, "--no-trigger")) {
			o.trigger = 0;
		} else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
			g_verbose = 1;
		} else if (!strcmp(a, "--quiet")) {
			o.quiet = 1;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage();
			return EXIT_OK;
		} else {
			errmsg("unknown argument '%s'", a);
			usage();
			return EXIT_USAGE;
		}
	}
	if (!o.mode) {
		usage();
		return EXIT_USAGE;
	}

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (!strcmp(o.mode, "parse-uevent"))
		return cmd_parse_uevent(o.country);
	if (!strcmp(o.mode, "dump"))
		return cmd_dump(&o, 0);
	if (!strcmp(o.mode, "dump-message"))
		return cmd_dump(&o, 1);
	if (!strcmp(o.mode, "read"))
		return cmd_read(&o);
	if (!strcmp(o.mode, "once"))
		return cmd_once(&o);
	return cmd_listen(&o);
}
