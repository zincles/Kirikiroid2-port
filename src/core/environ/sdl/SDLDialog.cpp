/*
	Shared modal-dialog helpers (see SDLDialog.h): FreeType text rendering,
	UTF-8 helpers, SDL text-input ownership, the single line editor and the
	scripted-input driver used by the acceptance runs.
*/

#include "tjsCommHead.h"

#include "SDLDialog.h"
#include "Host.h" // HostWindow
#include "Platform.h" // TVPPrintLog
#include "StorageIntf.h" // TVPGetAppPath

#include <ft2build.h>
#include FT_FREETYPE_H

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace krkr2sdl {

//===========================================================================
// UTF-8 helpers
//===========================================================================
size_t UTF8SeqLen(unsigned char c)
{
	if (c < 0x80) return 1;
	if ((c & 0xE0) == 0xC0) return 2;
	if ((c & 0xF0) == 0xE0) return 3;
	if ((c & 0xF8) == 0xF0) return 4;
	return 1;
}

size_t UTF8Next(const std::string &s, size_t i)
{
	if (i >= s.size()) return s.size();
	size_t n = UTF8SeqLen((unsigned char)s[i]);
	if (i + n > s.size()) n = 1;
	return i + n;
}

size_t UTF8Prev(const std::string &s, size_t i)
{
	if (i == 0) return 0;
	size_t j = i - 1;
	int guard = 0;
	while (j > 0 && (((unsigned char)s[j] & 0xC0) == 0x80) && guard++ < 3)
		--j;
	return j;
}

tjs_uint32 UTF8Decode(const std::string &s, size_t i, size_t &len)
{
	const unsigned char c = (unsigned char)s[i];
	len = UTF8SeqLen(c);
	if (len == 1 || i + len > s.size()) {
		len = 1;
		return c;
	}
	tjs_uint32 cp = 0;
	switch (len) {
	case 2: cp = c & 0x1F; break;
	case 3: cp = c & 0x0F; break;
	default: cp = c & 0x07; break;
	}
	for (size_t k = 1; k < len; ++k) {
		const unsigned char cc = (unsigned char)s[i + k];
		if ((cc & 0xC0) != 0x80) {
			len = 1;
			return c;
		}
		cp = (cp << 6) | (cc & 0x3F);
	}
	return cp;
}

size_t UTF8CharToByte(const std::string &s, int n)
{
	size_t i = 0;
	for (int k = 0; k < n && i < s.size(); ++k)
		i = UTF8Next(s, i);
	return i;
}

//===========================================================================
// FreeType state
//
// Process lifetime, like every other singleton in the engine: the font library
// is shared by every dialog and is deliberately not torn down at exit (tearing
// it down from an exit path would race with the dialog in flight).
//===========================================================================
namespace {

struct tTVPDialogFont
{
	FT_Library Library = nullptr;
	std::vector<FT_Face> Faces;       // loaded, in fallback order
	std::vector<std::string> Pending; // candidate files not opened yet
	int PixelSize = 0;
	int Ascent = 0;
	int LineHeight = 0;
	bool Initialized = false;
};

tTVPDialogFont _DialogFont;

bool TryLoadFace(const std::string &path)
{
	if (path.empty() || _DialogFont.Library == nullptr) return false;
	FT_Face face = nullptr;
	if (FT_New_Face(_DialogFont.Library, path.c_str(), 0, &face) != 0)
		return false;
	_DialogFont.Faces.push_back(face);
	return true;
}

// Collects candidate font files.  The order is deliberate:
//	1. the game's own font, exactly the names visual/FontImpl.cpp looks for
//	2. fonts the user dropped next to the game
//	3. well-known system fonts, CJK-capable ones first (they also cover Latin)
//	4. a bounded scan of the desktop font directories
void CollectFontCandidates(std::vector<std::string> &out)
{
	const std::string app = TVPGetAppPath().AsStdString();
	static const char *const appNames[] = {
		"default.ttf", "default.ttc", "default.otf", "default.otc",
	};
	for (size_t i = 0; i < sizeof(appNames) / sizeof(appNames[0]); ++i)
		out.push_back(app + appNames[i]);

	// <app>/fonts, the directory FontImpl.cpp also scans
	DIR *dir = opendir((app + "fonts").c_str());
	if (dir) {
		std::vector<std::string> files;
		while (struct dirent *ent = readdir(dir)) {
			const std::string name(ent->d_name);
			const size_t dot = name.find_last_of('.');
			if (dot == std::string::npos) continue;
			std::string ext(name.substr(dot));
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			if (ext == ".ttf" || ext == ".ttc" || ext == ".otf" || ext == ".otc")
				files.push_back(app + "fonts/" + name);
		}
		closedir(dir);
		std::sort(files.begin(), files.end());
		out.insert(out.end(), files.begin(), files.end());
	}

	static const char *const systemNames[] = {
		// CJK-capable: a KiriKiri game is usually Japanese or Chinese
		"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
		"/usr/local/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
		"/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
		"/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
		"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
		"/usr/share/fonts/truetype/arphic/uming.ttc",
		"/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
		"/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
		"/system/fonts/NotoSansCJK-Regular.ttc",
		"/system/fonts/DroidSansFallback.ttf",
		// Latin-only fallbacks
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
		"/usr/share/fonts/TTF/DejaVuSans.ttf",
	};
	for (size_t i = 0; i < sizeof(systemNames) / sizeof(systemNames[0]); ++i)
		out.push_back(systemNames[i]);
}

// Depth-limited recursive scan for font files; stops as soon as `limit` files
// have been collected, because every collected file may end up as an open
// FreeType face.
void ScanFontDir(const std::string &dir, int depth, size_t limit,
	std::vector<std::string> &out)
{
	if (depth < 0 || out.size() >= limit) return;
	DIR *d = opendir(dir.c_str());
	if (!d) return;
	std::vector<std::string> subdirs;
	while (struct dirent *ent = readdir(d)) {
		const std::string name(ent->d_name);
		if (name == "." || name == "..") continue;
		const std::string full = dir + "/" + name;
		struct stat st;
		if (stat(full.c_str(), &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) {
			subdirs.push_back(full);
			continue;
		}
		if (!S_ISREG(st.st_mode)) continue;
		const size_t dot = name.find_last_of('.');
		if (dot == std::string::npos) continue;
		std::string ext(name.substr(dot));
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext == ".ttf" || ext == ".ttc" || ext == ".otf" || ext == ".otc") {
			out.push_back(full);
			if (out.size() >= limit) break;
		}
	}
	closedir(d);
	std::sort(subdirs.begin(), subdirs.end());
	for (size_t i = 0; i < subdirs.size() && out.size() < limit; ++i)
		ScanFontDir(subdirs[i], depth - 1, limit, out);
}

void CollectSystemFontScan(std::vector<std::string> &out)
{
	const size_t limit = out.size() + 16;
	const char *home = getenv("HOME");
	if (home && *home) {
		ScanFontDir(std::string(home) + "/.local/share/fonts", 3, limit, out);
		ScanFontDir(std::string(home) + "/.fonts", 3, limit, out);
	}
	ScanFontDir("/usr/local/share/fonts", 3, limit, out);
	ScanFontDir("/usr/share/fonts", 3, limit, out);
}

// The face that can render `cp`, loading further candidates on demand so mixed
// scripts (a Japanese game with an English path, say) do not fall back to
// .notdef.  Returns nullptr when nothing is loaded at all.
FT_Face FaceFor(tjs_uint32 cp)
{
	for (size_t i = 0; i < _DialogFont.Faces.size(); ++i) {
		if (FT_Get_Char_Index(_DialogFont.Faces[i], cp) != 0)
			return _DialogFont.Faces[i];
	}
	while (!_DialogFont.Pending.empty()) {
		const std::string path = _DialogFont.Pending.front();
		_DialogFont.Pending.erase(_DialogFont.Pending.begin());
		if (!TryLoadFace(path)) continue;
		FT_Face face = _DialogFont.Faces.back();
		FT_Set_Pixel_Sizes(face, 0, _DialogFont.PixelSize);
		if (FT_Get_Char_Index(face, cp) != 0)
			return face;
	}
	return _DialogFont.Faces.empty() ? nullptr : _DialogFont.Faces[0];
}

// Copies one FreeType coverage bitmap into an ARGB8888 surface as straight
// (non-premultiplied) alpha, which is what SDL_BLENDMODE_BLEND expects.
void BlitCoverage(SDL_Surface *surface, const FT_Bitmap &bitmap,
	int dstx, int dsty, SDL_Color color)
{
	if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) return;
	for (unsigned int row = 0; row < bitmap.rows; ++row) {
		const int y = dsty + (int)row;
		if (y < 0 || y >= surface->h) continue;
		const unsigned char *src = bitmap.buffer + row * bitmap.pitch;
		Uint32 *dst = (Uint32 *)((Uint8 *)surface->pixels +
			(size_t)y * surface->pitch);
		for (unsigned int col = 0; col < bitmap.width; ++col) {
			const int x = dstx + (int)col;
			if (x < 0 || x >= surface->w) continue;
			const unsigned int a = (unsigned int)src[col] * color.a / 255;
			if (a == 0) continue;
			// Glyph boxes may touch for italic faces; overwriting the coverage
			// of a neighbour is invisible there and avoids a second blend.
			dst[x] = SDL_MapRGBA(surface->format, color.r, color.g, color.b,
				(Uint8)a);
		}
	}
}

} // anonymous namespace

