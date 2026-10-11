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

## Internal resolution (upscaling)

`PS2X_GS_SCALE=<n>` (or *Internal resolution* in the F2 settings menu) renders
every target at n x the GS size. Bookkeeping (dirty/stale rows, memory
ranges) stays in GS pixels; only the GL textures are scaled.

- **Drawing**: viewport and scissor scale by n; the GS->GL half-pixel shift
  becomes half a *target* pixel, so a GS pixel's sample point is the centre
  of its first sub-pixel (identical to before at 1x).
- **VRAM <-> target**: loads upscale nearest; resolves take each GS pixel's
  sample-point sub-pixel. Local memory therefore stays exactly what a native
  render would hold.
- **Render-to-texture**: a draw whose texture is a colour target (same
  TBP0/TBW, CT32/CT24 or matching CT16/CT16S) samples the target texture
  directly (`u_rtScale`), with GS wrap modes applied per GS texel and
  bilinear filtering at target resolution. A target pixel k sits at texel
  position k/n + 0.5 (GS pixel p is texel p + 0.5); mapping plain `uv*n`
  instead drifts the image half a pixel per copy pass. A draw sampling the
  target it renders to reads a copy (`feedbackCopy`). Palettized or Z
  reinterpretations still read local memory (resolved first). At 1x this
  path is bit-identical to the local-memory path (PS2X_GS_GPU_NO_RT_TEXTURE=1)
  and skips most resolves.
- **Presentation**: when every enabled read circuit shows a GPU target, the
  PCRTC merge (circuit 2 / background below, circuit 1 blended by ALP or its
  alpha) runs as a fragment pass at full resolution and one frame is read
  back; otherwise (preferred-source substitution, buffers the GPU never drew)
  the CPU composition of local memory is used.
- **Flicker filter**: games show the frame on both circuits a line apart,
  blended 50/50 (VCS: PMODE 0x8027). With the filter off (settings, default)
  the GS frontend drops the shorter circuit from the presentation request.

Replay timing (VCS capture, 54 presents): 1x 6.9 ms, 2x 5.3 ms, 3x 9.7 ms,
4x 12.9 ms per present.
