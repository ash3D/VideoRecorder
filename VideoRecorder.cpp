#include "VideoRecorder/include/VideoRecorder.h"
#include <filesystem>
#include <algorithm>
#include <iterator>
#include <new>
#include <cstdlib>
#include <cassert>
#include <cctype>
#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
extern "C"
{
#	include <libavcodec/avcodec.h>
#	include <libswscale/swscale.h>
#	include <libavutil/imgutils.h>
#	include <libavutil/opt.h>
}
#include "DirectXTex.h"

using std::wclog;
using std::wcerr;
using std::endl;

static inline void AssertHR(HRESULT hr) noexcept
{
	assert(SUCCEEDED(hr));
}

static inline void CheckHR(HRESULT hr)
{
	if (FAILED(hr))
		throw hr;
}

static const/*expr*/ unsigned int cache_line = 64;	// for common x86 CPUs

#define CODEC_ID AV_CODEC_ID_HEVC
const AVCodec *const CVideoRecorder::codec = (avcodec_register_all(), avcodec_find_encoder(CODEC_ID));

inline void CVideoRecorder::ContextDeleter::operator()(AVCodecContext *context) const
{
	avcodec_close(context);
	avcodec_free_context(&context);
}

void CVideoRecorder::FrameDeleter::operator()(AVFrame *frame) const
{
	av_freep(frame->data);
	av_frame_free(&frame);
}

const/*expr*/ char *const CVideoRecorder::screenshotErrorMsgPrefix = "Fail to save screenshot ";

inline const char *CVideoRecorder::EncodePerformance_2_Str(EncodeConfig::Performance performance)
{
#	define MAP_ENUM_2_STRING(r, enum, value) \
		case enum::value:	return BOOST_PP_STRINGIZE(value);

	switch (performance)
	{
		BOOST_PP_SEQ_FOR_EACH(MAP_ENUM_2_STRING, EncodeConfig::Performance, ENCODE_PERFORMANCE_VALUES)
	default:
		throw "Invalid encode performance value.";
	}

#	undef MAP_ENUM_2_STRING
}

void CVideoRecorder::KillRecordSession()
{
	avcodec_close(context.get());
	dstFrame.reset();

	videoFile.close();
	videoFile.clear();
}

/*
	NOTE: exceptions related to mutex locks
		- aren't handled in worker thread which leads to terminate()
		- calls abort() in main thread
*/

#if defined _MSC_VER && _MSC_VER < 1900
__declspec(noreturn)
#else
[[noreturn]]
#endif
void CVideoRecorder::Error(const std::system_error &error)
{
	wcerr << "System error occured: " << error.what() << endl;
	abort();
}

void CVideoRecorder::Error(const std::exception &error, const char errorMsgPrefix[], const std::wstring *filename)
{
	try
	{
		// wait to establish character order for wcerr and to free memory
		std::unique_lock<decltype(mtx)> lck(mtx);
		workerEvent.wait(lck, [this] { return workerCondition == WorkerCondition::WAIT; });
		wcerr << errorMsgPrefix;
		if (filename)
			wcerr << '\"' << filename << '\"';
		wcerr << ": " << error.what() << '.';
		switch (status)
		{
		case Status::OK:
			wcerr << " Try again...";
			break;
		case Status::CLEAN:
			assert(videoFile.good());
			if (videoFile.is_open())
			{
				KillRecordSession();
				taskQueue.clear();
			}
			break;
		}
		wcerr << endl;
	}
	catch (const std::system_error &error)
	{
		Error(error);
	}
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
	virtual operator bool() const { return true; }	// is task ready to handle
	virtual ~ITask() noexcept = default;
};

#pragma region CFrameTask
class CVideoRecorder::CFrameTask final : public ITask
{
	friend void CFrame::Cancel();

private:
	std::shared_ptr<CFrame> srcFrame;

public:
	CFrameTask(std::shared_ptr<CFrame> &&frame) noexcept : srcFrame(std::move(frame)) { assert(srcFrame); }
#if !(defined _MSC_VER && _MSC_VER < 1900)
	CFrameTask(CFrameTask &&) noexcept = default;
#endif

public:
	void operator ()(CVideoRecorder &parent) override;
	operator bool() const override { return srcFrame->ready; }
};
#pragma endregion

