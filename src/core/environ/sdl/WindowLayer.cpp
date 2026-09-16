/*
	SDL2 implementation of iWindowLayer (see WindowLayer.h).

	Reference implementations this mirrors:
		environ/win32/WindowFormUnit.cpp  - the Win32 native window form
		environ/cocos2d/MainScene.cpp     - the Android window layer (cocos2d)

	Like MainScene's TVPWindowLayer, all engine windows share one OS window;
	unlike it, there is no scene graph: the composited frame buffer the draw
	device hands over (UpdateDrawBuffer) is streamed into an SDL texture and
	blitted, letterboxed and zoomed, into the window.
*/

#include "tjsCommHead.h"

#include "WindowLayer.h"
#include "Host.h"
#include "vkdefine.h"

#include "DebugIntf.h"
#include "EventIntf.h"
#include "MsgIntf.h"
#include "TickCount.h"
#include "Random.h" // TVPPushEnvironNoise
#include "Application.h" // mrOk / mrCancel (close-query results)
#include "StorageIntf.h"
#include "RenderManager.h" // iTVPTexture2D

#include <algorithm>
#include <cstring>
#include <vector>

//---------------------------------------------------------------------------
// layer registry
//---------------------------------------------------------------------------
namespace {
	std::vector<TVPSDLWindowLayer *> g_layers;
	TVPSDLWindowLayer *g_current_layer = nullptr;
}

const std::vector<TVPSDLWindowLayer *> &TVPSDLWindowLayer::GetLayers()
{
	return g_layers;
}

TVPSDLWindowLayer *TVPSDLWindowLayer::GetCurrent()
{
	return g_current_layer;
}

void TVPSDLWindowLayer::SetCurrent(TVPSDLWindowLayer *layer)
{
	if (g_current_layer == layer) return;
	g_current_layer = layer;
	// The newly activated window must be presented on the next frame.
	if (layer) layer->Dirty = true;
}

TVPSDLWindowLayer *TVPSDLWindowLayer::GetPresentingLayer()
{
	if (g_current_layer && g_current_layer->Visible) return g_current_layer;
	// No active layer (e.g. during startup): present the topmost visible one.
	for (auto it = g_layers.rbegin(); it != g_layers.rend(); ++it) {
		if ((*it)->Visible) return *it;
	}
	return nullptr;
}

//---------------------------------------------------------------------------
// key translation (SDL keycode -> engine VK_ code, see environ/vkdefine.h)
//---------------------------------------------------------------------------
static tjs_uint SDLKeycodeToVK(SDL_Keycode key)
{
	// SDL keycodes for letters and digits are their ASCII values, which is
	// also where the VK_ letter/digit codes live.
	if (key >= SDLK_a && key <= SDLK_z) return VK_A + (key - SDLK_a);
	if (key >= SDLK_0 && key <= SDLK_9) return VK_0 + (key - SDLK_0);
	if (key >= SDLK_F1 && key <= SDLK_F24) return VK_F1 + (key - SDLK_F1);
	if (key >= SDLK_KP_0 && key <= SDLK_KP_9) return VK_NUMPAD0 + (key - SDLK_KP_0);

	switch (key) {
	case SDLK_BACKSPACE: return VK_BACK;
	case SDLK_TAB: return VK_TAB;
	case SDLK_CLEAR: return VK_CLEAR;
	case SDLK_RETURN: return VK_RETURN;
	case SDLK_KP_ENTER: return VK_RETURN;
	case SDLK_PAUSE: return VK_PAUSE;
	case SDLK_CAPSLOCK: return VK_CAPITAL;
	case SDLK_ESCAPE: return VK_ESCAPE;
	case SDLK_SPACE: return VK_SPACE;
	case SDLK_PAGEUP: return VK_PRIOR;
	case SDLK_PAGEDOWN: return VK_NEXT;
	case SDLK_END: return VK_END;
	case SDLK_HOME: return VK_HOME;
	case SDLK_LEFT: return VK_LEFT;
	case SDLK_UP: return VK_UP;
	case SDLK_RIGHT: return VK_RIGHT;
	case SDLK_DOWN: return VK_DOWN;
	case SDLK_SELECT: return VK_SELECT;
	case SDLK_PRINTSCREEN: return VK_SNAPSHOT;
	case SDLK_INSERT: return VK_INSERT;
	case SDLK_DELETE: return VK_DELETE;
	case SDLK_HELP: return VK_HELP;
	case SDLK_LSHIFT: return VK_SHIFT;
	case SDLK_RSHIFT: return VK_SHIFT;
	case SDLK_LCTRL: return VK_CONTROL;
	case SDLK_RCTRL: return VK_CONTROL;
	case SDLK_LALT: return VK_MENU;
	case SDLK_RALT: return VK_MENU;
	case SDLK_LGUI: return VK_LWIN;
	case SDLK_RGUI: return VK_RWIN;
	case SDLK_APPLICATION: return VK_APPS;
	case SDLK_NUMLOCKCLEAR: return VK_NUMLOCK;
	case SDLK_SCROLLLOCK: return VK_SCROLL;
	case SDLK_KP_MULTIPLY: return VK_MULTIPLY;
	case SDLK_KP_PLUS: return VK_ADD;
	case SDLK_KP_MINUS: return VK_SUBTRACT;
	case SDLK_KP_PERIOD: return VK_DECIMAL;
	case SDLK_KP_DIVIDE: return VK_DIVIDE;
	case SDLK_KP_EQUALS: return VK_SEPARATOR;
	case SDLK_KP_COMMA: return VK_SEPARATOR;
	// US-layout OEM keys, the values Windows uses for the same physical keys
	case SDLK_SEMICOLON: return VK_OEM_1;
	case SDLK_EQUALS: return VK_OEM_PLUS;
	case SDLK_COMMA: return VK_OEM_COMMA;
	case SDLK_MINUS: return VK_OEM_MINUS;
	case SDLK_PERIOD: return VK_OEM_PERIOD;
	case SDLK_SLASH: return VK_OEM_2;
	case SDLK_BACKQUOTE: return VK_OEM_3;
	case SDLK_LEFTBRACKET: return VK_OEM_4;
	case SDLK_BACKSLASH: return VK_OEM_5;
	case SDLK_RIGHTBRACKET: return VK_OEM_6;
	case SDLK_QUOTE: return VK_OEM_7;
	case SDLK_KP_CLEAR: return VK_OEM_CLEAR;
	default: return 0;
	}
}

