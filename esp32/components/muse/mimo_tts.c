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
 * The relay client (mimo_tts.h). One worker task owns the HTTPS connection;
 * the chat task only ever copies a request in (mimo_tts_start) and audio out
 * (mimo_tts_take), neither of which waits. Their handoff:
 *
 *   s_want   the job the chat task wants (0: none). The worker drops a job
 *            as soon as it stops being wanted.
 *   s_cur    the job s_audio belongs to. The worker empties s_audio, then
 *            publishes s_cur, so a reader never sees an older job's bytes.
 *   s_state  that job's state; the worker writes all its audio before DONE.
 */
#include "mimo_tts.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "network_proxy_tls.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "mimo_tts";

#define NS "mimo_tts"
#define AUDIO_BYTES (32 * 1024)        /* worker -> chat task; PSRAM */
#define READ_CHUNK 1024
#define OP_WAIT_MS 500                 /* per socket read: how soon a cancel is seen */
#define SEND_WAIT_MS 50                /* per wait for room in s_audio */
#define WORKER_STACK (10 * 1024)       /* TLS handshake runs here; PSRAM, as the chat task's */
#define BYTE_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_worker;
static StreamBufferHandle_t s_audio;

/* Settings, under s_lock. Static buffers go to PSRAM (EXT_RAM_BSS_ATTR, zeroed
 * at boot): internal RAM is what Wi-Fi, TLS and Link run short of. */
EXT_RAM_BSS_ATTR static char s_url[MIMO_TTS_URL_MAX + 1];
EXT_RAM_BSS_ATTR static char s_token[MIMO_TTS_TOKEN_MAX + 1];
EXT_RAM_BSS_ATTR static char s_voice[MIMO_TTS_VOICE_MAX + 1];
static bool s_enabled = true;
static mimo_tts_gate_t s_gate;

/* The request, under s_lock. */
static char *s_text;   /* MIMO_TTS_TEXT_MAX + 1, PSRAM */
static size_t s_text_len;
static uint32_t s_next_id;

static atomic_uint s_want;
static atomic_uint s_cur;
static atomic_int s_state = MIMO_TTS_FAILED;
static atomic_int s_last_err = MIMO_TTS_OK;

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static size_t internal_free(void)
{
    return heap_caps_get_free_size(BYTE_CAPS);
}

static bool memory_ok(void)
{
    return mimo_tts_admit(internal_free(), heap_caps_get_free_size(MALLOC_CAP_DMA));
}

static void fingerprint(const char *secret, char *out, size_t cap)
{
    if (!secret[0]) {
        snprintf(out, cap, "none");
        return;
    }
    uint8_t h[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md && mbedtls_md(md, (const unsigned char *)secret, strlen(secret), h) == 0) {
        snprintf(out, cap, "%u chars, sha256 %02x%02x%02x%02x", (unsigned)strlen(secret), h[0], h[1], h[2], h[3]);
    } else {
        snprintf(out, cap, "%u chars", (unsigned)strlen(secret));
    }
    memset(h, 0, sizeof(h));
}

/* ---- Settings ---- */

static void load_str(nvs_handle_t nvs, const char *key, char *out, size_t cap)
{
    size_t n = cap;
    if (nvs_get_str(nvs, key, out, &n) != ESP_OK) {
        out[0] = '\0';
    }
}

static const char *save(const char *key, const char *value)
{
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return "error: storage unavailable";
    }
    esp_err_t err = value ? nvs_set_str(nvs, key, value) : nvs_erase_key(nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK ? NULL : "error: could not save";
}

const char *mimo_tts_set_url(const char *url)
{
    if (!s_lock) return "error: speech not started";
    if (!mimo_tts_url_ok(url)) return "error: needs an https:// URL";
    LOCK();
    const char *res = save("url", url);
    if (!res) {
        strlcpy(s_url, url, sizeof(s_url));
        mimo_tts_gate_reset(&s_gate);
    }
    UNLOCK();
    return res;
}

const char *mimo_tts_set_token(const char *token)
{
    if (!s_lock) return "error: speech not started";
    if (!mimo_tts_token_ok(token)) return "error: token must be 32-256 URL-safe characters";
    LOCK();
    const char *res = save("token", token);
    if (!res) {
        strlcpy(s_token, token, sizeof(s_token));
        mimo_tts_gate_reset(&s_gate);   /* a new token: a refused one no longer blocks */
    }
    UNLOCK();
    char fp[48];
    fingerprint(token, fp, sizeof(fp));
    ESP_LOGI(TAG, "relay token saved (%s)", fp);
    return res;
}

