/*
	SDL2 host implementation and the engine hooks MainScene.cpp provided on
	Android (window layer factory, per-frame draw hook, console log, platform
	names, asynchronous key/button state).

	The frame pump itself lives in main.cpp; this file owns the state shared
	between it, the window layer and the engine.
*/

#include "tjsCommHead.h"

#include "Host.h"
#include "WindowLayer.h"
#include "vkdefine.h"

#include "Application.h"
#include "DebugIntf.h"
#include "EventIntf.h"
#include "StorageIntf.h"
#include "Platform.h" // TVPSendToOtherApp
#include "MenuItemIntf.h" // tTJSNI_MenuItem
#include "SystemIntf.h"
#include "TickCount.h"
#include "ConfigManager/IndividualConfigManager.h"
#include "RenderManager.h" // iTVPTexture2D::RecycleProcess

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

//---------------------------------------------------------------------------
// host state
//---------------------------------------------------------------------------
namespace krkr2sdl {
namespace {

SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
bool g_initialized = false;
bool g_quit_requested = false;
bool g_owns_video = false;
bool g_ever_created_window = false;

// Asynchronous key state, indexed by VK_ code (environ/vkdefine.h plus the
// KIRIKIRI game-pad codes from tvpinputdefs.h).
//	bit0 : currently down
//	bit4 : went down since the last query with getcurrent == false
const tjs_uint KEY_STATE_SIZE = 0x200;
tjs_uint8 g_key_state[KEY_STATE_SIZE];

std::vector<SDL_GameController *> g_controllers;

} // namespace

bool HostInit(const char *title, tjs_int w, tjs_int h, bool fullscreen, bool vsync)
{
	if (g_initialized) return true;
	std::memset(g_key_state, 0, sizeof(g_key_state));

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
		TVPAddImportantLog(ttstr(TJS_W("SDL: cannot initialize video: ")) + SDL_GetError());
		return false;
	}
	g_owns_video = true;

	Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
	if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

