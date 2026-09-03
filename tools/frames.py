#!/usr/bin/env python3
"""Summarize a tqflicker-frames.csv produced by [debug] performance_trace.

    tools/frames.py cache/run1.csv [cache/run2.csv ...]

The question the probe exists to answer is not "how slow was it" but "whose
time was it". So the headline is the split: of the time spent inside hitching
frames, how much landed in one of the mod's own phases, and how much did not --
the remainder being the game's own frame, which for Titan Quest means its
synchronous level and resource loading.

With `performance_trace=1` only hitching frames are in the file, so the
frame-time distribution below describes those rows and not the session;
`performance_trace=full` writes every frame and the distribution then means
what it says.

Only the standard library: a few thousand rows do not need numpy, and the venv
it would live in is not always present.
"""

import csv
import sys
from collections import Counter, defaultdict

# Timed by us but not spent by us: this is the game's own Present, which is
# where a frame waits when the GPU is behind. Counting it as the mod's share
# overstates that share by a lot.
WAIT = "present_call"

# Also timed by us and also not ours: the game's own Draw/DrawIndexed and Map,
# bracketed around the driver call and nothing else when [debug] draw_timing is
# on. These columns exist to split the residual in findings.md §35, and they are
# the game's time in the D3D11 path -- charging them to the mod would invert the
# one number this script exists to print. They read 0 with draw_timing off; the
# `# draw_timing=` header line says which kind of zero it is.
THEIRS = ("draw_submit", "map_resource")
NOT_OURS = (WAIT,) + THEIRS

# Where the session divides, and why these two columns.
#
# `game_collisions` counts InterpenetrationManager::FixupCharacterCollisions,
# which cannot run without a character in a world, so its first non-zero row is
# where the menu ends. It is NOT where play begins: across runs 14-33 the game
# simulates collisions for another 646-1,670 frames -- 8.8 to 14.4 seconds --
# while a loading screen is still up and the world is not drawn.
# Splitting on it alone puts that whole loading screen in the "in play" half,
# and the frame that ends it is the largest frame of the session.
#
# `draw_indexed` separates them. The loading screen issues one indexed draw a
# frame; the menu's character preview issues 77-80; the first frame that draws
# the world issues 1,456-1,793 and never falls that low again. Any threshold
# between 100 and 1,400 works and 500 was chosen for the margin on both sides.
# It is also the only marker of the two that runs 9-13 can use, since
# `game_collisions` did not exist yet.
PLAY_DRAW = 500
PLAY_COLUMN = "draw_indexed"
MENU_COLUMN = "game_collisions"


def sections(rows, every_frame):
    """Split a full-session file into the five observable session parts.

    Returns [(label, rows), ...], one entry when the file gives no reason to
    split. A hitches-only file is never split: its rows are the hitches, so a
    row above the threshold says the world was drawn at some point, not that
    the rows before it were the menu.
    """
    if not every_frame:
        return [("whole file", rows)]
    world = next((i for i, row in enumerate(rows)
                  if number(row, PLAY_COLUMN) >= PLAY_DRAW), None)
    if world is None or world == 0:
        return [("whole file", rows)]
    collisions = next((i for i, row in enumerate(rows[:world])
                       if number(row, MENU_COLUMN) > 0), None)

    # The load-game frame is the large synchronous Region::LoadLevel before
    # collision simulation starts. Restricting the search to that prefix keeps
    # a later loading-screen timeout from stealing the label. Older files have
    # no collision column; for those, the largest pre-world frame is the best
    # marker available and is still kept separate from the first world frame.
    load_limit = collisions if collisions is not None and collisions > 0 else world
    load_game = max(range(load_limit), key=lambda i: number(rows[i], "ms"))
    if load_game == 0:
        return [("before play", rows[:world]),
                ("first world frame", rows[world:world + 1]),
                ("play", rows[world + 1:])]
    return [("menu", rows[:load_game]),
            ("load-game frame", rows[load_game:load_game + 1]),
            ("loading screen", rows[load_game + 1:world]),
            ("first world frame", rows[world:world + 1]),
            ("play", rows[world + 1:])]


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(len(ordered) * fraction))]


def number(row, key):
    try:
        return float(row.get(key) or 0.0)
    except ValueError:
        return 0.0


