# Zcode prompt — MacAudio R1: shm driver reconnection (epoch/reopen protocol)

Repo: `/Users/cortexuvula/Development/MacAudio` (macOS Swift menu-bar audio app + C CoreAudio HAL driver + POSIX-shm SPSC ring buffer). Branch `main` @ `469ec4c`, working tree clean. This is the **R1 follow-up** from issue #3 (R2/R3 shipped in `469ec4c`). It is the genuinely hard, coordinated piece — do NOT half-implement it; if a sub-part cannot be done safely, gate it explicitly with a named follow-up and its acceptance criterion.

> **Already reviewed to closure** by repo-auditor (HARDENING verdict) and codie (APPROVE on the R2/R3 diff) in the issue thread. Their R1 framing is folded in verbatim below. Suggested owner: Ada or Turing to implement, Codie to review.

---

## 1. Hard facts (verified file:line — reuse, do not reinvent or guess)

- **Two-process design:** Swift app writes interleaved Float32 stereo into POSIX shm `/macaudio_ringbuffer`; a CoreAudio HAL driver reads it and presents a virtual input device.
- **Ring buffer struct** — `MacAudioDriver/SharedRingBuffer.c:13-24`: `writeHeadFrames`, `readHeadFrames`, `sampleRate`, `active`, `writerOwnerPid` (offset 24, carved from former padding), `buffer` at static-asserted offset 64. `kSHMSize` unchanged.
- **Writer ownership (shipped in 469ec4c)** — `SharedRingBuffer.c:34-108`: `CreateOrOpen(1)` acquires ownership (`TryAcquireWriterOwnership`) BEFORE resetting shared state; refused opens munmap+return NULL without touching the segment; same-process re-open + dead-pid takeover (kill/ESRCH, CAS-guarded) allowed; pid-reuse fails closed. `SharedRingBuffer.c:131-227`: `TryAcquire`/`Release`/`HasLiveWriterOwner`/`DestroyIfUnowned` (owner-only unlink). Header decls `SharedRingBuffer.h:34-41`.
- **Driver StartIO** — `MacAudioDriver/MacAudioDriver.c:1087-1127`: opens the ring buffer ONLY when `clientCount == 0` (`SharedRingBuffer_CreateOrOpen(0)` at :1111); on failure logs `"StartIO: failed to open ring buffer"` and continues silent (:1113-1116). Reaps `gRingBuffer_PendingClose` first (:1101-1104).
- **Driver StopIO** — `MacAudioDriver.c:1129-1162`: detaches via `atomic_exchange_explicit(&gRingBuffer, NULL, ...)` (:1145) and defers the munmap to the next StartIO via `gRingBuffer_PendingClose` (:1146-1154) to avoid SIGBUS in a racing DoIOOperation.
- **Driver DoIOOperation** — `MacAudioDriver.c:1213-1253`: loads `gRingBuffer` atomically (:1224); if `!rb || !GetActive(rb)` zero-fills and returns (:1226-1229); else `SharedRingBuffer_Read` (:1232) + underrun zero-fill (:1235-1238) + volume/mute (:1240-1250). This is the realtime callback — no allocation, no blocking, no syscalls.
- **The R1 bug:** when the app Quits→Relaunches (or otherwise unlinks+recreates the segment), the driver's `gRingBuffer` still points at the OLD (now-unlinked) inode. The driver has NO mechanism to detect the segment was replaced and reopen it — StartIO only opens when `clientCount == 0`, and a continuously-connected recording client keeps the client count > 0. Result: the driver outputs silence while the app reports capture.
- **Existing C test** — `tests/test_ring_buffer.c:683-726` `test_reader_receives_new_data_across_writer_close_reopen` covers the *no-unlink* Stop→Start preservation contract (D2) only — deliberately NOT an R1 claim (a reader kept mapped across a writer close/reopen on the SAME inode). R1 concerns a REPLACED segment after an unlink.

## 2. Design decisions (each resolved; do not reverse)

