/*
 * Copyright (c) the MIMO project contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * MIMO speech in the chat session: the production start_tts(), mimo_speak(),
 * mimo_pump() and decode() (sliced from muse_chat_session.cpp by
 * tests/test_mimo_tts.py) against a scripted relay and a player that drains
 * the reply buffer as the voice task does. The MP3 is the firmware's own
 * test_reply.mp3 (24 kHz mono, 48 kbps: the relay's format), decoded by the
 * real minimp3.
 */

/* ---- The relay, scripted ---- */

struct script_t {
    std::vector<uint8_t> audio;   /* what the relay sends */
    size_t per_take = 4096;       /* at most this much per take (the network's pace) */
    int wait_polls = 0;           /* takes that find nothing yet (connecting) */
    mimo_tts_state_t end = MIMO_TTS_DONE;
    mimo_tts_err_t err = MIMO_TTS_OK;
    bool refuse = false;          /* mimo_tts_start says no (not set up, resting, low memory) */
};
static script_t s_script;
static uint32_t s_want, s_next;
static size_t s_pos;
static int s_waited, s_starts, s_cancels, s_takes, s_stale_takes;
static std::string s_sent_text;

uint32_t mimo_tts_start(const char *text, size_t len)
{
    s_starts++;
    if (s_script.refuse) return 0;
    s_sent_text.assign(text, mimo_tts_clip(text, len));
    s_pos = 0;
    s_waited = 0;
    return s_want = ++s_next;
}

mimo_tts_state_t mimo_tts_take(uint32_t job, uint8_t *dst, size_t cap, size_t *got)
{
    *got = 0;
    s_takes++;
    if (!job || job != s_want) {
        s_stale_takes++;
        return MIMO_TTS_FAILED;
    }
    if (s_waited < s_script.wait_polls) {
        s_waited++;
        return MIMO_TTS_STREAMING;
    }
    size_t n = s_script.audio.size() - s_pos;
    n = n < cap ? n : cap;
    n = n < s_script.per_take ? n : s_script.per_take;
    if (n) memcpy(dst, s_script.audio.data() + s_pos, n);
    s_pos += n;
    *got = n;
    return s_pos < s_script.audio.size() ? MIMO_TTS_STREAMING : s_script.end;
}

void mimo_tts_cancel(void)
{
    s_cancels++;
    s_want = 0;
}

mimo_tts_err_t mimo_tts_last_error(void) { return s_script.err; }

/* ---- The player: the voice task reading s_out ---- */

static std::vector<int16_t> s_played;
static size_t s_drain = SIZE_MAX;   /* samples the player takes per step */

static void play(void)
{
    size_t n = s_sb.size() < s_drain ? s_sb.size() : s_drain;
    s_played.insert(s_played.end(), s_sb.begin(), s_sb.begin() + n);
    s_sb.erase(s_sb.begin(), s_sb.begin() + n);
}

/* ---- A turn ---- */

static uint8_t s_mp3_buf[MP3_BUF];
static char s_texts[MAX_MSGS * TEXT_MAX];
static std::vector<uint8_t> s_mp3_file;
static size_t s_expect;   /* test_reply.mp3's samples at 16 kHz, decoded on its own */

/* What turn_finish() does for MIMO speech (the wiring test checks it's there). */
static void finish(void)
{
    mimo_tts_cancel();
    s_mimo_job = 0;
    s_turn.phase = P_IDLE;
    s_turn.tts_msg = -1;
    s_turn.silent = false;
    s_turn.mp3_len = 0;
}