bool PrepareDialogFont(int pixel_size)
{
	if (!_DialogFont.Initialized) {
		_DialogFont.Initialized = true;
		if (FT_Init_FreeType(&_DialogFont.Library) != 0) {
			_DialogFont.Library = nullptr;
			TVPPrintLog("dialog: FreeType initialization failed");
			return false;
		}
		std::vector<std::string> candidates;
		CollectFontCandidates(candidates);
		CollectSystemFontScan(candidates);
		if (candidates.size() > 48)
			candidates.resize(48); // bound the fallback work per unknown glyph
		size_t next = 0;
		for (; next < candidates.size(); ++next) {
			if (TryLoadFace(candidates[next])) {
				++next;
				break;
			}
		}
		if (_DialogFont.Faces.empty()) {
			TVPPrintLog("dialog: no usable font file found (looked for "
				"default.ttf next to the game and in the system font "
				"directories)");
			return false;
		}
		// The remaining candidates are only opened if a code point turned out
		// to be missing from the faces loaded so far (see FaceFor).
		for (; next < candidates.size(); ++next)
			_DialogFont.Pending.push_back(candidates[next]);
	}

	if (_DialogFont.PixelSize != pixel_size) {
		for (size_t i = 0; i < _DialogFont.Faces.size(); ++i)
			FT_Set_Pixel_Sizes(_DialogFont.Faces[i], 0, pixel_size);
		_DialogFont.PixelSize = pixel_size;
		FT_Face face = _DialogFont.Faces[0];
		_DialogFont.Ascent = (int)(face->size->metrics.ascender >> 6);
		const int descent = (int)((-face->size->metrics.descender) >> 6);
		_DialogFont.LineHeight = _DialogFont.Ascent + descent + 2;
	}
	return true;
}

