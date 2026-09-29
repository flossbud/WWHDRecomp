// The Vulkan renderer (docs/recompiler-design.md D13; milestone G2): draws what the null GPU's
// command processor (src/gpu/null_gpu.cpp) decodes, from its register file (D12 as revised).
// It runs on the GPU thread, inside the command processor, and never writes guest memory, so the
// OS-call trace stays the reference's whether it renders or not.
//
// WWHD_RENDER=vk turns it on. CEMU_SHOT_FRAMES and CEMU_SHOT_DIR capture the TV image exactly as
// the reference's patched Cemu does (cemu-patches/0007): frame N is the Nth GX2SwapScanBuffers,
// written as f<N>.tv.ppm, so tools/reference/compare_frames.py can compare the two directories.
#pragma once
#include "Common/precompiled.h"

namespace wwhd::gpu
{
	bool RendererOn();                                             // WWHD_RENDER=vk, and Vulkan came up
	void RendererClear(const uint32be* body, uint32 nWords);       // IT_HLE_CLEAR_COLOR_DEPTH_STENCIL
	void RendererCopyToScanBuffer(const uint32be* body, uint32 nWords); // IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER
	void RendererSwap();                                           // IT_HLE_TRIGGER_SCANBUFFER_SWAP
}
