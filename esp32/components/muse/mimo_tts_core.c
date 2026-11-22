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

#include "mimo_tts_core.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

static const char *const ERR_NAMES[MIMO_TTS_ERR_COUNT] = {
    "ok", "not configured", "relay refused the token", "relay rate limit", "relay error",
    "not MP3", "too large", "timed out", "network error", "low memory", "cancelled",
};

const char *mimo_tts_err_name(mimo_tts_err_t err)
{
    return (unsigned)err < MIMO_TTS_ERR_COUNT ? ERR_NAMES[err] : "unknown";
}

/* ---- Credentials ---- */

bool mimo_tts_url_ok(const char *url)
{
    if (!url || strncmp(url, "https://", 8) != 0) {
        return false;
    }
    size_t len = strlen(url);
    if (len > MIMO_TTS_URL_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= ' ' || c >= 0x7f) {
            return false;
        }
    }
    /* A host: something before the path, and no credentials in it. */
    const char *host = url + 8;
    size_t host_len = strcspn(host, "/?#");
    if (!host_len || memchr(host, '@', host_len) || host[0] == '.' || host[0] == ':') {
        return false;
    }
    return true;
}

bool mimo_tts_token_ok(const char *token)
{
    if (!token) {
        return false;
    }
    size_t len = strlen(token);
    if (len < MIMO_TTS_TOKEN_MIN || len > MIMO_TTS_TOKEN_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)token[i];
        if (!(isalnum(c) || strchr("._~+/=-", c)) || c >= 0x80) {
            return false;
        }
    }
    return true;
}

bool mimo_tts_voice_ok(const char *voice)
{
    if (!voice || !voice[0] || strlen(voice) > MIMO_TTS_VOICE_MAX) {
        return false;
    }
    for (const char *p = voice; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return true;
}

/* ---- The request ---- */

size_t mimo_tts_clip(const char *text, size_t len)
{
    if (len <= MIMO_TTS_TEXT_MAX) {
        return len;
    }
    size_t limit = MIMO_TTS_TEXT_MAX;
    while (limit > 0 && ((unsigned char)text[limit] & 0xC0) == 0x80) {
        limit--;   /* text[limit] continues a character: cut before its start */
    }
    size_t sentence = 0, word = 0;
    for (size_t i = MIMO_TTS_TEXT_MAX / 2; i < limit; i++) {
        if (text[i] == ' ' || text[i] == '\n') {
            word = i;
            if (i > 0 && strchr(".!?", text[i - 1])) {
                sentence = i;
            }
        }
    }
    return sentence ? sentence : word ? word : limit;
}

size_t mimo_tts_body(char *out, size_t cap, const char *text, size_t len, const char *voice)
{
    if (!mimo_tts_voice_ok(voice)) {
        voice = "mimo";
    }
    len = text ? mimo_tts_clip(text, len) : 0;
    size_t o = 0;
#define PUT(s, n) do { if (o + (n) >= cap) return 0; memcpy(out + o, (s), (n)); o += (n); } while (0)
    static const char head[] = "{\"v\":1,\"voice\":\"";
    PUT(head, sizeof(head) - 1);
    PUT(voice, strlen(voice));
    static const char mid[] = "\",\"format\":\"mp3\",\"text\":\"";
    PUT(mid, sizeof(mid) - 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') {
            char esc[2] = { '\\', (char)c };
            PUT(esc, 2);
        } else if (c < 0x20 || c == 0x7f) {
            PUT(" ", 1);
        } else {
            PUT(&text[i], 1);
        }
    }
    PUT("\"}", 2);
#undef PUT
    out[o] = '\0';
    return o;
}

/* ---- The response ---- */

static bool type_is(const char *type, const char *want)
{
    size_t n = strlen(want);
    if (strncasecmp(type, want, n) != 0) {
        return false;
    }
    return type[n] == '\0' || type[n] == ';' || type[n] == ' ';
}

mimo_tts_err_t mimo_tts_check_headers(int status, const char *content_type, int64_t content_length)
{
    if (status == 401 || status == 403) {
        return MIMO_TTS_ERR_AUTH;
    }
    if (status == 429) {
        return MIMO_TTS_ERR_RATE;
    }
    if (status != 200 || content_length == 0) {
        return MIMO_TTS_ERR_STATUS;
    }
    if (!content_type) {
        return MIMO_TTS_ERR_TYPE;
    }
    while (*content_type == ' ') {
        content_type++;
    }
    if (!type_is(content_type, "audio/mpeg") && !type_is(content_type, "audio/mp3")) {
        return MIMO_TTS_ERR_TYPE;
    }
    if (content_length > (int64_t)MIMO_TTS_MAX_BYTES) {
        return MIMO_TTS_ERR_TOO_LARGE;
    }
    return MIMO_TTS_OK;
}

