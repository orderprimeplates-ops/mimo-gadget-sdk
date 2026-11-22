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

# MIMO

MIMO is a marshmallow AI companion built on this SDK for the Waveshare
ESP32-S3-Touch-AMOLED-1.75C.

- **Voice**: replies spoken through the MIMO voice relay, behind
  `CONFIG_MIMO_TTS` (off by default). See [docs/mimo-tts.md](../docs/mimo-tts.md).
  The voice is ElevenLabs "MIMO" (`HPxy2zOoCDbTjLDGYzNm`), set in the relay
  ([`mimo-relay/`](../../mimo-relay/README.md), not deployed), so it changes
  without a reflash. [voice-audition.md](voice-audition.md) compares voices.
- **Character**: `avatar/mimo_pixel.c` is a **placeholder** marshmallow that
  implements the whole avatar interface (`components/muse/muse_pixel.h`).
  The approved MIMO design replaces its drawing code.

## Preview the character without a board

```sh
python3 tools/mimo/preview.py                 # renders every state
python3 tools/mimo/preview.py --lvgl ~/deps/lvgl-9.5.0   # if LVGL's download is blocked
```

It builds the UI simulator with MIMO's renderer (`MUSE_SIM_AVATAR`, simulator
only, in `simulator/build-mimo/`) and writes `mimo/preview/contact_sheet.png`
and one PNG per state (boot, idle, listening, thinking, speaking, happy,
error, off). Nothing here reaches firmware: the firmware uses MIMO's renderer
only once it's copied to `components/muse/avatar/muse_pixel.c`, the
firmware's own custom-avatar slot (gitignored).