	g_window = SDL_CreateWindow(title ? title : "Kirikiroid2",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
	if (!g_window) {
		TVPAddImportantLog(ttstr(TJS_W("SDL: cannot create window: ")) + SDL_GetError());
		return false;
	}

	Uint32 renderer_flags = SDL_RENDERER_ACCELERATED;
	if (vsync) renderer_flags |= SDL_RENDERER_PRESENTVSYNC;
	g_renderer = SDL_CreateRenderer(g_window, -1, renderer_flags);
	if (!g_renderer) {
		// Handhelds and remote sessions often lack a hardware backend.
		g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
	}
	if (!g_renderer) {
		TVPAddImportantLog(ttstr(TJS_W("SDL: cannot create renderer: ")) + SDL_GetError());
		return false;
	}
	SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
	SDL_RenderClear(g_renderer);
	SDL_RenderPresent(g_renderer);

	// Game pads are optional: the engine reads them through the VK_PAD* codes,
	// and SDL re-scans on hot-plug through the event pump.
	for (int i = 0; i < SDL_NumJoysticks(); ++i) {
		if (SDL_IsGameController(i)) {
			SDL_GameController *ctrl = SDL_GameControllerOpen(i);
			if (ctrl) g_controllers.push_back(ctrl);
		}
	}

	g_initialized = true;
	g_quit_requested = false;
	return true;
}

void HostShutdown()
{
	if (!g_initialized) return;
	for (SDL_GameController *ctrl : g_controllers) SDL_GameControllerClose(ctrl);
	g_controllers.clear();
	if (g_renderer) {
		SDL_DestroyRenderer(g_renderer);
		g_renderer = nullptr;
	}
	if (g_window) {
		SDL_DestroyWindow(g_window);
		g_window = nullptr;
	}
	if (g_owns_video) {
		SDL_Quit();
		g_owns_video = false;
	}
	g_initialized = false;
}

bool HostInitialized()
{
	return g_initialized;
}

bool HostEverCreatedWindow()
{
	return g_ever_created_window;
}

void HostNoteWindowCreated()
{
	g_ever_created_window = true;
}

SDL_Window *HostWindow()
{
	return g_window;
}

SDL_Renderer *HostRenderer()
{
	return g_renderer;
}

void HostGetWindowSize(tjs_int &w, tjs_int &h)
{
	w = h = 0;
	if (g_renderer) {
		SDL_GetRendererOutputSize(g_renderer, &w, &h);
	} else if (g_window) {
		SDL_GetWindowSize(g_window, &w, &h);
	}
}

void HostSetWindowTitle(const char *title)
{
	if (g_window && title) SDL_SetWindowTitle(g_window, title);
}

bool HostQuitRequested()
{
	return g_quit_requested;
}

void HostRequestQuit()
{
	g_quit_requested = true;
}

void HostClearQuitRequest()
{
	g_quit_requested = false;
}

//---------------------------------------------------------------------------
// asynchronous input state
//---------------------------------------------------------------------------
void HostSetKeyState(tjs_uint vk, bool down)
{
	if (vk >= KEY_STATE_SIZE) return;
	if (down) {
		g_key_state[vk] = 0x11;
	} else {
		g_key_state[vk] &= 0x10;
	}
}

bool HostGetKeyState(tjs_uint vk, bool getcurrent)
{
	if (vk >= KEY_STATE_SIZE) return false;
	tjs_uint8 code = g_key_state[vk];
	g_key_state[vk] &= 1;
	return 0 != (code & (getcurrent ? 1 : 0x10));
}

tjs_uint32 HostGetShiftState()
{
	tjs_uint32 f = 0;
	if (g_key_state[VK_SHIFT] & 1) f |= ssShift;
	if (g_key_state[VK_MENU] & 1) f |= ssAlt;
	if (g_key_state[VK_CONTROL] & 1) f |= ssCtrl;
	if (g_key_state[VK_LBUTTON] & 1) f |= ssLeft;
	if (g_key_state[VK_RBUTTON] & 1) f |= ssRight;
	if (g_key_state[VK_MBUTTON] & 1) f |= ssMiddle;
	return f;
}

tjs_uint32 HostGetMouseButtonShiftState()
{
	tjs_uint32 f = 0;
	if (g_key_state[VK_LBUTTON] & 1) f |= ssLeft;
	if (g_key_state[VK_RBUTTON] & 1) f |= ssRight;
	if (g_key_state[VK_MBUTTON] & 1) f |= ssMiddle;
	return f;
}

tjs_uint HostMouseButtonToVK(tTVPMouseButton btn)
{
	// environ/cocos2d/CCKeyCodeConv.cpp does this mapping on Android; the same
	// values are what Windows reports for the same buttons.
	switch (btn) {
	case mbLeft: return VK_LBUTTON;
	case mbMiddle: return VK_MBUTTON;
	case mbRight: return VK_RBUTTON;
	case mbX1: return 0x05; // VK_XBUTTON1
	case mbX2: return 0x06; // VK_XBUTTON2
	default: return 0;
	}
}

DrawArea HostGetCurrentDrawArea()
{
	DrawArea area;
	TVPSDLWindowLayer *layer = TVPSDLWindowLayer::GetPresentingLayer();
	if (layer) layer->GetDrawArea(area.left, area.top, area.width, area.height);
	return area;
}

void HostWindowToDrawArea(tjs_int wx, tjs_int wy, tjs_int &dx, tjs_int &dy)
{
	dx = wx;
	dy = wy;
	TVPSDLWindowLayer *layer = TVPSDLWindowLayer::GetPresentingLayer();
	if (layer) layer->WindowToLayer(wx, wy, dx, dy);
}

void HostDispatchSDLEvent(const SDL_Event &e)
{
	if (e.type == SDL_CONTROLLERDEVICEADDED && SDL_IsGameController(e.cdevice.which)) {
		SDL_GameController *ctrl = SDL_GameControllerOpen(e.cdevice.which);
		if (ctrl) g_controllers.push_back(ctrl);
		return;
	}
	if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
		for (auto it = g_controllers.begin(); it != g_controllers.end(); ++it) {
			SDL_Joystick *joy = SDL_GameControllerGetJoystick(*it);
			if (joy && SDL_JoystickInstanceID(joy) == e.cdevice.which) {
				SDL_GameControllerClose(*it);
				g_controllers.erase(it);
				break;
			}
		}
		return;
	}
	TVPSDLWindowLayer::HandleSDLEvent(e);
}

