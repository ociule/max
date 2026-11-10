#!/usr/bin/env python3
"""Summarize the consistency checks of bench.py games: python3 consistency.py RESULTS_FOLDER [RESULTS_FOLDER...]

Prints per results folder the failed assertions, the violations per check and a few examples of each.
"""
import json,collections,glob,sys
for folder in sys.argv[1:]:
    tot=collections.Counter(); ex={}; asserts=0; games=0
    for f in sorted(glob.glob(f'{folder}/*/max-port/stats.jsonl') + glob.glob(f'{folder}/*/stats.jsonl')):
        games+=1
        for l in open(f):
            d=json.loads(l)
            c=d['consistency']
            if d['type']=='game': asserts+=d['assertions']; continue
            for k,v in c['by_check'].items(): tot[k]+=v
            for e in c['examples']:
                k=e.split(':')[0]
                ex.setdefault(k,[])
                if len(ex[k])<4: ex[k].append(f"{f.split('/')[-3]} t{d['turn']} {d['team']}: {e}")
    print(f'== {folder}: {games} games, assertions {asserts}, violations {dict(tot)}')
    for k,v in ex.items(): print('\n'.join('   '+x for x in v))
