/*
	Video overlay object factories for platform builds without the ffmpeg movie
	player.

	`visual/win32/VideoOvlImpl.cpp` (the video overlay native class, compiled on
	every platform) obtains its player through these four functions, which the
	movie module normally provides: on Windows they are exported by krmovie.dll,
	here they were provided by movie/ffmpeg (the Kodi-derived player).

	That player is not part of this build yet - it is written against the ffmpeg
	2.x API that current releases no longer offer, so porting it is a separate
	piece of work. Until then this translation unit reports the situation the
	engine already has a defined behaviour for: it raises the standard
	"movie module cannot be loaded" exception, exactly as the Windows build does
	when krmovie.dll (and its codec backends) are absent. Scripts that try to
	open a movie get a TVP exception instead of silently doing nothing, and the
	rest of the engine is unaffected.
*/

#include "tjsCommHead.h"

#include "MsgIntf.h"
#include "Exception.h"

class tTJSNI_VideoOverlay;
class iTVPVideoOverlay;
struct IStream;

namespace {

void TVPMovieModuleUnavailable(const tjs_char *type)
{
	// TVPCannotLoadKrMovieDLL is the message the engine uses when the movie
	// module is missing; the file type is appended for diagnosis.
	TVPThrowExceptionMessage(TVPCannotLoadKrMovieDLL,
		ttstr(TJS_W("(no movie player in this build: ")) + ttstr(type ? type : TJS_W("unknown")) + TJS_W(")"));
}

} // namespace

void GetVideoOverlayObject(
	tTJSNI_VideoOverlay *callbackwin, struct IStream *stream, const tjs_char *streamname,
	const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out)
{
	TVPMovieModuleUnavailable(type);
}

void GetVideoLayerObject(
	tTJSNI_VideoOverlay *callbackwin, struct IStream *stream, const tjs_char *streamname,
	const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out)
{
	TVPMovieModuleUnavailable(type);
}

void GetMixingVideoOverlayObject(
	tTJSNI_VideoOverlay *callbackwin, struct IStream *stream, const tjs_char *streamname,
	const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out)
{
	TVPMovieModuleUnavailable(type);
}

void GetMFVideoOverlayObject(
	tTJSNI_VideoOverlay *callbackwin, struct IStream *stream, const tjs_char *streamname,
	const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out)
{
	TVPMovieModuleUnavailable(type);
}