int DialogFontPixelSize()
{
	return _DialogFont.PixelSize;
}

int DialogLineHeight()
{
	return _DialogFont.LineHeight;
}

int MeasureGlyphRun(const std::string &utf8)
{
	int width = 0;
	size_t i = 0;
	while (i < utf8.size()) {
		size_t len = 0;
		const tjs_uint32 cp = UTF8Decode(utf8, i, len);
		i += len;
		if (cp == '\n' || cp == '\r') continue;
		FT_Face face = FaceFor(cp);
		if (!face) {
			width += _DialogFont.PixelSize / 2;
			continue;
		}
		if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) == 0)
			width += (int)(face->glyph->advance.x >> 6);
	}
	return width;
}

int MeasureChar(tjs_uint32 cp)
{
	FT_Face face = FaceFor(cp);
	if (!face) return _DialogFont.PixelSize / 2;
	if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) != 0) return 0;
	return (int)(face->glyph->advance.x >> 6);
}

int DrawGlyphRun(SDL_Renderer *renderer, const std::string &utf8, int x, int y,
	SDL_Color color)
{
	int width = MeasureGlyphRun(utf8);
	if (width <= 0) return x;
	const int height = _DialogFont.LineHeight;

	SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
		SDL_PIXELFORMAT_ARGB8888);
	if (!surface) return x + width;
	SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);
	SDL_FillRect(surface, nullptr, 0);

	long long pen = 0; // 26.6 fixed point
	size_t i = 0;
	while (i < utf8.size()) {
		size_t len = 0;
		const tjs_uint32 cp = UTF8Decode(utf8, i, len);
		i += len;
		if (cp == '\n' || cp == '\r') continue;
		FT_Face face = FaceFor(cp);
		if (!face) {
			pen += (long long)(_DialogFont.PixelSize / 2) << 6;
			continue;
		}
		if (FT_Load_Char(face, cp, FT_LOAD_RENDER) != 0) continue;
		const FT_GlyphSlot slot = face->glyph;
		BlitCoverage(surface, slot->bitmap,
			(int)(pen >> 6) + slot->bitmap_left,
			_DialogFont.Ascent - slot->bitmap_top, color);
		pen += slot->advance.x;
	}

	SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_FreeSurface(surface);
	if (!texture) return x + width;
	SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	SDL_Rect dst = { x, y, width, height };
	SDL_RenderCopy(renderer, texture, nullptr, &dst);
	SDL_DestroyTexture(texture);
	return x + width;
}

