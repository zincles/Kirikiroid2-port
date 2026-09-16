//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000-2007 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Platform contract ( environ/Platform.h ) for the SDL2 targets:
// Linux x86_64 desktop and Nintendo Switch.
//
// This file replaces environ/win32/Platform.cpp (Win32 API) and
// environ/android/AndroidUtils.cpp (JNI) for this port.  Everything below is
// POSIX or SDL2; nothing here is Windows- or Android-specific.
//
// Provided elsewhere for this platform -- intentionally NOT redefined here:
//	environ/linux/Platform.cpp : TVPGetMemoryInfo, TVPRelinquishCPU, TVP_utime
//	environ/Application.cpp    : TVPCheckMemory, ExePath, TVPGetErrorDialogTitle,
//	                             TVPShowSimpleMessageBox(ttstr,ttstr),
//	                             TVPShowSimpleMessageBoxYesNo
//	base/win32/StorageImpl.cpp : TVPGetAppPath, TVPCheckExistentLocal{File,Folder},
//	                             TVPGetLocalFileListAt, ...
//	base/SysInitIntf.cpp       : TVPSystemUninit, TVPSystemUninitCalled
//	base/win32/SysInitImpl.cpp : TVPTerminated, TVPTerminateCode
//
// Three symbols the platform contract needs have no compiled definition in this
// tree and are therefore provided here as well: TVPGetRoughTickCount32 (the
// body in utils/MiscUtility.cpp is inside an `#if 0`), TVP_stat (Platform.h
// declares it; base/win32/StorageImpl.cpp only calls it, its Win32 body being
// also `#if 0`ed) and TVPCreateFolders (same, and base/win32/SysInitImpl.cpp
// plus base/7zArchive.cpp call it at boot).
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "Host.h" // krkr2sdl::HostEverCreatedWindow
#include "SDLDialog.h" // shared modal-dialog text/edit/IME helpers

#ifdef __SWITCH__
#include <switch.h> // svcGetSystemInfo / SystemInfoType_*
#endif

#include "Platform.h"
#include "Application.h"
#include "SysInitIntf.h"
#include "SysInitImpl.h"
#include "TickCount.h"
#include "StorageIntf.h"
#include "StorageImpl.h"
#include "EventIntf.h"
#include "RenderManager.h"
#include "ConfigManager/LocaleConfigManager.h"
#include "Host.h"

#include <SDL.h>

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <algorithm>
#include <string>
#include <vector>

//---------------------------------------------------------------------------
// Application identity
//
// SDL's preference directory is keyed on an organization/application pair; the
// port has no other notion of either, so they are fixed here and shared by
// every path helper below.
//---------------------------------------------------------------------------
static const char *KRKR2_SDL2_PREF_ORG = "kirikiri2";
static const char *KRKR2_SDL2_PREF_APP = "Kirikiroid2";

// Reported by TVPGetPackageVersionString() and shown by the file selector's
// "about" box.  Kept in step with the Android application's versionName
// (project/android/AndroidManifest.xml).
static const char *KRKR2_SDL2_VERSION = "1.3.4-sdl2";

//---------------------------------------------------------------------------
// TVPGetRoughTickCount32
//
// A monotonic millisecond counter, which the engine uses for the memory
// query's cache window (and TickCount.cpp uses for the 32-bit tick watch).
// The copy in utils/MiscUtility.cpp is inside an `#if 0` block, so this is the
// definition for the whole build; the Android build used exactly this
// CLOCK_MONOTONIC expression.
//---------------------------------------------------------------------------
tjs_uint32 TVPGetRoughTickCount32()
{
	struct timespec on;
	if (clock_gettime(CLOCK_MONOTONIC, &on) == 0)
		return (tjs_uint32)((tjs_uint64)on.tv_sec * 1000 + on.tv_nsec / 1000000);
	return 0;
}

//---------------------------------------------------------------------------
// TVPPrintLog
//
// The Win32 build uses a bare printf(); the Android build goes to logcat.
// Here the log is the process' stdout, and it is flushed on every call so a
// message that arrives just before a crash (the dumps DumpSend.cpp reports)
// is not lost in a stdio buffer.  The caller's string does not have to end in a
// newline -- DumpSend.cpp's messages do not.
//---------------------------------------------------------------------------
void TVPPrintLog(const char *str)
{
	if (!str) return;
	const size_t len = strlen(str);
	fputs(str, stdout);
	if (len == 0 || str[len - 1] != '\n')
		fputc('\n', stdout);
	fflush(stdout);
}

//---------------------------------------------------------------------------
// Memory statistics
//
// Conventions mirrored from the two reference implementations:
//   TVPGetSystemFreeMemory()  - MB.  Win32: dwAvailPhys / (1024*1024).
//   TVPGetSelfUsedMemory()    - MB.  Win32: WorkingSetSize / (1024*1024), i.e.
//                               this process' resident set, which is what
//                               /proc/self/status's VmRSS reports.
// (The Android build divided its "used memory" by 1024 instead, reporting kB
// for a value its only consumer -- MainScene.cpp's debug overlay -- prints as
// MB.  The environ/Platform.h contract says MB for both, and the Win32 build
// matches it, so MB it is.)
//
// Both values come from the same cached sample, refreshed at most once every
// three seconds, exactly as the Android version's _updateMemoryInfo() does.
//---------------------------------------------------------------------------
namespace {

// Reads the first "<key>: <value> kB" line of /proc/meminfo.
bool ReadMemInfoKB(const char *key, unsigned long &value)
{
#ifdef __SWITCH__
	// No /proc on the Switch; the free-DRAM figure comes from the kernel.  The
	// caller asks for "MemAvailable:" first, then "MemFree:" - both mean the
	// same thing here.
	(void)key;
	u64 total = 0, used = 0;
	if (R_FAILED(svcGetSystemInfo(&total, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, 0)) ||
		R_FAILED(svcGetSystemInfo(&used, SystemInfoType_UsedPhysicalMemorySize, INVALID_HANDLE, 0)))
		return false;
	value = (unsigned long)((used < total ? total - used : 0) / 1024);
	return true;
#else
	FILE *fp = fopen("/proc/meminfo", "r");
	if (!fp) return false;
	const size_t keylen = strlen(key);
	char line[256];
	bool found = false;
	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, key, keylen) != 0) continue;
		const char *p = line + keylen;
		while (*p == ' ' || *p == '\t') ++p;
		char *end = nullptr;
		unsigned long v = strtoul(p, &end, 10);
		if (end != p) {
			value = v;
			found = true;
		}
		break;
	}
	fclose(fp);
	return found;
