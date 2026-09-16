#pragma once

/*
	SDL2 implementation of iWindowLayer for the Linux/Switch port.

	On Android this role was filled by TVPWindowLayer in
	environ/cocos2d/MainScene.cpp (a cocos2d ScrollView backed by the
	cocos2d-x renderer and its GLView). Here every TVP window object owns one
	SDL-backed layer; all layers share the single host SDL window/renderer, and
	the layer that is currently active (BringToFront / SetVisible) is the one
	whose frame buffer is presented.

	Coordinate systems:
		layer/paint-box  - what the engine renders into (SetPaintBoxSize)
		draw area        - where that buffer lands inside the window
		                   (letterboxed fit, times the ZoomNumer/ZoomDenom zoom)
		window           - real window pixels; input arrives in these
*/

#include "tjsCommHead.h"
#include "WindowIntf.h" // iWindowLayer, tTJSNI_Window, input event classes
#include "VelocityTracker.h"

#include <SDL.h>
#include <string>
#include <vector>

class TVPSDLWindowLayer : public iWindowLayer
{
public:
	explicit TVPSDLWindowLayer(tTJSNI_Window *owner);
	~TVPSDLWindowLayer() override;

	// ------------------------------------------------------------------
	// iWindowLayer
	// ------------------------------------------------------------------
	void SetPaintBoxSize(tjs_int w, tjs_int h) override;
	bool GetFormEnabled() override;
	void SetDefaultMouseCursor() override;
	void GetCursorPos(tjs_int &x, tjs_int &y) override;
	void SetCursorPos(tjs_int x, tjs_int y) override;
	void SetHintText(const ttstr &text) override;
	void SetAttentionPoint(tjs_int left, tjs_int top, const struct tTVPFont *font) override;
	void ZoomRectangle(tjs_int &left, tjs_int &top, tjs_int &right, tjs_int &bottom) override;
	void BringToFront() override;
	void ShowWindowAsModal() override;
	bool GetVisible() override;
	void SetVisible(bool bVisible) override;
	const char *GetCaption() override;
	void SetCaption(const std::string &caption) override;
	void SetWidth(tjs_int w) override;
	void SetHeight(tjs_int h) override;
	void SetSize(tjs_int w, tjs_int h) override;
	void GetSize(tjs_int &w, tjs_int &h) override;
	tjs_int GetWidth() const override;
	tjs_int GetHeight() const override;
	void GetWinSize(tjs_int &w, tjs_int &h) override;
	void SetZoom(tjs_int numer, tjs_int denom) override;
	void SetFullScreenMode(bool bFullScreen) override;
	bool GetFullScreenMode() override;
	void SetMouseCursorState(tTVPMouseCursorState mcs) override;
	void UpdateDrawBuffer(iTVPTexture2D *tex) override;
	void InvalidateClose() override;
	bool GetWindowActive() override;
	void Close() override;
	void OnCloseQueryCalled(bool b) override;
	void InternalKeyDown(tjs_uint16 key, tjs_uint32 shift) override;
	void OnKeyUp(tjs_uint16 vk, int shift) override;
	void OnKeyPress(tjs_uint16 vk, int repeat, bool prevkeystate, bool convertkey) override;
	tTVPImeMode GetDefaultImeMode() const override;
	void SetImeMode(tTVPImeMode mode) override;
	void ResetImeMode() override;
	void UpdateWindow(tTVPUpdateType type) override;
	void SetVisibleFromScript(bool b) override;
	void SetUseMouseKey(bool b) override;
	bool GetUseMouseKey() const override;
	void ResetMouseVelocity() override;
	void ResetTouchVelocity(tjs_int id) override;
	bool GetMouseVelocity(float &x, float &y, float &speed) const override;
	void TickBeat() override;

	// ------------------------------------------------------------------
	// host interface (called from environ/sdl/Host.cpp and main.cpp)
	// ------------------------------------------------------------------
	static void HandleSDLEvent(const SDL_Event &e);
	void PresentIfDirty();
	// Re-blits the texture uploaded by the last UpdateDrawBuffer() call; used
	// after a resize or a layer switch, and never touches engine textures.
	void PresentLastFrame();
	void OnWindowResized();
	void OnWindowFocusChanged(bool focused);
	// Where the paint box currently lands inside the window, and the
	// window->paint-box coordinate mapping (used by Host.h's helpers).
	void GetDrawArea(tjs_int &left, tjs_int &top, tjs_int &width, tjs_int &height) const;
	void WindowToLayer(tjs_int wx, tjs_int wy, tjs_int &lx, tjs_int &ly) const;
	// The user asked to close the OS window: run the engine's close query.
	void QueryUserClose();

