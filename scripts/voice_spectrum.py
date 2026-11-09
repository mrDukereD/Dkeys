# Спектрограмма и тон голоса (микрофон)
"""
Спектрограмма микрофона в реальном времени, главные тоны (Гц и ноты) и контроль строя для вокала.

Что показывает
  * слева - спектрограмма за последние 10 секунд (частота по вертикали - как клавиши рояля, нижние ноты внизу);
    белая цепочка точек - ваш тон: зелёная - в строе, жёлтая - почти, оранжевая - мимо, красная - вне тональности;
  * в середине - мгновенный спектр с 4 главными тонами (Гц, нота, отклонение в центах) и пунктиром - шум микрофона;
  * справа - нота крупно, шкала строя (+-50 центов), уровень сигнала, статистика попадания и шум.
Первые 2 секунды (и по клавише N) идёт калибровка шума: помолчите - скрипт запомнит спектр шума микрофона,
покажет его уровень, сетевой фон (50/100/150 Гц и т.п.) и во время пения - запас над шумом (SNR).

Относительный режим (O): оценивается не точность по абсолютной высоте, а чистота переходов - скрипт вычисляет ваш собственный
  сдвиг строя (по последним ~6 с пения) и вычитает его: если вы поёте стабильно чуть выше или ниже, но интервалы верные, всё зелёное.
Клавиши: Пробел - пауза   O - относительный режим   N - заново измерить шум   K / Shift+K - тоника выше / ниже   M - лад (мажор / минор / все ноты)
         + / - - контрастность   C - палитра   R - сбросить статистику   S - снимок PNG   Q или Esc - выход
Запуск: python voice_spectrum.py [--list] [--device НОМЕР] [--rate ГЦ] [--demo]
Нужно: numpy, matplotlib, sounddevice (pip install sounddevice). --demo - синтетический сигнал без микрофона.
"""
import argparse
import math
import os
import sys
import threading
import time

import numpy as np

FMIN, FMAX = 70.0, 4000.0            # диапазон показа, Гц
OCT = math.log2(FMAX / FMIN)         # вертикальная ось - октавы от FMIN
NB = 360                             # строк в спектрограмме
HIST = 10.0                          # секунд истории
FPS = 24
WIN = 4096                           # окно спектра: ~93 мс (короче - меньше задержка)
NFFT = 8192                          # дополнение нулями до 8192 - точнее частота пиков
DB_LO, DB_HI = -110.0, -10.0

NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
SOL = ['До', 'До#', 'Ре', 'Ре#', 'Ми', 'Фа', 'Фа#', 'Соль', 'Соль#', 'Ля', 'Ля#', 'Си']
SCALES = [('мажор', [0, 2, 4, 5, 7, 9, 11]), ('минор', [0, 2, 3, 5, 7, 8, 10]), ('все ноты', None)]
CMAPS = ['magma', 'inferno', 'viridis', 'turbo']

BG, PANEL, FG, DIM = '#0b0f17', '#121826', '#d7e0ee', '#7a869a'
C_GOOD, C_OK, C_BAD, C_OUT = '#38e07b', '#f4d03f', '#ff9f43', '#ff4d6d'
PEAKC = ['#4dd2ff', '#ffd166', '#ff7ad9', '#9dff7a']
STATUS_COL = {0: DIM, 1: C_GOOD, 2: C_OK, 3: C_BAD, 4: C_OUT}


def midi_of(f):
    return 69.0 + 12.0 * math.log2(f / 440.0)


def hz_of(m):
    return 440.0 * 2.0 ** ((m - 69.0) / 12.0)