void HostPresentLastFrame()
{
	TVPSDLWindowLayer *layer = TVPSDLWindowLayer::GetPresentingLayer();
	if (layer) layer->PresentIfDirty();
}

void HostRecycleTextures()
{
	iTVPTexture2D::RecycleProcess();
}

//---------------------------------------------------------------------------
// Scripted input (KRKR2_TEST_INPUT) - see Host.h
//---------------------------------------------------------------------------
namespace {

struct TestInputStep
{
	int Frame = 0;
	SDL_EventType Type = SDL_FIRSTEVENT; // a finger event, or SDL_FIRSTEVENT for a pad action
	int X = 0, Y = 0;
	Uint8 Button = 0xFF;
	bool Down = true;
	bool IsPad = false;
};

std::vector<TestInputStep> g_test_input;
bool g_test_input_read = false;
SDL_GameController *g_test_pad = nullptr;

std::string TrimBlanks(const std::string &s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t')) b++;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
	return s.substr(b, e - b);
}

std::string ToLowerCopy(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), ::tolower);
	return s;
}

void ParseTestInput()
{
	g_test_input_read = true;

	const char *env = getenv("KRKR2_TEST_INPUT");
	if (!env || !*env) return;

	const std::string script(env);
	size_t pos = 0;
	while (pos <= script.size()) {
		const size_t semi = script.find(';', pos);
		std::string item = TrimBlanks(semi == std::string::npos
			? script.substr(pos) : script.substr(pos, semi - pos));
		pos = (semi == std::string::npos) ? script.size() + 1 : semi + 1;
		if (item.empty()) continue;

		const size_t c1 = item.find(':');
		if (c1 == std::string::npos) {
			TVPPrintLog(("test input: ignoring '" + item +
				"' (want <frame>:<action>[:<payload>])").c_str());
			continue;
		}
		TestInputStep step;
		step.Frame = atoi(item.substr(0, c1).c_str());
		const std::string rest = item.substr(c1 + 1);
		const size_t c2 = rest.find(':');
		const std::string head = ToLowerCopy(c2 == std::string::npos
			? rest : rest.substr(0, c2));
		const std::string payload = (c2 == std::string::npos)
			? std::string() : rest.substr(c2 + 1);

		if (head == "fingerdown" || head == "fingermove" || head == "fingerup") {
			const size_t comma = payload.find(',');
			if (comma == std::string::npos) {
				TVPPrintLog(("test input: ignoring '" + item +
					"' (want <frame>:<action>:x,y)").c_str());
				continue;
			}
			step.X = atoi(payload.substr(0, comma).c_str());
			step.Y = atoi(payload.substr(comma + 1).c_str());
			step.Type = (head == "fingerdown") ? SDL_FINGERDOWN
				: (head == "fingermove") ? SDL_FINGERMOTION : SDL_FINGERUP;
		} else if (head == "mousedown" || head == "mouseup") {
			// The same positions as real mouse input, so a test can compare what
			// the touch translation produces with what a mouse produces.
			const size_t comma = payload.find(',');
			if (comma == std::string::npos) {
				TVPPrintLog(("test input: ignoring '" + item +
					"' (want <frame>:<action>:x,y)").c_str());
				continue;
			}
			step.X = atoi(payload.substr(0, comma).c_str());
			step.Y = atoi(payload.substr(comma + 1).c_str());
			step.Type = (head == "mousedown") ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
		} else if (head == "pad" || head == "padup") {
			step.IsPad = true;
			step.Down = (head == "pad");
			step.Button = HostGamePadButtonByName(payload);
			if (step.Button == 0xFF) {
				TVPPrintLog(("test input: unknown game pad button '" + payload + "'").c_str());
				continue;
			}
		} else {
			TVPPrintLog(("test input: unknown action '" + head + "'").c_str());
			continue;
		}
		g_test_input.push_back(step);
	}

	TVPPrintLog(("test input: " + std::to_string(g_test_input.size()) +
		" scripted event(s) from KRKR2_TEST_INPUT").c_str());
}

} // namespace

