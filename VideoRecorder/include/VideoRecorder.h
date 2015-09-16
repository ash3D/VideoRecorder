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
#include <thread>
#include <mutex>
#include <cstdint>

class CVideoRecorder
{
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
	std::unique_ptr<struct AVFrame, TFrameDeleter> frame;

	typedef std::chrono::steady_clock clock;
	clock::time_point nextFrame;

	std::ofstream videoFile;

	std::queue<std::wstring> screenshotPaths;

	struct TFrame
	{
		std::unique_ptr<std::array<uint8_t, 4> []> pixels;
		unsigned int width, height;
		decltype(screenshotPaths) screenshotPaths;
		bool video;

		// remove after transition to VS 2015 toolset which generates it automatically
	public:
		TFrame(TFrame &&) = default;
		TFrame &operator =(TFrame &&) = default;
	};
	std::queue<TFrame> frameQueue;
	typedef decltype(decltype(frameQueue)::value_type::pixels) TPixels;

	std::thread worker;
	std::mutex mtx;

private:
	void EnqueueFrame(unsigned int width, unsigned int height, const std::function<void (TPixels::pointer)> &GetPixelsCallback);
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