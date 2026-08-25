#!/usr/bin/env python3
"""
Turn a 10fps screen recording into a per-frame anomaly list, and align it
against the per-frame draw table the DLL writes.

    python3 tools/recording.py RECORDING.mov                 anomalies only
    python3 tools/recording.py RECORDING.mov FRAMES.log      ...aligned to the table

Needs numpy (and ffmpeg/ffprobe on PATH):  python3 -m venv cache/venv &&
cache/venv/bin/pip install numpy  &&  cache/venv/bin/python tools/recording.py ...

This is the Stage 0 detector (docs/rev/observed.md O12/O14), committed at last:

  * macOS records only on change, so at a 10fps cap one recorded frame is one
    game frame. The pts list says where that 1:1 region starts (100ms steps).
  * Frames are reduced to 16x16-block means in grayscale. A block is anomalous
    on frame i when it differs from BOTH neighbours by more than a threshold
    AND the neighbours agree with each other (|f(i-1)-f(i+1)| < 0.35 x
    |f(i-1)-f(i)|), which is the signature of something absent for exactly one
    frame and rejects motion. Both polarities: a missing beam darkens, a
    missing shadow brightens.

Alignment: the table rows carry wall-clock times and the recording carries a
creation_time, which is only good to a second. So every offset within a few
seconds is tried, and the one under which recording-anomaly frames coincide
most with draw-count dips in the table is reported - together with the number
of coincidences expected by chance, so "no alignment" is a result too.
"""
import json, subprocess, sys, datetime, collections
try:
    import numpy as np
except ImportError:
    sys.exit("numpy is needed: python3 -m venv cache/venv && cache/venv/bin/pip install numpy, then run with cache/venv/bin/python")

W, H, BS, THRESH, AGREE = 568, 430, 16, 3.0, 0.35


def probe(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                          'frame=pts_time:stream=width,height:format_tags=creation_time',
                          '-of', 'json', path], capture_output=True, text=True, check=True).stdout
    j = json.loads(out)
    pts = [float(f['pts_time']) for f in j['frames']]
    ct = j.get('format', {}).get('tags', {}).get('creation_time')
    return pts, ct, j['streams'][0]['width'], j['streams'][0]['height']


def frames(path, n):
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-fps_mode', 'passthrough', '-vf', f'scale={W}:{H},format=gray',
                          '-f', 'rawvideo', '-'], capture_output=True, check=True).stdout
    st = np.frombuffer(raw, np.uint8)
    got = len(st) // (W * H)
    if got != n:
        # -fps_mode passthrough keeps the variable frame rate as recorded; a
        # mismatch here means frames were duplicated or dropped and the 1:1
        # premise (O12) is gone for this file.
        print(f'!! ffmpeg produced {got} frames but ffprobe counted {n} - not 1:1, results are suspect')
    got = min(got, n)
    return st[:got * W * H].reshape(got, H, W).astype(np.float32)