//---------------------------------------------------------------------------
// game pad (SDL_GameController -> the engine's KIRIKIRI-specific VK_PAD* codes)
//---------------------------------------------------------------------------
static tjs_uint SDLGameControllerButtonToVK(Uint8 button)
{
	switch (button) {
	case SDL_CONTROLLER_BUTTON_A: return VK_PAD1;
	case SDL_CONTROLLER_BUTTON_B: return VK_PAD2;
	case SDL_CONTROLLER_BUTTON_X: return VK_PAD3;
	case SDL_CONTROLLER_BUTTON_Y: return VK_PAD4;
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return VK_PAD5;
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return VK_PAD6;
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return VK_PADLEFT;
	case SDL_CONTROLLER_BUTTON_DPAD_UP: return VK_PADUP;
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return VK_PADRIGHT;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return VK_PADDOWN;
	case SDL_CONTROLLER_BUTTON_START: return VK_RETURN;
	case SDL_CONTROLLER_BUTTON_BACK: return VK_ESCAPE;
	case SDL_CONTROLLER_BUTTON_GUIDE: return VK_PAD9;
	case SDL_CONTROLLER_BUTTON_LEFTSTICK: return VK_PAD10;
	case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return VK_PAD8;
	default: return 0;
	}
}

// Trigger axes are reported as buttons by SDL; the engine has no dedicated
// codes for L2/R2 (krkrz maps them to the remaining PAD slots).
static tjs_uint SDLGameControllerAxisButtonToVK(Uint8 axis)
{
	return axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? VK_PAD7 : VK_PAD_LAST;
}

//---------------------------------------------------------------------------
// helpers
//---------------------------------------------------------------------------
static void AdjustNumerAndDenom(tjs_int &n, tjs_int &d)
{
	tjs_int a = n, b = d;
	while (b) {
		tjs_int t = b;
		b = a % b;
		a = t;
	}
	if (a) {
		n /= a;
		d /= a;
	}
}

static inline tjs_int MulDiv(tjs_int value, tjs_int numer, tjs_int denom)
{
	return (tjs_int)((tjs_int64)value * numer / denom);
}

// Mouse-key emulation tuning, same values as the Win32 form
// (environ/win32/WindowFormUnit.h).
static const int TVP_MOUSE_MAX_ACCEL = 30;
static const int TVP_MOUSE_SHIFT_ACCEL = 40;

// Provided by environ/sdl/Host.cpp.
int TVPDrawSceneOnce(int interval);

static tjs_int GetShiftState()
{
	return (int)krkr2sdl::HostGetShiftState();
}

//---------------------------------------------------------------------------
// construction / destruction
//---------------------------------------------------------------------------
TVPSDLWindowLayer::TVPSDLWindowLayer(tTJSNI_Window *owner)
	: TJSNativeInstance(owner)
{
	DefaultImeMode = imDisable;
	ImeMode = DefaultImeMode;
	g_layers.push_back(this);
	if (!g_current_layer) g_current_layer = this;
	krkr2sdl::HostGetWindowSize(DestWidth, DestHeight);
	RecalcDrawArea();
}

TVPSDLWindowLayer::~TVPSDLWindowLayer()
{
	DestroyPresentTexture();
	auto it = std::find(g_layers.begin(), g_layers.end(), this);
	if (it != g_layers.end()) g_layers.erase(it);
	if (g_current_layer == this) {
		g_current_layer = g_layers.empty() ? nullptr : g_layers.back();
	}
	// The engine window object is gone; only reachable through
	// tTJSNI_Window::Invalidate(). With no window left there is nothing to
	// display, so the host shuts down (the Win32 form likewise quits the
	// message loop when its main window is destroyed).
	if (g_layers.empty()) krkr2sdl::HostRequestQuit();
}

//---------------------------------------------------------------------------
// geometry
//---------------------------------------------------------------------------
void TVPSDLWindowLayer::RecalcDrawArea()
{
	tjs_int ww = 0, hh = 0;
	krkr2sdl::HostGetWindowSize(ww, hh);

	if (LayerWidth <= 0 || LayerHeight <= 0 || ww <= 0 || hh <= 0) {
		DestLeft = DestTop = 0;
		DestWidth = ww;
		DestHeight = hh;
		return;
	}

	const double zoom = (double)ActualZoomNumer / (double)ActualZoomDenom;
	double want_w = LayerWidth * zoom;
	double want_h = LayerHeight * zoom;
	// Fit (and, in fullscreen, letterbox) the paint box into the window. When
	// the engine sized the window to the paint box this factor is 1.
	double fit = std::min((double)ww / want_w, (double)hh / want_h);
	DestWidth = (tjs_int)(want_w * fit + 0.5);
	DestHeight = (tjs_int)(want_h * fit + 0.5);
	DestLeft = (ww - DestWidth) / 2;
	DestTop = (hh - DestHeight) / 2;
	Dirty = true;
}

void TVPSDLWindowLayer::TranslateWindowToLayer(tjs_int wx, tjs_int wy, tjs_int &lx, tjs_int &ly) const
{
	if (DestWidth <= 0 || DestHeight <= 0) {
		lx = 0;
		ly = 0;
		return;
	}
	lx = MulDiv(wx - DestLeft, LayerWidth, DestWidth);
	ly = MulDiv(wy - DestTop, LayerHeight, DestHeight);
}

void TVPSDLWindowLayer::WindowToLayer(tjs_int wx, tjs_int wy, tjs_int &lx, tjs_int &ly) const
{
	TranslateWindowToLayer(wx, wy, lx, ly);
}

//---------------------------------------------------------------------------
// Touch
//---------------------------------------------------------------------------
void TVPSDLWindowLayer::FingerToWindow(const SDL_TouchFingerEvent &f,
	tjs_int &wx, tjs_int &wy) const
{
	// tfinger carries the position normalised over the window (0..1), which is
	// what a touch device reports; everything else in this class works in window
	// pixels.  Values are clamped so a finger that drifted outside the window
	// during a drag still lands on it.
	tjs_int w = 0, h = 0;
	const_cast<TVPSDLWindowLayer *>(this)->GetWinSize(w, h);
	if (w <= 0) w = 1;
	if (h <= 0) h = 1;
	wx = (tjs_int)(f.x * (float)w);
	wy = (tjs_int)(f.y * (float)h);
	if (wx < 0) wx = 0;
	if (wx >= w) wx = w - 1;
	if (wy < 0) wy = 0;
	if (wy >= h) wy = h - 1;
}

