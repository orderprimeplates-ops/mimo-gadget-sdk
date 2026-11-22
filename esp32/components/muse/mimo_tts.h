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
 * MIMO spoken replies (CONFIG_MIMO_TTS): a reply's text goes to the MIMO
 * voice relay over its own HTTPS connection, on a task of its own, and the
 * MP3 it returns comes back through a bounded buffer that the Muse chat task
 * drains without waiting (muse_chat_session.cpp, "MIMO speech"). The chat
 * session's connection, buffers and timing are untouched; if anything fails
 * before audio arrives, the reply is shown as text, as without the relay.
 *
 * The relay's URL and the gadget's relay token live in NVS (namespace
 * "mimo_tts"), set over the USB console or authenticated BLE with the
 * mimo.tts.* setup commands (docs/mimo-tts.md). Neither is compiled in, and
 * the token is never logged or echoed: only its length and a short hash.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mimo_tts_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MIMO_TTS_STREAMING,   /* more may come: take again later */
    MIMO_TTS_DONE,        /* the whole MP3 has been taken */
    MIMO_TTS_FAILED,      /* no more is coming (mimo_tts_last_error() says why) */
} mimo_tts_state_t;

/* Loads the settings and starts the worker. Safe to call more than once. */
void mimo_tts_init(void);

/*
 * Asks the relay to say text[0..len). Returns the job's id, or 0 if it won't
 * (not configured, turned off, resting after errors, refused token, no
 * memory): then show the text as usual. Starting a job cancels the one before.
 * Never blocks on the network.
 */
uint32_t mimo_tts_start(const char *text, size_t len);

/*
 * Moves up to cap bytes of job's MP3 into dst without waiting; *got says how
 * many. A job that isn't the current one (cancelled, replaced) is FAILED.
 */
mimo_tts_state_t mimo_tts_take(uint32_t job, uint8_t *dst, size_t cap, size_t *got);

/* Stops the current job, if any. Its unread audio is dropped. */
void mimo_tts_cancel(void);

mimo_tts_err_t mimo_tts_last_error(void);

/* ---- Setup commands (muse_ble.c's run_command) ---- */

/* Each returns NULL when done, or a short error for the setup reply. A whole
 * token (at most MIMO_TTS_TOKEN_MAX) fits one 600-byte setup write. */
const char *mimo_tts_set_url(const char *url);
const char *mimo_tts_set_token(const char *token);
const char *mimo_tts_set_voice(const char *voice);
const char *mimo_tts_set_enabled(bool on);
/* Forgets the URL and token. */
void mimo_tts_clear(void);
/* "on r:set t:1a2b3c4d ok" (t: the token's SHA-256 prefix, never the token),
 * and the same at length in the log. "!" after it: token refused; "~": resting. */
void mimo_tts_describe(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
