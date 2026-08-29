# stage-6-ship

**Deliberately a stub.**

This plan cannot honestly be written yet — see `CLAUDE.md`. Write it at the end
of the preceding stage, once the facts it depends on exist. The gate is in
`RUNBOOK.md`.



## The report, outlined — 2026-08-29 (write and file next session)

To CodeWeavers (CrossOver Preview 27.0.0, DXMT `v0.80-131-g2befd18`, i386):

1. **Symptom:** per-object one-frame dropouts, 1–2%/object/frame, DX11/DXMT,
   Titan Quest AE (GOG and Steam), Apple M5 Pro, FEX. O9–O14 measurements.
2. **The game is exonerated:** O30 — draws issued, counts flat, zero busy
   Maps, zero empty draws. DX9 clean (O35); DXVK black geometry (O44).
3. **Localization by intervention** (the table from O41/O42/O43): GPU-blit
   uploads into a stable allocation, or GPU-idle syncs, remove it entirely;
   every CPU-mapped arrangement flickers; more allocations/chunks = worse;
   dose-response on sync frequency; barriers, alignment (verified 16KB), rings,
   latency, chunk boundaries, argument re-encode all eliminated.
4. **Suspected area:** the i386 `CpuPlaced`/`bytesNoCopy` dynamic-buffer
   upload path — the GPU intermittently reads CPU-written mapped memory
   stale. Their fork's pages are ≥16KB (upstream 4096) — they know this code.
5. **Supporting:** O2 (their own border-colour warning), DXVK's compiled-in
   `TQ.exe` profile, O46 perf table for the workaround's cost.
6. **Ask:** an upload-path fix; offer logs, the shim, and (given Xcode) a
   `.gputrace` of a flagged bad frame.

Ship checklist unchanged below (ZIP, clean bottle, overlay restored — note
the GOG bottle has the THQ overlay intact; the rename was in the OLD bottle).
