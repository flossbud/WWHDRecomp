// wwhd frontend: Cemu's WindowSystem interface, replacing the wxWidgets GUI
// (docs/recompiler-design.md D11). The window is plain Xlib for now: the SDL3 that Cemu's
// vcpkg build ships has no video backends (Cemu uses it for controllers only). Cemu's own main() parses the command line (-g GAME, ...) and
// calls WindowSystem::Create(), which here is the whole application: boot, one TV window, and an
// event loop. Single screen by design: there is no GamePad window (D17).
#include "boot.h"
#include "interface/WindowSystem.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
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

void WindowSystem::Create()
{
	SetThreadName("wwhd");
	wwhd::SetupPaths();
	wwhd::LoadConfig();
	wwhd::CreateDefaultMLCFiles();
	ActiveSettings::Init();
	LatteOverlay_init();
	CemuCommonInit();

	auto game = LaunchSettings::GetLoadFile();
	if (!game)
		wwhd::Fatal("usage: wwhd -g <game.wua | title dir | .rpx>");

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
	setSize(w, h);
	g_windowInfo.dpi_scale = 1.0;
	g_windowInfo.pad_open = false;
	g_windowInfo.app_active = true;

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