Uint8 HostGamePadButtonByName(const std::string &name)
{
	std::string n(name);
	std::transform(n.begin(), n.end(), n.begin(), ::tolower);
	if (n == "a") return SDL_CONTROLLER_BUTTON_A;
	if (n == "b") return SDL_CONTROLLER_BUTTON_B;
	if (n == "x") return SDL_CONTROLLER_BUTTON_X;
	if (n == "y") return SDL_CONTROLLER_BUTTON_Y;
	if (n == "up") return SDL_CONTROLLER_BUTTON_DPAD_UP;
	if (n == "down") return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
	if (n == "left") return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
	if (n == "right") return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
	if (n == "start") return SDL_CONTROLLER_BUTTON_START;
	if (n == "back") return SDL_CONTROLLER_BUTTON_BACK;
	if (n == "lb") return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
	if (n == "rb") return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
	return 0xFF;
}

//---------------------------------------------------------------------------
// Launcher
//---------------------------------------------------------------------------
std::string HostBrowseForGame(const std::string &initial_directory,
	const std::string &preselect)
{
	// The dialog's filter is also its "type to jump" line, so a path in front of
	// it would be wrong here; the starting directory is passed separately.
	return TVPShowFileSelectorEx("Select a game (a .xp3/.7z, or a folder holding "
		"startup.tjs)", "*.xp3;*.7z", initial_directory, false, preselect);
}

static std::string LastGameFile()
{
	const std::string &dir = WritablePath();
	if (dir.empty()) return std::string();
	return dir + "/last-game.txt";
}

std::string HostReadLastGame()
{
	const std::string file = LastGameFile();
	if (file.empty()) return std::string();
	FILE *fp = fopen(file.c_str(), "rb");
	if (!fp) return std::string();
	std::string out;
	char buf[2048];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
	fclose(fp);
	// the file is written by this function, one path and a newline
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
	if (out.find('\n') != std::string::npos) out.clear(); // never valid as a path
	return out;
}

void HostWriteLastGame(const std::string &path)
{
	const std::string file = LastGameFile();
	if (file.empty() || path.find('\n') != std::string::npos) return;
	FILE *fp = fopen(file.c_str(), "wb");
	if (!fp) {
		TVPPrintLog(("launcher: cannot remember the last game in " + file).c_str());
		return;
	}
	fwrite(path.data(), 1, path.size(), fp);
	fputc('\n', fp);
	fclose(fp);
}

void HostInitTestInput()
{
	if (!g_test_input_read) ParseTestInput();
	if (g_test_input.empty()) return;

	bool wants_pad = false;
	for (const TestInputStep &step : g_test_input)
		if (step.IsPad) wants_pad = true;
	if (!wants_pad) return;

	// A virtual controller, so the button presses travel SDL's own controller
	// path (attach -> open -> set button -> SDL_CONTROLLERBUTTONDOWN) instead of
	// being pushed as pre-made events.
	if (SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 4, 15, 0) < 0) {
		TVPPrintLog(("test input: cannot attach a virtual controller: " +
			std::string(SDL_GetError())).c_str());
		return;
	}
	for (int i = 0; i < SDL_NumJoysticks(); i++) {
		if (!SDL_IsGameController(i)) continue;
		g_test_pad = SDL_GameControllerOpen(i);
		if (g_test_pad) break;
	}
	if (!g_test_pad)
		TVPPrintLog(("test input: the virtual controller did not open: " +
			std::string(SDL_GetError())).c_str());
}