#endif
}

// Reads this process' resident set size out of /proc/self/status (VmRSS).
bool ReadSelfRSSKB(unsigned long &value)
{
#ifdef __SWITCH__
	// libnx exposes no per-process resident-set accounting, so this reports the
	// system-wide usage instead of claiming 0; the value only feeds the
	// engine's debug/memory-log output.
	u64 used = 0;
	if (R_FAILED(svcGetSystemInfo(&used, SystemInfoType_UsedPhysicalMemorySize, INVALID_HANDLE, 0)))
		return false;
	value = (unsigned long)(used / 1024);
	return true;
#else
	FILE *fp = fopen("/proc/self/status", "r");
	if (!fp) return false;
	char line[256];
	bool found = false;
	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "VmRSS:", 6) != 0) continue;
		char *end = nullptr;
		unsigned long v = strtoul(line + 6, &end, 10);
		if (end != line + 6) {
			value = v;
			found = true;
		}
		break;
	}
	fclose(fp);
	return found;
#endif
}


tjs_uint32 _LastMemoryInfoQuery = 0;
tjs_int _AvailableMemory = 0;
tjs_int _UsedMemory = 0;

void UpdateMemoryInfo()
{
	// Same three-second window as the Android implementation (freq in 3s).
	if (_LastMemoryInfoQuery != 0 &&
		TVPGetRoughTickCount32() - _LastMemoryInfoQuery <= 3000)
		return;

	unsigned long kb = 0;
	// MemAvailable is what the kernel considers reclaimable without swapping;
	// MemFree alone ignores page cache.  Old kernels have no MemAvailable.
	if (ReadMemInfoKB("MemAvailable:", kb) || ReadMemInfoKB("MemFree:", kb))
		_AvailableMemory = (tjs_int)(kb / 1024);
	else
		_AvailableMemory = 0;

	if (ReadSelfRSSKB(kb))
		_UsedMemory = (tjs_int)(kb / 1024);
	else
		_UsedMemory = 0;

	_LastMemoryInfoQuery = TVPGetRoughTickCount32();
}

} // anonymous namespace

tjs_int TVPGetSystemFreeMemory()
{
	UpdateMemoryInfo();
	return _AvailableMemory;
}

tjs_int TVPGetSelfUsedMemory()
{
	UpdateMemoryInfo();
	return _UsedMemory;
}

//---------------------------------------------------------------------------
// Win32-compatible message box helper
//
// The engine's button vector is handed to SDL_ShowMessageBox(), not to
// SDL_ShowSimpleMessageBox(): SDL_ShowSimpleMessageBox() always creates a
// single "OK" button (its flags only pick the icon and the button order), so
// it cannot carry a multi-button dialog.  SDL_ShowMessageBox() takes the
// button array and reports the selected button through its buttonid out
// parameter, and is documented to work even before SDL_Init() and with a NULL
// parent window, which is exactly what the "message box before the window
// exists" case needs.
//
// SDL2 has no SDL_MESSAGEBOX_BUTTONS_WRAP (that is an SDL3 addition), so more
// than three buttons are not folded: SDL_ShowMessageBox lays out an arbitrary
// number of buttons itself.
//---------------------------------------------------------------------------
namespace {

int ShowMessageBoxU8(const std::string &text, const std::string &caption,
	const std::vector<std::string> &buttons)
{
	std::vector<SDL_MessageBoxButtonData> sdlButtons;
	sdlButtons.reserve(buttons.size());
	for (size_t i = 0; i < buttons.size(); ++i) {
		SDL_MessageBoxButtonData b;
		SDL_zero(b);
		b.flags = 0;
		b.buttonid = (int)i;
		b.text = buttons[i].c_str();
		// Return activates the first entry -- the "accept" action of every
		// caller in this tree, and the default of Win32's MB_OK/MB_YESNO.
		if (i == 0)
			b.flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
		// Escape (and closing the dialog) activates the last one, which is
		// where "Cancel"/"No" sit.  Win32's MessageBox returns IDCANCEL for a
		// dismissed MB_YESNO box for the same reason.
		if (i + 1 == buttons.size())
			b.flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
		sdlButtons.push_back(b);
	}
	if (sdlButtons.empty()) {
		// No caller does this, but a dialog without a way out would be a dead
		// end; Win32's MessageBox can not be built without a button either.
		SDL_MessageBoxButtonData b;
		SDL_zero(b);
		b.buttonid = 0;
		b.text = "OK";
		sdlButtons.push_back(b);
	}

	SDL_MessageBoxData data;
	SDL_zero(data);
	data.flags = SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
	data.window = krkr2sdl::HostWindow(); // NULL before HostInit(), which SDL accepts
	data.title = caption.c_str();
	data.message = text.c_str();
	data.numbuttons = (int)sdlButtons.size();
	data.buttons = &sdlButtons[0];
	data.colorScheme = nullptr;

	int buttonid = 0;
	if (SDL_ShowMessageBox(&data, &buttonid) < 0) {
		// No usable video backend (headless build, SDL video init failed...).
		// Neither reference implementation can show anything in that case
		// either, so report the failure and leave the decision to the caller:
		// -1 makes the out-of-memory retry loop in Application.cpp stop instead
		// of spinning, and every other caller ignores the result.
		std::string line("TVPShowSimpleMessageBox: cannot show a dialog (SDL: ");
		line += SDL_GetError();
		line += "): ";
		if (!caption.empty()) {
			line += caption;
			line += ": ";
		}
		line += text;
		TVPPrintLog(line.c_str());
		return -1;
	}
	return buttonid;
}

} // anonymous namespace

extern "C" int TVPShowSimpleMessageBox(const char *text, const char *caption,
	unsigned int nButton, const char **btnText)
{
	std::vector<std::string> buttons;
	buttons.reserve(nButton);
	for (unsigned int i = 0; i < nButton; ++i)
		buttons.emplace_back(btnText && btnText[i] ? btnText[i] : "");
	return ShowMessageBoxU8(text ? text : "", caption ? caption : "", buttons);
}

