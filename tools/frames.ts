/**
 * Read the per-frame table Stage 4 writes and say what is in it.
 *
 *   npm run frames                      the current table inside the bottle
 *   npm run frames -- cache/logs/x.tsv  a kept copy
 *   npm run frames -- --around 1234     the rows around frame 1234
 *   npm run frames -- --dips            every frame whose draw count is below
 *                                       both neighbours, with the delta
 *
 * The table is %TEMP%\tqflicker-frames.log: one tab-separated row per Present,
 * columns named in its header. It is truncated when the game creates its
 * device, so it always describes the most recent run - keep a copy with
 * scripts/keep-log.sh before launching again (Risk 12).
 */
import { existsSync, readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

const bottle =
  process.env.TQ_BOTTLE ??
  join(homedir(), 'Library/Application Support/CrossOver/Bottles/Titan Quest');
const DEFAULT = join(bottle, 'drive_c/users/crossover/AppData/Local/Temp/tqflicker-frames.log');

const args = process.argv.slice(2);
const file = args.find((a) => !a.startsWith('--') && !/^\d+$/.test(a)) ?? DEFAULT;
const around = args.includes('--around') ? Number(args[args.indexOf('--around') + 1]) : null;
const dipsOnly = args.includes('--dips');

if (!existsSync(file)) {
  console.error(`no frames table at ${file} - the game has not presented a frame with the Stage 4 build in`);
  process.exit(1);
}

export interface Row {
  time: string; pid: number; frame: number; dt: number; sync: number; draws: number;
  DrawIndexed: number; Draw: number; DrawIndexedInstanced: number; DrawInstanced: number;
  other: number; empty: number; verts: number; maps: number; maps_busy: number; new_buffers: number;
}

export function parse(text: string): Row[] {
  const lines = text.split(/\r?\n/).filter((l) => l.length);
  const head = lines[0].split('\t');
  return lines.slice(1).map((l) => {
    const c = l.split('\t');
    const o: Record<string, string | number> = {};
    head.forEach((h, i) => (o[h === 'dt_ms' ? 'dt' : h] = h === 'time' ? c[i] : Number(c[i])));
    return o as unknown as Row;
  });
}

/** Frames whose draw count is below both neighbours: the shape a missing draw
 *  would have in this table. */
export function dips(rows: Row[]): { row: Row; below: number }[] {
  const out: { row: Row; below: number }[] = [];
  for (let i = 1; i + 1 < rows.length; i++) {
    const a = rows[i - 1].draws, b = rows[i].draws, c = rows[i + 1].draws;
    if (b < a && b < c) out.push({ row: rows[i], below: Math.min(a, c) - b });
  }
  return out;
}

const rows = parse(readFileSync(file, 'utf8'));
if (!rows.length) { console.error('empty table'); process.exit(1); }

function fmt(r: Row): string {
  return `${r.time}  f${String(r.frame).padStart(5)}  dt ${r.dt.toFixed(1).padStart(6)}ms  draws ${String(r.draws).padStart(4)}` +
    `  (DI ${r.DrawIndexed} D ${r.Draw} DII ${r.DrawIndexedInstanced} DIn ${r.DrawInstanced} o ${r.other})` +
    `  empty ${r.empty}  verts ${r.verts}  maps ${r.maps}${r.maps_busy ? ` BUSY ${r.maps_busy}` : ''}` +
    `${r.new_buffers ? `  newbuf ${r.new_buffers}` : ''}`;
}

if (around !== null) {
  const i = rows.findIndex((r) => r.frame === around);
  if (i < 0) { console.error(`no frame ${around}`); process.exit(1); }
  for (let k = Math.max(0, i - 5); k <= Math.min(rows.length - 1, i + 5); k++)
    console.log((k === i ? '>> ' : '   ') + fmt(rows[k]));
  process.exit(0);
}

const d = dips(rows);
if (dipsOnly) {
  for (const { row, below } of d) console.log(`${fmt(row)}   <- ${below} below both neighbours`);
  console.log(`${d.length} dip frame(s) of ${rows.length}`);
  process.exit(0);
}

const draws = rows.map((r) => r.draws).sort((a, b) => a - b);
const dts = rows.map((r) => r.dt).slice(1);
const capped = dts.filter((x) => x > 95 && x < 105).length;
const first = rows[0], last = rows[rows.length - 1];
console.log(`${file}`);
console.log(`${rows.length} frames, pid ${first.pid}, ${first.time} .. ${last.time}`);
const medDt = dts.slice().sort((a, b) => a - b)[dts.length >> 1] ?? 0;
// The median, not the fraction: a session includes loading and menus, whose
// frames are not capped, so a strict fraction reads a properly capped
// gameplay run as uncapped. 909 frames at the cap gave 74% within 95-105ms.
console.log(`dt: median ${medDt.toFixed(1)}ms; ${capped} of ${dts.length} frames within 95-105ms` +
  (medDt > 95 && medDt < 105 ? '  (10fps cap in effect: 1 recorded frame == 1 game frame, O12)' : '  (NOT at the 10fps cap - a recording will not be 1:1)'));
console.log(`draws/frame: min ${draws[0]}  median ${draws[draws.length >> 1]}  max ${draws[draws.length - 1]}`);
console.log(`sync interval: ${[...new Set(rows.map((r) => r.sync))].join(', ')}`);
console.log(`frames with an empty draw: ${rows.filter((r) => r.empty).length};  with a busy Map: ${rows.filter((r) => r.maps_busy).length};  ` +
  `with a buffer created: ${rows.filter((r) => r.new_buffers).length}`);
console.log(`dip frames (draws below both neighbours): ${d.length}  (--dips to list them)`);
const hist = new Map<number, number>();
for (const { below } of d) hist.set(below, (hist.get(below) ?? 0) + 1);
if (hist.size) console.log(`  by depth: ${[...hist.entries()].sort((a, b) => a[0] - b[0]).map(([k, v]) => `${k}:${v}`).join('  ')}`);
