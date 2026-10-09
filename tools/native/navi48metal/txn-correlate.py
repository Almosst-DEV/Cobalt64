#!/usr/bin/env python3
"""txn-correlate.py - Stage 0c of an internal design note: merge the DTrace transaction log (tools/pc/txnlog.d) with the bundle's command-buffer log
(/private/var/tmp/n48m-cblog.<pid>.txt) and print the inversion count, the transacted-vs-presented IOSurface id comparison, and the decision of the memo's table.

    txn-correlate.py txnlog.txt n48m-cblog.<pid>.txt [more cblog files] [--match-ms 100] [--inv-min 1] [--mismatch-frac 0.01] [--shift-ms 0] [--first 20]
    txn-correlate.py --selftest        (synthetic data for the three decision rows; exits non-zero if any decision is wrong)

Clock: both logs carry CLOCK_REALTIME (DTrace `wall=`; the bundle's `K up= wall=` pairs, one per second). Bundle uptime stamps are converted to wall time by
the nearest K pair; the SCAN12 (selector 12 on N48N) calls in the DTrace log are matched to the bundle's P lines to report the residual offset (informational:
pass --shift-ms to apply a correction to the bundle's times if it is large).
"""
import sys, re, bisect, statistics, argparse, difflib, collections

KV = re.compile(r'(\w+)=(\S+)')

def parse_kv(line):
    d = dict(KV.findall(line)); d['_k'] = line.split(' ', 1)[0]; return d

def num(d, k, default=0, base=10):
    try:
        v = d.get(k)
        return default if v is None else int(v, 0 if base == 0 else base)
    except ValueError:
        return default

# ---------------------------------------------------------------- loaders
def load_txn(path):
    tx, s12, s8, ticks = [], [], [], []
    for ln in open(path, errors='replace'):
        ln = ln.strip()
        if not ln or ' ' not in ln: continue
        d = parse_kv(ln); k = d['_k']
        if k == 'TXN':
            tx.append(dict(wall=num(d, 'wall'), t=num(d, 't'), tid=num(d, 'tid'), conn=num(d, 'conn'), pipe=num(d, 'pipe'), mask=num(d, 'mask', 0, 0), opt=num(d, 'opt', 0, 0),
                           sid=num(d, 'p0sid'), p0type=num(d, 'p0type', 0, 0), rect=d.get('rect', ''), f08=d.get('f08', ''), f108=d.get('f108', ''), f110=d.get('f110', ''),
                           p1sid=num(d, 'p1sid')))
        elif k == 'SCAN12': s12.append(dict(wall=num(d, 'wall'), t=num(d, 't'), conn=num(d, 'conn')))
        elif k == 'SEL8O': s8.append(dict(wall=num(d, 'wall'), conn=num(d, 'conn'), size=num(d, 'instruct')))
        elif k == 'TICK': ticks.append((num(d, 't'), num(d, 'wall')))
    return tx, s12, s8, ticks