static void begin(std::initializer_list<const char *> messages)
{
    if (s_turn.phase != P_IDLE) finish();   /* turn_start(): a new press ends the turn in progress */
    uint32_t gen = s_turn.gen + 1;
    s_turn = turn_t{};
    s_turn.mp3 = s_mp3_buf;
    s_turn.texts = s_texts;
    s_turn.gen = gen;
    s_gen = gen;
    s_turn.tts_msg = -1;
    s_turn.phase = P_WAIT_REPLY;
    s_played.clear();
    s_sb.clear();
    s_shown = 0;
    int i = 0;
    for (const char *text : messages) {
        msg_t &m = s_turn.msgs[i];
        snprintf(m.id, sizeof(m.id), "msg%d", i);
        m.len = strlen(text);
        m.done = true;
        m.tts = TTS_QUEUED;
        strlcpy(s_texts + i * TEXT_MAX, text, TEXT_MAX);
        i++;
    }
    s_turn.nmsgs = i;
}

/* One pass of hatch_task's loop in P_WAIT_REPLY, then the player. */
static void step(void)
{
    start_tts();
    mimo_pump();
    decode();
    play();
}

static bool all_finished(void)
{
    for (int i = 0; i < s_turn.nmsgs; i++) {
        if (s_turn.msgs[i].tts != TTS_FINISHED) return false;
    }
    return s_turn.tts_msg < 0;
}

static int run(int max_steps = 100000)
{
    int steps = 0;
    while (!all_finished() && steps < max_steps) {
        step();
        steps++;
    }
    size_t pace = s_drain;
    s_drain = SIZE_MAX;   /* the speaker plays out what's left in s_out */
    play();
    s_drain = pace;
    return steps;
}

static bool silent_samples(size_t from, size_t to)
{
    for (size_t i = from; i < to; i++) {
        if (s_played[i]) return false;
    }
    return true;
}

static size_t near(size_t a, size_t b) { return a > b ? a - b : b - a; }
#define FRAME_16K (576 * 16000 / 24000 + 2)

/* ---- Cases ---- */

static void reset_script(void)
{
    s_script = script_t{};
    s_script.audio = s_mp3_file;
    s_drain = SIZE_MAX;
    s_speaker = true;
}

static void playback(void)
{
    reset_script();
    begin({"Hi! I'm MIMO. Want to hear a joke?"});
    run();
    const msg_t &m = s_turn.msgs[0];
    assert(s_starts == 1 && s_sent_text == "Hi! I'm MIMO. Want to hear a joke?");
    assert(m.tts == TTS_FINISHED && !s_turn.silent && s_shown >= 1);
    assert(near(s_played.size(), s_expect) <= FRAME_16K);   /* every frame, at 16 kHz */
    assert(!silent_samples(0, s_played.size()));            /* real audio, not reading-pace silence */
    assert(m.pcm_start == 0 && m.pcm_end == s_turn.pcm_out && m.pcm_frames == s_turn.pcm_out);
    assert(s_turn.down_rate == 24000 && s_turn.kbps == 48);
}

static void slow_network_and_player(void)
{
    /* Bytes trickle in 200 at a time after a connect, and the speaker drains
     * 20 ms a step: nothing is lost or repeated, the chat loop never waits. */
    reset_script();
    s_script.per_take = 200;
    s_script.wait_polls = 40;
    s_drain = 320;
    begin({"Slow and steady."});
    int steps = run();
    assert(steps > 40);
    assert(near(s_played.size(), s_expect) <= FRAME_16K);
}

static void relay_failure_before_audio(void)
{
    /* 500, 401, a network error: no audio at all, so the reply shows at
     * reading pace (silence paces the captions), exactly as without MIMO. */
    for (mimo_tts_err_t err : {MIMO_TTS_ERR_STATUS, MIMO_TTS_ERR_AUTH, MIMO_TTS_ERR_NET, MIMO_TTS_ERR_TYPE}) {
        reset_script();
        s_script.audio.clear();
        s_script.wait_polls = 3;
        s_script.end = MIMO_TTS_FAILED;
        s_script.err = err;
        const char *text = "The relay is down today.";
        begin({text});
        run();
        const msg_t &m = s_turn.msgs[0];
        assert(m.tts == TTS_FINISHED);
        assert(m.pcm_frames == strlen(text) * MIC_RATE / TEXT_CHARS_PER_S);
        assert(silent_samples(0, s_played.size()));
        assert(s_played.size() == m.pcm_frames + TEXT_HOLD_S * MIC_RATE);
    }
}

