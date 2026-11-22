/*
 * Copyright (c) the MIMO project contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * MIMO spoken replies: the relay client's policy, with no ESP-IDF in it so
 * the host tests can run every rule (tests/test_mimo_tts.py).
 *
 * The gadget never talks to a TTS provider. It POSTs a reply's text to the
 * MIMO voice relay, which holds the provider's key and picks the voice, and
 * plays the MP3 it streams back (docs/mimo-tts.md). Everything here decides
 * what to send, what to accept, and when to give up and show the text instead.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Request text: a little under a caption's 1 KB, cut at a sentence or word. */
#define MIMO_TTS_TEXT_MAX 1000
/* The JSON body: the text escaped at worst six bytes a byte, plus the rest. */
#define MIMO_TTS_BODY_MAX (MIMO_TTS_TEXT_MAX * 6 + 128)
#define MIMO_TTS_URL_MAX 200
#define MIMO_TTS_TOKEN_MIN 32
#define MIMO_TTS_TOKEN_MAX 256
#define MIMO_TTS_VOICE_MAX 32

/* One reply's MP3, at most: about 64 s at 48 kbps. A larger stream is cut
 * there (what came plays); a larger Content-Length is refused up front. */
#define MIMO_TTS_MAX_BYTES (384 * 1024)

/* Deadlines, from the request's start. */
#define MIMO_TTS_FIRST_BYTE_MS 8000   /* connect, send, and the first audio byte */
#define MIMO_TTS_STALL_MS 5000        /* longest wait for more audio once it flows */
#define MIMO_TTS_TOTAL_MS 60000       /* the whole download */

/* Free RAM a request needs before it starts, and the floor that ends one:
 * mbedTLS's record buffers are in PSRAM on boards with the full UI, so HTTPS
 * adds socket state and a little scratch; Link's Noise session keeps the
 * 16 KB of DMA-capable RAM it waits for (NOISE_TX_DMA_RESERVE_BYTES). */
#define MIMO_TTS_NEED_INTERNAL (12 * 1024)
#define MIMO_TTS_NEED_DMA (16 * 1024 + 8 * 1024)
#define MIMO_TTS_FLOOR_INTERNAL (4 * 1024)

/* After the relay refuses: */
#define MIMO_TTS_RATE_BACKOFF_MS 60000     /* 429: wait this long */
#define MIMO_TTS_ERROR_BACKOFF_MS 15000    /* three failures in a row: rest */
#define MIMO_TTS_ERRORS_BEFORE_REST 3

typedef enum {
    MIMO_TTS_OK = 0,
    MIMO_TTS_ERR_CONFIG,      /* no relay URL or token, or speech turned off */
    MIMO_TTS_ERR_AUTH,        /* 401/403: the token is wrong or revoked */
    MIMO_TTS_ERR_RATE,        /* 429 */
    MIMO_TTS_ERR_STATUS,      /* any other non-200 */
    MIMO_TTS_ERR_TYPE,        /* not audio/mpeg */
    MIMO_TTS_ERR_TOO_LARGE,   /* Content-Length over the cap, or the stream ran past it */
    MIMO_TTS_ERR_TIMEOUT,
    MIMO_TTS_ERR_NET,
    MIMO_TTS_ERR_MEMORY,
    MIMO_TTS_ERR_CANCELLED,
    MIMO_TTS_ERR_COUNT,
} mimo_tts_err_t;

const char *mimo_tts_err_name(mimo_tts_err_t err);

/* ---- Credentials (checked before they're saved, never logged) ---- */

/* https:// only, a host, printable ASCII, no spaces, at most MIMO_TTS_URL_MAX. */
bool mimo_tts_url_ok(const char *url);
/* MIMO_TTS_TOKEN_MIN..MAX characters of [A-Za-z0-9._~+/=-]: what a random
 * token in base64url, base64 or hex is made of, and nothing a header could
 * be split with. */
bool mimo_tts_token_ok(const char *token);
/* A voice profile name the relay maps to a provider voice: [a-z0-9_-]. */
bool mimo_tts_voice_ok(const char *voice);

/* ---- The request ---- */

/* How much of text[0..len) to speak: all of it if it fits MIMO_TTS_TEXT_MAX,
 * else up to the last sentence end, else word break, that fits, never
 * splitting a UTF-8 character. */
size_t mimo_tts_clip(const char *text, size_t len);

/* {"v":1,"voice":"...","format":"mp3","text":"..."} into out. The text is
 * clipped (above) and escaped; control characters become spaces. Returns the
 * length, or 0 if it doesn't fit cap (MIMO_TTS_BODY_MAX always does). */
size_t mimo_tts_body(char *out, size_t cap, const char *text, size_t len, const char *voice);

/* ---- The response ---- */

/* Status, Content-Type (may be NULL) and Content-Length (-1 when chunked). */
mimo_tts_err_t mimo_tts_check_headers(int status, const char *content_type, int64_t content_length);

/* One download's clock and byte count. */
typedef struct {
    int64_t start_us;
    int64_t last_us;      /* the last audio byte, or the start */
    size_t bytes;
    bool flowing;         /* the first audio byte came */
} mimo_tts_job_t;

void mimo_tts_job_begin(mimo_tts_job_t *job, int64_t now_us);
/* n more bytes arrived. Sets *take to how many of them fit the cap; returns
 * MIMO_TTS_ERR_TOO_LARGE once the cap is reached (take what fits, then stop). */
mimo_tts_err_t mimo_tts_job_data(mimo_tts_job_t *job, size_t n, int64_t now_us, size_t *take);
/* MIMO_TTS_ERR_TIMEOUT once a deadline has passed. */
mimo_tts_err_t mimo_tts_job_check(const mimo_tts_job_t *job, int64_t now_us);
/* How long the next socket operation may wait: the nearest deadline, at most
 * cap_ms and at least 1. */
int mimo_tts_job_wait_ms(const mimo_tts_job_t *job, int64_t now_us, int cap_ms);
/* Time spent waiting for room in the player doesn't count as a stall. */
void mimo_tts_job_waited(mimo_tts_job_t *job, int64_t waited_us);

/* ---- Memory ---- */

bool mimo_tts_admit(size_t internal_free, size_t dma_free);
bool mimo_tts_above_floor(size_t internal_free);

/* ---- When to ask again ---- */

typedef struct {
    bool auth_failed;       /* until the token changes */
    int64_t rest_until_us;  /* 0: no rest */
    int errors;             /* failures in a row */
} mimo_tts_gate_t;

/* May a request start now? */
bool mimo_tts_gate_open(const mimo_tts_gate_t *gate, int64_t now_us);
/* A request ended with err (MIMO_TTS_OK for success). Cancellations, the
 * gadget's own memory and config refusals don't count against the relay. */
void mimo_tts_gate_result(mimo_tts_gate_t *gate, mimo_tts_err_t err, int64_t now_us);
/* A new token (or URL) was saved: try again at once. */
void mimo_tts_gate_reset(mimo_tts_gate_t *gate);

#ifdef __cplusplus
}
#endif
