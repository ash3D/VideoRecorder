#include "VideoRecorder\include\VideoRecorder.h"
#include <iostream>
#include <exception>
#include <new>
#include <utility>
#include <ratio>
#include <cassert>
extern "C"
{
#	include "libavcodec/avcodec.h"
#	include "libswscale/swscale.h"
#	include "libavutil/imgutils.h"
}
#include "DirectXTex.h"

using std::wclog;
using std::wcerr;
using std::endl;

static inline void AssertHR(HRESULT hr)
{
	assert(SUCCEEDED(hr));
}

static const/*expr*/ unsigned int cache_line = 64;
static const/*expr*/ unsigned int fps = 25;

AVCodec *const CVideoRecorder::codec = (avcodec_register_all(), avcodec_find_encoder(AV_CODEC_ID_MPEG1VIDEO));

inline void CVideoRecorder::TContextDeleter::operator()(AVCodecContext *context) const
{
	avcodec_close(context);
	avcodec_free_context(&context);
}

void CVideoRecorder::TFrameDeleter::operator()(AVFrame *frame) const
{
	av_freep(frame->data);
	av_frame_free(&frame);
}

void CVideoRecorder::UpdatePixelData(unsigned int width, unsigned int height, const std::function<void(decltype(srcPixels)::pointer)> &GetPixelsCallback)
{
	srcPixels.resize(width * height);
	GetPixelsCallback(srcPixels.data());
}

CVideoRecorder::CVideoRecorder() try :
	context(avcodec_alloc_context3(codec)),
	cvtCtx(nullptr, sws_freeContext),
	packet(std::make_unique<decltype(packet)::element_type>())
{
	assert(codec);
	if (!context)
		throw std::bad_alloc();
}
catch (const std::exception &error)
{
	std::cerr << "Fail to init video recorder: " << error.what() << '.' << endl;
}

CVideoRecorder::~CVideoRecorder()
{
	assert(videoFile.good());
	if (videoFile.is_open())
	{
		wcerr << "Destroying video recorder without stopping current record session." << endl;
		StopRecord();
	}
}

void CVideoRecorder::Draw(unsigned int width, unsigned int height, const std::function<void(decltype(srcPixels)::pointer)> &GetPixelsCallback)
{
	const int srcStride = width * sizeof(decltype(srcPixels)::value_type);

	const bool takeScreenshot = !screenshotPaths.empty();
	if (takeScreenshot)
	{
		UpdatePixelData(width, height, GetPixelsCallback);
		do
		{
			wclog << "Saving screenshot " << screenshotPaths.front() << "..." << endl;

			using namespace DirectX;
			const auto result = SaveToWICFile(
				{ width, height, DXGI_FORMAT_B8G8R8A8_UNORM, srcStride, srcStride * height, reinterpret_cast<uint8_t *>(srcPixels.data()) },
				WIC_FLAGS_NONE, GetWICCodec(WIC_CODEC_JPEG), screenshotPaths.front().c_str());

			AssertHR(result);
			if (SUCCEEDED(result))
				wclog << "Screenshot " << screenshotPaths.front() << " have been saved." << endl;
			else
				wcerr << "Fail to save screenshot " << screenshotPaths.front() << " (hr=" << result << ")." << endl;

			screenshotPaths.pop();
		} while (!screenshotPaths.empty());
	}

	assert(videoFile.good());
	if (videoFile.is_open())
	{
		using std::ratio;
		using namespace std::chrono;
		const auto now = clock::now();
		if (now >= nextFrame)
		{
			av_init_packet(packet.get());
			packet->data = NULL;
			packet->size = 0;

			const auto delta = duration_cast<duration<clock::rep, ratio<1, fps>>>(now - nextFrame);

			if (!takeScreenshot)
				UpdatePixelData(width, height, GetPixelsCallback);

			const auto clean = [this]
			{
				avcodec_close(context.get());
				frame.reset();
				videoFile.close();
				videoFile.clear();
			};

			cvtCtx.reset(sws_getCachedContext(cvtCtx.release(),
				width,			height,				AV_PIX_FMT_BGRA,
				context->width,	context->height,	context->pix_fmt,
				SWS_BILINEAR, NULL, NULL, NULL));
			assert(cvtCtx);
			if (!cvtCtx)
			{
				wcerr << "Fail to convert frame for video." << endl;
				clean();
				return;
			}
			const auto src = reinterpret_cast<const uint8_t *const>(srcPixels.data());
			sws_scale(cvtCtx.get(), &src, &srcStride, 0, height, frame->data, frame->linesize);

			auto i = delta.count();
			do
			{
				int gotPacket;
				const auto result = avcodec_encode_video2(context.get(), packet.get(), frame.get(), &gotPacket);
				assert(result == 0);
				if (result != 0)
				{
					wcerr << "Fail to encode frame for video." << endl;
					clean();
					return;
				}
				if (gotPacket)
				{
					videoFile.write((const char *)packet->data, packet->size);
					av_free_packet(packet.get());
				}
				assert(videoFile.good());

				frame->pts++;
			} while (i--);
			if (videoFile.bad())
			{
				wcerr << "Fail to write video data to file." << endl;
				clean();
				return;
			}

			nextFrame += duration_cast<clock::duration>(delta + duration<clock::rep, ratio<1, fps>>(1u));
		}
	}
}