const char *mimo_tts_set_voice(const char *voice)
{
    if (!s_lock) return "error: speech not started";
    if (!mimo_tts_voice_ok(voice)) return "error: voice is 1-32 of a-z 0-9 _ -";
    LOCK();
    const char *res = save("voice", voice);
    if (!res) strlcpy(s_voice, voice, sizeof(s_voice));
    UNLOCK();
    return res;
}

const char *mimo_tts_set_enabled(bool on)
{
    if (!s_lock) return "error: speech not started";
    LOCK();
    const char *res = save("on", on ? "1" : "0");
    if (!res) s_enabled = on;
    UNLOCK();
    if (!on) mimo_tts_cancel();
    return res;
}

void mimo_tts_clear(void)
{
    if (!s_lock) return;
    mimo_tts_cancel();
    LOCK();
    save("url", NULL);
    save("token", NULL);
    memset(s_token, 0, sizeof(s_token));
    s_url[0] = '\0';
    mimo_tts_gate_reset(&s_gate);
    UNLOCK();
    ESP_LOGI(TAG, "relay URL and token forgotten");
}

void mimo_tts_describe(char *out, size_t cap)
{
    static const char *const SHORT[MIMO_TTS_ERR_COUNT] = {
        "ok", "config", "auth", "rate", "status", "type", "size", "timeout", "net", "memory", "cancel",
    };
    if (!s_lock) {
        snprintf(out, cap, "not started");
        return;
    }
    char fp[48];
    uint8_t h[32] = {};
    LOCK();
    fingerprint(s_token, fp, sizeof(fp));
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool hashed = s_token[0] && md && mbedtls_md(md, (const unsigned char *)s_token, strlen(s_token), h) == 0;
    bool on = s_enabled, has_url = s_url[0] != '\0', refused = s_gate.auth_failed;
    bool resting = !refused && s_gate.rest_until_us > now_us();
    UNLOCK();
    unsigned err = (unsigned)atomic_load(&s_last_err);
    const char *note = refused ? " (token refused: set a new one)" : resting ? " (resting after errors)" : "";
    ESP_LOGI(TAG, "%s, relay %s, token %s, last %s%s", on ? "on" : "off", has_url ? "set" : "none", fp,
             mimo_tts_err_name((mimo_tts_err_t)err), note);
    /* The setup reply fits 40 bytes: "on r:set t:1a2b3c4d ok". */
    char tok[12] = "none";
    if (hashed) snprintf(tok, sizeof(tok), "%02x%02x%02x%02x", h[0], h[1], h[2], h[3]);
    memset(h, 0, sizeof(h));
    snprintf(out, cap, "%s r:%s t:%s %s%s", on ? "on" : "off", has_url ? "set" : "none", tok,
             err < MIMO_TTS_ERR_COUNT ? SHORT[err] : "?", refused ? "!" : resting ? "~" : "");
}

/* ---- The download ---- */

typedef struct {
    char type[48];   /* Content-Type */
} resp_t;

static esp_err_t on_http(esp_http_client_event_t *evt)
{
    resp_t *r = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value &&
        !strcasecmp(evt->header_key, "Content-Type")) {
        strlcpy(r->type, evt->header_value, sizeof(r->type));
    }
    return ESP_OK;
}

static bool wanted(uint32_t id)
{
    return atomic_load(&s_want) == id;
}

/* Pushes n bytes to the chat task, waiting for room while the job is wanted. */
static mimo_tts_err_t push(uint32_t id, mimo_tts_job_t *job, const uint8_t *data, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        int64_t t0 = now_us();
        sent += xStreamBufferSend(s_audio, data + sent, n - sent, pdMS_TO_TICKS(SEND_WAIT_MS));
        mimo_tts_job_waited(job, now_us() - t0);   /* the player, not the network */
        if (!wanted(id)) {
            return MIMO_TTS_ERR_CANCELLED;
        }
    }
    return MIMO_TTS_OK;
}

