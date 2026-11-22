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

// The spending guard: characters sent to ElevenLabs, counted per device per
// UTC day and for everyone per billing period, in one Durable Object so every
// request sees the same count. Characters are reserved before ElevenLabs is
// called and given back if it fails, so a cap can't be passed even by
// requests in flight.
//
// The billing period starts each month on BILLING_DAY at BILLING_HOUR (UTC):
// set them to when the ElevenLabs subscription renews, so one relay period
// never straddles two ElevenLabs credit periods (scripts/check-elevenlabs.mjs
// prints them).

function dayKey(now) {
  return new Date(now).toISOString().slice(0, 10);
}

// The start of the billing period now is in, as epoch ms (day 1..28, hour 0..23, UTC).
export function periodStart(now, day = 1, hour = 0) {
  const d = new Date(now);
  let start = Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), day, hour);
  if (start > now) start = Date.UTC(d.getUTCFullYear(), d.getUTCMonth() - 1, day, hour);
  return start;
}

export function nextPeriodStart(now, day = 1, hour = 0) {
  const d = new Date(periodStart(now, day, hour));
  return Date.UTC(d.getUTCFullYear(), d.getUTCMonth() + 1, day, hour);
}

function periodKey(now, day, hour) {
  return new Date(periodStart(now, day, hour)).toISOString().slice(0, 13);   // "2026-11-15T10"
}

function secondsUntilNextDay(now) {
  const d = new Date(now);
  return Math.ceil((Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate() + 1) - now) / 1000);
}


// The rules, on any async get/put store (a Durable Object's storage, or a Map in tests).
export async function reserve(storage, { device, chars, now, deviceDaily, monthly, billingDay = 1, billingHour = 0 }) {
  const today = dayKey(now), month = periodKey(now, billingDay, billingHour);
  let day = (await storage.get('day')) || { day: today, chars: {} };
  if (day.day !== today) day = { day: today, chars: {} };   // a new day: yesterday's counts go
  let mon = (await storage.get('month')) || { month, chars: 0 };
  if (mon.month !== month) mon = { month, chars: 0 };
  const used = day.chars[device] || 0;
  if (used + chars > deviceDaily) {
    return { ok: false, reason: 'device daily limit', retryAfter: secondsUntilNextDay(now) };
  }
  if (mon.chars + chars > monthly) {
    return { ok: false, reason: 'monthly budget', retryAfter: Math.ceil((nextPeriodStart(now, billingDay, billingHour) - now) / 1000) };
  }
  day.chars[device] = used + chars;
  mon.chars += chars;
  await storage.put('day', day);
  await storage.put('month', mon);
  return { ok: true, deviceToday: day.chars[device], month: mon.chars };
}

export async function refund(storage, { device, chars, now, billingDay = 1, billingHour = 0 }) {
  const day = await storage.get('day');
  if (day && day.day === dayKey(now) && day.chars[device]) {
    day.chars[device] = Math.max(0, day.chars[device] - chars);
    await storage.put('day', day);
  }
  const mon = await storage.get('month');
  if (mon && mon.month === periodKey(now, billingDay, billingHour)) {
    mon.chars = Math.max(0, mon.chars - chars);
    await storage.put('month', mon);
  }
  return { ok: true };
}

// The Durable Object (SQLite-backed: wrangler.toml's new_sqlite_classes), one
// instance named "global". Its storage is durable: counts survive restarts and
// redeploys. Each read-check-write runs inside blockConcurrencyWhile, so no
// other request can interleave with it, whatever the runtime's I/O gates do.
export class Budget {
  constructor(state) {
    this.state = state;
    this.storage = state.storage;
  }

  async fetch(request) {
    const url = new URL(request.url);
    const body = await request.json();
    const op = url.pathname === '/reserve' ? reserve : url.pathname === '/refund' ? refund : null;
    if (!op) return new Response('not found', { status: 404 });
    const run = () => op(this.storage, body);
    const result = this.state.blockConcurrencyWhile ? await this.state.blockConcurrencyWhile(run) : await run();
    return Response.json(result);
  }
}
