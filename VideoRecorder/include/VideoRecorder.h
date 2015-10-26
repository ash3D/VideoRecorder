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
	static const/*expr*/ unsigned int fps = 30;
	static const struct AVCodec *const codec;

	struct ContextDeleter
	{
		inline void operator ()(struct AVCodecContext *context) const;
	};
	const std::unique_ptr<struct AVCodecContext, ContextDeleter> context;

	std::unique_ptr<struct SwsContext, void (*const)(struct SwsContext *swsContext)> cvtCtx;

	const std::unique_ptr<struct AVPacket> packet;

	struct FrameDeleter
	{
		void operator ()(struct AVFrame *frame) const;
	};
	std::unique_ptr<struct AVFrame, FrameDeleter> dstFrame;

	typedef std::chrono::steady_clock clock;
	typedef std::chrono::duration<clock::rep, std::ratio<1, fps>> FrameDuration;
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

public:
#	define ENCODE_PERFORMANCE_VALUES (placebo)(veryslow)(slower)(slow)(medium)(fast)(faster)(veryfast)(superfast)(ultrafast)
	struct EncodeConfig
	{
		int64_t crf;
		enum class Performance
		{
			BOOST_PP_SEQ_ENUM(ENCODE_PERFORMANCE_VALUES)
		} performance;
	};

private:
	static inline const char *EncodePerformance_2_Str(EncodeConfig::Performance performance);
	void KillRecordSession();
#if defined _MSC_VER && _MSC_VER < 1900
	__declspec(noreturn)
#else
	[[noreturn]]
#endif
	void Error(const std::system_error &error);
	void Error(const std::exception &error, const char errorMsgPrefix[], const std::wstring *filename = nullptr);
	void StartRecordImpl(std::wstring filename, unsigned int width, unsigned int height, bool _10bit, const EncodeConfig &config, std::unique_ptr<CStartVideoRecordRequest> &&task = nullptr);
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
	class CFrame
	{
		friend class CVideoRecorder;

	private:
		CVideoRecorder &parent;
		decltype(screenshotPaths) screenshotPaths;
		std::conditional<std::is_floating_point<FrameDuration::rep>::value, uintmax_t, FrameDuration::rep>::type videoPendingFrames;
		bool ready = false;

	public:
		typedef std::tuple<decltype(parent), decltype(screenshotPaths), decltype(videoPendingFrames)> &&Opaque;

	protected:
		CFrame(Opaque opaque);
		CFrame(CFrame &) = delete;
		void operator =(CFrame &) = delete;
		virtual ~CFrame() = default;

	public:
		void Ready(), Cancel();

	public:
		virtual struct FrameData
		{
			enum class Format
			{
				B8G8R8A8,
				R10G10B10A2,
			} format;
			unsigned int width, height;
			size_t stride;
			const void *pixels;
		} GetFrameData() const = 0;
	};

public:
	void SampleFrame(const std::function<std::shared_ptr<CFrame> (CFrame::Opaque)> &RequestFrameCallback);
	void StartRecord(std::wstring filename, unsigned int width, unsigned int height, bool _10bit/*8 if false*/, const EncodeConfig &config = { -1 });
	void StopRecord();
	void Screenshot(std::wstring filename);
};