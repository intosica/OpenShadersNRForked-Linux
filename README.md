# OpenShadersNRForked — DLSS Neural Rendering / Proton Stability Report

**Scope:** `feature/dlssnr-preupscale` branch of OpenShadersNRForked, running on Linux via Proton.
**Starting symptom:** occasional minor stuttering in the base mod became a **hard freeze** requiring a restart once the DLSS Neural Rendering (DLSS-NR) pre-upscale feature was added.
**Status at time of writing:** the hard freeze is fixed. A separate, less severe issue remains — DLSS-NR itself can still intermittently stop working during a play session. The cause of *that* specific issue has not been root-caused; what's been added is automatic recovery so it no longer requires a restart or stays broken for the rest of the session.

---

## 1. The freeze — fixed

### Cause
DLSS-NR needs to bridge two different graphics APIs every frame, because Skyrim renders in D3D11 but DLSS-NR requires D3D12. The bridge works by having D3D11 and D3D12 hand off to each other via a **shared GPU fence**. The original code used an *unbounded, GPU-side wait* for this handoff (`context11_->Wait(...)`, no timeout at all) — deliberately chosen to avoid a CPU stall each frame.

On native Windows, this kind of cross-API fence is backed by real kernel objects and is very reliable. Under Proton, D3D11 and D3D12 are each translated to Vulkan by a *different* compatibility layer (DXVK and VKD3D-Proton respectively), and the shared-fence handshake between them is emulated rather than native. That emulation is known to be one of the least mature corners of Proton. Occasionally the signal from one side doesn't reach the other, and because the wait had no timeout, the GPU queue would simply stall forever — a genuine hard freeze, not a CPU hang, which is why nothing (not even the desktop compositor) could recover from it.

### Fix
`D3D12Interop::EndD3D12()` was changed to use the existing bounded CPU-side wait (`WaitForFence()`, 250ms timeout) instead of the unbounded GPU-side wait. This reintroduces a small, bounded per-frame stall (comparable to the "occasional stutter" already considered tolerable in the base mod) as a trade for eliminating the unbounded freeze. If the cross-API fence does stall, the call now fails cleanly after 250ms instead of hanging the GPU indefinitely.

**Result, confirmed from logs during actual play:** the timeout condition does still occur under Proton (see Section 3), but it now fails gracefully instead of freezing the game. The hard-freeze symptom has not recurred.

---

## 2. Getting the project building at all

Once the freeze fix was written, actually getting a working build turned into its own multi-stage effort. In short: the project's dependencies (git submodules) had been re-linked incorrectly after the repo was migrated from a plain zip download into a fresh Git history, and several source files had accumulated small incompatibilities with the specific library versions the project actually needs. None of this is related to the freeze itself — it was blocking any build from succeeding at all.

### 2.1 Wrong dependency repositories
Two of the three vendored libraries (`extern/CommonLibSSE-NG`, `extern/FidelityFX-SDK`) had been linked to generic upstream repositories instead of the specific forks the project's own `.gitmodules` requires:

| Submodule | Was pointing at | Should point at |
|---|---|---|
| `extern/CommonLibSSE-NG` | `CharmedBaryon/CommonLibSSE-NG` | `alandtse/CommonLibVR` |
| `extern/FidelityFX-SDK` | `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK` | `alandtse/FidelityFX-SDK-DX11` (branch `optiscaler-build`) |

The wrong `CommonLibSSE-NG` fork was missing/restructuring symbols the code depends on (`RE::BSGraphics::RendererShadowState`, `RE::BSUtilityShader`, `REX::EnumSet`, etc.). The wrong `FidelityFX-SDK` had no DX11 backend at all, so a required header (`ffx_dx11.h`) simply didn't exist in it. Re-pointing both submodules at the correct repositories resolved both classes of error.