static mimo_tts_err_t fetch(uint32_t id, const char *url, const char *auth, const char *body, size_t body_len,
                            uint8_t *chunk)
{
    if (!memory_ok()) {
        ESP_LOGW(TAG, "not enough free memory: int %uK, dma %uK", (unsigned)(internal_free() / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024));
        return MIMO_TTS_ERR_MEMORY;
    }
    resp_t resp = {};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = OP_WAIT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .event_handler = on_http,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    network_proxy_config_t proxy;
    network_proxy_get_active(&proxy);
    bool use_proxy = proxy.type != NETWORK_PROXY_OFF;
    network_proxy_wipe(&proxy, sizeof(proxy));
    esp_transport_handle_t transport = use_proxy ? network_proxy_http_transport(true) : NULL;
    if (use_proxy && !transport) {
        return MIMO_TTS_ERR_NET;
    }
#if CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
    cfg.transport = transport;
#else
    if (transport) {
        esp_transport_destroy(transport);
        return MIMO_TTS_ERR_NET;
    }
#endif
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        if (transport) esp_transport_destroy(transport);
        return MIMO_TTS_ERR_MEMORY;
    }
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Accept", "audio/mpeg");
    esp_http_client_set_header(c, "User-Agent", "MIMO-Gadget/1");

    mimo_tts_job_t job;
    mimo_tts_job_begin(&job, now_us());
    mimo_tts_err_t err = MIMO_TTS_OK;
    int64_t length = -1;

    /* Connect and send: TLS can't be interrupted, so it gets the first-byte deadline. */
    esp_http_client_set_timeout_ms(c, mimo_tts_job_wait_ms(&job, now_us(), MIMO_TTS_FIRST_BYTE_MS));
    if (esp_http_client_open(c, (int)body_len) != ESP_OK) {
        err = mimo_tts_job_check(&job, now_us()) ? MIMO_TTS_ERR_TIMEOUT : MIMO_TTS_ERR_NET;
        goto out;
    }
    for (size_t sent = 0; sent < body_len;) {
        int n = esp_http_client_write(c, body + sent, (int)(body_len - sent));
        if (n <= 0) {
            err = MIMO_TTS_ERR_NET;
            goto out;
        }
        sent += (size_t)n;
    }
    for (;;) {
        if (!wanted(id)) { err = MIMO_TTS_ERR_CANCELLED; goto out; }
        if ((err = mimo_tts_job_check(&job, now_us()))) goto out;
        esp_http_client_set_timeout_ms(c, mimo_tts_job_wait_ms(&job, now_us(), OP_WAIT_MS));
        int64_t n = esp_http_client_fetch_headers(c);
        if (n == -ESP_ERR_HTTP_EAGAIN) continue;
        if (n < 0) { err = MIMO_TTS_ERR_NET; goto out; }
        length = n > 0 ? n : -1;   /* 0: chunked or none given */
        break;
    }
    err = mimo_tts_check_headers(esp_http_client_get_status_code(c), resp.type[0] ? resp.type : NULL, length);
    if (err) {
        ESP_LOGW(TAG, "relay answered %d (%s)", esp_http_client_get_status_code(c), mimo_tts_err_name(err));
        goto out;
    }
    for (;;) {
        if (!wanted(id)) { err = MIMO_TTS_ERR_CANCELLED; break; }
        if (!mimo_tts_above_floor(internal_free())) { err = MIMO_TTS_ERR_MEMORY; break; }
        if ((err = mimo_tts_job_check(&job, now_us()))) break;
        esp_http_client_set_timeout_ms(c, mimo_tts_job_wait_ms(&job, now_us(), OP_WAIT_MS));
        int n = esp_http_client_read(c, (char *)chunk, READ_CHUNK);
        if (n == -ESP_ERR_HTTP_EAGAIN) continue;
        if (n < 0) { err = MIMO_TTS_ERR_NET; break; }
        if (n == 0) {
            if (!esp_http_client_is_complete_data_received(c)) {
                err = MIMO_TTS_ERR_NET;   /* closed early */
            } else if (!job.bytes) {
                err = MIMO_TTS_ERR_STATUS;   /* an empty answer */
            }
            break;
        }
        size_t take;
        mimo_tts_err_t size = mimo_tts_job_data(&job, (size_t)n, now_us(), &take);
        if ((err = push(id, &job, chunk, take))) break;
        if ((err = size)) break;   /* past the cap: what came plays */
    }
    ESP_LOGI(TAG, "job %u: %u bytes in %lld ms (%s)", (unsigned)id, (unsigned)job.bytes,
             (now_us() - job.start_us) / 1000, mimo_tts_err_name(err));
out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (transport) esp_transport_destroy(transport);
    return err;
}

