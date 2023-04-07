#pragma once
#ifdef XR_USE_PLATFORM_WIN32
#ifdef ENGINE_DLL_EXPORTS
    /*Enabled as "export" while compiling the dll project*/
    #define DLLEXPORT __declspec(dllexport)  
#else
    /*Enabled as "import" in the Client side for using already created dll file*/
    #define DLLEXPORT __declspec(dllimport)  
#endif
#else
#define DLLEXPORT
#endif

#include "alxr_ctypes.h"

#ifdef __cplusplus
extern "C" {;
#endif

DLLEXPORT bool alxr_init(const ALXRRustCtx*, /*[out]*/ ALXRSystemProperties* systemProperties);
DLLEXPORT void alxr_destroy();
DLLEXPORT void alxr_request_exit_session();
DLLEXPORT void alxr_process_frame(bool* exitRenderLoop /*= non-null */, bool* requestRestart /*= non-null */);
DLLEXPORT bool alxr_is_session_running();

DLLEXPORT void alxr_set_stream_config(const ALXRStreamConfig config);
DLLEXPORT ALXRGuardianData alxr_get_guardian_data();

DLLEXPORT void alxr_on_receive(const unsigned char* packet, unsigned int packetSize);
DLLEXPORT void alxr_on_tracking_update(const bool clientsidePrediction);
DLLEXPORT void alxr_on_haptics_feedback(unsigned long long path, float duration_s, float frequency, float amplitude);
DLLEXPORT void alxr_on_server_disconnect();
DLLEXPORT void alxr_on_pause();
DLLEXPORT void alxr_on_resume();

DLLEXPORT void alxr_set_log_custom_output(ALXRLogOptions options, ALXRLogOutputFn outputFn);

#ifdef __cplusplus
}
#endif
