#!/usr/bin/env python3
"""The lockstep test bot's side (recomp/runtime/bot.hpp has the protocol).

Starts qilin_run with QILIN_BOT=1 and a hidden window, and calls a game plan
once per frame with what the frame showed and what was heard; the plan
answers with presses, timed in the game's own clock (time-base ticks), which
the runtime makes exactly then. The game's clock is stopped while the bot
thinks, so a slow plan or a busy computer changes nothing the game sees.

Standard library only. The game plan (e.g. beatthebeat.py) subclasses Plan.
"""
import array
import os
import struct
import subprocess
import sys
import threading

BUTTONS = {
    'LEFT': 0x0001, 'RIGHT': 0x0002, 'DOWN': 0x0004, 'UP': 0x0008, 'PLUS': 0x0010,
    '2': 0x0100, '1': 0x0200, 'B': 0x0400, 'A': 0x0800, 'MINUS': 0x1000, 'HOME': 0x8000,
}


def buttons(names):
    """'A', 'B+A', ... to the remote's bits."""
    bits = 0
    for name in names.split('+'):
        bits |= BUTTONS[name]
    return bits


class Frame:
    """One frame: its picture (RGB, width x height) and the audio heard since
    the last frame, with the game's clock for both."""

    def __init__(self, number, clock, width, height, rgb, audio_first, audio_started, samples):
        self.number, self.clock = number, clock
        self.width, self.height, self.rgb = width, height, rgb
        self.audio_first, self.audio_started = audio_first, audio_started
        self.samples = samples  # array('h'), L R interleaved

    def pixel(self, x, y):
        i = 3 * (y * self.width + x)
        return self.rgb[i], self.rgb[i + 1], self.rgb[i + 2]

    def fraction(self, x0, y0, x1, y1, test, step=1):
        """The share of pixels in [x0, x1) x [y0, y1) for which test(r, g, b)
        holds (every `step`th pixel each way)."""
        hit = total = 0
        rgb, w = self.rgb, self.width
        for y in range(y0, y1, step):
            row = 3 * y * w
            for x in range(x0, x1, step):
                i = row + 3 * x
                total += 1
                if test(rgb[i], rgb[i + 1], rgb[i + 2]):
                    hit += 1
        return hit / total if total else 0.0

    def mean(self, x0, y0, x1, y1):
        s, n = [0, 0, 0], 0
        rgb, w = self.rgb, self.width
        for y in range(y0, y1):
            row = 3 * y * w
            for x in range(x0, x1):
                i = row + 3 * x
                s[0] += rgb[i]; s[1] += rgb[i + 1]; s[2] += rgb[i + 2]; n += 1
        return tuple(v / n for v in s) if n else (0, 0, 0)

    def save_ppm(self, path):
        with open(path, 'wb') as f:
            f.write(b'P6\n%d %d\n255\n' % (self.width, self.height))
            f.write(self.rgb)


class Onsets:
    """Sharp rises in loudness, with the game's clock at each: the audio is cut
    into 2 ms windows (64 samples at 32 kHz), and an onset is a window much
    louder than the 50 ms before it, at least 80 ms after the last onset."""

    WINDOW = 64

    def __init__(self, ticks_per_sample, ratio=6.0, floor=2e5, gap_s=0.08):
        self.tps = ticks_per_sample
        self.ratio, self.floor = ratio, floor
        self.history = []  # the last 25 windows' energies
        self.pending = array.array('h')
        self.pending_first = None  # sample number of pending[0]
        self.pending_started = None  # clock of that sample
        self.last = None
        self.gap = gap_s

    def feed(self, frame):
        """New onsets in this frame's audio: a list of clock times."""
        out = []
        if not frame.samples:
            return out
        if self.pending_first is None or self.pending_first + len(self.pending) // 2 != frame.audio_first:
            self.pending = array.array('h')  # a gap (the first frame): start afresh
            self.pending_first, self.pending_started = frame.audio_first, frame.audio_started
        self.pending.extend(frame.samples)
        w2 = 2 * self.WINDOW
        while len(self.pending) >= w2:
            seg = self.pending[:w2]
            e = sum(v * v for v in seg) / w2
            t = self.pending_started
            past = sum(self.history) / len(self.history) if self.history else 0.0
            if (len(self.history) >= 10 and e > self.ratio * (past + 1e3) and e > self.floor and
                    (self.last is None or t - self.last > self.gap * self.tps * 32000)):
                out.append(t)
                self.last = t
            self.history.append(e)
            if len(self.history) > 25:
                self.history.pop(0)
            del self.pending[:w2]
            self.pending_first += self.WINDOW
            self.pending_started += int(round(self.WINDOW * self.tps))
        return out


