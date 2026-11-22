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

# MIMO spoken replies (`CONFIG_MIMO_TTS`)

MIMO speaks Muse's voice-note replies in its own voice. The gadget sends each
finished reply message's text to the **MIMO voice relay**, a small HTTPS
service that holds the TTS provider's API key and picks the voice, and plays
the MP3 the relay streams back through the firmware's existing reply decoder.

The option is **off by default**. With it off, the firmware is built exactly as
before: none of this code is compiled (the wiring tests check every MIMO line
sits behind `#if CONFIG_MIMO_TTS`).

```
Muse reply (text) ──► chat task ──start──► mimo_tts worker ──HTTPS──► MIMO relay ──► TTS provider
                         ▲                     (own task,              (provider key,
                         │                      own socket)             voice, limits)
   speaker ◄── decode() ◄── s_turn.mp3 ◄──take── 32 KB handoff ◄── MP3 stream ◄──┘
```

## Build it

```sh
MIMO_TTS=1 tools/muse/board.sh build s3      # Waveshare 1.75C, in build-muse-waveshare-s3-175c-mimo-tts/
```

`MIMO_TTS=1` adds `devices/sdkconfig.mimo-tts` (just `CONFIG_MIMO_TTS=y`) in a
build directory of its own, so the stock build is never affected.
`CONFIG_MIMO_TTS_VOICE_DEFAULT` names the voice profile the relay is asked for
(`mimo`); the relay maps it to a provider voice, so changing MIMO's voice never
needs new firmware.

## What happens in a turn

1. A reply message finishes (`TTS_QUEUED`). `start_tts()` asks `mimo_speak()`
   first. If the speaker is on and `mimo_tts_start()` accepts the job, the
   message becomes speech; otherwise it's shown at reading pace exactly as
   without MIMO.
2. The worker task POSTs to the relay and streams the MP3 into a 32 KB PSRAM
   buffer. Each pass of the chat task's loop, `mimo_pump()` moves what's there
   into the session's 512 KB MP3 buffer **without waiting**, and `decode()`
   plays it with captions following, as the firmware's TTS hook intended.
3. The message ends when the MP3 is drained. A new press (`turn_finish()`)
   cancels the job; its unread audio is dropped and never reaches the next turn.

### Failure rules

| What happens | Result |
|---|---|
| Not set up, speech off, speaker off, low memory, relay resting | Relay not asked; text at reading pace (as today) |
| Relay error, 401/403, 429, wrong type, network error, no first byte in 8 s | Text at reading pace |
| Connection drops or stalls (5 s) after audio started | What came plays, then the message ends |
| Stream passes 384 KB (or Content-Length says it will) | First 384 KB play (stream) / text (header) |
| Download over 60 s | Ended; what came plays |
| 401/403 | No more requests until a new token is saved |
| 429 | Rest 60 s |
| 3 failures in a row | Rest 15 s |

## Relay protocol (v1)

```
POST <relay URL>
Authorization: Bearer <device relay token>
Content-Type: application/json
Accept: audio/mpeg

{"v":1,"voice":"mimo","format":"mp3","text":"Hi! I'm MIMO."}
```

- `text`: at most 1000 bytes of UTF-8, cut at a sentence or word (longer
  replies speak their start; the whole reply stays in Latest reply).
- Answer `200` with `Content-Type: audio/mpeg`, mono MP3, ideally 24 kHz at
  48 kbps (what `test_reply.mp3` is), streamed or with a Content-Length.
- `401`/`403`: token unknown or revoked. `429`: slow down. No redirects.

The relay should, at minimum: accept HTTPS only; store each device token as
a SHA-256 hash and compare in constant time; allow two live tokens per device
(for rotation); limit text length and requests per token per minute and day;
cap spending at the provider; keep the provider key in its secret store; log
counts and errors, not reply text.

## Credentials: provisioning and rotation

What the gadget holds is a **device relay token**: a random secret that only
lets that one device ask that one relay for speech. The provider's API key
never leaves the relay.

- **Make a token** on your own computer, never in chat or in the repository:
  `python3 -c "import secrets; print('mimo_dev_' + secrets.token_urlsafe(32))"`.
  Register its hash with the relay.
- **Give it to MIMO** over the USB cable:
  `python3 tools/mimo/provision_relay.py --url https://<relay>/v1/speak`. It
  asks for the token at a hidden prompt (or `--token-file`), sends it once as a
  setup command, and checks the board reports the same SHA-256 prefix.
  Authenticated BLE (passkey pairing) carries the same `mimo.tts.*` commands.