void TVPSDLWindowLayer::GetDrawArea(tjs_int &left, tjs_int &top, tjs_int &width, tjs_int &height) const
{
	left = DestLeft;
	top = DestTop;
	width = DestWidth;
	height = DestHeight;
}

// Resize the OS window to the paint box (what SetInnerSize does on Win32); the
// window manager may refuse (fullscreen, tiling WMs, handhelds), in which case
// RecalcDrawArea() letterboxes instead.
void TVPSDLWindowLayer::SetWindowSizeFromPaintBox()
{
	if (FullScreen || LayerWidth <= 0 || LayerHeight <= 0) return;
	if (!krkr2sdl::HostWindow()) return;
	SDL_SetWindowSize(krkr2sdl::HostWindow(),
		MulDiv(LayerWidth, ActualZoomNumer, ActualZoomDenom),
		MulDiv(LayerHeight, ActualZoomNumer, ActualZoomDenom));
	OnWindowResized();
}

void TVPSDLWindowLayer::OnWindowResized()
{
	if (FullScreen) {
		RecalcDrawArea();
	} else {
		RecalcDrawArea();
	}
}

void TVPSDLWindowLayer::OnWindowFocusChanged(bool focused)
{
	Active = focused;
	// The engine tracks activation through events with the host.
	TVPPostInputEvent(new tTVPOnWindowActivateEvent(TJSNativeInstance, focused));
}

void TVPSDLWindowLayer::SetPaintBoxSize(tjs_int w, tjs_int h)
{
	if (LayerWidth == w && LayerHeight == h) return;
	LayerWidth = w;
	LayerHeight = h;
	SetWindowSizeFromPaintBox();
	RecalcDrawArea();
}

void TVPSDLWindowLayer::SetWidth(tjs_int w)
{
	SetSize(w, LayerHeight);
}

void TVPSDLWindowLayer::SetHeight(tjs_int h)
{
	SetSize(LayerWidth, h);
}

void TVPSDLWindowLayer::SetSize(tjs_int w, tjs_int h)
{
	LayerWidth = w;
	LayerHeight = h;
	SetWindowSizeFromPaintBox();
	RecalcDrawArea();
}

void TVPSDLWindowLayer::GetSize(tjs_int &w, tjs_int &h)
{
	w = LayerWidth;
	h = LayerHeight;
}

void TVPSDLWindowLayer::GetWinSize(tjs_int &w, tjs_int &h)
{
	// window size expressed in paint-box units (same as the Win32 form)
	tjs_int ww = 0, hh = 0;
	krkr2sdl::HostGetWindowSize(ww, hh);
	if (ActualZoomNumer <= 0 || ActualZoomDenom <= 0) {
		w = ww;
		h = hh;
		return;
	}
	w = MulDiv(ww, ActualZoomDenom, ActualZoomNumer);
	h = MulDiv(hh, ActualZoomDenom, ActualZoomNumer);
}

tjs_int TVPSDLWindowLayer::GetWidth() const
{
	return LayerWidth;
}

tjs_int TVPSDLWindowLayer::GetHeight() const
{
	return LayerHeight;
}

void TVPSDLWindowLayer::SetZoom(tjs_int numer, tjs_int denom)
{
	if (numer <= 0 || denom <= 0) return;
	AdjustNumerAndDenom(numer, denom);
	ZoomNumer = numer;
	ZoomDenom = denom;
	ActualZoomNumer = numer;
	ActualZoomDenom = denom;
	SetWindowSizeFromPaintBox();
	RecalcDrawArea();
}

void TVPSDLWindowLayer::ZoomRectangle(tjs_int &left, tjs_int &top, tjs_int &right, tjs_int &bottom)
{
	left = MulDiv(left, ActualZoomNumer, ActualZoomDenom);
	top = MulDiv(top, ActualZoomNumer, ActualZoomDenom);
	right = MulDiv(right, ActualZoomNumer, ActualZoomDenom);
	bottom = MulDiv(bottom, ActualZoomNumer, ActualZoomDenom);
}

void TVPSDLWindowLayer::SetFullScreenMode(bool bFullScreen)
{
	if (FullScreen == bFullScreen) return;
	SDL_Window *window = krkr2sdl::HostWindow();
	if (window) {
		if (SDL_SetWindowFullscreen(window, bFullScreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) {
			TVPAddLog(ttstr(TJS_W("SDL: failed to switch full screen mode: ")) +
				SDL_GetError());
			return;
		}
	}
	FullScreen = bFullScreen;
	RecalcDrawArea();
}

bool TVPSDLWindowLayer::GetFullScreenMode()
{
	return FullScreen;
}

//---------------------------------------------------------------------------
// presentation
//---------------------------------------------------------------------------
void TVPSDLWindowLayer::EnsurePresentTexture(tjs_int w, tjs_int h)
{
	SDL_Renderer *renderer = krkr2sdl::HostRenderer();
	if (!renderer || w <= 0 || h <= 0) return;
	if (PresentTexture && PresentTextureW == w && PresentTextureH == h) return;
	DestroyPresentTexture();
	// TVP's in-memory pixel order is R,G,B,A: Layer.Fill stores
	// TVP_REVRGB(0xRRGGBB) (LayerBitmapIntf.cpp), the PNG/BMP loaders copy
	// channels in that order and the BMP writer reverses them again on the way
	// out.  SDL's ABGR8888 is exactly R,G,B,A in memory on little-endian
	// (ARGB8888 would swap red and blue on screen).
	PresentTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
		SDL_TEXTUREACCESS_STREAMING, w, h);
	if (!PresentTexture) {
		TVPAddLog(ttstr(TJS_W("SDL: cannot create frame texture: ")) + SDL_GetError());
		return;
	}
	SDL_SetTextureBlendMode(PresentTexture, SDL_BLENDMODE_NONE);
	PresentTextureW = w;
	PresentTextureH = h;
}

void TVPSDLWindowLayer::DestroyPresentTexture()
{
	if (PresentTexture) SDL_DestroyTexture(PresentTexture);
	PresentTexture = nullptr;
	PresentTextureW = PresentTextureH = 0;
}

void TVPSDLWindowLayer::UpdateDrawBuffer(iTVPTexture2D *tex)
{
	if (!tex) return;
	// The Win32/Android draw devices present right after the update (the
	// latter through TVPForceSwapBuffer -> eglSwapBuffers); do the same so the
	// script's drawing becomes visible without waiting for the frame pump.
	UploadFrame(tex);
}

