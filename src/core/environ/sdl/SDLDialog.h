#pragma once

/*
	Shared helpers for the synchronous modal dialogs that are drawn on the host
	SDL_Renderer: the input box (Platform.cpp, TVPShowSimpleInputBox) and the
	file selector (FileSelector.cpp, TVPShowFileSelector).

	Why this exists: SDL2 has no text API, and the engine's own font machinery
	(visual/FontImpl) rasterizes through the render manager's layer bitmaps --
	which cannot be blitted onto the SDL_Renderer these dialogs draw on, and
	which are driven by the engine's frame pump that a modal dialog replaces.
	Both dialogs therefore rasterize their text with FreeType directly (linked
	as PkgConfig::FREETYPE) and share that code here rather than growing a
	second text renderer.

	This module also owns SDL's single text-input state on behalf of the
	dialogs.  The engine's TVPShowIME()/TVPHideIME() only *record* what the
	engine wants while a dialog is up, and the dialog restores that state when
	it closes, so an IME can never be left half-active by a dialog.

	Deterministic input: KRKR2_DIALOG_KEYS drives a dialog from a scripted
	event sequence.  The events are pushed into SDL's queue, so the dialog's
	ordinary SDL_PollEvent/SDL_WaitEventTimeout dispatch handles them exactly
	as it handles a keyboard, mouse or game pad -- nothing is bypassed.
*/

#include "tjsCommHead.h"

#include <SDL.h>

#include <string>
#include <vector>

namespace krkr2sdl {

//---------------------------------------------------------------------------
// UTF-8 helpers
//
// SDL delivers text as UTF-8 (SDL_TEXTINPUT) and the engine's ttstr converts
// to and from UTF-8, so edited strings are kept as UTF-8 and the caret is a
// byte index that must stay on a code point boundary.  Malformed bytes are
// treated as one-byte code points so caret arithmetic can never loop forever
// on a broken sequence.
//---------------------------------------------------------------------------
size_t UTF8SeqLen(unsigned char c);
size_t UTF8Next(const std::string &s, size_t i);
size_t UTF8Prev(const std::string &s, size_t i);
tjs_uint32 UTF8Decode(const std::string &s, size_t i, size_t &len);
// Byte offset of the n'th character of `s` (over-long n clamps to the end).
size_t UTF8CharToByte(const std::string &s, int n);

//---------------------------------------------------------------------------
// FreeType text rendering
//
// A glyph run is composited into one ARGB8888 surface and uploaded as a
// texture per draw.  Dialog text is short and only redrawn when something
// changes, so there is nothing to gain from a glyph texture cache.
//---------------------------------------------------------------------------

// Prepares the shared font at `pixel_size`, loading candidates on demand.
// Returns false when no font file could be found at all, in which case the
// dialog must not be shown: a legible dialog is the requirement, and a frame
// of empty boxes is not one.
bool PrepareDialogFont(int pixel_size);
int DialogFontPixelSize();
int DialogLineHeight();
int MeasureGlyphRun(const std::string &utf8);
int MeasureChar(tjs_uint32 cp);
int DrawGlyphRun(SDL_Renderer *renderer, const std::string &utf8, int x, int y,
	SDL_Color color);
// Greedy word wrap that also breaks words longer than the available width
// (CJK text has no spaces at all, and a long path must not overflow).
std::vector<std::string> WrapText(const std::string &text, int max_width);

//---------------------------------------------------------------------------
// SDL text input ownership
//---------------------------------------------------------------------------
bool TextInputUsable();
// What TVPShowIME()/TVPHideIME() want the text input state to be.
void TextInputEngineWants(bool on, const SDL_Rect &rect);
// A dialog takes the text input over for as long as it is up and gives the
// engine's recorded wish back on release.
void TextInputAcquire(const SDL_Rect &rect);
void TextInputRelease();

//---------------------------------------------------------------------------
// Single line text editor
//
// One editor shared by both dialogs: the editing rules (caret movement on
// code point boundaries, an IME composition that owns the caret, deletion and
// clipboard paste) are the same in the input box's field and the file
// selector's filter/name line.
//---------------------------------------------------------------------------
class TextEdit
{
public:
	enum KeyResult {
		KeyUnhandled, // not an editing key; the dialog may act on it
		KeyHandled,   // the text or the caret changed
		KeyAccept,    // RETURN/KP_ENTER: the dialog's accept action
		KeyCancel     // ESCAPE: the dialog's cancel action
	};