### 2.2 A third dependency was missing entirely
`extern/Streamline-DX12` (NVIDIA's Streamline SDK, providing `sl.h`) had never actually been added as a submodule — it was declared in `.gitmodules` but the folder itself was empty. It was added properly via `git submodule add`.

### 2.3 Submodule commit pinning
Even after pointing at the correct repositories, the exact **commit** mattered. Adding a submodule fresh (with no pinned commit) grabs whatever the default branch's tip happens to be at that moment — not necessarily the commit the project's code was actually written against. This surfaced as a real, if non-obvious, bug: at the commit initially picked up, `CommonLibVR` represents D3D11 render-target fields (texture/SRV/RTV/UAV) using its own internal `REX::W32::` wrapper types rather than native D3D11 types, which broke type-checking in multiple files (`EditorWindow.cpp`, `Deferred.cpp`, and by extension likely others).

This was resolved by comparing against the **exact commit the real upstream project** (`community-shaders/skyrim-community-shaders`) pins for the same submodule, and re-pinning to match:

| Submodule | Pinned to (matches upstream) |
|---|---|
| `extern/CommonLibSSE-NG` | `70c1acd5261210982bd52f6d4468a082fe04d798` |
| `extern/Streamline-DX12` | `a9ed1f58436864891f68b0458300464dc53d9a69` |
| `extern/FidelityFX-SDK` | already matched upstream (`d3fc8bcd...`) — no change needed |

At this correct commit, render-target fields are native D3D11 types as the code expects, so an earlier stopgap patch (manually casting between native and `REX::W32::` types in `EditorWindow.cpp`) became unnecessary and was reverted.

### 2.4 `Matrix` symbol ambiguity
The correct `CommonLibVR` fork has a real quirk: one of its headers (`RE/S/State.h`) does `using namespace DirectX::SimpleMath;` at global scope, which is against Microsoft's own documented best practice for headers. This collided with the project's own global `using Matrix = DirectX::XMFLOAT4X4;` alias, making every unqualified use of `Matrix` inside `Globals.h` ambiguous to the compiler.

**Fix:** moved the project's `Matrix`/`Vector2`/`Vector3`/`Vector4` aliases from true global scope into `namespace globals`, where they're already conceptually used. This lets nested lookup resolve them locally before ever reaching the polluted global scope, without needing to touch the handful of other files that intentionally rely on the *other* meaning (`DirectX::SimpleMath::Matrix`, the richer math type) at their own scope.

### 2.5 Storage type vs. math type mix-ups
Once the ambiguity above was resolved, a related but distinct issue surfaced: several call sites called math-only methods (`.Invert()`, `.Transpose()`) directly on values returned by camera accessors (e.g. `GetCameraViewProjUnjittered()`), which are declared to return the plain storage type (`globals::Matrix` = `XMFLOAT4X4`, a POD struct with no methods) rather than the full-featured `DirectX::SimpleMath::Matrix`. This was always latently broken, but had been masked until the ambiguity fix above let the compiler get far enough to notice it.

**Fix:** wrapped each of the 9 affected call sites (across `ExponentialHeightFog.cpp`, `FidelityFX.cpp`, `Streamline.cpp`, `ScreenSpaceShadows.cpp`, and `Upscaling.cpp`) in an explicit `Matrix(...)` conversion before calling the math method. One additional suspicious site (`ScreenSpaceGI.cpp`, `eye.viewMat.Invert()`) was identified but **left unfixed** — the type of `eye.viewMat` couldn't be confirmed from the source available at the time, so it wasn't touched rather than guessed at. This is worth checking if a similar error appears there in a future build.

### 2.6 CI workflow: release step failing on ordinary builds
Separately from the C++ build, the GitHub Actions release workflow (`release-build.yaml`) was failing its "Post Release" step on every ordinary branch build, because it only checked `github.event_name == 'workflow_dispatch'` rather than whether the run was actually against a tag — so *any* manual run, even against a branch, tried (and correctly refused) to publish a release.

**Fix:**
- The release/attach/Nexus-upload jobs now gate on `github.ref_type == 'tag'`, so non-tag runs are cleanly **skipped** rather than failed.
- Removed the explicit `exit 1` step that had been enforcing this the hard way.
- Added a credential check so the Nexus upload step is skipped gracefully (not failed) if Nexus API credentials aren't configured for a given repo/fork. (An initial version of this check used `secrets` directly inside a job's `if:` condition, which GitHub Actions doesn't permit and briefly broke the whole workflow file — corrected by moving the check into a proper step.)

With all of the above resolved, the project builds and all CI jobs (plugin build, all four shader-validation configurations, C++ unit tests, shader unit tests) pass.

---

## 3. DLSS-NR intermittently stopping mid-session — recovery added, root cause not diagnosed

### What's confirmed
With the freeze fix in place and a genuinely up-to-date build actually deployed, real play-session logs show the underlying cross-API fence **does** still occasionally time out under Proton — exactly the scenario the freeze fix was designed to survive:

```
[E] [DLSSNR] EndD3D12 failed hr/ngx=0x800705B4 status=initialized detail=
```

(`0x800705B4` is the Win32 `ERROR_TIMEOUT` code, confirming this is the 250ms bounded wait expiring, not some other failure.)

**This is not itself a bug** — it's the expected, survivable failure mode the freeze fix was built around. The actual problem was in how the mod responded to it.

### The real bug: a one-way failure switch
Internally, `NeuralRendering::Renderer` tracks a `failureLatched` flag. Once **any single frame** triggers this flag — including one bounded 250ms timeout — every subsequent frame's `Apply()` call immediately returned `false` and did nothing, **permanently, for the rest of the play session.** The only thing that ever cleared this flag was an unrelated `Reset()` call triggered elsewhere in the code (tied to things like resolution changes), not anything related to the failure itself.

In practice, this meant one transient stall — which is more likely to happen during exactly the moments of heavier GPU load, like a cell load, opening a menu, or a fast camera turn — would silently and permanently disable DLSS-NR, and it would only ever come back if the player happened to trigger a full loading-screen cell transition (which incidentally also triggers that unrelated `Reset()`). Walking around continuously in an exterior worldspace, which Skyrim streams in the background without a loading screen, never triggers that reset — matching the reported behavior that DLSS-NR reliably "fails and stays failed" specifically during that kind of continuous outdoor movement.

### Fix implemented: self-healing failure recovery
`Renderer.cpp` was changed so the failure latch is no longer permanent:

- On any failure, the timestamp and a consecutive-failure counter are recorded (in addition to the existing latch).
- On the next frame after a **2-second cooldown**, the renderer automatically performs a full `Reset()` (properly tearing down and reinitializing the D3D12 interop and NGX runtime) and attempts to resume — with no need for a loading screen, menu, or any other unrelated trigger.
- If it fails **5 times in a row** even with retries, it gives up and stays disabled rather than retrying forever every 2 seconds, since at that point something is more likely to be genuinely broken than transient, and constant retries would themselves become a source of stutter.
- Any single successful frame resets the consecutive-failure counter back to zero, so occasional stalls over a long session don't creep toward that give-up threshold.

### What this does and doesn't fix
- **Does fix:** DLSS-NR no longer requires a manual toggle-off/on or a loading screen to recover from a stall. It should now blink off for roughly 2 seconds and come back on its own, repeatedly, for as long as failures stay infrequent.
- **Does not fix:** *why* the cross-API fence times out in the first place. That remains an open question rooted in Proton's DXVK/VKD3D-Proton cross-API fence emulation being less mature than native Windows' handling of the same handshake. Recovery, not prevention, is what's been addressed.
- **Not yet confirmed:** whether the fix is actually in the binary being tested — an earlier test log showed no trace of the new recovery behavior at all, which was ultimately traced to the deployed build only including the shader-only `Upscaling` package (no plugin DLL) rather than `Core`/`AIO`, meaning none of the C++ changes in this report had actually reached the game yet. Confirming a build that includes the updated DLL, and gathering a fresh log from it, is the natural next step.

---

## 4. Open items / suggested next steps

1. **Confirm the fix is actually running.** Deploy a build that includes `Core` (or `AIO`) — not just the `Upscaling` shader package — and clear the shader disk cache before testing.
2. **Watch for recovery-cycle logging** (`attempting automatic recovery after failure (attempt N/5)`, and the updated `EndD3D12 failed ... (consecutive=N, retry in Xms)` line) in future logs to confirm the self-heal path is actually engaging, and how often.
3. **If failures cluster heavily around cell loads/menu opens/fast camera movement**, that's a hint the underlying timeout is load-correlated (e.g. GPU driver thread contention, big VRAM allocations, or motion-vector discontinuities feeding the DLSS-NR network) — worth investigating as the actual root cause once enough recovery-cycle data exists.
4. **Double-check `ScreenSpaceGI.cpp`'s `eye.viewMat.Invert()`** (Section 2.5) if a similar `Matrix`-type compile error appears there in a future build.
