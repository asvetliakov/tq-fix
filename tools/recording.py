#!/usr/bin/env python3
"""
Turn a 10fps screen recording into a per-frame anomaly list, and answer the
Stage 4 question against the per-frame draw table the DLL writes.

    python3 tools/recording.py RECORDING.mov                 anomalies only
    python3 tools/recording.py RECORDING.mov FRAMES.log      ...and the verdict

Needs numpy and ffmpeg/ffprobe:  python3 -m venv cache/venv &&
cache/venv/bin/pip install numpy  &&  cache/venv/bin/python tools/recording.py ...
Glob the recording's name - macOS puts a U+202F before AM/PM.

This is the Stage 0 detector (docs/rev/observed.md O12/O14):

  * macOS records only on change, so at a 10fps cap one recorded frame is one
    game frame. The pts list says where that 1:1 region is; outside it nothing
    can be concluded, and those frames are marked and excluded.
  * Frames are reduced to 16x16-block means in grayscale. A block is anomalous
    on frame i when it differs from BOTH neighbours by more than a threshold
    AND the neighbours agree with each other (|f(i-1)-f(i+1)| < 0.35 x
    |f(i-1)-f(i)|) - something absent for exactly one frame, with motion
    rejected. Both polarities: a missing beam darkens, a missing shadow
    brightens.
  * ffmpeg is asked for -fps_mode passthrough. Without it, frames are
    duplicated to a constant rate, the one-frame test breaks, and the anomaly
    count collapses (61 -> 2 on the E6 file).

## How the recording is aligned to the table, and why it is trustworthy

**By wall clock, not by matching anomalies to dips** - matching them would
assume the answer. The recording's container start time plus its duration give
the span; table rows carry local timestamps; the overlap is the recorded span.
Verified on the Stage 4 run: birth 00:24:40 + 38.74s = 00:25:18 = the file's
mtime, so the container time is the START of the recording (O30).

**The verdict is then offset-independent.** It compares two counts over that
span - how many frames show an object vanish, and how many show the draw count
fall below both neighbours - so a second or two of clock error cannot change
it. The per-anomaly listing that follows does depend on the offset and is
labelled as indicative.

Also printed: the instrument's own sensitivity, i.e. how often the draw count
is identical between consecutive frames. A missing draw is -1, so that number
is what says whether -1 would have been visible at all.
"""
import json, subprocess, sys, datetime, collections
try:
    import numpy as np
except ImportError:
    sys.exit("numpy is needed: python3 -m venv cache/venv && cache/venv/bin/pip install numpy, then run with cache/venv/bin/python")

W, H, BS, THRESH, AGREE = 568, 430, 16, 3.0, 0.35


def probe(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                          'frame=pts_time:stream=width,height:format=duration:format_tags=creation_time',
                          '-of', 'json', path], capture_output=True, text=True, check=True).stdout
    j = json.loads(out)
    pts = [float(f['pts_time']) for f in j['frames']]
    fmt = j.get('format', {})
    ct = fmt.get('tags', {}).get('creation_time')
    dur = float(fmt.get('duration', 0) or 0)
    return pts, ct, dur, j['streams'][0]['width'], j['streams'][0]['height']


def frames(path, n):
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-fps_mode', 'passthrough', '-vf',
                          f'scale={W}:{H},format=gray', '-f', 'rawvideo', '-'],
                         capture_output=True, check=True).stdout
    st = np.frombuffer(raw, np.uint8)
    got = len(st) // (W * H)
    if got != n:
        # -fps_mode passthrough keeps the variable frame rate as recorded; a
        # mismatch here means frames were duplicated or dropped and the 1:1
        # premise (O12) is gone for this file.
        print(f'!! ffmpeg produced {got} frames but ffprobe counted {n} - not 1:1, results are suspect')
    got = min(got, n)
    return st[:got * W * H].reshape(got, H, W).astype(np.float32)