void mimo_tts_job_begin(mimo_tts_job_t *job, int64_t now_us)
{
    memset(job, 0, sizeof(*job));
    job->start_us = job->last_us = now_us;
}

mimo_tts_err_t mimo_tts_job_data(mimo_tts_job_t *job, size_t n, int64_t now_us, size_t *take)
{
    size_t room = MIMO_TTS_MAX_BYTES - job->bytes;
    *take = n < room ? n : room;
    job->bytes += *take;
    if (*take) {
        job->flowing = true;
        job->last_us = now_us;
    }
    return *take < n || (n == 0 && !room) ? MIMO_TTS_ERR_TOO_LARGE : MIMO_TTS_OK;
}

mimo_tts_err_t mimo_tts_job_check(const mimo_tts_job_t *job, int64_t now_us)
{
    if (now_us - job->start_us > (int64_t)MIMO_TTS_TOTAL_MS * 1000) {
        return MIMO_TTS_ERR_TIMEOUT;
    }
    if (!job->flowing && now_us - job->start_us > (int64_t)MIMO_TTS_FIRST_BYTE_MS * 1000) {
        return MIMO_TTS_ERR_TIMEOUT;
    }
    if (job->flowing && now_us - job->last_us > (int64_t)MIMO_TTS_STALL_MS * 1000) {
        return MIMO_TTS_ERR_TIMEOUT;
    }
    return MIMO_TTS_OK;
}

int mimo_tts_job_wait_ms(const mimo_tts_job_t *job, int64_t now_us, int cap_ms)
{
    int64_t total = job->start_us + (int64_t)MIMO_TTS_TOTAL_MS * 1000 - now_us;
    int64_t next = job->flowing ? job->last_us + (int64_t)MIMO_TTS_STALL_MS * 1000 - now_us
                                : job->start_us + (int64_t)MIMO_TTS_FIRST_BYTE_MS * 1000 - now_us;
    int64_t left = (total < next ? total : next) / 1000;
    if (left > cap_ms) {
        left = cap_ms;
    }
    return left < 1 ? 1 : (int)left;
}

void mimo_tts_job_waited(mimo_tts_job_t *job, int64_t waited_us)
{
    if (waited_us > 0) {
        job->last_us += waited_us;
    }
}

/* ---- Memory ---- */

bool mimo_tts_admit(size_t internal_free, size_t dma_free)
{
    return internal_free >= MIMO_TTS_NEED_INTERNAL && dma_free >= MIMO_TTS_NEED_DMA;
}

bool mimo_tts_above_floor(size_t internal_free)
{
    return internal_free >= MIMO_TTS_FLOOR_INTERNAL;
}

/* ---- When to ask again ---- */

bool mimo_tts_gate_open(const mimo_tts_gate_t *gate, int64_t now_us)
{
    return !gate->auth_failed && now_us >= gate->rest_until_us;
}

void mimo_tts_gate_result(mimo_tts_gate_t *gate, mimo_tts_err_t err, int64_t now_us)
{
    switch (err) {
    case MIMO_TTS_OK:
        gate->errors = 0;
        gate->rest_until_us = 0;
        break;
    case MIMO_TTS_ERR_AUTH:
        gate->auth_failed = true;
        break;
    case MIMO_TTS_ERR_RATE:
        gate->rest_until_us = now_us + (int64_t)MIMO_TTS_RATE_BACKOFF_MS * 1000;
        break;
    case MIMO_TTS_ERR_CANCELLED:
    case MIMO_TTS_ERR_MEMORY:
    case MIMO_TTS_ERR_CONFIG:
        break;   /* nothing the relay did */
    default:
        if (++gate->errors >= MIMO_TTS_ERRORS_BEFORE_REST) {
            gate->errors = 0;
            gate->rest_until_us = now_us + (int64_t)MIMO_TTS_ERROR_BACKOFF_MS * 1000;
        }
        break;
    }
}

void mimo_tts_gate_reset(mimo_tts_gate_t *gate)
{
    memset(gate, 0, sizeof(*gate));
}
