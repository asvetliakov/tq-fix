# Grass cache capacity and eviction

## Evidence before the change

The local renderer-hook test is archived in
`cache/runs/renderer-hooks-before-grass-cache/` with its log, INI and installed
DLL. The CSV contains 6,992 consecutive frames. Renderer Draw/DrawIndexed
hooks installed, SMAA ran, and secondary admission suppressed 8,794 draws.

Of 415 frames with more than 256 grass draws, only 399 of 133,138 draws got
crossed companions, while the cache recorded 133,315 adoptions. The longest
zero-crossing interval was frames 2868–2943 (76 frames, 2.08 seconds).
Run 85 already showed the same pattern: 462 crossings out of 126,669 dense
draws, with 126,816 adoptions. Draw counts alone do not identify unique
buffers; the repeated adoption counts and the eviction logic establish the
working-set pressure. A 256-entry cache cannot retain a repeated traversal
of over 330 distinct streams when every miss evicts the oldest draw.

The newer lifetime fix correctly cancelled readbacks when their slots were
reused. Under this churn, 649 queued seeds produced only 127 completions,
with no reported seed failures. Cancellation is not a readback failure.

Comparable fully crossed steady frames (100–170 grass draws, frame time below
60 ms, no fills, adoptions, twin creations or seed activity) averaged 0.09475
ms grass CPU across 180 frames versus 0.10090 ms across 251 run-85 frames.
This is a workload-filtered comparison, not a controlled benchmark. The user
explicitly accepts first-world-frame loading stalls; those are outside this
change's scope.

## Policy

- Raise confirmed-stream capacity from 256 to 512; keep candidates at 256.
  Sources and twins allocate on demand. At capacity their combined buffer
  payload is 43.75 MiB, up from 21.875 MiB, excluding driver overhead and the
  unchanged candidate/staging/scratch allocations. Sources are retained game
  buffers, not additional copies.
- Keep the current and previous frame's drawn or mapped streams. This avoids
  eviction before a later draw in the new frame has refreshed its entry.
- Pin in-flight staging reads, mapped sources and pending uploads. Refilling
  a source still cancels its obsolete seed before publishing fresh contents.
- Choose the oldest eligible frame when eviction is necessary. If every
  entry is protected, reject new admissions for the remainder of that frame.
  Overflow blocks retain original grass instead of displacing visible twins.
- Double the pointer index to 4,096 buckets, preserving at most eight probes
  and the prior maximum occupancy ratio. Known draws and Map/Unmap do not
  scan the stream table or query descriptors. A failed capacity scan happens
  at most once per frame. There is no per-frame table sweep.
- Frame/admission bookkeeping is render-thread-owned. Advancing it at Present
  takes no cache lock; loader-thread creation notifications do not access it.
  Existing locks still protect shared candidates and buffer ownership.
- Preserve nonblocking staging reads and existing reference ownership.
  No native Draw-slot refresh or grass diagnostic tracing is added.

## Validation and limits

The release build, all three off-game self-test reports, renderer-site audit
against the uploaded DLLs, and complete Engine/Game site audit pass.

Regression cases use captured grass-card bytes and COM lifetime fixtures:
400 filled streams keep every crossing across reordered frames without new
descriptor queries; a 700-stream population remains bounded at 512; repeated
overflow admissions stop querying descriptors; early next-frame overflow
cannot evict the previous scene; cold entries can be replaced after the
protection interval; an in-flight seed survives overload and completes with
`DO_NOT_WAIT`; completed cold streams release both source and twin; refill,
address reuse and shutdown preserve the crash fix's ownership guarantees.

The next game run must confirm dense-scene coverage and performance. Rendering
previously missing companions adds intended GPU work, so lower cache churn
does not establish identical total GPU time. Scenes with more than 512
simultaneously active streams still have partial enhancement, deliberately
bounded rather than allowed to churn indefinitely.
