/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * TLS sequencing implementation.  See aa_tls.h for the provider contract and
 * the bounds.  No cryptography lives here.
 */
#include "aa_tls.h"

static aa_tls_status provider_ok(aa_tls_status status)
{
    /* A provider may report progress (OK) or completion (ESTABLISHED); any
     * negative value is an error. */
    return status == AA_TLS_OK || status == AA_TLS_ESTABLISHED ? AA_TLS_OK : status;
}

static aa_tls_status drain(aa_tls *tls, uint8_t *out, size_t out_cap, size_t *out_len)
{
    *out_len = 0u;
    if (out == NULL || out_cap == 0u)
        return AA_TLS_OK;
    const aa_tls_status status = tls->provider->take(tls->provider->context, out, out_cap, out_len);
    if (status != AA_TLS_OK) {
        tls->state = AA_TLS_FAILED;
        return status == AA_TLS_ERR_BUFFER ? AA_TLS_ERR_BUFFER : AA_TLS_ERR_PROVIDER;
    }
    if (*out_len > out_cap) {
        tls->state = AA_TLS_FAILED;
        return AA_TLS_ERR_PROVIDER;
    }
    return AA_TLS_OK;
}

static aa_tls_status advance(aa_tls *tls, uint8_t *out, size_t out_cap, size_t *out_len)
{
    const aa_tls_status handshake = tls->provider->handshake(tls->provider->context);
    if (handshake != AA_TLS_OK && handshake != AA_TLS_ESTABLISHED) {
        tls->state = AA_TLS_FAILED;
        return AA_TLS_ERR_PROVIDER;
    }
    const aa_tls_status drained = drain(tls, out, out_cap, out_len);
    if (drained != AA_TLS_OK)
        return drained;
    if (handshake == AA_TLS_ESTABLISHED)
        tls->state = AA_TLS_ESTABLISHED_STATE;
    else
        tls->state = AA_TLS_HANDSHAKING;
    return AA_TLS_OK;
}

void aa_tls_init(aa_tls *tls)
{
    if (tls == NULL)
        return;
    tls->provider = NULL;
    tls->state = AA_TLS_IDLE;
    tls->rounds = 0u;
}

bool aa_tls_is_established(const aa_tls *tls)
{
    return tls != NULL && tls->state == AA_TLS_ESTABLISHED_STATE;
}

aa_tls_status aa_tls_begin(aa_tls *tls, const aa_tls_provider *provider, uint8_t *out,
                           size_t out_cap, size_t *out_len)
{
    if (tls == NULL || provider == NULL || out_len == NULL)
        return AA_TLS_ERR_ARGUMENT;
    if (provider->handshake == NULL || provider->feed == NULL || provider->take == NULL ||
        provider->write == NULL || provider->read == NULL)
        return AA_TLS_ERR_ARGUMENT;
    if (tls->state != AA_TLS_IDLE)
        return AA_TLS_ERR_STATE;

    *out_len = 0u;
    tls->provider = provider;
    tls->rounds = 0u;
    if (provider->reset != NULL) {
        const aa_tls_status reset = provider->reset(provider->context);
        if (provider_ok(reset) != AA_TLS_OK) {
            tls->state = AA_TLS_FAILED;
            return AA_TLS_ERR_PROVIDER;
        }
    }
    return advance(tls, out, out_cap, out_len);
}