	std::string Text;             // UTF-8
	size_t Caret = 0;             // byte index into Text
	std::string Composition;      // in-progress IME composition (UTF-8)
	size_t CompositionCursor = 0; // byte index into Composition

	void Reset(const std::string &text);
	bool Composing() const { return !Composition.empty(); }

	void Insert(const std::string &utf8);
	void PasteClipboard();
	// SDL hands over the whole composition each time it changes; its event
	// carries a selection (start/length, in characters) rather than a cursor,
	// so the in-composition caret sits at the end of that selection.
	void OnTextEditing(const SDL_TextEditingEvent &edit);

	// Consumes the editing keys.  While a composition is active it owns
	// Backspace/Return/arrows and nothing here acts on them, as SDL's own IME
	// handling is already editing the composition.
	KeyResult OnKeyDown(const SDL_KeyboardEvent &key);

	// Width of the text up to the caret (plus the composition up to its own
	// cursor) in the field's local coordinate system.
	int CaretLocalX(int padding) const;
	void KeepCaretVisible(int field_width, int padding, int &scroll) const;
	// Places the caret at the code point nearest `local_x` (a field-local x,
	// with `scroll` already subtracted by the caller) and cancels a
	// composition, as a click does in native fields.
	void SetCaretFromX(int local_x, int padding);

	// Draws text, IME composition and caret, clipped to `field`.  The field's
	// background and frame belong to the dialog.
	void Draw(SDL_Renderer *renderer, const SDL_Rect &field, int padding,
		int scroll, bool caret_visible, SDL_Color color) const;
};

//---------------------------------------------------------------------------
// Scripted dialog input (KRKR2_DIALOG_KEYS)
//
// Comma-separated steps, replayed from the start for every dialog the process
// opens, so each dialog of a run is independently deterministic:
//
//	up down left right home end pageup pagedown	- arrow/page keys
//	enter esc escape backspace delete tab paste	- RETURN/ESCAPE/... ('paste'
//	                                                  is Ctrl+V)
//	space f1 f2 ... fn  w  s                      - named keys  ('w'/'s'/'space'
//	                                                  are also text: push the
//	                                                  matching text:x step too
//	                                                  to model a real keystroke)
//	text:<utf8>                                   - SDL_TEXTINPUT
//	click:<x>:<y>   dclick:<x>:<y>                - left button down
//	wheelup:<x>:<y> wheeldown:<x>:<y>             - mouse wheel
//	pad:up|down|left|right|a|b|x|y|start|lb|rb    - SDL_CONTROLLERBUTTONDOWN
//	axis:leftx|lefty|rightx|righty:<value>        - SDL_CONTROLLERAXISMOTION
//	wait:<ms>                                     - pause before the next step
//
// The sequence must end with a step that closes the dialog (an 'enter' or an
// 'esc'); once it runs out the dialog keeps running on real input and logs
// that once.  Steps are pushed into SDL's queue one per loop iteration, so a
// dialog consumes them through its normal event path.
//---------------------------------------------------------------------------
// Restarts the sequence: every dialog begins at step 0, so each dialog of a
// run is driven independently and deterministically.
void ResetScriptedDialogInput();
// Pushes the next scripted step into SDL's queue.  Returns false when the
// sequence is exhausted (or no script is configured).
bool PumpScriptedDialogInput();

} // namespace krkr2sdl