def note_name(n):
    return NAMES[n % 12] + str(n // 12 - 1)


def ypos(f):
    return math.log2(f / FMIN)


# --------------------------------------------------------------------------- источники звука
class Mic:
    def __init__(self, device, rate):
        import sounddevice as sd
        info = sd.query_devices(device, 'input')
        self.fs = int(rate or info['default_samplerate'])
        self.name = info['name']
        self.size = 1 << 16
        self.buf = np.zeros(self.size, np.float32)
        self.pos = 0
        self.lock = threading.Lock()
        self.stream = sd.InputStream(device=device, channels=1, samplerate=self.fs, dtype='float32', callback=self._cb)
        self.stream.start()

    def _cb(self, indata, frames, t, status):
        x = indata[:, 0]
        n = len(x)
        with self.lock:
            i = self.pos % self.size
            k = min(n, self.size - i)
            self.buf[i:i + k] = x[:k]
            if k < n:
                self.buf[:n - k] = x[k:]
            self.pos += n

    def latest(self, n):
        with self.lock:
            idx = (self.pos - n + np.arange(n)) % self.size
            return self.buf[idx].copy()


class Demo:
    """Синтетика: тишина с шумом и фоном 50 Гц, потом несколько нот (одна фальшивит, одна вне тональности A-минор)."""
    NOTES = [(2.6, 4.1, 57, 0), (4.3, 5.6, 60, 0), (5.8, 7.1, 64, -32), (7.3, 8.6, 62, 0), (8.8, 10.1, 66, 0), (10.3, 12.2, 57, 12)]

    def __init__(self, rate):
        self.fs = int(rate or 44100)
        self.name = 'демо (синтетический сигнал)'
        self.clock = time.time
        self.t0 = self.clock()
        self.rng = np.random.default_rng(1)

    def latest(self, n):
        t = (self.clock() - self.t0) - (n - 1 - np.arange(n)) / self.fs
        tm = np.mod(t, 14.0)
        x = self.rng.normal(0, 0.0012, n)
        x += 0.004 * np.sin(2 * np.pi * 50 * t) + 0.002 * np.sin(2 * np.pi * 100 * t) + 0.0012 * np.sin(2 * np.pi * 150 * t)
        for a, b, midi, det in self.NOTES:
            m = (tm >= a) & (tm < b)
            if not m.any():
                continue
            f = hz_of(midi + (det + float(os.environ.get('DEMO_SHIFT', '0'))) / 100.0)
            tt = t[m]
            env = np.minimum(1, (tm[m] - a) / 0.08) * np.minimum(1, (b - tm[m]) / 0.12)
            ph = 2 * np.pi * f * tt + (f * 0.012 / 5.5) * np.sin(2 * np.pi * 5.5 * tt)
            s = sum(amp * np.sin((k + 1) * ph) for k, amp in enumerate([1, .55, .38, .22, .14, .09]))
            x[m] += 0.16 * env * s
        return x.astype(np.float32)


# --------------------------------------------------------------------------- анализ
class Engine:
    def __init__(self, fs):
        self.fs = fs
        self.win = np.hanning(WIN)
        self.wsum = self.win.sum() / 2
        self.freqs = np.fft.rfftfreq(NFFT, 1.0 / fs)
        self.ylog = np.linspace(0, OCT, NB)
        self.flog = FMIN * 2.0 ** self.ylog
        self.cols = int(HIST * FPS)
        self.img = np.full((NB, self.cols), DB_LO, np.float32)
        self.pitch_y = np.full(self.cols, np.nan)
        self.pitch_s = np.zeros(self.cols, np.int8)
        self.floor_lin = None
        self.floor_log = np.full(NB, np.nan)
        self.floor_level = None
        self.hum = []
        self.cal_end = 0.0
        self.cal_pow = None
        self.cal_lv = []
        self.tonic = 0
        self.mode = 2
        self.rel = False
        self.off = 0.0                 # сдвиг строя певца, центы (в относительном режиме)
        self.chist = []                # (время, отклонение в центах от ближайшей ноты) за последние секунды
        self.hits = 0
        self.total = 0
        self.last_t = None
        self.f0s = []
        self.voiced_t = 0.0
        # результаты последнего шага
        self.level_db = -120.0
        self.peak_db = -120.0
        self.col = np.full(NB, DB_LO)
        self.db = np.full(len(self.freqs), DB_LO)
        self.peaks = []
        self.f0 = None
        self.conf = 0.0
        self.note = None            # (midi, cents, status, target_name)
        self.hold_peak = -120.0
        self.hold_t = 0.0

    # --- шум
    def calibrate(self, now):
        self.cal_end = now + 2.0
        self.cal_pow = np.zeros(len(self.freqs))
        self.cal_n = 0
        self.cal_lv = []

    def calibrating(self, now):
        return now < self.cal_end

    def _finish_cal(self):
        if self.cal_n == 0:
            self.cal_pow = None
            return
        p = self.cal_pow / self.cal_n
        self.floor_lin = 10 * np.log10(p + 1e-18)
        self.floor_level = float(np.mean(self.cal_lv))
        self.floor_log = np.interp(self.flog, self.freqs, self.floor_lin)
        # сетевой фон: узкие пики шума ниже 1 кГц
        lo, hi = np.searchsorted(self.freqs, 40), np.searchsorted(self.freqs, 1000)
        d = self.floor_lin[lo:hi]
        base = np.median(d)
        idx = [i for i in range(1, len(d) - 1) if d[i] > d[i - 1] and d[i] >= d[i + 1] and d[i] > base + 14]
        idx.sort(key=lambda i: -d[i])
        hum = sorted(int(round(self.freqs[lo + i])) for i in idx[:3])
        self.hum = hum
        self.cal_pow = None

    # --- ноты
    def degrees(self):
        return SCALES[self.mode][1]

    def in_key(self, n):
        deg = self.degrees()
        return deg is None or (n - self.tonic) % 12 in deg

    def nearest_scale(self, m):
        best, bd = None, 99
        for n in range(int(m) - 7, int(m) + 8):
            if self.in_key(n) and abs(n - m) < bd:
                best, bd = n, abs(n - m)
        return best

    # --- один шаг
    def step(self, x, now):
        if self.last_t is None:
            self.last_t = now
        shift = int(max(1, min(10, round((now - self.last_t) * FPS))))
        self.last_t = now
        seg = x[-WIN:].astype(np.float64)
        tail = x[-2048:].astype(np.float64)
        rms = math.sqrt(float(np.mean(tail * tail)))
        self.level_db = 20 * math.log10(rms + 1e-9)
        self.peak_db = 20 * math.log10(float(np.max(np.abs(tail))) + 1e-9)
        if self.peak_db > self.hold_peak or now - self.hold_t > 1.5:
            self.hold_peak, self.hold_t = self.peak_db, now
        mag = np.abs(np.fft.rfft(seg * self.win, NFFT)) / self.wsum
        db = 20 * np.log10(mag + 1e-9)
        self.db = db
        if self.cal_pow is not None:
            if self.calibrating(now):
                self.cal_pow += mag * mag
                self.cal_n += 1
                self.cal_lv.append(self.level_db)
            else:
                self._finish_cal()
        col = np.interp(self.flog, self.freqs, db).astype(np.float32)
        self.col = col
        self.img[:, :-shift] = self.img[:, shift:]
        self.img[:, -shift:] = col[:, None]
        self.peaks = self._peaks(db)
        f0, conf = self._yin(x)
        floor = self.floor_level if self.floor_level is not None else -70.0
        voiced = f0 is not None and conf > 0.85 and self.level_db > floor + 8 and not self.calibrating(now)
        status = 0
        if voiced:
            self.f0s = (self.f0s + [f0])[-3:]
            fm = float(np.median(self.f0s))
            self.f0, self.conf, self.voiced_t = fm, conf, now
            m = midi_of(fm)
            n0 = int(round(m))
            self.chist = [(t_, c_) for t_, c_ in self.chist if now - t_ < 6.0] + [(now, (m - n0) * 100.0)]
            if self.rel and len(self.chist) >= 8:          # круговое среднее отклонений: сдвиг строя без скачка на границе +-50 центов
                ang = np.array([c_ for _, c_ in self.chist]) * (2 * math.pi / 100.0)
                self.off = math.atan2(float(np.mean(np.sin(ang))), float(np.mean(np.cos(ang)))) * 100.0 / (2 * math.pi)
            elif not self.rel:
                self.off = 0.0
            m -= self.off / 100.0
            n = int(round(m))
            cents = (m - n) * 100.0
            tgt = n
            if self.in_key(n):
                status = 1 if abs(cents) <= 10 else 2 if abs(cents) <= 25 else 3
            else:
                status = 4
                tgt = self.nearest_scale(m)
            self.note = (m, cents, status, note_name(tgt) if tgt is not None else '')
            self.total += 1
            if status in (1, 2):
                self.hits += 1
        else:
            self.f0s = []
            if now - self.voiced_t > 4.0:
                self.chist = []
            if now - self.voiced_t > 0.3:
                self.f0 = None
                self.note = None
        py = ypos(self.f0 * 2.0 ** (-self.off / 1200.0)) if (voiced and FMIN <= self.f0 <= FMAX) else np.nan
        self.pitch_y[:-shift] = self.pitch_y[shift:]
        self.pitch_y[-shift:] = py
        self.pitch_s[:-shift] = self.pitch_s[shift:]
        self.pitch_s[-shift:] = status

    def _peaks(self, db):
        lo, hi = np.searchsorted(self.freqs, FMIN), np.searchsorted(self.freqs, FMAX)
        d = db[lo:hi]
        fr = self.freqs[lo:hi]
        m = (d[1:-1] > d[:-2]) & (d[1:-1] >= d[2:])
        idx = np.nonzero(m)[0] + 1
        if idx.size == 0:
            return []
        floor = np.interp(fr[idx], self.freqs, self.floor_lin) if self.floor_lin is not None else np.full(idx.size, -95.0)
        ok = d[idx] > np.maximum(floor + 10, -85)
        idx = idx[ok]
        idx = idx[np.argsort(-d[idx])]
        res = []
        df = self.freqs[1] - self.freqs[0]
        for i in idx:
            a, b, c = d[i - 1], d[i], d[i + 1]
            den = a - 2 * b + c
            p = 0.5 * (a - c) / den if den != 0 else 0.0
            f = fr[i] + p * df
            if f < FMIN or f > FMAX:
                continue
            if any(abs(math.log2(f / g)) < 0.06 for g, _ in res):
                continue
            res.append((f, b - 0.25 * (a - c) * p))
            if len(res) == 4:
                break
        return res

    def _yin(self, x):
        fs = self.fs
        tmax = int(fs / 70.0) + 1
        tmin = max(2, int(fs / 1100.0))
        M = 1024
        L = M + tmax
        if len(x) < L:
            return None, 0.0
        s = x[-L:].astype(np.float64)
        nfft = 1 << int(math.ceil(math.log2(L + M)))
        r = np.fft.irfft(np.conj(np.fft.rfft(s[:M], nfft)) * np.fft.rfft(s, nfft), nfft)[:tmax + 1]
        e = np.concatenate(([0.0], np.cumsum(s * s)))
        taus = np.arange(tmax + 1)
        d = (e[M] - e[0]) + (e[taus + M] - e[taus]) - 2 * r
        d[0] = 0.0
        cm = np.cumsum(d[1:])
        dp = np.ones(tmax + 1)
        dp[1:] = d[1:] * taus[1:] / np.maximum(cm, 1e-12)
        below = np.nonzero(dp[tmin:tmax] < 0.15)[0]
        if below.size:
            t = below[0] + tmin
            while t + 1 < tmax and dp[t + 1] < dp[t]:
                t += 1
        else:
            t = int(np.argmin(dp[tmin:tmax])) + tmin
        conf = 1.0 - float(dp[t])
        if 1 < t < tmax - 1:
            a, b, c = dp[t - 1], dp[t], dp[t + 1]
            den = a - 2 * b + c
            if den != 0:
                t = t + 0.5 * (a - c) / den
        return fs / t, conf


# --------------------------------------------------------------------------- окно
class Ui:
    def __init__(self, plt, mpl, eng, src, label):
        self.plt, self.eng, self.src = plt, eng, src
        self.paused = False
        self.bg = None
        self.cmap_i = 0
        self.vmin = -100.0
        for k in [k for k in mpl.rcParams if k.startswith('keymap.')]:
            mpl.rcParams[k] = []
        mpl.rcParams['font.family'] = 'DejaVu Sans'
        fig = plt.figure(figsize=(14.4, 7.8), facecolor=BG)
        self.fig = fig
        try:
            fig.canvas.manager.set_window_title('Спектрограмма и тон голоса')
        except Exception:
            pass
        gs = fig.add_gridspec(1, 2, width_ratios=[6.7, 2.35], left=0.055, right=0.985, top=0.915, bottom=0.085, wspace=0.05)
        lg = gs[0].subgridspec(2, 1, height_ratios=[3.0, 1.75], hspace=0.2)
        ax = fig.add_subplot(lg[0], facecolor='#05070c')
        asp = fig.add_subplot(lg[1], facecolor='#05070c')
        right = gs[1].subgridspec(5, 1, height_ratios=[3.5, 1.0, 2.0, 1.15, 2.4], hspace=0.28)
        self.ax, self.asp = ax, asp
        self.acard = fig.add_subplot(right[0], facecolor=PANEL)
        self.acent = fig.add_subplot(right[1], facecolor=PANEL)
        self.apeaks = fig.add_subplot(right[2], facecolor=PANEL)
        self.alvl = fig.add_subplot(right[3], facecolor=PANEL)
        self.astat = fig.add_subplot(right[4], facecolor=PANEL)
        for a in (self.acard, self.acent, self.apeaks, self.alvl, self.astat):
            for sp in a.spines.values():
                sp.set_color('#1f2a3d')
            a.set_xticks([])
            a.set_yticks([])

        fig.text(0.055, 0.945, 'СПЕКТРОГРАММА И ТОН ГОЛОСА', color=FG, fontsize=15, weight='bold')
        fig.text(0.055, 0.922, label, color=DIM, fontsize=9)
        self.t_state = fig.text(0.985, 0.945, '', color=C_OK, fontsize=12, weight='bold', ha='right')

        # спектрограмма
        self.lut = None
        self.set_lut()
        self.im = ax.imshow(self.rgba(), origin='lower', aspect='auto', extent=(-HIST, 0, 0, OCT), interpolation='nearest')
        ax.set_xlim(-HIST, 0)
        ax.set_ylim(0, OCT)
        ax.set_xticks(range(-10, 1, 2))
        ax.set_xticklabels(['%d с' % v if v else 'сейчас' for v in range(-10, 1, 2)])
        self.xt = np.linspace(-HIST, 0, eng.cols)
        self.sc = ax.scatter([], [], s=9, zorder=5)
        self.sc_head = ax.scatter([], [], s=70, zorder=6, edgecolors='white', linewidths=1.2)
        for a in (ax, asp):
            a.tick_params(colors=DIM, labelsize=8)
            for sp in a.spines.values():
                sp.set_color('#1f2a3d')
        ax.tick_params(axis='y', labelsize=7.5)

        # спектр
        asp.set_xlim(0, OCT)
        asp.set_ylim(DB_LO, DB_HI)
        asp.set_ylabel('дБ', color=DIM, fontsize=8)
        asp.yaxis.grid(True, color='#1c2638', lw=0.6)
        asp.set_axisbelow(True)
        from matplotlib.patches import Polygon, Rectangle
        self.poly = Polygon([[0, DB_LO], [OCT, DB_LO]], closed=True, facecolor='#2b7fff', alpha=0.28, edgecolor='none')
        asp.add_patch(self.poly)
        (self.l_spec,) = asp.plot([], [], color='#69b4ff', lw=1.3)
        (self.l_floor,) = asp.plot([], [], color='#ff6b6b', lw=1.0, ls='--', alpha=0.9)
        self.pk_sc = asp.scatter([], [], s=28, zorder=5)
        self.pk_txt = [asp.text(0, 0, '', fontsize=8.5, color=PEAKC[i], va='bottom', ha='center', weight='bold', clip_on=True,
                       bbox=dict(boxstyle='round,pad=0.15', fc='#05070c', ec='none', alpha=0.78)) for i in range(4)]
        asp.text(0.008, 0.97, 'СПЕКТР', transform=asp.transAxes, color=DIM, fontsize=8.5, ha='left', va='top')
        asp.text(0.992, 0.97, '- - шум микрофона', transform=asp.transAxes, color='#ff6b6b', fontsize=8, ha='right', va='top')

        # карточка ноты
        c = self.acard
        c.set_xlim(0, 1)
        c.set_ylim(0, 1)
        c.text(0.5, 0.94, 'НОТА', color=DIM, fontsize=9, ha='center', va='top')
        self.t_note = c.text(0.5, 0.58, '-', color=DIM, fontsize=62, weight='bold', ha='center', va='center')
        self.t_sol = c.text(0.5, 0.33, '', color=FG, fontsize=17, ha='center', va='center')
        self.t_hz = c.text(0.5, 0.19, '', color=FG, fontsize=15, ha='center', va='center', family='DejaVu Sans Mono')
        self.t_hint = c.text(0.5, 0.06, '', color=DIM, fontsize=10.5, ha='center', va='center')

        # шкала строя
        m = self.acent
        m.set_xlim(-50, 50)
        m.set_ylim(0, 1)
        m.axvspan(-25, 25, ymin=0.15, ymax=0.85, color=C_OK, alpha=0.18, lw=0)
        m.axvspan(-10, 10, ymin=0.15, ymax=0.85, color=C_GOOD, alpha=0.30, lw=0)
        for v in (-50, -25, 0, 25, 50):
            m.text(v, 0.02, '%+d' % v if v else '0', color=DIM, fontsize=7.5, ha='center', va='bottom')
        (self.l_needle,) = m.plot([0, 0], [0.2, 0.9], color=DIM, lw=4, solid_capstyle='round')
        m.text(0.02, 0.95, 'центы', color=DIM, fontsize=8, ha='left', va='top', transform=m.transAxes)
        self.t_cents = m.text(0.98, 0.95, '', color=FG, fontsize=9, ha='right', va='top', transform=m.transAxes)

        # главные тоны
        p = self.apeaks
        p.set_xlim(0, 1)
        p.set_ylim(0, 1)
        p.text(0.05, 0.95, 'ГЛАВНЫЕ ТОНА', color=DIM, fontsize=9, va='top')
        self.t_pk = [p.text(0.05, 0.76 - i * 0.185, '', color=PEAKC[i], fontsize=10.5, va='center', family='DejaVu Sans Mono') for i in range(4)]

        # уровень
        l = self.alvl
        l.set_xlim(-90, 0)
        l.set_ylim(0, 1)
        self.r_lvl = Rectangle((-90, 0.38), 0, 0.28, color=C_GOOD)
        l.add_patch(self.r_lvl)
        (self.l_hold,) = l.plot([-90, -90], [0.34, 0.70], color='white', lw=2)
        (self.l_nf,) = l.plot([np.nan, np.nan], [0.30, 0.74], color='#ff6b6b', lw=2, ls='--')
        l.text(0.02, 0.95, 'УРОВЕНЬ', color=DIM, fontsize=9, ha='left', va='top', transform=l.transAxes)
        self.t_lvl = l.text(0.98, 0.95, '', color=FG, fontsize=9, ha='right', va='top', transform=l.transAxes)
        for v in (-80, -60, -40, -20, 0):
            l.text(v, 0.04, str(v), color=DIM, fontsize=7, ha='center', va='bottom')

        # статистика
        s = self.astat
        s.set_xlim(0, 1)
        s.set_ylim(0, 1)
        s.text(0.05, 0.95, 'СТРОЙ И ШУМ', color=DIM, fontsize=9, va='top')
        self.t_key = s.text(0.05, 0.74, '', color=FG, fontsize=12, weight='bold', va='center')
        self.t_acc = s.text(0.05, 0.60, '', color=FG, fontsize=10.5, va='center')
        self.t_floor = s.text(0.05, 0.43, '', color=FG, fontsize=10, va='center')
        self.t_snr = s.text(0.05, 0.28, '', color=FG, fontsize=10, va='center')
        self.t_hum = s.text(0.05, 0.12, '', color=FG, fontsize=10, va='center')

        fig.text(0.5, 0.025, 'Пробел пауза  ·  O относительный режим  ·  N шум  ·  K тоника (Shift - вниз)  ·  M лад  ·  +/- контраст  ·  C палитра  ·  R сброс  ·  S снимок  ·  Q выход',
                 color=DIM, fontsize=9, ha='center')
        from matplotlib.collections import LineCollection
        self.grid1 = LineCollection([], colors='white', alpha=0.08, linewidths=0.6)
        self.grid2 = LineCollection([], colors='white', alpha=0.08, linewidths=0.6)
        ax.add_collection(self.grid1)
        asp.add_collection(self.grid2)
        self.set_ticks()
        # быстрая перерисовка: статичный фон запоминается, каждый кадр рисуются только меняющиеся элементы
        self.dyn = [self.im, self.grid1, self.grid2, self.poly, self.l_floor, self.l_spec, self.sc, self.sc_head, self.pk_sc, *self.pk_txt,
                    self.t_note, self.t_sol, self.t_hz, self.t_hint, self.l_needle, self.t_cents, *self.t_pk,
                    self.r_lvl, self.l_hold, self.l_nf, self.t_lvl, self.t_key, self.t_acc, self.t_floor, self.t_snr, self.t_hum, self.t_state]
        for a in self.dyn:
            a.set_animated(True)
        self.bg = None
        fig.canvas.mpl_connect('draw_event', self.on_draw)
        fig.canvas.mpl_connect('key_press_event', self.on_key)

    def on_draw(self, ev):
        self.bg = self.fig.canvas.copy_from_bbox(self.fig.bbox)
        for a in self.dyn:
            self.fig.draw_artist(a)

    def blit(self):
        c = self.fig.canvas
        if self.bg is None:
            c.draw()
            return
        c.restore_region(self.bg)
        for a in self.dyn:
            self.fig.draw_artist(a)
        c.blit(self.fig.bbox)
        c.flush_events()

    # --- подписи нот на оси
    def set_ticks(self):
        e = self.eng
        pos, lab, cols, grid = [], [], [], []
        deg = e.degrees()
        tri = (0, deg[2], deg[4]) if deg else ()           # подписываем тонику, терцию и квинту, линии - все ноты лада
        for n in range(int(math.ceil(midi_of(FMIN))), int(math.floor(midi_of(FMAX))) + 1):
            if deg is None:
                if NAMES[n % 12] not in ('C', 'E', 'G', 'A'):
                    continue
            elif not e.in_key(n):
                continue
            grid.append(ypos(hz_of(n)))
            if deg is not None and (n - e.tonic) % 12 not in tri:
                continue
            pos.append(ypos(hz_of(n)))
            lab.append(note_name(n))
            cols.append('#ffffff' if (deg is not None and (n - e.tonic) % 12 == 0) else DIM)
        self.ax.set_yticks(pos)
        self.ax.set_yticklabels(lab)
        for t, c in zip(self.ax.get_yticklabels(), cols):
            t.set_color(c)
            if c == '#ffffff':
                t.set_weight('bold')
        self.grid1.set_segments([[(-HIST, y), (0, y)] for y in grid])
        self.grid2.set_segments([[(y, DB_LO), (y, DB_HI)] for y in grid])
        xp, xl = [], []
        for n in range(int(math.ceil(midi_of(FMIN))), int(math.floor(midi_of(FMAX))) + 1):
            if NAMES[n % 12] == 'A' or (e.degrees() is not None and (n - e.tonic) % 12 == 0):
                xp.append(ypos(hz_of(n)))
                xl.append('%s\n%d Гц' % (note_name(n), round(hz_of(n))))
        self.asp.set_xticks(xp)
        self.asp.set_xticklabels(xl)
        if self.bg is not None:
            self.bg = None
            self.fig.canvas.draw_idle()

    def set_lut(self):
        import matplotlib
        self.lut = (matplotlib.colormaps[CMAPS[self.cmap_i]](np.linspace(0, 1, 256)) * 255).astype(np.uint8)

    def rgba(self):                                    # цвета считаем сами: так быстрее, чем через norm/cmap matplotlib
        k = np.clip((self.eng.img - self.vmin) / (-25.0 - self.vmin) * 255.0, 0, 255).astype(np.uint8)
        return self.lut[k]

    def on_key(self, ev):
        k, e = ev.key, self.eng
        if k == ' ':
            self.paused = not self.paused
        elif k == 'n':
            e.calibrate(self.now)
        elif k in ('k', 'K'):
            e.tonic = (e.tonic + (1 if k == 'k' else -1)) % 12
            e.hits = e.total = 0
            self.set_ticks()
        elif k in ('m', 'M'):
            e.mode = (e.mode + 1) % len(SCALES)
            e.hits = e.total = 0
            self.set_ticks()
        elif k in ('+', '='):
            self.vmin = min(-60.0, self.vmin + 5)
        elif k in ('-', '_'):
            self.vmin = max(-120.0, self.vmin - 5)
        elif k in ('c', 'C'):
            self.cmap_i = (self.cmap_i + 1) % len(CMAPS)
            self.set_lut()
        elif k in ('r', 'R'):
            e.hits = e.total = 0
            e.chist = []
            e.off = 0.0
        elif k in ('o', 'O'):
            e.rel = not e.rel
            e.chist = []
            e.off = 0.0
            e.hits = e.total = 0
        elif k in ('s', 'S'):
            name = time.strftime('spectrum_%Y%m%d_%H%M%S.png')
            self.fig.savefig(os.path.join(os.path.dirname(os.path.abspath(__file__)), name), facecolor=BG, dpi=130)
        elif k in ('q', 'Q', 'escape'):
            self.plt.close(self.fig)

    # --- кадр
    def tick(self, now):
        self.now = now
        e = self.eng
        if not self.paused:
            x = self.src.latest(NFFT)
            e.step(x, now)
        self.draw(now)
        self.blit()
        return []

    def draw(self, now):
        e = self.eng
        self.im.set_data(self.rgba())
        # цепочка тона
        ok = ~np.isnan(e.pitch_y)
        pts = np.column_stack((self.xt[ok], e.pitch_y[ok])) if ok.any() else np.empty((0, 2))
        self.sc.set_offsets(pts)
        self.sc.set_facecolors([STATUS_COL[int(v)] for v in e.pitch_s[ok]] if ok.any() else [])
        if ok.any() and not np.isnan(e.pitch_y[-1]):
            self.sc_head.set_offsets([[0.0, e.pitch_y[-1]]])
            self.sc_head.set_facecolors([STATUS_COL[int(e.pitch_s[-1])]])
        else:
            self.sc_head.set_offsets(np.empty((0, 2)))
        # спектр
        col = np.clip(e.col, DB_LO, DB_HI)
        self.l_spec.set_data(e.ylog, col)
        self.poly.set_xy(np.vstack(([0, DB_LO], np.column_stack((e.ylog, col)), [OCT, DB_LO])))
        if e.floor_level is not None:
            self.l_floor.set_data(e.ylog, np.clip(e.floor_log, DB_LO, DB_HI))
        offs, cs, ys = [], [], []
        for i, t in enumerate(self.pk_txt):
            if i < len(e.peaks):
                f, a = e.peaks[i]
                m = midi_of(f)
                n = int(round(m))
                y = ypos(f)
                t.set_position((min(max(y, 0.35), OCT - 0.35), min(a + 5 + 7 * (i % 3), DB_HI - 14)))
                t.set_text('%d Гц  %s' % (round(f), note_name(n)))
                offs.append([y, a])
                cs.append(PEAKC[i])
            else:
                t.set_text('')
        self.pk_sc.set_offsets(np.array(offs) if offs else np.empty((0, 2)))
        self.pk_sc.set_facecolors(cs)
        for i, t in enumerate(self.t_pk):
            if i < len(e.peaks):
                f, a = e.peaks[i]
                m = midi_of(f)
                n = int(round(m))
                t.set_text('%d %7.1f Гц %-4s %+3.0f¢ %4.0f дБ' % (i + 1, f, note_name(n), (m - n) * 100, a))
            else:
                t.set_text('')
        # нота
        if e.note:
            m, cents, st, tgt = e.note
            n = int(round(m))
            col_ = STATUS_COL[st]
            self.t_note.set_text(note_name(n))
            self.t_note.set_color(col_)
            self.t_sol.set_text(SOL[n % 12])
            self.t_hz.set_text('%.1f Гц' % e.f0)
            self.l_needle.set_data([cents, cents], [0.2, 0.9])
            self.l_needle.set_color(col_)
            self.t_cents.set_text('%+.0f ¢' % cents)
            self.t_cents.set_color(col_)
            if st == 4:
                self.t_hint.set_text('вне тональности → ближайшая %s' % tgt)
            elif abs(cents) <= 10:
                self.t_hint.set_text('✓ в точку')
            else:
                self.t_hint.set_text(('▼ ниже, поднимите' if cents < 0 else '▲ выше, опустите') + (' чуть' if abs(cents) <= 25 else ''))
            self.t_hint.set_color(col_)
        else:
            self.t_note.set_text('-')
            self.t_note.set_color(DIM)
            self.t_sol.set_text('')
            self.t_hz.set_text('')
            self.t_hint.set_text('пойте - тон появится здесь')
            self.t_hint.set_color(DIM)
            self.l_needle.set_data([0, 0], [0.2, 0.9])
            self.l_needle.set_color(DIM)
            self.t_cents.set_text('')
        # уровень
        lv = max(-90.0, e.level_db)
        self.r_lvl.set_width(lv + 90)
        self.r_lvl.set_color(C_BAD if lv > -6 else C_OK if lv > -18 else C_GOOD)
        hp = max(-90.0, e.hold_peak)
        self.l_hold.set_data([hp, hp], [0.34, 0.70])
        if e.floor_level is not None:
            self.l_nf.set_data([e.floor_level, e.floor_level], [0.30, 0.74])
        self.t_lvl.set_text('%.0f дБ%s' % (e.level_db, '  ПЕРЕГРУЗ!' if e.peak_db > -1 else ''))
        self.t_lvl.set_color(C_OUT if e.peak_db > -1 else FG)
        # статистика
        deg = SCALES[e.mode][0]
        self.t_key.set_text('Тональность: %s %s' % (NAMES[e.tonic], deg) if e.mode != 2 else 'Тональность: любая (M - выбрать)')
        acc = (100.0 * e.hits / e.total) if e.total else 0.0
        self.t_acc.set_text('Попадание: %.0f%%  (%d кадров)' % (acc, e.total) if e.total else 'Попадание: -')
        if e.floor_level is not None:
            self.t_floor.set_text('Шум микрофона: %.0f дБ' % e.floor_level)
            if e.rel:
                self.t_snr.set_text('Сдвиг вашего строя: %+.0f ¢' % e.off)
            else:
                self.t_snr.set_text('Запас над шумом: %.0f дБ' % (e.level_db - e.floor_level) if e.note else 'Запас над шумом: -')
            self.t_hum.set_text('Фон: %s Гц' % ', '.join(map(str, e.hum)) if e.hum else 'Фона сети не видно')
        else:
            self.t_floor.set_text('Шум: идёт измерение...')
            self.t_snr.set_text('')
            self.t_hum.set_text('')
        if e.calibrating(now):
            self.t_state.set_text('ИЗМЕРЯЮ ШУМ - помолчите %.1f с' % (e.cal_end - now))
            self.t_state.set_color(C_OK)
        elif self.paused:
            self.t_state.set_text('ПАУЗА')
            self.t_state.set_color(C_BAD)
        elif e.rel:
            self.t_state.set_text('ОТНОСИТЕЛЬНЫЙ РЕЖИМ')
            self.t_state.set_color('#4dd2ff')
        else:
            self.t_state.set_text('')


def main():
    ap = argparse.ArgumentParser(description='Спектрограмма и тон голоса')
    ap.add_argument('--list', action='store_true', help='показать устройства ввода')
    ap.add_argument('--device', type=int, default=None, help='номер устройства ввода')
    ap.add_argument('--rate', type=int, default=None, help='частота дискретизации')
    ap.add_argument('--demo', action='store_true', help='синтетический сигнал вместо микрофона')
    ap.add_argument('--selftest', default=None, help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.list:
        import sounddevice as sd
        for i, d in enumerate(sd.query_devices()):
            if d['max_input_channels'] > 0:
                print(i, d['name'], '| %d Гц' % d['default_samplerate'])
        return
    import matplotlib
    if a.selftest:
        matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    if a.demo or a.selftest:
        src = Demo(a.rate)
    else:
        try:
            src = Mic(a.device, a.rate)
        except Exception as ex:
            print('Не удалось открыть микрофон: %s\nНужен sounddevice (pip install sounddevice); номер устройства - --list.' % ex)
            sys.exit(1)
    eng = Engine(src.fs)
    ui = Ui(plt, matplotlib, eng, src, 'Источник: %s · %d Гц' % (src.name, src.fs))

    if a.selftest:                                    # прогон без окна: кадры по модельному времени, снимок и замер скорости
        t = [0.0]
        src.clock = lambda: t[0]
        src.t0 = 0.0
        eng.tonic, eng.mode = 9, 1
        eng.rel = os.environ.get('DEMO_REL') == '1'
        ui.set_ticks()
        eng.calibrate(0.0)
        cost = []
        while t[0] < float(os.environ.get('DEMO_T', '12.5')):
            t[0] += 1.0 / FPS
            t0 = time.perf_counter()
            ui.tick(t[0])
            cost.append(time.perf_counter() - t0)
        ui.fig.savefig(a.selftest, facecolor=BG, dpi=100)
        print('кадров %d, среднее %.0f мс на кадр, макс %.0f мс' % (len(cost), 1000 * np.mean(cost), 1000 * max(cost)))
        return

    eng.calibrate(time.time())
    timer = ui.fig.canvas.new_timer(interval=int(1000 / FPS))
    timer.add_callback(lambda: ui.tick(time.time()))
    ui.timer = timer
    timer.start()
    plt.show()


if __name__ == '__main__':
    main()