int TVPShowSimpleMessageBox(const ttstr &text, const ttstr &caption,
	const std::vector<ttstr> &vecButtons)
{
	std::vector<std::string> buttons;
	buttons.reserve(vecButtons.size());
	for (size_t i = 0; i < vecButtons.size(); ++i)
		buttons.emplace_back(vecButtons[i].AsStdString());
	// ttstr holds UTF-16 internally and both conversions below round-trip
	// through UTF-8 (TJS_wcstombs/TJS_mbstowcs), so CJK labels survive.
	return ShowMessageBoxU8(text.AsStdString(), caption.AsStdString(), buttons);
}

void TVPShowIME(int x, int y, int w, int h)
{
	// The engine passes drawing-area coordinates (GameMainMenu calls this with
	// the screen size); SDL wants window coordinates, so the draw area's origin
	// is added back in.  SDLDialog.cpp owns the text-input state itself: while
	// a modal dialog is up it only records this request and the dialog applies
	// it (or restores it) when it closes.
	const krkr2sdl::DrawArea area = krkr2sdl::HostGetCurrentDrawArea();
	SDL_Rect rect;
	rect.x = area.left + x;
	rect.y = area.top + y;
	rect.w = w > 0 ? w : 1;
	rect.h = h > 0 ? h : 1;
	krkr2sdl::TextInputEngineWants(true, rect);
}

void TVPHideIME()
{
	// While a modal dialog is up it owns SDL's text input; stopping it here
	// would take the keyboard and IME away from the dialog's own field.
	krkr2sdl::TextInputEngineWants(false, SDL_Rect{ 0, 0, 0, 0 });
}

//===========================================================================
//
// Modal input box
//
// SDL2 has no input dialog, so the box is drawn on the host's SDL_Renderer and
// driven by its own event loop -- the engine's frame pump is blocked while the
// dialog is up, which is what makes it modal.  While the loop runs it consumes
// every SDL event, so the engine never sees the keystrokes that belong to the
// field.
//===========================================================================
namespace {

class tTVPSDLInputDialog
{
public:
	tTVPSDLInputDialog(SDL_Window *window, SDL_Renderer *renderer)
		: Window(window), Renderer(renderer) {}

	// Returns the index of the pressed button, or -1 when the dialog could not
	// be shown at all (in that case `text` is left untouched).
	int Run(const std::string &caption, const std::string &prompt,
		const std::vector<std::string> &buttons, std::string &text)
	{
		if (Window == nullptr || Renderer == nullptr) return -1;

		// Each dialog of a run starts from the first scripted step (see
		// SDLDialog.h, KRKR2_DIALOG_KEYS).
		krkr2sdl::ResetScriptedDialogInput();

		SDL_GetWindowSize(Window, &WindowWidth, &WindowHeight);
		FontSize = std::max(14, std::min(28, WindowHeight / 36));
		if (!krkr2sdl::PrepareDialogFont(FontSize)) return -1;

		Caption = caption;
		Prompt = prompt;
		ButtonLabels = buttons;
		if (ButtonLabels.empty())
			ButtonLabels.push_back("OK"); // every caller passes labels; a box
			                              // with no way out would be a dead end
		Edit.Reset(text);
		Scroll = 0;
		Hover = -1;
		CaretVisible = true;
		LastBlink = SDL_GetTicks();

		Layout();
		EnsureCaretVisible();

		krkr2sdl::TextInputAcquire(Field);
		Draw();

		while (!Done) {
			// KRKR2_DIALOG_KEYS, when set, feeds the next scripted event into
			// SDL's queue so the dispatch below sees it like a real device.
			krkr2sdl::PumpScriptedDialogInput();
			SDL_Event ev;
			if (SDL_WaitEventTimeout(&ev, CaretBlinkMs) == 0) {
				// nothing happened: keep the caret blinking
				if (SDL_GetTicks() - LastBlink >= CaretBlinkMs) {
					CaretVisible = !CaretVisible;
					LastBlink = SDL_GetTicks();
					Dirty = true;
				}
			} else {
				HandleEvent(ev);
			}
			if (Done) break;
			if (SDL_GetTicks() - LastBlink >= CaretBlinkMs) {
				CaretVisible = !CaretVisible;
				LastBlink = SDL_GetTicks();
				Dirty = true;
			}
			if (Dirty) {
				Draw();
				Dirty = false;
			}
		}

		// Give the engine its text input back the way it left it: if it had
		// asked for an IME rectangle (TVPShowIME) that state is restored, and
		// otherwise SDL's text input goes back off (TVPHideIME / never asked).
		krkr2sdl::TextInputRelease();
		if (Result >= 0) text = Edit.Text;
		return Result;
	}

private:
	// ---- state ----
	SDL_Window *Window;
	SDL_Renderer *Renderer;
	std::string Caption, Prompt;
	krkr2sdl::TextEdit Edit;
	std::vector<std::string> ButtonLabels;
	std::vector<std::string> PromptLines;
	std::vector<SDL_Rect> ButtonRects;
	SDL_Rect Panel = { 0, 0, 0, 0 };
	SDL_Rect Field = { 0, 0, 0, 0 };
	int CaptionY = 0, PromptY = 0;
	int WindowWidth = 0, WindowHeight = 0;
	int FontSize = 18;
	int Padding = 16;
	int FieldPadding = 8;
	int Scroll = 0;
	int Hover = -1;
	bool CaretVisible = true;
	bool Dirty = true;
	bool Done = false;
	int Result = -1;
	Uint32 LastBlink = 0;

	static const Uint32 CaretBlinkMs = 500;

