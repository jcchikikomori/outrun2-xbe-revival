# OutRun 2 Original Xbox Recompilation Implementation Plan

- **Plan date:** 2026-11-18
- **Primary development target:** Linux x64 headless validation plus a cross-built Windows executable
- **Primary playtest target:** Steam Deck through Wine, Bottles, or Proton
- **Release target:** Native Windows x64 when a Windows build host is available
- **Source media:** User-supplied Original Xbox DVD image for *OutRun 2*
- **Design baseline:** `/home/deck/Documents/OutRun2 Xbox Static/OutRun 2 Original Xbox Static Recomp Design Document.md`

## 1. Objective and Planning Assumptions

The objective is to produce a Windows-compatible x64 build of the 2004 Original Xbox version of *OutRun 2* by statically recompiling `default.xbe`, while keeping deterministic Linux validation available through the SDK's headless harness.

The immediate playable target is the current Linux Steam Deck environment. The preferred first playable build is a cross-compiled Windows executable run through Wine, Bottles, or Proton. Native Windows remains the eventual release target. A native Linux Vulkan runtime is a later alternative, not a prerequisite for the first playable build.

The target is a **PC port**, not software intended to run on Original Xbox hardware.

The plan assumes:

- The user owns or otherwise has lawful access to an Original Xbox *OutRun 2* disc image.
- The ISO, extracted game assets, generated lifted code, and game-specific keys remain local.
- Version 1.0 prioritizes faithful boot, menus, gameplay, audio, input, saves, and frame pacing.
- Modern presentation features and online-service emulation are deferred **until the original game is FULLY STABLE**.
- A clean-room reference implementation is allowed to use the public [XboxXBERecomp SDK](https://github.com/TGP482/XboxXBERecomp), but no original game code or assets are redistributed with the project.
- **A simple but similar experience from the other projects such as [BLACK-XBERecomp](https://github.com/TGP482/BLACK-XBERecomp). No compilation in background during gameplay.**

A successful result is a build that starts from the user's own disc, reaches the title screen, plays a complete race, saves progress, and remains stable across repeated sessions.

## 2. Scope Boundaries

### Version 1.0 includes

- A reproducible Linux x64 headless build for deterministic validation.
- A Windows-compatible x64 executable generated from the user's `default.xbe`.
- Recompile everything from the original game (including shaders), before booting up.
- To compile the shaders, we can dump the shader cache being generated, then we will use it for recompilation to minimize stutters in-game.
- Minimum graphics pipeline should be DirectX 9, since the game and the console runs on DirectX 8.
- A Steam Deck playtest path through Wine, Bottles, or Proton.
- Boot to the title screen and navigation through the menus.
- At least one complete race course, followed by coverage of all five OutRun courses.
- Faithful Xbox-era visuals, audio, controls, and original simulation cadence.
- Save-game persistence with recoverable backups.
- Controller input plus keyboard and mouse fallback.
- Two-player System Link over LAN or direct IP.
- Diagnostic tooling for symbols, rendering, input replay, frame timing, and crashes.
- A local disc verifier that rejects unsupported or damaged media.

### Version 1.0 excludes

- Xbox Live 1.0 matchmaking or any replacement online service.
- Original Xbox hardware output.
- Ultrawide rendering, HDR10, arbitrary high frame rates, and custom music.
- A native Linux Vulkan, macOS, Android, or console backend.
- A polished end-user launcher.
- Steering-wheel force feedback unless the Phase 0 audit proves it is required for the original game to function.

These items remain valid post-1.0 candidates, but they must not delay the core port.

## 3. Corrections to the Initial Design Document

The upstream toolchain and command sequence in the design document are broadly sound. The following details should be corrected before implementation:

| Design-document statement | Planning decision |
| --- | --- |
| `pgr2-recomp` uses canonical `TGP482/XboxXBERecomp` directly. | It uses the `hk21x/XboxXBERecomp` fork, which contains useful harness, Ghidra, and input work. Compare both before choosing the pinned SDK. |
| Wheel force feedback is an entirely unsolved runtime feature. | `pgr2-recomp` demonstrates a reusable DirectInput force-feedback layer. OutRun 2-specific XID behavior still requires analysis. |
| XboxXBERecomp cannot support NV2A push-buffer commands. | Canonical upstream has basic push-command and `RunPushBuffer` handling; the `hk21x` fork adds ordered push-buffer recording and fixups. This XBE actively calls `BeginPush` and `EndPush`, so the exact command patterns must be verified against the selected SDK. |
| The selected SDK does not translate `fs:` segment accesses. | Both canonical upstream and the `hk21x` fork implement an `fs_base` guest segment, lift `fs:` accesses through it, initialize it from the thread's KPCR, and handle SEH frames. Runtime verification is still required. |
| Recompiled code is generated only at developer build time. | Upstream also supports a release model that builds the user's game code locally on first start. Choose either model deliberately during packaging. |
| `bustinout-recomp` is a verified keyboard/mouse reference. | No such repository was verified. Use `fuzionfrenzy-recomp` or `urbz-recomp` as the keyboard/mouse references instead. |
| The `takedawn` Vulkan/Metal claims, exact verifier timing, and compilation timings are established. | Treat these as unverified until they are measured or supported by primary documentation. |
| Emulator issue reports are direct evidence of current runtime defects. | Treat them as investigation leads only. Reproduce each failure in the new runtime before designing a fix. |

The `xbe_symbols`, `xbe2pe.py`, `add_xbe_image`, `X86_FUNC`, `gpu::Extension`, `app::kGame`, and `XLaunchNewImage` interfaces were confirmed in current upstream documentation.

## 4. Architecture Decision

### Recommended baseline

Use the public [XboxXBERecomp SDK](https://github.com/TGP482/XboxXBERecomp) as the architectural base and the [PGR2 recomp project](https://github.com/hk21x/pgr2-recomp) as the closest implementation reference.

Phase 0 compared these revisions:

1. Canonical `TGP482/XboxXBERecomp` at `cc9edda3a821a4204dc28783f429b88a27ade4f9`.
2. The `hk21x/XboxXBERecomp` fork used by `pgr2-recomp` at `b908ae7f05908ff335f94037c673e4b437c7c1d6`.
3. The `pgr2-recomp` game-side configuration and scripts.

The **selected baseline is the `hk21x` fork**, pinned at `b908ae7f05908ff335f94037c673e4b437c7c1d6`. It is newer, retains the upstream `fs:` and SEH translation, adds ordered push-buffer recording and fixups, and includes the Linux headless harness and Ghidra scripts needed for this title's evidence path. Canonical upstream remains the compatibility fallback and the source for any runtime fix that must be proposed upstream.

The Ghidra audit confirmed that the fork recognizes every raw NV2A command method found in this XBE. The selection is therefore no longer provisional.

### Runtime boundaries

- The guest Xbox executable remains the source of game behavior.
- Xbox kernel, XDK, Direct3D 8, DirectSound, XInput, and XNet calls are supplied by the host runtime.
- Game-specific fixes belong in `config/outrun2.toml`, `src/game/`, or narrowly scoped runtime changes.
- Generated lifted code is never edited or committed.

### Deployment lanes

1. **Linux headless harness:** The primary development and regression lane. It runs the guest code, host runtime, scripted input, CPU reference renderer, frame logs, and network tests without a window or GPU.
2. **Windows-compatible executable:** A cross-built executable run through Wine, Bottles, or Proton. This is the first playable Steam Deck lane and preserves the upstream Direct3D 12, XAudio2, and DirectInput implementation.
3. **Native Windows x64:** The eventual release build when a Windows build host becomes available.
4. **Native Linux Vulkan:** Deferred. It would require a separate renderer backend and is not on the critical path.

The implementation follows the existing `hk21x` harness, launcher, diagnostic, and recompiler patterns rather than inventing a second runtime.

### Windows cross-build route

The preferred cross-build proof uses the upstream Clang-based toolchain rather than MSVC:

- Clang 19 or newer targeting `x86_64-pc-windows-msvc`.
- An `xwin`-splat Windows SDK and UCRT sysroot.
- LLD for the final Windows link.
- The developer model (`XBE_GAME_DLL` disabled), so the executable links the generated game code directly and does not stage Windows `clang.exe` or `lld-link.exe` at runtime.
- A small game-side CMake toolchain file, rather than a broad fork of the SDK runtime.

The first playable test should use Proton's vkd3d path for Direct3D 12. If Wine's `d3dcompiler_47` cannot compile a runtime shader, install the native Microsoft DLL or switch to the SDK's existing Direct3D 11 backend under DXVK. The release model's first-start compiler is not part of the Linux cross-build.

## 5. Repository Shape

```text
outrun-2-xbox-recomp/
├── CMakeLists.txt
├── build.bat
├── .github/workflows/ci.yml     # Validation, code-free tools build, self-hosted game build
├── sdk/                       # Pinned SDK submodule
├── assets/                    # Local only: ISO, XBE, extracted media
├── generated/                 # Local only: generated lifted code
├── .local/                    # Local only: analysis exports and scratch output
├── config/
│   ├── game.toml              # Generated by xbe_symbols
│   ├── outrun2.toml           # Hand-authored analysis and hook configuration
│   └── default.xbe.sha1       # Pinned SHA-1 of the user's XBE, checked by CI
├── scripts/
│   ├── inventory-disc.*       # Image and XBE identity checks
│   ├── audit-xbe.*            # Headless static-analysis helpers
│   ├── replay-input.*         # Deterministic controller sequences
│   ├── verify-assets.*        # Local disc manifest verification
│   └── ci/                    # CI checks and the self-hosted runner setup
├── src/
│   ├── common/                # Application, input, settings, and runtime glue
│   └── game/                  # Title-specific function overrides
└── docs/
    ├── evidence/              # Redacted findings and test evidence
    ├── ci.md                  # Runner setup, variables, artifact and publishing policy
    └── verification.md        # Verification matrix and exit criteria
```

The repository must enforce ignores for `assets/`, `generated/`, `.local/`, crash dumps, logs, and user data. A tracked-file audit is part of every release gate.

## 6. Phased Delivery Plan

### Phase 0: Media Intake and Feasibility Spike

**Goal:** Establish that the target disc and existing runtime can support a viable PC port before building a full game project.

#### Completed baseline

- The supplied European ISO was inventoried without modification. Its identity and XDVDFS tree are recorded in `docs/evidence/phase0/media-inventory.md`.
- `default.xbe` was exported and identified as the signed European `SE-054` title `53450036`.
- `xbe_symbols` matched XDK `5849`, reported **64 partitions**, and found 186 of 278 known functions plus 12 of 13 known data objects.
- `update.xbe` and `dashupdate.xbe` were classified as the Online Updater Application and Xbox Dashboard Updater. They are not gameplay images unless an `XLaunchNewImage` reference proves otherwise.
- A flat PE was exported for Ghidra and its SHA-256 was recorded.
- `xbe_recomp` accepted the complete XBE and generated all 64 partitions: 12,691 functions, 721,662 instructions, 359 jump tables, and 96 MiB of private C++ output.
- The generated C++ compiled into the Linux headless harness. A sixty-second run booted the real disc, initialized the Xbox kernel, created a 640×480 device, and reached frame 2,340 with 14,168 draws. `XBE_PERF` confirmed a 60-frame-per-second startup/menu interval from frames 240–720. Reference captures rendered the Sega AM2 and Sumo Digital startup screens without an instruction trap.
- Ghidra found 547 `FS:`-prefixed instructions in 200 containing function labels. Both SDK candidates implement the required `fs_base` translation and SEH support.
- Ghidra found seven direct `BeginPush`/`EndPush` call sites containing nine push sequences. Those sequences use only NV2A methods `0x17FC` and `0x1818`, both recognized by the selected SDK. No `CreatePushBuffer` or `RunPushBuffer` reference was found.
- Both CVM containers were inventoried. The 15 `.SFD` files are MPEG-1 Program Streams; no Bink or `.xmv` media was found.
- The 92 unmatched functions were classified against the XBE's `.XTLID` table. No symbol with an export ID present in the XBE remains unresolved.
- The input path uses four standard XInput ports and standard rumble state. No wheel-specific XID packet path was found, so native force feedback is not a Version 1.0 blocker.
- Both SDK candidates were inspected. The `hk21x` fork additionally provides ordered push-buffer recording, fixups, and the required headless diagnostics.
- The Linux-to-Windows developer build succeeded with Clang 20, LLD, and an `xwin` Windows SDK sysroot. GE-Proton 11 and vkd3d-proton rendered the Sega startup screen, profile creation, and a 3D tunnel scene on the Steam Deck.
- The first playable run exposed a null default FVF shader object. Ghidra traced the guest initialization to `0x1A67C8`; the title-specific `D3D_FvfShader` data override removes the former profile-scene access violation. Independent fixed-build runs lasted 203 and 186 seconds without a crash.
- A GitHub Actions pipeline validates every change and builds the code-free host tools on GitHub-hosted runners, while the game executable is built on a dedicated self-hosted runner that holds the user's `default.xbe`. The compiled executable is published only behind an explicit gate, because artifacts of a public repository are public.
- OUTRUN MODE no longer traps. The divide error at guest `0x00195D6B` came from a zero cached vertex layout in the `CDevice`: the SDK's HLE for `D3DDevice_SetVertexShader` and `D3DDevice_SetStreamSource` replaces the guest bodies that filled it. A hook at `0x00195D09` caches the draw's own stride, and the headless run reaches the race start (`docs/evidence/phase0/outrun-mode-trap.md`).
- The SDK fork merged the Direct3D 9 renderer fork (`bechapm/XboxXBERecomp-D3D9`) behind the new `renderer` settings key, and restored the headless harness and the `hk21x` helper scripts the project tooling expects. After the runtime's Linux-compat bring-up, the headless boot of the extracted disc data reaches frame 720 at the measured 60 Hz cadence with 1431 draws and no traps. The playable lane now has two renderer options: `d3d12` (vkd3d-proton) and `d3d9` (DXVK), with D3D9 as the plan's minimum pipeline.

#### Remaining tasks

1. Carry the `fs:`/SEH and push-command audits into the Phase 1 runtime boot test.
2. Measure race cadence and investigate the non-reproduced divide-error trap if it recurs.

The working symbol and flat-PE commands are:

```bash
xbe_symbols assets/default.xbe sdk/xdk --out config/game.toml
python3 sdk/tools/symbols/xbe2pe.py config/game.toml .local/game.exe
```

#### Exit gate

Phase 0 passes only if all of the following are true:

- The ISO and XBE identities are recorded and repeatable.
- XDK `5849` signatures match, and the generated configuration uses 64 partitions.
- The `fs:` implementation is confirmed statically and scheduled for runtime verification.
- Every raw push command emitted by the nine identified sequences is covered by the selected SDK.
- The video inventory is complete, and the input-device and unmatched-symbol audits have explicit outcomes.
- The generated C++ compiles and links into the Linux headless harness without unresolved instruction failures.
- A Linux-to-Windows cross-build route is selected and proven with a minimal executable that starts under the available Wine or Proton environment.
- No copyrighted media, generated game code, or keys are tracked by Git.

#### Stop conditions

Stop and revise the architecture if a runtime test finds an unsupported push command or proves that `fs:`/SEH translation is incomplete.

### Phase 1: Reproducible Skeleton and Title/Menu Boot

**Goal:** Produce the first Linux headless boot and the first Windows-compatible executable that reach the title screen and menus.

#### Tasks

1. Create the project from the selected SDK's template.
2. Pin `sdk/` as a submodule and configure `add_xbe_image` for *OutRun 2* with 64 partitions.
3. Populate `app::kGame` with title, window, logging, settings, and lifecycle fields.
4. Create `config/outrun2.toml` for title-specific overrides without hand-editing generated code.
5. Resolve boot-time trapping stubs in small batches, recording each resolved address and evidence.
6. Build the Linux headless application in a clean checkout with the documented command sequence.
7. Cross-compile the Windows-compatible executable and run it through Wine, Bottles, or Proton.
8. Add deterministic input replay for title and menu navigation.
9. Capture a crash dump and symbol map for every unresolved failure.
10. Establish a local manifest of required disc files and their hashes.
11. Measure the game's update and presentation cadence from the running build and record whether the simulation is 60 Hz, 30 Hz, or variable.
12. Exercise an SEH or C++ exception path that reaches the guest's `fs:[0]` chain.

#### Exit gate

- A clean source checkout builds the Linux headless application on the current Steam Deck host.
- A clean source checkout cross-builds a Windows-compatible executable and starts it through the selected Wine, Bottles, or Proton path.
- The executable reaches the title screen without manual debugger intervention.
- Every menu can be entered and exited with scripted input.
- The title screen and each menu have repeatable PNG evidence.
- Every remaining trapping stub has an owner and a disposition.
- The measured simulation cadence is documented and stable before race work begins.
- The guest `fs:[0]` SEH chain behaves correctly in at least one exercised failure path.

### Phase 2: Race Rendering and Simulation Parity

**Goal:** Complete a race on the original, measured simulation cadence without visual or timing defects.

#### Tasks

1. Capture one course at a fixed baseline resolution with no modern enhancements enabled.
2. Use the headless reference renderer to validate draw lists, road surfaces, car geometry, lighting, palettes, textures, and HUD placement.
3. Reproduce and fix menu texture swizzling and car-selector behavior if present.
4. Confirm bound-resource lifetime by running through repeated course transitions.
5. Verify ordered push-buffer handling in the selected SDK and extend it only if the Phase 0 audit found an unsupported command pattern.
6. Separate simulation cadence from render presentation and enforce the cadence measured in Phase 1.
7. Measure frame timing with `XBE_PERF`; do not infer frame rate from visual appearance alone.
8. Complete one course end to end in the headless harness, then in the Windows-compatible executable under Wine, Bottles, or Proton.
9. Repeat the course across all five courses.

#### Exit gate

- One full course completes in the headless harness and the playable Windows-compatible build.
- The playable build has no missing geometry, corrupted textures, or mid-race crashes.
- The simulation advances at the cadence established in Phase 1.
- HUD and menus remain visually coherent at the baseline resolution.
- Repeated course starts and transitions do not expose resource-lifetime faults.
- Failures seen in emulator reports are either reproduced and fixed or explicitly shown not to apply.

### Phase 3: Modes, Audio, Input, and Saves

**Goal:** Make the ordinary single-player experience complete and stable.

#### Tasks

1. Validate all available game modes and progression unlocks.
2. Verify music, effects, and voice clips, including transitions where the original may truncate speech.
3. Reproduce the reported pause-menu restart problem and fix it at the responsible synchronization boundary.
4. Implement keyboard and mouse fallback using a virtual controller port.
5. Validate hot-plug, remapping, and rumble for supported SDL3 controllers.
6. Verify that standard controller axes, buttons, and rumble work through the Wine or Proton path.
7. Treat host-side wheel force feedback as an optional post-1.0 enhancement.
8. Validate save creation, reload, corruption recovery, and backup rotation.
9. Run a soak test covering menu use, race completion, pause/restart, and multiple reloads.

#### Exit gate

- All single-player modes are traversable.
- Voice and music clips complete at menu-to-course transitions.
- At least 100 scripted pause/restart cycles complete without a hang.
- Save data survives a normal restart and a forced process termination.
- A one-hour automated soak completes without a crash, unbounded memory growth, or stuck state.

### Phase 4: System Link and Steam Deck Packaging

**Goal:** Deliver a reproducible Linux build and Windows-compatible package for Steam Deck play, and verify the supported multiplayer path.

#### Tasks

1. Verify two local instances can establish a System Link session.
2. Expand to the supported player count after the two-instance path is stable.
3. Test direct-IP joining separately from LAN discovery.
4. Add an asset verifier that checks the supported disc against the local manifest.
5. Decide between a developer source build and a clean-room launcher that compiles the user's XBE locally.
6. Package the runtime, diagnostics, legal notices, and source-build instructions.
7. Run a clean-machine reproduction test using only documented prerequisites.
8. Audit all tracked files and release artifacts for game assets and generated code.

#### Exit gate

- Two-player System Link completes a race without desynchronization or crash.
- The supported disc passes the asset verifier.
- A clean Linux machine can build and launch the game using documented steps.
- The packaged Windows-compatible executable starts under the selected Steam Deck compatibility layer.
- No tracked or packaged file contains user media or generated lifted code.
- The release notes accurately state supported regions, revisions, and known limitations.

### Phase 5: Optional Enhancements

Only begin this phase after Version 1.0 is stable.

Evaluate each feature independently:

- Native ultrawide rendering with an undistorted HUD.
- MSAA, anisotropic filtering, and HDR10.
- Wheel force feedback beyond the original game's behavior.
- User-owned MP3 or FLAC soundtracks.
- Linux Vulkan runtime.
- Replacement online lobby service.

Each enhancement requires its own regression gate and must not alter game simulation unless separately verified.

### Phase 6: OutRun 2006: Coast 2 Coast (Xbox) as a second title (planned)

Follow-on title, decided 2026-11-20. The PC port (OutRun 2006: Coast 2 Coast, DirectX 9)
shares the source lineage (Xbox OutRun 2 -> arcade SDX -> PC port) and doubles as a static
analysis reference for the shared code, so most of this project's runtime work transfers.

Decisions:

- **Repository: a fork of this repo per title.** The game-specific surface is already
  isolated (`scripts/game.env`, `config/`, the `add_xbe_image` target, smoke scripts), so
  the fork keeps the SDK submodule pointing at the shared `recomp-the-duke` fork — every
  DSOUND/D3D9/kernel fix lands once and serves both titles.
- **Gate: user acquires the retail C2C Xbox disc first.** No scan, no CI, no code until a
  legally owned XBE exists (same media rule as section 1).
- **Start: after OutRun 2 Version 1.0 is playable** (the DSOUND lifetime-fix pattern now
  being proven is a prerequisite anyway).

Bootstrap checklist (for the fork, once the disc exists):

1. Extract the disc with xdvdfs; note the XDK of the XBE (OutRun 2 is 5849; C2C ships on
   a later XDK). `xbe_symbols` needs a signature database for *that* XDK — expect the
   same class of dropped/degraded signatures that cost this project the ~20 s
   C0000005 (see the 5849 SetPitch/SetEG lesson) and budget a scan-fix-verify loop.
2. New `game.env` + `config/game.toml` scan against the user-owned XBE; new
   `add_xbe_image` target; new smoke/pad scripts for its menus.
3. Audit new runtime surface: XONLINE/XNET (C2C has live features — for offline play,
   follow the OutRun 2 pattern of HLE-covering the online path), save/YSI differences,
   audio library deltas vs 5849.
4. CI: fork inherits the workflows; set its own `OUTRUN2_XBE_PATH`-style repository
   variable pointing at that disc's XBE; the same self-hosted runner serves both repos
   (one registration, one work dir — AGENTS.md section 8).
5. Reuse as-is: extract-once ISO flow, D3D9 backend and its settings, cross-build/
   release model, harness lanes, the PC `OR2006C2C.exe` as RE reference for shared code.

## 7. Phase 0 Static Feasibility Audit

The audit uses the flat PE and a Ghidra project containing the `Decomp`, `FindOperand`, and `Callers` scripts used by the `pgr2-recomp` reference project.

| Question | Evidence required | Current result | Remaining gate |
| --- | --- | --- | --- |
| Does the game use raw NV2A push commands? | Import/caller reports plus reviewed decompilation of every hit. | Nine sequences across seven call sites use only methods `0x17FC` and `0x1818`; both SDKs recognize them. | Verify rendered output and state ordering in Phase 2. |
| Does the game use `fs:` segment accesses for TLS, the TIB, or SEH? | Operand search, caller review, and runtime segment-handling inspection. | Ghidra found 547 `FS:` instructions in 200 containing labels. Both SDKs translate them through `fs_base`. | Exercise a guest SEH path in the Phase 1 runtime. |
| Does the disc contain secondary XBE images? | XDVDFS tree plus `XLaunchNewImage` references. | `update.xbe` is the Online Updater Application; `dashupdate.xbe` is the Dashboard Updater. | Confirm no gameplay path launches them. |
| Which video formats are present? | Media inventory and decoder calls in the XBE. | The 15 `.SFD` files are MPEG-1 Program Streams; the `WMADEC` section is part of the guest XBE. | Verify movie playback in Phase 1 or 2. |
| Does the input layer send wheel-specific XID packets? | XInput and USB device-call paths plus packet construction review. | Four standard XInput ports and standard rumble state were found; no wheel-specific path was found. | Treat native force feedback as optional, not a Version 1.0 gate. |
| Which XDK build produced the XBE? | `xbe_symbols` output and XBE library versions. | XDK `5849` matched. | Review unmatched entries for actual calls. |
| What is the actual partition count and code layout? | `xbe_symbols` output and XBE section table. | 64 partitions and 13 code sections were identified. | Use 64 in `add_xbe_image` and validate the first build. |

The `hk21x/XboxXBERecomp` SDK fork includes a `scripts/ghidra/PushBufferScan.java` helper. Reuse it only after checking that its addresses and assumptions match the *OutRun 2* XBE.

## 8. Verification Matrix

| Claim | Minimum evidence | Acceptance condition | Owner |
| --- | --- | --- | --- |
| The selected disc is identified correctly. | Outer image digest, XBE digests, metadata, and file tree. | All values are recorded and repeatable. | Implementer |
| The XBE can be symbol-matched. | `xbe_symbols` report and generated configuration. | XDK 5849 matches; every unmatched entry is proven unused or reviewed. | Implementer |
| The native toolchain is reproducible. | Clean-checkout build log and version manifest. | Build succeeds using only documented prerequisites. | Implementer |
| Push-buffer and segment assumptions are valid. | Static-audit report with reviewed call sites. | Every use is supported or has an approved implementation. | Implementer and reviewer |
| The game boots correctly. | Automated input sequence and PNG captures. | Title and all menus are reached without intervention. | Implementer |
| Rendering is faithful enough to play. | Fixed-resolution frame captures and course completion. | No missing road, corrupted texture, or rendering crash. | Implementer and reviewer |
| Simulation timing is preserved. | `XBE_PERF` or equivalent time series. | The cadence measured in Phase 1 is met across repeated runs. | Implementer |
| Restart and save flows are stable. | Repetition script and save comparisons. | No hang; saves reload and recover as designed. | Implementer |
| Multiplayer is stable. | Two-instance and multi-instance session logs. | Supported sessions complete without desynchronization. | Implementer |
| Copyright separation is maintained. | `git ls-files`, ignore audit, and package scan. | No media, keys, or generated game code is tracked or shipped. | Reviewer |

### Verification budget

- Run the complete automated suite only at phase exits.
- Run focused tests after each hook, runtime, or configuration change.
- Reuse captures and logs while the corresponding code, media, and configuration are unchanged.
- Repeat expensive graphics and soak tests after changes that affect timing, resource lifetime, threads, or the renderer.
- Treat visual comparisons as supporting evidence; gameplay completion is the primary gate.

## 9. Primary Risks and Responses

| Risk | Probability | Impact | Response and trigger |
| --- | --- | --- | --- |
| Push command semantics are not fully honored at runtime. | Medium | Medium | Static analysis found only methods `0x17FC` and `0x1818`, both recognized. Verify state ordering and rendered output in Phase 2. |
| Incomplete `fs:`/SEH behavior despite static translation support. | Medium | High | Both SDKs implement `fs_base`; require a runtime exception-path test in Phase 1. |
| Startup or menu transition stalls in the headless runtime. | Medium | High | The first 240 frames render, but later reference capture and scripted navigation need diagnosis. Keep frame logs and draw captures for each transition. |
| Missing kernel import is reached. | Low | Low | `XcPKGetKeyLen` is the only unresolved import; Ghidra found no reference to its import slot. |
| An unmatched XDK function or data object is actually required. | Low | Medium | 43 unmatched IDs are absent from the XBE and 49 are optional variants. The missing FVF data object was found through a runtime crash and fixed explicitly. |
| Linux-to-Windows cross-build or Wine compatibility blocks playtesting. | Low | Medium | The Clang 20, LLD, and `xwin` build is validated; GE-Proton 11 and vkd3d-proton render the game on this Deck. |
| CI publishes recompiled game code on a public repository. | Medium | Medium | The publish gate is closed by default, the self-hosted runner is dedicated to this repository, and fork pull requests never reach the build job. |
| Rendering-state or resource-lifetime mismatch. | High | Medium | Fixed-resolution captures, deferred release, and repeated transition tests. |
| Frame-rate change alters physics. | Medium | High | Measure the original cadence in Phase 1, preserve it in Version 1.0, and defer higher rates. |
| Threading or restart deadlock. | Medium | High | Reproduce early and test at least 100 restart cycles. |
| Upstream or fork drift. | High | Medium | Pin exact commits and keep game-specific patches outside generated code. |
| Video codec gap. | Medium | Medium | The 15 `.SFD` files are MPEG-1 Program Streams and `WMADEC` is linked into the guest. Verify playback in the runtime. |
| Sensitive assets enter source control. | Low | High | Ignore rules, local-only storage, and tracked-file audits. |
| Wheel behavior expands scope. | Low | Low | No native wheel path was found. Ship standard controller support first and treat host FFB as optional. |

## 10. Tooling and Host Setup

### Linux and Steam Deck development host

The current workstation is the active development and playtest host. It has Git, CMake, Clang, LLD, Python, Java, Cargo, `xdvdfs`, Ghidra 12.1.4, Bottles integration, and several Proton installations. RADV exposes Vulkan 1.4.350 on the current AMD GPU. The host lacks native development headers and Ninja, so compiler-dependent Linux tools are built in ephemeral Ubuntu containers with Docker/Podman instead of changing the host installation.

The host uses three build or validation paths:

- The SDK's Linux headless harness for deterministic guest-code and reference-render tests.
- A Linux-to-Windows cross-build for the Direct3D 12, XAudio2, and DirectInput executable.
- Wine, Bottles, or Proton for playable Steam Deck testing of that executable.

The Windows-compatible build must use an established `xwin` or LLVM-MinGW route rather than an unverified ad hoc compiler configuration. The cross-build proof is a Phase 0 and Phase 1 gate.

### Native Windows release host

A native Windows x64 release host is deferred because none is currently available. When one becomes available, it should use Windows 10 or 11, Visual Studio 2022 with the C++ Clang tools component, CMake, Ninja, LLD, Python, and Git. Native Windows remains the final distribution target, but it is not a prerequisite for Linux headless development or Steam Deck playtesting.

### Disc inspection example

On a system with `xdvdfs-cli` installed:

```bash
xdvdfs info "/path/to/outrun2.iso"
xdvdfs tree "/path/to/outrun2.iso"
xdvdfs checksum "/path/to/outrun2.iso"
xdvdfs copy-out "/path/to/outrun2.iso" "/default.xbe" "assets/default.xbe"
sha256sum "/path/to/outrun2.iso"
sha1sum "assets/default.xbe"
```

Use the exact internal path reported by `xdvdfs tree`. Do not treat the Xbox DVD as an ordinary ISO filesystem, and do not edit the source image.

## 11. How to Use the ISO

The running game never reads the disc image. Every lane consumes an **extracted asset tree** in the local, ignored `assets/` directory, next to `default.xbe`:

1. Keep the supplied *OutRun 2* Original Xbox disc image unchanged; it is the extraction input, not a runtime dependency.
2. Inspect it with XDVDFS-aware tools before extracting anything (`xdvdfs tree`, `xdvdfs checksum`).
3. Export `default.xbe` and the game's data directories into `assets/`:

   ```bash
   xdvdfs copy-out "assets/OutRun2.iso" "/media" "assets/"
   mkdir -p assets/media && mv assets/data*.cvm assets/media/
   ```

   `xdvdfs copy-out` flattens directories, so move the extracted files into their original layout (`D:\media\data1.cvm` becomes `assets/media/data1.cvm`).
4. Record hashes for the source image, the XBE, and the extracted files; a later asset verifier (Phase 4) rejects a tree that does not match the manifest.
5. The headless harness mounts `assets/` as `D:` directly and cannot open an ISO itself (the disc-mounting code lives in the Windows-only `runtime/app/game_files.cpp`), so validation always runs from the extracted tree.
6. The Windows-compatible build keeps the ISO as a convenience fallback for players: `game_files.cpp` prefers the extracted files and otherwise opens an ISO found in `assets/` through the runtime's own XDVDFS parser (`runtime/kernel/disc.cpp`). The disc-read pacing fix applies to that fallback path only.
7. Keep the ISO, extracted assets, generated code, saves, and logs out of Git.

Current extracted tree: `assets/default.xbe`, `assets/media/data1.cvm` (755 MB), `assets/media/data2.cvm` (320 MB).

The supplied image details are:

- **Image path:** `/run/media/deck/SN512/Roms/xbox/OutRun 2 (Europe) (En,Fr,De,Es,It).iso`
- **Outer SHA-256:** `3ebd704fd1eb49401d1ffe1a863582f9e902fdcc6524e87a375883c64afbfcc0`

The image remains on the removable volume and is not copied into the repository.

## 12. First Ten Working Days

1. [x] Obtain the ISO path and record its identity.
2. [x] Pin the selected SDK commit and prove the Linux headless and cross-build toolchains.
3. [x] Inventory and hash the ISO.
4. [x] Extract `default.xbe` and record its identity.
5. [x] Run `xbe_symbols` and confirm XDK `5849` with 64 partitions.
6. [x] Export the flat PE and open it in Ghidra.
7. [x] Complete the input-device and unmatched-symbol audits.
8. [x] Create the reproducible project skeleton, Linux headless build, and Windows cross-build.
9. [x] Resolve the first boot-time failures and establish crash/log evidence.
10. [ ] Reach the title screen and turn the menu traversal into a repeatable test.

## 13. Definition of Done for Version 1.0

Version 1.0 is complete when:

- A clean Linux checkout builds the headless harness and cross-builds the Windows-compatible executable using only documented prerequisites.
- The executable starts and is playable on the Steam Deck through the selected Wine, Bottles, or Proton path.
- The user's supported disc is identified and verified locally.
- The title screen, menus, all five courses, audio, saves, and supported input work.
- Race simulation preserves the cadence measured from the original release.
- Repeated restart, save, and course-transition tests pass.
- Two-player System Link passes.
- No game assets, keys, or generated game code are tracked or distributed.
- Known limitations and supported regional revisions are documented accurately.

## 14. Primary References

- [XboxXBERecomp SDK documentation](https://github.com/TGP482/XboxXBERecomp)
- [hk21x XboxXBERecomp fork](https://github.com/hk21x/XboxXBERecomp)
- [PGR2 recomp reference project](https://github.com/hk21x/pgr2-recomp)
- [Windows cross-build reference project](https://github.com/tijayfly/gt64_recomp)
- [CommonLibSSE cross-compilation reference](https://github.com/alandtse/CommonLibSSE-NG)
- [Native Linux XboxXBERecomp example](https://github.com/abilliontokens/fuzionfrenzy-recomp)
- [xdvdfs inspection and extraction tool](https://github.com/antangelo/xdvdfs)
- [XboxDev Ghidra XBE loader](https://github.com/XboxDev/ghidra-xbe)
- [Disc dumping guide](https://wiki.redump.info/Microsoft_Xbox_and_Xbox_360_Dumping_Guide)
- [Xbox game disc format reference](https://xboxdevwiki.net/Xbox_Game_Disc)
