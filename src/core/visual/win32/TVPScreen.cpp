#include "tjsCommHead.h"

#include "TVPScreen.h"
#include "Application.h"

#include <SDL.h>

//---------------------------------------------------------------------------
// Desktop metrics of the primary display.
//
// Android replaced this with a fixed virtual screen (2048 px wide) because the
// cocos2d GLView did the scaling; here the engine window maps 1:1 onto the
// display, so the SDL display bounds are the right answer (this is also what
// the Win32 form does with GetSystemMetrics / GetMonitorInfo).
//---------------------------------------------------------------------------
int tTVPScreen::GetWidth() {
	SDL_Rect r;
	if (SDL_GetDisplayUsableBounds(0, &r) != 0) return 0;
	return r.w;
}

int tTVPScreen::GetHeight() {
	SDL_Rect r;
	if (SDL_GetDisplayUsableBounds(0, &r) != 0) return 0;
	return r.h;
}

int tTVPScreen::GetDesktopLeft() {
	SDL_Rect r;
	if (SDL_GetDisplayUsableBounds(0, &r) != 0) return 0;
	return r.x;
}

int tTVPScreen::GetDesktopTop() {
	SDL_Rect r;
	if (SDL_GetDisplayUsableBounds(0, &r) != 0) return 0;
	return r.y;
}

int tTVPScreen::GetDesktopWidth() {
	return GetWidth();
}

int tTVPScreen::GetDesktopHeight() {
	return GetHeight();
}
