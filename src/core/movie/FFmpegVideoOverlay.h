//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Modern ffmpeg video overlay backend for iTVPVideoOverlay
//---------------------------------------------------------------------------
#ifndef FFmpegVideoOverlayH
#define FFmpegVideoOverlayH

#include "tjsCommHead.h"

class tTJSNI_VideoOverlay;
class iTVPVideoOverlay;
struct IStream;

/*
	Entry points visual/win32/VideoOvlImpl.cpp uses to obtain the player for
	each overlay mode (vomOverlay / vomLayer / vomMixer / vomMFEVR). All four
	are served by the same implementation: the mode decides which calls the
	engine makes into the object, not what the object is.
*/
extern void GetVideoOverlayObject(
	tTJSNI_VideoOverlay* callbackwin, struct IStream *stream, const tjs_char * streamname,
	const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out);

extern void GetVideoLayerObject(
	tTJSNI_VideoOverlay* callbackwin, struct IStream *stream, const tjs_char * streamname,
	const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out);

extern void GetMixingVideoOverlayObject(
	tTJSNI_VideoOverlay* callbackwin, struct IStream *stream, const tjs_char * streamname,
	const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out);

extern void GetMFVideoOverlayObject(
	tTJSNI_VideoOverlay* callbackwin, struct IStream *stream, const tjs_char * streamname,
	const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out);

#endif
