#!/usr/bin/env python3
"""Subtitle packs for Ace Ventura from the transcripts of its speech (tgwave.py, then tgstt.py, per version).

    tgsubs.py lines WORK                      -> WORK/en-lines.txt: the English lines to translate, numbered
    tgsubs.py build WORK EN_GAME DE_GAME OUT  -> OUT/en.tsv, OUT/de.tsv (for the English and the German game)

WORK holds en/ and de/ (clips.tsv, speech.tsv and the WAVs) and, for Czech, cs-lines.txt: the lines of
en-lines.txt translated, under the same numbers.

A pack: "#game INTRO.BIN size" (which game it's for), then a line a subtitle:
    data file, clip's offset, clip's size, from, to (seconds into the clip), English, German, Czech
The two versions hold the same clips in the same order with a few more here and there: their resource tables
are aligned by the resources' types, which pairs the clips, so each version gets the other's text too (its
times stretched to the clip's own length).
"""
import collections
import difflib
import hashlib
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tg7l

OLD_TYPE = {17: 19, 18: 20, 16: 18, 19: 21, 9: 12, 13: 15, 12: 14}   # the first version's type numbers as the later's
ABBREV = ('mr.', 'mrs.', 'ms.', 'dr.', 'st.', 'hr.', 'fr.', 'no.', 'nr.')


def norm(text):
    return re.sub(r'[^\w ]', '', text.lower()).strip()


def tidy(lines):
    """A clip's lines put right: a word cut off its sentence, or a title off its name, back together."""
    out = []
    for a, b, t in lines:
        t = t.strip()
        if out:
            pa, pb, pt = out[-1]
            alone = len(pt.split()) == 1 and pt[-1:] not in '.?!…'
            if (alone or pt.lower().endswith(ABBREV) or (pt[-1:] == ',' and len(pt) < 12)) and a - pb < 1.6:
                out[-1] = (pa, b, pt + ' ' + t)
                continue
        out.append((a, b, t))
    res = []
    for a, b, t in out:
        if t and t[0].islower():
            t = t[0].upper() + t[1:]
        if t and t[-1].isalnum():
            t += '.'
        res.append((a, b, t))
    return res


def load(work, lang):
    """clip name -> [(from, to, text)], and clip name -> (base, index, offset, size, seconds).
    The same sound under several names (the common remarks are in every scene's file) gets the transcript
    most of them agreed on."""
    info = {}
    for l in open('%s/%s/clips.tsv' % (work, lang)):
        f, i, off, size, _, sec = l.rstrip('\n').split('\t')
        info['%s-%s' % (f, i)] = (f, int(i), int(off), int(size), float(sec))
    speech = collections.defaultdict(list)
    for l in open('%s/%s/speech.tsv' % (work, lang)):
        c, a, b, t = l.rstrip('\n').split('\t')
        if t.strip():
            speech[c].append((float(a), float(b), t))
    speech = {c: tidy(v) for c, v in speech.items()}
    same = collections.defaultdict(list)
    for c in info:
        p = '%s/%s/%s.wav' % (work, lang, c)
        if os.path.exists(p):
            same[hashlib.md5(open(p, 'rb').read()).hexdigest()].append(c)
    for group in same.values():
        if len(group) < 2:
            continue
        votes = collections.Counter(' '.join(t for _, _, t in speech.get(c, [])) for c in group)
        best = max(votes, key=lambda k: (votes[k], k[:1].isupper(), len(k)))
        src = next(c for c in group if ' '.join(t for _, _, t in speech.get(c, [])) == best)
        for c in group:
            if src in speech:
                speech[c] = speech[src]
            else:
                speech.pop(c, None)
    return speech, info


def story_order(speech, info):
    return sorted(speech, key=lambda c: (info[c][0] != 'INTRO', info[c][0], info[c][1]))


def unique_lines(speech, info):
    seen, out = set(), []
    for c in story_order(speech, info):
        for _, _, t in speech[c]:
            if norm(t) and norm(t) not in seen:
                seen.add(norm(t))
                out.append(t)
    return out


