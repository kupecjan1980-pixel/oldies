#!/usr/bin/env python3
"""The lines of en-lines.txt (tgsubs.py lines) into another language by a translation model on this computer
(NLLB-200 through ctranslate2): a machine's translation, to be read through and put right by hand.

    tgtranslate.py WORK [LANGUAGE [MODEL]]     -> WORK/cs-lines.txt     (LANGUAGE: NLLB's code, ces_Latn)
Lines already there are kept (so are corrections made by hand).
"""
import os
import sys

import ctranslate2
from huggingface_hub import snapshot_download
from tokenizers import Tokenizer

work = sys.argv[1]
target = sys.argv[2] if len(sys.argv) > 2 else 'ces_Latn'
repo = sys.argv[3] if len(sys.argv) > 3 else 'OpenNMT/nllb-200-distilled-1.3B-ct2-int8'
path = snapshot_download(repo)
tok = Tokenizer.from_file(path + '/tokenizer.json')
model = ctranslate2.Translator(path, device='cpu', compute_type='int8', intra_threads=int(os.environ.get('MT_THREADS', '12')))

out_path = work + '/cs-lines.txt'
done = {}
if os.path.exists(out_path):
    done = dict(l.rstrip('\n').split('\t', 1) for l in open(out_path) if '\t' in l)
lines = [l.rstrip('\n').split('\t', 1) for l in open(work + '/en-lines.txt')]
todo = [(k, t) for k, t in lines if k not in done]
with open(out_path, 'a') as out:
    for at in range(0, len(todo), 16):
        part = todo[at:at + 16]
        src = [['eng_Latn'] + tok.encode(t, add_special_tokens=False).tokens + ['</s>'] for _, t in part]
        res = model.translate_batch(src, target_prefix=[[target]] * len(part), beam_size=5, repetition_penalty=1.1,
                                    no_repeat_ngram_size=4, max_decoding_length=200)
        for (k, _), r in zip(part, res):
            ids = [tok.token_to_id(x) for x in r.hypotheses[0][1:]]
            out.write('%s\t%s\n' % (k, tok.decode(ids, skip_special_tokens=True).strip()))
        out.flush()