#pragma region CStartVideoRecordRequest
class CVideoRecorder::CStartVideoRecordRequest final : public ITask
{
	const std::wstring filename;
	const unsigned int width, height;
	const EncodeConfig config;
	const bool matchedStop;

public:
	const std::wstring &GetFilename() const noexcept { return filename; }

public:
	CStartVideoRecordRequest(std::wstring &&filename, unsigned int width, unsigned int height, const EncodeConfig &config, bool matchedStop) noexcept :
		filename(std::move(filename)), width(width), height(height),
		config(config), matchedStop(matchedStop) {}
#if !(defined _MSC_VER && _MSC_VER < 1900)
	CStartVideoRecordRequest(CStartVideoRecordRequest &&) noexcept = default;
#endif

public:
	void operator ()(CVideoRecorder &parent) override;
};
#pragma endregion

#pragma region CStopVideoRecordRequest
class CVideoRecorder::CStopVideoRecordRequest final : public ITask
{
	const bool matchedStart;

public:
	CStopVideoRecordRequest(bool matchedStart) noexcept : matchedStart(matchedStart) {}
#if !(defined _MSC_VER && _MSC_VER < 1900)
	CStopVideoRecordRequest(CStopVideoRecordRequest &&) noexcept = default;
#endif

public:
	void operator ()(CVideoRecorder &parent) override;
};
#pragma endregion

