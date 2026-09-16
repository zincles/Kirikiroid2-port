# Kirikiroid2 — Linux / SDL2 build (no cocos2d-x)
#
# This is a port build tree, not the Android ndk-build one: it compiles the
# engine core (tjs2 + krkrz-derived core + software renderer) against system
# libraries and hosts it in an SDL2 window instead of cocos2d-x.
#
# Authoritative Android source list: src/core/Android.mk, src/plugins/Android.mk.
# Wildcards below intentionally mirror those files so the two builds stay in sync.

cmake_minimum_required(VERSION 3.20)
project(krkr2 CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(KRKR2_BUILD_PLUGINS      "Build the built-in (ncbind) plugin set"      ON)
# The movie player (movie/FFmpegVideoOverlay.cpp) needs ffmpeg; without it the
# overlay factories report "movie module unavailable" to scripts, which is the
# honest behaviour for a build without the player.
option(KRKR2_ENABLE_MOVIE       "Build the ffmpeg movie player (VideoOverlay)" ON)
option(KRKR2_ENABLE_BPG         "Build the BPG image loader"                 OFF)
option(KRKR2_ENABLE_JXR         "Build the JPEG-XR image loader"             OFF)
# .7z game archives are read through libarchive (base/7zArchive.cpp), which is
# a required dependency either way; the option only exists to drop the format.
option(KRKR2_ENABLE_7Z          "Build the libarchive-backed 7z archive reader" ON)
option(KRKR2_ENABLE_UNRAR       "Build the Windows unrar.dll backend"        OFF)
option(KRKR2_ENABLE_OPENCV      "Build the (heavier) OpenCV code paths"      OFF)
option(KRKR2_ENABLE_FFWAVE      "Build the ffmpeg-backed generic wave decoder (mp3/aac/...)" ON)
option(KRKR2_ASAN               "Build with AddressSanitizer"                OFF)
set(KRKR2_SDL2_PREFIX "" CACHE PATH
	"Prefix of an SDL2 2.x installation to build against (e.g. ~/.local/sdl2). \
Empty uses whatever pkg-config finds; note that sdl2-compat (SDL2 API on SDL3) \
is known to crash inside SDL_ShowMessageBox, so a real SDL2 build is preferred.")

if(KRKR2_ENABLE_MOVIE OR KRKR2_ENABLE_FFWAVE)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale libswresample)
endif()

if(KRKR2_ASAN)
	# The option was declared but never turned into flags (found by an agent
	# while debugging the decoder's IO buffer).
	add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
	add_link_options(-fsanitize=address)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
