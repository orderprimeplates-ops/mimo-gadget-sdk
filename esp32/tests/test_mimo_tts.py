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

"""MIMO spoken replies (CONFIG_MIMO_TTS, docs/mimo-tts.md).

- MimoTtsCore: the relay client's rules (mimo_tts_core.c) on their own.
- MimoSpeechSession: the chat session's production start_tts(), mimo_speak(),
  mimo_pump() and decode(), sliced from muse_chat_session.cpp, against a
  scripted relay, decoding real MP3 with minimp3.
- MimoWiring: off by default, the stock build untouched, nothing secret
  compiled in or logged.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(' '.join(map(str, cmd)) + '\n' + result.stdout + result.stderr)
    return result


def between(source, start, end, include_end=False):
    i = source.index(start)
    j = source.index(end, i)
    return source[i:j + (len(end) if include_end else 0)]


class MimoTtsCore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'core'
        run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror', *SANITIZE,
             '-I', str(MUSE), str(ROOT / 'tests/mimo_tts_core_harness.c'), str(MUSE / 'mimo_tts_core.c'),
             '-o', str(cls.binary)])

    def case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS', result.stdout)

    def test_credentials_are_validated(self): self.case('credentials')
    def test_request_body_is_escaped_json(self): self.case('body')
    def test_long_text_is_clipped_at_a_sentence(self): self.case('clip')
    def test_response_headers(self): self.case('headers')
    def test_oversized_stream_is_cut_at_the_cap(self): self.case('oversize_stream')
    def test_timeouts(self): self.case('timeouts')
    def test_memory_admission_keeps_links_reserve(self): self.case('memory')
    def test_backoff_and_refused_token(self): self.case('gate')


class MimoSpeechSession(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        src = (MUSE / 'muse_chat_session.cpp').read_text()
        constants = between(src, '#define MIC_RATE', '/* ---- Voice task')
        types = between(src, 'enum phase_t', '/* 10 KB')
        state = between(src, 'static uint32_t s_mimo_job;', '#endif')
        resampling = between(src, '/* ---- Resampling ---- */', '/* ---- Turn: dictation ---- */')
        speech = between(src, '/* ---- MIMO speech:', '/* ---- end MIMO speech ---- */', include_end=True)
        tts = src.index('static void start_tts(void)')
        start_tts = src[tts:src.index('\n}\n', tts) + 3]
        decode = between(src, '/* Speaker off: queues the shown message', '/* Ends the turn once the reply is complete')
        prelude = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
#include "host_compat.h"
#include "muse_options.h"
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "muse_chat_priv.h"
#include "mimo_tts.h"
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
static const char *TAG = "test";
''' + constants + types + r'''
static turn_t s_turn;
static std::atomic<uint32_t> s_gen{0};
static int16_t *s_pcm, *s_pcm16;
enum mark_t { M_TTS, M_MP3, M_AUDIO };
static void mark(mark_t) {}
static int s_shown;
static void show_reply_start(const msg_t &) { s_shown++; }
static bool s_speaker = true;
static bool muse_settings_speaker_on(void) { return s_speaker; }
/* s_out: the 2 s reply buffer the voice task plays from. */
typedef int StreamBufferHandle_t;
static StreamBufferHandle_t s_out = 1;
static std::vector<int16_t> s_sb;
static size_t s_mp3_peak;
static size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t) { return OUT_BYTES - s_sb.size() * sizeof(int16_t); }
static size_t xStreamBufferSend(StreamBufferHandle_t, const void *data, size_t n, int) {
    assert(n % 2 == 0 && n <= xStreamBufferSpacesAvailable(s_out));
    const int16_t *p = static_cast<const int16_t *>(data);
    s_sb.insert(s_sb.end(), p, p + n / 2);
    return n;
}
''' + state + resampling + '#if CONFIG_MIMO_TTS\n' + speech + '\n#endif\n' + start_tts + decode
        cases = (ROOT / 'tests/mimo_speech_cases.inc').read_text()
        cases = cases.replace('    mimo_pump();\n    decode();\n',
                              '    mimo_pump();\n    s_mp3_peak = std::max(s_mp3_peak, s_turn.mp3_len);\n    decode();\n')
        (out / 'speech.cpp').write_text(prelude + cases)
        flags = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-function', *SANITIZE, '-DCONFIG_MIMO_TTS=1',
                 '-I', str(ROOT / 'tests'), '-I', str(MUSE),
                 '-I', str(ROOT / 'components/minimp3/include')]
        run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', *flags, '-c', str(MUSE / 'mimo_tts_core.c'),
             '-o', str(out / 'core.o')])
        run([*shlex.split(os.environ.get('CXX', 'c++')), '-std=gnu++17', *flags, str(out / 'speech.cpp'),
             str(out / 'core.o'), '-o', str(out / 'speech')])
        cls.binary = out / 'speech'

    def case(self, name):
        result = subprocess.run([str(self.binary), str(MUSE / 'test_reply.mp3'), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS', result.stdout)

    def test_successful_playback(self): self.case('playback')
    def test_slow_network_and_slow_speaker(self): self.case('slow_network_and_player')
    def test_relay_failure_shows_the_text(self): self.case('relay_failure_before_audio')
    def test_not_set_up_or_speaker_off_is_unchanged(self): self.case('not_set_up')
    def test_relay_timeout_falls_back_to_text(self): self.case('timeout')
    def test_failure_mid_speech_plays_what_came(self): self.case('failure_mid_speech')
    def test_oversized_reply_is_bounded(self): self.case('oversized')
    def test_new_press_interrupts_cleanly(self): self.case('interrupted')
    def test_back_to_back_messages_and_turns(self): self.case('back_to_back')
    def test_junk_audio_does_not_hang(self): self.case('junk_audio')


class MimoWiring(unittest.TestCase):
    def setUp(self):
        self.session = (MUSE / 'muse_chat_session.cpp').read_text()

    def test_off_by_default(self):
        kconfig = (MUSE / 'Kconfig').read_text()
        block = between(kconfig, 'config MIMO_TTS\n', 'config MIMO_TTS_VOICE_DEFAULT')
        self.assertIn('default n', block)
        enabling = [p.name for p in (ROOT / 'devices').glob('sdkconfig.*')
                    if re.search(r'^CONFIG_MIMO_TTS=y', p.read_text(), re.M)]
        self.assertEqual(enabling, ['sdkconfig.mimo-tts'])   # only the opt-in overlay
        for name in ('sdkconfig.defaults', 'devices/sdkconfig.muse', 'devices/sdkconfig.muse-waveshare-s3-175c'):
            self.assertNotIn('MIMO', (ROOT / name).read_text(), name)

    def test_stock_build_compiles_none_of_it(self):
        cmake = (MUSE / 'CMakeLists.txt').read_text()
        self.assertRegex(cmake, r'if\(CONFIG_MIMO_TTS\)\n\s+list\(APPEND srcs "mimo_tts.c" "mimo_tts_core.c"\)')
        # Every MIMO line in the shared files sits inside #if CONFIG_MIMO_TTS.
        for path in (MUSE / 'muse_chat_session.cpp', MUSE / 'muse_ble.c'):
            depth, inside = 0, []
            for no, line in enumerate(path.read_text().splitlines(), 1):
                s = line.strip()
                if s.startswith('#if'):
                    inside.append('CONFIG_MIMO_TTS' in s)
                elif s.startswith('#endif'):
                    inside.pop()
                elif ('mimo' in s.lower()) and not any(inside):
                    self.fail(f'{path.name}:{no} outside #if CONFIG_MIMO_TTS: {s}')
                depth = len(inside)
            self.assertEqual(depth, 0)

    def test_session_hooks(self):
        finish = between(self.session, 'static void turn_finish(void)', '\n}\n')
        self.assertIn('mimo_tts_cancel();', finish)   # a new press or a failure drops the job
        loop = between(self.session, 'static void hatch_task(void *arg)', '/* ---- Public API')
        self.assertRegex(loop, r'start_tts\(\);\n#if CONFIG_MIMO_TTS\n\s+mimo_pump\(\);\n#endif\n\s+decode\(\);')
        start = between(self.session, 'static void start_tts(void)', '\n}\n')
        self.assertRegex(start, r'#if CONFIG_MIMO_TTS\n\s+if \(mimo_speak\(i\)\) \{\n\s+return;')
        self.assertIn('mimo_tts_init();', self.session)

    def test_chat_task_never_waits_on_the_relay(self):
        # The chat task's side copies in and out without blocking (timeouts 0);
        # only the worker touches the network.
        client = (MUSE / 'mimo_tts.c').read_text()
        take = between(client, 'mimo_tts_state_t mimo_tts_take(', '\n}\n')
        self.assertIn('xStreamBufferReceive(s_audio, dst, cap, 0)', take)
        start = between(client, 'uint32_t mimo_tts_start(', '\n}\n')
        self.assertNotRegex(start, r'esp_http|vTaskDelay|portMAX_DELAY\)|ulTaskNotifyTake')
        for name in ('esp_http_client_open', 'esp_http_client_read', 'esp_http_client_fetch_headers'):
            self.assertIn(name, between(client, 'static mimo_tts_err_t fetch(', '\nstatic void worker('))
        self.assertIn('MALLOC_CAP_SPIRAM', between(client, 'void mimo_tts_init(void)', '\n}\n'))

    def test_no_secrets_compiled_in_or_logged(self):
        kconfig = (MUSE / 'Kconfig').read_text()
        mimo_options = re.findall(r'config (MIMO_\w+)', kconfig)
        self.assertFalse([o for o in mimo_options if re.search('TOKEN|KEY|SECRET|URL', o)], mimo_options)
        self.assertNotRegex((ROOT / 'devices/sdkconfig.mimo-tts').read_text(), r'(?i)^[^#]*(token|key|url)')
        client = (MUSE / 'mimo_tts.c').read_text()
        for call in re.findall(r'ESP_LOG\w\(TAG,[^;]*;', client, re.S):
            args = re.sub(r'"(?:\\.|[^"\\])*"', '""', call)   # the format strings aren't values
            self.assertNotRegex(args, r'\b(s_token|token|auth|s_url|url)\b(?!\s*(\(|\[0\]))', call)
        ble = (MUSE / 'muse_ble.c').read_text()
        secret = between(ble, 'bool secret =', 'snprintf(s_last')
        self.assertIn('"mimo.tts.token"', secret)
        self.assertIn('"mimo.tts.url"', secret)
        # The relay must be HTTPS with certificate checks.
        self.assertIn('.crt_bundle_attach = esp_crt_bundle_attach', client)
        self.assertIn('.disable_auto_redirect = true', client)


class MimoProvisioning(unittest.TestCase):
    """tools/mimo/provision_relay.py against a board that answers like muse_ble.c."""

    TOKEN = 'mimo_dev_' + 'Ab3-_x' * 7   # 51 characters of the allowed set

    class FakeBoard:
        def __init__(self, reported_token=None):
            self.sent, self.pending = [], []
            self.reported = reported_token

        def write_line(self, cmd):
            self.sent.append(cmd)
            import hashlib
            name = cmd.split('=', 1)[0]
            if name == 'mimo.tts.status':
                t = hashlib.sha256(self.reported.encode()).hexdigest()[:8] if self.reported else 'none'
                reply = f'on r:set t:{t} ok'
            else:
                reply = 'ok'
            # muse_ble.c never echoes the token or URL value.
            shown = name if name in ('mimo.tts.token', 'mimo.tts.url') else cmd
            self.pending.append(f'I (1234) muse_ble: cmd {shown}{"" if shown == name else ""} -> {reply}')

        def read_line(self, deadline):
            return self.pending.pop(0) if self.pending else None

    def load(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('provision_relay', ROOT / 'tools/mimo/provision_relay.py')
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod

    def test_sends_commands_and_never_prints_the_token(self):
        mod = self.load()
        board, printed = self.FakeBoard(self.TOKEN), []
        ok = mod.provision(board, url='https://relay.example.com/v1/speak', token=self.TOKEN, out=printed.append)
        self.assertTrue(ok, printed)
        self.assertEqual([c.split('=')[0] for c in board.sent], ['mimo.tts.url', 'mimo.tts.token', 'mimo.tts.status'])
        self.assertIn(f'mimo.tts.token={self.TOKEN}', board.sent)
        self.assertNotIn(self.TOKEN, '\n'.join(printed))
        self.assertIn(mod.token_prefix(self.TOKEN), '\n'.join(printed))

    def test_detects_a_board_holding_another_token(self):
        mod = self.load()
        printed = []
        ok = mod.provision(self.FakeBoard('mimo_dev_' + 'z' * 40), token=self.TOKEN, out=printed.append)
        self.assertFalse(ok)
        self.assertIn('different token', '\n'.join(printed))

    def test_rejects_bad_tokens_locally(self):
        mod = self.load()
        for bad in ('short', 'has space ' + 'a' * 40, 'line\nbreak' + 'a' * 40, 'a' * 257):
            self.assertIsNone(mod.TOKEN_RE.match(bad), bad)
        self.assertTrue(mod.TOKEN_RE.match(self.TOKEN))


if __name__ == '__main__':
    unittest.main()
