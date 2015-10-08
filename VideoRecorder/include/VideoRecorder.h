#pragma once

#include <array>
#include <vector>
#include <string>
#include <queue>
#include <memory>
#include <utility>
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

	typedef std::unique_ptr<std::array<uint8_t, 4> []> TPixels;

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

	void Draw(unsigned int width, unsigned int height, const std::function<void (TPixels::pointer)> &GetPixelsCallback);
	
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