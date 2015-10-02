#include "VideoRecorder\include\VideoRecorder.h"
#include <iostream>
#include <exception>
#include <new>
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

const AVCodec *const CVideoRecorder::codec = (avcodec_register_all(), avcodec_find_encoder(AV_CODEC_ID_MPEG1VIDEO));

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

void CVideoRecorder::Process()
{
	while (true)
	{
		std::unique_lock<decltype(mtx)> lck(mtx);
		workerEvent.wait(lck, [this] { return workerCondition != WorkerCondition::WAIT; });
		switch (workerCondition)
		{
		case WorkerCondition::DO_JOB:
			if (frameQueue.empty())
			{
				workerCondition = WorkerCondition::WAIT;
				workerEvent.notify_one();
			}
			else
			{
				auto srcFrame = std::move(frameQueue.front());
				frameQueue.pop();
				lck.unlock();

				const int srcStride = srcFrame.width * sizeof(TPixels::element_type);

				while (!srcFrame.screenshotPaths.empty())
				{
					wclog << "Saving screenshot " << srcFrame.screenshotPaths.front() << "..." << endl;

					using namespace DirectX;
					const auto result = SaveToWICFile(
					{
						srcFrame.width, srcFrame.height, DXGI_FORMAT_B8G8R8A8_UNORM,
						srcStride, srcStride * srcFrame.height, reinterpret_cast<uint8_t *>(srcFrame.pixels.get())
					},
						WIC_FLAGS_NONE, GetWICCodec(WIC_CODEC_JPEG), srcFrame.screenshotPaths.front().c_str());

					AssertHR(result);
					if (SUCCEEDED(result))
						wclog << "Screenshot " << srcFrame.screenshotPaths.front() << " have been saved." << endl;
					else
						wcerr << "Fail to save screenshot " << srcFrame.screenshotPaths.front() << " (hr=" << result << ")." << endl;

					srcFrame.screenshotPaths.pop();
				}

				if (srcFrame.videoPendingFrames)
				{
					av_init_packet(packet.get());
					packet->data = NULL;
					packet->size = 0;

					const auto clean = [this]
					{
						avcodec_close(context.get());
						dstFrame.reset();

						std::lock_guard<decltype(videoFileMtx)> videoFileLck(videoFileMtx);
						videoFile.close();
						videoFile.clear();
					};

					cvtCtx.reset(sws_getCachedContext(cvtCtx.release(),
						srcFrame.width, srcFrame.height, AV_PIX_FMT_BGRA,
						context->width, context->height, context->pix_fmt,
						SWS_BILINEAR, NULL, NULL, NULL));
					assert(cvtCtx);
					if (!cvtCtx)
					{
						wcerr << "Fail to convert frame for video." << endl;
						clean();
						continue;
					}
					const auto src = reinterpret_cast<const uint8_t *const>(srcFrame.pixels.get());
					sws_scale(cvtCtx.get(), &src, &srcStride, 0, srcFrame.height, dstFrame->data, dstFrame->linesize);

					do
					{
						int gotPacket;
						const auto result = avcodec_encode_video2(context.get(), packet.get(), dstFrame.get(), &gotPacket);
						assert(result == 0);
						if (result != 0)
						{
							wcerr << "Fail to encode frame for video." << endl;
							clean();
							continue;
						}
						if (gotPacket)
						{
							{
								std::lock_guard<decltype(videoFileMtx)> videoFileLck(videoFileMtx);
								videoFile.write((const char *)packet->data, packet->size);
							}
							av_free_packet(packet.get());
						}
						assert(videoFile.good());

						dstFrame->pts++;
					} while (--srcFrame.videoPendingFrames);

					std::lock_guard<decltype(videoFileMtx)> videoFileLck(videoFileMtx);
					if (videoFile.bad())
					{
						wcerr << "Fail to write video data to file." << endl;
						clean();
						continue;
					}
				}
			}
			break;
		case WorkerCondition::FINISH:
			return;
		default:
			assert(false);
			__assume(false);
		}

	}
}