def one_to_one_start(pts):
    """First index from which every step is ~100ms: the region where recorded == game frame."""
    for i in range(1, len(pts)):
        if all(0.095 < pts[k] - pts[k - 1] < 0.105 for k in range(i, min(i + 20, len(pts)))):
            return i
    return len(pts)


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


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    rec = sys.argv[1]
    table = sys.argv[2] if len(sys.argv) > 2 else None

    pts, ct, w, h = probe(rec)
    st = frames(rec, len(pts))
    start = one_to_one_start(pts)
    print(f'{rec}: {w}x{h}, {len(pts)} frames, creation_time {ct}')
    print(f'1:1 region (100ms steps) from frame {start}: {len(pts) - start} clean frames')
    ev = anomalies(st, start)
    print(f'frames with >=1 confirmed one-frame anomaly: {len(ev)} '
          f'({100 * len(ev) / max(1, len(pts) - start):.1f}%)')
    def clean(i):
        """Are this frame and both neighbours 100ms apart? Outside that, one
        recorded frame is not one game frame and an anomaly means nothing."""
        return all(0.095 < pts[k] - pts[k - 1] < 0.105 for k in (i, i + 1) if 0 < k < len(pts))
    for i in sorted(ev):
        blocks = ev[i]
        xs = [b[0] for b in blocks]; ys = [b[1] for b in blocks]
        pol = 'DARK' if sum(b[2] for b in blocks) < 0 else 'BRIGHT'
        print(f'  rec frame {i:4d}  t={pts[i]:7.3f}s  {pol:6s} blocks={len(blocks):2d}  '
              f'x {min(xs)}-{max(xs) + BS} y {min(ys)}-{max(ys) + BS}  peak {max(abs(b[2]) for b in blocks):.1f}'
              + ('' if clean(i) else '   (NOT 1:1 here - ignore)'))
    ev = {i: b for i, b in ev.items() if clean(i)}
    print(f'{len(ev)} anomaly frames inside the 1:1 region are used for alignment')
    if not table:
        return

    rows = read_table(table)
    dips = {int(rows[i]['frame']): min(rows[i - 1]['draws'], rows[i + 1]['draws']) - rows[i]['draws']
            for i in range(1, len(rows) - 1)
            if rows[i]['draws'] < rows[i - 1]['draws'] and rows[i]['draws'] < rows[i + 1]['draws']}
    print(f'\n{table}: {len(rows)} rows, {len(dips)} dip frames (draws below both neighbours)')

    # Coarse offset from the clocks. creation_time is UTC; the table is local time.
    guess = None
    if ct:
        utc = datetime.datetime.fromisoformat(ct.replace('Z', '+00:00'))
        local = utc.astimezone()
        rec_t0 = local.hour * 3600 + local.minute * 60 + local.second + local.microsecond / 1e6
        # table row t is when the game finished submitting that frame
        near = min(range(len(rows)), key=lambda k: abs(rows[k]['t'] - rec_t0 - pts[start]))
        guess = near - start
        print(f'clock guess: recording frame {start} ~ table row {near} (game frame {int(rows[near]["frame"])}); '
              f'offset {guess} rows (creation_time is only good to ~1s = ~10 frames)')

    # Try offsets: rec frame i <-> table row i + off. Score = anomaly frames that land on a dip.
    anom = sorted(ev)
    frame_of_row = [int(r['frame']) for r in rows]
    dip_rows = {k for k, r in enumerate(rows) if int(r['frame']) in dips}
    lo, hi = (guess - 40, guess + 40) if guess is not None else (-len(rows), len(rows))
    best = []
    for off in range(lo, hi + 1):
        hits = sum(1 for i in anom if 0 <= i + off < len(rows) and (i + off) in dip_rows)
        best.append((hits, off))
    best.sort(reverse=True)
    chance = len(anom) * len(dip_rows) / max(1, len(rows))
    print(f'\nalignment search over offsets {lo}..{hi}: anomaly frames landing on a dip row')
    print(f'  expected by chance: {chance:.1f} of {len(anom)}')
    for hits, off in best[:5]:
        print(f'  offset {off:+5d}: {hits} of {len(anom)}')
    hits, off = best[0]
    verdict = ('ALIGNED - the recording\'s missing objects coincide with draw-count dips'
               if hits >= max(3, 2 * chance) else
               'NO alignment - anomalies do not coincide with dips at any offset: the draw count'
               ' does not fall on the bad frames')
    print(f'\n{verdict}')
    print('per anomaly frame, at the best offset:')
    for i in anom:
        k = i + off
        if not 0 <= k < len(rows):
            continue
        r = rows[k]
        prev = rows[k - 1]['draws'] if k else float('nan')
        nxt = rows[k + 1]['draws'] if k + 1 < len(rows) else float('nan')
        mark = f'DIP -{dips[int(r["frame"])]:.0f}' if k in dip_rows else '   '
        print(f'  rec {i:4d} -> row {k:4d} game frame {int(r["frame"]):5d} {r["time"]}  '
              f'draws {prev:.0f} | {r["draws"]:.0f} | {nxt:.0f}  {mark}  '
              f'empty {r["empty"]:.0f} busy {r["maps_busy"]:.0f}')


if __name__ == '__main__':
    main()