def load(path):
    rows, notes = [], []
    with open(path, newline="") as handle:
        text = [line for line in handle]
    notes = [line.strip() for line in text if line.startswith("#")]
    reader = csv.DictReader(line for line in text if not line.startswith("#"))
    rows = [row for row in reader if row.get("frame")]
    if not rows:
        raise SystemExit(f"{path}: no rows -- was performance_trace on for this run?")
    return rows, notes


def summarize(path):
    rows, notes = load(path)
    phases = [k[:-3] for k in rows[0]
              if k.endswith("_ms") and k != "ms" and not k.startswith("gpu_")]
    gpu = [k[:-3] for k in rows[0] if k.startswith("gpu_") and k.endswith("_ms")]

    frames = [int(row["frame"]) for row in rows]
    span = frames[-1] - frames[0] + 1
    # The header names the mode; the contiguity heuristic is only for files
    # from before the marker existed, and it can misread a single burst of
    # consecutive hitches as a full session.
    marker = next((n for n in notes if "performance_trace=" in n), "")
    if marker:
        every_frame = "full" in marker
    else:
        every_frame = len(rows) == span
    frame_ms = [number(row, "ms") for row in rows]

    total = sum(frame_ms)
    ours = 0.0
    waiting = 0.0
    theirs = 0.0
    per_phase = defaultdict(float)
    for row in rows:
        # Every _ms column is exclusive by construction -- the probe subtracts
        # nested scopes before writing the row -- so summing is attribution.
        # (Files from before 1.7 wrote `present` inclusive; re-analyse those
        # knowing their present column double-counts its children.)
        values = {p: number(row, p + "_ms") for p in phases}
        ours += sum(v for k, v in values.items() if k not in NOT_OURS)
        waiting += values.get(WAIT, 0.0)
        theirs += sum(values.get(k, 0.0) for k in THEIRS)
        for name, value in values.items():
            per_phase[name] += value

    print(f"\n{path}")
    mode = "every frame" if every_frame else "hitching frames only"
    print(f"  {len(rows)} rows, {mode}; frame indices {frames[0]}-{frames[-1]}"
          f" ({span} frames presented)")
    label = "frame time" if every_frame else "time in the logged rows"
    print(f"  {label}: p50 {percentile(frame_ms, 0.50):.1f} ms"
          f"   p99 {percentile(frame_ms, 0.99):.1f} ms"
          f"   max {max(frame_ms):.1f} ms   total {total:.0f} ms")

    print(f"\n  whose time was it, across those {len(rows)} rows")
    print(f"    the mod            {ours:8.0f} ms  {ours / total * 100:5.1f}%")
    print(f"    waiting on Present {waiting:8.0f} ms  {waiting / total * 100:5.1f}%")
    if theirs:
        print(f"    the game's D3D     {theirs:8.0f} ms  {theirs / total * 100:5.1f}%"
              f"   (draw_submit + map_resource)")
    rest = total - ours - waiting - theirs
    print(f"    the game's frame   {rest:8.0f} ms  {rest / total * 100:5.1f}%")

    # Every number above this line is the whole file, menu included, and the
    # menu is a quarter to nearly half of the rows and far cheaper than play --
    # so read the split below instead. It also varies: runs 14-33 hold
    # 1,719-5,454 menu rows against a near-constant 3,853-4,656 rows of play,
    # which is what made run-to-run p50 comparisons compare a varying amount
    # of menu. Menu p50 is 7.3-8.2 ms and play p50 13.5-14.3 ms, so a
    # whole-file p50 lands between the two and describes neither.
    parts = sections(rows, every_frame)
    if len(parts) > 1:
        print("\n  by phase of the session")
        print(f"    {'':<15} {'rows':>6} {'p50':>7} {'p99':>8} {'max':>9}"
              f" {'total':>9} {'mod':>7}")
        for label, part in parts:
            times = [number(row, "ms") for row in part]
            if not times:
                continue
            elapsed = sum(times)
            mine = sum(sum(number(row, name + "_ms") for name in phases
                           if name not in NOT_OURS) for row in part)
            print(f"    {label:<15} {len(part):>6} {percentile(times, 0.50):>6.1f}ms"
                  f" {percentile(times, 0.99):>7.1f}ms {max(times):>8.1f}ms"
                  f" {elapsed:>8.0f}ms"
                  f" {mine / elapsed * 100 if elapsed else 0:>6.1f}%")

    marked = [row for row in rows if number(row, "stutter_marker") > 0]
    if marked:
        part_for_frame = {}
        for label, part in parts:
            for row in part:
                part_for_frame[int(row["frame"])] = label
        print("\n  stutter markers (F12; candidates are before the reaction)")
        for row in marked:
            frame = int(row["frame"])
            at = rows.index(row)
            print(f"    marker frame {frame:<7}"
                  f" {part_for_frame.get(frame, 'whole file'):<17}"
                  f" {number(row, 'ms'):7.1f} ms")
            candidates = []
            window = []
            age = 0.0
            for candidate_index in range(at, -1, -1):
                if candidate_index < at:
                    age += number(rows[candidate_index + 1], "ms")
                if age > 2000.0:
                    break
                candidate = rows[candidate_index]
                window.append((candidate, age))
                if number(candidate, "ms") >= 40.0:
                    candidates.append((candidate, age))
            if not candidates:
                candidates = [max(window,
                                  key=lambda item: number(item[0], "ms"))]
            for candidate, age in candidates[:12]:
                onset = age + number(candidate, "ms")
                print(f"      candidate {int(candidate['frame']):<7}"
                      f" ended {age:5.0f} ms before marker;"
                      f" onset-to-marker {onset:5.0f} ms:"
                      f" frame {number(candidate, 'ms'):6.1f} ms,"
                      f" render {number(candidate, 'engine_render_us') / 1000:6.1f} ms,"
                      f" pump {number(candidate, 'pump_peek_us') / 1000:6.1f} ms,"
                      f" sent-wndproc"
                      f" {number(candidate, 'pump_sent_wndproc_us') / 1000:6.1f} ms")

    print("\n  the mod's share, by phase")
    for name, value in sorted(per_phase.items(), key=lambda item: -item[1]):
        if value < 0.5 or name in NOT_OURS:
            continue
        print(f"    {name:<16} {value:8.1f} ms")

    timed = [(name, per_phase[name]) for name in THEIRS if per_phase.get(name)]
    if timed:
        print("\n  the game's own D3D11 calls, timed but not ours")
        for name, value in timed:
            print(f"    {name:<16} {value:8.1f} ms")

    counters = [k for k in rows[0]
                if not k.endswith("_ms") and k not in ("frame", "ms", "unusual")]
    totals = {c: sum(int(row.get(c) or 0) for row in rows) for c in counters}
    interesting = {c: v for c, v in totals.items() if v and (
        c.startswith("grass_") or c.startswith("upload_")
        or c in ("shadow_fit_change", "shader_create"))}
    if interesting:
        print("\n  notable counts in those rows")
        for name, value in sorted(interesting.items(), key=lambda i: -i[1]):
            print(f"    {name:<20} {value}")

    # The names in `gpu` already have "_ms" stripped; look the column back up
    # under its real header or every row reads as unresolved.
    resolved = sum(1 for row in rows if row.get(gpu[0] + "_ms")) if gpu else 0
    if gpu:
        if resolved:
            print("\n  GPU, mean over the rows that resolved")
            for name in gpu:
                values = [number(row, name + "_ms") for row in rows
                          if row.get(name + "_ms")]
                if values:
                    print(f"    {name:<20} {sum(values) / len(values):6.2f} ms"
                          f"   max {max(values):6.2f} ms")
        else:
            print("\n  GPU: no row resolved a timestamp")

    hitches = Counter()
    for row in rows:
        fields = (row.get("unusual") or "").split()
        for field in fields:
            name, _, delta = field.partition(":+")
            if name in phases and delta:
                hitches[name] += 1
                break
    if hitches:
        print("\n  hitches by dominant phase")
        for name, n in hitches.most_common():
            print(f"    {name:<16} {n}")
        unnamed = len(rows) - sum(hitches.values())
        if unnamed:
            print(f"    {'(none of ours)':<16} {unnamed}")

    for note in notes:
        print(f"\n  {note.lstrip('# ')}")


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    for path in argv[1:]:
        summarize(path)
    print()


if __name__ == "__main__":
    main(sys.argv)
