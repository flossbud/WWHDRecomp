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
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;         // of every subresource
		std::vector<VkImageView> layerViews;                     // 2D views of layers 1.. (LayerView)
		uint64 written = 0;                                      // s.writes when last drawn into or cleared (surfaces)
		uint32 bytes = 0;                                        // guest memory it covers from its address (surfaces)
		uint64 resetFor = 0;                                     // the overlapping write it was last reset for (surfaces)
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
	void Transition(Image& img, VkImageLayout layout);
	Image CreateImage(VkFormat format, VkImageAspectFlags aspect, uint32 w, uint32 h, VkImageUsageFlags usage, uint32 layers = 1);
	VkImageView LayerView(Image& img, uint32 layer);             // a 2D view of one layer (img.view for layer 0)
	void DestroyImage(Image& img);
	VkDeviceSize RingAlloc(VkDeviceSize size, VkDeviceSize align); // offset into s.ring (submits when full)

	struct Format { VkFormat vk; VkImageAspectFlags aspect; };
	Format ColorFormat(uint32 gx2);
	Format DepthFormat(uint32 gx2);
	// the surface at (addr, gx2 format), at least w x h with `layers` array slices
	Image& Surface(uint32 addr, uint32 gx2, bool depth, uint32 w, uint32 h, uint32 layers = 1);

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
		enum class Kind : uint8 { Other, Pass, Upload, Copy, Mips, Grow, Reset, Clear, Scan, Present, Count };
		bool On();
		void Init();                                              // after the device and the first command buffer
		void Begin();                                             // a command buffer begins (its slot's fence waited for)
		void Mark(Kind kind, std::string_view label = {});        // from here on the GPU does `kind` work
		void Frame();                                             // a swap
		// a span of one kind of work, "other" after it
		struct Scope
		{
			Scope(Kind kind, std::string_view label = {}) { Mark(kind, label); }
			~Scope() { Mark(Kind::Other); }
		};
	}

	// texture.cpp
	struct Sampled { VkImageView view; VkSampler sampler; VkImageLayout layout; };
	void TextureInit();
	// texture unit `unit` of a stage as the draw samples it; outside rendering (it may upload or
	// transition). A surface among the draw's attachments is never sampled (a placeholder is).
	Sampled SampleTexture(const LatteDecompilerShader* dec, bool vertex, uint32 unit, std::span<Image* const> attachments);
	void ForgetImage(VkImage image);                              // before a surface's image is destroyed
}
