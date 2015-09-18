#pragma once

#include <array>
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

	struct TFrame
	{
		std::unique_ptr<std::array<uint8_t, 4> []> pixels;
		unsigned int width, height;
		decltype(screenshotPaths) screenshotPaths;
		std::conditional<std::is_floating_point<TFrameDuration::rep>::value, unsigned long long int, TFrameDuration::rep>::type videoPendingFrames;

		// TODO: remove after transition to VS 2015 toolset which generates it automatically
	public:
		TFrame(decltype(pixels) &&pixels, unsigned int width, unsigned int height, decltype(screenshotPaths) &&screenshotPaths, decltype(videoPendingFrames) &&videoPendingFrames) :
			pixels(std::move(pixels)), width(width), height(height), screenshotPaths(std::move(screenshotPaths)), videoPendingFrames(std::move(videoPendingFrames))
		{}
		TFrame(TFrame &&src) :
			pixels(std::move(src.pixels)),
			width(src.width), height(src.height),
			screenshotPaths(std::move(src.screenshotPaths)),
			videoPendingFrames(std::move(src.videoPendingFrames))
		{}
		TFrame &operator =(TFrame &&src)
		{
			pixels = std::move(src.pixels);
			width = src.width, height = src.height;
			screenshotPaths = std::move(src.screenshotPaths);
			videoPendingFrames = std::move(src.videoPendingFrames);
			return *this;
		}
	};
	std::queue<TFrame> frameQueue;
	typedef decltype(decltype(frameQueue)::value_type::pixels) TPixels;

	std::recursive_mutex mtx, videoFileMtx;
	std::condition_variable_any workerEvent;
	enum class WorkerCondition : uint_least8_t
	{
		WAIT,
		DO_JOB,
		FINISH,
	} workerCondition = WorkerCondition::WAIT;
	std::thread worker;

private:
	void Process();

public:
	CVideoRecorder();
	~CVideoRecorder();

public:
	void Draw(unsigned int width, unsigned int height, const std::function<void (TPixels::pointer)> &GetPixelsCallback);
	
	void StartRecord(unsigned int width, unsigned int height, const wchar_t filename[]);
	void StopRecord();

	void Screenshot(std::wstring &&filename);
	template<typename String>
	void Screenshot(String &&filename)
	{
		Screenshot(std::wstring(std::forward<String>(filename)));
	}
};