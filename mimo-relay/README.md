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
2. **Per request**: at most 300 characters spoken (`MAX_TEXT_CHARS`). Longer
   replies are cut at a sentence and only that part is spoken and counted;
   MIMO still shows the whole reply.
3. **Bursts**: 12 requests a minute per device (Cloudflare rate limiter).
4. **Exact budgets** (Durable Object, characters reserved before ElevenLabs is
   called, refunded if it fails): `DEVICE_DAILY_CHARS` (500) per device per
   UTC day and `MONTHLY_CHARS` (8,000) for all devices per billing period
   (from `BILLING_DAY` at `BILLING_HOUR`, matching the ElevenLabs renewal).
   Over budget, MIMO is told to wait (`429`, `Retry-After`) and shows replies
   as text.
5. **At ElevenLabs**: use a plan with a fixed credit allowance and no
   overage or auto top-up (Subscription → Manage, or Pay As You Go limits), so
   even a relay bug can't spend past the plan.

Cloudflare's Workers Free plan never bills: past its daily request quota,
requests fail until the next day.

## Tests

```sh
cd mimo-relay && npm test      # Node 20+, no network, no Cloudflare or ElevenLabs account
```

31 tests: routing, the kill switch, authentication and rotation, the exact
ElevenLabs request (voice, model, format, key header), MIMO's own request
body, streaming the MP3 byte for byte, request checks, burst limits, daily and
billing-period budgets (aligned to the ElevenLabs renewal) with refunds,
`blockConcurrencyWhile`, ElevenLabs failures and time-outs, nothing secret in
logs or responses, and both scripts. `esp32/tests/test_mimo_tts.py` checks the
firmware and relay agree on the token rule, text limit, audio format and voice.

Validated without deploying: `npx wrangler deploy --dry-run` accepts the
config (`BUDGET` Durable Object, `BURST` rate limit 12/60 s), and `npx wrangler
dev` ran the Worker in Cloudflare's local runtime: SQLite-backed Durable
Object, 401s, refunds and the burst limit behaved as tested.

## Spending counter

- **Persistent**: kept in the Durable Object's SQLite storage, which survives
  restarts and redeploys.
- **Race-safe**: one Durable Object ("global") serves every request, and each
  reserve or refund runs inside `blockConcurrencyWhile`, so read-check-write
  never interleaves. Characters are reserved before ElevenLabs is called.
- **Resets**: per device at 00:00 UTC; for everyone at the start of the billing
  period, `BILLING_DAY` at `BILLING_HOUR` UTC. Set both to the ElevenLabs
  renewal time (`scripts/check-elevenlabs.mjs` prints it), so one relay period
  never covers two ElevenLabs credit periods. Renewals after the 28th use 28.

## Costs

Characters per 30-day month = replies a day × 250 characters × 30.

| Use | Replies/day | Characters/month | Credits at 0.5/char | Credits at 1/char |
|---|---|---|---|---|
| Light | 15 | 112,500 | 56,250 | 112,500 |
| Regular | 40 | 300,000 | 150,000 | 300,000 |
| Heavy | 100 | 750,000 | 375,000 | 750,000 |

