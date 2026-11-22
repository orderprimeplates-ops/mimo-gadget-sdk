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

"""Give MIMO its voice relay URL and relay token over the USB cable.

The token is read from a hidden prompt (or a file you name) and sent once as
a console setup command; it's never printed, logged or put on the command
line. The board answers with the token's SHA-256 prefix, which is checked
against the one computed here. Needs firmware built with CONFIG_MIMO_TTS
and a USB data cable; opening the port doesn't reset the board.

    python3 tools/mimo/provision_relay.py --url https://relay.example.com/v1/speak
    python3 tools/mimo/provision_relay.py --token-file ~/mimo-token.txt
    python3 tools/mimo/provision_relay.py --status
    python3 tools/mimo/provision_relay.py --off          # speech off; --on brings it back
    python3 tools/mimo/provision_relay.py --clear        # forget the URL and token

Rotating the token: make a new one on the relay (both stay valid for now),
run this with it, check the prefix it prints matches, speak once, then
revoke the old one on the relay (docs/mimo-tts.md).
"""
import argparse
import getpass
import hashlib
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'muse'))

TOKEN_RE = re.compile(r'^[A-Za-z0-9._~+/=-]{32,256}$')
REPLY_RE = re.compile(r'muse_ble: cmd (mimo\.tts\.[a-z]+)\S* -> (.*)$')


def token_prefix(token):
    return hashlib.sha256(token.encode()).hexdigest()[:8]


def send(board, cmd, value=None, timeout=3.0):
    """Sends one setup command; returns the board's reply text, or None."""
    board.write_line(cmd if value is None else f'{cmd}={value}')
    deadline = time.monotonic() + timeout
    while True:
        line = board.read_line(deadline)
        if line is None:
            return None
        m = REPLY_RE.search(line)
        if m and m.group(1) == cmd:
            return m.group(2).strip()


def provision(board, url=None, token=None, voice=None, on=None, clear=False, out=print):
    """The commands asked for, in order; True if every one was accepted."""
    ok = True
    steps = []
    if clear:
        steps.append(('mimo.tts.clear', None, 'URL and token forgotten'))
    if url:
        steps.append(('mimo.tts.url', url, 'relay URL saved'))
    if token:
        steps.append(('mimo.tts.token', token, f'token saved (sha256 {token_prefix(token)})'))
    if voice:
        steps.append(('mimo.tts.voice', voice, f'voice "{voice}" saved'))
    if on is not None:
        steps.append(('mimo.tts.on', '1' if on else '0', 'speech on' if on else 'speech off'))
    for cmd, value, done in steps:
        reply = send(board, cmd, value)
        if reply == 'ok':
            out(done)
        else:
            ok = False
            out(f'{cmd}: {reply or "no answer (is this firmware built with CONFIG_MIMO_TTS?)"}')
    status = send(board, 'mimo.tts.status')
    out(f'status: {status or "no answer"}')
    if token and status:
        m = re.search(r't:([0-9a-f]{8})', status)
        if not m or m.group(1) != token_prefix(token):
            ok = False
            out('the board reports a different token than the one sent')
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', help='serial port (default: the one board attached)')
    ap.add_argument('--url', help='the relay endpoint, https:// only')
    ap.add_argument('--token-file', help='read the token from this file instead of a hidden prompt')
    ap.add_argument('--no-token', action='store_true', help="don't ask for a token")
    ap.add_argument('--voice', help='voice profile name the relay knows (a-z 0-9 _ -)')
    ap.add_argument('--on', action='store_true')
    ap.add_argument('--off', action='store_true')
    ap.add_argument('--clear', action='store_true', help='forget the URL and token')
    ap.add_argument('--status', action='store_true', help='only show the status')
    args = ap.parse_args()
    if args.url and not re.match(r'^https://[^/@\s]+', args.url):
        sys.exit('the relay URL must start with https:// and name a host')
    token = None
    if not (args.status or args.clear or args.no_token or args.on or args.off or args.voice) or args.token_file:
        if args.token_file:
            token = Path(args.token_file).expanduser().read_text().strip()
        else:
            token = getpass.getpass('Relay token (hidden): ').strip()
        if not TOKEN_RE.match(token):
            sys.exit('that token is not 32-256 characters of A-Z a-z 0-9 . _ ~ + / = -')
    import chat   # tools/muse/chat.py: opens the port without resetting the board
    port = args.port
    if not port:
        found = chat.find_ports()
        if len(found) != 1:
            sys.exit(f'found {len(found)} boards; pass --port')
        port = found[0]
    with chat.Board(port) as board:
        ok = provision(board, url=args.url, token=token, voice=args.voice,
                       on=True if args.on else False if args.off else None, clear=args.clear)
    token = None
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