std::vector<std::string> WrapText(const std::string &text, int max_width)
{
	std::vector<std::string> lines;
	if (text.empty()) return lines;
	size_t pos = 0;
	while (pos <= text.size()) {
		size_t nl = text.find('\n', pos);
		std::string paragraph = (nl == std::string::npos)
			? text.substr(pos) : text.substr(pos, nl - pos);
		if (!paragraph.empty() && paragraph[paragraph.size() - 1] == '\r')
			paragraph.erase(paragraph.size() - 1);

		size_t i = 0;
		std::string line;
		while (i < paragraph.size()) {
			// one word: up to the next space, or one code point for scripts
			// without spaces
			size_t wordEnd = i;
			if (paragraph[i] == ' ' || paragraph[i] == '\t') {
				wordEnd = UTF8Next(paragraph, i);
			} else {
				while (wordEnd < paragraph.size() &&
					paragraph[wordEnd] != ' ' && paragraph[wordEnd] != '\t')
					wordEnd = UTF8Next(paragraph, wordEnd);
				// keep a trailing space with the word
				while (wordEnd < paragraph.size() &&
					(paragraph[wordEnd] == ' ' || paragraph[wordEnd] == '\t'))
					wordEnd = UTF8Next(paragraph, wordEnd);
			}
			const std::string word = paragraph.substr(i, wordEnd - i);
			i = wordEnd;

			if (!line.empty() && MeasureGlyphRun(line + word) > max_width) {
				lines.push_back(line);
				line.clear();
			}
			if (MeasureGlyphRun(word) > max_width && line.empty()) {
				// the word does not fit on a line of its own: break it at the
				// code point that crosses the edge
				size_t k = 0;
				std::string piece;
				while (k < word.size()) {
					const size_t n = UTF8Next(word, k);
					const std::string next = piece + word.substr(k, n - k);
					if (!piece.empty() && MeasureGlyphRun(next) > max_width) {
						lines.push_back(piece);
						piece.clear();
						continue;
					}
					piece = next;
					k = n;
				}
				line = piece;
				continue;
			}
			line += word;
		}
		lines.push_back(line);
		if (nl == std::string::npos) break;
		pos = nl + 1;
	}
	return lines;
}

//===========================================================================
// SDL text input ownership
//
// TVPShowIME()/TVPHideIME() are the engine's entry points for the platform
// IME, while a modal dialog runs its own text input.  The two share SDL's
// single text-input state, so a dialog takes ownership while it is up: the
// engine's calls only record what it wants (rectangle and on/off) and that
// state is restored when the dialog closes.
//===========================================================================
namespace {

bool _SDLTextInputOwnedByDialog = false;
bool _SDLTextInputWanted = false; // what TVPShowIME/TVPHideIME last asked for
bool _SDLTextInputRectValid = false;
SDL_Rect _SDLTextInputRect = { 0, 0, 0, 0 };

} // anonymous namespace

bool TextInputUsable()
{
	return HostWindow() != nullptr && SDL_WasInit(SDL_INIT_VIDEO) != 0;
}