def anomalies(st, start):
    N = st.shape[0]
    blocks = st[:, :H // BS * BS, :W // BS * BS].reshape(N, H // BS, BS, W // BS, BS)
    ev = collections.defaultdict(list)
    for by in range(H // BS):
        for bx in range(W // BS):
            s = blocks[:, by, :, bx, :]
            m = s.mean(axis=(1, 2))
            for i in range(max(start, 1), N - 1):
                ref = (m[i - 1] + m[i + 1]) / 2
                if abs(m[i] - ref) < THRESH:
                    continue
                nb = np.abs(s[i - 1] - s[i + 1]).mean()
                sig = np.abs(s[i - 1] - s[i]).mean()
                if nb < AGREE * sig:
                    ev[i].append((bx * BS, by * BS, m[i] - ref))
    return ev


def read_table(path):
    rows = []
    with open(path, encoding='utf8', errors='replace') as f:
        head = f.readline().rstrip('\r\n').split('\t')
        for l in f:
            c = l.rstrip('\r\n').split('\t')
            if len(c) != len(head):
                continue
            r = dict(zip(head, c))
            for k in r:
                if k != 'time':
                    r[k] = float(r[k])
            hh, mm, ss = r['time'].split(':')
            r['t'] = int(hh) * 3600 + int(mm) * 60 + float(ss)
            rows.append(r)
    return rows


def one_to_one(pts):
    """Indices whose interval to BOTH neighbours is ~100ms: where one recorded
    frame is one game frame. Outside this, an anomaly means nothing."""
    ok = set()
    for i in range(1, len(pts) - 1):
        if 0.095 < pts[i] - pts[i - 1] < 0.105 and 0.095 < pts[i + 1] - pts[i] < 0.105:
            ok.add(i)
    return ok


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    rec = sys.argv[1]
    table = sys.argv[2] if len(sys.argv) > 2 else None

    pts, ct, dur, w, h = probe(rec)
    st = frames(rec, len(pts))
    ok = one_to_one(pts)
    first = min(ok) if ok else 0
    print(f'{rec}: {w}x{h}, {len(pts)} frames, {dur:.2f}s, container time {ct}')
    print(f'1:1 frames (100ms to both neighbours): {len(ok)} of {len(pts)}, from frame {first}')

    ev = anomalies(st, 1)
    outside = [i for i in ev if i not in ok]
    for i in sorted(ev):
        blocks = ev[i]
        xs = [b[0] for b in blocks]; ys = [b[1] for b in blocks]
        pol = 'DARK' if sum(b[2] for b in blocks) < 0 else 'BRIGHT'
        print(f'  rec frame {i:4d}  t={pts[i]:7.3f}s  {pol:6s} blocks={len(blocks):2d}  '
              f'x {min(xs)}-{max(xs) + BS} y {min(ys)}-{max(ys) + BS}  peak {max(abs(b[2]) for b in blocks):.1f}'
              + ('' if i in ok else '   (NOT 1:1 here - ignore)'))
    ev = {i: b for i, b in ev.items() if i in ok}
    print(f'\nconfirmed one-frame anomalies inside the 1:1 region: {len(ev)} '
          f'({100 * len(ev) / max(1, len(ok)):.1f}% of 1:1 frames)')
    if outside:
        print(f'({len(outside)} more outside it, ignored)')
    if not table:
        return

    rows = read_table(table)
    if not rows:
        sys.exit(f'{table}: no rows')

    # ---- the span, by wall clock. The container time is the recording's START
    # (verified: birth + duration == mtime, O30). Table rows are local time.
    if not ct:
        sys.exit('the recording has no container timestamp - cannot align by clock')
    local = datetime.datetime.fromisoformat(ct.replace('Z', '+00:00')).astimezone()
    t0 = local.hour * 3600 + local.minute * 60 + local.second + local.microsecond / 1e6
    t1 = t0 + dur
    span = [k for k, r in enumerate(rows) if t0 <= r['t'] <= t1]
    if len(span) < 10:
        sys.exit(f'the recording ({local:%H:%M:%S}, {dur:.1f}s) does not overlap the table '
                 f'({rows[0]["time"]} .. {rows[-1]["time"]}) - wrong pair of files?')
    a, b = span[0], span[-1]
    draws = np.array([r['draws'] for r in rows])
    seg = draws[a:b + 1]
    print(f'\n{table}: {len(rows)} rows, {rows[0]["time"]} .. {rows[-1]["time"]}')
    print(f'recorded span = rows {a}..{b} ({len(seg)} rows, {rows[a]["time"]} .. {rows[b]["time"]})')
    print(f'draws over the span: min {seg.min():.0f} median {np.median(seg):.0f} max {seg.max():.0f}')

    # ---- sensitivity: a missing draw is -1, so how quiet is the count?
    d = np.diff(seg)
    same = int((d == 0).sum())
    print(f'\nINSTRUMENT SENSITIVITY: the draw count is IDENTICAL between consecutive frames on '
          f'{same} of {len(d)} frames ({100 * same / max(1, len(d)):.1f}%)')
    print('  so a single un-issued draw would show as a -1 against a flat line.')

    dip_rows = [i for i in range(1, len(seg) - 1) if seg[i] < seg[i - 1] and seg[i] < seg[i + 1]]
    print(f'\ndraw-count DIPS inside the recorded span: {len(dip_rows)}')
    for i in dip_rows:
        print(f'  row {a + i} ({rows[a + i]["time"]}): {seg[i - 1]:.0f} | {seg[i]:.0f} | {seg[i + 1]:.0f}'
              f'   (-{min(seg[i - 1], seg[i + 1]) - seg[i]:.0f})')

    # ---- THE VERDICT, and it does not depend on the offset within the span.
    n_drop, n_dip = len(ev), len(dip_rows)
    print(f'\n=== Stage 4: did the game issue the draw? ===')
    print(f'over the same {len(seg)} frames: {n_drop} frame(s) show an object vanish, '
          f'{n_dip} frame(s) show the draw count fall.')
    if n_drop and n_dip < n_drop / 4:
        print(f'ANSWER: the game ISSUED the draw. Objects vanish {n_drop / max(1, n_dip):.0f}x more often '
              f'than the draw count falls, so the engine was still submitting them.')
        print('        The defect is in the D3D11->Metal translation, not in the engine skipping.')
    elif n_dip >= n_drop * 0.5:
        print('ANSWER: the draw count falls about as often as objects vanish - consistent with the')
        print('        engine SKIPPING the draw. Check the per-frame listing below to confirm.')
    else:
        print('AMBIGUOUS: neither reading is clean. Do not record an answer from this run.')
    print('This comparison is offset-independent: it is two counts over one span, so a second of')
    print('clock error cannot change it. The per-frame listing below does depend on the offset.')

    # ---- indicative per-anomaly listing at the clock offset
    off = a - first
    print(f'\nindicative, at the clock offset (rec frame i -> row i+{off}):')
    empties = busies = 0
    for i in sorted(ev):
        k = i + off
        if not 0 <= k < len(rows):
            continue
        r = rows[k]
        prev = rows[k - 1]['draws'] if k else float('nan')
        nxt = rows[k + 1]['draws'] if k + 1 < len(rows) else float('nan')
        mark = 'DIP' if (k - a) in dip_rows else '   '
        empties += int(r['empty'] > 0); busies += int(r['maps_busy'] > 0)
        print(f'  rec {i:4d} -> row {k:4d} {r["time"]}  draws {prev:.0f} | {r["draws"]:.0f} | {nxt:.0f}'
              f'  {mark}  empty {r["empty"]:.0f} busy {r["maps_busy"]:.0f}')
    print(f'\nof those frames: {empties} had a draw issued with zero indices, '
          f'{busies} had a Map return WAS_STILL_DRAWING')
    tot_e = sum(1 for r in rows if r['empty'])
    tot_b = sum(1 for r in rows if r['maps_busy'])
    print(f'over the whole session: {tot_e} frame(s) with an empty draw, {tot_b} with a busy Map')


if __name__ == '__main__':
    main()
