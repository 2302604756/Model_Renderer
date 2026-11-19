---
name: dx12-renderer
description: Develop and debug the native C++ Direct3D 12 renderer in RenderingProgrameByHand. Use for render passes, GPU resource lifetime and synchronization, HLSL bindings, DXR inline ray queries, and probe-based GI.
---

# DX12 Renderer

Use the existing Windows SDK and DXC stack. Resolve questions about current behavior from the working tree, especially pass order and probe features that may have changed since the README.

## Find the relevant code

Paths below are relative to the `RenderingProgrameByHand/` source directory.

- Start at `App.cpp` for resource creation, shader entry points and PSO setup, and pass registration order. Follow reads and writes through `RenderGraph.h`, `RenderGraph.cpp`, and the relevant pass. Check both graph-managed transitions and barriers emitted inside a pass before changing either.
- For descriptors, follow allocation through `BindlessDescriptorHeap` or `CPUDescriptorAllocator` and consumption through the pass and HLSL. For reuse, follow the owning frame and fence through `RenderContext`, `UploadHeap`, and `TransientHeap` as applicable. Keep descriptors and backing memory valid until their GPU consumers finish.
- For ray tracing, follow `AccelerationStructure.cpp` and `TLASUpdatePass.cpp` through to inline `RayQuery` consumers in `ShadersCommon.hlsl` and `ShadersProbe.hlsl`. Check build or update ordering before traversal, instance-to-object mapping, and the transforms and geometry data used to shade hits.
- For probe GI, follow `ProbeGrid`, the ray and shading passes, the irradiance and distance updates, filtering and border fixup, and the lighting consumer. Confirm which passes are active in `App.cpp`. For leaks, inspect distance moments and visibility weights, probe validity, atlas addressing and borders before changing bias or blending constants.

## Implement and verify

For a new or changed pass, trace each resource from its producer to consumer. Check resource state at each access, ordering of dependent UAV accesses and acceleration-structure builds, and aliasing where placed resources reuse heap memory. For C++/HLSL changes, match shader entry point and target, root parameter index, registers and spaces, bindless descriptor type and index, and the field offsets and stride of shared data. Check both writers and readers of any changed layout.

Use the solution's documented x64 build with Visual Studio 2022 and NuGet restore. Keep the project and filters in sync when adding source files. For runtime shader changes, exercise the affected pass so DXC compiles the changed entry point. Use a debug run and inspect D3D12 Debug Layer output, or use a PIX GPU capture if pass order or resource contents need inspection. For visual regressions, compare the same scene, camera, settings and settling time, and inspect the relevant intermediate buffers if the final image is ambiguous. Report build, shader compilation, and visual verification separately when any of them could not be run.

Consult [Microsoft DirectX Graphics Samples](https://github.com/microsoft/DirectX-Graphics-Samples) for implementation examples and [DirectX specifications](https://github.com/microsoft/DirectX-Specs) for API guarantees.
