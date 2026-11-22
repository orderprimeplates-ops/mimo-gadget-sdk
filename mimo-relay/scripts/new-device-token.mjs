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

// Makes a device relay token on YOUR computer.
//
//   node scripts/new-device-token.mjs --device mimo-175c --out ~/mimo-token.txt
//
// The token goes only into --out (created with owner-only permissions, never
// overwritten) for tools/mimo/provision_relay.py --token-file. What's printed
// is its SHA-256, for the relay's DEVICE_TOKENS secret. Delete the file once
// MIMO has the token.

import { createHash, randomBytes } from 'node:crypto';
import { writeFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { resolve } from 'node:path';

function arg(name) {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : undefined;
}

const device = arg('--device');
let out = arg('--out');
if (!device || !/^[a-z0-9_-]{1,40}$/.test(device) || !out) {
  console.error('usage: node scripts/new-device-token.mjs --device mimo-175c --out ~/mimo-token.txt');
  process.exit(2);
}
if (out.startsWith('~/')) out = resolve(homedir(), out.slice(2));

const token = 'mimo_dev_' + randomBytes(32).toString('base64url');
try {
  writeFileSync(out, token + '\n', { mode: 0o600, flag: 'wx' });
} catch (err) {
  console.error(`could not create ${out}: ${err.code === 'EEXIST' ? 'it already exists' : err.message}`);
  process.exit(1);
}
const hash = createHash('sha256').update(token).digest('hex');
console.log(`Token for "${device}" written to ${out} (owner-only).`);
console.log(`SHA-256 prefix the board will report: t:${hash.slice(0, 8)}`);
console.log('Add this hash to the DEVICE_TOKENS secret (npx wrangler secret put DEVICE_TOKENS):');
console.log(JSON.stringify({ [device]: [hash] }));
