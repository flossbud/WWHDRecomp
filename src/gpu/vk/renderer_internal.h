// Shared state of the renderer's files (renderer.cpp: device, surfaces, clears, scan-out, capture;
// draw.cpp: draws; latte_glue.cpp: the pieces of Latte the shader decompiler needs).
#pragma once
#include "renderer.h"
#include "vk.h"
#include <map>
#include <set>
#include <span>
#include <unordered_map>

struct LatteDecompilerShader;

namespace wwhd::gpu
{
	using namespace wwhd::vk;

	struct Image
	{
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkImageAspectFlags aspect = 0;
		uint32 width = 0, height = 0, layers = 1;
		uint32 gw = 0, gh = 0;                                   // the guest's size it stands for (width x height but for
		bool scaled = false;                                     // WWHD_RENDER_SCALE's surfaces and their copies: Scaled)
		float scale = 1.0f;                                      // the scale it was made at (dynamic resolution: one of several)
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;         // of every subresource
		std::vector<VkImageView> layerViews;                     // 2D views of layers 1.. (LayerView)
		uint64 written = 0;                                      // s.writes when last drawn into or cleared (surfaces)
		uint32 bytes = 0;                                        // guest memory it covers from its address (surfaces)
		uint64 resetFor = 0;                                     // the overlapping write it was last reset for (surfaces)
		uint32 readH = 0;                                        // the most rows any read of it wanted (surfaces: SurfaceRead)
		uint32 fitH = 0;                                         // WWHD_SURFACE_FIT: the height it was fitted to (or would be)
		uint32 readSince = 0;                                    // the frame readH last grew (fitted only once it's stable)
		bool noFit = false;                                      // read past fitH, or past its own height: never fitted
		bool clearPending = false;                               // a whole-image clear not recorded yet (clears as load ops:
		VkClearValue clearValue{};                               //   (DeferClear), done by the next pass's load op or FlushClear
	};

