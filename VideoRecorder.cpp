#include "VideoRecorder\include\VideoRecorder.h"
#include <filesystem>
#include <algorithm>
#include <iterator>
#include <iostream>
#include <exception>
#include <new>
#include <cassert>
#include <cctype>
#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
extern "C"
{
#	include "libavcodec/avcodec.h"
#	include "libswscale/swscale.h"
#	include "libavutil/imgutils.h"
#	include "libavutil/opt.h"
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

#define CODEC_ID AV_CODEC_ID_HEVC
const AVCodec *const CVideoRecorder::codec = (avcodec_register_all(), avcodec_find_encoder(CODEC_ID));

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

inline const char *CVideoRecorder::EncodePerformance_2_Str(EncodePerformance performance)
{
#	define MAP_ENUM_2_STRING(r, enum, value) \
		case enum::value:	return BOOST_PP_STRINGIZE(value);

	switch (performance)
	{
		BOOST_PP_SEQ_FOR_EACH(MAP_ENUM_2_STRING, EncodePerformance, ENCODE_PERFORMANCE_VALUES)
	default:
		throw "Invalid encode performance value.";
	}

#	undef MAP_ENUM_2_STRING
}

static const/*expr*/ std::underlying_type<DirectX::WICCodecs>::type CODEC_DDS = 0xFFFF0001, CODEC_TGA = 0xFFFF0002;
static const/*expr*/ std::pair<const wchar_t *, DirectX::WICCodecs> pictureFormats[] =
{
	{ L".bmp",	DirectX::WIC_CODEC_BMP			},
	{ L".jpg",	DirectX::WIC_CODEC_JPEG			},
	{ L".jpeg",	DirectX::WIC_CODEC_JPEG			},
	{ L".png",	DirectX::WIC_CODEC_PNG			},
	{ L".tif",	DirectX::WIC_CODEC_TIFF			},
	{ L".tiff",	DirectX::WIC_CODEC_TIFF			},
	{ L".gif",	DirectX::WIC_CODEC_GIF			},
	{ L".hdp",	DirectX::WIC_CODEC_WMP			},
	{ L".jxr",	DirectX::WIC_CODEC_WMP			},
	{ L".wdp",	DirectX::WIC_CODEC_WMP			},
	{ L".ico",	DirectX::WIC_CODEC_ICO			},
	{ L".dds",	DirectX::WICCodecs(CODEC_DDS)	},
	{ L".tga",	DirectX::WICCodecs(CODEC_TGA)	},
};

static DirectX::WICCodecs GetScreenshotCodec(std::wstring &&ext)
{
	std::transform(ext.begin(), ext.end(), ext.begin(), tolower);
	const auto found = std::find_if(std::begin(pictureFormats), std::end(pictureFormats), [&ext](const std::remove_extent<decltype(pictureFormats)>::type &format)
	{
		return ext == format.first;
	});
	if (found == std::end(pictureFormats))
	{
		wcerr << "Unrecognized screenshot format \"" << ext << "\". Using \"tga\" as fallback." << endl;
		return DirectX::WICCodecs(CODEC_TGA);
	}
	else
		return found->second;
}

#pragma region Task
struct CVideoRecorder::ITask
{
	virtual void operator ()(CVideoRecorder &parent) = 0;
	virtual ~ITask() = default;
};

#pragma region CFrameTask
class CVideoRecorder::CFrameTask final : public ITask
{
	std::shared_ptr<CFrame> srcFrame;

public:
	CFrameTask(std::shared_ptr<CFrame> &&frame) : srcFrame(std::move(frame)) {}
#if !(defined _MSC_VER && _MSC_VER < 1900)
	CFrameTask(CFrameTask &&) = default;
#endif

public:
	void operator ()(CVideoRecorder &parent) override;
	~CFrameTask() override = default;
};
#pragma endregion

#pragma region CStartVideoRecordRequest
class CVideoRecorder::CStartVideoRecordRequest final : public ITask
{
	const std::wstring filename;
	const unsigned int width, height;
	const TEncodeConfig config;
	const bool matchedStop;

public:
	CStartVideoRecordRequest(std::wstring &&filename, unsigned int width, unsigned int height, const TEncodeConfig &config, bool matchedStop) :
		filename(std::move(filename)), width(width), height(height),
		config(config), matchedStop(matchedStop) {}
#if !(defined _MSC_VER && _MSC_VER < 1900)
	CStartVideoRecordRequest(CStartVideoRecordRequest &&) = default;
#endif

public:
	void operator ()(CVideoRecorder &parent) override;
	~CStartVideoRecordRequest() override = default;
};
#pragma endregion

#pragma region CStopVideoRecordRequest
class CVideoRecorder::CStopVideoRecordRequest final : public ITask
{
	const bool matchedStart;

public:
	CStopVideoRecordRequest(bool matchedStart) : matchedStart(matchedStart) {}
#if !(defined _MSC_VER && _MSC_VER < 1900)
	CStopVideoRecordRequest(CStopVideoRecordRequest &&) = default;
#endif

public:
	void operator ()(CVideoRecorder &parent) override;
	~CStopVideoRecordRequest() override = default;
};
#pragma endregion

void CVideoRecorder::CFrameTask::operator ()(CVideoRecorder &parent)
{
	const auto srcFrameData = srcFrame->GetFrameData();

	while (!srcFrame->screenshotPaths.empty())
	{
		wclog << "Saving screenshot " << srcFrame->screenshotPaths.front() << "..." << endl;

		using namespace DirectX;

		std::tr2::sys::wpath screenshotPath(srcFrame->screenshotPaths.front());
		const auto screenshotCodec = GetScreenshotCodec(screenshotPath.extension());

		const Image image =
		{
			srcFrameData.width, srcFrameData.height, DXGI_FORMAT_B8G8R8A8_UNORM,
			srcFrameData.stride, srcFrameData.stride * srcFrameData.height, const_cast<uint8_t *>(srcFrameData.pixels)
		};

		HRESULT hr;
		switch (screenshotCodec)
		{
		case CODEC_DDS:
			hr = SaveToDDSFile(image, DDS_FLAGS_NONE, srcFrame->screenshotPaths.front().c_str());
			break;
		case CODEC_TGA:
			hr = SaveToTGAFile(image, srcFrame->screenshotPaths.front().c_str());
			break;
		default:
			hr = SaveToWICFile(image, WIC_FLAGS_NONE, GetWICCodec(screenshotCodec), srcFrame->screenshotPaths.front().c_str());
			break;
		}

		AssertHR(hr);
		if (SUCCEEDED(hr))
			wclog << "Screenshot " << srcFrame->screenshotPaths.front() << " has been saved." << endl;
		else
			wcerr << "Fail to save screenshot " << srcFrame->screenshotPaths.front() << " (hr=" << hr << ")." << endl;

		srcFrame->screenshotPaths.pop();
	}

	if (srcFrame->videoPendingFrames && (assert(parent.videoFile.good()), parent.videoFile.is_open()))
	{
		av_init_packet(parent.packet.get());
		parent.packet->data = NULL;
		parent.packet->size = 0;

		const auto clean = [this, &parent]
		{
			avcodec_close(parent.context.get());
			parent.dstFrame.reset();

			parent.videoFile.close();
			parent.videoFile.clear();
		};

		parent.cvtCtx.reset(sws_getCachedContext(parent.cvtCtx.release(),
			srcFrameData.width, srcFrameData.height, AV_PIX_FMT_BGRA,
			parent.context->width, parent.context->height, parent.context->pix_fmt,
			SWS_BILINEAR, NULL, NULL, NULL));
		assert(parent.cvtCtx);
		if (!parent.cvtCtx)
		{
			wcerr << "Fail to convert frame for video." << endl;
			clean();
			return;
		}
		const int srcStride = srcFrameData.stride;
		sws_scale(parent.cvtCtx.get(), &srcFrameData.pixels, &srcStride, 0, srcFrameData.height, parent.dstFrame->data, parent.dstFrame->linesize);

		do
		{
			int gotPacket;
			const auto result = avcodec_encode_video2(parent.context.get(), parent.packet.get(), parent.dstFrame.get(), &gotPacket);
			assert(result == 0);
			if (result != 0)
			{
				wcerr << "Fail to encode frame for video." << endl;
				clean();
				return;
			}
			if (gotPacket)
			{
				parent.videoFile.write((const char *)parent.packet->data, parent.packet->size);
				av_free_packet(parent.packet.get());
			}
			assert(parent.videoFile.good());

			parent.dstFrame->pts++;
		} while (--srcFrame->videoPendingFrames);

		if (parent.videoFile.bad())
		{
			wcerr << "Fail to write video data to file." << endl;
			clean();
			return;
		}
	}
}

void CVideoRecorder::CStartVideoRecordRequest::operator ()(CVideoRecorder &parent)
{
	assert(parent.videoFile.good());
	assert(!parent.videoFile.is_open());

	if (!matchedStop)
		wcerr << "Starting new video record session without stopping previouse one." << endl;

	if (parent.videoFile.is_open())
	{
		CStopVideoRecordRequest stopRecord(true);
		stopRecord(parent);
	}

	parent.context->width = width & ~1;
	parent.context->height = height & ~1;
	parent.context->time_base = { 1, fps };
	parent.context->pix_fmt = AV_PIX_FMT_YUV420P;
	if (const auto availableThreads = std::thread::hardware_concurrency())
		parent.context->thread_count = availableThreads;	// TODO: consider reserving 1 or more threads for other stuff
#if CODEC_ID == AV_CODEC_ID_H264 || CODEC_ID == AV_CODEC_ID_HEVC
	if (config.crf != -1)
	{
		try
		{
			av_opt_set(parent.context->priv_data, "preset", EncodePerformance_2_Str(config.performance), 0);
			av_opt_set_int(parent.context.get(), "crf", config.crf, AV_OPT_SEARCH_CHILDREN);
		}
		catch (const char error[])
		{
			wcerr << error << endl;
			return;
		}
	}
#endif

	wclog << "Recording video " << filename << " (using " << parent.context->thread_count << " threads for encoding)..." << endl;

	{
		const auto result = avcodec_open2(parent.context.get(), codec, NULL);
		assert(result == 0);
		if (result != 0)
		{
			wcerr << "Fail to open codec for video " << filename << '.' << endl;
			return;
		}
	}

	parent.dstFrame.reset(av_frame_alloc());
	parent.dstFrame->format = AV_PIX_FMT_YUV420P;
	parent.dstFrame->width = parent.context->width;
	parent.dstFrame->height = parent.context->height;
	{
		const auto result = av_image_alloc(parent.dstFrame->data, parent.dstFrame->linesize, parent.context->width, parent.context->height, parent.context->pix_fmt, cache_line);
		assert(result >= 0);
		if (result < 0)
		{
			wcerr << "Fail to allocate frame for video " << filename << '.' << endl;
			avcodec_close(parent.context.get());
			return;
		}
	}
	parent.dstFrame->pts = 0;

	using std::ios_base;
	parent.videoFile.open(filename, ios_base::out | ios_base::binary);
	assert(parent.videoFile.good());
	if (parent.videoFile.bad())
	{
		std::wcerr << "Fail to create video file " << filename << '.' << endl;
		avcodec_close(parent.context.get());
		parent.dstFrame.reset();
		parent.videoFile.clear();
		return;
	}
}

void CVideoRecorder::CStopVideoRecordRequest::operator ()(CVideoRecorder &parent)
{
	assert(parent.videoFile.good());
	assert(parent.videoFile.is_open());

	if (!matchedStart)
		wcerr << "Stopping video record without matched start." << endl;

	if (!parent.videoFile.is_open())
		return;

	int result, gotPacket;
	do
	{
		result = avcodec_encode_video2(parent.context.get(), parent.packet.get(), NULL, &gotPacket);
		assert(result == 0);
		if (gotPacket && result == 0)
		{
			parent.videoFile.write((const char *)parent.packet->data, parent.packet->size);
			av_free_packet(parent.packet.get());
		}
	} while (gotPacket && result == 0);

	static const uint8_t endcode[] = { 0, 0, 1, 0xb7 };
	parent.videoFile.write((const char *)endcode, sizeof endcode);

	parent.videoFile.close();
	assert(parent.videoFile.good());
	if (result == 0 && parent.videoFile.good())
		wclog << "Video has been recorded." << endl;
	else
		wcerr << "Fail to record video." << endl;

	avcodec_close(parent.context.get());

	parent.dstFrame.reset();
}
#pragma endregion

CVideoRecorder::CFrame::CFrame(TOpaque opaque) :
	screenshotPaths(std::move(opaque.first)), videoPendingFrames(std::move(opaque.second)) {}

void CVideoRecorder::Process()
{
	while (true)
	{
		std::unique_lock<decltype(mtx)> lck(mtx);
		workerEvent.wait(lck, [this] { return workerCondition != WorkerCondition::WAIT; });
		switch (workerCondition)
		{
		case WorkerCondition::DO_JOB:
			if (taskQueue.empty())
			{
				workerCondition = WorkerCondition::WAIT;
				workerEvent.notify_one();
			}
			else
			{
				auto task = std::move(taskQueue.front());
				taskQueue.pop();
				lck.unlock();
				task->operator ()(*this);
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

/*
	make it external in order to allow for forward decl for std::unique_ptr
	declaring move ctor also disables copy ctor/assignment which is desired
*/
#if !(defined _MSC_VER && _MSC_VER < 1900)
CVideoRecorder::CVideoRecorder(CVideoRecorder &&) = default;
#endif

CVideoRecorder::~CVideoRecorder()
{
	if (videoRecordStarted)
	{
		wcerr << "Destroying video recorder without stopping current record session." << endl;
		StopRecord();
	}

	{
		std::unique_lock<decltype(mtx)> lck(mtx);
		workerEvent.wait(lck, [this] { return workerCondition == WorkerCondition::WAIT; });

		workerCondition = WorkerCondition::FINISH;
		workerEvent.notify_all();
	}

	worker.join();
}

void CVideoRecorder::Sample(const std::function<void (CFrame::TOpaque)> &RequestFrameCallback)
{
	decltype(CFrame::videoPendingFrames) videoPendingFrames = 0;

	if (videoRecordStarted)
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
		RequestFrameCallback(std::make_pair(std::move(screenshotPaths), std::move(videoPendingFrames)));
}

void CVideoRecorder::EnqueueFrame(std::shared_ptr<CFrame> frame)
{
	std::lock_guard<decltype(mtx)> lck(mtx);
	taskQueue.emplace(new CFrameTask(std::move(frame)));
	workerCondition = WorkerCondition::DO_JOB;
	workerEvent.notify_all();
}

void CVideoRecorder::StartRecordImpl(std::wstring &&filename, unsigned int width, unsigned int height, const TEncodeConfig &config)
{
	nextFrame = clock::now();

	std::lock_guard<decltype(mtx)> lck(mtx);
	taskQueue.emplace(new CStartVideoRecordRequest(std::move(filename), width, height, config, !videoRecordStarted));
	workerCondition = WorkerCondition::DO_JOB;
	workerEvent.notify_all();
	videoRecordStarted = true;
}

void CVideoRecorder::StopRecord()
{
	std::lock_guard<decltype(mtx)> lck(mtx);
	taskQueue.emplace(new CStopVideoRecordRequest(videoRecordStarted));
	workerCondition = WorkerCondition::DO_JOB;
	workerEvent.notify_all();
	videoRecordStarted = false;
}

void CVideoRecorder::Screenshot(std::wstring &&filename)
{
	screenshotPaths.push(std::move(filename));
}