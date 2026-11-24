/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone tests for the TLS seam: provider contract checks, the handshake
 * state machine, the anti-flood and size bounds, and the establishment gate on
 * encrypt/decrypt.  The provider here is a stub, not a cipher: this file tests
 * sequencing, never security.
 */
#include <stdio.h>
#include <string.h>

#include "../src/aa_tls.h"

static int g_failures;

static void check(const char *name, bool ok)
{
    if (!ok) {
        g_failures++;
        fprintf(stderr, "FAIL %s\n", name);
    }
}

typedef struct {
    int handshake_calls;
    int establish_at; /* handshake call that reports ESTABLISHED; 0 = never */
    int take_len;     /* bytes reported by take() */
    bool handshake_error;
    bool feed_error;
    bool take_buffer_error;
    uint32_t fed_bytes;
    uint32_t written_bytes;
    uint32_t read_bytes;
} stub;

static aa_tls_status stub_reset(void *context)
{
    stub *state = context;
    state->handshake_calls = 0;
    state->fed_bytes = 0u;
    state->written_bytes = 0u;
    state->read_bytes = 0u;
    return AA_TLS_OK;
}

static aa_tls_status stub_handshake(void *context)
{
    stub *state = context;
    state->handshake_calls++;
    if (state->handshake_error)
        return AA_TLS_ERR_PROVIDER;
    if (state->establish_at != 0 && state->handshake_calls >= state->establish_at)
        return AA_TLS_ESTABLISHED;
    return AA_TLS_OK;
}

static aa_tls_status stub_feed(void *context, const uint8_t *data, size_t len)
{
    stub *state = context;
    (void)data;
    if (state->feed_error)
        return AA_TLS_ERR_PROVIDER;
    state->fed_bytes += (uint32_t)len;
    return AA_TLS_OK;
}

static aa_tls_status stub_take(void *context, uint8_t *out, size_t out_cap, size_t *out_len)
{
    stub *state = context;
    if (state->take_buffer_error)
        return AA_TLS_ERR_BUFFER;
    const size_t length = (size_t)state->take_len > out_cap ? out_cap : (size_t)state->take_len;
    for (size_t i = 0u; i < length; i++)
        out[i] = 0x5Au;
    *out_len = length;
    return AA_TLS_OK;
}

static aa_tls_status stub_write(void *context, const uint8_t *data, size_t len)
{
    stub *state = context;
    (void)data;
    state->written_bytes += (uint32_t)len;
    return AA_TLS_OK;
}

static aa_tls_status stub_read(void *context, uint8_t *out, size_t out_cap, size_t *out_len)
{
    stub *state = context;
    const size_t length = out_cap < 2u ? out_cap : 2u;
    for (size_t i = 0u; i < length; i++)
        out[i] = 0x24u;
    *out_len = length;
    state->read_bytes += (uint32_t)length;
    return AA_TLS_OK;
}

static aa_tls_provider make_provider(stub *state)
{
    aa_tls_provider provider;
    provider.context = state;
    provider.reset = stub_reset;
    provider.handshake = stub_handshake;
    provider.feed = stub_feed;
    provider.take = stub_take;
    provider.write = stub_write;
    provider.read = stub_read;
    return provider;
}

static void test_establish_immediately(void)
{
    stub state;
    memset(&state, 0, sizeof state);
    state.establish_at = 1;
    state.take_len = 4;
    aa_tls_provider provider = make_provider(&state);

    aa_tls tls;
    aa_tls_init(&tls);
    uint8_t out[64];
    size_t out_len = 0u;
    check("immediate established", aa_tls_begin(&tls, &provider, out, sizeof out, &out_len) == AA_TLS_OK);
    check("immediate state", aa_tls_is_established(&tls));
    check("immediate output", out_len == 4u && out[0] == 0x5Au);
    check("immediate handshake calls", state.handshake_calls == 1);
}