void HostPumpTestInput(int frame)
{
	if (!g_test_input_read) return; // nothing scripted: no per-frame work
	if (g_test_input.empty()) return;

	tjs_int w = 0, h = 0;
	HostGetWindowSize(w, h);
	if (w <= 0) w = 1;
	if (h <= 0) h = 1;

	for (const TestInputStep &step : g_test_input) {
		if (step.Frame != frame) continue;
		if (step.IsPad) {
			if (g_test_pad)
				SDL_JoystickSetVirtualButton(SDL_GameControllerGetJoystick(g_test_pad),
					step.Button, step.Down ? SDL_PRESSED : SDL_RELEASED);
			continue;
		}
		SDL_Event e;
		SDL_zero(e);
		e.type = step.Type;
		if (step.Type == SDL_FINGERDOWN || step.Type == SDL_FINGERMOTION ||
			step.Type == SDL_FINGERUP) {
			e.tfinger.type = step.Type;
			e.tfinger.touchId = 0;
			e.tfinger.fingerId = 1; // the window layer translates the first finger
			e.tfinger.x = (float)step.X / (float)w;
			e.tfinger.y = (float)step.Y / (float)h;
			e.tfinger.pressure = 1.0f;
		} else {
			e.button.type = step.Type;
			e.button.button = SDL_BUTTON_LEFT;
			e.button.state = (step.Type == SDL_MOUSEBUTTONDOWN) ? SDL_PRESSED : SDL_RELEASED;
			e.button.clicks = 1;
			e.button.x = step.X;
			e.button.y = step.Y;
		}
		SDL_PushEvent(&e);
	}
}

} // namespace krkr2sdl

//---------------------------------------------------------------------------
// engine hooks (formerly environ/cocos2d/MainScene.cpp)
//---------------------------------------------------------------------------
static void (*_postUpdate)() = nullptr;

void TVPSetPostUpdateEvent(void (*f)())
{
	// Registered by the OpenGL render manager to restore GL state after the
	// engine has drawn; the software renderer leaves it null.
	_postUpdate = f;
}

void TVPForceSwapBuffer()
{
	// Android: eglSwapBuffers(eglGetCurrentDisplay(), eglGetCurrentSurface()).
	// Here the frame is presented from UpdateDrawBuffer(); this makes the
	// engine's "swap now" request re-blit the frame it just produced.
	krkr2sdl::HostPresentLastFrame();
}

void TVPProcessInputEvents()
{
	// SDL input is pumped by the host loop in main.cpp; input events reach the
	// engine through TVPPostInputEvent(), so there is nothing to flush here
	// (the Android implementation is empty for the same reason).
}

int TVPDrawSceneOnce(int interval)
{
	static tjs_uint64 lastTick = TVPGetRoughTickCount32();
	tjs_uint64 curTick = TVPGetRoughTickCount32();
	int remain = interval - (int)(curTick - lastTick);
	if (remain <= 0) {
		if (_postUpdate) _postUpdate();
		krkr2sdl::HostPresentLastFrame();
		lastTick = curTick;
		return 0;
	}
	return remain;
}

iWindowLayer *TVPCreateAndAddWindow(tTJSNI_Window *w)
{
	krkr2sdl::HostNoteWindowCreated();
	return new TVPSDLWindowLayer(w);
}

void TVPRemoveWindowLayer(iWindowLayer *lay)
{
	delete lay;
}

bool TVPGetKeyMouseAsyncState(tjs_uint keycode, bool getcurrent)
{
	return krkr2sdl::HostGetKeyState(keycode, getcurrent);
}

bool TVPGetJoyPadAsyncState(tjs_uint keycode, bool getcurrent)
{
	// Game-pad codes share the asynchronous state table with the keyboard
	// (VK_PAD*, tvpinputdefs.h), exactly as the Android layer did.
	return krkr2sdl::HostGetKeyState(keycode, getcurrent);
}

