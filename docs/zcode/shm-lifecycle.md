# Zcode prompt — MacAudio shm teardown ownership + driver reconnection lifecycle (issue #3, R1/R2/R3)

Repo: `/Users/cortexuvula/Development/MacAudio` (macOS Swift menu-bar audio app + C CoreAudio HAL driver + POSIX-shm SPSC ring buffer). Branch `main` @ `813762b`, working tree clean. This is a **design-level lifecycle** task, NOT a destroy-on-stop fix.

> **Already reviewed to closure** by repo-auditor (verdict: HARDENING — defer-ticket) and codie in the issue thread. Their decisions are folded in verbatim below; do not re-litigate them. Suggested owner per repo-auditor: Ada or Turing to implement, Codie to review.

---

## 1. Hard facts (verified file:line — reuse, do not reinvent or guess)

- **Two-process design:** Swift app writes interleaved Float32 stereo into POSIX shm `/macaudio_ringbuffer`; a CoreAudio HAL driver reads it and presents a virtual input device.
- **Segment deliberately preserved across Stop→Start** — `MacAudioDriver/SharedRingBuffer.c:28-34` documents do-NOT-unlink-on-writer-open so a continuously-connected recording client's driver mapping stays valid.
- **Open semantics** — `SharedRingBuffer.c:36-42`: writer-open `O_CREAT|O_RDWR`, reader-open `O_RDWR` (no O_CREAT); both `MAP_SHARED` `PROT_READ|PROT_WRITE` (:81-84). Writer-open reuses a sufficiently-large segment; recreates only when too small or fstat fails (:56-78). On writer-open it resets active/heads/sample-rate/contents (:91-102).
- **Close vs Destroy** — `SharedRingBuffer.c:108-112` close only unmaps; `:114-116` `SharedRingBuffer_Destroy()` = `shm_unlink(kSHM_Name)`. `SharedRingBufferWriter.swift:62-74` close = SetActive(0)+munmap; `:76-79` destroy = close + `SharedRingBuffer_Destroy()`; `:81-85` deinit only closes (never unlinks).
- **AppState lifecycle** — `MacAudio/Models/AppState.swift:405-409` `cleanupForTermination()` = `audioMixer?.stop()` + `audioMixer?.destroySharedMemory()` + `audioMixer = nil` (optional-chained). `:411-418` `stopAudio()` = `audioMixer?.stop()` + `audioMixer = nil` (NO destroy). `:138-142` double-failure branch retains the stopped mixer (comment) so termination cleanup still reaches shm. `:389` `startAudio` unconditionally builds a fresh `AudioMixer`.
- **AppDelegate wiring** — `MacAudio/MacAudioApp.swift:25-27` `applicationWillTerminate` → `cleanupForTermination()`; Quit menu uses `NSApplication.terminate` (:233-237).
- **Driver IO lifecycle** — `MacAudioDriver/MacAudioDriver.c:1087-1127` `StartIO` opens the ring buffer only when client count was 0 (`SharedRingBuffer_CreateOrOpen(0)` at :1111); `:1129-1162` `StopIO` detaches via `atomic_exchange_explicit(&gRingBuffer, NULL, ...)` (:1145) and defers the munmap to the next StartIO; `:1213-1253` `DoIOOperation` reads the ring buffer, zero-fills on underrun (:1231-1237).
- **Misleading test** — `tests/test_source_integrity.py:580-583` asserts `destroySharedMemory` appears in AppState while its message claims `stopAudio` must call it — contradicts intended Stop semantics. Replace with behavioral coverage.
- **Existing C test** — `tests/test_ring_buffer.c:497-528` checks inode preservation across writer opens, but does NOT keep a reader mapped and verify resumed delivery.

## 2. Design decisions (each resolved; do not reverse)

