# AMD FidelityFX FSR 3.1 upscaler and frame generation (vendored)

From AMD's FidelityFX SDK, tag **v1.1.4**, commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`
(github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), under the MIT license in `LICENSE.txt` (this directory
stays MIT; the rest of the project is MPL-2.0). v1.1.4 is the last SDK with a Vulkan backend (SDK 2.x is
DirectX 12 only); its own backend is not used: `src/gpu/vk/fsr3.cpp` implements the `FfxInterface` the host code
calls, and compiles the shaders here with glslang at run time.

What's here, at the SDK's own paths:
- `include/FidelityFX/host/`: `ffx_interface.h`, `ffx_types.h`, `ffx_error.h`, `ffx_util.h`, `ffx_assert.h`,
  `ffx_message.h`, `ffx_fsr3upscaler.h`;
- `include/FidelityFX/gpu/`: `ffx_common_types.h`, `ffx_core*.h`, `fsr1/ffx_fsr1.h`, `spd/ffx_spd.h`,
  `fsr3upscaler/*.h` (the HLSL callbacks too: the host code includes them for its resource names);
- `src/components/fsr3upscaler/`: `ffx_fsr3upscaler.cpp`, `ffx_fsr3upscaler_private.h` (the host code);
- frame generation (b-framegen): `include/FidelityFX/host/ffx_frameinterpolation.h`, `ffx_opticalflow.h`;
  `include/FidelityFX/gpu/frameinterpolation/*.h`, `opticalflow/*.h`; `src/components/frameinterpolation/` and
  `src/components/opticalflow/` (`.cpp`, `_private.h`); `shaders/frameinterpolation/*.glsl`, `shaders/opticalflow/*.glsl`.
  Not taken: `ffx_fsr3.cpp` (its frame generation drives the SDK's own swapchain, `FrameInterpolationSwapchainVK`;
  `fsr3.cpp` drives optical flow and interpolation itself and presents through the renderer's swapchain);
- `src/shared/`: `ffx_object_management.cpp`, `.h` (`ffx_assert.cpp` and `ffx_message.cpp` are not taken: they
  use Windows APIs; `src/gpu/vk/fsr3.cpp` defines `ffxAssertReport`, `ffxPrintMessage` and their setters);
- `shaders/fsr3upscaler/*.glsl`: the passes, from `sdk/src/backends/vk/shaders/fsr3upscaler/`.

Built as they are (`src/CMakeLists.txt`, target `wwhd_fsr3`, C++20) with `src/gpu/vk/fsr3_compat.h` force-included:
`wcscpy_s` and `_countof` (Windows CRT names the host code uses) and a larger `FFX_SDK_DEFAULT_CONTEXT_SIZE`
(the private context holds `wchar_t` names, 4 bytes outside Windows). **The one change to these files:**
`include/FidelityFX/host/ffx_types.h` defines `FFX_SDK_DEFAULT_CONTEXT_SIZE` only `#ifndef`, so the shim's
value wins. To update: copy the same files from a newer tag, re-apply that guard, rebuild.
