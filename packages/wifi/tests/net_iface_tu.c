/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * net_iface.inc must compile strictly and work on a real interface.  Loopback is
 * the only interface a test may touch, so the setters are exercised for their
 * error paths only: a test must never change the host's addresses.
 */
/* Feature macro first, before any system header: the include needs struct
 * ifreq/IFNAMSIZ, which glibc only exposes behind the misc set (see the header
 * comment in net_iface.inc). */
#define _DEFAULT_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../common/net_iface.inc"

int main(void)
{
	uint8_t mac[6] = { 0 };
	uint32_t ip = 0xdeadbeefu;
	uint32_t parsed;
	char text[32];
	int ok = 0;

	assert(neti_ifindex("lo") > 0);
	assert(neti_ifindex("") < 0);
	assert(neti_ifindex("0123456789abcdefgh") < 0);   /* longer than IFNAMSIZ */
	assert(neti_mac("lo", mac) == 0);
	assert(neti_ipv4("lo", &ip) == 0);
	assert((ip >> 24) == 127u);   /* loopback: 127.0.0.1 */
	assert(neti_ipv4("nosuch0", &ip) < 0);

	/* Names longer than IFNAMSIZ are refused, never truncated. */
	assert(neti_set_ipv4("0123456789abcdefgh", 1, 2) < 0);
	assert(neti_up("0123456789abcdefgh") < 0);
	assert(neti_default_route("0123456789abcdefgh", 1) < 0);

	parsed = neti_ipv4_parse("10.99.0.50", &ok);
	assert(ok == 1 && parsed == ((10u << 24) | (99u << 16) | 50u));
	assert(neti_ipv4_parse("10.99.0", &ok) == 0 && ok == 0);
	assert(neti_ipv4_parse(NULL, &ok) == 0 && ok == 0);

	neti_ipv4_str((10u << 24) | (42u << 16) | 1u, text, sizeof text);
	assert(!strcmp(text, "10.42.0.1"));
	neti_ipv4_str(0, text, sizeof text);
	assert(!strcmp(text, "0.0.0.0"));

	printf("PASS: net_iface.inc reads lo and refuses bad names/addresses\n");
	return 0;
}