void CVideoRecorder::StartRecord(unsigned int width, unsigned int height, const wchar_t filename[])
{
	assert(videoFile.good());
	assert(!videoFile.is_open());

	if (videoFile.is_open())
	{
		wcerr << "Starting new video recording before stoping previouse one." << endl;
		StopRecord();
	}

	wclog << "Recording video " << filename << "..." << endl;

	context->bit_rate = 400000 * 8;
	context->width = width & ~1;
	context->height = height & ~1;
	context->time_base = { 1, fps };
	context->gop_size = 10;
	context->max_b_frames = 1;
	context->pix_fmt = AV_PIX_FMT_YUV420P;
	{
		const auto result = avcodec_open2(context.get(), codec, NULL);
		assert(result == 0);
		if (result != 0)
		{
			wcerr << "Fail to open codec for video " << filename << '.' << endl;
			return;
		}
	}

	frame.reset(av_frame_alloc());
	frame->format = AV_PIX_FMT_YUV420P;
	frame->width = context->width;
	frame->height = context->height;
	{
		const auto result = av_image_alloc(frame->data, frame->linesize, context->width, context->height, context->pix_fmt, cache_line);
		assert(result >= 0);
		if (result < 0)
		{
			wcerr << "Fail to allocate frame for video " << filename << '.' << endl;
			avcodec_close(context.get());
			return;
		}
	}
	frame->pts = 0;

	using std::ios_base;
	videoFile.open(filename, ios_base::out | ios_base::binary);
	assert(videoFile.good());
	if (videoFile.bad())
	{
		std::wcerr << "Fail to create video file " << filename << '.' << endl;
		avcodec_close(context.get());
		frame.reset();
		videoFile.clear();
		return;
	}

	nextFrame = clock::now();
}

void CVideoRecorder::StopRecord()
{
	assert(videoFile.good());
	assert(videoFile.is_open());

	if (!videoFile.is_open())
	{
		wcerr << "Stopping not running video record." << endl;
		return;
	}

	int result, gotPacket;
	do
	{
		result = avcodec_encode_video2(context.get(), packet.get(), NULL, &gotPacket);
		assert(result == 0);
		if (gotPacket && result == 0)
		{
			videoFile.write((const char *)packet->data, packet->size);
			av_free_packet(packet.get());
		}
	} while (gotPacket && result == 0);

	static const uint8_t endcode[] = { 0, 0, 1, 0xb7 };
	videoFile.write((const char *)endcode, sizeof endcode);

	videoFile.close();
	assert(videoFile.good());
	if (result == 0 && videoFile.good())
		wclog << "Video have been recorded." << endl;
	else
		wcerr << "Fail to record video." << endl;

	avcodec_close(context.get());

	frame.reset();
}