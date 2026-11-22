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

/* The MIMO relay client's rules (mimo_tts_core.c), one case per argv[1]. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mimo_tts_core.h"

#define MS 1000LL
#define T0 1000000LL

static void credentials(void)
{
    assert(mimo_tts_url_ok("https://relay.example.com/v1/speak"));
    assert(mimo_tts_url_ok("https://relay.example.com"));
    assert(!mimo_tts_url_ok("http://relay.example.com/v1/speak"));   /* never plain HTTP */
    assert(!mimo_tts_url_ok("https://"));
    assert(!mimo_tts_url_ok("https:///path"));
    assert(!mimo_tts_url_ok("https://user:pass@relay.example.com/"));   /* no credentials in the URL */
    assert(!mimo_tts_url_ok("https://relay.example.com/a b"));
    assert(!mimo_tts_url_ok("https://relay.example.com/\r\nX-Evil: 1"));
    assert(!mimo_tts_url_ok(NULL));
    char longurl[MIMO_TTS_URL_MAX + 2];
    memset(longurl, 'a', sizeof(longurl) - 1);
    memcpy(longurl, "https://", 8);
    longurl[sizeof(longurl) - 1] = '\0';
    assert(!mimo_tts_url_ok(longurl));

    char tok[MIMO_TTS_TOKEN_MAX + 2];
    memset(tok, 'A', sizeof(tok));
    tok[MIMO_TTS_TOKEN_MIN] = '\0';
    assert(mimo_tts_token_ok(tok));
    tok[MIMO_TTS_TOKEN_MIN - 1] = '\0';
    assert(!mimo_tts_token_ok(tok));   /* too short to be a random secret */
    memset(tok, 'A', sizeof(tok));
    tok[MIMO_TTS_TOKEN_MAX] = '\0';
    assert(mimo_tts_token_ok(tok));
    tok[MIMO_TTS_TOKEN_MAX] = 'A';
    tok[MIMO_TTS_TOKEN_MAX + 1] = '\0';
    assert(!mimo_tts_token_ok(tok));
    assert(mimo_tts_token_ok("mimo_dev_0123456789abcdef-ABCDEF.~+/="));
    assert(!mimo_tts_token_ok("mimo_dev_0123456789abcdef ABCDEF0123"));    /* space */
    assert(!mimo_tts_token_ok("mimo_dev_0123456789abcdef\r\nABCDEF012"));  /* header split */
    assert(!mimo_tts_token_ok("mimo_dev_0123456789abcdef\"ABCDEF0123"));
    assert(!mimo_tts_token_ok("mimo_dev_0123456789abcdef\xc3\xa9" "ABCDEF01"));
    assert(!mimo_tts_token_ok(NULL));

    assert(mimo_tts_voice_ok("mimo"));
    assert(mimo_tts_voice_ok("mimo-sweet_2"));
    assert(!mimo_tts_voice_ok(""));
    assert(!mimo_tts_voice_ok("Mimo"));
    assert(!mimo_tts_voice_ok("mimo\""));
    assert(!mimo_tts_voice_ok("abcdefghijklmnopqrstuvwxyz0123456"));   /* 33 */
}

static void body(void)
{
    static char out[MIMO_TTS_BODY_MAX];
    const char *text = "Hi \"friend\"\\ \n\ttab\x01 caf\xc3\xa9!";
    size_t n = mimo_tts_body(out, sizeof(out), text, strlen(text), "mimo");
    const char *want = "{\"v\":1,\"voice\":\"mimo\",\"format\":\"mp3\","
                       "\"text\":\"Hi \\\"friend\\\"\\\\   tab  caf\xc3\xa9!\"}";
    if (strcmp(out, want)) {
        fprintf(stderr, "got  %s\nwant %s\n", out, want);
        abort();
    }
    assert(n == strlen(want));
    /* A bad voice name never reaches the JSON. */
    mimo_tts_body(out, sizeof(out), "x", 1, "bad\"voice");
    assert(strstr(out, "\"voice\":\"mimo\""));
    /* Worst case: every byte escaped, still fits MIMO_TTS_BODY_MAX. */
    static char quotes[MIMO_TTS_TEXT_MAX];
    memset(quotes, '"', sizeof(quotes));
    assert(mimo_tts_body(out, sizeof(out), quotes, sizeof(quotes), "mimo") > 2 * MIMO_TTS_TEXT_MAX);
    /* Too small a buffer: 0, never a cut JSON. */
    assert(mimo_tts_body(out, 20, "hello", 5, "mimo") == 0);
}