ElevenLabs bills Flash v2.5 through the API at 0.5 to 1 credit per character
depending on plan (`check-elevenlabs.mjs` reports yours). Plans (check
[elevenlabs.io/pricing](https://elevenlabs.io/pricing) first): Starter $6 =
30,000 credits, Creator $22 = 121,000, Pro $99 = 600,000, Scale $299 = 1.8M.

| Use | Plan at 0.5/char | Plan at 1/char | Pay as you go (~$0.04 per 1k chars, if offered) | Cloudflare |
|---|---|---|---|---|
| Light | Creator $22 | Creator $22 | ~$4.50 | $0 |
| Regular | Pro $99 | Pro $99 | ~$12 | $0 |
| Heavy | Pro $99 | Scale $299 | ~$30 | $0 |

### Free plan (the shipped defaults)

MIMO starts on the **ElevenLabs Free plan: 10,000 credits a month, $0**, with
Cloudflare's free plan: **$0 in total**, and nothing can be charged (the Free
plan has no overage). The defaults fit inside it:

| Setting | Value | Means |
|---|---|---|
| `MAX_TEXT_CHARS` | 300 | at most 300 characters spoken per reply |
| `DEVICE_DAILY_CHARS` | 500 | 1–2 spoken replies a day per device |
| `MONTHLY_CHARS` | 8,000 | at most 8,000 credits a month even at 1 credit/char: about 26 full-length or 32 typical (250-character) replies, leaving 2,000 credits for tests in the ElevenLabs web app |

When a limit is reached, MIMO shows replies as text until the day or billing
period starts over. Free-plan voices are for non-commercial use: check
ElevenLabs' terms before using MIMO for business.

### Paid plans (later)

`MONTHLY_CHARS` of 100,000 would be about 13 replies a day (100,000 ÷ 30 ÷
250): 50,000 credits at 0.5/char or 100,000 at 1/char, which fits the
**Creator plan ($22)** either way but not Starter. Regular use (40 a day)
needs Pro. Raise `MONTHLY_CHARS`, `DEVICE_DAILY_CHARS` and `MAX_TEXT_CHARS`
only together with the plan.

## Deployed from GitHub (Cloudflare Workers Builds)

The relay deploys through Cloudflare's GitHub integration, set up from the
Cloudflare dashboard; nothing has to be installed on a computer.

| Setting (Workers & Pages → mimo-relay → Settings → Build) | Value |
|---|---|
| Repository | `orderprimeplates-ops/mimo-gadget-sdk` |
| Worker name | `mimo-relay` (must match `name` in `wrangler.toml`) |
| Production branch | `claude/jolly-faraday-ewyut7` (not `main`, which has no `mimo-relay/`) |
| Root directory | `mimo-relay` |
| Build command | none |
| Deploy command | `npx wrangler deploy` |
| Preview builds | off |

**What a push does.** Every push to the production branch builds and deploys
the relay: Cloudflare installs the pinned Wrangler (`package-lock.json`) and
runs `npx wrangler deploy` from `mimo-relay/`, which applies `wrangler.toml`.
If the dashboard offers build watch paths, `mimo-relay/*` keeps firmware-only
pushes from rebuilding it.

**`wrangler.toml` is the source of truth.** Each deploy sets the variables
from its `[vars]`, replacing any edited in the dashboard, so these always come
from the repository:

| Variable | Value | Why |
|---|---|---|
| `SPEECH_ENABLED` | `"false"` | Nothing reaches ElevenLabs until it's switched on here and pushed |
| `MAX_TEXT_CHARS` | `"300"` | At most 300 characters spoken per reply |
| `DEVICE_DAILY_CHARS` | `"500"` | About 1–2 spoken replies a day per device |
| `MONTHLY_CHARS` | `"8000"` | Inside the ElevenLabs Free plan's 10,000 credits |

To switch speech on (later), change `SPEECH_ENABLED` to `"true"` in this file
and push; to switch it off, change it back and push. Turning it on in the
dashboard alone is undone by the next deploy.

**Secrets are separate.** `ELEVENLABS_API_KEY` and `DEVICE_TOKENS` are added
in the dashboard (Settings → Variables and Secrets, type **Secret**) and are
kept across deploys. Until both exist, the relay answers `503 relay not set
up` even with speech on. They never go in this repository.

**Checking a deploy.** The build log names the commit and ends with
`wrangler deploy` listing the `BUDGET` (Durable Object) and `BURST` (rate
limit) bindings. Settings → Variables should show the four values above, and
opening `https://mimo-relay.<your-subdomain>.workers.dev/v1/speak` in a
browser should answer `{"error":"POST only"}`: live, and spending nothing.
If Cloudflare refuses the rate limiter on the Free plan, removing the
`[[ratelimits]]` block keeps the relay safe: the character budgets still cap
spending.

## Activate on a Mac (later; each step only when you decide)

You need: a Mac, the ElevenLabs account with the MIMO voice, and about 30
minutes. Nothing is charged until step 8, and only for test sentences.

1. **Install Node.js**: the LTS installer from [nodejs.org](https://nodejs.org).
   Open Terminal (Applications → Utilities).
2. **Get the code** (sign in to GitHub if asked):
   `git clone -b claude/jolly-faraday-ewyut7 https://github.com/orderprimeplates-ops/mimo-gadget-sdk.git`,
   then `cd mimo-gadget-sdk/mimo-relay` and `npm install`.
3. **ElevenLabs**: the Free plan fits the shipped limits (a paid plan needs
   higher ones); make sure usage-based billing / overage is off; create
   an API key with Text to Speech and User (read) access. Don't paste it anywhere
   but the prompts below.
4. **Check the account**: `node scripts/check-elevenlabs.mjs --sample` and
   paste the key at the hidden prompt. Listen to `mimo-sample.mp3` (MIMO on
   Flash v2.5, about 40 characters of credit). Put the `BILLING_DAY`,
   `BILLING_HOUR` and (if you like) `MONTHLY_CHARS` it suggests into
   `wrangler.toml`. Fix anything it marks `FIX:`.
5. **Cloudflare**: make a free account at
   [dash.cloudflare.com](https://dash.cloudflare.com), then `npx wrangler login`.
6. **Check and deploy, speech still off**: `npx wrangler deploy --dry-run`,
   then `npx wrangler deploy`. It prints the relay's address,
   `https://mimo-relay.<you>.workers.dev`. If Cloudflare refuses the rate
   limiter on the Free plan, delete the `[[ratelimits]]` block and deploy again
   (the character budgets still cap spending).
7. **Secrets**: `npx wrangler secret put ELEVENLABS_API_KEY` (paste the key);
   `node scripts/new-device-token.mjs --device mimo-175c --out ~/mimo-token.txt`;
   `npx wrangler secret put DEVICE_TOKENS` (paste the `{"mimo-175c":[...]}` line
   it printed).
8. **Switch on and test**: set `SPEECH_ENABLED = "true"` in `wrangler.toml`,
   `npx wrangler deploy`, then
   `curl -sS -X POST https://mimo-relay.<you>.workers.dev/v1/speak -H "Authorization: Bearer $(cat ~/mimo-token.txt)" -H "Content-Type: application/json" -d '{"v":1,"voice":"mimo","format":"mp3","text":"Hi! I am MIMO."}' -o test.mp3 && afplay test.mp3`
   You should hear MIMO.
9. **Off switch**, any time: `SPEECH_ENABLED = "false"` and `npx wrangler deploy`.
   Revoke a token by removing its hash from `DEVICE_TOKENS`.

Giving the relay to the Waveshare (firmware backup, `MIMO_TTS=1` flash,
`tools/mimo/provision_relay.py`) is a separate step.