- **D1 — Do NOT add `destroySharedMemory()` to `stopAudio()`.** Unlink-on-Stop would strand a continuously-running driver (R1). This is non-negotiable.
- **D2 — Preserve no-unlink-on-Stop.** The segment must survive Stop→Start so a connected recording client's driver mapping stays valid.
- **D3 — R3 narrow fix (only if R1/R2 are handled):** remove the mixer-clearing `audioMixer = nil` at `AppState.swift:413`, retaining the stopped mixer until termination or replacement by a successful `startAudio()` at :389. Its writer mapping is already closed, so retention keeps nothing live. Matches the existing double-failure retention policy at :138-142. Do NOT rush this through as a complete lifecycle fix.
- **D4 — R1 driver reconnection is the hard part:** StartIO only opens when client count was 0; adding a client while one remains connected does not reopen. Recovery requires all clients to stop + a fresh StartIO, or an equivalent driver lifecycle reset. Reopening must happen OUTSIDE the realtime callback, with safe retirement of old mappings. If a full reconnect mechanism is out of scope for this pass, say so explicitly and gate it as a follow-up — do not half-implement it.
- **D5 — R2 exclusive writer ownership:** establish exclusive writer ownership before resetting shared state; only the owner performs teardown. A second instance must refuse ownership or do an explicit handoff without resetting/silencing/unlinking the first writer's segment.
- **D6 — Scope discipline:** this is a coordinated lifecycle piece. If R1's driver-reconnect mechanism is genuinely a separate design effort, split it: ship R3 + R2 (ownership) + the misleading-test fix now, and leave R1 as an explicitly-gated follow-up. Do not pretend R1 is solved by a one-liner.

## 3. Non-negotiable constraints

- Do NOT change the SPSC ring-buffer atomics/contract (reader owns `readHeadFrames`, writer owns `writeHeadFrames`; acquire/release; overrun-resync caps at one buffer). The C tests guard this.
- Do NOT touch packaging/, .github/, entitlements, Info.plists, or project.yml target definitions beyond a file-list entry if a new test file needs it (then regenerate via `generate.sh` and restore entitlements + driver Info.plist CFBundleExecutable exactly as the repo docs prescribe, verifying those two files are unchanged in `git diff`).
- Match existing style. No unrelated reformatting. No `.mimosa/` or `.zcode/` in any commit.
- No behavior change to the realtime IO path beyond what the lifecycle fix requires.

## 4. Deliverable (numbered, each with an acceptance criterion)

1. **R3 — Stop→Quit teardown gap (Low):** `stopAudio()` retains the stopped mixer (remove `audioMixer = nil` at AppState.swift:413) so `cleanupForTermination`'s destroy still reaches shm on quit-after-Stop. **AC:** user-toggle Stop calls `stop()` but never `destroy()`; Stop→termination reaches destruction; repeated termination cleanup does not repeat destruction.
2. **R2 — exclusive writer ownership (Medium, conditional):** a second concurrent app instance must not reset/silence/unlink the first writer's segment. **AC:** starting a second instance while the first streams either refuses ownership or performs an explicit handoff without corrupting the first writer's state.
3. **Misleading test fix:** replace `tests/test_source_integrity.py:580-583`'s misleading assertion (message claims stopAudio must call destroy) with behavioral coverage that matches intended Stop semantics. **AC:** the test no longer asserts stopAudio calls destroy; a mock test covers Stop-calls-stop-never-destroy and Stop→termination-reaches-destroy.
4. **R1 — driver reconnection (Medium):** EITHER implement a coordinated unlink/replacement + driver-reconnect mechanism (reopen outside the realtime callback, safe retirement of old mappings), OR explicitly gate it as a follow-up with a clear statement of what's needed. **AC:** if implemented, a recording client kept continuously connected across Stop→Start and Quit→Relaunch resumes audio; if gated, the follow-up is named with its acceptance criteria and R1 is NOT claimed as solved.

## 5. Verification gates (run individually; do not chain into one opaque failure)

- `tests/run_tests.sh` — C ring-buffer 26/26, Python 96 + 126 must stay green.
- `xcodebuild test -scheme MacAudio -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO CODE_SIGN_IDENTITY=""` — existing 20 + new tests pass.
- New behavioral tests for R3 (Stop-calls-stop-never-destroy; Stop→termination-reaches-destroy; repeated cleanup doesn't repeat destroy) and R2 (second-instance ownership refusal/handoff).
- If R1 implemented: a reader kept mapped across writer close/reopen receives newly written samples without reopening (extend `tests/test_ring_buffer.c` — the existing inode test at :497-528 does not cover this).

## 6. Report

Report back: per-deliverable change summary (files changed, exact diff), which of R1/R2/R3 you shipped vs gated (and the gated item's acceptance criteria), test tails for each gate, and any deviation from the hard facts above (verify against source before accepting either side). Do NOT commit — leave the diff for review.