void TVPSDLWindowLayer::UploadFrame(iTVPTexture2D *tex)
{
	const void *pixels = tex->GetPixelData();
	if (!pixels) {
		// Only software textures expose their pixels. The OpenGL renderer
		// (visual/ogl) is not part of this build, so this cannot happen with
		// the software render manager this port selects.
		static bool warned = false;
		if (!warned) {
			warned = true;
			TVPAddLog(TJS_W("krkr2: frame buffer has no CPU pixels; "
				"this build supports the software renderer only"));
		}
		return;
	}

	tjs_uint tex_w = tex->GetWidth();
	tjs_uint tex_h = tex->GetHeight();
	tjs_int pitch = tex->GetPitch();
	if (tex_w == 0 || tex_h == 0 || pitch <= 0) return;

	// A texture may hold a larger internal surface than the visible paint box
	// (and may carry a scale factor for half-resolution rendering).
	float scale_x = 1.f, scale_y = 1.f;
	tex->GetScale(scale_x, scale_y);
	tjs_int src_w = (tjs_int)tex_w;
	tjs_int src_h = (tjs_int)tex_h;
	if (scale_x != 1.f && LayerWidth > 0)
		src_w = (tjs_int)(tex->GetInternalWidth() * ((float)LayerWidth / tex_w));
	if (scale_y != 1.f && LayerHeight > 0)
		src_h = (tjs_int)(tex->GetInternalHeight() * ((float)LayerHeight / tex_h));
	src_w = std::min<tjs_int>(src_w, (tjs_int)tex_w);
	src_h = std::min<tjs_int>(src_h, (tjs_int)tex_h);
	if (src_w <= 0 || src_h <= 0) return;

	EnsurePresentTexture(src_w, src_h);
	if (!PresentTexture) return;

	SDL_Rect src;
	src.x = 0;
	src.y = 0;
	src.w = src_w;
	src.h = src_h;
	if (SDL_UpdateTexture(PresentTexture, &src, pixels, pitch) != 0) {
		TVPAddLog(ttstr(TJS_W("SDL: cannot update frame texture: ")) + SDL_GetError());
		return;
	}
	HasFrame = true;
	Dirty = true;
	PresentLastFrame();
}

void TVPSDLWindowLayer::PresentIfDirty()
{
	if (Dirty) PresentLastFrame();
}

void TVPSDLWindowLayer::PresentLastFrame()
{
	if (!HasFrame || !PresentTexture) return;
	if (GetPresentingLayer() != this) return;
	SDL_Renderer *renderer = krkr2sdl::HostRenderer();
	if (!renderer) return;

	SDL_Rect src;
	src.x = 0;
	src.y = 0;
	src.w = PresentTextureW;
	src.h = PresentTextureH;

	SDL_Rect dst;
	dst.x = DestLeft;
	dst.y = DestTop;
	dst.w = DestWidth;
	dst.h = DestHeight;

	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, PresentTexture, &src, &dst);

	SDL_RenderPresent(renderer);
	Dirty = false;
}

//---------------------------------------------------------------------------
// visibility / caption / window state
//---------------------------------------------------------------------------
bool TVPSDLWindowLayer::GetFormEnabled()
{
	return true;
}

bool TVPSDLWindowLayer::GetVisible()
{
	return Visible;
}

void TVPSDLWindowLayer::SyncVisibleState()
{
	SDL_Window *window = krkr2sdl::HostWindow();
	if (!window) return;
	if (Visible) {
		SDL_ShowWindow(window);
	} else {
		SDL_HideWindow(window);
	}
	if (Visible) {
		SetCurrent(this);
		Dirty = true;
	}
}

void TVPSDLWindowLayer::SetVisible(bool bVisible)
{
	if (Visible == bVisible && VisibleSent) return;
	Visible = bVisible;
	VisibleSent = true;
	SyncVisibleState();
}

void TVPSDLWindowLayer::SetVisibleFromScript(bool b)
{
	SetVisible(b);
}

void TVPSDLWindowLayer::BringToFront()
{
	SDL_Window *window = krkr2sdl::HostWindow();
	if (window) SDL_RaiseWindow(window);
	SetCurrent(this);
	RecalcDrawArea();
}

void TVPSDLWindowLayer::ShowWindowAsModal()
{
	Modal = true;
	ModalResult = 0;
	SetVisible(true);
	BringToFront();
}

const char *TVPSDLWindowLayer::GetCaption()
{
	return Caption.c_str();
}

void TVPSDLWindowLayer::SyncCaption()
{
	SDL_Window *window = krkr2sdl::HostWindow();
	if (window) SDL_SetWindowTitle(window, Caption.c_str());
}

void TVPSDLWindowLayer::SetCaption(const std::string &caption)
{
	Caption = caption;
	SyncCaption();
}

void TVPSDLWindowLayer::TickBeat()
{
	// Keep the emulated mouse-key moving while it is held down in the active
	// window (the Win32 form and MainScene do the same from their timer).
	if (UseMouseKey && GetWindowActive()) {
		GenerateMouseEvent(false, false, false, false);
	}
}

void TVPSDLWindowLayer::UpdateWindow(tTVPUpdateType type)
{
	if (!TJSNativeInstance) return;
	tTVPRect r;
	r.left = 0;
	r.top = 0;
	r.right = LayerWidth;
	r.bottom = LayerHeight;
	TJSNativeInstance->NotifyWindowExposureToLayer(r);
	TVPDeliverWindowUpdateEvents();
}

bool TVPSDLWindowLayer::GetWindowActive()
{
	SDL_Window *window = krkr2sdl::HostWindow();
	bool focused = window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
	return focused && GetCurrent() == this;
}

//---------------------------------------------------------------------------
// closing
//---------------------------------------------------------------------------
void TVPSDLWindowLayer::InvalidateClose()
{
	// Closing action by object invalidation: no user confirmation, the layer
	// is destroyed (Win32 form: DestroyWindow + delete this).
	TJSNativeInstance = nullptr;
	Visible = false;
	SDL_Window *window = krkr2sdl::HostWindow();
	if (window) SDL_HideWindow(window);
	delete this;
}