static void worker(void *arg)
{
    (void)arg;
    /* The worker's own copies, so the lock isn't held across the network. */
    EXT_RAM_BSS_ATTR static char url[MIMO_TTS_URL_MAX + 1], auth[MIMO_TTS_TOKEN_MAX + 8];
    char *body = heap_caps_malloc(MIMO_TTS_BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *chunk = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body || !chunk) {
        ESP_LOGE(TAG, "no memory for the worker: speech off");
        free(body);
        free(chunk);
        s_worker = NULL;
        vTaskDelete(NULL);
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            uint32_t id = atomic_load(&s_want);
            if (!id || id == atomic_load(&s_cur)) {
                break;   /* nothing new wanted */
            }
            LOCK();
            id = atomic_load(&s_want);   /* the latest, with its text */
            size_t body_len = id ? mimo_tts_body(body, MIMO_TTS_BODY_MAX, s_text, s_text_len, s_voice) : 0;
            strlcpy(url, s_url, sizeof(url));
            snprintf(auth, sizeof(auth), "Bearer %s", s_token);
            UNLOCK();
            if (!id) {
                break;
            }
            /* Only this task writes s_audio and the reader never waits on it. */
            xStreamBufferReset(s_audio);
            atomic_store(&s_state, MIMO_TTS_STREAMING);
            atomic_store(&s_cur, id);
            mimo_tts_err_t err = fetch(id, url, auth, body, body_len, chunk);
            memset(auth, 0, sizeof(auth));
            atomic_store(&s_last_err, err);
            LOCK();
            mimo_tts_gate_result(&s_gate, err, now_us());
            UNLOCK();
            if (err == MIMO_TTS_ERR_AUTH) {
                ESP_LOGW(TAG, "relay refused the token: speech waits for a new one (mimo.tts.token=)");
            }
            atomic_store(&s_state, err == MIMO_TTS_OK ? MIMO_TTS_DONE : MIMO_TTS_FAILED);
        }
    }
}

/* ---- The chat task's side ---- */

void mimo_tts_init(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    s_text = heap_caps_malloc(MIMO_TTS_TEXT_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* A byte over: a stream buffer made with caps keeps one back. */
    s_audio = xStreamBufferCreateWithCaps(AUDIO_BYTES + 1, 1, MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_text || !s_audio) {
        ESP_LOGE(TAG, "no memory: speech off");
        return;
    }
    strlcpy(s_voice, CONFIG_MIMO_TTS_VOICE_DEFAULT, sizeof(s_voice));
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) == ESP_OK) {
        char on[2];
        load_str(nvs, "url", s_url, sizeof(s_url));
        load_str(nvs, "token", s_token, sizeof(s_token));
        load_str(nvs, "voice", s_voice, sizeof(s_voice));
        load_str(nvs, "on", on, sizeof(on));
        nvs_close(nvs);
        s_enabled = on[0] != '0';
        if (!mimo_tts_url_ok(s_url)) s_url[0] = '\0';
        if (!mimo_tts_token_ok(s_token)) memset(s_token, 0, sizeof(s_token));
        if (!s_voice[0] || !mimo_tts_voice_ok(s_voice)) strlcpy(s_voice, CONFIG_MIMO_TTS_VOICE_DEFAULT, sizeof(s_voice));
    }
    if (xTaskCreatePinnedToCoreWithCaps(worker, "mimo_tts", WORKER_STACK, NULL, 4, &s_worker, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_worker = NULL;
        ESP_LOGE(TAG, "worker didn't start: speech off");
        return;
    }
    char fp[48];
    fingerprint(s_token, fp, sizeof(fp));
    ESP_LOGI(TAG, "ready: %s, relay %s, token %s", s_enabled ? "on" : "off", s_url[0] ? "set" : "none", fp);
}

uint32_t mimo_tts_start(const char *text, size_t len)
{
    if (!s_worker || !text || !len) {
        return 0;
    }
    if (!memory_ok()) {
        atomic_store(&s_last_err, MIMO_TTS_ERR_MEMORY);
        return 0;
    }
    LOCK();
    if (!s_enabled || !s_url[0] || !s_token[0] || !mimo_tts_gate_open(&s_gate, now_us())) {
        UNLOCK();
        return 0;
    }
    s_text_len = mimo_tts_clip(text, len);
    memcpy(s_text, text, s_text_len);
    s_text[s_text_len] = '\0';
    uint32_t id = ++s_next_id ? s_next_id : ++s_next_id;
    atomic_store(&s_want, id);
    UNLOCK();
    xTaskNotifyGive(s_worker);
    return id;
}

mimo_tts_state_t mimo_tts_take(uint32_t job, uint8_t *dst, size_t cap, size_t *got)
{
    *got = 0;
    if (!job || job != atomic_load(&s_want)) {
        return MIMO_TTS_FAILED;
    }
    if (job != atomic_load(&s_cur)) {
        return MIMO_TTS_STREAMING;   /* the worker hasn't picked it up yet */
    }
    /* The state first: everything a finished job wrote is already in s_audio. */
    mimo_tts_state_t st = (mimo_tts_state_t)atomic_load(&s_state);
    if (cap) {
        *got = xStreamBufferReceive(s_audio, dst, cap, 0);
    }
    if (st == MIMO_TTS_STREAMING || !xStreamBufferIsEmpty(s_audio)) {
        return MIMO_TTS_STREAMING;
    }
    return st;
}

void mimo_tts_cancel(void)
{
    atomic_store(&s_want, 0);
}

mimo_tts_err_t mimo_tts_last_error(void)
{
    return (mimo_tts_err_t)atomic_load(&s_last_err);
}
