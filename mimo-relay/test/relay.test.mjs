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

// The relay, run in Node against fakes: no Cloudflare account, no ElevenLabs
// key, no network. `node --test test/` (npm test).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { execFileSync, spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, statSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { handle, config } from '../src/index.js';
import { Budget, reserve } from '../src/budget.js';
import { sameHex, parseDeviceTokens, TOKEN_RE } from '../src/auth.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const RELAY = join(HERE, '..');
const MP3 = readFileSync(join(RELAY, '../esp32/components/muse/test_reply.mp3'));   // 24 kHz mono 48 kbps
const MIMO_VOICE = 'HPxy2zOoCDbTjLDGYzNm';

// Fake credentials, for these tests only.
const TOKEN = 'mimo_dev_' + 'T'.repeat(43);
const NEXT_TOKEN = 'mimo_dev_' + 'N'.repeat(43);
const OTHER_TOKEN = 'mimo_dev_' + 'O'.repeat(43);
const API_KEY = 'fake-elevenlabs-key-for-tests-0000';
const sha = (s) => createHash('sha256').update(s).digest('hex');

// wrangler.toml's [vars], so the tests use the shipped defaults.
function wranglerVars() {
  const toml = readFileSync(join(RELAY, 'wrangler.toml'), 'utf8');
  const block = toml.split('[vars]')[1].split('\n[')[0];
  const vars = {};
  for (const m of block.matchAll(/^([A-Z_]+)\s*=\s*(?:"([^"]*)"|'([^']*)')/gm)) vars[m[1]] = m[2] ?? m[3];
  return vars;
}

function memoryStorage() {
  const map = new Map();
  return {
    map,
    async get(k) { return map.has(k) ? structuredClone(map.get(k)) : undefined; },
    async put(k, v) { map.set(k, structuredClone(v)); },
  };
}

function fakeBudgetNamespace(storage = memoryStorage()) {
  const obj = new Budget({ storage });
  return {
    storage,
    idFromName: (name) => name,
    get: () => ({ fetch: (url, init) => obj.fetch(new Request(url, init)) }),
  };
}

function fakeLimiter(allow = true) {
  const calls = [];
  return { calls, limit: async (o) => { calls.push(o.key); return { success: typeof allow === 'function' ? allow() : allow }; } };
}

function env(over = {}) {
  return {
    ...wranglerVars(),
    SPEECH_ENABLED: 'true',
    ELEVENLABS_API_KEY: API_KEY,
    DEVICE_TOKENS: JSON.stringify({ 'mimo-175c': [sha(TOKEN)], 'mimo-desk': [sha(OTHER_TOKEN)] }),
    BUDGET: fakeBudgetNamespace(),
    BURST: fakeLimiter(true),
    ...over,
  };
}

function mp3Response(bytes = MP3, chunk = 4096) {
  let i = 0;
  const stream = new ReadableStream({
    pull(c) {
      if (i >= bytes.length) return c.close();
      c.enqueue(bytes.subarray(i, i + chunk));
      i += chunk;
    },
  });
  return new Response(stream, { status: 200, headers: { 'content-type': 'audio/mpeg' } });
}

function fakeFetch(respond = () => mp3Response()) {
  const calls = [];
  const fn = async (url, init) => {
    calls.push({ url, init, body: JSON.parse(init.body) });
    return respond(url, init);
  };
  fn.calls = calls;
  return fn;
}

function speak({ token = TOKEN, body, text = "Hi! I'm MIMO.", voice = 'mimo', raw, method = 'POST', path = '/v1/speak' } = {}) {
  const payload = raw ?? JSON.stringify(body ?? { v: 1, voice, format: 'mp3', text });
  const headers = { 'content-type': 'application/json' };
  if (token) headers.authorization = `Bearer ${token}`;
  return new Request(`https://relay.example.com${path}`, method === 'POST' ? { method, headers, body: payload } : { method, headers });
}

const NOON = Date.UTC(2026, 10, 22, 12, 0, 0);

async function run(req, e = env(), deps = {}) {
  const fetch = deps.fetch || fakeFetch();
  const res = await handle(req, e, {}, { fetch, now: () => NOON, ...deps });
  return { res, fetch, env: e };
}

async function captureLogs(fn) {
  const lines = [];
  const orig = console.log;
  console.log = (...a) => lines.push(a.join(' '));
  try { await fn(); } finally { console.log = orig; }
  return lines.join('\n');
}

// ---- Shipped configuration ----

test('wrangler.toml: MIMO voice by default, decoder-compatible MP3, off until activated', () => {
  const vars = wranglerVars();
  const cfg = config(vars);
  assert.equal(cfg.voices.mimo, MIMO_VOICE);
  assert.equal(cfg.format, 'mp3_24000_48');
  assert.equal(cfg.model, 'eleven_flash_v2_5');
  assert.equal(cfg.enabled, false);
  assert.equal(cfg.maxText, 1000);   // the firmware's MIMO_TTS_TEXT_MAX
});

test('wrangler.toml and .gitignore hold no secrets and keep local ones out of git', () => {
  const toml = readFileSync(join(RELAY, 'wrangler.toml'), 'utf8');
  assert.doesNotMatch(toml, /^\s*(ELEVENLABS_API_KEY|DEVICE_TOKENS)\s*=/m);
  assert.doesNotMatch(toml, /xi-api-key|sk_[A-Za-z0-9]{10}/);
  const ignore = readFileSync(join(RELAY, '.gitignore'), 'utf8');
  for (const pattern of ['.dev.vars', '.env', 'node_modules/', '.wrangler/']) assert.ok(ignore.includes(pattern), pattern);
});

test('switched off: 503, nothing spent', async () => {
  const { res, fetch } = await run(speak(), env({ SPEECH_ENABLED: 'false' }));
  assert.equal(res.status, 503);
  assert.equal(fetch.calls.length, 0);
});

test('not set up (no key or tokens): 503', async () => {
  for (const missing of ['ELEVENLABS_API_KEY', 'DEVICE_TOKENS']) {
    const { res, fetch } = await run(speak(), env({ [missing]: undefined }));
    assert.equal(res.status, 503, missing);
    assert.equal(fetch.calls.length, 0);
  }
});

test('routes: POST /v1/speak only', async () => {
  assert.equal((await run(speak({ path: '/' }))).res.status, 404);
  assert.equal((await run(speak({ method: 'GET' }))).res.status, 405);
});

// ---- Authentication ----

test('a missing, malformed, unknown or revoked token gets 401 and costs nothing', async () => {
  for (const token of [null, 'short', `${TOKEN} extra`, 'mimo_dev_' + 'X'.repeat(43)]) {
    const { res, fetch } = await run(speak({ token }));
    assert.equal(res.status, 401, String(token));
    assert.equal(fetch.calls.length, 0);
  }
  const revoked = env({ DEVICE_TOKENS: JSON.stringify({ 'mimo-desk': [sha(OTHER_TOKEN)] }) });
  assert.equal((await run(speak(), revoked)).res.status, 401);
  assert.equal((await run(speak(), env({ DEVICE_TOKENS: 'not json' }))).res.status, 401);
});

test('rotation: old and new token both work until the old hash is removed', async () => {
  const both = env({ DEVICE_TOKENS: JSON.stringify({ 'mimo-175c': [sha(TOKEN), sha(NEXT_TOKEN)] }) });
  assert.equal((await run(speak({ token: TOKEN }), both)).res.status, 200);
  assert.equal((await run(speak({ token: NEXT_TOKEN }), both)).res.status, 200);
  const rotated = env({ DEVICE_TOKENS: JSON.stringify({ 'mimo-175c': [sha(NEXT_TOKEN)] }) });
  assert.equal((await run(speak({ token: TOKEN }), rotated)).res.status, 401);
  assert.equal((await run(speak({ token: NEXT_TOKEN }), rotated)).res.status, 200);
});

test('token helpers: constant-time compare, strict table, the firmware token rule', () => {
  assert.ok(sameHex(sha('a'), sha('a')));
  assert.ok(!sameHex(sha('a'), sha('b')));
  assert.ok(!sameHex('ab', 'ab'));
  assert.equal(parseDeviceTokens('{"MIMO":["x"]}'), null);
  assert.equal(parseDeviceTokens('[]'), null);
  assert.equal(parseDeviceTokens(JSON.stringify({ a: ['0'.repeat(64)] })).length, 1);
  assert.ok(TOKEN_RE.test(TOKEN));
  assert.ok(!TOKEN_RE.test('x'.repeat(31)) && !TOKEN_RE.test('x'.repeat(257)) && !TOKEN_RE.test('a b' + 'x'.repeat(40)));
});

// ---- The ElevenLabs call ----

test('calls ElevenLabs streaming TTS with MIMO\'s voice, Flash v2.5 and 24 kHz/48 kbps MP3', async () => {
  const { res, fetch } = await run(speak({ text: "Hi! I'm MIMO." }));
  assert.equal(res.status, 200);
  assert.equal(fetch.calls.length, 1);
  const call = fetch.calls[0];
  assert.equal(call.url, `https://api.elevenlabs.io/v1/text-to-speech/${MIMO_VOICE}/stream?output_format=mp3_24000_48`);
  assert.equal(call.init.method, 'POST');
  assert.equal(call.init.headers['xi-api-key'], API_KEY);
  assert.equal(call.init.headers.accept, 'audio/mpeg');
  assert.deepEqual(call.body, { text: "Hi! I'm MIMO.", model_id: 'eleven_flash_v2_5' });   // the voice's own settings
});

test('the firmware\'s exact request body is accepted (mimo_tts_body, escaped JSON)', async () => {
  const raw = '{"v":1,"voice":"mimo","format":"mp3","text":"Hi \\"friend\\"\\\\   tab  café!"}';
  const { res, fetch } = await run(speak({ raw }));
  assert.equal(res.status, 200);
  assert.equal(fetch.calls[0].body.text, 'Hi "friend"\\   tab  café!');
});

test('MIMO\'s voice changes with the relay\'s VOICES and VOICE_SETTINGS, no reflash', async () => {
  const e = env({
    VOICES: JSON.stringify({ mimo: 'NewVoiceId1234567890', sleepy: 'SleepyVoice123456789' }),
    VOICE_SETTINGS: '{"stability":0.4,"similarity_boost":0.8}',
    MODEL_ID: 'eleven_multilingual_v2',
  });
  let { fetch } = await run(speak(), e);
  assert.match(fetch.calls[0].url, /\/NewVoiceId1234567890\/stream\?/);
  assert.equal(fetch.calls[0].body.model_id, 'eleven_multilingual_v2');
  assert.deepEqual(fetch.calls[0].body.voice_settings, { stability: 0.4, similarity_boost: 0.8 });
  ({ fetch } = await run(speak({ voice: 'sleepy' }), e));
  assert.match(fetch.calls[0].url, /\/SleepyVoice123456789\/stream\?/);
});

test('streams the MP3 through untouched, as audio/mpeg', async () => {
  const { res } = await run(speak(), env(), { fetch: fakeFetch(() => mp3Response(MP3, 997)) });
  assert.equal(res.headers.get('content-type'), 'audio/mpeg');
  assert.equal(res.headers.get('cache-control'), 'no-store');
  const got = Buffer.from(await res.arrayBuffer());
  assert.ok(got.equals(MP3));
});

// ---- Request checks ----

test('bad requests are refused before anything is spent', async () => {
  const cases = [
    [{ raw: '{nope' }, 400],
    [{ body: { v: 2, voice: 'mimo', format: 'mp3', text: 'hi' } }, 400],
    [{ body: { v: 1, voice: 'mimo', format: 'wav', text: 'hi' } }, 400],
    [{ text: '   ' }, 400],
    [{ text: 'a'.repeat(1001) }, 400],
    [{ voice: 'nobody' }, 400],
    [{ voice: 'Bad Voice"' }, 400],
    [{ raw: JSON.stringify({ v: 1, voice: 'mimo', format: 'mp3', text: 'x'.repeat(9000) }) }, 413],
  ];
  for (const [req, status] of cases) {
    const e = env();
    const { res, fetch } = await run(speak(req), e);
    assert.equal(res.status, status, JSON.stringify(req).slice(0, 60));
    assert.equal(fetch.calls.length, 0);
    assert.equal(e.BUDGET.storage.map.size, 0);
  }
  assert.equal((await run(speak({ text: 'a'.repeat(1000) }))).res.status, 200);
  assert.equal((await run(speak({ text: '\u{1F600}'.repeat(1000) }))).res.status, 200);   // characters, not bytes
});

// ---- Rate limits and spending ----

test('burst limit: 429 with Retry-After, ElevenLabs not called, nothing charged', async () => {
  const e = env({ BURST: fakeLimiter(false) });
  const { res, fetch } = await run(speak(), e);
  assert.equal(res.status, 429);
  assert.equal(res.headers.get('retry-after'), '60');
  assert.equal(fetch.calls.length, 0);
  assert.deepEqual(e.BURST.calls, ['mimo-175c']);   // per device
  assert.equal(e.BUDGET.storage.map.size, 0);
});

test('per-device daily cap: the next day is fresh; other devices are separate', async () => {
  const e = env({ DEVICE_DAILY_CHARS: '30' });
  const text = 'x'.repeat(20);
  assert.equal((await run(speak({ text }), e)).res.status, 200);
  const { res, fetch } = await run(speak({ text }), e);
  assert.equal(res.status, 429);
  assert.equal(res.headers.get('retry-after'), String(12 * 3600));   // until UTC midnight
  assert.equal(fetch.calls.length, 0);
  assert.equal((await run(speak({ text, token: OTHER_TOKEN }), e)).res.status, 200);
  const tomorrow = await handle(speak({ text }), e, {}, { fetch: fakeFetch(), now: () => NOON + 86400e3 });
  assert.equal(tomorrow.status, 200);
});

test('monthly budget across all devices; resets with the month', async () => {
  const e = env({ MONTHLY_CHARS: '50', DEVICE_DAILY_CHARS: '1000' });
  const text = 'y'.repeat(20);
  assert.equal((await run(speak({ text }), e)).res.status, 200);
  assert.equal((await run(speak({ text, token: OTHER_TOKEN }), e)).res.status, 200);
  const { res } = await run(speak({ text }), e);
  assert.equal(res.status, 429);
  assert.equal(Number(res.headers.get('retry-after')), (Date.UTC(2026, 11, 1) - NOON) / 1000);
  const nextMonth = await handle(speak({ text }), e, {}, { fetch: fakeFetch(), now: () => Date.UTC(2026, 11, 1, 0, 0, 5) });
  assert.equal(nextMonth.status, 200);
});

test('budget never lets the cap pass, a request at a time (as the Durable Object runs them)', async () => {
  const storage = memoryStorage();
  let ok = 0;
  for (let i = 0; i < 20; i++) ok += (await reserve(storage, { device: 'd', chars: 10, now: NOON, deviceDaily: 100, monthly: 1e6 })).ok;
  assert.equal(ok, 10);
  assert.deepEqual((await storage.get('day')).chars, { d: 100 });
});

// ---- ElevenLabs failures ----

test('provider errors: 5xx -> 502 and the characters are given back', async () => {
  const e = env({ DEVICE_DAILY_CHARS: '20' });
  const fail = fakeFetch(() => new Response('{"detail":{"status":"internal"}}', { status: 500 }));
  const { res } = await run(speak({ text: 'z'.repeat(20) }), e, { fetch: fail });
  assert.equal(res.status, 502);
  assert.equal((await run(speak({ text: 'z'.repeat(20) }), e)).res.status, 200);   // refunded
});

test('ElevenLabs 401 never reaches MIMO as 401 (MIMO would think its own token was revoked)', async () => {
  const quota = fakeFetch(() => Response.json({ detail: { status: 'quota_exceeded', message: 'x' } }, { status: 401 }));
  let { res } = await run(speak(), env(), { fetch: quota });
  assert.equal(res.status, 429);   // out of credits: MIMO rests and shows text
  const badKey = fakeFetch(() => Response.json({ detail: { status: 'invalid_api_key' } }, { status: 401 }));
  ({ res } = await run(speak(), env(), { fetch: badKey }));
  assert.equal(res.status, 502);
  const busy = fakeFetch(() => new Response('slow down', { status: 429 }));
  ({ res } = await run(speak(), env(), { fetch: busy }));
  assert.equal(res.status, 429);
});

test('ElevenLabs unreachable or too slow: 504, refunded', async () => {
  const e = env({ DEVICE_DAILY_CHARS: '20' });
  const hang = fakeFetch((url, init) => new Promise((_, reject) => {
    init.signal.addEventListener('abort', () => reject(new Error('aborted')));
  }));
  const { res } = await run(speak({ text: 'q'.repeat(20) }), e, { fetch: hang, upstreamTimeoutMs: 20 });
  assert.equal(res.status, 504);
  const down = fakeFetch(() => { throw new TypeError('network'); });
  assert.equal((await run(speak({ text: 'q'.repeat(20) }), e, { fetch: down })).res.status, 504);
  assert.equal((await run(speak({ text: 'q'.repeat(20) }), e)).res.status, 200);
});

// ---- Privacy ----

test('logs and responses never contain the token, the API key or the reply text', async () => {
  const secretText = 'my secret reply about table twelve';
  let out = [];
  const logs = await captureLogs(async () => {
    out.push(await run(speak({ text: secretText })));
    out.push(await run(speak({ token: 'mimo_dev_' + 'W'.repeat(43), text: secretText })));
    out.push(await run(speak({ text: secretText }), env(), {
      fetch: fakeFetch(() => Response.json({ detail: { status: 'quota_exceeded', message: API_KEY } }, { status: 401 })),
    }));
  });
  for (const s of [TOKEN, API_KEY, secretText, 'W'.repeat(43)]) assert.ok(!logs.includes(s), `log leaked ${s.slice(0, 12)}`);
  for (const { res } of out.slice(1)) {
    const body = await res.text();
    assert.ok(!body.includes(API_KEY) && !body.includes(secretText));
  }
  assert.match(logs, /"outcome":"ok"/);
});

// ---- Device token script ----

test('new-device-token.mjs: owner-only file, prints only the hash, never overwrites', () => {
  const dir = mkdtempSync(join(tmpdir(), 'mimo-token-'));
  const out = join(dir, 'token.txt');
  const script = join(RELAY, 'scripts/new-device-token.mjs');
  const stdout = execFileSync(process.execPath, [script, '--device', 'mimo-175c', '--out', out], { encoding: 'utf8' });
  const token = readFileSync(out, 'utf8').trim();
  assert.ok(TOKEN_RE.test(token) && token.startsWith('mimo_dev_') && token.length >= 50);
  assert.equal(statSync(out).mode & 0o777, 0o600);
  assert.ok(!stdout.includes(token));
  const table = JSON.parse(stdout.trim().split('\n').pop());
  assert.deepEqual(table, { 'mimo-175c': [sha(token)] });
  assert.ok(stdout.includes(`t:${sha(token).slice(0, 8)}`));
  const again = spawnSync(process.execPath, [script, '--device', 'mimo-175c', '--out', out], { encoding: 'utf8' });
  assert.notEqual(again.status, 0);
  assert.equal(readFileSync(out, 'utf8').trim(), token);
  assert.ok(existsSync(out));
});
