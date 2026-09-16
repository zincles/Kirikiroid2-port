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

// Name -> SDL controller button for the scripted-input helpers ("a", "b", "x",
// "y", "up", "down", "left", "right", "start", "back", "lb", "rb"); 0xFF when
// the name is unknown.  Shared by KRKR2_DIALOG_KEYS (SDLDialog.cpp) and
// KRKR2_TEST_INPUT below so both scripts spell game-pad buttons the same way.
Uint8 HostGamePadButtonByName(const std::string &name);

// ---------------------------------------------------------------------------
// Scripted input (KRKR2_TEST_INPUT)
//
// A touchscreen and a game pad are what the Switch port is for, and neither is
// attached to the machine that builds this - nor to the offscreen video driver
// the tests run under - so KRKR2_TEST_INPUT replays a scripted sequence of input
// events into the ordinary SDL event queue, in engine frames:
//
//   KRKR2_TEST_INPUT="30:fingerdown:160,120;34:fingermove:200,140;38:fingerup:200,140"
//   KRKR2_TEST_INPUT="50:pad:a;56:padup:a"
//
// Actions are "<frame>:<action>[:<payload>]" separated by ';':
//   fingerdown:X,Y / fingermove:X,Y / fingerup:X,Y
//       pushed as SDL_FINGER* events with the position given in *window pixels*
//       (converted to the normalised form a touch device reports), so what the
//       window layer's touch-to-mouse translation does is what gets exercised.
//   pad:<name> / padup:<name>
//       pressed and released on a virtual SDL controller attached by
//       HostInitTestInput(), so the events travel SDL's own controller path.
//
// Test-only: with the variable unset both functions do nothing.
void HostInitTestInput();
void HostPumpTestInput(int frame);

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
