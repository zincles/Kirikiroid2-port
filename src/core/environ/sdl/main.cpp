/*
	Kirikiroid2 host entry point for the SDL2 port (Linux, Nintendo Switch).

	On Android this role was split between the Java activity, SDL_android_main.cpp
	and TVPAppDelegate (environ/cocos2d/AppDelegate.cpp): the activity owned the
	window and forwarded input, and the cocos2d Director drove the frame loop,
	calling TVPMainScene::update() - which is where the engine's own pump
	(Application->Run()) and TVPDrawSceneOnce() were called from.

	Here the same sequence is a plain SDL2 loop:

		boot:   HostInit -> Application->StartApplication(game path)
		frame:  SDL events -> Application->Run() -> post-update hook -> present

	The game to run is argv[1] (an .xp3 archive or a directory containing
	startup.tjs); remaining arguments are handed to the engine as options, the
	same way TVPCheckStartupArg() did on Windows.
*/

#include "tjsCommHead.h"

#include "Host.h"
#include "WindowLayer.h"
#include "WaveMixer.h" // TVPInitDirectSound

#include "Application.h"
#include "DebugIntf.h"
#include "EventIntf.h"
#include "StorageImpl.h"
#include "Exception.h" // TJS::eTJSError
#include "SysInitIntf.h"
#include "SysInitImpl.h" // TVPTerminateCode
#include "TickCount.h"
#include "ConfigManager/GlobalConfigManager.h"
#include "ConfigManager/IndividualConfigManager.h"

#include <SDL.h>

#include <cerrno>
#include <unistd.h>
#include <climits>
#include <cstdlib>

#include <algorithm>
#include <vector>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// Defined in base/win32/StorageImpl.cpp; returns non-zero for a supported
// archive (1 = XP3), the same check the Windows entry path uses.
int TVPCheckArchive(const ttstr &localname);

extern std::thread::id TVPMainThreadID;
extern "C" void TVPSetPostUpdateEvent(void (*f)());

namespace {

struct Options {
	std::string game_path;
	std::string data_dir;
	int width = 1280;
	int height = 720;
	bool fullscreen = false;
	bool vsync = true;
	bool has_game_path = false;
};

void PrintUsage(const char *argv0)
{
	fprintf(stderr,
		"Kirikiroid2 (SDL2 port)\n"
		"usage: %s <game.xp3 | game-directory> [options] [engine options]\n"
		"\n"
		"options:\n"
		"  --data-dir=<dir>   writable directory for savedata/preferences\n"
		"  --width=<px>       window width  (default 1280)\n"
		"  --height=<px>      window height (default 720)\n"
		"  --fullscreen       start in full screen\n"
		"  --no-vsync         disable vertical sync\n"
		"  --help             this text\n"
		"\n"
		"anything else (e.g. -timerprec=0.5) is passed to the engine as a\n"
		"command line option, as TVPCheckStartupArg() does on Windows.\n",
		argv0);
}

bool ParseOptions(int argc, char **argv, Options &opt)
{
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--help" || arg == "-h") {
			PrintUsage(argv[0]);
			return false;
		}
		if (arg == "--fullscreen") { opt.fullscreen = true; continue; }
		if (arg == "--no-vsync") { opt.vsync = false; continue; }

		auto value_of = [&arg](const char *name) -> const char * {
			size_t len = strlen(name);
			if (arg.compare(0, len, name) == 0) return arg.c_str() + len;
			return nullptr;
		};
		if (const char *v = value_of("--width=")) { opt.width = atoi(v); continue; }
		if (const char *v = value_of("--height=")) { opt.height = atoi(v); continue; }
		if (const char *v = value_of("--data-dir=")) { opt.data_dir = v; continue; }

		if (!opt.has_game_path && arg[0] != '-') {
			opt.game_path = arg;
			opt.has_game_path = true;
			continue;
		}

		// Engine options: "name=value" or a bare "name" (Win32 behaviour).
		size_t pos = arg.find('=');
		if (pos == std::string::npos) {
			TVPSetCommandLine(ttstr(arg.c_str()).c_str(), ttstr(TJS_W("yes")));
		} else {
			TVPSetCommandLine(ttstr(arg.substr(0, pos).c_str()).c_str(),
				ttstr(arg.substr(pos + 1).c_str()));
		}
	}
	return true;
}

