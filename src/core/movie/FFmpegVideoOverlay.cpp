//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Modern ffmpeg video overlay backend for iTVPVideoOverlay
//
// This is the movie player behind visual/win32/VideoOvlImpl.cpp (the
// script-visible VideoOverlay object) for this port.  It implements the
// overlay interface (visual/win32/krmovie.h) directly on top of
// libavformat/libavcodec/libswscale/libswresample - the Kodi-derived
// movie/ffmpeg tree is not part of this build.
//
// What each mode the engine can select means here:
//
//   layer (vomLayer, mode 1)
//     the engine hands over two 32bpp engine textures through
//     SetVideoBuffer() and, for every EC_UPDATE event the player posts, asks
//     GetFrontBuffer() which of the two holds the current picture; that one
//     becomes the layer's main image and is drawn by the engine.  No window,
//     GL or SDL call is involved: frames are converted to AV_PIX_FMT_BGRA
//     (TVP's 32bpp layout is 0xAARRGGBB, i.e. B,G,R,A in memory) and written
//     into the back buffer on the main thread inside GetFrontBuffer().
//
//   mixer (vomMixer, mode 2) and MFEVR (vomMFEVR, mode 3)
//     the engine supplies no textures; the player owns two of them and
//     composites the front one into the mixing bitmap given to
//     SetMixingBitmap() from PresentVideoImage() (also main thread).
//
//   overlay (vomOverlay, mode 0)
//     decodes and reports state like the other modes, but this port has no
//     window overlay surface for the player to draw into (the host window's
//     AddOverlay/UpdateOverlay hooks are compiled out), so nothing appears on
//     screen; a log line says so.  Scripts that want to see the movie pick
//     layer or mixer mode.
//
// Audio goes into the engine mixer (TVPCreateSoundBuffer()/iTVPSoundBuffer,
// the sink the wave player uses), resampled with libswresample to interleaved
// signed 16 bit.  The audio playback position is the master clock while a
// device really consumes samples; otherwise a wall clock on TVPGetTickCount()
// drives the video, so a machine without a working audio device still plays.
// Video is paced against that clock: a frame is presented when it is due and
// dropped when it is more than kVideoDropLateMs behind, which bounds drift
// after a stall instead of letting it accumulate.
//---------------------------------------------------------------------------

#include "tjsCommHead.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/channel_layout.h"
#include "libavutil/pixdesc.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
}

#include "FFmpegVideoOverlay.h"

#include "krmovie.h"
#include "combase.h"           // IStream, LARGE_INTEGER, HRESULT
#include "LayerBitmapIntf.h"   // tTVPBaseTexture
#include "RenderManager.h"     // iTVPTexture2D
#include "WaveMixer.h"         // TVPCreateSoundBuffer(), iTVPSoundBuffer
#include "DebugIntf.h"
#include "MsgIntf.h"
#include "TickCount.h"
#include "VideoOvlImpl.h"      // tTJSNI_VideoOverlay::PostEvent()
#include "NativeEventQueue.h"  // NativeEvent