static void clip(void)
{
    static char text[3000];
    memset(text, 'a', sizeof(text));
    assert(mimo_tts_clip(text, 10) == 10);
    assert(mimo_tts_clip(text, MIMO_TTS_TEXT_MAX) == MIMO_TTS_TEXT_MAX);
    /* No break anywhere: cut at the limit. */
    assert(mimo_tts_clip(text, sizeof(text)) == MIMO_TTS_TEXT_MAX);
    /* A word break: cut there. */
    text[800] = ' ';
    assert(mimo_tts_clip(text, sizeof(text)) == 800);
    /* A sentence end beats a later word break. */
    text[700] = '.';
    text[701] = ' ';
    assert(mimo_tts_clip(text, sizeof(text)) == 701);
    /* Never inside a UTF-8 character. */
    memset(text, 'a', sizeof(text));
    text[MIMO_TTS_TEXT_MAX - 1] = '\xe2';
    text[MIMO_TTS_TEXT_MAX] = '\x82';
    text[MIMO_TTS_TEXT_MAX + 1] = '\xac';
    size_t n = mimo_tts_clip(text, sizeof(text));
    assert(n == MIMO_TTS_TEXT_MAX - 1);
}

static void headers(void)
{
    assert(mimo_tts_check_headers(200, "audio/mpeg", 1000) == MIMO_TTS_OK);
    assert(mimo_tts_check_headers(200, "Audio/MPEG; charset=binary", -1) == MIMO_TTS_OK);
    assert(mimo_tts_check_headers(200, "audio/mp3", -1) == MIMO_TTS_OK);
    assert(mimo_tts_check_headers(200, "audio/mpegurl", -1) == MIMO_TTS_ERR_TYPE);
    assert(mimo_tts_check_headers(200, "application/json", 50) == MIMO_TTS_ERR_TYPE);
    assert(mimo_tts_check_headers(200, NULL, 50) == MIMO_TTS_ERR_TYPE);
    assert(mimo_tts_check_headers(401, "application/json", 10) == MIMO_TTS_ERR_AUTH);
    assert(mimo_tts_check_headers(403, NULL, -1) == MIMO_TTS_ERR_AUTH);
    assert(mimo_tts_check_headers(429, NULL, -1) == MIMO_TTS_ERR_RATE);
    assert(mimo_tts_check_headers(500, "audio/mpeg", 100) == MIMO_TTS_ERR_STATUS);
    assert(mimo_tts_check_headers(302, "audio/mpeg", 100) == MIMO_TTS_ERR_STATUS);   /* no redirects */
    assert(mimo_tts_check_headers(200, "audio/mpeg", 0) == MIMO_TTS_ERR_STATUS);
    assert(mimo_tts_check_headers(200, "audio/mpeg", MIMO_TTS_MAX_BYTES) == MIMO_TTS_OK);
    assert(mimo_tts_check_headers(200, "audio/mpeg", MIMO_TTS_MAX_BYTES + 1) == MIMO_TTS_ERR_TOO_LARGE);
    assert(mimo_tts_check_headers(200, "audio/mpeg", 50LL * 1024 * 1024 * 1024) == MIMO_TTS_ERR_TOO_LARGE);
}

static void oversize_stream(void)
{
    mimo_tts_job_t job;
    mimo_tts_job_begin(&job, T0);
    size_t take;
    assert(mimo_tts_job_data(&job, MIMO_TTS_MAX_BYTES - 100, T0, &take) == MIMO_TTS_OK);
    assert(take == MIMO_TTS_MAX_BYTES - 100);
    /* The chunk that crosses the cap: what fits is kept, then it stops. */
    assert(mimo_tts_job_data(&job, 1024, T0, &take) == MIMO_TTS_ERR_TOO_LARGE);
    assert(take == 100 && job.bytes == MIMO_TTS_MAX_BYTES);
    assert(mimo_tts_job_data(&job, 1, T0, &take) == MIMO_TTS_ERR_TOO_LARGE && take == 0);
    /* Exactly at the cap is fine. */
    mimo_tts_job_begin(&job, T0);
    assert(mimo_tts_job_data(&job, MIMO_TTS_MAX_BYTES, T0, &take) == MIMO_TTS_OK);
}

