//---------------------------------------------------------------------------
// layerExMovie.dll - the KiriKiriZ extension that plays a movie inside a Layer.
//
// Upstream builds this on the Kodi-derived movie tree
// (movie/ffmpeg/KRMovieLayer.h: KRMovie::VideoPresentLayer plus the KRMovieEvent
// callback protocol).  That tree is not part of this build; the movie player here
// is movie/FFmpegVideoOverlay.cpp, which implements the iTVPVideoOverlay
// interface declared in krmovie.h - the same interface the engine's own
// VideoOverlay object uses - and it serves what this extension needs:
//
//   * GetVideoLayerObject(..., &player) hands back a player that renders into the
//     two engine textures given to SetVideoBuffer(), which is how the Kodi layer
//     presented frames here as well (BuildGraph + SetVideoBuffer);
//   * GetFrontBuffer() brings the current picture into the front texture, so the
//     extension no longer needs a queue of KRMovieEvent callbacks to know when
//     to copy: it asks the player once per engine tick (OnContinuousCallback),
//     refreshes the layer image when the presented frame number changes, and
//     treats a status change to vsEnded as the end of the movie.
//
// The script-visible surface is unchanged: openMovie(filename, alpha),
// startMovie(loop), stopMovie(), isPlayingMovie() and the onStartMovie /
// onUpdateMovie / onStopMovie callbacks.
//
// The alpha layout is KiriKiriZ's: an alpha movie stores colour in the upper half
// of the frame and its alpha in the lower half, so the layer takes half the
// height and the copy uses the colour-aware blend.
//---------------------------------------------------------------------------
#include <stdlib.h>
#include <stdint.h>
#include <algorithm>

#include "tjsCommHead.h"
#include "EventIntf.h"
#include "layerExBase.hpp"
#include "ncbind/ncbind.hpp"
#include "Application.h"
#include "LayerBitmapIntf.h"
#include "StorageImpl.h"         // TVPCreateIStream
#include "combase.h"             // IStream
#include "krmovie.h"             // iTVPVideoOverlay
#include "movie/FFmpegVideoOverlay.h" // GetVideoLayerObject
#include "RenderManager.h"
#include "DebugIntf.h"

#define NCB_MODULE_NAME TJS_W("layerExMovie.dll")

/*
 * Movie drawing layer.
 */
struct layerExMovie : public layerExBase_GL, tTVPContinuousEventCallbackIntf
{
protected:
	iTVPVideoOverlay *VideoOverlay;

	long movieWidth;
	long movieHeight;
	class tTVPBaseTexture *Bitmap[2];

	bool loop;
	bool alpha;

	void clearMovie();

	DispatchT onStartMovie;
	DispatchT onUpdateMovie;
	DispatchT onStopMovie;

	bool playing;
	// The presented frame number the layer image currently holds (-1: none), and
	// the last status seen, so a change can be reported once.
	int lastFrame;
	tTVPVideoStatus lastStatus;

public:
	layerExMovie(DispatchT obj);
	~layerExMovie();

public:
	// Prepare a movie file for playback.
	void openMovie(const tjs_char *filename, bool alpha);

	void startMovie(bool loop);
	void stopMovie();

	void start();
	void stop();

	bool isPlayingMovie();

	void onUpdate();
	void onEnded();

	/*
	 * Continuous callback: called whenever the engine is idle.  It refreshes the
	 * layer image and reports the end of the movie.
	 */
	virtual void TJS_INTF_METHOD OnContinuousCallback(tjs_uint64 tick);
};

/**
 * Constructor
 */
layerExMovie::layerExMovie(DispatchT obj) : layerExBase_GL(obj)
{
	VideoOverlay = NULL;
	loop = false;
	alpha = false;
	movieWidth = 0;
	movieHeight = 0;
	{
		tTJSVariant var;
		if (TJS_SUCCEEDED(obj->PropGet(TJS_IGNOREPROP, TJS_W("onStartMovie"), NULL, &var, obj))) onStartMovie = var;
		else onStartMovie = NULL;
		if (TJS_SUCCEEDED(obj->PropGet(TJS_IGNOREPROP, TJS_W("onStopMovie"), NULL, &var, obj))) onStopMovie = var;
		else onStopMovie = NULL;
		if (TJS_SUCCEEDED(obj->PropGet(TJS_IGNOREPROP, TJS_W("onUpdateMovie"), NULL, &var, obj))) onUpdateMovie = var;
		else onUpdateMovie = NULL;
	}
	playing = false;
	lastFrame = -1;
	lastStatus = vsStopped;
	Bitmap[0] = Bitmap[1] = nullptr;
}