def load_cb(paths, shift_ns):
    C, S, F, P, X, I = [], [], [], [], [], []
    K = []; hdr = []
    rows = []
    for path in paths:
        for ln in open(path, errors='replace'):
            ln = ln.strip()
            if not ln or ' ' not in ln: continue
            d = parse_kv(ln); d['_f'] = path; rows.append(d)
            if d['_k'] in ('K', 'H') and 'up' in d and 'wall' in d: K.append((num(d, 'up'), num(d, 'wall')))
    K.sort()
    ups = [k[0] for k in K]
    def wall(up):
        if not K: return up
        i = bisect.bisect_left(ups, up)
        if i == 0: j = 0
        elif i == len(K): j = len(K) - 1
        else: j = i if abs(ups[i] - up) < abs(ups[i - 1] - up) else i - 1
        return up + (K[j][1] - K[j][0]) + shift_ns
    def sids(s):
        return [] if s in (None, '-', '') else [int(x) for x in s.split(',') if x]
    for d in rows:
        k = d['_k']
        if k == 'C': C.append(dict(w=wall(num(d, 't')), cb=num(d, 'cb'), q=d.get('q'), wr=sids(d.get('w')), rd=sids(d.get('r'))))
        elif k == 'S': S.append(dict(w=wall(num(d, 't')), wq=wall(num(d, 'tq')), wc=wall(num(d, 'cmt')) if num(d, 'cmt') else 0, cb=num(d, 'cb'), q=d.get('q'), sub=num(d, 'sub'), dseq=num(d, 'dseq'),
                                  cls=d.get('cls', '-'), disp=num(d, 'disp'), slot=num(d, 'slot', -1), wr=sids(d.get('w')), rd=sids(d.get('r')), inv=num(d, 'inv')))
        elif k == 'F': F.append(dict(w=wall(num(d, 't')), cb=num(d, 'cb'), vk=num(d, 'vk')))
        elif k == 'P': P.append(dict(w0=wall(num(d, 't0')), w1=wall(num(d, 't1')), sid=num(d, 'sid'), seq=num(d, 'seq'), slot=num(d, 'slot'), rc=num(d, 'rc')))
        elif k == 'X': X.append(dict(w=wall(num(d, 't')), sid=num(d, 'sid'), seq=num(d, 'seq'), why=d.get('why', '?')))
        elif k == 'I': I.append(dict(w=wall(num(d, 't')), rd_cb=num(d, 'rd_cb'), rd_q=d.get('rd_q'), rd_sub=num(d, 'rd_sub'), sid=num(d, 'sid'), wr_cb=num(d, 'wr_cb'), wr_q=d.get('wr_q'), wr_commit=wall(num(d, 'wr_commit'))))
        elif k == 'K10': hdr.append(d)
    return dict(C=C, S=S, F=F, P=P, X=X, I=I, K10=hdr, nK=len(K))

# ---------------------------------------------------------------- analysis
def ms(ns): return ns / 1e6

