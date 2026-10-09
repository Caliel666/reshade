# FSR pre-upscale ReShade integration

This branch is the working area for an experimental Direct3D 12 integration that applies ReShade effects to the render-resolution color input before FidelityFX Super Resolution upscaling.

## Required order

```text
Game render passes
  -> ReShade effect passes on the FSR render-resolution color resource
  -> original FSR upscaler dispatch
  -> optional frame generation
  -> swapchain presentation
```

A hook on `IDXGISwapChain::Present` is not sufficient: by that point the upscaler has already consumed the input and produced display-resolution output.

## Integration constraints

- FSR 3.1's frontend dispatch receives an opaque context pointer and a dispatch-description pointer. The dispatch description includes the command list, render-resolution color/depth/motion-vector resources, output resource, render size, jitter, and timing metadata.
- The hook must preserve the game's original dispatch parameters and must forward exactly once when the effect pass is disabled, unsupported, or fails.
- ReShade effects must execute on the same D3D12 command list/queue and use the game's current resource state. Resource barriers and queue synchronization must be correct; copying the input to the swapchain or running effects at Present is not equivalent.
- FSR 4 must be detected independently. Do not assume its integration path or dispatch ABI is identical to FSR 3.1.
- The implementation must be opt-in until runtime validation proves that it is safe. If the frontend dispatch cannot be identified reliably, leave the game's FSR path untouched.

## Current scope

The initial commit only enables the Windows CI workflow to run on this feature branch. The actual dispatch interception and effect execution are not implemented yet. Do not treat this document or a successful stock ReShade build as evidence that pre-upscale effects work.