	// ---- layout ----
	void Layout()
	{
		SDL_GetWindowSize(Window, &WindowWidth, &WindowHeight);

		const int margin = 32;
		Padding = 16;
		FieldPadding = 8;
		const int lineHeight = krkr2sdl::DialogLineHeight();

		int panelWidth = WindowWidth - 2 * margin;
		panelWidth = std::max(280, std::min(panelWidth, 640));
		const int innerWidth = panelWidth - 2 * Padding;

		PromptLines = krkr2sdl::WrapText(Prompt, innerWidth);

		const int fieldHeight = lineHeight + 2 * FieldPadding;
		const int buttonHeight = lineHeight + 2 * FieldPadding;
		int panelHeight = Padding + lineHeight + 10;
		if (!PromptLines.empty())
			panelHeight += (int)PromptLines.size() * lineHeight + 10;
		panelHeight += fieldHeight + 14 + buttonHeight + Padding;

		Panel.w = panelWidth;
		Panel.h = panelHeight;
		Panel.x = (WindowWidth - panelWidth) / 2;
		Panel.y = (WindowHeight - panelHeight) / 2;
		if (Panel.y < 0) Panel.y = 0;

		int y = Panel.y + Padding;
		CaptionY = y;
		y += lineHeight + 10;

		PromptY = y;
		if (!PromptLines.empty())
			y += (int)PromptLines.size() * lineHeight + 10;

		Field.x = Panel.x + Padding;
		Field.y = y;
		Field.w = innerWidth;
		Field.h = fieldHeight;
		y += fieldHeight + 14;

		// Buttons are right aligned, in the caller's order, which is what
		// Win32's MessageBox does with a button vector.
		const int gap = 8;
		const int buttonPadding = 18;
		std::vector<int> widths(ButtonLabels.size(), 0);
		int total = 0;
		for (size_t i = 0; i < ButtonLabels.size(); ++i) {
			widths[i] = krkr2sdl::MeasureGlyphRun(ButtonLabels[i]) + 2 * buttonPadding;
			widths[i] = std::max(widths[i], 64);
			total += widths[i];
		}
		if (ButtonLabels.size() > 1)
			total += gap * ((int)ButtonLabels.size() - 1);

		ButtonRects.resize(ButtonLabels.size());
		int x = Panel.x + Panel.w - Padding - total;
		if (x < Panel.x + Padding) x = Panel.x + Padding; // very narrow window
		for (size_t i = 0; i < ButtonLabels.size(); ++i) {
			ButtonRects[i].x = x;
			ButtonRects[i].y = y;
			ButtonRects[i].w = widths[i];
			ButtonRects[i].h = buttonHeight;
			x += widths[i] + gap;
		}
	}

	// ---- text field geometry ----
	void EnsureCaretVisible()
	{
		Edit.KeepCaretVisible(Field.w, FieldPadding, Scroll);
	}

	// ---- editing ----
	void Finish(int index)
	{
		Result = index;
		Done = true;
	}

	void TouchText()
	{
		CaretVisible = true;
		LastBlink = SDL_GetTicks();
		EnsureCaretVisible();
		Dirty = true;
	}

	void SetCaretFromX(int windowX)
	{
		Edit.SetCaretFromX(windowX - Field.x + Scroll, FieldPadding);
		TouchText();
	}

	void OnKeyDown(const SDL_KeyboardEvent &key)
	{
		switch (Edit.OnKeyDown(key)) {
		case krkr2sdl::TextEdit::KeyAccept:
			Finish(0); // the first button is the accept action
			return;
		case krkr2sdl::TextEdit::KeyCancel:
			Finish((int)ButtonLabels.size() - 1);
			return;
		case krkr2sdl::TextEdit::KeyHandled:
			TouchText();
			return;
		default:
			return; // printable characters arrive as SDL_TEXTINPUT
		}
	}

	void HandleEvent(const SDL_Event &ev)
	{
		switch (ev.type) {
		case SDL_TEXTINPUT:
			Edit.Insert(ev.text.text);
			TouchText();
			break;
		case SDL_TEXTEDITING:
			Edit.OnTextEditing(ev.edit);
			TouchText();
			break;
		case SDL_KEYDOWN:
			OnKeyDown(ev.key);
			break;
		case SDL_MOUSEBUTTONDOWN:
			if (ev.button.button != SDL_BUTTON_LEFT) break;
			for (size_t i = 0; i < ButtonRects.size(); ++i) {
				const SDL_Rect &r = ButtonRects[i];
				if (ev.button.x >= r.x && ev.button.x < r.x + r.w &&
					ev.button.y >= r.y && ev.button.y < r.y + r.h) {
					Finish((int)i);
					return;
				}
			}
			if (ev.button.x >= Field.x && ev.button.x < Field.x + Field.w &&
				ev.button.y >= Field.y && ev.button.y < Field.y + Field.h)
				SetCaretFromX(ev.button.x);
			break;
		case SDL_MOUSEMOTION: {
			int hover = -1;
			for (size_t i = 0; i < ButtonRects.size(); ++i) {
				const SDL_Rect &r = ButtonRects[i];
				if (ev.motion.x >= r.x && ev.motion.x < r.x + r.w &&
					ev.motion.y >= r.y && ev.motion.y < r.y + r.h) {
					hover = (int)i;
					break;
				}
			}
			if (hover != Hover) {
				Hover = hover;
				Dirty = true;
			}
			break;
		}
		case SDL_WINDOWEVENT:
			switch (ev.window.event) {
			case SDL_WINDOWEVENT_SIZE_CHANGED:
			case SDL_WINDOWEVENT_RESIZED:
				Layout();
				EnsureCaretVisible();
				SDL_SetTextInputRect(&Field);
				Dirty = true;
				break;
			case SDL_WINDOWEVENT_EXPOSED:
				Dirty = true;
				break;
			case SDL_WINDOWEVENT_CLOSE:
				// The user asked to close the game while the dialog is up:
				// cancel the dialog and let the engine's own close query run
				// once it regains control (Host.h).
				krkr2sdl::HostRequestQuit();
				Finish((int)ButtonLabels.size() - 1);
				break;
			default:
				break;
			}
			break;
		case SDL_QUIT:
			krkr2sdl::HostRequestQuit();
			Finish((int)ButtonLabels.size() - 1);
			break;
		default:
			break;
		}
	}

