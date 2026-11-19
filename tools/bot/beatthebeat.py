#!/usr/bin/env python3
"""Beat the Beat's plan for the lockstep test bot (qilinbot.py).

    python3 tools/bot/beatthebeat.py --exe build-rt/qilin_run --disc DISC --out DIR [--seconds N] [--discover]

Plays from boot as far as it can: it recognises each screen from a few
measured features of the picture (160x114), acts as a player would, and
falls back to advancing dialogue on screens it doesn't know, saving a picture
every few seconds to DIR so a run can be looked at afterwards. Its output
is game-derived: keep DIR out of the repository.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qilinbot  # noqa: E402


def orange(r, g, b):
    return r > 200 and 90 < g < 200 and b < 110


def features(f):
    """What the plan tells screens apart by, measured on the 160x114 picture
    (regions placed on the bot's own pictures of each screen)."""
    W, H = f.width, f.height
    digit = f.fraction(73, 12, 87, 20, orange)
    return {
        'white': f.fraction(0, 0, W, H, lambda r, g, b: r > 230 and g > 230 and b > 230, 2),
        'curtain': f.fraction(0, 0, W, H, lambda r, g, b: r > 140 and g < 100 and b < 90, 2),
        'board': f.fraction(45, 36, 115, 75, lambda r, g, b: g > 150 and r < 130 and b < 130, 2),
        'topbar': f.fraction(45, 11, 115, 21, lambda r, g, b: r < 40 and g < 40 and b < 40),
        'bubble': f.fraction(60, 88, 150, 108, lambda r, g, b: r > 235 and g > 235 and b > 235, 2),
        # the countdown's box (lit orange on each beat) and the spikes round a zero
        'digit': digit,
        'spikes': f.fraction(63, 8, 97, 24, orange) - digit * (14 * 8) / (34 * 16),
        'box': f.fraction(72, 4, 88, 14, lambda r, g, b: r > 200 and 120 < g < 200 and b < 110),
        'ring': f.fraction(66, 0, 94, 18, orange),
        'blue': f.fraction(50, 30, 110, 75, lambda r, g, b: b > 170 and r < 120, 2),
        'yes': f.fraction(85, 78, 125, 90, lambda r, g, b: b > 200 and g > 200 and r < 190),
        # the remote icon (a cyan disc, top centre) the tutorials show for A+B
        'remote': f.fraction(62, 2, 98, 32, lambda r, g, b: r < 150 and g > 190 and b > 200),
        'yellow': f.fraction(0, 0, W, H, lambda r, g, b: r > 210 and g > 210 and b < 170, 2),
        'black': f.fraction(0, 0, W, H, lambda r, g, b: r < 20 and g < 20 and b < 20, 2),
    }


def screen(x):
    """The screen, from features(): a name the plan acts on."""
    if x['black'] > 0.9:
        return 'black'
    if x['white'] > 0.6 and x['bubble'] > 0.5:
        return 'strap'
    if x['board'] > 0.15 and x['topbar'] > 0.3:
        return 'board'
    if x['curtain'] > 0.4:
        return 'dialogue'
    if x['box'] > 0.12:
        return 'names' if x['blue'] > 0.12 else 'continue'
    if x['white'] > 0.12 and x['yellow'] > 0.22 and x['ring'] > 0.05:
        return 'files'
    if 0.2 < x['yellow'] < 0.42 and 0.05 < x['black'] < 0.2:
        return 'title'
    return 'unknown'


class Play(qilinbot.Plan):
    trace = None  # (from_s, to_s): log the board's features each frame then
    """Plays: each frame, recognise the screen and act as a player would.
    Rhythm: the beat is heard (onsets in the game's own audio, with the
    game's clock), and presses are put on it; the countdown's zeros are seen
    (the "0" flashes with spikes round it) and the next ones predicted four
    beats on."""

    def start(self, bot):
        super().start(bot)
        self.onsets = qilinbot.Onsets(bot.tps)
        self.beat = qilinbot.Beat(bot.hz)
        self.last_screen, self.since = None, 0.0
        self.next_press = 0.0          # menus and dialogue: not before this (seconds)
        self.pressed_beats = set()     # beats already pressed (clock, rounded)
        self.digit_on = False          # the countdown's box is lit
        self.zero_at = None            # the clock of the last zero seen or predicted
        self.continue_moved = False
        self.next_log = 0.0
        self.my_presses = []
        self.flashes = []
        self.counting_until = 0
        self.since_zero = 0
        self.cycle = None

    def tap(self, f, names, wait_s, length_s=0.08):
        t = self.bot.time_s
        if t < self.next_press:
            return []
        self.next_press = t + wait_s
        return [self.press(names, f.clock, length_s)]

    def press(self, names, at, length_s=0.05):
        self.my_presses.append(at)
        self.my_presses = self.my_presses[-32:]
        return self.bot.press(names, at, length_s)

    def mine(self, t):
        """An onset the bot's own press made (the game's sound for it):
        not the beat. Without this the bot's presses steered its beat."""
        return any(0 <= t - p < 0.2 * self.bot.hz for p in self.my_presses)

    def step(self, f):
        for t in self.onsets.feed(f):
            if not self.mine(t):
                self.beat.add(t)
        x = features(f)
        s = screen(x)
        t = self.bot.time_s
        if s != self.last_screen and s not in ('unknown', 'black'):
            self.bot.say('screen', s, ' '.join('%s=%.2f' % kv for kv in x.items() if kv[1] > 0.01))
            if s != self.last_screen:
                self.beat = qilinbot.Beat(self.bot.hz)  # a new scene: its own beat
            self.last_screen, self.since = s, t
            self.continue_moved = False
        elif t >= self.next_log:
            self.next_log = t + 10.0
            self.bot.say('still', s, 'beat %.3f s' % (self.beat.period / self.bot.hz) if self.beat.ready() else 'no beat yet')
        hz = self.bot.hz
        if s == 'strap' or s == 'files' or s == 'names':
            return self.tap(f, 'A', 1.5)
        if s == 'title':
            return self.tap(f, 'B+A', 2.0, 0.3)
        if s == 'continue':
            if x['yes'] > 0.2:
                return self.tap(f, 'A', 1.5)
            return self.tap(f, 'RIGHT', 0.8)
        if s == 'dialogue':
            if x['remote'] > 0.15:  # "press A and B at the same time"
                return self.tap(f, 'B+A', 1.2, 0.15)
            return self.tap(f, 'A', 1.2)
        if s == 'board':
            return self.board(f, x)
        if s == 'unknown' and t - self.since > 3.0:
            return self.tap(f, 'A', 2.0)  # dialogue somewhere new: move it on
        return []

    @staticmethod
    def two_sided_rows(f):
        """Rows of the countdown's box with a dark stroke at both outer edges
        of the digit (x 77-78 and 81-82, y 13-18): an oval "0" has them in
        most rows, a "3" in two (its top and bottom curves), "2" and "1"
        in one or none. Structure, not stored glyphs."""
        def dark(x, y):
            return sum(f.pixel(x, y)) < 120
        return sum(1 for y in range(13, 19) if (dark(77, y) or dark(78, y)) and (dark(81, y) or dark(82, y)))

    def flash_period(self):
        """The countdown's beat, from its flashes (the median gap near a beat)."""
        gaps = sorted(b - a for a, b in zip(self.flashes, self.flashes[1:]) if 0.3 * self.bot.hz < b - a < 0.9 * self.bot.hz)
        if gaps:
            return gaps[len(gaps) // 2]
        return self.beat.period if self.beat.ready() else None

    def board(self, f, x):
        hz, now = self.bot.hz, f.clock
        out = []
        if self.trace and self.trace[0] <= self.bot.time_s < self.trace[1]:
            self.bot.say('trace digit=%.2f spikes=%.2f ring=%.2f' % (x['digit'], x['spikes'], x['ring']),
                         ''.join('#' if f.pixel(xx, yy)[0] > 200 and 90 < f.pixel(xx, yy)[1] < 200 and f.pixel(xx, yy)[2] < 110 else
                                 ('k' if sum(f.pixel(xx, yy)) < 120 else '.') for yy in range(10, 22) for xx in range(70, 90)))
        lit = x['digit'] > 0.3
        if lit and not self.digit_on:
            # A countdown flash begins (they're on the beat: their spacing is
            # the countdown's beat). A zero: press now if none is due (at most
            # a frame late), and the next zeros are predicted four beats on.
            self.flashes = (self.flashes + [now])[-8:]
            self.counting_until = now + 3 * hz
            self.since_zero += 1
            sides = self.two_sided_rows(f)
            kind = '0' if sides >= 3 else ('3' if sides == 2 else 'other')
            self.bot.say('countdown flash: %s (two-sided rows %d)' % (kind, sides))
            if kind == '0':
                expected = self.zero_at is not None and self.cycle and \
                    abs(now - (self.zero_at + self.cycle * self.flash_period())) < 0.15 * hz
                if not expected:
                    out.append(self.press('A', now))
                # the countdown's length (4 beats, 8 ...): flashes from one zero to the next
                self.cycle = self.since_zero if self.zero_at is not None else None
                self.since_zero = 0
                self.zero_at = now
        self.digit_on = lit
        if now < self.counting_until:
            # The countdown: presses only on the zeros (any other press is early).
            if self.zero_at is not None and self.cycle and self.flash_period():
                nxt = self.zero_at + self.cycle * self.flash_period()
                key = int(nxt // (hz // 100))
                if now <= nxt < now + hz // 20 and key not in self.pressed_beats:
                    self.pressed_beats.add(key)
                    out.append(self.press('A', nxt))
            return out
        if self.beat.ready():
            nb = self.beat.next_after(now)
            key = int(nb // (hz // 100))
            if nb < now + hz // 20 and key not in self.pressed_beats:  # within the next three frames
                self.pressed_beats.add(key)
                out.append(self.press('A', nb))
            return out
        out += self.tap(f, 'A', 1.5)  # no beat heard yet: the board's dialogue
        return out


class Discover(qilinbot.Plan):
    """The press sequence known to reach the calibration, with features
    printed every second: to measure the screens."""

    SCRIPT = [(10, 'A'), (20, 'B+A'), (26, 'A'), (31, 'A'), (36, 'RIGHT'), (38, 'A')] + \
             [(t, 'A') for t in range(44, 60, 2)] + [(60 + 0.5 * k, 'A') for k in range(12)]

    def __init__(self, first_press=None, quiet=False):
        self.first_press, self.quiet = first_press, quiet

    def start(self, bot):
        super().start(bot)
        self.t0, self.next_print, self.todo = None, 0.0, list(self.SCRIPT)
        if self.first_press is not None:
            self.todo[0] = (self.first_press, self.todo[0][1])

    def step(self, f):
        if self.t0 is None:
            self.t0 = f.clock
        t = (f.clock - self.t0) / self.bot.hz
        out = []
        while self.todo and self.todo[0][0] <= t:
            _, names = self.todo.pop(0)
            out.append(self.bot.press(names, f.clock, 0.1 if '+' not in names else 0.5))
        if t >= self.next_print and not self.quiet:
            self.next_print += 1.0
            self.bot.say('features', ' '.join('%s=%.2f' % kv for kv in features(f).items()))
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--disc', required=True)
    ap.add_argument('--out', required=True, help='pictures and the runtime log (game-derived: not for the repo)')
    ap.add_argument('--seconds', type=float, default=None)
    ap.add_argument('--discover', action='store_true')
    ap.add_argument('--first-press', type=float, default=None, help='discover: the first press at this many seconds')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--trace', type=float, nargs=2, default=None, help='log the board each frame between these times')
    ap.add_argument('--env', action='append', default=[], help='KEY=VALUE for the runtime')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    env = dict(kv.split('=', 1) for kv in a.env)
    plan = Discover(a.first_press, a.quiet) if a.discover else Play()
    if a.trace and not a.discover:
        plan.trace = tuple(a.trace)
    bot = qilinbot.Bot(a.exe, a.disc, plan, env=env, log=os.path.join(a.out, 'runtime.log'), seconds=a.seconds,
                       shots=a.out, shot_every_s=1.0 if a.discover else 5.0)
    status, code = bot.run()
    print('[bot] finished: %s (runtime exit %s)' % (status, code))


if __name__ == '__main__':
    main()
