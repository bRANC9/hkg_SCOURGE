/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 * Reuse synthetic identity/wire fixtures, not OEM material. */
#define main original_bootstrap_tests
#include "test_carplay_bootstrap.c"
#undef main
#include <assert.h>

static void async_identify(cp_bootstrap *s, uint64_t *now)
{
    uint8_t out[CP_WIRE_MAX_FRAME];
    cp_boot_config cfg = base_config();
    assert(cp_bootstrap_init(s, &cfg) == CP_BOOT_OK);
    assert(cp_bootstrap_set_async_auth(s, true) == CP_BOOT_OK);
    assert(feed(s, "404000061d00", ++*now, out, sizeof(out)).error == CP_BOOT_OK);
    assert(feed(s, "404000061d02", ++*now, out, sizeof(out)).event == CP_BOOT_EV_IDENTIFIED);
}

int main(void)
{
    cp_bootstrap s;
    cp_boot_auth_request req, same;
    uint8_t out[CP_WIRE_MAX_FRAME], blob[] = {1, 2, 3, 4};
    uint64_t now = 0;
    cp_boot_result result;
    assert(original_bootstrap_tests(1, NULL) == 0);
    async_identify(&s, &now);
    assert(feed(&s, k_aa00, ++now, out, sizeof(out)).event == CP_BOOT_EV_AUTH_PENDING);
    assert(cp_bootstrap_auth_request(&s, &req));
    assert(req.op == CP_BOOT_AUTH_OP_CERTIFICATE && req.challenge_len == 0);
    assert(feed(&s, k_aa00, ++now, out, sizeof(out)).event == CP_BOOT_EV_AUTH_PENDING);
    assert(cp_bootstrap_auth_request(&s, &same));
    assert(same.token == req.token && same.deadline_ms == req.deadline_ms);
    assert(cp_bootstrap_auth_complete(&s, req.generation, req.token + 1,
        req.op, true, blob, sizeof(blob), ++now, out, sizeof(out)).error == CP_BOOT_ERR_ORDER);
    assert(cp_bootstrap_auth_request(&s, &same) && same.token == req.token);
    assert(cp_bootstrap_auth_complete(&s, req.generation, req.token, req.op,
        true, blob, sizeof(blob), ++now, out, 0).error == CP_BOOT_ERR_BUFFER);
    assert(cp_bootstrap_auth_request(&s, &same));
    result = cp_bootstrap_auth_complete(&s, req.generation, req.token, req.op,
        true, blob, sizeof(blob), ++now, out, sizeof(out));
    assert(result.event == CP_BOOT_EV_AUTH_CERT_SENT && result.out_len > 0);
    assert(!cp_bootstrap_auth_request(&s, &same));
    assert(cp_bootstrap_auth_complete(&s, req.generation, req.token, req.op,
        true, blob, sizeof(blob), ++now, out, sizeof(out)).error == CP_BOOT_ERR_ORDER);
    assert(feed(&s, k_aa02, ++now, out, sizeof(out)).event == CP_BOOT_EV_AUTH_PENDING);
    assert(cp_bootstrap_auth_request(&s, &req));
    assert(req.op == CP_BOOT_AUTH_OP_SIGNATURE && req.challenge_len == 4);
    assert(!memcmp(req.challenge, blob, sizeof(blob)));
    assert(feed(&s, k_aa02, ++now, out, sizeof(out)).event == CP_BOOT_EV_AUTH_PENDING);
    result = cp_bootstrap_auth_complete(&s, req.generation, req.token, req.op,
        true, blob, sizeof(blob), ++now, out, sizeof(out));
    assert(result.event == CP_BOOT_EV_AUTH_RESPONSE_SENT);
    assert(s.auth_state != CP_BOOT_AUTH_SUCCEEDED);
    assert(feed(&s, "40400006aa05", ++now, out, sizeof(out)).event == CP_BOOT_EV_AUTHENTICATED);
    assert(!cp_bootstrap_projection_active(&s));

    async_identify(&s, &now);
    assert(feed(&s, k_aa00, ++now, out, sizeof(out)).event == CP_BOOT_EV_AUTH_PENDING);
    assert(cp_bootstrap_auth_request(&s, &req));
    assert(cp_bootstrap_tick(&s, req.deadline_ms, out, sizeof(out)).error == CP_BOOT_ERR_TIMEOUT);
    assert(!cp_bootstrap_auth_request(&s, &same));
    assert(cp_bootstrap_auth_complete(&s, req.generation, req.token, req.op,
        true, blob, sizeof(blob), req.deadline_ms, out, sizeof(out)).error == CP_BOOT_ERR_ORDER);
    now = req.deadline_ms;
    async_identify(&s, &now);
    assert(feed(&s, k_aa00, ++now, out, sizeof(out)).event == CP_BOOT_EV_AUTH_PENDING);
    assert(cp_bootstrap_auth_request(&s, &req));
    (void)cp_bootstrap_transport_reset(&s, ++now);
    assert(!cp_bootstrap_auth_request(&s, &same));
    assert(cp_bootstrap_auth_complete(&s, req.generation, req.token, req.op,
        true, blob, sizeof(blob), ++now, out, sizeof(out)).error == CP_BOOT_ERR_ORDER);
    async_identify(&s, &now);
    assert(feed(&s, k_aa00, ++now, out, sizeof(out)).event == CP_BOOT_EV_AUTH_PENDING);
    assert(cp_bootstrap_auth_request(&s, &req));
    assert(cp_bootstrap_auth_complete(&s, req.generation, req.token, req.op,
        false, NULL, 0, ++now, out, sizeof(out)).event == CP_BOOT_EV_AUTH_FAILED);
    assert(s.auth_state == CP_BOOT_AUTH_FAILED);
    puts("PASS: deferred authentication completion, tokens, timeout, reset and phone-only success");
    return 0;
}
