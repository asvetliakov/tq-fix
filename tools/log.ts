/**
 * Tail the in-game log from macOS.
 *
 *   npm run log            follow it
 *   npm run log -- --all   print it and exit
 *
 * The log is the debugger (CLAUDE.md), and it lives inside the bottle at
 * %TEMP%\tqflicker.log. %TEMP% resolves to AppData\Local\Temp here, **not**
 * users/crossover/Temp, which does not exist - Stage 2 got that wrong once in a
 * printed message and it cost a minute of looking in the wrong place.
 */
import { existsSync, statSync, createReadStream } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

const bottle =
  process.env.TQ_BOTTLE ??
  join(homedir(), 'Library/Application Support/CrossOver/Bottles/New Bottle');

const LOG = join(bottle, 'drive_c/users/crossover/AppData/Local/Temp/tqflicker.log');

const all = process.argv.includes('--all');

function dump(from: number, to: number): Promise<void> {
  return new Promise((resolve) => {
    if (to <= from) return resolve();
    const s = createReadStream(LOG, { start: from, end: to - 1, encoding: 'utf8' });
    s.on('data', (c) => process.stdout.write(String(c).replace(/\r\n/g, '\n')));
    s.on('end', () => resolve());
    s.on('error', () => resolve());
  });
}

if (!existsSync(LOG)) {
  console.error(`no log yet: ${LOG}`);
  console.error('the DLL writes it at DLL_PROCESS_ATTACH - so either the game has not run,');
  console.error('or the proxy was not loaded. Check: npm run doctor');
  if (all) process.exit(1);
}

let pos = 0;
if (existsSync(LOG)) {
  const size = statSync(LOG).size;
  // Follow mode starts from the end unless asked for everything, so a long
  // accumulated log does not bury the run being watched.
  pos = all ? 0 : Math.max(0, size - 4096);
  await dump(pos, size);
  pos = size;
}

if (all) process.exit(0);

console.error(`--- following ${LOG} (ctrl-c to stop) ---`);
setInterval(async () => {
  if (!existsSync(LOG)) return;
  const size = statSync(LOG).size;
  // A shrinking file means something replaced it; start over rather than
  // reading from a stale offset into the middle of a line.
  if (size < pos) pos = 0;
  if (size > pos) {
    await dump(pos, size);
    pos = size;
  }
}, 500);