	// host-visible memory that per-frame data (uniforms, vertices, indices) is written into; reset
	// after every submit
	struct Ring
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		uint8* data = nullptr;
		VkDeviceSize size = 0, used = 0;
	};

	struct State
	{
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice physical = VK_NULL_HANDLE;
		VkPhysicalDeviceProperties props{};
		VkDevice device = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		uint32 queueFamily = 0;
		VkPhysicalDeviceMemoryProperties memory{};
		bool depthClip = false;                                  // VK_EXT_depth_clip_enable
		bool customBorder = false;                               // VK_EXT_custom_border_color, without format
		bool anisotropy = false;
		bool float16 = false;                                    // shaderFloat16 and 16-bit storage (FSR 1's fp16 path)
		bool subgroupHalf = false;                               // and subgroup operations on halves (FSR 3's fp16 path)
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkDescriptorPool descriptors = VK_NULL_HANDLE;           // reset after every submit
		// the draw path's last binds in the command buffer being recorded (draw.cpp: an unchanged pipeline, viewport,
		// scissor, blend constants or depth bias isn't set again; every pipeline has those four dynamic, so they
		// outlive a pipeline change); cleared when a command buffer begins. WWHD_BINDCACHE=0: off
		struct Bound
		{
			bool valid = false;
			VkPipeline pipeline = VK_NULL_HANDLE;
			VkViewport viewport{};
			VkRect2D scissor{};
			float blend[4]{};
			float bias[3]{};
		} bound;
		Ring ring;
		// the lazy GX2DrawDone (WWHD_LAZY_DRAWDONE=1; real time; docs/research/deck-plan.md
		// item 2): two of each per-frame resource, the frame's submit (SubmitFrame) not waiting for the GPU. Slot 1's
		// are made only when it's on; cmd, fence and descriptors are the current slot's, and the ring's
		// [ringBase, ringEnd) its half (the whole ring when off)
		VkCommandBuffer cmds[2]{};
		VkFence fences[2]{};
		VkDescriptorPool pools[2]{};
		bool pending[2]{};                                       // submitted, its fence not yet waited for
		uint32 slot = 0;
		VkDeviceSize ringBase = 0, ringEnd = 0;
		std::map<std::pair<uint32, uint32>, Image> surfaces;     // (physical address, GX2 format | depth flag)
		uint64 writes = 0;                                       // surface writes so far (Image::written)
		Image scan[2];                                           // TV, DRC
		uint32 frame = 0;
		std::set<uint32> shotFrames;
		std::string shotDir = ".";
	};
	extern State s;

	void Log(const std::string& msg);
	[[noreturn]] void Fail(const std::string& msg);
	void Check(VkResult r, const char* what);
	uint32 MemoryType(uint32 bits, VkMemoryPropertyFlags props);
	void SubmitAndWait();                                         // ends rendering first; the GPU idle after it
	void SubmitFrame();                                           // a swap's submit (lazy: not waited for)
	bool LazyDrawDone();                                          // WWHD_LAZY_DRAWDONE=1 in real time
	void HostReadBarrier();                                       // a copy's results visible to the host after the fence
	void WaitPending();                                           // the GPU done with every submitted frame (no submit)
	void Transition(Image& img, VkImageLayout layout);            // a pending clear first (FlushClear)
	// Clears as load ops (gpu-plan item 2's rest; the default, WWHD_CLEAR_LOADOP=0 turns it off): a clear of a whole
	// single-layer image (the game's clears, the overwritten surfaces' resets) is kept on the image and done by the load
	// op of the next pass into it when that pass covers the whole image; any other use records it first
	bool DeferClear(Image& img, const VkClearValue& value);       // false: not deferred, the caller clears
	void FlushClear(Image& img);
	Image CreateImage(VkFormat format, VkImageAspectFlags aspect, uint32 w, uint32 h, VkImageUsageFlags usage, uint32 layers = 1);
	VkImageView LayerView(Image& img, uint32 layer);             // a 2D view of one layer (img.view for layer 0)
	void DestroyImage(Image& img);
	VkDeviceSize RingAlloc(VkDeviceSize size, VkDeviceSize align); // offset into s.ring (submits when full)

	// WWHD_RENDER_SCALE (gpu-plan.md item 3): the screen-sized surfaces (ScaledSurface) are made Scaled(guest size), with
	// their copies, mip chains and the scan images taken from them; everything the guest sees stays in its sizes:
	// surface lookups, aliasing and reads by size use gw/gh, viewports and scissors are scaled by the target's width/gw,
	// the shaders' uf_fragCoordScale and uf_texNScale undo it. Off (1): every image the guest's size
	// WWHD_RENDER_SCALE=auto: dynamic resolution, the scale chosen at each swap from the GPU's time a frame (DynamicScale)
	float RenderScale();                                          // the current scale
	bool DynamicScale();
	bool ScaledSurface(uint32 gw, uint32 gh);
	uint32 Scaled(uint32 guest, bool scaled = true);            // an image's size for a guest size (scaled or not), now
	uint32 ScaledBy(uint32 guest, float scale);                 // at a given scale (an existing image's: Image::scale)

	struct Format { VkFormat vk; VkImageAspectFlags aspect; };
	Format ColorFormat(uint32 gx2);
	Format DepthFormat(uint32 gx2);
	// the surface at (addr, gx2 format), at least w x h with `layers` array slices (a fitted one: h at most its fitH)
	Image& Surface(uint32 addr, uint32 gx2, bool depth, uint32 w, uint32 h, uint32 layers = 1);
	// a read of a surface's first `rows` rows (sampled, copied, scanned out): WWHD_SURFACE_FIT's evidence
	void SurfaceRead(Image& img, uint32 rows);

	// present.cpp: the window (renderer.h), if there is one
	bool HasWindow();
	std::vector<const char*> WindowInstanceExtensions();
	void CreateWindowSurface();                                   // after the instance
	bool CanPresent(uint32 queueFamily);                          // true without a window
	void PresentRecord(Image& scan);                              // before the swap's submit: into the next window image
	void PresentQueue();                                          // after it
	VkSemaphore PresentSemaphore();                               // the swap's submit signals it when the lazy path presents

	// shader_cache.cpp: the cache on disk (design D20); its records are draw.cpp's byte strings
	namespace cache
	{
		void Open();                                              // after the device is up
		const std::vector<std::vector<uint8>>& Shaders();         // the records read at Open
		const std::vector<std::vector<uint8>>& Pipelines();
		void AddShader(std::span<const uint8> record);            // appended as they are first seen
		void AddPipeline(std::span<const uint8> record);
		VkPipelineCache Driver();                                 // the driver's, for every pipeline built
		void Save();                                              // the driver's to disk, now
		void SaveNowAndThen();                                    // at a swap: saves on another thread when due
	}
	// shader_list.cpp: the shader list (D20: a first start without hitches)
	namespace shaderlist
	{
		struct List
		{
			struct Program { uint64 hash = 0; uint32 size = 0; std::string file; };
			struct Fetch { uint64 hash = 0; std::vector<uint8> code; std::string registers; };
			struct Shader { bool vertex = false; uint64 key = 0, program = 0, fetch = 0; std::string registers; };
			std::unordered_map<uint64, Program> programs;
			std::unordered_map<uint64, Fetch> fetches;
			std::vector<Shader> shaders;
			std::vector<std::vector<uint8>> pipelines;
		};
		std::string Registers(const uint32* regs);                // the registers translation reads, as text
		bool SetRegisters(uint32* regs, size_t count, std::string_view text);   // and back; the rest become 0
		std::string Hex(std::span<const uint8> bytes);
		bool Capturing();                                         // WWHD_SHADER_SOURCES=path
		void Capture(const std::string& line);
		bool Read(List& list);                                    // the shipped list; false if there is none
		// the programs among `wanted` that the list says where to find, read from the game's files
		std::unordered_map<uint64, std::vector<uint8>> FindPrograms(const List& list, const std::set<uint64>& wanted);
	}

	// draw.cpp
	void DrawInit();
	void EndRendering();                                          // before anything outside a render pass
	void OnSubmitted();                                           // per-frame resources can be reused
	void ForgetSets();                                            // an image view is destroyed: no set may be reused
	void DrawStats(uint32 frame);

	// gpu_timing.cpp: WWHD_GPU_TIMING=1, GPU time by kind of work (docs/research/gpu-plan.md item 1)
	namespace timing
	{
		enum class Kind : uint8 { Other, Pass, Upload, Copy, Mips, Grow, Reset, Clear, Scan, Present, Upscale, Count };
		bool On();                                                // WWHD_GPU_TIMING (every mark; not the light mode)
		void Init(bool light);                                    // after the device and the first command buffer; light:
		                                                          // the start and end of each command buffer only
		void Begin();                                             // a command buffer begins (its slot's fence waited for)
		void End();                                               // it ends (before vkEndCommandBuffer)
		void Mark(Kind kind, std::string_view label = {});        // from here on the GPU does `kind` work
		void Write(Kind kind, std::string_view label);            // the timestamp itself (Mark's, Begin's, End's)
		void Frame();                                             // a swap
		// the GPU's mean ms a frame over the frames since the last call, once at least `frames` swaps were counted (the
		// timestamps are read a frame or two late; over a window that's the same rate)
		bool TakeWindow(uint32 frames, double& msPerFrame);
		// a span of one kind of work, "other" after it
		struct Scope
		{
			Scope(Kind kind, std::string_view label = {}) { Mark(kind, label); }
			~Scope() { Mark(Kind::Other); }
		};
	}

	// texture.cpp
	// scale: the image's size over the guest's (a surface's under WWHD_RENDER_SCALE), for the shader's uf_texNScale
	// surface: from a render target (not guest memory): what tells the game's post passes from its HUD (HudBegins)
	struct Sampled { VkImageView view; VkSampler sampler; VkImageLayout layout; float scaleX = 1.0f, scaleY = 1.0f; bool surface = false; };
	void TextureInit();
	// texture unit `unit` of a stage as the draw samples it; outside rendering (it may upload or
	// transition). A surface among the draw's attachments is never sampled (a placeholder is).
	Sampled SampleTexture(const LatteDecompilerShader* dec, bool vertex, uint32 unit, std::span<Image* const> attachments);
	void ForgetImage(VkImage image);                              // before a surface's image is destroyed

	// fsr1.cpp: AMD FSR 1 for the render scale (WWHD_UPSCALER=fsr1)
	namespace fsr1
	{
		bool On();
		// `src` (a scaled image) upscaled into `dst` (the guest's size, a colour attachment); false: couldn't (logged)
		bool Upscale(Image& src, Image& dst);
	}
	// fsr1.cpp: a full-screen pass of a fragment shader (GLSL, "src" a sampler2D at binding 0, push constants uvec4
	// c0..c3) from `src` into `dst` (a colour attachment); its pipelines made per target format
	namespace fullscreen
	{
		VkShaderModule Fragment(const std::string& glsl, const char* name);   // null if it doesn't compile (logged)
		void Draw(VkShaderModule fragment, Image& src, Image& dst, const uint32 (&c)[16]);
	}
	// motion.cpp: motion vectors (WWHD_MOTION=1|debug; b-motion)
	namespace motion
	{
		constexpr uint32 kPrevBinding = 64;                       // the previous frame's uniform blocks: binding + this
		constexpr uint32 kSlot = 7;                               // the motion target's colour slot
		constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // motion in R and G, the reactive mask in B
		// a vertex shader binding's data this draw: its ring offset, the guest's bytes in it, the range the shader reads,
		// and the block's guest address (0: the uniform vars)
		struct UniformData { VkDeviceSize offset = 0; uint32 bytes = 0, readable = 0; MPTR phys = 0; };
		bool On();
		bool Debug();
		uint32 VaryingBase();                                     // the two clip positions' locations (0: no room)
		bool VertexVariant(const std::string& in, std::string& out);
		bool PixelVariant(const std::string& in, std::string& out);
		bool ReactiveVariant(const std::string& in, std::string& out);   // a blended draw's: the mask alone
		// a scene draw's constants kept, and the previous frame's for it (matched, or the camera's) pushed as offsets
		void Remember(uint64 group, uint64 vsKey, std::span<const UniformData> data, std::vector<uint32>& prevOffsets, bool want);
		void FrameEnd(uint32 frame);                              // a presented frame's swap
		void OnSubmitted();                                       // the ring was handed back (its offsets are gone)
		Image* Target(const Image& depth, bool& clear);           // the motion target for a scene pass (clear: first this frame)
		void DrawDebug(Image& tv);                                // WWHD_MOTION=debug: the motion over the TV image
		Image* TargetImage();                                     // this frame's motion target (null if none yet)
		Image* SceneDepth();                                      // the depth target the scene drew with this frame
		bool JitterOn();                                          // WWHD_JITTER=1 (with WWHD_MOTION)
		void Jitter(uint32 frame, float& x, float& y);            // this frame's offset, target pixels (-0.5..0.5)
	}
	// fsr3.cpp: AMD FSR 3.1's upscaler on the motion vectors (WWHD_UPSCALER=fsr3)
	namespace fsr3
	{
		bool On();
		// the scaled scene (`color`, its `depth`, the motion target) upscaled into `out` (the guest's size, storage usage)
		bool Upscale(Image& color, Image& depth, Image& motionTarget, Image& out, float jitterX, float jitterY, bool reset);
	}
	// renderer.cpp: where the TV image's HUD begins (a draw into the TV surface alone, sampling only textures from
	// memory, after a full-screen pass into it that sampled surfaces): with FSR 1 the scaled image is upscaled there
	Image* TvSurface();                                           // the TV scan buffer's source last frame, if known
	void HudBegins(bool atScanCopy = false);
	bool HudActive();                                             // the TV surface is the full-size image (this frame)
	void HudPromote(Image& img);                                  // a scaled target drawn with the TV after the split: full size
	// draw.cpp: GLSL to SPIR-V with glslang (the game's shaders' compiler)
	bool CompileGlsl(const std::string& glsl, int stage, std::vector<uint32>& spirv, std::string& log);
}
