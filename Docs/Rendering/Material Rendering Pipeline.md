# Material Rendering Pipeline

The material rendering pipeline is a fully GPU-driven path for opaque and transparent triangle meshes. Frustum culling, cluster compaction, cluster culling, and indirect draw argument generation all run in compute shaders — there is no per-instance work on the CPU.

The pipeline produces ready-to-use indirect draw argument buffers for each shader and render view.

## Render Views

We create a render view for the main camera, each shadow cascade, and each environment map face. Directional lights create 4 render views each for Cascaded Shadow Mapping.

Each render view maps to one bit of a 64-bit view mask. The culling passes produce one indirect draw argument buffer per shader, per render view and sub-bucket, through a shared compaction pass that batches clusters into per-shader record chunks and mesh dispatch arguments.

## Culling Passes

Culling is done using 3 indirect compute dispatches:

**Instance culling** runs one 64-thread group per mesh instance page, tests each instance against every render view, and writes visible instances as compacted records into a per-page visibility buffer. Each page emits one indirect argument that drives the next stage.

**Cluster compaction** runs one 128-thread group per page, groups the page's visible instances by material shader, and writes cluster records into per-shader chunks of a shared record buffer. Each chunk appends one cluster culling argument, the argument encodes cluster record into the root constants.

**Cluster culling** runs one 128-thread group per 128 cluster records, performs per-cluster frustum and screen-size tests per view, and builds a 128-bit visibility mask per view and sub-bucket. Each non-empty mask is appended as an indirect mesh dispatch argument into that view, shader and sub-bucket's draw argument buffer.

## Draw Arguments

Three sub-buckets exist per shader per view: opaque, alpha-tested and alpha-blended. The sub-bucket is selected at runtime using the shader flags specified in the material.

Each bucket has its own draw counter and draw argument buffer; culling appends masks through atomic counters, batching many clusters into a small number of mesh dispatches per bucket.

Each render pass executes its draw argument buffers with the per-bucket counters. Shadow passes do a single depth pass; the forward shading pass runs a depth prepass followed by opaque and alpha blending passes.

## Light and Decal Culling

Lights and decals use a spatial hash for world-space culling.

The world is partitioned into a 6-level LOD hierarchy -- the hash works with arbitrary spatial positions and any cell coordinate you feed it; it just happens to be camera-relative by default. The coarsest LOD spans the entire scene, finer LODs refine near the origin.

Unlike a dense grid, the hash stores only occupied cells -- empty space costs no memory, and the structure naturally conforms to complex world topology without extra cost.

Unlike screen-space tiling, the spatial hash supports arbitrary world-space lookups -- reflection passes and ray tracing shaders can query off-screen lights and decals.

A compute shader tests each light against its cell, constrained by parent page ranges, and writes per-cell bitmasks into an open-addressing hash table. The culling pass runs on the async compute queue, overlapped with the depth prepass and the previous frame's post processing.

The pixel shader looks up its cell via `LoadPayloadCell` and iterates the compacted light list with a scalarized bitscan loop.

## Extending the Pipeline

At the moment the renderer is “vertically integrated” — extending it means writing code directly. We are working on a more customizable and data-driven solution, current renderer code is under heavy development.

There is no plugin system or high-level scripting API.

The pipeline has two natural extension points:

1. **Custom mesh rendering passes** inside the material pipeline. Derive from`RenderPass_ForwardShading` or `RenderPass_CascadedShadow` in the engine source. Prefer this when triangle mesh rendering fits into the existing pipeline.
2. **Custom rendering passes** outside the material pipeline. Derive from `RenderPass_PostProcess` in the engine source. Prefer this for image filtering or anything that does not fit the triangle mesh category.

`Renderer_ForwardShading.h/.cpp` implements the clustered forward rendering pipeline. It can be extended or used as a reference for a custom renderer.

Shader authoring is covered in [Shaders](Shaders.md) . Mesh data and compression are in [Meshes](Meshes.md) [.](Meshes.md)