void TVPSDLWindowLayer::QueryUserClose()
{
	// The user closed the OS window. Ask the running script first
	// (onCloseQuery), exactly like the Win32 WM_CLOSE handling.
	if (Closing) return;
	if (Modal) {
		ModalResult = mrCancel;
	}
	Closing = true;
	TVPPostInputEvent(new tTVPOnCloseInputEvent(TJSNativeInstance));
}

void TVPSDLWindowLayer::Close()
{
	// Closing action by the "close" method.
	if (Closing) return;
	ProgramClosing = true;
	try {
		Closing = true;
		if (OnCloseQueryWork()) {
			TVPPostInputEvent(new tTVPOnCloseInputEvent(TJSNativeInstance));
		} else {
			Closing = false;
		}
	} catch (...) {
		ProgramClosing = false;
		throw;
	}
	ProgramClosing = false;
}

// Shared by Close(): the immediate close query used when the program itself
// asks to close (Win32 runs the onCloseQuery handler synchronously then).
bool TVPSDLWindowLayer::OnCloseQueryWork()
{
	if (!TJSNativeInstance) return true;
	iTJSDispatch2 *obj = TJSNativeInstance->GetOwnerNoAddRef();
	if (!obj) return true;

	tTJSVariant arg[1] = { true };
	static ttstr eventname(TJS_W("onCloseQuery"));
	CanCloseWork = false;
	TVPPostEvent(obj, obj, eventname, 0, TVP_EPT_IMMEDIATE, 1, arg);
	TVPDrawSceneOnce(0);
	return CanCloseWork;
}

void TVPSDLWindowLayer::OnCloseQueryCalled(bool b)
{
	// The script answered the close query (tTVPOnCloseInputEvent delivery).
	if (!ProgramClosing) {
		// Closing action by the user: hide the window (the game object stays
		// alive, as on Win32/Android) or keep it open.
		if (b) {
			if (Modal) {
				ModalResult = 1;
			} else {
				SetVisible(false);
			}
			if (TJSNativeInstance && TJSNativeInstance->IsMainWindow()) {
				iTJSDispatch2 *obj = TJSNativeInstance->GetOwnerNoAddRef();
				obj->Invalidate(0, NULL, NULL, obj);
			}
		}
		Closing = false;
	} else {
		CanCloseWork = b;
	}
}

//---------------------------------------------------------------------------
// cursor
//---------------------------------------------------------------------------
void TVPSDLWindowLayer::SetDefaultMouseCursor()
{
	CursorHiddenByEngine = false;
	SDL_ShowCursor(SDL_ENABLE);
}

void TVPSDLWindowLayer::UpdateCursorVisibility()
{
	// SDL has one system cursor per window; the engine's state maps onto it.
	bool visible = MouseCursorState == mcsVisible;
	SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
	if (!visible && SDL_GetMouseFocus() != nullptr) {
		// keep the engine's idea of the cursor position when it is hidden
	}
}

void TVPSDLWindowLayer::SetMouseCursorState(tTVPMouseCursorState mcs)
{
	MouseCursorState = mcs;
	UpdateCursorVisibility();
}

void TVPSDLWindowLayer::GetCursorPos(tjs_int &x, tjs_int &y)
{
	tjs_int wx = 0, wy = 0;
	SDL_GetMouseState(&wx, &wy);
	TranslateWindowToLayer(wx, wy, x, y);
}

void TVPSDLWindowLayer::SetCursorPos(tjs_int x, tjs_int y)
{
	SDL_Window *window = krkr2sdl::HostWindow();
	if (!window || DestWidth <= 0 || DestHeight <= 0) return;
	tjs_int wx = DestLeft + MulDiv(x, DestWidth, LayerWidth);
	tjs_int wy = DestTop + MulDiv(y, DestHeight, LayerHeight);
	SDL_WarpMouseInWindow(window, wx, wy);
}

void TVPSDLWindowLayer::SetHintText(const ttstr &text)
{
	HintText = text;
}

void TVPSDLWindowLayer::SetAttentionPoint(tjs_int left, tjs_int top, const struct tTVPFont *font)
{
	// No separate hint window on this platform; hints are shown by the game's
	// own layers. Nothing to do beyond remembering the request.
}

//---------------------------------------------------------------------------
// mouse
//---------------------------------------------------------------------------
int TVPSDLWindowLayer::GetMouseButtonShiftState() const
{
	return (int)krkr2sdl::HostGetMouseButtonShiftState();
}

void TVPSDLWindowLayer::PostMouseDown(tTVPMouseButton button, tjs_int lx, tjs_int ly)
{
	if (!TJSNativeInstance) return;
	krkr2sdl::HostSetKeyState(krkr2sdl::HostMouseButtonToVK(button), true);
	LastMouseDownX = lx;
	LastMouseDownY = ly;
	LastMouseDownTick = TVPGetRoughTickCount32();
	TVPPostInputEvent(new tTVPOnMouseDownInputEvent(TJSNativeInstance, lx, ly, button,
		(tjs_uint32)GetShiftState() | (tjs_uint32)GetMouseButtonShiftState()));
}

void TVPSDLWindowLayer::PostMouseUp(tTVPMouseButton button, tjs_int lx, tjs_int ly)
{
	if (!TJSNativeInstance) return;
	bool was_down = krkr2sdl::HostGetKeyState(krkr2sdl::HostMouseButtonToVK(button), true);
	krkr2sdl::HostSetKeyState(krkr2sdl::HostMouseButtonToVK(button), false);
	if (!was_down) return;
	TVPPostInputEvent(new tTVPOnMouseUpInputEvent(TJSNativeInstance, lx, ly, button,
		(tjs_uint32)GetShiftState() | (tjs_uint32)GetMouseButtonShiftState()));
	// A click is a press+release on the same button (Win32 sends WM_LBUTTONUP
	// followed by the click notification).
	PostMouseClick(button, lx, ly);
}

void TVPSDLWindowLayer::PostMouseClick(tTVPMouseButton button, tjs_int lx, tjs_int ly)
{
	if (!TJSNativeInstance) return;
	if (button != mbLeft) return;
	bool dbl = (TVPGetRoughTickCount32() - LastMouseDownTick) < 500;
	if (dbl) {
		TVPPostInputEvent(new tTVPOnDoubleClickInputEvent(TJSNativeInstance,
			LastMouseDownX, LastMouseDownY));
	}
	TVPPostInputEvent(new tTVPOnClickInputEvent(TJSNativeInstance, LastMouseDownX, LastMouseDownY));
}

