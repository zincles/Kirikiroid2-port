/*
	File selection dialog for the SDL2 targets (Linux desktop, Nintendo Switch).
	This is the definition of TVPShowFileSelector(), which the engine reaches
	through Storages.selectFile() (base/win32/StorageImpl.cpp registers the
	method and base/win32/FileSelector.cpp builds its arguments).

	It replaces the stub Host.cpp used to carry, which logged "not implemented"
	and reported a cancel.  Without it a Switch build cannot open a game at all
	(there is no command line to pass one) and scripts that call
	Storages.selectFile() get nothing back.

	The dialog is synchronous and modal: the engine calls it from the script
	thread while the frame pump is not running, so it pumps SDL_PollEvent
	itself -- exactly like the input box in Platform.cpp.  Both dialogs share
	their text rendering and text-input handling through SDLDialog.{h,cpp}.

	Behaviour
	---------
	* Lists the current directory: ".." first (when the directory has a parent,
	  i.e. not at the top of a volume), then directories, then files, each sorted
	  case-insensitively by name; the selected row is the cursor.  Activating
	  ".." goes to the parent, which is the visible equivalent of BACKSPACE and
	  the only one a game pad has; going up leaves the cursor on the directory
	  that was left, so up-then-back-in is two keystrokes.  The cursor starts on
	  the first real entry rather than on "..", so ENTER opens something rather
	  than leaving the directory.
	* Open mode: the `filename` argument filters the listing (DOS wildcards and
	  plain substrings, ';'/space separated alternatives).  The field is that
	  filter and doubles as the "type to jump" line: typing while the list is
	  focused moves the focus into it and filters as you type.  W/S/SPACE are
	  navigation keys in the list and text in the field.
	* Save mode: the field is the file name, seeded from `filename`; the
	  listing is not filtered so the whole directory stays browsable.
	* Navigation: keyboard (UP/DOWN or W/S, PAGEUP/PAGEDOWN, HOME/END,
	  ENTER/SPACE, BACKSPACE/LEFT, TAB, F1 roots, F2 chooses the current folder
	  in open mode, ESC), mouse (click to select, click again to activate,
	  double click, wheel, the buttons, the root label, the ".." row) and game pad
	  (DPAD/stick, A, B, X, Y, START, BACK, shoulders) -- the Switch has no
	  keyboard, so the game pad path is the one that matters there.
	* Roots: TVPGetDriverPath() (the mounted volumes, "/" and $HOME on the
	  desktop; sdmc: on the Switch); F1, the root label or BACK cycles them.
	* Returns the absolute path of the chosen file or folder, or "" when the
	  dialog was cancelled (the engine's callers read that as "nothing
	  selected").  Nothing is allocated across calls: every texture and surface
	  is released before the dialog returns, and SDL's text input is released
	  on every exit path.

	Debug/acceptance knobs, both off by default:
	  KRKR2_DIALOG_KEYS   scripted SDL events (see SDLDialog.h), replayed for
	                      every dialog, so a headless run is deterministic.
	  KRKR2_FILESEL_TRACE one line per state change (directory, cursor, filter,
	                      roots), i.e. the visited state of a scripted run.

	A deterministic run therefore looks like this (offscreen needs no display;
	without a display the SDL video driver must be picked explicitly):

	  SDL_VIDEODRIVER=offscreen KRKR2_DIALOG_KEYS=down,enter \
	    KRKR2_FILESEL_TRACE=1 ./build/krkr2 tests/file-selector/open
*/

#include "tjsCommHead.h"

#include "SDLDialog.h"
#include "Host.h" // krkr2sdl::HostWindow/HostRenderer/HostRequestQuit
#include "Platform.h" // TVPGetDriverPath, TVPPrintLog
#include "StorageImpl.h" // TVPListDir

#include <SDL.h>

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