static void not_set_up(void)
{
    /* Not configured / resting / low memory: mimo_tts_start says no and the
     * message is shown exactly as with MIMO off. */
    reset_script();
    s_script.refuse = true;
    begin({"Plain text."});
    step();
    assert(s_turn.silent && s_mimo_job == 0 && s_takes == 0);
    run();
    assert(silent_samples(0, s_played.size()));
    /* Speaker off: the relay isn't even asked. */
    reset_script();
    s_speaker = false;
    int starts = s_starts;
    begin({"Shh."});
    run();
    assert(s_starts == starts && silent_samples(0, s_played.size()));
}

static void timeout(void)
{
    /* The relay accepts, then never answers: the worker's first-byte deadline
     * ends the job as FAILED (mimo_tts_core's rule, tested on its own). Until
     * then the message waits as speech; after, it's shown as text. */
    reset_script();
    s_script.audio.clear();
    s_script.wait_polls = 4000;   /* 8 s of 2 ms polls */
    s_script.end = MIMO_TTS_FAILED;
    s_script.err = MIMO_TTS_ERR_TIMEOUT;
    begin({"Hello?"});
    for (int i = 0; i < 3999; i++) step();
    assert(s_turn.msgs[0].tts == TTS_ACTIVE && !s_turn.silent && s_played.empty());
    run();
    assert(s_turn.msgs[0].tts == TTS_FINISHED && silent_samples(0, s_played.size()));
}

static void failure_mid_speech(void)
{
    /* The connection drops halfway: what arrived plays, then the message ends. */
    reset_script();
    s_script.audio.resize(s_mp3_file.size() / 2);
    s_script.end = MIMO_TTS_FAILED;
    s_script.err = MIMO_TTS_ERR_NET;
    begin({"Cut off."});
    run();
    assert(s_turn.msgs[0].tts == TTS_FINISHED && !s_turn.silent);
    assert(s_played.size() > s_expect / 3 && s_played.size() < s_expect * 2 / 3);
    assert(!silent_samples(0, s_played.size()));
}

static void oversized(void)
{
    /* A relay that keeps sending: the worker stops at MIMO_TTS_MAX_BYTES and
     * reports TOO_LARGE; the audio it kept plays, nothing overflows the
     * session's MP3 buffer (MP3_BUF), and the turn ends. */
    reset_script();
    s_script.audio.clear();
    while (s_script.audio.size() + s_mp3_file.size() <= MIMO_TTS_MAX_BYTES) {
        s_script.audio.insert(s_script.audio.end(), s_mp3_file.begin(), s_mp3_file.end());
    }
    s_script.end = MIMO_TTS_FAILED;
    s_script.err = MIMO_TTS_ERR_TOO_LARGE;
    s_script.per_take = 16384;
    begin({"A very long story."});
    int steps = run();
    assert(steps < 100000 && s_turn.msgs[0].tts == TTS_FINISHED);
    size_t copies = s_script.audio.size() / s_mp3_file.size();
    assert(near(s_played.size(), copies * s_expect) <= copies * FRAME_16K);
    assert(s_mp3_peak <= MP3_BUF);
    /* Even a buffer that fills: tts_data()'s bound holds. */
    static_assert(MIMO_TTS_MAX_BYTES < MP3_BUF, "a whole reply fits the session's MP3 buffer");
}