class Beat:
    """A steady beat from onsets.

    First a rough period: the median of recent gaps near each other. Then
    the grid itself by least squares: each onset is given its beat number on
    the rough grid, those far off it (sounds between the beats, other
    sounds) are dropped, and a line t = anchor + k * period is fitted through
    the rest. With dozens of onsets the period is right to well under a
    millisecond (averaged gaps wandered by 2%, enough to drift off the beat
    within a dozen beats). Once held, a different rough period takes over
    only after eight updates in a row disagree."""

    def __init__(self, tps_second, min_s=0.25, max_s=1.5):
        self.hz = tps_second
        self.min, self.max = min_s * tps_second, max_s * tps_second
        self.times = []
        self.rough = None
        self.period = None
        self.anchor = None
        self.disagree = 0

    def add(self, t):
        self.times.append(t)
        self.times = self.times[-48:]
        recent = self.times[-16:]
        gaps = sorted(b - a for a, b in zip(recent, recent[1:]) if self.min <= b - a <= self.max)
        if len(gaps) >= 3:
            median = gaps[len(gaps) // 2]
            close = [g for g in gaps if abs(g - median) < 0.08 * median]
            if len(close) >= 3:
                estimate = sum(close) / len(close)
                if self.rough is None or abs(estimate - self.rough) < 0.1 * self.rough:
                    self.rough, self.disagree = estimate, 0
                else:
                    self.disagree += 1
                    if self.disagree >= 8:
                        self.rough, self.disagree = estimate, 0
                        self.times = self.times[-8:]  # the old beat's onsets don't fit the new one
        if self.rough is None:
            return
        # The fit, on the grid through the latest onset.
        ref, p = self.times[-1], self.period or self.rough
        pts = []
        for s in self.times:
            k = round((s - ref) / p)
            if abs(s - (ref + k * p)) < 0.12 * p:
                pts.append((k, s))
        if len(pts) < 4:
            self.period, self.anchor = self.period or self.rough, self.anchor if self.anchor is not None else t
            return
        n = len(pts)
        mk = sum(k for k, _ in pts) / n
        ms = sum(s for _, s in pts) / n
        var = sum((k - mk) ** 2 for k, _ in pts)
        if var == 0:
            return
        slope = sum((k - mk) * (s - ms) for k, s in pts) / var
        if abs(slope - self.rough) < 0.1 * self.rough:
            self.period = slope
            self.anchor = ms - slope * mk  # the grid's beat 0 (relative to ref); any beat serves as anchor

    def ready(self):
        return self.period is not None and self.anchor is not None

    def next_after(self, t):
        """The first beat at or after clock t."""
        k = max(0, -(-(t - self.anchor) // self.period))
        return int(self.anchor + k * self.period)


class Plan:
    """A game plan: step() is called once per frame and returns presses as a
    list of (buttons, from_clock, to_clock)."""

    def start(self, bot):
        self.bot = bot

    def step(self, frame):
        return []


class Bot:
    def __init__(self, exe, disc, plan, env=None, log=None, seconds=None, shots=None, shot_every_s=5.0, stall_s=30.0):
        self.exe, self.disc, self.plan = exe, disc, plan
        self.stall_s = stall_s  # no change in picture or sound for this long: the run has stalled
        self.env = dict(os.environ)
        self.env.update({'QILIN_BOT': '1', 'QILIN_HIDDEN': '1'})
        self.env.update(env or {})
        self.log_path, self.seconds = log, seconds
        self.shots, self.shot_every = shots, shot_every_s
        self.hz = None

    def say(self, *parts):
        print('[bot %8.2f s] ' % (self.time_s if self.hz else 0.0) + ' '.join(str(p) for p in parts), flush=True)

    def press(self, names, at, seconds=0.05):
        """A press of the named buttons at clock `at` for `seconds`."""
        return (buttons(names), int(at), int(at + seconds * self.hz))

    def run(self):
        log = open(self.log_path, 'wb') if self.log_path else subprocess.DEVNULL
        p = subprocess.Popen([self.exe, self.disc], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, env=self.env)
        rd, wr = p.stdout, p.stdin

        def read(n):
            b = rd.read(n)
            if len(b) != n:
                raise EOFError('the runtime ended (its log has why)')
            return b

        if read(4) != b'QBOT':
            raise RuntimeError('not the bot protocol')
        version, self.hz, rate, width, height = struct.unpack('<IQIII', read(24))
        if version != 1:
            raise RuntimeError('protocol version %d' % version)
        self.rate, self.tps = rate, self.hz / rate
        self.plan.start(self)
        start_clock, next_shot = None, 0.0
        last_picture, last_change = None, 0.0
        status = 'ended by the plan'
        try:
            while True:
                number, clock, first, count, started = struct.unpack('<QQQIQ', read(36))
                rgb = read(width * height * 3)
                samples = array.array('h')
                samples.frombytes(read(count * 4))
                frame = Frame(number, clock, width, height, rgb, first, started, samples)
                if start_clock is None:
                    start_clock = clock
                self.time_s = (clock - start_clock) / self.hz
                if self.shots and self.time_s >= next_shot:
                    frame.save_ppm(os.path.join(self.shots, 'shot_%07.1f.ppm' % self.time_s))
                    next_shot += self.shot_every
                # The watchdog: a game that neither draws anything new nor
                # makes a sound for stall_s has stalled (a hang the runtime
                # doesn't see): end the run, keeping its last picture.
                picture = hash(rgb)
                if picture != last_picture or any(abs(v) > 200 for v in samples[::16]):
                    last_change = self.time_s
                last_picture = picture
                if self.time_s - last_change > self.stall_s:
                    status = 'stalled: no new picture or sound for %.0f s' % self.stall_s
                    if self.shots:
                        frame.save_ppm(os.path.join(self.shots, 'stalled.ppm'))
                    break
                presses = None
                if self.seconds is not None and self.time_s >= self.seconds:
                    status = 'time limit'
                    break
                presses = self.plan.step(frame)
                if presses is None:
                    break
                wr.write(struct.pack('<I', len(presses)))
                for bits, a, b in presses:
                    wr.write(struct.pack('<HHQQ', bits, 0, a, b))
                wr.flush()
            wr.write(struct.pack('<I', 0xFFFFFFFF))
            wr.flush()
        except EOFError as e:
            status = str(e)
        p.wait()
        if log is not subprocess.DEVNULL:
            log.close()
        return status, p.returncode
