#pragma once

#include <vector>
#include <string>
#include <queue>
#include <deque>
#include <memory>
#include <utility>
#include <tuple>
#include <type_traits>
#include <functional>
#include <fstream>
#include <iostream>
#include <chrono>
#include <ratio>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <exception>
#include <system_error>
#include <cstdint>
#include <boost/preprocessor/seq/enum.hpp>

#if defined _MSC_VER && _MSC_VER < 1900
#define noexcept
#endif

class CVideoRecorder
{
	static const/*expr*/ unsigned int fps = 25;
	static const struct AVCodec *const codec;

	struct TContextDeleter
	{
		inline void operator ()(struct AVCodecContext *context) const;
	};
	const std::unique_ptr<struct AVCodecContext, TContextDeleter> context;

	std::unique_ptr<struct SwsContext, void (*const)(struct SwsContext *swsContext)> cvtCtx;

	const std::unique_ptr<struct AVPacket> packet;

	struct TFrameDeleter
	{
		void operator ()(struct AVFrame *frame) const;
	};
	std::unique_ptr<struct AVFrame, TFrameDeleter> dstFrame;

	typedef std::chrono::steady_clock clock;
	typedef std::chrono::duration<clock::rep, std::ratio<1, fps>> TFrameDuration;
	clock::time_point nextFrame;

	std::ofstream videoFile;

	std::queue<std::wstring> screenshotPaths;

	struct ITask;
	class CFrameTask;
	class CStartVideoRecordRequest;
	class CStopVideoRecordRequest;
	std::deque<std::unique_ptr<ITask>> taskQueue;

	std::mutex mtx;
	std::condition_variable workerEvent;
	std::thread worker;
	enum class WorkerCondition : uint_least8_t
	{
		WAIT,
		DO_JOB,
		FINISH,
	} workerCondition = WorkerCondition::WAIT;

	enum class Status : uint_least8_t
	{
		OK,
		RETRY,
		CLEAN,
	} status = Status::OK;

	bool videoRecordStarted = false;

	static const/*expr*/ char *const screenshotErrorMsgPrefix;

public:
	enum class EncodePerformance;
private:
	struct TEncodeConfig
	{
		int64_t crf;
		EncodePerformance performance;
	};

private:
	template<typename Char>
	static const Char *c_str(const Char *str) noexcept { return str; }
	template<typename Char, class CharTraits, class Allocator>
	static const Char *c_str(const std::basic_string<Char, CharTraits, Allocator> &str) noexcept { return str.c_str(); }
	static inline const char *EncodePerformance_2_Str(EncodePerformance performance);
	void KillRecordSession();
#if defined _MSC_VER && _MSC_VER < 1900
	__declspec(noreturn)
#else
	[[noreturn]]
#endif
	void Error(const std::system_error &error);
	void Error(const std::exception &error, const char errorMsgPrefix[], const wchar_t *filename = nullptr);
	template<typename String>
	void StartRecordImpl(String &&filename, unsigned int width, unsigned int height, const TEncodeConfig &config, std::unique_ptr<CStartVideoRecordRequest> &&task = nullptr);
	void StartRecordImpl(std::wstring &&filename, unsigned int width, unsigned int height, const TEncodeConfig &config, std::unique_ptr<CStartVideoRecordRequest> &&task);
	void ScreenshotImpl(std::wstring &&filename);
	void Process();

public:
	CVideoRecorder();
#if defined _MSC_VER && _MSC_VER < 1900
	CVideoRecorder(CVideoRecorder &) = delete;
	void operator =(CVideoRecorder &) = delete;
#else
	CVideoRecorder(CVideoRecorder &&);
#endif
	~CVideoRecorder();

public:
#	define ENCODE_PERFORMANCE_VALUES (placebo)(veryslow)(slower)(slow)(medium)(fast)(faster)(veryfast)(superfast)(ultrafast)
	enum class EncodePerformance
	{
		BOOST_PP_SEQ_ENUM(ENCODE_PERFORMANCE_VALUES)
	};

	class CFrame
	{
		friend class CVideoRecorder;

	private:
		CVideoRecorder &parent;
		decltype(screenshotPaths) screenshotPaths;
		std::conditional<std::is_floating_point<TFrameDuration::rep>::value, uintmax_t, TFrameDuration::rep>::type videoPendingFrames;
		bool ready = false;

	public:
		typedef std::tuple<decltype(parent), decltype(screenshotPaths), decltype(videoPendingFrames)> &&TOpaque;

	protected:
		CFrame(TOpaque opaque);
		CFrame(CFrame &) = delete;
		void operator =(CFrame &) = delete;
		virtual ~CFrame() = default;

	public:
		void Ready(), Cancel();

	public:
		virtual struct TFrameData
		{
			unsigned int width, height;
			size_t stride;
			const void *pixels;
		} GetFrameData() const = 0;
	};

public:
	void SampleFrame(const std::function<std::shared_ptr<CFrame> (CFrame::TOpaque)> &RequestFrameCallback);
	
	template<typename String>
	void StartRecord(String &&filename, unsigned int width, unsigned int height);

	template<typename String>
	void StartRecord(String &&filename, unsigned int width, unsigned int height, EncodePerformance performance, int64_t crf);

	void StopRecord();

	template<typename String>
	void Screenshot(String &&filename);
};

#pragma region template
template<typename String>
void CVideoRecorder::StartRecordImpl(String &&filename, unsigned int width, unsigned int height, const TEncodeConfig &config, std::unique_ptr<CStartVideoRecordRequest> &&task)
{
	std::unique_ptr<CStartVideoRecordRequest> task;
	try
	{
		StartRecordImpl(task ? (std::wstring &&)task->filename : std::wstring(std::forward<String>(filename)), width, height, config, std::move(task));
	}
	catch (const std::system_error &error)
	{
		Error(error);
	}
	catch (const std::exception &error)
	{
		Error(error, "Fail to start video record ", c_str(task ? task->filename : filename));
		if (status == Status::OK)
		{
			status = Status::RETRY;
			StartRecordImpl<>(std::forward<String>(filename), width, height, config, std::move(task));
			status = Status::OK;
		}
	}
}

template<typename String>
inline void CVideoRecorder::StartRecord(String &&filename, unsigned int width, unsigned int height)
{
	StartRecordImpl(std::forward<String>(filename), width, height, { -1 });
}

template<typename String>
inline void CVideoRecorder::StartRecord(String &&filename, unsigned int width, unsigned int height, EncodePerformance performance, int64_t crf)
{
	StartRecordImpl(std::forward<String>(filename), width, height, { crf, performance });
}

template<typename String>
void CVideoRecorder::Screenshot(String &&filename)
{
	try
	{
		ScreenshotImpl(std::wstring(std::forward<String>(filename)));
	}
	// locks does not happen here => no need to catch 'std::system_error'
	catch (const std::exception &error)
	{
		Error(error, screenshotErrorMsgPrefix, c_str(filename));
		if (status == Status::OK)
		{
			status = Status::RETRY;
			Screenshot(std::forward<String>(filename));
			status = Status::OK;
		}
	}
}
#pragma endregion implementation