- **D1 — Epoch/generation counter in the shm header.** Add an `_Atomic uint32_t epoch` (or `generation`) field to the ring-buffer struct, carved from the remaining header padding (bytes 28-63; buffer stays at static-asserted offset 64, `kSHMSize` unchanged — verify the padding arithmetic). Bumped (incremented) on every writer-ownership acquisition in `CreateOrOpen(1)` (i.e. whenever a writer takes ownership, including takeover from a dead owner). The driver reads it to detect a replaced segment.
- **D2 — Driver-side epoch check with reopen OUTSIDE the realtime callback.** The driver must NOT reopen inside `DoIOOperation` (realtime, no syscalls). Detect a stale epoch in the realtime path (cheap atomic compare against a cached value) and set a flag; perform the actual reopen (munmap old, `CreateOrOpen(0)`, remap) on a non-realtime path. The existing `gDevice_IOMutex` + `gRingBuffer_PendingClose` pattern is the retirement mechanism to reuse — do not invent a new one.
- **D3 — Reopen must be safe against a racing DoIOOperation.** Reuse the exact deferred-close discipline already proven in StopIO: detach `gRingBuffer` atomically, defer the munmap to a point where the HAL guarantees no in-flight IO cycle touches the old mapping. Do not munmap a mapping a realtime callback may have already loaded.
- **D4 — Do NOT weaken the SPSC contract or the writer-ownership handshake.** Reader owns `readHeadFrames`, writer owns `writeHeadFrames`; acquire/release; overrun-resync caps at one buffer. The driver is reader-only and must never touch ownership.
- **D5 — Scope discipline.** If the full reopen-on-epoch-change cannot be implemented safely (e.g. the HAL does not offer a clean non-realtime hook to reopen), gate it explicitly: ship the epoch counter + driver-side detection + flag, and name the reopen mechanism as a follow-up with its AC. Do NOT claim R1 solved by a detection-only change.

## 3. Non-negotiable constraints

- Do NOT change the SPSC ring-buffer atomics/contract, the writer-ownership handshake, or the driver's property vtable semantics.
- Do NOT allocate, block, or syscall inside `DoIOOperation` (realtime). The epoch check there must be a single atomic load + compare.
- Do NOT touch packaging/, .github/, entitlements, Info.plists, or project.yml target definitions beyond a file-list entry if a new test file needs it (then regenerate via `generate.sh` and restore entitlements + driver Info.plist CFBundleExecutable exactly as the repo docs prescribe, verifying those two files are unchanged in `git diff`).
- Match existing style. No unrelated reformatting. No `.mimosa/` or `.zcode/` in any commit.
- The driver runs inside `coreaudiod` — a crash there is a system-level failure. Prefer fail-safe (output silence) over a crash or a corrupt read.

## 4. Deliverable (numbered, each with an acceptance criterion)

1. **Epoch counter in the shm header.** Add `_Atomic uint32_t epoch` carved from padding (buffer offset stays 64, `kSHMSize` unchanged, static-asserted). Bump on every writer-ownership acquisition in `CreateOrOpen(1)`. **AC:** a new writer-open after a previous writer's release (or dead-owner takeover) increments the epoch; the C test suite asserts the value changes across ownership transitions and the layout static-assert still holds.
2. **Driver-side epoch detection.** The driver caches the epoch it last saw and, in `DoIOOperation`, compares the shm header's current epoch against the cache (single atomic load + compare — no allocation/blocking). On mismatch, set a flag (and keep outputting silence for that cycle). **AC:** a replaced segment (new epoch) is detected by the realtime path without any syscall or allocation.
3. **Reopen outside the realtime callback.** On the flagged epoch change, reopen the segment (munmap old via the `gRingBuffer_PendingClose` deferred-close discipline, `CreateOrOpen(0)`, remap, update the cached epoch) on a non-realtime path, safe against a racing `DoIOOperation`. **AC:** a recording client kept continuously connected across app Quit→Relaunch (segment unlink + recreate) and across a StartIO-client-remains-connected transition resumes receiving audio without the client stopping/restarting the device. If this cannot be done safely, gate it per D5 and name the reopen mechanism as a follow-up with this AC.
4. **Regression coverage.** Extend `tests/test_ring_buffer.c` (or add a focused test) for the epoch semantics: epoch increments on ownership acquisition; a reader that reopens after an unlink+recreate sees the new epoch and reads the new writer's data. **AC:** the new C tests pass and the existing 31/31 stay green.

## 5. Verification gates (run individually; do not chain into one opaque failure)

- `tests/run_tests.sh` — C ring-buffer 31/31 (existing) + new epoch tests must pass; Python 96 + 127 must stay green.
- `xcodebuild test -scheme MacAudio -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO CODE_SIGN_IDENTITY=""` — existing 24 + any new Swift tests pass.
- If the driver C changed: `cc -std=c11 -Wall -Wextra -Werror -I MacAudioDriver ...` compiles clean (the driver is not unit-testable outside coreaudiod; source-integrity regex guards in `tests/test_source_integrity.py` are the practical defense — keep them green).
- If reopen is gated (D5): state the follow-up AC explicitly and do NOT claim R1 solved.

## 6. Report

Report back: per-deliverable change summary (files changed, exact diff), which of the 4 deliverables you shipped vs gated (and the gated item's AC), test tails for each gate, and any deviation from the hard facts above (verify against source before accepting either side). Do NOT commit — leave the diff for review.
