// wwhd frontend: Cemu's WindowSystem interface, replacing the wxWidgets GUI
// (docs/recompiler-design.md D11). Cemu's own main() parses the command line (-g GAME, ...) and
// calls WindowSystem::Create(), which here is the whole application: boot, one TV window, and an
// event loop. Single screen by design: there is no GamePad window (D17).
// wwhd (Cemu's Latte) has a plain Xlib window. wwhd-null (WWHD_NULL_GPU, see src/gpu/null_gpu.cpp)
// is headless unless WWHD_WINDOW=1 opens an SDL3 window that our renderer (WWHD_RENDER=vk)
// presents the TV image to (F11 or Alt+Enter: fullscreen). It links its own SDL3 build, with video,
// in place of the one Cemu's vcpkg build ships for controllers only (src/build.sh). Headless, the
// process runs until the title exits it (e.g. CEMU_HLE_TRACE_EXIT_FRAME).
#include "boot.h"
#include "interface/WindowSystem.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Cafe/CafeSystem.h"
#ifdef WWHD_NULL_GPU
#include "../runtime/runtime.h"
#include "../gpu/vk/renderer.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#else
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#endif
#include "util/helpers/helpers.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>

void CemuCommonInit();          // main.cpp
void LatteOverlay_init();

static WindowSystem::WindowInfo g_windowInfo{};

static void setSize(int w, int h)
{
	g_windowInfo.width = w; g_windowInfo.height = h;
	g_windowInfo.phys_width = w; g_windowInfo.phys_height = h;
}

#ifdef WWHD_NULL_GPU
// The window's drawable size, kept by the event loop for the renderer's thread.
static std::atomic<uint32> s_pixelWidth, s_pixelHeight;

static void StorePixelSize(SDL_Window* window)
{
	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(window, &w, &h);
	s_pixelWidth = (uint32)w;
	s_pixelHeight = (uint32)h;
	setSize(w, h);
}

// WWHD_WINDOW=1: the TV window, handed to the renderer; nullptr otherwise
static SDL_Window* OpenWindow()
{
	const char* on = getenv("WWHD_WINDOW");
	if (!on || strcmp(on, "1") != 0)
		return nullptr;
	if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
		wwhd::Fatal(fmt::format("SDL video: {}", SDL_GetError()));
	SDL_Window* window = SDL_CreateWindow("The Legend of Zelda: The Wind Waker HD", 1280, 720,
		SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!window)
		wwhd::Fatal(fmt::format("SDL window: {}", SDL_GetError()));
	StorePixelSize(window);
	Uint32 n = 0;
	const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&n);
	if (!extensions)
		wwhd::Fatal(fmt::format("SDL Vulkan: {}", SDL_GetError()));
	wwhd::gpu::Window w;
	w.instanceExtensions.assign(extensions, extensions + n);
	w.createSurface = [window](void* instance) -> uint64 {
		VkSurfaceKHR surface{};
		if (!SDL_Vulkan_CreateSurface(window, (VkInstance)instance, nullptr, &surface))
		{
			cemuLog_log(LogType::Force, "wwhd: SDL_Vulkan_CreateSurface: {}", SDL_GetError());
			return 0;
		}
		return (uint64)surface;
	};
	w.size = [](uint32& width, uint32& height) { width = s_pixelWidth; height = s_pixelHeight; };
	wwhd::gpu::SetWindow(std::move(w));
	cemuLog_log(LogType::Force, "wwhd: window on SDL's {} video driver", SDL_GetCurrentVideoDriver());
	return window;
}

[[noreturn]] static void EventLoop(SDL_Window* window)
{
	for (;;)
	{
		SDL_Event ev;
		if (!SDL_WaitEvent(&ev))
			continue;
		switch (ev.type)
		{
		case SDL_EVENT_QUIT:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			cemuLog_log(LogType::Force, "wwhd: window closed");
			_exit(0);   // like Cemu's own exit path mid-game: skip global destructors
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: StorePixelSize(window); break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED: g_windowInfo.app_active = true; break;
		case SDL_EVENT_WINDOW_FOCUS_LOST: g_windowInfo.app_active = false; break;
		case SDL_EVENT_KEY_DOWN:
			if (!ev.key.repeat && (ev.key.key == SDLK_F11 || (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))))
				SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
			break;
		default: break;
		}
	}
}
#endif