void TVPSDLWindowLayer::OnMouseMove(TVPSDLWindowLayer *self, tjs_int wx, tjs_int wy)
{
	tjs_int lx = 0, ly = 0;
	TranslateWindowToLayer(wx, wy, lx, ly);
	LastMouseX = lx;
	LastMouseY = ly;
	MouseVelocityTracker.addMovement(TVPGetRoughTickCount32(), (float)lx, (float)ly);
	if (!TJSNativeInstance) return;
	TVPPostInputEvent(new tTVPOnMouseMoveInputEvent(TJSNativeInstance, lx, ly,
		(tjs_uint32)GetShiftState() | (tjs_uint32)GetMouseButtonShiftState()),
		TVP_EPT_DISCARDABLE);
}

void TVPSDLWindowLayer::ResetMouseVelocity()
{
	MouseVelocityTracker.clear();
}

void TVPSDLWindowLayer::ResetTouchVelocity(tjs_int id)
{
	TouchVelocityTracker.end(id);
}

bool TVPSDLWindowLayer::GetMouseVelocity(float &x, float &y, float &speed) const
{
	if (MouseVelocityTracker.getVelocity(x, y)) {
		speed = sqrtf(x * x + y * y);
		return true;
	}
	speed = 0;
	return false;
}

void TVPSDLWindowLayer::SetUseMouseKey(bool b)
{
	UseMouseKey = b;
	MouseLeftButtonEmulatedPushed = false;
	MouseRightButtonEmulatedPushed = false;
	MouseKeyXAccel = MouseKeyYAccel = 0;
	LastMouseKeyTick = TVPGetRoughTickCount32();
}

bool TVPSDLWindowLayer::GetUseMouseKey() const
{
	return UseMouseKey;
}

void TVPSDLWindowLayer::GenerateMouseEvent(bool fl, bool fr, bool fu, bool fd)
{
	if (!fl && !fr && !fu && !fd) {
		if (TVPGetRoughTickCount32() - 45 < LastMouseKeyTick) return;
	}

	bool shift = 0 != (krkr2sdl::HostGetKeyState(VK_SHIFT, true));
	bool left = fl || krkr2sdl::HostGetKeyState(VK_LEFT, true) || krkr2sdl::HostGetKeyState(VK_PADLEFT, true);
	bool right = fr || krkr2sdl::HostGetKeyState(VK_RIGHT, true) || krkr2sdl::HostGetKeyState(VK_PADRIGHT, true);
	bool up = fu || krkr2sdl::HostGetKeyState(VK_UP, true) || krkr2sdl::HostGetKeyState(VK_PADUP, true);
	bool down = fd || krkr2sdl::HostGetKeyState(VK_DOWN, true) || krkr2sdl::HostGetKeyState(VK_PADDOWN, true);

	bool moved = false;
	if (left || right || up || down) {
		if (!shift) {
			if (left) MouseKeyXAccel = (MouseKeyXAccel > 0) ? -2 : MouseKeyXAccel - 2;
			if (right) MouseKeyXAccel = (MouseKeyXAccel < 0) ? 2 : MouseKeyXAccel + 2;
			if (!left && !right) MouseKeyXAccel /= 2;
			if (up) MouseKeyYAccel = (MouseKeyYAccel > 0) ? -2 : MouseKeyYAccel - 2;
			if (down) MouseKeyYAccel = (MouseKeyYAccel < 0) ? 2 : MouseKeyYAccel + 2;
			if (!up && !down) MouseKeyYAccel /= 2;
		} else {
			MouseKeyXAccel = (left ? -TVP_MOUSE_SHIFT_ACCEL : 0) + (right ? TVP_MOUSE_SHIFT_ACCEL : 0);
			MouseKeyYAccel = (up ? -TVP_MOUSE_SHIFT_ACCEL : 0) + (down ? TVP_MOUSE_SHIFT_ACCEL : 0);
		}
		MouseKeyXAccel = std::max(-TVP_MOUSE_MAX_ACCEL, std::min(TVP_MOUSE_MAX_ACCEL, MouseKeyXAccel));
		MouseKeyYAccel = std::max(-TVP_MOUSE_MAX_ACCEL, std::min(TVP_MOUSE_MAX_ACCEL, MouseKeyYAccel));
		if (MouseKeyXAccel || MouseKeyYAccel) {
			LastMouseX += MouseKeyXAccel >> 1;
			LastMouseY += MouseKeyYAccel >> 1;
			LastMouseX = std::max(0, std::min(LayerWidth - 1, LastMouseX));
			LastMouseY = std::max(0, std::min(LayerHeight - 1, LastMouseY));
			moved = true;
		}
	} else {
		MouseKeyXAccel = MouseKeyYAccel = 0;
	}

	if (moved && TJSNativeInstance) {
		SDL_Window *window = krkr2sdl::HostWindow();
		if (window && DestWidth > 0 && DestHeight > 0) {
			tjs_int wx = DestLeft + MulDiv(LastMouseX, DestWidth, LayerWidth);
			tjs_int wy = DestTop + MulDiv(LastMouseY, DestHeight, LayerHeight);
			SDL_WarpMouseInWindow(window, wx, wy);
		}
		TVPPostInputEvent(new tTVPOnMouseMoveInputEvent(TJSNativeInstance, LastMouseX, LastMouseY,
			(tjs_uint32)GetShiftState() | (tjs_uint32)GetMouseButtonShiftState()),
			TVP_EPT_DISCARDABLE);
	}
	LastMouseKeyTick = TVPGetRoughTickCount32();
}