static void interrupted(void)
{
    /* A new press mid-speech: turn_finish() cancels the job, the next turn
     * starts clean and never reads the old job's audio. */
    reset_script();
    s_script.per_take = 1000;
    s_drain = 320;
    begin({"First answer, interrupted."});
    for (int i = 0; i < 6; i++) step();
    uint32_t old = s_mimo_job;
    assert(old && !s_played.empty() && s_turn.msgs[0].tts == TTS_ACTIVE);
    int cancels = s_cancels;
    begin({"Second answer."});   /* the press */
    assert(s_cancels == cancels + 1 && s_mimo_job == 0 && s_want == 0);
    assert(s_turn.mp3_len == 0 && s_played.empty());
    int stale = s_stale_takes;
    reset_script();
    run();
    assert(s_stale_takes == stale && s_sent_text == "Second answer.");
    assert(near(s_played.size(), s_expect) <= FRAME_16K);
}

static void back_to_back(void)
{
    /* Two messages in one turn, then three turns in a row: each message gets
     * its own job, in order, with nothing carried over. */
    reset_script();
    begin({"One.", "Two!"});
    run();
    assert(s_sent_text == "Two!" && s_turn.msgs[0].tts == TTS_FINISHED && s_turn.msgs[1].tts == TTS_FINISHED);
    assert(near(s_played.size(), 2 * s_expect) <= 2 * FRAME_16K);
    assert(s_turn.msgs[1].pcm_start == s_turn.msgs[0].pcm_end);
    for (int turn = 0; turn < 3; turn++) {
        int starts = s_starts;
        begin({"Again!"});
        run();
        assert(s_starts == starts + 1);
        assert(near(s_played.size(), s_expect) <= FRAME_16K);
        assert(s_turn.msgs[0].pcm_start == 0);
    }
    /* A failure between successes doesn't stick to the next message. */
    reset_script();
    s_script.audio.clear();
    s_script.end = MIMO_TTS_FAILED;
    s_script.err = MIMO_TTS_ERR_NET;
    begin({"Bad one."});
    run();
    assert(silent_samples(0, s_played.size()));
    reset_script();
    begin({"Good one."});
    run();
    assert(!s_turn.silent && near(s_played.size(), s_expect) <= FRAME_16K);
}

static void junk_audio(void)
{
    /* A relay bug: 200 audio/mpeg with no MP3 frames in it. The message ends
     * (no hang, no crash) and the turn can move on. */
    reset_script();
    s_script.audio.assign(5000, 0x55);
    begin({"Garbled."});
    int steps = run();
    assert(steps < 100 && s_turn.msgs[0].tts == TTS_FINISHED && s_played.empty());
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f))) s_mp3_file.insert(s_mp3_file.end(), buf, buf + n);
    fclose(f);
    /* The reference: the file decoded on its own and resampled the same way. */
    static mp3dec_t dec;
    mp3dec_init(&dec);
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME], out[MINIMP3_MAX_SAMPLES_PER_FRAME + 8];
    resampler_t rs;
    resampler_init(&rs, 24000, MIC_RATE);
    for (size_t off = 0; off < s_mp3_file.size();) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&dec, s_mp3_file.data() + off, (int)(s_mp3_file.size() - off), pcm, &info);
        if (!info.frame_bytes) break;
        off += info.frame_bytes;
        s_expect += resample(&rs, pcm, samples, out);
    }
    assert(s_expect > MIC_RATE * 3);
    s_pcm = new int16_t[MINIMP3_MAX_SAMPLES_PER_FRAME];
    s_pcm16 = new int16_t[MINIMP3_MAX_SAMPLES_PER_FRAME + 8];

    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "playback", playback }, { "slow_network_and_player", slow_network_and_player },
        { "relay_failure_before_audio", relay_failure_before_audio }, { "not_set_up", not_set_up },
        { "timeout", timeout }, { "failure_mid_speech", failure_mid_speech }, { "oversized", oversized },
        { "interrupted", interrupted }, { "back_to_back", back_to_back }, { "junk_audio", junk_audio },
    };
    for (auto &c : cases) {
        if (!strcmp(argv[2], c.name)) {
            c.fn();
            puts("PASS");
            return 0;
        }
    }
    fprintf(stderr, "unknown case %s\n", argv[2]);
    return 2;
}
