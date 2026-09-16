#pragma once

/*
	SDL2 host state and engine hooks for the Linux/Switch port.

	On Android these roles were filled by cocos2d-x: AppDelegate owned the
	window/renderer and the frame pump, MainScene owned the window layer and the
	asynchronous input state. On this port:

		main.cpp       - SDL event pump, frame pacing, engine boot
		WindowLayer.cpp- iWindowLayer implementation (window, input, present)
		Host.cpp       - engine hooks MainScene.cpp used to provide
		Platform.cpp   - the Platform.h contract (files, dialogs, logging)
*/

#include "tjsCommHead.h"
#include "tvpinputdefs.h"
#include <SDL.h>

namespace krkr2sdl {

// ---------------------------------------------------------------------------
// SDL host
// ---------------------------------------------------------------------------
bool HostInit(const char *title, tjs_int w, tjs_int h, bool fullscreen, bool vsync);
void HostShutdown();
bool HostInitialized();

SDL_Window *HostWindow();
SDL_Renderer *HostRenderer();
void HostGetWindowSize(tjs_int &w, tjs_int &h);
void HostSetWindowTitle(const char *title);

// Set when SDL_QUIT / the window close button arrives; main() turns this into
// the engine's close query so the running script can veto the shutdown.
bool HostQuitRequested();
void HostRequestQuit();
void HostClearQuitRequest();

// ---------------------------------------------------------------------------
// Asynchronous input state
//
// Indexed by the engine's VK_ codes (environ/vkdefine.h, tvpinputdefs.h).
// Bit layout the engine expects from TVPGetKeyMouseAsyncState():
//   bit0 : currently down
//   bit4 : went down since the last query with getcurrent == false
// ---------------------------------------------------------------------------
void HostSetKeyState(tjs_uint vk, bool down);
bool HostGetKeyState(tjs_uint vk, bool getcurrent);
// The same table also carries the KIRIKIRI-specific game-pad codes (VK_PAD*,
// tvpinputdefs.h) — System.getKeyState addresses both through it.
tjs_uint32 HostGetShiftState();
tjs_uint32 HostGetMouseButtonShiftState();

// Mouse button -> VK_ code, the job environ/cocos2d/CCKeyCodeConv.cpp does on
// Android (tTVPMouseButton -> VK_LBUTTON/VK_RBUTTON/VK_MBUTTON/VK_XBUTTON*).
tjs_uint HostMouseButtonToVK(tTVPMouseButton btn);

// The window layer maps pointer coordinates through this so Host.cpp's cursor
// queries and the engine agree on the drawing-area coordinate system.
struct DrawArea {
	tjs_int left = 0, top = 0, width = 0, height = 0;
};
DrawArea HostGetCurrentDrawArea();
void HostWindowToDrawArea(tjs_int wx, tjs_int wy, tjs_int &dx, tjs_int &dy);

// ---------------------------------------------------------------------------
// Window layer registry (WindowLayer.cpp); TVPCreateAndAddWindow() is the
// engine-facing entry point and lives in Host.cpp.
// ---------------------------------------------------------------------------
void HostDispatchSDLEvent(const SDL_Event &e);
// Set by TVPCreateAndAddWindow() when the engine creates its first window
// layer; HostEverCreatedWindow() reports whether that ever happened, which is
// how the port tells "the game ran and exited" from "the game never started".
void HostNoteWindowCreated();
bool HostEverCreatedWindow();
// Re-blits the most recently uploaded frame (used after a resize or when a new
// layer became active). Never touches engine texture objects.
void HostPresentLastFrame();
void HostRecycleTextures();

} // namespace krkr2sdl
