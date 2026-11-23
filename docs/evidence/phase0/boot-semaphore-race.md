# Boot semaphore publication race

## Symptom and observation

Run 8 of the headless boot harness froze at frame 180. The preserved log and F3 stack show two guest
threads on the same request object:

- requester: host `td403`, guest `0x14`, in `sub_000CBB70`
- worker: host `tcc64`, guest `0x2C`, in `sub_000CB530`

Relevant guest addresses and fields:

- `0x000CBB70` requester, request list `0x52A7B8`
- `0x000CB530` worker, work queue `0x52A7C8`
- `+0x1C` pending work count
- `+0x20` semaphore handle
- `0x000F0795` guest close wrapper (`NtClose`, kernel ordinal 187)
- `0x000F2D50` guest release wrapper (`NtReleaseSemaphore`, ordinal 222)
- `0x000F2E0C` guest wait wrapper (`NtWaitForSingleObjectEx`)

The bad interleaving was:

1. Requester read `pending = 1` at `0x000CBB96`.
2. Worker decremented `pending` to 0 at `0x000CB56F` / `0x000CB575`.
3. Worker read `[resource+0x20] == 0` at `0x000CB57E`, so it skipped the release at `0x000CB58A`.
4. Requester created a count-0, limit-1 semaphore at `0x000CBBA5` and published it at `0x000CBBAD`.
5. Requester waited at `0x000CBBB0` with no release ever queued.

The requester and worker guest ranges contain no `Interlocked`, critical-section, IRQL, or other lock
primitive around the pending check and publication. SDK semaphore and wait internals were traced and
are consistent; no object-wait correction is justified. The detached host guest threads make the
guest's check/create/publish window much more likely than on the console.

## Hook semantics

`config/outrun2.toml` installs an instruction hook after `0x000CBBAD`, immediately after the
requester stores the new handle in `[resource+0x20]`. `FixBootSemaphoreRace` in
`src/game/hooks.cpp` re-reads `[resource+0x1C]`:

- pending is still nonzero: return false; the original wait at `0x000CBBB0` runs unchanged.
- pending is now zero: return true; the generated `on_true` exit jumps to `0x000CBBB5`.

The jump target is the wait's normal successor, not a new cleanup path. The guest there reloads
`[resource+0x20]`, calls its own `F0795` close wrapper, clears the slot, then runs the callback at
`0x000CBBC5` and frees the resource through `CC510`. The hook therefore skips only the blocking wait
and leaves original cleanup and callback/free semantics intact.

The hook also checks that the slot still holds the handle produced by this invocation and guards the
resource against re-entry, so a duplicate or superseded publication cannot jump twice.

## Separate pacing change

The staged `runtime/kernel/files.cpp` change implements the SDK-documented `GameInfo::disc_rate`
for optical reads. It is not the race fix: it paces reads and its A/B comparison was inconclusive.
The standalone SDK copy and the game's `sdk` submodule carry identical source for that change.

## Class B: WX I/O completion starvation in upper-engine wait (separate blocking path)

Observed in `boot-diag10/attempt1` (frame 60, 20.6s): requester thread 14 (`t8d90`, guest 0x14)
published sem `0x0000109C` for resource `0x0107AF50` (item `0x0107B8C0`), saw `pending==1`, and kept
the guest wait. `FixBootSemaphoreRace` correctly did not skip the wait because pending was nonzero.
Worker thread 2C (`t0742`, guest 0x2C, started at `000CB940`) was stuck in `sub_000CB6E0`'s poll
loop at `0x000CB8E4/0x000CB8E5`, repeatedly calling `[edi+0x0C]` with `esi=resource` until return
matched `ebx==0`.

Callback mapping (from generated guest code/XBE static data):
- `sub_000CB6E0` loads callback object from `[resource+0x2C]` (`edi=[esi+0x2C]` at `0x000CB719`,
  saved at `esp+0x34/0x44`, reloaded at `0x000CB8AF`) and calls `[edi+0x0C](esi)` at `0x000CB8B4`
  and `0x000CB8E5`.
- Diag10 stall: `esi=0107AF50` (resource), `edi=002D4BFC` (callback object), `ecx=0107B8C0` (item).
- XBE `.data` object at `0x002D4BFC` is a static vtable: +0x00=`sub_000CB360`, +0x04=`sub_000CB3B0`,
  +0x08=null, +0x0C=`sub_000CB3F0` (poll), +0x10=`sub_000CB420`.
- `sub_000CB3F0(resource)` reads `handle=[resource+0]`, calls `sub_001362D0(handle)` which returns
  `movsx eax, byte ptr [handle+1]` (async I/O status byte; `sub_00136800` is identical).
- Poll contract: status `1` or `3` => return `0` (done, CB6E0 exits poll); status `4` => return
  `-1` (error; CB6E0 sets `0x52A814` flag but continues looping while return!=0); any other status
  => return `1` (pending, loop continues with delay via `sub_000F2F50([0x52A7FC])`).

Root cause (confirmed in generated guest code, read-only analysis):
- WX read queue `sub_0013C390` (WX vtable+0x20, `x86_recomp.35.cpp:18692`) defers reads: it stores
  buffer/offset/len at `obj+0x04/+0x48/+0x4C/+0x50/+0x28`, sets `byte[obj+0x01]=2` (busy),
  `dword[obj+0x54]=1` (submit requested), `dword[obj+0x58]=0`, releases lock, and returns. It does
  **not** issue ReadFile.