static void test_handshake_rounds(void)
{
    stub state;
    memset(&state, 0, sizeof state);
    state.establish_at = 2;
    aa_tls_provider provider = make_provider(&state);

    aa_tls tls;
    aa_tls_init(&tls);
    uint8_t out[64];
    size_t out_len = 0u;
    check("begin handshaking", aa_tls_begin(&tls, &provider, out, sizeof out, &out_len) == AA_TLS_OK);
    check("begin not established", !aa_tls_is_established(&tls) && tls.state == AA_TLS_HANDSHAKING);
    check("begin twice", aa_tls_begin(&tls, &provider, out, sizeof out, &out_len) == AA_TLS_ERR_STATE);
    check("encrypt before established",
          aa_tls_encrypt(&tls, (const uint8_t *)"x", 1u, out, sizeof out, &out_len) == AA_TLS_ERR_STATE);
    check("decrypt before established",
          aa_tls_decrypt(&tls, (const uint8_t *)"x", 1u, out, sizeof out, &out_len) == AA_TLS_ERR_STATE);

    check("handshake step",
          aa_tls_on_handshake(&tls, (const uint8_t *)"peer", 4u, out, sizeof out, &out_len) ==
              AA_TLS_OK);
    check("handshake established", aa_tls_is_established(&tls));

    /* After establishment both directions work and are counted by size. */
    check("encrypt after established",
          aa_tls_encrypt(&tls, (const uint8_t *)"abc", 3u, out, sizeof out, &out_len) == AA_TLS_OK);
    check("encrypt counted", state.written_bytes == 3u);
    check("decrypt after established",
          aa_tls_decrypt(&tls, (const uint8_t *)"abc", 3u, out, sizeof out, &out_len) == AA_TLS_OK);
    check("decrypt length", out_len == 2u && out[0] == 0x24u);
    check("oversize plaintext",
          aa_tls_encrypt(&tls, out, AA_TLS_MAX_PLAINTEXT + 1u, out, sizeof out, &out_len) ==
              AA_TLS_ERR_OVERSIZE);
}

static void test_bounds_and_errors(void)
{
    stub state;
    memset(&state, 0, sizeof state);
    aa_tls_provider provider = make_provider(&state);

    /* Missing provider ops are refused outright. */
    aa_tls_provider broken = provider;
    broken.read = NULL;
    aa_tls tls;
    aa_tls_init(&tls);
    uint8_t out[64];
    size_t out_len = 0u;
    check("missing op", aa_tls_begin(&tls, &broken, out, sizeof out, &out_len) == AA_TLS_ERR_ARGUMENT);

    /* A provider error during the handshake is fatal. */
    memset(&state, 0, sizeof state);
    state.handshake_error = true;
    provider = make_provider(&state);
    aa_tls_init(&tls);
    check("handshake error",
          aa_tls_begin(&tls, &provider, out, sizeof out, &out_len) == AA_TLS_ERR_PROVIDER &&
              tls.state == AA_TLS_FAILED);

    /* A take() that cannot fit the pending output is reported as a buffer
     * error and fails the session (the transport would retry with more room,
     * which this seam deliberately does not guess). */
    memset(&state, 0, sizeof state);
    state.take_buffer_error = true;
    provider = make_provider(&state);
    aa_tls_init(&tls);
    check("take buffer error",
          aa_tls_begin(&tls, &provider, out, sizeof out, &out_len) == AA_TLS_ERR_BUFFER &&
              tls.state == AA_TLS_FAILED);

    /* Handshake body bound. */
    memset(&state, 0, sizeof state);
    state.establish_at = 100;
    provider = make_provider(&state);
    aa_tls_init(&tls);
    check("oversize begin", aa_tls_begin(&tls, &provider, out, sizeof out, &out_len) == AA_TLS_OK);
    uint8_t body[AA_TLS_MAX_HANDSHAKE_BLOB + 1u];
    memset(body, 0x41, sizeof body);
    check("oversize handshake body",
          aa_tls_on_handshake(&tls, body, sizeof body, out, sizeof out, &out_len) ==
              AA_TLS_ERR_OVERSIZE &&
              tls.state == AA_TLS_FAILED);

    /* Round bound: a peer may not send handshake messages forever. */
    memset(&state, 0, sizeof state);
    state.establish_at = 1000;
    provider = make_provider(&state);
    aa_tls_init(&tls);
    check("flood begin", aa_tls_begin(&tls, &provider, out, sizeof out, &out_len) == AA_TLS_OK);
    aa_tls_status status = AA_TLS_OK;
    for (uint32_t round = 0u; round <= AA_TLS_MAX_ROUNDS; round++) {
        status = aa_tls_on_handshake(&tls, (const uint8_t *)"x", 1u, out, sizeof out, &out_len);
        if (status != AA_TLS_OK)
            break;
    }
    check("flood rounds", status == AA_TLS_ERR_ROUNDS && tls.state == AA_TLS_FAILED);

    check("state names", strcmp(aa_tls_state_name(AA_TLS_ESTABLISHED_STATE), "established") == 0);
    check("status names", strcmp(aa_tls_status_name(AA_TLS_ERR_ROUNDS), "rounds") == 0);
}

int main(void)
{
    test_establish_immediately();
    test_handshake_rounds();
    test_bounds_and_errors();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("aa_tls: all checks passed\n");
    return 0;
}