void TextInputEngineWants(bool on, const SDL_Rect &rect)
{
	_SDLTextInputWanted = on;
	if (on && rect.w > 0 && rect.h > 0) {
		_SDLTextInputRect = rect;
		_SDLTextInputRectValid = true;
	}
	if (_SDLTextInputOwnedByDialog) return; // recorded; the dialog applies it
	if (!TextInputUsable()) return;
	if (on) {
		if (_SDLTextInputRectValid) SDL_SetTextInputRect(&_SDLTextInputRect);
		if (!SDL_IsTextInputActive()) SDL_StartTextInput();
	} else if (SDL_IsTextInputActive()) {
		SDL_StopTextInput();
	}
}

void TextInputAcquire(const SDL_Rect &rect)
{
	_SDLTextInputOwnedByDialog = true;
	if (!TextInputUsable()) return;
	SDL_StartTextInput();
	SDL_SetTextInputRect(&rect);
}

void TextInputRelease()
{
	_SDLTextInputOwnedByDialog = false;
	if (!TextInputUsable()) return;
	// Give the engine its text input back the way it left it: if it had asked
	// for an IME rectangle (TVPShowIME) that state is restored, and otherwise
	// SDL's text input goes back off (TVPHideIME / never asked).
	if (_SDLTextInputWanted) {
		if (_SDLTextInputRectValid) SDL_SetTextInputRect(&_SDLTextInputRect);
		SDL_StartTextInput();
	} else if (SDL_IsTextInputActive()) {
		SDL_StopTextInput();
	}
}

//===========================================================================
// Single line editor
//===========================================================================
void TextEdit::Reset(const std::string &text)
{
	Text = text;
	Caret = Text.size();
	Composition.clear();
	CompositionCursor = 0;
}

void TextEdit::Insert(const std::string &utf8)
{
	// A commit replaces the in-progress composition: SDL delivers the
	// committed text through SDL_TEXTINPUT and then clears SDL_TEXTEDITING.
	Composition.clear();
	CompositionCursor = 0;
	if (utf8.empty()) return;
	// The field is single line: control characters (TAB, RETURN, and anything
	// else SDL may hand over) are not text.
	std::string clean;
	clean.reserve(utf8.size());
	for (size_t i = 0; i < utf8.size(); ++i) {
		const unsigned char c = (unsigned char)utf8[i];
		if (c < 0x20 || c == 0x7F) continue;
		clean += utf8[i];
	}
	if (clean.empty()) return;
	Text.insert(Caret, clean);
	Caret += clean.size();
}

void TextEdit::PasteClipboard()
{
	char *clipboard = SDL_GetClipboardText();
	if (!clipboard) return;
	const std::string raw(clipboard);
	SDL_free(clipboard);
	std::string clean;
	clean.reserve(raw.size());
	for (size_t i = 0; i < raw.size(); ++i) {
		// the field is single line
		if (raw[i] != '\r' && raw[i] != '\n' && raw[i] != '\t')
			clean += raw[i];
	}
	Insert(clean);
}

void TextEdit::OnTextEditing(const SDL_TextEditingEvent &edit)
{
	Composition.assign(edit.text);
	const int cursor = edit.start + (edit.length > 0 ? edit.length : 0);
	CompositionCursor = UTF8CharToByte(Composition, cursor);
}

TextEdit::KeyResult TextEdit::OnKeyDown(const SDL_KeyboardEvent &key)
{
	// While an IME composition is active it owns Backspace/Return/arrows;
	// acting on them here as well would edit the text twice.
	if (!Composition.empty()) return KeyUnhandled;

	const bool ctrl = (key.keysym.mod & KMOD_CTRL) != 0;
	switch (key.keysym.sym) {
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		return KeyAccept;
	case SDLK_ESCAPE:
		return KeyCancel;
	case SDLK_BACKSPACE:
		if (Caret > 0) {
			const size_t prev = UTF8Prev(Text, Caret);
			Text.erase(prev, Caret - prev);
			Caret = prev;
		}
		break;
	case SDLK_DELETE:
		if (Caret < Text.size()) {
			const size_t next = UTF8Next(Text, Caret);
			Text.erase(Caret, next - Caret);
		}
		break;
	case SDLK_LEFT:
		if (ctrl) { // Ctrl+Left: to the start of the previous word
			while (Caret > 0 && Text[UTF8Prev(Text, Caret)] == ' ')
				Caret = UTF8Prev(Text, Caret);
			while (Caret > 0 && Text[UTF8Prev(Text, Caret)] != ' ')
				Caret = UTF8Prev(Text, Caret);
		} else {
			Caret = UTF8Prev(Text, Caret);
		}
		break;
	case SDLK_RIGHT:
		if (ctrl) { // Ctrl+Right: past the current word
			while (Caret < Text.size() && Text[Caret] != ' ')
				Caret = UTF8Next(Text, Caret);
			while (Caret < Text.size() && Text[Caret] == ' ')
				Caret = UTF8Next(Text, Caret);
		} else {
			Caret = UTF8Next(Text, Caret);
		}
		break;
	case SDLK_HOME: Caret = 0; break;
	case SDLK_END: Caret = Text.size(); break;
	case SDLK_v:
		if (!ctrl) return KeyUnhandled;
		PasteClipboard();
		break;
	default:
		return KeyUnhandled; // printable characters arrive as SDL_TEXTINPUT
	}
	return KeyHandled;
}