// A directory is bootable when it carries startup.tjs (same rule as the Win32
// TVPCheckStartupArg).
bool IsBootableGameDir(const std::string &path)
{
	bool bootable = false;
	TVPListDir(path, [&bootable](const std::string &name, int mask) {
		if (!(mask & S_IFREG)) return;
		std::string lower(name);
		std::transform(lower.begin(), lower.end(), lower.begin(),
			[](unsigned char c) { return (char)tolower(c); });
		if (lower == "startup.tjs") bootable = true;
	});
	return bootable;
}

// Directory holding the running executable. On the Switch this is how a game
// placed next to the .nro is found (there is no command line there and the
// working directory is the mount root), on Linux it is what /proc/self/exe
// reports.
std::string ExecutableDirectory(const char *argv0)
{
	std::string exe;
#ifdef __SWITCH__
	// libnx passes the .nro path as argv[0]
	if (argv0 && *argv0) exe = argv0;
#else
	char resolved[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
	if (n > 0) {
		resolved[n] = 0;
		exe = resolved;
	}
#endif
	const size_t slash = exe.find_last_of('/');
	return slash == std::string::npos ? std::string() : exe.substr(0, slash);
}

bool HasXp3Extension(const std::string &name)
{
	if (name.size() <= 4) return false;
	std::string tail = name.substr(name.size() - 4);
	for (char &c : tail) c = (char)tolower((unsigned char)c);
	return tail == ".xp3";
}

// Without an explicit argument, find a game sitting next to the executable or in
// the working directory - the launcher UI (MainFileSelectorForm) is not part of
// this build.  data.xp3 wins over any other archive, and patch.xp3 never counts
// as a game (it is a patch for one).
bool FindDefaultGame(std::string &path, const std::string &exe_dir)
{
	std::vector<std::string> dirs;
	if (!exe_dir.empty()) dirs.push_back(exe_dir);
	if (dirs.empty() || dirs[0] != ".") dirs.push_back(".");
	// also accept a "games" sub-directory, the usual layout on handhelds
	if (!exe_dir.empty()) dirs.push_back(exe_dir + "/games");

	for (const std::string &dir : dirs) {
		const std::string data_xp3 = dir + "/data.xp3";
		if (TVPCheckExistentLocalFile(data_xp3) && TVPCheckArchive(ttstr(data_xp3)) == 1) {
			path = data_xp3;
			return true;
		}
	}

	std::vector<std::string> archives;
	for (const std::string &dir : dirs) {
		TVPListDir(dir, [&archives, &dir](const std::string &name, int mask) {
			if (!(mask & S_IFREG) || !HasXp3Extension(name)) return;
			std::string lower = name;
			for (char &c : lower) c = (char)tolower((unsigned char)c);
			if (lower == "patch.xp3") return;
			archives.push_back(dir + "/" + name);
		});
	}
	std::sort(archives.begin(), archives.end());
	for (const std::string &candidate : archives) {
		if (TVPCheckArchive(ttstr(candidate)) == 1) {
			path = candidate;
			return true;
		}
	}

	for (const std::string &dir : dirs) {
		if (IsBootableGameDir(dir)) {
			path = dir;
			return true;
		}
	}
	return false;
}

int RunFrameLoop(int fps_limit)
{
	// SDL_GetTicks64() counts milliseconds, so the budget must be in
	// milliseconds as well (a nanosecond budget here meant a ~4.6 h sleep per
	// frame at 60 fps - the loop advanced exactly once).
	const Uint64 frame_ms = fps_limit > 0 ? (Uint64)(1000 / fps_limit) : 0;
	Uint64 next_frame = SDL_GetTicks64();
	int frame_index = 0;

	while (!krkr2sdl::HostQuitRequested()) {
		// Scripted input (KRKR2_TEST_INPUT, Host.h): inject this frame's events
		// into the queue before they are read, so tests can drive touch and
		// game-pad input without the hardware.
		krkr2sdl::HostPumpTestInput(frame_index++);

		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) {
				TVPSDLWindowLayer *layer = TVPSDLWindowLayer::GetCurrent();
				if (layer) {
					layer->QueryUserClose();
				} else {
					krkr2sdl::HostRequestQuit();
				}
				continue;
			}
			krkr2sdl::HostDispatchSDLEvent(event);
		}

		// The engine pump: delivers events, ticks every window (which is where
		// the game draws and the frame gets presented) and advances timers.
		::Application->Run();

		// Reclaim textures the engine released this frame.
		krkr2sdl::HostRecycleTextures();

		// A window that was activated or resized needs its frame re-blitted.
		krkr2sdl::HostPresentLastFrame();

		if (frame_ms) {
			next_frame += frame_ms;
			Uint64 now = SDL_GetTicks64();
			if (next_frame > now) SDL_Delay((Uint32)(next_frame - now));
			else next_frame = now;
		}
	}
	return TVPTerminateCode;
}

} // namespace

