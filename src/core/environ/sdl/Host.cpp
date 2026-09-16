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
