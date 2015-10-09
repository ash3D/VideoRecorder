#pragma once

#include <vector>
#include <string>
#include <queue>
#include <memory>
#include <utility>
#include <type_traits>
#include <functional>
#include <fstream>
#include <chrono>
#include <ratio>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <boost/preprocessor/seq/enum.hpp>

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
	std::queue<std::unique_ptr<ITask>> taskQueue;

	std::mutex mtx;
	std::condition_variable workerEvent;
	enum class WorkerCondition : uint_least8_t
	{
		WAIT,
		DO_JOB,
		FINISH,
	} workerCondition = WorkerCondition::WAIT;
	std::thread worker;

	bool videoRecordStarted = false;

public:
	enum class EncodePerformance;
private:
	struct TEncodeConfig
	{
		int64_t crf;
		EncodePerformance performance;
	};
	static inline const char *EncodePerformance_2_Str(EncodePerformance performance);
	void StartRecordImpl(std::wstring &&filename, unsigned int width, unsigned int height, const TEncodeConfig &config);
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
		decltype(screenshotPaths) screenshotPaths;
		std::conditional<std::is_floating_point<TFrameDuration::rep>::value, uintmax_t, TFrameDuration::rep>::type videoPendingFrames;

	public:
		typedef std::pair<decltype(screenshotPaths), decltype(videoPendingFrames)> &&TOpaque;
		CFrame(TOpaque opaque);
		CFrame(CFrame &) = delete;
		void operator =(CFrame &) = delete;
		virtual ~CFrame() = default;

	public:
		virtual struct TFrameData
		{
			unsigned int width, height;
			size_t stride;
			const uint8_t *pixels;
		} GetFrameData() const = 0;
	};

public:
	void Sample(const std::function<void (CFrame::TOpaque)> &RequestFrameCallback);
	void EnqueueFrame(std::shared_ptr<CFrame> frame);
	
	template<typename String>
	void StartRecord(String &&filename, unsigned int width, unsigned int height)
	{
		const TEncodeConfig config = { -1 };
		StartRecordImpl(std::wstring(std::forward<String>(filename)), width, height, config);
	}

	template<typename String>
	void StartRecord(String &&filename, unsigned int width, unsigned int height, EncodePerformance performance, int64_t crf)
	{
		const TEncodeConfig config = { crf, performance };
		StartRecordImpl(std::wstring(std::forward<String>(filename)), width, height, config);
	}

	void StopRecord();

	void Screenshot(std::wstring &&filename);

	template<typename String>
	void Screenshot(String &&filename)
	{
		Screenshot(std::wstring(std::forward<String>(filename)));
	}
};