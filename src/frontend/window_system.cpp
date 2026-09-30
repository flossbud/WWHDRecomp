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
#include "../os/input.h"
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

// ---- input: the keyboard and one gamepad, as the Pro Controller (src/os/input.cpp) ----------------
// Keys as the reference's controller0.xml has them (tools/reference/press.sh): A=X B=Z X=S Y=A L=Q
// R=W ZL=1 ZR=2 +=Return -=Backspace, D-pad on the arrows, left stick I/J/K/L, right stick T/F/G/H.
// Gamepad face buttons go by their printed label (A is A on Nintendo and Xbox pads; on PlayStation
// pads by the Xbox positions), triggers are ZL/ZR.
static std::mutex s_gamepadLock;
static SDL_Gamepad* s_gamepad = nullptr;

static void OpenGamepad(SDL_JoystickID id)
{
	std::lock_guard lock(s_gamepadLock);
	if (s_gamepad)
		return;
	s_gamepad = SDL_OpenGamepad(id);
	if (s_gamepad)
		cemuLog_log(LogType::Force, "wwhd: gamepad {}", SDL_GetGamepadName(s_gamepad));
}

static void CloseGamepad(SDL_JoystickID id)
{
	{
		std::lock_guard lock(s_gamepadLock);
		if (!s_gamepad || SDL_GetGamepadID(s_gamepad) != id)
			return;
		SDL_CloseGamepad(s_gamepad);
		s_gamepad = nullptr;
	}
	int n = 0;
	if (SDL_JoystickID* ids = SDL_GetGamepads(&n))
	{
		if (n > 0)
			OpenGamepad(ids[0]);
		SDL_free(ids);
	}
}

static uint32 FaceButton(SDL_GamepadButtonLabel label)
{
	using namespace wwhd::os::input;
	switch (label)
	{
	case SDL_GAMEPAD_BUTTON_LABEL_A: case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return A;
	case SDL_GAMEPAD_BUTTON_LABEL_B: case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return B;
	case SDL_GAMEPAD_BUTTON_LABEL_X: case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return X;
	case SDL_GAMEPAD_BUTTON_LABEL_Y: case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return Y;
	default: return 0;
	}
}

// a stick from SDL's axes: up positive, a round dead zone, at most 1 long
static void Stick(SDL_Gamepad* g, SDL_GamepadAxis ax, SDL_GamepadAxis ay, float& x, float& y)
{
	constexpr float kDeadZone = 0.15f;
	float sx = SDL_GetGamepadAxis(g, ax) / 32767.0f, sy = -SDL_GetGamepadAxis(g, ay) / 32767.0f;
	float len = std::sqrt(sx * sx + sy * sy);
	if (len <= kDeadZone)
		return;
	float scale = std::min(1.0f, (len - kDeadZone) / (1.0f - kDeadZone)) / len;
	x = std::clamp(x + sx * scale, -1.0f, 1.0f);
	y = std::clamp(y + sy * scale, -1.0f, 1.0f);
}

static void PublishInput()
{
	using namespace wwhd::os::input;
	Pad pad;
	const bool* key = SDL_GetKeyboardState(nullptr);
	static const std::pair<SDL_Scancode, uint32> kKeys[] = { { SDL_SCANCODE_X, A }, { SDL_SCANCODE_Z, B },
		{ SDL_SCANCODE_S, X }, { SDL_SCANCODE_A, Y }, { SDL_SCANCODE_Q, L }, { SDL_SCANCODE_W, R }, { SDL_SCANCODE_1, ZL },
		{ SDL_SCANCODE_2, ZR }, { SDL_SCANCODE_BACKSPACE, MINUS }, { SDL_SCANCODE_UP, UP }, { SDL_SCANCODE_DOWN, DOWN },
		{ SDL_SCANCODE_LEFT, LEFT }, { SDL_SCANCODE_RIGHT, RIGHT } };
	for (auto [k, b] : kKeys)
		if (key[k])
			pad.buttons |= b;
	if (key[SDL_SCANCODE_RETURN] && !(SDL_GetModState() & SDL_KMOD_ALT))   // Alt+Enter is fullscreen
		pad.buttons |= PLUS;
	pad.lx = (float)key[SDL_SCANCODE_L] - key[SDL_SCANCODE_J];
	pad.ly = (float)key[SDL_SCANCODE_I] - key[SDL_SCANCODE_K];
	pad.rx = (float)key[SDL_SCANCODE_H] - key[SDL_SCANCODE_F];
	pad.ry = (float)key[SDL_SCANCODE_T] - key[SDL_SCANCODE_G];
	{
		std::lock_guard lock(s_gamepadLock);
		if (SDL_Gamepad* g = s_gamepad)
		{
			for (SDL_GamepadButton b : { SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH })
				if (SDL_GetGamepadButton(g, b))
					pad.buttons |= FaceButton(SDL_GetGamepadButtonLabel(g, b));
			static const std::pair<SDL_GamepadButton, uint32> kButtons[] = { { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, L },
				{ SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, R }, { SDL_GAMEPAD_BUTTON_START, PLUS }, { SDL_GAMEPAD_BUTTON_BACK, MINUS },
				{ SDL_GAMEPAD_BUTTON_GUIDE, HOME }, { SDL_GAMEPAD_BUTTON_DPAD_UP, UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, DOWN },
				{ SDL_GAMEPAD_BUTTON_DPAD_LEFT, LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, RIGHT },
				{ SDL_GAMEPAD_BUTTON_LEFT_STICK, LCLICK }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, RCLICK } };
			for (auto [b, to] : kButtons)
				if (SDL_GetGamepadButton(g, b))
					pad.buttons |= to;
			if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384)
				pad.buttons |= ZL;
			if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384)
				pad.buttons |= ZR;
			Stick(g, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, pad.lx, pad.ly);
			Stick(g, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, pad.rx, pad.ry);
		}
	}
	SetLive(pad);
}

static void Rumble(bool on)
{
	std::lock_guard lock(s_gamepadLock);
	if (s_gamepad)
		SDL_RumbleGamepad(s_gamepad, on ? 0xC000 : 0, on ? 0xC000 : 0, on ? 10000 : 0);   // until stopped (10 s at most)
}

// WWHD_WINDOW=1: the TV window, handed to the renderer; nullptr otherwise
static SDL_Window* OpenWindow()
{
	const char* on = getenv("WWHD_WINDOW");
	if (!on || strcmp(on, "1") != 0)
		return nullptr;
	if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
		wwhd::Fatal(fmt::format("SDL video: {}", SDL_GetError()));
	if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
		cemuLog_log(LogType::Force, "wwhd: no gamepads: {}", SDL_GetError());   // the keyboard still works
	wwhd::os::input::SetRumble(Rumble);
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
		case SDL_EVENT_WINDOW_FOCUS_LOST: g_windowInfo.app_active = false; PublishInput(); break;
		case SDL_EVENT_KEY_DOWN:
			if (!ev.key.repeat && (ev.key.key == SDLK_F11 || (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))))
				SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
			PublishInput();
			break;
		case SDL_EVENT_KEY_UP:
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
			PublishInput();
			break;
		case SDL_EVENT_GAMEPAD_ADDED: OpenGamepad(ev.gdevice.which); break;
		case SDL_EVENT_GAMEPAD_REMOVED: CloseGamepad(ev.gdevice.which); PublishInput(); break;
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