int TextEdit::CaretLocalX(int padding) const
{
	int x = padding + MeasureGlyphRun(Text.substr(0, Caret));
	if (!Composition.empty())
		x += MeasureGlyphRun(Composition.substr(0, CompositionCursor));
	return x;
}

void TextEdit::KeepCaretVisible(int field_width, int padding, int &scroll) const
{
	const int caret = CaretLocalX(padding);
	const int limit = field_width - padding;
	if (caret - scroll > limit)
		scroll = caret - limit;
	if (caret - scroll < padding)
		scroll = caret - padding;
	if (scroll < 0) scroll = 0;
}

void TextEdit::SetCaretFromX(int local_x, int padding)
{
	const int target = local_x - padding;
	size_t i = 0;
	int x = 0;
	while (i < Text.size()) {
		size_t len = 0;
		const tjs_uint32 cp = UTF8Decode(Text, i, len);
		const int w = MeasureChar(cp);
		if (target < x + w / 2) break;
		x += w;
		i += len;
	}
	Caret = i;
	// a click during composition cancels it, as it does in native fields
	Composition.clear();
	CompositionCursor = 0;
}

void TextEdit::Draw(SDL_Renderer *renderer, const SDL_Rect &field, int padding,
	int scroll, bool caret_visible, SDL_Color color) const
{
	const int textY = field.y + (field.h - DialogLineHeight()) / 2;
	SDL_Rect clip = { field.x + 1, field.y + 1, field.w - 2, field.h - 2 };
	SDL_RenderSetClipRect(renderer, &clip);

	const int x0 = field.x + padding - scroll;
	const int prefixEnd = DrawGlyphRun(renderer, Text.substr(0, Caret), x0,
		textY, color);
	// The caret sits at the IME's insertion point inside the composition,
	// while the rest of the composition and the text after it continue after
	// the whole composition.
	int caretX = prefixEnd;
	int afterCompositionX = prefixEnd;
	if (!Composition.empty()) {
		caretX = DrawGlyphRun(renderer, Composition.substr(0, CompositionCursor),
			prefixEnd, textY, color);
		afterCompositionX = DrawGlyphRun(renderer,
			Composition.substr(CompositionCursor), caretX, textY, color);
		// underline the composition, as every IME-aware field does
		SDL_SetRenderDrawColor(renderer, 0x5C, 0x8F, 0xD8, 0xFF);
		SDL_Rect underline = { prefixEnd, textY + DialogLineHeight() - 3,
			MeasureGlyphRun(Composition), 1 };
		SDL_RenderFillRect(renderer, &underline);
	}
	DrawGlyphRun(renderer, Text.substr(Caret), afterCompositionX, textY, color);

	if (caret_visible) {
		SDL_SetRenderDrawColor(renderer, 0xF2, 0xF4, 0xF8, 0xFF);
		SDL_Rect caret = { caretX, textY, 2, DialogLineHeight() };
		SDL_RenderFillRect(renderer, &caret);
	}
	SDL_RenderSetClipRect(renderer, nullptr);
}

