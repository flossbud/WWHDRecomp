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
		Ring ring;
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
	void SubmitAndWait();                                         // ends rendering first
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
	// draw.cpp
	void DrawInit();
	void EndRendering();                                          // before anything outside a render pass
	void OnSubmitted();                                           // per-frame resources can be reused
	void DrawStats(uint32 frame);

	// texture.cpp
	struct Sampled { VkImageView view; VkSampler sampler; VkImageLayout layout; };
	void TextureInit();
	// texture unit `unit` of a stage as the draw samples it; outside rendering (it may upload or
	// transition). A surface among the draw's attachments is never sampled (a placeholder is).
	Sampled SampleTexture(const LatteDecompilerShader* dec, bool vertex, uint32 unit, std::span<Image* const> attachments);
	void ForgetImage(VkImage image);                              // before a surface's image is destroyed
}
