# GPU GS renderer

Goal: move GS rasterization from the CPU (`GSCpuBackend`, banded across worker
threads) to the GPU, so the GS stops being the limiting pipeline stage and so
internal resolution can later be raised (upscaling).

The CPU backend stays as the reference implementation and fallback.

## Shape

`GSGpuBackend` implements `GSRasterBackend` (`runtime/gs/gs_backend.h`), so
the GS frontend, the GIF arbiter and the async VIF1 path are unchanged.
Selected with `PS2X_GS_GPU=1` until it is the default.

- **Context**: OpenGL 4.6 core on an EGL *surfaceless* context owned by a
  dedicated render thread (no window needed, works from any thread and in
  `tools/gs_replay`). GL entry points are loaded with `eglGetProcAddress`.
  (Windows/macOS later: WGL hidden window or Vulkan.)
- **Threading**: the frontend's calls are queued; the render thread batches
  consecutive primitives with identical draw state into one vertex buffer
  draw.
- **Local memory model**: the 4 MiB CPU copy (`GSMem` layout) stays the source
  of truth for host->local uploads, CLUTs and textures. Render targets live on
  the GPU. Pages written by GPU draws are tracked; when the CPU side needs them
  (local->host readback, local->local copy, a texture format the GPU path can't
  reinterpret) the render target is read back and swizzled into local memory.
- **Render target cache**: key (FBP, FBW, PSM) -> RGBA8 colour texture
  (x scale factor later) plus a depth texture (ZBP, PSM). Created on first draw.
- **Texture cache**: key (TBP0, TBW, PSM, TW, TH [, CLUT hash]) -> RGBA8 texture
  decoded from local memory; invalidated by uploads/draws touching its pages.
  If the texture address coincides with a render target (VCS bloom/post passes)
  the render target texture is sampled directly.
- **Pixel pipeline**: one shader with uniforms for TFX/TCC, fog, alpha test
  (+AFAIL), DATE, the GS blend equation ((A-B)*C>>7)+D (fixed-function where it
  maps, otherwise reading the destination via texture barrier), FBMSK, FBA.
  Depth: 32-bit integer semantics emulated in the shader where needed.
- **Presentation**: read back the displayed buffer (DISPFB/DISPLAY) into
  `PresentationFrame` (later: share the texture with the host window directly).

## Phases

1. Context + GL loader + skeleton backend (CPU fallback for everything);
   `gs_replay` can run it headless.
2. Untextured triangles/sprites/lines into render targets; presentation;
   frame diff vs the CPU backend in `gs_replay`.
3. Textures: CT32/CT24/CT16, P8/P4 + CLUT, filtering, wrap/region modes.
4. Blending, alpha/destination-alpha tests, fog, FBMSK, depth formats.
5. Render-to-texture, local->local copies, local->host readbacks, uploads
   into render-target memory.
6. Live game; performance; then upscaling.

## Verification

`PS2X_GS_CAPTURE` + `tools/gs_replay` replay the same GS stream through both
backends; presented frames are compared (max/mean difference, PNG dumps) and
the CPU backend (1 worker) remains the bit-exact reference.