//---------------------------------------------------------------------------
namespace {

const AVRational TVPMsTimeBase = { 1, 1000 };

// Audio is handed to the mixer in chunks of this many frames; with
// kAudioBufferChunks of them queued the audio latency stays around 180ms,
// which is what the A/V sync leans on.
const int kAudioChunkFrames = 1024;
const int kAudioBufferChunks = 8;
// Do not decode more than this much audio ahead of the device.
const double kMaxAudioQueuedSecs = 0.35;
// Present a video frame this many ms before it is due (covers the latency
// between posting the EC_UPDATE event and the engine drawing the frame).
const double kVideoLookaheadMs = 4.0;
// A frame later than this is dropped rather than presented, so the decoder
// catches up with the clock instead of drifting further behind.
const double kVideoDropLateMs = 500.0;
// Hand the clock over to the wall clock when the audio device stops
// advancing for this long while playing.
const double kAudioStallMs = 400.0;

inline void TVPSleepMs(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline int ClampByte(float v)
{
	int i = (int)(v + 0.5f);
	return i < 0 ? 0 : (i > 255 ? 255 : i);
}

//---------------------------------------------------------------------------
// The engine gives the player an IStream (StorageImpl.cpp:tTVPIStreamAdapter
// over a tTJSBinaryStream) and releases its own reference as soon as the
// factory returns, so the player holds a reference of its own and feeds
// libavformat through an AVIOContext reading from it.
//---------------------------------------------------------------------------
struct tTVPStreamSource
{
	IStream *Stream;
	int64_t Size;
};

int TVPIStreamRead(void *opaque, uint8_t *buf, int buf_size)
{
	tTVPStreamSource *src = static_cast<tTVPStreamSource*>(opaque);
	ULONG read = 0;
	HRESULT hr = src->Stream->Read(buf, (ULONG)buf_size, &read);
	if(FAILED(hr)) return AVERROR(EIO);
	// The end of the stream has to be reported as such: a 0 return makes some
	// demuxers (flac, through avio_read_partial) read in circles.
	if(read == 0) return AVERROR_EOF;
	return (int)read;
}

int64_t TVPIStreamSeek(void *opaque, int64_t offset, int whence)
{
	tTVPStreamSource *src = static_cast<tTVPStreamSource*>(opaque);
	// avio passes AVSEEK_FORCE and AVSEEK_SIZE in the upper bits
	if((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) return src->Size;
	LARGE_INTEGER move;
	move.QuadPart = offset;
	ULARGE_INTEGER newpos;
	HRESULT hr = src->Stream->Seek(move, (DWORD)(whence & 0xff), &newpos);
	if(FAILED(hr)) return AVERROR(EIO);
	return (int64_t)newpos.QuadPart;
}

//---------------------------------------------------------------------------
// One input stream's decoder, plus the packets the demuxer has read for it.
//---------------------------------------------------------------------------
enum class tTVPPullResult { Frame, Unavailable, Eof };

struct tTVPDecoderStream
{
	AVStream *Stream = nullptr;
	AVCodecContext *Codec = nullptr;
	std::deque<AVPacket*> Queue;
	bool InputEnded = false;  // a null packet has been sent to the decoder
	bool Drained = false;     // the decoder reported AVERROR_EOF

	bool Active() const { return Codec != nullptr; }

	void ClearQueue()
	{
		for(AVPacket *pkt : Queue) av_packet_free(&pkt);
		Queue.clear();
	}

	void ResetState(bool flush)
	{
		if(flush && Codec) avcodec_flush_buffers(Codec);
		ClearQueue();
		InputEnded = false;
		Drained = false;
	}

	void Close()
	{
		ClearQueue();
		if(Codec) avcodec_free_context(&Codec);
		Stream = nullptr;
		InputEnded = false;
		Drained = false;
	}
};

//---------------------------------------------------------------------------
// Colour adjustment (contrast / brightness / hue / saturation) applied to the
// converted BGRA frame: a 3x3 matrix about mid grey.  The defaults are the
// identity and the whole pass is skipped then.
//---------------------------------------------------------------------------
struct tTVPColorAdjust
{
	float Contrast = 1.0f;    // 0.0 - 2.0, default 1.0
	float Brightness = 0.0f;  // -1.0 - 1.0, default 0.0
	float Hue = 0.0f;         // -180 - 180 degrees, default 0.0
	float Saturation = 1.0f;  // 0.0 - 2.0, default 1.0

	bool IsDefault() const
	{
		return Contrast == 1.0f && Brightness == 0.0f &&
			Hue == 0.0f && Saturation == 1.0f;
	}
};

void TVPBuildColorMatrix(const tTVPColorAdjust &adj, float m[3][3], float &offset)
{
	const float lr = 0.299f, lg = 0.587f, lb = 0.114f;

	// hue rotation about the luma axis
	float hue[3][3];
	const float rad = adj.Hue * 3.14159265358979f / 180.0f;
	const float c = cosf(rad), s = sinf(rad);
	hue[0][0] = lr + c * (1 - lr) - s * lr;
	hue[0][1] = lg - c * lg - s * lg;
	hue[0][2] = lb - c * lb + s * (1 - lb);
	hue[1][0] = lr - c * lr + s * 0.143f;
	hue[1][1] = lg + c * (1 - lg) + s * 0.140f;
	hue[1][2] = lb - c * lb - s * 0.283f;
	hue[2][0] = lr - c * lr - s * (1 - lr);
	hue[2][1] = lg - c * lg + s * lg;
	hue[2][2] = lb + c * (1 - lb) + s * lb;

	// saturation: v + s * (v - luma)
	float sat[3][3];
	const float w[3] = { lr, lg, lb };
	for(int i = 0; i < 3; i++)
		for(int j = 0; j < 3; j++)
			sat[i][j] = w[j] * (1.0f - adj.Saturation) +
				(i == j ? adj.Saturation : 0.0f);

	for(int i = 0; i < 3; i++)
		for(int j = 0; j < 3; j++)
			m[i][j] = hue[i][0] * sat[0][j] + hue[i][1] * sat[1][j] +
				hue[i][2] * sat[2][j];

	// contrast/brightness around mid grey
	for(int i = 0; i < 3; i++)
		for(int j = 0; j < 3; j++)
			m[i][j] *= adj.Contrast;
	offset = 128.0f * (1.0f - adj.Contrast) + adj.Brightness * 255.0f;
}

} // anonymous namespace

//---------------------------------------------------------------------------
// tTVPFFmpegVideoOverlay
//---------------------------------------------------------------------------
class tTVPFFmpegVideoOverlay : public iTVPVideoOverlay
{
	enum class tState { Ready, Playing, Paused, Ended, Stopped };

public:
	tTVPFFmpegVideoOverlay(tTJSNI_VideoOverlay *callbackwin, IStream *stream,
		const tjs_char *streamname, const tjs_char *type, uint64_t size);
	~tTVPFFmpegVideoOverlay();

	// iTVPVideoOverlay
	void AddRef() override { RefCount++; }
	void Release() override;
	void SetWindow(tTJSNI_Window *window) override { OwnerWindow = window; }
	void SetMessageDrainWindow(void *window) override {}
	void SetRect(int l, int t, int r, int b) override
	{
		RectLeft = l; RectTop = t; RectRight = r; RectBottom = b;
	}
	void SetVisible(bool b) override { Visible = b; }
	void Play() override;
	void Stop() override;
	void Pause() override;
	void SetPosition(uint64_t tick) override;
	void GetPosition(uint64_t *tick) override;
	void GetStatus(tTVPVideoStatus *status) override;
	void Rewind() override;
	void SetFrame(int f) override;
	void GetFrame(int *f) override;
	void GetFPS(double *f) override { if(f) *f = FPS; }
	void GetNumberOfFrame(int *f) override { if(f) *f = (int)TotalFrames; }
	void GetTotalTime(int64_t *t) override { if(t) *t = DurationMs; }
	void GetVideoSize(long *width, long *height) override
	{
		if(width) *width = VideoWidth;
		if(height) *height = VideoHeight;
	}
	tTVPBaseTexture *GetFrontBuffer() override;
	void SetVideoBuffer(tTVPBaseTexture *buff1, tTVPBaseTexture *buff2, long size) override;
	void SetStopFrame(int frame) override;
	void GetStopFrame(int *frame) override;
	void SetDefaultStopFrame() override;
	void SetPlayRate(double rate) override;
	void GetPlayRate(double *rate) override { if(rate) *rate = Rate; }
	void SetAudioBalance(long balance) override;
	void GetAudioBalance(long *balance) override;
	void SetAudioVolume(long volume) override;
	void GetAudioVolume(long *volume) override;
	void GetNumberOfAudioStream(unsigned long *streamCount) override
	{
		if(streamCount) *streamCount = (unsigned long)AudioStreams.size();
	}
	void SelectAudioStream(unsigned long num) override;
	void GetEnableAudioStreamNum(long *num) override { if(num) *num = SelectedAudio; }
	void DisableAudioStream(void) override;
	void GetNumberOfVideoStream(unsigned long *streamCount) override
	{
		if(streamCount) *streamCount = (unsigned long)VideoStreams.size();
	}
	void SelectVideoStream(unsigned long num) override;
	void GetEnableVideoStreamNum(long *num) override { if(num) *num = SelectedVideo; }
	void SetMixingBitmap(tTVPBaseTexture *dest, float alpha) override;
	void ResetMixingBitmap() override;
	void SetMixingMovieAlpha(float a) override { MixingMovieAlpha = a; }
	void GetMixingMovieAlpha(float *a) override { if(a) *a = MixingMovieAlpha; }
	void SetMixingMovieBGColor(unsigned long col) override { MixingBGColor = (uint32_t)col; }
	void GetMixingMovieBGColor(unsigned long *col) override { if(col) *col = MixingBGColor; }
	void PresentVideoImage() override;
	void GetContrastRangeMin(float *v) override { if(v) *v = 0.0f; }
	void GetContrastRangeMax(float *v) override { if(v) *v = 2.0f; }
	void GetContrastDefaultValue(float *v) override { if(v) *v = 1.0f; }
	void GetContrastStepSize(float *v) override { if(v) *v = 0.01f; }
	void GetContrast(float *v) override { if(v) *v = Color.Contrast; }
	void SetContrast(float v) override { Color.Contrast = Clamp(v, 0.0f, 2.0f); }
	void GetBrightnessRangeMin(float *v) override { if(v) *v = -1.0f; }
	void GetBrightnessRangeMax(float *v) override { if(v) *v = 1.0f; }
	void GetBrightnessDefaultValue(float *v) override { if(v) *v = 0.0f; }
	void GetBrightnessStepSize(float *v) override { if(v) *v = 0.01f; }
	void GetBrightness(float *v) override { if(v) *v = Color.Brightness; }
	void SetBrightness(float v) override { Color.Brightness = Clamp(v, -1.0f, 1.0f); }
	void GetHueRangeMin(float *v) override { if(v) *v = -180.0f; }
	void GetHueRangeMax(float *v) override { if(v) *v = 180.0f; }
	void GetHueDefaultValue(float *v) override { if(v) *v = 0.0f; }
	void GetHueStepSize(float *v) override { if(v) *v = 1.0f; }
	void GetHue(float *v) override { if(v) *v = Color.Hue; }
	void SetHue(float v) override { Color.Hue = Clamp(v, -180.0f, 180.0f); }
	void GetSaturationRangeMin(float *v) override { if(v) *v = 0.0f; }
	void GetSaturationRangeMax(float *v) override { if(v) *v = 2.0f; }
	void GetSaturationDefaultValue(float *v) override { if(v) *v = 1.0f; }
	void GetSaturationStepSize(float *v) override { if(v) *v = 0.01f; }
	void GetSaturation(float *v) override { if(v) *v = Color.Saturation; }
	void SetSaturation(float v) override { Color.Saturation = Clamp(v, 0.0f, 2.0f); }
	void SetLoopSegement(int beginFrame, int endFrame) override
	{
		// the segment loop itself is driven by VideoOvlImpl (it compares
		// frame with segmentLoopEndFrame and calls SetFrame): remember the
		// range and honour SetFrame(), which is what the engine uses
		std::lock_guard<std::mutex> lk(StateMutex);
		SegLoopBeginFrame = beginFrame;
		SegLoopEndFrame = endFrame;
	}

private:
	static float Clamp(float v, float lo, float hi)
	{
		return v < lo ? lo : (v > hi ? hi : v);
	}

	// ---------------------------------------------------------------- setup
	void OpenStream(const ttstr &streamname);
	void Cleanup();
	bool OpenVideoStream(int fileStreamIndex);
	bool OpenAudioStream(int fileStreamIndex);
	void ReadStreamProperties();
	bool BuildResampler();
	double OutputSampleRate() const
	{
		return Rate * (double)SourceSampleRate;
	}
	void SetupSoundBuffer();
	void TeardownSoundBuffer();

	// ------------------------------------------------------------- decoding
	int DemuxOnePacket();                                            // StateMutex held
	tTVPPullResult PullFrame(tTVPDecoderStream &d, AVFrame *frame);  // StateMutex held
	bool SeekLocked(int64_t ms);                                     // StateMutex held
	void FlushInputLocked(int64_t targetMs);                         // StateMutex held

	// -------------------------------------------------------- worker thread
	void StartWorkerLocked();           // StateMutex held
	void WorkerMain();
	void ServiceAudio();
	bool DecodeAudioFrame();            // StateMutex held
	void AppendQueuedAudio();           // takes StateMutex itself
	double AudioQueuePtsMs();           // StateMutex held
	bool ConvertVideoFrame(const AVFrame *frame, bool hasAlpha);
	void ServiceVideo();
	void CheckAudioClock();             // StateMutex held
	void HandleEndOfStream();           // StateMutex held
	void PostEvent(int message, intptr_t wparam, intptr_t lparam);

	// ------------------------------------------------------------ helpers
	double ClockMs();                   // StateMutex held
	int64_t FrameIndexForPts(int64_t ptsMs) const
	{
		if(ptsMs < 0) ptsMs = 0;
		return (int64_t)llround((double)ptsMs * FPS / 1000.0);
	}
	// the frame SetStopFrame() asked playback to pause at (-1 = none set)
	int RequestedStopFrame() const { return StopFrameValue; }
	// the same value as reported to scripts: unset means the last frame
	int StopFrameIndex() const
	{
		if(StopFrameValue >= 0) return StopFrameValue;
		if(TotalFrames > 0) return (int)(TotalFrames - 1);
		return -1;
	}
	void ApplyColorAdjust();
	bool CopyPendingToFrontLocked();    // FrameMutex held; true if a frame moved
	void CopyStagingToTexture(tTVPBaseTexture *tex);
	void CaptureMixingBackgroundLocked();   // FrameMutex held

	// -------------------------------------------------------------- state
	std::atomic<int> RefCount{ 1 };
	tTJSNI_VideoOverlay *CallbackWin;
	tTJSNI_Window *OwnerWindow = nullptr;

	// input
	tTVPStreamSource Source{ nullptr, 0 };
	AVIOContext *IO = nullptr;
	AVFormatContext *FormatCtx = nullptr;
	bool DemuxEof = false;

	// streams and decoders
	std::vector<int> VideoStreams;
	std::vector<int> AudioStreams;
	int SelectedVideo = 0;   // index into VideoStreams, -1 = none
	int SelectedAudio = 0;   // index into AudioStreams, -1 = disabled
	tTVPDecoderStream Video;
	tTVPDecoderStream Audio;
	AVFrame *DecodedFrame = nullptr;
	AVFrame *AudioFrame = nullptr;
	SwsContext *Sws = nullptr;

	// video presentation
	int VideoWidth = 0;
	int VideoHeight = 0;
	double FPS = 30.0;
	int64_t DurationMs = 0;
	int64_t TotalFrames = 0;
	std::vector<uint8_t> Staging;      // BGRA, written by the decode thread
	int StagingPitch = 0;
	std::mutex FrameMutex;             // guards Staging/HasPending/front buffer
	bool HasPending = false;
	int64_t PendingPtsMs = 0;
	int PendingFrameIndex = 0;
	tTVPBaseTexture *Buffers[2] = { nullptr, nullptr };
	bool OwnBuffers = false;
	int FrontIndex = 0;
	bool FrontValid = false;

	// audio
	iTVPSoundBuffer *Sound = nullptr;
	tTVPWaveFormat SoundFormat{};
	int SourceSampleRate = 48000;
	int SourceChannels = 2;
	SwrContext *Swr = nullptr;
	struct tAudioChunk
	{
		std::vector<uint8_t> Data;
		int64_t PtsMs = 0;
	};
	std::deque<tAudioChunk> AudioQueue;
	size_t AudioQueueOffset = 0;    // consumed bytes in AudioQueue.front()
	bool AudioInputEnded = false;   // decoder drained and resampler flushed
	int64_t DiscardAudioUntilMs = -1;
	bool AudioAnchorSet = false;
	double AudioAnchorMs = 0;
	bool AudioClockTrusted = false;
	bool AudioClockRejected = false;   // sticky until the next play/seek
	tjs_uint AudioLastSamples = 0;
	tjs_uint64 AudioLastAdvanceTick = 0;

	// controls
	// The rect and the visible flag are the overlay-mode presentation state the
	// engine sets (SetRectangleToVideoOverlay()/SetVisible()); this port shows
	// the movie through the engine's own textures (layer mode) or the mixing
	// bitmap (mixer mode) instead of a window surface, so there is nothing to
	// position or hide here - see the diagnostic in ServiceVideo().
	bool Visible = false;
	int RectLeft = 0, RectTop = 0, RectRight = 320, RectBottom = 240;
	bool WarnedNoSurface = false;
	std::atomic<bool> MixingAttached{ false };
	tTVPBaseTexture *MixingBitmap = nullptr;
	// the mixing target's content when the movie was attached to it: the movie
	// is blended over this every frame, so a half-transparent movie does not
	// accumulate over its own previous output
	std::vector<uint32_t> MixingBackground;
	int MixingBackgroundW = 0;
	int MixingBackgroundH = 0;
	float MixingAlpha = 1.0f;
	float MixingMovieAlpha = 1.0f;
	uint32_t MixingBGColor = 0xff000000;
	long AudioVolumeValue = 100000;
	long AudioBalanceValue = 0;
	double Rate = 1.0;
	int StopFrameValue = -1;
	int SegLoopBeginFrame = -1;
	int SegLoopEndFrame = -1;
	tTVPColorAdjust Color;

	// playback state and clock
	std::mutex StateMutex;
	std::condition_variable StateCond;
	tState State_ = tState::Ready;
	bool StopRequested = false;
	double WallAnchorMs = 0;
	tjs_uint64 WallAnchorTick = 0;
	int64_t LastPositionMs = 0;
	int LastFrameIndex = 0;
	bool HaveDecodedVideo = false;
	double PendingVideoDueMs = 0;
	int64_t DiscardUntilMs = -1;
	int64_t LastVideoPtsMs = -1;
	bool Completed = false;
	bool EofSeen = false;
	tjs_uint64 EofTick = 0;
	std::thread Worker;

	// Event posting: PostMutex/CanPost guarantee that no post from the decode
	// thread is in flight after Pause()/Stop() returns (the engine pauses the
	// overlay and may drop it right after).
	std::mutex PostMutex;
	bool CanPost = true;
};

//---------------------------------------------------------------------------
// construction / teardown
//---------------------------------------------------------------------------
tTVPFFmpegVideoOverlay::tTVPFFmpegVideoOverlay(tTJSNI_VideoOverlay *callbackwin,
	IStream *stream, const tjs_char *streamname, const tjs_char *type, uint64_t size)
{
	CallbackWin = callbackwin;
	Source.Stream = stream;
	Source.Size = (int64_t)size;
	if(Source.Stream) Source.Stream->AddRef();

	ttstr name(streamname ? streamname : TJS_W(""));
	(void)type;
	try
	{
		OpenStream(name);
	}
	catch(...)
	{
		// a destructor does not run when a constructor throws
		Cleanup();
		throw;
	}
}

//---------------------------------------------------------------------------
tTVPFFmpegVideoOverlay::~tTVPFFmpegVideoOverlay()
{
	{
		std::lock_guard<std::mutex> lk(StateMutex);
		StopRequested = true;
		State_ = tState::Stopped;
	}
	StateCond.notify_all();
	if(Worker.joinable()) Worker.join();
	{
		std::lock_guard<std::mutex> lk(PostMutex);
		CanPost = false;
	}
	Cleanup();
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::Release()
{
	if(--RefCount == 0) delete this;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::Cleanup()
{
	TeardownSoundBuffer();
	AudioQueue.clear();
	AudioQueueOffset = 0;
	if(Swr) swr_free(&Swr);
	if(Sws) sws_freeContext(Sws), Sws = nullptr;
	if(DecodedFrame) av_frame_free(&DecodedFrame);
	if(AudioFrame) av_frame_free(&AudioFrame);
	Video.Close();
	Audio.Close();
	if(FormatCtx) avformat_close_input(&FormatCtx);
	if(IO)
	{
		// the AVIOContext owns the buffer it was created with
		av_freep(&IO->buffer);
		avio_context_free(&IO);
		IO = nullptr;
	}
	if(OwnBuffers)
	{
		for(int i = 0; i < 2; i++)
			if(Buffers[i]) delete Buffers[i], Buffers[i] = nullptr;
		OwnBuffers = false;
	}
	Buffers[0] = Buffers[1] = nullptr;
	if(Source.Stream) Source.Stream->Release(), Source.Stream = nullptr;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::OpenStream(const ttstr &streamname)
{
	// ---- input: an AVIOContext over the IStream the engine handed over ----
	const int bufSize = 32 * 1024;
	unsigned char *iobuf = (unsigned char*)av_malloc(bufSize + AVPROBE_PADDING_SIZE);
	if(!iobuf) TVPThrowExceptionMessage(TVPErrorInKrMovieDLL, ttstr(TJS_W("out of memory")));
	AVIOContext *io = avio_alloc_context(iobuf, bufSize, 0, &Source,
		TVPIStreamRead, nullptr, TVPIStreamSeek);
	if(!io)
	{
		av_free(iobuf);
		TVPThrowExceptionMessage(TVPErrorInKrMovieDLL,
			ttstr(TJS_W("cannot allocate the input context")));
	}
	IO = io;

	tTJSNarrowStringHolder holder(streamname.c_str());
	const AVInputFormat *fmt = nullptr;
	if(av_probe_input_buffer2(IO, &fmt, holder.operator const tjs_nchar *(),
			nullptr, 0, 0) < 0)
		fmt = nullptr;   // avformat_open_input() probes again

	AVFormatContext *ic = avformat_alloc_context();
	if(!ic) TVPThrowExceptionMessage(TVPErrorInKrMovieDLL, ttstr(TJS_W("out of memory")));
	ic->pb = IO;
	if(avformat_open_input(&ic, "", fmt, nullptr) < 0)
	{
		FormatCtx = nullptr;
		TVPThrowExceptionMessage(TVPErrorInKrMovieDLL,
			ttstr(TJS_W("cannot open movie ")) + streamname);
	}
	FormatCtx = ic;
	if(avformat_find_stream_info(ic, nullptr) < 0)
		TVPThrowExceptionMessage(TVPErrorInKrMovieDLL,
			ttstr(TJS_W("cannot read the streams of ")) + streamname);
	if(ic->pb) ic->pb->eof_reached = 0;

	for(unsigned int i = 0; i < ic->nb_streams; i++)
	{
		const AVMediaType t = ic->streams[i]->codecpar->codec_type;
		if(t == AVMEDIA_TYPE_VIDEO)
			VideoStreams.push_back((int)i);
		else if(t == AVMEDIA_TYPE_AUDIO)
			AudioStreams.push_back((int)i);
	}

	if(!VideoStreams.empty())
	{
		if(!OpenVideoStream(VideoStreams[0]))
			TVPThrowExceptionMessage(TVPErrorInKrMovieDLL,
				ttstr(TJS_W("no decoder for the video stream of ")) + streamname);
	}
	else
	{
		SelectedVideo = -1;
	}

	if(!AudioStreams.empty())
	{
		if(!OpenAudioStream(AudioStreams[0]))
			SelectedAudio = -1;
	}
	else
	{
		SelectedAudio = -1;
	}

	if(VideoWidth <= 0 || VideoHeight <= 0)
		TVPThrowExceptionMessage(TVPErrorInKrMovieDLL,
			ttstr(TJS_W("movie has no usable video stream: ")) + streamname);

	// the decoder converts into this staging buffer; the textures the engine
	// renders are only written from the main thread
	StagingPitch = VideoWidth * 4;
	Staging.resize((size_t)StagingPitch * (size_t)VideoHeight);
	DecodedFrame = av_frame_alloc();
	AudioFrame = av_frame_alloc();
	if(!DecodedFrame || !AudioFrame)
		TVPThrowExceptionMessage(TVPErrorInKrMovieDLL, ttstr(TJS_W("out of memory")));

	// textures for the modes in which the engine supplies none (mixer/MFEVR);
	// SetVideoBuffer() replaces these in layer mode
	for(int i = 0; i < 2; i++)
	{
		Buffers[i] = new tTVPBaseTexture(VideoWidth, VideoHeight, 32);
		iTVPTexture2D *tex = Buffers[i]->GetTexture();
		memset(tex->GetScanLineForWrite(0), 0,
			(size_t)tex->GetPitch() * (size_t)VideoHeight);
	}
	OwnBuffers = true;
	FrontValid = true;

	char fpsbuf[32];
	snprintf(fpsbuf, sizeof(fpsbuf), "%.3f", FPS);
	TVPAddImportantLog(ttstr(TJS_W("(info) movie: ")) + streamname +
		TJS_W(" ") + ttstr(VideoWidth) + TJS_W("x") + ttstr(VideoHeight) +
		TJS_W(" ") + ttstr(std::string(fpsbuf)) + TJS_W("fps ") +
		ttstr((int)DurationMs) + TJS_W("ms, video streams=") +
		ttstr((int)VideoStreams.size()) + TJS_W(", audio streams=") +
		ttstr((int)AudioStreams.size()) +
		(Audio.Active() ? (TJS_W(", audio=") + ttstr(SourceSampleRate) +
			TJS_W("Hz ") + ttstr(SourceChannels) + TJS_W("ch")) : ttstr(TJS_W(""))));
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::ReadStreamProperties()
{
	AVStream *vs = Video.Stream;
	AVRational fr = vs->avg_frame_rate;
	if(fr.num <= 0 || fr.den <= 0) fr = vs->r_frame_rate;
	if(fr.num > 0 && fr.den > 0 && av_q2d(fr) > 0.001)
		FPS = av_q2d(fr);
	else
		FPS = 30.0;

	VideoWidth = vs->codecpar->width;
	VideoHeight = vs->codecpar->height;

	const int64_t streamDur = vs->duration;
	if(streamDur != AV_NOPTS_VALUE && streamDur > 0)
	{
		// both are in the stream's own time base
		DurationMs = av_rescale_q(streamDur, vs->time_base, TVPMsTimeBase);
	}
	else if(FormatCtx->duration != AV_NOPTS_VALUE && FormatCtx->duration > 0)
	{
		// the container duration is in AV_TIME_BASE units
		DurationMs = av_rescale_q(FormatCtx->duration, AV_TIME_BASE_Q, TVPMsTimeBase);
	}
	else
	{
		DurationMs = 0;
	}

	if(vs->nb_frames > 0)
		TotalFrames = vs->nb_frames;
	else if(DurationMs > 0)
		TotalFrames = (int64_t)llround((double)DurationMs * FPS / 1000.0);
	else
		TotalFrames = 0;
}

//---------------------------------------------------------------------------
bool tTVPFFmpegVideoOverlay::OpenVideoStream(int fileStreamIndex)
{
	AVStream *st = FormatCtx->streams[fileStreamIndex];
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	if(!codec) return false;
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if(!ctx) return false;
	if(avcodec_parameters_to_context(ctx, st->codecpar) < 0)
	{
		avcodec_free_context(&ctx);
		return false;
	}
	// decoded frames report their timestamps in this time base
	ctx->pkt_timebase = st->time_base;
	if(avcodec_open2(ctx, codec, nullptr) < 0)
	{
		avcodec_free_context(&ctx);
		return false;
	}
	Video.Stream = st;
	Video.Codec = ctx;
	Video.ResetState(false);
	ReadStreamProperties();
	return true;
}

//---------------------------------------------------------------------------
bool tTVPFFmpegVideoOverlay::BuildResampler()
{
	if(!Audio.Codec) return false;
	AVChannelLayout outLayout;
	av_channel_layout_default(&outLayout, SourceChannels);
	SwrContext *swr = nullptr;
	int err = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16,
		(int)(OutputSampleRate() + 0.5), &Audio.Codec->ch_layout,
		Audio.Codec->sample_fmt, Audio.Codec->sample_rate, 0, nullptr);
	av_channel_layout_uninit(&outLayout);
	if(err < 0 || !swr) return false;
	if(swr_init(swr) < 0)
	{
		swr_free(&swr);
		return false;
	}
	if(Swr) swr_free(&Swr);
	Swr = swr;
	return true;
}

//---------------------------------------------------------------------------
bool tTVPFFmpegVideoOverlay::OpenAudioStream(int fileStreamIndex)
{
	AVStream *st = FormatCtx->streams[fileStreamIndex];
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	if(!codec) return false;
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if(!ctx) return false;
	if(avcodec_parameters_to_context(ctx, st->codecpar) < 0)
	{
		avcodec_free_context(&ctx);
		return false;
	}
	ctx->pkt_timebase = st->time_base;
	if(avcodec_open2(ctx, codec, nullptr) < 0)
	{
		avcodec_free_context(&ctx);
		return false;
	}
	Audio.Stream = st;
	Audio.Codec = ctx;
	Audio.ResetState(false);
	SourceSampleRate = ctx->sample_rate > 0 ? ctx->sample_rate : 44100;
	SourceChannels = ctx->ch_layout.nb_channels;
	if(SourceChannels < 1) SourceChannels = 2;
	if(SourceChannels > 2) SourceChannels = 2;   // downmixed by libswresample
	if(!BuildResampler()) return false;
	SetupSoundBuffer();
	return true;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::SetupSoundBuffer()
{
	TeardownSoundBuffer();
	if(!Audio.Codec || Swr == nullptr) return;
	memset(&SoundFormat, 0, sizeof(SoundFormat));
	SoundFormat.SamplesPerSec = (tjs_uint)(OutputSampleRate() + 0.5);
	SoundFormat.Channels = (tjs_uint)SourceChannels;
	SoundFormat.BitsPerSample = 16;
	SoundFormat.BytesPerSample = 2;
	SoundFormat.IsFloat = false;
	SoundFormat.Seekable = true;
	SoundFormat.SpeakerConfig = 0;
	if(Audio.Stream->duration != AV_NOPTS_VALUE && Audio.Stream->duration > 0)
	{
		SoundFormat.TotalSamples = (tjs_uint64)av_rescale_q(Audio.Stream->duration,
			Audio.Stream->time_base, AVRational{ 1, (int)(OutputSampleRate() + 0.5) });
		SoundFormat.TotalTime = (tjs_uint64)av_rescale_q(Audio.Stream->duration,
			Audio.Stream->time_base, TVPMsTimeBase);
	}
	try
	{
		// the mixer is lazily created by the wave player; a game that plays a
		// movie before any sound would otherwise have no audio renderer yet
		TVPInitDirectSound();
		Sound = TVPCreateSoundBuffer(SoundFormat, kAudioBufferChunks);
	}
	catch(...)
	{
		Sound = nullptr;   // no usable audio device: the movie plays without it
	}
	if(Sound)
	{
		Sound->SetVolume(AudioVolumeValue / 100000.0f);
		Sound->SetPan(AudioBalanceValue / 100000.0f);
	}
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::TeardownSoundBuffer()
{
	if(Sound)
	{
		Sound->Stop();
		Sound->Release();
		Sound = nullptr;
	}
}

//---------------------------------------------------------------------------
// demuxing / decoding (all with StateMutex held unless noted)
//---------------------------------------------------------------------------
int tTVPFFmpegVideoOverlay::DemuxOnePacket()
{
	if(DemuxEof) return 0;
	AVPacket *pkt = av_packet_alloc();
	if(!pkt) return 0;
	int ret = av_read_frame(FormatCtx, pkt);
	if(ret < 0)
	{
		av_packet_free(&pkt);
		DemuxEof = true;   // end of the file, or a read error: no more input
		return 0;
	}
	if(Video.Stream && pkt->stream_index == Video.Stream->index)
		Video.Queue.push_back(pkt);
	else if(Audio.Stream && pkt->stream_index == Audio.Stream->index)
		Audio.Queue.push_back(pkt);
	else
		av_packet_free(&pkt);
	return 1;
}

//---------------------------------------------------------------------------
tTVPPullResult tTVPFFmpegVideoOverlay::PullFrame(tTVPDecoderStream &d, AVFrame *frame)
{
	if(!d.Active()) return tTVPPullResult::Eof;
	for(;;)
	{
		int ret = avcodec_receive_frame(d.Codec, frame);
		if(ret == 0) return tTVPPullResult::Frame;
		if(ret == AVERROR_EOF)
		{
			d.Drained = true;
			return tTVPPullResult::Eof;
		}
		if(ret != AVERROR(EAGAIN))
		{
			// a damaged frame: skip it instead of stopping the movie
			av_frame_unref(frame);
			continue;
		}

		if(!d.Queue.empty())
		{
			AVPacket *pkt = d.Queue.front();
			int sret = avcodec_send_packet(d.Codec, pkt);
			if(sret == AVERROR(EAGAIN)) return tTVPPullResult::Unavailable;
			d.Queue.pop_front();
			av_packet_free(&pkt);
			continue;
		}

		if(!d.InputEnded)
		{
			if(DemuxOnePacket() == 0)
			{
				avcodec_send_packet(d.Codec, nullptr);
				d.InputEnded = true;
			}
			continue;
		}
		return tTVPPullResult::Unavailable;
	}
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::FlushInputLocked(int64_t targetMs)
{
	Video.ResetState(true);
	Audio.ResetState(true);
	DemuxEof = false;
	Completed = false;
	EofSeen = false;
	EofTick = 0;
	AudioInputEnded = false;
	AudioAnchorSet = false;
	AudioClockTrusted = false;
	AudioClockRejected = false;
	AudioLastAdvanceTick = 0;
	AudioLastSamples = 0;
	AudioQueue.clear();
	AudioQueueOffset = 0;
	DiscardUntilMs = targetMs;
	DiscardAudioUntilMs = targetMs;
	LastVideoPtsMs = -1;
	HaveDecodedVideo = false;
	if(Sound)
	{
		Sound->Stop();
		Sound->Reset();
	}
}

//---------------------------------------------------------------------------
bool tTVPFFmpegVideoOverlay::SeekLocked(int64_t ms)
{
	AVStream *st = Video.Stream ? Video.Stream : Audio.Stream;
	if(!st || !FormatCtx) return false;
	if(ms < 0) ms = 0;
	int64_t ts = av_rescale_q(ms, TVPMsTimeBase, st->time_base);
	if(st->start_time != AV_NOPTS_VALUE) ts += st->start_time;
	if(av_seek_frame(FormatCtx, st->index, ts, AVSEEK_FLAG_BACKWARD) < 0)
		return false;
	FlushInputLocked(ms);
	LastPositionMs = ms;
	WallAnchorMs = (double)ms;
	WallAnchorTick = TVPGetTickCount();
	LastFrameIndex = (int)FrameIndexForPts(ms);
	return true;
}

//---------------------------------------------------------------------------
double tTVPFFmpegVideoOverlay::ClockMs()
{
	if(AudioClockTrusted && Sound)
	{
		return AudioAnchorMs +
			(double)Sound->GetCurrentPlaySamples() * 1000.0 / OutputSampleRate();
	}
	double ms = WallAnchorMs;
	if(State_ == tState::Playing)
		ms += (double)(TVPGetTickCount() - WallAnchorTick) * Rate;
	return ms;
}

//---------------------------------------------------------------------------
// worker thread
//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::WorkerMain()
{
	for(;;)
	{
		{
			std::unique_lock<std::mutex> lk(StateMutex);
			if(StopRequested) return;
			if(State_ != tState::Playing)
			{
				StateCond.wait_for(lk, std::chrono::milliseconds(20));
				continue;
			}
			if(Completed)
			{
				StateCond.wait_for(lk, std::chrono::milliseconds(20));
				continue;
			}
			CheckAudioClock();
			HandleEndOfStream();
		}

		ServiceAudio();
		ServiceVideo();
		// one pass per ms: the audio queue is topped up while a video frame
		// waits for its due time
		TVPSleepMs(1);
	}
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::CheckAudioClock()
{
	if(!Sound) return;
	tjs_uint samples = Sound->GetCurrentPlaySamples();
	if(!AudioClockTrusted)
	{
		// Believe the device once it has consumed something.  Until then (no
		// device, or a device that has not started) the wall clock runs the
		// movie, which keeps video-only playback going on such a machine.  A
		// device that stalled once is not trusted again until the next
		// play/seek: its sample counter would freeze the clock again.
		if(AudioAnchorSet && !AudioClockRejected && samples > 0)
		{
			AudioClockTrusted = true;
			AudioLastSamples = samples;
			AudioLastAdvanceTick = TVPGetTickCount();
		}
		return;
	}
	if(samples != AudioLastSamples)
	{
		AudioLastSamples = samples;
		AudioLastAdvanceTick = TVPGetTickCount();
		return;
	}
	if(TVPGetTickCount() - AudioLastAdvanceTick < (tjs_uint64)kAudioStallMs) return;
	// the device ran dry (end of the audio, or a failure): carry on from where
	// the audio stopped, on the wall clock
	WallAnchorMs = AudioAnchorMs +
		(double)samples * 1000.0 / OutputSampleRate();
	WallAnchorTick = TVPGetTickCount();
	AudioClockTrusted = false;
	AudioClockRejected = true;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::HandleEndOfStream()
{
	if(Completed || !EofSeen) return;
	if(Audio.Active() && !AudioInputEnded) return;
	if(!AudioQueue.empty()) return;
	if(!EofTick) EofTick = TVPGetTickCount();
	// wait for the clock to reach the end of the movie (the audio still has to
	// drain through the mixer), with a bound in case it never does
	if(DurationMs > 0 && ClockMs() < (double)DurationMs &&
		(TVPGetTickCount() - EofTick) < 3000) return;

	Completed = true;
	State_ = tState::Ended;
	if(DurationMs > 0) LastPositionMs = DurationMs;
	PostEvent(WM_GRAPHNOTIFY, EC_COMPLETE, 0);
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::ServiceAudio()
{
	if(!Audio.Active() || AudioInputEnded) return;

	AppendQueuedAudio();

	// stop decoding ahead once enough is buffered
	float queuedSecs;
	{
		std::lock_guard<std::mutex> lk(StateMutex);
		size_t bytes = 0;
		for(size_t i = 0; i < AudioQueue.size(); i++)
			bytes += AudioQueue[i].Data.size();
		if(!AudioQueue.empty()) bytes -= AudioQueueOffset;
		queuedSecs = (float)bytes /
			(float)(SourceChannels * 2 * (int)(OutputSampleRate() + 0.5));
		if(Sound && Sound->GetRemainBuffers() >= kAudioBufferChunks) return;
		if(queuedSecs > (float)kMaxAudioQueuedSecs) return;
		if(StopRequested || State_ != tState::Playing) return;
		const bool got = DecodeAudioFrame();
		if(got) return;
		if(!Audio.Drained) return;   // no more audio in this file

		// end of the audio stream: drain what the resampler still holds
		int maxOut = swr_get_out_samples(Swr, 0);
		if(maxOut > 0)
		{
			tAudioChunk chunk;
			chunk.Data.resize((size_t)maxOut * SourceChannels * 2);
			uint8_t *outPtr = chunk.Data.data();
			int got2 = swr_convert(Swr, &outPtr, maxOut, nullptr, 0);
			if(got2 > 0)
			{
				chunk.Data.resize((size_t)got2 * SourceChannels * 2);
				chunk.PtsMs = AudioQueuePtsMs();
				AudioQueue.push_back(std::move(chunk));
				return;
			}
		}
		AudioInputEnded = true;
	}
}

//---------------------------------------------------------------------------
// media time of the end of the queued audio (StateMutex held)
//---------------------------------------------------------------------------
double tTVPFFmpegVideoOverlay::AudioQueuePtsMs()
{
	if(AudioQueue.empty()) return (double)LastVideoPtsMs;
	const tAudioChunk &back = AudioQueue.back();
	return (double)back.PtsMs + (double)back.Data.size() /
		(double)(SourceChannels * 2) * 1000.0 / OutputSampleRate();
}

//---------------------------------------------------------------------------
// Decodes one audio frame and appends its resampled PCM to the queue.
// StateMutex held by the caller.
//---------------------------------------------------------------------------
bool tTVPFFmpegVideoOverlay::DecodeAudioFrame()
{
	if(!Audio.Active()) return false;
	av_frame_unref(AudioFrame);
	tTVPPullResult res = PullFrame(Audio, AudioFrame);
	if(res != tTVPPullResult::Frame) return false;
	if(AudioFrame->nb_samples <= 0) return true;

	int64_t ptsMs;
	int64_t ts = AudioFrame->best_effort_timestamp;
	if(ts != AV_NOPTS_VALUE)
	{
		if(Audio.Stream->start_time != AV_NOPTS_VALUE)
			ts -= Audio.Stream->start_time;
		ptsMs = av_rescale_q(ts, Audio.Stream->time_base, TVPMsTimeBase);
	}
	else
	{
		ptsMs = (int64_t)AudioQueuePtsMs();
	}

	int maxOut = swr_get_out_samples(Swr, AudioFrame->nb_samples);
	if(maxOut <= 0) maxOut = AudioFrame->nb_samples + 64;
	tAudioChunk chunk;
	chunk.Data.resize((size_t)maxOut * SourceChannels * 2);
	uint8_t *outPtr = chunk.Data.data();
	int got = swr_convert(Swr, &outPtr, maxOut,
		(const uint8_t **)AudioFrame->extended_data, AudioFrame->nb_samples);
	if(got < 0) return true;
	chunk.Data.resize((size_t)got * SourceChannels * 2);
	chunk.PtsMs = ptsMs;
	if(chunk.Data.empty()) return true;

	if(DiscardAudioUntilMs >= 0)
	{
		const size_t frameBytes = (size_t)SourceChannels * 2;
		const double chunkMs = (double)(chunk.Data.size() / frameBytes) * 1000.0 /
			OutputSampleRate();
		if((double)ptsMs + chunkMs <= (double)DiscardAudioUntilMs) return true;
		if((double)ptsMs < (double)DiscardAudioUntilMs)
		{
			// the chunk straddles the target: start it at the target
			size_t skip = (size_t)(((double)DiscardAudioUntilMs - (double)ptsMs) *
				OutputSampleRate() / 1000.0) * frameBytes;
			if(skip >= chunk.Data.size()) return true;
			chunk.Data.erase(chunk.Data.begin(), chunk.Data.begin() + skip);
			chunk.PtsMs = DiscardAudioUntilMs;
		}
		DiscardAudioUntilMs = -1;
	}
	AudioQueue.push_back(std::move(chunk));
	return true;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::AppendQueuedAudio()
{
	if(!Sound) return;
	const size_t frameBytes = (size_t)SourceChannels * 2;
	std::lock_guard<std::mutex> lk(StateMutex);
	if(StopRequested || State_ != tState::Playing) return;
	bool appended = false;
	while(!AudioQueue.empty())
	{
		if(Sound->GetRemainBuffers() >= kAudioBufferChunks) return;
		tAudioChunk &chunk = AudioQueue.front();
		if(AudioQueueOffset >= chunk.Data.size())
		{
			AudioQueue.pop_front();
			AudioQueueOffset = 0;
			continue;
		}
		size_t avail = chunk.Data.size() - AudioQueueOffset;
		size_t take = std::min(avail, (size_t)kAudioChunkFrames * frameBytes);
		if(!AudioAnchorSet)
		{
			AudioAnchorMs = (double)chunk.PtsMs +
				(double)AudioQueueOffset / (double)frameBytes * 1000.0 /
				OutputSampleRate();
			AudioAnchorSet = true;
		}
		Sound->AppendBuffer(chunk.Data.data() + AudioQueueOffset, (unsigned int)take);
		AudioQueueOffset += take;
		appended = true;
	}
	// the mixer stream has to be (re)started here: an OpenAL source whose queue
	// ran empty stops, and queuing more buffers alone does not restart it
	if(appended) Sound->Play();
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::ServiceVideo()
{
	std::unique_lock<std::mutex> lk(StateMutex);
	if(StopRequested || State_ != tState::Playing) return;
	if(!Video.Active() || Video.Drained) return;   // end of the video (EofSeen)

	if(!HaveDecodedVideo)
	{
		av_frame_unref(DecodedFrame);
		const tTVPPullResult res = PullFrame(Video, DecodedFrame);
		if(res != tTVPPullResult::Frame)
		{
			if(res == tTVPPullResult::Eof) EofSeen = true;
			return;
		}

		int64_t ts = DecodedFrame->best_effort_timestamp;
		int64_t ptsMs;
		if(ts != AV_NOPTS_VALUE)
		{
			if(Video.Stream->start_time != AV_NOPTS_VALUE)
				ts -= Video.Stream->start_time;
			ptsMs = av_rescale_q(ts, Video.Stream->time_base, TVPMsTimeBase);
		}
		else
		{
			ptsMs = LastVideoPtsMs < 0 ? 0 :
				LastVideoPtsMs + (int64_t)llround(1000.0 / FPS);
		}
		LastVideoPtsMs = ptsMs;

		if(DiscardUntilMs >= 0 && ptsMs < DiscardUntilMs)
		{
			// after a seek: these frames only rebuild the reference frames
			return;
		}
		DiscardUntilMs = -1;
		HaveDecodedVideo = true;
		PendingVideoDueMs = (double)ptsMs - kVideoLookaheadMs;
		PendingPtsMs = ptsMs;
	}

	const double ptsMs = (double)PendingPtsMs;

	// the frame stays here until it is due: the worker loop keeps servicing
	// the audio in between, so a long wait cannot starve the sound device
	if(ClockMs() < PendingVideoDueMs) return;

	const bool tooLate = ClockMs() - ptsMs > kVideoDropLateMs;
	if(tooLate)
	{
		// dropped: keep decoding so the picture catches up with the clock
		HaveDecodedVideo = false;
		return;
	}

	const AVPixFmtDescriptor *desc =
		av_pix_fmt_desc_get((AVPixelFormat)DecodedFrame->format);
	const bool hasAlpha = desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
	if(!ConvertVideoFrame(DecodedFrame, hasAlpha))
	{
		HaveDecodedVideo = false;
		return;
	}
	HaveDecodedVideo = false;

	const int frameIndex = (int)FrameIndexForPts((int64_t)ptsMs);
	{
		std::lock_guard<std::mutex> flk(FrameMutex);
		HasPending = true;
		PendingPtsMs = (int64_t)ptsMs;
		PendingFrameIndex = frameIndex;
	}
	LastPositionMs = (int64_t)ptsMs;
	LastFrameIndex = frameIndex;
	// only an explicitly requested stop frame pauses playback; by default the
	// movie runs out to the end of the stream, which is what makes the engine
	// fire EC_COMPLETE (and loop, when the script asked for it)
	const bool reachedStopFrame =
		RequestedStopFrame() >= 0 && frameIndex >= RequestedStopFrame();
	if(reachedStopFrame)
	{
		State_ = tState::Paused;
		WallAnchorMs = ptsMs;
		if(Sound) Sound->Pause();
	}
	lk.unlock();

	if(!WarnedNoSurface)
	{
		WarnedNoSurface = true;
		if(OwnerWindow && OwnBuffers && !MixingAttached)
			TVPAddImportantLog(TJS_W("(info) movie: this overlay mode has no ")
				TJS_W("on-screen surface in this port; set mode = 1 (layer) or ")
				TJS_W("2 (mixer) to display the video"));
	}

	// VideoOvlImpl's WndProc handles this: in layer mode it takes
	// GetFrontBuffer() into the layer, in mixer mode it calls
	// PresentVideoImage(); both also fire the script's frame events
	PostEvent(WM_GRAPHNOTIFY, EC_UPDATE, (intptr_t)frameIndex);
	if(reachedStopFrame) StateCond.notify_all();
}

//---------------------------------------------------------------------------
// frame conversion and presentation
//---------------------------------------------------------------------------
bool tTVPFFmpegVideoOverlay::ConvertVideoFrame(const AVFrame *frame, bool hasAlpha)
{
	// FrameMutex: the engine copies this buffer into its texture inside
	// GetFrontBuffer()/PresentVideoImage() (main thread), so the conversion
	// must not run while that copy is in flight - this is what keeps a
	// half-written frame from ever being handed out.  Lock order is always
	// StateMutex before FrameMutex; nothing takes them the other way round.
	std::lock_guard<std::mutex> flk(FrameMutex);
	Sws = sws_getCachedContext(Sws, frame->width, frame->height,
		(AVPixelFormat)frame->format, VideoWidth, VideoHeight, AV_PIX_FMT_BGRA,
		SWS_BILINEAR, nullptr, nullptr, nullptr);
	if(!Sws) return false;
	uint8_t *dstData[4] = { Staging.data(), nullptr, nullptr, nullptr };
	int dstLine[4] = { StagingPitch, 0, 0, 0 };
	sws_scale(Sws, frame->data, frame->linesize, 0, frame->height, dstData, dstLine);

	if(hasAlpha)
	{
		// composite the video's own alpha over the movie background colour, so
		// the engine always receives an opaque frame
		const uint8_t bgB = (uint8_t)(MixingBGColor & 0xff);
		const uint8_t bgG = (uint8_t)((MixingBGColor >> 8) & 0xff);
		const uint8_t bgR = (uint8_t)((MixingBGColor >> 16) & 0xff);
		for(int y = 0; y < VideoHeight; y++)
		{
			uint8_t *p = Staging.data() + (size_t)y * StagingPitch;
			for(int x = 0; x < VideoWidth; x++, p += 4)
			{
				const int a = p[3];
				if(a == 255) continue;
				p[0] = (uint8_t)((p[0] * a + bgB * (255 - a)) / 255);
				p[1] = (uint8_t)((p[1] * a + bgG * (255 - a)) / 255);
				p[2] = (uint8_t)((p[2] * a + bgR * (255 - a)) / 255);
				p[3] = 255;
			}
		}
	}

	ApplyColorAdjust();
	return true;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::ApplyColorAdjust()
{
	if(Color.IsDefault()) return;
	float m[3][3];
	float offset;
	TVPBuildColorMatrix(Color, m, offset);
	for(int y = 0; y < VideoHeight; y++)
	{
		uint8_t *p = Staging.data() + (size_t)y * StagingPitch;
		for(int x = 0; x < VideoWidth; x++, p += 4)
		{
			const float b = p[0], g = p[1], r = p[2];
			p[0] = (uint8_t)ClampByte(m[2][0] * r + m[2][1] * g + m[2][2] * b + offset);
			p[1] = (uint8_t)ClampByte(m[1][0] * r + m[1][1] * g + m[1][2] * b + offset);
			p[2] = (uint8_t)ClampByte(m[0][0] * r + m[0][1] * g + m[0][2] * b + offset);
		}
	}
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::CopyStagingToTexture(tTVPBaseTexture *tex)
{
	iTVPTexture2D *t = tex->GetTexture();
	if(!t) return;
	const int pitch = t->GetPitch();
	const int rows = std::min<int>(VideoHeight, (int)t->GetHeight());
	const int cols = std::min<int>(VideoWidth, (int)(t->GetWidth()));
	uint8_t *dst = (uint8_t*)t->GetScanLineForWrite(0);
	const uint8_t *src = Staging.data();
	for(int y = 0; y < rows; y++, dst += pitch, src += StagingPitch)
		memcpy(dst, src, (size_t)cols * 4);
}

//---------------------------------------------------------------------------
bool tTVPFFmpegVideoOverlay::CopyPendingToFrontLocked()
{
	if(!HasPending || !Buffers[0] || !Buffers[1] || StagingPitch <= 0) return false;
	// write into the buffer the engine is not showing, then hand it out
	FrontIndex ^= 1;
	CopyStagingToTexture(Buffers[FrontIndex]);
	FrontValid = true;
	HasPending = false;
	return true;
}

//---------------------------------------------------------------------------
tTVPBaseTexture *tTVPFFmpegVideoOverlay::GetFrontBuffer()
{
	std::lock_guard<std::mutex> lk(FrameMutex);
	CopyPendingToFrontLocked();
	if(Buffers[0] && Buffers[1]) return Buffers[FrontIndex];
	return Buffers[0] ? Buffers[0] : Buffers[1];
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::SetVideoBuffer(tTVPBaseTexture *buff1,
	tTVPBaseTexture *buff2, long size)
{
	std::lock_guard<std::mutex> flk(FrameMutex);
	if(OwnBuffers)
	{
		for(int i = 0; i < 2; i++)
			if(Buffers[i]) delete Buffers[i], Buffers[i] = nullptr;
		OwnBuffers = false;
	}
	Buffers[0] = buff1;
	Buffers[1] = buff2;
	FrontIndex = 0;
	for(int i = 0; i < 2; i++)
	{
		if(!Buffers[i]) continue;
		iTVPTexture2D *tex = Buffers[i]->GetTexture();
		memset(tex->GetScanLineForWrite(0), 0,
			(size_t)tex->GetPitch() * (size_t)tex->GetHeight());
	}
	FrontValid = Buffers[0] != nullptr;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::PresentVideoImage()
{
	tTVPBaseTexture *mix;
	float alpha;
	{
		std::lock_guard<std::mutex> lk(FrameMutex);
		CopyPendingToFrontLocked();
		if(!FrontValid || !Buffers[0] || !Buffers[1]) return;
		mix = MixingBitmap;
		alpha = MixingAlpha * MixingMovieAlpha;
	}
	if(!mix || alpha <= 0.0f) return;

	iTVPTexture2D *srcTex = Buffers[FrontIndex]->GetTexture();
	iTVPTexture2D *dstTex = mix->GetTexture();
	if(!srcTex || !dstTex) return;
	const int srcPitch = srcTex->GetPitch() / 4;
	const int dstPitch = dstTex->GetPitch() / 4;
	const int w = std::min<int>(VideoWidth, (int)dstTex->GetWidth());
	const int h = std::min<int>(VideoHeight, (int)dstTex->GetHeight());
	if(w <= 0 || h <= 0) return;
	int opa = (int)(alpha * 255.0f + 0.5f);
	if(opa > 255) opa = 255;
	if(opa <= 0) return;
	const uint32_t *src = (const uint32_t*)srcTex->GetScanLineForRead(0);
	uint32_t *dst = (uint32_t*)dstTex->GetScanLineForWrite(0);

	const bool haveBackground =
		MixingBackgroundW == w && MixingBackgroundH == h &&
		MixingBackground.size() >= (size_t)w * (size_t)h;
	for(int y = 0; y < h; y++)
	{
		uint32_t *dstRow = dst + (size_t)y * dstPitch;
		if(haveBackground)
		{
			// start from the background the movie was attached to, then blend
			// this frame over it: the result does not depend on the previous
			// frame, so a partial mixingMovieAlpha stays partial
			memcpy(dstRow, &MixingBackground[(size_t)y * w], (size_t)w * 4);
			TVPConstAlphaBlend(dstRow, src + (size_t)y * srcPitch, w, opa);
		}
		else
		{
			TVPConstAlphaBlend(dstRow, src + (size_t)y * srcPitch, w, opa);
		}
	}
}

//---------------------------------------------------------------------------
// Snapshot of the mixing target (FrameMutex held)
//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::CaptureMixingBackgroundLocked()
{
	MixingBackground.clear();
	MixingBackgroundW = MixingBackgroundH = 0;
	if(!MixingBitmap) return;
	iTVPTexture2D *t = MixingBitmap->GetTexture();
	if(!t) return;
	const int w = std::min<int>(VideoWidth, (int)t->GetWidth());
	const int h = std::min<int>(VideoHeight, (int)t->GetHeight());
	if(w <= 0 || h <= 0) return;
	const int pitch = t->GetPitch() / 4;
	const uint32_t *src = (const uint32_t*)t->GetScanLineForRead(0);
	MixingBackground.resize((size_t)w * (size_t)h);
	for(int y = 0; y < h; y++)
		memcpy(&MixingBackground[(size_t)y * w], src + (size_t)y * pitch, (size_t)w * 4);
	MixingBackgroundW = w;
	MixingBackgroundH = h;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::SetMixingBitmap(tTVPBaseTexture *dest, float alpha)
{
	std::lock_guard<std::mutex> lk(FrameMutex);
	MixingBitmap = dest;
	MixingAlpha = alpha;
	MixingAttached = true;
	CaptureMixingBackgroundLocked();
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::ResetMixingBitmap()
{
	// the movie comes off the mixing target: the area it covered goes back to
	// the movie background colour and no frame is blended in any more
	std::lock_guard<std::mutex> lk(FrameMutex);
	if(MixingBitmap)
	{
		iTVPTexture2D *dstTex = MixingBitmap->GetTexture();
		if(dstTex)
		{
			const int pitch = dstTex->GetPitch() / 4;
			uint32_t *dst = (uint32_t*)dstTex->GetScanLineForWrite(0);
			const int w = std::min<int>(VideoWidth, (int)dstTex->GetWidth());
			const int h = std::min<int>(VideoHeight, (int)dstTex->GetHeight());
			const uint32_t col = MixingBGColor | 0xff000000;
			for(int y = 0; y < h; y++)
			{
				uint32_t *p = dst + (size_t)y * pitch;
				for(int x = 0; x < w; x++) p[x] = col;
			}
		}
	}
	MixingBitmap = nullptr;
	MixingBackground.clear();
	MixingBackgroundW = MixingBackgroundH = 0;
}

//---------------------------------------------------------------------------
// events into the engine
//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::PostEvent(int message, intptr_t wparam, intptr_t lparam)
{
	std::lock_guard<std::mutex> lk(PostMutex);
	if(!CanPost || !CallbackWin) return;
	NativeEvent ev(message);
	ev.WParam = wparam;
	ev.LParam = lparam;
	CallbackWin->PostEvent(ev);
}

//---------------------------------------------------------------------------
// playback control
//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::Play()
{
	{
		std::lock_guard<std::mutex> lk(StateMutex);
		if(State_ == tState::Playing) return;
		if(State_ == tState::Ended && !SeekLocked(0)) return;
		WallAnchorMs = (double)LastPositionMs;
		WallAnchorTick = TVPGetTickCount();
		AudioClockTrusted = false;
		AudioClockRejected = false;
		AudioAnchorSet = false;
		AudioLastAdvanceTick = 0;
		AudioLastSamples = 0;
		Completed = false;
		EofSeen = false;
		EofTick = 0;
		DiscardUntilMs = -1;
		State_ = tState::Playing;
		if(Sound)
		{
			Sound->Reset();
			Sound->Play();
		}
		if(!Worker.joinable()) StartWorkerLocked();
	}
	StateCond.notify_all();
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::StartWorkerLocked()
{
	StopRequested = false;
	Worker = std::thread(&tTVPFFmpegVideoOverlay::WorkerMain, this);
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::Pause()
{
	{
		std::lock_guard<std::mutex> lk(StateMutex);
		if(State_ == tState::Playing)
		{
			WallAnchorMs = ClockMs();
			State_ = tState::Paused;
		}
		if(Sound) Sound->Pause();
	}
	StateCond.notify_all();
	std::lock_guard<std::mutex> plk(PostMutex);
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::Stop()
{
	{
		std::lock_guard<std::mutex> lk(StateMutex);
		State_ = tState::Stopped;
		SeekLocked(0);
		if(Sound) Sound->Stop();
	}
	StateCond.notify_all();
	std::lock_guard<std::mutex> plk(PostMutex);
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::Rewind()
{
	{
		std::lock_guard<std::mutex> lk(StateMutex);
		// VideoOvlImpl calls this directly to restart a looping movie when it
		// gets EC_COMPLETE (that path does not call Play()), so playback
		// continues from the start; a rewind from a paused/ready movie just
		// goes back to the start
		const bool resume = State_ == tState::Playing || State_ == tState::Ended;
		if(!SeekLocked(0)) return;
		if(resume)
		{
			State_ = tState::Playing;
			WallAnchorTick = TVPGetTickCount();
			AudioClockTrusted = false;
			AudioAnchorSet = false;
			AudioLastSamples = 0;
			Completed = false;
			EofSeen = false;
			EofTick = 0;
			if(Sound)
			{
				Sound->Reset();
				Sound->Play();
			}
			if(!Worker.joinable()) StartWorkerLocked();
		}
		else if(State_ != tState::Stopped)
		{
			State_ = tState::Ready;
		}
	}
	StateCond.notify_all();
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::SetPosition(uint64_t tick)
{
	std::lock_guard<std::mutex> lk(StateMutex);
	const bool wasPlaying = State_ == tState::Playing;
	if(!SeekLocked((int64_t)tick)) return;
	if(wasPlaying)
	{
		WallAnchorTick = TVPGetTickCount();
		AudioClockTrusted = false;
		AudioAnchorSet = false;
		AudioLastSamples = 0;
		if(Sound)
		{
			Sound->Reset();
			Sound->Play();
		}
	}
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::GetPosition(uint64_t *tick)
{
	if(!tick) return;
	std::lock_guard<std::mutex> lk(StateMutex);
	double pos = ClockMs();
	if(pos < (double)LastPositionMs) pos = (double)LastPositionMs;
	*tick = (uint64_t)(pos < 0 ? 0 : pos);
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::GetStatus(tTVPVideoStatus *status)
{
	if(!status) return;
	std::lock_guard<std::mutex> lk(StateMutex);
	switch(State_)
	{
	case tState::Ready:   *status = vsReady; break;
	case tState::Playing: *status = vsPlaying; break;
	case tState::Paused:  *status = vsPaused; break;
	case tState::Ended:   *status = vsEnded; break;
	case tState::Stopped:
	default:              *status = vsStopped; break;
	}
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::SetFrame(int f)
{
	std::lock_guard<std::mutex> lk(StateMutex);
	const bool wasPlaying = State_ == tState::Playing;
	int64_t ms = (int64_t)llround((double)(f < 0 ? 0 : f) * 1000.0 / FPS);
	if(!SeekLocked(ms)) return;
	if(wasPlaying)
	{
		WallAnchorTick = TVPGetTickCount();
		AudioClockTrusted = false;
		AudioAnchorSet = false;
		AudioLastSamples = 0;
		if(Sound)
		{
			Sound->Reset();
			Sound->Play();
		}
	}
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::GetFrame(int *f)
{
	if(!f) return;
	std::lock_guard<std::mutex> lk(StateMutex);
	*f = LastFrameIndex;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::SetStopFrame(int frame)
{
	std::lock_guard<std::mutex> lk(StateMutex);
	StopFrameValue = frame;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::GetStopFrame(int *frame)
{
	if(!frame) return;
	std::lock_guard<std::mutex> lk(StateMutex);
	*frame = StopFrameIndex();
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::SetDefaultStopFrame()
{
	std::lock_guard<std::mutex> lk(StateMutex);
	StopFrameValue = -1;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::SetPlayRate(double rate)
{
	if(!(rate > 0.0)) return;
	if(rate > 16.0) rate = 16.0;
	{
		std::lock_guard<std::mutex> lk(StateMutex);
		if(rate == Rate) return;
		const bool wasPlaying = State_ == tState::Playing;
		const double pos = ClockMs();
		Rate = rate;
		if(Audio.Active())
		{
			// the audio is resampled to rate * source rate, so the mixer's
			// stream is recreated at the new rate (sped-up audio, like a
			// rate-changed player)
			BuildResampler();
			SetupSoundBuffer();
			if(Sound && wasPlaying) Sound->Play();
		}
		AudioQueue.clear();
		AudioQueueOffset = 0;
		AudioAnchorSet = false;
		AudioClockTrusted = false;
		AudioClockRejected = false;
		AudioLastAdvanceTick = 0;
		WallAnchorMs = pos;
		WallAnchorTick = TVPGetTickCount();
	}
	StateCond.notify_all();
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::SetAudioBalance(long balance)
{
	std::lock_guard<std::mutex> lk(FrameMutex);
	AudioBalanceValue = balance;
	if(Sound) Sound->SetPan(balance / 100000.0f);
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::GetAudioBalance(long *balance)
{
	if(!balance) return;
	std::lock_guard<std::mutex> lk(FrameMutex);
	*balance = Sound ? (long)(Sound->GetPan() * 100000.0f) : AudioBalanceValue;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::SetAudioVolume(long volume)
{
	std::lock_guard<std::mutex> lk(FrameMutex);
	AudioVolumeValue = volume;
	if(Sound) Sound->SetVolume(volume / 100000.0f);
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::GetAudioVolume(long *volume)
{
	if(!volume) return;
	std::lock_guard<std::mutex> lk(FrameMutex);
	*volume = Sound ? (long)(Sound->GetVolume() * 100000.0f) : AudioVolumeValue;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::DisableAudioStream(void)
{
	{
		std::lock_guard<std::mutex> lk(StateMutex);
		SelectedAudio = -1;
		Audio.Close();
		AudioQueue.clear();
		AudioQueueOffset = 0;
		AudioAnchorSet = false;
		AudioClockTrusted = false;
		AudioClockRejected = true;
		AudioInputEnded = true;
		if(Sound)
		{
			TeardownSoundBuffer();
		}
		// the wall clock carries the rest of the movie
		WallAnchorMs = ClockMs();
		WallAnchorTick = TVPGetTickCount();
	}
	StateCond.notify_all();
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::SelectAudioStream(unsigned long num)
{
	std::lock_guard<std::mutex> lk(StateMutex);
	if(num >= AudioStreams.size())
	{
		SelectedAudio = -1;
		Audio.Close();
		AudioQueue.clear();
		AudioQueueOffset = 0;
		AudioAnchorSet = false;
		AudioClockTrusted = false;
		AudioInputEnded = true;
		TeardownSoundBuffer();
		WallAnchorMs = ClockMs();
		WallAnchorTick = TVPGetTickCount();
		StateCond.notify_all();
		return;
	}

	const int64_t pos = (int64_t)ClockMs();
	if(Audio.Codec) Audio.Close();
	if(!OpenAudioStream(AudioStreams[num]))
	{
		SelectedAudio = -1;
		AudioInputEnded = true;
		return;
	}
	SelectedAudio = (int)num;

	// reposition both decoders at the current position
	AudioInputEnded = false;
	FlushInputLocked(pos);
	if(FormatCtx) avformat_flush(FormatCtx);
	AVStream *st = Video.Stream ? Video.Stream : Audio.Stream;
	if(st)
	{
		int64_t ts = av_rescale_q(pos, TVPMsTimeBase, st->time_base);
		if(st->start_time != AV_NOPTS_VALUE) ts += st->start_time;
		av_seek_frame(FormatCtx, st->index, ts, AVSEEK_FLAG_BACKWARD);
		Video.ResetState(true);
		Audio.ResetState(true);
		DiscardUntilMs = pos;
	}
	WallAnchorMs = (double)pos;
	WallAnchorTick = TVPGetTickCount();
	LastPositionMs = pos;
}

//---------------------------------------------------------------------------
void tTVPFFmpegVideoOverlay::SelectVideoStream(unsigned long num)
{
	std::lock_guard<std::mutex> lk(StateMutex);
	if(num >= VideoStreams.size()) return;
	const int64_t pos = (int64_t)ClockMs();
	if(Video.Codec) Video.Close();
	if(!OpenVideoStream(VideoStreams[num])) return;
	SelectedVideo = (int)num;
	StagingPitch = VideoWidth * 4;
	Staging.resize((size_t)StagingPitch * (size_t)VideoHeight);

	FlushInputLocked(pos);
	if(FormatCtx) avformat_flush(FormatCtx);
	int64_t ts = av_rescale_q(pos, TVPMsTimeBase, Video.Stream->time_base);
	if(Video.Stream->start_time != AV_NOPTS_VALUE) ts += Video.Stream->start_time;
	av_seek_frame(FormatCtx, Video.Stream->index, ts, AVSEEK_FLAG_BACKWARD);
	Video.ResetState(true);
	Audio.ResetState(true);
	DiscardUntilMs = pos;
	WallAnchorMs = (double)pos;
	WallAnchorTick = TVPGetTickCount();
	LastPositionMs = pos;
}

//---------------------------------------------------------------------------
// the entry points the engine uses
//---------------------------------------------------------------------------
namespace {

tTVPFFmpegVideoOverlay *TVPCreateOverlay(tTJSNI_VideoOverlay *callbackwin,
	IStream *stream, const tjs_char *streamname, const tjs_char *type, uint64_t size)
{
	// opening happens in the constructor; on failure it throws after freeing
	// what it allocated, exactly like a missing krmovie module would
	return new tTVPFFmpegVideoOverlay(callbackwin, stream, streamname, type, size);
}

} // anonymous namespace

//---------------------------------------------------------------------------
void GetVideoOverlayObject(
	tTJSNI_VideoOverlay *callbackwin, struct IStream *stream, const tjs_char *streamname,
	const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out)
{
	*out = TVPCreateOverlay(callbackwin, stream, streamname, type, size);
}

//---------------------------------------------------------------------------
void GetVideoLayerObject(
	tTJSNI_VideoOverlay *callbackwin, struct IStream *stream, const tjs_char *streamname,
	const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out)
{
	*out = TVPCreateOverlay(callbackwin, stream, streamname, type, size);
}

//---------------------------------------------------------------------------
void GetMixingVideoOverlayObject(
	tTJSNI_VideoOverlay *callbackwin, struct IStream *stream, const tjs_char *streamname,
	const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out)
{
	*out = TVPCreateOverlay(callbackwin, stream, streamname, type, size);
}

//---------------------------------------------------------------------------
void GetMFVideoOverlayObject(
	tTJSNI_VideoOverlay *callbackwin, struct IStream *stream, const tjs_char *streamname,
	const tjs_char *type, uint64_t size, class iTVPVideoOverlay **out)
{
	*out = TVPCreateOverlay(callbackwin, stream, streamname, type, size);
}