void WindowSystem::Create()
{
	SetThreadName("wwhd");
	wwhd::SetupPaths();
	wwhd::LoadConfig();
	wwhd::CreateDefaultMLCFiles();
	ActiveSettings::Init();
#ifndef WWHD_NULL_GPU
	LatteOverlay_init();
#endif
	CemuCommonInit();

	auto game = LaunchSettings::GetLoadFile();
	if (!game)
		wwhd::Fatal("usage: wwhd -g <game.wua | title dir | .rpx>");
	setSize(1920, 1080);
	g_windowInfo.dpi_scale = 1.0;
	g_windowInfo.pad_open = false;
	g_windowInfo.app_active = true;

#ifdef WWHD_NULL_GPU
	SDL_Window* window = OpenWindow();
	wwhd::PrepareTitle(*game);
	wwhd::rt::Install();    // the execution seam: interprets, or runs diff mode (src/runtime)
	CafeSystem::LaunchForegroundTitle();
	if (window)
		EventLoop(window);
	for (;;)
		std::this_thread::sleep_for(std::chrono::seconds(1));
#else

	Display* dpy = XOpenDisplay(nullptr);
	if (!dpy)
		wwhd::Fatal("cannot open the X display (set DISPLAY)");
	const int w = 1920, h = 1080;
	Window window = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, w, h, 0, 0, BlackPixel(dpy, DefaultScreen(dpy)));
	XStoreName(dpy, window, "WWHD");
	Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", 0);
	XSetWMProtocols(dpy, window, &wmDelete, 1);
	XSelectInput(dpy, window, StructureNotifyMask | FocusChangeMask);
	XMapWindow(dpy, window);
	XFlush(dpy);
	auto& canvas = g_windowInfo.canvas_main;
	canvas.backend = WindowHandleInfo::Backend::X11;
	canvas.display = dpy;
	canvas.surface = reinterpret_cast<void*>(window);
	g_windowInfo.window_main = canvas;

	if (!InitializeGlobalVulkan() || !g_vulkan_available)
		wwhd::Fatal("Vulkan is not available");
	try
	{
		g_renderer = std::make_unique<VulkanRenderer>();
		VulkanRenderer::GetInstance()->InitializeSurface({w, h}, true);
	}
	catch (const std::exception& e)
	{
		wwhd::Fatal(fmt::format("Vulkan renderer: {}", e.what()));
	}

	wwhd::PrepareTitle(*game);
	CafeSystem::LaunchForegroundTitle();

	for (;;)
	{
		XEvent ev;
		XNextEvent(dpy, &ev);
		switch (ev.type)
		{
		case 33:   // ClientMessage (Cemu's headers #undef it, and False)
			if ((Atom)ev.xclient.data.l[0] == wmDelete)
			{
				cemuLog_log(LogType::Force, "wwhd: window closed");
				_exit(0);   // like Cemu's own exit path mid-game: skip global destructors
			}
			break;
		case ConfigureNotify: setSize(ev.xconfigure.width, ev.xconfigure.height); break;
		case FocusIn: g_windowInfo.app_active = true; break;
		case FocusOut: g_windowInfo.app_active = false; break;
		default: break;
		}
	}
#endif
}

void WindowSystem::ShowErrorDialog(std::string_view message, std::string_view title, std::optional<ErrorCategory>)
{
	cemuLog_log(LogType::Force, "error: {} {}", title, message);
	fprintf(stderr, "error: %.*s\n", (int)message.size(), message.data());
}

WindowSystem::WindowInfo& WindowSystem::GetWindowInfo() { return g_windowInfo; }
void WindowSystem::UpdateWindowTitles(bool, bool, double) {}
void WindowSystem::GetWindowSize(int& w, int& h) { w = g_windowInfo.width; h = g_windowInfo.height; }
void WindowSystem::GetPadWindowSize(int& w, int& h) { w = 0; h = 0; }
void WindowSystem::GetWindowPhysSize(int& w, int& h) { w = g_windowInfo.phys_width; h = g_windowInfo.phys_height; }
void WindowSystem::GetPadWindowPhysSize(int& w, int& h) { w = 0; h = 0; }
double WindowSystem::GetWindowDPIScale() { return g_windowInfo.dpi_scale; }
double WindowSystem::GetPadDPIScale() { return 1.0; }
bool WindowSystem::IsPadWindowOpen() { return false; }
bool WindowSystem::IsKeyDown(uint32 key) { return g_windowInfo.get_keystate(key); }
bool WindowSystem::IsKeyDown(PlatformKeyCodes) { return false; }
std::string WindowSystem::GetKeyCodeName(uint32 key) { return fmt::format("key {}", key); }
bool WindowSystem::InputConfigWindowHasFocus() { return false; }
void WindowSystem::NotifyGameLoaded() {}
void WindowSystem::NotifyGameExited() {}
void WindowSystem::RefreshGameList() {}
bool WindowSystem::IsFullScreen() { return false; }
void WindowSystem::CaptureInput(const ControllerState&, const ControllerState&) {}