//===========================================================================
// Scripted dialog input
//===========================================================================
namespace {

struct ScriptedStep
{
	SDL_Event Event;
	bool IsWait = false;
	Uint32 WaitMs = 0;
};

std::vector<ScriptedStep> &ScriptedSteps()
{
	static std::vector<ScriptedStep> steps = []() {
		std::vector<ScriptedStep> out;
		const char *env = getenv("KRKR2_DIALOG_KEYS");
		if (!env || !*env) return out;
		std::string script(env);
		size_t pos = 0;
		while (pos <= script.size()) {
			const size_t comma = script.find(',', pos);
			std::string token = (comma == std::string::npos)
				? script.substr(pos) : script.substr(pos, comma - pos);
			pos = (comma == std::string::npos) ? script.size() + 1 : comma + 1;
			// trim surrounding blanks; a 'text:' payload is taken verbatim, so
			// that "text: " is a single space, as SDL would deliver it
			while (!token.empty() && (token[0] == ' ' || token[0] == '\t'))
				token.erase(0, 1);
			if (token.empty()) continue;
			const bool verbatim = token.compare(0, 5, "text:") == 0;
			if (!verbatim) {
				while (!token.empty() && (token[token.size() - 1] == ' ' ||
					token[token.size() - 1] == '\t'))
					token.erase(token.size() - 1);
				if (token.empty()) continue;
			}

			ScriptedStep step;
			SDL_zero(step.Event);
			std::string payload;
			const size_t colon = token.find(':');
			std::string head = colon == std::string::npos
				? token : token.substr(0, colon);
			if (colon != std::string::npos) payload = token.substr(colon + 1);
			std::transform(head.begin(), head.end(), head.begin(), ::tolower);

			if (head == "wait") {
				step.IsWait = true;
				step.WaitMs = (Uint32)strtoul(payload.c_str(), nullptr, 10);
				out.push_back(step);
				continue;
			}
			if (head == "text") {
				step.Event.type = SDL_TEXTINPUT;
				std::strncpy(step.Event.text.text, payload.c_str(),
					sizeof(step.Event.text.text) - 1);
				out.push_back(step);
				continue;
			}
			if (head == "click" || head == "dclick" || head == "wheelup" ||
				head == "wheeldown") {
				int x = 0, y = 0;
				const size_t sep = payload.find(':');
				if (sep != std::string::npos) {
					x = atoi(payload.substr(0, sep).c_str());
					y = atoi(payload.substr(sep + 1).c_str());
				}
				step.Event.type = (head == "wheelup" || head == "wheeldown")
					? SDL_MOUSEWHEEL : SDL_MOUSEBUTTONDOWN;
				if (step.Event.type == SDL_MOUSEWHEEL) {
					step.Event.wheel.x = x;
					step.Event.wheel.y = (head == "wheelup") ? 1 : -1;
					step.Event.wheel.mouseX = x;
					step.Event.wheel.mouseY = y;
					// SDL_MOUSEWHEEL carries no position: push a motion first
					// so the dialog knows where the pointer is (as a real
					// wheel event is always preceded by one).
					ScriptedStep motion;
					SDL_zero(motion.Event);
					motion.Event.type = SDL_MOUSEMOTION;
					motion.Event.motion.x = x;
					motion.Event.motion.y = y;
					out.push_back(motion);
				} else {
					step.Event.button.button = SDL_BUTTON_LEFT;
					step.Event.button.state = SDL_PRESSED;
					step.Event.button.clicks = (head == "dclick") ? 2 : 1;
					step.Event.button.x = x;
					step.Event.button.y = y;
				}
				out.push_back(step);
				continue;
			}
			if (head == "pad" || head == "padbutton") {
				const Uint8 button = krkr2sdl::HostGamePadButtonByName(payload);
				if (button == 0xFF) {
					TVPPrintLog(("dialog: KRKR2_DIALOG_KEYS: unknown game pad "
						"button '" + payload + "'").c_str());
					continue;
				}
				step.Event.type = SDL_CONTROLLERBUTTONDOWN;
				step.Event.cbutton.button = button;
				step.Event.cbutton.state = SDL_PRESSED;
				out.push_back(step);
				continue;
			}
			if (head == "axis") {
				const size_t sep = payload.find(':');
				std::string name = sep == std::string::npos
					? payload : payload.substr(0, sep);
				const Sint16 value = (Sint16)(sep == std::string::npos
					? 0 : atoi(payload.substr(sep + 1).c_str()));
				std::transform(name.begin(), name.end(), name.begin(),
					::tolower);
				Uint8 axis = 0xFF;
				if (name == "leftx") axis = SDL_CONTROLLER_AXIS_LEFTX;
				else if (name == "lefty") axis = SDL_CONTROLLER_AXIS_LEFTY;
				else if (name == "rightx") axis = SDL_CONTROLLER_AXIS_RIGHTX;
				else if (name == "righty") axis = SDL_CONTROLLER_AXIS_RIGHTY;
				if (axis == 0xFF) {
					TVPPrintLog(("dialog: KRKR2_DIALOG_KEYS: unknown axis '" +
						name + "'").c_str());
					continue;
				}
				step.Event.type = SDL_CONTROLLERAXISMOTION;
				step.Event.caxis.axis = axis;
				step.Event.caxis.value = value;
				out.push_back(step);
				continue;
			}

			// named keys
			SDL_Keycode sym = SDLK_UNKNOWN;
			if (head == "up") sym = SDLK_UP;
			else if (head == "down") sym = SDLK_DOWN;
			else if (head == "left") sym = SDLK_LEFT;
			else if (head == "right") sym = SDLK_RIGHT;
			else if (head == "home") sym = SDLK_HOME;
			else if (head == "end") sym = SDLK_END;
			else if (head == "pageup" || head == "pgup") sym = SDLK_PAGEUP;
			else if (head == "pagedown" || head == "pgdn") sym = SDLK_PAGEDOWN;
			else if (head == "enter" || head == "return") sym = SDLK_RETURN;
			else if (head == "esc" || head == "escape") sym = SDLK_ESCAPE;
			else if (head == "backspace") sym = SDLK_BACKSPACE;
			else if (head == "delete" || head == "del") sym = SDLK_DELETE;
			else if (head == "tab") sym = SDLK_TAB;
			else if (head == "space") sym = SDLK_SPACE;
			else if (head == "paste") sym = SDLK_v;
			else if (head == "w") sym = SDLK_w;
			else if (head == "s") sym = SDLK_s;
			else if (head.size() == 2 && head[0] == 'f' && head[1] >= '1' &&
				head[1] <= '9')
				sym = SDLK_F1 + (head[1] - '1');
			if (sym == SDLK_UNKNOWN) {
				TVPPrintLog(("dialog: KRKR2_DIALOG_KEYS: unknown step '" +
					token + "'").c_str());
				continue;
			}
			step.Event.type = SDL_KEYDOWN;
			step.Event.key.type = SDL_KEYDOWN;
			step.Event.key.state = SDL_PRESSED;
			step.Event.key.keysym.sym = sym;
			step.Event.key.keysym.mod = (head == "paste") ? KMOD_CTRL : KMOD_NONE;
			out.push_back(step);
		}
		return out;
	}();
	return steps;
}

bool _ScriptedExhaustedLogged = false;
size_t _ScriptedNext = 0;

} // anonymous namespace