def pair(en_game, de_game, en_info, de_info, en=None, de=None):
    """English clip name -> German clip name. (en, de: the speech; clips with speech that the tables left
    unpaired are paired in their order, in a file where both versions have as many of them.)"""
    pairs = {}
    for base in sorted({v[0] for v in en_info.values()}):
        try:
            e = tg7l.ResFile(find(en_game, base + '.BIN'))
            d = tg7l.ResFile(find(de_game, base + '.BIN'))
        except (OSError, ValueError, TypeError):
            continue
        a = [OLD_TYPE.get(t, t) for t, _, _, _ in e.res]
        b = [t for t, _, _, _ in d.res]
        for i, j, k in difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks():
            for x in range(k):
                if '%s-%d' % (base, i + x) in en_info and '%s-%d' % (base, j + x) in de_info:
                    pairs['%s-%d' % (base, i + x)] = '%s-%d' % (base, j + x)
    if en is not None:
        taken = set(pairs.values())
        for base in sorted({v[0] for v in en_info.values()}):
            a = sorted((en_info[c][1], c) for c in en if en_info[c][0] == base and c not in pairs)
            b = sorted((de_info[c][1], c) for c in de if de_info[c][0] == base and c not in taken)
            if a and len(a) == len(b):
                for (_, x), (_, y) in zip(a, b):
                    pairs[x] = y
            elif a or b:
                print('unpaired in', base, [c for _, c in a], [c for _, c in b])
    return pairs


def find(folder, name):
    for n in os.listdir(folder):
        if n.upper() == name.upper():
            return os.path.join(folder, n)
    return None


def stretch(lines, src_sec, dst_sec):
    k = dst_sec / src_sec if src_sec > 0 else 1
    return [(a * k, b * k, t) for a, b, t in lines]


def write_pack(path, game, ext, speech, info, text_for):
    """text_for(clip, lines) -> [(from, to, en, de, cs)]"""
    n = 0
    with open(path, 'w') as o:
        o.write('#game INTRO.BIN %d\n' % os.path.getsize(find(game, 'INTRO.BIN')))
        for c in story_order(speech, info):
            base, _, off, size, _ = info[c]
            data = find(game, '%s.%s' % (base, ext)) or find(game, base + '.BIN')
            for a, b, en, de, cs in text_for(c):
                o.write('%s\t%d\t%d\t%.2f\t%.2f\t%s\t%s\t%s\n' % (os.path.basename(data).upper(), off, size, a, b, en, de, cs))
                n += 1
    print(path, n, 'lines')


def main():
    cmd, work = sys.argv[1], sys.argv[2]
    en, en_info = load(work, 'en')
    if cmd == 'lines':
        lines = unique_lines(en, en_info)
        open(work + '/en-lines.txt', 'w').write(''.join('%d\t%s\n' % (i + 1, t) for i, t in enumerate(lines)))
        print(len(lines), 'lines,', sum(len(t.split()) for t in lines), 'words')
        return
    en_game, de_game, out = sys.argv[3:6]
    de, de_info = load(work, 'de')
    cs = {}
    if os.path.exists(work + '/cs-lines.txt'):
        src = dict(l.rstrip('\n').split('\t', 1) for l in open(work + '/en-lines.txt'))
        for l in open(work + '/cs-lines.txt'):
            if '\t' in l:
                k, t = l.rstrip('\n').split('\t', 1)
                if k in src and t.strip():
                    cs[norm(src[k])] = t.strip().lstrip('-– ').strip()
    e2d = pair(en_game, de_game, en_info, de_info, en, de)
    d2e = {v: k for k, v in e2d.items()}
    os.makedirs(out, exist_ok=True)

    def rows(own, own_info, other, other_info, mate, own_is_en):
        def text_for(c):
            mine = own[c]
            theirs = other.get(mate.get(c), [])
            res = []
            if theirs and len(theirs) == len(mine):       # line for line
                for (a, b, t), (_, _, u) in zip(mine, theirs):
                    res.append((a, b, t, u))
            elif theirs:
                # (the other version's lines by its own times, stretched; own text where the other has none)
                sec, osec = own_info[c][4], other_info[mate[c]][4]
                st = stretch(theirs, osec, sec)
                for a, b, t in mine:
                    res.append((a, b, t, ''))
                for a, b, u in st:
                    res.append((a, b, '', u))
            else:
                for a, b, t in mine:
                    res.append((a, b, t, ''))
            outl = []
            for a, b, t, u in res:
                e, d = (t, u) if own_is_en else (u, t)
                c_ = cs.get(norm(e), '') if e else ''
                outl.append((a, b, e, d, c_))
            # (a language with no text for a clip shows another's rather than nothing)
            if not any(x[2] for x in outl):
                outl = [(a, b, d, d, d) for a, b, e, d, c_ in outl]
            if not any(x[3] for x in outl):
                outl = [(a, b, e, e, c_) for a, b, e, d, c_ in outl]
            if not any(x[4] for x in outl):
                outl = [(a, b, e, d, e) for a, b, e, d, c_ in outl]
            return outl
        return text_for

    write_pack(out + '/en.tsv', en_game, 'BIN', en, en_info, rows(en, en_info, de, de_info, e2d, True))
    write_pack(out + '/de.tsv', de_game, '000', de, de_info, rows(de, de_info, en, en_info, d2e, False))


if __name__ == '__main__':
    main()
