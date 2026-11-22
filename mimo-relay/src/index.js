// Copyright (c) the MIMO project contributors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// The MIMO voice relay (relay protocol v1, esp32/docs/mimo-tts.md).
//
//   POST /v1/speak
//   Authorization: Bearer <device relay token>
//   {"v":1,"voice":"mimo","format":"mp3","text":"Hi! I'm MIMO."}
//
// -> 200 audio/mpeg, streamed from ElevenLabs as mono MP3, 24 kHz, 48 kbps.
//
// The ElevenLabs API key is the ELEVENLABS_API_KEY secret and never leaves
// the relay. Logs hold counts and outcomes: never a token, key or reply text.

import { authenticate } from './auth.js';
import { Budget } from './budget.js';

export { Budget };

const ELEVENLABS = 'https://api.elevenlabs.io/v1/text-to-speech';
const BODY_MAX = 8192;                 // bytes of request JSON
const UPSTREAM_HEADERS_MS = 15000;     // ElevenLabs must start answering by then
const VOICE_PROFILE_RE = /^[a-z0-9_-]{1,32}$/;
const VOICE_ID_RE = /^[A-Za-z0-9]{10,40}$/;

function json(status, error, extra = {}) {
  return new Response(JSON.stringify({ error }), {
    status,
    headers: { 'content-type': 'application/json', 'cache-control': 'no-store', ...extra },
  });
}

// At most max characters (code points) of text: up to the last sentence end
// that fits, else the last word break, else max. Only what's kept is spoken
// and counted; MIMO shows the whole reply on screen.
export function clip(text, max) {
  const cps = [...text];
  if (cps.length <= max) return text;
  const cut = cps.slice(0, max).join('');
  const sentence = /^[\s\S]*[.!?](?=\s|$)/.exec(cut);
  if (sentence && [...sentence[0]].length >= max / 2) return sentence[0];
  const space = cut.lastIndexOf(' ');
  if (space > cut.length / 2) return cut.slice(0, space);
  return cut;
}

function intVar(value, fallback) {
  const n = Number.parseInt(value, 10);
  return Number.isFinite(n) && n > 0 ? n : fallback;
}

export function config(env) {
  let voices = {};
  try {
    voices = JSON.parse(env.VOICES || '{}');
  } catch {
    voices = {};
  }
  let voiceSettings = null;
  if (env.VOICE_SETTINGS) {
    try {
      voiceSettings = JSON.parse(env.VOICE_SETTINGS);
    } catch {
      voiceSettings = null;
    }
  }
  return {
    enabled: env.SPEECH_ENABLED === 'true',
    model: env.MODEL_ID || 'eleven_flash_v2_5',
    format: env.OUTPUT_FORMAT || 'mp3_24000_48',
    voices,
    voiceSettings,
    maxText: intVar(env.MAX_TEXT_CHARS, 300),
    deviceDaily: intVar(env.DEVICE_DAILY_CHARS, 500),
    monthly: intVar(env.MONTHLY_CHARS, 8000),
    billingDay: Math.min(intVar(env.BILLING_DAY, 1), 28),
    billingHour: Math.max(0, Math.min(Number.parseInt(env.BILLING_HOUR || '0', 10) || 0, 23)),
  };
}

async function budget(env, path, body) {
  const stub = env.BUDGET.get(env.BUDGET.idFromName('global'));
  const res = await stub.fetch(`https://budget${path}`, { method: 'POST', body: JSON.stringify(body) });
  return res.json();
}

function log(fields) {
  console.log(JSON.stringify({ at: 'mimo-relay', ...fields }));
}