void ResetScriptedDialogInput()
{
	_ScriptedNext = 0;
	_ScriptedExhaustedLogged = false;
}

bool PumpScriptedDialogInput()
{
	std::vector<ScriptedStep> &steps = ScriptedSteps();
	if (steps.empty()) return false;
	if (_ScriptedNext >= steps.size()) {
		if (!_ScriptedExhaustedLogged) {
			_ScriptedExhaustedLogged = true;
			TVPPrintLog("(info) KRKR2_DIALOG_KEYS is exhausted; the dialog now "
				"waits for real input");
		}
		return false;
	}
	// One step in flight at a time: a step is pushed only once the queue is
	// empty, which keeps the script in step with the events the dialog
	// actually processes (and drains stray host events first).  Without this
	// gate the steps would run ahead whenever the dialog waited on something
	// else, and a 'wait:' step would be consumed before the key before it.
	SDL_Event peek;
	if (SDL_PeepEvents(&peek, 1, SDL_PEEKEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0)
		return false;

	const ScriptedStep &step = steps[_ScriptedNext++];
	if (step.IsWait) {
		SDL_Delay(step.WaitMs);
		return true; // a pause is still a step: keep the dialog pumping
	}
	SDL_Event event = step.Event;
	if (SDL_PushEvent(&event) < 0)
		TVPPrintLog("(warning) KRKR2_DIALOG_KEYS: SDL_PushEvent failed");
	return true;
}

} // namespace krkr2sdl
