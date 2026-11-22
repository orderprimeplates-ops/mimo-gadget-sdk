<!--
Copyright (c) the MIMO project contributors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# MIMO voice relay

A Cloudflare Worker that turns MIMO's reply text into speech with
ElevenLabs, in MIMO's own voice, and streams the MP3 back to the board.
**Not deployed.** Nothing here runs until the activation steps below.

```
MIMO (Waveshare 1.75C) ──HTTPS, device token──► Worker /v1/speak ──xi-api-key──► ElevenLabs
                       ◄── audio/mpeg stream ◄──                 ◄── MP3 stream ◄──
```

| | |
|---|---|
| Voice | Profile `mimo` → ElevenLabs voice **MIMO** (`HPxy2zOoCDbTjLDGYzNm`), set in `wrangler.toml` `VOICES` |
| Model | `eleven_flash_v2_5` (fast and cheap; `MODEL_ID` changes it) |
| Audio | `mp3_24000_48`: mono MP3, 24 kHz, 48 kbps, the format MIMO's decoder tests play |
| Secrets | `ELEVENLABS_API_KEY` and `DEVICE_TOKENS`, in Cloudflare's secret store only |

Change the voice, model, voice settings or limits in `wrangler.toml` and
redeploy: MIMO never needs a reflash. MIMO can also pick another profile
(`mimo.tts.voice=sleepy`) once one is added to `VOICES`.

## Security

- **The ElevenLabs key** is a Worker secret (`wrangler secret put`). It's never
  in this repository, in `wrangler.toml`, in the firmware, in logs or in
  responses. Tests check all four.
- **Device tokens**: each board has its own random token (`mimo_dev_…`, 256
  bits). The relay keeps only SHA-256 hashes (the `DEVICE_TOKENS` secret,
  `{"mimo-175c": ["<hash>", "<next hash>"]}`) and compares them in constant
  time. Two hashes per device allow rotation without downtime. Removing a hash
  revokes that token at once.
- **Upstream errors are translated**: ElevenLabs' own 401 (bad key, out of
  credits) is never passed on as 401, which MIMO would read as its token being
  revoked.
- **Privacy**: logs record device, character count, outcome and timing, never
  reply text, tokens or keys. Responses are `no-store`. No CORS: browsers can't
  call it.

## Spending safeguards

Five layers, any of which stops spending:

1. **Off by default**: `SPEECH_ENABLED = "false"` until you switch it on; it's
   also the kill switch.
2. **Per request**: at most 1000 characters (`MAX_TEXT_CHARS`, MIMO's own limit).
3. **Bursts**: 12 requests a minute per device (Cloudflare rate limiter).
4. **Exact budgets** (Durable Object, characters reserved before ElevenLabs is
   called, refunded if it fails): `DEVICE_DAILY_CHARS` (10,000) per device per
   UTC day and `MONTHLY_CHARS` (100,000) for all devices per UTC month.
   Over budget, MIMO is told to wait (`429`, `Retry-After`) and shows replies
   as text.
5. **At ElevenLabs**: use a plan with a fixed credit allowance and no
   overage or auto top-up (Subscription → Manage, or Pay As You Go limits), so
   even a relay bug can't spend past the plan.

Cloudflare's Workers Free plan never bills: past its daily request quota,
requests fail until the next day.

## Tests

```sh
cd mimo-relay && npm test      # Node 20+, no packages, no network
```

22 tests: routing, the kill switch, authentication and rotation, the exact
ElevenLabs request (voice, model, format, key header), MIMO's own request
body, streaming the MP3 byte for byte, request checks, burst limits, daily and
monthly budgets with refunds, ElevenLabs failures and time-outs, nothing
secret in logs or responses, and the token script.
`esp32/tests/test_mimo_tts.py` checks the firmware and relay agree on the
token rule, text limit, audio format and voice profile.

## Activate (later, step by step, nothing done yet)

1. **ElevenLabs**: on a plan with API access, confirm the MIMO voice works
   with `eleven_flash_v2_5` (a short test in the web app), turn overage /
   usage-based billing off or cap it, and create an API key restricted to
   Text to Speech.
2. **Cloudflare**: a free account; `npm install --save-dev wrangler` here, then
   `npx wrangler login`.
3. **Check the config**: `npx wrangler deploy --dry-run` (validates the rate
   limiter and Durable Object bindings without deploying).
4. **Secrets** (typed at the prompts, never on the command line or in chat):
   `npx wrangler secret put ELEVENLABS_API_KEY`, then
   `node scripts/new-device-token.mjs --device mimo-175c --out ~/mimo-token.txt`
   and `npx wrangler secret put DEVICE_TOKENS` with the JSON it prints.
5. **Deploy, still off**: `npx wrangler deploy`. Check `/v1/speak` answers 503.
6. **Switch on** `SPEECH_ENABLED = "true"`, deploy, and test with `curl` from
   your computer (save the MP3, listen).
7. **Give MIMO the relay** (firmware built with `MIMO_TTS=1`):
   `python3 tools/mimo/provision_relay.py --url https://mimo-relay.<you>.workers.dev/v1/speak --token-file ~/mimo-token.txt`,
   then delete `~/mimo-token.txt`.

## Costs (estimates: check the providers' pricing pages before activating)

MIMO speaks only voice-note replies, each at most 1000 characters; a typical
short reply is about 250 characters.

| Use | Replies/day | Characters/month |
|---|---|---|
| Light | 15 | ~110,000 |
| Regular | 40 | ~300,000 |
| Heavy | 100 | ~750,000 |

| Item | Light | Regular | Heavy |
|---|---|---|---|
| ElevenLabs, Flash v2.5 at 0.5 credit/char on a plan | Creator, $22 (121k credits) | Pro, $99 (600k credits) | Pro, $99 |
| ElevenLabs, Flash at 1 credit/char (upper bound) | Creator, $22 | Pro, $99 | Scale, $299 |
| ElevenLabs Pay As You Go, ~$0.04 per 1k chars (if offered on your account) | ~$4.50 | ~$12 | ~$30 |
| Cloudflare Workers Free (100k requests/day; Durable Objects included) | $0 | $0 | $0 |
| **Total** | **$5–22** | **$12–99** | **$30–299** |

The relay's default `MONTHLY_CHARS` (100,000) keeps MIMO inside the Creator
plan even at 1 credit per character; raise it with the plan.