//---------------------------------------------------------------------------
// path and pattern helpers
//---------------------------------------------------------------------------
char LowerAscii(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool EndsWithSlash(const std::string &path)
{
	return !path.empty() && path[path.size() - 1] == '/';
}

// True for the top of a volume as the engine names it on POSIX targets:
// "/" and "<name>:/" (the Switch's "sdmc:/", "save:/", ...).  The trailing
// slash belongs to the volume name and must survive normalisation, or the
// path would turn into "sdmc:" and stop being a directory.
bool IsVolumeRoot(const std::string &path)
{
	if (path == "/") return true;
	return path.size() >= 3 && path[path.size() - 1] == '/' &&
		path.find(':') == path.size() - 2;
}

// "/a/b/" -> "/a/b", but "sdmc:/" stays "sdmc:/" and "/" stays "/".
std::string StripTrailingSlash(const std::string &path)
{
	std::string out(path);
	while (out.size() > 1 && out[out.size() - 1] == '/' && !IsVolumeRoot(out))
		out.erase(out.size() - 1);
	return out;
}

// A path that is already rooted: "/x" or "<volume>:/x".
bool IsAbsolutePath(const std::string &path)
{
	if (path.empty()) return false;
	if (path[0] == '/') return true;
	const size_t colon = path.find(':');
	if (colon == std::string::npos) return false;
	const size_t slash = path.find('/');
	return slash == std::string::npos || colon < slash;
}

bool IsDirectory(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string JoinPath(const std::string &dir, const std::string &name)
{
	if (dir.empty()) return name;
	if (EndsWithSlash(dir)) return dir + name;
	return dir + "/" + name;
}

std::string ParentPath(const std::string &dir)
{
	const std::string d = StripTrailingSlash(dir);
	if (d.empty()) return "/";
	if (IsVolumeRoot(d)) return d; // already at the top of this volume
	const size_t pos = d.find_last_of('/');
	if (pos == std::string::npos) return d;
	if (pos == 0) return "/";
	const std::string parent = d.substr(0, pos);
	if (IsVolumeRoot(parent + "/")) return parent + "/";
	return parent;
}

// Wildcard match, ASCII case-insensitive, '*' and '?' (the Win32 file
// selector's DOS wildcards; everything else is a literal).
bool GlobMatchFold(const std::string &name, const std::string &pattern)
{
	size_t n = 0, p = 0;
	size_t starPattern = std::string::npos, starName = std::string::npos;
	while (n < name.size()) {
		if (p < pattern.size() && (pattern[p] == '?' ||
			LowerAscii(pattern[p]) == LowerAscii(name[n]))) {
			++n;
			++p;
		} else if (p < pattern.size() && pattern[p] == '*') {
			starPattern = p++;
			starName = n;
		} else if (starPattern != std::string::npos) {
			p = starPattern + 1;
			n = ++starName;
		} else {
			return false;
		}
	}
	while (p < pattern.size() && pattern[p] == '*') ++p;
	return p == pattern.size();
}

bool HasWildcard(const std::string &s)
{
	return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

bool ContainsFold(const std::string &haystack, const std::string &needle)
{
	if (needle.empty()) return true;
	if (needle.size() > haystack.size()) return false;
	for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
		size_t j = 0;
		while (j < needle.size() &&
			LowerAscii(haystack[i + j]) == LowerAscii(needle[j]))
			++j;
		if (j == needle.size()) return true;
	}
	return false;
}

// The filter language of the `filename` argument: ';' and whitespace separate
// alternatives, an alternative with wildcards is a DOS glob and one without is
// a case-insensitive substring (so "data.xp3" selects that file, and a typed
// prefix jumps to the first match).  An empty filter matches everything.
bool FilterMatches(const std::string &name, const std::string &filter)
{
	if (filter.empty()) return true;
	size_t i = 0;
	bool any = false;
	while (i <= filter.size()) {
		size_t end = i;
		while (end < filter.size() && filter[end] != ';' &&
			filter[end] != ' ' && filter[end] != '\t')
			++end;
		const std::string part = filter.substr(i, end - i);
		i = end + 1;
		if (part.empty()) continue;
		any = true;
		if (HasWildcard(part)) {
			if (GlobMatchFold(name, part)) return true;
		} else if (ContainsFold(name, part)) {
			return true;
		}
	}
	// a filter of only separators behaves like no filter at all
	return !any;
}

std::string ElideLeft(const std::string &text, int maxWidth)
{
	if (maxWidth <= 0) return std::string();
	if (krkr2sdl::MeasureGlyphRun(text) <= maxWidth) return text;
	const std::string ellipsis = "\xE2\x80\xA6"; // U+2026
	const int ellipsisWidth = krkr2sdl::MeasureGlyphRun(ellipsis);
	size_t i = text.size();
	int width = 0;
	while (i > 0) {
		const size_t prev = krkr2sdl::UTF8Prev(text, i);
		const int cw = krkr2sdl::MeasureGlyphRun(text.substr(prev, i - prev));
		if (width + cw + ellipsisWidth > maxWidth) break;
		width += cw;
		i = prev;
	}
	return ellipsis + text.substr(i);
}

std::string FormatSize(tjs_uint64 size)
{
	char buffer[48];
	if (size < 1024) {
		snprintf(buffer, sizeof(buffer), "%llu B", (unsigned long long)size);
	} else if (size < 1024 * 1024) {
		snprintf(buffer, sizeof(buffer), "%.1f kB", size / 1024.0);
	} else if (size < 1024ull * 1024 * 1024) {
		snprintf(buffer, sizeof(buffer), "%.1f MB", size / (1024.0 * 1024.0));
	} else {
		snprintf(buffer, sizeof(buffer), "%.2f GB",
			size / (1024.0 * 1024.0 * 1024.0));
	}
	return buffer;
}

struct tListEntry {
	std::string Name;
	bool Dir = false;
	tjs_uint64 Size = 0;
	// The ".." row this dialog puts above everything else; activating it goes to
	// the parent directory (see Reload()/GoParent()).
	bool Parent = false;
};

//---------------------------------------------------------------------------
// The dialog
//---------------------------------------------------------------------------
class tTVPSDLFileSelector
{
public:
	tTVPSDLFileSelector(SDL_Window *window, SDL_Renderer *renderer)
		: Window(window), Renderer(renderer) {}

	std::string Run(const std::string &title, const std::string &filename,
		std::string initdir, bool issave, const std::string &preselect = std::string())
	{
		// Each dialog of a run starts from the first scripted step, so a run
		// that opens several dialogs scripts each of them independently.
		krkr2sdl::ResetScriptedDialogInput();

		SaveMode = issave;
		Title = title.empty()
			? (SaveMode ? std::string("Save file") : std::string("Open file"))
			: title;
		// Start with the cursor on this entry (the launcher uses it to leave the
		// cursor on something it just rejected, so one step picks the next one).
		Preselect = preselect;

		SDL_GetWindowSize(Window, &WindowWidth, &WindowHeight);
		FontSize = std::max(14, std::min(28, WindowHeight / 36));
		if (!krkr2sdl::PrepareDialogFont(FontSize)) {
			TVPPrintLog("TVPShowFileSelector: no usable font; cannot show the "
				"file selector (reporting a cancel)");
			return std::string();
		}

		Roots = TVPGetDriverPath();
		if (Roots.empty()) Roots.push_back("/");
		for (size_t i = 0; i < Roots.size(); ++i)
			Roots[i] = StripTrailingSlash(Roots[i]);

		// The engine's caller hands the name and the directory over separately;
		// a name that still carries a path (KAG does this) is split here.
		std::string field = StripTrailingSlash(filename);
		{
			const size_t slash = field.find_last_of('/');
			if (slash != std::string::npos) {
				if (initdir.empty()) initdir = field.substr(0, slash);
				field = field.substr(slash + 1);
			}
		}
		Field.Reset(field);
		FieldScroll = 0;

		Dir = StripTrailingSlash(initdir);
		if (!Dir.empty() && !IsAbsolutePath(Dir)) {
			// A relative directory is relative to the game's own directory,
			// the way the storage layer resolves a relative name.
			std::string base = StripTrailingSlash(TVPGetAppPath().AsStdString());
			while (Dir.size() > 1 && Dir[0] == '.' && Dir[1] == '/')
				Dir.erase(0, 2);
			Dir = JoinPath(base, Dir);
		}
		if (!Dir.empty() && !IsDirectory(Dir)) {
			const std::string parent = ParentPath(Dir);
			Status = "cannot open " + Dir;
			Dir = IsDirectory(parent) ? parent : std::string();
		}
		if (Dir.empty()) {
			Dir = Roots[0];
			RootIndex = 0;
		} else {
			RootIndex = RootForPath(Dir);
		}
		Reload();

		TraceLog.enabled = TraceEnabled();
		if (TraceLog.enabled) {
			char buffer[64];
			snprintf(buffer, sizeof(buffer), " save=%d roots=%d", SaveMode ? 1 : 0,
				(int)Roots.size());
			TVPPrintLog(("filesel: call title=\"" + Title + "\" name=\"" + filename +
				"\" initdir=\"" + initdir + "\"" + buffer).c_str());
		}

		Layout();
		// The list has the focus unless there is a name to edit; SDL's text
		// input stays acquired either way so that a typed character can move
		// the focus into the field (type to jump).
		if (SaveMode) FocusFieldNow();
		else FocusList();
		krkr2sdl::TextInputAcquire(FieldTextRect);
		TraceState(true);
		Draw();

		while (!Done) {
			// KRKR2_DIALOG_KEYS, when set, feeds the next scripted event into
			// SDL's queue so the dispatch below sees it like a real device.
			krkr2sdl::PumpScriptedDialogInput();
			SDL_Event ev;
			if (SDL_WaitEventTimeout(&ev, BlinkMs) != 0)
				HandleEvent(ev);
			if (Done) break;
			if (SDL_GetTicks() - LastBlink >= BlinkMs) {
				CaretVisible = !CaretVisible;
				LastBlink = SDL_GetTicks();
				Dirty = true;
			}
			if (Dirty) {
				Draw();
				Dirty = false;
				TraceState(false);
			}
		}

		// Every exit path releases the text input, whether the dialog was
		// accepted, cancelled or closed with its window.
		krkr2sdl::TextInputRelease();
		if (TraceLog.enabled) {
			TVPPrintLog(SDL_IsTextInputActive()
				? "filesel: WARNING text input still active after the dialog"
				: "filesel: text input released");
		}
		return Result;
	}

private:
	// ---- state ----
	SDL_Window *Window;
	SDL_Renderer *Renderer;
	std::string Title, Dir, Status, Result;
	bool SaveMode = false;
	// Entry to select after the next Reload(), used by GoParent() (it is cleared
	// as soon as Reload() has looked at it).
	std::string Preselect;
	std::vector<std::string> Roots;
	size_t RootIndex = 0;
	std::vector<tListEntry> Entries;
	int Cursor = 0;
	int Scroll = 0;
	krkr2sdl::TextEdit Field;
	int FieldScroll = 0;
	bool FieldFocused = false;
	// Set when a key acted as navigation although SDL will also deliver it as
	// text (W/S, SPACE): the matching SDL_TEXTINPUT is then ignored.
	bool SwallowNextText = false;
	size_t HoverButton = (size_t)-1;
	bool HoverRoot = false;
	int HoverRow = -1;
	bool CaretVisible = true;
	bool Dirty = true;
	bool Done = false;
	Uint32 LastBlink = 0;
	Sint16 AxisLatch[4] = { 0, 0, 0, 0 };

	// ---- layout ----
	SDL_Rect Panel = { 0, 0, 0, 0 };
	SDL_Rect ListArea = { 0, 0, 0, 0 };
	SDL_Rect FieldRect = { 0, 0, 0, 0 };     // the field, including its label
	SDL_Rect FieldTextRect = { 0, 0, 0, 0 }; // the editable part of the field
	SDL_Rect RootRect = { 0, 0, 0, 0 };
	std::vector<SDL_Rect> ButtonRects;
	std::vector<std::string> ButtonLabels;
	int WindowWidth = 0, WindowHeight = 0;
	int FontSize = 18;
	int Padding = 14;
	int FieldPadding = 8;
	int TitleY = 0, PathY = 0, HintY = 0;
	int RowHeight = 0;
	int VisibleRows = 1;

	static const Uint32 BlinkMs = 500;
	static const size_t NoButton = (size_t)-1;

	// ---- tracing (KRKR2_FILESEL_TRACE) ----
	struct tTrace {
		bool enabled = false;
		std::string last;
	};
	tTrace TraceLog;

	static bool TraceEnabled()
	{
		const char *env = getenv("KRKR2_FILESEL_TRACE");
		return env && *env;
	}

	std::string StateLine() const
	{
		char buffer[64];
		std::string line = SaveMode ? "filesel: save" : "filesel: open";
		line += " dir=\"" + Dir + "\"";
		snprintf(buffer, sizeof(buffer), " entries=%d cursor=%d scroll=%d",
			(int)Entries.size(), Cursor, Scroll);
		line += buffer;
		snprintf(buffer, sizeof(buffer), " root=%d/%d", (int)RootIndex + 1,
			(int)Roots.size());
		line += buffer;
		line += " field=\"" + Field.Text + "\"";
		line += FieldFocused ? " focus=field" : " focus=list";
		if (!Entries.empty() && Cursor >= 0 && Cursor < (int)Entries.size())
			line += " selected=\"" + Entries[Cursor].Name + "\"";
		if (!Status.empty()) line += " status=\"" + Status + "\"";
		return line;
	}

	void TraceState(bool force)
	{
		if (!TraceLog.enabled) return;
		const std::string line = StateLine();
		if (!force && line == TraceLog.last) return;
		TraceLog.last = line;
		TVPPrintLog(line.c_str());
	}

	// ---- directory handling ----
	size_t RootForPath(const std::string &path) const
	{
		size_t best = 0;
		size_t bestLen = 0;
		for (size_t i = 0; i < Roots.size(); ++i) {
			const std::string &root = Roots[i];
			if (path.compare(0, root.size(), root) != 0) continue;
			if (root.size() > bestLen) {
				bestLen = root.size();
				best = i;
			}
		}
		return best;
	}

	std::string ListingFilter() const
	{
		// Open mode: the field is the filter.  Save mode: the field is the
		// file name, and the whole directory stays browsable.
		return SaveMode ? std::string() : Field.Text;
	}

	void Reload()
	{
		const std::string selected = !Preselect.empty()
			? Preselect
			: ((Cursor >= 0 && Cursor < (int)Entries.size())
				? Entries[Cursor].Name : std::string());
		Preselect.clear();
		const std::string filter = ListingFilter();
		Entries.clear();
		TVPListDir(Dir, [&](const std::string &name, int mask) {
			if (name == "." || name == "..") return;
			tListEntry entry;
			entry.Name = name;
			entry.Dir = (mask & S_IFDIR) != 0;
			if (!entry.Dir) {
				if (!FilterMatches(name, filter)) return;
				struct stat st;
				if (stat(JoinPath(Dir, name).c_str(), &st) == 0)
					entry.Size = (tjs_uint64)st.st_size;
			}
			Entries.push_back(entry);
		});

		// ".." goes above everything, when there is somewhere to go: it is what
		// makes going up discoverable, and on a game pad (a handheld has no
		// keyboard) it is the only visible way to do it.  At the top of a volume
		// there is no parent to show - BACKSPACE/LEFT/B and the roots list are
		// what a user has there.
		const std::string here = StripTrailingSlash(Dir);
		const bool has_parent = ParentPath(here) != here;
		if (has_parent) {
			tListEntry up;
			up.Name = "..";
			up.Dir = true;
			up.Parent = true;
			Entries.push_back(up);
		}

		std::sort(Entries.begin(), Entries.end(),
			[](const tListEntry &a, const tListEntry &b) {
				if (a.Parent != b.Parent) return a.Parent;
				if (a.Dir != b.Dir) return a.Dir;
				const size_t n = std::min(a.Name.size(), b.Name.size());
				for (size_t i = 0; i < n; ++i) {
					const char ca = LowerAscii(a.Name[i]);
					const char cb = LowerAscii(b.Name[i]);
					if (ca != cb) return ca < cb;
				}
				return a.Name.size() < b.Name.size();
			});

		// keep the previously selected entry selected when it is still there
		Cursor = 0;
		if (!selected.empty()) {
			for (size_t i = 0; i < Entries.size(); ++i) {
				if (Entries[i].Name == selected) {
					Cursor = (int)i;
					break;
				}
			}
		} else {
			// Start on the first real entry, not on "..": ENTER would otherwise
			// leave the directory instead of opening something, and one extra
			// cursor step is cheaper than a surprise.
			for (size_t i = 0; i < Entries.size(); ++i) {
				if (!Entries[i].Parent) {
					Cursor = (int)i;
					break;
				}
			}
		}
		Scroll = 0;
		KeepCursorVisible();
		Dirty = true;
	}

	bool ChangeDir(const std::string &path)
	{
		const std::string dir = StripTrailingSlash(path);
		if (!IsDirectory(dir)) {
			Status = "cannot open " + dir;
			Dirty = true;
			return false;
		}
		Dir = dir;
		Status.clear();
		RootIndex = RootForPath(Dir);
		Reload();
		return true;
	}

	void NextRoot()
	{
		if (Roots.empty()) return;
		RootIndex = (RootIndex + 1) % Roots.size();
		ChangeDir(Roots[RootIndex]);
	}

	// ---- cursor ----
	void KeepCursorVisible()
	{
		if (Entries.empty()) {
			Cursor = 0;
			Scroll = 0;
			return;
		}
		if (Cursor < 0) Cursor = 0;
		if (Cursor >= (int)Entries.size()) Cursor = (int)Entries.size() - 1;
		if (Cursor < Scroll) Scroll = Cursor;
		if (Cursor >= Scroll + VisibleRows) Scroll = Cursor - VisibleRows + 1;
		if (Scroll > (int)Entries.size() - VisibleRows)
			Scroll = std::max(0, (int)Entries.size() - VisibleRows);
		if (Scroll < 0) Scroll = 0;
	}

	void MoveCursor(int delta)
	{
		if (Entries.empty()) return;
		Cursor += delta;
		if (Cursor < 0) Cursor = 0;
		if (Cursor >= (int)Entries.size()) Cursor = (int)Entries.size() - 1;
		KeepCursorVisible();
		Dirty = true;
	}

	void MoveCursorTo(int index)
	{
		if (Entries.empty()) return;
		Cursor = index;
		KeepCursorVisible();
		Dirty = true;
	}

	void ScrollBy(int rows)
	{
		if (Entries.empty()) return;
		Scroll += rows;
		if (Scroll > (int)Entries.size() - VisibleRows)
			Scroll = std::max(0, (int)Entries.size() - VisibleRows);
		if (Scroll < 0) Scroll = 0;
		if (Cursor < Scroll) Cursor = Scroll;
		if (Cursor >= Scroll + VisibleRows) Cursor = Scroll + VisibleRows - 1;
		KeepCursorVisible();
		Dirty = true;
	}

	// ---- focus and editing ----
	void ResetFieldBlink()
	{
		CaretVisible = true;
		LastBlink = SDL_GetTicks();
	}

	void FocusFieldNow()
	{
		FieldFocused = true;
		Field.KeepCaretVisible(FieldTextRect.w, FieldPadding, FieldScroll);
		krkr2sdl::TextInputAcquire(FieldTextRect);
		ResetFieldBlink();
		Dirty = true;
	}

	void FocusList()
	{
		FieldFocused = false;
		ResetFieldBlink();
		Dirty = true;
	}

	void OnFieldChanged()
	{
		if (!SaveMode) {
			// Open mode: the field is the filter, so the listing follows it.
			Cursor = 0;
			Scroll = 0;
			Reload();
			// Type-to-jump lands on what was typed: the listing always starts
			// with the directories (they are never filtered out), so the
			// cursor goes to the first file match instead of the first entry.
			for (size_t i = 0; i < Entries.size(); ++i) {
				if (!Entries[i].Dir) {
					Cursor = (int)i;
					break;
				}
			}
			KeepCursorVisible();
		}
		Field.KeepCaretVisible(FieldTextRect.w, FieldPadding, FieldScroll);
		ResetFieldBlink();
		Dirty = true;
	}

	// ---- actions ----
	void EnterDirectory(const std::string &name)
	{
		FocusList();
		ChangeDir(JoinPath(Dir, name));
	}

	void Activate()
	{
		if (Entries.empty()) return;
		const tListEntry &entry = Entries[Cursor];
		if (entry.Parent) {
			GoParent();
			return;
		}
		if (entry.Dir) {
			EnterDirectory(entry.Name);
			return;
		}
		if (SaveMode) {
			// Activating a file fills the name line in, as a save dialog does;
			// ENTER then accepts it.
			Field.Reset(entry.Name);
			FieldScroll = 0;
			FocusFieldNow();
			return;
		}
		Result = JoinPath(Dir, entry.Name);
		Done = true;
	}

	void Accept()
	{
		if (!SaveMode) {
			Activate();
			return;
		}
		std::string name = Field.Text;
		// A leading "/" would turn the save into an absolute path; the name is
		// relative to the browsed directory.
		while (!name.empty() && name[0] == '/') name.erase(0, 1);
		if (name.empty()) {
			Status = "enter a file name";
			FocusFieldNow();
			return;
		}
		Result = JoinPath(Dir, name);
		Done = true;
	}

	void Cancel()
	{
		Result.clear();
		Done = true;
	}

	void ChooseFolder()
	{
		if (SaveMode) return; // a save target is a file name, not a folder
		Result = Dir;
		Done = true;
	}

	void GoParent()
	{
		const std::string current = StripTrailingSlash(Dir);
		const std::string parent = ParentPath(current);
		if (parent == current) {
			// already at the top of this volume: B/ESC backs out
			Cancel();
			return;
		}
		// Going up leaves the cursor on the directory that was just left, so a
		// round trip (up, then back in) is two keystrokes instead of a search.
		const size_t slash = current.find_last_of('/');
		Preselect = (slash == std::string::npos) ? current : current.substr(slash + 1);
		ChangeDir(parent);
	}

	// ---- layout ----
	void Layout()
	{
		SDL_GetWindowSize(Window, &WindowWidth, &WindowHeight);
		const int lineHeight = krkr2sdl::DialogLineHeight();
		Padding = 14;
		FieldPadding = 8;

		const int margin = 16;
		Panel.x = margin;
		Panel.y = margin;
		Panel.w = std::max(320, WindowWidth - 2 * margin);
		Panel.h = std::max(240, WindowHeight - 2 * margin);
		if (Panel.x + Panel.w > WindowWidth) Panel.w = WindowWidth - Panel.x;
		if (Panel.y + Panel.h > WindowHeight) Panel.h = WindowHeight - Panel.y;

		const int innerX = Panel.x + Padding;
		const int innerW = Panel.w - 2 * Padding;
		int y = Panel.y + Padding;
		TitleY = y;
		y += lineHeight + 8;
		PathY = y;
		y += lineHeight + 10;

		FieldRect.x = innerX;
		FieldRect.y = y;
		FieldRect.w = innerW;
		FieldRect.h = lineHeight + 2 * FieldPadding;
		{
			const int labelWidth = krkr2sdl::MeasureGlyphRun(SaveMode ? "Name:" : "Filter:");
			FieldTextRect = FieldRect;
			FieldTextRect.x += labelWidth + 8;
			FieldTextRect.w -= labelWidth + 8;
			if (FieldTextRect.w < 24) FieldTextRect.w = 24;
		}
		y += FieldRect.h + 12;

		const int buttonH = lineHeight + 2 * FieldPadding;
		const int footerY = Panel.y + Panel.h - Padding - buttonH;
		HintY = footerY - lineHeight - 6;

		ListArea.x = innerX;
		ListArea.y = y;
		ListArea.w = innerW;
		ListArea.h = std::max(lineHeight, HintY - 8 - y);

		RowHeight = lineHeight + 6;
		VisibleRows = std::max(1, ListArea.h / RowHeight);

		ButtonLabels.clear();
		if (SaveMode) {
			ButtonLabels.emplace_back("Save");
			ButtonLabels.emplace_back("Cancel");
		} else {
			ButtonLabels.emplace_back("Select folder");
			ButtonLabels.emplace_back("Cancel");
		}
		const int gap = 8;
		const int buttonPadding = 16;
		ButtonRects.resize(ButtonLabels.size());
		std::vector<int> widths(ButtonLabels.size(), 0);
		int total = 0;
		for (size_t i = 0; i < ButtonLabels.size(); ++i) {
			widths[i] = std::max(80,
				krkr2sdl::MeasureGlyphRun(ButtonLabels[i]) + 2 * buttonPadding);
			total += widths[i];
		}
		total += gap * ((int)ButtonLabels.size() - 1);
		int x = Panel.x + Panel.w - Padding - total;
		if (x < Panel.x + Padding) x = Panel.x + Padding;
		for (size_t i = 0; i < ButtonRects.size(); ++i) {
			ButtonRects[i] = { x, footerY, widths[i], buttonH };
			x += widths[i] + gap;
		}

		// the root label sits at the right end of the title line
		const std::string rootLabel = RootLabel();
		const int rootWidth = krkr2sdl::MeasureGlyphRun(rootLabel) + 16;
		RootRect = { Panel.x + Panel.w - Padding - rootWidth, TitleY - 4,
			rootWidth, lineHeight + 8 };

		if (TraceLog.enabled) {
			char buffer[160];
			snprintf(buffer, sizeof(buffer),
				"filesel: layout list=[%d,%d,%d,%d] row=%d rows=%d field=[%d,%d,%d,%d]",
				ListArea.x, ListArea.y, ListArea.w, ListArea.h, RowHeight,
				VisibleRows, FieldTextRect.x, FieldTextRect.y, FieldTextRect.w,
				FieldTextRect.h);
			TVPPrintLog(buffer);
		}
		KeepCursorVisible();
	}

	std::string RootLabel() const
	{
		char buffer[64];
		snprintf(buffer, sizeof(buffer), "[%d/%d] ", (int)RootIndex + 1,
			(int)Roots.size());
		return std::string(buffer) + Roots[RootIndex];
	}

	// ---- events ----
	size_t ButtonAt(int x, int y) const
	{
		for (size_t i = 0; i < ButtonRects.size(); ++i) {
			const SDL_Rect &r = ButtonRects[i];
			if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
				return i;
		}
		return NoButton;
	}

	bool InRect(const SDL_Rect &r, int x, int y) const
	{
		return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
	}

	int RowAt(int y) const
	{
		if (y < ListArea.y || y >= ListArea.y + ListArea.h) return -1;
		const int row = Scroll + (y - ListArea.y) / RowHeight;
		if (row < 0 || row >= (int)Entries.size()) return -1;
		return row;
	}

	void OnKeyDown(const SDL_KeyboardEvent &key)
	{
		// Any key ends the "swallow the text of the previous key" window (see
		// the W/S and SPACE cases below).
		SwallowNextText = false;

		// Editing keys belong to the field while it has the focus; everything
		// the editor does not consume (arrows, TAB, F1/F2) still navigates.
		if (FieldFocused) {
			switch (Field.OnKeyDown(key)) {
			case krkr2sdl::TextEdit::KeyAccept:
				Accept();
				return;
			case krkr2sdl::TextEdit::KeyCancel:
				if (!SaveMode && !Field.Text.empty()) {
					// open mode: ESC clears the filter (and the listing), a
					// second ESC cancels the dialog
					Field.Reset(std::string());
					OnFieldChanged();
					return;
				}
				Cancel();
				return;
			case krkr2sdl::TextEdit::KeyHandled:
				OnFieldChanged();
				return;
			default:
				break;
			}
		}

		switch (key.keysym.sym) {
		case SDLK_UP:
			MoveCursor(-1);
			return;
		case SDLK_DOWN:
			MoveCursor(1);
			return;
		// W/S navigate the list; while the field has the focus they are text
		// and never reach this code (the editor consumes them as characters,
		// and it returns KeyUnhandled only for keys it does not use).
		case SDLK_w:
		case SDLK_s:
			if (FieldFocused) return;
			// SDL sends SDL_TEXTINPUT for this same keystroke right after the
			// KEYDOWN; drop it so navigating does not also start a filter.
			SwallowNextText = true;
			MoveCursor(key.keysym.sym == SDLK_w ? -1 : 1);
			return;
		case SDLK_PAGEUP:
			MoveCursor(-VisibleRows);
			return;
		case SDLK_PAGEDOWN:
			MoveCursor(VisibleRows);
			return;
		case SDLK_HOME:
			MoveCursorTo(0);
			return;
		case SDLK_END:
			MoveCursorTo((int)Entries.size() - 1);
			return;
		case SDLK_RETURN:
		case SDLK_KP_ENTER:
			Accept();
			return;
		case SDLK_SPACE:
			// A space is text in the field and the accept key in the list.
			if (FieldFocused) return;
			SwallowNextText = true;
			Accept();
			return;
		case SDLK_ESCAPE:
			Cancel();
			return;
		case SDLK_BACKSPACE:
		case SDLK_LEFT:
			GoParent();
			return;
		case SDLK_RIGHT:
			if (!Entries.empty() && Entries[Cursor].Dir)
				EnterDirectory(Entries[Cursor].Name);
			return;
		case SDLK_TAB:
			if (FieldFocused) FocusList();
			else FocusFieldNow();
			return;
		case SDLK_F1:
			NextRoot();
			return;
		case SDLK_F2:
			ChooseFolder();
			return;
		default:
			return;
		}
	}

	void OnTextInput(const SDL_TextInputEvent &text)
	{
		// SDL sends SDL_TEXTINPUT right after the KEYDOWN of the same
		// keystroke; a key that acted as navigation (W/S, SPACE) drops it.
		if (SwallowNextText) {
			SwallowNextText = false;
			return;
		}
		// Type-to-jump: typing while the list is focused moves the focus into
		// the filter/name line and continues there.
		if (!FieldFocused) FocusFieldNow();
		Field.Insert(text.text);
		OnFieldChanged();
	}

	void OnTextEditing(const SDL_TextEditingEvent &edit)
	{
		if (!FieldFocused) FocusFieldNow();
		Field.OnTextEditing(edit);
		ResetFieldBlink();
		Dirty = true;
	}

	void OnMouseButton(const SDL_MouseButtonEvent &button)
	{
		if (button.button != SDL_BUTTON_LEFT || button.state != SDL_PRESSED) return;
		const size_t hit = ButtonAt(button.x, button.y);
		if (hit != NoButton) {
			const bool accept = (hit == 0);
			if (SaveMode) {
				if (accept) Accept();
				else Cancel();
			} else {
				if (accept) ChooseFolder();
				else Cancel();
			}
			return;
		}
		if (InRect(RootRect, button.x, button.y)) {
			NextRoot();
			return;
		}
		if (InRect(FieldRect, button.x, button.y)) {
			FocusFieldNow();
			Field.SetCaretFromX(button.x - FieldTextRect.x + FieldScroll,
				FieldPadding);
			ResetFieldBlink();
			Dirty = true;
			return;
		}
		const int row = RowAt(button.y);
		if (row < 0) return;
		if (row == Cursor || button.clicks >= 2) {
			FocusList();
			MoveCursorTo(row);
			Activate(); // a second click activates, as a double click does
			return;
		}
		FocusList();
		MoveCursorTo(row);
	}

	void OnMouseWheel(const SDL_MouseWheelEvent &wheel)
	{
		if (wheel.y == 0) return;
		ScrollBy(wheel.y > 0 ? -3 : 3);
	}

	void OnMouseMotion(const SDL_MouseMotionEvent &motion)
	{
		const size_t hoverButton = ButtonAt(motion.x, motion.y);
		const bool hoverRoot = InRect(RootRect, motion.x, motion.y);
		const int row = RowAt(motion.y);
		if (hoverButton != HoverButton || hoverRoot != HoverRoot ||
			row != HoverRow) {
			HoverButton = hoverButton;
			HoverRoot = hoverRoot;
			HoverRow = row;
			Dirty = true;
		}
	}

	void OnControllerButton(const SDL_ControllerButtonEvent &button)
	{
		if (button.state != SDL_PRESSED) return;
		switch (button.button) {
		case SDL_CONTROLLER_BUTTON_A: Accept(); break;
		case SDL_CONTROLLER_BUTTON_B: GoParent(); break;
		case SDL_CONTROLLER_BUTTON_X: ChooseFolder(); break;
		case SDL_CONTROLLER_BUTTON_Y: Cancel(); break;
		case SDL_CONTROLLER_BUTTON_START: Accept(); break;
		case SDL_CONTROLLER_BUTTON_BACK: NextRoot(); break;
		case SDL_CONTROLLER_BUTTON_DPAD_UP: MoveCursor(-1); break;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: MoveCursor(1); break;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: GoParent(); break;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
			if (!Entries.empty() && Entries[Cursor].Dir)
				EnterDirectory(Entries[Cursor].Name);
			break;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: MoveCursor(-VisibleRows); break;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: MoveCursor(VisibleRows); break;
		default: break;
		}
	}

	void OnControllerAxis(const SDL_ControllerAxisEvent &axis)
	{
		if (axis.axis > SDL_CONTROLLER_AXIS_RIGHTY) return;
		const Sint16 value = axis.value;
		if (AxisLatch[axis.axis] != 0 && value > -8000 && value < 8000)
			AxisLatch[axis.axis] = 0; // released: arm the next step
		if (AxisLatch[axis.axis] != 0) return; // held: one step per push
		const bool vertical = axis.axis == SDL_CONTROLLER_AXIS_LEFTY ||
			axis.axis == SDL_CONTROLLER_AXIS_RIGHTY;
		if (value > 20000) {
			AxisLatch[axis.axis] = 1;
			if (vertical) MoveCursor(1);
			else if (!Entries.empty() && Entries[Cursor].Dir)
				EnterDirectory(Entries[Cursor].Name);
		} else if (value < -20000) {
			AxisLatch[axis.axis] = -1;
			if (vertical) MoveCursor(-1);
			else GoParent();
		}
	}

	void HandleEvent(const SDL_Event &ev)
	{
		switch (ev.type) {
		case SDL_KEYDOWN:
			OnKeyDown(ev.key);
			break;
		case SDL_TEXTINPUT:
			OnTextInput(ev.text);
			break;
		case SDL_TEXTEDITING:
			OnTextEditing(ev.edit);
			break;
		case SDL_MOUSEBUTTONDOWN:
			OnMouseButton(ev.button);
			break;
		case SDL_MOUSEWHEEL:
			OnMouseWheel(ev.wheel);
			break;
		case SDL_MOUSEMOTION:
			OnMouseMotion(ev.motion);
			break;
		case SDL_CONTROLLERBUTTONDOWN:
			OnControllerButton(ev.cbutton);
			break;
		case SDL_CONTROLLERAXISMOTION:
			OnControllerAxis(ev.caxis);
			break;
		case SDL_WINDOWEVENT:
			switch (ev.window.event) {
			case SDL_WINDOWEVENT_SIZE_CHANGED:
			case SDL_WINDOWEVENT_RESIZED:
				Layout();
				if (FieldFocused) krkr2sdl::TextInputAcquire(FieldTextRect);
				Dirty = true;
				break;
			case SDL_WINDOWEVENT_EXPOSED:
				Dirty = true;
				break;
			case SDL_WINDOWEVENT_CLOSE:
				// The user asked to close the game while the selector is up:
				// cancel it and let the engine's own close query run once it
				// regains control (Host.h).
				krkr2sdl::HostRequestQuit();
				Cancel();
				break;
			default:
				break;
			}
			break;
		case SDL_QUIT:
			krkr2sdl::HostRequestQuit();
			Cancel();
			break;
		default:
			break;
		}
	}

	// ---- drawing ----
	static void DrawFolderIcon(SDL_Renderer *renderer, int x, int y,
		SDL_Color color)
	{
		SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
		SDL_Rect tab = { x, y + 1, 7, 3 };
		SDL_Rect body = { x, y + 3, 15, 10 };
		SDL_RenderFillRect(renderer, &tab);
		SDL_RenderFillRect(renderer, &body);
	}

	static void DrawFileIcon(SDL_Renderer *renderer, int x, int y,
		SDL_Color color)
	{
		SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
		SDL_Rect body = { x + 1, y + 1, 11, 13 };
		SDL_RenderDrawRect(renderer, &body);
		SDL_Rect line = { x + 3, y + 4, 7, 1 };
		SDL_RenderFillRect(renderer, &line);
		SDL_Rect line2 = { x + 3, y + 7, 7, 1 };
		SDL_RenderFillRect(renderer, &line2);
	}

	void DrawField(const SDL_Color &bodyColor, const SDL_Color &dimColor)
	{
		SDL_SetRenderDrawColor(Renderer, 0x14, 0x17, 0x1C, 0xFF);
		SDL_RenderFillRect(Renderer, &FieldRect);
		if (FieldFocused)
			SDL_SetRenderDrawColor(Renderer, 0x5C, 0x8F, 0xD8, 0xFF);
		else
			SDL_SetRenderDrawColor(Renderer, 0x3A, 0x3F, 0x4B, 0xFF);
		SDL_RenderDrawRect(Renderer, &FieldRect);

		const std::string label = SaveMode ? "Name:" : "Filter:";
		krkr2sdl::DrawGlyphRun(Renderer, label, FieldRect.x + FieldPadding,
			FieldRect.y + (FieldRect.h - krkr2sdl::DialogLineHeight()) / 2,
			dimColor);
		Field.Draw(Renderer, FieldTextRect, FieldPadding, FieldScroll,
			FieldFocused && CaretVisible, bodyColor);
	}

	void Draw()
	{
		const int lineHeight = krkr2sdl::DialogLineHeight();
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
		const SDL_Color dimColor = { 0x8A, 0x92, 0xA4, 0xFF };
		const SDL_Color accentColor = { 0x8F, 0xC0, 0xF0, 0xFF };
		const SDL_Color warnColor = { 0xF0, 0xB0, 0x80, 0xFF };
		const SDL_Color dirColor = { 0xA8, 0xD0, 0xF8, 0xFF };

		// title line, with the current root at its right end
		krkr2sdl::DrawGlyphRun(Renderer, Title, Panel.x + Padding, TitleY,
			titleColor);
		{
			const std::string label = RootLabel();
			const int width = krkr2sdl::MeasureGlyphRun(label);
			krkr2sdl::DrawGlyphRun(Renderer, label,
				RootRect.x + (RootRect.w - width) / 2,
				RootRect.y + (RootRect.h - lineHeight) / 2,
				HoverRoot ? accentColor : dimColor);
		}

		// path line (elided from the left so its tail stays readable)
		const int pathWidth = Panel.w - 2 * Padding;
		if (!Status.empty()) {
			krkr2sdl::DrawGlyphRun(Renderer, ElideLeft(Status, pathWidth),
				Panel.x + Padding, PathY, warnColor);
		} else {
			krkr2sdl::DrawGlyphRun(Renderer, ElideLeft(Dir, pathWidth),
				Panel.x + Padding, PathY, bodyColor);
		}

		DrawField(bodyColor, dimColor);

		// listing
		SDL_SetRenderDrawColor(Renderer, 0x1A, 0x1E, 0x25, 0xFF);
		SDL_RenderFillRect(Renderer, &ListArea);
		SDL_SetRenderDrawColor(Renderer, 0x4A, 0x50, 0x5E, 0xFF);
		SDL_RenderDrawRect(Renderer, &ListArea);
		{
			SDL_Rect clip = { ListArea.x + 1, ListArea.y + 1, ListArea.w - 2,
				ListArea.h - 2 };
			SDL_RenderSetClipRect(Renderer, &clip);

			if (Entries.empty()) {
				krkr2sdl::DrawGlyphRun(Renderer, "(no matching entries)",
					ListArea.x + Padding,
					ListArea.y + (ListArea.h - lineHeight) / 2, dimColor);
			}
			const int last = std::min((int)Entries.size(), Scroll + VisibleRows);
			for (int i = Scroll; i < last; ++i) {
				const tListEntry &entry = Entries[i];
				SDL_Rect row = { ListArea.x + 1,
					ListArea.y + 1 + (i - Scroll) * RowHeight, ListArea.w - 2,
					RowHeight };
				const bool selected = (i == Cursor);
				if (selected) {
					if (FieldFocused)
						SDL_SetRenderDrawColor(Renderer, 0x3A, 0x3F, 0x4B, 0xFF);
					else
						SDL_SetRenderDrawColor(Renderer, 0x3D, 0x6E, 0xA5, 0xFF);
					SDL_RenderFillRect(Renderer, &row);
					// a left marker makes the cursor visible even in a
					// screenshot that has no colour contrast
					SDL_SetRenderDrawColor(Renderer, 0xF2, 0xF4, 0xF8, 0xFF);
					SDL_Rect marker = { row.x, row.y + 2, 3, row.h - 4 };
					SDL_RenderFillRect(Renderer, &marker);
				} else if (i == HoverRow) {
					SDL_SetRenderDrawColor(Renderer, 0x26, 0x2B, 0x35, 0xFF);
					SDL_RenderFillRect(Renderer, &row);
				}
				const int iconY = row.y + (RowHeight - 14) / 2;
				if (entry.Dir)
					DrawFolderIcon(Renderer, row.x + 8, iconY,
						selected ? titleColor : dirColor);
				else
					DrawFileIcon(Renderer, row.x + 9, iconY,
						selected ? titleColor : dimColor);

				const int nameX = row.x + 32;
				int sizeWidth = 0;
				if (!entry.Dir) {
					const std::string sizeText = FormatSize(entry.Size);
					sizeWidth = krkr2sdl::MeasureGlyphRun(sizeText) + 18;
					krkr2sdl::DrawGlyphRun(Renderer, sizeText,
						row.x + row.w - sizeWidth + 6,
						row.y + (RowHeight - lineHeight) / 2,
						selected ? bodyColor : dimColor);
				}
				const int nameWidth = row.w - (nameX - row.x) - sizeWidth - 8;
				const std::string name = ElideLeft(entry.Name,
					std::max(24, nameWidth));
				SDL_Color nameColor = bodyColor;
				if (entry.Dir) nameColor = dirColor;
				if (selected) nameColor = titleColor;
				krkr2sdl::DrawGlyphRun(Renderer, name, nameX,
					row.y + (RowHeight - lineHeight) / 2, nameColor);
			}
			SDL_RenderSetClipRect(Renderer, nullptr);
		}

		// hint line
		const std::string hint = SaveMode
			? "TAB field   UP/DOWN move   ENTER save   .. / BACKSPACE parent   F1 root   ESC cancel"
			: "TAB field   UP/DOWN move   ENTER open   .. / BACKSPACE parent   F1 root   F2 folder   ESC cancel";
		krkr2sdl::DrawGlyphRun(Renderer, ElideLeft(hint, Panel.w - 2 * Padding),
			Panel.x + Padding, HintY, dimColor);

		// buttons
		for (size_t i = 0; i < ButtonRects.size(); ++i) {
			const SDL_Rect &r = ButtonRects[i];
			if (i == HoverButton)
				SDL_SetRenderDrawColor(Renderer, 0x3D, 0x6E, 0xA5, 0xFF);
			else
				SDL_SetRenderDrawColor(Renderer, 0x3A, 0x3F, 0x4B, 0xFF);
			SDL_RenderFillRect(Renderer, &r);
			SDL_SetRenderDrawColor(Renderer, 0x9A, 0xA4, 0xB8, 0xFF);
			SDL_RenderDrawRect(Renderer, &r);
			const int width = krkr2sdl::MeasureGlyphRun(ButtonLabels[i]);
			krkr2sdl::DrawGlyphRun(Renderer, ButtonLabels[i],
				r.x + (r.w - width) / 2, r.y + (r.h - lineHeight) / 2,
				titleColor);
		}

		SDL_RenderPresent(Renderer);
	}
};

} // anonymous namespace

//---------------------------------------------------------------------------
// TVPShowFileSelector
//
// The engine's platform contract, declared by base/win32/FileSelector.cpp
// (whose TVPSelectFile() builds these arguments from the script's parameter
// object and writes the result back into its `name` member).
//---------------------------------------------------------------------------
std::string TVPShowFileSelector(
	const std::string &title,
	const std::string &filename,
	std::string initdir,
	bool issave)
{
	// The engine's entry point (base/win32/FileSelector.cpp declares and calls
	// this); like the Win32 one it has no initial selection.
	return TVPShowFileSelectorEx(title, filename, initdir, issave, std::string());
}

std::string TVPShowFileSelectorEx(
	const std::string &title,
	const std::string &filename,
	std::string initdir,
	bool issave,
	const std::string &preselect)
{
	SDL_Window *window = krkr2sdl::HostWindow();
	SDL_Renderer *renderer = krkr2sdl::HostRenderer();
	if (window == nullptr || renderer == nullptr) {
		// Before HostInit() (or after HostShutdown()) there is no surface to
		// draw on.  Report a cancel, which the engine's callers take as
		// "nothing selected".
		TVPPrintLog("TVPShowFileSelector: the SDL host has no window/renderer");
		return std::string();
	}
	tTVPSDLFileSelector selector(window, renderer);
	return selector.Run(title, filename, initdir, issave, preselect);
}