export async function handle(request, env, ctx, deps = {}) {
  const fetchUpstream = deps.fetch || fetch;
  const now = deps.now ? deps.now() : Date.now();
  const started = now;
  const url = new URL(request.url);

  if (url.pathname !== '/v1/speak') return json(404, 'not found');
  if (request.method !== 'POST') return json(405, 'POST only', { allow: 'POST' });

  const cfg = config(env);
  if (!cfg.enabled) return json(503, 'speech is switched off', { 'retry-after': '3600' });
  if (!env.ELEVENLABS_API_KEY || !env.DEVICE_TOKENS || !env.BUDGET) {
    log({ outcome: 'misconfigured' });
    return json(503, 'relay not set up', { 'retry-after': '3600' });
  }

  const device = await authenticate(request, env);
  if (!device) {
    log({ outcome: 'unauthorized' });
    return json(401, 'unknown or revoked token');
  }

  // The request: small, JSON, v1, mp3, a known voice profile.
  const declared = Number.parseInt(request.headers.get('content-length') || '0', 10);
  if (declared > BODY_MAX) return json(413, 'request too large');
  const raw = await request.text();
  if (raw.length > BODY_MAX) return json(413, 'request too large');
  let body;
  try {
    body = JSON.parse(raw);
  } catch {
    return json(400, 'not JSON');
  }
  if (!body || body.v !== 1 || body.format !== 'mp3' || typeof body.text !== 'string') {
    return json(400, 'expected {"v":1,"format":"mp3","voice":...,"text":...}');
  }
  const whole = body.text.trim();
  if (!whole) return json(400, 'empty text');
  const text = clip(whole, cfg.maxText);   // longer replies: their start is spoken
  const chars = [...text].length;
  const clipped = text.length < whole.length;
  const profile = body.voice ?? 'mimo';
  if (typeof profile !== 'string' || !VOICE_PROFILE_RE.test(profile)) return json(400, 'bad voice profile');
  const voiceId = cfg.voices[profile];
  if (!voiceId || !VOICE_ID_RE.test(voiceId)) return json(400, `no voice profile "${profile}"`);

  // Bursts: Cloudflare's rate limiter, per device (approximate, per location).
  if (env.BURST) {
    const { success } = await env.BURST.limit({ key: device });
    if (!success) {
      log({ device, outcome: 'burst limit' });
      return json(429, 'too many requests', { 'retry-after': '60' });
    }
  }

  // Spending: exact, shared counts, reserved before anything is spent.
  const held = await budget(env, '/reserve', {
    device, chars, now, deviceDaily: cfg.deviceDaily, monthly: cfg.monthly,
    billingDay: cfg.billingDay, billingHour: cfg.billingHour,
  });
  if (!held.ok) {
    log({ device, chars, outcome: held.reason });
    return json(429, held.reason, { 'retry-after': String(held.retryAfter) });
  }
  const giveBack = () => budget(env, '/refund', { device, chars, now, billingDay: cfg.billingDay, billingHour: cfg.billingHour });

  const target = `${ELEVENLABS}/${encodeURIComponent(voiceId)}/stream?output_format=${encodeURIComponent(cfg.format)}`;
  const upstreamBody = { text, model_id: cfg.model };
  if (cfg.voiceSettings) upstreamBody.voice_settings = cfg.voiceSettings;
  const abort = new AbortController();
  const timer = setTimeout(() => abort.abort(), deps.upstreamTimeoutMs || UPSTREAM_HEADERS_MS);
  let upstream;
  try {
    upstream = await fetchUpstream(target, {
      method: 'POST',
      headers: {
        'xi-api-key': env.ELEVENLABS_API_KEY,
        'content-type': 'application/json',
        accept: 'audio/mpeg',
      },
      body: JSON.stringify(upstreamBody),
      signal: abort.signal,
    });
  } catch (err) {
    clearTimeout(timer);
    await giveBack();
    log({ device, chars, outcome: abort.signal.aborted ? 'provider timeout' : 'provider unreachable' });
    return json(504, 'voice provider did not answer');
  }
  clearTimeout(timer);

  if (upstream.status !== 200) {
    await giveBack();
    let detail = '';
    try {
      const j = await upstream.json();
      detail = (j && j.detail && (j.detail.status || j.detail.code)) || '';
    } catch {
      detail = '';
    }
    log({ device, chars, outcome: 'provider error', providerStatus: upstream.status, detail: String(detail).slice(0, 40) });
    // Never pass ElevenLabs' 401 on: MIMO would take it as its own token refused.
    if (upstream.status === 429 || detail === 'quota_exceeded') {
      return json(429, 'voice provider busy or out of credits', { 'retry-after': '300' });
    }
    return json(502, 'voice provider error');
  }

  log({ device, chars, clipped, outcome: 'ok', ms: (deps.now ? deps.now() : Date.now()) - started, deviceToday: held.deviceToday, month: held.month });
  return new Response(upstream.body, {
    status: 200,
    headers: {
      'content-type': 'audio/mpeg',
      'cache-control': 'no-store',
      'x-mimo-chars': String(chars),
      'x-mimo-clipped': clipped ? '1' : '0',
    },
  });
}

export default {
  fetch(request, env, ctx) {
    return handle(request, env, ctx);
  },
};