	tTJSNI_Window *GetWindow() const { return TJSNativeInstance; }

	// Layers that exist right now, in creation order.
	static const std::vector<TVPSDLWindowLayer *> &GetLayers();
	static TVPSDLWindowLayer *GetCurrent();
	static void SetCurrent(TVPSDLWindowLayer *layer);
	// Layer owning the game window the user should see, or nullptr.
	static TVPSDLWindowLayer *GetPresentingLayer();

private:
	void RecalcDrawArea();
	void SyncVisibleState();
	void SyncCaption();
	void SetWindowSizeFromPaintBox();
	void EnsurePresentTexture(tjs_int w, tjs_int h);
	void DestroyPresentTexture();
	void UploadFrame(iTVPTexture2D *tex);
	void TranslateWindowToLayer(tjs_int wx, tjs_int wy, tjs_int &lx, tjs_int &ly) const;
	void UpdateCursorVisibility();
	int GetMouseButtonShiftState() const;
	// Runs the script's onCloseQuery handler synchronously (program-initiated
	// close, as the Win32 form does); returns whether closing is allowed.
	bool OnCloseQueryWork();
	void InternalKeyUp(tjs_uint16 key, tjs_uint32 shift);

	void OnMouseMove(TVPSDLWindowLayer *self, tjs_int wx, tjs_int wy);
	void PostMouseDown(tTVPMouseButton button, tjs_int lx, tjs_int ly);
	void PostMouseUp(tTVPMouseButton button, tjs_int lx, tjs_int ly);
	void PostMouseClick(tTVPMouseButton button, tjs_int lx, tjs_int ly);
	void GenerateMouseEvent(bool left, bool right, bool up, bool down);
	// Finger coordinates are normalised over the window; the engine's input is
	// mouse input, so a touch becomes a left-button press at the same place.
	void FingerToWindow(const SDL_TouchFingerEvent &f, tjs_int &wx, tjs_int &wy) const;

	// ------------------------------------------------------------------
	// state
	// ------------------------------------------------------------------
	tTJSNI_Window *TJSNativeInstance;

	// paint box (layer pixels) and zoom
	tjs_int LayerWidth = 0, LayerHeight = 0;
	tjs_int ActualZoomDenom = 1;
	tjs_int ActualZoomNumer = 1;
	bool FullScreen = false;
	bool FullScreenBefore = false;

	// draw area inside the window, derived from the above
	tjs_int DestLeft = 0, DestTop = 0, DestWidth = 0, DestHeight = 0;

	// presentation
	SDL_Texture *PresentTexture = nullptr;
	tjs_int PresentTextureW = 0, PresentTextureH = 0;
	bool HasFrame = false;


	bool Dirty = false;

	// window state
	std::string Caption;
	bool Visible = false, VisibleSent = false;
	bool Active = true;
	bool Closing = false, ProgramClosing = false, CanCloseWork = false;
	bool Modal = false;
	int ModalResult = 0;
	bool CloseAllowed = false;

	// input
	tjs_int LastMouseX = 0, LastMouseY = 0;
	tjs_int LastMouseDownX = 0, LastMouseDownY = 0;
	tjs_uint32 LastMouseDownTick = 0;
	tjs_uint32 LastMouseKeyTick = 0;
	bool MouseLeftButtonEmulatedPushed = false, MouseRightButtonEmulatedPushed = false;
	bool UseMouseKey = false;
	tjs_int MouseKeyXAccel = 0, MouseKeyYAccel = 0;
	VelocityTracker MouseVelocityTracker;
	// The finger currently translated into mouse input (0: none).  A second
	// finger is a pinch, not a click, so it is ignored.
	SDL_FingerID ActiveTouchFinger = 0;
	VelocityTrackers TouchVelocityTracker;

	// ime / cursor
	tTVPImeMode DefaultImeMode = imDisable;
	tTVPImeMode ImeMode = imDisable;
	bool ImeVisible = false;
	ttstr HintText;
	bool CursorHiddenByEngine = false;
};