if(KRKR2_SDL2_PREFIX)
	list(PREPEND CMAKE_PREFIX_PATH ${KRKR2_SDL2_PREFIX})
	set(ENV{PKG_CONFIG_PATH} "${KRKR2_SDL2_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
pkg_check_modules(SDL2     REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(PNG      REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(JPEG     REQUIRED IMPORTED_TARGET libjpeg)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
pkg_check_modules(ZLIB     REQUIRED IMPORTED_TARGET zlib)
# bzip2 has no pkg-config file on devkitPro; fall back to the plain library.
pkg_check_modules(BZIP2 IMPORTED_TARGET bzip2)
if(BZIP2_FOUND)
	set(KRKR2_BZIP2 ${BZIP2_LINK_LIBRARIES})
else()
	find_path(KRKR2_BZIP2_INCLUDE_DIR NAMES bzlib.h REQUIRED)
	find_library(KRKR2_BZIP2_LIBRARY NAMES bz2 libbz2 REQUIRED)
	list(APPEND KRKR2_INCLUDE_DIRS ${KRKR2_BZIP2_INCLUDE_DIR})
	set(KRKR2_BZIP2 ${KRKR2_BZIP2_LIBRARY})
endif()
pkg_check_modules(LZ4      REQUIRED IMPORTED_TARGET liblz4)
pkg_check_modules(ONIG     REQUIRED IMPORTED_TARGET oniguruma)
pkg_check_modules(OPENAL   REQUIRED IMPORTED_TARGET openal)
pkg_check_modules(OGG      REQUIRED IMPORTED_TARGET ogg)
pkg_check_modules(VORBIS   REQUIRED IMPORTED_TARGET vorbis vorbisfile)
pkg_check_modules(OPUSFILE REQUIRED IMPORTED_TARGET opusfile)
pkg_check_modules(ARCHIVE  REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(TINYXML2 REQUIRED IMPORTED_TARGET tinyxml2)
pkg_check_modules(WEBP     REQUIRED IMPORTED_TARGET libwebp)
pkg_check_modules(XXHASH   REQUIRED IMPORTED_TARGET libxxhash)
find_package(Threads REQUIRED)

# newlib/libnx has no libdl (CMake still reports "dl" for this platform).
if(KRKR2_TARGET_SWITCH)
	set(KRKR2_DL_LIBS "")
else()
	set(KRKR2_DL_LIBS ${CMAKE_DL_LIBS})
endif()

# sdl2-compat reimplements the SDL2 API on top of SDL3 and reports an SDL2
# version, so it cannot be told apart by version. It is known to crash inside
# SDL_ShowMessageBox (verified on Arch, sdl2-compat 2.32.72), so say so instead
# of letting the port fail at runtime in an error dialog.
if(NOT KRKR2_TARGET_SWITCH)
	pkg_get_variable(SDL2_PC_DIR sdl2 pcfiledir)
	if(SDL2_PC_DIR AND EXISTS "${SDL2_PC_DIR}/sdl2.pc")
		file(READ "${SDL2_PC_DIR}/sdl2.pc" KRKR2_SDL2_PC_CONTENT)
		if(KRKR2_SDL2_PC_CONTENT MATCHES "sdl2[-_]compat")
			message(WARNING
				"SDL2 comes from sdl2-compat (${SDL2_PC_DIR}/sdl2.pc). "
				"sdl2-compat is known to crash in SDL_ShowMessageBox; build or install a real "
				"SDL2 2.x and pass -DKRKR2_SDL2_PREFIX=<prefix> to use it.")
		endif()
	endif()
endif()


# ---------------------------------------------------------------------------
# Include paths (mirrors src/core/Android.mk minus the missing vendor/ tree)
# ---------------------------------------------------------------------------
set(KRKR2_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

set(KRKR2_INCLUDE_DIRS
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/compat-include
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${KRKR2_CORE_DIR}
	${KRKR2_CORE_DIR}/base
	${KRKR2_CORE_DIR}/base/win32
	${KRKR2_CORE_DIR}/environ
	${KRKR2_CORE_DIR}/environ/win32
	${KRKR2_CORE_DIR}/environ/sdl
	${KRKR2_CORE_DIR}/environ/ConfigManager
	${KRKR2_CORE_DIR}/msg
	${KRKR2_CORE_DIR}/msg/win32
	${KRKR2_CORE_DIR}/extension
	${KRKR2_CORE_DIR}/sound
	${KRKR2_CORE_DIR}/sound/win32
	${KRKR2_CORE_DIR}/tjs2
	${KRKR2_CORE_DIR}/utils
	${KRKR2_CORE_DIR}/utils/win32
	${KRKR2_CORE_DIR}/utils/minizip
	${KRKR2_CORE_DIR}/visual
	${KRKR2_CORE_DIR}/visual/ARM
	${KRKR2_CORE_DIR}/visual/win32
	${KRKR2_CORE_DIR}/visual/gl
	${KRKR2_CORE_DIR}/visual/ogl
	${CMAKE_CURRENT_SOURCE_DIR}/src/plugins
	${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/ncbind
)

set(KRKR2_COMPILE_OPTIONS
	-fsigned-char
	-D__STDC_CONSTANT_MACROS
	-DTVP_SDL2=1
)
if(KRKR2_ENABLE_7Z)
	# base/7zArchive.cpp registers TVPOpen7ZArchive in the archive table.
	list(APPEND KRKR2_COMPILE_OPTIONS -DKRKR2_ENABLE_7Z=1)
endif()
if(KRKR2_ENABLE_BPG)
	list(APPEND KRKR2_COMPILE_OPTIONS -DKRKR2_ENABLE_BPG=1)
endif()
if(KRKR2_ENABLE_JXR)
	list(APPEND KRKR2_COMPILE_OPTIONS -DKRKR2_ENABLE_JXR=1)
endif()
if(KRKR2_ENABLE_OPENCV)
	list(APPEND KRKR2_COMPILE_OPTIONS -DKRKR2_ENABLE_OPENCV=1)
endif()
if(KRKR2_ENABLE_FFWAVE)
	# WaveIntf.cpp registers the ffmpeg-backed decoder only under this macro;
	# the option above alone would just add the source file to the build.
	list(APPEND KRKR2_COMPILE_OPTIONS -DKRKR2_ENABLE_FFWAVE=1)
endif()

# ---------------------------------------------------------------------------
# Source lists
# ---------------------------------------------------------------------------
file(GLOB KRKR2_TJS2      CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/tjs2/*.cpp)
file(GLOB KRKR2_BASE      CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/base/*.cpp)
file(GLOB KRKR2_BASE_W32  CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/base/win32/*.cpp)
file(GLOB KRKR2_ENVIRON   CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/environ/*.cpp)
file(GLOB KRKR2_ENVIRON_CFG CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/environ/ConfigManager/*.cpp)
file(GLOB KRKR2_ENVIRON_SDL CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/environ/sdl/*.cpp)
file(GLOB KRKR2_MSG       CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/msg/*.cpp)
file(GLOB KRKR2_SOUND     CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/sound/*.cpp)
file(GLOB KRKR2_SOUND_W32 CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/sound/win32/*.cpp)
file(GLOB KRKR2_UTILS_C   CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/utils/*.c)
file(GLOB KRKR2_UTILS     CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/utils/*.cpp)
file(GLOB KRKR2_UTILS_ENC CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/utils/encoding/*.c)
file(GLOB KRKR2_UTILS_W32 CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/utils/win32/*.cpp)
file(GLOB KRKR2_MINIZIP_C CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/utils/minizip/*.c)
file(GLOB KRKR2_MINIZIP   CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/utils/minizip/*.cpp)
file(GLOB KRKR2_VISUAL    CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/visual/*.cpp)
file(GLOB KRKR2_VISUAL_GL CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/visual/gl/*.cpp)
file(GLOB KRKR2_VISUAL_W32 CONFIGURE_DEPENDS ${KRKR2_CORE_DIR}/visual/win32/*.cpp)
file(GLOB KRKR2_PLUGINS   CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/*.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/*.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/ncbind/*.cpp)

list(APPEND KRKR2_ENVIRON ${KRKR2_CORE_DIR}/environ/win32/SystemControl.cpp)
# POSIX pieces of the platform layer shared with the Android build.
list(APPEND KRKR2_ENVIRON ${KRKR2_CORE_DIR}/environ/linux/Platform.cpp)
# main.cpp is the executable's entry point, not part of the library.
list(REMOVE_ITEM KRKR2_ENVIRON_SDL ${KRKR2_CORE_DIR}/environ/sdl/main.cpp)
list(APPEND KRKR2_MSG
	${KRKR2_CORE_DIR}/msg/win32/MsgImpl.cpp
	${KRKR2_CORE_DIR}/msg/win32/OptionsDesc.cpp)

# Same exclusions as src/core/Android.mk (Win32-only files that the Android
# build also drops), plus the subsystems gated by the options above.
list(REMOVE_ITEM KRKR2_BASE_W32
	${KRKR2_CORE_DIR}/base/win32/FuncStubs.cpp
	${KRKR2_CORE_DIR}/base/win32/SusieArchive.cpp)
list(REMOVE_ITEM KRKR2_VISUAL_W32
	${KRKR2_CORE_DIR}/visual/win32/GDIFontRasterizer.cpp
	${KRKR2_CORE_DIR}/visual/win32/NativeFreeTypeFace.cpp
	${KRKR2_CORE_DIR}/visual/win32/TVPSysFont.cpp
	${KRKR2_CORE_DIR}/visual/win32/VSyncTimingThread.cpp)

# XP3ArchiveRepack embeds the 7-Zip *encoder* (7zOut.h and the C++ 7-Zip SDK,
# which lived in vendor/ and is not part of this port).  That is independent of
# the archive *reader* option above: the reader is libarchive based and is
# built whenever KRKR2_ENABLE_7Z is on, while the repacker cannot be built at
# all here.
list(REMOVE_ITEM KRKR2_ENVIRON ${KRKR2_CORE_DIR}/environ/XP3ArchiveRepack.cpp)
if(NOT KRKR2_ENABLE_7Z)
	list(REMOVE_ITEM KRKR2_BASE ${KRKR2_CORE_DIR}/base/7zArchive.cpp)
endif()
if(NOT KRKR2_ENABLE_BPG)
	list(REMOVE_ITEM KRKR2_VISUAL ${KRKR2_CORE_DIR}/visual/LoadBPG.cpp)
endif()
if(NOT KRKR2_ENABLE_JXR)
	list(REMOVE_ITEM KRKR2_VISUAL ${KRKR2_CORE_DIR}/visual/LoadJXR.cpp)
endif()

# Software renderer build: only the texture-codec helpers from visual/ogl are
# compiled (RenderManager_ogl.cpp is the cocos2d-coupled GL renderer).
list(APPEND KRKR2_VISUAL
	${KRKR2_CORE_DIR}/visual/ogl/etcpak.cpp
	${KRKR2_CORE_DIR}/visual/ogl/PVRTDecompress.cpp)

# CPU-routine hook: on ARM the NEON TVPGL implementations are registered by
# visual/ARM/tvpgl_arm.cpp; visual/tvpgl_cpu_init.cpp provides the (empty)
# non-ARM definition of the same hook.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7|armv8|aarch64|ARM64)")
	list(APPEND KRKR2_VISUAL ${KRKR2_CORE_DIR}/visual/ARM/tvpgl_arm.cpp)
endif()
if(NOT KRKR2_ENABLE_FFWAVE)
	list(REMOVE_ITEM KRKR2_SOUND ${KRKR2_CORE_DIR}/sound/FFWaveDecoder.cpp)
endif()
# visual/win32/VideoOvlImpl.cpp provides the VideoOverlay native class (the
# script-visible overlay object) and the layer/texture machinery around it; the
# player behind it comes from movie/FFmpegVideoOverlay.cpp, or from
# movie/krmovie_unavailable.cpp in builds without ffmpeg movie support.
list(APPEND KRKR2_VISUAL_W32 ${KRKR2_CORE_DIR}/visual/win32/VideoOvlImpl.cpp)
# layerExMovie.cpp implements the KiriKiriZ "layerExMovie.dll" extension on top
# of the Kodi-derived movie/ffmpeg tree (KRMovieLayer etc.), which this port
# does not build - its video player is movie/FFmpegVideoOverlay.cpp instead.
# Porting the extension onto that backend is a separate follow-up, so the plugin
# is left out here (the standard VideoOverlay/movie path games use is provided).
list(REMOVE_ITEM KRKR2_PLUGINS ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/layerExMovie.cpp)

set(KRKR2_SOURCES
	${KRKR2_TJS2} ${KRKR2_BASE} ${KRKR2_BASE_W32}
	${KRKR2_ENVIRON} ${KRKR2_ENVIRON_CFG} ${KRKR2_ENVIRON_SDL} ${KRKR2_MSG}
	${KRKR2_SOUND} ${KRKR2_SOUND_W32}
	${KRKR2_UTILS_C} ${KRKR2_UTILS} ${KRKR2_UTILS_ENC} ${KRKR2_UTILS_W32}
	${KRKR2_MINIZIP_C} ${KRKR2_MINIZIP}
	${KRKR2_VISUAL} ${KRKR2_VISUAL_GL} ${KRKR2_VISUAL_W32}
	${KRKR2_CORE_DIR}/extension/Extension.cpp)

if(KRKR2_ENABLE_MOVIE)
	# Modern-ffmpeg VideoOverlay implementation (decode + engine-texture
	# presentation + mixer audio).  The Kodi-derived movie/ffmpeg tree is not
	# part of this build.
	list(APPEND KRKR2_SOURCES ${KRKR2_CORE_DIR}/movie/FFmpegVideoOverlay.cpp)
else()
	list(APPEND KRKR2_SOURCES ${KRKR2_CORE_DIR}/movie/krmovie_unavailable.cpp)
endif()

add_library(krkr2core STATIC ${KRKR2_SOURCES})
target_include_directories(krkr2core PUBLIC ${KRKR2_INCLUDE_DIRS})
target_compile_options(krkr2core PUBLIC ${KRKR2_COMPILE_OPTIONS}
	-Wno-deprecated-declarations -Wno-narrowing -Wno-write-strings)
target_link_libraries(krkr2core PUBLIC
	PkgConfig::SDL2 PkgConfig::FREETYPE PkgConfig::PNG PkgConfig::JPEG
	PkgConfig::ZLIB PkgConfig::LZ4 PkgConfig::ONIG
	PkgConfig::OPENAL PkgConfig::OGG PkgConfig::VORBIS PkgConfig::OPUSFILE
	PkgConfig::ARCHIVE PkgConfig::TINYXML2 PkgConfig::WEBP PkgConfig::XXHASH
	PkgConfig::TURBOJPEG ${KRKR2_BZIP2} Threads::Threads ${KRKR2_DL_LIBS})

if(KRKR2_ENABLE_MOVIE OR KRKR2_ENABLE_FFWAVE)
	target_link_libraries(krkr2core PUBLIC PkgConfig::FFMPEG)
endif()

if(KRKR2_BUILD_PLUGINS)
	add_library(krkr2plugin STATIC ${KRKR2_PLUGINS})
	target_include_directories(krkr2plugin PRIVATE ${KRKR2_INCLUDE_DIRS})
	target_link_libraries(krkr2plugin PUBLIC krkr2core)
endif()

# ---------------------------------------------------------------------------
# Host executable (SDL2 window / input / audio, no cocos2d-x)
# ---------------------------------------------------------------------------
if(EXISTS ${KRKR2_CORE_DIR}/environ/sdl/main.cpp)
	add_executable(krkr2 ${KRKR2_CORE_DIR}/environ/sdl/main.cpp)
	target_link_libraries(krkr2 PRIVATE krkr2core)
	if(KRKR2_BUILD_PLUGINS)
		# The built-in plugins register themselves through static initialisers
		# (ncbAutoRegister instances) that no symbol references, so a plain
		# static-library link drops every one of their objects and the plugins
		# silently do not exist (verified: "xp3filter.dll" was absent from the
		# binary, so encrypted archives could not load). Link them in whole.
		#
		# The raw linker flags are used rather than $<LINK_LIBRARY:WHOLE_ARCHIVE>,
		# which CMake rejects for the Switch target ("Feature 'WHOLE_ARCHIVE' ...
		# is not supported for the 'CXX' link language"): CMake only knows the
		# feature for platforms it has support tables for. GNU ld (Linux and
		# devkitA64) takes --whole-archive.
		if(MSVC)
			target_link_libraries(krkr2 PRIVATE /WHOLEARCHIVE:$<TARGET_FILE:krkr2plugin>)
		else()
			# $<TARGET_FILE:...> rather than the bare target name: CMake orders
			# link items itself, and a target can end up outside a bare
			# -Wl,--whole-archive ... -Wl,--no-whole-archive pair.
			target_link_libraries(krkr2 PRIVATE
				-Wl,--whole-archive $<TARGET_FILE:krkr2plugin> -Wl,--no-whole-archive)
		endif()
	endif()
	# Same allocator interception the Android build uses (src/core/Android.mk):
	# Application.cpp's memory tracking and the out-of-memory handler resolve
	# __real_malloc/__real_free/... through these wrap options.
	if(UNIX AND NOT APPLE AND NOT KRKR2_TARGET_SWITCH)
		target_link_options(krkr2 PRIVATE
			-Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=realloc -Wl,--wrap=calloc)
	endif()
	# A local SDL2 build lives outside the loader's default search path.
	if(KRKR2_SDL2_PREFIX AND NOT KRKR2_TARGET_SWITCH)
		target_link_options(krkr2 PRIVATE "-Wl,-rpath,${KRKR2_SDL2_PREFIX}/lib")
	endif()
	# Switch homebrew ships as an .nro produced from the linked ELF.
	if(KRKR2_TARGET_SWITCH)
		find_program(KRKR2_ELF2NRO elf2nro HINTS "${DEVKITPRO}/tools/bin" REQUIRED)
		set(KRKR2_NRO ${CMAKE_BINARY_DIR}/krkr2.nro)
		add_custom_command(TARGET krkr2 POST_BUILD
			COMMAND ${KRKR2_ELF2NRO} $<TARGET_FILE:krkr2> ${KRKR2_NRO}
			COMMENT "Packaging Switch homebrew binary ${KRKR2_NRO}")
	endif()
endif()
