# Upstream Issues

Bugs and oddities found in upstream Esoterica code. **This is the only place they are recorded.**

Most of these are not Linux problems. They are platform-neutral defects that the port happened to
walk into, and Windows has them too. They are written down because a future session will hit the
same wall, and because upstream accepts bug reports.

**The default is: do not fix them here.** [Conventions rule 3](00-Conventions.md#rule-3---change-nothing-you-were-not-asked-to-change)
says record an upstream bug and move on, because every unnecessary edit to an upstream file is a
merge conflict someone pays for later. A few are fixed anyway, either because the port cannot run
without the fix or because a human asked for it. Each of those says so, and each has a row in
[TouchedFiles.md](TouchedFiles.md).

Status values:

| Status | Meaning |
|---|---|
| `not fixed` | Recorded only. The fork still contains the bug. |
| `fixed here` | The fork edits the upstream file. There is a `TouchedFiles.md` row for it. |
| `worked around` | The upstream file is untouched; the Linux side avoids or absorbs the problem. |
| `fixed as a side effect` | In a file this port rewrites anyway, so fixing it costs no merge debt. |

---

## Index

| # | Where | In one line | Status |
|---|---|---|---|
| 1 | `ImguiGizmo_Translate.cpp:76` | Clicking an entity while looking down a world axis kills the editor | `not fixed` |
| 2 | `GLTF.cpp:111` | The node scale assert is inverted, so a uniformly scaled glTF cannot be imported | `fixed here` |
| 3 | `RawFileInspector.cpp:154` | A glTF mesh with no name kills the editor when you select the file | `fixed here` |
| 4 | `ResourceCompiler_RenderMesh.cpp:327` | Auto-generated LODs index past the end of the geometry list on a multi-part mesh | `fixed here` |
| 5 | `GLTF.cpp:18` | A missing comma leaves the glTF error table one entry short | `not fixed` |
| 6 | `RawFileInspector.cpp:178`, `:190` | glTF skeleton and animation names are optional too, and unnamed ones still assert | `not fixed` |
| 7 | `UFbx::ReadAnimation` | An animation whose last key sits exactly on the stack end time asserts | `not fixed` |
| 8 | `Server_WS::ConnectClient` | Connecting a client twice takes the Resource Server down | `not fixed` |
| 9 | `NetworkServer.cpp`, `NetworkClient.h` | Neither side drains its received-message queue at shutdown, so exit reports a leak | `not fixed` |
| 10 | `ResourceServerContext::Initialize` | Leaks its `CompilerRegistry` on every failure path | `not fixed` |
| 11 | `ResourceServerApplication::Shutdown` | Asserts that the app was initialized, so a failed start hides the real error | `worked around` |
| 12 | `Engine::Shutdown` | Crashes when `Engine::Initialize` failed, hiding the real error | `not fixed` |
| 13 | `RHI.h:1044` | `LoadAction` defaults to discarding every attachment | `worked around` |
| 14 | `RHI_Direct3D12.cpp:3981`, `:3978`, `:3969` | Three faults in the raytracing path, none of which has ever run | `not fixed` |
| 15 | `RHI_Direct3D12.cpp:3490` | `CmdBeginQuery` calls `BeginQuery` on a timestamp query | `not fixed` |
| 16 | `RHI_Direct3D12.cpp:3714` | `CmdWriteDebugMarker` packs its auto flags two different ways | `not fixed` |
| 17 | `RHI_Direct3D12.cpp:284` | `RGB565_UNorm` and `BGR565_UNorm` map to the same DXGI format | `not fixed` |
| 18 | Eleven functions across nine files | `inline` on one side of the declaration only, which is ill-formed | `fixed here` |
| 19 | Five functions across two files | A `va_list` is read twice, which segfaults on the System V ABI | `fixed here` |
| 20 | `FileSystem.h:97` | An inline that never returns, so every caller reads garbage | `not fixed` |
| 21 | `Math_Win32.h` | `GetMostSignificantBit` truncates above 2^32, so the platforms disagree | `not fixed` |
| 22 | `SystemLog_Win32.cpp` | Drops newlines on messages between 509 and 2045 characters | `not fixed` |
| 23 | `IniFile.cpp` | The whole file body sits inside `#if defined(_MSC_VER)` | `fixed here` |
| 24 | Two `#include` directives | Use a backslash, which clang does not treat as a separator | `fixed here` |
| 25 | The Reflector, five constants plus an array | Hardcoded Windows path separators, and two wrong-case paths | `fixed here` |
| 26 | `ClangUtils.cpp` | The builtin type table assumes LLP64, so no 64-bit property resolved | `fixed here` |
| 27 | `.vcxproj` files, seven entries | Path case does not match the disk | `not fixed` |
| 28 | `NinjaGen.py`, four defects | The stale build script does not run at all | `fixed as a side effect` |
| 29 | `Code/Applications/BuildGenerator/` | Does not work, and parses a solution format the repo no longer uses | `not fixed` |
| 30 | `RHI_Direct3D12.cpp` | 6084 lines of Direct3D 12 with no platform guard and no platform suffix | `not fixed` |

---

## The editor

### 1. `ImguiGizmo_Translate.cpp:76` - clicking an entity while looking down a world axis kills the editor

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
mis-flips its axes, which is cosmetic.

**What was done:** nothing. Recorded per Conventions rule 3 and Phase 8's "do not" list.

---

## glTF import

Items 2, 3 and 4 are one story. **Importing the Khronos Sponza sample hits all three in sequence**,
and each one kills a process outright. They were found and fixed together on 2026-09-04, after a
human asked for the fixes rather than another record-and-move-on entry.

The reason none of them showed up sooner: **the only glTF asset in the repository,
`Data/Editor/MaterialBall/MaterialBall.gltf`, has named meshes, named nodes, and no `scale` key at
all.** It dodges items 2 and 3 by construction, and it has a single geometry so it cannot reach
item 4. One sample asset is not a test suite.

### 2. `GLTF.cpp:111` - the node scale assert is inverted, so a uniformly scaled glTF cannot be imported

Originally found by P8.2 while looking for a rigged asset. `Import::gltf::GetNodeTransform` read:

```cpp
float scale = 1.0f;
if ( pNode->has_scale )
{
    // TODO: log warning
    EE_ASSERT( pNode->scale[0] != pNode->scale[1] || pNode->scale[1] != pNode->scale[2] );
    scale = pNode->scale[0];
}
```

**In plain terms: it asserted that the scale is *non*-uniform, which is backwards.** The line below
takes `scale[0]` on its own, so uniform scale is the only kind the code can represent, and a uniform
scale is exactly what the assert rejected. The same check written the right way round is a few
hundred lines further down at `GLTF.cpp:435`, in the animation path.

Sponza's root node is `scale: [0.008, 0.008, 0.008]`, so every attempt to read it hit this.

Two related asserts sit on the same path and fire on the assets that get past this one:
`GLTF.cpp:435` (correctly requiring a uniform scale) and `Transform.h:71` (the same check inside the
`Transform` constructor).

Measured against the Khronos glTF sample assets before the fix, all four of which failed:

| Asset | Skeleton | Skeletal mesh | Animation |
|---|---|---|---|
| Fox | compiles | `Transform.h:71` | "Root scaling detected!" |
| RiggedFigure | compiles | `Transform.h:71` | `GLTF.cpp:435` |
| RiggedSimple | compiles | compiles | `GLTF.cpp:435` |
| CesiumMan | `GLTF.cpp:111` | `Transform.h:71` | `GLTF.cpp:111` |

**Platform-neutral, so Windows has it too, and the "TODO: log warning" says the author knew the
handling was unfinished.** It is why `FetchTestAssets.sh` uses FBX through ufbx rather than glTF.

**What was done:** the comparison is flipped to `==` / `&&`, so it now matches the working sibling at
`GLTF.cpp:435` and accepts a uniform scale. **3 added, 1 removed** - one line changed, two of
comment. The assert
still fires on a genuinely non-uniform scale, which is correct: `Transform` cannot hold one. The
rows above for `Transform.h:71` and `GLTF.cpp:435` are untouched, so the skeletal and animation
paths still fail where the table says they do.

### 3. `RawFileInspector.cpp:154` - a glTF mesh with no name kills the editor when you select the file

Found on 2026-09-04, importing Sponza. `InspectGLTF` named every importable mesh with

```cpp
pImportableItem->m_nameID = StringID( mesh.name );
```

**Mesh names are optional in glTF.** `StringID( nullptr )` is safe and yields an invalid ID, so
nothing fails at that line. It fails three frames later:
`ImportableItem::IsValid()` requires a valid `m_nameID`, and
`ResourceImporterEditorTool::UpdateSelectedFile` asserts on it right after inspecting the file:

```cpp
// EditorTool_ResourceImporter.cpp:744
EE_ASSERT( pImportableItem != nullptr && pImportableItem->IsValid() );
```

Sponza was exported by glTF-Transform, which strips names. Its one mesh has no name, so all 103 of
its importable items came back invalid and **the editor died the moment the file was selected in the
Resource Importer** - before any import was even requested. `EE_DEBUG_BREAK` is
`__builtin_debugtrap()` on Linux, so this is a `SIGTRAP` and the process is simply gone. Asserts are
live in Release as well as Debug; only Shipping compiles them out.

Note that `InspectFBX` names its items after the *node* rather than the mesh, and FBX nodes are
named in practice, which is why the FBX path never showed this.

**What was done:** when `mesh.name` is absent or empty, the item is named `Mesh_<index>` from the
mesh's index in the file. **15 added, 1 removed.** A named mesh keeps exactly the name it had
before, so no existing asset changes. The synthesized name does not need to round-trip through the
mesh filter in `gltf::ReadStaticMesh`: that filter skips the name check entirely for a mesh whose
name is null (`GLTF.cpp:824`), so an unnamed mesh is always imported whole.

### 4. `ResourceCompiler_RenderMesh.cpp:327` - auto-generated LODs index past the end of the geometry list

Found on 2026-09-04, compiling Sponza against `Highpoly.meshgrp`.

`MeshCompiler::CompileMesh` builds each LOD by walking the source geometries and appending the ones
that produced usable output, then walks the submeshes to point each at its geometry. **The second
walk assumed the first one appended exactly one geometry per submesh, in order.** It tracked
position with a running counter:

```cpp
Geometry const& geometry = mesh.m_geometry[lodGeometryBaseIndex + geometryIndex];
```

Three things break that assumption, and all three are already in the code: an empty geometry is
skipped, a geometry whose clusters come out empty is skipped, and - the one that bites - **on any
LOD past the first, a geometry is only appended when simplification succeeds.** The
`if ( simplificationSuccess || &lod == meshGroup.m_lodSettings.begin() )` at line 278 has no `else`.

Simplification fails on small geometry, which a 103-part mesh has plenty of. For Sponza, LOD 1
appended 64 of the 103, so on the 65th submesh the index reached exactly one past the end:

```
eastl::vector<EE::Render::Geometry>::operator[] (n=167)   EASTL/vector.h:899
EE::Render::MeshCompiler::CompileMesh                     ResourceCompiler_RenderMesh.cpp:327
EE::Render::StaticMeshCompiler::Compile                   ResourceCompiler_RenderMesh.cpp:487
```

The same running counter was also used to index `convertedMesh.m_geometryBuilders`, which is indexed
by *source* geometry. That is wrong for the same reason, and separately wrong because a submesh
already carries the geometry it refers to in `Import::Mesh::Submesh::m_geometryIdx`, and two
submeshes may share one geometry.

**Platform-neutral. It needs a mesh with several geometries and a mesh group that auto-generates
LODs**, which is why nothing in `Data/` reaches it: `MaterialBall` and `SkyDome` have one geometry
each, and `Boulder` has one too.

**What was done:** the append loop now records where each source geometry landed in a
`TVector<int32_t> lodGeometryIndices`, holding `InvalidIndex` for one it did not emit. The submesh
loop looks up `importedSubmesh.m_geometryIdx` in that table and skips the submesh when its geometry
is absent from this LOD. **19 added, 12 removed** - the running counter and the now-unused
`lodGeometryBaseIndex` are both gone.

The four existing mesh descriptors in `Data/` were recompiled before and after and are
**byte-identical**, so the change corrects the broken case without moving the working one:

```
b81598589ca64ffaed5cdfee082c5d58  boulder.mesh
70980e449a43b9e5d226cb17ee99e467  floor.mesh
818ae62f4c15a77a95282c03ec9d97e1  materialball.mesh
be54832e8c22080c66d666f90ec2f53c  skydome.mesh
```

### 5. `GLTF.cpp:18` - a missing comma leaves the glTF error table one entry short

Noticed on 2026-09-04 while reading the file for item 2. `g_errorStrings` opens:

```cpp
static char const* const g_errorStrings[] =
{
    ""
    "data_too_short",
    ...
```

There is no comma after `""`. C++ concatenates adjacent string literals, so those two lines make a
**single** element and the array holds 9 strings for the 10 values of `cgltf_result`. Every element
is shifted by one, so `g_errorStrings[parseResult]` names the wrong error for every parse failure,
and `cgltf_result_legacy_gltf` (9) reads one past the end of the array.

Only reachable on a failed parse, which is why it has never been noticed. Platform-neutral.

**What was done:** nothing. It is a one-character fix, but it is unrelated to the crash that was
being fixed, and Conventions rule 3 is explicit about riders. Good candidate for an upstream report.

### 6. `RawFileInspector.cpp:178` and `:190` - glTF skeleton and animation names are optional too

The same defect as item 3, in the two loops below it. `InspectGLTF` names skeleton items from
`pSceneData->skins[i].joints[0]->name` and animation items from `pSceneData->animations[i].name`.
Neither is required to exist by the glTF specification, and an absent one produces an invalid
`m_nameID` and the same fatal assert at `EditorTool_ResourceImporter.cpp:744`.

Not reproduced: Sponza has no skins and no animations, so there was nothing to test a fix against.

**What was done:** nothing, deliberately. Item 3's fix covers the mesh loop only. Synthesizing a
name here would be worse than it looks for animations: the name is what
`gltf::ReadAnimation` matches against to *find* the animation, and a synthesized one matches
nothing, so it would trade a crash for a failed import. Fixing it properly means giving the reader
a way to select an animation by index. Left for whoever has an unnamed-animation asset to test with.

---

## FBX import

### 7. `UFbx::ReadAnimation` asserts on an animation whose last key sits on the stack end time

`huesitos.fbx`, from assimp's test models, imports its skeleton and skinned mesh fine and then fails
its animation on

```
time <= ( pAnimStack->time_end + Math::Epsilon )
```

A one-epsilon boundary condition in the ufbx import path. Platform-neutral.

**What was done:** nothing. Found by P8.2 and not chased further, because
`animation_with_skeleton.fbx` imports all three resources and was enough.

---

## Networking and the Resource Server

### 8. `Server_WS::ConnectClient` asserts that the client is not already in the socket map

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

**What was done:** nothing.

### 9. `Network::Server` and `Network::Client` never drain `m_receivedMessages` at shutdown

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

**What was done:** nothing.

> **The same message from a different cause.** `rpmalloc` prints that line for *any* outstanding
> allocation at exit, so it is not proof of this bug. A tool or harness that allocates and does not
> free will print it too. Check what leaked before assuming this entry explains it.

### 10. `ResourceServerContext::Initialize` leaks its `CompilerRegistry` on every failure path

Found in P7.3, and true on both platforms by inspection. `Initialize` allocates
`m_pCompilerRegistry` with `EE::New<CompilerRegistry>`, then returns false if the network server
cannot bind, if the compiled resource DB will not connect, or on any later step. Nothing deletes
it, so `~ResourceServerContext` asserts on `m_pCompilerRegistry == nullptr`. Windows never sees
it because the single-instance mutex in `_tWinMain` stops a second server reaching the bind.

**What was done:** nothing.

### 11. `ResourceServerApplication::Shutdown` asserts that the application was initialized

Same shape as item 12, and the same on both platforms. `Win32Application::Run` and
`LinuxApplication::Run` both call `Shutdown()` when `Initialize()` returns false, before setting
`m_initialized`. `Shutdown` opens with `EE_ASSERT( WasInitialized() )`, so a failed start asserts
instead of reporting the real error.

**What was done:** the Linux sibling returns early instead of asserting. The upstream file is
untouched.

### 12. `Engine::Shutdown` crashes when `Engine::Initialize` failed

Found during the P6.7 bring-up, and true on both platforms by inspection. A failed `Initialize`
leaves `RenderSystem` unconstructed, and `Shutdown` calls `RenderSystem::WaitAllQueuesIdle`
anyway, which dereferences a null queue. It only shows on a failed start, so it hides the real
error behind a segfault. `Engine::m_initializationStageReached` already records how far the start
got, and `Shutdown` could tear down only what that stage covers.

**What was done:** nothing.

---

## RHI and Direct3D 12

None of the Direct3D 12 items below can be tested from this fork - the backend does not build on
Linux. They were all found by writing the Vulkan backend against the same interface and comparing.

### 13. `RHI.h:1044` - `LoadAction` defaults to discarding every attachment

`LoadAction` is zero initialised, `LoadActionType::DontCare` is zero and `StoreActionType::DontCare`
is zero, so every action a caller does not set says "discard". That is harmless on Direct3D 12,
which has no load or store actions and preserves a bound render target either way, and it is not
harmless on any backend that honours them.

**No engine pass sets a store action at all**, and `RenderPass_DebugDraw.cpp:1316` builds a
`LoadAction` that sets only the depth action and then binds the frame's final colour target with
it at `:1358`. On a backend that honours the values, the first discards every render pass output
in the frame and the second discards the rendered frame.

**What was done:** the Vulkan backend maps both `DontCare` values to preserve, and leaves `Clear`,
`Load` and `StoreActionType::None` exact. A caller that really wants an attachment left alone still
has `StoreActionType::None`. Worth raising upstream: the fix there is either a non-discarding
default or explicit actions at each call site.

### 14. `RHI_Direct3D12.cpp:3981`, `:3978` and `:3969` - three faults in the raytracing path

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

**What was done:** nothing to the Direct3D 12 file. The Vulkan backend does not reproduce any of the
three, and each is written up at the corresponding line in `RHI_Vulkan.cpp`.

### 15. `RHI_Direct3D12.cpp:3490` - `CmdBeginQuery` calls `BeginQuery` on a timestamp query

`ID3D12GraphicsCommandList::BeginQuery` does not support `D3D12_QUERY_TYPE_TIMESTAMP`; a timestamp
is written by `EndQuery` alone. The reference switches on exactly that type and calls `BeginQuery`
for it, which the debug layer rejects.

Nothing in the engine calls `CmdBeginQuery`, so it has never run.

**What was done:** nothing. The Vulkan backend does nothing for a timestamp begin, which is what
the reference effectively achieves minus the complaint.

### 16. `RHI_Direct3D12.cpp:3714` - `CmdWriteDebugMarker` packs its auto flags two different ways

The `InOut` branch builds its flag from the enum's ordinal, `UINT( MarkerTypeFlags::In ) << 30`,
which is `1 << 30`. The single-marker branch builds it from the bit field, `markerType << 30`,
and `TBitFlags` converts to `1 << flagIndex`, so the same `In` becomes `2 << 30`. One of the two
is wrong and they cannot both be right.

Nothing in the engine calls `CmdWriteDebugMarker` and `DeviceCapabilities::m_breadcrumbs` is
`false` on both backends, so it costs nothing today.

**What was done:** nothing. The Vulkan backend reproduces both branches exactly, so the two write
identical bytes. Fixing it belongs upstream, next to a decision about which one was meant.

### 17. `RHI_Direct3D12.cpp:284` - `RGB565_UNorm` and `BGR565_UNorm` map to the same DXGI format

Both return `DXGI_FORMAT_B5G6R5_UNORM`. Under DXGI's naming, which lists a packed format's
components least significant first, that is the `BGR565` one; `RGB565` has no DXGI format at all.
Nothing in the engine uses either format, so this costs nothing today.

**What was done:** nothing. Vulkan has both formats, so the Vulkan backend could tell them apart and
**chooses not to**: mapping them faithfully would make the two backends draw the same asset
differently. Recorded because a future reader will see two `DataFormat` members reaching one
`VkFormat` and assume it is a copy-paste slip.

---

## Portability defects that are real bugs on Windows too

The two items here are the best upstream pull requests in this document. Both are latent bugs on
MSVC, not merely places where the two compilers differ, and both cost Windows nothing to fix.

### 18. Eleven functions are `inline` on one side of the declaration only

Eight headers declare a member `inline` whose only definition is out of line in the matching
`.cpp` (`ResourceRecord.h`, `InputSystem.h`, `ImguiX.h`, `AnimationSkeleton.h`,
`Animation_RuntimeGraph_Instance.h` twice, `Animation_RuntimeGraphNode_Blend1D.h`,
`ResourceCompilerContext.h`), plus `NodeGraph_FlowGraph.h`'s `GetInputPin` and `GetOutputPin`.
`MathUtils.cpp` has the mirror image: three `ToString` definitions marked `inline` where the header
declares them `EE_BASE_API` without it.

Both are ill-formed. An inline function has to be defined in every translation unit that uses it,
and clang emits nothing for a definition no other TU can see. MSVC emits them anyway because
`__declspec( dllexport )` forces it, so the bug is invisible on Windows.

**What was done:** `inline` is dropped at each site, which is correct on both compilers. Worth an
upstream PR: it is a one-word change per site.

### 19. `va_list` is read twice in five places

`CompileContext::LogError`, `LogWarning` and `LogMessage` each `va_start` once and then hand the
same `va_list` to two consumers - `SystemLog::AddEntryVarArgs` and `m_log.Log*`. `ImguiX.h`'s two
`DrawShadowedText` overloads do the same to draw the shadow and then the text.

On MSVC x64 a `va_list` is a pointer passed by value, so the callee's advance does not disturb the
caller's copy and the second read happens to work. In the System V ABI it is a one-element array
that decays to a pointer, the callee advances the *caller's* state, and the second read walks off
the end of the argument area. **This segfaulted every standalone resource compile**, immediately
after the resource had compiled successfully.

**What was done:** fixed with `va_copy`. This one is a genuine latent bug on Windows too, and is the
better upstream PR of the two.

### 20. `FileSystem.h:97` - an inline that never returns

`UpdateBinaryFile` forwards to its overload and drops the result:

```cpp
EE_FORCE_INLINE bool UpdateBinaryFile( char const* pFilePath, Blob const& fileData, bool* pWasFileUpdated = nullptr )
{ UpdateBinaryFile( pFilePath, fileData.data(), fileData.size(), pWasFileUpdated ); }
```

Falling off the end of a non-void function is undefined behaviour. Every caller reads a garbage
return value.

**What was done:** nothing, per Conventions rule 3. Worth reporting upstream: this one is a real bug
on Windows too.

### 21. `Math_Win32.h` truncates `GetMostSignificantBit` above 2^32

`Code/Base/Math/Platform/Math_Win32.h` casts its argument to `unsigned long` before the scan:

```cpp
_BitScanReverse64( &index, (unsigned long) value );
```

`unsigned long` is 32 bits on Windows, so every value above `2^32` gives the wrong answer.
`Math_Linux.h` uses `__builtin_clzll` and is correct for the full 64-bit range. **The two platforms
therefore disagree**, which is worse than either bug alone.

**What was done:** nothing on the Win32 side, per Conventions rule 3. It is commented in the Linux
header as well as recorded here.

### 22. `SystemLog_Win32.cpp` drops newlines on medium-length messages

`TraceMessage` bounds its newline append at `numCharsWritten < 509` while the buffer is 2048
bytes, so messages between 509 and 2045 characters silently lose their newline.

**What was done:** nothing to the Win32 file. `SystemLog_Linux.cpp` bounds it at the real buffer
size, and the difference is commented there.

---

## Build defects the port could not run without

Everything in this section is `fixed here` or `fixed as a side effect`, because the port does not
build or run otherwise. Each has a row in [TouchedFiles.md](TouchedFiles.md) with the exact edit.

### 23. `IniFile.cpp` puts its whole body inside `#if defined(_MSC_VER)`

The guard opens at line 4 and its matching `#endif` is the **last line of the file**. On any
non-MSVC compiler the translation unit produces nothing at all: it compiles cleanly and leaves
`IniFile::Load`, `Save`, `GetString` and `SetString` undefined until something tries to link an
executable. **This is the single most expensive bug found so far, because there is no compile error
to point at.**

**What was done:** the guard is closed right after the pragmas and reopened around the trailing
`#pragma warning( pop )`. 2 lines added, 0 modified.

### 24. Two `#include` directives use a backslash

`Code/Base/Utils/GlobalRegistryBase.h` and
`Code/Base/Input/InputDevices/InputDevice_Controller.cpp`. clang does not treat `\` as a path
separator inside an include, so neither header was found on Linux.

**What was done:** changed to `/`. MSVC accepts `/`, so this costs Windows nothing.

### 25. The Reflector hardcodes Windows path separators in five places

`ReflectorSettings.h` spells `g_codeFolderPath`, `g_buildFolderPath`, `g_buildTempFolderPath`,
`g_runtimeEngineProjectPath` and `g_toolsEngineProjectPath` with backslashes, and
`ClangParser.cpp`'s `g_includePaths` array does the same. `Reflector.cpp` also appends `.vcxproj`
paths verbatim. On Linux a backslash is an ordinary filename character, so this created a
directory literally named `Build\_Temp\` in the repository root and found no headers at all.

Two of the `g_includePaths` entries also have the wrong case: `EABase\include\common` against
`EABase/include/Common`, and `EASTL\include` against `EASTL/Include`. `ClangUtils.h` has a third,
`<clang/AST/Ast.h>` against the real `AST.h`.

**What was done:** the five constants are spelled per platform, and the case mismatches corrected.

### 26. The Reflector's builtin type table assumes LLP64

`ClangUtils.cpp` maps `clang::BuiltinType::ULongLong` to `uint64_t` and has no case for `ULong`.
That is correct on Windows, where `uint64_t` is `unsigned long long`. Linux is LP64, so
`uint64_t` is `unsigned long`, and every 64-bit property failed with
"Cannot resolve property typename (uint64_t)".

**What was done:** `ULong` and `Long` cases added, guarded to non-Windows.

### 27. Path case mismatches between the `.vcxproj` files and the disk

Found on 2026-08-27. MSBuild ignores case, so Windows builds fine. On Linux the file is simply
not found.

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

**What was done:** **not fixed in the `.vcxproj` files** (Conventions rule 3, TouchedFiles.md).
`SyncUpstream.py` writes the on-disk spelling into `UpstreamProjects.txt` and warns. Worth reporting
upstream: they cost nothing on Windows, so upstream will not notice them on its own.

### 28. `NinjaGen.py` has four defects and does not run

Noted during the first survey. This port rewrites that stale build script, so these are fixed as a
side effect rather than as upstream fixes:

- It parses `Esoterica.sln`, which the repo no longer contains. The project moved to
  `Esoterica.slnx`.
- `cpp_rule` calls `toolchain.compiler_c` instead of `compiler_cpp`.
- `-fsanitize-address` is not a valid flag. It should be `-fsanitize=address`.
- It declares `-std=c++17`, but the project needs C++20.

**What was done:** all four are gone with the rewrite. See
[01-UpstreamMerges.md](01-UpstreamMerges.md#special-case-codescriptsninjagenninjagenpy) for why that
file is a special case.

---

## Recorded for context, not as defects

### 29. `Code/Applications/BuildGenerator/` does not work

It emits rule references with no rule definitions, and it parses the legacy `.sln` GUID format.
`Esoterica.slnx` also references `Docs/docs/CodingGuidelines.md`, which the repository does not
contain.

**What was done:** left alone on purpose. `NinjaGen.py` reports `no sources in <configuration>` for
this project in all 9 configurations, which is expected output and not a regression.

### 30. `RHI_Direct3D12.cpp` has no platform guard

`Code/Base/Render/RHI_Direct3D12.cpp` is 6084 lines of Direct3D 12 with no `#if _WIN32` at the
top and no platform suffix in its name. The same is true of the vendored
`Code/Base/ThirdParty/D3D12MemoryAllocator/`. Nothing about the file marks it as Windows-only,
so nothing but an explicit entry in `Exclusions.txt` keeps it out of a Linux build.

**What was done:** nothing, and nothing is needed. This is fine for upstream, which is Windows-only.
Recorded because it is the single clearest argument against deciding what to build from filenames.
