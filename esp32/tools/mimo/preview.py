#!/usr/bin/env python3
# Copyright (c) the MIMO project contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Preview an avatar renderer in the UI simulator, without a board.

Builds the simulator with that renderer (MUSE_SIM_AVATAR: simulator only,
in its own build directory) and renders every state headless into one
contact sheet PNG, plus one PNG per state.

    python3 tools/mimo/preview.py                       # MIMO's renderer
    python3 tools/mimo/preview.py --avatar avatar/muse_pixel.c --out /tmp/default
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ESP32 = Path(__file__).resolve().parents[2]
STATES = [
    ('boot', 'face=boot\nadvance=900\n'),
    ('idle', 'face=idle\n'),
    ('listening', 'face=listening\nlevel=0.6\n'),
    ('thinking', 'face=thinking\nprogress=0.4\n'),
    ('speaking', 'face=speaking\nlevel=0.8\ncaption=Hi! I am MIMO.\n'),
    ('happy', 'face=happy\nadvance=300\n'),
    ('error', 'face=error\n'),
    ('off', 'face=off\n'),
]
COMMON = 'paired=true\nlink=online\nwifi=connected\nbattery=80\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--avatar', default=str(ESP32 / 'mimo/avatar/mimo_pixel.c'))
    ap.add_argument('--out', default=str(ESP32 / 'mimo/preview'))
    ap.add_argument('--lvgl', help='a local LVGL v9.5.0 checkout, if its download is blocked')
    args = ap.parse_args()
    avatar = Path(args.avatar).resolve()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    build = ESP32 / 'simulator/build-mimo'
    cfg = ['cmake', '-S', str(ESP32 / 'simulator'), '-B', str(build), '-G', 'Ninja',
           '-DCMAKE_BUILD_TYPE=Debug', f'-DMUSE_SIM_AVATAR={avatar}']
    if args.lvgl:
        cfg.append(f'-DFETCHCONTENT_SOURCE_DIR_LVGL={args.lvgl}')
    for cmd in (cfg, ['cmake', '--build', str(build), '--target', 'muse_simulator']):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stdout[-3000:] + r.stderr[-3000:])
    from PIL import Image, ImageDraw

    shots = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, scenario in STATES:
            scn = Path(tmp) / f'{name}.scn'
            scn.write_text(COMMON + scenario)
            ppm = Path(tmp) / f'{name}.ppm'
            r = subprocess.run([str(build / 'muse_simulator'), '--headless', '--scenario', str(scn),
                                '--run-ms', '1200', '--screenshot', str(ppm)],
                               capture_output=True, text=True, env={'SDL_VIDEODRIVER': 'dummy', 'PATH': '/usr/bin:/bin'})
            if r.returncode:
                sys.exit(f'{name}: {r.stderr}')
            img = Image.open(ppm).convert('RGB')
            img.save(out / f'{name}.png')
            shots.append((name, img))
    w, h = shots[0][1].size
    sheet = Image.new('RGB', (4 * w, 2 * (h + 24)), (20, 20, 28))
    draw = ImageDraw.Draw(sheet)
    for i, (name, img) in enumerate(shots):
        x, y = (i % 4) * w, (i // 4) * (h + 24)
        sheet.paste(img, (x, y + 24))
        draw.text((x + 8, y + 6), name, fill=(230, 230, 240))
    sheet.save(out / 'contact_sheet.png')
    print(out / 'contact_sheet.png')


if __name__ == '__main__':
    main()