	// ---- drawing ----
	void Draw()
	{
		SDL_SetRenderTarget(Renderer, nullptr);
		SDL_SetRenderDrawBlendMode(Renderer, SDL_BLENDMODE_BLEND);

		// dim the frozen engine frame behind the dialog
		SDL_SetRenderDrawColor(Renderer, 0, 0, 0, 0xA0);
		SDL_RenderFillRect(Renderer, nullptr);

		SDL_SetRenderDrawColor(Renderer, 0x2B, 0x2F, 0x38, 0xFF);
		SDL_RenderFillRect(Renderer, &Panel);
		SDL_SetRenderDrawColor(Renderer, 0x9A, 0xA4, 0xB8, 0xFF);
		SDL_RenderDrawRect(Renderer, &Panel);

		const SDL_Color titleColor = { 0xF2, 0xF4, 0xF8, 0xFF };
		const SDL_Color bodyColor = { 0xD8, 0xDC, 0xE4, 0xFF };

		krkr2sdl::DrawGlyphRun(Renderer, Caption, Panel.x + Padding, CaptionY, titleColor);
		for (size_t i = 0; i < PromptLines.size(); ++i)
			krkr2sdl::DrawGlyphRun(Renderer, PromptLines[i], Panel.x + Padding,
				PromptY + (int)i * krkr2sdl::DialogLineHeight(), bodyColor);

		// text field
		SDL_SetRenderDrawColor(Renderer, 0x14, 0x17, 0x1C, 0xFF);
		SDL_RenderFillRect(Renderer, &Field);
		SDL_SetRenderDrawColor(Renderer, 0x5C, 0x8F, 0xD8, 0xFF);
		SDL_RenderDrawRect(Renderer, &Field);

		Edit.Draw(Renderer, Field, FieldPadding, Scroll, CaretVisible, bodyColor);

		// buttons
		for (size_t i = 0; i < ButtonRects.size(); ++i) {
			const SDL_Rect &r = ButtonRects[i];
			if ((int)i == Hover)
				SDL_SetRenderDrawColor(Renderer, 0x3D, 0x6E, 0xA5, 0xFF);
			else
				SDL_SetRenderDrawColor(Renderer, 0x3A, 0x3F, 0x4B, 0xFF);
			SDL_RenderFillRect(Renderer, &r);
			SDL_SetRenderDrawColor(Renderer, 0x9A, 0xA4, 0xB8, 0xFF);
			SDL_RenderDrawRect(Renderer, &r);
			const int labelWidth = krkr2sdl::MeasureGlyphRun(ButtonLabels[i]);
			krkr2sdl::DrawGlyphRun(Renderer, ButtonLabels[i],
				r.x + (r.w - labelWidth) / 2,
				r.y + (r.h - krkr2sdl::DialogLineHeight()) / 2, titleColor);
		}

		SDL_RenderPresent(Renderer);
	}
};

} // anonymous namespace

int TVPShowSimpleInputBox(ttstr &text, const ttstr &caption, const ttstr &prompt,
	const std::vector<ttstr> &vecButtons)
{
	SDL_Window *window = krkr2sdl::HostWindow();
	SDL_Renderer *renderer = krkr2sdl::HostRenderer();
	if (window == nullptr || renderer == nullptr) {
		// Before HostInit() (or after HostShutdown()) there is no surface to
		// draw on and no event source to read from, and SDL offers no input
		// dialog to fall back on.  The Android build reports -1 in the same
		// situation (its JNI dialog unavailable), and so does this one.
		TVPPrintLog("TVPShowSimpleInputBox: the SDL host has no window/renderer");
		return -1;
	}

	std::vector<std::string> buttons;
	buttons.reserve(vecButtons.size());
	for (size_t i = 0; i < vecButtons.size(); ++i)
		buttons.emplace_back(vecButtons[i].AsStdString());

	std::string value = text.AsStdString();
	tTVPSDLInputDialog dialog(window, renderer);
	const int result = dialog.Run(caption.AsStdString(), prompt.AsStdString(),
		buttons, value);
	if (result >= 0)
		text = ttstr(value);
	return result;
}

//---------------------------------------------------------------------------
// Paths and directories
//---------------------------------------------------------------------------
namespace {

std::string StripTrailingSlash(const std::string &path)
{
	if (path.size() <= 1) return path;
	std::string ret(path);
	while (ret.size() > 1 && ret[ret.size() - 1] == '/')
		ret.erase(ret.size() - 1);
	return ret;
}

// The writable directory the engine keeps its data (saves, global preference,
// game list) in.
//
// "KRKR2_DATA_DIR" wins when set: the entry point exports its --data-dir=<dir>
// argument through it, so that a handheld or Switch user can keep the games and
// their savedata together on a removable volume next to the game -- which is
// where SDL_GetPrefPath(), the internal-storage default, would put them
// instead.  The directory is created here if it does not exist yet.
//
// Otherwise SDL_GetPrefPath() is used: $XDG_DATA_HOME/<org>/<app> on the Linux
// desktop and "sdmc:/switch/<app>" on the Switch.  If even that fails, the
// project directory is the only location this process is known to be able to
// write to (TVPCheckStartupPath() has already verified that).
//
// The chosen path is logged once, and has no trailing slash.
const std::string &WritablePath()
{
	static std::string cached;
	if (!cached.empty()) return cached;

	const char *dataDir = getenv("KRKR2_DATA_DIR");
	if (dataDir && *dataDir) {
		const std::string dir = StripTrailingSlash(dataDir);
		if (!dir.empty() &&
			(TVPCheckExistentLocalFolder(ttstr(dir)) ||
			 TVPCreateFolders(ttstr(dir)))) {
			cached = dir;
			TVPPrintLog(("writable data directory (KRKR2_DATA_DIR): " +
				cached).c_str());
			return cached;
		}
		TVPPrintLog(("warning: KRKR2_DATA_DIR=" + std::string(dataDir) +
			" cannot be used (" + strerror(errno) +
			"), falling back to SDL_GetPrefPath()").c_str());
	}

	char *pref = SDL_GetPrefPath(KRKR2_SDL2_PREF_ORG, KRKR2_SDL2_PREF_APP);
	if (pref) {
		cached = pref;
		SDL_free(pref);
	}
	if (cached.empty()) {
		cached = TVPGetAppPath().AsStdString();
		TVPPrintLog("warning: SDL_GetPrefPath() failed, using the game "
			"directory for writable data");
	}
	cached = StripTrailingSlash(cached);
	TVPPrintLog(("writable data directory: " + cached).c_str());
	return cached;
}

// Undoes /proc/mounts' octal escapes (\040 for a space) in a field.
std::string UnescapeMountField(const std::string &in)
{
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		if (in[i] == '\\' && i + 3 < in.size() &&
			in[i + 1] >= '0' && in[i + 1] <= '7' &&
			in[i + 2] >= '0' && in[i + 2] <= '7' &&
			in[i + 3] >= '0' && in[i + 3] <= '7') {
			out += (char)(((in[i + 1] - '0') << 6) | ((in[i + 2] - '0') << 3) |
				(in[i + 3] - '0'));
			i += 3;
			continue;
		}
		out += in[i];
	}
	return out;
}