// The engine's storage layer turns POSIX absolute paths into "file://./..."
// names (TVPPreNormalizeStorageName); a relative name carries no media and
// fails storage-name normalisation. Both reference entry points hand the engine
// an absolute path as well.
bool MakeAbsolutePath(const std::string &path, std::string &out)
{
	char resolved[PATH_MAX];
	if (!realpath(path.c_str(), resolved)) return false;
	out = resolved;
	return true;
}

int main(int argc, char **argv)
{
	Options opt;
	if (!ParseOptions(argc, argv, opt)) return 0;

	_argc = argc;
	_argv = argv;
	TVPMainThreadID = std::this_thread::get_id();

	std::string game_path = opt.game_path;
	if (game_path.empty() && !FindDefaultGame(game_path, ExecutableDirectory(argv[0]))) {
		fprintf(stderr,
			"krkr2: no game specified and none found next to the executable.\n"
			"       pass a .xp3 archive or a directory containing startup.tjs.\n");
		PrintUsage(argv[0]);
		return 2;
	}

	std::string abs_game_path;
	if (!MakeAbsolutePath(game_path, abs_game_path)) {
		fprintf(stderr, "krkr2: cannot resolve '%s': %s\n", game_path.c_str(), strerror(errno));
		return 2;
	}

	if (!opt.data_dir.empty()) {
		// The engine resolves its preference/save directories through
		// TVPGetInternalPreferencePath()/TVPGetDataPath(); the platform layer
		// honours KRKR2_DATA_DIR so this option needs no extra plumbing.
		setenv("KRKR2_DATA_DIR", opt.data_dir.c_str(), 1);
	}

	if (!krkr2sdl::HostInit("Kirikiroid2", opt.width, opt.height, opt.fullscreen, opt.vsync)) {
		fprintf(stderr, "krkr2: cannot initialize the SDL2 host.\n");
		return 1;
	}
	atexit(krkr2sdl::HostShutdown);

	// Scripted input for the tests (KRKR2_TEST_INPUT, Host.h); a no-op unless set.
	krkr2sdl::HostInitTestInput();

	// Bring the audio device up before any script runs.  The engine would
	// otherwise create it lazily on the first wave/movie play, and a buffer that
	// starts against a device still being opened can sit at position 0 while
	// reporting "play" (observed intermittently by the API conformance run, and
	// the reason the movie player calls this itself before creating its stream).
	TVPInitDirectSound();

	// Start the engine.  Failures here are almost always "the game could not be
	// opened": a wrong path, an encrypted archive without its patch, or a
	// storage whose name does not resolve.  The engine has already reported its
	// own message (through the platform layer's message box, which this port
	// mirrors to the console when no dialog can be shown); add the concrete
	// hint the user needs instead of leaving them with the tid.
	try {
		::Application->StartApplication(abs_game_path.c_str());
	} catch (const TJS::eTJSError &e) {
		fprintf(stderr, "krkr2: cannot start '%s': %s\n",
			abs_game_path.c_str(), e.GetMessage().AsStdString().c_str());
		fprintf(stderr,
			"krkr2: if this is an encrypted archive, place its patch files\n"
			"       (patch.tjs / patch.xp3 / xp3filter.tjs) next to the game;\n"
			"       encrypted data without a patch is rejected by design.\n");
		return 3;
	} catch (const std::exception &e) {
		fprintf(stderr, "krkr2: cannot start '%s': %s\n", abs_game_path.c_str(), e.what());
		return 3;
	}

	// The engine terminates by itself when the game produced no window at all
	// (TVPTerminateOnNoWindowStartup).  That is what happens whenever the
	// startup script could not be read - a directory without startup.tjs, a
	// wrong path, or an encrypted archive whose patch is missing - and in that
	// path the engine's own diagnostic is a bare message tid, so add the
	// actionable hint here.
	if (TVPGetWindowCount() == 0) {
		fprintf(stderr,
			"krkr2: '%s' did not start a game window (no startup script ran).\n"
			"       check that the path holds startup.tjs (or is an .xp3 that does),\n"
			"       and for an encrypted archive that its patch is next to it\n"
			"       (patch.tjs / patch.xp3 / xp3filter.tjs from the patch library).\n",
			abs_game_path.c_str());
	}

	int fps_limit = IndividualConfigManager::GetInstance()->GetValue<int>("fps_limit", 60);
	return RunFrameLoop(fps_limit);
}
