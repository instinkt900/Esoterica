# Progress Log

Running state of the Linux port. **Every task appends here before it counts as done**
(Conventions rule 9). Newest entries go at the top of each section.

This file keeps a chain of independent agent sessions coherent. When you start a session, read
"Current state" and "In flight" first.

**What is written but not verified lives in [Blocked.md](Blocked.md)**, indexed by the machine
that would unblock it. The entries below are dated and the blocked items are scattered through
them; that file is how they are found. A task that leaves something unverified adds a row there.

---

## Current state

> ### **The engine renders the pbrdemo map correctly on Linux.**
>
> Lit, shaded, textured, shadowed geometry with a sky and a reflective ground plane, on an RTX
> 3090, with host validation on and **zero validation messages** over 30 seconds - and **no
> `VK_LAYER_MESSAGE_ID_FILTER`**, which is new. No device memory leaked, clean shutdown. See
> [Rendering: where we are](#rendering-where-we-are).
>
> **The advice "do not chase rendering on this machine" applied to the first machine only.** A
> second machine has a discrete NVIDIA GPU and rendering is exactly what should be chased there.
> See [[dev-machines]] in the 2026-08-31 entries.
>
> **The laptop cannot run the editor at all.** It dies about three seconds into its frame loop,
> on the device loss its Intel UHD 620 has had since Phase 6. That is what makes P7.6 and the
> last link of P7.5 machine-blocked; see [Blocked.md](Blocked.md).
>
> **The GPU-blocked queue is being worked through on the 3090.** P5.12, P5.15 and four of the
> seven features in criterion 8 are verified, and so is P5.11. What is left there is **P5.16
> raytracing** and the rows that need the editor rather than the engine.
>
> ### **The editor has been shaken down, and every tool opens.**
>
> P7.6's second pass, 2026-09-02, an hour of driving on the RTX 3090 with **zero errors and zero
> validation messages**. Every tool opens - importer, dependency viewer, system info, bulk edit,
> memory tracker, settings, and the animation graph editor with its own live 3D preview.
> **Multi-viewport works**, a tool drags out into its own OS window and renders. **Hot reload
> closes end to end**, which is P7.5's fifth link: a material rewritten with the
> rename-over-original pattern changed the live viewport with no restart. Clean shutdown, no leaks.
>
> **Two defects found and fixed, both in this fork's own files, no upstream edit.** Every
> popped-out viewport was a window with no swapchain - `PlatformHandleRaw` is null on Linux, so
> `ImguiRenderer` fell back to an `SDL_WindowID` and `SDL_Vulkan_CreateSurface` rejected it. And
> the title bar hit test's stale hover state, diagnosed on 2026-09-01, is fixed and measured.
> See the entry below.

**Phases 0 to 7 are done. [Phase 8](Phases/Phase8-Completion.md) is what is left**, and it is
verification and debt rather than porting. The whole tree builds in Debug and Release, and
**nothing in it fails to compile.** `Esoterica.Applications.Editor` and
`Esoterica.Applications.ResourceServer` both build, link and launch; the Resource Server serves on
127.0.0.1:5556, spawns its compiler workers and draws its full UI. Per-task state is in
[In flight](#in-flight).

**One thing has never been checked at all, and it is the whole of Phase 8's remaining risk: no
Windows build has been run at any point in this port.**

**The engine simulates.** P8.2, 2026-09-02: "Play Map" starts and stops game preview cleanly, three
dynamic physics bodies fall and settle on a static collision mesh, camera control works from
keyboard and mouse, and a skeletal asset opens in the skeleton, animation graph and ragdoll editors.
That run also found and fixed a port bug that segfaulted the editor on the first skeletal mesh it
ever loaded - an unaligned `mprotect` in `VirtualMemoryCommit` that was failing silently. See the
entry below.

**The engine and the editor no longer need `-packaged`.** `EnsureResourceServerIsRunning` was
Windows-only and returned false, so the network resource provider could never start. P7.3 opened
it to Linux; see the entry below for the one piece of local configuration it needs.

**`Path::Split` asserted on every absolute Linux path**, and the editor could not initialise.

Escalated, approved and fixed in P7.1: one character in
`Code/Base/FileSystem/FileSystemPath.cpp`, registered in [TouchedFiles.md](TouchedFiles.md).

**Phase 6 is written and does not meet its goal. The engine reaches its frame loop.** Open question 8 is answered and the
`VK_ERROR_UNKNOWN` is gone. `Shaders::Initialize` runs to the end, every compute and graphics
pipeline is created, and the engine stops at **`CmdExecuteIndirect`**, which is P5.13's refusal
and [P5.17](Phases/Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change)'s
job. That is the wall this port has been aiming at since Phase 5.

### Start here

```bash
python3 Code/Scripts/NinjaGen/NinjaGen.py
ninja -f Build/Linux/Esoterica.ninja Build/Linux_Release/Esoterica.Applications.Reflector
./CompileShaders.sh
python3 Code/Scripts/NinjaGen/NinjaGen.py
ninja -f Build/Linux/Esoterica.ninja Build/Linux_Release/Esoterica.Applications.Engine

printf '[Render:RHI]\nEnable_Host_Validation = true\n' > Build/Linux_Release/Esoterica.ini

VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true \
  ./Build/Linux_Release/Esoterica.Applications.Engine \
  -map data://demo/render/pbr/pbrdemo.map
```

**Five things about that, each of which cost a session to find:**

- **`-packaged` is no longer required, and the command above no longer passes it.** P7.3 opened
  `EnsureResourceServerIsRunning` to Linux, so the engine starts the Resource Server and reaches
  it over the network, as the Windows build does. That needs two keys in `Esoterica.ini`; the
  P7.3 entry has them. `-packaged` still works, and reads
  `Build/Linux_<configuration>/CompiledData` directly, which is what Phase 3 filled.
- **Run `CompileShaders.sh` and then `NinjaGen.py` again after any shader change.** The generated
  `.cpp` files are picked up by a glob, so the build will not see a new one otherwise.
  **"Any shader change" includes one that arrives in a merge**, and nothing warns you: `ninja`
  does not know the `.esh` and `.esf` sources, so the build stays green while the binary carries
  the old SPIR-V. The symptom is a `vkCreateShaderModule` validation error naming a capability
  the device lacks, which reads exactly like a hardware gap. Check the dates before believing it:
  ```bash
  ls -l Code/Engine/_Module/_AutoGenerated/Shaders/ShaderRegistration.cpp Code/Base/Render/RHI.esh
  ```
- **Validation is off unless the ini says otherwise.** `RenderSettings::m_enableHostValidation`
  defaults to false and only a Debug build forces it on. The generated `Esoterica.ini` is empty
  because `Settings::SaveSettings` skips every property still at its default, so the section has
  to be written by hand. The key names come from the reflected `Category` and `FriendlyName`.
  That is upstream behaviour, not a Linux defect.
- **`VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true` replaces
  `VK_LOADER_LAYERS_DISABLE='*'`.** It turns off only the validation layer's bundled spirv-val,
  which is where the stale SPIRV-Tools lives, and leaves every other check on. P6.7 turned the
  layers off entirely and lost validation with them. No newer layer package is needed.
  `VK_LAYER_MESSAGE_ID_FILTER=<vuid>[,<vuid>]` silences one VUID at a time, which is how several
  walls were surveyed in one run.
- **The binary is `Esoterica.Applications.Engine`**, named after its project like the Reflector
  and the ResourceCompiler, not `EsotericaEngine` as the phase document originally wrote.

### Where it stops, and it depends on validation

> **Superseded. The hang is fixed and the engine draws geometry.** It was
> `vkCmdSetFragmentShadingRateKHR` on a transfer command buffer, not the dropped mesh draws. See
> [Rendering: where we are](#rendering-where-we-are). The paragraph below is kept for the record.

**The whole frame records and submits with zero validation errors, and the GPU hangs executing
it.** `VK_ERROR_DEVICE_LOST`. **Deliberately not chased**: the mesh draws are dropped on this
hardware, so later passes read what the geometry path never wrote, and a real defect cannot be
told apart from an artefact of that without mesh shader hardware. The image layouts entry records
the two candidates and how to bisect it when there is hardware to bisect on.


**With validation on**, in the same place, with **zero validation messages** on the way there
once the four hardware-gap VUIDs are filtered. That was not true a day ago; see the
`NoDescriptors` entry.

### What the first machine still cannot do

**This table is about the development laptop, not about the port.** The RTX 3090 machine closes
four of the five rows, which is why it renders and this one does not. `storageInputOutput16` is
absent on both, and since P5.20 **no shader in the engine needs it** - the row is a device fact
with no consequence left.

**ANV accepts every one of these modules with validation off**, so the engine runs past them on
the laptop; they are shaders that are invalid by the spec and tolerated by the driver, which is
not the same as correct.

| Gap | Modules that declare it | Intel UHD 620 | NVIDIA MX250 | llvmpipe |
|---|---|---|---|---|
| `VK_KHR_fragment_shader_barycentric` | 13 | no | no | no |
| `storageInputOutput16` | 1 | no | no | no |
| `shaderSharedInt64Atomics` | 1 | no | yes | yes |
| `VK_EXT_mesh_shader` | debug draw | no | no | yes |
| `VK_EXT_mutable_descriptor_type` | all of them | yes | **no** | yes |

### What is behind that

**Esoterica renders on Linux.** P6.6 made a `VkSurfaceKHR` from the `SDL_Window*` and cleared and
presented twelve frames with **no Vulkan validation errors**. Every Phase 5 RHI call in that path
had never executed before. Running the backend for the first time found **four defects in it**,
all fixed; see the P6.6 and P6.7 entries.

**The engine binary builds, links and starts.** It reads its settings, loads compiled data, opens
a window and creates a Vulkan device. **Phase 6 acceptance criterion 1 is met.**

**The window, input and imgui layers are all done and each was proved by running it.** SDL3
`release-3.4.14` builds from source, `LinuxApplication` runs a window and an event loop, the imgui
platform backend has **multi-viewport verified** - three imgui windows became three live SDL
windows - and keyboard, mouse and gamepad all work: a **complete scancode table, 105 scancodes to
105 distinct `InputID`s**, and a full `InputSystem` pass driven by an SDL virtual gamepad.
**Open question 4 is answered: the port always builds SDL3, because Ubuntu 24.04 LTS packages
none.**

**The port now edits one upstream file that is not a pure include switch.**
`RHI::MaxPendingFrames` is 3 on Linux, because the Intel UHD 620 and llvmpipe both report a
swapchain `minImageCount` of 3. Four lines added, zero modified, Windows bit for bit unchanged.
Escalated, approved, made, and registered in [TouchedFiles.md](TouchedFiles.md).

**`EE_UNIMPLEMENTED_FUNCTION` is gone from `Base` outside `RHI_Vulkan.cpp`.** The three there are
Phase 5's; two more in `Triangle.h` and `Encoding.cpp` are upstream's own.

**Two findings from P6.3 that later phases need.** First, the vendored `ImguiPlatform_Win32.cpp`
is about three years behind the imgui core beside it: the core is `v1.92.9b-docking`, the backend
is roughly 1.89.1. The Linux backend is therefore built from `v1.92.9b-docking`'s
`imgui_impl_sdl3.cpp`, and its vendored region keeps upstream's formatting so it can be
re-synced. Second, **imgui will not enable viewports on Wayland**: `ImGui_ImplSDL3_Init` sets
`ImGuiBackendFlags_PlatformHasViewports` only for video drivers on its global-mouse white list,
and `wayland` is not on it. The editor's docking UI depends on viewports, so Phase 7 needs to
know.

**The Phase 5 / Phase 6 surface question is answered, and P6.6 implements it.**
`Platform::SetMainWindowHandle` holds the `SDL_Window*`, and **`RHI_Vulkan.cpp` creates the
`VkSurfaceKHR` itself**, through a new Linux-only `Platform::CreateVulkanSurface` in
`Platform_Linux.cpp`. That edits no upstream file, and `Base/Render` still includes no window
system header. It revises the second half of P5.3's answer, so read the two decision entries
together. **Not written yet: P6.6 owns it.**

**Phase 5 remains merged and incomplete, but it is no longer unrun.** All sixteen groups are on
`main`. **The parts P6.6 and P6.7 exercised are verified** - context, queues, command pools and
buffers, the swapchain, barriers, a render pass, submit and present - and everything else is
still compile-verified only. Each P5.x entry's "Not verified" list stands apart from those.

**Open question 7 is answered, and the work is scheduled as P5.17.** The shader reads its own
command's root data out of the argument buffer, indexed by `DrawIndex`. **It is deliberately
sequenced after Phase 6 bring-up**, because nothing here can be tested until the engine runs. Read
the decision entry and
[P5.17](Phases/Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change)
before starting it. **The frame still cannot draw until it lands.**

**All 16 groups are written. 15 of them are real; P5.13 is the exception and cannot be finished
here.** Phase 5's own note says this count is the single most important piece of state.
**Verification is now partial rather than absent**: the P6.6 and P6.7 bring-up exercised P5.1,
P5.2, P5.3, P5.4, P5.7's shader creation, P5.8 and P5.9, and found four defects across them. No
other group has executed.

**3 `EE_UNIMPLEMENTED_FUNCTION` remain, none of them a whole function.** One is the indirect
refusal from open question 7. The other two are markers that name a caller if it ever appears: a
sampler border colour Vulkan cannot express without `VK_EXT_custom_border_color`, and the
static-sampler path the binding model does not use.

**"Written" is not "works".** Four of the sixteen groups cannot be exercised on any hardware in
this machine, and one cannot be finished at all:

| Group | Why it is unverifiable here |
|---|---|
| P5.13 indirect draws | Open question 7. Half written by decision, and the frame is built on it. |
| P5.14 mesh shaders | Neither real GPU here has `VK_EXT_mesh_shader`. |
| P5.15 variable rate shading | Switched off to match the Direct3D 12 backend, which reports `NotSupported`. |
| P5.16 raytracing | No caller anywhere, no way to build a shader table on either backend, and only `llvmpipe` has the extensions. |

**Neither GPU in this development machine supports mesh shaders**, so the engine's debug draw
cannot run here. `VK_EXT_mesh_shader` is present only on `llvmpipe`, the software rasteriser. The
Intel UHD 620 and the NVIDIA MX250 both lack it, and both would lack it on Direct3D 12 as well, so
this is the hardware being below the engine's bar rather than a port problem. See the P5.14 entry.

**The frame cannot draw. The decision is made, the work is not done.** Every render
pass is built on `CmdExecuteIndirect` - `RenderPass_ForwardShading` uses it six times,
`RenderPass_CascadedShadow` twice, `RenderPass_DebugDraw` six times - and **the engine's command
signatures cannot be expressed by any Vulkan indirect draw.** A Direct3D 12 command signature sets
root constants and binds root descriptors per command; Vulkan's indirect draws read draw arguments
and nothing else, and a compute pre-pass does not help because a pre-pass cannot bind a descriptor
either. **The answer is a shader change, and it is P5.17.** P5.13 landed its mechanical half by
decision and refuses the rest at the line; see the P5.13 entry, the decision entry, and P5.17.

**Superseded: `m_pNativeWindowHandle` is an `SDL_Window*` on Linux, and `RHI_Vulkan.cpp` makes
the surface from it** through `Platform::Linux::CreateVulkanSurface`. P5.3 said the application
would create the surface and hand it over, and there turned out to be nowhere for it to do that.
See the 2026-08-30 decision entry. **The application does still drive swapchain recreation, not
the RHI**, which is the other answer Phase 6 was promised, and that half of P5.3 stands.

**A Vulkan queue does not execute its submits in order and a Direct3D 12 queue does**, so every
submit now waits on the value the previous submit on that queue signalled. The engine depends on
the Direct3D guarantee and `RHI.h` gives it no way to ask for it. See the P5.3 entry.

**The render pass is opened by the first draw, not by `CmdSetRenderTargets`.** The engine records
image layout barriers between the two and a barrier may not run inside dynamic rendering, so the
begin is deferred. See the P5.9 entry; anything that changes `CmdSetRenderTargets` or a draw has
to keep that order.

**The `DataFormat` to `VkFormat` mapping is complete, and there is exactly one of it.** All 99
formats map, the three `DeviceCapabilities` format arrays are filled from the device, and every
texture, buffer view and pipeline attachment format reads the same function.

**Both Phase 4 decisions are now implemented, each in exactly one place.** Clip-space Y is
inverted in `CmdSetViewport` with a negative viewport height, and nowhere else. Heap set 1 is
bound in `CmdSetPipeline`, not in `BeginCommandBuffer`. `CmdSetViewport` has run; the heap bind
has not, because nothing has set a pipeline yet.

**The bindless heap now exists in code.** `CreateContext` builds set 1 exactly as the Phase 4
binding model specifies, and `GetBufferHandle` returns an index into it. **One correction to that
recorded decision was needed**, on a flag Vulkan does not allow where the entry put it; it is
written up below and it changes nothing the shaders can see.

**Phase 5 was written against the compiler and link only, on purpose**, because nothing on Linux
could reach `RHI::CreateContext` until Phase 6 provided an entry point. See the 2026-08-28
decision entry. **That has now happened**, and the first execution found four defects in four
different groups. **Treat every P5.x entry as compile-verified and run-unverified except where
the P6.6 and P6.7 entries say otherwise** - which is context, queues, command pools and buffers,
the swapchain, barriers, a render pass, submit and present.

Previously: **Phase 4 (done on Linux).** DXC is built from source with three patches that fix its SPIR-V back
end, and **all 46 shader stages compile and pass `spirv-val`**. `./CompileShaders.sh` exits 0 and
fills `_AutoGenerated/Shaders/`; the generated C++ compiles and links. Validate with
`External/DirectXShaderCompiler/bin/x64/spirv-val --target-env vulkan1.3 --scalar-block-layout`
- not the one on `PATH`, and not without the layout flag. Both matter; see the entries below.

**No known correctness bugs are outstanding.** Defect 3 is fixed by patch 0003: bitfields that
differ only in signedness now merge, so `MeshCluster` is 32 bytes and `RenderView` 352, matching
Direct3D and C++. DXC carries three patches, all in its SPIR-V back end.

**Both binding decisions are made and written for Phase 5 to implement against**: the bindless
binding model, and clip-space Y, which the Vulkan viewport inverts. Phase 3's five `.material`
resources compile now too, which closes the last Phase 3 gap that did not need Windows. See the
2026-08-28 entries below.

Previously: **Phase 3 (all build and compile criteria met).** `EsotericaResourceCompiler` builds,
links and compiles 22 of the 27 real resources under `Data/` into 38 output files: every texture,
mesh, physics mesh, physics material database and the map. Debug and Release produce
byte-identical output. All four libraries link. The file system watcher passes a 15-check
scratch test, including a subdirectory created after watching started and `IN_Q_OVERFLOW`.

The 5 that do not compile are the `.material` files, and they fail for one reason: they
deserialize `EE::Render::Shaders::DefaultPBRParameters`, a type that only the Reflector's shader
pass generates.

**Correction.** Phase 3 recorded that pass as blocked because "DXC crashes on the
`ResourceDescriptorHeap` bindless model". That is wrong, and it sent this session looking in the
wrong place. Bindless is fine: a shader using `ResourceDescriptorHeap` and `SamplerDescriptorHeap`
compiles to SPIR-V cleanly. There are two unrelated defects in DXC's SPIR-V back end, and neither
has anything to do with bindless. Both are described below.

Previously: **Phase 2 (complete on Linux).** `./RunReflection.sh` generates reflection for all five modules
and exits 0. 245 files, 238 of them `.cpp`. The run is idempotent, and clean plus rebuild
reproduces byte-identical output. The Windows build has not been run.

| Phase | Status |
|---|---|
| 0 - Build System | **done** |
| 1 - Base Platform Layer | **done** (Tester itself still blocked on Phase 2) |
| 2 - Reflector | **done on Linux** (criterion 5 and 8 need a Windows machine) |
| 3 - Resource Compiler | **done on Linux.** The 5 materials compile as of Phase 4's defect 2 fix; only the byte-comparison against Windows remains |
| 4 - Shader Pipeline | **done on Linux** (criteria 6 and 10 need a Windows machine). DXC builds from source with three patches; all 46 shader stages compile, validate and link with layouts matching Direct3D, and `CompileShaders.sh` runs them |
| 5 - Vulkan RHI | **all 16 groups written and merged to `main`, and the backend has now run.** P6.6 and P6.7 executed it for the first time and found four defects, all fixed. Context, queues, command pools and buffers, the swapchain, barriers, a render pass, submit and present are **verified**; the rest is still compile-verified only. 3 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 103, and none is a whole function. P5.13 is finished by P5.17, which still comes after P6.8. Criteria 5 to 10 are now checkable |
| 6 - Windowing and Input | **started.** P6.1 to P6.7 done: SDL3 builds from source, `LinuxApplication` runs a window and an event loop, imgui and all three input devices work, the port renders on Linux, and **the engine binary builds, links and starts**. Criterion 1 is met. P6.8, first light, is next, and it owns the `VK_ERROR_UNKNOWN` compute pipeline that blocks a map |
| 7 - Editor and Tools | not started |

Linux build status: `libEsoterica.Base.so`, `libEsoterica.Engine.Runtime.so`,
`libEsoterica.Engine.Tools.so`, `libEsoterica.Game.Runtime.so`, `libEsoterica.Game.Tools.so`,
`Esoterica.Applications.Reflector` and `Esoterica.Applications.ResourceCompiler` all build.
`Applications/Editor` and `Applications/ResourceServer` do not, and are Phase 7.
`Applications/BuildGenerator` is excluded permanently.
Windows build status: **not run.** 69 upstream files carry `+494 -71` lines across Phases 0-3.

## Rendering: where we are

**Read this before touching anything in the render path.** It records what works, and - more
valuable - the wrong turns that cost two sessions, so the next one does not repeat them.

### What works

Verified by running, on an RTX 3090, with `Enable_Host_Validation = true` and **zero validation
messages** over 30 seconds:

- The full frame records and submits. No `VK_ERROR_DEVICE_LOST`, no kernel `Xid`, no leaked device
  memory.
- Cluster culling runs on the GPU and is correct. Counter read back to the CPU:
  `clusterVisible = (1,83) (1,84) (1,85) (1,86)`, count 15, dispatch 15x1x1.
- Mesh shader draws execute: `vkCmdDrawMeshTasksIndirectCountEXT`, resolved indirect count 1,
  `<612,1,1>` workgroups in a full frame.
- **The frame is correct.** Geometry, UVs, textures, normals, image-based lighting, direct
  lighting, shadows, the sky and the reflective ground plane all render.
- **Named passes, from a frame capture**: light culling, instance culling, cluster culling,
  forward shading (depth only, opaque + alpha test, alpha blend), 4 shadow cascades, XeGTAO
  (prefilter, main, denoise), SMAA (3 draws), post processing and debug draw (3 depth modes).
  **GTAO and SMAA are each verified by an A/B**, not just by the pass being recorded.
- **Debug names and markers work.** 75 nested label scopes and 834 named objects in one capture.

### What the two stage-interface defects looked like, and why they hid

Both are in the 2026-09-01 entry. The short version, because the *symptom* is the misleading part:

- **Locations, not names.** Vulkan matches inter-stage variables by `Location` and DXC numbers
  them in declaration order, so a mesh shader that declares `out primitives` before
  `out vertices` disagrees with the pixel shader about every slot. Fixed with
  `-fvk-stage-io-order=alpha`.
- **A pixel shader in an indirect draw has no root arguments.** P5.17's `#define RootCBV
  EE_g_RootCBV` covers the whole translation unit, and only the mesh shader entry point filled
  the static. Fixed with `EE_INDIRECT_PIXEL_ENTRY_INIT`.

**Neither produces an error, anywhere.** `SV_Position` is a builtin with no location, so geometry
still lands in the right place and only shading is wrong - which reads as a geometry bug. Both
modules are individually valid, so validation is silent. **When shading is wrong but the
silhouettes are right, suspect the stage interface before the vertex data.**

**`HLSL_STATIC_ASSERT` is compiled out on SPIR-V** (`RHI.esh:68`), because DXC's SPIR-V back end
does not implement `_Static_assert`. Every shared-struct size check is therefore absent on Linux
and present on Windows. That is worth remembering for any layout suspicion.

**A layout suspicion is answerable in a minute, so answer it instead of carrying it.** Write a
ten-line compute shader that includes the `.esh` and touches the struct, compile it with the same
flags the Reflector uses, and read the `OpMemberDecorate ... Offset` lines out of `-Fc`. That is
how `StaticMeshVertex` and `MeshCluster` - the previous handoff's prime suspect - were cleared:
both match the C++ byte for byte, including the signed 24-bit anchor bitfields.

### The two earlier bugs, and why they hid so well

**Every triangle was back-face culled.** `frontFace` was inverted. The comment on that line said
so itself - "the classic porting bug ... reasoned, not verified" - and the reasoning counted a
double negative once too often.

**Every triangle index read as zero.** `DefaultMeshShader.esh` declares `Buffer<uint>`, a typed
buffer, but the cluster triangle buffer was created with a stride and **no format**, so the RHI
could not tell `Buffer<T>` from `StructuredBuffer<T>` and wrote a storage-buffer descriptor where
the shader wanted a uniform texel buffer. **A mutable descriptor heap swaps one for the other in
silence** - no validation error, reads return 0. Direct3D 12 makes a structured SRV there and
tolerates reading it as `Buffer<uint>`, which is why it never surfaced.

Either bug alone produces a completely black frame, which is why fixing one at a time showed
nothing and made both look like something else.

### The two techniques that cracked it

**`vkCmdClearAttachments` inside the live render pass, immediately after the draw.** It turned the
window green while the draw itself wrote nothing. A clear inside a render pass instance bypasses
culling and fragment shading but shares the attachment, the render area, the image view and
present - so it separates "the primitive was culled" from every other explanation in one step.

**Bisecting the pixel shader one line at a time, against a deterministic frame.** Force
`debugVisMode` to `ALBEDO`, then `NORMALS`, then `SSAO`; tint the inside of a branch to prove it
was entered; write the three lighting terms into R, G and B at the end of `PS_main`. Each is one
line, a two-minute rebuild and a screenshot, and each removes a branch of the tree for good. This
is what separated "the geometry is wrong" from "the shading inputs are wrong", which was the whole
question.

It only works because the frame is reproducible - see the Xvfb note below. Do this before
reaching for RenderDoc.

### Measurement traps that produced confidently wrong answers

Each of these was believed, written down, and later disproved:

| Trap | What it looked like | The truth |
|---|---|---|
| Fire-once diagnostics at startup | "No geometry is registered, cluster capacity is 1" | 1 is the empty baseline and the map had not finished loading. Measured late: **9286 clusters**. Always gate a render diagnostic on a frame counter |
| RenderDoc's Mesh Output view | "Every vertex is identical, so the vertex fetch is broken" | That view is **index-driven**. Every index was 0, so it displayed vertex 0 over and over. The vertices were fine |
| Reading a texture back with the wrong `oldLayout` | "The HDR target is all zeros" | A barrier with the wrong `oldLayout` makes the contents undefined. Pass the layout the resource is actually in |
| De-duplicating the log by collapsing adjacent lines | "Descriptor heap slot 2 is double-allocated" | Every message is logged twice and the two copies interleave. Add a sequence number before drawing conclusions from ordering |
| Testing one override at a time | "Not the indices, not the counts, not the culling" | Two independent bugs. Any single override still gave black. Combine overrides, or fix the cheapest confirmed bug first |
| Overriding a shader with an early `return` | Engine asserts at startup | DXC strips resources nothing references, the reflected layout stops matching the root signature. Keep every declared resource live - multiply it by `1e-9` and add it to the result |
| "Scrambled geometry" | A vertex decode bug, and a whole handoff written around one | Correct geometry, drawn in wireframe by a debug overlay that garbage flags had switched on, over surfaces shaded from garbage. **Name what you can see, not what you infer**: the orange was `float3( 1.0, 0.4, 0.0 )`, a literal in `MaterialShaderPBR.esh`, and grepping the colour would have found it in a minute |
| Screenshotting the developer's desktop | An occluding window captured instead of the engine, twice | Run the engine on its own `Xvfb` display. It also makes the frame byte-identical run to run, which is what makes a one-line A/B worth anything |

### How to capture a frame

**Render on a private display**, not the desktop. It keeps captures reproducible and keeps other
windows out of them:

```bash
Xvfb :77 -screen 0 5120x1440x24 &
DISPLAY=:77 ./Build/Linux_Release/Esoterica.Applications.Engine -map data://demo/render/pbr/pbrdemo.map -packaged &
sleep 22
WID=$( DISPLAY=:77 xwininfo -root -tree | grep '"Esoterica Engine"' | grep -oP '0x[0-9a-f]+' | head -1 )
DISPLAY=:77 xwd -id $WID -silent > frame.xwd && ffmpeg -i frame.xwd frame.png
```

**The screen has to be at least as large as the saved window position.** SDL restores it from
`EsotericaEngine.layout.ini`, which on a multi-monitor desktop can be `+3424+74`; a smaller Xvfb
screen puts the window off-screen and every capture comes back black.

The same 22-second capture is pixel-identical between runs, so two builds can be diffed directly.

RenderDoc is **not** packaged by Ubuntu; it is an official tarball, and `renderdoc_1.45` matches
the `renderdoc_app.h` pin in `DownloadDependencies.sh`.

```bash
curl -L -o /tmp/renderdoc.tar.gz https://renderdoc.org/stable/1.45/renderdoc_1.45.tar.gz
tar -xzf /tmp/renderdoc.tar.gz -C ~/
~/renderdoc_1.45/bin/renderdoccmd vulkanlayer --register --user

printf '[Render:RHI]\nEnable_Render_Doc = true\n' > Build/Linux_Release/Esoterica.ini
LD_LIBRARY_PATH=~/renderdoc_1.45/lib ENABLE_VULKAN_RENDERDOC_CAPTURE=1 \
  ./Build/Linux_Release/Esoterica.Applications.Engine -map data://demo/render/pbr/pbrdemo.map -packaged
```

The ini key is `Enable_Render_Doc`. Nothing in the engine calls `TriggerCapture`, so a capture
needs the capture key or a temporary call to it.

> **Turn host validation off for any RenderDoc run.** With `Enable_Host_Validation = true` the
> engine segfaults about a second in, inside `librenderdoc.so`, at the `vkCmdPushDescriptorSetKHR`
> in `CmdSetRootConstants` (`RHI_Vulkan.cpp:3595`). It needs only the layer to be loaded, not any
> RenderDoc API call, so **it is not a port defect** - it is the two layers together. Use
> RenderDoc's own `--opt-api-validation` if you want validation inside a capture.

**There is no `xdotool` on either machine**, so the capture key cannot be pressed from a script on
an `Xvfb` display. The 2026-09-01 capture used a temporary `TriggerCapture()` in `QueuePresent`
gated on a present counter, reverted afterwards. Installing `xdotool` would remove the need for
that, and would also let the editor's title bar buttons and menus be driven from a script in P7.6.

**A capture can be read without the GUI.** `renderdoccmd convert -f cap.rdc -o cap.zip.xml -c
zip.xml` writes the whole chunk list as XML plus a sibling `.zip` holding every CPU-supplied
buffer. That is how the push constants, the command signature, the pipeline state, the execution
modes and the shader SPIR-V were all checked here. What it cannot give is anything the **GPU**
wrote during the frame - those need a replay, or a readback.

**Readback recipe**, which answered more questions than the capture did: create a
`ResourceMemoryType::DeviceToHost` buffer with `PersistentMap`, `RHI::CmdCopyBuffer` into it, and
read `m_pMappedAddress_WriteCombined` a few hundred frames later. For a texture, a temporary
`vkCmdCopyImageToBuffer` plus the correct `oldLayout` works the same way.

### Still open

- **`PrimitiveOutput` cannot carry `PerPrimitiveEXT`, and should.** `DebugDrawPrimitiveOutput` is
  fixed; `PrimitiveOutput` in `RendererTypes.esh` - the material shaders and `DebugDrawMesh.esf` -
  is not. Any `vk::ext_decorate` in that struct makes DXC flatten it per AST field in the pixel
  shader, and `MaterialShaderInput::New` copies the whole struct into a local, so DXC rebuilds
  `PrimitiveOutputFlags` with one constituent per bitfield into the single member those bitfields
  lower to. spirv-val rejects the module outright, so it cannot ship half-done by accident. Two
  ways out, neither cheap: give `PrimitiveOutput` a packed `uint` and accessors instead of a
  nested bitfield struct, which is an upstream change that reaches Direct3D 12; or a fourth entry
  in `Code/Scripts/DXCPatches`. **Not urgent** - NVIDIA renders correctly without it - but it is a
  real conformance gap and another driver need not be so forgiving.
- **The same query-as-enable-request pattern** used for the mesh shader features is still in place
  for the shading rate, acceleration structure and ray tracing blocks (`RHI_Vulkan.cpp:1157-1232`).
  Confirmed still present on 2026-09-01, and confirmed that **no VUID fires** for it on the RTX
  3090 with driver 580.173.02 and host validation on.
- **OIT is dead code, on both backends.** `OITResolve.esf` compiles, and nothing looks it up or
  builds a pipeline from it. `OIT.esh` has no consumers at all. Phase 5 criterion 8 asks for OIT
  parity and there is no OIT to have parity with.
- ~~**The Shipping configuration does not link on the RTX 3090 machine.**~~ **Fixed by P8.6 on
  2026-09-03, and it was never a dependency gap.** Shipping compiles with `-flto`, so the linker
  has to read LLVM bitcode; GNU ld can only do that through `LLVMgold.so`, which the official LLVM
  release archives do not ship. Whether the link worked came down to whether a distro LLVM happened
  to be installed - and that plugin would have been a different major version to the pinned clang.
  `Toolchain.py` now gives Shipping the same `ld.lld` the Reflector already used; it reads bitcode
  natively and comes from the same archive as the compiler. **Shipping now links and runs on this
  machine**, and the binary reports `Linker: LLD 21.1.8`.
- **The non-rpmalloc allocator path does not build on Linux.** `Memory.cpp` calls
  `_aligned_malloc`, `_aligned_realloc` and `_aligned_free` in the `#else` of
  `EE_USE_CUSTOM_ALLOCATOR`, with no `_WIN32` guard - the same shape as the `VirtualMemory*`
  functions Phase 1 found. It costs nothing today, because `Memory.h:13` hardcodes the define to 1
  and nothing turns it off. **It costs AddressSanitizer almost everything**: rpmalloc takes its
  memory from `VirtualMemoryReserve`, so ASan never sees an engine allocation. Found by P8.6 while
  trying to widen ASan's coverage; deliberately not ported, because it is an upstream file and a
  path nothing uses by default.

---

## Deferred on purpose

**Known shortcuts, chosen rather than missed.** Priorities were set explicitly on 2026-08-31:
blockers before correctness, because the port could not draw anything at all. Each of these is
correct-enough to keep going and wrong enough to sweep before the port is called done.

**Do not rediscover these. Check here first when something looks wrong.**

| What | Where | Why it was deferred | What it costs |
|---|---|---|---|
| ~~The cluster culling argument buffer is never cleared~~ **Fixed.** The editor passes `maxNumCommands` = 146 where the engine passes 1, so 145 dispatches a frame read stale argument slots. Real, and **not** the P7.6 editor hang, which was the `EE_IndirectRoot` inheritance below | `Renderer_ForwardShading.cpp:732` | Was deferred as "one line in an upstream engine file", on the assumption that `maxNumCommands` stays 1 | Nothing now. See the P7.6 deadlock entry below |
| ~~Attachment transitions use `ALL_COMMANDS` and all-access masks~~ **Decided by P8.4: permanent.** | `TransitionAttachmentIfNeeded`, `RHI_Vulkan.cpp` | Nothing at the call site says what last touched the image or what the pass will do to it | Over-synchronisation. Correct, slow. It moved to the [`ALL_COMMANDS` sites](#all_commands-sites) table with the other six, and the reasoning for all seven is there |
| Mesh draws are dropped instead of halting | `CmdSetPipeline`, `RHI_Vulkan.cpp` | No GPU here has `VK_EXT_mesh_shader`, and halting stopped the frame before anything else could be exercised | **A frame missing its geometry is not a rendered frame.** It warns once. On hardware with mesh shaders the branch never runs |
| A pixel shader in an indirect draw reads command 0's root **constants** | `EE_INDIRECT_PIXEL_ENTRY_INIT`, `RHI.esh` | There is no `DrawIndex` in a fragment shader, and carrying the command index across the stage boundary means a new per-primitive attribute in upstream structs | Nothing today. Exact for the root **CBV**, because `BucketResolve.esf:41` writes the same address into every command. The per-command root constants - `m_clusterOffset`, `m_renderViewIndex`, `m_clusterVisibleBuffer` - would be command 0's, and no pixel shader reads one. **A pixel shader that starts reading one gets silently wrong data past the first command** |
| An indirect `RootSRV` cannot be read | `CreateCommandSignature`, `RHI_Vulkan.cpp` | Only `RootConstants` and `RootCBV` have indirect declarations. `DebugDrawMesh.esf` declares a `RootSRV`, so a signature can carry one | Nothing indexes it yet. A shader that did would need a third declaration macro |
| ~~The `GPU hang` after a full frame~~ **Fixed.** It was `vkCmdSetFragmentShadingRateKHR` on a transfer command buffer, not the dropped mesh draws. See the 2026-08-31 NVIDIA entry | `BeginCommandBuffer`, `RHI_Vulkan.cpp` | Was recorded as unresolvable without mesh hardware. That was wrong: the kernel named it as `Xid 32` throughout, and a GPU with dedicated queue families made it reproducible | Nothing now. The frame loop runs and shuts down clean |

**Five upstream shader files are unverified on Windows.** `RHI.esh` and the four shaders changed
by open question 8 and P5.17 compile for both platforms and have only ever been built on Linux.
They are guarded by `#ifdef __spirv__`, so the Direct3D path should be untouched - "should" is
the word that needs a Windows build. This is the highest-risk item in the port right now, and it
needs a **Windows machine**, not new GPU hardware. The two waits are different.

---

## In flight

> ### **Phase 7 is done. Phase 8 is what is left.**
>
> | Task | State |
> |---|---|
> | P7.0 - P7.5 | merged (#68, #69 and #70 landed on 2026-09-01; P7.5's fifth link closed on 2026-09-02) |
> | P7.6 editor shakedown | **done**, 2026-09-02, PR #79. Criteria 5, 6, 8, 9 and 10 met; two defects found and fixed |
>
> Four things remain from Phase 7 and all four are in [Blocked.md](Blocked.md): minimize and
> maximize need a window manager that implements them, the ragdoll editor needs a skeletal asset
> the dataset does not have, seven `OpenInExplorer` call sites are unexercised, and the Test
> Compile panel overlap is an upstream bug to report rather than fix.
>
> ### **[Phase 8](Phases/Phase8-Completion.md), added 2026-09-02.**
>
> | Task | State |
> |---|---|
> | P8.1 The Windows build | not started. **The largest unmeasured risk in the port** |
> | P8.2 Runtime shakedown | **mostly done**, 2026-09-02. Game preview runs, physics simulates, all three animation editors open. Gamepad camera control still needs a controller |
> | P8.3 Raytracing, or the decision not to | **deferred**, 2026-09-03, by the developer. Still open - a deferral is not one of the two outcomes the task asks for |
> | P8.4 RHI debt sweep | **done**, 2026-09-03. Phase 5 criteria 1, 8 and 9 closed. Found an upstream translate-gizmo crash |
> | P8.5 Shader conformance | not started |
> | P8.6 Sanitizers and build coverage | **done**, 2026-09-03. Nine configurations build; Shipping links; TSan blocked on the NVIDIA driver |
> | P8.7 Fork review | not started. Do this last |
>
> **The original plan ended at Phase 7 and was right that the port would work by then.** What it
> had no place for was the work left over *after* the thing works: verification that no machine
> could do at the time, shortcuts taken deliberately, and the question of what this fork is.

---

**Phase 5 is merged, has run, and the frame it produces is correct.** All seventeen groups went
in through PRs #24 to #41 and then P5.17 to P5.20. **One group has still never executed - P5.16
raytracing.** P5.12 and P5.15 were verified on 2026-09-01 from a frame capture and P5.11 from a
temporary in-engine harness; every other group's "Not verified" list is historical, because the
correct frame exercised P5.1 to P5.10, P5.13 and P5.17. [Blocked.md](Blocked.md) is the list that
matters now.

---

## `ALL_COMMANDS` sites

Phase 5's "do not" list says to record every temporary `ALL_COMMANDS` barrier rather than leave it
to be found later. All seven sites are in `RHI_Vulkan.cpp`.

> ### **P8.4 swept these on 2026-09-03. All seven are permanent, for two different reasons.**
>
> **Three are not debt at all.** `QueueSubmit`'s signal mask, `PipelineStage::All` and
> `RecordQueueOrderingWait` mean "everything", and `ALL_COMMANDS` is the Vulkan spelling of that.
> Narrowing them would make them wrong.
>
> **Four are correct-but-slow, and narrowing every one of them needs information the RHI
> abstraction does not carry.** The call site cannot say what last touched a resource or what the
> next pass will do to it, because `RHI.h` has nowhere to put that. Adding it is a change to a
> shared header that reaches Direct3D 12, which Phase 5 criterion 2 protects and which is
> upstream's side of the line - see [Escalation triggers](00-Conventions.md#escalation-triggers).
>
> **There is also no measured reason to.** No frame here is GPU-bound, no profile names a barrier
> stall, and **over-synchronising is the safe direction**: a barrier that is too wide costs time,
> and one that is too narrow is a corruption bug that appears on somebody else's driver. Trading a
> known cost for an unknown risk with no measurement is a bad trade.
>
> **What would reopen this:** a profile that names one of these barriers. Then narrow *that* one,
> against a captured frame, and leave the rest.

| Site | Why it is there | What narrowing it needs | P8.4 decision |
|---|---|---|---|
| `QueueDeviceWait`, wait `stageMask` (P5.2) | `ID3D12CommandQueue::Wait` blocks the whole queue, and this has to mean the same thing. | Knowledge of what the waiting submit does, which the caller does not pass. | **Permanent.** Needs an `RHI.h` change to carry what the waiting submit does. Reaches Direct3D 12 |
| `QueueSubmit`, signal `stageMask` (P5.2) | The signalled timeline value has to mean "everything in this submit finished". | Nothing. `ALL_COMMANDS` is arguably correct here rather than lazy, since that is the semantic. | **Permanent, and not debt.** It is the semantic |
| `VulkanPipelineStage`, `PipelineStage::All` (P5.9) | `D3D12_BARRIER_SYNC_ALL` means every stage, and so does this. The reference returns it the same way, as an early return rather than one bit among many. | Nothing. It is the meaning of the flag. | **Permanent, and not debt.** It is the meaning of the flag |
| `VulkanAccess`, `ResourceAccess::Common` (P5.9) | `D3D12_BARRIER_ACCESS_COMMON` is "any access", which Vulkan spells `MEMORY_READ` plus `MEMORY_WRITE`. `DeviceTextureState` starts every texture at `Common`, so this is the source mask of the first barrier on any texture. | The engine would have to say what it actually did, which the tracker does not record. Narrowing it is a change on the engine side, not here. | **Permanent.** Needs the engine to record what it actually did. Engine-side, not RHI-side |
| `RecordQueueOrderingWait`, both masks (P5.3) | A Direct3D 12 queue runs its command lists in submission order and a Vulkan queue does not, so every submit waits on the previous submit's timeline value. "The previous submit finished" is the whole meaning of the wait. | Nothing. It is the semantic, the way `QueueSubmit`'s signal mask is. | **Permanent, and not debt.** It is the semantic |
| `RecordClearVisibilityBarrier`, destination masks (P5.10) | A clear has to be visible to whatever reads it next, and the engine's own barrier after a clear names a shader write as the source, which does not cover a Vulkan transfer write. Nothing at the call site says who the reader is. | The reader. In practice it is a compute dispatch, an indirect argument fetch or a copy to a host buffer, and naming those three would narrow it. Confirm against a captured frame first. | **Permanent for now.** The only one narrowable without an API change, and nothing measured says it is worth the risk |
| `TransitionAttachmentIfNeeded`, both masks and all-access (P5.9) | An attachment has to be in the right layout before a pass uses it, and nothing at the call site says what last touched the image or what the pass will do to it. | The same information the other four want. Moved here from [Deferred on purpose](#deferred-on-purpose) by P8.4, because it is the same decision. | **Permanent.** Needs an `RHI.h` change. Reaches Direct3D 12 |

---

## Completed work

<!--
Append one entry per completed task, newest first. Format:

### YYYY-MM-DD - P<phase>.<task> <short title>
- What you did, concretely.
- Files added: ...
- Upstream files edited: ... (must match TouchedFiles.md)
- Acceptance criteria met: which ones, and which not.
- Anything the next agent needs to know.
-->

### 2026-09-03 - P8.6 Sanitizers and build coverage. **All nine configurations now build**, Shipping links, and TSan found a real defect in the crash handler

**Every one of the nine configurations has now been built and the engine run in five of them.**
Before this task, six had never produced an output directory and Shipping had never linked here.

| Configuration | Before | Now |
|---|---|---|
| `Linux_Release` | built and run | unchanged |
| `Linux_Debug` | **never built on this machine** | builds, runs pbrdemo for 60s, no asserts, no leaks |
| `Linux_Shipping` | **compiled, never linked** | links with `ld.lld`, runs pbrdemo, no leaks |
| `Linux_Release_ASan` | **never built** | builds, runs, **zero errors** |
| `Linux_Release_TSan` | **never built** | builds, runs, **one real defect found** |
| `Linux_Release_UBSan` | **never built** | builds, runs, **zero reports** |
| `Linux_Debug_{ASan,TSan,UBSan}` | never built | generate and are reachable; not built, and the Release ones are the useful pair |

#### Shipping was never a dependency gap - it was the wrong linker

The 2026-09-02 entry recorded Shipping's `LLVMgold.so: cannot open shared object file` as "a
dependency gap between the two development machines". **That was wrong, and the fix is one flag.**

Shipping compiles with `-flto`, so the linker has to read LLVM bitcode. **GNU ld can only do that
through `LLVMgold.so`, which the official LLVM release archives do not ship.** Whether the link
worked came down to whether a distro LLVM happened to be installed - and that plugin would have
been a different major version to the pinned clang, reading its bitcode.

`find_linker_flags` already picked `External/LLVM/bin/ld.lld` for the Reflector, for exactly the
same reason - the LLVM release archives hold bitcode, not ELF objects. `Toolchain.py` now applies
it to any configuration whose linker flags contain `-flto`, which is Shipping and nothing else.
`readelf -p .comment` on the result says `Linker: LLD 21.1.8`.

**This makes Shipping machine-independent**, which the old diagnosis would not have.

#### TSan found the one real defect: the crash handler was not signal-safe

`Platform_Linux.cpp`'s `CrashSignalHandler` is written to be async-signal-safe - `write()` rather
than `fputs`, `backtrace_symbols_fd` rather than `backtrace_symbols`, with a comment saying so.
**`backtrace()` itself is the hole.** glibc loads the unwinder from `libgcc_s.so` lazily, so the
*first* call reaches `dlopen` and mallocs:

```
WARNING: ThreadSanitizer: signal-unsafe call inside of a signal
    #0 malloc
    #1 malloc                          ld-linux-x86-64.so.2
    #2 _dl_map_object_deps             elf/dl-deps.c:463
    #3 EE::Platform::CrashSignalHandler  Code/Base/Platform/Platform_Linux.cpp:54
```

**A crash inside the allocator would then deadlock in the handler reporting it** - which is the
case the handler exists for. `Platform::Initialize` now calls `backtrace()` once on the main
thread with nothing held, so the library is already loaded when a signal arrives. Confirmed fixed:
the warning is gone from a re-run.

This is the port's own file, so it is fixed here rather than recorded.

#### What each sanitizer could actually see

**Read this before trusting "no errors found".**

- **ASan: zero errors over 45 seconds**, rendering pbrdemo correctly. Six leaks at exit, all in
  `libdbus` and the driver, none in `Code/`. **But ASan sees almost none of this engine.**
  `Memory.h:13` hardcodes `EE_USE_CUSTOM_ALLOCATOR` to 1, so every engine allocation comes from
  rpmalloc, which takes memory from `VirtualMemoryReserve` - an `mmap` ASan does not intercept.
  What ASan covered here is stack, globals and libc-level heap. Turning rpmalloc off does not
  compile on Linux; see [Still open](#still-open).
- **UBSan: zero reports** over 60 seconds. This one is not narrowed by the allocator, so it is the
  cleanest result of the three.
- **TSan: cannot run against the NVIDIA driver at all.** `vkCreateDevice` is followed immediately
  by an internal TSan failure:
  ```
  ThreadSanitizer: CHECK failed: tsan_interceptors_posix.cpp:2156 "((thr->slot)) != (0)"
  ```
  `ignore_noninstrumented_modules`, `ignore_interceptors_accesses`, `detect_deadlocks=0`,
  `handle_segv=0` and `handle_sigbus=0` were each tried and none helps. **This is the driver, not
  the engine or TSan** - proved by swapping in Mesa's software ICD, where the same binary runs.

#### How to run TSan here, given that

Two routes, both used in this session:

```bash
# 1. The engine, on the software ICD. Reaches the render path but not far - lavapipe has no
#    VK_EXT_mesh_shader, so the frame halts the way any device without it does.
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./Esoterica.Applications.Engine -packaged -map data://demo/render/pbr/pbrdemo.map

# 2. The resource compiler, which is threaded and touches no Vulkan at all.
./Esoterica.Applications.ResourceCompiler -compile data://demo/render/pbr/boulder/boulder.mesh -force
```

Route 1 reported two data races, **both entirely inside `libvulkan_lvp.so`** between its own
`llvmpipe-N` worker threads; the only `Code/` frames are the stack that initialised the driver.
Route 2 compiled the mesh with **zero races**.

**So the engine's own threading is still largely uncovered by TSan.** The task system under a real
frame is the interesting target and it is out of reach on this machine. That is the honest state,
and it is a row in [Blocked.md](Blocked.md) rather than a clean result.

#### ASan needs one option, or Vulkan will not start

```bash
ASAN_OPTIONS=protect_shadow_gap=0 ./Esoterica.Applications.Engine ...
```

Without it `vkCreateDevice` fails and the engine halts on `EE_ASSERT( result == VK_SUCCESS )` at
`RHI_Vulkan.cpp:1288`. The NVIDIA driver maps memory in the region ASan reserves as its shadow gap.
**The symptom names Vulkan and the cause is the sanitizer**, which is worth an hour to whoever
meets it next; it is in [04-BuildAndRun.md](04-BuildAndRun.md).

#### The CI decision: no CI, and the reason is a number

**Decided with the developer on 2026-09-03: no CI.** Not a judgement about whether CI is worth
having - a measurement:

- **`External/` is 12 GB, of which 11 GB is the extracted LLVM release.**
- GitHub Actions caps a repository's cache at **10 GB**, so `External/` cannot be cached at all.
- A hosted runner would therefore re-download and re-extract LLVM on **every run**, before the
  first translation unit compiles, and it is doubtful it fits in a standard runner's free disk.

Two things would change the answer, and both are real work rather than a workflow file: **a
prebuilt container image with `External/` baked in**, or **trimming the LLVM install** to the
toolchain plus the clang libraries the Reflector links against. A self-hosted runner on either
development machine was also considered and declined, because CI that only passes when a
particular desktop is switched on is worse than none.

**The consequence is accepted and written down here:** nine configurations that nothing builds
automatically will rot, and the guard against that is that every task builds what it touches.
P8.6 is the evidence for how fast it happens - six configurations had never been built at all.

- Files added: none.
- Upstream files edited: `Code/Scripts/NinjaGen/Toolchain.py` and `NinjaGen.py`, both already
  registered as this port's own rewrite. `Code/Base/Platform/Platform_Linux.cpp` is a port file.
  **No shared C++ header or engine source changed, so Windows sees none of this.**
- Acceptance criteria met: P8.6's "ASan and TSan builds of the engine have been run against
  pbrdemo with their findings recorded" - **met, with the TSan caveat above stated rather than
  glossed**; "Shipping links on both machines" - **met on this one and made
  machine-independent**; "the CI decision is written down" - met.

### 2026-09-03 - P8.4 RHI debt sweep. **Mesh picking works**, the barrier debt is permanent, and the translate gizmo has an upstream crash

**This task was four decisions and one measurement.** The measurement passed, the four decisions
are recorded below, and **Phase 5's criteria 1, 8 and 9 are now closed.** Only 7 and 11 are left
there, and both need a Windows machine.

**No code changed.** That is the expected shape of a debt sweep: the point was to decide, not to
edit.

#### Mesh picking is verified - Phase 5 criterion 8 is complete

Three clicks in the map editor viewport on `pbrdemo`, each selecting exactly what was under the
cursor, confirmed in both the Outliner highlight and the Inspector contents:

| Clicked on | Selected |
|---|---|
| The boulder | `Boulder`, with its Static Mesh Component |
| Sky, and the reflective ground | `Skydome`, with its Static Mesh and Directional Light Components |
| The floor | `Floor`, with its Static Mesh Component |

**The floor is the interesting one.** At the default camera it is edge-on, so it is about two
pixels tall on screen - selecting it from the Outliner draws its bounds as a single yellow line
across the viewport. Clicking that two-pixel sliver still returns `Floor`, which is a much better
test of the picking buffer than a large target would have been. `m_lastKnownPickingPixelRadius`
is 4, which is what makes it hittable.

**Do not go looking for the floor below the horizon.** The bright band across the middle of the
default view is the skydome's horizon, not the ground, and the large reflective blue area under it
is the skydome as well. Both return `Skydome`, correctly. Twenty minutes went into clicking those
two before selecting `Floor` in the Outliner showed where it actually was.

#### The barrier debt is permanent, and the reasoning is in one place now

The [`ALL_COMMANDS` sites](#all_commands-sites) table has a decision column, and the attachment
transitions moved into it from [Deferred on purpose](#deferred-on-purpose) because they are the
same decision. Seven sites, two reasons:

- **Three are not debt.** `QueueSubmit`'s signal mask, `PipelineStage::All` and
  `RecordQueueOrderingWait` mean "everything". `ALL_COMMANDS` is how Vulkan spells that.
- **Four are correct-but-slow, and narrowing them needs information `RHI.h` does not carry.**
  The call site cannot say what last touched a resource. Adding that is a shared-header change
  that reaches Direct3D 12, which criterion 2 protects and Conventions rule 3 puts on upstream's
  side of the line.

**And nothing measured says it is worth it.** No profile here names a barrier stall. Over-
synchronising costs time; under-synchronising is a corruption bug on somebody else's driver. A
profile naming one of these is what would reopen it.

#### The two `EE_UNIMPLEMENTED_FUNCTION` markers stay

Criterion 1 asked for zero. **Two remain, down from 103, and neither is an unimplemented
function** - each guards a branch inside a working function that nothing reaches. A sampler border
colour that is not transparent black, opaque black or opaque white would need
`VK_EXT_custom_border_color`; a static sampler would need the binding model to use one.

**Each names the caller that started needing the feature, the moment one appears.** Deleting them
trades a loud precise failure for a silent wrong result - the wrong border colour, or a sampler
quietly not bound. Neither can be implemented without a caller to test against, and an untested
implementation written to satisfy the wording of a criterion is worse than the marker. Criterion 1
is now recorded as met in substance and not to the letter, with that reasoning.

#### No in-engine RenderDoc trigger, for the same reason as raytracing

`BeginFrameCapture` and `EndFrameCapture` are implemented and work. They have **zero callers in
`Code/` on either backend** - `RHI_Direct3D12.cpp` implements them and nothing calls them there
either. **Having no trigger is parity**, and adding one is a feature upstream does not have, which
Phase 8's "do not" list forbids. RenderDoc must attach before `vkCreateInstance` anyway, so a
trigger would not save the relaunch. The capture key is the trigger; a temporary in-engine call is
the fallback when a specific frame has to be caught, which is what the 2026-09-01 capture did.
Host validation still has to be off, or the engine segfaults inside `librenderdoc.so`.

#### The find: the translate gizmo crashes when an axis lines up with the camera

Verifying picking crashed the editor twice before it succeeded once. It is **upstream, platform-
neutral, deterministic, and easy for a user to hit** - the default gizmo mode, an ordinary camera
angle, and any selected entity. Written up under
[Upstream issues observed](#upstream-issues-observed) with a gdb backtrace and a reproduction.
**Not fixed here.**

Two things about finding it that are worth reusing:

- **`gdb` cannot attach on this machine** - `ptrace_scope` refuses, and `apport` swallows the core
  dump, so `Trace/breakpoint trap (core dumped)` leaves nothing behind. **Launching the editor
  under `gdb --batch` works**, and `-ex "handle SIGTRAP stop nopass" -ex run -ex "bt 15"` turns an
  assert into a backtrace. Symbol loading takes a couple of minutes; run it in the background and
  drive the repro with `xdotool` once the window appears.
- **A failed reproduction is not a disproof.** The deliberate attempt - pitch the camera straight
  down with an entity selected - did not fire, because it went *past* the degenerate angle rather
  than through it. Replaying the original sequence verbatim reproduced it first try. Replay before
  concluding it is intermittent.

- Files added: none.
- Upstream files edited: none. **No code changed at all.**
- Docs: Phase 5 criteria 1, 8 and 9 closed; the `ALL_COMMANDS` table gained a decision column and a
  seventh row; the attachment-transition entry in Deferred on purpose points at it; Blocked.md's
  mesh picking row is deleted.
- Acceptance criteria met: Phase 8 criterion 7 (mesh picking verified from an editor viewport), and
  P8.4's own "every item is either fixed or has a recorded decision".

### 2026-09-02 - P8.2 Runtime shakedown. **The engine simulates**, and an unaligned `mprotect` was silently doing nothing

**"Play Map" has now been pressed, and a physics body has been seen to move.** Both were the
largest functional unknowns left after Windows. The session also found and fixed **a real Linux
port bug that crashed the editor the first time a skeletal mesh was ever loaded**.

#### What was verified

- **Game preview starts and stops cleanly.** Three Play Map / Stop Playing cycles in a row, the
  editor alive after each, and no new errors in the log beyond the documented startup
  `Connection refused` retries.
- **Physics runs.** Three dynamic sphere bodies dropped from 25 m, 35 m and 45 m onto
  `Floor.physmesh`, collided with each other, settled at a resting centre height of 1.486 m for a
  1.5 m radius, and went to sleep. Both the engine and the editor's game preview do it.
- **Camera control works from keyboard and mouse** in a running engine. Hold right mouse in the
  viewport, then `W` moves and mouse delta looks - `Component_ToolsCamera.cpp`. This closes the
  keyboard and mouse halves of Phase 6 criterion 3. **Gamepad is still not verified**: no
  controller is attached to this machine, and the only `*-event-joystick` node is the Keychron
  keyboard's HID artifact. That is a new row in [Blocked.md](Blocked.md).
- **A skeletal asset is imported and all three editors open with it.** The skeleton editor shows an
  8-bone hierarchy with its animation clip and preview mesh, the animation graph editor builds a
  default root graph with graph view, variation editor and debugger, and **the ragdoll editor opens
  with the character skeleton and renders its skinned preview mesh**. The ragdoll editor was a row
  in Blocked.md for exactly this reason; it is deleted.

#### The port bug: `mprotect` cannot take an unaligned address, and nobody checked

Opening the skeleton editor **segfaulted the whole editor**:

```
Caught signal: Segmentation fault
EE::Memory::CopyToWriteCombined
EE::Render::RenderSystem::QueueMeshUpdate
EE::Render::RenderSystem::StartResourceUpdates
EE::Engine::Update
```

Not an alignment assert - asserts are live in Release and none fired. `PageAllocator::Commit`
(`Code/Base/Render/PageAllocator.h:38`) grows the allocator with

```cpp
Memory::VirtualMemoryCommit( m_pPageMemory + m_pageMemoryComitted, requiredMemoryComitted - m_pageMemoryComitted );
```

so the start address is `base + committedElements`, which is page-aligned only by accident.
`VirtualAlloc( MEM_COMMIT )` commits every page the range touches and does not care.
**`mprotect` does care: an unaligned address fails with `EINVAL` and the pages stay `PROT_NONE`.**
The return value was never checked, so the commit silently did nothing and the next read faulted.

Measured, rather than assumed, with a temporary log in `VirtualMemoryCommit`:

```
mprotect ptr=0x7f6ab2000000 size=2048 pageOffset=0    result=0  errno=0
mprotect ptr=0x7f6ab2000800 size=2048 pageOffset=2048 result=-1 errno=22
```

**This is the port's own bug, in the port's own `#else` branch** - `Memory.cpp` has been in
[TouchedFiles.md](TouchedFiles.md) since Phase 1. The fix rounds the range out to page boundaries
to match `VirtualAlloc`, and asserts on the result so the next one is loud. The skeleton editor,
the animation graph editor and the ragdoll editor then all open, and the ragdoll editor renders a
skinned mesh.

**Why it hid for eight phases:** it only bites when an allocator grows across a page boundary from
an unaligned offset. The initial commits are 2048 bytes at a page-aligned base, and Linux rounds
*length* up to a page, so the first growth landed inside a page that was already mapped. **A
skeletal mesh was the first thing to push it past one.** Anything that grows a `PageAllocator`
further can hit it, so this was not a skeletal-mesh bug.

#### Test content is fetched, not committed

`Data/` ships no skeleton, no animation and no non-skeletal-only meshes, and `PBRDemo.map` has no
physics components at all. Both gaps are closed by
[`Docs/Linux/Scripts/FetchTestAssets.sh`](Scripts/FetchTestAssets.sh), which writes everything into
`Data/PortTests/` - a **gitignored** directory. No third-party asset enters the fork's history, and
Conventions rule 6 still holds for every tracked file in `Data/`.

Validate what it writes without a GUI:

```bash
cd Build/Linux_Release
./Esoterica.Applications.ResourceCompiler -compile data://porttests/rig/rig.skeleton
```

**The empty `.ag` and `.ragdoll` it writes deliberately fail to compile.** They are authoring
files: a graph with no nodes and a ragdoll with no bodies are exactly what the editors exist to
fill in, and both editors open them and say so.

#### glTF skeletal import does not work, and it is upstream's

The Khronos glTF sample assets would have been the tidier source - CC BY 4.0, stable URLs, no FBX.
**Fox, RiggedFigure, RiggedSimple and CesiumMan all fail**, so the script uses assimp's
BSD-licensed FBX test models through ufbx instead. Recorded under
[Upstream issues observed](#upstream-issues-observed); platform-neutral, so Windows has it too.

#### Three traps that cost time and are not port defects

- **`pkill -f "Esoterica.Applications"` kills the calling shell.** An agent's shell has the pattern
  in its own command line, so `pkill -f` matches it. This killed two sessions outright, with only
  an exit code to show for it. There was no `KillEsotericaProcesses.sh`, only the `.bat`; there is
  now, and it matches on `/proc/<pid>/exe` the way the `.bat` matches on image name. It also sends
  `SIGKILL`, matching `taskkill /F` - **`SIGTERM` is ignored while an application is showing a
  modal error dialog**.
- **An error dialog can hold port 5556 on its own.** After a failed start, `zenity` had inherited
  the Resource Server's listening socket and kept it after every Esoterica process was gone. The
  documented orphaned-worker symptom, from a process nothing would think to look for.
  `KillEsotericaProcesses.sh` now names whoever still holds the port.
- **The Resource Server must stay on a visible workspace.** An i3 `assign` rule to park it out of
  the way leaves its window unmapped, and an unmapped Resource Server **stops servicing requests**:
  it still binds and listens, then every request times out and the application dies with
  `Failed to load required engine module resources`. Ask for a wider virtual screen instead.

#### The virtual display is now a script

Previous sessions set up `Xvfb` plus a minimal `i3` by hand each time.
[`Docs/Linux/Scripts/VirtualDisplay.sh`](Scripts/VirtualDisplay.sh) is that, with `start`, `stop`,
`shot`, `status` and `run`. It writes its own i3 config rather than using the developer's, which
starts `xss-lock` - a lock screen inside the virtual display would replace every screenshot with
i3lock's image. NVIDIA presents to Xvfb, as before; `vkcube` reports the RTX 3090 there.

`ffmpeg -f x11grab` replaces `maim`, which is not installed on this machine. Recording a video and
tiling frames with `-vf "fps=1,tile=5x4"` reads motion far better than a series of stills.

#### One dead end worth not repeating

**The first physics map simulated correctly and looked completely static.** A boulder at mesh scale
6 dropped over the origin lands *around* the default camera, so every frame was the inside of a
rock. Twenty minutes went into reading the box3d integration for a bug that was not there. The map
the script writes now puts three smaller boulders off to the sides at three heights.

- Files added: `Docs/Linux/Scripts/VirtualDisplay.sh`, `Docs/Linux/Scripts/FetchTestAssets.sh`,
  `KillEsotericaProcesses.sh`.
- Upstream files edited: `Code/Base/Memory/Memory.cpp` (already in TouchedFiles.md; row updated).
  `.gitignore` gains `/Data/PortTests/`.
- Acceptance criteria met: Phase 8 criterion 4 (game preview runs, a physics body moves),
  criterion 5 (skeletal asset imported, animation graph editor and ragdoll editor open with it),
  and criterion 6 **partly** - keyboard and mouse yes, gamepad not verified.
- Built and run in `Linux_Release`. `Build/Linux_Debug/libEsoterica.Base.so` also links, which is
  the configuration where the new assert is live. **A full Debug build was not attempted** and
  **no Windows build was run** - the change is inside the existing `#else`, so Direct3D 12 sees
  none of it.

### 2026-09-02 - Dependencies. `DownloadDependencies.sh` pins the `protoc` that matches libprotobuf

**A clean-checkout run died, and it was the last row of [Blocked.md](Blocked.md).** After
`git clean -xdf`, `./DownloadDependencies.sh` failed building GameNetworkingSockets with
`#error This file was generated by an older version of protoc`, followed by a missing
`google/protobuf/generated_message_table_driven.h`. That row is now closed.

**The cause is a split between two protobuf installs, not a stale tree.** CMake's `FindProtobuf`
resolves the **library** from the standard prefix but **protoc** from `PATH`. On this machine a
`~/bin/protoc` 3.17.3 shadowed the system 3.21.12, so protoc generated `.pb.h` files that include
`generated_message_table_driven.h` - a header protobuf deleted in 3.21 - and they were then
compiled against 3.21 headers. Neither half is wrong on its own; only the pairing is.

**The fix resolves the compiler instead of checking its version.** The protoc that matches the
library is the one in the same install prefix, so a new `protobuf_protoc` asks
`pkg-config --variable=exec_prefix protobuf` and `fetch_gamenetworkingsockets` passes the result
to CMake as `-DProtobuf_PROTOC_EXECUTABLE`. `PATH` order stops mattering. An `info` line names the
compiler whenever it is not the one on `PATH`, so the override is never silent.

**A version check was considered and rejected.** It would have mirrored the Rust MSRV check in
`fetch_ctt`, but it only ever produces a better error message and still leaves the reader to fix
their own `PATH`. It is also fragile going forward: protobuf's C++ runtime and its compiler use
different version numbers above 3.21, where protoc 25.1 pairs with libprotobuf 4.25.1, so
comparing the two strings needs arithmetic that a newer distro would break.

**`requirements_gamenetworkingsockets` now checks the resolved binary**, not `command -v protoc`,
through a new `require_executable` helper next to `require_header`. A machine with only a stale
`protoc` and no `protobuf-compiler` package is now reported in the script's single batched
"missing system packages" message rather than failing later.

- Files edited: `DownloadDependencies.sh`. No upstream files, so no `TouchedFiles.md` change - it
  is already registered there as a port-owned file with 0 upstream lines modified.
- Docs: this entry, the Blocked.md row deleted, and the `PATH=/usr/bin:$PATH` workaround in
  [04-BuildAndRun.md](04-BuildAndRun.md) replaced with a note that it is no longer needed.
- **[04-BuildAndRun.md](04-BuildAndRun.md)'s package list was telling the wrong story**, found
  while making the change above. It headed eight packages "Packages it does not check for", and
  the script checks five of them: `libprotobuf-dev` and `rustup` always did, `libxss-dev` and
  `libxtst-dev` since PR #60, and `protobuf-compiler` as of this change. Only three are genuinely
  unchecked, for two different reasons now written down - `libsqlite3-dev` is needed by the
  *engine* build (`ResourceCompilationDatabase.cpp` includes `sqlite3.h`) and the script only ever
  builds `External/`, while `vulkan-tools` and `vulkan-validationlayers` are not build
  dependencies at all. The list is now per-package and says which are checked.
- Verified by rebuilding GameNetworkingSockets **with the stale 3.17.3 still first on `PATH`**:
  69/69 objects, both `.pb.cc` files compiled, `libGameNetworkingSockets.so` installed, exit 0.
  The `.pb.cc` files compiling is the proof - 3.17 output cannot build against 3.21 headers.

---

### 2026-09-02 - Docs. Phase 8 exists, and the port's remaining work is one list instead of five

**A survey of the whole port, and the documents brought up to what it found.** No code changed.
The survey read every phase document, `Blocked.md` and `TouchedFiles.md`, and then checked each
claim against the tree rather than believing it. Three findings changed how much work is left, and
in one case which direction.

#### There is almost no unported code

The remaining-work list is long and reads worse than it is, so this is worth stating first. **Every
`*_Win32.*` file has a `_Linux` sibling** - 27 of them, checked by basename across `Code/`. And
`Exclusions.txt` drops **five** things: the Direct3D 12 backend, `D3D12MemoryAllocator`, three
Windows entry points, and the Windows-only BuildGenerator. **No engine subsystem is excluded.**
Physics, animation, the entity system, navmesh and networking all compile and ship on Linux.

Two things that look like gaps and are not: **NavPower is off upstream too**
(`NAVPOWER_INCLUDED=False` in `Code/PropertySheets/NavPower.props`), so navmesh generation is
parity rather than a Linux hole; and **there is no audio module** in the engine at all.

#### Raytracing is smaller than every document said, and in a way that changes the plan

`Blocked.md` called P5.16 "the largest thing left". It is not. `RHI_Vulkan.cpp` **implements** it -
acceleration structures, raytracing pipelines, shader binding tables, `vkCmdTraceRays` and
`vkCmdTraceRaysIndirect2`, all entry points resolved.

**`CreateAccelerationStructure`, `CmdDispatchRays` and `RaytracingShaderTable` have zero callers**
across `Code/Engine`, `Code/EngineTools` and `Code/Game`, and the tree contains no raytracing
shaders. It is dead code on Direct3D 12 for the same reason. So it cannot be verified by running
the engine, and the honest options are a scratch harness or a formal decision not to - which is
what [P8.3](Phases/Phase8-Completion.md#p83---raytracing-or-the-decision-not-to) now says. It is
the same shape as OIT, which Phase 5 already closed as "cannot be met".

#### The engine has rendered. It has never simulated

Every measurement in this port is a static scene held for about thirty seconds. **"Play Map" does
not appear anywhere in this file**, and `MapEditor.cpp:353` is where it lives. No physics body has
been seen to move, no animation graph has been evaluated, and Phase 6 criterion 3's "works for
camera control" has never been checked in a running engine.

Animation is blocked by data before code: `Data/` has **no `.skel`, no `.anim` and no `.ag`**, and
the only FBXs are non-skeletal. Importing one skeletal asset unblocks both the animation runtime
and the ragdoll editor, which is a `Blocked.md` row for exactly that reason.
[P8.2](Phases/Phase8-Completion.md#p82---runtime-shakedown) is now the second-largest item in the
port.

#### What was corrected

- **Five `TouchedFiles.md` rows were still `planned`** that P5.17 completed. Verified against the
  tree - `RHI.esh` has 4 indirect-macro sites and the four shaders have 5 to 6 each - and set to
  `done`. The registry is what makes the post-merge audit in
  [01-UpstreamMerges.md](01-UpstreamMerges.md) mechanical, so a stale row there is a defect in the
  merge procedure, not a documentation nit. Its "watch for one more" note was stale too:
  `Renderer_ForwardShading.cpp` did need the clear, and is now registered.
- **Phase 5 criterion 8 said debug draw "settles in the editor, under P7.6".** P7.6 settled it on
  2026-09-01 - the yellow selection bounds - so it is five of seven now, not four. **Mesh picking
  is still not verified**; P7.6 opened the viewport but did not check picking, and it is owed to
  P8.4.
- **Open question 2** (which LLVM the Reflector needs) was still `open` though answered every
  session since Phase 2. Closed: clang 21.1.8.
- **Phase 7 said "this is the last planned phase".** It no longer is.

#### The sanitizer configurations have never been built

`Toolchain.py` generates nine configurations and the ninja file carries all nine, including
`Linux_{Debug,Release}_{ASan,TSan,UBSan}`. **No sanitizer output directory has ever existed**, and
`Linux_Debug` has never been built on the RTX 3090 machine either. On an engine with a task system,
TSan is the interesting one. [P8.6](Phases/Phase8-Completion.md#p86---sanitizers-and-build-coverage).

#### Phase 8, and why it is a phase rather than a list

The remaining work did not fit the documents that existed. `Blocked.md` is strictly *"written but
not verified, indexed by the machine that unblocks it"* and that is what makes it usable at a given
desk - but half of what is left is blocked on nothing at all. Folding sanitizers and doc drift into
it would have cost the property that makes it work.

So [Phase8-Completion.md](Phases/Phase8-Completion.md) collects it, in the same task-and-criteria
shape as every other phase, ordered by size and risk rather than dependency. `Blocked.md` stays
machine-indexed and unchanged. **P8.7 is new work rather than carried-over work**: a fork review
that measures how far this fork has diverged from upstream, what a merge costs as a standing
expense, and whether any slice of it could ever go upstream safely. It is specified, not run.

---

### 2026-09-02 - P7.6 second pass. Every tool opens, hot reload closes, and two defects that hid the rest are fixed

**The editor was driven for an hour on the RTX 3090 with zero errors and zero validation messages**,
and the shakedown that P7.6's first pass could not reach is done. Seven of the eight rows in
[Blocked.md](Blocked.md)'s editor queue are retired. **Two defects were found and fixed**, both of
which had to be fixed *first*, because each one hid the work behind it.

**No upstream file was edited.** All four files are this fork's own, so
[TouchedFiles.md](TouchedFiles.md) is unchanged.

#### Every popped-out viewport was a window with no swapchain

Opening any tool that imgui placed outside the main window logged one error and drew nothing:

```
SDL_Vulkan_CreateSurface failed: Invalid window   PlatformUtils_Linux.cpp:313
```

`ImguiRenderer::ImGui_CreateWindowContext` (`ImguiRenderer.cpp:20`) picks the handle to build the
viewport's swapchain from:

```cpp
void* hwnd = pViewport->PlatformHandleRaw ? pViewport->PlatformHandleRaw : pViewport->PlatformHandle;
```

`ImGui_ImplSDL3_SetupPlatformHandles` fills `PlatformHandleRaw` in on Windows and macOS and leaves
it **null everywhere else**, so Linux took the fallback - and `PlatformHandle` is an
`SDL_WindowID`, an integer, not a window. `SDL_Vulkan_CreateSurface` rejected it as an "Invalid
window", the viewport got no surface, and every tool dragged out of the dock was a blank OS window.

**Fixed in `ImguiPlatform_Linux.cpp`**, as an `#elif defined(__linux__)` branch in the existing
`_WIN32`/`__APPLE__` chain, setting `PlatformHandleRaw` to the `SDL_Window*`. The port already
defines the native window handle on Linux as the `SDL_Window*` - see
`Platform::Linux::CreateVulkanSurface` and the comment at `RHI_Vulkan.cpp:2367` - so
`ImguiRenderer.cpp` then works unchanged, and **needs no upstream edit at all.** The stale comment
in `ImguiX_Linux.cpp` that said `PlatformHandleRaw` is null on Linux was corrected.

#### The title bar hit test, fixed and measured

Row 6 was diagnosed on 2026-09-01 and deliberately left. It is fixed now, the way that entry
proposed: `ApplicationTitleBar::Draw` records its three non-draggable sub-rects - the menu section,
the controls section and the window controls - and `LinuxApplication::BorderlessWindowHitTest`
tests the cursor against those instead of against `ImGui::IsAnyItemHovered()`.

New file `Code/Base/Imgui/Platform/ImguiX_Linux.h` declares the one query. The hovered flag is
still read, and still ignored, so the measurement below could be taken without a second build.

Measured on a 2560-wide window with a temporary `printf` in the hit test, since `ptrace_scope` is
1 here and gdb cannot attach to a running editor:

| window x | region | result |
|---|---|---|
| 100-380 | menu section, 8-408 | NORMAL |
| 420-1580 | the gap, 408-1617 | DRAGGABLE |
| 1620-2380 | controls section, 1617-2417 | NORMAL |
| 2420 | the 8 px section padding | DRAGGABLE |
| 2450, 2500, 2540 | minimize, maximize, close | NORMAL |

**The same trace shows the old flag still lagging** - `700,700` in the client area reported
`hovered=1`, inherited from the close button visited before it. Under the old code that lag broke
the hit test in *both* directions: `700,18` is empty title bar and reported `hovered=1`, so the
window would not drag; `2450,18` is the minimize button and reported `hovered=0`, so pressing it
would have dragged the window instead. Both are right now, and neither depends on frame order.

**The draggable region is now the gap between the sections rather than "anywhere no widget is
hovered".** That is the design's own model: `s_minimumDraggableGap` exists to guarantee the gap,
and the sections are fixed at 400 and 800 px, so the gap grows with the window - 1209 px at 2560
wide, 249 px at 1600. The gaps between the three window-control buttons are correctly
non-draggable now, where before they were not.

#### What the shakedown found

Criterion 8, every tool opened, no crashes, no errors:

| Tool | Result |
|---|---|
| Resource Importer | Opens. Parsed `Floor.fbx`, listed its meshes and materials and its two referencing resources. **Its window had never been seen** |
| Resource Dependency Viewer | Opens, with its picker and toolbar |
| Resource System Info | Opens, populated - 5 resources with load, wait and install times, plus record history |
| Resource Bulk Edit | Opens |
| Memory Tracker | Opens, populated. CPU 7.42 MB tracked / 45.03 MB, **and live GPU accounting**: VRAM 183.61 MB, per-resource-type down to `Buffer [SRV\|UAV\|Indirect]` x171 |
| System Settings | Opens, full reflected settings tree |
| UI Test, ImGui Demo, ImPlot Demo | All three open and render |
| Animation graph editor | Opens complete - graph view, control parameters, variation editor, debugger, **and a second live 3D preview viewport** |
| Property grids | Render. The Inspector's entity grid, and the ragdoll create dialog's |
| Ragdoll editor | **Not reachable with this dataset.** It requires a Skeleton resource and `Data/` has none - no `.skel` and no `.anim`, and the only FBXs are non-skeletal. Not a port defect |

Criterion 9, multi-viewport: **met.** The Outliner was dragged out of the dock into its own OS
window, entirely outside the main window, rendering through the engine's own renderer - then
dragged back in, with the destroy path clean.

Criterion 6, hot reload end to end: **met, and this closes P7.5's fifth link.** With the map open,
`Boulder.material` was rewritten to a temp file and **renamed over the original**, which is the
write pattern the criterion names. The boulder turned white in the live viewport with no restart;
restoring the file the same way turned it back. The Resource Server's own request table is the
independent record: `Boulder.material` compiled in 48.6 ms at 12:49:56 followed by
`DefaultWhite.texture`, then 29.5 ms at 12:50:36 followed by `BoulderAlbedo.texture`.

Criterion 5, `OpenInExplorer`: **met for the resource browser.** Nautilus was D-Bus-activated and
reported `"p76_scratch.ag" selected (69 bytes)` - the containing folder opened with the item
selected, which is what `explorer.exe /select,` does. Only that one call site was exercised; the
other seven build the path differently but all funnel into the same `Platform::OpenInExplorer`.

Criterion 10: clean shutdown from the title bar close button - "Shutdown Started", "No device
memory leaked", gone. The only two `[Error]` lines in the whole session are the websocket's
`Normal closure` at shutdown, which upstream logs at error level.

#### The Test Compile panel overlap, reproduced and explained

Row 5 is confirmed, and the trigger is narrow panels, not Linux:

```cpp
ImGui::SameLine( ImGui::GetContentRegionAvail().x - 200 );   // ResourceServerUI.cpp:881
```

`GetContentRegionAvail().x` is the full content width at that point, so the button is placed 200 px
from the right. Once the panel is narrower than the checkbox plus 200 px the offset lands left of
the checkbox's right edge and imgui draws the button on top of it. Dragging the server's dock
splitter in reproduces it exactly. **Deliberately not fixed** - `ResourceServerUI.cpp` is upstream
and platform-neutral, and the phase document's "do not" list says to record such bugs rather than
fix them. It will do the same thing on Windows.

#### One thing that was not a defect after all

P7.6's first pass saw the Outliner's selection do nothing and deliberately declined to call it a
defect, because the device was lost later in that run. **Retested: it works.** Selecting `Boulder`
populates the Inspector with the entity, its Static Mesh Component and its property grid, and the
viewport's transform gizmo buttons appear.

#### Two traps in driving imgui with synthetic input, neither a port defect

- **`xdotool click` puts the press and the release in one frame.** At 15-30 FPS a menu opens and
  closes inside a single frame, so menus appear not to respond and the failure is intermittent.
  Send `mousedown`, sleep, `mouseup`. This cost an hour and looked exactly like the title bar
  defect.
- **SDL's own drag and resize do not respond to synthetic pointer events under Xvfb.** Resize from
  a window edge does not either, and that path was never touched, so neither result says anything
  about the hit test. That is why the table above was measured from inside the code instead.

#### A Shipping build was attempted, and this machine cannot link one

The hit test's new call is inside `#if EE_DEVELOPMENT_TOOLS`, and `ImguiX_Linux.cpp` compiles to
nothing without it, so Shipping is the configuration that proves the guard. **All 607 of its
compile steps pass**, which is what was being checked. The link then fails in `ld` rather than in
the code - `LLVMgold.so: cannot open shared object file` - because
`External/LLVM/lib/LLVMgold.so` does not exist in this machine's LLVM install. No Shipping binary
has ever been linked here; the 2026-08-31 "Shipping links" entry was the first machine. Recorded
in [Still open](#still-open).

#### Still blocked, and why

Minimize and maximize (row 4) still need a window manager that implements them; i3 is the only one
installed and the developer declined an install. Row 9 needs a different driver. **P5.16 raytracing
is untouched** and is the largest thing left in the GPU queue.

---

### 2026-09-01 - P7.6 first pass. The editor opens a map, and the hang that killed it is fixed

**The editor has now been used, not just launched.** It draws its whole docked UI on the RTX 3090,
opens `demo/render/pbr/pbrdemo.map` in the map editor and renders it - sky, reflective ground,
geometry - with the Outliner listing `pbrdemo`/`Boulder`/`Floor`/`Skydome`, host validation on and
**zero validation messages, zero errors and zero warnings**. It also spawns the Resource Server
itself, which compiled and served five resources on demand.

**P7.6 is not finished.** This is the first pass: two defects found, one fixed, and a third
observation that must not be recorded as a defect yet. The rows are in [Blocked.md](Blocked.md).

#### The whole session ran on a virtual display, and that is worth reusing

`Xvfb :99 -screen 0 2560x1440x24` plus `i3 -c <minimal config>`, driven with `xdotool` and captured
with `maim -u -i <window>`. **NVIDIA presents to Xvfb**: `vkcube` picked the RTX 3090 there and drew
correctly, and so did the editor. Nothing appears on the developer's own desktop, which is the
point - the alternative takes over the machine.

Two limits, both learned the hard way and both in the caveats below: the device is lost after two
or three minutes there, and **`maim -u` is required** because the rendered cursor otherwise changes
every hash you compare.

#### The title bar hit test reads a stale imgui hover state, and it is not cosmetic

`LinuxApplication::BorderlessWindowHitTest` (`Application_Linux.cpp:453`) asks
`EditorUI::GetBorderlessTitleBarInfo`, which returns `ImGui::IsAnyItemHovered()`
(`EditorUI.cpp:234`). **SDL calls the hit test while draining the motion event, before imgui has
processed that motion**, so the flag always describes the *previous* cursor position. Measured with
a gdb `dprintf` on the live editor - cursor, then the hovered flag it reported:

```
700,700 (client area)                       -> 0
 79,20  (Editor menu, visibly highlighted)  -> 0     wrong, the item IS hovered
2446,20 (minimize)                          -> 1
2492,20 (maximize)                          -> 1
2538,20 (close)                             -> 1
300,20  (EMPTY title bar)                   -> 1     wrong, nothing is hovered
 79,20  (Editor menu)                       -> 0
```

Every value is the one before it. One motion behind, exactly.

**The user-visible cost is that the application menus can be unreachable.** Drag the window by the
same empty title bar point and it works or does nothing depending only on where the pointer came
from - from the client area it moved the window 400,200 to 550,270, from the close button it did
not move at all. Clicking a menu after arriving from the client area loses the click; nudging three
pixels first opens it. Within one session the title bar can stay unclickable while a click on the
map editor's own `Window` menu forty pixels below works immediately, because SDL coalesces motion
events and which position was last evaluated is not predictable.

**Not fixed, on purpose.** The fix belongs in this fork's own `ImguiX_Linux.cpp`, which already
computes the menu and control sub-rects (`windowControlsStartPosX`, `menuSectionFinalWidth`, lines
36-50): test the cursor against those rather than against a frame-scoped imgui flag. That removes
the frame-order dependency instead of racing it.

#### `GetEnumInfo<T>()` was MSVC-only, and it killed the editor

Opening **Resources > Resource Importer** killed the editor on
`EE_ASSERT( pObjectCategoryEnumInfo != nullptr )`,
`PropertyGrid_CollisionSettings.cpp:61`. The property grid is innocent.
`TypeRegistry::GetEnumInfo<T>()` (`TypeRegistry.h:101`) did this:

```cpp
char const* pEnumName = typeid( T ).name();
TypeID const enumTypeID( pEnumName + 5 );
```

MSVC returns `"enum EE::Physics::ObjectCategory"` and `+ 5` skips exactly `"enum "`. **The Itanium
ABI returns the mangled name.** Measured with the clang in `External/LLVM`:

```
typeid().name() = "N2EE7Physics14ObjectCategoryE"
name()+5        = "Physics14ObjectCategoryE"
registered ID   = "EE::Physics::ObjectCategory"
```

So the template overload returned **nullptr for every enum on Linux**, and every caller that
asserts on the result took the editor down. Four callers, all in
`PropertyGrid_CollisionSettings.cpp`. It is the only `typeid().name()` site in `Code/` outside
ThirdParty.

**Escalated and approved**, because Rule 2 does not cover it: the Windows body is wrapped in
`#if _WIN32` and an `#elif defined( __linux__ )` branch added beside it - **15 lines added, 0
modified**, the Windows branch byte for byte the original. The ABI-specific half is
`Platform::DemangleTypeInfoName` in this fork's own `Platform_Linux.h`, which is where the other
MSVC compatibility shims already live, so `TypeRegistry.h` gains no include. The `TypeID` is cached
per type because the demangle allocates and property grids call this every frame. Registered in
[TouchedFiles.md](TouchedFiles.md). Verified: the importer no longer asserts and the editor
survives it.

#### The editor hang: a direct draw inherited the previous indirect draw's root arguments. **Fixed**

**The developer found it and had to correct this entry twice.** Opening the map and left-clicking
once in the viewport - enough to select an entity - froze the whole editor, hard enough to need a
`kill -9`. Two earlier claims that it was fixed were wrong, and both came from bad measurement
rather than a bad hypothesis; the mistakes are listed at the end because they cost most of a day.

**It is a GPU fault, not a deadlock.** The kernel names it, once per hung run:

```
NVRM: Xid (PCI:0000:01:00): 109, pid=..., channel 0x56, errorString CTX SWITCH TIMEOUT
```

Xid 109 is a context that could not be preempted - a dispatch that never terminates. The main
thread sits in `QueueHostWait` (`RHI_Vulkan.cpp`), every other thread idle, the GPU parked at
210 MHz, and the queue counters 15 graphics and 6 compute submits behind. **Every wait at the stall
is satisfied**: a 31,922-line submit trace showed no unsatisfiable waits and no cycles, and the
argument strides measured 48/48 with the dispatch arguments at offset 32. Nothing was waiting on
anything - the GPU had simply accepted a command buffer and never finished it.

#### What it was

`EE_IndirectRoot` is the push constant block P5.17 added so a shader can find its own command in an
argument buffer. `EE_DECLARE_INDIRECT_ROOT_CONSTANTS` (RHI.esh:190) chooses how to read the root
constants from it:

```
if ( EE_IndirectRoot.m_stride == 0 )  EE_g_RootConstants = RootConstants;                  // direct
else                                  EE_g_RootConstants = vk::RawBufferLoad<Type>( ... );  // indirect
```

**Only `CmdExecuteIndirect` ever wrote that block**, so it kept whatever the last indirect call left
in it. A *direct* draw recorded after an indirect one therefore saw a non-zero `m_stride`, took the
indirect path, and loaded its root constants from the previous call's argument buffer at a stale
device address.

`DebugDraw.esf` is where that bites, because the debug draw pass uses one shader both ways: an
indirect draw for debug meshes, then `CmdSetPipeline`, `CmdSetRootConstants` and a **direct**
`CmdDispatchMesh` for the debug commands. The direct dispatch came back with a garbage
`m_numDebugDrawCommands` and garbage command addresses, and the mesh shader that ran on them never
terminated.

**Why only the editor, and only after a click.** The direct dispatch is guarded by
`if ( numCommands )`. The engine submits no debug geometry, so it never runs there at all.
The editor only has debug geometry once something is selected - which is exactly the developer's
trigger, and why an editor with a map open but nothing selected survives.

#### The fix

Zero the block on every pipeline bind, in `CmdSetPipeline` (`RHI_Vulkan.cpp`, this fork's own
file). Every draw binds a pipeline first, and `CmdExecuteIndirect` pushes the real block
immediately before its own dispatch, so the indirect path is untouched.

#### How it was found, and how it was verified

Bisected with a probe script that starts the Resource Server separately, opens the map, **checks
the viewport is actually rendering** before trusting the run, fires the single left click, and then
reads liveness from `/proc/<pid>/stat` CPU ticks and the kernel Xid count:

| Build | verdict |
|---|---|
| baseline | FROZEN, xid=1 |
| debug draw pass disabled | alive |
| debug draw pass on, only the direct `CmdDispatchMesh` disabled | alive |
| unbounded indirect mesh dispatch disabled instead | FROZEN - not that one |
| mesh dispatch size clamped to 4096 groups | FROZEN - not the dispatch size |
| **the fix** | alive, three probe runs and a six-round soak, zero Xid |

**The debug draw pass now draws.** A selected entity's bounds appear in the viewport as a yellow
line, which is the first debug primitive this port has ever produced - see the row it retires in
[Blocked.md](Blocked.md).

#### Two fixes that came out of the hunt and are NOT the hang

Both are real defects, both verified, neither fixes the freeze. They are kept because they are
correct on their own terms:

- **The cluster culling argument buffer was never cleared** (`Renderer_ForwardShading.cpp`).
  `CmdExecuteIndirect` documents the invariant - "the engine clears the buffer for us" - and the
  clear did not exist. Vulkan has no indirect dispatch count, so the RHI records one dispatch per
  possible command and the ones past the GPU-written count read their slots regardless. Measured
  `maxNumCommands` = 146 in the editor against 1 in the engine.
- **An unbarriered write-after-write on the cluster buffer** (`RenderSystem.h`). The whole-buffer
  copy on growth and a later sub-range copy in the same frame both write the same bytes with no
  order between them. Reported by `SYNC-HAZARD-WRITE-AFTER-WRITE` and gone after the barrier.

#### The measurement mistakes, recorded so they are not repeated

- **The submit trace contained two processes.** The editor and the Resource Server share a
  terminal, so counting `[SUB]` lines counted the server's submits too and a frozen editor looked
  busy. Start the server separately, or filter by queue pointer.
- **`ps`/`top -b -n1` report CPU averaged over process lifetime**, so a process that hung two
  minutes ago still reads 50%. Sample `/proc/<pid>/stat` twice instead.
- **A clean run means nothing if the map closed partway.** Two "five minutes clean" results were
  worthless because a stray click had closed the map tab. The probe now checks viewport brightness
  before believing a run.
- **The virtual display is not a substitute for the real one.** Xvfb reproduces the fault only
  sometimes, and turned two lucky runs into a false "fixed".

#### The device loss on the virtual display was the same defect

`vkQueueSubmit2` returns **`VK_ERROR_DEVICE_LOST`** - read as `$eax = -4` at the loader boundary,
since the release build optimises `result` away - and from then on every submit and every
`vkWaitSemaphores` fails:

```
RHI_Vulkan.cpp:2352  EE_ASSERT( result == VK_SUCCESS )   SubmitToQueue
RHI_Vulkan.cpp:2241  the same assert                     QueueHostWait
#1 SubmitToQueue  #2 RenderSystem::SubmitFrame (RenderSystem.cpp:927)
#3 Engine::Update (Engine.cpp:710)  #4 LinuxApplication::Run (Application_Linux.cpp:555)
```

No validation message precedes it, and `dmesg` is restricted here so no Xid could be read.

**This was the same defect, and the Xvfb suspicion was wrong.** Where the real display simply
stalled forever, the virtual display's driver escalated the same never-completing work to
`VK_ERROR_DEVICE_LOST` after a minute or two. Both went away with the argument buffer clear.

The suspicion was reasonable and worth recording as a warning: the two device losses were both on
Xvfb, and the only long clean session at that point was on the real display. It was the developer's
own reproduction - a hard hang on an awake, unlocked desktop - that broke the tie. **A caveat held
long enough to be tested is cheap; the same caveat asserted as a conclusion would have sent the
next session chasing the display server.**

#### Three things that look like defects and are not

- **Running the editor under gdb freezes it on any dialog.** Every file and message dialog
  fork/execs `zenity`; under gdb the forked child can be left in tracing-stop while the editor
  blocks forever in `poll()`. Symptom: identical frames, 0.2% CPU, main thread wchan `do_poll`, a
  child in state `tl`. Attach gdb for shutdown and crash work, never for dialog work.
- **i3 has no iconify, so the minimize button cannot work.** `xdotool windowminimize` on the editor
  is equally a no-op. Maximize is the same story - i3 does not implement it either, so both buttons
  are untestable here and need a WM that does.
- **The zenity save dialog's filter reads "(None)".** The editor passes
  `--file-filter=*.map | *.map` and the combo still opens unfiltered - and the identical zenity
  command run standalone does the same, so it is GTK on Ubuntu 24.04, not the port. Criterion 4's
  "filter by extension" is offered, not applied, on this distro.

#### What ran, and what is still untouched

Verified working: the editor launches and draws its full docked UI at 13-119 FPS; the map editor
opens and renders pbrdemo; the resource browser's tree, search box, row selection and seven-item
context menu; data paths display with forward slashes; the zenity save dialog opens from the map
editor with the right start directory; window resize relayouts the UI and resizes the swapchain
(1685x1291 to 1598x998 to 2560x1440); the Resource Server is spawned by the editor, serves it, and
PR #71's "still connected clients" prompt renders and answers; and **the title bar close button
works with a clean shutdown** - "Shutdown Started", "No device memory leaked", gone in under two
seconds, which is criterion 10 for the editor.

Still untouched: the importer's own window (the crash is fixed, the window has not been seen), the
dependency viewer, system info, bulk update, the animation graph and ragdoll editors, property
grids, mesh picking, debug draw, hot reload's last link and multi-viewport drag-out.

**One observation is deliberately not recorded as a defect.** Selecting an entity in the Outliner
did nothing - the Inspector stayed "Nothing To Inspect" and the expander would not toggle - but the
device was lost minutes later in that same run, so it may be a symptom of that rather than a defect
of its own. Retest it first on an awake real display.

#### A stale table, corrected

[In flight](#in-flight) listed PRs #68, #69 and #70 as open. All three merged on 2026-09-01.

---

### 2026-09-01 - P5.11 query pools. The frequency is the right way up, and the whole path runs

**The group had never executed, and it could not be made to by running the engine.**
`RHI::CreateQueryPool` and `RHI::GetQueryTimestampFrequency` have **zero callers anywhere in
`Code/`** - the engine's profiling path went out with Optick - so there was nothing to observe.

Exercised instead with a temporary harness inside `RHI_Vulkan.cpp`, **reverted before commit**: a
timestamp pool created at present 1400, a query pair opened in `BeginCommandBuffer` and closed in
`EndCommandBuffer` for every graphics command buffer of frame 1500, resolved into a
`DeviceToHost` + `PersistentMap` buffer, and read back at present 1560.

#### What it measured

```
timestampPeriod = 1.000000 ns/tick, validBits = 64, GetQueryTimestampFrequency = 1000000000.0 Hz

graphics command buffer 0:     928 ticks -> 0.0009 ms
graphics command buffer 1:  152576 ticks -> 0.1526 ms
graphics command buffer 2:  427232 ticks -> 0.4272 ms
graphics command buffer 3: 1107136 ticks -> 1.1071 ms
graphics command buffer 4:    1120 ticks -> 0.0011 ms
```

**The inversion is correct.** Vulkan reports `timestampPeriod` in nanoseconds per tick and
Direct3D 12's `GetTimestampFrequency` reports ticks per second, and `GetQueryTimestampFrequency`
returns `1.0e9 / period` - 1,000,000,000 Hz here, which is the Direct3D sense. **This is the
failure the row warned about and it is worth saying how loud it would have been**: get it the
wrong way up and command buffer 3 reports 1.1e15 ms instead of 1.1 ms. It is not a subtle
constant-factor error after all, at least on a device whose period is 1.0.

**The numbers are not a fixed offset.** They span three orders of magnitude and track how much
work each command buffer carries - two are near-empty, three do real work, and the total of about
1.7 ms of GPU time is plausible for pbrdemo on an RTX 3090. That is what separates "the path runs"
from "the path returns two adjacent timestamps".

Everything in the group ran: `CreateQueryPool`, `CmdResetQueryPool`, `CmdEndQuery` on a timestamp
pool, `CmdResolveQuery` and `GetQueryTimestampFrequency`. Host validation was on throughout and
said nothing. `CmdBeginQuery` is still unexercised, and it cannot be otherwise - it returns
immediately for a timestamp pool, and nothing creates a `PipelineStatistics` one.

#### An accidental confirmation

The harness leaked its pool and its readback buffer on purpose-by-omission, and shutdown asserted
on `pair.second.m_numAllocations == 0`. **`ReportDeviceMemoryLeaks` catches a real leak**, which
is more than criterion 10 has been able to say from a clean run alone.

#### Files

- Files changed: **none.** Documentation only. The harness in `Code/Base/Render/RHI_Vulkan.cpp`
  was reverted.
- Upstream files edited: **none.** No [TouchedFiles.md](TouchedFiles.md) change.
- Acceptance criteria: no change. P5.11 was never a criterion of its own; it was a Blocked.md row.

### 2026-09-01 - The GPU-blocked queue, part 1. P5.12, P5.15 and most of criterion 8, from one capture

**One RenderDoc capture of one pbrdemo frame answered four Blocked.md rows.** Taken on the RTX
3090 reference machine (driver 580.173.02, Ubuntu 24.04.4, X11). Frame 1501, 645 MB, read as XML
with `renderdoccmd convert` - no GUI, and no replay needed for any of it.

#### RenderDoc and the validation layer cannot both be on. This costs a session if you hit it

**With `Enable_Host_Validation = true`, the engine segfaults about one second in**, at
`RHI_Vulkan.cpp:3595` - the `vkCmdPushDescriptorSetKHR` in `CmdSetRootConstants`, reached from the
light culling dispatch, the first root constant write in the frame. The fault is inside
`librenderdoc.so`, below it inside the NVIDIA driver, with a null first argument.

**Turn host validation off and RenderDoc is fine.** Same binary, same map, same everything else.
It is a RenderDoc-plus-validation-layer interaction, **not a port defect**: the crash needs no
RenderDoc API use at all, only the layer being loaded, so nothing the engine calls is involved.

Do not spend time on it. If you want validation inside a capture, RenderDoc has
`--opt-api-validation` for exactly that.

#### There is still no way to trigger a capture without editing code

Nothing in the engine calls `TriggerCapture`, `BeginFrameCapture` or `EndFrameCapture` - the two
RHI entry points have **zero callers anywhere in `Code/`**. The capture here came from a temporary
`g_pTempRenderDocAPI->TriggerCapture()` in `QueuePresent`, gated on a present counter, **reverted
before commit**. There is no `xdotool` on this machine either, so the capture key is not reachable
from a script on an `Xvfb` display.

Phase 5 criterion 9 therefore stays half met, for the same reason as before.

#### P5.12 debug names and markers - **verified**

| Half | Evidence in the capture |
|---|---|
| Markers | **75 `vkCmdBeginDebugUtilsLabelEXT` / `vkCmdEndDebugUtilsLabelEXT` pairs**, correctly nested, covering every pass |
| Object names | **834 `vkSetDebugUtilsObjectNameEXT` chunks, 297 distinct names** - queues, buffers, textures by data path, root signatures and all 30 pipelines |

**The two halves are driven differently, and only one of them has a public caller.** Markers come
from `EE_RHI_COMMAND_BUFFER_PROFILE_SCOPE`, used at 35 sites across `Code/Engine/Render/`. Object
names come from the `m_debugName` field on the various `*Parameters` creation structs, which
`RHI_Vulkan.cpp` forwards to its internal `SetVulkanObjectName` at 25 sites.

**The nine public `RHI::SetDebugName` overloads have zero callers in `Code/`.** That is an
upstream fact about the engine, not a port gap. Do not go looking for the caller; there isn't one.

#### P5.15 variable rate shading - **verified**

The row's concern was the pipeline change, not the feature. On a device with
`VK_KHR_fragment_shading_rate`:

- **all 23 graphics and mesh pipelines** in the capture declare
  `VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR`, which is every one of them,
- `vkCmdSetFragmentShadingRateKHR` is called 5 times across the frame's 8 command buffers - once
  per command buffer from a pool with `VK_QUEUE_GRAPHICS_BIT`, and not at all on the other three.
  That is the guard at `RHI_Vulkan.cpp:2921` doing its job; setting it on a transfer command
  buffer is what caused the GPU hang that P6.9 fixed,
- and the frame renders correctly with zero validation messages.

The feature itself still reports `ShadingRate::NotSupported` (`RHI_Vulkan.cpp:625`), matching the
Direct3D 12 backend, which is deliberate.

#### The debug draw pass records and issues draws

Its pipelines create - `RenderPass_DebugDraw Pipeline Transparent DepthOn_Color` and
`... DepthOn_Depth` are both named in the capture - and the `Debug Draw` scope contains three
sub-scopes issuing **5 `vkCmdDrawMeshTasksIndirectEXT`** between them, plus the `DebugDrawResolve`
dispatch and its copy-backs.

**What is not established is that anything visible comes out.** The draws are indirect with
GPU-written counts and the pbrdemo scene submits no debug geometry, so the count may be zero. The
place that settles it is the editor, which draws gizmos and bounds. Left as a narrowed row.

#### Criterion 8, feature parity, named one at a time

| Feature | Verdict | How |
|---|---|---|
| Forward shading | **Verified** | Three passes - depth only, opaque + alpha test, alpha blend - 8 `vkCmdDrawMeshTasksIndirectCountEXT` each across 4 material buckets, and the frame is correct |
| Cascaded shadows | **Verified** | 4 cascades x 4 buckets x 8 indirect mesh draws, and the shadow is visible in the frame |
| GTAO | **Verified** | 3 sub-passes, 5 dispatches. A/B against `Enable_SSAO = false`: the difference lands on geometry and ground contact and is **exactly zero in the sky**, which is what ambient occlusion should do |
| SMAA | **Verified** | 3 pipelines, 3 draws, stencil reference set per draw. A/B against `Enable_SMAA = false`: rock silhouettes against the sky are smooth with it on and visibly stair-stepped with it off |
| Debug draw | **Partly.** Records and draws; visible output unproven | Above |
| OIT | **Cannot be verified. It is not wired into the engine** | Below |
| Mesh picking | **Not reachable from the engine. It is editor-gated** | Below |

**Both A/Bs need no rebuild.** `m_enableSSAO` and `m_enableSMAA` are reflected settings in
`Code/Base/Render/Settings/Settings_Render.h` under `Category = "Render"`, so the ini keys are:

```ini
[Render:Render]
Enable_SSAO = false
Enable_SMAA = false
```

**OIT is dead code in this engine, on both backends.** `OITResolve.esf` compiles and
`RenderPass_DebugDraw.cpp:19` includes its generated header, but **nothing anywhere looks up an
`"OITResolve"` shader or creates a pipeline for it**, and `OIT.esh` has no consumers at all. The
"Forward Shading Alpha Blend Pass" is ordinary alpha blending, not OIT. So criterion 8 cannot be
met for OIT by any amount of Linux work - the feature does not run on Direct3D 12 either.

**Mesh picking is gated on `pRenderViewport->IsPickingEnabled()`**, inside `#if
EE_DEVELOPMENT_TOOLS` at `Renderer_ForwardShading.cpp:1022`. No `InstancePickingResolve` pipeline
is created in a standalone engine frame. It belongs to P7.6, in the editor viewport, not here.

#### The query-as-enable-request pattern is still in place, and this driver does not catch it

Read all four feature blocks (`RHI_Vulkan.cpp:1157-1232`). **The mesh shader block is the only one
that clears the bits it does not want** - `multiviewMeshShader`,
`primitiveFragmentShadingRateMeshShader`, `meshShaderQueries`. The shading rate, acceleration
structure and ray tracing pipeline blocks each pass the struct `vkGetPhysicalDeviceFeatures2`
filled straight into `vkCreateDevice`, so **every bit the device supports is being asked for**.

Measured, rather than assumed: on the RTX 3090 with driver 580.173.02 and host validation on,
**no VUID fires** and device creation succeeds. Raytracing is enabled on this device - RenderDoc's
own log says "Acceleration structures enabled". So the pattern is latent here, exactly as the row
said. Narrowed, not deleted.

#### Reproducing any of this

```bash
export PATH="$PWD/External/LLVM/bin:$PATH"     # or NinjaGen.py cannot find clang++
Xvfb :77 -screen 0 5120x1440x24 &
/tmp/shot.sh <ini-file> <out.png>              # the A/B recipe, 25s per frame
```

The capture and its XML are at `/tmp/RenderDoc/`. They are worth keeping for the session: the
convert step takes about 90 seconds and the capture itself takes 20 minutes of engine run time to
reach.

#### Files

- Files changed: **none.** Documentation only. The measurement scaffold in
  `Code/Base/Render/RHI_Vulkan.cpp` was reverted.
- Upstream files edited: **none.** No [TouchedFiles.md](TouchedFiles.md) change.
- Acceptance criteria: Phase 5 criterion 8 moves from "not met" to met for four of seven, with
  three that cannot be met as written. Criterion 9 unchanged.

### 2026-09-01 - The `storageInputOutput16` startup warning outlived the problem it described

**Every start on both development machines printed a warning about a gap that no longer exists.**
`CreateContext` reported `storageInputOutput16` alongside `shaderInt16` and `shaderFloat16` and
said "Shaders that use them will fail to create". That was true when P6.8 wrote it. **P5.20 made
it false**: `DebugDraw.esf`'s `DebugDrawPrimitiveOutput` was the engine's only 16-bit
interpolant, and `EE_INTERSTAGE_HANDLE` now carries it as a `uint` on SPIR-V.

- Files changed: `Code/Base/Render/RHI_Vulkan.cpp`. The port owns it.
- Upstream files edited: **none.** No [TouchedFiles.md](TouchedFiles.md) change.
- The row for this in [Blocked.md](Blocked.md) is removed.

#### What it says now

The warning keeps `shaderInt16` and `shaderFloat16`, which shaders **do** still use for buffer
accesses, and keeps the "will fail to create" wording for them, because it is still true.

`storageInputOutput16` moves to its own line, as a **message** rather than a warning, and says
what the device is rather than what the engine will do:

```
[Message][Rendering][RHI/CreateContext] This device has no storageInputOutput16. No shader in the
engine declares it, so nothing depends on it.
```

The feature is still **enabled when the device has it**. Nothing declares it today; a future
shader may want it back, and enabling an available feature costs nothing.

#### Why a stale warning is worth a commit

It named the first row of [What the first machine still cannot do](#what-the-first-machine-still-cannot-do)
on every single start, on hardware where that row no longer has any consequence. Two sessions
spent time asking whether it was the cause of something. A warning that cannot be acted on
trains the reader to skip warnings.

Verified by running the Resource Server: the warning is gone and the message appears in its
place. This machine has `shaderInt16` and `shaderFloat16`, so the remaining warning correctly
stays silent.

### 2026-09-01 - P7.3 follow-up. The Resource Server asks before it exits, as Windows does

**`OnUserExitRequest` confirms again.** P7.3 had to give it up: `MessageDialog::Confirmation`
returned `Cancel` unconditionally, so asking would have refused every exit and trapped the user in
a window that would not close. P7.2 made the dialogs real, and this restores the behaviour.

- Files changed: `Code/Applications/ResourceServer/Linux/ResourceServerApplication_Linux.cpp`.
  The port owns it.
- Upstream files edited: **none.** No [TouchedFiles.md](TouchedFiles.md) change.
- The row for this in [Blocked.md](Blocked.md) is removed.

#### `ShowEx`, not `Confirmation`, and the reason is the old trap

`Confirmation` collapses two different answers into one `false`: **the user said No**, and **no
dialog could be shown**. A server that cannot show a dialog would then refuse every exit - which
is exactly the failure P7.3 avoided by not asking at all.

`ShowEx` returns the three cases apart, so the call site can say what it means:

| Result | What happened | What the server does |
|---|---|---|
| `Yes` | the user confirmed | exit |
| `No` | the user declined | stay open |
| `Cancel` | no dialog was shown; the message went to the log instead | **exit** |

`return result != MessageDialog::Result::No;` is the whole rule.

#### Verified, all four paths

| Case | Result |
|---|---|
| No clients connected | Exits immediately, no dialog |
| A client connected, **No** | The dialog closes and **the server stays open**, window and all |
| A client connected, **Yes** | The server exits |
| A client connected, no `zenity` on `PATH` | No dialog, `[Warning][Tools][Dialog] Resource Server: There are still connected clients!` in the log, and the server exits |

**`SIGTERM` is a much better test trigger than clicking the title bar.** SDL turns it into
`SDL_EVENT_QUIT`, which reaches `OnUserExitRequest` by exactly the same path as the window
manager's close. Clicking the imgui close button with `xdotool` works but is flaky: the button
fires on release while hovered, and a fast synthetic click can fall inside one frame.

#### Three things found while testing, none of them this change

**`ninja` with no target builds Debug only.** The generated `default` rule lists the nine
`Linux_Debug` outputs and nothing else. So `ninja -f Build/Linux/Esoterica.ninja -k 0` - the
command in [/AGENTS.md](../../AGENTS.md#definition-of-done), and the one several entries above
call a whole-tree build - **never rebuilds `Linux_Release`**. This cost half an hour: the code
change was in, the build was green, and the running Release binary was a day old. Name the
configuration you want, or check the timestamp.

**The Resource Server does not shut down cleanly.** On exit it prints

```
Shutting down low level socket/threading support.
Memory leak detected (span->list_size == span->used_count) at Code/Base/ThirdParty/rpmalloc/rpmalloc.c:1424
```

and traps. **Reproduced with zero clients and no dialog shown**, so it is not this change, and not
P7.2's. **Root-caused since**: `Network::Server` never drains `m_receivedMessages` at shutdown.
It is upstream, platform-neutral, and intermittent. See
[Upstream issues observed](#upstream-issues-observed).

**Destroying the window out from under the application asserts.** `xdotool windowclose` sends
`XDestroyWindow` rather than `WM_DELETE_WINDOW`, and the next swapchain acquire fails:
`result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR`. There is no Windows equivalent to compare
against and no user path that does it, so it is recorded rather than fixed. It also means
`xdotool windowclose` is **not** a way to test an exit path.

### 2026-09-01 - P7.5 Hot reload. **Four links of five are proved. The fifth needs a working GPU**

**A source edit reaches a connected client as a `ResourceUpdated` message, with a fresh compiled
file behind it.** What is not proved is the editor acting on that message, because the editor
cannot stay alive on this machine's GPU for the three seconds the round trip takes.

- **No code changed.** This task found no defect. The Phase 3 watcher, the Resource Server and
  the network path all did what they were supposed to.
- Upstream files edited: **none.** No [TouchedFiles.md](TouchedFiles.md) change.
- Acceptance criteria: **P7.5 is not fully met.** Links 1 to 4 below are verified. Link 5 is not,
  and cannot be on this machine.

#### The chain, and where the evidence stops

| # | Link | Verified |
|---|---|---|
| 1 | A source edit produces an inotify event | **yes**, for four different write patterns |
| 2 | The Resource Server turns it into a compile request | **yes**, visible in the requests table |
| 3 | A worker recompiles and the compiled file is rewritten | **yes**, 19-28ms, mtime moves |
| 4 | The server pushes `ResourceUpdated` to connected clients | **yes**, received by a client |
| 5 | The client's `ResourceSystem` reloads and its users rebind | **no** - see below |

#### The watcher survives every way an editor writes a file

The phase document expected trouble here, above all with editors that save by writing a temp file
and renaming it over the original, which produces `IN_MOVED_TO` and not `IN_CLOSE_WRITE`. **There
is no trouble.** A scratch program watching `Data/` reported, in order:

| What was done | Events reported |
|---|---|
| `cat backup > file` (in-place write) | `FileModified` on the file |
| write a temp in the same directory, `mv` over the file | `FileCreated` + `FileModified` on the temp, then `FileRenamed` with `m_oldPath` set to the temp |
| `sed -i` (which is a rename, with its own temp name) | the same three, correctly paired |
| create then delete a file | `FileCreated`, `FileModified`, `FileDeleted` |

The rename pairing works because `ProcessListOfDirectoryChanges` holds an `IN_MOVED_FROM` by its
cookie until the matching `IN_MOVED_TO` arrives.

**And the rename case would not have mattered even if it were reported as a create.**
`ResourceServer::UpdateFileSystemWatcher` skips directory events and then treats every file event
the same, whatever its type. There is no `FileModified`-only path to fall through.

#### Recompilation is correct, including when it does nothing

The first three edits wrote **byte-identical content**. The server logged a request for each and
reported them **up to date** in 15-16ms with 0ms of compile time. Only the fourth edit, which
actually changed a byte, compiled: 19.0ms. The compiled file's mtime moved to match.

That is right, and it is worth knowing before someone concludes the watcher is broken: an editor
that rewrites a file without changing it produces a request that correctly does no work.

#### Link 4, and how it was measured without a renderer

The server sends `NetworkMessageID::ResourceUpdated` to every connected client except the one that
asked for the compile. The client's `NetworkResourceProvider` puts the ID in
`m_externallyUpdatedResources`, and `ResourceSystem::UpdateResourceProvider` turns that into
`RequestResourceHotReload`. **Nothing on that path logs**, so there is nothing to read in a log
file.

The editor cannot be used to observe it (see below), so a **scratch client with no renderer** was
built against `libEsoterica.Base.so`: it starts `NetworkResourceProvider`, requests
`data://demo/render/pbr/boulder/boulder.material`, and prints whatever
`GetExternallyUpdatedResources()` returns each tick. Editing the material printed

```
RESOURCE UPDATED: data://demo/render/pbr/boulder/boulder.material
```

That is the exact value `RequestResourceHotReload` is called with. Everything past that point is
platform-neutral engine code.

`ResourceProvider`'s virtuals are public on the base and private on the override, so a client like
this drives them through a `ResourceProvider*`. `ResourceRequest` needs a non-null `ResourceLoader`
in its constructor but never calls it unless the load stages are driven, so a stub loader is
enough.

#### Why link 5 cannot be measured here, and it is not a port defect

**The editor dies about three seconds after it reaches its frame loop.** Every time, with or
without a startup map:

```
#0  EE::Render::RHI::QueueHostWait (semaphore=64) at Code/Base/Render/RHI_Vulkan.cpp:2232
#1  EE::Render::RenderSystem::WaitForFrameStart () at Code/Engine/Render/RenderSystem.cpp:321
#2  EE::Engine::Update () at Code/Engine/Engine.cpp:647
```

`EE_ASSERT( result == VK_SUCCESS )` on the semaphore wait. This is the device loss this machine's
Intel UHD 620 has had since Phase 6: it has no `VK_EXT_mesh_shader`, so every mesh draw is dropped
and later passes read what the geometry path never wrote. [Progress.md](#current-state) already
says not to chase rendering here.

**The GPU reset takes the Resource Server down with it for about seven seconds.** Both processes
present through the same GPU, so when the editor hangs it, the server's frame loop stalls and its
`Update()` stops running. Measured directly: a file edit at `11:37:51.296` produced a compile
request timestamped `11:37:58`, while the same edit with no editor running was serviced in the
same second. **The editor was dead before the recompile finished.** That is why racing the crash
does not work, and it is worth writing down: a seven second lag between an edit and a compile is
alarming and has nothing to do with the watcher.

**Where to finish P7.5: the RTX 3090 machine.** Open the map, edit a material, and watch the
viewport. Nothing more is needed, and the four links below it are already proved.

#### Two things this run confirmed for other tasks

- **P7.3's "serving a resource to a client" is no longer unverified.** The editor's own requests
  are in the server's table, tagged with the client icon: the material, the physics database,
  three textures and the map, all fetched over the wire with no `-packaged`.
- **P7.2's message dialogs work in the real editor.** A deliberately broken Vulkan ICD made
  `SDL_CreateWindow` fail, and the fatal error came up as a zenity dialog rather than a log line.

#### One thing for P7.6

The Resource Server's left panel draws **"Force Recompile" and "Request Compilation" on top of
each other** in the Test Compile section. It is a layout problem in shared imgui code, not a
platform one, but P7.6 should record it properly.

### 2026-09-01 - P7.4 `OpenInExplorer`. It compiled, and it opened the wrong applications

**The eight call sites were fine. The function underneath them was not.** P7.4 was written as a
verification task. Running it found that `xdg-open` is not what "Open In Explorer" means, and
`OpenInExplorer` is now built on `org.freedesktop.FileManager1.ShowItems` instead.

- Files changed: `Code/Base/Platform/PlatformUtils_Linux.cpp`. The port owns it.
- Upstream files edited: **none.** No [TouchedFiles.md](TouchedFiles.md) change. All eight call
  sites are unchanged, and have been since Phase 1.
- Acceptance criteria: P7.4 is met, including the "decide which behavior you want, and record it"
  part. The decision is below.

#### What `xdg-open` actually did on this machine

The old implementation was `xdg-open <path>`, for both files and directories. `xdg-open` runs the
handler registered for the file's MIME type, and nothing guarantees that is a file manager:

| Path | MIME type | What `xdg-open` launches |
|---|---|---|
| `Data/Demo/Render/PBR/PBRDemo.map` | `text/plain` | **calibre-ebook-viewer** |
| `Data/` | `inode/directory` | **org.gnome.baobab** - Disk Usage Analyzer |

So neither the file case nor the directory case opened a file manager. That is not specific to a
strange machine: `inode/directory` is claimed by whatever the user installed last, and every
Esoterica resource extension is unregistered, so a data file falls back to its detected type.

Read the menu labels the call sites use and the intent is not ambiguous: "Open In Explorer",
"Go to Source File", "Go to Compiled File". They all mean *show me this file where it lives*.

#### The decision: select the item, matching Windows

`explorer.exe /select, <path>` opens the containing folder and highlights the item.
`org.freedesktop.FileManager1.ShowItems` is the same operation, and **Nautilus, Dolphin, Thunar,
Nemo and PCManFM all implement it**. It is a session-bus call, so D-Bus starts the file manager if
it is not running.

The call goes out through `dbus-send`, not a D-Bus client library. No new dependency, and it
matches how P7.2 reached zenity.

`--print-reply` is load bearing. Without it `dbus-send` fires and forgets and always exits 0; with
it, the exit code says whether a file manager took the call, which is what selects the fallback.

**The fallback is `xdg-open` on the containing directory, and never on the path itself.** A
desktop with no `FileManager1` provider gets the directory opened by whatever claims
`inode/directory`. That is a poor answer, but it is the only portable one left, and it is strictly
better than launching an ebook reader.

A directory argument arrives with a trailing delimiter from `FileSystem::Path`. It is stripped
before the URI is built, so the file manager selects that directory inside its parent - again what
Windows does. Both paths agree: `.../Render/PBR/` opens `Render` with `PBR` selected.

#### Two things that are easy to get wrong here

**The URI must be percent-encoded.** `ShowItems` takes `file://` URIs, not paths. A space or a `#`
in a directory name silently addresses the wrong file, or no file, without it. Verified against a
real path containing both.

**A forked child of a threaded process must not allocate.** Every string the children need - the
URI argument and the fallback directory - is built before the first `fork`. The children only
`exec` and `_exit`.

The fork is now a **double fork**, so the helper is reparented to init. The old single fork left a
zombie behind on every click, because nothing waits for it. Checked with
`waitpid( -1, WNOHANG )` straight after the call: it returns `-1`/`ECHILD`, so no child is left.

#### How it was verified

From a scratch binary linked against `libEsoterica.Base.so`, calling `Platform::Win32::OpenInExplorer`
directly, then reading the resulting window titles with `xdotool`:

| Argument | Window that opened |
|---|---|
| `.../Data/Demo/Render/PBR/PBRDemo.map` | `PBR - Thunar`, with the file selected |
| `.../Data/Demo/Render/PBR/` | `Render - Thunar`, with `PBR` selected |
| `.../dir with spaces/a file #1.map` | `dir with spaces - Thunar`, with the file selected |

Both fallback branches were driven with a fake `dbus-send` on `PATH`: one that exits non-zero, and
one that is absent entirely. Both reached `xdg-open` with the containing directory, and the argv
log shows the `%20` and `%23` encoding going out on the real call.

**Not verified: clicking the menu items in the running editor.** The function every one of them
calls is verified, with both the file and the directory shapes they pass. Only Thunar was
exercised; the other four file managers are not installed here.

### 2026-09-01 - P7.2 `SystemDialogs_Linux.cpp`. The file and message dialogs are real

**The halting stubs are gone.** `SystemDialogs_Linux.cpp` now opens the desktop's own file
chooser and message boxes. Nothing in `EngineTools` calls `EE_UNIMPLEMENTED_FUNCTION` any more.

- Files changed: `Code/EngineTools/Core/SystemDialogs_Linux.cpp`. The port owns it. It was already
  in `LinuxSources.txt` and `Exclusions.txt` from Phase 3, so no build file changed.
- Upstream files edited: **none.** No [TouchedFiles.md](TouchedFiles.md) change.
- Acceptance criteria: P7.2 is met. `FileDialog::SelectFolder`, `Load`, `Save`, the four
  `LoadResourceOrDataFile` / `SaveResourceOrDataFile` overloads, and `MessageDialog::ShowInternal`
  all work.

#### No new dependency: the phase document's option 2 was already in the tree, twice over

The document said to start with `pfd` (portable-file-dialogs), and to fetch it into `External/` if
it was missing. It is missing, and it is not needed. `pfd` is a wrapper that shells out to
`zenity` or `kdialog` and parses the output. Both halves of that are already here:

- `Code/EngineTools/ThirdParty/subprocess/` spawns the process. P7.3 proved it works on Linux.
- `zenity`'s command line is what the desktops agree on. `qarma` is the Qt port of it and
  `matedialog` is the MATE fork, and **both take the same arguments**, so supporting all three
  costs one array of names.

So `External/`, `DownloadDependencies.sh` and the include paths are all unchanged.

**`subprocess_create` takes an argv array, not a command line.** Nothing needs quoting or
escaping, and a filename with spaces or quotes in it survives the round trip. That was checked.

#### What the tool is called with

| Entry point | Arguments |
|---|---|
| `SelectFolder` | `--file-selection --directory --filename=<dir>/` |
| `Load` | `--file-selection [--multiple --separator=\n] --title= --filename= --file-filter=...` |
| `Save` | `--file-selection --save --confirm-overwrite --title= --filename= --file-filter=...` |
| `MessageDialog` | `--info` / `--warning` / `--error` / `--question`, plus button labels |

**`ExtensionFilter` needed no header change, and the escalation the document warned about did not
happen.** `m_filter` holds the Windows double-NUL wide-string format, and it is left alone. The
filter argument is built from `m_extension` and `m_displayText` instead, which is what the phase
document asked for. The constructor is otherwise a copy of the Windows one, so both platforms fill
the struct identically.

**`--confirm-overwrite` is deprecated in zenity 4 and warns on stderr.** It is passed anyway:
zenity 4 confirms overwrites on its own and ignores the flag, and zenity 3 needs it. Dropping it
would silently overwrite files for anyone on the older version.

**`--no-markup` on every message.** Messages carry file paths and type names, so `<` and `&` in
them are text, not Pango markup.

#### Seven Win32 message box layouts onto three buttons

The tool has an OK button, a Cancel button, and one extra button. The extra button reports itself
by **printing its own label and exiting non-zero**, which is why the label is compared against the
output. The exit code alone cannot tell "extra button" from "cancel".

| `MessageDialog::Type` | OK label | Cancel label | Extra button |
|---|---|---|---|
| `Ok` | - | - | - |
| `OkCancel` | OK | Cancel | - |
| `YesNo` | Yes | No | - |
| `YesNoCancel` | Yes | Cancel | No |
| `RetryCancel` | Retry | Cancel | - |
| `AbortRetryIgnore` | Retry | Abort | Ignore |
| `CancelTryContinue` | Try Again | Cancel | Continue |

`AbortRetryIgnore` maps Abort onto Cancel and Ignore onto Continue, which is what the Win32
implementation does with `IDABORT` and `IDIGNORE`.

#### With no display, or no tool, the message still reaches the log

`FileDialog` returns an empty result, which every caller already reads as a cancel. `MessageDialog`
logs the message and returns `Cancel`, exactly as the Phase 3 stub did. **That is the case the
ResourceCompiler workers run in**, and it is why the check is for `DISPLAY` and `WAYLAND_DISPLAY`
rather than for the tool alone. The result is resolved once and cached: a missing tool is a
property of the machine, not of the call.

#### How it was verified

The dialogs cannot be driven by a test, so the code was exercised from a scratch binary linked
against `libEsoterica.Engine.Tools.so`, with a fake `zenity` first on `PATH` that logs its argv
and returns a scripted exit code and stdout. That covers the parts a person clicking cannot check
repeatably:

- Multi-select returns both paths, including one with spaces in the name.
- `Save` on a name typed without an extension comes back as `newmap.map`. `ValidateResult` appends
  it, the tool does not.
- `SelectFolder` on a real directory returns a path with `IsDirectoryPath()` true.
  `GetFullPathString` stats the path and appends the trailing slash, so nothing extra is needed.
- All three `YesNoCancel` outcomes map correctly: exit 0 to `Yes`, exit 1 with `No` on stdout to
  `No`, exit 1 with nothing on stdout to `Cancel`.
- Both fallbacks log and return `Cancel`.

Then with the **real** zenity: the child process was inspected in `/proc/<pid>/cmdline` while the
dialog was on screen, and closing it returned an empty result cleanly. **What is still unproven is
a human clicking a file in the real dialog**, and the appearance of the dialogs themselves.

#### A follow-up this unblocks in the Resource Server

`ResourceServerApplication_Linux` logs a warning and exits when clients are connected, instead of
asking. The P7.3 entry records why: `MessageDialog::Confirmation` returned `Cancel` unconditionally,
so asking would have refused every exit and trapped the user in a window that would not close. That
is no longer true. The confirmation can now be restored to match Windows.

### 2026-08-31 - Set 0 for an indirect draw, and RenderDoc attaches at last

**Every material draw in the frame was invalid, and the frame is now clean.** Thirty seconds with
host validation on and zero validation messages, where it previously halted on the first mesh
draw. **The window is still black** - see "What is still unknown" below, which is now a short list.

#### An indirect draw's set 0 is declared but never written

A Direct3D 12 command signature writes the root constants and binds the root CBV per command as
the GPU walks the argument buffer, so the engine binds neither on the CPU.
`RenderPass_ForwardShading.cpp` calls `CmdSetRootConstants` with a **null** pointer, which the
reference treats as a no-op, and never calls `CmdSetRootParameter` for the root CBV at all.

P5.17 moved those reads into the shader, but `RHI.esh` keeps the `ConstantBuffer` declarations on
purpose - they are the direct-bind fallback, and they preserve the reflected layout the command
signature is built from - so the shader **statically** uses set 0, and Vulkan requires a
statically used binding to be bound whether or not the shader reaches it.

`CmdExecuteIndirect` now fills only the bindings the engine left alone, tracked by a mask on the
command buffer and pointed at the root constant ring. **A direct draw gets no such help**, so a
genuinely forgotten binding still fails validation there rather than quietly reading the ring.
Nothing consumes ring space: reserving a block per indirect draw would burn a 64KB ring that
asserts rather than wraps, for bytes nothing reads.

**This never fired on the first machine** because `CmdSetPipeline` dropped every mesh draw there
for want of hardware, so `CmdSetRootConstants` returned early and no draw was ever recorded.

#### RenderDoc could never connect on Linux

`CreateContext` probed for the library with `dlopen( RTLD_NOLOAD )` **before** `vkCreateInstance`.
On Windows RenderDoc injects itself before `main`, so `GetModuleHandleA` finds it at any point. On
Linux it arrives as an implicit Vulkan layer and the loader only maps `librenderdoc.so` while
servicing `vkCreateInstance`, so the probe always missed and `m_pRenderDocAPI` stayed null.

The probe now runs just after `vkCreateInstance`. **The ini key is `Enable_Render_Doc`** under
`[Render:RHI]`, and the run also needs `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` and RenderDoc's layer
registered (`renderdoccmd vulkanlayer --register --user`). RenderDoc is not packaged by Ubuntu; it
is an official tarball, and `renderdoc_1.45` matches the `renderdoc_app.h` pin already in
`DownloadDependencies.sh`.

#### What was ruled out, and how

Everything up to the mesh shader is confirmed working. Recorded so the next session does not
repeat any of it:

| Checked | Result |
|---|---|
| Map, entities, components | Loads. 3 entities, components register on task threads |
| Geometry registered | **9286 clusters** in the `ComplexSurfacePBR` bucket |
| Cluster culling on the GPU | **Works.** Read the counter back to the CPU: `drawCounters = 1 0 0 0 0 0`, which is right for 9286 clusters packed into one command |
| Command signature | `hasRoot=1 stride=64 rcOffset=0 cbvOffset=32 drawArgOffset=40`, and the offsets match the `DrawArgument` layout |
| Indirect push constants | Captured and decoded: address non-zero, `stride=64`, `commandIndexBase=0`, offsets as above |
| The draw call itself | `vkCmdDrawMeshTasksIndirectCountEXT`, `offset=40`, count buffer bound, `maxDrawCount=1`, `stride=64` |
| `DrawIndex` in mesh shaders | Present in **all 13** captured `MS_main` modules, with the `DrawParameters` capability |
| Presentation | Works. Forcing every colour attachment to clear magenta turns the window magenta |

**Measure late, not early.** Several of these look wrong if sampled in the first frames, because
the map is still loading: cluster capacity reads 1, which is the empty baseline, and no components
have registered yet. A fire-once diagnostic at startup gives the wrong answer to every question
above. That cost a session.

#### What is still unknown

The mesh shader runs, with a correct command and correct push constants, and no geometry appears.
What it reads out of the argument buffer at runtime, and whether it emits any primitives, needs a
**replay**, not a capture: the argument buffer is GPU-written mid-frame, so its contents are not in
the capture's initial state. `renderdoccmd convert -c zip.xml` gives the full chunk list and every
CPU-supplied buffer as XML, which is how the table above was checked, but not GPU-written
contents.

Open in `qrenderdoc` and look at the first `ComplexSurfacePBR DepthOnly` draw: the mesh output, and
the contents of the draw argument buffer at that point.

**A caution for whoever does that.** Forcing `MS_main` to emit a fixed triangle to isolate the
draw path does not work naively: an early `return` makes the resource declarations dead code, DXC
strips them, the reflected layout stops matching the root signature, and the engine asserts at
startup. `RHI.esh` documents that hazard. Keep every declared resource referenced.

#### Files

- Files changed: `Code/Base/Render/RHI_Vulkan.cpp`. The port owns it.
- Upstream files edited: **none.** No [TouchedFiles.md](TouchedFiles.md) change.
- Acceptance criteria: no change. Phase 6 criterion 2 is still not met.

### 2026-08-31 - P7.3 Resource Server. **It builds, serves, spawns workers and draws its UI**

`Build/Linux_Release/Esoterica.Applications.ResourceServer` runs. It creates its window, listens
on `127.0.0.1:5556`, spawns three `Esoterica.Applications.ResourceCompiler -worker N` processes,
and draws the whole docked UI: the server panel, the connected worker list, the requests table,
packaging and recompilation blockers, with the custom title bar. **`ninja -k 0` over the whole
tree now fails nowhere.**

- Files added: `Code/Applications/ResourceServer/Linux/ResourceServerApplication_Linux.{h,cpp}`,
  listed in `LinuxSources.txt` under a new `[Esoterica.Applications.ResourceServer]` section.
- `Code/Applications/ResourceServer/ResourceServerApplication.cpp` is excluded in
  `Exclusions.txt`. It is `_tWinMain`, a `NOTIFYICONDATA` tray icon, an `ITaskbarList3` progress
  overlay and a named mutex. Its header is the only place `<shellapi.h>` and
  `Application_Win32.h` come in, and only that `.cpp` includes it, so one exclusion drops both.
- Upstream files edited: `ResourceServerUI.cpp` (the 3-line `PlatformUtils_Linux.h` include) and
  `Code/Base/_Module/BaseModule.cpp`. Both in [TouchedFiles.md](TouchedFiles.md).

#### Decision (a): GUI, not headless - and the phase document's reasoning held

`LinuxApplication` and the imgui backend already existed, so the GUI cost almost nothing. The
whole `.cpp` is 200 lines and most of it is copied from the Win32 sibling unchanged.

#### Decision (b): worker processes needed no new code at all

`ResourceServerWorker.cpp` already uses the vendored `subprocess` library
(`Code/EngineTools/ThirdParty/subprocess/`), not `CreateProcess`. It compiled and ran on Linux
with no platform split and no change. The phase document asked for this to be checked before
writing `fork` and `execv` by hand; the check paid off.

#### What the Win32 application has that the Linux one does not

| Win32 | Linux | Why |
|---|---|---|
| System tray icon, `Shell_NotifyIcon` | Nothing | A tray needs libayatana-appindicator or a StatusNotifierItem over D-Bus. Neither is present, and several desktops have no tray at all |
| `StartMinimized`, and `HideApplicationWindow` on first show | Neither | Both only make sense with a tray to restore from. Without one they strand the user in an invisible process |
| `WM_CLOSE` hides the window | Close exits | Same reason |
| `ITaskbarList3` progress and busy overlay icons | Nothing | No portable equivalent. The busy state is already on screen in the request list, and the window is visible now |
| A named single-instance mutex | An `flock` on a lock file | **Kept, and it turned out to be load bearing.** See below |
| `MessageBox` confirming exit with clients connected | A logged warning, and the exit proceeds | `MessageDialog::Confirmation` on Linux logged and returned false until P7.2. Asking would have refused every exit and trapped the user in a window that will not close. **P7.2 fixed that, so this can now be restored** |

#### The single-instance guard is not a nicety, and cutting it was the wrong call

It was cut first, on the reasoning that a second server fails to bind port 5556 and reports
`Cant open network connection on port: 5556`, which is the message the mutex existed to give.
**Running it disproved that.** The second instance reports the error, then dies in an assert:
`ResourceServerContext::Initialize` allocates its `CompilerRegistry` before it opens the socket
and does not delete it on the failure path, so `~ResourceServerContext` asserts on
`m_pCompilerRegistry == nullptr`. The Windows mutex is what stops that path ever being reached.

The guard is `flock( LOCK_EX | LOCK_NB )` on `$XDG_RUNTIME_DIR/EsotericaResourceServer.lock`,
falling back to `/tmp/EsotericaResourceServer.<uid>.lock`. **`flock`, not a PID file:** the
kernel drops the lock however the process dies, so a crashed server leaves nothing stale behind.

#### `IsMainWindowMinimized`, added to `LinuxApplication`

The Win32 loop skips its render work under `IsIconic( m_windowHandle )`. Rather than put SDL3 on
the Resource Server's include path for one flag test, `LinuxApplication` grew
`bool IsMainWindowMinimized() const`, defined out of line in `Application_Linux.cpp`. Both are
this fork's own files. `Toolchain.py`'s `LINUX_ONLY_SHEETS` is unchanged, and the Resource Server
still reaches SDL only through `Esoterica.Base`.

#### `EnsureResourceServerIsRunning` was the real blocker, and it is not in the Resource Server

`Code/Base/_Module/BaseModule.cpp:22` gated the whole function behind `#if _WIN32` and returned
false otherwise. So building the Resource Server would have changed nothing for the engine or the
editor: both still failed at `Couldn't start resource server` and still needed `-packaged`.

The body needed no rewriting. Every call in it - `GetProcessID`, `GetProcessPath`,
`GetCurrentModulePath`, `KillProcess`, `StartProcess` - is `Platform::Win32::`, and
`PlatformUtils_Linux.h` aliases `namespace Win32 = Linux` over working implementations. The edit
is two lines. **`BaseModule.cpp` was not on the survey list, so this was escalated and approved
before the edit**, per Conventions rule 2.

#### `GetProcessID` could never have matched, and it would have matched the wrong process

Fixed in `Code/Base/Platform/PlatformUtils_Linux.cpp`, this fork's own Phase 1 file. It compared
the requested name against `/proc/<pid>/comm`, **which the kernel truncates to 15 characters**.
`Esoterica.Applications.ResourceServer` and `Esoterica.Applications.ResourceCompiler` both
truncate to `Esoterica.Appli`, so the comparison could never succeed and, had the names been
shorter, would have confused the server with the compiler. It now reads the basename of
`/proc/<pid>/exe`, which is the full path. `readlink` fails with `EACCES` for another user's
process, which is the right answer: every caller is looking for a process it started itself.

#### One piece of local configuration, and it is not in the repository

The default executable names are `EsotericaResourceServer.exe` and
`EsotericaResourceCompiler.exe`. `Build/Linux_<configuration>/Esoterica.ini` is untracked and
hand written, so add:

```ini
[Resource]
Resource_Server_Exe_Name = Esoterica.Applications.ResourceServer
Resource_Compiler_Exe_Name = Esoterica.Applications.ResourceCompiler
```

The key names are the reflected `FriendlyName` with spaces replaced by underscores, which is what
`Settings::GenerateSectionAndKeyIDs` builds. Without them the editor spawns nothing and reports
`Couldn't start resource server (.../EsotericaResourceServer.exe)`.

#### What is not verified

- **The title bar buttons.** Minimize, maximize and close are drawn and were not clicked.
- **Serving a resource to a client.** The server listens and the editor connects, but no resource
  has travelled the wire yet. That is P7.5.
- **The `OpenInExplorer` context menu items** at `ResourceServerUI.cpp:799` and `:811` compile and
  were not exercised. That is P7.4.
- **Windows.** `ResourceServerApplication.cpp` and its header are untouched, the `ResourceServerUI.cpp`
  and `BaseModule.cpp` edits are `#elif` branches, and no `.vcxproj` changed - but no Windows
  build has been run.

#### This machine still cannot render, and that is unrelated

Both the Resource Server and the editor halt at
`vkCreateShaderModule(): SPIR-V contains an 16-bit OpVariable with Input Storage Class, but
storageInputOutput16 was not enabled` **when host validation is on**. This machine's Intel UHD
620 lacks `storageInputOutput16`, and its NVIDIA MX250 is skipped for missing
`VK_EXT_mutable_descriptor_type`. It is the shared render path, identical for both applications,
and it has nothing to do with P7.3. With `Enable_Host_Validation = false` the Resource Server runs
and draws correctly, which is how everything above was measured. Chase this on the RTX 3090
machine or not at all.

---

### 2026-08-31 - P7.1 `EditorApplication_Linux`. **The editor runs, and one assert blocked it**

`Build/Linux_Release/Esoterica.Applications.Editor` builds, links, launches, initialises and
reaches the frame loop. It stops where the engine stops: the GPU hang after a complete frame,
which is [deferred on purpose](#deferred-on-purpose).

> **Superseded on the same day.** That hang is fixed - see the NVIDIA entry below. The editor has
> not been re-run since, so what it does now is unmeasured rather than known.

- Files added: `Code/Applications/Editor/Linux/EditorApplication_Linux.{h,cpp}`, listed in
  `LinuxSources.txt`. Upstream files edited: none.
- It mirrors `Win32/EditorApplication_Win32.{h,cpp}` line for line. `EditorEngine` is copied
  across unchanged. `EditorApplication` derives from `LinuxApplication`, keeps the `Borderless`
  init option, and drops the two `EE_ENABLE_LPP` hooks.
- **No new SDL3 include path.** The file only takes the address of the `SDL_Event`, and
  `Application_Linux.h` forward declares the type, so `Toolchain.py`'s `LINUX_ONLY_SHEETS` is
  unchanged. The Editor project still reaches SDL through `Esoterica.Base`.
- **The borderless window needed no new code.** P6.2 already wrote `BorderlessWindowHitTest` and
  wired `SDL_SetWindowHitTest`; the hit test calls `GetBorderlessTitleBarInfo`, which this class
  now overrides. The phase document expected to iterate here and there was nothing to iterate on.
- `EditorApplication::FatalError` calls `MessageDialog::Confirmation`, which on Linux is the
  Phase 3 sibling that logs and returns `Cancel`. So a fatal error is logged and unsaved work is
  not offered for saving. **P7.2 fixed that**, and the save prompt now appears.

**A whole-tree `ninja -k 0` now fails in the ResourceServer and nowhere else.** The editor links
in every configuration.

#### What running it found: `Path::Split` asserts on every absolute Linux path

**This is a port defect, not a hardware gap, and it stopped the editor during initialisation.**
`Path::Split` (`Code/Base/FileSystem/FileSystemPath.cpp:256`) asserts
`currentDelimiterIdx > previousDelimiterIdx`. On Windows the first delimiter of `C:\a\b\` is at
index 2, so the assert holds. Every absolute Linux path starts with `/`, so the first delimiter
is at index 0 and the assert fires immediately, on `0 > 0`.

`FileRegistry::FindDirectory` and `FindOrCreateDirectory` are the only two callers, and both run
while the resource browser builds its tree. So the editor cannot start.

The fix is one character: `>` becomes `>=`. That emits a leading empty segment, which is exactly
what the depth arithmetic already wants - the empty string plays the part `C:` plays on Windows,
and both callers index with `m_dataDirectoryPathDepth + 1`, which `GetDirectoryDepth` derives
from the same delimiter count. Windows is unaffected: no Windows path it accepts today produces
two delimiters in a row.

**`Code/Base/FileSystem/FileSystemPath.cpp` was not in [TouchedFiles.md](TouchedFiles.md), so it
was escalated rather than changed. Approved, made, and registered there.** It is the second
upstream file this port edits for a reason other than an include switch, after
`RHI::MaxPendingFrames`.

**With the assert relaxed the editor runs to the frame loop.** It loads compiled data, creates
the Vulkan device, builds the tools UI, drops the mesh draws with the usual warning, and dies on
`result == VK_SUCCESS` in the present path - the same wall the engine hits, on the same hardware,
for the same reason.

**Two things about running it that are not obvious:**

- **`-packaged` is required, as it is for the engine.** Without it the editor tries to start
  `EsotericaResourceServer.exe`, fails, and calls `FatalError`, which opens an
  `SDL_ShowSimpleMessageBox`. That dialog is zenity, it is modal, and with no one to click it the
  process hangs forever rather than exiting. A terminal `timeout` will not kill it without
  `-s KILL`.
- **Host validation has to be off to get past the shader modules.** With
  `Enable_Host_Validation = true` the editor halts in `vkCreateShaderModule` on the
  `storageInputOutput16` VUID, which is one of the four hardware gaps in "What this machine still
  cannot do". ANV accepts the module with validation off.

**Not checked, because they need a frame this machine cannot draw:** criteria 3, 5, 8, 9 and 10.
The window is created, is borderless, and the hit test is installed; whether dragging and edge
resizing feel right cannot be judged from a window that never presents.

### 2026-08-31 - P7.0 The `EditorUI.h` include. The editor compiles

`Code/Applications/Editor/EditorUI.h` uses `EE::EditorTool` as a complete type in the
`IsToolOpen`, `GetTool` and `CreateTool` templates, and only forward-declares it. MSVC supplies
the definition through another header; clang does not, so every use failed with `member access
into incomplete type`.

- **One line added**: `#include "EngineTools/Core/EditorTool.h"`, in the existing include block.
  Registered in [TouchedFiles.md](TouchedFiles.md) under "Missing includes that MSVC supplies
  transitively". Windows is unaffected - the header was already reaching the definition, just not
  by name.
- `EditorUI.cpp` and `EditorTool_GamePreviewer.cpp` now compile in every configuration.

**What still fails, and it is the rest of the phase.** A whole-tree `ninja -k 0` fails in exactly
two places now:

- `Esoterica.Applications.Editor` does not link: `undefined reference to 'main'`. There is no
  `EditorApplication_Linux` yet. That is P7.1.
- `Esoterica.Applications.ResourceServer` does not compile: `shellapi.h` not found in
  `ResourceServerApplication.h:13`, and `Platform::Win32::OpenInExplorer` at
  `ResourceServerUI.cpp:799` and `:811`. That is P7.3.

Nothing else in the tree fails.
### 2026-09-01 - The last VUID filter is gone, and a fragment shader can say "per-primitive" after all

**The engine runs with no `VK_LAYER_MESSAGE_ID_FILTER` at all.** 30 seconds, host validation on,
zero validation messages, no device memory leaked, clean shutdown, correct frame. Only
`VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true` remains, and that is the layer's stale
bundled spirv-val, not this engine.

#### `storageInputOutput16`: one shader, one word

`DebugDraw.esf` passed a `TextureHandle` - a `uint16_t` - from its mesh stage to its pixel stage.
A 16-bit interpolant needs `storageInputOutput16`, **no NVIDIA part reports that feature**, and
the module fails to create outright with `VUID-VkShaderModuleCreateInfo-pCode-08740`. So the debug
draw pipelines never existed on this GPU and two VUIDs had to be filtered to get past it.

`EE_INTERSTAGE_HANDLE` in `RHI.esh` is `uint` on SPIR-V and the 16-bit handle on Direct3D 12. A
stage variable occupies one location either way, a handle never exceeds 16 bits, and the `~0` "no
texture" sentinel widens with the type it is assigned to, so no value changes.

**Surveyed, not guessed.** Compiling every `.esf` in the tree at every profile and grepping the
SPIR-V for `OpCapability StorageInputOutput16` found exactly two hits, both stages of that one
file, and finds zero now. That sweep is worth keeping - it is about a minute of compiler time and
it turns "which shaders have this problem" from a reading exercise into a fact.

#### `PerPrimitiveEXT`: no compiler patch needed, and one place it still cannot go

Vulkan matches a mesh shader's per-primitive output to a fragment input **only if the fragment
input carries `PerPrimitiveEXT` too**. Direct3D fixes that up at link time and makes no
distinction in the pixel shader at all; Vulkan cannot, because graphics pipeline libraries leave
no link step to infer it in. DXC decorates the mesh shader side by itself and has no way to
express the pixel shader side - `[[vk::perprimitiveEXT]]` does not exist and is silently ignored
as an unknown attribute (microsoft/DirectXShaderCompiler#6862).

The previous entry assumed that meant a fourth DXC patch. **It does not.** The SPIR-V inline
intrinsics reach it directly: `[[vk::ext_decorate( 5271 )]]` on the member, plus
`[[vk::ext_capability( 5283 )]]` and `[[vk::ext_extension( "SPV_EXT_mesh_shader" )]]` on the entry
point. `EE_PER_PRIMITIVE` and `EE_PER_PRIMITIVE_PIXEL_ENTRY` in `RHI.esh` wrap both, gated on
`__SHADER_TARGET_STAGE == __SHADER_STAGE_PIXEL` so the mesh shader is untouched and Windows sees
nothing.

`DebugDrawPrimitiveOutput` is now correctly decorated on both sides at matching locations.
**`PrimitiveOutput` is not, and cannot be with this DXC** - see [Still open](#still-open).

Four things that cost a try each, all invisible without `-Fc`:

| | |
|---|---|
| `[[vk::...]]` after `[RootSignature(...)]` | "expected identifier". The C++-style attributes have to come **first** |
| The decoration on a struct-typed member | Does not reach the flattened stage variable. It has to go on that struct's own members |
| The decoration anywhere in a struct the pixel shader copies into a local | Invalid SPIR-V, rejected by spirv-val. This is what blocks `PrimitiveOutput` |
| `-O0` vs `-O3` | `SV_CullPrimitive` in a pixel shader input struct is a `CullPrimitiveEXT` builtin with Input storage class, which is illegal - and vanishes at `-O3` because nothing reads it. **Check at the level the Reflector actually uses** |

#### Files

- Upstream files edited: `Code/Base/Render/RHI.esh` (53 added, 0 modified),
  `Code/Engine/Render/Shaders/Debug/DebugDraw.esf` (13 added, 3 modified),
  `Code/Engine/Render/Shaders/Renderer/RendererTypes.esh` (comment only). All `__spirv__`-gated,
  all no-ops on Direct3D 12. Registered in [TouchedFiles.md](TouchedFiles.md).
- Verified: `VK_LAYER_MESSAGE_ID_FILTER` removed entirely, 30 seconds, zero validation messages.

### 2026-09-01 - **The frame is correct.** Two stages disagreed on every location, and the pixel shader had no root arguments

**The pbrdemo map renders correctly on Linux**: lit, textured, shadowed geometry, a sky, a
reflective ground plane. 30 seconds, host validation on, **zero validation messages**, no device
memory leaked, clean shutdown. Two defects, both found by measurement, neither where the previous
session's handoff pointed.

**The handoff's prime suspect was wrong, and cheap to clear.** It named
`StaticMeshVertex::GetPosition` in `MeshData.esh` and 16-bit packing. Compiling the struct's
decode to SPIR-V and reading the offsets settles it in one step: positions at byte 0/2/4, normals
at 6/8/10, UVs at 12 and 20, colour at 28 - identical to the C++ side. `MeshCluster` is the same
story, and its signed 24-bit anchor bitfields come out as `OpBitFieldSExtract 0 24`, correct.
**Do this before believing any layout hypothesis**: `dxc -spirv -Fc` on a ten-line shader that
touches the struct costs a minute and answers the question exactly.

#### The mesh shader and the pixel shader disagreed on every `Location`

**Direct3D matches inter-stage variables by semantic name. Vulkan matches them by `Location`,
and DXC numbers those in declaration order.**

`MS_main` declares `out primitives PrimitiveOutput` *before* `out vertices VertexOutput`, so the
mesh shader gave TEXCOORD4-9 locations 0-5 and TEXCOORD0-3 locations 6-9. `PS_main` takes
`VertexOutput` first, so the pixel shader numbered the same ten semantics 0-9 in order. Nothing
lined up:

| Semantic | MS output | PS input |
|---|---|---|
| TEXCOORD0 `m_worldPosition` | 6 | 0 |
| TEXCOORD2 `m_uv` | 8 | 2 |
| TEXCOORD4 `m_data0` | 0 | 4 |
| TEXCOORD6 `m_primitiveFlags` | 2 | 6 |

The pixel shader read the primitive flags where the world position was written. The visible
result was **the orange wireframe overlay** - `RENDERER_DEBUG_FLAG_SHOW_WIREFRAME` happened to
fall out of the garbage, and `float3( 1.0, 0.4, 0.0 )` in `MaterialShaderPBR.esh:389` is exactly
the colour on screen. What the session before called scrambled geometry was **not geometry at
all**: it was correct triangles, traced in wireframe by a debug path that was never asked for,
over surfaces shaded from garbage.

Fixed with `-fvk-stage-io-order=alpha`, Linux only. Both stages sort by semantic string before
numbering, so the order the entry point happens to declare its parameters in stops mattering.
DXC already forces this for hull and domain shaders, for exactly this reason. **No shader file
changes.** There are no vertex-buffer inputs anywhere in this engine - geometry is pulled in the
mesh shader, and `RHI_Vulkan.cpp` never builds a `VkVertexInputAttributeDescription` - so VSIn
ordering, the one case where a location is a contract with the CPU side, cannot be affected.

**Nothing diagnoses this.** Both modules are internally valid, the location sets are the same
size, and `SV_Position` is a builtin that never had a location - so the geometry rasterises in
the right place and only the shading is wrong.

#### The pixel shader read its root arguments out of two zeroed statics

With the locations fixed the geometry appeared, correctly shaped and correctly textured, and
**every lit surface was still pure black.**

P5.17 made the shader pull its own root arguments out of the indirect argument buffer, and
`DefaultMeshShader.esh` renames `RootConstants` and `RootCBV` onto two statics **for the whole
translation unit**. `MaterialShaderPBR.esh` is included into the same file, so `PS_main` reads
those statics too - and only `MS_main` ever calls the init that fills them. A static nothing
assigns is zero. The pixel shader's SPIR-V has **no set 0 binding at all**, which is the tell:
`grep DescriptorSet` on the module shows set 1 and nothing else.

So every `RootCBV` field the pixel shader read came back 0. `m_dfgTexture`, `m_radianceTexture`
and `m_irradianceTexture` were handle 0, so `ComputeIBL` returned black; `m_numDirectionalLightPages`
was 0, so the light loop never ran. Albedo was correct throughout, which is what made this look
like a lighting bug rather than a binding bug - `m_shaderDataBuffer` was 0 too, and heap slot 0
happens to be that buffer.

`EE_INDIRECT_PIXEL_ENTRY_INIT` in `RHI.esh`, one line at the top of each affected `PS_main`,
empty on Direct3D 12.

**There is no `DrawIndex` in a fragment shader**, so it reads command `m_commandIndexBase`, which
`CmdExecuteIndirect` sets to 0 for a draw. That is exact for the root CBV:
`BucketResolve.esf:41` writes the same `RootCBV.m_deviceAddress` into every command it emits, so
the CBV does not vary across the argument buffer. It is not exact for root constants that do vary
per command, and that is recorded in [Deferred on purpose](#deferred-on-purpose).

#### How each step was measured

Every one of these was a single-line shader change, a rebuild and a screenshot, and each
eliminated one branch of the tree:

| Change | What it proved |
|---|---|
| `IsTriangleCulled(...) && false` | Not the software cull |
| `triangleIndices = min( triangleIndices, numClusterVertices - 1 )` | Pixel-identical frame, so no index was ever out of range |
| Colour pass depth `Equal` to `GreaterEqual`, then `m_depthTest = false` | Not a depth-prepass mismatch - and the frame that came back was **clean wireframe**, which named the defect |
| `debugFlags = 0; debugVisMode = NONE` | The wireframe overlay was supplying every visible pixel |
| `debugVisMode = ALBEDO`, then `NORMALS` | Geometry, UVs, textures and normals all correct |
| `result.rgb += float3( 0, 0.25, 0 )` inside the lighting branch | The rocks do enter it, so `DISABLE_LIGHTING` was not set |
| `result.rgb = float3( IBL, directLight, shadow )` | IBL 0, direct light 0, shadow 1 - uniformly, everywhere |

**Run it on a private display.** `Xvfb :77 -screen 0 5120x1440x24` plus
`xwd -id <window>` captures the engine window without photographing the developer's desktop, and
makes the frame reproducible: the same 22-second capture is pixel-identical run to run, which is
what makes an A/B of a one-line shader change worth anything.

**The window position comes from `EsotericaEngine.layout.ini`**, so the Xvfb screen has to be at
least as large as that saved position or the window lands off-screen and every capture is black.

#### Files

- Files changed: `Code/Applications/Reflector/ShaderReflection/ShaderReflection_ShaderCompiler.cpp`
  (the port owns its Linux branches).
- Upstream files edited: `Code/Base/Render/RHI.esh` (19 added, 0 modified),
  `Code/Engine/Render/Shaders/Renderer/MaterialShaderPBR.esh` and
  `Code/Engine/Render/Shaders/Debug/DebugDrawMesh.esf` (2 added each, 0 modified). All additions,
  all no-ops on Direct3D 12. Registered in [TouchedFiles.md](TouchedFiles.md).
- Acceptance criteria: **Phase 6 criterion 2 is met.** A map renders, and it renders correctly.

### 2026-08-31 - **Geometry renders.** Every triangle was culled, and every index read zero

**The engine draws lit, shaded, textured geometry on Linux.** Host validation on, zero validation
messages, 25 seconds. **The image is wrong** - the geometry is scrambled - and that is one further
defect, not a return of any of the earlier ones. Full handoff in
[Rendering: where we are](#rendering-where-we-are); this entry records the two fixes.

**Two independent bugs. Either one alone produces a completely black frame**, which is why fixing
either in isolation showed nothing and made both look like something else.

#### The front face was inverted

The comment on that line said what it was: "**the classic porting bug and it is reasoned, not
verified**". The reasoning ran - Direct3D 12 sets `FrontCounterClockwise = ( m_frontFace ==
ClockWise )`, which is already an inversion of the name; the Vulkan viewport reverses winding with
its negative height; so inverting the inversion lands back on the name. That counted the double
negative once too often. **Every triangle in the engine was back-face culled.**

Measured this time, not reasoned. With the mapping inverted a fullscreen triangle rasterises with
culling left on; with the old mapping the same draw wrote nothing while `vkCmdClearAttachments`
inside the same render pass still wrote.

The Y flip in `CmdSetViewport` is the other half of that pair and is unchanged.

#### The cluster triangle buffer had no format

`DefaultMeshShader.esh` declares `Buffer<uint>`, a **typed** buffer.
`ResourceLoader_RenderMesh.cpp` created the buffer with a stride and no format, and the RHI decides:

```cpp
bool const isTypedBuffer = parameters.m_format != DataFormat::Undefined && !usageTypes.IsFlagSet( Raw );
```

So Vulkan wrote a **storage buffer** descriptor where the shader wanted a **uniform texel buffer**.
**A mutable descriptor heap swaps one for the other in silence**: no validation error, and every
read returns 0. Every triangle index was 0, so every primitive was degenerate.

Direct3D 12 makes a structured SRV there and tolerates reading it as `Buffer<uint>`, which is why
it was never noticed. The fix names the format, guarded to Linux, so **Windows is bit for bit
unchanged**. It is an upstream engine file and is registered in
[TouchedFiles.md](TouchedFiles.md).

#### What this cost, and the lesson

Most of a session went into geometry-level hypotheses that were all downstream of these two. The
[measurement traps](#measurement-traps-that-produced-confidently-wrong-answers) table is the more
useful half of this entry: a fire-once diagnostic at startup, RenderDoc's index-driven mesh view,
a readback with the wrong `oldLayout`, and a log de-duplicated by adjacency each produced a
confident, written-down, wrong conclusion.

**The technique that actually worked** was `vkCmdClearAttachments` inside the live render pass right
after the draw. It shares the attachment, image view, render area and present path with the draw,
and bypasses culling and fragment shading - so it isolates "the primitive was culled" in one step.

#### Files

- Files changed: `Code/Base/Render/RHI_Vulkan.cpp` (the port owns it).
- Upstream files edited: `Code/Engine/Render/ResourceLoaders/ResourceLoader_RenderMesh.cpp`, 8
  added, 0 modified, Linux-guarded. Registered in [TouchedFiles.md](TouchedFiles.md).
- Acceptance criteria: **Phase 6 criterion 2 is now half met** - a map renders, but not correctly.
  Say which half, as that criterion asks.

### 2026-08-31 - A discrete NVIDIA GPU finds four defects, and the GPU hang is gone

**The engine runs a continuous frame loop on an RTX 3090 and shuts down clean.** Forty seconds, no
`VK_ERROR_DEVICE_LOST`, no kernel `Xid`, and `ReportDeviceMemoryLeaks` reports "No device memory
leaked". **The window is black: no geometry.** See "What still stops a picture".

**A second machine did this, not new work on the first.** NVIDIA GeForce RTX 3090, driver
580.173.02, Ubuntu 24.04.4, X11. It closes four of the five hardware gaps in the table above:
`VK_KHR_fragment_shader_barycentric`, `VK_EXT_mesh_shader`, `VK_EXT_mutable_descriptor_type` and
`shaderSharedInt64Atomics` are all present. **`storageInputOutput16` is still absent** - NVIDIA
does not expose it - so that row stands on both machines.

**The gaps closing is what found the defects.** Every one below sits on a path no previous GPU
could reach: three need mesh shader hardware or a real shading rate extension, and one needs
dedicated queue families. All four are in `RHI_Vulkan.cpp`, which the port owns. **No upstream file
is edited.**

#### The `GPU hang` had a cause, and it was ours

The `VK_ERROR_DEVICE_LOST` recorded above as unresolvable here **was not the dropped mesh draws.**
It was an illegal command in the transfer command buffer, and the kernel said so all along:
`NVRM: Xid 32`, an invalid or corrupted push buffer stream.

**`BeginCommandBuffer` called `vkCmdSetFragmentShadingRateKHR` on every command buffer**, transfer
and compute included. That call requires `VK_QUEUE_GRAPHICS_BIT`. It was guarded on the entry point
being non-null, which tests whether the *device* has the extension, not whether *this queue* can
run the command. The first machine never saw it because neither GPU there enabled
`VK_KHR_fragment_shading_rate` at all.

`VulkanCommandPool` now carries the `VkQueueFlags` of its family, and the entry point is left null
on a non-graphics pool. `BeginCommandBuffer`'s existing null check then does the right thing, and
`CmdSetShadingRate`'s assert names the caller if a pass ever asks for a rate on such a buffer.

#### A Vulkan barrier may only name stages its queue can run

Directly behind it, the same shape on the compute queue:
`VUID-vkCmdPipelineBarrier2-dstStageMask-03850`, a barrier with `VERTEX_SHADER` as its destination
recorded on a compute-only family.

**Upstream is not wrong.** `D3D12_BARRIER_SYNC_*` carries no queue restriction, so the engine
transitions a resource on whichever queue owns the work while naming the stage that reads it next,
and that reader is often on another queue. Only a device with dedicated families notices. The RTX
3090 has a transfer-only family and a compute-only one; an Intel iGPU exposing one universal family
accepts everything.

`FlushBarriers` now clamps every stage mask to the queue's capabilities, in the one place every
barrier passes through on its way to the device. **Dropping those stages is correct, not a
workaround**: a barrier orders work within one queue, and the queue-to-queue half is already
carried by the timeline semaphore every submit waits on (`RecordQueueOrderingWait`, P5.3). Where
clamping would empty a mask, it becomes `ALL_COMMANDS` rather than `NONE`, because an access bit
with no compatible stage is a fresh validation error. **That is a new `ALL_COMMANDS` site** and
belongs with the others listed above.

#### Two device-creation defects

**The mesh shader feature struct was echoed back as an enable request.** The query result and the
`vkCreateDevice` request were the same struct, so every bit the device supported was asked for,
including `multiviewMeshShader` - which additionally requires `multiview`, which nothing enables.
`vkCreateDevice` refused the device outright
(`VUID-VkPhysicalDeviceMeshShaderFeaturesEXT-multiviewMeshShader-07032`). Only the two bits the
engine uses are asked for now. **The same query-as-request pattern is still in place for the
shading rate, acceleration structure and ray tracing blocks.** None of them has a cross-dependency
VUID today, so none was touched.

**`shaderStorageTexelBufferArrayNonUniformIndexing` was never enabled**, and the shaders declare
the capability, so `vkCreateComputePipelines` rejected the first pipeline it was given. This is the
other half of the pair P5.5 missed: `descriptorBindingStorageTexelBufferUpdateAfterBind` was added
when that was found, and the matching non-uniform-indexing bit was missed with it. Both the
requirement check and the enable list carry it now.

#### What still stops a picture

**Set 0 is not bound at a mesh draw.** With validation on, the frame reaches
`vkCmdDrawMeshTasksIndirectCountEXT` for the `ComplexSurfacePBR DepthOnly Pipeline` and stops on
`VUID-vkCmdDrawMeshTasksIndirectCountEXT-None-08600`: the pipeline statically uses set 0 and
nothing is bound there. P5.17 routes a command's root data through the argument buffer instead of
set 0, and the shader still declares the set.

**This is P5.17's mesh half, and it has never run anywhere.** The indexed-draw half was verified on
the first machine; the mesh half could not be, because `CmdSetPipeline` dropped every mesh draw
there for want of hardware. Treat "P5.17 is done" as true for indexed draws only.

**`storageInputOutput16` now blocks a real pipeline rather than producing a warning.**
`DebugDraw.esf` passes a `uint16_t` handle from the mesh stage to the pixel stage, and with mesh
shaders enabled those modules are created for the first time. Two VUIDs guard it, `08740` and
`06334`. Surveying past it needs
`VK_LAYER_MESSAGE_ID_FILTER=0x6e224e9,0x715035dd`.

#### Not chased, deliberately

The black frame was not investigated beyond finding the set 0 error. The mesh draw is the geometry,
and there is no point reading a frame whose geometry pass is undefined. Fix set 0 first.

#### Files

- Files changed: `Code/Base/Render/RHI_Vulkan.cpp`. The port owns it.
- Upstream files edited: **none.** No [TouchedFiles.md](TouchedFiles.md) change.
- Acceptance criteria: **Phase 6 criterion 9 is now met** - the engine reaches and runs a frame
  loop. Criterion 2 is still not met, because the window is black. Criterion 8 holds, measured
  again here.

### 2026-08-31 - The SDL3 requirement check misses two X11 packages

**Bootstrapping a second machine found a gap in `requirements_sdl3`.** It checks nine pkg-config
packages and passes, and SDL's CMake then fails on a tenth. Twice, one package per run:
`XSCRNSAVER` first, then `XTEST`.

That is the one thing the check exists to prevent.
[03-Dependencies.md](03-Dependencies.md#downloaddependenciessh) says it must "check for the
required system packages first, and fail with a message that lists the missing ones. Do not fail
deep inside a nested CMake build."

SDL's `CheckX11` macro calls `SDL_missing_dependency` for nine extensions and stops at the first
one it cannot find. Seven arrive with packages the check already names. **`libxss-dev` and
`libxtst-dev` arrive with nothing else**, so they are the two a fresh machine trips over. All
nine headers were checked directly rather than waiting for the next CMake run to name one.

Verified by pointing `PKG_CONFIG_LIBDIR` at a directory holding every system `.pc` file except
those two. The script now reports both in one message:

```
error: missing system packages for: sdl3

    sudo apt install libxss-dev libxtst-dev
```

**Not changed, and worth knowing.** `requirements_gamenetworkingsockets` checks that `protoc`
exists but not what version it is. A stale `protoc` earlier on `PATH` than the system one is
accepted, and the build then fails deep inside GameNetworkingSockets with "This file was
generated by an older version of protoc". `requirements_ctt` already version-checks Rust, so
there is a pattern to follow if this is worth closing.

### 2026-08-31 - The Shipping configuration links, for the first time

**`Build/Linux_Shipping/Esoterica.Applications.Engine` exists.** It had never been produced: the
link failed with undefined references to `EE::Animation::GraphController` from the Game module,
and nobody had tried that configuration since Phase 0 set it up.

**A defect in `NinjaGen.py`, not upstream's.** `topological_order` was a pre-order walk, which is
right for a chain and wrong for a fan. Given references `[Engine.Runtime, Game.Runtime]` it
emitted `Engine.Runtime` first, and `Game.Runtime`'s references into it were then already past.
A static archive is scanned once, in order, and only pulls the symbols undefined at the moment
the linker reaches it, so every archive has to appear **before** the archives it needs.

It is a post-order walk reversed now, which is the real reverse topological order. The link line
went from

```
libEsoterica.Engine.Runtime.a libEsoterica.Base.a libEsoterica.Game.Runtime.a
```

to

```
libEsoterica.Game.Runtime.a libEsoterica.Engine.Runtime.a libEsoterica.Base.a
```

**Only Shipping was affected.** Debug and Release build shared libraries, where the loader
resolves the graph and link order does not matter, which is why nothing noticed for seven phases.
Both still link unchanged.

A cycle between two archives cannot be fixed by ordering at all. Nothing in the solution has one;
the walk guards against it so a future one is a wrong link rather than a hang.

#### Two things the Shipping binary told us

**`ReportDeviceMemoryLeaks` reports "No device memory leaked", through the engine.** That is
**Phase 6 acceptance criterion 8**, which P6.8 recorded as met for the RHI path but never
exercised through the engine. It is met now.

**It has no data to run.** `Build/Linux_Shipping/CompiledData` does not exist - Phase 3 filled
the Release directory only - so the run fails at initialisation, shuts down cleanly and exits 0.
Compiling data for the other configurations is unfinished business, not a defect.

### 2026-08-31 - Image layouts, dropped mesh draws and the swapchain spelling. **The whole frame records**

**The engine records and submits a complete frame with zero validation errors.** Every pass runs:
cluster culling, the depth pass, the environment map capture, forward shading, post process,
imgui, the swapchain. **The GPU then hangs executing it** - `VK_ERROR_DEVICE_LOST` - which is the
one thing left between this port and a picture.

Priorities for this run were set explicitly: **blockers before correctness.** Several things below
are deliberately blunt, and each says so.

#### The RHI transitions attachments the engine never barriers

Direct3D 12 has no image layouts, so the engine binds a render target it has not transitioned and
nothing there is wrong. Vulkan needs the layout to match the use. A texture arrived at
`CmdSetRenderTargets` in one of two wrong states: still `UNDEFINED`, because `vkCreateImage` can
only start it there, or in whatever its last read left it, usually `SHADER_READ_ONLY_OPTIMAL`.

`CmdSetRenderTargets` now transitions it. The masks are `ALL_COMMANDS` and all-access on both
sides, because nothing at that call site says what last touched the image or what the pass will
do with it. **Blunt on purpose**; it belongs with the other `ALL_COMMANDS` sites.

#### One layout per subresource, not per image

`VulkanTexture::m_currentLayout` was a single layout. `RenderPass_GlobalEnvironmentMap` broke it:
it draws each cube face in turn, so face 1 is a colour attachment while face 0 has already been
transitioned to be sampled. One variable cannot describe that, and Vulkan rejected the barrier
that tried.

It is now `m_subresourceLayouts`, one entry per mip and array layer. **`RecordTextureBarrier`
splits a barrier** when the subresources it covers do not agree: the engine barriers whole
textures - `DeviceResourceStates::FlushBarriers` passes an empty region - and after a face-by-face
pass they legitimately disagree.

The old assert that the caller's belief matched the truth is gone. With the RHI transitioning
images on its own the two differ as a matter of course, and the barrier reads the truth anyway.

#### Mesh draws are dropped rather than halted

No GPU here has `VK_EXT_mesh_shader` and the engine's whole geometry path is mesh shaders, so
`CmdSetPipeline` used to halt. It now binds nothing, warns once, and every draw against that
pipeline is skipped - as are the push descriptor writes, which crashed the Intel driver when they
were made against a layout that was never bound.

**A frame missing its geometry is not a rendered frame.** This exists so everything either side of
the mesh path can be exercised on hardware that cannot run it at all, and it says so in the log.

#### The swapchain spelling reaches the pipelines

Every Linux surface measured offers only `VK_FORMAT_B8G8R8A8_*`. The engine hardcodes the RGBA
spelling for its present path - `ImguiRenderer.cpp:115` and `:236`,
`RenderPass_DebugDraw.cpp:487`, `RenderPass_PostProcess.cpp:19` and `:58` - because DXGI hands out
RGBA and nothing there has to ask. Dynamic rendering demands the pipeline's attachment format
match the image exactly.

`SubstituteSwapchainColorFormat` relabels **render targets only**: `RGBA8_sRGB` becomes
`BGRA8_sRGB` in `CreateTexture`, in the pipeline's colour formats and in the swapchain's own
candidate order, so all three agree. It is a relabel and not a swizzle - a shader's red output
lands in the format's red component wherever that byte sits - so the picture is unchanged. **A
sampled texture keeps the spelling it was given**, because its bytes really are in the order the
engine says.

It is unconditional rather than driven by what the swapchain chose, because pipelines are built in
`Shaders::Initialize`, before there is a window to make a surface from.

#### Where it stops

**`VK_ERROR_DEVICE_LOST`.** The frame is validation-clean, so this is the GPU faulting on
something validation cannot see: an out-of-bounds access, an unbounded loop, or a dispatch reading
uninitialised memory. Two candidates, in order:

1. **The cluster culling argument buffer is never cleared**, so its slots are uninitialised on the
   first frame. This is the clear that was deliberately deferred - see the P5.17 entry - and it is
   the first thing to try, because a garbage dispatch size hangs a GPU exactly like this.
   Skipping the indirect dispatch entirely did **not** stop the hang, so it is not the only cause.
2. Any of the compute shaders reading a buffer nothing wrote this frame.

`VK_EXT_device_fault` is not present on this driver, so `VK_LAYER_LUNARG_crash_diagnostic`
produces nothing. Bisecting by disabling passes is the tool that is left.

### 2026-08-31 - P5.17. **`CmdExecuteIndirect` executes, and the frame runs past it**

**The last `EE_UNIMPLEMENTED_FUNCTION` that stopped the frame is gone.** The engine records the
cluster culling indirect dispatch and runs on, into `RenderPass_GlobalEnvironmentMap`. Phase 5's
16 groups have all now executed at least once except the four that need absent hardware.

#### The shape that landed

Close to the plan, with two corrections it did not anticipate.

- **One push constant range**, the backend's only one, on every pipeline layout. It carries the
  argument buffer address, the stride, a command index base and the two root block offsets.
- **`CmdExecuteIndirect` fills it, then draws.** For a draw or a mesh dispatch that is one
  indirect call and the shader adds `DrawIndex`. For an indirect **compute** dispatch it is a CPU
  loop, one `vkCmdDispatchIndirect` per possible command with the index pushed each time, because
  Vulkan has no indirect dispatch count at all and no `DrawIndex` in a compute stage.
- **`RHI.esh` hides it.** `EE_DECLARE_INDIRECT_ROOT_CONSTANTS` and `_CBV` declare statics loaded
  once at the top of the entry point. Each shader maps `RootConstants` and `RootCBV` onto them
  with a two-line `#define`, so **no shader body changed**.
- Everything is inside `#ifdef __spirv__`, with an `#else` that is the declaration that was there
  before. The Direct3D 12 path is untouched.

#### Correction 1: the `ConstantBuffer` declarations cannot be removed

The plan said the declaration line was all that changed. **Removing it breaks the command
signature.** `EngineShader.cpp` builds the indirect argument list by walking the root signature's
`m_descriptorReflections`, which `CreateRootSignature` gets from SPIRV-Reflect over the module -
and **DXC strips a resource nothing references**, measured. With the bindings gone the signature
carries only its dispatch argument, its stride stops matching what the shader wrote, and
`CmdSetRootConstants` indexes an empty vector. That is what the first attempt did.

So `EE_DECLARE_INDIRECT_ROOT_CONSTANTS` still emits the `ConstantBuffer`, and the loader reads it
in a branch guarded on `EE_IndirectRoot.m_stride == 0`. `m_stride` is a push constant, so nothing
can prove the branch dead and the binding survives. It is also a real fallback: a shader declared
this way and bound directly still reads the descriptor the engine wrote.

#### Correction 2: `shaderDrawParameters`

`DrawIndex` carries the `DrawParameters` capability. `VK_KHR_shader_draw_parameters` is core in
Vulkan 1.1, which is what the plan checked, but **the feature bit still has to be enabled**. The
seventh `CreateContext` gap of this class.

#### Measured

- **`CmdExecuteIndirect` has no `EE_UNIMPLEMENTED_FUNCTION` left.** `RHI_Vulkan.cpp` is down to
  2 markers, both unreachable-caller markers, which is P5.17's first "done when".
- `./CompileShaders.sh` exits 0.
- **All 48 shader modules the engine creates pass `spirv-val --target-env vulkan1.3
  --scalar-block-layout`.**
- The two mesh shaders that the engine skips on this device were compiled directly with the
  Reflector's own flags: both carry `BuiltIn DrawIndex` and both validate, apart from the stale
  `VUID-CullPrimitiveEXT-CullPrimitiveEXT-07036` that Phase 4 and P6.8 already documented as a
  false positive.
- The engine now stops in `CmdSetRenderTargets`, on a depth texture still in
  `VK_IMAGE_LAYOUT_UNDEFINED`. **A texture layout defect, not this task's**; see below.

#### Not done, and it needs a decision

**The indirect compute loop is only correct while `maxNumCommands` is 1**, which is what the
pbrdemo scene happens to pass. Beyond that, a command past the GPU-written count reads a **stale
argument slot**: `Renderer_ForwardShading.cpp:730` clears both counter buffers each frame and
**nothing clears `m_ClusterCulling_ArgumentBuffer`**. Direct3D 12 never needed it to, because its
count stops the walk there; Vulkan has no such count for a dispatch.

**The fix is one line in the engine**, beside the two clears that are already there:

```cpp
RHI::CmdClearBuffer( pCommandBuffer_DepthPass, m_ClusterCulling_ArgumentBuffer.m_pBuffer, 0 );
```

An unwritten slot then dispatches `(0,0,0)`, a legal no-op. It is an upstream engine file and it
costs Windows one extra clear of a small buffer per frame, so **it is escalated, not made.** The
P5.17 plan predicted exactly this and said to escalate if the engine did not already clear.

#### The next wall

`CmdSetRenderTargets` reads `pVulkanTexture->m_currentLayout` for the depth attachment and finds
`VK_IMAGE_LAYOUT_UNDEFINED`. `RenderPass_ForwardShading.cpp:121` binds a depth target the engine
never barriered, because Direct3D 12 has no layouts to barrier into. **P5.6 and P5.9 territory**,
and the first defect past the indirect draw.

#### Files

- Upstream shader files edited: five, all inside `#ifdef __spirv__`, all in
  [TouchedFiles.md](TouchedFiles.md#shader-edits). **None is verified on Windows.**
- `Code/Base/Render/RHI_Vulkan.cpp`, which the port owns.

### 2026-08-31 - `NoDescriptors` and the tessellation stages. **The frame is validation-clean to P5.17**

**The engine now runs from startup to `CmdExecuteIndirect` with host validation on and zero
validation messages.** Two defects stood between it and that, both found by running with
validation once open question 8 let the engine reach its frame loop.

#### `BufferFlags::NoDescriptors` answered two questions with one flag

`CreateBuffer` cleared `descriptorTypes` outright when it saw `NoDescriptors`
(`RHI_Vulkan.cpp:5243`) and then derived the `VkBufferUsageFlags` from the cleared copy.

**Those are separate questions.** On Direct3D 12, "no descriptors" can only mean "no descriptor
heap slot", because usage is not a property of a buffer there at all. On Vulkan it is, and a push
descriptor write still needs the matching usage bit on the buffer it names.

**P5.4's root constant ring is what proved it.** `CreateCommandBuffer` builds it at `:2772` with
`m_descriptorTypes = ConstantBuffer` and `NoDescriptors` together, which is correct -
`CmdSetRootConstants` pushes a descriptor at an offset into it rather than giving it a slot - and
it came out without `VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT`. Every root constant push in the engine
was invalid.

The fix reads the usage from `parameters.m_descriptorTypes`. Nothing else moves: the descriptor
heap work below tests `NoDescriptors` on its own already, and the copy stored on the buffer stays
cleared, so a `NoDescriptors` buffer still has no handle to hand out - which the P5.16 comment at
`:4711` depends on.

**A second buffer was quietly wrong the same way.** The raytracing top level structure buffer
(`:4719`) is `RWBuffer` plus `NoDescriptors`, and was losing `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`.
Never observed, because nothing calls it. Fixed by the same change.

#### The barrier named stages the device does not have

`VulkanPipelineStage` put `TESSELLATION_CONTROL`, `TESSELLATION_EVALUATION` and `GEOMETRY` into
the mask for `PipelineStage::NonPixelShader` and `PipelineStage::AllShader`. All three are
optional Vulkan features, `CreateContext` enables none of them, and naming a stage from a
disabled feature is a validation error - the same rule the mesh stage bits two lines above were
already gated on.

**The bits are gone rather than gated.** The engine has no tessellation or geometry shader for a
barrier to wait on - no `.esf` declares one - so there is nothing to synchronise and a flag that
is always false would be scaffolding. If a stage ever appears, the feature and the bits go in
together. Direct3D 12 has nothing to switch on, so the reference lists them freely.

#### Measured

With host validation on and `VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true`:

- **Zero validation messages** from startup to `CmdExecuteIndirect`, with the four hardware-gap
  VUIDs filtered. Every RHI call the frame makes before the indirect draw is now clean.
- **One** without that filter, and it is the first hardware gap, `storageInputOutput16`.
- The engine stops in the same place with validation on as with it off, which was the point.

**`CmdExecuteIndirect` is now the only thing left.** That is
[P5.17](Phases/Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change),
and it can be written against a live engine with validation on.

#### Files

- Files changed: `Code/Base/Render/RHI_Vulkan.cpp`. The port owns it.
- Upstream files edited: **none.**

### 2026-08-31 - Open question 8 answered: `Buffer<uint2>`. **The engine reaches its frame loop**

**The engine now runs every shader in the engine, creates every pipeline, and enters its frame
loop.** It stops at `CmdExecuteIndirect`, which is P5.13's refusal and P5.17's job. That is the
wall the port has been aiming at since Phase 5.

#### The change

`Buffer<uint64_t>` becomes `Buffer<uint2>`, and the shader packs. Six files, listed in
[TouchedFiles.md](TouchedFiles.md#shader-edits), with two helpers added to `RHI.esh` next to the
existing `RWBufferToBuffer` pair.

**This is not a behaviour change on Direct3D 12, and that is the point.** The RHI already
creates all five of these buffers as `RG32_UInt` - `DeviceRenderWorld.cpp:604`, `:628`, `:649`,
`:670` and `SpatialHash.cpp:50` - and both backends hand that straight to the view
(`RHI_Direct3D12.cpp:4171`, `RHI_Vulkan.cpp:5394`). The bytes were always two 32-bit words, low
word first. `Buffer<uint2>` names what the view already is. `Buffer<uint64_t>` did not: DXC's
SPIR-V back end turned it into a 64-bit sampled image with format `R64ui`, which no view the RHI
creates can match, which needs a capability the shader has no other use for, and which Mesa
refuses to lower at all. DXIL, given the same HLSL, just read 64 bits and said nothing.

**No C++ changed.** That was the test of the approach: if the fix had needed the RHI to create a
different kind of buffer, the abstraction really was wrong. It did not.

**No atomics were lost.** The only atomic in `SpatialHash.esh` is an `InterlockedCompareExchange`
on `m_keys`, a 32-bit `RWBuffer<uint>`, untouched. The 64-bit payload buffer is only read and
written by index, which is what made the cheap fix legal. `LoadMetadata` and `StoreMetadata` got
simpler rather than more complex: they always returned and took a `uint2`, and the element is
now that `uint2`, so their packing is gone.

#### Measured

Before, three compute pipelines failed with `VK_ERROR_UNKNOWN`, named in that run's log:
`InstancePickingResolve`, `InstanceCulling` and `LightCulling_CullLights` - exactly the three
compute shaders that read a `Buffer<uint64_t>`.

After:

- **`Int64ImageEXT` is gone from all 48 shader modules**, checked by dumping every module the
  engine creates and running `spirv-dis` over the lot.
- **Zero `spirv_to_nir` failures.**
- **The engine reaches `CmdExecuteIndirect`**, so `Shaders::Initialize`, every compute pipeline
  and every graphics pipeline now succeed.

The full capability inventory across those 48 modules, which is worth having:

| Capability | Modules |
|---|---|
| `RuntimeDescriptorArray`, `SPV_EXT_descriptor_indexing` | 38 |
| `Int16` | 31 |
| `Int64` | 18 |
| `SampledBuffer` | 18 |
| `StorageImageExtendedFormats` | 16 |
| `FragmentBarycentricKHR` | 13 |
| `StorageBuffer16BitAccess`, `GroupNonUniformArithmetic` | 13 |
| `DemoteToHelperInvocation` | 11 |
| `StorageInputOutput16` | 1 |
| `Int64Atomics` | 1 |

#### A sixth `CreateContext` feature defect, found on the way

`depthClamp`. `CreateGraphicsOrMeshPipeline` sets `depthClampEnable` from the engine's rasterizer
state - it is the inverse of Direct3D 12's `DepthClipEnable`, so it is on for any pass that does
not clip - and the feature was never enabled. Direct3D 12 has no bit for it. Fixed the same way
as the other five. It only became visible once pipelines started being created at all.

#### Correction to the P6.8 entry

P6.8 said no GPU here can run the engine's shaders. **Too strong.** With validation off, ANV
accepts every module and the engine runs to `CmdExecuteIndirect`. What the four gaps mean is that
those shaders are invalid by the spec and the driver tolerates them; a stricter driver would not,
and tolerated is not the same as correct. Three of the four are now much smaller than P6.8
measured, because that count was of validation messages rather than modules: **13 modules declare
barycentrics, 1 declares `StorageInputOutput16`, and 1 declares `Int64Atomics`.**

#### The next wall, and it is not P5.17's

With validation on, the engine now stops earlier than `CmdExecuteIndirect`, in
`vkCmdPushDescriptorSetKHR`:

```
pDescriptorWrites[0].pBufferInfo[0].buffer was created with
VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_2_TRANSFER_DST_BIT|VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT,
but descriptorType is VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
[VUID-VkWriteDescriptorSet-descriptorType-00330]
```

**This is P5.4's own root constant ring, and it is a small fix rather than a design question.**
`CreateCommandBuffer` builds the ring at `RHI_Vulkan.cpp:2772` with
`m_descriptorTypes = ConstantBuffer` and `BufferFlags::NoDescriptors`, which is exactly right:
it **is** a constant buffer, and `CmdSetRootConstants` pushes a descriptor at an offset into it
rather than giving it a heap slot.

**`CreateBuffer` conflates the two.** At `:5243` it clears `descriptorTypes` outright when
`NoDescriptors` is set, and the usage flags are derived from the cleared copy at `:5256`. So the
ring loses `VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT` and every push descriptor write against it is
invalid. On Direct3D 12 "no descriptors" means "no heap slots" and says nothing about what the
buffer is; on Vulkan the usage flags are what the buffer *is*, and the two are separate
questions.

The descriptor-heap allocation at `:5360` already tests `NoDescriptors` on its own, so the clear
at `:5243` buys nothing and only damages the usage. **Not started.**

### 2026-08-31 - P6.8 First light. **The `VK_ERROR_UNKNOWN` is explained, and it is not P5.7's**

**Not first light.** The engine still does not render a map, and it cannot on this machine. What
this task did instead: root-caused the `VK_ERROR_UNKNOWN`, fixed five real defects in
`CreateContext`, made startup survive a device without mesh shaders, and measured exactly what
stops the engine here. Read [the "Start here" block](#start-here) before running anything.

#### Validation works now, and the stale SPIRV-Tools is no longer a reason to lose it

P6.7 ran with `VK_LOADER_LAYERS_DISABLE='*'`, which turns validation off. It does not have to be.

```bash
VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true
```

`debug_disable_spirv_val` is a setting the Khronos layer already has. It switches off the layer's
bundled spirv-val, which is where the stale SPIRV-Tools that rejects the `DebugDraw` mesh shader
lives, and leaves every other check on. **No newer layer package is needed**, and Ubuntu 24.04's
`vulkan-validationlayers 1.4.309.0` is fine with that one flag. `VK_LAYER_MESSAGE_ID_FILTER`
takes a comma separated VUID list and silences those messages, which is how the remaining walls
below were found in one run rather than one rebuild each.

Host validation also has to be switched on. It defaults to false and only a Debug build forces
it. The generated `Esoterica.ini` is empty because `Settings::SaveSettings` skips any property
still at its default, so the section is written by hand:

```ini
[Render:RHI]
Enable_Host_Validation = true
```

The key names come from the reflected `Category` and `FriendlyName`, with spaces replaced by
underscores (`Settings.cpp:11`). **The empty ini is upstream behaviour, not a Linux defect.**

#### The `VK_ERROR_UNKNOWN` is `Buffer<uint64_t>`, and it is open question 8

`vkCreateComputePipelines` failed for `InstancePickingResolve`. With validation on, Mesa says
what it is:

```
SPIR-V offset 4620: SPIR-V parsing FAILED:
    glsl_type_is_texture(type->glsl_image)
spirv_to_nir failed (VK_ERROR_UNKNOWN)
```

The instruction is `%type_buffer_image = OpTypeImage %ulong Buffer 2 0 0 1 R64ui`, from
`Buffer<uint64_t>` in the HLSL. **This is not a P5.7 defect.** P5.7 builds the pipeline
correctly; the module it is handed cannot be lowered.

The RHI creates the matching buffer with `RHI::DataFormat::RG32_UInt`
(`DeviceRenderWorld.cpp:604`), which is right for Direct3D 12: a typed buffer load there returns
two 32-bit words and HLSL packs them into a `uint64_t`. DXC's SPIR-V backend does not do that. It
emits a 64-bit sampled image, whose sampled type has to match the view's format, and
`VK_FORMAT_R64_UINT` is not a uniform texel buffer format on this hardware. Mesa refuses the type
before any of that matters.

**The fix is a shader change on both backends, the same shape as
[P5.17](Phases/Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change):**
read the pages as `Buffer<uint2>` and assemble the `uint64_t` in the shader. Six sites:

| File | Line |
|---|---|
| `Code/Engine/Render/Shaders/SpatialHash.esh` | 185, 207 |
| `Code/Engine/Render/Shaders/Renderer/LightCulling_CullLights.esf` | 125, 126 |
| `Code/Engine/Render/Shaders/Renderer/InstanceCulling.esf` | 45 |
| `Code/Engine/Render/Shaders/Picking/InstancePickingResolve.esf` | 16 |
| `Code/Engine/Render/Shaders/Renderer/MaterialShaderPBR.esh` | 117 |

The last one puts a `Buffer<uint64_t>` in **every material pixel shader**, so the whole frame is
on it. **Escalated, not started.**

#### Five defects in `CreateContext`, all the same class as P6.7's 16-bit finding

The engine's shaders declare capabilities that Direct3D 12 folds into the shader model and Vulkan
gates one at a time. `vkCreateShaderModule` rejects any the device did not enable. P6.7 found the
first three; validation named five more.

| Enabled now | Who needs it | Intel UHD 620 |
|---|---|---|
| `storageInputOutput16` | a `uint16_t` bindless handle crossing a stage interface | **no** |
| `shaderInt64` | `uint64_t` in `InstancePickingResolve` | yes |
| `shaderSubgroupExtendedTypes` | a wave operation on a 16- or 64-bit operand | yes |
| `shaderDemoteToHelperInvocation` | HLSL `discard` under Shader Model 6.6 | yes |
| `shaderBufferInt64Atomics`, `shaderSharedInt64Atomics` | a 64-bit `InterlockedMin` or `InterlockedAdd` | yes, **no** |

Two optional extensions are now asked for when the device has them, chained the way the mesh
shader and shading rate blocks already are: `VK_EXT_shader_image_atomic_int64` and
`VK_KHR_fragment_shader_barycentric`. Neither has an RHI entry point to assert in, so a device
without one fails at the shader that declares it.

**`storageInputOutput16` is worth its own note.** A bindless handle is a `uint16_t`
(`RHI.esh:91-96`), and shaders pass handles down the stage interface -
`DebugDraw.esf`'s `DebugDrawPrimitiveOutput` carries a `TextureHandle` from the mesh stage to the
pixel stage. Vulkan gates 16-bit stage I/O separately from 16-bit buffer access. Direct3D 12 does
not: `Native16BitShaderOps` covers every use at once.

#### Startup now survives a device without mesh shaders

`Shaders::Initialize` creates every shader in the engine at startup, mesh stages included,
whatever the device supports. Three changes, all in `RHI_Vulkan.cpp`:

- **`CreateShader` skips `vkCreateShaderModule` for a Task or Mesh stage** when the device has no
  `VK_EXT_mesh_shader`, and leaves the handle null. Reflection still runs, because it reads the
  SPIR-V and not the module, so the root signature is built either way.
- **`CreatePipeline( MeshPipelineParameters )` returns a pipeline with a null handle** instead of
  asserting. `SurfaceShader`'s constructor builds one at startup for every shader, so the assert
  stopped the engine before it had a frame loop over a pass it may never run. **`CmdSetPipeline`
  asserts instead**, which names the pass.
- **`SetVulkanObjectName` ignores a null handle.** Naming `VK_NULL_HANDLE` is a validation error,
  and the guard belongs in the shared function rather than at each call site.

With these, `Shaders::Initialize` runs to the end on a device with no mesh shader support.

#### What this machine cannot do

Measured on all three GPUs with `vulkaninfo` and confirmed by running. Counts are shader modules
rejected in one startup.

| Gap | Modules | Intel UHD 620 | NVIDIA MX250 | llvmpipe |
|---|---|---|---|---|
| `VK_KHR_fragment_shader_barycentric` | 17 | no | no | no |
| `shaderSharedInt64Atomics` | 4 | no | yes | yes |
| `storageInputOutput16` | 2 | no | no | no |
| `VK_EXT_mesh_shader` | debug draw | no | no | yes |
| `VK_EXT_mutable_descriptor_type` | all of them | yes | **no** | yes |

**No GPU here can run the engine's shaders.** The MX250 is refused at device selection for
`VK_EXT_mutable_descriptor_type`, which the binding model requires. llvmpipe and the UHD 620 both
lack `storageInputOutput16` and barycentrics. **And open question 8 stops every device, including
ones that pass this table.**

**Barycentrics look like a development tools cost, and a Shipping build does not escape them.**
`MaterialShaderPBR.esh:82` adds `SV_Barycentrics` to the material pixel shader only when
`EE_DEVELOPMENT_TOOLS` is set, for a wireframe overlay. **But the Reflector always defines it
when it compiles shaders** (`ShaderReflection_ShaderCompiler.cpp:21` and `:23`, reached from
`COMMON_DXC_ARGUMENTS` at `:72`), and the SPIR-V it embeds is one variant shared by every build
configuration. So the barycentric capability is in the bytecode whatever the engine is built as.
Dropping it means changing the Reflector, not the build configuration.

**The Shipping Engine binary did not link, and never had.** A `NinjaGen.py` defect in the
archive ordering. **Fixed 2026-08-31**; see the Shipping configuration entry.

#### Files

- Files added: none.
- Files changed: `Code/Base/Render/RHI_Vulkan.cpp`. The port owns it; it is not upstream.
- Upstream files edited: **none.** [TouchedFiles.md](TouchedFiles.md) is unchanged.

#### Acceptance criteria

Criterion 1 stays met. **Nothing else moved.** The engine gets further - `Shaders::Initialize`
now runs to the end where it used to stop at shader 14 of 28 - but it still reaches no frame
loop, so criteria 2 to 9 are exactly as P6.7 left them. Criteria 10 to 12 stay met: this task
edited no upstream file.

#### For the next session

- **Answer open question 8 before anything else.** Nothing in the frame runs until
  `Buffer<uint64_t>` has a Vulkan spelling, and it sits in front of P5.17 rather than behind it.
- **Run with validation on.** The two lines at the top of this entry cost a session to find and
  turn every remaining wall into a named message instead of a `VK_ERROR_UNKNOWN`.
- **Do not read `VK_ERROR_UNKNOWN` from Mesa as a compile failure.** Here it meant
  `spirv_to_nir` refused a type. `INTEL_DEBUG=cs` dumps the shaders that did compile, which is
  how the failing one was narrowed down by elimination.
- **A machine with a current GPU is needed to finish Phase 6.** The four gaps above are hardware,
  not port defects. Any device with `VK_EXT_mutable_descriptor_type`, `VK_EXT_mesh_shader`,
  `VK_KHR_fragment_shader_barycentric` and `storageInputOutput16` clears the table.

### 2026-08-30 - P6.7 `EngineApplication_Linux`. **The engine binary exists and runs**

**`Build/Linux_Release/Esoterica.Applications.Engine` builds, links and starts.** It reads its
settings, loads compiled data, opens a window, creates a Vulkan device and gets as far as
compiling shaders. **Phase 6 acceptance criterion 1 is met.** It does not yet render a map; the
two things stopping it are named below and neither is P6.7's.

`EngineApplication_Linux.{h,cpp}` mirror `EngineApplication_Win32.{h,cpp}`. `_tWinMain` becomes
`int main( int argc, char** argv )`, which also drops the `__argc` and `__argv` fetch, and the
Live++ hooks are absent because `EE_ENABLE_LPP` stays unset on Linux. `ProcessInputEvent` is the
one line the P6.4 entry specified. The constructor passes no icon and no splash screen: the
Windows build reaches both through a `.rc` file, and Phase 6 says not to parse those.

#### Two build system defects, both found by running the binary

**1. The `External/` rpath was repository relative, so a binary only ran from the repository
root.** `-L` is resolved by the linker, which ninja runs from the root, so a relative path is
fine there. An **rpath is resolved by the loader against the working directory**, so it only
worked by accident. `Toolchain.py` now emits `-Wl,-rpath,'$ORIGIN/../../<directory>'`; every
output directory is `Build/Linux_<configuration>/`, two levels below the root. P6.2 predicted
this and left it for P6.7.

**2. A shared library had no soname, so its dependents recorded a path.** The dependency
libraries are passed to the linker as repository-relative paths, and with no soname the linker
copies that path verbatim into the dependent's `DT_NEEDED`. The loader then resolves
`Build/Linux_Release/libEsoterica.Base.so` against the working directory, and `$ORIGIN` never
gets a say. `NinjaGen.py` now passes `-Wl,-soname,lib<Project>.so` for every shared library, so
`DT_NEEDED` carries a bare filename that `$ORIGIN` finds.

Together these are why the engine failed with *"error while loading shared libraries"* from its
own output directory. `ldd` from `/tmp` now resolves everything.

#### A Phase 5 defect, found by running it: 16-bit shader types were never enabled

**`CreateContext` did not enable the 16-bit feature bits, and the engine's shaders need them.**
`MeshData.esh`, `CommonPacking.esh`, `XeGTAO.esh`, `RendererTypes.esh` and several `.esf` files
declare `float16_t` and `uint16_t`; DXC emits the matching SPIR-V capabilities, and
`vkCreateShaderModule` rejects a module whose capabilities the device did not enable. The first
shader it saw was `InstancePickingResolve`, and validation said:

```
vkCreateShaderModule(): SPIR-V Capability Int16 was declared, but one of the following
requirements is required (VkPhysicalDeviceFeatures::shaderInt16).
```

**Direct3D 12 has no equivalent step.** `Native16BitShaderOps` is a capability a driver either
has or does not, with nothing to switch on, so P5.1 had nothing to mirror and the gap was
invisible until a shader was created.

`shaderInt16`, `shaderFloat16`, `storageBuffer16BitAccess`,
`uniformAndStorageBuffer16BitAccess` and `storagePushConstant16` are now asked for when the
device has them, and a warning names any that are missing. Asked for rather than required,
because a device without them fails at the shader that needs one, which names the shader.

#### **What stops the engine rendering a map, and neither is Phase 6's**

With the above fixed, the engine reaches `Shaders::Initialize` and halts on the **`DebugDraw`
mesh shader**:

```
vkCreateShaderModule(): pCreateInfo->pCode (spirv-val produced an error):
[VUID-CullPrimitiveEXT-CullPrimitiveEXT-07036] According to the Vulkan spec BuiltIn
CullPrimitiveEXT variable needs to be a boolean value array. ID <10> (OpVariable) is not a
bool scalar.
```

**The SPIR-V is not the problem, and the validation layer is.** This is the *same* stale
SPIRV-Tools that Phase 4 already ran into, now inside the Vulkan validation layer rather than in
`/usr/bin/spirv-val`. Ubuntu 24.04 ships `vulkan-validationlayers 1.4.309.0`, which carries a
SPIRV-Tools old enough to have the bug Phase 4 documented: it reads an
`OpVariable %_ptr_Output__arr_bool_uint_64 Output` - an array of 64 bools, which is exactly what
the spec asks for - and reports that it "is not a bool scalar".

**Measured, not assumed.** With `VK_LOADER_LAYERS_DISABLE='*'`, `vkCreateShaderModule` accepts
the `DebugDraw` mesh shader and the engine walks straight past it. The driver has no complaint;
only the layer does.

**So there is no fourth DXC defect.** Phase 4's conclusion stands, and its rule stands with it:
*a distribution's SPIRV-Tools is not interchangeable with the one the compiler validates
against.* This is the second time that has cost a session, so it is worth stating in the form it
now takes: **the Vulkan validation layers on Ubuntu 24.04 cannot be used on the mesh shader
stages.** P6.8 must either disable them, or install a newer set, and record which.

#### What actually blocks the engine, then

Two things, and neither is what the paragraph above looked like:

1. **`vkCreateComputePipelines` returns `VK_ERROR_UNKNOWN` for `InstancePickingResolve`.** That
   is the first shader after the mesh stages, and it is the real wall. `VK_ERROR_UNKNOWN` from
   Intel's ANV usually means the driver refused to compile the module, so the next step is to
   run that one shader through the driver with `VK_LOADER_LAYERS_DISABLE` unset and a newer
   validation layer, or through `spirv-val` and the driver's own shader cache diagnostics. This
   is a Phase 5 group's first execution, so treat it as a P5.7 defect until shown otherwise.
2. **`Shaders::Initialize` creates every shader module at startup**, mesh shaders included,
   whatever the device supports. That is not fatal by itself - the driver accepts the module -
   but it means the engine pays for shaders it can never dispatch, and any future driver that is
   stricter about an unsupported stage would stop here. Worth knowing; not urgent.

**Both belong to P6.8 and Phase 5's completion, not to P6.7.** P6.7's deliverable is the binary,
and the binary exists.

#### Verification

- Files added: `Code/Applications/Engine/Linux/EngineApplication_Linux.{h,cpp}`.
- Files edited: `Code/Scripts/NinjaGen/LinuxSources.txt` (the new source),
  `Code/Scripts/NinjaGen/Toolchain.py` (the `$ORIGIN` rpath, and the SDL3 sheet for
  `Esoterica.Applications.Engine`, whose `.cpp` reads `SDL_Event` fields),
  `Code/Scripts/NinjaGen/NinjaGen.py` (the soname), `Code/Base/Render/RHI_Vulkan.cpp` (the
  16-bit features).
- **Upstream files edited: none.**
- Build: `Checks.py` passes. `ninja -k 0` fails on `Esoterica.Applications.Editor` and
  `Esoterica.Applications.ResourceServer` only, which is where it failed before. **A third
  executable now builds**, next to the Reflector and the ResourceCompiler.
- Run: `./Build/Linux_Release/Esoterica.Applications.Engine -map
  data://demo/render/pbr/pbrdemo.map -packaged`, **from any directory**, reads its settings,
  loads compiled data, opens a window, selects the Intel UHD 620 and builds both descriptor
  pools before halting at the `DebugDraw` mesh shader.
- Acceptance criteria: **criterion 1 is met.** Criterion 2 is not; see above. Criterion 10 is
  untouched: both new files are guarded with `#ifdef __linux__`, they live in a directory no
  `.vcxproj` references, and the two generator changes are Linux-only tooling.

#### The `-packaged` flag is needed, and that is not a defect

Without it the engine uses the **network** resource provider, which tries to start
`EsotericaResourceServer.exe`. The ResourceServer is Phase 7 and does not build on Linux yet.
`-packaged` selects `PackagedResourceProvider`, which reads
`Build/Linux_<configuration>/CompiledData` directly - the data Phase 3 compiled. Nothing needs
changing; P6.8 and Phase 7 should simply expect the flag until the ResourceServer exists.

**A related upstream observation, not fixed here.** When `Engine::Initialize` fails, the
following `Engine::Shutdown` segfaults in `RenderSystem::WaitAllQueuesIdle`, because it tears
down systems that were never initialized. That is upstream behaviour on both platforms and it
only shows up on a failed start; it is recorded under "Upstream issues observed".

### 2026-08-30 - P6.6 The Vulkan surface. **Esoterica renders on Linux**

**The port drew its first frame.** A window opens, `RHI::CreateContext` picks a device, a
`VkSurfaceKHR` is made from the `SDL_Window*`, a swapchain is created, and twelve frames are
cleared and presented to the screen with **no Vulkan validation errors**. The swapchain recreates
on resize and tears down clean. Every Phase 5 RHI call in that path had never executed before
today.

**It needed one change to an upstream file that was not on the registry.** That was escalated,
approved and made: `RHI::MaxPendingFrames` is 3 on Linux. See below, and
[TouchedFiles.md](TouchedFiles.md).

#### Bring-up order, and what each step found

Phase 5 wrote all of this against the compiler. Running it turned up three real defects and one
measured limit. In the order they appeared:

1. **`RHI::CreateContext` worked first time.** It skipped the NVIDIA MX250 for a missing
   `VK_EXT_mutable_descriptor_type`, chose the Intel UHD 620, warned about the absent
   `VK_EXT_mesh_shader` exactly as P5.14 said it would, and built both descriptor pools. Nothing
   needed fixing.

2. **Defect: the surface offers no RGBA format.** `CreateSwapchain` asked for `RGBA8_sRGB` then
   `RGBA8_UNorm`, which is what DXGI hands out, and asserted. **Every surface on this machine
   offers only `VK_FORMAT_B8G8R8A8_SRGB` and `VK_FORMAT_B8G8R8A8_UNORM`** - measured on the Intel
   UHD 620, the NVIDIA MX250 and llvmpipe alike. So `BGRA8_sRGB` and `BGRA8_UNorm` are now
   candidates after the two the caller asked for. **The swap costs nothing and needs no shader
   change**: a Vulkan format names its components in memory order, and a shader's red output
   lands in the format's red component wherever that byte sits. The sRGB preference is kept, and
   `DataFormat` already had both spellings.

3. **Measured limit: `minImageCount` is 3, and `MaxPendingFrames` is 2.** See the escalation.

4. **Defect: `Window::DestroySwapchain` destroys command pools before command buffers.** That is
   fine on Direct3D 12, where an allocator and a command list are independent objects, and it is
   a validation error on Vulkan, where destroying a `VkCommandPool` frees its buffers and a later
   `vkFreeCommandBuffers` on the dead pool is invalid. `RenderWindow.cpp:53` does exactly this.
   **P5.4 assumed the other order** - its comment said "the engine destroys buffers first; see
   `RenderSystem::Shutdown`" - and that is true of `RenderSystem` and false of `Window`.

   Fixed in `RHI_Vulkan.cpp`, not upstream: `VulkanCommandPool` now knows the buffers it
   allocated and nulls their handles as it is destroyed, so `DestroyCommandBuffer` skips a free
   that already happened and never reads a pool it no longer owns. Either order now works.

#### `RHI::MaxPendingFrames` is 3 on Linux. **Escalated, approved, and made**

**This is the one thing Phase 6 predicted by name, and it is real.** `RHI.h:31` set
`MaxPendingFrames = 2`. Measured on this machine:

| Surface | `minImageCount` | `maxImageCount` |
|---|---|---|
| Intel UHD Graphics 620 | **3** | unlimited |
| NVIDIA GeForce MX250 | 2 | 8 |
| llvmpipe | **3** | unlimited |

`minImageCount` is a hard minimum, so `CreateSwapchain` gets three images back and halts on its
own check, with the message P5.3 wrote for this exact case:

```
[Error][Rendering][RHI/CreateSwapchain] The surface needs 3 swapchain images and RHI::MaxPendingFrames is 2.
```

`Swapchain::m_renderTargets` is a `TArray<Texture*, MaxPendingFrames>`, and `AcquireNextImage`
returns an index into it, so there is no way to absorb this in the backend.

**The edit, exactly, at `RHI.h:31`. Four lines added, zero modified:**

```cpp
#if defined( __linux__ )
        MaxPendingFrames = 3, // Several Linux drivers report a minImageCount of 3
#else
        MaxPendingFrames = 2, // Set this value to 2 for double buffering, or 3 for triple buffering.
#endif
```

The existing line survives verbatim inside the `#else`, so **the Windows build is bit for bit
unchanged** and stays double buffered. `git diff --stat upstream/main -- Code/Base/Render/RHI.h`
reports `4 ++++` with no deletions. That is the shape Conventions rule 2 asks for. What made it an
escalation is only that `Code/Base/Render/RHI.h` was not in
[TouchedFiles.md](TouchedFiles.md); **it was escalated, approved, made, and is registered there
now.**

**With it, everything above passes.** Without it, nothing after `CreateContext` runs at all.

The one alternative that touches no upstream file was to give the backend three real swapchain
images, render into two of its own, and blit into the acquired image at present time. That is a
full screen copy every frame to avoid four lines, and it was not taken.

#### What actually ran

Recorded here because Phase 5 could never state it. `CmdBarrier` on a swapchain image in both
directions, `CmdSetRenderTargets` opening and closing a dynamic render pass with a clear load
action, `CmdSetViewport`, `EndCommandBuffer`, `QueueSubmit`, `AcquireNextImage`, `QueuePresent`,
`WaitQueueIdle`, and the whole create and destroy path for the context, queues, command pools,
command buffers, swapchain and its render targets. **The Vulkan validation layers were on
throughout and said nothing.**

Also confirmed incidentally: Phase 1's crash handler works. A null dereference during bring-up
printed a symbolised backtrace and re-raised, which is what it was written to do.

#### Verification

- Files edited: `Code/Base/Platform/PlatformUtils_Linux.{h,cpp}` (the two surface functions),
  `Code/Base/Render/RHI_Vulkan.cpp` (the surface call, the BGRA candidates, the command pool
  teardown fix).
- **Upstream files edited: one.** `Code/Base/Render/RHI.h:31`, 4 added and 0 modified,
  registered in [TouchedFiles.md](TouchedFiles.md). Escalated and approved before the edit.
- Build: `Checks.py` passes. `ninja -k 0` fails on `Esoterica.Applications.Editor` and
  `Esoterica.Applications.ResourceServer` only, which is where it failed before.
- Run: a scratch application deriving from `LinuxApplication` (not committed) reported every
  check passing, including that all three swapchain images were acquired across twelve frames and
  that the swapchain survived a resize.
- Acceptance criteria: **criterion 4 is met** - resize recreates the swapchain with no validation
  errors. **Criterion 8 is met for this path** - shutdown is clean with validation on. Criterion 1
  is not: there was still no engine binary when this was written, which is P6.7. Criterion 10
  is untouched.

#### For P6.7

- `EngineApplication_Linux::ProcessInputEvent` is one line; see the P6.4 entry for the exact
  `GenericMessage`.
- The `Esoterica.Applications.Engine` project needs the SDL3 sheet in `LINUX_ONLY_SHEETS` as soon
  as its `.cpp` reads an SDL event field.
- The engine paces frames on `RenderSystem`'s frame semaphores. A harness without them reuses the
  swapchain's acquire semaphores while they are still pending, and validation says so. That is
  not a defect; it is a reminder that `AcquireNextImage` is only sound inside the engine's own
  frame pacing.

### 2026-08-30 - P6.5 Gamepads. The last Phase 6 stub in `Base` is gone

**`InputDevice_XBoxController_Linux.cpp` is a real device, on SDL3's gamepad API.** It replaces
the Phase 1 stub, and with it **`Base` has no `EE_UNIMPLEMENTED_FUNCTION` left outside
`RHI_Vulkan.cpp`.** The three that remain there are Phase 5's, and two more are upstream's own in
`Triangle.h` and `Encoding.cpp`.

**`InputSystem::Initialize()` works now.** It constructs two `XBoxControllerInputDevice`s and
calls `Initialize` on each, and that used to halt, which meant nothing could touch `InputSystem`
at all. P6.4 had to drive its device directly for that reason. That obstacle is gone.

**The name stays `InputDevice_XBoxController_Linux.cpp`**, per Conventions rule 3 and the phase
document, even though SDL3 handles any gamepad.

#### Three things that differ from XInput, all of them real

1. **The vertical axes are negated.** SDL follows the joystick convention, where pushing the
   stick down gives a positive Y. XInput's `sThumbLY` is positive upwards, and the engine is
   written against XInput. **Without the negation every controller would be inverted on Linux
   only**, which is the sort of defect that survives a long time because it looks like a user
   setting. Proved in both directions, on both sticks.
2. **Triggers use a different raw range.** XInput reports a byte, 0 to 255; SDL reports 0 to
   `SDL_JOYSTICK_AXIS_MAX`. Both normalize to 0..1, so only the divisor changes and
   `GetDefaultTriggerThreshold` keeps XInput's 30/255. All three dead zone values are unchanged
   from the Win32 device.
3. **Face buttons are named by position, not by letter.** `SDL_GAMEPAD_BUTTON_SOUTH` is the
   XInput A button, and "south" is exactly what `Controller_FaceButtonDown` means, so the mapping
   is more direct than XInput's.

#### Where the `SDL_Gamepad*` lives, and why it is not a member

**In a file-static array, indexed by hardware controller index.** `XBoxControllerInputDevice` has
no member to hold one and `InputDevice_XBoxController.h` is an upstream file, so adding one would
be an unregistered edit to a shared header - an escalation trigger. XInput needs no such storage:
it polls a slot number and the OS owns the connection.

The array holds four entries; `InputSystem` creates `s_maxControllers`, which is 2. An index
outside the array is treated as permanently disconnected rather than as an error.

**Hot plug is handled by re-reading the slot every frame.** `SDL_GetGamepads` returns a list, and
a device unplugged earlier in that list shifts the rest down, so the joystick ID at a slot is not
stable and cannot be cached. When it changes, the old handle is closed and the new one opened.

#### The gamepad subsystem initializes itself

`SDL_InitSubSystem( SDL_INIT_GAMEPAD )` is called from `XBoxControllerInputDevice::Initialize`
rather than from `LinuxApplication::Run`, so anything holding an `InputSystem` gets working
gamepads without knowing about SDL. SDL reference counts subsystems, so both devices doing it is
correct. **This replaces P6.2's note that P6.5 would add `SDL_INIT_GAMEPAD` to `Run`**; that
comment is updated in place.

**There are no `SDL_EVENT_GAMEPAD_*` cases in `LinuxApplication::ProcessEvent` either.** The
device polls, the way the XInput sibling does, and `SDL_UpdateGamepads` picks up plug and unplug
on its own. P6.3's placeholder comment is updated to say so.

#### Verification

- Files replaced: `Code/Base/Input/InputDevices/Platform/InputDevice_XBoxController_Linux.cpp`,
  which was a Phase 1 stub.
- Files edited: `Code/Base/Application/Platform/Application_Linux.cpp` (two stale comments),
  `Code/Scripts/NinjaGen/LinuxSources.txt` (the file moves out of the stub group, and
  `Application_Linux.cpp` moves to the Phase 6 group where it belongs).
- **Upstream files edited: none.**
- Build: `Checks.py` passes. `ninja -k 0` fails on `Esoterica.Applications.Editor` and
  `Esoterica.Applications.ResourceServer` only, which is where it failed before.
- **Run, end to end through `InputSystem`, with a scratch harness (not committed). All checks
  pass.** No gamepad is plugged into this machine, so the harness attaches an
  **SDL virtual joystick** (`SDL_AttachVirtualJoystick`) and drives it with
  `SDL_SetJoystickVirtualButton` and `SDL_SetJoystickVirtualAxis`. What it covers:
  - `InputSystem::Initialize` returns true and reports one connected controller.
  - All 14 buttons, one at a time, each checked to raise its own `InputID` **and nothing else**.
  - Stick Y inverted correctly in both directions, on both sticks: SDL -32768 becomes engine
    +1.000, SDL +32767 becomes engine -1.000.
  - Stick X passes through unchanged, and the two sticks are independent.
  - Triggers press and release independently.
  - Dead zones read 0.2395, 0.2652 and 0.1176, which are XInput's 7849, 8689 and 30 normalized.
  - Unplugging the virtual pad disconnects the device and drops the controller count to zero.
- **A note for anyone writing a similar harness.** SDL generates the mapping `lefttrigger:a4`
  for a virtual pad, which maps the full joystick range onto the trigger's 0..32767. A released
  trigger is therefore joystick axis **-32768**, not 0; setting 0 reads back as a half-pulled
  trigger. That cost a failing check before it was understood, and it is the harness rather than
  the device.
- Acceptance criteria: criterion 3 is met at the device level for keyboard, mouse and gamepad.
  "Works for camera control" still needs a running engine, which is P6.7 and P6.8. Criterion 10
  is untouched.

#### Not done, and not needed

Rumble, LEDs, gyro, touchpads and battery. The engine's `ControllerDevice` has no concept of any
of them, and XInput's device exposes none either. SDL3 offers them all; adding them would be a
feature this port does not owe.

### 2026-08-30 - P6.4 Keyboard and mouse. The mapping table is complete and proved complete

**`InputDevice_KeyboardMouse_Linux.cpp` is a real device.** It replaces the Phase 1 stub of four
halting functions. **The scancode table is complete: 105 scancodes map to 105 distinct
`InputID`s, one to one, with none missing and none duplicated.** That is checked by running it,
not by reading it.

**Scancodes, not keycodes**, as the phase document requires. A scancode names the physical key and
does not move with the layout, which is what the Win32 sibling gets from raw input. `SDL_SCANCODE_W`
is `Keyboard_W` on AZERTY too.

#### How an `SDL_Event` reaches the device

**By pointer, in `GenericMessage::m_data0`.** `GenericMessage` is four `uint64_t` and an
`SDL_Event` is 128 bytes, so it cannot be copied in. This is safe because
`InputSystem::ForwardInputMessageToInputDevices` dispatches synchronously, from inside
`LinuxApplication`'s event loop, while the event is still on the stack. The Win32 sibling passes
an `HRAWINPUT` handle through the same field, so the shape is not new.

**This is the contract P6.7 has to honour.** `EngineApplication_Linux::ProcessInputEvent` is one
line:

```cpp
m_engine.GetInputSystem()->ForwardInputMessageToInputDevices( { (uint64_t) &event, 0, 0, 0 } );
```

Nothing may queue that message for later.

#### What got simpler, and what got harder

**Simpler.** The Win32 device spends 60 lines in `ConvertKeyMessageToInputID` fixing up
`VK_SHIFT`, `VK_CONTROL`, `VK_MENU` and every numpad key from the message's scan code and
extended bit, because a Windows virtual key does not distinguish left from right, or numpad from
cursor block. Scancodes already do. That whole function is a hash lookup here.

Wheel deltas arrive in notches, so there is no `WHEEL_DELTA` to divide by. A "natural scrolling"
setting arrives as `SDL_MOUSEWHEEL_FLIPPED` rather than negated values, and is undone, so the
engine sees what Windows reports.

**Auto-repeat is dropped.** `SDL_EVENT_KEY_DOWN` repeats while a key is held and raw input does
not, so the Win32 sibling never sees one. `Press` on an already held key would restart its state.

#### Two behaviour gaps, both recorded rather than guessed at

1. **Mouse deltas stop at the screen edge; on Windows they do not.** The Win32 device reads raw
   input, which has no cursor and no bounds. `SDL_EVENT_MOUSE_MOTION`'s `xrel` and `yrel` follow
   the pointer. X11 takes an implicit pointer grab on button press, so a camera drag keeps
   receiving motion outside the *window*, but the *screen* edge still stops it.

   **`SDL_SetWindowRelativeMouseMode` is the fix, and P6.4 deliberately does not call it.**
   Turning it on for any button press would hide and warp the cursor and break every imgui drag,
   and the device cannot tell a camera drag from a slider drag. imgui's own backend already calls
   `SDL_CaptureMouse`, so a second owner of capture would fight it. **P6.8 should decide this
   against a live camera**, which is the first time anyone can see whether it matters.

2. **`m_charKeyPressed` only fills while text input is active.** SDL3 delivers
   `SDL_EVENT_TEXT_INPUT` only after `SDL_StartTextInput`, and the only caller of that is imgui's
   backend, when a text field has focus. `WM_CHAR` always arrives on Windows. **Nothing in the
   engine reads `GetCharKeyPressed()`** - the grep is empty - so this blocks nothing today.

   Non-ASCII is dropped rather than truncated. The field is one byte and `SDL_EVENT_TEXT_INPUT`
   is UTF-8; the Win32 sibling truncates a `WM_CHAR` code point to a `char`, which is the same
   loss written differently. imgui does its own text input and is unaffected.

#### Verification

- Files replaced: `Code/Base/Input/InputDevices/Platform/InputDevice_KeyboardMouse_Linux.cpp`,
  which was a Phase 1 stub.
- Files edited: `Code/Scripts/NinjaGen/LinuxSources.txt`, which now lists the file under Phase 6
  rather than under the stubs.
- **Upstream files edited: none.**
- Build: `Checks.py` passes. `ninja -k 0` fails on `Esoterica.Applications.Editor` and
  `Esoterica.Applications.ResourceServer` only, which is where it failed before.
- **Run, with a scratch harness (not committed). 20 checks, all passing.** It drives the device
  through an `InputDevice*`, because `InputDevice` declares the overrides public and access is
  checked on the static type:
  - Every scancode from 1 to `SDL_SCANCODE_COUNT` is fed in and the resulting `InputID`s
    collected. **105 scancodes produce 105 distinct IDs**; every ID from `Keyboard_A` to
    `Keyboard_RAlt` is produced by exactly one scancode.
  - Left and right Shift are distinct, and `Numpad4` is not the Left arrow. Those are the two
    cases the Win32 fix-up code exists for.
  - Press, hold, release; auto-repeat ignored.
  - All five mouse buttons; the vertical and horizontal wheel; `SDL_MOUSEWHEEL_FLIPPED` undone.
  - Two motion events in one frame accumulate, and the delta resets on the next.
  - Char input; non-ASCII dropped; focus loss clears held keys.
- **`InputSystem` cannot be used end to end yet.** `InputSystem::Initialize` constructs two
  `XBoxControllerInputDevice`s and calls `Initialize` on each, and that is still the P6.5 stub,
  which halts. The harness drives the keyboard and mouse device directly for that reason. P6.5
  removes the obstacle.
- Acceptance criteria: criterion 3 is half met. Keyboard and mouse are implemented and tested at
  the device level; gamepad is P6.5, and "works for camera control" needs a running engine, which
  is P6.7 and P6.8. Criterion 10 is untouched: the file is guarded with `#ifdef __linux__` and no
  `.vcxproj` lists it.

### 2026-08-30 - P6.3 imgui platform backend. Multi-viewport works, and the Win32 copy is stale

**The imgui platform backend runs on SDL3, and multi-viewport is verified rather than assumed.**
`ImguiPlatform_Linux.{h,cpp}` replace the Phase 1 stub, `ImguiX_Linux.cpp` is the sibling of
`ImguiX_Win32.cpp`, and `LinuxApplication::ProcessEvent` now calls imgui first.

**Proved by running it.** Three imgui windows became three real SDL windows, each with a live
`SDL_Window*` found from its viewport, one of them at 1100,200 which is outside the main window.
Shutdown destroyed all three. Under i3 on X11.

#### The vendored Win32 backend is three years behind the imgui core it sits next to

**This changes the plan for this task.** [Phase6-WindowingInput.md](Phases/Phase6-WindowingInput.md)
says to diff `ImguiPlatform_Win32.cpp` against "the matching upstream release" of
`imgui_impl_win32.cpp` and treat that as the worklist. There is no matching release:

- `Code/Base/ThirdParty/imgui/imgui.cpp` is **byte-identical to `v1.92.9b-docking`**, bumped
  upstream on 2026-08-06.
- `ImguiPlatform_Win32.cpp` was last touched on 2026-07-19 and last named a version in
  "Upgrade to DearImgui 1.89.1", 2022-11-29. Its functions still match roughly 1.89: it has
  `ImGui_ImplWin32_VirtualKeyToImGuiKey`, which upstream renamed to `KeyEventToImGuiKey`, and
  lacks `WndProcHandlerEx` and `AdjustWindowRect`.

So this task starts from **`v1.92.9b-docking`'s `imgui_impl_sdl3.cpp`**, which matches the
vendored core, and ports Esoterica's *adaptations* rather than mirroring a stale file. A
1.89-era backend against a 1.92 core would be wrong: 1.92 changed the font and texture contract
(`ImGuiBackendFlags_RendererHasTextures`).

#### Esoterica's adaptations, and what each became on SDL3

| Win32 adaptation | Linux |
|---|---|
| Wrapped in `EE::ImGuiX::Platform`, guarded, reformatted into house style | Same wrapper and guards. **Not reformatted**; see below |
| `Init`/`Shutdown`/`NewFrame` folded into `ImguiSystem::InitializePlatform`/`ShutdownPlatform`/`PlatformNewFrame` | Same, but as thin calls into the vendored functions rather than an inlined copy |
| Window read from `Platform::GetMainWindowHandle()`, not passed to `Init` | Same |
| Gamepad and XInput removed; the engine owns gamepads | Same. `ImGui_ImplSDL3_UpdateGamepads` and friends are gone |
| DPI awareness left to `Win32Application` | Left to `LinuxApplication`, which sets `SDL_WINDOW_HIGH_PIXEL_DENSITY` |
| Backend data through `EE::New` / `EE::Delete` | Same |
| A child wnd proc forwards input to `InputSystem`, so viewport windows keep feeding the engine | **Not needed.** SDL has one event queue, and `LinuxApplication::ProcessEvent` already forwards every input event whatever window it came from |
| A window class registered for viewport windows, with the app icon | Not needed. SDL has no window classes |
| `EE_BASE_API intptr_t WindowMessageProcessor(...)` | `EE_BASE_API bool ProcessEvent( SDL_Event const& )` |

**Two adaptations are new, and neither has a Win32 counterpart:**

1. **The `NewFrame` time step is removed.** Upstream's `ImGui_ImplSDL3_NewFrame` sets
   `io.DeltaTime` from `SDL_GetPerformanceCounter`. `ImguiSystem::StartFrame` sets it from the
   engine clock and then calls `PlatformNewFrame`, so upstream's version would overwrite it every
   frame. Verified: the engine's 0.01667 survives.
2. **`ProcessEvent`'s return value is ignored by the caller**, and it guards against a null
   context. Both matter, and the second is written up under P6.2's file below.

#### **The return value of the two backends does not mean the same thing**

`Win32Application::WindowMessageProcessor` returns early when
`ImGuiX::Platform::WindowMessageProcessor` returns non-zero. **Copying that would break the
application.** A wnd proc returns non-zero only for a message it truly consumed, and
`imgui_impl_win32.cpp` returns 0 for nearly everything. `imgui_impl_sdl3.cpp` returns `true` for
every event it recognises, including `SDL_EVENT_WINDOW_CLOSE_REQUESTED` and both focus events. An
early return there swallows the application's own close and stops input reaching the engine.
`LinuxApplication::ProcessEvent` therefore calls imgui and ignores the answer, which is what
upstream's own SDL3 examples do.

`ProcessEvent` also returns false when there is no imgui context or backend.
`ImGui_ImplSDL3_ProcessEvent` asserts in that case, and `LinuxApplication` pumps events for any
subclass, including one that never starts imgui.

#### The vendored region keeps upstream's formatting, and that is deliberate

**This breaks Conventions rule 8, knowingly.** The vendored region of `ImguiPlatform_Linux.cpp`
is left at upstream's indentation, at column zero rather than indented into the namespace, so
that

```
diff <upstream imgui_impl_sdl3.cpp> <the vendored region>
```

still works. Every deliberate change carries an `EE:` comment for exactly that reason. The
argument is the finding above: `ImguiPlatform_Win32.cpp` was reformatted into house style and is
now three years behind the core it sits beside. Rule 8's own rationale is consistency with the
neighbour; here the neighbour's approach is what produced the drift. A banner at the top of the
file says all of this, so nobody has to rediscover it.

Everything Esoterica wrote in that file, and all of `ImguiPlatform_Linux.h` and
`ImguiX_Linux.cpp`, is in house style.

#### `PlatformHandleRaw` is null on Linux

`ImGui_ImplSDL3_SetupPlatformHandles` fills `PlatformHandleRaw` only on Windows and macOS, and
puts the `SDL_WindowID` in `PlatformHandle`. So `ImguiX_Linux.cpp`'s window controls look the
window up with `SDL_GetWindowFromID` where the Win32 sibling casts `PlatformHandleRaw` to an
`HWND`. Minimize, maximize, restore and close map to `SDL_MinimizeWindow`, `SDL_MaximizeWindow`,
`SDL_RestoreWindow` and a pushed `SDL_EVENT_WINDOW_CLOSE_REQUESTED`; there is no SDL equivalent
of `SendMessage( hwnd, WM_CLOSE )`.

#### The renderer half is confirmed separate, and it gates viewports

The phase document asked for this to be confirmed rather than assumed. It is separate:
`ImguiRenderer` goes through the RHI, and nothing in `ImguiPlatform_Linux.cpp` touches it.

**But imgui gates `ImGuiConfigFlags_ViewportsEnable` on both halves** (`imgui.cpp:11791`): the
platform backend must set `ImGuiBackendFlags_PlatformHasViewports` *and* the renderer must set
`ImGuiBackendFlags_RendererHasViewports`. With only the platform half, imgui silently disables
viewports for the frame and every window merges into the main one. That cost an hour here, and
it is the first thing to check if the editor's windows will not detach in Phase 7.

#### Verification

- Files added: `Code/Base/Imgui/Platform/ImguiPlatform_Linux.h`,
  `Code/Base/Imgui/Platform/ImguiX_Linux.cpp`.
- Files replaced: `Code/Base/Imgui/Platform/ImguiPlatform_Linux.cpp`, which was a Phase 1 stub of
  three halting functions.
- Files edited: `Code/Base/Application/Platform/Application_Linux.cpp` (the imgui hook),
  `Code/Scripts/NinjaGen/LinuxSources.txt`.
- **Upstream files edited: one.** `Code/Base/Imgui/ImguiSystem.cpp:12`, `#if _WIN32` to
  `#if _WIN32 || defined( __linux__ )`. `git diff --stat upstream/main` shows **1 line changed**,
  which is Phase 6 acceptance criterion 12. Registered in [TouchedFiles.md](TouchedFiles.md).
  **It turns out to be cosmetic**: `imconfig.h` defines `IMGUI_ENABLE_FREETYPE` unconditionally,
  so `imgui_freetype.cpp` was already compiled into `libEsoterica.Base.so` before this change.
- Build: `Checks.py` passes. `ninja -k 0` fails on `Esoterica.Applications.Editor` and
  `Esoterica.Applications.ResourceServer` only, which is where it failed before. Both are Phase 7.
- Run, with a scratch subclass (not committed): the backend reports itself as
  `imgui_impl_sdl3 (3.4.14; 3.4.14) (x11)`; `ImGuiBackendFlags_PlatformHasViewports` and
  `HasMouseCursors` are set and `HasGamepad` is not, which is the intended removal; one monitor is
  enumerated at 1920x1080, DPI scale 1.0; `io.DisplaySize` is 958x1042 with framebuffer scale
  1.0; `io.DeltaTime` keeps the engine's value; draw data is produced; and three imgui windows
  became three live SDL windows at the positions imgui asked for, destroyed cleanly on shutdown.
  The harness fakes the two renderer flags, because it has no renderer.
- Acceptance criteria: **criterion 12 is met.** Criterion 5 needs the renderer and a running
  engine, so it is P6.7 and P6.8. Criterion 10, the Windows build, is untouched: the one upstream
  edit adds a Linux branch to an existing `#if` and changes nothing Windows compiles.

#### Still open

- **Wayland is untested.** This machine runs i3 on X11 and has no Wayland compositor, so nothing
  here says how viewports behave under one. P6.8 owns it. Note that
  `ImGui_ImplSDL3_Init` sets `bd->IsWayland` and the mouse capture and global state white list
  excludes wayland, so `ImGuiBackendFlags_PlatformHasViewports` **will not be set on Wayland at
  all**. Read `ImGui_ImplSDL3_Init` before assuming the editor's docking UI works there.
- **i3 honoured the viewport positions**, which was the worry Phase 6 recorded about tiling and
  Wayland window placement. It ignores `SDL_SetWindowPosition` on the main window and honours it
  on the viewport windows, which are borderless utility windows.

### 2026-08-30 - P6.2 `LinuxApplication`, and a Phase 5 / Phase 6 conflict to settle

**`LinuxApplication` is written and it runs.** `Application_Linux.{h,cpp}` mirror
`Application_Win32.{h,cpp}` on SDL3. A window opens, resizes, persists its layout and shuts down
cleanly. **Nothing derives from it yet**: `EngineApplication_Linux` is P6.7.

**Verified by running it, not only by building it.** A scratch subclass linked against
`libEsoterica.Base.so` opened a window, took a resize, wrote and re-read its layout file, ran the
borderless hit test and exited 0. Details below. The program is not committed.

#### What carried over unchanged

`Initialize`, `Shutdown`, `ApplicationLoop`, `ResizeMainWindow`, `OnUserExitRequest`,
`FatalError`, `OnFirstShowMainWindow`, `ProcessWindowDestructionMessage`, `ReadWindowSettings`,
`WriteWindowSettings`, `RequestApplicationExit`, `GetBorderlessTitleBarInfo`, `Run`,
`WasInitialized`, and both `InitOptions` flags. A subclass reads the same on both platforms.

#### What changed, and why

| Win32 | Linux |
|---|---|
| `Win32Application( HINSTANCE, name, iconResourceID, splashResourceID, options )` | `LinuxApplication( name, iconFilePath, splashScreenFilePath, options )`. Both paths may be null, and both are null today. |
| `WindowMessageProcessor( HWND, UINT, WPARAM, LPARAM )` | `bool ProcessEvent( SDL_Event const& )` |
| `ProcessInputMessage( UINT, WPARAM, LPARAM )` | `ProcessInputEvent( SDL_Event const& )` |
| `HICON GetIcon()` | `SDL_Surface* GetIcon()` |
| `BorderlessWindowHitTest( POINT )` returning an `HT*` code | Same shape, returning an `SDL_HitTestResult` through `SDL_SetWindowHitTest` |
| `RECT m_windowRect` | `Int2 m_windowPosition` and `Int2 m_windowSize` |
| `WM_GETMINMAXINFO` clamps to 320x240 | `SDL_SetWindowMinimumSize` |
| Live++ agent, hooks and `#if EE_ENABLE_LPP` blocks | Absent. There is no Live++ on Linux. |

**The header forward declares `SDL_Window`, `SDL_Surface` and `SDL_Event` and includes no SDL
header.** `ImguiPlatform_Win32.h` forward declares `HWND__` for the same reason. This keeps SDL3
off the include path of everything that derives from `LinuxApplication`, so
`Esoterica.Applications.Engine` needs the SDL3 sheet only when P6.7 writes a `.cpp` that reads
event fields.

**`m_windowPosition` and `m_windowSize` are in different units, on purpose.** The position is
logical desktop coordinates, which is what `SDL_SetWindowPosition` takes. The size is pixels,
which is what the swapchain needs. They are the same numbers on a non-HiDPI display. The window is
created with `SDL_WINDOW_HIGH_PIXEL_DENSITY`, and `m_windowSize` is read back with
`SDL_GetWindowSizeInPixels` after creation and from `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` after
that. **`SDL_EVENT_WINDOW_RESIZED` is the wrong event here**: it reports logical coordinates.

**The layout file keeps the Win32 key names.** `Left`, `Right`, `Top`, `Bottom`, `WasMaximized`
under `[WindowSettings]`, so one `.layout.ini` is portable between the two builds.

**`WriteWindowSettings` is called from `Run` before `Shutdown`, not only from an event.** The
window is destroyed in the destructor, so `SDL_EVENT_WINDOW_DESTROYED` never arrives while the
loop is running, and `WM_DESTROY`'s job has to be done explicitly.

**Window events are filtered by window id.** imgui multi-viewport creates windows of its own, and
closing one of those is not a request to close the application.

**`SDL_Init( SDL_INIT_VIDEO )` happens in `Run`, and `SDL_Quit` in the destructor.**
`Win32Application` has no equivalent step. P6.5 adds `SDL_INIT_GAMEPAD`.

**`FatalError` logs as well as showing `SDL_ShowSimpleMessageBox`.** A dialog needs a desktop, and
this runs from a terminal often enough that the message has to survive without one.

**The exit code is always 0 on a clean run.** `Win32Application` takes its code from `WM_QUIT`'s
`wParam`; SDL has no such value.

#### Left for the next tasks, marked in the code

- **P6.3.** `ProcessEvent` has no imgui hook yet. `Win32Application` calls
  `ImGuiX::Platform::WindowMessageProcessor` first, before anything else; there is no
  `ImguiPlatform_Linux.h` to call into. A comment marks the spot.
- **P6.5.** `ProcessEvent` has no `SDL_EVENT_GAMEPAD_*` cases. XInput polls, so the Win32 sibling
  has nothing to copy, and `SDL_EVENT_GAMEPAD_ADDED` and `_REMOVED` have no polling equivalent.
  A comment marks the spot.

#### **Escalation: Phase 5 and Phase 6 disagree about `Platform::SetMainWindowHandle`**

**This blocks P6.6 and it is a human decision.** `LinuxApplication::TryCreateMainWindow` stores
the `SDL_Window*`, exactly as `Win32Application` stores the `HWND`. That is what the imgui and
input backends need. But:

- `EngineModule.cpp:135` does `m_renderWindow.SetNativeWindowHandle( Platform::GetMainWindowHandle() )`.
- `RHI_Vulkan.cpp:2216` casts that value straight to a `VkSurfaceKHR`, which is P5.3's recorded
  decision.
- **An `SDL_Window*` is not a `VkSurfaceKHR`.**

P5.3's answer was "the application creates the surface and hands it over", and **the application
has no place to do it.** `EngineModule::InitializeModule` calls `RenderSystem::Initialize`, which
creates the `VkInstance`, and then calls `SetNativeWindowHandle` three lines later. Nothing the
application owns runs in between. Before that call there is no instance, so no surface can exist.

**`Code/Engine/_Module/EngineModule.cpp` is not in [TouchedFiles.md](TouchedFiles.md)**, which is
the escalation trigger. Three candidates, best first:

1. **`RHI_Vulkan.cpp` creates the surface itself, through a Linux-only `Platform` function.**
   `Platform::CreateVulkanSurface( instance, pNativeWindowHandle )` lives in `Platform_Linux.cpp`,
   which may link SDL3 because `Esoterica.Base` already does. **No upstream file is edited**, and
   `Base/Render` still includes no window system header, which is what P5.3 actually required.
   It does revise P5.3's "the application owns it", so it is a decision, not a detail.
2. **A two-line `#elif defined( __linux__ )` in `EngineModule.cpp:135`.** The right shape under
   Conventions rule 2, but it puts SDL3 into `Esoterica.Engine.Runtime` and adds a file to the
   registry that a full survey did not predict.
3. **Store the surface in `Platform::SetMainWindowHandle`.** Does not work, for the ordering
   reason above.

**Answered the same day: candidate 1.** See the decision entry, "`RHI_Vulkan.cpp` creates the
Vulkan surface, through `Platform_Linux.cpp`". P6.2 itself does not change: it stores the
`SDL_Window*`, which is right for imgui and input either way. **P6.6 writes the surface function.**

#### X11 and Wayland findings so far

**The development session runs i3 on X11, and a tiling window manager makes two Phase 6
acceptance criteria untestable here.** i3 tiled the window to 958x1042 and ignored both
`SDL_SetWindowPosition` and `SDL_SetWindowSize`. That is correct behaviour, not a defect, and the
code handles it: the tiling arrives as `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` and
`ResizeMainWindow` gets the real size. **Criterion 7, DPI scaling, and the parts of criterion 4
that need a client-driven resize need a floating window manager or a second session.** Wayland is
not tested yet; P6.8 owns that.

#### Known issue for P6.7, not introduced here

**`External/` shared libraries are on a relative rpath, so a binary only resolves them from the
repository root.** `ldd Build/Linux_Release/libEsoterica.Base.so` from any other directory reports
`libSDL3.so.0`, `libdxcompiler.so` and `libGameNetworkingSockets.so` as not found. This predates
Phase 6: `Toolchain.py:503` emits `-Wl,-rpath,<repo-relative directory>`. The Phase 6 deliverable
is run from the repository root, so it works, but P6.7 should make that path absolute in one place
rather than working around it.

#### Verification

- Files added: `Code/Base/Application/Platform/Application_Linux.h`,
  `Code/Base/Application/Platform/Application_Linux.cpp`.
- Files edited: `Code/Scripts/NinjaGen/LinuxSources.txt` (adds the new `.cpp`, and drops the stale
  "still to come in Phase 1" note that listed it).
- **Upstream files edited: none.**
- Build: `Checks.py` passes. `ninja -k 0` fails on `Esoterica.Applications.Editor` and
  `Esoterica.Applications.ResourceServer` only, which is where it failed before. Both are Phase 7.
  `libEsoterica.Base.so` builds and links in Debug and Release.
- Run: a scratch subclass linked against `libEsoterica.Base.so`. `Initialize` saw 640x480 at
  100,100 on the first run; the window opened; `ResizeMainWindow` got 958x1042 from i3;
  `SDL_EVENT_WINDOW_CLOSE_REQUESTED` reached `OnUserExitRequest`; `Shutdown` ran and `Run`
  returned 0. The layout file was written with the real geometry, and a second run read it back
  and reported 958x1042 at 961,1. With `InitOptions::Borderless` set, the hit test returned
  `RESIZE_TOPLEFT` at 4,4, `DRAGGABLE` inside the reported title bar, and `NORMAL` in the client
  area, and input events reached `ProcessInputEvent`.
- Acceptance criteria: P6.2 has none of its own. Phase 6 criterion 1 is not met, and cannot be
  until P6.7. Criterion 10, the Windows build, is untouched: the two new files are guarded with
  `#ifdef __linux__` and no `.vcxproj` lists them.

### 2026-08-29 - P6.1 SDL3, and open question 4 is answered

**SDL3 is fetched, built and wired into the generator.** `./DownloadDependencies.sh sdl3` builds
`release-3.4.14` from source into `External/SDL3/`, and `Esoterica.Base` now links `-lSDL3`.
Nothing includes an SDL header yet: P6.2 onwards does that.

**Open question 4 is answered: no, and the port always builds SDL3 from source.** Ubuntu 24.04
LTS, the development target, packages no SDL3 at all. SDL3 first shipped in January 2025 and
reaches the archives from Ubuntu 25.04 onwards, so a distribution package cannot be relied on.
That also settles the version question: a source build pins one version for every distribution,
which matters because the vendored Dear ImGui is 1.92.9b and its `imgui_impl_sdl3.cpp` calls
recent SDL3 additions.

**The sheet points at `External/`, not at `pkg-config --libs sdl3`**, which is what
[Phase6-WindowingInput.md](Phases/Phase6-WindowingInput.md) planned. There is no `sdl3.pc` on the
system to find, and once `External/SDL3/` holds a build, a pkg-config lookup would need
`PKG_CONFIG_PATH` set to reach it. Every other `External/` dependency uses include and library
directories, so SDL3 does too. `Toolchain.py` adds `-Wl,-rpath` for every library directory
already, so the built binaries find `libSDL3.so.0` without a staging step.

**X11 and Wayland are both forced on in the CMake configure, rather than left to detection.**
SDL's CMake drops a video backend whose headers are missing and still configures successfully,
which produces a library that builds, links, and then finds no display at run time. That is the
silent failure this build guards against, so `requirements_sdl3()` checks all the X11, Wayland
and xkbcommon packages by `pkg-config` name and reports every missing one in one message.
`CMAKE_INSTALL_LIBDIR=lib` is set too, because `GNUInstallDirs` on Debian and Ubuntu would
install into `lib/x86_64-linux-gnu`.

**Checked at run time, once, with a scratch program.** A green build hides all of this. The
built library reports video drivers `wayland`, `x11`, `kmsdrm`, `offscreen`, `dummy` and
`evdev`; `SDL_CreateWindow` with `SDL_WINDOW_VULKAN` succeeds; and
`SDL_Vulkan_GetInstanceExtensions` returns `VK_KHR_surface` and `VK_KHR_xlib_surface`, both of
which `CreateContext` already enables. This session runs X11. The program is not committed:
the build is the test, and this was a one-off check of a run-time property.

- Files added: none.
- Files edited: `DownloadDependencies.sh` (the `sdl3` target),
  `Code/Scripts/NinjaGen/Toolchain.py` (the `SDL3` sheet, and `Esoterica.Base` in
  `LINUX_ONLY_SHEETS`). Both are new files this fork owns, and both are already registered.
- **Upstream files edited: none.**
- Build: `python3 Code/Scripts/NinjaGen/NinjaGen.py` and `ninja -f Build/Linux/Esoterica.ninja
  -k 0` fail on `Esoterica.Applications.Editor` and `Esoterica.Applications.ResourceServer`
  only, which is exactly where they failed before. `Checks.py` passes.
- Acceptance criteria: P6.1 has none of its own. Phase 6 criterion 10, the Windows build, is
  untouched: neither edited file is read by MSBuild.

**For P6.2.** `External/SDL3/lib/cmake` and `External/SDL3/lib/pkgconfig` are installed and
unused. Leave them; deleting them would only make a future `find_package` harder.

### 2026-08-29 - The Phase 5 stack is merged, and one question is left

**All seventeen Phase 5 branches are on `main`.** PRs #24 to #41 went in as merge commits, ending
with `p5.16-raytracing` at `117d45b`. Nothing is in flight.

**Nothing about the code changed on merge, so nothing above changes.** Every group is still
compile-verified and run-unverified, 3 `EE_UNIMPLEMENTED_FUNCTION` remain, and Phase 5 acceptance
criteria 5 to 10 still need a running engine.

**One item in Phase 5 is left, and it is a decision rather than code: open question 7.** The
engine's command signatures carry root data, no Vulkan indirect draw can bind it, and both
candidate answers change the shaders. It blocks a rendered frame and nothing else in Phase 5 moves
past it. Phase 6 can bring up the window, the input and the swapchain without it.

Docs brought in line with the merge: [README.md](README.md) status,
[Phase5-VulkanRHI.md](Phases/Phase5-VulkanRHI.md) (the indirect note, acceptance criterion 1, and
the P5.13 group), and [Phase6-WindowingInput.md](Phases/Phase6-WindowingInput.md) (its
prerequisites, and P6.6, which still planned to pass an `SDL_Window*` into the RHI).

**Upstream files edited: none.**

### 2026-08-29 - P5.16 Raytracing. The last group, and the least reachable

**`CreateAccelerationStructure`, `GetAccelerationStructureHandle`, `CmdBuildAccelerationStructure`,
`CmdDispatchRays`, the raytracing `CreatePipeline` overload and the indirect ray path are
implemented.** 3 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 9, and none of them is a whole
function. Nothing has run.

**All sixteen groups are now written.**

#### Unreachable three times over, and that is worth stating plainly

- **No caller.** Nothing in `Code/Engine` or `Code/Applications` creates an acceleration
  structure. `RHI.esh` defines `GetRaytracingAccelerationStructure` and no shader uses it.
- **No shader table, on either backend.** `RHI.h` declares no factory for a
  `RaytracingShaderTable`, and `RHI_Direct3D12.cpp` never constructs its own version either, so
  `CmdDispatchRays` is unreachable by construction on both sides.
- **No hardware here.** Only `llvmpipe` reports `VK_KHR_ray_tracing_pipeline` in this machine, the
  same story as mesh shaders.

It is written because the phase document asks for full parity and criterion 1 counts the markers.
It should be treated as unproven code until something calls it.

#### The heap question the binding model left open, answered without changing anything

P5.7 left a note asking whether `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` has to join the
heap's mutable type list, since `RHI.esh` reads an acceleration structure straight out of the heap.

**It does not, because no shader does.** `GetAccelerationStructureHandle` returns a buffer handle
on the top level structure buffer, which is exactly what `RHI_Direct3D12.cpp:4002` returns. The day
a shader actually writes `GetRaytracingAccelerationStructure`, the heap needs the descriptor type
and that is a Phase 4 binding model decision, not this file's.

#### Three places the reference is broken and this is not

| Reference | What happens |
|---|---|
| `RHI_Direct3D12.cpp:3981` | The line that fills in `m_instanceBuffer` is commented out, and `:3390` then dereferences it. The top level build would crash. `CreateAccelerationStructure` here records the buffer. |
| `RHI_Direct3D12.cpp:3978` | The top level structure buffer is created with `BufferFlags::NoDescriptors` and descriptor types `RWBuffer\|Raw`, and `GetAccelerationStructureHandle` then asks it for a `DescriptorTypeFlags::Buffer` handle it cannot have. Two asserts. This one gets the descriptor it is about to be asked for. |
| `RHI_Direct3D12.cpp:3969` | The scratch buffer is sized from the bottom level prebuild alone and then reused for the top level build at `:3392`, which overruns whenever the top level needs more. Here it is sized to the larger of the two. |

All three are recorded under "Upstream issues observed".

#### Four pipeline parameters have no Vulkan equivalent

`m_pEmptyRootSignature`, `m_pRayGenRootSignature`, `m_rayMissRootSignatures` and each hit group's
`m_pRootSignature` are Direct3D 12 **local** root signatures, which let each shader binding table
record carry its own bindings. **Vulkan has one pipeline layout for the whole raytracing pipeline
and nothing else.** Moving the per-record data into the table and reading it in the shader is a
shader change, so the local signatures are dropped and the empty case is asserted.

`m_payloadSize` and `m_attributeSize` are Direct3D's shader config; Vulkan reads both out of the
SPIR-V. `m_maxNumRays` has no counterpart at all.

#### The role of a raytracing shader comes from where it sits, not from the shader

`RHI.h` has one `ShaderStage::RayTracing` for all five roles, so `VulkanShaderStage` cannot tell a
miss shader from a closest hit one. The Vulkan stage is decided by which parameter field the
`Shader` arrived in.

The entry point names need a null terminator that `StringView` does not carry, so they are copied
into a vector **reserved to its exact maximum before the first one is added**. Every
`VkPipelineShaderStageCreateInfo::pName` points into it, and one reallocation part way through
would dangle every pointer taken so far.

#### The third and last file static

`CreateBuffer` has no `Context`, and a raytracing build reads its inputs and stores its result in
ordinary buffers, which need usage bits that only exist once `VK_KHR_acceleration_structure` is
enabled. So every buffer carries them when `g_raytracingEnabled` is true, rather than the RHI
growing a flag it does not have. Same shape as `g_meshShaderEnabled` and
`g_fragmentShadingRateEnabled`.

All three extensions go on together or not at all: `VK_KHR_acceleration_structure`,
`VK_KHR_ray_tracing_pipeline`, and `VK_KHR_deferred_host_operations`, which the first depends on
and which carries no feature bit of its own.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 111 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved. Unchanged on purpose: all
  nine raytracing entry points are extension functions looked up through `vkGetDeviceProcAddr`.
- The new code adds no compiler warning.
- The three remaining markers were read and attributed.

#### Not verified

None of it. No structure built, no ray traced, no raytracing pipeline compiled, and no hardware
here to try. **The first machine with the extensions should not assume any of this works.**

**Upstream files edited: none.**

### 2026-08-29 - P5.15 Variable rate shading. Written, and deliberately switched off

**`CmdSetShadingRate` is implemented and the reported capability stays `NotSupported`.** 9
`EE_UNIMPLEMENTED_FUNCTION` remain, down from 10. Nothing has run, and **nothing can reach this
code until a decision is taken on the Direct3D 12 side.**

#### Why it is off, which is the whole of the group

`RHI_Direct3D12.cpp:2177` sets `m_shadingRate` and `m_shadingRateCaps` to `NotSupported` with a
TODO, so `CmdSetShadingRate` is a no-op on the reference too. Nothing in the engine calls it either
way.

Reporting the real device capability here would be a divergence **we** introduced: the engine would
start shading at a reduced rate on Linux and not on Windows, and acceptance criterion 7 is a
screenshot comparison between the two. So the capability line matches the reference exactly, and
the code behind it is written and reachable the moment both backends change together.

#### Turning it on needs three things, not one

| What | Where |
|---|---|
| The capability line | `FillDeviceCapabilities` here and `RHI_Direct3D12.cpp:2177` there. Both, or the two backends diverge. |
| `VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR` on a rate image | `CreateTexture` does not set it, and a rate image requires it. |
| A view for the rate image | The per-tile path uses the render target view, which P5.6 only builds for a texture created with `DescriptorTypeFlags::RenderTarget`. A rate image wants a view of its own. |

The last two are guesswork until something creates a rate image, and both are small.

#### The structural difference: a command against an attachment

**Direct3D 12 binds the shading rate image with `RSSetShadingRateImage`, a command. Vulkan makes it
an attachment of the render pass.** So `CmdSetShadingRate` records the view on the command buffer
and `BeginRenderingIfPending` chains a `VkRenderingFragmentShadingRateAttachmentInfoKHR` onto the
`VkRenderingInfo`. A pass without one carries no `pNext` at all.

The per-draw rate maps directly: `vkCmdSetFragmentShadingRateKHR`, guarded on the same
`m_shadingRateCaps` the reference copies onto its command buffer at `RHI_Direct3D12.cpp:2862`.

#### Four combiners map and the fifth does not

`Passthrough`, `Override`, `Min` and `Max` are `KEEP`, `REPLACE`, `MIN` and `MAX`. **Direct3D's
`SUM` adds the two rates and Vulkan's nearest operation, `MUL`, multiplies them.** There is no
Vulkan combiner that sums, so `Sum` maps to `MUL` and the two backends would disagree on it.
Nothing calls `CmdSetShadingRate`, so nothing disagrees today.

#### A dynamic state that has to be set even though nothing uses it

Every graphics pipeline declares `VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR` when the extension is
enabled, because `vkCmdSetFragmentShadingRateKHR` needs it. **A declared dynamic state that is
never set leaves every draw undefined**, and nothing in the engine sets it, so `BeginCommandBuffer`
sets the full rate once per command buffer. That is what a pipeline without variable rate shading
does anyway.

The list is built conditionally, through a second file static, `g_fragmentShadingRateEnabled`,
for the same reason `g_meshShaderEnabled` exists: declaring a dynamic state from a disabled
extension is a validation error, and `CreateGraphicsOrMeshPipeline` has no `Context`.

`VK_KHR_fragment_shading_rate` is optional like `VK_EXT_mesh_shader`, and needs both
`pipelineFragmentShadingRate` and `attachmentFragmentShadingRate`.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 111 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved. Unchanged on purpose:
  `vkCmdSetFragmentShadingRateKHR` is an extension function looked up through
  `vkGetDeviceProcAddr`.
- The new code adds no compiler warning.

#### Not verified, and unverifiable as it stands

Nothing here runs while the capability says `NotSupported`. **The pipeline change is the one to
watch**: every graphics pipeline now declares one more dynamic state on a device that has the
extension, and a mistake there breaks every draw rather than only the shading rate. The default
rate set in `BeginCommandBuffer` is what stops that.

**Upstream files edited: none.**

### 2026-08-29 - P5.14 Mesh shaders. No GPU in this machine has them

**`CmdDispatchMesh`, the mesh `CreatePipeline` overload and the indirect mesh path are
implemented.** 10 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 13. Nothing has run.

#### The finding that matters more than the code

**Neither real GPU in this development machine supports `VK_EXT_mesh_shader`.** `vulkaninfo`
reports it on `llvmpipe` alone, the software rasteriser. The Intel UHD 620 and the NVIDIA MX250
both lack it.

That is not a port problem. Both are below the hardware bar for mesh shaders on Direct3D 12 too,
so the engine's debug draw would fail on Windows on this machine as well. It does mean **the debug
draw path cannot be verified here**, and whoever first runs a development build on this machine
will hit the assert in `CmdDispatchMesh`.

#### Optional, not required, and that is a decision

Every other capability the backend needs is in `g_requiredDeviceExtensions`, and a device missing
one is refused at `CreateContext` with the extension named. Mesh shaders are not, for one reason:
**the engine has no capability flag for them and no fallback path.** `RenderPass_DebugDraw` calls
`CmdDispatchMesh` outright, `RHI.h` has no mesh shader field in `DeviceCapabilities`, and nothing
anywhere asks whether they exist. Direct3D 12 simply assumes the hardware has them.

Requiring the extension would refuse a device the rest of the engine renders on perfectly well,
which is exactly the situation this machine is in. So the extension is asked for when present,
`CreateContext` logs a warning when it is missing, and every use asserts.

#### A mesh dispatch is a draw

`CmdDispatchMesh` calls `PrepareDraw`, not the flush-and-suspend pair `CmdDispatchCompute` uses.
`DispatchMesh` is Direct3D's name for it; it rasterises, it runs inside a render pass, and leaving
the pass for it would be wrong.

#### One pipeline body for two overloads

`MeshPipelineParameters` derives from `GraphicsPipelineParameters` and adds nothing, and Vulkan
builds both with `vkCreateGraphicsPipelines`. The whole delta is which shader stages are wanted
and whether there is an input assembler, so `CreateGraphicsOrMeshPipeline` takes a flag and both
overloads call it. A second copy of two hundred lines of blend, depth, raster and dynamic
rendering state would only be a place for the two to drift apart.

`pVertexInputState` and `pInputAssemblyState` are null on a mesh pipeline. There is no input
assembler in front of a mesh shader, and the spec says to leave both out.

#### A barrier correction that P5.9 flagged and could not make

`VulkanPipelineStage` mapped `PipelineStage::NonPixelShader` and `AllShader` onto every shader
stage **except task and mesh**, because naming a stage from a disabled extension is a validation
error. Now that the extension is conditionally enabled, both bits go in when it is.

**Without them a barrier before a mesh draw would not cover the stage that reads the result**,
which is a silent wrong-data bug rather than a validation one. `VulkanPipelineStage` is handed
flags and no `Context`, so the answer is a file static, `g_meshShaderEnabled`, set by
`CreateContext` and cleared by `DestroyContext`. One context at a time, which is what the engine
creates, and the same shape the leak counters already use.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 111 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved. The count is unchanged on
  purpose: all three mesh entry points are extension functions looked up through
  `vkGetDeviceProcAddr`.
- The new code adds no compiler warning.
- `vulkaninfo` was read for the mesh shader support on both GPUs in this machine, rather than
  assumed.

#### Not verified

No mesh shader compiled into a pipeline, no mesh draw issued, and **it cannot be verified on this
machine at all**. The first hardware with `VK_EXT_mesh_shader` should check the debug draw pass
before anything else, because it is the only consumer.

**Upstream files edited: none.**

### 2026-08-29 - P5.11 Query pools

**All seven query functions are implemented, and the `SetDebugName` overload P5.12 owed to this
group is filled in.** 13 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 21. Nothing has run.

#### A correction: nothing in the engine calls any of this

The P5.12 entry said P5.11 was needed for "the query pools a development build's profile scopes
use every frame". **That is wrong.** `EE_RHI_COMMAND_BUFFER_PROFILE_SCOPE` at `RHI.h:1705` expands
to a CPU profile scope and a `CommandBufferMarkerScope`, which is a debug marker. It records no
timestamp. There is no `CreateQueryPool` call anywhere in `Code/Engine` or `Code/Applications`.

The group is written for parity, not because the frame needs it.

#### The four mappings that are not one for one

| `RHI.h` | Vulkan |
|---|---|
| `CmdResetQueryPool` | **Direct3D 12 does nothing here and Vulkan requires it.** This is the one place in the backend where the asymmetry runs that way: a Vulkan query is undefined until it has been reset. It may not run inside a render pass, so it goes through `PrepareTransfer`. |
| `CmdBeginQuery` on a timestamp pool | Nothing. A timestamp is written at one point, not over a range, and `vkCmdBeginQuery` on a timestamp pool is a validation error. It matches what the reference achieves anyway; see "Upstream issues observed". |
| `CmdEndQuery` on a timestamp pool | `vkCmdWriteTimestamp2` at `BOTTOM_OF_PIPE`, because Direct3D's `EndQuery` timestamp is taken after the work the scope covers. **It is legal inside a render pass**, which matters: a profile scope around a pass must not tear it the way a reset would. |
| `GetQueryTimestampFrequency` | `1e9 / timestampPeriod`. Vulkan reports nanoseconds per tick and Direct3D 12 reports ticks per second, which is the inversion the phase document asks for. |

#### Two things Direct3D 12 has no equivalent of, and both are asserted

- **A queue family may report zero valid timestamp bits**, meaning it cannot write one at all.
  `VulkanQueue` now carries `m_timestampValidBits` and `m_timestampPeriod`, both read in
  `CreateQueue`, because `GetQueryTimestampFrequency` is handed a `Queue` and no `Context`.
- **`pipelineStatisticsQuery` is a device feature**, and a `PipelineStatistics` pool cannot be
  created without it. It is enabled **when the device has it and never required**, so a device
  missing it is not refused over a capability nothing uses. `CreateQueryPool` asserts on the flag.

#### The resolve writes eight bytes per query, which is only right for a timestamp

The destination offset is the reference's, `startQuery * 8` at `RHI_Direct3D12.cpp:3528`, and a
pipeline statistics query resolves to eleven counters rather than one. Both backends have to write
the same layout, so `CmdResolveQuery` asserts the pool is a timestamp pool rather than inventing a
second layout. Nothing creates a statistics pool.

`VK_QUERY_RESULT_WAIT_BIT` is set, because Direct3D's `ResolveQueryData` reads finished results.
Without it the copy could write nothing and report availability separately.

#### The eleven pipeline statistics all map

`D3D12_QUERY_DATA_PIPELINE_STATISTICS` has eleven counters and every one has a Vulkan equivalent,
which is unusual enough to be worth saying. They are listed in declaration order in
`CreateQueryPool`.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 111 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved. The seven this adds are
  `vkCreateQueryPool`, `vkDestroyQueryPool`, `vkCmdResetQueryPool`, `vkCmdBeginQuery`,
  `vkCmdEndQuery`, `vkCmdWriteTimestamp2` and `vkCmdCopyQueryPoolResults`.
- The new code adds no compiler warning.
- **All 13 remaining `EE_UNIMPLEMENTED_FUNCTION` were read and attributed**: six P5.16, three
  P5.14, one P5.15, one open question 7, two markers.

#### Not verified

No query written, no timestamp read. The first thing worth checking is the frequency inversion,
because a wrong one gives plausible-looking timings that are wrong by a constant factor, which is
the sort of thing nobody notices.

**Upstream files edited: none.**

### 2026-08-29 - P5.12 Debug names and markers

**Eight of the nine `SetDebugName` overloads, `CmdBeginDebugMarker`, `CmdEndDebugMarker` and
`CmdWriteDebugMarker` are implemented.** 21 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 32.
`BeginFrameCapture` and `EndFrameCapture` were already done in P5.1, so the group is complete
apart from the one overload P5.11 has to unblock. Nothing has run.

Most of this group was already paid for. `SetVulkanObjectName` has existed since P5.2 and every
`Create*` call has been naming its objects through it, so the nine overloads are a handle and an
object type each.

#### The three that needed thought

**`SetDebugName( QueryPool* )` halts**, because there is no `VulkanQueryPool` type to cast to
until P5.11 defines one. It is the only one of the nine that does.

**`SetDebugName( CommandSignature* )` does nothing, and that is the finished answer.** P5.13's
command signature is a record of one command's byte layout and creates no Vulkan object, so there
is no handle for a name to reach. Its parameters are unnamed to say so.

**`CmdWriteDebugMarker` is `vkCmdFillBuffer`, and it loses the ordering.** Direct3D 12 uses
`WriteBufferImmediate`, whose `MARKER_IN` and `MARKER_OUT` modes mean "before everything already
submitted" and "after". Vulkan spells that `VK_AMD_buffer_marker`, which is not enabled here and
would be a device requirement the Phase 4 list does not have.

The concrete loss is in the `InOut` case: **Direct3D writes the In value at the top of the pipe
and the Out value at the bottom, so a crash between the two leaves the In value in the buffer,
which is the entire point of a breadcrumb.** Two fills run in order and the second overwrites the
first. `DeviceCapabilities::m_breadcrumbs` is `false` on this backend and nothing in the engine
calls the function, so nothing loses anything today. Turning breadcrumbs on means enabling the
extension first.

#### Markers

`VK_EXT_debug_utils` labels, which RenderDoc and every Vulkan profiler read the way PIX reads a
Direct3D event. The extension is enabled whenever the loader has it, with or without the
validation layer, so markers are present in a Release build too.

The golden-ratio HSV colour walk is copied from `RHI_Direct3D12.cpp:3628` verbatim, starting from
the same `0.5F`, so a marker gets the same colour on both backends. `RHI.h` holds no such helper
and this file may not add one to it.

**`EndCommandBuffer` now asserts the marker scope counter is zero**, the same check
`RHI_Direct3D12.cpp:2947` makes. Vulkan is stricter than Direct3D here: an unbalanced label is a
validation error rather than a cosmetic problem. The counter is kept even when the extension is
missing, so the assert still catches an unmatched scope on a machine without it.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 104 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved. The count is unchanged on
  purpose: the two label entry points are looked up through `vkGetInstanceProcAddr` like every
  other `VK_EXT_debug_utils` function, and `vkCmdFillBuffer` was already linked by P5.10.
- The new code adds no compiler warning.

#### Not verified

No object named, no marker recorded. The first thing to check is that names appear in a RenderDoc
capture, which is also the cheapest thing to check, and it makes every remaining group easier.

**Upstream files edited: none.**

### 2026-08-29 - P5.13 Indirect draws. The engine's command signatures do not fit Vulkan

**`CreateCommandSignature` and `DestroyCommandSignature` are implemented. `CmdExecuteIndirect` is
implemented for a signature that carries only a draw or dispatch argument, and refuses the rest at
the line.** **P5.13 is not a real group**, because no engine call site takes the path that works.
Nothing has run.

The `EE_UNIMPLEMENTED_FUNCTION` count is unchanged at 32, and that is the honest number: two whole
functions became real and `CmdExecuteIndirect` gained two named refusals in place of one blanket
one.

#### Why it does not fit, in one picture

A Direct3D 12 command signature can set root constants and bind root descriptors per command.
**Vulkan's indirect draws read draw arguments and nothing else.** `EngineShader.cpp:108` walks the
root signature's descriptor reflections and emits one argument per root parameter ahead of the draw
argument, so one material command is laid out like this:

```
[ root constants   40 bytes ]   set 0 binding b0, a uniform buffer on Vulkan
[ root CBV address  8 bytes ]   set 0 binding b1, a uniform buffer on Vulkan
[ dispatch args    12 bytes ]   VkDispatchIndirectCommand
```

`vkCmdDrawIndirect` takes a stride, so it reads the last block out of the fat struct without help.
It cannot rebind the first two per command, and `BucketResolve.esf:36` writes a different value
into them for each command in the buffer.

**A compute pre-pass does not cover it**, which answers the question Phase 4 left open when it set
`m_indirectRootConstant` to `false`. A pre-pass can repack the draw arguments, and repacking is not
needed because of the stride. A pre-pass cannot bind a descriptor.

**Every engine signature carries root data.** `MaterialShader`, `SurfaceShader` and `ComputeShader`
all build theirs the same way, and the two argument-writing shaders, `BucketResolve.esf` and
`InstanceCulling.esf`, both fill in a per-command root CBV address. Nothing reads the
`m_indirectRootConstant` capability, so the engine does not offer a narrower path.

#### The decision taken

**Land the mechanical half and refuse the rest, by decision.** The alternative shapes both need a
change on the shader side, which is Phase 4's and not this file's:

| Shape | What it costs |
|---|---|
| The shader reads its own command's root data by indexing the argument buffer with `SV_DrawIndex`, which Vulkan has as core `gl_DrawID`. One indirect call then covers the whole buffer. | No extension. Changes `RHI.esh` and the renderer shaders, so all 46 stages recompile and revalidate, and Windows sees the change too. |
| Root constants become Vulkan push constants and root descriptors become buffer device addresses, driven by `VK_EXT_device_generated_commands`. | That extension sets push constants per command and still cannot bind a descriptor set, so it needs the same shader change **plus** a device requirement the Phase 4 list does not have. |

Recorded as open question 7.

#### What is real

- **`CreateCommandSignature`** records the byte layout of one command: the argument type, the
  stride, the offset of the draw argument inside the command, and whether root arguments are
  present. The arithmetic accumulates the same byte sizes `RHI_Direct3D12.cpp:3746` does, because
  both backends read one buffer a shader wrote and have to agree on where every field is. There is
  no Vulkan object to create.
- **`CmdExecuteIndirect`** maps `Draw` and `DrawIndexed` onto `vkCmdDrawIndirect` and
  `vkCmdDrawIndexedIndirect`, or their `Count` forms when a counter buffer is passed, reading the
  draw argument at its offset with the signature's stride. `DispatchCompute` maps onto
  `vkCmdDispatchIndirect`.

#### Three things the working path still cannot do, each asserted at the line

| Case | Why |
|---|---|
| A `DispatchCompute` signature with a counter buffer, or `maxNumCommands` above 1 | `vkCmdDispatchIndirect` runs exactly one dispatch and reads no count buffer, where Direct3D 12 runs `min( maxNumCommands, count )`. A caller would silently get one dispatch, so both are refused. |
| A `DispatchMesh` signature | `vkCmdDrawMeshTasksIndirectEXT`, which cannot be named until P5.14 enables `VK_EXT_mesh_shader`. |
| A `DispatchRays` signature | `vkCmdTraceRaysIndirect2KHR`, which is P5.16's. |

#### A correction to what P5.10 recorded as owed here

That entry said `CmdExecuteIndirect` should call `PrepareTransfer` and then
`BeginRenderingIfPending`, which would end the render pass and immediately restart it. **An
indirect draw wants exactly what `PrepareDraw` gives an ordinary one**, and only an indirect
dispatch has to leave the pass. It now branches on the signature's argument type.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 104 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved.
- Every engine `CmdExecuteIndirect` and `CreateCommandSignature` call site was read, along with the
  two shaders that write the argument buffers and the `DrawArgument` and `DebugDrawMeshArgument`
  layouts. That is what established that no call site takes the working path.

#### Not verified

Nothing indirect has been drawn. There is no point checking the working path against the engine
until open question 7 is answered, because no engine call site reaches it.

**Upstream files edited: none.**

### 2026-08-29 - P5.3 Swapchain. A Vulkan queue does not run its submits in order

**`CreateSwapchain`, `DestroySwapchain`, `AcquireNextImage`, `SetVSync` and `QueuePresent` are
implemented.** 32 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 37. Nothing has run.

#### The correction this group forced on P5.2, which matters more than the swapchain

**A Direct3D 12 queue executes its command lists in the order they were submitted. A Vulkan queue
does not.** Two `vkQueueSubmit2` calls on one `VkQueue` may overlap unless something orders them.

The engine relies on the Direct3D guarantee. `ForwardShadingRenderer::SubmitGraphicsCommandBuffer`
submits several graphics command buffers a frame with the barriers recorded across them, and
**`RHI.h` gives it no way to ask for the ordering**: `QueueDeviceWait` asserts that the two queues
differ, so a queue cannot be made to wait on itself.

`RecordQueueOrderingWait` now makes every submit wait on the value the previous submit on that
queue signalled. That is the Direct3D semantics exactly. It costs the overlap a Vulkan driver
might otherwise have found, and the alternative is a race that no validation layer reports.

It is also what makes the swapchain sound, which is how it was found. See below.

#### `m_pNativeWindowHandle` is a `VkSurfaceKHR`, and the application owns it

This is the first of the two answers Phase 5 owes Phase 6.

Direct3D 12 takes an `HWND` and asks DXGI for a swapchain. Vulkan needs a `VkSurfaceKHR`, and
creating one needs a window system library. **`Base/Render` depends on no such library and must
not start to**, so the application creates the surface from the instance and hands it over.
`SDL_Vulkan_CreateSurface` returns exactly that.

Two consequences:

- **`CreateContext` now enables the surface instance extensions**, even though no window exists
  yet. A surface may only be created from an instance that enabled its platform extension, and
  the instance is created once. `VK_KHR_surface` plus xlib, xcb and wayland, whichever the loader
  reports. They are named by string rather than by macro, because the macros only exist once
  `VK_USE_PLATFORM_*` is defined and that drags X11 headers into a file with no other use for
  them.
- **`DestroySwapchain` never destroys the surface.** `Window::ResizeSwapchain` destroys and
  recreates around an unchanged `m_pNativeWindowHandle`.

#### The application drives swapchain recreation, not the RHI

The second answer. `Engine.cpp:754` and `ImguiRenderer.cpp:91` both compare the window size
against `GetSwapchainSize()` and call `Window::ResizeSwapchain`, and each waits the graphics queue
idle first. So `AcquireNextImage` and `QueuePresent` accept `VK_SUBOPTIMAL_KHR` and
`VK_ERROR_OUT_OF_DATE_KHR` instead of recreating behind the engine's back.

`VK_ERROR_OUT_OF_DATE_KHR` from the acquire is the one case with teeth: **no image is acquired and
the semaphore is not signalled**, so recording a wait on it would hang the queue. That path returns
the image index it already held.

#### A null handle means headless, which is all of Phase 5

There is no window until Phase 6, so a null `m_pNativeWindowHandle` builds a swapchain with no
`VkSurfaceKHR` and no `VkSwapchainKHR`: a ring of ordinary offscreen render targets,
`AcquireNextImage` cycles the index, and `QueuePresent` signals its timeline value and presents
nothing. That is the phase document's own bring-up order, and it is what lets ladder steps 6 and
7 run as soon as there is an entry point to run them from.

#### Binary semaphores, which P5.2 left to this group

`VkPresentInfoKHR` has no timeline path, so:

| Semaphore | Count | Why |
|---|---|---|
| Acquire | One per image, used as a ring | `vkAcquireNextImageKHR` is told which semaphore to signal *before* it says which image it gave, so it cannot be indexed by image. The engine host-waits on the previous frame's timeline value before reusing a slot, so a ring of `MaxPendingFrames` is safe. |
| Present | One per image | An image is not presented again until it has been acquired again. |

The acquire wait goes onto the present queue's `m_pendingWaits`, which the next submit drains.
That is only sound because of the queue ordering above: the submit that writes the swapchain image
is not the first one after the acquire.

`QueuePresent` submits and then presents, where Direct3D 12 presents and then signals. It has to:
`vkQueuePresentKHR` waits on a semaphore only a submit can signal. The returned value still means
"the frame is done", which is all the engine reads it for.

#### Two smaller mappings

- **The swapchain image is created sRGB.** Direct3D 12 creates it `UNorm` and puts an sRGB render
  target view on it, which in Vulkan would need `VK_KHR_swapchain_mutable_format`. Creating the
  image in `m_renderTargetFormat` gives the same conversion on write and the same picture, with no
  extension. `m_colorFormat` is the fallback if the surface refuses it.
- **`SetVSync` picks a present mode and a later call needs a recreation to take effect**, because
  Vulkan fixes the mode when the swapchain is created. FIFO with vsync; MAILBOX or else IMMEDIATE
  without it. Nothing in the engine calls it outside `CreateSwapchain`, so the two backends behave
  identically today.

#### The one thing that can stop Phase 6 dead

**`minImageCount` is a minimum, so a driver may return more swapchain images than were asked for.**
`Swapchain::m_renderTargets` is a fixed `TArray` of `MaxPendingFrames`, which is 2, and several
Linux drivers want three or four. `CreateSwapchain` logs the two numbers and halts. The fix is
`MaxPendingFrames` in `RHI.h`, which is an upstream file and a human decision.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 99 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved.
- Every engine call site of `AcquireNextImage`, `QueuePresent`, `CreateSwapchain` and
  `QueueSubmit` was read, along with the frame order in `Engine.cpp`. That is what found the
  queue ordering problem.

#### Not verified

Nothing has been acquired or presented. The order to check things in:

1. **The image count.** It halts on a driver that wants more than two, and it is the first thing
   that will happen on real hardware.
2. **The queue ordering wait.** If it is wrong, passes read each other's half-written targets.
   Sync validation is what names it.
3. **The acquire wait reaching the right submit.** A validation error names it.

**Upstream files edited: none.**

### 2026-08-29 - P5.10 Copies and clears. A clear needs a barrier the engine does not record

**`CmdCopyBuffer`, both `CmdCopyTexture` overloads, `CmdClearTexture` and `CmdClearBuffer` are
implemented.** 37 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 42. Nothing has run.

The five commands are short. Three things around them are not, and each is a place where Direct3D
12 needs nothing and Vulkan needs something.

#### A Direct3D clear is a shader write, and a Vulkan clear is a transfer write

`ClearUnorderedAccessViewUint` writes through an unordered access view, so
`Renderer_ForwardShading.cpp:753` follows its clears with a barrier whose source is
`ResourceAccess::UnorderedAccess`. That is a shader storage write. `vkCmdFillBuffer` is a transfer
write, which that barrier does not cover at all, so **the culling counters would be read stale and
no validation layer would say a word**.

So every clear records the transfer half of its own visibility barrier, batched like the rest and
flushed with them at the next dispatch. `RecordClearVisibilityBarrier` is the one place it lives.
Its destination is `ALL_COMMANDS` and it is listed in the table above as a site to narrow.

#### A copy needs a layout the engine never asks for

`D3D12_BARRIER_LAYOUT_COMMON` is already a legal copy source, copy destination and unordered
access view clear target, so **the engine issues no layout barrier before a texture upload**. It
issues a global memory barrier and nothing else: `RenderSystem.cpp:489` and `:759` are the two
sites. Vulkan needs `GENERAL` or one of the `TRANSFER` layouts, and the image is still in the
`UNDEFINED` that `vkCreateImage` gave it, which is P5.6's recorded obligation.

`TransitionTextureForTransfer` records that barrier, once per texture, into `GENERAL` and not
`TRANSFER_DST_OPTIMAL`. `GENERAL` is what `TextureState::Common` maps to, so the engine's belief
about this texture stays true and the next barrier it records still passes `CmdBarrier`'s assert.
Every texture the engine copies into is created `Common`; `RenderSystem.cpp:650` asserts it.

#### The staging row stride, which P5.6 left to this task

`vkCmdCopyBufferToImage` takes its row length in texels and the engine lays out its staging rows
at the byte stride `GetTextureCopyRowStride` reports. `CopyRowLengthInTexels` converts the one
into the other, and **both** copy overloads use it, so the readback direction reads rows at the
stride the upload direction writes them. Direct3D 12 uses the destination buffer's own footprint
for the readback, which for a buffer resource is the whole buffer as a single row and says nothing
about texture rows.

#### Two clear-value divergences, and nothing calls either path today

- `CmdClearTexture` has **no caller anywhere in the engine**. `vkCmdClearColorImage` converts the
  clear value to the image format and `ClearUnorderedAccessViewUint` writes the raw bits, so the
  two agree on an integer format and disagree on a normalised one.
- `CmdClearBuffer` fills 32-bit words. Direct3D fills components of the view's format. Every
  buffer the engine clears is a counter or a 32-bit typed buffer, so the two agree; a 16-bit
  format would not.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 91 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved. The five this task adds -
  `vkCmdCopyBuffer`, `vkCmdCopyBufferToImage`, `vkCmdCopyImageToBuffer`, `vkCmdFillBuffer` and
  `vkCmdClearColorImage` - are all among them.
- Every engine `CmdClearBuffer`, `CmdCopyBuffer` and `CmdCopyTexture` call site was read, along
  with the barriers around it, which is what found the clear visibility hole.

#### Not verified

No copy issued, no buffer filled. The order to check things in:

1. **The clear visibility barrier.** If it is wrong the culling counters are garbage and the frame
   is empty or wildly wrong. Sync validation is the tool that names it.
2. **The staging row stride.** A wrong stride skews every uploaded texture, which looks like a
   decode bug rather than a copy bug.
3. That `TransitionTextureForTransfer` leaves `m_currentLayout` agreeing with the engine's
   tracker. `CmdBarrier`'s assert is what says otherwise.

**Upstream files edited: none.**

### 2026-08-29 - P5.9 Barriers. The render pass now opens at the draw, not at the bind

**All three `CmdBarrier` overloads are implemented**, on synchronization2. 42
`EE_UNIMPLEMENTED_FUNCTION` remain, down from 45. Nothing has run.

Barriers are batched on the command buffer and flushed in one `vkCmdPipelineBarrier2` at every
draw, every dispatch and `EndCommandBuffer`, which are the points `RHI_Direct3D12.cpp:1586`
flushes at. The mapping work is the small half of this task. The large half is below.

#### The render pass had to become lazy, and this is the reason

**The engine records image layout barriers between `CmdSetRenderTargets` and the first draw.** It
is not an accident or one pass being odd; it is the shape of every pass in the renderer:

```
resourceStates.Writeable( target, ... )      // the target becomes a render target
resourceStates.FlushBarriers( cb )           //   -> RHI::CmdBarrier
RHI::CmdSetRenderTargets( cb, target, ... )
rootConstants.SetSourceTexture( resourceStates, ... )   // a source becomes shader-readable
resourceStates.FlushBarriers( cb )           //   -> RHI::CmdBarrier, *after* the bind
RHI::CmdDraw( cb, 3, 0 )
```

`RenderPass_SMAA.cpp:154`, `:190` and `:218`, `RenderPass_GTAO.cpp:435` and `:462` all do it. **A
barrier may not run inside dynamic rendering**, so a `CmdSetRenderTargets` that called
`vkCmdBeginRendering` would put every one of those barriers inside a pass.

So `CmdSetRenderTargets` records the attachment configuration and does not begin. The first draw
flushes the barriers and then begins, through `PrepareDraw`. Direct3D 12 has the same shape for
its own reason: `OMSetRenderTargets` is state, and its batched barriers flush at the draw.

Three consequences worth knowing before touching that code:

| Case | What happens |
|---|---|
| A pass with a clear and no draw | `FlushRendering` begins and immediately ends it, so the clear still happens. `CmdSetRenderTargets` and `EndCommandBuffer` both call it. |
| A barrier or dispatch between two draws of one pass | `SuspendRendering` ends the pass and forces every load op to `LOAD`, so the next draw resumes it without losing what the first half drew. No engine pass does this today. |
| The attachment's `imageLayout` | Read from `VulkanTexture::m_currentLayout`, not hard-coded. `RenderPass_DebugDraw.cpp:1342` binds a depth target it only reads, which is `DEPTH_STENCIL_READ_ONLY_OPTIMAL` and not the attachment layout. |

#### A correction to P5.8: load and store actions were discarding the frame

**`LoadAction` is zero initialised, and zero is `DontCare` for both actions.** Direct3D 12 has no
load or store actions at all - binding a render target preserves it, and the backend reads
`m_loadActionsColor` only to decide whether to call `ClearRenderTargetView` - so every action the
engine leaves alone arrives in the backend as `DontCare`.

- **No engine pass sets a store action at all.** Mapping `StoreActionType::DontCare` to
  `VK_ATTACHMENT_STORE_OP_DONT_CARE` discards the output of every render pass in the frame.
- **`RenderPass_DebugDraw.cpp:1316` builds a `LoadAction` that sets only the depth action**, then
  binds the frame's final colour target with it at `:1358`. `VK_ATTACHMENT_LOAD_OP_DONT_CARE`
  there discards the whole rendered frame.

Both `DontCare` values now preserve, which is what the reference backend does. `Clear`, `Load`
and `StoreActionType::None` are unchanged, so a caller that means "discard" still has
`StoreActionType::None` to say it with. **This is a deliberate divergence from the phase
document's literal mapping** and it is written up under "Upstream issues observed" below.

#### Queue ownership: `CONCURRENT`, which P5.5 left to this task

Direct3D 12 resources have no queue ownership. A buffer written on the compute queue is read on
the graphics queue with only a barrier between, and the engine's async compute path relies on it.
Vulkan's `EXCLUSIVE` sharing needs an ownership transfer for the same thing, and **nothing in
`RHI.h` says which queue last touched a resource**, so there is nothing to build a transfer from.

`SetSharingMode` gives every buffer and image `CONCURRENT` across the distinct queue families the
context uses, and leaves `EXCLUSIVE` when there is only one family. It costs some compression on
some hardware. It is the mapping that reproduces the Direct3D semantics exactly, and the
alternative is silent corruption across queues.

#### The mappings, and the two entries that are deliberately broad

`PipelineStage` to `VkPipelineStageFlags2`, `ResourceAccess` to `VkAccessFlags2`, `TextureState`
to `VkImageLayout`. Four of them do not line up one for one:

| `RHI.h` | Vulkan |
|---|---|
| `PipelineStage::Draw` | `ALL_GRAPHICS`, because `D3D12_BARRIER_SYNC_DRAW` is every stage a draw runs through. It has to cover the depth test stages: the engine transitions a depth target with `Draw` and never with a depth stage of its own. |
| `PipelineStage::NonPixelShader` | Every shader stage except fragment, compute included, as Direct3D has it. The task and mesh stage bits belong here and are left out until P5.14 enables `VK_EXT_mesh_shader`, because naming a stage from a disabled extension is a validation error. |
| `PipelineStage::Copy` | `ALL_TRANSFER`, not `COPY`. Direct3D's `SYNC_COPY` sits next to `SYNC_CLEAR` and `SYNC_RESOLVE` and the RHI has no separate flag, so a clear arrives here as `Copy`. |
| `TextureState::ShaderResource` | `VulkanTexture::m_shaderReadLayout`, which P5.6 set. It is `GENERAL` when the texture is also an `RWTexture`, because that is the layout its heap descriptor was written with. |

`PipelineStage::All` and `ResourceAccess::Common` map to `ALL_COMMANDS` and
`MEMORY_READ|MEMORY_WRITE`. Both are in the `ALL_COMMANDS` table above with why, and both are the
meaning of the flag rather than laziness.

`PipelineStage::VideoProcess`, `ResourceAccess::VideoProcessRead` and `VideoProcessWrite` have no
Vulkan equivalent at all: Vulkan has no video processing queue. Nothing asks for them.

#### P5.6's three obligations are met

1. `CmdBarrier` takes `oldLayout` from `VulkanTexture::m_currentLayout`, never from the caller's
   `sourceState`, because a `VkImage` starts in `UNDEFINED` whatever `m_initialState` says. The
   caller's belief is asserted against the truth rather than used, so the two cannot drift.
2. `TextureState::ShaderResource` resolves through `m_shaderReadLayout`.
3. `UnorderedAccess` is `GENERAL`.

One layout is tracked per image, which is exact only while callers barrier the whole texture.
`DeviceResourceStates::FlushBarriers` passes an empty `TextureBarrierRegion`, so every engine
barrier does.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 87 `vk*` symbols resolve against `libvulkan.so.1`, up from 86. None unresolved.
- Every engine `CmdSetRenderTargets` call site was read to confirm the deferred begin is needed
  and sufficient, rather than assumed from one pass.

#### Not verified

No barrier issued, no layout moved. The order to check things in:

1. **The load and store action correction.** If it is wrong, the frame is blank or garbage, and
   that is the loudest failure here rather than the quietest.
2. That the deferred begin puts every barrier outside its pass. A validation error names the
   exact draw if not.
3. That `m_currentLayout` agrees with the engine's tracker after a frame. The assert in
   `CmdBarrier` is what says otherwise.

**Upstream files edited: none.**

### 2026-08-29 - P5.6 Textures and samplers. The format mapping is complete

**All seven functions are implemented, and the `DataFormat` to `VkFormat` mapping is finished.**
45 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 52. Nothing has run.

**All 99 `DataFormat` members map, and there is exactly one mapping.** The phase document warns
that two mappings which disagree corrupt textures in a way that looks like a bug somewhere else,
so `VulkanFormat` is the only one: image creation, buffer views, pipeline attachment formats and
the device capability query all come through it.

#### Two things in the format mapping that are easy to get backwards

**Vulkan names a packed format most significant component first, and DXGI names it least
significant first.** So `DXGI_FORMAT_B5G6R5_UNORM` is `VK_FORMAT_R5G6B5_UNORM_PACK16`, not
`VK_FORMAT_B5G6R5_UNORM_PACK16`. The same reversal applies to every packed entry:
`B5G5R5A1` becomes `A1R5G5B5`, `B4G4R4A4` becomes `A4R4G4B4`, `R10G10B10A2` becomes
`A2B10G10R10`, `R11G11B10` becomes `B10G11R11`, `R9G9B9E5` becomes `E5B9G9R9`. Getting one
backwards swaps red and blue on that format alone, which is exactly the kind of failure that
looks like a bug in the asset.

**`RGB565_UNorm` and `BGR565_UNorm` deliberately map to the same `VkFormat`.** Vulkan can tell
them apart and Direct3D cannot, so mapping them faithfully would make the two backends draw the
same asset differently. See the upstream note below. Nothing in the engine uses either.

Two places where Vulkan is the more exact of the two, and the mapping says so rather than
copying Direct3D:

| `DataFormat` | Direct3D 12 | Vulkan |
|---|---|---|
| `DXBC1_RGB_*` against `DXBC1_RGBA_*` | Both are `DXGI_FORMAT_BC1_UNORM` | `VK_FORMAT_BC1_RGB_*` and `VK_FORMAT_BC1_RGBA_*`, which is the distinction the `DataFormat` enum already makes |
| The 28 ASTC formats | `DXGI_FORMAT_UNKNOWN`, no ASTC in Direct3D | All present, gated on `textureCompressionASTC_LDR`, which nothing enables. `FillDeviceCapabilities` reports each one honestly |

`R1_UNorm` has no Vulkan equivalent at all and returns `VK_FORMAT_UNDEFINED` without asserting,
which mirrors what the Direct3D 12 backend does with the ASTC formats it cannot express.

#### Views, because Vulkan puts the subresource in the view

Direct3D 12 selects a subresource in the descriptor. Vulkan selects it in the `VkImageView`, so
`CreateTexture` builds every view the engine can ask for, up front:

| View | How many | Why |
|---|---|---|
| Sampled | one | Covers every mip and layer. A depth-stencil image must name one aspect, and depth is the one the engine reads, matching `RHI_Direct3D12.cpp:4597`. |
| Storage | one per mip level | An `RWTexture` handle names a mip. `RenderPass_GTAO.cpp:263` asks for five of them on one texture. |
| Attachment | one per mip level per array layer | `RenderPass_GlobalEnvironmentMap.cpp:304` renders to one face of a cube at one mip, and `RenderPass_CascadedShadow.cpp` to one of four array slices. |

**That closes the assert P5.8 left.** `CmdSetRenderTargets` now honours `colorArraySlices`,
`colorMipSlices`, `depthArraySlice` and `depthMipSlice`, and the render area is the mip's extent
rather than the whole texture's.

An attachment view takes every aspect the image has and the sampled view takes one, which is not
a detail either API makes obvious.

#### Three things P5.9 has to know, and none of them are guessable from `RHI.h`

These are the reason the "owed by later groups" table above now has a long P5.9 row.

1. **A `VkImage` is always created in `VK_IMAGE_LAYOUT_UNDEFINED`.** `vkCreateImage` accepts that
   or `PREINITIALIZED`, and the second is for linear tiling. Direct3D 12 takes the initial layout
   directly, so the engine believes a fresh texture is already in `m_initialState` and the image
   is not. `VulkanTexture::m_currentLayout` records the truth, and the first barrier has to read
   it rather than the state the caller passes.
2. **A texture that is both `Texture` and `RWTexture` sits in `VK_IMAGE_LAYOUT_GENERAL`**, not in
   `SHADER_READ_ONLY_OPTIMAL`. A storage image descriptor may name no other layout and one image
   cannot be in two layouts at once. `VulkanTexture::m_shaderReadLayout` carries the layout the
   descriptor was actually written with. `RenderPass_GTAO.cpp:111` creates such a texture.
3. **`GetTextureCopyRowStride` is P5.10's `bufferRowLength`.** Direct3D 12 reads its answer out of
   `GetCopyableFootprints`; Vulkan has no such call because the staging layout is the caller's to
   choose, so the row stride is `ComputeFormatRowStride` rounded up to the device's
   `optimalBufferCopyRowPitchAlignment`. The engine writes its rows at that stride and
   `vkCmdCopyBufferToImage` has to read them at it.

#### Samplers

Six samplers, created once in `RenderSystem::Initialize`, and two of them need something Vulkan
puts outside `VkSamplerCreateInfo`:

- **`FilterMode::Min` and `FilterMode::Max` are a reduction mode**, not a filter.
  `COMMON_SAMPLER_LINEAR_CLAMP_MAX` is one of the six, so `samplerFilterMinmax` is now a device
  requirement. It is core in Vulkan 1.2.
- **The border colour is one of six fixed values.** Direct3D 12 takes any colour;
  `VK_EXT_custom_border_color` is the only way to that and it is not enabled. Transparent black,
  opaque black and opaque white map; anything else halts and names the sampler. Every sampler the
  engine creates leaves the default of transparent black, and only a `ClampToBorder` address mode
  reads it at all.

#### One correction to P5.5

**`descriptorBindingStorageTexelBufferUpdateAfterBind` was missing.** Every descriptor type in
the heap's mutable list has to support update-after-bind, and the list holds a storage texel
buffer for `RWBuffer<T>`. The other five types were asked for and this one was not, so creating
the set layout would have failed validation on the first run. It is one line, next to its five
siblings, and it changes nothing else.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build still compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged; confirmed by building them without this change.
- 86 `vk*` symbols resolve against `libvulkan.so.1`, up from 81. None unresolved.
- Every one of the 99 `DataFormat` members has a case in `VulkanFormat`, checked by script
  against the enum in `RHI.h` rather than by reading.

#### Not verified

No image created, no view bound, no texture sampled. The order to check things in when Phase 6
makes that possible:

1. **The packed formats.** Nothing in the engine uses one today, so a mistake there will surface
   the first time an asset does, long after this.
2. That a cube face at a given mip is the subresource the attachment view actually names. Six
   faces and nine mips give 54 views and only one right answer per draw.
3. That the sampled and storage descriptors of one texture agree with the layout P5.9 puts it in.

**Upstream files edited: none.**

### 2026-08-28 - P5.8 Render pass and draw commands. Both Phase 4 decisions land

**All thirteen functions are implemented.** 52 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 65.
Nothing has run.

**This is where both Phase 4 decisions become code**, and each is in exactly one place:

- **Clip-space Y is inverted in `CmdSetViewport`**, by moving the origin to the bottom of the
  rectangle and making the height negative. The shader compiler does not invert, and
  `-fvk-invert-y` must never be added. The comment at that line says so and names the other half
  of the decision, so that nobody adds a second flip.
- **Heap set 1 is bound in `CmdSetPipeline`**, not in `BeginCommandBuffer` where Direct3D 12 calls
  `SetDescriptorHeaps` (`:2917`). Vulkan cannot do it once per command buffer: binding a pipeline
  whose layout differs from set N onwards disturbs every set from N up, and set 0 varies per
  shader. The binding model accepted the redundant rebind deliberately.

#### Three places where Vulkan needs something Direct3D 12 does not

**Dynamic rendering has a begin and an end; `OMSetRenderTargets` has neither.** So
`VulkanCommandBuffer` carries an `m_isRendering` flag, `CmdSetRenderTargets` closes the previous
pass before opening the next, and `EndCommandBuffer` closes the last one. **Anything that may not
run inside a render pass has to close it too**: `CmdDispatchCompute` does, and P5.9's barriers and
P5.10's copies will have to. `EndRenderingIfActive` is the one call, and the requirement is
written into the "in flight" table above so it is not discovered by a validation error.

**Clears become load ops.** Direct3D 12 binds targets and then clears with a separate
`ClearRenderTargetView`; here `LoadActionType::Clear` is `VK_ATTACHMENT_LOAD_OP_CLEAR` on the
attachment. That is what the phase document's mapping asks for and what a tiling GPU needs.
`StoreActionType::None` maps to `VK_ATTACHMENT_STORE_OP_NONE`, core in 1.3.

**Dynamic rendering needs a render area and Direct3D 12 has none.** The full extent of the
attachments is used, which is the same thing, and the viewport still restricts what is drawn.

#### Root constants are a ring buffer, as the binding model said they would be

`RHI.esh` declares the block through `EE_DECLARE_ROOT_CONSTANTS` as a `ConstantBuffer`, so DXC
emits a uniform buffer and Vulkan push constants are not available without `[[vk::push_constant]]`
in `RHI.esh`, which Phase 4 rule 4 forbids. So `CmdSetRootConstants` copies into a ring and pushes
a descriptor at the copy.

**One ring per command buffer, 64 KB, reset in `BeginCommandBuffer`.** That is safe without any
frame tracking, because Vulkan already requires a command buffer's previous submission to have
completed before it can be re-recorded. **The ring asserts rather than wraps**: wrapping would
overwrite constants the GPU is still reading, and the failure would look like a shader reading
wrong values, which is a miserable thing to chase. 64 KB against a handful of `uint32`s per set is
generous by a wide margin.

`CmdSetRootParameter` is the same push with the caller's buffer and offset and `VK_WHOLE_SIZE` for
the range, which is what a Direct3D 12 root descriptor is: an address with no size. The binding
model says so explicitly.

Both use `m_shaderResources[index].m_registerIndex`, which P5.7 recorded as holding the **Vulkan
binding** rather than the HLSL register. That is why no shift is applied here.

#### A file reorganisation, because the declaration order forced it

`RHI.h` declares the draw commands before the buffers, textures and pipelines they act on, so a
`VulkanBuffer` defined next to `CreateBuffer` comes too late for `CmdSetRootConstants`. Every
`VulkanXxx` type now sits in one "Resource types" block near the top. **The functions keep
`RHI.h`'s section order**, which is what Conventions rule and the phase document ask for; only the
type definitions moved.

#### Verified, in the only sense available

- All eight Linux targets compile and link. `ninja` exits 0, `Checks.py` passes.
- 81 `vk*` symbols resolve against `libvulkan.so.1`, up from 70.
- Every one of the thirteen functions plus `CmdSetShadingRate` is present exactly once. That was
  checked by name, because the splice that replaced the stubs spanned `CmdSetShadingRate`, which
  belongs to P5.15 and had to be put back.

#### Not verified

No viewport set, no pass begun, no descriptor pushed. The two decisions this group implements are
the ones Phase 4 spent the most effort on, and neither has run. The order to check them in:

1. **The Y flip and the front face together.** Getting both wrong looks correct. Render something
   with a known handedness before trusting either.
2. That a push descriptor lands on set 0 while set 1 stays bound across a pipeline change.
3. That the root constant ring holds a value long enough for the GPU to read it.

**Upstream files edited: none.**

### 2026-08-28 - P5.7 Shaders, root signatures and pipelines. Written, never run

**Graphics and compute pipelines are implemented, and SPIRV-Reflect finally has a caller.** 65
`EE_UNIMPLEMENTED_FUNCTION` remain, down from 74. Four of the 65 are markers rather than
unimplemented RHI functions: the `VulkanFormat` default, the static-sampler path, and the mesh and
raytracing `CreatePipeline` overloads. Nothing has run.

**This closes the set of five groups `RenderSystem::Initialize` needs.** P5.1, P5.2, P5.4, P5.5
and P5.7 are all written. Whether they work is a different question that nothing can answer before
Phase 6.

#### Reflection

`ExtractReflection` mirrors `RHI_Direct3D12.cpp:1003` and produces the same `ShaderReflection` the
engine already reads. The interesting mappings:

| Direct3D 12 | Vulkan |
|---|---|
| `D3D12_SHADER_INPUT_BIND_DESC::Type` | `SpvReflectDescriptorBinding::descriptor_type` |
| `D3D_SIT_STRUCTURED` against `D3D_SIT_UAV_RWSTRUCTURED` | `resource_type & SPV_REFLECT_RESOURCE_FLAG_UAV`. A SPIR-V storage buffer is both read and read-write, so the resource flag is the discriminator rather than the type. |
| `GetThreadGroupSize` | `entry_points[0].local_size` |
| `ID3D12ShaderReflectionConstantBuffer` members | `SpvReflectDescriptorBinding::block.members` |

**`m_registerIndex` holds the Vulkan binding, not the HLSL register.** The binding model shifts
`b`/`t`/`u`/`s` to 0/8/16/24, and un-shifting in reflection only to re-shift in
`CreateRootSignature` is two chances to get it wrong instead of none. Nothing outside the backend
reads it: `EngineShader.cpp` reads only `m_descriptorTypeFlags` and `m_numConstants` from a
`DescriptorReflection`, and uses position in the vector as the parameter index. This is written
down because the field means something different on the two backends, which is exactly the sort of
thing a later reader assumes rather than checks.

**Set 1 bindings are skipped during reflection.** They are the bindless heap, whose layout is
fixed and shared, so they are not root parameters and must not become them.

#### Root signatures

`RootSignature` becomes a `VkPipelineLayout` over two set layouts: set 0, built from reflection
with `VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR`, and set 1, the shared heap layout
from `CreateContext`. The merge of per-stage resources, first seen wins, is copied from
`RHI_Direct3D12.cpp:4946` including its cross-stage asserts, because the order decides
`m_parameterIndex` and both `CmdSetRootParameter` and `EngineShader.cpp` index by position.

**Root constants are a uniform buffer, not Vulkan push constants.** `RHI.esh` declares the block
through `EE_DECLARE_ROOT_CONSTANTS` as a `ConstantBuffer`, so DXC emits a uniform buffer, and
making it a push constant block needs `[[vk::push_constant]]` in `RHI.esh`, which Phase 4 rule 4
forbids. The binding model already recorded the answer: `CmdSetRootConstants` copies into a
per-frame upload ring and pushes a descriptor at it. P5.8 writes that.

**The static sampler path halts rather than warns.** The binding model found no shader using one,
and `CreateRootSignature` keeps the Direct3D 12 warning for a name that matches nothing. If a name
*does* match, that is a new shader using a static sampler and the halt says so at the point it
happens.

#### Pipelines

Graphics uses dynamic rendering, so there is no `VkRenderPass` and no framebuffer:
`VkPipelineRenderingCreateInfo` carries the formats instead. Direct3D 12's single `DSVFormat`
splits into `depthAttachmentFormat` and `stencilAttachmentFormat`.

Viewport, scissor and stencil reference are dynamic state, because `CmdSetViewport`,
`CmdSetScissor` and `CmdSetStencilReference` exist.

**No vertex input state, and that is correct.** `GraphicsPipelineParameters` carries no input
layout at all: the engine pulls vertices out of buffers in the shader. An empty
`VkPipelineVertexInputStateCreateInfo` says exactly that.

**The winding line is the one most likely to be wrong.** The Direct3D 12 backend sets
`FrontCounterClockwise = ( m_frontFace == ClockWise )`, already an inversion of the name. The
Vulkan viewport inverts Y with a negative height, which P5.8 applies, and that reverses winding in
framebuffer space. Inverting the inversion lands back on the name, so `ClockWise` maps to
`VK_FRONT_FACE_CLOCKWISE`. **This is reasoning, not a measurement.** If faces come out inside
out, this line and the sign of the viewport height in `CmdSetViewport` are the only two places
that can be responsible, and getting both wrong looks correct.

Three smaller mismatches, all recorded at the line rather than silently absorbed:

| `RHI.h` | Vulkan |
|---|---|
| `m_depthClip` | `depthClampEnable = !m_depthClip`. Clipping discards the primitive and clamping keeps it at the plane, so the two are not identical. `VK_EXT_depth_clip_enable` is the exact control and is not enabled. Nothing sets `m_depthClip` today. |
| `m_sampleQuality` | No equivalent. It is a Direct3D quality level for a sample count and Vulkan exposes only the count. |
| `m_independentBlend` | No switch. Vulkan is per attachment when the feature is supported and uniform otherwise, and the engine fills every attachment either way. |

A render target outside `m_renderTargetMask` gets a fully default attachment: blending off, no
colour written. That is what Direct3D 12 leaves behind for an unmasked target, and defaulting to a
full write mask instead would silently change what is drawn.

**The pipeline cache is implemented, which the reference is not.** `RHI_Direct3D12.cpp:5232`
asserts `pPipelineCache == nullptr` with "Not implemented yet". `VkPipelineCache` is a handful of
lines, so it is real here; `GetPipelineCacheData` keeps the bytes on the cache object because it
hands back a view.

#### Verified, in the only sense available

- All eight Linux targets compile and link. `ninja` exits 0, `Checks.py` passes.
- **SPIRV-Reflect is genuinely linked**: 45 `spvReflect*` symbols are pulled out of
  `libspirv-reflect.a` and into `libEsoterica.Base.so`. Until this group it was an archive nothing
  referenced.
- 70 `vk*` symbols resolve against `libvulkan.so.1`, up from 60.
- `VulkanFormat` gained the ten render target and depth formats the engine uses, measured from
  every `DataFormat` a texture or pipeline is created with in `Code/Engine`.

#### Not verified

No shader module has been created, no SPIR-V reflected, no pipeline compiled. The specific things
to check first, in order:

1. That `ExtractReflection` produces the same resource list Direct3D 12 does for the same shader.
   A difference here silently changes the root parameter order.
2. The winding, above.
3. That a push descriptor set layout with the reflected bindings is accepted alongside the
   update-after-bind heap set in one pipeline layout.

**Upstream files edited: none.**

### 2026-08-28 - P5.5 Buffers, and the bindless heap becomes code. Written, never run

**All seven functions are implemented, and `CreateContext` now builds the bindless descriptor
heap.** 74 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 80. One of those 74 is not an RHI
function: it is the default case of `VulkanFormat`, which P5.6 fills in. Nothing has run.

This is the group the phase document calls the point where the Phase 4 binding model stops being
a document, so this entry is long.

#### A correction to the binding model, on one flag

The binding model entry says both heap bindings take `PARTIALLY_BOUND` and
`VARIABLE_DESCRIPTOR_COUNT`. **Vulkan does not allow that.** Only the highest-numbered binding in
a set may carry `VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT`
(`VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-pBindingFlags-03004`), so binding 0 cannot have
it while binding 1 exists.

**The flag is also not needed.** It exists to let a set be allocated smaller than its layout
declares, and both heaps are allocated at their full declared size: 65472 and 2048. So it is
dropped from both bindings rather than kept on one.

`PARTIALLY_BOUND` and `UPDATE_AFTER_BIND` stay, and the layout keeps
`UPDATE_AFTER_BIND_POOL`. **Nothing a shader can observe changes**: the set, the bindings, the
descriptor types, the counts and the mutable type list are all exactly as recorded. The flag never
reaches the SPIR-V. This is written up here rather than escalated as a re-decision because it
corrects a detail of how the recorded model is built, not the model itself. **Say so if you
disagree** - the entry it corrects is the hard prerequisite for the whole phase.

#### What the heap looks like in code

Built in `CreateContext`, once, and torn down in `DestroyContext`:

| Binding | Type | Count | Flags |
|---|---|---|---|
| 0 | `VK_DESCRIPTOR_TYPE_MUTABLE_EXT` over six types | 65472 | `PARTIALLY_BOUND`, `UPDATE_AFTER_BIND` |
| 1 | `VK_DESCRIPTOR_TYPE_SAMPLER` | 2048 | `PARTIALLY_BOUND`, `UPDATE_AFTER_BIND` |

The counts are Direct3D 12's, from `RHI_Direct3D12.cpp:2229` and `:2236`. Keeping them identical
is what makes a handle mean the same thing on both backends.

**Index allocation reuses `HandleAllocator<GenericResourceHandle>`**, the platform-neutral
allocator in `Code/Base/Render/HandleAllocator.h` that the Direct3D 12 backend already uses for
its own descriptor heaps. So a handle is allocated the same way on both sides, and nothing new was
written to do it.

**Writing a mutable descriptor uses the actual type, never `VK_DESCRIPTOR_TYPE_MUTABLE_EXT`.**
That constant only ever appears in the layout and the pool size. `WriteResourceHeapSlot` is the
one place a heap slot is written.

#### Buffers

`GetBufferHandle` lays out its descriptors in the same order Direct3D 12 does - constant buffer
first if present, then the read view, then the read-write view - because the handle arithmetic has
to agree.

Vulkan wants buffer usage up front where Direct3D 12 derives it from the view, so the usage flags
come from the requested descriptor types. The awkward part is that a *typed* buffer is a different
descriptor type from a structured one:

| HLSL | `m_format` | Vulkan descriptor | Needs |
|---|---|---|---|
| `ConstantBuffer<T>` | undefined | `UNIFORM_BUFFER` | range |
| `StructuredBuffer<T>`, `ByteAddressBuffer` | undefined | `STORAGE_BUFFER` | range |
| `RWStructuredBuffer<T>` | undefined | `STORAGE_BUFFER` | range |
| `Buffer<T>` | set | `UNIFORM_TEXEL_BUFFER` | a `VkBufferView` |
| `RWBuffer<T>` | set | `STORAGE_TEXEL_BUFFER` | a `VkBufferView` |

**A `VkBufferView` needs a `VkFormat`, which is P5.6's mapping.** Rather than write a second
mapping, `VulkanFormat` is created here with the entries buffers actually need and asserts on
everything else. **P5.6 completes this function; it must not write another one.** That is exactly
the failure the phase document warns about, where two mappings disagree and textures corrupt in a
way that looks like a bug somewhere else.

Which entries were needed was **measured, not guessed**: every `BufferParameters::m_format`
assignment in `Code/Engine` uses one of `R32_UInt`, `RG32_UInt` or `R32_SFloat`, across
`SpatialHash.cpp`, `DeviceRenderView.cpp`, `Renderer_ForwardShading.cpp`, `DeviceAppendBuffer.cpp`,
`DeviceRenderWorld.cpp` and `ImguiRenderer.cpp`. The signed and float siblings of those three are
filled in too, because they cost a line each.

**`BufferParameters::m_pCounterBuffer` has nowhere to go, and needs nowhere.** Direct3D 12 hands a
counter resource to `CreateUnorderedAccessView`; Vulkan has no equivalent. `CreateBuffer` asserts
it is null. The binding model entry reaches the same conclusion from the shader side: no `.esh` or
`.esf` in the repository uses `IncrementCounter`, `DecrementCounter`, `AppendStructuredBuffer` or
`ConsumeStructuredBuffer`, and `AppendBuffer.esh` carries its own explicit `RWBuffer<uint>` counter
and does its own `InterlockedAdd`.

**Suballocation is a `VmaVirtualBlock`**, the direct equivalent of the `D3D12MA` virtual block the
reference uses. `BufferSubAllocation::m_internal` holds the `VmaVirtualAllocation`, with a
`static_assert` on the size, the same guard the Direct3D 12 side has.

**Freed heap slots are not cleared.** `PARTIALLY_BOUND` means a stale descriptor only matters if a
shader reads it, and a shader reading a freed handle is a bug either way. Direct3D 12 frees its
descriptors the same way.

**`VK_SHARING_MODE_EXCLUSIVE`, not `CONCURRENT`.** Direct3D 12 buffers have no queue ownership at
all, and `CONCURRENT` would reproduce that at a cost on some hardware. P5.9 owns barriers and has
to get the ownership transfers right regardless, so the stricter mode is the one to start from.

#### Verified, in the only sense available

- All eight Linux targets compile and link. `ninja` exits 0, `Checks.py` passes.
- 60 `vk*` symbols now resolve against `libvulkan.so.1`, up from 51.
- The three buffer formats were read out of the engine's own call sites.
- Every Vulkan structure and flag was read out of `/usr/include/vulkan/vulkan_core.h`.

#### Not verified, and this is the group where that matters most

No descriptor set has been created, no descriptor written, no handle handed to a shader. The heap
is the single highest-risk thing in Phase 5 - the phase document calls bringing it up "the highest
risk step" and says not to go past it on an unverified assumption. **Every claim in this entry is
an unverified assumption.** The specific things to check first, with validation layers on:

1. That a mutable descriptor binding of 65472 with `UPDATE_AFTER_BIND` allocates at all.
2. That writing a `UNIFORM_TEXEL_BUFFER` into a mutable slot is accepted.
3. That the heap index a shader sees equals the handle `GetBufferHandle` returned.

**Upstream files edited: none.**

### 2026-08-28 - P5.4 Command pools and buffers. Written, never run

**All seven functions are implemented.** 80 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 87.
Nothing has run.

The group is nearly a straight translation, so this entry is short. Four things are worth keeping.

**The pool takes `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`.** Direct3D 12 resets an
individual command list against its allocator, in `ID3D12GraphicsCommandList::Reset`, and the
engine calls `BeginCommandBuffer` per buffer rather than resetting the pool each time. Without the
bit, a second `vkBeginCommandBuffer` with no intervening `vkResetCommandPool` is invalid. Some
drivers give such a pool per-buffer allocators, which costs a little; correctness first, and the
flag is the direct equivalent of what the reference does.

**`ResetCommandPool` passes no flags**, so `VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT` is off.
`ID3D12CommandAllocator::Reset` keeps its memory for reuse, and this runs once per frame per pool,
so handing memory back to the driver every frame is the opposite of what the caller wants.

**`BeginCommandBuffer` does not set `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT`.** It would be
faster, and it is an assumption rather than a fact: Direct3D 12 allows a closed command list to be
submitted more than once without re-recording, and nothing here proves the engine never does. The
flag is a validation error the moment it is wrong. Set it once someone has checked, not before.

**`BeginCommandBuffer` does not bind any descriptor set**, and Direct3D 12's does the equivalent.
`RHI_Direct3D12.cpp:2917` calls `SetDescriptorHeaps` once per command buffer. The Phase 4 binding
model puts the matching `vkCmdBindDescriptorSets` in `CmdSetPipeline` instead, because set 1 is
disturbed whenever a pipeline with a different set 0 layout is bound. P5.8 writes it there. The
comment at the empty spot says so, so that the absence reads as a decision rather than an
oversight.

Two smaller notes. Vulkan command buffers start in the initial state, so there is nothing matching
Direct3D 12's trick of creating a list already recording and closing it immediately; the stage is
just set to `Closed`. And `VulkanCommandBuffer` gains the `Stage` enum its Direct3D 12 sibling
has: the validation layers track the same lifecycle, but an `EE_ASSERT` names the caller that got
it wrong instead of a layer message three frames later.

**Verified, in the only sense available:** all eight Linux targets compile and link, `ninja` exits
0, and 51 `vk*` symbols now resolve against `libvulkan.so.1`, up from 44.

**Not verified:** that a pool is ever created or a command buffer ever recorded.

**Upstream files edited: none.**

### 2026-08-28 - P5.2 Queues and synchronization. Written, never run

**Eight of the nine P5.2 functions are implemented. `QueuePresent` is not, and cannot be.** 87
`EE_UNIMPLEMENTED_FUNCTION` remain, down from 95. Nothing has run.

**The monotonic counter is a timeline semaphore, and the mapping is nearly free.** One
`VK_SEMAPHORE_TYPE_TIMELINE` semaphore per queue. `QueueGetCompletedSemaphore` is
`vkGetSemaphoreCounterValue`, `QueueHostWait` is `vkWaitSemaphores`, `WaitQueueIdle` is
`vkQueueWaitIdle`. The phase document calls this the one thing to get right first; it is right
because `RHI.h` was already written against a counter rather than a fence object.

**The counter starts at 1 and means "the next value to be signalled".** `QueueGetCurrentSemaphore`
therefore returns a value that has *not* happened yet. That looks wrong and is not: it is exactly
what `Direct3D12Queue::m_fenceValue` does, initialised to 1 at `RHI_Direct3D12.cpp:2554`, and the
engine is written against that meaning. The timeline's initial value is 0, which is why both
backends skip a wait on 0.

**`QueueDeviceWait` is the one real mismatch.** `ID3D12CommandQueue::Wait` is a standalone queue
operation that blocks everything submitted after it. Vulkan has no such thing: a wait only exists
attached to a submit. So the wait is recorded on the queue and the next `QueueSubmit` drains it.

The obvious alternative is wrong and worth writing down. Submitting an empty `vkQueueSubmit2`
carrying only the wait does **not** reproduce the semantics, because submissions on one queue may
overlap: a wait in submit N does not hold back submit N+1. The pending-wait list does.

A wait with no submit after it is dropped. That is a behaviour difference from Direct3D 12 with no
observable consequence, since a queue wait that nothing follows cannot be observed.

**P5.1 had to be amended, and the reason is a deadlock.** `CreateContext` previously asked for one
`VkQueue` per unique family. On a device with no dedicated async compute or transfer family, all
three RHI queues then share one `VkQueue`, and a `QueueDeviceWait` between two of them waits for a
timeline value that only a later submit on that same `VkQueue` can signal. That is a hang, not a
slowdown. `CreateContext` now asks for as many queues per family as the engine will take, clamped
to `VkQueueFamilyProperties::queueCount`, and `CreateQueue` hands out distinct queue indices.
Where the family really does expose one queue, the queues still share it, which is correct but
serialised.

**`QueuePresent` belongs to P5.3, not to P5.2.** `VkPresentInfoKHR` accepts binary semaphores
only; there is no timeline path. Presenting needs the swapchain to carry a binary semaphore per
image, the submit before the present to signal it alongside the timeline value, and the present
to wait on it. None of that can be written before `VulkanSwapchain` exists. The stub says so, at
the stub.

**Two things `QueueParameters` asks for that Vulkan will not give:**

| Parameter | Status |
|---|---|
| `QueuePriority` | **Not honoured.** Vulkan fixes queue priorities at `vkCreateDevice`, and `CreateQueue` runs long after. Honouring it would mean recreating the device. Nothing in the engine sets it: `RenderSystem::Initialize` leaves all three queues on `Normal`. `GlobalRealtime` would additionally need `VK_EXT_global_priority`, also at device creation. |
| `QueueFlags::DisableTimeout` | **No equivalent.** `D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT` has no Vulkan counterpart. Nothing sets it either. |

Both are recorded rather than silently ignored, so that the first caller to set one finds an
explanation instead of a mystery.

**Two structures were written here that other groups own.**

- `VulkanCommandBuffer`, with the single `VkCommandBuffer` member `QueueSubmit` reads. P5.4 owns
  command buffers and extends it. The type has to exist for `QueueSubmit` to compile at all.
- `SetVulkanObjectName`, the one `vkSetDebugUtilsObjectNameEXT` call underneath all nine
  `SetDebugName` overloads. P5.12 owns those and builds them on this. It is written now because
  `CreateQueue` names its queue, and because the phase document says to do debug utils early: a
  named object makes every later group easier to debug.

`Queue::m_unifiedMemory` comes from a new `VulkanContext::m_isUnifiedMemory`, computed in
`FillDeviceCapabilities` as "no memory type is device-local without also being host-visible".
Direct3D 12 reads the same flag from D3D12MA's `IsUMA()`.

**Verified, in the only sense available:**

- All eight Linux targets compile and link. `ninja` exits 0.
- 44 `vk*` symbols are now undefined in `libEsoterica.Base.so` and resolve against
  `libvulkan.so.1`, up from 37.
- `Checks.py` passes.
- Every Vulkan structure, enum and entry point was read out of
  `/usr/include/vulkan/vulkan_core.h` before use.

**Not verified:** that a queue is ever created, that a timeline semaphore ever signals, or that
the pending-wait scheme behaves as reasoned. The `QueueDeviceWait` argument above is the piece
most worth re-checking with validation layers on, because it is reasoning about Vulkan's execution
model rather than a mechanical translation.

**Upstream files edited: none.**

### 2026-08-28 - P5.1 Device, context and memory. Written, never run

**P5.1 is implemented and has never executed a single instruction.** `CreateContext`,
`DestroyContext`, `GetTotalAllocatedDeviceMemory`, `GetDetailedMemoryStatistics`,
`GetResourceAllocationStatistics` and `ReportDeviceMemoryLeaks` are real, and so are
`BeginFrameCapture` and `EndFrameCapture`, which are P5.12's but are four lines each once
`CreateContext` has the RenderDoc API pointer. 95 `EE_UNIMPLEMENTED_FUNCTION` remain, down from
103.

Read every claim below as "the compiler and linker agree", never as "this works".

**What `CreateContext` does.** Instance with `VK_LAYER_KHRONOS_validation` and
`VK_EXT_debug_utils` when validation is asked for; physical device selection; device with the
binding model's features; VMA. Device selection honours `DeviceSelectionPreference`:
`UseProvidedIndex` takes the index and refuses it if it does not qualify, and the other two score
discrete against integrated in opposite directions, which mirrors what `DXGI_GPU_PREFERENCE` asks
the factory for rather than inventing a different notion of "best".

**The device is refused rather than worked around.** `GetDeviceRejectionReason` checks Vulkan 1.3,
the three required extensions, and 20 feature bits, and returns the **name of the first missing
one** so the log says what is wrong instead of "no suitable device". The list is the binding
model's, not a guess: `mutableDescriptorType`, `VK_KHR_push_descriptor`, `scalarBlockLayout`, the
descriptor indexing bits, the four `*UpdateAfterBind` bits and the four `*ArrayNonUniformIndexing`
bits, plus `timelineSemaphore`, `bufferDeviceAddress`, `drawIndirectCount`, `dynamicRendering` and
`synchronization2`. There is no fallback path, because the shaders are compiled for this model.

`VK_KHR_swapchain` is enabled here even though P5.3 owns the swapchain. A device is created once,
and adding the extension later would mean recreating it.

**Two symbols moved here that had no home on Linux.** `RHI_Direct3D12.cpp` defines both, and
exactly one backend compiles per platform:

- `Memory::Allocators::g_RHI`, named by seven `TVector` and `THashMap` members in `RHI.h`.
- `GenericResource::~GenericResource`, which `Context` derives from.

Neither was referenced before, because no Linux code instantiated the types that need them. The
link error for the second one is worth remembering: `undefined reference: vtable for
EE::Render::RHI::GenericResource`, which names the base class and not the derived one that caused
it.

**A validation error halts.** The debug messenger logs through `EE_LOG_FATAL_ERROR`, which
carries `EE_HALT()`. Phase 5 says to treat any validation error as a build break; this is what
makes that true rather than aspirational. The callback initialises the thread heap first, because
the layers call it from whichever thread tripped the check, exactly as the Direct3D 12 info queue
callback does.

**RenderDoc uses `RTLD_NOLOAD`.** `dlopen( "librenderdoc.so", RTLD_NOW | RTLD_NOLOAD )` attaches
to a RenderDoc that already injected itself and never loads one that is not there, which is what
`GetModuleHandleA` does on Windows. It still takes a reference, so `DestroyContext` calls
`dlclose`. The Vulkan device pointer is `RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE`, the dispatch
table at the start of the instance, not the instance handle.

**`ReportDeviceMemoryLeaks` had to be inverted.** Direct3D 12 asks DXGI for live objects, and
Vulkan has no global registry. It is also called from `BaseModule::ShutdownModule`, *after* the
context is destroyed, so the allocator that knows the answer is already gone. `DestroyContext`
therefore runs `vmaCalculateStatistics` while the allocator still exists and records any surviving
allocations in two file statics, which `ReportDeviceMemoryLeaks` then reports. Leaked Vulkan
*handles*, as opposed to memory, are the validation layers' job.

**What is deliberately not filled in, and why.**

| Left alone | Reason |
|---|---|
| `m_canShaderReadFrom`, `m_canShaderWriteTo`, `m_canRenderTargetWriteTo` | All three need the complete `DataFormat` to `VkFormat` mapping, which is P5.6's largest piece. Two partial mappings that disagree is the failure the phase document says corrupts textures in a way that looks like a bug somewhere else. |
| `m_shadingRate`, `m_shadingRateCaps` | `NotSupported`, matching the Direct3D 12 backend, which sets the same with a TODO. P5.15 owns them, and reporting a capability the backend cannot honour makes the engine issue calls that halt. |
| `m_indirectRootConstant` | `false`. Direct3D 12 command signatures set root constants per draw and Vulkan's indirect draws cannot. P5.13 decides whether a compute pre-pass covers it. |
| `m_breadcrumbs` | `false`. DRED's equivalents are `VK_AMD_buffer_marker` and `VK_NV_device_diagnostic_checkpoints`, neither wired up. |
| `m_rasterizerOrderViews` | `false`. `VK_EXT_fragment_shader_interlock` is the equivalent and nothing enables it. |
| `m_hdr` | `false`. It needs a swapchain colour space, which is P5.3. |
| `m_numRaytracingCores` | `0`. No Vulkan query exposes it, and nothing in the engine reads it. |

`m_optimalRootSignatureSizeInDWORDs` is copied from Direct3D 12 as 13 rather than derived. It is
an AMD packet-size heuristic with no Vulkan meaning, and it feeds the engine's own root signature
sizing, which has to agree across both backends.

**Queue families are chosen here, which looks like P5.2's work.** `vkCreateDevice` takes the queue
create infos, so it cannot be deferred. A device with no dedicated async compute or transfer
family falls back to the graphics family, which is correct rather than degraded: every
graphics-capable family also supports compute and transfer.

**`DeviceMode` is always `Single`.** Vulkan has no equivalent of a Direct3D 12 linked-node
adapter; multi-GPU is explicit device groups, which the engine never asks for. `m_numLinkedNodes`
is 1.

**Verified, in the only sense available:**

- All eight Linux targets compile and link: the five `.so` files, `Reflector`,
  `ResourceCompiler`, `Tester`. `ninja` exits 0 and a re-run reports "no work to do".
- `nm -DC libEsoterica.Base.so` shows `EE::Render::RHI::CreateContext` as a defined exported
  symbol and `EE::Memory::Allocators::g_RHI` in BSS. 37 `vk*` symbols are undefined and resolve
  against `libvulkan.so.1`.
- `Checks.py` passes.
- Every Vulkan extension macro, feature bit and struct name was read out of
  `/usr/include/vulkan/vulkan_core.h` before use, not recalled.

**Not verified, and not verifiable here:** that any of it produces a working device. No
instance has been created, no device enumerated, no capability read. The two GPUs in this machine
were surveyed during Phase 4 and both expose everything the binding model needs, so the
requirement list is not speculative, but the code that reads it has never executed.

**One `#pragma clang diagnostic` is used**, around the `vk_mem_alloc.h` include, to silence about
200 `-Wnullability-completeness` warnings. It is a new Linux-only file, so no upstream file is
touched, and the suppression is scoped to that include.

**Upstream files edited: none.** `RHI_Vulkan.cpp` is a file this fork added; it is listed in
`LinuxSources.txt` and does not appear in [TouchedFiles.md](TouchedFiles.md), which is correct.

### 2026-08-28 - P5.0 The Vulkan dependencies, and open question 3

Phase 5's dependency plumbing. **No RHI function is implemented.** `RHI_Vulkan.cpp` is untouched,
so all 16 groups are still Phase 1 stubs.

Three dependencies arrive, and one long-standing generator problem goes away:

| Dependency | Pin | Layout | Built? |
|---|---|---|---|
| VMA | `v3.4.0` | `External/VMA/include/vk_mem_alloc.h` | no, header only |
| SPIRV-Reflect | `vulkan-sdk-1.4.357.0` | `External/SPIRV-Reflect/`, plus `lib/libspirv-reflect.a` | one C file, `cc` and `ar` |
| RenderDoc header | `v1.45` | `External/RenderDoc/renderdoc_app.h` | no, one header |

**The Vulkan loader and headers are not fetched.** `libvulkan-dev` supplies both. The loader
carries an ICD layer that has to match the installed drivers, so a source build of it would fight
the distribution rather than help. `pkg-config vulkan` gives the include path and `-lvulkan`, the
same way `FreeType.props` already works.

**`External/RenderDoc` did not exist, and NinjaGen had reported that as a problem since Phase 0.**
`RenderDoc.props` maps to that include directory, `Esoterica.Base` imports the sheet, and nothing
had ever put the header there. `fetch_renderdoc` fixes it, so the generator now reports 17
problems instead of 18, and every one that remains is "no sources in <configuration>".

**Open question 3 is answered: the plain Vulkan loader, not `volk`.** The phase document's
default stands, because the question asks for a decision from measured dispatch overhead and
there is nothing to measure until the backend renders. Switching is one line in
`Toolchain.SHEETS` plus an include, so nothing is lost by waiting for a profile that argues for
it.

**Linux-only property sheets are a new concept in the generator, and they had to be.** The three
sheets have no `Code/PropertySheets/*.props` sibling and never will: the Windows build has no use
for Vulkan, VMA or SPIRV-Reflect. Every other sheet reaches a project through
`UpstreamProjects.txt`, which `SyncUpstream.py` regenerates from the `.vcxproj` files, so a
hand-written entry there would not survive the next sync - and `Checks.py` verifies that file
against upstream, so it would fail loudly first. `Toolchain.LINUX_ONLY_SHEETS` attaches a sheet
to a project **by name** instead, and `project_compile_flags` and `project_link_flags` fold it in
alongside the imported sheets. Only `Esoterica.Base` has an entry; every other project reaches
the RHI through it.

**One check was added**, and it is the kind `Checks.py` is for. A typo in either name of a
`LINUX_ONLY_SHEETS` entry silently contributes nothing. The build stays green until something
needs the symbols, and the link error then names `vkCreateInstance` rather than the misspelling.
The two new checks confirm that every key names a real project and every value is in `SHEETS`.

**Verified:**

- All eight Linux targets build and link: the five `.so` files, `Reflector`, `ResourceCompiler`
  and `Tester`. `ninja` exits 0.
- `ldd libEsoterica.Base.so` lists `libvulkan.so.1`. It is a real `DT_NEEDED`, not just a flag in
  the ninja file.
- `-lspirv-reflect` resolves at link time. No symbol is referenced yet, so the archive
  contributes nothing, but a missing archive would have failed the link.
- `vk_mem_alloc.h` with `VMA_IMPLEMENTATION`, `spirv_reflect.h` and `renderdoc_app.h` compile
  together in one translation unit under the project's own language flags: `-std=c++20
  -fno-exceptions -msse4.2 -mavx -mavx2`. That is the combination P5.1 will write, and it was
  checked rather than assumed.
- `Checks.py` passes, including the two new checks and the existing determinism and upstream-sync
  ones.
- Re-running the three fetchers reports "already present" and rebuilds nothing.
- `Editor` and `ResourceServer` still fail to compile, unchanged and for their existing Phase 7
  reasons: `shellapi.h`, `EE::Platform::Win32`, and an incomplete `EditorTool`.

**One thing for P5.1 to expect.** `vk_mem_alloc.h` emits about 200
`-Wnullability-completeness` warnings under `-Wall -Wextra`. They are noise, not errors - the
Linux build passes no `-Werror` - but the translation unit that defines `VMA_IMPLEMENTATION`
will want `-Wno-nullability-completeness` to keep the build log readable. That is a decision for
the task that writes the file, not for this one.

**Upstream files edited: `Code/Scripts/NinjaGen/NinjaGen.py` only**, which
[TouchedFiles.md](TouchedFiles.md) already registers as a large rewrite and a special case. Two
functions gain one line each. No new registry entry is needed, and no `.vcxproj` or `.slnx` file
is touched.

### 2026-08-28 - Criterion 3 is met: the shaders were valid, the validator was stale

**All 46 shader stages pass `spirv-val --target-env vulkan1.3`.** Acceptance criterion 3 is met
in full. Nothing about the shaders changed to achieve this.

**The `CullPrimitiveEXT` failure was never a defect in this project.** It was recorded twice, in
the P4.1 entry and again after the defect 2 fix, as pre-existing and unrelated, blocking
criterion 3 until "somebody works out which one is right". The answer is that the validator was
wrong:

| Validator | Version | Result on all 46 stages |
|---|---|---|
| `/usr/bin/spirv-val`, from Ubuntu 24.04 | SPIRV-Tools **v2025.1** | 6 mesh stages rejected |
| built from the SPIRV-Tools DXC vendors | SPIRV-Tools **v2026.3** (`1c336172`) | **0 rejected** |

The rejected variable is `%12 = OpVariable %_ptr_Output__arr_bool_uint_64 Output`, an array of 64
bools, and the v2025.1 diagnostic is "needs to be a boolean value array. ID <12> (OpVariable) is
not a bool scalar" - it asks for the thing it is looking at. The mesh built-in validation was
rewritten upstream since: the vendored `validate_builtins.cpp` has
`ValidateMeshBuiltinInterfaceRules`, `cull_primitive_entry_points_` and VUID 10591, none of which
exist in v2025.1. DXC's internal validation passed all along, and it passed because DXC validates
with the version it vendors.

**The lesson worth keeping: a distribution's `spirv-val` is not interchangeable with the one the
compiler validates against.** Two different judgements of the same module, and the older tool was
the one being believed.

**`spirv-val` is now installed next to `dxc`,** at
`External/DirectXShaderCompiler/bin/x64/spirv-val`, built by `DownloadDependencies.sh` from
`external/SPIRV-Tools` in the DXC clone. It costs 28 seconds against DXC's twenty minutes and
needs no extra download, because the source is already there. A criterion that can only be
checked by hand-building a tool is a criterion nobody re-checks.

`fetch_dxc`'s `git clean` now excludes `build-spirv-tools` as well as `build`, so a re-run does
not rebuild it. `git clean -e` takes a path, so it needed naming separately.

**Verified:** every one of the 46 stages compiled and validated under both validators in the same
sweep, which is how the 6-against-0 split above was measured rather than inferred.

**Use the installed one from now on.** `External/DirectXShaderCompiler/bin/x64/spirv-val`, not
whatever is on `PATH`.

### 2026-08-28 - P4.5 `CompileShaders.sh`, and `-hotreload` is not what the plan assumed

`CompileShaders.sh` exists and runs the Reflector's shader pass. Four modes, all exercised:

| Invocation | Reflector arguments | Result |
|---|---|---|
| `./CompileShaders.sh` | `-shaders` | exit 0, 34 generated files |
| `./CompileShaders.sh --hotreload` | `-shaders -hotreload` | exit 0 when the shader parameters have not changed |
| `./CompileShaders.sh --rebuild` | `-shaders -rebuild` | exit 0, reproduces the same 34 files |
| `./CompileShaders.sh --clean` | `-shaders -clean` | exit 0, removes them |

**`-hotreload` does not notify a running process.** The phase document guessed that it might, and
said to find out before including it. It does not. It is an in-process shortcut: a plain
`-shaders` run promotes itself to a full type reflection pass when the generated
`*.parameters.h` files change, and `-hotreload` refuses that promotion, exiting 1 with "Shader
Hot-Reload will result in changes to the engine runtime types requiring type reflection. Aborting
Hot-Reload!". See `Reflector.cpp:746` and `:1084`.

**Decision: `--hotreload` is not the default, though `CompileShaders.bat` passes `-hotreload`.**
This is the only place the two scripts differ. A first run always rewrites the parameters files,
so a literal mirror of the `.bat` gives a script that exits 1 on a fresh checkout. Both
behaviours were measured, not assumed: from a clean tree `-shaders -hotreload` exits 1, and on an
up-to-date tree it exits 0. The default is the run that always works, and the fast path is one
flag away. The script says why, at the top, so the divergence is not a mystery to whoever diffs
the two.

`--clean` and `--rebuild` pass straight through. `-shaders` scopes them to the shader output and
`CleanOutputs` cleans the right directories itself, so unlike `RunReflection.sh` this script
deletes nothing of its own. `RunReflection.sh` does its own deletion because `Reflect.nmake`
does; there is no nmake shader target to mirror here.

**Verified:** all four modes exit as expected; two consecutive default runs produce
**byte-identical** output; `--rebuild` from clean reproduces the incremental output byte for
byte; the documented `--hotreload` failure reproduces from a clean tree; and the engine still
compiles and links afterwards.

**A correction to the phase document's counts.** Criteria 2 and 3 are written in terms of "26
`.esh` files", one of them in `Base`. That conflates two file types:

- **`.esh` are headers**, 26 of them, and the Reflector never compiles one on its own.
  `Code/Base/Render/RHI.esh` is include-only, which is why `Base` reports "no shaders found" and
  its `_AutoGenerated/Shaders` directory stays empty. **No output is generated for `Base`, and
  none should be.**
- **`.esf` are the shaders**, 28 of them, all in `Esoterica.Engine.Runtime`, and they produce the
  46 stages that P4.3's defect 2 entry counts.

So criterion 2's "fills `_Module/_Autogenerated/Shaders/` for `Base` (`RHI.esh`) and `Engine`
(25 `.esh` files)" is met in substance and wrong in its numbers: 28 shaders, `Engine` only.

**Left in Phase 4:** P4.6, the structural diff against the Windows-generated output, which needs
a Windows machine. Criterion 9, clip-space Y. The constant buffer layout rule. Criterion 3's
`spirv-val` half, which is blocked on the pre-existing `CullPrimitiveEXT` failure.

### 2026-08-28 - P4.3 Defect 2 is fixed, and every shader compiles

**All 46 shader stages compile to SPIR-V, up from 41.** The Reflector's shader pass exits 0 and
fills `_AutoGenerated/Shaders/` with 34 files, the generated C++ compiles and links into
`libEsoterica.Engine.Runtime.so`, and the five `.material` resources Phase 3 could not compile
now compile. The fix is
`Code/Scripts/DXCPatches/0002-spirv-counter-for-a-heap-sourced-structured-buffer.patch`.

**What defect 2 actually was.** Assigning a counter-bearing resource out of a descriptor heap
failed, while initialising one from the same expression worked. The previous entry concluded the
destination was the hard case and that a correct fix meant implementing counters inside
structures in DXC, "a real project, not a patch". That was wrong. The destination already has a
counter; only the source failed to resolve, and it failed for **three** reasons, not two. All
three had to be fixed together, which is why the source-side half was written, looked correct,
and still did not compile a single shader:

1. `getFinalACSBufferCounter` tests AssocCounter#1 first, and `heap[i]` is a
   `CXXOperatorCallExpr`, so `getReferencedDef` resolves it to the subscript operator and the
   function returns that operator's empty pair before reaching the descriptor heap branch.
2. That branch tested `isResourceDescriptorHeap` against the expression's own type.
   `isDescriptorHeap` is the predicate that matches, as the previous entry worked out.
3. **The one that was missed.** The counter is resolved *before* the right-hand side is emitted.
   A descriptor heap access creates the heap's SPIR-V variable as a side effect of being
   emitted, and that is what puts the heap into `declRWSBuffers`, which is what lets a counter
   be created for it. Resolve first and there is nothing to find. `doBinaryOperator` now
   evaluates the rhs before calling `tryToAssignCounterVar`; its own comment already said "we
   need to evaluate rhs before lhs", and the counter call sat above the evaluation.

A fourth point was needed to avoid over-reporting, and it is not obvious: **one heap declaration
is shared by every resource type read out of it.** Registering the `RWStructuredBuffer` variant
puts that declaration into `declRWSBuffers`, so a later read of a `StructuredBuffer` or an
`RWBuffer` from the same heap reports the RW variant's counter and is then rejected against a
destination that rightly has none. `getDescriptorHeapResourceType` reads the resource type from
the implicit cast above the subscript, where `doCXXOperatorCallExpr` already reads it from, and
the heap branch returns no counter unless that type is counter-bearing. Without this, five
shaders compiled and two others that used to compile broke.

**Verified:**

- **The 41 stages that compiled before produce byte-identical SPIR-V.** Compared by hash against
  the previously installed compiler, over every `.esf` and every entry point. 41 identical, 0
  changed, 5 newly compiling.
- `spirv-val --target-env vulkan1.3` passes on the new shaders, except the two mesh shaders,
  which fail on `VUID-CullPrimitiveEXT-CullPrimitiveEXT-07036`. **That is the pre-existing and
  unrelated failure** already recorded for the material mesh shaders in the P4.1 entry.
- No shader emits a `counter_var`, so the counter heap reserved at set 1 binding 2 by the
  binding model stays unused, as that entry predicted.
- **DXC's own test suite: 1343 of 1357 CodeGenSPIRV tests pass, against 1345 before.** The two
  are `spirv.legal.sbuffer.struct.hlsl` and `spirv.legal.sbuffer.usage.hlsl`, and both fail only
  on `CHECK-NEXT` lines that assume the counter store is emitted before the right-hand side
  rather than after. The opcode histogram of each is **identical** to the unpatched compiler's,
  so this is a reordering and not a change of output. The tests are deliberately not edited, so
  that the fork's diff against the pinned tag stays confined to the fix. Upstream would want
  them updated in a submission.
- The patch applies to a pristine clone of the pinned tag after 0001, which is the path
  `DownloadDependencies.sh` takes. Checked by `git reset --hard`, `git clean`, applying both in
  name order, and rebuilding.
- The rebuilt `libdxcompiler.so` was installed and the Reflector relinked against it before the
  shader pass was run.

**Two things this unblocks, both checked:**

- **Acceptance criterion 5.** The generated `Shaders/*.cpp` compile and link. This needed
  `NinjaGen.py` to be re-run, because the ninja file was generated when the directory was empty.
  The case-insensitive match in `SourceLists.py` picks the files up correctly.
- **Phase 3's five `.material` resources.** All five compile, with
  `-compile data://... -force`. They were blocked on `DefaultPBRParameters`, which only the
  shader pass generates.

**How the test suite was run,** since the build has no test target. The DXC build here is
configured with `HLSL_INCLUDE_TESTS=OFF`, so there is no `FileCheck` and no `check-clang-spirv`.
The runner in the scratch directory parses each test's `// RUN:` line, substitutes `%dxc` and
`%s`, and pipes to the system `FileCheck-19`. 12 of the 1357 fail before any change of ours,
mostly cooperative-matrix and node tests; those are the baseline, not a regression.

**Still open in Phase 4:** P4.5, `CompileShaders.sh`, which does not exist yet. P4.6, the
structural diff of the generated output against Windows. Criterion 9, clip-space Y. The constant
buffer layout rule. Criterion 3 is met for compilation but not for `spirv-val`, because of the
pre-existing `CullPrimitiveEXT` failure on mesh shaders.

### 2026-08-28 - P4.1 DXC is built from source and patched, and the material shaders compile

DXC's SPIR-V back end has two defects that stop this engine's shaders. Neither is about bindless,
which is what Phase 3 assumed. Both were reduced to minimal shaders of under ten lines, and both
were confirmed against the official prebuilt DXC so they are not artifacts of building it here.

**Defect 1, fixed.** A mesh shader output struct with a **struct member** segfaults the compiler,
in `SpirvEmitter::assignToMSOutAttribute` by way of `SpirvBuilder::createAccessChain`. This is
[DXC issue 8475](https://github.com/microsoft/DirectXShaderCompiler/issues/8475), open since
2026-05 with no fix and no comments; the stack there matches ours exactly. It stops five shaders,
because `PrimitiveOutput` in `RendererTypes.esh` holds a `PrimitiveOutputFlags m_primitiveFlags`
member. The same shaders compile on DXIL, so this is Linux-only and Windows is unaffected.

The fix is `Code/Scripts/DXCPatches/0001-spirv-nested-struct-in-mesh-shader-output.patch`, which
carries the full explanation. In short, `createStructOutputVar` flattens such an output into one
stage variable per **leaf** field, and the emitter never mirrored that. Three things were needed,
and the first two are the ones that are easy to get wrong:

- Recurse on the **type alone**, not on whether the member has a semantic. A struct member with
  its own semantic is flattened too. Testing the semantic looks right, compiles, fixes the toy
  case, and still leaves the engine's shaders crashing. It cost a build to find out.
- Extract by the **lowered** field index, not the AST one. Bitfields merge into one member when a
  struct is lowered, and `PrimitiveOutputFlags` is all bitfields, so the AST index runs off the
  end and the validator rejects the module with "Index is out of bounds". The bitfield's own bits
  then have to come back out with `createBitFieldExtract`.
- `collectArrayStructIndices` stopped walking at the first member, so `prims[i].f.a = x` pushed
  an index for a struct that has no SPIR-V counterpart.

**Defect 2, not fixed.** Assigning a **counter-bearing** resource from `ResourceDescriptorHeap`
fails with "cannot handle associated counter variable assignment". It stops the other five
shaders. See the decision entry below for why it stopped here.

**Verified:**

- **The Reflector's shader pass runs end to end with no crashes, and 23 of the 28 shaders it
  parses compile to SPIR-V.** Before this, DXC segfaulted inside `libdxcompiler.so`. Every
  remaining failure is now a clean diagnostic, and all five are defect 2.
- All four material mesh shaders (`DefaultPBR`, `DefaultColorOnlyPBR`, `ComplexSurfacePBR`,
  `Placeholder`) compile to SPIR-V. `DebugDrawMesh` moved off the segfault onto defect 2.
- `libdxcompiler.so` builds and `Esoterica.Applications.Reflector` links against it, which is
  acceptance criterion 1. Confirmed with `ldd`.
- Seven reduced shaders covering every shape of the bug pass, and the flat-struct controls that
  worked before still work.
- The patch applies cleanly to the pristine tag, and a build from pristine-plus-patch reproduces
  the result. That is the path `DownloadDependencies.sh` takes, not just my working tree.
- **No regression:** for a shader the patch does not affect, the patched compiler produces
  **byte-identical** SPIR-V to the official prebuilt one.

**`spirv-val` note. Resolved, see the P4.6 entry above: the shaders were valid all along and the
system `spirv-val` was wrong.** What follows is what was known at the time.

The compiled material shaders fail `spirv-val --target-env vulkan1.3` on
`VUID-CullPrimitiveEXT-CullPrimitiveEXT-07036`. **This is pre-existing and unrelated.** The
unpatched official DXC produces the identical error for a flat output struct with
`SV_CullPrimitive`, which `PrimitiveOutput` has at `RendererTypes.esh:473`. DXC's own internal
validation passes, so it is a disagreement between DXC and the system `spirv-val`. Acceptance
criterion 3 depends on it and cannot be met until somebody works out which one is right.

**Files added:** `Code/Scripts/DXCPatches/0001-*.patch`.
**Files edited:** `DownloadDependencies.sh` (`fetch_dxc`, `requirements_dxc`). No upstream
`Code/` file was touched, so [TouchedFiles.md](TouchedFiles.md) is unchanged. No `.esh` was
edited, as Phase 4 requires.

### 2026-08-28 - P3.1-P3.7 Resource compiler builds, links and compiles data

- All four libraries link, `EsotericaResourceCompiler` links, and it compiles 22 of the 27 real
  resources under `Data/`: 15 textures, 4 meshes, 1 physics mesh, 1 physics material database,
  1 map. No errors, no crashes.
- `ctt` answered, and answered well. See the decision entry below.
- The file system watcher passes a 15-check scratch test at
  `$SCRATCH/watchertest/main.cpp`, not committed: create, modify, rename and delete in the
  watched root; the same four in a subdirectory created **after** watching started; a directory
  two levels below that; directory deletion; and `IN_Q_OVERFLOW` firing
  `OnMassiveChangeDetected`.

**Files added:** `Code/EngineTools/FileSystem/FileSystemWatcher_Linux.cpp`,
`Code/EngineTools/Core/SystemDialogs_Linux.cpp`, `Code/Base/Render/ComPtr_Linux.h`, and the
`fetch_ctt` function in `DownloadDependencies.sh`.

**Upstream files edited:** see [TouchedFiles.md](TouchedFiles.md). The Phase 3 additions this
session are 11 `inline` removals, 5 `va_copy` fixes, one CTAD fix in `PageAllocator.h`, one
`<time.h>` in vendored `delabella`, and 3-line include switches in four EngineTools UI files.

**Acceptance criteria:**

| # | Criterion | Result |
|---|---|---|
| 1 | Four libraries build | **met** |
| 2 | `EsotericaResourceCompiler` builds and links | **met**, Debug and Release. Release found two more `inline` declarations that Debug did not: `NodeGraph_FlowGraph.h` `GetInputPin` and `GetOutputPin`. Build Release too, or that class of bug hides. |
| 3 | Compiles the full `Data/` tree without errors | **partly.** 22 of 27 resources. The 5 `.material` files need shader-generated types. The other 13 files in `Data/` are `EE_DATA_FILE`, not resources: `.txtg`, `.meshgrp` and `.pml` have no compiler on any platform, and "failed to find a compiler" is the correct answer. |
| 4 | Byte-identical to Windows output | **not checked**, needs a Windows machine. What *was* checked: the Debug and Release compilers produce byte-identical output for all 38 files. That rules out the float-formatting and optimisation-dependent differences the phase doc warns about, and leaves only genuinely platform-dependent ones. |
| 5 | Watcher works, including a subdirectory created after the start | **met** |
| 6 | Reports `OnMassiveChangeDetected` on `IN_Q_OVERFLOW` | **met** |
| 7 | `FileRegistry.cpp` compiles and its watcher handling works | **met** (compiles and links; the watcher itself is tested above) |
| 8 | Open question 1 (`ctt`) answered and recorded | **met** |
| 9 | Windows MSBuild still succeeds | **not run.** Every edit is either `__linux__`-guarded or standard C++ that MSVC already accepts. |
| 10 | Every upstream file edited is in TouchedFiles.md | **met** |
| 11 | `FileSystemWatcher.h` shows 1 line changed | **met** |

## What the next session needs to know

- **`RenderSystem.cpp` is in the build again, on a placeholder.** `NinjaGen.py` writes an empty
  `Code/Engine/_Module/_AutoGenerated/Shaders/ShaderRegistration.{h,cpp}` when the Reflector has
  not produced one, and prints a `problem:` line about it on **every** run. That is deliberate:
  a stale placeholder is exactly the failure that leaves a green build behind. The Reflector
  overwrites it the moment its shader pass works, and the placeholder is never written over real
  output.
- **`Applications/BuildGenerator` is excluded permanently.** It parses `Esoterica.slnx` to write
  the reflection file lists MSBuild consumes; `NinjaGen.py` does that job here. Its only actual
  compile error was `__debugbreak`, so a shim would have made it build - and building a `.sln`
  parser on a platform with no `.sln` is pointless.
- **`Applications/Editor` and `Applications/ResourceServer` do not build.** Editor hits
  "member access into incomplete type `EE::EditorTool`" in `EditorUI.h`, which has the shape of
  the missing-forward-declaration bug that showed up three times in Phase 3. ResourceServer wants
  `shellapi.h`. Both are Phase 7.
- **The two big DXC facts.** Shader reflection works. Shader *compilation* segfaults inside
  `libdxcompiler.so` from `ShaderCompiler::CompileShaderStage`. Everything downstream of that -
  the materials, the renderer, `Shaders::Initialize` - waits on the bindless decision.

### 2026-08-27 - P2.1-P2.5 Reflector builds, links and runs

`Build/Linux_Release/Esoterica.Applications.Reflector` builds and links, and `./RunReflection.sh`
runs it against `Esoterica.slnx`. libclang parses the whole codebase with **no compile errors**.
It does **not** generate code yet: see "Where reflection stops".

Acceptance criteria met: **1, 2, 3, 4, 6, 7, 9**.

| # | Result |
|---|---|
| 1 | `Esoterica.Applications.Reflector` builds and links |
| 2 | `./RunReflection.sh` exits 0 and fills `_Module/_AutoGenerated/TypeInfo/` for all five modules: Base 7, Engine 114, EngineTools 91, Game 24, GameTools 2 `.cpp` |
| 3 | A second run produces byte-identical output, checked with `sha256sum` over all 245 files |
| 4 | `--clean` empties the directories, `--rebuild` regenerates them byte-identically |
| 6 | The build generator picks the new files up: 614 sources before reflection, **848** after |
| 7 | LLVM 21.1.8, pinned and recorded |
| 9 | Every upstream file edited is in TouchedFiles.md |

**Not met: 5 and 8.** Both need a Windows machine. Criterion 5 is the cross-platform output
diff, which is the strongest correctness check available and has **not been done**. Until it is,
"the output is correct" rests on the Reflector exiting 0 and the output being self-consistent,
which is weaker.

**P2.1, LLVM.** Pinned to **21.1.8**, fetched as the official prebuilt
`LLVM-21.1.8-Linux-X64.tar.xz` by `./DownloadDependencies.sh llvm`.

The version is **inferred, not documented**. `LLVM.props` records no version at all: it points at
`External/LLVM`, which the prebuilt Windows `External.zip` fills. The version comes from the
library list it links. `LLVMFrontendDirective` and `LLVMDebugInfoDWARFLowLevel` both first appear
in LLVM 21, checked against the llvm-project tree at `llvmorg-19.1.0`, `20.1.0` and `21.1.0`;
21.1.8 is the last 21.x release. **If the Reflector ever misbehaves in a way that smells like an
AST mismatch, this pin is the first thing to question.**

The official prebuilt release is used rather than a source build: same artifact the LLVM project
ships, pinned, and minutes rather than hours. A distro `libclang-dev` is deliberately not used;
Ubuntu 24.04 offers 18, three major versions adrift.

**The release archives hold LLVM IR bitcode, not ELF objects** - the release is built with LTO.
GNU `ld` rejects them with "file format not recognized". The generator therefore links anything
importing `LLVM.props` with the `ld.lld` that ships in the same tarball. This is the single least
obvious thing in the phase.

**P2.2, platform code.** As small as the plan said: guard two unused Windows includes in
`ReflectorApplication.cpp`, and route one `GetShortPath` call through `Platform::Linux`.

**P2.3, shader reflection.** Excluded in the generator, which is the phase document's preferred
option 1. `Code/Applications/Reflector/ShaderReflection/**` is one line in `Exclusions.txt`. The
`ShaderCompiler.{h,cpp}` bodies are also wrapped in `#ifdef _WIN32`, and `Reflector.cpp` takes an
early return at the top of `ReflectShaders`. `RunReflection.sh` passes `-typeinfo`.

**Phase 4 reverses all four of those**, and nothing is deleted.

**P2.4, the autogenerated directory case.** It is **`_AutoGenerated`**, capital G.
`ReflectorSettings.h:15` sets `g_autogeneratedDirectoryName = "_AutoGenerated"`, and the
Reflector is self-consistent about it. `.gitignore` already matches. Only `Esoterica.props`
disagrees, with `_Autogenerated`, and that is an MSBuild wildcard which matches either way on
Windows; the build generator globs case-insensitively, so no change is needed.

**P2.5, `RunReflection.sh`.** Mirrors `RunReflection.bat` plus the `clean` and `rebuild` targets
of `Reflect.nmake`, including emptying the autogenerated directories itself. It does **not**
build the Reflector first, unlike `Reflect.nmake`: that hides which step failed, and the ninja
command is one line.

**The Reflector handles `.slnx` natively.** `ReflectorApplication.cpp:57` checks
`MatchesExtension( "slnx" )`. There was no solution-format problem to solve.

## What was actually wrong

Every failure in this phase was upstream code that assumes Windows. In rough order of how long
each took to find:

**`IsUnderToolsPath` searched for a hardcoded `"\EngineTools\"`.** On Linux that never matches,
so no header was ever classified as tools-only, so every tools header was parsed in the
no-dev-tools pass, where the `ImGuiX` types it references do not exist. This produced a dozen
confusing errors about `Gizmo`, `FilterWidget` and `TextBuffer` that looked like missing
platform guards and were nothing of the kind. It now builds the search string from
`FileSystem::Path::s_pathDelimiter`.

**Five path constants and an include-path array spelled with backslashes.** The Reflector created
a directory literally named `Build\_Temp\` in the repository root, and found no headers at all.
Two include paths also had the wrong case.

**The builtin type table assumes LLP64.** `ULongLong` maps to `uint64_t` and there is no `ULong`
case, so on LP64 Linux every 64-bit property failed to resolve.

**`-fms-compatibility`** makes clang emulate MSVC closely enough to break glibc's headers.

**libclang needs `-resource-dir`** when it is linked directly rather than driven by a clang
executable.

**Three case-mismatched includes**, and two two-phase-lookup problems that MSVC's delayed lookup
hides.

## Notes for the next session

- **The Reflector parses all project headers as a single translation unit.** One Windows-gated
  type breaks reflection for the whole project. This is the root cause of most of what went
  wrong in this phase.
- **`-fms-extensions` and `-fms-compatibility` are off on Linux.** `-fms-compatibility` makes
  clang behave enough like MSVC to break glibc's headers: `__STRICT_ANSI__ seems to have been
  undefined`, then `char16_t` and `char32_t` undeclared. Nothing needs them here, because
  `_Module/API.h` takes its `visibility( "default" )` branch.
- **libclang needs `-resource-dir`.** It normally finds its builtin headers relative to the
  clang executable, and the Reflector links libclang directly, so there is none. `ClangParser.cpp`
  discovers the one versioned directory under `External/LLVM/lib/clang` rather than writing the
  version down.
- **`FileSystem::Path::GetCorrectCaseForPath` now does real work on Linux.** Phase 1 made it a
  pass-through, reasoning that the correct case for a path is the case it was given. That holds
  for paths the engine produces and fails for paths out of a `.vcxproj`, several of which
  disagree with the disk. It walks the path a component at a time and recovers the real spelling.
- The Windows build has **not** been run against any of this, and **criterion 5, the
  cross-platform output diff, has not been done.** That diff is the real correctness check for
  this phase. Someone with a Windows machine should run the Reflector at this commit and compare
  `_Module/_AutoGenerated` against the Linux output, expecting only line-ending differences.

### 2026-08-27 - P1.1-P1.10 Base platform layer

`Esoterica.Base` compiles and links on Linux. 132 of 132 translation units, in Debug, Release
and Shipping. It went from 1 of 132 at the start of the phase.

New files, all listed in `Code/Scripts/NinjaGen/LinuxSources.txt`:

| File | Task |
|---|---|
| `Platform/Platform_Linux.h` | P1.1 |
| `Math/Platform/Math_Linux.h` | P1.2 |
| `Threading/Platform/Threading_Linux.cpp` | P1.3 |
| `FileSystem/Platform/FileSystem_Linux.cpp`, `FileSystemPath_Linux.cpp` | P1.4 |
| `Platform/PlatformUtils_Linux.{h,cpp}` | P1.5 |
| `Types/Platform/Types_Linux.cpp` | P1.6 |
| `Logging/Platform/SystemLog_Linux.cpp` | P1.7 |
| `Platform/Platform_Linux.cpp` | P1.8 |
| `Render/RHI_Vulkan.cpp` | P1.9, 102 stubs |
| `Imgui/Platform/ImguiPlatform_Linux.cpp`, `Input/InputDevices/Platform/InputDevice_KeyboardMouse_Linux.cpp`, `InputDevice_XBoxController_Linux.cpp` | **not in the plan**, see below |

Acceptance criteria met: **1, 2, 3, 5, 6, 7, 9.**

Not met:

- **4, `Tester` links and runs.** Not achievable in Phase 1. See the correction below.
- **8, the Windows MSBuild build.** Not run; there is no Windows machine on this side. This
  phase edits 12 upstream files, so this is a real gap, not a formality.

**Answers the notes the phase document asked for:**

- **`SyncEvent` is a manual reset event.** `Threading_Win32.cpp` calls
  `CreateEvent( nullptr, TRUE, FALSE, nullptr )`, and the second argument is `bManualReset`. So
  it stays signalled once signalled, releasing every current and future waiter, until `Reset()`
  is called explicitly; waiting does not clear it. The Linux version is a condition variable
  plus an explicit flag, with `notify_all`.
- **`GetFileModifiedTime` returns nanoseconds since the epoch.** Every caller stores the value
  and later compares it for equality, so it never has to agree with the Win32 FILETIME.
  Nanoseconds rather than seconds because seconds let two edits inside the same second look
  identical, which shows up as a resource that silently fails to recompile.
- **Open question 5, does GameNetworkingSockets block Base?** Yes, and at *compile* time, not
  link time. `DownloadDependencies.sh` builds it.
- **Open question 6, does `Memory.cpp` have a working non-Windows path?** No. Three unguarded
  functions called `VirtualAlloc`, `VirtualFree` and `InterlockedAdd64`.

## Plan corrections found during Phase 1

**P1.8 was mis-scoped.** The document calls it the largest task in the phase, 249 lines of stack
walking and crash dumps. Lines 25 to 235 of `Platform_Win32.cpp` are inside an `#if 0`, and
`Initialize` and `Shutdown` have their bodies commented out. None of it runs, so there was no
behaviour to match. The Linux version is a real signal handler anyway, because acceptance
criterion 6 asks for a backtrace, but it is ~90 lines rather than ~250.

**Three Phase 6 stubs were needed in Phase 1.** The plan has the RHI stub for exactly this
reason but missed the same argument for imgui and input: `ImguiPlatform_Win32.cpp`,
`InputDevice_KeyboardMouse_Win32.cpp` and `InputDevice_XboxController_Win32.cpp` are excluded,
and without definitions `Esoterica.Base` does not link, so nothing downstream can be built or
tested. They halt, except the controller dead zone and threshold getters, which return the
XInput constants: those are read during device construction before anything is plugged in, and
halting there would stop the engine starting at all.

**Only five `_Module/API.h` files exist, not six.** `Code/Applications/Tester/_Module/API.h` is
a single `#pragma once` and declares no export macro.

**`Esoterica.Applications.Tester` is not an empty console app.** The plan describes it as one,
and says its only job is to prove the `.so` loads. Its `Main.cpp` includes
`EngineTools/_Module/_AutoGenerated/TypeInfo/TypeRegistration.h`, a Reflector output, plus a
dozen `EngineTools` headers. It cannot link until Phase 2 and Phase 3. Criterion 5's scratch
program was used instead, which is what proved Base works.

**Optick cannot be dropped.** `Profiling.h` includes `<optick.h>` unconditionally, and
Conventions rule 4 forbids stripping that include, so the header has to exist. It also only
zeroes `USE_OPTICK` when `EE_DEVELOPMENT_TOOLS` is off, so Debug and Release emitted real Optick
calls. `DownloadDependencies.sh` fetches the headers, and the generator passes `-DUSE_OPTICK=0`.

**`DownloadDependencies.bat` has no Linux analogue.** It downloads one prebuilt `External.zip`
of Windows binaries from an upstream release. Each dependency is fetched and built from source
instead.

## Notes for the next session

- **Run the Windows build.** Twelve upstream files changed. The riskiest is `IniFile.cpp`, where
  an `#endif` moved.
- **Phase 2 is the critical path.** Nothing above `Base` builds without the reflection codegen.
- The RHI stub generator is not committed, on purpose: Phase 5 replaces those bodies, so
  regenerating would be destructive.
- `ixWebSocket` is pinned to **v12.0.1**. `NetworkServer_WebSockets.cpp` builds
  `ix::WebSocketServer` with 8 arguments, and the 8th only exists from v12.0.0.
- The smoke test used for criteria 5 and 6 is not committed. It links against `Base` directly,
  which `Tester` cannot do yet.

### 2026-08-27 - P0.5-P0.8, P0.10 Ninja emitter, flags, linking

`python3 Code/Scripts/NinjaGen/NinjaGen.py` writes `Build/Linux/Esoterica.ninja` and
`compile_commands.json`. `ninja -f Build/Linux/Esoterica.ninja` then reaches the compiler.
**Nothing links, and only one translation unit compiles.** That is the expected end state for
Phase 0, and the error output is Phase 1's worklist. See below.

- **P0.5 flags.** `Toolchain.py` translates `Esoterica.props`. `-std=c++20`, `-std=c17`,
  `-fno-exceptions`, `-msse4.2 -mavx`, `-g`, `-fPIC`, `-O0` or `-O2`, `-flto` in Shipping,
  `-fvisibility=hidden` on shared libraries. `-Wall -Wextra` and **no `-Werror`**, per the phase
  document. `NOMINMAX`, `WIN32_LEAN_AND_MEAN` and `_CRT_SECURE_NO_WARNINGS` are dropped;
  `NDEBUG` is kept in every configuration, because upstream conditions it on `$(Platform)`, not
  on `$(Configuration)`.
- **P0.6 linking.** All 14 imported property sheets are mapped. `pkg-config` for Freetype,
  plain `-l` otherwise, `llvm-config` for LLVM. The six dropped sheets are named in the table
  with no flags, so they are recognised rather than forgotten, and **none contributes an
  `EE_ENABLE_*` define** (Conventions rule 4).
- **P0.7 layout.** `Build/Linux_<Config>/` for binaries, `Build/_Temp/Linux_<Config>/<Project>/`
  for objects, derived from the repo-relative source path. Nine configurations: the three from
  the `.slnx`, plus ASan, TSan and UBSan on Debug and Release. MSan is dropped.
- **P0.8 `compile_commands.json`.** Generated with `ninja -t compdb` against the Debug rules
  only, using the real Linux toolchain. 603 entries.
- **P0.10 `.gitignore`.** Added `Build/`, `compile_commands.json`, `.ninja_deps`, `.ninja_log`.
  None of the four was ignored: the existing entry is lowercase `build/`.

Files added:

- `Code/Scripts/NinjaGen/Toolchain.py` - the property sheet and flag translation.
- `Code/Scripts/NinjaGen/Checks.py` - 20 checks, exits 0. See the decision below on
  what belongs there.

`Code/Scripts/NinjaGen/NinjaGen.py` is rewritten. It is the one upstream file this port replaces
wholesale; see [01-UpstreamMerges.md](01-UpstreamMerges.md).

**Upstream files edited, beyond `NinjaGen.py`:** two, both one character, both registered in
[TouchedFiles.md](TouchedFiles.md).

| File | Change |
|---|---|
| `Code/Base/Utils/GlobalRegistryBase.h` | `#include "Base\Esoterica.h"` to `"Base/Esoterica.h"` |
| `Code/Base/Input/InputDevices/InputDevice_Controller.cpp` | `#include "Base\Math\Vector.h"` to `"Base/Math/Vector.h"` |

clang does not treat `\` as a path separator inside an include, so on Linux neither header was
found. The first accounted for 43 not-found errors in `Esoterica.Base` on its own. MSVC accepts
`/`, so Windows is unaffected. This needed escalation, because neither file was in
the registry.

Acceptance criteria met: **1, 2, 3, 4, 5, 6, 7, 8, 9, 11.** Not met: **10**, the Windows MSBuild
build, which has not been run. There is no Windows machine on this side.

## The Phase 1 worklist

From `ninja -f Build/Linux/Esoterica.ninja Build/Linux_Debug/libEsoterica.Base.so`. 132
translation units attempted, 131 failed, 1 compiled clean
(`Code/Base/ThirdParty/mpack/mpack.c`).

**No error is a generator fault.** There are no missing include paths, no bad flags, and no
malformed rules. Every one traces to a platform implementation that does not exist yet, and each
already has a row in [TouchedFiles.md](TouchedFiles.md).

| Errors | Message | Root cause | Fix |
|---|---|---|---|
| 1451 | `'__declspec' attributes are not enabled` | `EE_BASE_API` and friends expand to `__declspec(dllexport)` | The six `_Module/API.h` files need their `__attribute__(( visibility( "default" ) ))` branch |
| 502 | `unknown type name 'size_t'` | `Esoterica.h:55` includes `Platform/Platform_Win32.h` under `#if _WIN32` and has no `#elif` | `Platform_Linux.h`, and the 2-line guard addition |
| 254 | `unknown type name 'va_list'` | as above | as above |
| 199 | `use of undeclared identifier 'EE_DEBUG_BREAK'` | defined in `Platform_Win32.h` | `Platform_Linux.h` must define it |

The macros clang names in its expansion notes, in order: `EE_BASE_API` (1473), `EE_TRACE_HALT` (107),
`EE_UNIMPLEMENTED_FUNCTION` (106), `EE_ASSERT` (92), then the vendored `EASTL_ATOMIC_*` (88),
`PUGIXML_API` and `PUGIXML_CLASS` (32), and `ENKI_ASSERT` (9). The vendored ones key off
`_MSC_VER` inside `Code/**/ThirdParty/`, which Conventions rule 5 puts out of bounds; fix them
with a `-D` in the generator, not by editing the source.

Do **not** add `-fdeclspec` to make these go away. It hides the `_Module/API.h` work rather than
doing it.

What the next session needs to know:

- **`NinjaGen.py` runs `SyncUpstream.py` in check mode first and stops on a non-zero exit.**
  There is deliberately no flag to skip it.
- **The generator reports problems but does not fail on them.** 11 against this tree, all
  expected: `External/RenderDoc` and `llvm-config` are deferred dependencies, and
  `Esoterica.Applications.Engine` has no sources until Phase 6.
- **`clang/AST/Ast.h` is not found** when building the Reflector. That is Phase 2's LLVM
  dependency, and note the casing: the real header is `clang/AST/AST.h`, so this may be another
  case mismatch. Check when LLVM is installed.
- Sanitizer builds exist for Debug and Release only. `Build/Linux_Debug_ASan/` and so on.
- **A whole-tree `ninja` run gets killed on a small machine.** `ninja -n` walks all 588 Debug
  edges and the graph is valid, but a real build at the default `-j8` on 7 GB of RAM dies partway
  with no ninja summary line, which looks like the OOM killer. clang holds a lot of memory per
  translation unit at `-O0` with `-g`. Use `-j2` or `-j4` on a small machine. This is an
  environment limit, not a generator problem: `Esoterica.Base` on its own completes reliably and
  is what Phase 1 targets.

### 2026-08-27 - P0.1-P0.4 Source lists and the upstream sync tool

The Linux build now has a source model. It reads three text files, not XML. It writes no ninja
file yet; that is P0.5 to P0.8.

| File | Maintained | Contents |
|---|---|---|
| `Code/Scripts/NinjaGen/UpstreamProjects.txt` | generated | 13 projects, 620 sources, per-configuration types, references, property sheets |
| `Code/Scripts/NinjaGen/Exclusions.txt` | by hand | 4 globs, each with a reason |
| `Code/Scripts/NinjaGen/LinuxSources.txt` | by hand | empty. Phase 1 adds the first entries |

- **P0.1 sync tool.** `SyncUpstream.py --update` reads `Esoterica.slnx` and the `.vcxproj`
  files and rewrites `UpstreamProjects.txt`. With no arguments it re-derives the list and exits
  1 with a unified diff if the committed copy is stale. It is the only thing in the build that
  reads XML. It honors `<Build Project="false"/>`, `<Build Solution="Shipping|*"
  Project="false"/>`, and reads configuration names from `<Configurations>`.
- **P0.2 per-configuration facts.** `ConfigurationType` and property sheet imports are both read
  per configuration. `Esoterica.Base` is `DynamicLibrary` in Debug and Release and
  `StaticLibrary` in Shipping, and it drops `ixWebSocket.props` in Shipping.
- **P0.3 exclusions.** `Exclusions.txt` holds 4 globs: `**/*_Win32.cpp`, `**/Win32/**`,
  `Code/Base/Render/RHI_Direct3D12.cpp`, and `Code/Base/ThirdParty/D3D12MemoryAllocator/**`.
  A glob that matches nothing is reported as a problem.
- **P0.4 autogenerated globbing.** `_Module/_Autogenerated/{TypeInfo,Shaders}/*.cpp` per
  project, case-insensitively. These are Reflector outputs, so they stay globbed.

Files added:

- `Code/Scripts/NinjaGen/SourceLists.py` - the list format and the build model.
  `python3 Code/Scripts/NinjaGen/SourceLists.py` prints the model and reports problems.
- `Code/Scripts/NinjaGen/SyncUpstream.py` - the only reader of the Visual Studio projects.
- `Code/Scripts/NinjaGen/Checks.py` - the generator's checks. No test framework.
- The three list files above.

Upstream files edited: **none.** `NinjaGen.py` is untouched and still stale. P0.5 rewrites it.

Acceptance criteria met: part of 2 (`Esoterica.Base` keeps 132 sources and excludes 15, which
accounts for all 147 `ClCompile` entries), 4, 7, and 9. Criteria 1, 3, 5, 6, 8, 10 and 11 need
the emitter, which is P0.5 to P0.8.

What the next session needs to know:

- **Run `SyncUpstream.py` in check mode at the top of `NinjaGen.py`** and stop on a non-zero
  exit. The check is what makes the lists safe, and it is worthless if nothing runs it.
- **`Esoterica.Applications.Engine` has zero sources.** Its only source was
  `Win32/EngineApplication_Win32.cpp`. Phase 6 adds the Linux entry point. The emitter must not
  treat an empty source list as an error.
- **`Esoterica.Scripts.Reflect` is `ConfigurationType: Makefile`** and is never built directly.
  Skip it rather than warning about it.
- **14 property sheets are imported** by at least one project: `AmdAgs`, `Box3D`, `CTT`, `DXC`,
  `Esoterica`, `FreeType`, `GameNetworkingSockets`, `LLVM`, `LivePP`, `MeshOptimizer`,
  `RenderDoc`, `SQLite`, `WinPixEventRuntime`, `ixWebSocket`. **None are mapped to link flags
  yet.** That is P0.6. `LivePP` is imported, so P0.6 must skip it without defining
  `EE_ENABLE_LPP` (Conventions rule 4).
- `Esoterica.slnx` also lists `EA.props`, `FBXSDK.props`, `Imgui.props`, `NavPower.props`,
  `Optick.props` and `SuperLuminal.props`, but **no `.vcxproj` imports them.** P0.6 needs no
  mapping for those.
- Headers are no longer listed anywhere. The `REFLECT` rule's dependency set should be a
  `**/*.h` glob per project. Phase 2 needs this.

---

## Decisions made during implementation

Record any decision that a future reader would otherwise have to work out from the code. Give
the reasoning, not just the outcome.

<!--
### YYYY-MM-DD - <decision>
**Context:** ...
**Decision:** ...
**Rationale:** ...
**Alternatives rejected:** ...
-->

### 2026-08-30 - `RHI_Vulkan.cpp` creates the Vulkan surface, through `Platform_Linux.cpp`

**Context:** P6.2 raised it. `Platform::SetMainWindowHandle` holds an `SDL_Window*`, because that
is what the imgui and input backends need and what `Win32Application` stores. But
`EngineModule.cpp:135` hands the same value to `RenderWindow::SetNativeWindowHandle`, and
`RHI_Vulkan.cpp:2216` casts it to a `VkSurfaceKHR`, which is what P5.3 decided. The two do not
agree.

P5.3's answer was "the application creates the surface and hands it over", and **the application
has no place to do it**. `EngineModule::InitializeModule` calls `RenderSystem::Initialize`, which
creates the `VkInstance`, and calls `SetNativeWindowHandle` three lines later. Nothing the
application owns runs in between, and before that call there is no instance for a surface to come
from.

**Decision:** the RHI creates the surface. `m_pNativeWindowHandle` stays an `SDL_Window*` on
Linux, and `CreateSwapchain` calls a new Linux-only `Platform` function to turn it into a
`VkSurfaceKHR`:

```cpp
// Platform_Linux.h, in namespace EE::Platform
EE_BASE_API void* CreateVulkanSurface( void* pVulkanInstance, void* pNativeWindowHandle );
EE_BASE_API void DestroyVulkanSurface( void* pVulkanInstance, void* pSurface );
```

`Platform_Linux.cpp` is the only file that includes both SDL3 and Vulkan. `RHI_Vulkan.cpp` calls
through `void*` and never sees an SDL header.

**Rationale:** it edits no upstream file, which is the prime directive. It also keeps what P5.3
actually required, which was that **`Base/Render` depends on no window system library** - the
dependency lands in `Base/Platform`, and `Esoterica.Base` already links SDL3, so no project gains
a dependency it did not have. What it revises is only the second half of P5.3's sentence, "and the
application owns it", which turned out not to be reachable.

Surface ownership moves with it: `DestroySwapchain` still never destroys the surface, because
`ResizeSwapchain` recreates around an unchanged handle, but the RHI now destroys it when the
swapchain is destroyed for good rather than resized. **P6.6 owns writing this**, including where
the destroy belongs, and P5.3's entry should be read together with this one.

**Alternatives rejected:**

- **A two-line `#elif defined( __linux__ )` in `EngineModule.cpp:135`.** The right shape under
  Conventions rule 2, but `Code/Engine/_Module/EngineModule.cpp` is not in
  [TouchedFiles.md](TouchedFiles.md), and it would put SDL3 into `Esoterica.Engine.Runtime`, which
  has no other use for it.
- **Storing the `VkSurfaceKHR` in `Platform::SetMainWindowHandle`.** Does not work: there is no
  instance when the window is created, and the imgui and input backends need the `SDL_Window*`
  from that same accessor.

### 2026-08-29 - Open question 7: the shader reads its own indirect arguments

**Context:** Every engine render pass draws through `CmdExecuteIndirect`, and every engine command
signature sets root constants and binds a root CBV per command. **No Vulkan indirect draw rebinds
anything per command.** P5.13 landed the mechanical half and refused the rest at the line, which
left the frame unable to draw.

**Decision:** **Turn the push into a pull.** The shader reads its own command's root data out of
the argument buffer, indexing with the `DrawIndex` builtin, which is core Vulkan 1.1 through
`VK_KHR_shader_draw_parameters`. The change hides in the `RHI.esh` macros and is guarded on
`__spirv__`, so shader bodies and the whole Direct3D 12 path stay as they are. Scheduled as
**[P5.17](Phases/Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change)**,
**after Phase 6 bring-up**, because nothing here can be tested until the engine runs and this
touches shaders Windows also compiles.

**Rationale:** It needs no extension and no new device requirement, and it is how GPU-driven
renderers normally solve this. Two things were checked before choosing it, because every engine
indirect draw is a mesh dispatch rather than a vertex draw: the bundled SPIR-V validator allows
`DrawIndex` in `MeshEXT` and `TaskEXT` (`validate_builtins.cpp:4009`), and DXC accepts
`[[vk::builtin( "DrawIndex" )]]` on mesh and amplification inputs
(`DeclResultIdMapper.cpp:3508`). The Vulkan backend uses no push constants today, so the range
that carries the argument buffer address is free.

**Alternatives rejected:**

- **`VK_EXT_device_generated_commands`.** It sets push constants per command and binds index and
  vertex buffers per command, but it **still cannot bind a descriptor set** per command. It
  therefore needs the same shader change *plus* a device extension the Phase 4 requirement list
  does not have.
- **A compute pre-pass.** This is the usual answer when an argument buffer is the wrong shape, and
  it does not apply. Repacking is not needed - `vkCmdDrawMeshTasksIndirectEXT` takes a stride and
  already reads the draw block correctly. What is needed is a descriptor binding, and a compute
  shader cannot bind a descriptor for a later draw either. This also answers the question Phase 4
  left open when it set `m_indirectRootConstant` to `false`.
- **A CPU loop, one indirect call per command.** The command count is produced on the GPU, so the
  CPU does not know how many commands there are and cannot read the GPU-written root data to bind
  it. It survives only as a fallback for the indirect *compute* case, where the index is known
  because the CPU wrote the loop.

**Consequences worth stating:** This edits upstream shader files, which the escalation list
normally forbids. It was escalated and approved. The five files are on
[TouchedFiles.md](TouchedFiles.md) as `planned`, each edit inside an `#ifdef __spirv__`.
**Verifying it needs mesh shader hardware**, which this development machine does not have.

### 2026-08-28 - Phase 5 is written blind, because nothing on Linux can run it

**Decision: implement the Vulkan backend verified by compile and link only, and first execute it
when Phase 6 lands.** Recorded because it makes every Phase 5 entry weaker than it looks, and a
later session reading "P5.1 done" needs to know what "done" meant.

**The blocker, verified rather than assumed.** Nothing on Linux can reach `RHI::CreateContext`:

- Of the seven applications, `BuildGenerator` is excluded permanently, `Editor` and
  `ResourceServer` do not compile and are Phase 7, `Reflector` and `ResourceCompiler` are offline
  tools that never touch the RHI, and `Tester` is an upstream scratchpad.
- `Esoterica.Applications.Engine` has exactly one source file, `Win32/EngineApplication_Win32.cpp`,
  which the `**/Win32/**` exclusion glob drops. There is no Linux engine binary.
- Writing one does not help on its own. `BaseModule::InitializeModule` calls
  `m_inputSystem.Initialize()` at `:151`, which calls `Initialize()` on a `KeyboardMouseDevice`
  and four `XBoxControllerInputDevice`s, and `m_imguiSystem.Initialize()` at `:157`, which calls
  `InitializePlatform()`. All three are Phase 6 stubs that halt. `RHI::CreateContext` lives at
  `RenderSystem.cpp:45`, inside `EngineModule::InitializeModule`, which runs after both.

So running the engine needs P6.1 through P6.5 real first - SDL3, `LinuxApplication`, the imgui
SDL3 backend, keyboard and mouse, gamepad. That is essentially all of Phase 6, whose own estimate
is 3-4 weeks, before a line of Vulkan is written. Phase 6's stated prerequisite is "Phase 5
through bring-up step 8", so the two phases are circular.

**Rejected: the `Tester` harness the phase document names.** `Code/Applications/Tester/Main.cpp`
is 114 lines, roughly 90 of them commented-out experiments, and the live code loads and saves an
ini file from a hardcoded `D:\Esoterica\...` path. `int numTestFailures` is returned and never
incremented. It is a scratchpad, not a test framework, and it is an upstream file that
[TouchedFiles.md](TouchedFiles.md) does not list. **Acceptance criterion 4 cannot be met as
written**, and the bring-up steps 1 to 8 it asks for have no home.

**Rejected: a small Linux-only bring-up binary.** It would have verified P5.1 today, at the cost
of a target with no Windows counterpart.

**Rejected: the `EE_SHIPPING` loophole.** `EE_UNIMPLEMENTED_FUNCTION` compiles to nothing when
`EE_DEVELOPMENT_TOOLS` is off, so a Shipping build walks past all three Phase 6 stubs silently.
It also disables every assert and the validation layers, which Phase 5's "do not" list rules out
explicitly.

**What this costs, stated plainly.** Device feature checks, capability reporting, memory
statistics and the whole binding model are unexercised. A wrong assumption in any of them
surfaces at Phase 6 on top of several finished task groups rather than immediately. The
mitigation available is that every extension name, feature bit and struct name is read out of the
system Vulkan headers rather than recalled, and the two GPUs in this machine were surveyed during
Phase 4 against the binding model's requirement list.

**One thing that makes this less bad than it looks.** The engine's own initialisation order is
already headless-first: `EngineModule::InitializeModule` calls `m_renderSystem.Initialize()`
*before* `m_renderWindow.SetNativeWindowHandle( Platform::GetMainWindowHandle() )` at `:135`, and
`Platform::GetMainWindowHandle` is a plain `void*` global with no Win32 types in it. When Phase 6
lands, bring-up steps 1 to 8 map onto the real engine with no window needed, and step 9 is where
the swapchain arrives. The ladder the phase document wanted still exists; it just runs later.

### 2026-08-28 - P4.6 is deferred to a Windows machine, with the Linux half done in advance

**Phase 4 is done on Linux.** What is left of P4.6 is one thing: the structural diff of the
generated shader code against the Windows-generated output, and it cannot be done without a
Windows machine. Criterion 10, that the Windows MSBuild build still succeeds, is in the same
position.

**P4.6 is four checks and three of them were already done:**

| Check | State |
|---|---|
| 1. The generated C++ compiles | done, and it links into `libEsoterica.Engine.Runtime.so` |
| 2. Structural diff against Windows, bytecode ignored | **needs Windows** |
| 3. Binding indices agree with what Phase 5 expects | not applicable, see below |
| 4. The Phase 0 generator's glob picks the files up, which is P0.4 | done |

**Check 3 rests on a wrong picture of the generated code, and is worth correcting.** A generated
`.cpp` is a base85 LZAV-compressed bytecode blob, three size constants, and a `Register_<Name>`
function that emplaces a `ShaderByteCode` into one of three vectors. **There are no binding tables
and no binding indices in it at all.** Binding information lives in the SPIR-V and is reflected at
runtime by the backend, which is the same thing P4.2 and P4.4 found when they established that the
Reflector never reflects bytecode. So check 3 was settled by the P4.3 binding model decision, and
check 2's "the same binding tables" has nothing to compare.

**Deferring blocks nothing.** `P4.6` is referenced nowhere outside its own task heading. Phase 5's
prerequisite names what actually matters - "Phase 4's binding-model decisions are hard
prerequisites" - and those are made and recorded. Every phase from 0 to 7 carries a "the Windows
MSBuild build still succeeds" criterion, so Windows work is owed across the whole plan rather than
here in particular, and Phases 2 and 3 are already marked done on Linux with Windows criteria
outstanding. This is the same call, made the same way.

**The Linux half is done in advance, so that P4.6 is a one-command diff later rather than a task
somebody has to re-derive.** `Code/Scripts/ShaderStructure/ExtractShaderStructure.py` emits a
normalised structural summary of a generated shaders directory, and
`Code/Scripts/ShaderStructure/Linux.structure.txt` is the Linux side of the comparison, committed.
Whoever has a Windows machine runs the same script there and diffs the two files.

The script removes only what is bytecode-dependent: the base85 payload lines, the three
`g_byteCode_*_{decompressed,decoded,compressed}Size` constants, the array bound in
`static char const g_byteCode_X[9501]`, and the `Generated For:` line, which is an absolute path.
Everything else is kept **verbatim**, with whitespace collapsed. Keeping the rest verbatim rather
than parsing out the interesting parts is deliberate: a parser only compares what it was taught to
look for, and this check exists to catch what nobody predicted.

What survives is exactly the structure criterion 6 asks about - here is a whole entry:

```
===== WorldUpdate.cpp =====
...
void Register_WorldUpdate( RHI::Context* pContextRHI, TVector<MaterialShader>& materialShaders, ... )
{
ByteCodeList shaderByteCodeParameters;
shaderByteCodeParameters.clear();
shaderByteCodeParameters.emplace_back( RHI::ShaderByteCode( RHI::ShaderStage::Compute, StringID( "CS" ), g_byteCode_WorldUpdate_CS, ... ) );
computeShaders.emplace_back( ComputeShader( pContextRHI, StringID( "WorldUpdate" ), shaderByteCodeParameters ) );
}
```

The shader set, each shader's stages, its `StringID` names, which of the three vectors it
registers into, the `Initialize` call order, and the `.parameters.h` structs with their members and
defaults. 34 files, 1010 lines.

**Verified:** the snapshot contains no base85 payload line and no size constant definition, checked
by grep; and it is **byte-identical after a full `./CompileShaders.sh --rebuild`**, so a future
diff against Windows will show real divergence rather than regeneration noise.

**Internal consistency already holds on Linux**, which is the part of criterion 6 that can be
checked without Windows: 28 `.esf` files produce 28 generated `.cpp`, 28 `Register_` declarations
and 28 `Initialize` calls, with no gaps in either direction.

**What a Windows machine still owes Phase 4:** criterion 6, the structural diff, now one command;
and criterion 10, that the Windows Reflector's DXIL output is unchanged. Criterion 10 is the more
valuable of the two. `ShaderReflection_ShaderCompiler.cpp` has been edited three times in this
phase - the `-spirv` switch, `DXC_ARG_BINDING_MODEL` and `DXC_ARG_MEMORY_LAYOUT` - and every one
expands to nothing under `_WIN32`, so Windows is unchanged **by construction**. That is an
argument, not a test.

### 2026-08-28 - Defect 3 is fixed: bitfields that differ only in signedness now merge

**Fixed**, in `Code/Scripts/DXCPatches/0003-spirv-merge-bitfields-that-differ-only-in-signedness.patch`.
Every structured buffer stride now equals the size the engine's own
`HLSL_STATIC_ASSERT`s demand, `MeshCluster` included.

**It was worse than the previous entry recorded.** That entry named `MeshCluster` as the only
affected type, on the grounds that it is the only struct mixing signed and unsigned bitfields.
That was the right observation and the wrong conclusion: the refusal is on the *exact* base type,
not on signedness, so **two distinct enum types are also refused**. `RenderView` has
`RenderViewFlags : 2`, `RenderViewLayerFlags : 8` and `uint : 22` adjacent, gets three words
instead of one, and came out at 360 bytes where C++ and DXIL both say 352. It was silently wrong
too, and nothing in the earlier survey would have caught it, because `RenderView` carries no
`HLSL_STATIC_ASSERT`. The lesson is that the asserts are a partial oracle: they cover 13 types
out of everything shared with C++.

| Type | C++ / DXIL | Before | After |
|---|---|---|---|
| `MeshCluster` | 32 | 40 | 32 |
| `RenderView` | 352 | 360 | 352 |

Both confirmed against C++ by compiling the declarations, and against Direct3D by compiling the
engine's asserts on the DXIL back end, which implements `_Static_assert`.

**Why it was not a one-line change.** The comment DXC left behind is true:

> Bitfields can only be merged if they have the exact base type.
> (SPIR-V cannot handle mixed-types bitfields).

A SPIR-V struct member cannot be signed and unsigned at once, and both extract opcodes require
Base and Result Type to be **the same type**, not merely the same width. `spirv-val` rejects
`OpBitFieldUExtract %uint %int_base` with "Expected Base Type to be equal to Result Type", which
was checked directly with a hand-assembled module rather than assumed from the spec text. So
merging is only half the job; the accesses have to be fixed too, and that is most of the patch.

The merged member takes the first field's type. Each field is reached by reinterpreting the
member into the field's own type and extracting in that type, so Base and Result Type agree and
the signed-or-unsigned choice still follows the field:

```
%35 = OpBitcast %int %34
%36 = OpBitFieldSExtract %int %35 %uint_0 %uint_24     ; int sgn : 24  -> sign-extends
%40 = OpBitcast %uint %39
%41 = OpBitFieldUExtract %uint %40 %uint_24 %uint_8    ; uint uns : 8  -> zero-extends
```

Writes mirror it: the value is reinterpreted into the member's type before `OpBitFieldInsert`,
which has the same requirement.

**Four places changed, and the second one is the trap.** `AlignmentSizeCalculator` computes sizes
and offsets from the AST with its own copy of the merge rule, entirely separately from
`LowerTypeVisitor`, which decides which fields share a member. Fixing only the lowering produces
a struct whose members are merged and whose stride says otherwise - which is what the first
attempt here did, and it is worse than the original bug. The two have to agree.

**Verified:**

- All 46 stages compile and pass `spirv-val --target-env vulkan1.3 --scalar-block-layout`.
- **Every** stride now matches the asserted size. Before this patch `MeshCluster` did not.
- **32 of the 46 stages are byte-identical to the previous compiler.** The 14 that change are
  exactly those reading `MeshCluster` or `RenderView`, and both move to the Direct3D layout.
- Sign extension checked instruction by instruction on a shader that reads and writes both a
  signed and an unsigned field of one merged word, in both directions.
- `./CompileShaders.sh --rebuild` exits 0, and the engine compiles and links against the result.
- The patch applies to a pristine clone of the pinned tag after 0001 and 0002, which is the path
  `DownloadDependencies.sh` takes.

**DXC's own tests: 1340 of 1357 pass, against a 1345 baseline.** Five fail. Two are the
instruction reordering from patch 0002, already recorded. The other three -
`vk.layout.struct.bitfield.hlsl`, `vk.layout.struct.bitfield.assignment.hlsl` and
`op.structured-buffer.access.bitfield.hlsl` - encode the behaviour this patch changes rather than
catching a fault in it. The first expects `%S6 = OpTypeStruct %uint %int` for
`struct S6 { uint f2 : 1; int f1 : 1; }`; compiling that struct with
`_Static_assert( sizeof( S6 ) == 4 )` on the DXIL back end passes, so one member is the Direct3D
answer and the test encodes the divergence. Upstream would want all three updated in a
submission; they are deliberately not touched, so the fork's diff stays confined to the fix.

**Three patches now, all in the SPIR-V back end.** The DXC decision entry says to move to a fork
"if the patching becomes substantial". Three patches across six files is the point at which that
is worth asking rather than assuming. All three are worth submitting upstream.

### 2026-08-28 - P4.3 Memory layout: `-fvk-use-dx-layout`, and the defect it exposed

**Decision: the Reflector passes `-fvk-use-dx-layout` on Linux.** It is not optional. Without it
seven shared struct types are laid out wrong, and nothing anywhere reports it.

**Why it is forced.** The `.esh` files compile as both C++ and HLSL, so the same struct
declaration describes the bytes the engine writes and the bytes the shader reads. DXC's default
is Vulkan's standard buffer layout, which rounds a struct's size up to its largest member's
alignment; Direct3D packs tightly. Measured against the sizes the engine's own
`HLSL_STATIC_ASSERT`s demand, over every `.esf` and entry point:

| Type | Asserted | Default rules | `-fvk-use-dx-layout` |
|---|---|---|---|
| `ImguiVertex` | 20 | **24** | 20 |
| `MeshInstance` | 64 | **80** | 64 |
| `MeshInstanceTransformUpdateCommand` | 64 | **96** | 64 |
| `PointLightUpdateCommand` | 64 | **80** | 64 |
| `SpotLightUpdateCommand` | 64 | **80** | 64 |
| `DirectionalLightUpdateCommand` | 32 | **48** | 32 |
| `MeshCluster` | 32 | **40** | **40** |
| the other six | - | correct | correct |

Seven wrong by default, one wrong with the flag. `ImguiVertex` is the clearest: 20 bytes in C++,
confirmed by compiling the declaration, and a 24-byte array stride under the default rules, so
every vertex after the first would be read from the wrong offset. No validation error, no crash,
just wrong geometry - exactly the failure the phase document says to prevent now rather than
debug in Phase 5.

**Two consequences that must not be missed.**

1. **The Vulkan device must enable `scalarBlockLayout`.** DXC's documentation says
   `-fvk-use-dx-layout` requires `VK_EXT_scalar_block_layout`, and it is not advisory: without it
   the modules are invalid. `spirv-val` rejects 18 of the 46 stages with "Matrix with a stride 12
   not satisfying alignment to 16 ... may be allowed if you enable the scalarBlockLayout
   feature". The feature is core in Vulkan 1.2 and the baseline is 1.3, so it is a feature bit,
   not an extension. Both GPUs in this machine report `scalarBlockLayout = true`. **P5.1 must
   enable it, and refuse a device that lacks it**, alongside the binding model's requirements.
2. **Validate with `--scalar-block-layout` from now on.** All 46 stages pass with it and 18 fail
   without it, and those 18 failures are the validator being told the wrong rules rather than bad
   modules. The criterion 3 check recorded earlier needs the flag.

**The defect this exposed. Defect 3: DXC's SPIR-V back end does not merge bitfields of different
signedness.**

`MeshCluster` is the one type the layout flag does not fix, and it is not a layout-rule problem.
Reduced to a minimal shader, with `-fvk-use-dx-layout` in force:

| Struct | Stride |
|---|---|
| `struct { int a : 24; uint b : 8; }` | **8** |
| `struct { int a : 24; int b : 8; }` | 4 |
| `struct { uint a : 24; uint b : 8; }` | 4 |

Same-signedness pairs merge into one 32-bit word. A mixed pair does not, and takes two.
`MeshCluster` has two mixed pairs - `int32_t m_anchorX : 24` with `uint32_t m_numVertices : 8`,
and the same for Y - so it comes out at 32 + 8 = 40.

**Direct3D does not do this.** The DXIL back end implements `_Static_assert`, so the engine's own
asserts are live there, and a DXIL compile of `ClusterCulling.esf` accepts
`sizeof( MeshCluster ) == 32`. A minimal `_Static_assert( sizeof( MixedSign ) == 4 )` also passes
on DXIL. C++ agrees: `sizeof` is 32, confirmed by compiling the declaration. So SPIR-V is the
odd one out, and `MeshCluster` reads are silently wrong on Linux today.

`MeshCluster` is the only affected type. Every other bitfield struct in the engine uses `uint` or
an unsigned enum throughout, which is why the layout table above has exactly one residual row.

**It is deliberate in DXC, and it is not a one-line fix.** `LowerTypeVisitor.cpp:1376`:

```cpp
    // Bitfields can only be merged if they have the exact base type.
    // (SPIR-V cannot handle mixed-types bitfields).
    if (previousField->type != loweredField.type)
      return loweredField;
```

That is true as far as it goes - a SPIR-V struct member cannot be both signed and unsigned - but
it is soluble: pick one base type for the merged word and choose `OpBitFieldSExtract` or
`OpBitFieldUExtract` per field at each access. Patch 0001 already added
`createBitFieldExtract` calls for exactly this shape of problem. The work is in type lowering and
at every read and write of a merged field, so it is closer to defect 2 in size than to a
one-liner. **Not attempted here.** Recorded so that whoever picks it up does not have to
re-derive it.

**Impact until it is fixed.** `MeshCluster` is read as a `StructuredBuffer` by the cluster
culling and mesh shading path. Reads past the first element take data from the wrong offset. This
will not show up until Phase 5 renders, and it will look like corrupt geometry rather than like a
layout bug, so it is written here in advance.

**Verified:** all 46 stages compile and pass `spirv-val --target-env vulkan1.3
--scalar-block-layout`; `./CompileShaders.sh --rebuild` exits 0 and regenerates all 34 files; the
engine compiles and links against the regenerated code.

### 2026-08-28 - Clip-space Y is inverted in the Vulkan viewport, not in the shader compiler

Acceptance criterion 9. It names one layer, as the criterion demands.

**Context.** The engine builds its projection matrices from DirectXMath, and says so:
`Math::CreatePerspectiveProjectionMatrix` is commented "Taken from DirectXMath:
XMMatrixPerspectiveFovRH", and the orthographic pair likewise (`ViewVolume.cpp:9` and `:29`).
Those are right-handed with a **Y-up NDC and a 0..1 depth range**. Vulkan agrees on the depth
range, which is why nothing here needs a depth fix, and disagrees on Y: Vulkan's NDC has +Y
pointing down. Uncorrected, every rendered image is vertically mirrored.

`RHI_Direct3D12.cpp:3049` sets the viewport straight through, with no flip of its own, so the
Direct3D path is the plain DirectXMath convention end to end.

**Decision: `RHI_Vulkan.cpp`'s `CmdSetViewport` performs the inversion, with a negative viewport
height. The shader compiler does not. `-fvk-invert-y` is not passed, and must not be added.**

```
vkViewport.y      = y + height;
vkViewport.height = -height;
```

`x`, `width`, `minDepth` and `maxDepth` pass through unchanged. A negative viewport height needs
no extension: it is core Vulkan from 1.1, and the baseline is 1.3.

The decision is written into `RHI_Vulkan.cpp` at the `CmdSetViewport` stub, not only here,
because the failure this criterion guards against is doing the flip twice, and the second flip is
silent. A comment in the function Phase 5 is about to fill in is the cheapest place to stop that.

**Why not `-fvk-invert-y`, which is what the phase document suggested first.**

- **It is a hard error on pixel and compute shaders.** Measured, not assumed:
  `error: -fvk-invert-y can only be used in VS/DS/GS/MS/Lib`. It compiles fine on `vs_6_6` and
  `ms_6_6`, and emits exactly what it says - an `OpFNegate` on component 1 before the
  `OpStore` to `gl_Position` - but the Reflector builds one `COMMON_DXC_ARGUMENTS` list and
  hands it to every stage. Adopting the flag means splitting that list per stage inside
  `ShaderReflection_ShaderCompiler.cpp`, a file the Windows build shares, for no capability the
  viewport does not already give.
- **A flip baked into bytecode is invisible at the API level.** Nothing in the backend would show
  it. The viewport version sits in the function anyone debugging an upside-down frame opens
  first.
- **It keeps the SPIR-V a faithful translation of the HLSL**, which matters for P4.6: a
  compiler-inserted negate in every vertex and mesh shader is noise in a structural comparison
  against the DXIL output.
- It is also the mapping every Direct3D-to-Vulkan translation layer uses, vkd3d-proton included.

**Why not flip the projection matrices.** They are engine code, shared with the Direct3D path,
and shaders do their own clip-space arithmetic against them. `RendererTypes.esh:92` converts a
projected bounding box from clip space to UV space with
`aabb.xwzy * float4( 0.5, -0.5, 0.5, -0.5 ) + 0.5`, where the negative Y terms encode the
Direct3D convention, and then samples a depth pyramid with the result. Flipping the matrix breaks
that; flipping the viewport does not, because the pyramid is rasterised through the same flipped
viewport and lands in memory the same way it does on Direct3D. This is the whole reason the flip
belongs in the rasteriser and not in the maths.

**The consequence that is easy to miss: front-face winding.**

Mirroring the viewport inverts triangle winding in framebuffer space, so the front-face mapping
has to absorb it. Two inversions apply on the way to Vulkan and they cancel:

1. `RHI_Direct3D12.cpp:5287` sets
   `FrontCounterClockwise = ( m_frontFace == FrontFace::ClockWise )`. That reads backwards -
   `FrontFace::CounterClockWise`, the RHI default, means front faces are **clockwise** in screen
   space on Direct3D. It is upstream's code and Conventions rule 3 says to leave it alone. To
   match Direct3D with no Y flip, Vulkan would have to invert the enum the same way.
2. The negative viewport height inverts winding again.

**So `RHI_Vulkan.cpp` maps `FrontFace` literally** - `ClockWise` to `VK_FRONT_FACE_CLOCKWISE` -
and it is only correct *because* of the viewport flip. Remove the flip and this has to be swapped
in the same commit. That derivation is recorded at the `CreatePipeline` stub too.

**What needs nothing, checked rather than assumed:**

- **`-fvk-use-dx-position-w` is not needed.** In Vulkan the `w` of `SV_Position` read by a pixel
  shader holds `1/w` where Direct3D holds `w`. **No pixel shader in this engine reads
  `SV_Position.w`.** Two read `SV_Position.xy`, `BilateralUpsample.esf:66` and
  `OITResolve.esf:32`, both as integer pixel coordinates for a `Load`. `FragCoord.xy` is
  framebuffer space with an upper-left origin in Vulkan exactly as `SV_Position.xy` is in
  Direct3D, so both are already right. The `.w` uses elsewhere are on clip positions the shaders
  compute themselves, not on the pixel shader input.
- **Depth.** DirectXMath's projections already produce 0..1, which is Vulkan's range. Nothing to
  do, and no `depthClipControl`.
- **The fullscreen triangle.** `FullscreenTriangle.esh` writes clip-space positions directly,
  bypassing the projection matrix, and encodes the same Direct3D convention: uv `(0,0)` maps to
  clip `(-1, 1)`, the top of the screen with Y up. It goes through the viewport like everything
  else, so the one flip covers it.

**Not verified, and cannot be until Phase 5 renders.** Everything above is derived from the
source and from measured compiler behaviour. The first real check is bring-up step 6, the
offscreen clear, and step 7, the offscreen triangle: if the image is upside down, the flip is
being applied twice or not at all. Check that before believing anything downstream of it.

### 2026-08-28 - P4.3 The bindless binding model

This is acceptance criterion 8, and Phase 5's hard prerequisite. It is written to be implemented
against, so it names every set, binding, descriptor type and feature bit.

**Context.** Direct3D 12 gives the engine two shader-visible descriptor heaps and a root
signature that holds nothing else. `CreateRootSignature` (`RHI_Direct3D12.cpp:4938`) builds only
root constants and root descriptors, never a descriptor table, and sets
`CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED`. `SetDescriptorHeaps` is
called once per command buffer (`:2917`). `GetBufferHandle`, `GetTextureHandle` and
`GetSamplerStateHandle` return a `uint16_t` offset into one of those two heaps, and the shaders
index them through the `GetBuffer` / `GetTexture` / `GetSamplerState` macros in `RHI.esh`, which
are `ResourceDescriptorHeap[i]` and `SamplerDescriptorHeap[i]`.

Vulkan has to reproduce that exactly, because the handle is the shared currency: the Reflector's
generated resource tables pack these same `uint16_t` values (see
`_AutoGenerated/ShaderReflection/WorldUpdate.esh`), and `InvalidResourceHandle` is `UINT16_MAX`.

#### How DXC maps SM6.6 heaps onto Vulkan

Established by compiling reduced shaders and by reading
`tools/clang/lib/SPIRV/DeclResultIdMapper.cpp` and `docs/SPIR-V.rst`, not from memory:

- The default is the **emulated** heap (`-fspv-use-emulated-heap`). Every distinct HLSL resource
  type read out of `ResourceDescriptorHeap` becomes its own SPIR-V `OpTypeRuntimeArray`
  variable, and **all of them are decorated with the same set and binding**. The heap index is
  the array index, unchanged. `SamplerDescriptorHeap` gets a second binding.
- Aliasing several descriptor types on one binding is what **`VK_EXT_mutable_descriptor_type`**
  exists for. DXC's own documentation names the extension as a requirement.
- The alternative native path, `-fspv-use-descriptor-heap`, emits `SPV_EXT_descriptor_heap`.
  DXC warns "SPV_EXT_descriptor_heap support is incomplete" on this engine's shaders, and the
  build treats warnings as errors. **Ruled out.**
- Heap bindings are otherwise allocated **lazily, from the first free binding numbers in set 0**,
  after every other resource is placed. That makes them a function of which registers a given
  shader happens to use, which is not something a backend can bind against. They must be pinned.

**Decision.** Two descriptor sets.

**Set 1 - the heaps.** One layout, identical for every pipeline in the engine, allocated once
and bound once.

| Binding | Vulkan descriptor type | Count | Source |
|---|---|---|---|
| 0 | `VK_DESCRIPTOR_TYPE_MUTABLE_EXT` | 65472 | `ResourceDescriptorHeap` |
| 1 | `VK_DESCRIPTOR_TYPE_SAMPLER` | 2048 | `SamplerDescriptorHeap` |
| 2 | reserved for the counter heap; do not create it | - | see below |

The counts are D3D12's, from `CreateContext`: `64 * 1023` for the CBV/SRV/UAV heap and `2048`
for the sampler heap (`RHI_Direct3D12.cpp:2229` and `:2236`). Keeping them identical is what
makes a handle mean the same thing on both backends. Both fit under `UINT16_MAX`, so
`InvalidResourceHandle` stays outside either heap.

The mutable type list for binding 0, taken from every `OpTypeImage` and buffer type that the
engine's shaders actually pull out of the heap:

| Vulkan descriptor type | HLSL that produces it |
|---|---|
| `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` | `Texture2D`, `Texture2DArray`, `Texture3D`, `TextureCube` |
| `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` | `RWTexture2D` |
| `VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER` | `Buffer<T>` |
| `VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER` | `RWBuffer<T>` |
| `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | `StructuredBuffer<T>`, `RWStructuredBuffer<T>`, `ByteAddressBuffer` |
| `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | no shader today, but `GetBufferHandle` accepts `DescriptorTypeFlags::ConstantBuffer` and returns a heap offset, so the heap must be able to hold one |

Binding 0 and binding 1 both take `PARTIALLY_BOUND` and `VARIABLE_DESCRIPTOR_COUNT`, and the
layout takes `UPDATE_AFTER_BIND_POOL`, because the engine writes handles into the heap while
command buffers that reference the heap are recording, exactly as
`CopyDescriptorsSimple` does on Direct3D 12.

Sampler heap slots 0 to 5 are the engine's common samplers, at the fixed indices in
`CommonSamplers.esh`. There are no Vulkan immutable samplers in this model, and no static
samplers in the pipeline layout: `CreateRootSignature`'s static sampler path finds no matching
shader resource in any current shader.

**Set 0 - the root parameters.** Per-pipeline layout, derived from reflection, created with
`VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT`.

| HLSL register | Set 0 binding | Vulkan descriptor type |
|---|---|---|
| `b<N>` | `N` | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` |
| `t<N>` | `8 + N` | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` |
| `u<N>` | `16 + N` | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` or `STORAGE_IMAGE` |
| `s<N>` | `24 + N` | `VK_DESCRIPTOR_TYPE_SAMPLER` |

The shifts exist because DXC ignores the register **type** when it derives a binding from
`register( xN, spaceY )`, so `b2` and `t2` would otherwise both be binding 2. Nothing in the
engine trips that today, and DXC does not diagnose the overlap when it happens, so the shifts
are there to make a future collision impossible rather than to fix a present one.

`CmdSetRootConstants` cannot become Vulkan push constants: the shader declares the block as
`ConstantBuffer<T> RootConstants : register( b0 )` through `EE_DECLARE_ROOT_CONSTANTS`, so DXC
emits a uniform buffer, and turning it into a push constant block needs `[[vk::push_constant]]`
in `RHI.esh`, which Phase 4 rule 4 forbids. Implement it instead as a copy into a per-frame
upload ring buffer followed by `vkCmdPushDescriptorSetKHR`. `CmdSetRootParameter` is the same
call with the caller's buffer and offset, and `VK_WHOLE_SIZE` for the range, which matches a
Direct3D 12 root descriptor: an address, with no size.

**The DXC flags.** Added to `DXC_ARG_BINDING_MODEL` in `ShaderReflection_ShaderCompiler.cpp`,
Linux only:

```
-fvk-auto-shift-bindings
-fvk-b-shift 0 0    -fvk-t-shift 8 0    -fvk-u-shift 16 0    -fvk-s-shift 24 0
-fvk-bind-resource-heap 0 1
-fvk-bind-sampler-heap  1 1
-fvk-bind-counter-heap  2 1
```

`-fvk-auto-shift-bindings` applies the same shifts to any resource that carries no `register()`,
which no shader has today; it is insurance against one arriving from upstream.

**There is no counter heap, and the engine needs none.** This corrects the previous entry.
`IncrementCounter`, `DecrementCounter`, `AppendStructuredBuffer` and `ConsumeStructuredBuffer`
appear in **no** `.esh` or `.esf` file in the repository. `AppendBuffer<T>` in `AppendBuffer.esh`
carries its own explicit `RWBuffer<uint> m_counterBuffer` and does its own `InterlockedAdd`, so
the hidden Direct3D counter is dead weight. DXC only emits the counter array when a counter is
actually used - `docs/SPIR-V.rst` says so, and compiling all 41 shader stages that build today
produces **zero** `counter_var` variables. Binding 2 of set 1 is therefore reserved and never
created; the flag is passed only so that a counter, if one ever appears, lands somewhere known
instead of displacing the sampler heap.

**Rationale for set 1 holding the heaps, rather than set 0.**

The Vulkan-idiomatic ordering is the opposite: the most stable set should be set 0, because
binding a pipeline whose layout differs from set N onwards disturbs every set from N up. Here
set 0 varies per shader and set 1 does not, so the heap set is disturbed whenever a pipeline
with a different root-parameter layout is bound, and the backend must re-bind set 1 on a
pipeline-layout change. **`CmdSetPipeline` is where that belongs**, next to the existing
`ResetRootSignature` call. One `vkCmdBindDescriptorSets` per pipeline change is a rounding error.

The reason for accepting that is the alternative. Putting the heaps in set 0 needs the root
parameters moved to set 1, and the only way to move a *set* without editing a `.esh` file is
`-fvk-bind-register`, which "requires all source code resources have `:register()` attribute and
all registers have corresponding Vulkan descriptors specified using this option". That makes the
Linux build fail the day upstream adds a global resource without a register, or with a register
outside our table, and upstream is active. The prime directive is cheap upstream merges. A
redundant descriptor-set bind is a much smaller price than a shader-compiler flag list that has
to track every register upstream ever writes.

**Verified.** Not asserted:

- All four heap and shift schemes were compared by compiling **every `.esf` in the repository**
  across `cs/ps/vs/ms/as_6_6` entry points. 41 stages compile; the flags introduce **no new
  failure**. The 5 that fail are exactly the known defect-2 shaders: `DebugDraw` MS,
  `DebugDrawMesh` MS, `DebugDrawResolve` CS, `InstancePickingResolve` CS, `WorldUpdate` CS.
- The set and binding decorations of every compiled stage were dumped and grouped. The result is
  the table above: set 0 binding 0 and 1 hold only uniform buffers, set 1 binding 0 holds the
  five aliased array types, set 1 binding 1 holds the sampler array.
- A real shader (`DefaultPBR` MS) shows nine aliased runtime arrays on set 1 binding 0.
- `spirv-val --target-env vulkan1.3` passes on the reduced heap shaders.
- The Reflector was rebuilt with the flags and run. `strings -eL` confirms the flags reach the
  binary, and the shader pass produces the same five failures and no others.
- Both GPUs in this machine (Intel UHD 620 on Mesa 25.2.8, NVIDIA driver 580.173) expose
  `VK_EXT_mutable_descriptor_type` with `mutableDescriptorType = true`, `VK_KHR_push_descriptor`
  with `maxPushDescriptors = 32`, and `runtimeDescriptorArray`,
  `descriptorBindingPartiallyBound`, `descriptorBindingVariableDescriptorCount`, the
  `*UpdateAfterBind` bits and the `*ArrayNonUniformIndexing` bits for sampled image, storage
  image, storage buffer and uniform texel buffer. The update-after-bind limits are six orders of
  magnitude above 65472.

**Required of the Vulkan device, so P5.1 can check it up front:** Vulkan 1.3, plus
`VK_EXT_mutable_descriptor_type` and `VK_KHR_push_descriptor` (core in 1.4), plus the descriptor
indexing feature bits listed above. Refuse the device if any is missing; there is no fallback
path, because the shaders are compiled for this model.

**One thing left open, for P5.16.** `RHI.esh` maps
`GetRaytracingAccelerationStructure( index )` to `ResourceDescriptorHeap[index]`, and
`GetAccelerationStructureHandle` returns `GetBufferHandle( ..., DescriptorTypeFlags::Buffer )`,
so on Direct3D 12 an acceleration structure is just another heap slot. On Vulkan it needs
`VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`, and whether that type may appear in a mutable
descriptor type list has **not** been checked here. No current shader reads an acceleration
structure from the heap, so this blocks nothing before raytracing. Settle it with the validation
layers on, in the `Tester` harness, before writing P5.16.

**Alternatives rejected:**

- `-fspv-use-descriptor-heap`, the native `SPV_EXT_descriptor_heap` path. DXC itself reports its
  support as incomplete on these shaders.
- Leaving the heap bindings to DXC's lazy allocation. The heap binding then depends on how many
  registers the individual shader used, so no two shaders agree, and the backend has nothing
  fixed to bind against.
- `-fvk-bind-register` to put the heaps in set 0 and the root parameters in set 1. Correct set
  ordering, but it requires an exhaustive register table and fails on any unregistered resource.
  See the rationale above.
- One descriptor set for everything. A push descriptor set layout cannot also hold the
  unbounded, update-after-bind heap arrays, so the root parameters and the heaps have to be in
  different sets whichever way round they go.

### 2026-08-28 - DXC is built from source and patched, rather than forked or vendored

**Context:** two defects in DXC's SPIR-V back end stop nine of this engine's shaders. We are on
the newest DXC release, so there is no version to upgrade to, and no flag avoids either defect
(`-fspv-use-descriptor-heap`, `-fvk-bind-counter-heap` and include-order variants were all
tried). The alternatives were to patch DXC, or to restructure the `.esh` files, which Phase 4
rule 4 forbids and which would change what Windows compiles.

**Decision: patch DXC.** The patches live in `Code/Scripts/DXCPatches/` and
`DownloadDependencies.sh` applies them to a pinned source clone, in name order, before building.
A patch that fails to apply is fatal: that means the pin moved and nobody re-checked the fix,
which is exactly the silent-breakage case worth stopping on.

**Why not vendor the DXC tree into this repository.** It is a 250MB LLVM fork with four
submodules, `/External/` is in `.gitignore` and nothing in it is tracked, and every other
dependency is fetched rather than vendored. Vendoring would also bury our two fixes among three
million lines of Microsoft's code, where no reviewer would ever find them.

**Why not fork DXC on GitHub, yet.** A fork is the right home once the patches grow or need to
track DXC `main`, and it is the mechanism for submitting upstream. For two fixes in one file, a
patch file next to the port's own documentation is easier to review and keeps the rationale with
the port. **Move to a fork if the patching becomes substantial.**

**Consequences.**

- The dependency now takes about 20 minutes to build, on 8 jobs, and is by far the slowest.
- **A re-run takes 20 seconds**, recompiling `SpirvEmitter.cpp` and relinking. Measured, after
  the fix below.
- **Two settings protect that, and both are easy to undo by accident.** `git clean` in
  `fetch_dxc` excludes `build`, or the clean deletes the build tree. And the configure passes
  `-DLLVM_APPEND_VC_REV=OFF`, against DXC's own cache file, which turns it on: with it on the
  generated version header derives from git state, and anything that moves the commit count
  rewrites that header and rebuilds **all 1018 targets**. That was measured the hard way, by
  committing a patch locally while debugging it and watching the next run rebuild the world.
  Being careful about the claim: the reset in `fetch_dxc` keeps the tree on the pinned tag, so
  the commit count should be stable without the flag as well. The flag pins the LLVM half of the
  version too, and costs nothing, so it stays.
- `libdxil.so` is no longer installed. It signs DXIL containers, which is a Windows concern.
- The reported version changes from the official `1.10(5373-c4d8f4f9)`. The **source** is the same
  commit the official binary was built from, `c4d8f4f9`, so the only difference between the two
  compilers is the patches.
- Both patches are worth sending upstream. Defect 1 already has an open issue with no owner.

### 2026-08-28 - P4.4 has nothing to replace, and P4.2 belongs to Phase 5

**Context:** P4.4 is written as the largest task in the phase: "replace `ID3D12ShaderReflection`
with SPIRV-Reflect", "this is the real work", "the one place in the port where a new abstraction
is the right call". It describes `ShaderReflection_ShaderCompiler.h` as exposing reflection
results in D3D terms and `ShaderReflection_ShaderInputReflector.cpp` as consuming them.

**None of that is true of this codebase.** Checked before starting the work:

- Every use of `ID3D12ShaderReflection` in the repository is in `RHI_Direct3D12.cpp`, lines 1007
  to 1096. That file is in `Exclusions.txt` and is Phase 5's business. The Reflector only
  *includes* `d3d12shader.h`, and never calls anything from it. Phase 3 noticed the same thing
  and left a comment saying so at `ShaderReflection_ShaderCompiler.h:10`.
- `ShaderCompiler` exposes no reflection results at all. Its members are a task system, two
  paths, a mutex and some strings.
- `ReflectedShader` is entirely platform-neutral: strings, paths, byte vectors, and a
  `ParameterInfo` of type, name, stride and offset.
- `m_parameters` and `m_resourceTable` are filled by `ShaderReflection_ShaderParser.cpp`, which
  **parses the `.esh` source text**. `ShaderInputReflector` reads only those and writes C++
  structs. No reflection API is involved on either side.

So the pipeline never reflects bytecode, and there is no D3D-shaped intermediate to replace. The
platform-neutral structure P4.4 asks for already exists, and is called `ReflectedShader`.

**P4.2 follows from this.** Nothing in Phase 4 would consume SPIRV-Reflect. The only code that
reflects compiled shader bytecode is the Direct3D 12 backend, building root signature and
descriptor information at runtime. Its Vulkan sibling is what needs SPIRV-Reflect, so the
dependency belongs to **Phase 5**, not here. Vendoring it now would pin a library with no caller.

**Decision:** P4.4 is closed as not applicable, and P4.2 is moved to Phase 5. Both are marked in
[Phase4-ShaderPipeline.md](Phases/Phase4-ShaderPipeline.md). This removes what the plan called
the bulk of the phase, so what is really left in Phase 4 is the five shaders, the two decisions,
and P4.5 and P4.6.

### 2026-08-28 - Defect 2 stops here, because the fix needs the binding model decision

**Superseded, same day. This entry is kept for the analysis, but its conclusions are wrong on
two counts and the defect is fixed.** See "P4.3 Defect 2 is fixed" above for what was actually
required.

- **The fix does not need the binding model decision.** No `.esh` or `.esf` file in this
  repository uses `IncrementCounter`, `DecrementCounter`, `AppendStructuredBuffer` or
  `ConsumeStructuredBuffer`; `AppendBuffer<T>` carries its own explicit `RWBuffer<uint>` counter.
  DXC emits the counter array only when a counter is used, so no shader here produces a
  `counter_var` at all, and there is no counter heap to bind or to number.
- **No destination-side work was needed, and counters-inside-structures did not have to be
  implemented.** The claim below that the destination "must have a counter to store into" and
  that the fix therefore needs the work `DeclResultIdMapper.cpp:1289` calls non-trivial is wrong.
  The destination already has a counter, created eagerly by `createFieldCounterVars`. Only the
  **source** failed to resolve, for three reasons rather than the two identified here, and the
  third one - that the counter is resolved before the right-hand side is emitted - is why the
  source-side half looked correct and still did not work.

**Context:** assigning a counter-bearing resource from a descriptor heap fails with "cannot
handle associated counter variable assignment". It stops `DebugDraw` MS, `DebugDrawMesh` MS, and
`DebugDrawResolve`, `InstancePickingResolve` and `WorldUpdate` CS.

**What it actually is,** narrowed with minimal shaders so nobody has to re-derive it:

| Case | Result |
|---|---|
| `RWStructuredBuffer` **initialized** from the heap, as a local | works |
| `RWStructuredBuffer` **assigned** from the heap, to a local | fails |
| `RWStructuredBuffer` **assigned** from the heap, to a struct field | fails |
| `RWBuffer` or `RWByteAddressBuffer`, either way | works, they carry no counter |

So it is assignment, not initialization, and only for a type that carries a hidden counter.
`AppendBuffer<T>` in `AppendBuffer.esh` holds a `RWStructuredBuffer<T>` and is built exactly this
way. `RawAppendBuffer` next to it uses `RWByteAddressBuffer` and was never affected.

**The Reflector's own code generator emits this pattern too**, which matters for option 3 below.
Two of the five failures are not in hand-written shader source at all:

```
_AutoGenerated/ShaderReflection/WorldUpdate.esh:91
    result.m_meshInstanceBuffer = ResourceDescriptorHeap[NonUniformResourceIndex( ... )];
_AutoGenerated/ShaderReflection/DebugDrawResolve.esh:54
    result.ArgumentBuffer_TransparentDepthOn = ResourceDescriptorHeap[NonUniformResourceIndex( ... )];
```

Those come from `ShaderReflection_CodeGenerator.cpp` building a resource table struct. Rewriting
`AppendBuffer.esh` by hand would therefore fix three of the five and leave these two, so option 3
is not sufficient on its own even if somebody were willing to break the no-`.esh`-edits rule.

`tryToAssignCounterVar` requires source and destination to agree on whether they have a counter.
The destination gets one created on demand; the source never resolves to one, so they disagree.
`getFinalACSBufferCounter` cannot see the heap for two reasons at once: `heap[i]` is a
`CXXOperatorCallExpr`, so `getReferencedDef` resolves it to the subscript operator and the
`AssocCounter#1` early return hands back that operator's null pair; and the heap branch below it
tests `isResourceDescriptorHeap` against the expression's own type, which is the indexed resource,
where `.Resource` is the type of the heap object being indexed. `isDescriptorHeap`, which
`getDescriptorHeapOperands` already asserts on, is the predicate that would work.

**Why that is not enough, and why this stopped.** Correcting both only gets as far as
`getOrCreateCounterIdAliasPair`, which looks in `counterVars` and `declRWSBuffers`. A
`ResourceDescriptorHeap` declaration is in neither, so **no counter exists for a heap-sourced
`RWStructuredBuffer` at all.** Making one exist means emitting a counter heap, which is what
`-fvk-bind-counter-heap` binds, and that defines a descriptor set and binding the engine must
then bind. That is the P4.3 bindless binding model decision, which
[Phase4-ShaderPipeline.md](Phases/Phase4-ShaderPipeline.md) calls the most consequential in the
phase and says to make **jointly with Phase 5**. It has not been made.

The reordering fix was written and then **reverted**, so it is not in the shipped patch. It
changes behaviour without fixing the shaders, which is unverified risk for no gain. Redoing it is
maybe ten minutes from this entry.

**Options, for whoever picks this up:**

1. Decide the binding model, then teach DXC to emit a counter heap. Largest, and it is the work
   Phase 5 needs anyway.
2. ~~Have DXC skip the association when the source is a heap access with no counter, instead of
   erroring.~~ **Tried on 2026-08-28, and it is wrong. Do not repeat it.** See below.
3. Restructure `AppendBuffer.esh` to keep the `RWStructuredBuffer` out of the struct. Forbidden by
   Phase 4 rule 4, changes Windows, and **fixes only three of the five** anyway, because the
   other two come out of the Reflector's code generator. Escalate before anyone tries it.

**Why option 2 is struck, in detail.** Skipping the association compiles all five shaders and
produces a module that **passes `spirv-val`** and is silently wrong. The counter resolves to
`OpUndef`:

```
%21 = OpUndef %_ptr_StorageBuffer_type_ACSBuffer_counter
%23 = OpAccessChain %_ptr_StorageBuffer_int %21 %uint_0
%24 = OpAtomicIAdd %int %23 %uint_1 %uint_0 %int_1        <- atomic on an undefined pointer
```

Compare the initialization path, which is correct, and addresses the counter heap by the same
index as the resource:

```
%22 = OpAccessChain %_ptr_StorageBuffer_int %counter_var_ResourceDescriptorHeap %uint_1 %uint_0
```

So "make assignment behave like initialization" is the wrong description of the fix.
Initialization does not skip anything: it resolves the counter properly. A green `spirv-val` is
not evidence here, which is worth remembering for the rest of this phase.

**What a correct fix needs, and why it is not small.** Two halves:

- *Source side.* `getFinalACSBufferCounter` must recognise the heap, using `isDescriptorHeap` and
  before the `AssocCounter#1` early return, as described above. This half is written, correct,
  and about fifteen lines.
- *Destination side.* The destination must have a counter to store into. It does not: a local
  `RWStructuredBuffer`, or a field of a local struct, gets no counter alias. With only the source
  half applied, the mismatch check still fails. This half is the work.

And for this engine the destination is the hard case. `AppendBuffer<T>` is a **struct containing a
structured buffer**, which DXC calls out as unsupported in its own source, at
`DeclResultIdMapper.cpp:1289`:

> Any kind of structured buffer has associated counters. The current DXC code is not written in a
> way to place associated counters inside a structure. Changing this behavior is non-trivial.
> There's also significant work to be done both in DXC (to properly generate binding numbers for
> the resource and its associated counters at correct offsets) and in spirv-opt (to flatten such
> structures and modify the binding numbers accordingly).

So unblocking these five shaders means implementing counters-inside-structures in DXC, alongside
the binding model decision. That is a real project, not a patch, and it is the point at which
[AGENTS.md](../../AGENTS.md) says to move the DXC work to a fork.

### 2026-08-28 - A Windows-only `.cpp` is excluded, never wrapped in `#ifdef _WIN32`

**Context:** `Code/EngineTools/Core/SystemDialogs.cpp` is 552 lines of COM `IFileDialog` with an
unguarded `<windows.h>`. The Phase 3 plan said to wrap the whole body in `#ifdef _WIN32`, the
"wrap, do not split" technique, and that is what the first Phase 3 commit did.

**Decision:** Name it in `Exclusions.txt` instead. The upstream file is byte-identical again.

**Rationale:** Both approaches produce the same object code: nothing. The difference is where
the knowledge lives.

- **Zero lines added beats two.** Every line in an upstream file is a line a future
  `git merge upstream/main` can conflict on. The wrap touches the first and last line of the
  file, which are exactly the lines an upstream reformat or a new include is most likely to
  move.
- **An exclusion is checked; an `#ifdef` is not.** `SourceLists.load` reports any pattern that
  matches nothing, so an upstream rename or delete fails the build with a named problem. A
  `#ifdef _WIN32` whose file upstream has replaced sits there silently forever.
- **It states the reason in the place a reviewer already reads.** `Exclusions.txt` is the one
  file a post-merge audit opens to see what the Linux build drops and why. A guard buried at
  line 1 of a 552-line file is not in that inventory.
- **The shared header was never the obstacle.** The original note claimed the file had to be
  wrapped "because the header is shared". `SystemDialogs.h` is untouched either way - it is
  `SystemDialogs_Linux.cpp` that includes it and defines every symbol it declares out of line.

**The rule:** a `.cpp` gets an exclusion. A header gets the wrap, because nothing lists headers,
so there is no way to exclude one. `ShaderReflection_ShaderCompiler.h` is the only file the wrap
still applies to, and Phase 4 has already replaced its whole-body wrap with targeted guards.

**Alternatives rejected:** keeping the wrap for symmetry with the header. Symmetry between a
`.cpp` and a `.h` is worth less than an upstream file with no diff at all.

### 2026-08-28 - `ctt` is open source, and is built from crates.io rather than substituted

Open question 1 is answered, and it is the best of the three outcomes
[Phase3-ResourceCompiler.md](Phases/Phase3-ResourceCompiler.md) lists: option 1, port it.

It did not look that way at first. `CTT.props` links `ctt_capi.lib` alongside `ws2_32`, `bcrypt`
and `ntdll`, upstream's `External.zip` contains only `ctt_capi.dll`, `ctt_capi.lib` and `ctt.h`,
and there is no source anywhere in the tree. That is the shape of a closed Windows binary, and
the plan document calls it "the least certain dependency in the whole port".

The `README.md` inside the zip says otherwise. `ctt` is a Rust library with C bindings, published
on crates.io as `ctt-c-api`, licensed MIT / Apache-2.0 / Zlib. It binds five open-source encoder
backends - bc7enc-rdo, Intel ISPC Texture Compressor, etcpak, AMD Compressonator and astcenc -
and it ships prebuilt ISPC static libraries for every supported platform, so a default build
needs only a Rust toolchain and a C++ compiler.

`DownloadDependencies.sh fetch_ctt` therefore downloads the `ctt-c-api` 0.5.0 crate tarball and
runs `cargo build --release`, producing `libctt_capi.so` in the layout `CTT.props` expects.

**Version pinning matters here.** 0.5.0 was published 2026-07-16 and the `ctt.h` in upstream's
zip is dated 2026-07-19, so it is the matching release. All 64 `ctt_*` and `CTT_*` identifiers
`ResourceCompiler_RenderTexture.cpp` uses are present in the 0.5.0 header. Check that again
before bumping the pin; a mismatch would be silent API drift rather than a clean error.

**Consequences:** none of the bad ones. This is the same library at the same version, not a
substituted compressor, so compressed texture bytes stay comparable with Windows and the
byte-identical criterion survives. The costs are a Rust toolchain of 1.90 or newer (the crate is
edition 2024; the check is in `fetch_ctt` because cargo's own failure does not mention the
version) and about four minutes of build time for the five C/C++ encoder backends.

### 2026-08-27 - DXC on Linux is blocked by clashing COM shims, not by DXC itself

**Context:** Eight `Engine/Render` files include `.esf` shader sources, which include generated
`.esh` reflection headers. Those come from the Reflector's `-shaders` pass, which Phase 2
disabled because it needs DXC. So Phase 3's "Engine builds" criterion depends on Phase 4 work.
The decision was to bring DXC forward rather than exclude the files or fake the headers.

**What worked.** DXC itself is easy. Microsoft ships official prebuilt Linux binaries, so
`./DownloadDependencies.sh dxc` is a download, not an hours-long LLVM build. It lands in
`External/DirectXShaderCompiler` with `inc/` and `lib/x64/`, matching what `DXC.props` expects.

`ShaderReflection_ShaderCompiler.h` also includes **`d3d12shader.h`**, which is a Windows SDK
header that the Linux DXC tarball does not ship. **DirectX-Headers**, Microsoft's cross-platform
D3D12 headers, supplies it. The plan never mentions this dependency and it is not optional.

Neither header is reachable through a property sheet: the Reflector imports only `Esoterica` and
`LLVM`, because on Windows both headers come from the Windows SDK and are on the default search
path. The Linux equivalent is to put them in the global include set, which is what
`ESOTERICA_INCLUDE_DIRECTORIES` now does. The DXC *library* stays on `DXC.props`.

**Where it stops.** DXC's `WinAdapter.h` and DirectX-Headers' `wsl/stubs` are **two alternative
COM shims for the same types**, and the shader compiler needs headers from both. Include them
together and `IUnknown` ends up declared but never defined, so every
`ID3D10Blob : public IUnknown` in `d3dcommon.h` fails with "base class has incomplete type".

Measured, so the next attempt does not repeat it:

| Include set | Errors |
|---|---|
| DXC + DirectX-Headers `directx/` only | 1 (`rpc.h` missing) |
| plus the trivial stubs, no `basetsd.h` | 1 |
| plus `basetsd.h`, `unknwnbase.h`, `oaidl.h` | 1 (`ocidl.h` missing) |
| plus `ocidl.h` | **20** |
| `dxcapi.h` included before `d3d12shader.h` | 17 |

`ocidl.h` is a five-line empty stub, so it is not the cause: adding it simply lets compilation
reach line 423 of `d3dcommon.h`, where the real problem is. Include order does not fix it either.

**Current state:** the four Phase 2 shader guards are **restored**, so the tree builds. The DXC
and DirectX-Headers dependencies, and the include-path plumbing, are kept, because any solution
needs them.

**What to try next**, in order:

1. Find the guard macro that makes DXC's `WinAdapter.h` and DirectX-Headers cooperate. Both are
   Microsoft projects and this combination must work somewhere; `__EMULATE_UUID` and the
   `__IUnknown_INTERFACE_DEFINED__` family are the places to look.
2. Use DXC's reflection interfaces (`IDxcContainerReflection`) instead of
   `ID3D12ShaderReflection`, dropping `d3d12shader.h` entirely. This is an upstream code change
   to `ShaderReflection_ShaderCompiler`, so **escalate first**.
3. Reconsider whether Linux shader reflection should go through SPIRV-Reflect, which Phase 5
   brings in for Vulkan anyway. That is a larger design change and definitely needs escalation.

### 2026-08-27 - Upstream merges happen on request only

**Context:** The plan called for merging `upstream/main` weekly, or before each new phase,
whichever came first, on the reasoning that merge cost grows faster than drift.

**Decision:** No cadence. Merge only when explicitly asked. `AGENTS.md` and
[01-UpstreamMerges.md](01-UpstreamMerges.md) both say so.

**Rationale:** The churn measurement taken during Phase 0 undercuts the original reasoning.
Upstream has 107 commits in total, and four in the last twelve months touched a source list. The
drift is cheap and stays cheap. What is not cheap is debugging a half-finished port and a merge
at the same time: when something breaks, there is no way to tell which change caused it. The
port is therefore built against **one fixed upstream commit** until it works end-to-end.

**Alternatives rejected:** Merging before each phase, which is the same trade at a slower rate.
Merging when `SyncUpstream.py` reports drift, which turns a useful signal into an interrupt.

**Note:** `upstream/main` is at `47e6293` (2026-08-26) and this fork is based on `6813cf9`. One
commit of drift, touching a `.vcxproj` source list by +2/-1. Recorded so nobody has to rediscover
it; **not** a reason to merge.

### 2026-08-27 - The build is the test. Check only what a green build would hide

**Context:** The first cut of the generator carried 520 lines of checks against 1441 lines of
generator, 27% of the Python. Most of it re-checked things the build proves anyway: that a rule
passes `-std=c++20`, that `Esoterica.Base` resolves to a `.so`, that link order is topological,
that ninja parses the file.

**Decision:** One `Code/Scripts/NinjaGen/Checks.py`, 192 lines, 20 checks. **Add a check only
when the failure would leave a green build behind.** Everything else is the compiler's job.

What that leaves:

| Check | Why the build cannot catch it |
|---|---|
| Sync drift detection | The whole safety mechanism of the three-list design. It fails silently until an upstream merge, months later. |
| Determinism of the ninja file | Building twice never reveals a non-deterministic build file. Acceptance criterion 6. |
| Stale exclusion globs | A glob that stopped matching silently readmits a file. |
| Glob semantics | A wrong glob silently includes or excludes sources, and the build may still succeed. |
| Property sheet coverage | An unmapped sheet surfaces much later as a link error naming nothing useful. |
| No `.vcxproj` modified | The prime directive. Nothing about a successful build says the project files are untouched. |
| `-ffast-math` never appears | It changes float behaviour without failing anything. |

**Rationale:** A compile error is louder, more specific and cheaper than any assertion that
duplicates it. Time spent asserting what the compiler already tells you is time not spent
getting Linux to build, which is the actual goal.

**Alternatives rejected:** Full coverage of the generator, which is what the first cut drifted
towards. No checks at all, which would drop the drift detection that makes the three-list design
safe in the first place.

### 2026-08-27 - Sanitizers on Debug and Release, never on Shipping

**Context:** The stale upstream generator built ASan, MSan and TSan variants of all three
configurations.
**Decision:** ASan, TSan and UBSan on Debug and Release. None on Shipping. MSan is dropped
entirely.
**Rationale:** Shipping is the `-flto` configuration. Instrumenting it measures a binary nobody
ships and takes far longer to build. MSan needs an instrumented libc++ to give usable output,
and without one it buries the reader in false positives from the standard library.
**Alternatives rejected:** Sanitizing every configuration, which triples the size of the ninja
file to describe builds nobody runs.

### 2026-08-27 - One compile rule per project and configuration

**Context:** Compiler flags vary by project and by configuration, and ninja has no scoping
between the two.
**Decision:** Emit a `cc_<Project>_<Config>` and `cxx_<Project>_<Config>` rule with the flags
baked into the command, so each build edge is a single line.
**Rationale:** The alternative is a per-edge variable, which adds an indented line to every one
of the 5300 edges and roughly doubles the file. 234 rules cost nothing to parse.
**Alternatives rejected:** One rule per configuration with per-edge flag variables.

### 2026-08-27 - Three source lists, not a source list derived from the `.vcxproj` files live

**Context:** The plan said the generator should read the `.vcxproj` files on every build, so that
upstream source changes need no Linux-side work. The first implementation did that, and needed
filename heuristics to decide what was Windows-only.

Those heuristics were wrong in both directions, silently:

- `Code/EngineTools/Core/SystemDialogs_Linux.cpp` would not have been found. The rule only
  searched directories that had lost a Windows file, and `SystemDialogs.cpp` was to be guarded
  rather than excluded. (Phase 3 excluded it instead, so this particular example no longer
  holds. The two below still do, and they were the decisive ones.)
- `Code/Base/Render/RHI_Direct3D12.cpp` was **in** the Linux build. It carries no platform
  suffix and no `#if _WIN32` guard, so nothing kept it out. Same for
  `Code/Base/ThirdParty/D3D12MemoryAllocator/`.
- `Code/Base/Render/RHI_Vulkan.cpp`, which Phase 5 adds, would not have been found either. It
  has no platform token in its name.

**The measurement that settled it.** Upstream has 107 commits in total: 63 in 2022, 28 in 2023,
4 in 2024, 1 in 2025, 11 in 2026. In the twelve months to 2026-08-27, four commits touched a
`.vcxproj` source list, and the churn was `+1/-0`, `+1/-0`, `+3/-3`, and `+265/-137`
(`5d02c90`, "Push Esoterica DX12").

**Decision:** The build reads three text files. `UpstreamProjects.txt` is generated by
`SyncUpstream.py` and never hand-edited. `Exclusions.txt` and `LinuxSources.txt` are
hand-maintained. Every build first runs `SyncUpstream.py` in check mode and stops if the
generated list is stale.

**Rationale:** Live parsing saved about five lines of list editing a year, and saved nothing on
`5d02c90`, because 265 new files need classifying by hand whatever the build system does. In
exchange it introduced a class of silent wrong answers. An explicit list cannot make that
mistake, and `Exclusions.txt` records *why* each file is dropped, which no filename convention
can. The check keeps the one real benefit of live parsing: a new upstream source stops the build
with a named error rather than joining it unnoticed.

**Alternatives rejected:** Live parsing with better heuristics; the heuristics are the problem,
not their quality. A one-time bootstrap with no ongoing check; that turns upstream drift back
into a silent failure, which is the thing worth preventing.

### 2026-08-27 - Path case is corrected once, at sync time

**Context:** Seven `.vcxproj` entries disagree with the case of the file on disk. MSBuild ignores
case. Linux does not.
**Decision:** `SyncUpstream.py` writes the on-disk spelling into `UpstreamProjects.txt`. The
build never sees the mismatch.
**Rationale:** Conventions rule 3 forbids fixing upstream files the task does not need, and rule
5 says to fix a vendored-code problem in the build generator. Doing it at sync time rather than
at build time means the correction is visible in a reviewed file instead of happening invisibly
on every run.
**Alternatives rejected:** Editing the `.vcxproj` files, which
[TouchedFiles.md](TouchedFiles.md) rules out. Resolving case on every build, which hides the
problem.

### 2026-08-27 - Headers are globbed, not listed

**Context:** The `REFLECT` rule needs a header dependency set. `<ClInclude>` entries would supply
it.
**Decision:** `SyncUpstream.py` ignores `<ClInclude>`. Phase 2 globs `**/*.h` per project
instead.
**Rationale:** A header list is pure maintenance with no decisions in it: nothing is ever
excluded from a dependency set for being Windows-only, because an unused header costs nothing.
Listing them would roughly double `UpstreamProjects.txt` and add a merge conflict surface for no
gain.

---

## Open questions

Carried from
[03-Dependencies.md](03-Dependencies.md#open-questions-to-resolve-during-implementation). Move a
question to "Decisions made" once you answer it.

| # | Question | Blocks | Status |
|---|---|---|---|
| 1 | ~~Does `ctt` (texture compression) build on Linux?~~ | Phase 3 | **answered: yes, it is open source. Built from crates.io.** |
| 2 | ~~Which LLVM version does the Reflector need, and does `clangAST` compile against it on Linux?~~ | Phase 2 | **answered in practice, closed 2026-09-02: clang 21.1.8, built from source into `External/LLVM`.** `clangAST` compiles against it and the Reflector has run every session since Phase 2. The version matters because clang's C++ AST API is not stable across major versions; see [04-BuildAndRun.md](04-BuildAndRun.md#the-toolchain). It was left open only because nobody closed it |
| 3 | ~~Use `volk`, or the plain Vulkan loader?~~ | Phase 5 | **answered: the plain loader.** Nothing is profiled yet, and nothing can be until the backend renders |
| 4 | ~~Do the target distros package SDL3, or must we always build it?~~ | Phase 6 | **answered 2026-08-29: always build it.** Ubuntu 24.04 LTS has no SDL3 package; it arrives from 25.04. `DownloadDependencies.sh` builds `release-3.4.14` from source |
| 5 | ~~Does `GameNetworkingSockets` block the first `Base` link?~~ | Phase 1 | **answered: yes, and at compile time, not link** |
| 6 | ~~Does the `VirtualAlloc` region in `Memory.cpp` have a working non-Windows path?~~ | Phase 1 | **answered: no** |
| 7 | ~~How do the engine's indirect draws reach Vulkan?~~ | Phase 5, and the whole frame | **answered 2026-08-29: the shader reads its own command's root data out of the argument buffer, indexed by `DrawIndex`.** Scheduled as P5.17, after Phase 6 bring-up. See the decision entry |
| 8 | ~~How does `Buffer<uint64_t>` reach Vulkan?~~ | Phase 6, and the whole frame | **answered 2026-08-31: it does not, so the shader reads `Buffer<uint2>` and packs.** The RHI already creates every one of these buffers as `RG32_UInt`, so no C++ changed. The engine now reaches its frame loop. See the decision entry |

Answered:

- **Is the autogenerated directory `_Autogenerated` or `_AutoGenerated`?** It is `_AutoGenerated`
  on disk, which is what `Reflect.nmake` and `.gitignore` use. `Esoterica.props` writes
  `_Autogenerated`. The generator globs case-insensitively, so both work.

- **How often does upstream actually change its source lists?** About five entries a year. 107
  commits in total: 63 in 2022, 28 in 2023, 4 in 2024, 1 in 2025, 11 in 2026. Four commits in
  the twelve months to 2026-08-27 touched a `.vcxproj` source list: `+1/-0`, `+1/-0`, `+3/-3`,
  and `+265/-137`. This settled the build system design; see "Decisions made" above.

---

## Upstream issues observed

Bugs and oddities in upstream code. **Do not fix them here** (Conventions rule 3). Record them,
and file them upstream as issues if they matter.

<!-- ### <file>:<line> - <description> -->

Noted during the first survey, in `Code/Scripts/NinjaGen/NinjaGen.py`. This port rewrites that
stale build script, so these get fixed as a side effect rather than as upstream fixes:

- It parses `Esoterica.sln`, which the repo no longer contains. The project moved to
  `Esoterica.slnx`.
- `cpp_rule` calls `toolchain.compiler_c` instead of `compiler_cpp`.
- `-fsanitize-address` is not a valid flag. It should be `-fsanitize=address`.
- It declares `-std=c++17`, but the project needs C++20.

Also noted, and not fixed:

- `Code/Applications/BuildGenerator/` does not work. It emits rule references with no rule
  definitions, and it parses the legacy `.sln` GUID format. Left alone on purpose.
- `Esoterica.slnx` references `Docs/docs/CodingGuidelines.md`, which the repository does not
  contain.

### `ImguiGizmo_Translate.cpp:76` - selecting an entity while looking down a world axis kills the editor

**The most reachable upstream bug this port has found.** Found by P8.4 on 2026-09-03 while
verifying mesh picking, and **deterministically reproducible**.

`TranslationGizmo::SetupManipulators` builds each axis's screen-space direction with

```cpp
( m_axes[axisIdx].m_axisEndSS - ctx.m_positionSS ).ToDirectionAndLength2( m_axes[axisIdx].m_axisDirSS, m_axes[axisIdx].m_axisLengthSS );
```

then `TryFlipAxes` passes two of those to `Math::CalculateAngleBetweenUnitVectors`, which asserts
that both are unit length. **When an axis points nearly at the camera its screen-space projection
is nearly zero long, `ToDirectionAndLength2` yields a zero direction, and the assert fires.**

Backtrace, from the editor launched under gdb:

```
#2  EE::Math::CalculateAngleBetweenUnitVectors           Code/Base/Math/MathUtils.h:115
#3  TranslationGizmo::SetupManipulators::$_1( 0, 1 )     ImguiGizmo_Translate.cpp:76
#4  TranslationGizmo::SetupManipulators                  ImguiGizmo_Translate.cpp:99
#5  GizmoBase::UpdateAndDraw                             ImguiGizmo_Base.cpp:45
#6  ImGuiX::Gizmo::UpdateAndDraw                         ImguiGizmo.cpp:229
#7  MapEditor::DrawViewportUI                            MapEditor.cpp:435
```

The failing pair is `axisIdx0 = 0`, `axisIdx1 = 1` - **X against Y**, not the vertical axis.

To reproduce, in the map editor on `pbrdemo`: hold right mouse in the viewport, hold `S` for about
three seconds to back the camera along its own forward axis, nudge the pitch down slightly, release,
then click any entity. The gizmo appears and the editor dies with
`Trace/breakpoint trap (core dumped)`.

**Why it matters:** this is the *default* gizmo mode, and "line the camera up with a world axis and
click something" is an ordinary thing to do in a map editor. `ImguiGizmo_Scale.cpp:71` has the same
call on the same kind of value, so the scale gizmo is presumably reachable the same way.

**Platform-neutral - `Code/Engine/Imgui/Gizmos/` and `Code/Base/Math/MathUtils.h` are untouched by
this port - so Windows has it too.** It only halts in a build with `EE_DEVELOPMENT_TOOLS`; with
asserts compiled out, `CalculateAngleBetweenUnitVectors` returns a meaningless angle and the gizmo
mis-flips its axes, which is cosmetic. Not fixed here, per Conventions rule 3 and Phase 8's "do
not" list.

### `GLTF.cpp:111` - the node scale assert is inverted, so glTF skeletal import always fails

Found by P8.2 while looking for a rigged asset. `Import::gltf::GetNodeTransform` reads:

```cpp
float scale = 1.0f;
if ( pNode->has_scale )
{
    // TODO: log warning
    EE_ASSERT( pNode->scale[0] != pNode->scale[1] || pNode->scale[1] != pNode->scale[2] );
    scale = pNode->scale[0];
}
```

It asserts that the scale is **non**-uniform. The condition is inverted - the surrounding code wants
a uniform scale, and takes `scale[0]` as if it had one. Any glTF whose skeleton nodes carry a scale
trips it.

Two related asserts sit on the same path and fire on the assets that get past the first:
`GLTF.cpp:435` (correctly requiring a uniform scale) and `Transform.h:71` (the same check inside the
`Transform` constructor).

Measured against the Khronos glTF sample assets, all four of which fail:

| Asset | Skeleton | Skeletal mesh | Animation |
|---|---|---|---|
| Fox | compiles | `Transform.h:71` | "Root scaling detected!" |
| RiggedFigure | compiles | `Transform.h:71` | `GLTF.cpp:435` |
| RiggedSimple | compiles | compiles | `GLTF.cpp:435` |
| CesiumMan | `GLTF.cpp:111` | `Transform.h:71` | `GLTF.cpp:111` |

**Platform-neutral, so Windows has it too, and the "TODO: log warning" says the author knew the
handling was unfinished.** Not fixed here, per Conventions rule 3. It is why
`FetchTestAssets.sh` uses FBX through ufbx rather than glTF.

### `UFbx::ReadAnimation` asserts on an animation whose last key sits on the stack end time

`huesitos.fbx`, from assimp's test models, imports its skeleton and skinned mesh fine and then fails
its animation on

```
time <= ( pAnimStack->time_end + Math::Epsilon )
```

A one-epsilon boundary condition in the ufbx import path. Platform-neutral. Found by P8.2, not
chased further, because `animation_with_skeleton.fbx` imports all three resources and was enough.

### `Server_WS::ConnectClient` asserts that the client is not already in the socket map

Seen once in four editor starts, on the Resource Server that the editor spawns for itself:

```
m_clientSocketMap.find( clientID ) == m_clientSocketMap.end()
```

`NetworkServer_WebSockets.cpp:143`. The receive callback calls `ConnectClient` from the `Open`
message, and again from the `Message` case when `HasConnectedClient` is false. `ConnectClient`
adds the client to the base class's list first and then asserts that the socket map does not
already hold it, so any second call for the same `clientID` fires the assert and takes the whole
Resource Server down with it - and the editor with that.

**The mechanism is not proved**, only the crash. Both call sites are reachable and the ordering
between them is not obviously serialized, but ixWebSocket delivers a connection's callbacks on
that connection's own thread, so the simple two-threads race is not the explanation. Whoever
chases this should start by logging the `clientID` and the calling path at both sites.

Platform-neutral upstream code, so Windows has it too. It is **intermittent**: three of four
starts on this machine were clean.

### `Network::Server` and `Network::Client` never drain `m_receivedMessages` at shutdown

**This is the Resource Server's "Memory leak detected" on exit, and it is not a Linux defect.**

`Server::m_receivedMessages` is a `TLockFreeQueue<Message*>`. The websocket receive callback runs
on ixWebSocket's per-connection threads and fills it with `EE::New<Message>`
(`NetworkServer_WebSockets.cpp:35`). It is drained in exactly one place: `Server::Update`, on the
main thread, which deletes each message as it handles it (`NetworkServer.cpp:24-37`).

**Nothing drains it at shutdown.** `~Server()` is `= default`, and `Server_WS::Stop` deletes the
`ix::WebSocketServer` without touching the queue. So every message that arrives after the last
`Update` and before `stop()` is allocated and never freed, and `rpmalloc_finalize` reports it:

```
Shutting down low level socket/threading support.
Memory leak detected (span->list_size == span->used_count) at Code/Base/ThirdParty/rpmalloc/rpmalloc.c:1424
```

**`Client` has the same shape** - same undrained queue, same `= default` destructor
(`NetworkClient.h:31`, `:81`) - so the engine and the editor can hit it too. They rarely do,
because a client's inbound traffic at shutdown is much thinner than a server's.

Both files are platform-neutral. **Windows has this too**, and has probably never noticed, because
it only fires when a message lands in that window.

#### Why it looks like a Linux defect, and how to tell it apart

- **It is intermittent.** Three runs in four on this machine, with **zero external clients**: the
  three compiler workers are themselves websocket clients and heartbeat continuously, so the
  server always has traffic to catch.
- **It does not reproduce under `gdb`**, which is the tell. The debugger changes the timing enough
  that the last `Update` drains the queue.
- **The engine shuts down clean**, which made it look Resource-Server-specific. It is not; it is
  `Server` versus `Client` traffic volume.

Do not go looking for a missing `ShutdownThreadHeap`. `Memory::InitializeThreadHeap` is called on
these threads and deliberately never finalized - the comment in `Memory.cpp:68` says so, and
relies on `rpmalloc_finalize` to release the heaps. That is fine. The leak is a real one.

### `ResourceServerContext::Initialize` leaks its `CompilerRegistry` on every failure path

Found in P7.3, and true on both platforms by inspection. `Initialize` allocates
`m_pCompilerRegistry` with `EE::New<CompilerRegistry>`, then returns false if the network server
cannot bind, if the compiled resource DB will not connect, or on any later step. Nothing deletes
it, so `~ResourceServerContext` asserts on `m_pCompilerRegistry == nullptr`. Windows never sees
it because the single-instance mutex in `_tWinMain` stops a second server reaching the bind.

### `ResourceServerApplication::Shutdown` asserts that the application was initialized

Same shape as the `Engine::Shutdown` entry below, and the same on both platforms.
`Win32Application::Run` and `LinuxApplication::Run` both call `Shutdown()` when `Initialize()`
returns false, before setting `m_initialized`. `Shutdown` opens with `EE_ASSERT( WasInitialized() )`,
so a failed start asserts instead of reporting the real error. The Linux sibling returns early
instead of asserting; the upstream file is untouched.

### `Engine::Shutdown` crashes when `Engine::Initialize` failed

Found during the P6.7 bring-up, and true on both platforms by inspection. A failed `Initialize`
leaves `RenderSystem` unconstructed, and `Shutdown` calls `RenderSystem::WaitAllQueuesIdle`
anyway, which dereferences a null queue. It only shows on a failed start, so it hides the real
error behind a segfault. `Engine::m_initializationStageReached` already records how far the start
got, and `Shutdown` could tear down only what that stage covers.

### `RHI.h:1044` - `LoadAction` defaults to discarding every attachment

`LoadAction` is zero initialised, `LoadActionType::DontCare` is zero and `StoreActionType::DontCare`
is zero, so every action a caller does not set says "discard". That is harmless on Direct3D 12,
which has no load or store actions and preserves a bound render target either way, and it is not
harmless on any backend that honours them.

**No engine pass sets a store action at all**, and `RenderPass_DebugDraw.cpp:1316` builds a
`LoadAction` that sets only the depth action and then binds the frame's final colour target with
it at `:1358`. On a backend that honours the values, the first discards every render pass output
in the frame and the second discards the rendered frame.

The Vulkan backend maps both `DontCare` values to preserve, and leaves `Clear`, `Load` and
`StoreActionType::None` exact. A caller that really wants an attachment left alone still has
`StoreActionType::None`. Worth raising upstream: the fix there is either a non-discarding default
or explicit actions at each call site.

### `RHI_Direct3D12.cpp:3981`, `:3978` and `:3969` - three faults in the raytracing path

None has ever run: nothing in the engine creates an acceleration structure.

- **`:3981`** has the line that fills in `Direct3D12AccelerationStructure::m_instanceBuffer`
  commented out, and `:3390` dereferences it during the top level build. That is a null pointer.
- **`:3978`** creates the top level structure buffer with `BufferFlags::NoDescriptors` and
  descriptor types `RWBuffer|Raw`, and `GetAccelerationStructureHandle` at `:4002` then asks it for
  a `DescriptorTypeFlags::Buffer` handle. Two asserts fire: one for the missing descriptor type and
  one for the missing handle.
- **`:3969`** sizes the scratch buffer from the bottom level prebuild alone and then reuses it for
  the top level build at `:3392`. It overruns whenever the top level needs more scratch, which is
  common.

The Vulkan backend does not reproduce any of the three. Each is written up at the line in
`RHI_Vulkan.cpp`.

### `RHI_Direct3D12.cpp:3490` - `CmdBeginQuery` calls `BeginQuery` on a timestamp query

`ID3D12GraphicsCommandList::BeginQuery` does not support `D3D12_QUERY_TYPE_TIMESTAMP`; a timestamp
is written by `EndQuery` alone. The reference switches on exactly that type and calls `BeginQuery`
for it, which the debug layer rejects.

Nothing in the engine calls `CmdBeginQuery`, so it has never run. The Vulkan backend does nothing
for a timestamp begin, which is what the reference effectively achieves minus the complaint.

### `RHI_Direct3D12.cpp:3714` - `CmdWriteDebugMarker` packs its auto flags two different ways

The `InOut` branch builds its flag from the enum's ordinal, `UINT( MarkerTypeFlags::In ) << 30`,
which is `1 << 30`. The single-marker branch builds it from the bit field, `markerType << 30`,
and `TBitFlags` converts to `1 << flagIndex`, so the same `In` becomes `2 << 30`. One of the two
is wrong and they cannot both be right.

Nothing in the engine calls `CmdWriteDebugMarker` and `DeviceCapabilities::m_breadcrumbs` is
`false` on both backends, so it costs nothing today. The Vulkan backend reproduces both branches
exactly, so the two write identical bytes; fixing it belongs upstream, next to a decision about
which one was meant.

### `RHI_Direct3D12.cpp:284` - `RGB565_UNorm` and `BGR565_UNorm` map to the same DXGI format

Both return `DXGI_FORMAT_B5G6R5_UNORM`. Under DXGI's naming, which lists a packed format's
components least significant first, that is the `BGR565` one; `RGB565` has no DXGI format at all.
Vulkan has both, so the Vulkan backend could tell them apart and chooses not to: mapping them
faithfully would make the two backends draw the same asset differently. Nothing in the engine uses
either format, so this costs nothing today. Recorded because a future reader will see two
`DataFormat` members reaching one `VkFormat` and assume it is a copy-paste slip.

### Eleven functions are `inline` on one side of the declaration only

Eight headers declare a member `inline` whose only definition is out of line in the matching
`.cpp` (`ResourceRecord.h`, `InputSystem.h`, `ImguiX.h`, `AnimationSkeleton.h`,
`Animation_RuntimeGraph_Instance.h` twice, `Animation_RuntimeGraphNode_Blend1D.h`,
`ResourceCompilerContext.h`). `MathUtils.cpp` has the mirror image: three `ToString` definitions
marked `inline` where the header declares them `EE_BASE_API` without it.

Both are ill-formed. An inline function has to be defined in every translation unit that uses it,
and clang emits nothing for a definition no other TU can see. MSVC emits them anyway because
`__declspec( dllexport )` forces it, so the bug is invisible on Windows. Fixed here by dropping
`inline`, which is correct on both compilers. Worth an upstream PR: it is a one-word change per
site and it costs Windows nothing.

### `va_list` is read twice in five places

`CompileContext::LogError`, `LogWarning` and `LogMessage` each `va_start` once and then hand the
same `va_list` to two consumers - `SystemLog::AddEntryVarArgs` and `m_log.Log*`. `ImguiX.h`'s two
`DrawShadowedText` overloads do the same to draw the shadow and then the text.

On MSVC x64 a `va_list` is a pointer passed by value, so the callee's advance does not disturb the
caller's copy and the second read happens to work. In the System V ABI it is a one-element array
that decays to a pointer, the callee advances the *caller's* state, and the second read walks off
the end of the argument area. This segfaulted every standalone resource compile, immediately after
the resource had compiled successfully. Fixed here with `va_copy`. This one is a genuine latent
bug on Windows too, not merely a portability difference, and is the better upstream PR of the two.

### The Reflector hardcodes Windows path separators in five places

`ReflectorSettings.h` spells `g_codeFolderPath`, `g_buildFolderPath`, `g_buildTempFolderPath`,
`g_runtimeEngineProjectPath` and `g_toolsEngineProjectPath` with backslashes, and
`ClangParser.cpp`'s `g_includePaths` array does the same. `Reflector.cpp` also appends `.vcxproj`
paths verbatim. On Linux a backslash is an ordinary filename character, so this created a
directory literally named `Build\_Temp\` in the repository root and found no headers at all.

Two of the `g_includePaths` entries also have the wrong case: `EABase\include\common` against
`EABase/include/Common`, and `EASTL\include` against `EASTL/Include`.

### The Reflector's builtin type table assumes LLP64

`ClangUtils.cpp` maps `clang::BuiltinType::ULongLong` to `uint64_t` and has no case for `ULong`.
That is correct on Windows, where `uint64_t` is `unsigned long long`. Linux is LP64, so
`uint64_t` is `unsigned long`, and every 64-bit property failed with
"Cannot resolve property typename (uint64_t)".

### `FileSystem.h` has an inline that never returns

`UpdateBinaryFile` at line 97 forwards to its overload and drops the result:

```cpp
EE_FORCE_INLINE bool UpdateBinaryFile( char const* pFilePath, Blob const& fileData, bool* pWasFileUpdated = nullptr )
{ UpdateBinaryFile( pFilePath, fileData.data(), fileData.size(), pWasFileUpdated ); }
```

Falling off the end of a non-void function is undefined behaviour. Every caller reads a garbage
return value. Not fixed here (Conventions rule 3), and worth reporting upstream: this one is a
real bug on Windows too.

### `Math_Win32.h` truncates `GetMostSignificantBit` above 2^32

`Code/Base/Math/Platform/Math_Win32.h` casts its argument to `unsigned long` before the scan:

```cpp
_BitScanReverse64( &index, (unsigned long) value );
```

`unsigned long` is 32 bits on Windows, so every value above `2^32` gives the wrong answer.
`Math_Linux.h` uses `__builtin_clzll` and is correct for the full 64-bit range. **The two
platforms therefore disagree**, which is worse than either bug alone, so it is commented in the
Linux header as well as recorded here. Not fixed on the Win32 side (Conventions rule 3).

### `SystemLog_Win32.cpp` drops newlines on medium-length messages

`TraceMessage` bounds its newline append at `numCharsWritten < 509` while the buffer is 2048
bytes, so messages between 509 and 2045 characters silently lose their newline.
`SystemLog_Linux.cpp` bounds it at the real buffer size. Commented in the Linux file.

### `IniFile.cpp` puts its whole body inside `#if defined(_MSC_VER)`

The guard opens at line 4 and its matching `#endif` is the **last line of the file**. On any
non-MSVC compiler the translation unit produces nothing at all: it compiles cleanly and leaves
`IniFile::Load`, `Save`, `GetString` and `SetString` undefined until something tries to link an
executable. This is the single most expensive bug found so far, because there is no compile
error to point at.

### Two `#include` directives use a backslash

`Code/Base/Utils/GlobalRegistryBase.h` and
`Code/Base/Input/InputDevices/InputDevice_Controller.cpp`. clang does not treat `\` as a path
separator inside an include, so neither header was found on Linux. MSVC accepts `/`, so fixing
them costs Windows nothing.

### Path case mismatches between the `.vcxproj` files and the disk

Found on 2026-08-27. MSBuild ignores case, so Windows builds fine. On Linux the file is simply
not found. `SyncUpstream.py` writes the on-disk spelling into `UpstreamProjects.txt` and warns.
**Not fixed in the `.vcxproj` files** (Conventions rule 3, TouchedFiles.md).

| Project | Listed | On disk | Affects the build |
|---|---|---|---|
| `Esoterica.Base` | `ThirdParty/enkits/TaskScheduler.cpp` | `ThirdParty/EnkiTS/TaskScheduler.cpp` | yes |
| `Esoterica.Engine.Runtime` | `Navmesh/NavPower.cpp` | `Navmesh/Navpower.cpp` | yes |
| `Esoterica.Base` | `ThirdParty/enkits/TaskScheduler.h` | `ThirdParty/EnkiTS/TaskScheduler.h` | no, header |
| `Esoterica.Base` | `ThirdParty/enkits/TaskScheduler_Esoterica.h` | `ThirdParty/EnkiTS/TaskScheduler_Esoterica.h` | no, header |
| `Esoterica.Base` | `ThirdParty/enkits/LockLessMultiReadPipe.h` | `ThirdParty/EnkiTS/LockLessMultiReadPipe.h` | no, header |
| `Esoterica.Applications.Reflector` | `Resources/Resource.h` | `Resources/resource.h` | no, header |
| `Esoterica.Applications.BuildGenerator` | `Resources/Resource.h` | `Resources/resource.h` | no, header |

The header rows no longer reach the build, because `SyncUpstream.py` ignores `<ClInclude>`. They
are recorded so nobody investigates them twice.

### `RHI_Direct3D12.cpp` has no platform guard

`Code/Base/Render/RHI_Direct3D12.cpp` is 6084 lines of Direct3D 12 with no `#if _WIN32` at the
top and no platform suffix in its name. The same is true of the vendored
`Code/Base/ThirdParty/D3D12MemoryAllocator/`. Nothing about the file marks it as Windows-only,
so nothing but an explicit entry in `Exclusions.txt` keeps it out of a Linux build.

This is fine for upstream, which is Windows-only. Recorded because it is the single clearest
argument against deciding what to build from filenames.

The case mismatches are worth reporting upstream. They cost nothing on Windows, so upstream will
not notice them on its own.

---

## Merge notes

See [01-UpstreamMerges.md](01-UpstreamMerges.md#merge-notes) for the sync-point table. Record
hard conflict resolutions there, not here.