- WX pump/completion `sub_0013CE40` -> `sub_0013CD90` (`x86_recomp.35.cpp:20618/20481`) is the only
  path that submits via `sub_0013C960` (ReadFile wrapper `000F221A`), polls via `sub_0013C070`
  (GetOverlappedResult `000F2BE3`, wait=0), and on completion sets `byte[obj+0x01]=1`.
- The only static pump `sub_0013D050` is an init-time drain reached through `0004B120` (called once
  at `x86_recomp.7.cpp:17546`, `00055906`); it is not persistent.
- Upper-engine blocking wait `sub_00137AD0` (`x86_recomp.35.cpp:4264`) calls `sub_001374A0` once,
  then loops `00137940` until `byte[esi+1]==1 && [esi+0x20]==0`. It never pumps WX. A queued WX read
  can therefore remain pending forever while the waiter spins in `00137AD0`, starving the
  upper-engine handle and leaving `sub_000CB3F0` pending.
- SDK/kernel I/O semantics are not the indicated cause: `sdk/runtime/kernel/files.cpp` NtReadFile is
  synchronous and never returns `STATUS_PENDING 0x103`; `000F221A`/`000F2BE3` wrappers are consistent.
  Even with synchronous completion, the guest WX state machine still requires `0013CD90` to run.

Diag10 context near stall: SDK logged `open d:\media\data1.cvm`, `missing z:\Voice1.xwb`, then
`open z:\Voice1.xwb` before the stuck item `0107B8C0` was enqueued on resource `0107AF50`. The
missing/opened Voice1 path is consistent with WX/upper-engine I/O involvement but not by itself proof.

## Class B hook (implemented)

`config/outrun2.toml` adds void `[[hook]]`s calling `PumpWxWhileIoWait` (`after = false`) at:
- `0x00137AE0`: first instruction of `sub_00137AD0`'s wait loop (`call sub_00137940`), before the
  upper-engine poll.
- `0x000CB3F0`: entry of worker callback status poll (`sub_000CB3F0`, vtable +0x0C used by
  `sub_000CB6E0`), before reading `handle+1` status via `sub_001362D0`.

`PumpWxWhileIoWait` in `src/game/hooks.cpp` saves the full `X86Context`, calls the guest's existing
WX pump `sub_0013CE40(ctx, base)`, then restores the full `X86Context`. This preserves all
CPU-visible caller state (GPRs, flags, x87/SSE/MMX/runtime-owned fields as copied) so the original
wait/poll logic observes unchanged register state; only intentional guest-memory side effects from
the pump remain.

This is a narrow, title-specific scheduling adaptation. It does not change SDK/kernel I/O object
semantics, does not add a global scheduler fix, does not edit generated game code, and preserves the
separate staged `runtime/kernel/files.cpp` optical pacing patch.

## Validation

Containerized headless validation (`scripts/container.sh harness 150 scripts/pad/boot.txt`, Podman,
`localhost/outrun2-xbe-tools:24.04`, fresh runs, sequential, no overlap, same runner/setting).
Disposable `.local/run` cleaned after capture; evidence under `.local/validation/boot-classb-hook1/`.

| Attempt | Hooks | Result | Frames / timing | Class A log |
|---:|---|---|---|---|
| 1 | `00137AE0` only | **Fail/stall** | No renderer frame lines; log ended at 3.957s | `FixBootSemaphoreRace` correctly kept wait: `000CBB70 resource 0107AF50 handle 00001088 pending 1, keeping the guest wait` |
| 2 | `00137AE0` + `000CB3F0` | Pass | 6000 frames @148.126s | no `keeping the guest wait` / `skipped the wait` observed |
| 3 | `00137AE0` + `000CB3F0` | Pass | 6000 frames @147.978s | no `keeping the guest wait` / `skipped the wait` observed |
| 4 | `00137AE0` + `000CB3F0` | Pass | 6060 frames @148.088s | no `keeping the guest wait` / `skipped the wait` observed |
| 5 | `00137AE0` + `000CB3F0` | Pass | 6000 frames @148.003s | no `keeping the guest wait` / `skipped the wait` observed |
| 6 | `00137AE0` + `000CB3F0` | Pass | 6000 frames @147.964s | no `keeping the guest wait` / `skipped the wait` observed |

- Attempt 1 confirms the `00137AE0`-only hook did not cover the observed `sub_000CB6E0`/`sub_000CB3F0`
  worker-poll stall path; adding `000CB3F0` covers that path.
- Attempts 2-6 are five consecutive clean passes with both hooks (>=2100 frames, no stuck pending
  WX/Class A semaphore-wait symptom observed). Parent independently verified the 5 headless logs and
  reported no guest traps/OOM.
- `FixBootSemaphoreRace` (Class A) remains present in `config/outrun2.toml` + `src/game/hooks.cpp`.
  In passing attempts 2-6, the specific Class A wait-skip/keep log lines were not observed; this is
  not evidence the race cannot occur.
- Diag10 (pre-fix Class B reproduction) remains under `.local/validation/boot-diag10/attempt1/1/`.

**Limitation:** headless harness only, same runner/setting. **Proton pending** (parent will ask user).
**Do not claim full boot fixed until Proton passes.**
