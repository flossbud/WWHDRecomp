// The Vulkan renderer (docs/recompiler-design.md D13; milestone G2): draws what the null GPU's
// command processor (src/gpu/null_gpu.cpp) decodes, from its register file (D12 as revised).
// It runs on the GPU thread, inside the command processor, and never writes guest memory, so the
// OS-call trace stays the reference's whether it renders or not.
//
// WWHD_RENDER=vk turns it on. CEMU_SHOT_FRAMES and CEMU_SHOT_DIR capture the TV image exactly as
// the reference's patched Cemu does (cemu-patches/0007), written as f<N>.tv.ppm, so
// tools/reference/compare_frames.py can compare the two directories. The reference requests shot N
// at the Nth GX2SwapScanBuffers and its renderer takes it at the next present, so the image in
// f<N> is the one the (N+1)th swap presents; captures here follow that.
#pragma once
#include "Common/precompiled.h"

namespace wwhd::gpu
{
	// The window the TV image is presented to at every swap: the frontend's (SDL3), set before the
	// renderer starts. Without one the renderer is headless.
	struct Window
	{
		std::vector<const char*> instanceExtensions;              // what createSurface needs
		std::function<uint64(void* instance)> createSurface;     // a VkSurfaceKHR for it, 0 on failure
		std::function<void(uint32& w, uint32& h)> size;           // its drawable size in pixels
	};
	void SetWindow(Window window);
	// An image laid over the TV image in the window, never in captures: the system's own screens
	// (keyboard, error dialogs). RGBA8 pixels, placed at (x, y) and sized w x h in the TV image's
	// 1920x1080 frame, scaled with it. No pixels: nothing.
	void SetOverlay(sint32 x, sint32 y, uint32 w, uint32 h, std::vector<uint32> rgba);

	bool RendererOn();                                             // WWHD_RENDER=vk, and Vulkan came up

	// The shader cache (docs/recompiler-design.md D20). Before the game starts, PrepareShaders builds
	// every shader and pipeline the cache on disk knows (portable/shaderCache/wwhd), on all cores but
	// the caller's, and calls progress(done, total) on the caller's thread about every 30 ms meanwhile.
	// Nothing without WWHD_RENDER=vk.
	void PrepareShaders(const std::function<void(uint32 done, uint32 total)>& progress);
	void PresentOverlayOnly();                                     // the overlay alone on black: the progress screen
	void SaveShaderCache();                                        // the driver's part to disk, now (at exit)
	void RebuildSwapchain();                                       // at the next present (the settings' vsync)
	// shaders translated and pipelines built during play since the last call (first sights: the
	// hitches), and the time they took
	struct FirstSights { uint32 shaders = 0, pipelines = 0; double shaderMs = 0, pipelineMs = 0; };
	FirstSights TakeFirstSights();

	void RendererClear(const uint32be* body, uint32 nWords);       // IT_HLE_CLEAR_COLOR_DEPTH_STENCIL
	void RendererCopyToScanBuffer(const uint32be* body, uint32 nWords); // IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER
	void RendererSwap();                                           // IT_HLE_TRIGGER_SCANBUFFER_SWAP
	void RendererDraw(uint32 op, const uint32be* body, uint32 nWords); // IT_DRAW_INDEX_2 / _AUTO (draw.cpp)
	void RendererCopySurface(const uint32be* body, uint32 nWords); // IT_HLE_COPY_SURFACE_NEW (texture.cpp)
	void RendererFrameDropped(bool dropped);                       // 0xFC, a dropped frame's mark (draw.cpp: its screen-sized draws left out)
}