- **Where it lives**: the device's NVS (namespace `mimo_tts`), loaded into
  PSRAM. Nothing is compiled in: there is no Kconfig option for a URL or token
  and `devices/sdkconfig.mimo-tts` holds none.
- **Never shown**: the token and URL are never logged or echoed. The console
  line `>mimo.tts.status` answers `on r:set t:1a2b3c4d ok`, where `t:` is the
  token's SHA-256 prefix.
- **Rotate** (say every 90 days, or whenever a cable or backup leaves your
  hands): make a new token on the relay (the old stays valid), provision it,
  check the prefix, let MIMO speak once (the relay sees the new token used),
  then revoke the old one.
- **Revoke in an emergency**: revoke it on the relay. MIMO gets `401`, stops
  asking (no retry storm) and shows replies as text until a new token is set.
- **Forget**: `python3 tools/mimo/provision_relay.py --clear`.

**Limits.** Flash encryption stays off on this board (AGENTS.md: it would stop
reflashing), so someone holding the device can read its NVS. That's why the
device token is low-value by design: speech only, one device, rate-limited,
spending-capped and revocable. A full flash backup contains it too: keep
backups private, and rotate the token if one is shared.

| Setup command | Does |
|---|---|
| `mimo.tts.url=https://…` | Saves the relay URL (https only, no credentials in it) |
| `mimo.tts.token=…` | Saves the token (32–256 of `A-Z a-z 0-9 . _ ~ + / = -`) |
| `mimo.tts.voice=mimo` | Voice profile name |
| `mimo.tts.on=0` / `=1` | Turns speech off or on (kept) |
| `mimo.tts.clear` | Forgets URL and token |
| `mimo.tts.status` | `on r:set t:<sha256 prefix> <last result>`; `!` refused token, `~` resting |

## Memory and the Muse connection

- **Separate connection and task.** The worker (`mimo_tts`, priority 4, below
  the chat task's 5) owns its own HTTPS socket. The Noise session's socket,
  buffers and timing are untouched; the chat task never waits on the relay
  (`mimo_tts_take()` reads with a zero timeout; `mimo_tts_start()` only copies
  text). The test `test_chat_task_never_waits_on_the_relay` checks this.
- **PSRAM, not internal RAM.** Worker stack 10 KB, handoff buffer 32 KB,
  request text 1 KB, JSON body 6 KB, read chunk 1 KB, settings: about 51 KB of
  the board's 8 MB PSRAM. The session's 512 KB MP3 buffer already existed.
  mbedTLS's record buffers are in PSRAM on this board
  (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`). Static internal RAM grows by
  **64 bytes** (`esp_idf_size`, 1.75C build).
- **Admission.** A request starts only with 12 KB of internal RAM free and
  24 KB of DMA-capable RAM, which keeps Link's 16 KB DMA reserve for
  `link.register`; a download stops if internal RAM falls under 4 KB.
- **Bounded.** One request at a time; 384 KB of MP3 at most, which fits the
  512 KB session buffer; every socket wait 500 ms or less so a cancel lands
  quickly; 8 s to the first byte, 5 s stalls, 60 s in all.
- **Not yet measured on hardware**: free internal heap during a real HTTPS
  request beside the Noise session, TLS handshake time, and latency to first
  audio. The worker logs memory if it refuses; first hardware runs should
  capture these.

## Tests

`python3 -m unittest discover -s tests -p 'test_mimo_tts.py'`. No board, IDF
or network needed:

- `MimoTtsCore`: credential checks, JSON escaping, clipping, header rules,
  size cap, deadlines, memory admission and back-off (AddressSanitizer and
  UBSan on).
- `MimoSpeechSession`: the production `start_tts()`, `mimo_speak()`,
  `mimo_pump()` and `decode()` sliced from `muse_chat_session.cpp`, decoding
  `test_reply.mp3` with the real minimp3, against a scripted relay: playback,
  slow network and speaker, failures before and during audio, time-out,
  oversized replies, interruption, back-to-back messages and turns, junk audio.
- `MimoWiring`: off by default, nothing compiled in the stock build, the
  session hooks in place, no secrets compiled or logged.
- `MimoProvisioning`: the provisioning tool never prints the token and
  catches a board holding another one.