/**
 * Destructor
 */
layerExMovie::~layerExMovie()
{
	stopMovie();
	// clearMovie() has released the player by now, and the player only owns the
	// textures it allocated itself (SetVideoBuffer takes over ours), so these are
	// ours to delete.
	if (Bitmap[0]) delete Bitmap[0];
	if (Bitmap[1]) delete Bitmap[1];
}

void
layerExMovie::clearMovie()
{
	// The player holds a reference to the IStream built in openMovie(), and that
	// adapter owns the tTJSBinaryStream it was created from
	// (tTVPIStreamAdapter::~tTVPIStreamAdapter deletes it), so neither object is
	// deleted here: releasing the player releases the stream with it.  Deleting
	// the stream here as well was a double free that crashed in stopMovie().
	if (VideoOverlay) {
		VideoOverlay->Release(), VideoOverlay = NULL;
	}
	lastFrame = -1;
	lastStatus = vsStopped;
}

/**
 * Open a movie file and prepare it for playback.
 * @param filename file name
 * @param alpha alpha flag (the frame is processed at half height, see above)
 */
void
layerExMovie::openMovie(const tjs_char *filename, bool alpha)
{
	clearMovie();
	this->alpha = alpha;
	movieWidth = 0;
	movieHeight = 0;

	// The stream is wrapped into an IStream and ownership moves to that adapter
	// (TVPCreateIStream(stream) hands it to tTVPIStreamAdapter, whose destructor
	// deletes it), so it must not be deleted here - not even on the error paths
	// below once the adapter exists.
	tTJSBinaryStream *in = TVPCreateStream(filename, TJS_BS_READ);
	if (in == NULL) {
		ttstr error = filename;
		error += TJS_W(": file cannot be opened");
		TVPAddLog(error);
		return;
	}
	const uint64_t size = in->GetSize();

	ttstr ext = TVPExtractStorageExt(filename);
	ext.ToLowerCase();

	// The factory keeps its own reference on the stream (see the note on the
	// IStream ownership in FFmpegVideoOverlay.cpp), so drop ours immediately.
	IStream *stream = TVPCreateIStream(in);
	if(!stream) {
		// no adapter was created, so the stream is still ours to delete
		delete in;
		TVPAddLog(ttstr(filename) + TJS_W(": cannot wrap the stream"));
		clearMovie();
		return;
	}
	iTVPVideoOverlay *player = nullptr;
	GetVideoLayerObject(nullptr, stream, filename, ext.c_str(), size, &player);
	stream->Release();
	if (!player) {
		TVPAddLog(ttstr(filename) + TJS_W(": the movie player could not open it"));
		clearMovie();
		return;
	}
	VideoOverlay = player;

	long w = 0, h = 0;
	VideoOverlay->GetVideoSize(&w, &h);
	movieWidth = w;
	movieHeight = h;

	if (Bitmap[0]) delete Bitmap[0];
	if (Bitmap[1]) delete Bitmap[1];
	Bitmap[0] = new tTVPBaseTexture((tjs_int)w, (tjs_int)h);
	Bitmap[1] = new tTVPBaseTexture((tjs_int)w, (tjs_int)h);
	VideoOverlay->SetVideoBuffer(Bitmap[0], Bitmap[1], (long)(w * h * 4));

	if (alpha) {
		movieWidth /= 2;
	}
	_this->SetSize((tjs_int)movieWidth, (tjs_int)movieHeight);
	_this->SetType(alpha ? ltAlpha : ltOpaque);
}

/**
 * Start the movie.
 */