static void timeouts(void)
{
    mimo_tts_job_t job;
    size_t take;
    /* Nothing at all: the first-byte deadline. */
    mimo_tts_job_begin(&job, T0);
    assert(mimo_tts_job_check(&job, T0 + MIMO_TTS_FIRST_BYTE_MS * MS) == MIMO_TTS_OK);
    assert(mimo_tts_job_check(&job, T0 + MIMO_TTS_FIRST_BYTE_MS * MS + 1) == MIMO_TTS_ERR_TIMEOUT);
    assert(mimo_tts_job_wait_ms(&job, T0, 500) == 500);
    assert(mimo_tts_job_wait_ms(&job, T0 + (MIMO_TTS_FIRST_BYTE_MS - 200) * MS, 500) == 200);
    assert(mimo_tts_job_wait_ms(&job, T0 + MIMO_TTS_FIRST_BYTE_MS * MS + 5, 500) == 1);
    /* Flowing, then silent: the stall deadline, from the last byte. */
    mimo_tts_job_begin(&job, T0);
    mimo_tts_job_data(&job, 10, T0 + 7000 * MS, &take);
    assert(mimo_tts_job_check(&job, T0 + 9000 * MS) == MIMO_TTS_OK);   /* past first-byte, but flowing */
    assert(mimo_tts_job_check(&job, T0 + (7000 + MIMO_TTS_STALL_MS) * MS + 1) == MIMO_TTS_ERR_TIMEOUT);
    /* Waiting for the player isn't a stall. */
    mimo_tts_job_begin(&job, T0);
    mimo_tts_job_data(&job, 10, T0, &take);
    mimo_tts_job_waited(&job, 4000 * MS);
    assert(mimo_tts_job_check(&job, T0 + (MIMO_TTS_STALL_MS + 3000) * MS) == MIMO_TTS_OK);
    /* A trickle can't outlast the total deadline. */
    mimo_tts_job_begin(&job, T0);
    for (int64_t t = T0; t < T0 + MIMO_TTS_TOTAL_MS * MS; t += 1000 * MS) {
        mimo_tts_job_data(&job, 1, t, &take);
        assert(mimo_tts_job_check(&job, t) == MIMO_TTS_OK);
    }
    assert(mimo_tts_job_check(&job, T0 + MIMO_TTS_TOTAL_MS * MS + 1) == MIMO_TTS_ERR_TIMEOUT);
}

static void memory(void)
{
    assert(mimo_tts_admit(MIMO_TTS_NEED_INTERNAL, MIMO_TTS_NEED_DMA));
    assert(!mimo_tts_admit(MIMO_TTS_NEED_INTERNAL - 1, MIMO_TTS_NEED_DMA));
    /* Link's 16 KB of DMA-capable RAM for link.register is never spent. */
    assert(!mimo_tts_admit(1 << 20, 16 * 1024));
    assert(mimo_tts_above_floor(MIMO_TTS_FLOOR_INTERNAL));
    assert(!mimo_tts_above_floor(MIMO_TTS_FLOOR_INTERNAL - 1));
}

static void gate(void)
{
    mimo_tts_gate_t g;
    mimo_tts_gate_reset(&g);
    assert(mimo_tts_gate_open(&g, T0));
    /* A refused token stops all requests until a new one is saved. */
    mimo_tts_gate_result(&g, MIMO_TTS_ERR_AUTH, T0);
    assert(!mimo_tts_gate_open(&g, T0 + 3600000 * MS));
    mimo_tts_gate_reset(&g);
    assert(mimo_tts_gate_open(&g, T0));
    /* 429: back off. */
    mimo_tts_gate_result(&g, MIMO_TTS_ERR_RATE, T0);
    assert(!mimo_tts_gate_open(&g, T0 + (MIMO_TTS_RATE_BACKOFF_MS - 1) * MS));
    assert(mimo_tts_gate_open(&g, T0 + MIMO_TTS_RATE_BACKOFF_MS * MS));
    /* Three failures in a row rest it; a success in between doesn't. */
    mimo_tts_gate_reset(&g);
    mimo_tts_gate_result(&g, MIMO_TTS_ERR_TIMEOUT, T0);
    mimo_tts_gate_result(&g, MIMO_TTS_ERR_NET, T0);
    mimo_tts_gate_result(&g, MIMO_TTS_OK, T0);
    mimo_tts_gate_result(&g, MIMO_TTS_ERR_STATUS, T0);
    assert(mimo_tts_gate_open(&g, T0));
    mimo_tts_gate_result(&g, MIMO_TTS_ERR_TYPE, T0);
    mimo_tts_gate_result(&g, MIMO_TTS_ERR_TOO_LARGE, T0);
    assert(!mimo_tts_gate_open(&g, T0 + 1));
    assert(mimo_tts_gate_open(&g, T0 + MIMO_TTS_ERROR_BACKOFF_MS * MS));
    /* The gadget's own reasons don't count against the relay. */
    mimo_tts_gate_reset(&g);
    for (int i = 0; i < 10; i++) {
        mimo_tts_gate_result(&g, MIMO_TTS_ERR_CANCELLED, T0);
        mimo_tts_gate_result(&g, MIMO_TTS_ERR_MEMORY, T0);
        mimo_tts_gate_result(&g, MIMO_TTS_ERR_CONFIG, T0);
    }
    assert(mimo_tts_gate_open(&g, T0));
    for (int e = 0; e <= MIMO_TTS_ERR_COUNT; e++) {
        assert(mimo_tts_err_name((mimo_tts_err_t)e)[0]);
    }
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "credentials", credentials }, { "body", body }, { "clip", clip }, { "headers", headers },
        { "oversize_stream", oversize_stream }, { "timeouts", timeouts }, { "memory", memory },
        { "gate", gate },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && !strcmp(argv[1], cases[i].name)) {
            cases[i].fn();
            puts("PASS");
            return 0;
        }
    }
    fprintf(stderr, "unknown case\n");
    return 2;
}