//---------------------------------------------------------------------------
// keyboard
//---------------------------------------------------------------------------
void TVPSDLWindowLayer::InternalKeyDown(tjs_uint16 key, tjs_uint32 shift)
{
	tjs_uint32 tick = TVPGetRoughTickCount32();
	TVPPushEnvironNoise(&tick, sizeof(tick));
	TVPPushEnvironNoise(&key, sizeof(key));
	TVPPushEnvironNoise(&shift, sizeof(shift));

	if (!TJSNativeInstance) return;

	if (UseMouseKey) {
		// While the mouse-key mode is on, the decision keys and the arrow keys
		// drive the virtual pointer instead of the game.
		if (key == VK_RETURN || key == VK_SPACE || key == VK_ESCAPE ||
			key == VK_PAD1 || key == VK_PAD2) {
			if (LastMouseX >= 0 && LastMouseY >= 0 &&
				LastMouseX < LayerWidth && LastMouseY < LayerHeight) {
				if (key == VK_RETURN || key == VK_SPACE || key == VK_PAD1) {
					MouseLeftButtonEmulatedPushed = true;
					PostMouseDown(mbLeft, LastMouseX, LastMouseY);
				}
				if (key == VK_ESCAPE || key == VK_PAD2) {
					MouseRightButtonEmulatedPushed = true;
					PostMouseDown(mbRight, LastMouseX, LastMouseY);
				}
			}
			return;
		}
		switch (key) {
		case VK_LEFT:
		case VK_PADLEFT:
			if (MouseKeyXAccel == 0 && MouseKeyYAccel == 0)
				GenerateMouseEvent(true, false, false, false);
			return;
		case VK_RIGHT:
		case VK_PADRIGHT:
			if (MouseKeyXAccel == 0 && MouseKeyYAccel == 0)
				GenerateMouseEvent(false, true, false, false);
			return;
		case VK_UP:
		case VK_PADUP:
			if (MouseKeyXAccel == 0 && MouseKeyYAccel == 0)
				GenerateMouseEvent(false, false, true, false);
			return;
		case VK_DOWN:
		case VK_PADDOWN:
			if (MouseKeyXAccel == 0 && MouseKeyYAccel == 0)
				GenerateMouseEvent(false, false, false, true);
			return;
		default:
			break;
		}
	}

	TVPPostInputEvent(new tTVPOnKeyDownInputEvent(TJSNativeInstance, key, shift));
}

void TVPSDLWindowLayer::InternalKeyUp(tjs_uint16 key, tjs_uint32 shift)
{
	if (!TJSNativeInstance) return;

	if (UseMouseKey) {
		if (MouseLeftButtonEmulatedPushed &&
			(key == VK_RETURN || key == VK_SPACE || key == VK_PAD1)) {
			MouseLeftButtonEmulatedPushed = false;
			PostMouseUp(mbLeft, LastMouseX, LastMouseY);
			return;
		}
		if (MouseRightButtonEmulatedPushed &&
			(key == VK_ESCAPE || key == VK_PAD2)) {
			MouseRightButtonEmulatedPushed = false;
			PostMouseUp(mbRight, LastMouseX, LastMouseY);
			return;
		}
	}

	TVPPostInputEvent(new tTVPOnKeyUpInputEvent(TJSNativeInstance, key, shift));
}

void TVPSDLWindowLayer::OnKeyUp(tjs_uint16 vk, int shift)
{
	tjs_uint32 s = (tjs_uint32)shift;
	s |= (tjs_uint32)GetMouseButtonShiftState();
	InternalKeyUp(vk, s);
}

void TVPSDLWindowLayer::OnKeyPress(tjs_uint16 vk, int repeat, bool prevkeystate, bool convertkey)
{
	if (!TJSNativeInstance || !vk) return;
	if (UseMouseKey && (vk == 0x1b || vk == 13 || vk == 32)) return;
	TVPPostInputEvent(new tTVPOnKeyPressInputEvent(TJSNativeInstance, vk));
}

//---------------------------------------------------------------------------
// IME
//---------------------------------------------------------------------------
tTVPImeMode TVPSDLWindowLayer::GetDefaultImeMode() const
{
	return DefaultImeMode;
}

void TVPSDLWindowLayer::SetImeMode(tTVPImeMode mode)
{
	ImeMode = mode;
	switch (mode) {
	case imDisable:
	case imClose:
		if (ImeVisible) {
			SDL_StopTextInput();
			ImeVisible = false;
		}
		break;
	default:
		// SDL delivers composed text through SDL_TEXTINPUT; on-screen
		// keyboards appear where the platform provides one (Switch, phones).
		if (!ImeVisible) {
			SDL_StartTextInput();
			ImeVisible = true;
		}
		break;
	}
}

void TVPSDLWindowLayer::ResetImeMode()
{
	SetImeMode(DefaultImeMode);
}

