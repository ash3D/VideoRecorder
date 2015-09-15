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
#include <cstdint>

class CVideoRecorder
{
	static struct AVCodec *const codec;

	struct TContextDeleter
	{
		inline void operator ()(struct AVCodecContext *context) const;
	};
	const std::unique_ptr<struct AVCodecContext, TContextDeleter> context;

	std::unique_ptr<struct SwsContext, void (*const)(struct SwsContext *swsContext)> cvtCtx;

	std::queue<std::vector<std::array<uint8_t, 4>>> frameQueue;

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

	std::thread worker;

private:
	void UpdatePixelData(unsigned int width, unsigned int height, const std::function<void (decltype(frameQueue)::value_type::pointer)> &GetPixelsCallback);
	void Process();

public:
	CVideoRecorder();
	~CVideoRecorder();

public:
	void Draw(unsigned int width, unsigned int height, const std::function<void (decltype(frameQueue)::value_type::pointer)> &GetPixelsCallback);
	
	void StartRecord(unsigned int width, unsigned int height, const wchar_t filename[]);
	void StopRecord();

	template<typename String>
	void Screenshot(String &&filename)
	{
		screenshotPaths.push(std::forward<String>(filename));
	}
};