CVideoRecorder::CVideoRecorder() try :
	context(avcodec_alloc_context3(codec)),
	cvtCtx(nullptr, sws_freeContext),
	packet(std::make_unique<decltype(packet)::element_type>()),
	worker(std::mem_fn(&CVideoRecorder::Process), this)
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
	{
		std::unique_lock<decltype(mtx)> lck(mtx);
		workerEvent.wait(lck, [this] { return workerCondition == WorkerCondition::WAIT; });

		assert(videoFile.good());
		if (videoFile.is_open())
		{
			wcerr << "Destroying video recorder without stopping current record session." << endl;
			StopRecord();
		}

		workerCondition = WorkerCondition::FINISH;
		workerEvent.notify_all();
	}
	worker.join();
}

void CVideoRecorder::Draw(unsigned int width, unsigned int height, const std::function<void (TPixels::pointer)> &GetPixelsCallback)
{
	decltype(decltype(frameQueue)::value_type::videoPendingFrames) videoPendingFrames = 0;

	std::unique_lock<decltype(videoFileMtx)> videoFileLck(videoFileMtx, std::try_to_lock);
	const bool needVideo = !videoFileLck || (assert(videoFile.good()), videoFile.is_open());
	if (videoFileLck)
		videoFileLck.unlock();

	if (needVideo)
	{
		const auto now = clock::now();
		if (now >= nextFrame)
		{
			using std::chrono::duration_cast;
			const auto delta = duration_cast<TFrameDuration>(now - nextFrame) + TFrameDuration(1u);
			nextFrame += duration_cast<clock::duration>(delta);
			videoPendingFrames = delta.count();
		}
	}

	if (videoPendingFrames || !screenshotPaths.empty())
	{
		decltype(frameQueue)::value_type srcFrame
		{
			std::make_unique<TPixels::element_type []>(width * height),
			width, height,
			std::move(screenshotPaths),
			std::move(videoPendingFrames)
		};
		GetPixelsCallback(srcFrame.pixels.get());

		std::lock_guard<decltype(mtx)> lck(mtx);
		frameQueue.push(std::move(srcFrame));
		workerCondition = WorkerCondition::DO_JOB;
		workerEvent.notify_all();
	}
}

void CVideoRecorder::StartRecord(unsigned int width, unsigned int height, const wchar_t filename[])
{
	std::unique_lock<decltype(mtx)> lck(mtx);
	workerEvent.wait(lck, [this] { return workerCondition == WorkerCondition::WAIT; });

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

	dstFrame.reset(av_frame_alloc());
	dstFrame->format = AV_PIX_FMT_YUV420P;
	dstFrame->width = context->width;
	dstFrame->height = context->height;
	{
		const auto result = av_image_alloc(dstFrame->data, dstFrame->linesize, context->width, context->height, context->pix_fmt, cache_line);
		assert(result >= 0);
		if (result < 0)
		{
			wcerr << "Fail to allocate frame for video " << filename << '.' << endl;
			avcodec_close(context.get());
			return;
		}
	}
	dstFrame->pts = 0;

	using std::ios_base;
	videoFile.open(filename, ios_base::out | ios_base::binary);
	assert(videoFile.good());
	if (videoFile.bad())
	{
		std::wcerr << "Fail to create video file " << filename << '.' << endl;
		avcodec_close(context.get());
		dstFrame.reset();
		videoFile.clear();
		return;
	}

	nextFrame = clock::now();
}

void CVideoRecorder::StopRecord()
{
	std::unique_lock<decltype(mtx)> lck(mtx);
	workerEvent.wait(lck, [this] { return workerCondition == WorkerCondition::WAIT; });

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

	dstFrame.reset();
}

void CVideoRecorder::Screenshot(std::wstring &&filename)
{
	std::lock_guard<decltype(mtx)> lck(mtx);
	screenshotPaths.push(std::move(filename));
}