aa_tls_status aa_tls_on_handshake(aa_tls *tls, const uint8_t *in, size_t in_len, uint8_t *out,
                                  size_t out_cap, size_t *out_len)
{
    if (tls == NULL || out_len == NULL)
        return AA_TLS_ERR_ARGUMENT;
    if (in == NULL && in_len != 0u)
        return AA_TLS_ERR_ARGUMENT;
    if (tls->state != AA_TLS_HANDSHAKING)
        return AA_TLS_ERR_STATE;
    if (in_len > AA_TLS_MAX_HANDSHAKE_BLOB) {
        tls->state = AA_TLS_FAILED;
        return AA_TLS_ERR_OVERSIZE;
    }
    if (tls->rounds >= AA_TLS_MAX_ROUNDS) {
        tls->state = AA_TLS_FAILED;
        return AA_TLS_ERR_ROUNDS;
    }
    tls->rounds++;

    *out_len = 0u;
    if (in_len != 0u) {
        const aa_tls_status fed = tls->provider->feed(tls->provider->context, in, in_len);
        if (provider_ok(fed) != AA_TLS_OK) {
            tls->state = AA_TLS_FAILED;
            return AA_TLS_ERR_PROVIDER;
        }
    }
    return advance(tls, out, out_cap, out_len);
}

aa_tls_status aa_tls_encrypt(aa_tls *tls, const uint8_t *in, size_t in_len, uint8_t *out,
                             size_t out_cap, size_t *out_len)
{
    if (tls == NULL || out_len == NULL)
        return AA_TLS_ERR_ARGUMENT;
    if (in == NULL && in_len != 0u)
        return AA_TLS_ERR_ARGUMENT;
    if (tls->state != AA_TLS_ESTABLISHED_STATE)
        return AA_TLS_ERR_STATE;
    if (in_len > AA_TLS_MAX_PLAINTEXT)
        return AA_TLS_ERR_OVERSIZE;

    *out_len = 0u;
    const aa_tls_status written = tls->provider->write(tls->provider->context, in, in_len);
    if (provider_ok(written) != AA_TLS_OK)
        return AA_TLS_ERR_PROVIDER;
    return drain(tls, out, out_cap, out_len);
}

aa_tls_status aa_tls_decrypt(aa_tls *tls, const uint8_t *in, size_t in_len, uint8_t *out,
                             size_t out_cap, size_t *out_len)
{
    if (tls == NULL || out_len == NULL)
        return AA_TLS_ERR_ARGUMENT;
    if (in == NULL && in_len != 0u)
        return AA_TLS_ERR_ARGUMENT;
    if (tls->state != AA_TLS_ESTABLISHED_STATE)
        return AA_TLS_ERR_STATE;

    *out_len = 0u;
    if (in_len != 0u) {
        const aa_tls_status fed = tls->provider->feed(tls->provider->context, in, in_len);
        if (provider_ok(fed) != AA_TLS_OK)
            return AA_TLS_ERR_PROVIDER;
    }
    const aa_tls_status read = tls->provider->read(tls->provider->context, out, out_cap, out_len);
    if (read != AA_TLS_OK)
        return read == AA_TLS_ERR_BUFFER ? AA_TLS_ERR_BUFFER : AA_TLS_ERR_PROVIDER;
    if (*out_len > out_cap)
        return AA_TLS_ERR_PROVIDER;
    return AA_TLS_OK;
}

const char *aa_tls_state_name(aa_tls_state state)
{
    switch (state) {
    case AA_TLS_IDLE:
        return "idle";
    case AA_TLS_HANDSHAKING:
        return "handshaking";
    case AA_TLS_ESTABLISHED_STATE:
        return "established";
    case AA_TLS_FAILED:
        return "failed";
    }
    return "unknown";
}

const char *aa_tls_status_name(aa_tls_status status)
{
    switch (status) {
    case AA_TLS_OK:
        return "ok";
    case AA_TLS_ESTABLISHED:
        return "established";
    case AA_TLS_ERR_ARGUMENT:
        return "argument";
    case AA_TLS_ERR_STATE:
        return "state";
    case AA_TLS_ERR_PROVIDER:
        return "provider";
    case AA_TLS_ERR_BUFFER:
        return "buffer";
    case AA_TLS_ERR_ROUNDS:
        return "rounds";
    case AA_TLS_ERR_OVERSIZE:
        return "oversize";
    }
    return "unknown";
}
