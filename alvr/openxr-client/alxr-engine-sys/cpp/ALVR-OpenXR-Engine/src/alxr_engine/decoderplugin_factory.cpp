#include "decoderplugin.h"

#ifdef XR_USE_PLATFORM_ANDROID
std::shared_ptr<IDecoderPlugin> CreateDecoderPlugin_MediaCodec();
#elif 1
std::shared_ptr<IDecoderPlugin> CreateDecoderPlugin_FFMPEG();
#else
std::shared_ptr<IDecoderPlugin> CreateDecoderPlugin_Dummy();
#endif

std::shared_ptr<IDecoderPlugin> CreateDecoderPlugin() {
#ifdef XR_USE_PLATFORM_ANDROID
	return CreateDecoderPlugin_MediaCodec();
#elif 1
	return CreateDecoderPlugin_FFMPEG();
#else
	return CreateDecoderPlugin_Dummy();
#endif
}