void CVideoRecorder::CFrameTask::operator ()(CVideoRecorder &parent)
{
	const auto srcFrameData = srcFrame->GetFrameData();
	if (!srcFrameData.pixels)
	{
		wcerr << "Invalid frame occured. Skipping it." << endl;
		return;
	}

	while (!srcFrame->screenshotPaths.empty())
	{
		wclog << "Saving screenshot " << srcFrame->screenshotPaths.front() << "..." << endl;

		using namespace DirectX;

		try
		{
			std::tr2::sys::wpath screenshotPath(srcFrame->screenshotPaths.front());
			const auto screenshotCodec = GetScreenshotCodec(screenshotPath.extension());

			const Image image =
			{
				srcFrameData.width, srcFrameData.height, DXGI_FORMAT_B8G8R8A8_UNORM,
				srcFrameData.stride, srcFrameData.stride * srcFrameData.height, const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(srcFrameData.pixels))
			};

			switch (screenshotCodec)
			{
			case CODEC_DDS:
				CheckHR(SaveToDDSFile(image, DDS_FLAGS_NONE, srcFrame->screenshotPaths.front().c_str()));
				break;
			case CODEC_TGA:
				CheckHR(SaveToTGAFile(image, srcFrame->screenshotPaths.front().c_str()));
				break;
			default:
				CheckHR(SaveToWICFile(image, WIC_FLAGS_NONE, GetWICCodec(screenshotCodec), srcFrame->screenshotPaths.front().c_str()));
				break;
			}

			wclog << "Screenshot " << srcFrame->screenshotPaths.front() << " has been saved." << endl;
		}
		catch (HRESULT hr)
		{
			wcerr << screenshotErrorMsgPrefix << srcFrame->screenshotPaths.front() << " (hr=" << hr << ")." << endl;
		}
		catch (const std::exception &error)
		{
			wcerr << screenshotErrorMsgPrefix << srcFrame->screenshotPaths.front() << ": " << error.what() << '.' << endl;
		}

		srcFrame->screenshotPaths.pop();
	}

	if (srcFrame->videoPendingFrames && (assert(parent.videoFile.good()), parent.videoFile.is_open()))
	{
		av_init_packet(parent.packet.get());
		parent.packet->data = NULL;
		parent.packet->size = 0;

		parent.cvtCtx.reset(sws_getCachedContext(parent.cvtCtx.release(),
			srcFrameData.width, srcFrameData.height, AV_PIX_FMT_BGRA,
			parent.context->width, parent.context->height, parent.context->pix_fmt,
			SWS_BILINEAR, NULL, NULL, NULL));
		assert(parent.cvtCtx);
		if (!parent.cvtCtx)
		{
			wcerr << "Fail to convert frame for video." << endl;
			parent.KillRecordSession();
			return;
		}
		const int srcStride = srcFrameData.stride;
		sws_scale(parent.cvtCtx.get(), reinterpret_cast<const uint8_t *const*>(&srcFrameData.pixels), &srcStride, 0, srcFrameData.height, parent.dstFrame->data, parent.dstFrame->linesize);

		do
		{
			int gotPacket;
			const auto result = avcodec_encode_video2(parent.context.get(), parent.packet.get(), parent.dstFrame.get(), &gotPacket);
			assert(result == 0);
			if (result != 0)
			{
				wcerr << "Fail to encode frame for video." << endl;
				parent.KillRecordSession();
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
			parent.KillRecordSession();
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

#pragma region CFrame
CVideoRecorder::CFrame::CFrame(Opaque opaque) :
	parent				(std::get<0>(std::move(opaque))),
	screenshotPaths		(std::get<1>(std::move(opaque))),
	videoPendingFrames	(std::get<2>(std::move(opaque)))
{}

void CVideoRecorder::CFrame::Ready()
{
	try
	{
		std::lock_guard<decltype(mtx)> lck(parent.mtx);
		ready = true;
		parent.workerCondition = WorkerCondition::DO_JOB;
		parent.workerEvent.notify_all();
	}
	catch (const std::system_error &error)
	{
		parent.Error(error);
	}
}

void CVideoRecorder::CFrame::Cancel()
{
	try
	{
		std::lock_guard<decltype(mtx)> lck(parent.mtx);
		/*
			remove_if always traverses all the range
			find_if/erase pair allows to stop traverse after element being removed was found

			no exception should be thrown during erase() because std::unique_ptr has noexcept move ctor/assignment
		*/
		const auto pred = [this](decltype(parent.taskQueue)::const_reference task)
		{
			if (const CFrameTask *frameTask = dynamic_cast<const CFrameTask *>(task.get()))
				return frameTask->srcFrame.get() == this;
			return false;
		};
		const auto taskToDelete = std::find_if(parent.taskQueue.cbegin(), parent.taskQueue.cend(), pred);
		if (taskToDelete != parent.taskQueue.cend())
		{
			parent.taskQueue.erase(taskToDelete);
			assert(std::find_if(parent.taskQueue.cbegin(), parent.taskQueue.cend(), pred) == parent.taskQueue.cend());
		}
		parent.workerCondition = WorkerCondition::DO_JOB;
		parent.workerEvent.notify_all();
	}
	catch (const std::system_error &error)
	{
		parent.Error(error);
	}
}
#pragma endregion

void CVideoRecorder::Process()
{
	std::unique_lock<decltype(mtx)> lck(mtx);
	while (true)
	{
		switch (workerCondition)
		{
		case WorkerCondition::WAIT:
			workerEvent.wait(lck);
			break;
		case WorkerCondition::DO_JOB:
			if (taskQueue.empty() || !*taskQueue.front())
			{
				workerCondition = WorkerCondition::WAIT;
				workerEvent.notify_one();
			}
			else
			{
				auto task = std::move(taskQueue.front());
				taskQueue.pop_front();
				lck.unlock();
				task->operator ()(*this);
				lck.lock();
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
	wcerr << "Fail to init video recorder: " << error.what() << '.' << endl;
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
	try
	{
		if (videoRecordStarted)
		{
			// wait to establish character order for wcerr
			std::unique_lock<decltype(mtx)> lck(mtx);
			workerEvent.wait(lck, [this] { return workerCondition == WorkerCondition::WAIT; });

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
	catch (const std::system_error &error)
	{
		Error(error);
	}
}

void CVideoRecorder::SampleFrame(const std::function<std::shared_ptr<CFrame> (CFrame::Opaque)> &RequestFrameCallback)
{
	decltype(CFrame::videoPendingFrames) videoPendingFrames = 0;

	const auto nextFrameBackup = nextFrame;
	if (videoRecordStarted)
	{
		const auto now = clock::now();
		if (now >= nextFrame)
		{
			using std::chrono::duration_cast;
			const auto delta = duration_cast<FrameDuration>(now - nextFrame) + FrameDuration(1u);
			nextFrame += duration_cast<clock::duration>(delta);
			videoPendingFrames = delta.count();
		}
	}

	if (videoPendingFrames || !screenshotPaths.empty())
	{
		try
		{
			auto task = std::make_unique<CFrameTask>(RequestFrameCallback(std::make_tuple(std::ref(*this), std::move(screenshotPaths), std::move(videoPendingFrames))));
			std::lock_guard<decltype(mtx)> lck(mtx);
			taskQueue.push_back(std::move(task));
			workerCondition = WorkerCondition::DO_JOB;
			workerEvent.notify_all();
		}
		catch (const std::system_error &error)
		{
			Error(error);
		}
		catch (const std::exception &error)
		{
			nextFrame = nextFrameBackup;
			Error(error, "Fail to sample frame");
			if (status == Status::OK)
			{
				status = Status::RETRY;
				SampleFrame(RequestFrameCallback);
				status = Status::OK;
			}
		}
	}
}

// CStartVideoRecordRequest steals (moves) filename during construction => can not reuse filename during retry => reuse task instead (if it was created successfully)
void CVideoRecorder::StartRecordImpl(std::wstring filename, unsigned int width, unsigned int height, const EncodeConfig &config, std::unique_ptr<CStartVideoRecordRequest> &&task)
{
	try
	{
		if (!task)
			task.reset(new CStartVideoRecordRequest(std::move(filename), width, height, config, !videoRecordStarted));
		std::lock_guard<decltype(mtx)> lck(mtx);
		taskQueue.push_back(std::move(task));
		workerCondition = WorkerCondition::DO_JOB;
		workerEvent.notify_all();
		videoRecordStarted = true;
		nextFrame = clock::now();
	}
	catch (const std::system_error &error)
	{
		Error(error);
	}
	catch (const std::exception &error)
	{
		Error(error, "Fail to start video record ", &(task ? task->GetFilename() : filename));
		if (status == Status::OK)
		{
			status = Status::RETRY;
			StartRecordImpl(std::move(filename), width, height, config, std::move(task));
			status = Status::OK;
		}
	}
}

void CVideoRecorder::StartRecord(std::wstring filename, unsigned int width, unsigned int height, const EncodeConfig &config)
{
	StartRecordImpl(std::move(filename), width, height, config);
}

void CVideoRecorder::StopRecord()
{
	try
	{
		auto task = std::make_unique<CStopVideoRecordRequest>(videoRecordStarted);
		std::lock_guard<decltype(mtx)> lck(mtx);
		taskQueue.push_back(std::move(task));
		workerCondition = WorkerCondition::DO_JOB;
		workerEvent.notify_all();
		videoRecordStarted = false;
	}
	catch (const std::system_error &error)
	{
		Error(error);
	}
	catch (const std::exception &error)
	{
		Error(error, "Fail to stop video record");
		if (status == Status::OK)
		{
			status = Status::CLEAN;
			StopRecord();
			status = Status::OK;
		}
	}
}

void CVideoRecorder::Screenshot(std::wstring filename)
{
	try
	{
		screenshotPaths.push(std::move(filename));
	}
	// locks does not happen here => no need to catch 'std::system_error'
	catch (const std::exception &error)
	{
		Error(error, screenshotErrorMsgPrefix, &filename);
		if (status == Status::OK)
		{
			status = Status::RETRY;
			Screenshot(std::move(filename));
			status = Status::OK;
		}
	}
}