// Filesystems that are kernel-internal and useless as a browsable root.
bool IsPseudoFileSystem(const std::string &fstype)
{
	static const char *const pseudo[] = {
		"proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "cgroup", "cgroup2",
		"securityfs", "debugfs", "tracefs", "pstore", "bpf", "configfs",
		"fusectl", "mqueue", "hugetlbfs", "rpc_pipefs", "autofs",
		"binfmt_misc", "efivarfs", "ramfs", "nsfs", "selinuxfs", "smackfs",
	};
	for (size_t i = 0; i < sizeof(pseudo) / sizeof(pseudo[0]); ++i) {
		if (fstype == pseudo[i]) return true;
	}
	return false;
}

} // anonymous namespace

std::vector<std::string> TVPGetDriverPath()
{
#ifdef __SWITCH__
	// libnx mounts the SD card as "sdmc:" (switch/runtime/devices/fs_dev.h);
	// that is where a player keeps games, patches and savedata, and there is no
	// /proc or mount table on the console.  The directory the .nro runs from is
	// added as well, so a game dropped next to it is one click away in the file
	// selector.
	std::vector<std::string> ret;
	ret.emplace_back("sdmc:/");
	if (char *base = SDL_GetBasePath()) {
		std::string basepath = base;
		SDL_free(base);
		if (!basepath.empty() && basepath != "sdmc:/") ret.push_back(basepath);
	}
	return ret;
#else
	// Android enumerates its mounted volumes and Win32 enumerates the drive
	// letters; the equivalent browsable roots here are the real mount points.
	std::vector<std::string> ret;
	ret.emplace_back("/");

	FILE *fp = fopen("/proc/mounts", "r");
	std::vector<std::string> seen;
	std::vector<std::string> seenDevices;
	seen.push_back("/");
	if (fp) {
		char line[512];
		while (fgets(line, sizeof(line), fp)) {
			// <source> <mountpoint> <fstype> <options> <dump> <pass>
			std::string fields[4];
			size_t field = 0;
			const char *p = line;
			while (*p && field < 4) {
				while (*p == ' ' || *p == '\t') ++p;
				if (*p == '\0' || *p == '\n') break;
				std::string token;
				while (*p && *p != ' ' && *p != '\t' && *p != '\n')
					token += *p++;
				fields[field++] = UnescapeMountField(token);
			}
			if (field < 3) continue;
			if (IsPseudoFileSystem(fields[2])) continue;

			const std::string &mountpoint = fields[1];
			if (mountpoint.empty() || mountpoint[0] != '/') continue;
			if (mountpoint == "/") {
				// reported above; remember its volume so that bind mounts of it
				// are not offered as separate roots
				if (std::find(seenDevices.begin(), seenDevices.end(), fields[0]) ==
					seenDevices.end())
					seenDevices.push_back(fields[0]);
				continue;
			}
			if (std::find(seen.begin(), seen.end(), mountpoint) != seen.end())
				continue;
			// One entry per *filesystem*, which is what the Android version
			// does by de-duplicating on the mount source ("/proc/mounts" in a
			// desktop session lists the same volume once per bind mount).
			// "/" is always reported, so this never hides a volume entirely.
			if (std::find(seenDevices.begin(), seenDevices.end(), fields[0]) !=
				seenDevices.end())
				continue;
			// Only report something that can actually be browsed.
			struct stat st;
			if (stat(mountpoint.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
				continue;

			seenDevices.push_back(fields[0]);
			seen.push_back(mountpoint);
			ret.push_back(mountpoint);
		}
		fclose(fp);
	} else {
		// No /proc (Switch, some chroots): the SDL base directory is the one
		// location that is known to hold the installation.
		char *base = SDL_GetBasePath();
		if (base) {
			std::string path = StripTrailingSlash(base);
			SDL_free(base);
			if (!path.empty() && path != "/") ret.push_back(path);
		}
	}

	// The user's home directory: the one place a desktop user can always write
	// to, and what the file selector opens by default.
	const char *home = getenv("HOME");
	if (home && *home) {
		std::string path = StripTrailingSlash(home);
		if (path != "/" && std::find(seen.begin(), seen.end(), path) == seen.end())
			ret.push_back(path);
	}
	return ret;
#endif
}

std::vector<std::string> TVPGetAppStoragePath()
{
	// Android returns the app's writable directories for the file selector's
	// shortcut list, Win32 returns the current directory.  Here: SDL's
	// preference directory (where the engine keeps its own data) and the game
	// directory, which is what the Win32 build browses from and which
	// TVPCheckStartupPath() has verified to be writable.
	std::vector<std::string> ret;
	const std::string pref = WritablePath();
	if (!pref.empty()) ret.push_back(pref);
	const std::string app = StripTrailingSlash(TVPGetAppPath().AsStdString());
	if (!app.empty() && std::find(ret.begin(), ret.end(), app) == ret.end())
		ret.push_back(app);
	return ret;
}

bool TVPCheckStartupPath(const std::string &path)
{
	// Mirrors the Android check: the location the game will write to must be
	// writable, which is probed by creating and removing a temporary file.  As
	// on Android the probe runs in the *parent* of `path`, because that is
	// where save data and the per-game preference directory land.
	const size_t pos = path.find_last_of('/');
	if (pos == std::string::npos) return false;
	const std::string parent = pos == 0 ? "/" : path.substr(0, pos);

	char probe[PATH_MAX];
	snprintf(probe, sizeof(probe), "%s/_check_save_%ld.tmp",
		parent.c_str(), (long)time(nullptr));

	bool success = false;
	int probeErrno = 0;
	FILE *fp = fopen(probe, "wb");
	if (fp) {
		success = true;
		fclose(fp);
		remove(probe);
	} else {
		probeErrno = errno;
	}

	if (!success) {
		// Same reporting as Android: tell the user which directory could not
		// be written and where the writable one is.
		const std::string writable = WritablePath();
		LocaleConfigManager *mgr = LocaleConfigManager::GetInstance();
		char buffer[PATH_MAX * 2];
		snprintf(buffer, sizeof(buffer), "%s\n%s\n\n%s: %s",
			mgr->GetText("use_internal_path").c_str(), writable.c_str(),
			parent.c_str(), strerror(probeErrno));
		std::vector<ttstr> buttons;
		buttons.emplace_back("OK");
		TVPShowSimpleMessageBox(buffer, mgr->GetText("readonly_storage"), buttons);
		return false;
	}
	return true;
}

std::string TVPGetPackageVersionString()
{
	return KRKR2_SDL2_VERSION;
}

const std::string &TVPGetInternalPreferencePath()
{
	// Mirrors cocos2d/MainScene.cpp's _TVPGetInternalPreferencePath(): the
	// writable directory plus ".preference", created on first use, with a
	// trailing slash.
	static std::string ret = []() -> std::string {
		std::string path = WritablePath();
		if (path.empty()) return path;
		path += "/.preference";
		if (!TVPCheckExistentLocalFolder(ttstr(path)))
			TVPCreateFolders(ttstr(path));
		path += "/";
		return path;
	}();
	return ret;
}

//---------------------------------------------------------------------------
// File helpers
//---------------------------------------------------------------------------
bool TVPCreateFolders(const ttstr &folder)
{
	// "mkdir -p".  environ/Platform.h's comment on the Win32 original is the
	// contract: make folders recursively, like mkdir -p; folder must be an OS
	// NATIVE folder name.  That original lives inside an `#if 0` in
	// base/win32/StorageImpl.cpp (and the Android one is JNI), so this is the
	// definition for the SDL2 build.
	//
	// An empty path and a path that already is a directory are successes; a
	// path whose parent is a file (or which cannot be created) is a failure.
	// ttstr is UTF-8 here, which is what mkdir() takes.
	const std::string path = StripTrailingSlash(folder.AsStdString());
	if (path.empty()) return true;

	std::string partial;
	for (size_t i = 0; i <= path.size(); ++i) {
		if (i < path.size() && path[i] != '/') {
			partial += path[i];
			continue;
		}
		if (!partial.empty() && partial != "/") {
			if (mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) {
				struct stat st;
				if (stat(partial.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
					TVPPrintLog(("TVPCreateFolders: cannot create " + partial +
						": " + strerror(errno)).c_str());
					return false;
				}
			}
		}
		if (i < path.size()) partial += '/';
	}

	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool TVPDeleteFile(const std::string &filename)
{
	return unlink(filename.c_str()) == 0;
}

bool TVPRenameFile(const std::string &from, const std::string &to)
{
	return rename(from.c_str(), to.c_str()) == 0;
}

bool TVPCopyFile(const std::string &from, const std::string &to)
{
	// Copies the *contents*; it is never a rename, and `to` may already exist
	// (FileSelectorForm::onPaste asks the user before overwriting).
	struct stat st;
	if (stat(from.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
		// Directories are not copied: the Android build delegates that to its
		// Java layer, and the only in-tree caller that handled folders lived in
		// the cocos2d file utils this port replaced.
		return false;
	}

	const int src = open(from.c_str(), O_RDONLY);
	if (src < 0) return false;
	const int dst = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (dst < 0) {
		close(src);
		return false;
	}

	std::vector<char> buffer(64 * 1024);
	bool ok = true;
	for (;;) {
		ssize_t got = read(src, &buffer[0], buffer.size());
		if (got < 0) {
			if (errno == EINTR) continue;
			ok = false;
			break;
		}
		if (got == 0) break;
		ssize_t written = 0;
		while (written < got) {
			const ssize_t n = write(dst, &buffer[written], got - written);
			if (n < 0) {
				if (errno == EINTR) continue;
				ok = false;
				break;
			}
			written += n;
		}
		if (!ok) break;
	}
	close(src);
	close(dst);

	if (!ok) {
		// do not leave a truncated copy behind
		unlink(to.c_str());
		return false;
	}
	return true;
}

bool TVPWriteDataToFile(const ttstr &filepath, const void *data, unsigned int len)
{
	// The Win32 version writes straight into the target and returns whether the
	// whole buffer reached the file.  Same contract here, but staged through
	// "<file>.tmp" and rename()d into place: a crash or a full disk can then not
	// leave a truncated GlobalPreference.xml, which the next start would parse.
	const std::string path = filepath.AsStdString();
	if (path.empty()) return false;

	// ConfigManager writes its preferences into a directory that the caller may
	// have just named but not created yet (a fresh profile, or a game's own
	// .preference directory), so make sure the parent exists first.
	const size_t slash = path.find_last_of('/');
	if (slash != std::string::npos && slash != 0) {
		const std::string parent = path.substr(0, slash);
		if (!TVPCheckExistentLocalFolder(ttstr(parent)) &&
			!TVPCreateFolders(ttstr(parent))) {
			TVPPrintLog(("TVPWriteDataToFile: cannot create " + parent).c_str());
			return false;
		}
	}

	const std::string temp = path + ".tmp";

	FILE *fp = fopen(temp.c_str(), "wb");
	const bool staged = (fp != nullptr);
	if (!fp) {
		// The directory is not writable but the file itself may be: retry the
		// plain write so the caller still sees the Win32 behaviour.
		fp = fopen(path.c_str(), "wb");
		if (!fp) {
			TVPPrintLog(("TVPWriteDataToFile: cannot open " + path + ": " +
				strerror(errno)).c_str());
			return false;
		}
	}

	bool ok = fwrite(data, 1, len, fp) == len;
	// Push the payload out before a rename could publish an empty file.
	if (ok && staged)
		ok = fflush(fp) == 0 && fsync(fileno(fp)) == 0;
	fclose(fp);

	if (!ok) {
		TVPPrintLog(("TVPWriteDataToFile: write failed for " + path).c_str());
		if (staged) remove(temp.c_str());
		return false;
	}
	if (staged && rename(temp.c_str(), path.c_str()) != 0) {
		TVPPrintLog(("TVPWriteDataToFile: cannot replace " + path + ": " +
			strerror(errno)).c_str());
		remove(temp.c_str());
		return false;
	}
	return true;
}

//---------------------------------------------------------------------------
// stat
//
// environ/Platform.h declares these and base/win32/StorageImpl.cpp only *calls*
// them (its original GetFileAttributes version is inside an `#if 0`), so this
// is the definition for the SDL2 build -- the same POSIX implementation the
// Android build uses.
//---------------------------------------------------------------------------
#undef st_atime
#undef st_ctime
#undef st_mtime

bool TVP_stat(const tjs_char *name, tTVP_stat &s)
{
	tTJSNarrowStringHolder holder(name);
	return TVP_stat(holder, s);
}

bool TVP_stat(const char *name, tTVP_stat &s)
{
	struct stat t;
	static_assert(sizeof(t.st_size) == 8, "64-bit off_t expected");
	const bool ret = stat(name, &t) == 0;
	s.st_mode = t.st_mode;
	s.st_size = t.st_size;
	s.st_atime = t.st_atim.tv_sec;
	s.st_mtime = t.st_mtim.tv_sec;
	s.st_ctime = t.st_ctim.tv_sec;
	return ret;
}

//---------------------------------------------------------------------------
// Shutdown
//---------------------------------------------------------------------------
void TVPExitApplication(int code)
{
	// The Win32 and Android versions both free the render caches and then
	// leave; on top of that TVPSystemUninit() runs first, which is what every
	// in-tree caller does around TVPExitApplication()
	// (tTVPApplication::ShowException, TVPTerminateSync).  It is guarded
	// internally, so an already-uninitialized system stays uninitialized.
	TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_MAX);
	if (!TVPIsSoftwareRenderManager())
		iTVPTexture2D::RecycleProcess();

	if (!TVPSystemUninitCalled)
		TVPSystemUninit();

	TVPTerminateCode = code;

	// A start-up failure reaches here as TVPExitApplication(0): the engine
	// catches the exception, shows it (which this port mirrors to the console
	// when no dialog is available) and exits, so the process status would claim
	// success.  If no game window ever appeared, no game ran: say what to check
	// and report a failing status, which is what a caller on the command line
	// (or a launcher on a handheld) needs to see.
	if (!krkr2sdl::HostEverCreatedWindow()) {
		fprintf(stderr,
			"krkr2: the game did not start - no startup script ran.\n"
			"       * a directory must contain startup.tjs\n"
			"       * an .xp3 must contain it (check the message above)\n"
			"       * an encrypted archive needs its patch beside it\n"
			"         (patch.tjs / patch.xp3 / xp3filter.tjs)\n");
		if (code == 0) code = 3;
	}

	exit(code);
}

//---------------------------------------------------------------------------
// Storage permission
//---------------------------------------------------------------------------
void TVPFetchSDCardPermission()
{
	// Android only: it asks the platform for READ/WRITE_EXTERNAL_STORAGE, which
	// Android 5+ grants from a system dialog, and TVPCheckStartupPath() there
	// retries with the requested permission.
	//
	// This target has no comparable runtime permission model: on the Linux
	// desktop access to a mount point follows the file mode and the process'
	// uid, and on the Switch it follows the mounted device's own access rules.
	// There is nothing to request and nothing to wait for, so the correct
	// behaviour is to make sure the storage the engine writes to exists and to
	// report plainly when it is unusable -- that is the decision the Android
	// dialog exists to drive.
	const std::string path = WritablePath();
	if (path.empty()) {
		TVPPrintLog("TVPFetchSDCardPermission: no writable storage directory");
		return;
	}
	if (!TVPCheckExistentLocalFolder(ttstr(path))) {
		if (!TVPCreateFolders(ttstr(path))) {
			TVPPrintLog(("TVPFetchSDCardPermission: cannot create " + path).c_str());
			return;
		}
	}
	if (access(path.c_str(), W_OK | X_OK) != 0) {
		TVPPrintLog(("TVPFetchSDCardPermission: " + path +
			" is not writable: " + strerror(errno)).c_str());
	}
}

//---------------------------------------------------------------------------
// Open a file or URL with the platform's handler
//---------------------------------------------------------------------------
void TVPSendToOtherApp(const std::string &filename)
{
#if defined(__linux__)
	// The libc has no shell-association API; xdg-open is the freedesktop.org
	// entry point and is what every desktop file manager uses to open a file or
	// URL with the user's default handler.  It is spawned rather than system()d
	// so no shell parses the filename, and it is reaped right away because it
	// returns as soon as the handler has been started.
	if (filename.empty()) return;
	const pid_t pid = fork();
	if (pid < 0) {
		TVPPrintLog(("TVPSendToOtherApp: fork failed: " +
			std::string(strerror(errno))).c_str());
		return;
	}
	if (pid == 0) {
		// child: detach from the engine's terminal and hand over to xdg-open
		int nullfd = open("/dev/null", O_RDWR);
		if (nullfd >= 0) {
			dup2(nullfd, STDIN_FILENO);
			dup2(nullfd, STDOUT_FILENO);
			dup2(nullfd, STDERR_FILENO);
			if (nullfd > STDERR_FILENO) close(nullfd);
		}
		setsid();
		execlp("xdg-open", "xdg-open", filename.c_str(), (char *)nullptr);
		_exit(127);
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		// retry
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		TVPPrintLog(("TVPSendToOtherApp: xdg-open could not open " + filename +
			" (is xdg-utils installed?)").c_str());
	}
#else
	// The Switch has no association database and no second application to hand
	// a path to, so there is nothing to launch.  Say so instead of dropping the
	// request silently.
	TVPPrintLog(("TVPSendToOtherApp: opening external files is not supported on "
		"this target: " + filename).c_str());
#endif
}

//---------------------------------------------------------------------------
// Current language
//
// LocaleConfigManager::Initialize() loads "locale/<code>.xml" and the resource
// tree ships en_us / ja_jp / zh_cn / zh_tw, so the expected form is the
// language plus the country, lower case -- the same shape the Win32
// implementation builds (it lower-cases the ISO-3166 country code), so
// "zh_CN.UTF-8" becomes "zh_cn".
//---------------------------------------------------------------------------
std::string TVPGetCurrentLanguage()
{
	const char *raw = getenv("LC_ALL");
	if (!raw || !*raw) raw = getenv("LC_MESSAGES");
	if (!raw || !*raw) raw = getenv("LANG");
	if (!raw || !*raw) return ""; // Android reports nothing too; the manager
	                              // falls back to en_us in that case

	std::string locale(raw);
	// strip the codeset and the modifier: "zh_CN.UTF-8@euro" -> "zh_CN"
	const size_t cut = locale.find_first_of(".@");
	if (cut != std::string::npos) locale.erase(cut);
	std::transform(locale.begin(), locale.end(), locale.begin(), ::tolower);

	// "C" and "POSIX" are the unlocalized POSIX locale: no language at all.
	if (locale == "c" || locale == "posix" || locale.empty()) return "";
	return locale;
}