void
layerExMovie::startMovie(bool loop)
{
	if (VideoOverlay) {
		this->loop = loop;
		lastFrame = -1;
		lastStatus = vsStopped;
		VideoOverlay->Play();
		start();
		if (onStartMovie != NULL) {
			onStartMovie->FuncCall(0, NULL, NULL, NULL, 0, NULL, _obj);
		}
	}
}

/**
 * Stop the movie.
 */
void
layerExMovie::stopMovie()
{
	bool p = playing;
	if (VideoOverlay) VideoOverlay->Stop();
	stop();
	clearMovie();
	if (p) {
		if (onStopMovie != NULL) {
			onStopMovie->FuncCall(0, NULL, NULL, NULL, 0, NULL, _obj);
		}
	}
}

bool
layerExMovie::isPlayingMovie()
{
	return playing;
}

void
layerExMovie::start()
{
	stop();
	TVPAddContinuousEventHook(this);
	playing = true;
}

/**
 * Stop the continuous callback hook.
 */
void
layerExMovie::stop()
{
	TVPRemoveContinuousEventHook(this);
	playing = false;
}

void
layerExMovie::onUpdate()
{
	// Copy the current picture from the player into the layer image.
	reset();
	tTVPBaseTexture *frontbmp = VideoOverlay->GetFrontBuffer();
	if (frontbmp) {
		iTVPTexture2D *src = frontbmp->GetTexture();
		iTVPTexture2D *dst = _this->GetMainImage()->GetTextureForRender(false, nullptr);
		tTVPRect rcdst(_clipLeft, _clipTop, _clipLeft + _clipWidth, _clipTop + _clipHeight);
		iTVPRenderMethod *method;
		if (alpha) {
			static iTVPRenderMethod *_method = TVPGetRenderManager()->GetRenderMethod("CopyColor");
			method = _method;
		} else {
			static iTVPRenderMethod *_method = TVPGetRenderManager()->GetRenderMethod("Copy");
			method = _method;
		}
		tRenderTexRectArray::Element src_tex[] = {
			tRenderTexRectArray::Element(src,
			tTVPRect(0, 0, std::min(_width, (tjs_int)movieWidth), std::min(_height, (tjs_int)movieHeight)))
		};
		TVPGetRenderManager()->OperateRect(method, dst, nullptr, rcdst, src_tex);
	}
	if (onUpdateMovie != NULL) {
		onUpdateMovie->FuncCall(0, NULL, NULL, NULL, 0, NULL, _obj);
	}
}

void
layerExMovie::onEnded()
{
	if (loop) {
		VideoOverlay->Rewind();
		VideoOverlay->Play();
		lastFrame = -1;
		lastStatus = vsStopped;
	} else {
		Application->PostUserMessage(std::bind(&layerExMovie::stopMovie, this));
	}
}

void TJS_INTF_METHOD
layerExMovie::OnContinuousCallback(tjs_uint64 tick)
{
	if (VideoOverlay) {
		int frame = -1;
		VideoOverlay->GetFrame(&frame);
		if (frame != lastFrame) {
			lastFrame = frame;
			onUpdate();
		}
		tTVPVideoStatus status = vsStopped;
		VideoOverlay->GetStatus(&status);
		if (status == vsEnded && lastStatus != vsEnded) {
			onEnded();
		}
		lastStatus = status;
	} else {
		stop();
	}
}

// ----------------------------------- class registration

NCB_GET_INSTANCE_HOOK(layerExMovie)
{
	// instance getter
	NCB_INSTANCE_GETTER(objthis) { // objthis as an iTJSDispatch2* argument
		ClassT* obj = GetNativeInstance(objthis);	// get the native instance pointer
		if (!obj) {
			obj = new ClassT(objthis);				// create it if it does not exist
			SetNativeInstance(objthis, obj);		// register it as objthis' native instance
		}
		return obj;
	}

	// destructor (called after the last method call)
	~NCB_GET_INSTANCE_HOOK_CLASS() {
	}
};


// attach with hook
NCB_ATTACH_CLASS_WITH_HOOK(layerExMovie, Layer) {
	NCB_METHOD(openMovie);
	NCB_METHOD(startMovie);
	NCB_METHOD(stopMovie);
	NCB_METHOD(isPlayingMovie);
}