//---------------------------------------------------------------------------
// SDL event handling
//---------------------------------------------------------------------------
void TVPSDLWindowLayer::HandleSDLEvent(const SDL_Event &e)
{
	TVPSDLWindowLayer *layer = GetCurrent();
	if (!layer) return;

	switch (e.type) {
	case SDL_WINDOWEVENT:
		switch (e.window.event) {
		case SDL_WINDOWEVENT_SIZE_CHANGED:
		case SDL_WINDOWEVENT_RESIZED:
			layer->OnWindowResized();
			break;
		case SDL_WINDOWEVENT_FOCUS_GAINED:
			layer->OnWindowFocusChanged(true);
			break;
		case SDL_WINDOWEVENT_FOCUS_LOST:
			layer->OnWindowFocusChanged(false);
			break;
		case SDL_WINDOWEVENT_CLOSE:
			layer->QueryUserClose();
			break;
		default:
			break;
		}
		break;

	case SDL_MOUSEMOTION:
		// Ignore the mouse events SDL synthesises from touch when the platform
		// also delivers finger events (see SDL_FINGERDOWN below): the tap would
		// otherwise be handled twice.
		if (e.motion.which != SDL_TOUCH_MOUSEID || SDL_GetNumTouchDevices() == 0)
			layer->OnMouseMove(layer, e.motion.x, e.motion.y);
		break;

	case SDL_FINGERDOWN: {
		// Touch input becomes mouse input: the engine's windows only take mouse
		// events, and a tap is how a game is driven on a touchscreen (the Switch
		// in handheld mode, phones, tablets).  Only the first finger is
		// translated; while it is down, its moves drag the cursor.
		if (layer->ActiveTouchFinger != 0) break;
		layer->ActiveTouchFinger = e.tfinger.fingerId;
		tjs_int wx = 0, wy = 0;
		layer->FingerToWindow(e.tfinger, wx, wy);
		tjs_int lx = 0, ly = 0;
		layer->TranslateWindowToLayer(wx, wy, lx, ly);
		layer->LastMouseX = lx;
		layer->LastMouseY = ly;
		layer->PostMouseDown(mbLeft, lx, ly);
		break;
	}

	case SDL_FINGERMOTION: {
		if (e.tfinger.fingerId != layer->ActiveTouchFinger) break;
		tjs_int wx = 0, wy = 0;
		layer->FingerToWindow(e.tfinger, wx, wy);
		layer->OnMouseMove(layer, wx, wy);
		break;
	}

	case SDL_FINGERUP: {
		if (e.tfinger.fingerId != layer->ActiveTouchFinger) break;
		layer->ActiveTouchFinger = 0;
		tjs_int wx = 0, wy = 0;
		layer->FingerToWindow(e.tfinger, wx, wy);
		tjs_int lx = 0, ly = 0;
		layer->TranslateWindowToLayer(wx, wy, lx, ly);
		layer->LastMouseX = lx;
		layer->LastMouseY = ly;
		layer->PostMouseUp(mbLeft, lx, ly);
		break;
	}

	case SDL_MOUSEBUTTONDOWN: {
		if (e.button.which == SDL_TOUCH_MOUSEID && SDL_GetNumTouchDevices() > 0) break;
		tjs_int lx = 0, ly = 0;
		layer->TranslateWindowToLayer(e.button.x, e.button.y, lx, ly);
		layer->LastMouseX = lx;
		layer->LastMouseY = ly;
		switch (e.button.button) {
		case SDL_BUTTON_LEFT: layer->PostMouseDown(mbLeft, lx, ly); break;
		case SDL_BUTTON_RIGHT: layer->PostMouseDown(mbRight, lx, ly); break;
		case SDL_BUTTON_MIDDLE: layer->PostMouseDown(mbMiddle, lx, ly); break;
		case SDL_BUTTON_X1: layer->PostMouseDown(mbX1, lx, ly); break;
		case SDL_BUTTON_X2: layer->PostMouseDown(mbX2, lx, ly); break;
		default: break;
		}
		break;
	}

	case SDL_MOUSEBUTTONUP: {
		if (e.button.which == SDL_TOUCH_MOUSEID && SDL_GetNumTouchDevices() > 0) break;
		tjs_int lx = 0, ly = 0;
		layer->TranslateWindowToLayer(e.button.x, e.button.y, lx, ly);
		layer->LastMouseX = lx;
		layer->LastMouseY = ly;
		switch (e.button.button) {
		case SDL_BUTTON_LEFT: layer->PostMouseUp(mbLeft, lx, ly); break;
		case SDL_BUTTON_RIGHT: layer->PostMouseUp(mbRight, lx, ly); break;
		case SDL_BUTTON_MIDDLE: layer->PostMouseUp(mbMiddle, lx, ly); break;
		case SDL_BUTTON_X1: layer->PostMouseUp(mbX1, lx, ly); break;
		case SDL_BUTTON_X2: layer->PostMouseUp(mbX2, lx, ly); break;
		default: break;
		}
		break;
	}

	case SDL_MOUSEWHEEL: {
		tjs_int wx = 0, wy = 0;
		SDL_GetMouseState(&wx, &wy);
		tjs_int lx = 0, ly = 0;
		layer->TranslateWindowToLayer(wx, wy, lx, ly);
		int delta = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
		if (delta == 0) delta = e.wheel.x;
		if (delta && layer->TJSNativeInstance) {
			TVPPostInputEvent(new tTVPOnMouseWheelInputEvent(layer->TJSNativeInstance,
				(tjs_uint32)GetShiftState() | (tjs_uint32)layer->GetMouseButtonShiftState(),
				delta, lx, ly));
		}
		break;
	}

	case SDL_KEYDOWN: {
		tjs_uint vk = SDLKeycodeToVK(e.key.keysym.sym);
		if (!vk || vk >= 0x200) break;
		krkr2sdl::HostSetKeyState(vk, true);
		tjs_uint32 shift = (tjs_uint32)GetShiftState() | (tjs_uint32)layer->GetMouseButtonShiftState();
		if (e.key.repeat) shift |= TVP_SS_REPEAT;
		layer->InternalKeyDown((tjs_uint16)vk, shift);
		break;
	}

	case SDL_KEYUP: {
		tjs_uint vk = SDLKeycodeToVK(e.key.keysym.sym);
		if (!vk || vk >= 0x200) break;
		bool is_pressed = krkr2sdl::HostGetKeyState(vk, true);
		krkr2sdl::HostSetKeyState(vk, false);
		if (is_pressed) layer->OnKeyUp((tjs_uint16)vk, GetShiftState());
		break;
	}

	case SDL_TEXTINPUT: {
		if (!layer->ImeVisible) break;
		// UTF-8 -> UTF-16 code units, one OnKeyPress per unit (this is how the
		// cocos path delivered IME text on Android).
		std::u16string text = std::u16string();
		const unsigned char *p = (const unsigned char *)e.text.text;
		while (*p) {
			tjs_char ch = 0;
			if (*p < 0x80) {
				ch = *p++;
			} else if ((*p & 0xE0) == 0xC0 && p[1]) {
				ch = (tjs_char)(((p[0] & 0x1F) << 6) | (p[1] & 0x3F));
				p += 2;
			} else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
				ch = (tjs_char)(((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
				p += 3;
			} else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
				// outside the BMP: emit the UTF-16 surrogate pair
				unsigned int cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
					((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
				cp -= 0x10000;
				text.push_back((tjs_char)(0xD800 + (cp >> 10)));
				ch = (tjs_char)(0xDC00 + (cp & 0x3FF));
				p += 4;
			} else {
				p++;
				continue;
			}
			text.push_back(ch);
		}
		for (tjs_char ch : text) layer->OnKeyPress(ch, 0, false, false);
		break;
	}

	case SDL_CONTROLLERBUTTONDOWN: {
		tjs_uint vk = SDLGameControllerButtonToVK((Uint8)e.cbutton.button);
		// Only 0 means "no mapping": the KiriKiri game-pad codes this produces
		// (VK_PAD*, 0x1B0-0x1C9, tvpinputdefs.h) are above 0x200, and the guard
		// used to be `vk >= 0x200`, so no pad button ever reached the engine.
		if (!vk) break;
		krkr2sdl::HostSetKeyState(vk, true);
		layer->InternalKeyDown((tjs_uint16)vk,
			(tjs_uint32)GetShiftState() | (tjs_uint32)layer->GetMouseButtonShiftState());
		break;
	}

	case SDL_CONTROLLERBUTTONUP: {
		tjs_uint vk = SDLGameControllerButtonToVK((Uint8)e.cbutton.button);
		if (!vk) break;
		bool is_pressed = krkr2sdl::HostGetKeyState(vk, true);
		krkr2sdl::HostSetKeyState(vk, false);
		if (is_pressed) layer->OnKeyUp((tjs_uint16)vk, GetShiftState());
		break;
	}

	default:
		break;
	}
}
