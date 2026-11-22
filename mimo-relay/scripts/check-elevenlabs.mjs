#!/usr/bin/env node
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

// Checks the ElevenLabs account before the relay goes live. Read-only and free
// unless --sample is given. Run it on YOUR computer:
//
//   node scripts/check-elevenlabs.mjs            # asks for the API key, hidden
//   node scripts/check-elevenlabs.mjs --sample   # also makes mimo-sample.mp3 (~60 characters of credit)
//
// It reports: the MIMO voice and whether Flash v2.5 lists it as supported,
// Flash v2.5's credit cost per character, the plan's credits and renewal time
// (-> BILLING_DAY / BILLING_HOUR / MONTHLY_CHARS for wrangler.toml), and
// whether overage billing is on. The key is never printed or saved.

import { createInterface } from 'node:readline';
import { writeFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

export const API = 'https://api.elevenlabs.io/v1';
export const VOICE_ID = 'HPxy2zOoCDbTjLDGYzNm';
export const MODEL_ID = 'eleven_flash_v2_5';

// The facts that matter, from GET /voices/{id}, /user/subscription and /models.
export function summarize({ voice, subscription, models }, voiceId = VOICE_ID, modelId = MODEL_ID) {
  const out = { problems: [], notes: [] };
  if (!voice) {
    out.problems.push(`voice ${voiceId} not found on this account`);
  } else {
    out.voiceName = voice.name;
    const hq = voice.high_quality_base_model_ids;
    if (Array.isArray(hq) && hq.length) {
      out.flashSupported = hq.includes(modelId);
      if (!out.flashSupported) out.problems.push(`voice "${voice.name}" doesn't list ${modelId} (it lists ${hq.join(', ')}): set MODEL_ID to one of those, or run --sample to hear Flash anyway`);
    } else {
      out.flashSupported = null;
      out.notes.push(`the voice doesn't list its models: run with --sample to confirm ${modelId} works`);
    }
  }
  const model = (models || []).find((m) => m.model_id === modelId);
  if (!model) out.problems.push(`${modelId} isn't offered to this account`);
  else if (model.can_do_text_to_speech === false) out.problems.push(`${modelId} can't do text to speech here`);
  const mult = model && model.model_rates && model.model_rates.character_cost_multiplier;
  out.creditsPerChar = typeof mult === 'number' ? mult : null;
  if (out.creditsPerChar === null) out.notes.push('credit cost per character not reported: assume 1 to be safe');
  if (subscription) {
    out.tier = subscription.tier;
    out.used = subscription.character_count;
    out.limit = subscription.character_limit;
    const reset = subscription.next_character_count_reset_unix;
    if (reset) {
      const d = new Date(reset * 1000);
      out.renewal = d.toISOString();
      out.billingDay = d.getUTCDate();
      out.billingHour = d.getUTCHours();
      if (out.billingDay > 28) {
        out.notes.push(`renews on day ${out.billingDay}: use BILLING_DAY = "28" (a few days' overlap; keep MONTHLY_CHARS conservative)`);
        out.billingDay = 28;
      }
    }
    const perChar = out.creditsPerChar ?? 1;
    if (typeof out.limit === 'number') {
      // 90% of the plan's credits, so other uses of the account have room too.
      out.suggestedMonthlyChars = Math.floor((out.limit * 0.9) / perChar);
    }
    const ext = subscription.max_credit_limit_extension ?? subscription.max_character_limit_extension;
    const canExtend = subscription.can_extend_character_limit;
    if (canExtend && ext !== 0) {
      out.problems.push('overage (usage-based billing) is ON: turn it off or cap it in Subscription settings so a bug can never overspend');
    } else {
      out.overage = 'off';
    }
  } else {
    out.problems.push('could not read the subscription (does the key have User read access?)');
  }
  return out;
}

async function get(path, key, fetchImpl) {
  const res = await fetchImpl(`${API}${path}`, { headers: { 'xi-api-key': key } });
  if (res.status === 404) return null;
  if (!res.ok) throw new Error(`GET ${path}: HTTP ${res.status}`);
  return res.json();
}

export async function check(key, { fetchImpl = fetch, voiceId = VOICE_ID } = {}) {
  const [voice, subscription, models] = await Promise.all([
    get(`/voices/${voiceId}`, key, fetchImpl),
    get('/user/subscription', key, fetchImpl).catch(() => null),
    get('/models', key, fetchImpl),
  ]);
  return summarize({ voice, subscription, models }, voiceId);
}

function askHidden(prompt) {
  return new Promise((resolve) => {
    const rl = createInterface({ input: process.stdin, output: process.stdout, terminal: true });
    rl._writeToOutput = (s) => { if (s.startsWith(prompt)) rl.output.write(prompt); };
    rl.question(prompt, (answer) => { rl.close(); process.stdout.write('\n'); resolve(answer.trim()); });
  });
}

async function main() {
  const key = process.env.ELEVENLABS_API_KEY || await askHidden('ElevenLabs API key (hidden): ');
  if (!key) process.exit(2);
  const r = await check(key);
  console.log(`Voice:        ${r.voiceName ?? 'not found'} (${VOICE_ID})`);
  console.log(`Flash v2.5:   ${r.flashSupported === true ? 'supported' : r.flashSupported === false ? 'NOT listed' : 'unknown (try --sample)'}`);
  console.log(`Credits/char: ${r.creditsPerChar ?? 'not reported'}`);
  console.log(`Plan:         ${r.tier ?? '?'}, ${r.used ?? '?'} of ${r.limit ?? '?'} credits used`);
  if (r.renewal) console.log(`Renews:       ${r.renewal}  ->  BILLING_DAY = "${r.billingDay}", BILLING_HOUR = "${r.billingHour}"`);
  if (r.suggestedMonthlyChars) console.log(`Suggested:    MONTHLY_CHARS = "${r.suggestedMonthlyChars}" (90% of the plan)`);
  console.log(`Overage:      ${r.overage ?? 'see below'}`);
  for (const n of r.notes) console.log(`note: ${n}`);
  for (const p of r.problems) console.log(`FIX: ${p}`);
  if (process.argv.includes('--sample')) {
    const res = await fetch(`${API}/text-to-speech/${VOICE_ID}/stream?output_format=mp3_24000_48`, {
      method: 'POST',
      headers: { 'xi-api-key': key, 'content-type': 'application/json', accept: 'audio/mpeg' },
      body: JSON.stringify({ text: "Hi! I'm MIMO. I'm so happy to see you!", model_id: MODEL_ID }),
    });
    if (res.ok) {
      writeFileSync('mimo-sample.mp3', Buffer.from(await res.arrayBuffer()));
      console.log('Sample:       mimo-sample.mp3 (MIMO with Flash v2.5, 24 kHz / 48 kbps). Listen to it.');
    } else {
      console.log(`Sample:       failed, HTTP ${res.status}`);
    }
  }
  process.exit(r.problems.length ? 1 : 0);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((err) => {
    console.error(`check failed: ${err.message}`);
    process.exit(1);
  });
}
