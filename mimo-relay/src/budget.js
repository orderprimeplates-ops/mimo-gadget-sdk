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
// UTC day and for everyone per UTC month, in one Durable Object so every
// request sees the same count (a Durable Object handles one request at a
// time). Characters are reserved before ElevenLabs is called and given back
// if it fails, so a cap can't be passed even by requests in flight.

function dayKey(now) {
  return new Date(now).toISOString().slice(0, 10);
}

function monthKey(now) {
  return new Date(now).toISOString().slice(0, 7);
}

function secondsUntilNextDay(now) {
  const d = new Date(now);
  return Math.ceil((Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate() + 1) - now) / 1000);
}

function secondsUntilNextMonth(now) {
  const d = new Date(now);
  return Math.ceil((Date.UTC(d.getUTCFullYear(), d.getUTCMonth() + 1, 1) - now) / 1000);
}

// The rules, on any async get/put store (a Durable Object's storage, or a Map in tests).
export async function reserve(storage, { device, chars, now, deviceDaily, monthly }) {
  const today = dayKey(now), month = monthKey(now);
  let day = (await storage.get('day')) || { day: today, chars: {} };
  if (day.day !== today) day = { day: today, chars: {} };   // a new day: yesterday's counts go
  let mon = (await storage.get('month')) || { month, chars: 0 };
  if (mon.month !== month) mon = { month, chars: 0 };
  const used = day.chars[device] || 0;
  if (used + chars > deviceDaily) {
    return { ok: false, reason: 'device daily limit', retryAfter: secondsUntilNextDay(now) };
  }
  if (mon.chars + chars > monthly) {
    return { ok: false, reason: 'monthly budget', retryAfter: secondsUntilNextMonth(now) };
  }
  day.chars[device] = used + chars;
  mon.chars += chars;
  await storage.put('day', day);
  await storage.put('month', mon);
  return { ok: true, deviceToday: day.chars[device], month: mon.chars };
}

export async function refund(storage, { device, chars, now }) {
  const day = await storage.get('day');
  if (day && day.day === dayKey(now) && day.chars[device]) {
    day.chars[device] = Math.max(0, day.chars[device] - chars);
    await storage.put('day', day);
  }
  const mon = await storage.get('month');
  if (mon && mon.month === monthKey(now)) {
    mon.chars = Math.max(0, mon.chars - chars);
    await storage.put('month', mon);
  }
  return { ok: true };
}

// The Durable Object (SQLite-backed: wrangler.toml's new_sqlite_classes, which
// the Workers Free plan supports). One instance, named "global".
export class Budget {
  constructor(state) {
    this.storage = state.storage;
  }

  async fetch(request) {
    const url = new URL(request.url);
    const body = await request.json();
    const result = url.pathname === '/reserve' ? await reserve(this.storage, body)
      : url.pathname === '/refund' ? await refund(this.storage, body)
      : null;
    return result ? Response.json(result) : new Response('not found', { status: 404 });
  }
}