tjs_uint32 TVPGetCurrentShiftKeyState()
{
	return krkr2sdl::HostGetShiftState();
}

ttstr TVPGetPlatformName()
{
	// Reported to scripts as System.platformName; the engine prefers the
	// platform names used by the Win32/Android builds.
#if defined(__ANDROID__)
	return TJS_W("Android");
#elif defined(__APPLE__)
	return TJS_W("MacOS");
#elif defined(_WIN32)
	return TJS_W("Win32");
#else
	return TJS_W("Linux");
#endif
}

ttstr TVPGetOSName()
{
#if defined(__ANDROID__)
	return TJS_W("Android");
#elif defined(__APPLE__)
	return TJS_W("MacOS");
#elif defined(_WIN32)
	return TJS_W("Win32");
#else
	// glibc's uname() is not part of the engine's portable surface; the
	// platform name is what scripts compare against.
	return TJS_W("Linux");
#endif
}

//---------------------------------------------------------------------------
// Features the cocos2d UI layer provided
//
// The Android build got these from its UI layer (environ/ui/InGameMenuForm.cpp
// for the menu, environ/cocos2d/AppDelegate.cpp for the patch URL).  The file
// selector that was the third one now lives in FileSelector.cpp, drawn on the
// host SDL_Renderer; what is left here is implemented with the behaviour that
// is safe for a script to observe, and logs what happened.  Replacing the menu
// with a real SDL surface is part of the UI port.
//---------------------------------------------------------------------------

// Window "menu bar"/popup menu (MenuItem.show(), Window popup menus). There is
// no menu surface here; report and behave like a menu the user dismissed.
void TVPShowPopMenu(tTJSNI_MenuItem *menu)
{
	static bool warned = false;
	if (!warned) {
		warned = true;
		TVPAddImportantLog(TJS_W("(info) popup menus are not implemented in this build; "
			"MenuItem.show() has no effect"));
	}
}

// The "browse the patch library" button of the startup-patch error dialog.
// On Android this opened the patch page in the browser; do the same through the
// platform's URL handler (Platform.cpp).
void TVPOpenPatchLibUrl()
{
	TVPSendToOtherApp("https://zeas2.github.io/Kirikiroid2_patch/patch");
}

//---------------------------------------------------------------------------
// console logging
//---------------------------------------------------------------------------
namespace {

void WriteConsoleLine(const ttstr &message, bool important)
{
	std::string utf8 = message.AsStdString(); // tjsConfig.cpp converts UTF-16 to UTF-8
	if (utf8.empty() && !message.IsEmpty()) return;
	FILE *out = important ? stderr : stdout;
	fputs(utf8.c_str(), out);
	fputc('\n', out);
	fflush(out);
}

void WriteConsoleLine(const char *utf8)
{
	if (!utf8) return;
	fputs(utf8, stdout);
	fputc('\n', stdout);
	fflush(stdout);
}

} // namespace

void TVPConsoleLog(const ttstr &l, bool important)
{
	static bool logging_to_console = true;
	static bool initialized = false;
	if (!initialized) {
		initialized = true;
		logging_to_console = IndividualConfigManager::GetInstance()->GetValue<bool>("outputlog", true);
	}
	if (!logging_to_console) return;
	WriteConsoleLine(l, important);
}

void TVPConsoleLog(const tjs_char *l)
{
	if (!l) return;
	WriteConsoleLine(ttstr(l), false);
}

namespace TJS {

void TVPConsoleLog(const tjs_char *l)
{
	if (!l) return;
	WriteConsoleLine(ttstr(l), false);
}

void TVPConsoleLog(const tjs_nchar *format, ...)
{
	const int MAX_LOG_LENGTH = 16 * 1024;
	char buf[MAX_LOG_LENGTH];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, MAX_LOG_LENGTH - 3, format, args);
	va_end(args);
	WriteConsoleLine(buf);
}

} // namespace TJS