def analyse(tx, s12, cb, a):
    out = []; pr = out.append
    pipe = [t for t in tx if t['pipe'] == 1]
    res = dict(decision=None)
    pr('== inputs ==')
    pr('transactions logged: %d (display-pipe connection: %d; other 280-byte selector-8 connections: %d); N48N selector-12 calls: %d' % (len(tx), len(pipe), len(tx) - len(pipe), len(s12)))
    pr('bundle: commits %d, submits %d, fences %d, presents %d (rc==0: %d), skipped presents %d, K clock pairs %d' % (len(cb['C']), len(cb['S']), len(cb['F']), len(cb['P']), sum(1 for p in cb['P'] if p['rc'] == 0), len(cb['X']), cb['nK']))
    if not pipe:
        pr('NO display-pipe transactions in the DTrace log: the TXN probe did not fire (probe not matched, or WindowServer was not the target). Nothing can be decided.')
        res['decision'] = 'NO-DATA'; return res, out
    pipe.sort(key=lambda t: t['wall'])
    w0, w1 = pipe[0]['wall'], pipe[-1]['wall']; dur = max((w1 - w0) / 1e9, 1e-9)
    pr('transaction window: %.2f s, %d transactions = %.1f/s' % (dur, len(pipe), len(pipe) / dur))

    # clock residual
    pw = sorted(p['w0'] for p in cb['P'] if p['rc'] == 0)
    sw = sorted(s['wall'] for s in s12)
    if pw and sw:
        res_d = []; j = 0
        for x in pw:
            while j < len(sw) and sw[j] < x - 25e6: j += 1
            if j < len(sw) and abs(sw[j] - x) <= 25e6: res_d.append(sw[j] - x); j += 1
        if len(res_d) >= 5: pr('clock check: %d bundle presents matched to selector-12 calls, DTrace minus bundle call time: median %.3f ms (p10 %.3f, p90 %.3f); expect a small positive value (probe-to-stamp latency); a large value: rerun with --shift-ms' % (len(res_d), ms(statistics.median(res_d)), ms(sorted(res_d)[len(res_d) // 10]), ms(sorted(res_d)[len(res_d) * 9 // 10])))
        else: pr('clock check: only %d presents matched a selector-12 call (needs >= 5): the bundle times are on the wall clock only' % len(res_d))
    else:
        pr('clock check: no presents or no selector-12 calls: skipped')

    # ---- inversions
    pr('\n== 1. cross-queue RAW inversions (bundle, per vkQueueSubmit) ==')
    Sw = [s for s in cb['S'] if w0 - 1e9 <= s['w'] <= w1 + 1e9]
    inv_n = sum(s['inv'] for s in Sw); inv_cb = sum(1 for s in Sw if s['inv'])
    inv_all = sum(s['inv'] for s in cb['S'])
    pr('whole log: %d submits, %d inversions in %d submits;  transaction window (+-1 s): %d submits, %d inversions in %d submits = %.2f inversions/s (transactions %.1f/s)' % (len(cb['S']), inv_all, sum(1 for s in cb['S'] if s['inv']), len(Sw), inv_n, inv_cb, inv_n / dur, len(pipe) / dur))
    if cb['K10']:
        l = cb['K10'][-1]; pr('last K10 line (cumulative in-bundle counters): inv_total=%s inv_cbs=%s commits=%s evicted=%s no_commit=%s' % (l.get('inv_total'), l.get('inv_cbs'), l.get('commits'), l.get('evicted'), l.get('no_commit')))
    for i in cb['I'][:a.first]:
        pr('  case: cb %d (queue %s, submit #%d) read IOSurface %d, writer cb %d (queue %s, committed %.3f ms before the reader submitted) still unsubmitted; same queue: %s' % (i['rd_cb'], i['rd_q'], i['rd_sub'], i['sid'], i['wr_cb'], i['wr_q'], ms(i['w'] - i['wr_commit']), 'yes' if i['rd_q'] == i['wr_q'] else 'NO'))
    cross = sum(1 for i in cb['I'] if i['rd_q'] != i['wr_q'])
    if cb['I']: pr('  of the %d logged cases, %d are across different queues' % (len(cb['I']), cross))
    qc = collections.defaultdict(lambda: [0, 0, 0])
    for s in Sw:
        c = qc[s['q']]; c[0] += 1
        if s['disp']: c[1] += 1
        if s['inv']: c[2] += s['inv']
    pr('queue census in the window: ' + ('; '.join('queue %s: %d submits, %d writing a display surface, %d inversions' % (q, c[0], c[1], c[2]) for q, c in sorted(qc.items())) or 'none'))
    pr('  => %d distinct MTLCommandQueues submitted (H-X needs producer and consumer on different queues)' % len(qc))

    # ---- sid sequences
    pr('\n== 2. transacted IOSurface ids vs presented ids ==')
    T = [(t['wall'], t['sid']) for t in pipe]
    z = sum(1 for _, s in T if s == 0)
    Pn = sorted([(p['w0'], p['sid']) for p in cb['P'] if p['rc'] == 0 and w0 - 2e9 <= p['w0'] <= w1 + 2e9])
    pr('transactions %d (plane-0 sid 0: %d), presents in window %d' % (len(T), z, len(Pn)))
    cT = collections.Counter(s for _, s in T); cP = collections.Counter(s for _, s in Pn)
    pr('per sid (transacted / presented): ' + ', '.join('%d: %d/%d' % (s, cT.get(s, 0), cP.get(s, 0)) for s in sorted(set(cT) | set(cP))))
    never_p = {s: n for s, n in cT.items() if s not in cP}; never_t = {s: n for s, n in cP.items() if s not in cT}
    if never_p: pr('TRANSACTED but NEVER presented: ' + ', '.join('sid %d (%d txns)' % kv for kv in sorted(never_p.items())))
    if never_t: pr('PRESENTED but NEVER transacted: ' + ', '.join('sid %d (%d presents)' % kv for kv in sorted(never_t.items())))
    # one-to-one time matching within +-match window
    W = a.match_ms * 1e6
    used = [False] * len(Pn); pairs = []; un_t = 0
    for (tw, ts) in T:
        best = -1; bd = None
        for j, (pw_, ps) in enumerate(Pn):
            if used[j] or ps != ts: continue
            d = abs(pw_ - tw)
            if d <= W and (bd is None or d < bd): best, bd = j, d
        if best < 0: un_t += 1
        else: used[best] = True; pairs.append((tw, best))
    un_p = sum(1 for u in used if not u)
    swaps = sum(1 for i in range(1, len(pairs)) if pairs[i][1] < pairs[i - 1][1])
    pr('matching (same sid within +-%g ms, one-to-one): matched %d, transactions without a present %d (%.1f%%), presents without a transaction %d (%.1f%%), order swaps among matched pairs %d' % (a.match_ms, len(pairs), un_t, 100.0 * un_t / max(len(T), 1), un_p, 100.0 * un_p / max(len(Pn), 1), swaps))
    def collapse(seq):
        o = []
        for s in seq:
            if not o or o[-1] != s: o.append(s)
        return o
    ct, cpz = collapse([s for _, s in T]), collapse([s for _, s in Pn])
    if ct and cpz:
        sm = difflib.SequenceMatcher(None, ct, cpz, autojunk=False) if len(ct) * len(cpz) < 4_000_000 else None
        if sm: pr('collapsed sid sequences: transacted %d runs, presented %d runs, similarity %.3f' % (len(ct), len(cpz), sm.ratio()))
    pr('first 12 transacted sids: %s' % [s for _, s in T[:12]]); pr('first 12 presented sids:  %s' % [s for _, s in Pn[:12]])
    # who writes the transacted sids, and commit-before-transaction
    wcls = collections.defaultdict(collections.Counter); wr_by_sid = collections.defaultdict(list)
    for s in cb['S']:
        for sid in s['wr']: wr_by_sid[sid].append(s)
        if s['disp']: wcls[s['disp']][s['cls']] += 1
    pr('classes of the command buffers that wrote each transacted display surface (the presented one per cb): ' + ('; '.join('sid %d: %s' % (sid, dict(wcls[sid])) for sid in sorted(cT) if sid in wcls) or 'none seen'))
    cm = collections.defaultdict(list)
    for c in cb['C']:
        for sid in c['wr']: cm[sid].append(c['w'])
    for v in cm.values(): v.sort()
    gaps = []; nowriter = 0; after = 0
    for (tw, ts) in T:
        v = cm.get(ts)
        if not v: nowriter += 1; continue
        i = bisect.bisect_right(v, tw)
        if i: gaps.append(tw - v[i - 1])
        else: after += 1
    if gaps: pr('commit before transaction (basis of option C): %d/%d transactions have a committed writer of their plane-0 sid before them; gap median %.2f ms, p90 %.2f ms; %d transactions with no writer seen at all (e.g. a surface only written by Apple code outside our bundle, or before logging), %d with writers only after' % (len(gaps), len(T), ms(statistics.median(gaps)), ms(sorted(gaps)[len(gaps) * 9 // 10]), nowriter, after))
    else: pr('commit before transaction: no transaction has a writer commit before it (%d with no writer at all)' % nowriter)
    other = collections.Counter((t['opt'], t['mask'], t['p1sid'] != 0) for t in pipe)
    pr('transaction shapes (options, dirty mask, plane-1 present): ' + ', '.join('%s x%d' % (('opt=%#x mask=%#x p1=%s' % k), n) for k, n in other.most_common(6)))

    # ---- decision
    pr('\n== 3. decision (NATIVE-S5-TXN.md section 2 table) ==')
    mism = (un_t + un_p + swaps) / max(len(T) + len(Pn), 1)
    inv_bad = inv_n >= a.inv_min
    seq_bad = bool(never_p or never_t) or mism > a.mismatch_frac
    if inv_bad:
        d = 'H-X'
        pr('DECISION: H-X (stale content): %d inversions during the transaction window (%.2f/s), %d of the logged cases across queues. Next: S1, fix the submission order in the bundle (hold a command buffer until every earlier-committed writer of an IOSurface it touches is submitted, with a timeout fallback). Present path unchanged.' % (inv_n, inv_n / dur, cross))
        if seq_bad: pr('  note: the sid sequences ALSO disagree (mismatch %.1f%%); fix S1 first, then re-measure; option C may still be needed.' % (100 * mism))
    elif seq_bad:
        d = 'OPTION-C'
        pr('DECISION: the brief\'s hypothesis (present order): 0 inversions, but the transacted and presented sid sequences differ (mismatch %.1f%% of events; unmatched transactions %d, unmatched presents %d, order swaps %d, never-presented sids %s, never-transacted sids %s). Next: option C, copy at write, present by transaction (memo section 3/4, S1\').' % (100 * mism, un_t, un_p, swaps, sorted(never_p), sorted(never_t)))
    else:
        d = 'NEITHER'
        pr('DECISION: neither: 0 inversions and the sid sequences agree (mismatch %.2f%%). Next suspect: in-pass feedback (a composite sampling its own target through a second texture over the same IOSurface: count same-IOSurface read-after-write across distinct textures inside one command buffer).' % (100 * mism))
    if len(pipe) < 50: pr('WARNING: only %d transactions: too few for a decision; the drag/tab-switch phase may not have been recorded.' % len(pipe))
    res['decision'] = d; res['inv'] = inv_n; res['mismatch'] = mism
    return res, out

# ---------------------------------------------------------------- selftest
def synth(mode, n=400):
    """Writes txnlog + cblog text for a scenario; returns (txn_lines, cb_lines)."""
    base_w = 1_700_000_000_000_000_000; base_up = 5_000_000_000_000
    tl, cl = [], []
    cl.append('H pid=1 up=%d wall=%d cap=1 fmt=1' % (base_up, base_w))
    for k in range(40): cl.append('K up=%d wall=%d' % (base_up + k * 10**9, base_w + k * 10**9))
    sids = [2, 3, 5]; cbn = 0; seq = 0
    for i in range(n):
        t = i * 16_666_667 + 1_000_000_000
        sid = sids[i % 3]
        comp = 1 if i % 2 == 0 else 4
        # composite writer (queue A) then gpupass (queue B), commit order A then B
        ca, cbb = cbn + 1, cbn + 2; cbn += 2
        cl.append('C t=%d cb=%d q=0xA tid=1 w=%d r=-' % (base_up + t, ca, comp))
        cl.append('C t=%d cb=%d q=0xB tid=2 w=%d r=%d' % (base_up + t + 200_000, cbb, sid, comp))
        inv = 0
        if mode == 'inv' and i % 5 == 0:   # B submitted before A
            cl.append('S t=%d tq=%d cb=%d q=0xB sub=%d dseq=%d cmt=%d cls=final-GPUPass disp=%d slot=0 w=%d r=%d inv=1' % (base_up + t + 400_000, base_up + t + 300_000, cbb, 2 * i + 1, i + 1, base_up + t + 200_000, sid, sid, comp))
            cl.append('S t=%d tq=%d cb=%d q=0xA sub=%d dseq=0 cmt=%d cls=render-composite disp=%d slot=-1 w=%d r=-  inv=0' % (base_up + t + 600_000, base_up + t + 500_000, ca, 2 * i + 2, base_up + t, comp, comp))
            cl.append('I t=%d rd_cb=%d rd_q=0xB rd_sub=%d sid=%d wr_cb=%d wr_q=0xA wr_commit=%d' % (base_up + t + 400_000, cbb, 2 * i + 1, comp, ca, base_up + t))
        else:
            cl.append('S t=%d tq=%d cb=%d q=0xA sub=%d dseq=0 cmt=%d cls=render-composite disp=%d slot=-1 w=%d r=-  inv=0' % (base_up + t + 300_000, base_up + t + 250_000, ca, 2 * i + 1, base_up + t, comp, comp))
            cl.append('S t=%d tq=%d cb=%d q=0xB sub=%d dseq=%d cmt=%d cls=final-GPUPass disp=%d slot=0 w=%d r=%d inv=0' % (base_up + t + 500_000, base_up + t + 450_000, cbb, 2 * i + 2, i + 1, base_up + t + 200_000, sid, sid, comp))
        seq += 1
        psid = sid if mode != 'seqbad' or i % 4 else comp   # seqbad: every 4th present shows a composite surface instead
        cl.append('P t0=%d t1=%d sid=%d seq=%d slot=0 rc=0' % (base_up + t + 3_000_000, base_up + t + 3_100_000, psid, seq))
        tsid = sid
        tl.append('TXN t=%d wall=%d tid=7 conn=9 pipe=1 via=IOConnectCallMethod mask=0x1 opt=0 p0type=0x1 p0flags=0 p0sid=%d p0sid2=0 rect=0:0:0:0 f08=0 f108=0 f110=0 p1type=0 p1sid=0' % (base_up + t + 2_900_000, base_w + t + 2_900_000, tsid))
        tl.append('SCAN12 t=%d wall=%d tid=8 conn=11 via=IOConnectCallMethod instruct=24 scalars=0' % (base_up + t + 3_000_100, base_w + t + 3_000_100))
    return tl, cl

def selftest():
    import tempfile, os
    ok = True
    for mode, want in (('inv', 'H-X'), ('seqbad', 'OPTION-C'), ('clean', 'NEITHER')):
        tl, cl = synth(mode)
        with tempfile.TemporaryDirectory() as d:
            a, b = os.path.join(d, 'txn.txt'), os.path.join(d, 'cb.txt')
            open(a, 'w').write('\n'.join(tl) + '\n'); open(b, 'w').write('\n'.join(cl) + '\n')
            args = argparse.Namespace(match_ms=100.0, inv_min=1, mismatch_frac=0.01, shift_ms=0.0, first=3)
            tx, s12, _, _ = load_txn(a); cb = load_cb([b], 0)
            res, out = analyse(tx, s12, cb, args)
        good = res['decision'] == want
        print('selftest %-7s -> %-9s (expect %s) %s' % (mode, res['decision'], want, 'ok' if good else 'FAIL'))
        if mode == 'inv' and not good: print('\n'.join(out))
        ok &= good
    return 0 if ok else 1

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='*'); ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--match-ms', type=float, default=100.0); ap.add_argument('--inv-min', type=int, default=1)
    ap.add_argument('--mismatch-frac', type=float, default=0.01); ap.add_argument('--shift-ms', type=float, default=0.0); ap.add_argument('--first', type=int, default=20)
    a = ap.parse_args()
    if a.selftest: sys.exit(selftest())
    if len(a.files) < 2: ap.error('need a txnlog file and at least one cblog file')
    tx, s12, s8, _ = load_txn(a.files[0]); cb = load_cb(a.files[1:], int(a.shift_ms * 1e6))
    res, out = analyse(tx, s12, cb, a)
    print('\n'.join(out))
    if s8: print('\n(selector 8 calls with another struct size, e.g. N48N WAITSEQ: %d, connections %s)' % (len(s8), sorted(set(x['conn'] for x in s8))))

if __name__ == '__main__': main()
