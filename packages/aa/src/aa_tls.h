/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * TLS seam of the AA core.
 *
 * This module owns the *sequencing* of the secure session and nothing else: it
 * drives a provider behind a memory-BIO style contract, bounds the handshake,
 * and gates encryption and decryption on an established session.  It contains
 * no cryptography, no key material and no library calls, so the transport
 * checkpoint can supply a provider backed by a real TLS library while the
 * tests supply a deterministic stand-in.
 *
 * Ordering fact (verified read-only against aasdk commit
 * 046b3b381595509d0939fa84b14a90978f46ff63): the head unit sends the version
 * request in the clear, the phone answers in the clear, and then the TLS
 * handshake records travel as control-channel `SSL_HANDSHAKE` (0x0003) message
 * bodies.  Once the session is up, every non-control channel message and the
 * discovery response onwards are encrypted; `AUTH_COMPLETE` is sent only after
 * the secure session exists.
 *
 * Provider contract.  All ops return an aa_tls_status and receive the
 * provider's own context:
 *   reset      start a fresh session (called once by aa_tls_begin)
 *   handshake  advance the handshake; AA_TLS_OK means "more data needed",
 *              AA_TLS_ESTABLISHED means the secure session is up
 *   feed       hand incoming ciphertext to the session
 *   take       drain pending outgoing ciphertext; AA_TLS_OK with *out_len == 0
 *              is legal, a full output buffer is AA_TLS_ERR_BUFFER so the
 *              caller can retry with a larger one
 *   write      queue plaintext that take() will hand out encrypted
 *   read       decrypt available ciphertext
 * A provider must never report AA_TLS_ESTABLISHED unless its own handshake
 * verification succeeded.  This module cannot and does not verify it.
 *
 * Local bounds, not wire facts: one handshake body is at most
 * AA_TLS_MAX_HANDSHAKE_BLOB, and a session survives at most
 * AA_TLS_MAX_ROUNDS peer handshake messages; both are anti-flood limits.
 *
 * Nothing here allocates, logs, reads configuration or performs I/O.
 */

#ifndef AA_TLS_H
#define AA_TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AA_TLS_MAX_HANDSHAKE_BLOB 3072u
#define AA_TLS_MAX_ROUNDS 16u
#define AA_TLS_MAX_PLAINTEXT 4096u

typedef enum {
    AA_TLS_OK = 0,
    AA_TLS_ESTABLISHED = 1,
    AA_TLS_ERR_ARGUMENT = -1,
    AA_TLS_ERR_STATE = -2,
    AA_TLS_ERR_PROVIDER = -3,
    AA_TLS_ERR_BUFFER = -4,
    AA_TLS_ERR_ROUNDS = -5,
    AA_TLS_ERR_OVERSIZE = -6
} aa_tls_status;

typedef struct aa_tls_provider {
    void *context;
    aa_tls_status (*reset)(void *context);
    aa_tls_status (*handshake)(void *context);
    aa_tls_status (*feed)(void *context, const uint8_t *data, size_t len);
    aa_tls_status (*take)(void *context, uint8_t *out, size_t out_cap, size_t *out_len);
    aa_tls_status (*write)(void *context, const uint8_t *data, size_t len);
    aa_tls_status (*read)(void *context, uint8_t *out, size_t out_cap, size_t *out_len);
} aa_tls_provider;

typedef enum {
    AA_TLS_IDLE = 0,
    AA_TLS_HANDSHAKING = 1,
    AA_TLS_ESTABLISHED_STATE = 2,
    AA_TLS_FAILED = 3
} aa_tls_state;

typedef struct {
    const aa_tls_provider *provider;
    aa_tls_state state;
    uint32_t rounds;
} aa_tls;

void aa_tls_init(aa_tls *tls);

/* Start the session and produce the first handshake body (possibly empty). */
aa_tls_status aa_tls_begin(aa_tls *tls, const aa_tls_provider *provider, uint8_t *out,
                           size_t out_cap, size_t *out_len);

/* Feed one peer handshake body and produce our next one (possibly empty). */
aa_tls_status aa_tls_on_handshake(aa_tls *tls, const uint8_t *in, size_t in_len, uint8_t *out,
                                  size_t out_cap, size_t *out_len);

/* Encrypt a plaintext channel payload for the wire. */
aa_tls_status aa_tls_encrypt(aa_tls *tls, const uint8_t *in, size_t in_len, uint8_t *out,
                             size_t out_cap, size_t *out_len);

/* Decrypt a received channel payload. */
aa_tls_status aa_tls_decrypt(aa_tls *tls, const uint8_t *in, size_t in_len, uint8_t *out,
                             size_t out_cap, size_t *out_len);

bool aa_tls_is_established(const aa_tls *tls);

const char *aa_tls_state_name(aa_tls_state state);
const char *aa_tls_status_name(aa_tls_status status);

#ifdef __cplusplus
}
#endif

#endif /* AA_TLS_H */
