#!/usr/bin/env python3
"""Speech of the extracted clips to text with Whisper (faster-whisper).

    tgstt.py WAV_FOLDER LANGUAGE [MODEL]        -> WAV_FOLDER/speech.tsv: clip, start, end, text

Whisper works on 30 seconds at a time, however short the sound: short clips are put one after another with a
second of silence between, a window's worth at once, and the words are dealt back to the clips by their times.
Clips already in speech.tsv are skipped (it can be stopped and run again). A clip with no speech (a sound
effect, music) gets one line with no text.
"""
import glob
import os
import sys
import wave

import numpy as np
from faster_whisper import WhisperModel

RATE = 16000
GAP = 1.0          # silence between two clips of a window, seconds
WINDOW = 29.0
SHORT = 14.0       # clips up to this long share a window


def load(path):
    """A WAV file as 16 kHz mono floats (low-passed, then picked by linear interpolation)."""
    w = wave.open(path)
    a = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(np.float32) / 32768
    if w.getnchannels() > 1:
        a = a.reshape(-1, w.getnchannels()).mean(axis=1)
    rate = w.getframerate()
    if rate != RATE and len(a):
        k = np.arange(-32, 33)
        fir = np.sinc(k * min(1, RATE / rate) * 0.95) * np.hamming(65)
        a = np.convolve(a, fir / fir.sum(), 'same')
        a = np.interp(np.arange(0, len(a), rate / RATE), np.arange(len(a)), a).astype(np.float32)
    return a


def lines_of(words):
    """Words (start, end, text) as subtitle lines: a sentence, or what's said without a pause, a line."""
    out, cur = [], []
    for i, (a, b, t) in enumerate(words):
        cur.append((a, b, t))
        last = i + 1 == len(words)
        text = ''.join(w[2] for w in cur).strip()
        pause = not last and words[i + 1][0] - b > 0.8
        if last or pause or t.rstrip()[-1:] in '.?!' or (len(text) > 90 and t.rstrip()[-1:] in ',;:'):
            if text:
                out.append((cur[0][0], cur[-1][1], text))
            cur = []
    return out


def main():
    folder, lang = sys.argv[1], sys.argv[2]
    model = WhisperModel(sys.argv[3] if len(sys.argv) > 3 else 'large-v3-turbo', device='cpu', compute_type='int8',
                         cpu_threads=int(os.environ.get('STT_THREADS', '8')))
    path = folder + '/speech.tsv'
    done = set()
    if os.path.exists(path):
        done = {l.split('\t')[0] for l in open(path)}
    todo = [w for w in sorted(glob.glob(folder + '/*.wav')) if os.path.basename(w)[:-4] not in done]
    out = open(path, 'a')

    def write(clip, lines):
        for a, b, t in lines:
            out.write('%s\t%.2f\t%.2f\t%s\n' % (clip, a, b, t))
        if not lines:
            out.write('%s\t0\t0\t\n' % clip)
        out.flush()

    def transcribe(audio):
        segs, _ = model.transcribe(audio, language=lang, vad_filter=True, condition_on_previous_text=False,
                                   beam_size=5, no_speech_threshold=0.5, word_timestamps=True)
        return [(w.start, w.end, w.word) for s in segs for w in (s.words or [])]

    batch, length = [], 0.0      # (clip, samples, its start in the window)

    def flush():
        nonlocal batch, length
        if not batch:
            return
        gap = np.zeros(int(GAP * RATE), dtype=np.float32)
        audio = np.concatenate([x for _, a, _ in batch for x in (a, gap)])
        words = transcribe(audio)
        for clip, a, start in batch:
            end = start + len(a) / RATE
            mine = [(max(0.0, ws - start), min(len(a) / RATE, we - start), t) for ws, we, t in words
                    if start - GAP / 2 <= (ws + we) / 2 < end + GAP / 2]
            write(clip, lines_of(mine))
        batch, length = [], 0.0

    for wav in todo:
        clip = os.path.basename(wav)[:-4]
        a = load(wav)
        sec = len(a) / RATE
        if sec > SHORT:
            write(clip, lines_of(transcribe(a)))
            continue
        if length + sec > WINDOW:
            flush()
        batch.append((clip, a, length))
        length += sec + GAP
    flush()


if __name__ == '__main__':
    main()
