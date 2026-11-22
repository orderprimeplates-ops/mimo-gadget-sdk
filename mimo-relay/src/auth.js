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

// Device tokens. The relay never stores a token, only its SHA-256, in the
// DEVICE_TOKENS secret: {"mimo-175c": ["<sha256 hex>", "<sha256 hex>"]}.
// Two hashes per device let a new token go live before the old is revoked.

// What the firmware accepts (mimo_tts_token_ok): nothing a header could be split with.
export const TOKEN_RE = /^[A-Za-z0-9._~+/=-]{32,256}$/;

export async function sha256Hex(text) {
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// Same time whatever the inputs share, so a guess learns nothing from timing.
export function sameHex(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string' || a.length !== 64 || b.length !== 64) return false;
  let diff = 0;
  for (let i = 0; i < 64; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

export function parseDeviceTokens(secret) {
  let table;
  try {
    table = JSON.parse(secret || '{}');
  } catch {
    return null;
  }
  if (!table || typeof table !== 'object' || Array.isArray(table)) return null;
  const out = [];
  for (const [device, hashes] of Object.entries(table)) {
    if (!/^[a-z0-9_-]{1,40}$/.test(device) || !Array.isArray(hashes)) return null;
    for (const h of hashes) {
      if (typeof h !== 'string' || !/^[0-9a-f]{64}$/.test(h)) return null;
      out.push([device, h]);
    }
  }
  return out;
}

// The device a request's bearer token belongs to, or null.
export async function authenticate(request, env) {
  const header = request.headers.get('Authorization') || '';
  const m = /^Bearer ([^\s]+)$/.exec(header);
  if (!m || !TOKEN_RE.test(m[1])) return null;
  const table = parseDeviceTokens(env.DEVICE_TOKENS);
  if (!table) return null;
  const presented = await sha256Hex(m[1]);
  let device = null;
  for (const [name, hash] of table) {
    if (sameHex(presented, hash) && device === null) device = name;   // no early exit
  }
  return device;
}
