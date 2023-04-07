#pragma once
#ifndef ALXR_DECODER_THREAD_H
#define ALXR_DECODER_THREAD_H

#include <memory>
#include <atomic>
#include <thread>

#include "alxr_ctypes.h"
#include "ALVR-common/packet_types.h"
#include "fec.h"

struct IDecoderPlugin;
struct IOpenXrProgram;

class XrDecoderThread {
	using DecoderPluginPtr = std::shared_ptr<IDecoderPlugin>;
	using FECQueuePtr = std::shared_ptr<FECQueue>;
	using CodecType = std::atomic<ALVR_CODEC>;

	DecoderPluginPtr  m_decoderPlugin{ nullptr };
	FECQueuePtr		  m_fecQueue{ nullptr };
	std::atomic<bool> m_isRuningToken{ false };
	std::thread		  m_decoderThread;

public:

	inline XrDecoderThread() = default;
	
	inline XrDecoderThread(XrDecoderThread&&) = default;
	inline XrDecoderThread& operator=(XrDecoderThread&&) = default;

	inline XrDecoderThread(const XrDecoderThread&) = delete;
	inline XrDecoderThread& operator=(const XrDecoderThread&) = delete;

	inline ~XrDecoderThread() {
		Stop();
	}

	struct StartCtx {
		using IOpenXrProgramPtr = std::shared_ptr<IOpenXrProgram>;
		using ALXRRustCtxPtr	= std::shared_ptr<const ALXRRustCtx>;

		ALXRDecoderConfig decoderConfig;
		IOpenXrProgramPtr programPtr;
		ALXRRustCtxPtr	  rustCtx;
	};
	void Start(const StartCtx& ctx);
	void Stop();
	bool QueuePacket(const VideoFrame& header, const std::size_t packetSize);
};
#endif
