#include "VideoEncoderVCE.h"

#include "alvr_server/Statistics.h"
#include "alvr_server/Logger.h"
#include "alvr_server/Settings.h"

#define AMF_THROW_IF(expr) {AMF_RESULT res = expr;\
if(res != AMF_OK){throw MakeException("AMF Error %d. %s", res, L#expr);}}

const wchar_t *VideoEncoderVCE::START_TIME_PROPERTY = L"StartTimeProperty";
const wchar_t *VideoEncoderVCE::FRAME_INDEX_PROPERTY = L"FrameIndexProperty";

//
// AMFTextureEncoder
//

AMFTextureEncoder::AMFTextureEncoder(const amf::AMFContextPtr &amfContext
	, int codec, int width, int height, int refreshRate, int bitrateInMbits
	, amf::AMF_SURFACE_FORMAT inputFormat
	, AMFTextureReceiver receiver) : m_receiver(receiver)
{
	const wchar_t *pCodec;

	amf_int32 frameRateIn = refreshRate;
	amf_int64 bitRateIn = bitrateInMbits * 1000000L; // in bits

	switch (codec) {
	case ALVR_CODEC_H264:
		pCodec = AMFVideoEncoderVCE_AVC;
		break;
	case ALVR_CODEC_H265:
		pCodec = AMFVideoEncoder_HEVC;
		break;
	default:
		throw MakeException("Unsupported video encoding %d", codec);
	}

	// Create encoder component.
	AMF_THROW_IF(g_AMFFactory.GetFactory()->CreateComponent(amfContext, pCodec, &m_amfEncoder));

	if (codec == ALVR_CODEC_H264)
	{
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY);
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitRateIn);
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, ::AMFConstructSize(width, height));
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, ::AMFConstructRate(frameRateIn, 1));
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0);
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_HIGH);
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE_LEVEL, 51);
		
		//No noticable visual difference between PRESET_QUALITY and PRESET_SPEED but the latter has better latency when the GPU is under heavy load
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);

		//No noticable performance difference and should improve subjective quality by allocating more bits to smooth areas
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_ENABLE_VBAQ, true);
		
		//Fixes rythmic pixelation. I-frames were overcompressed on default settings
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_MAX_QP, 30);

		//Does not seem to make a difference but turned on anyway in case it does on other hardware
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
	}
	else
	{
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY);
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bitRateIn);
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, ::AMFConstructSize(width, height));
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, ::AMFConstructRate(frameRateIn, 1));	

		//No noticable visual difference between PRESET_QUALITY and PRESET_SPEED but the latter has better latency when the GPU is under heavy load
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED);	

		//No noticable performance difference and should improve subjective quality by allocating more bits to smooth areas
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, true);

		//Fixes rythmic pixelation. I-frames were overcompressed on default settings
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_MAX_QP_I, 30);

		//Does not seem to make a difference but turned on anyway in case it does on other hardware
		m_amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, true);
	}
	AMF_THROW_IF(m_amfEncoder->Init(inputFormat, width, height));

	Debug("Initialized AMFTextureEncoder.\n");
}

AMFTextureEncoder::~AMFTextureEncoder()
{
}

void AMFTextureEncoder::Start()
{
	m_thread = new std::thread(&AMFTextureEncoder::Run, this);
}

void AMFTextureEncoder::Shutdown()
{
	Debug("AMFTextureEncoder::Shutdown() m_amfEncoder->Drain\n");
	m_amfEncoder->Drain();
	Debug("AMFTextureEncoder::Shutdown() m_thread->join\n");
	m_thread->join();
	Debug("AMFTextureEncoder::Shutdown() joined.\n");
	delete m_thread;
	m_thread = NULL;
}

void AMFTextureEncoder::Submit(amf::AMFData *data)
{
	while (true)
	{
		Debug("AMFTextureEncoder::Submit.\n");
		auto res = m_amfEncoder->SubmitInput(data);
		if (res == AMF_INPUT_FULL)
		{
			return;
		}
		else
		{
			break;
		}
	}
}

amf::AMFComponentPtr AMFTextureEncoder::Get()
{
	return m_amfEncoder;
}

void AMFTextureEncoder::Run()
{
	Debug("Start AMFTextureEncoder thread. Thread Id=%d\n", GetCurrentThreadId());
	amf::AMFDataPtr data;
	while (true)
	{
		auto res = m_amfEncoder->QueryOutput(&data);
		if (res == AMF_EOF)
		{
			Warn("m_amfEncoder->QueryOutput returns AMF_EOF.\n");
			return;
		}

		if (data != NULL)
		{
			m_receiver(data);
		}
		else
		{
			Sleep(1);
		}
	}
}

//
// AMFTextureConverter
//

AMFTextureConverter::AMFTextureConverter(const amf::AMFContextPtr &amfContext
	, int width, int height
	, amf::AMF_SURFACE_FORMAT inputFormat, amf::AMF_SURFACE_FORMAT outputFormat
	, AMFTextureReceiver receiver) : m_receiver(receiver)
{
	AMF_THROW_IF(g_AMFFactory.GetFactory()->CreateComponent(amfContext, AMFVideoConverter, &m_amfConverter));

	AMF_THROW_IF(m_amfConverter->SetProperty(AMF_VIDEO_CONVERTER_MEMORY_TYPE, amf::AMF_MEMORY_DX11));
	AMF_THROW_IF(m_amfConverter->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, outputFormat));
	AMF_THROW_IF(m_amfConverter->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_SIZE, ::AMFConstructSize(width, height)));

	AMF_THROW_IF(m_amfConverter->Init(inputFormat, width, height));

	Debug("Initialized AMFTextureConverter.\n");
}

AMFTextureConverter::~AMFTextureConverter()
{
}

void AMFTextureConverter::Start()
{
	m_thread = new std::thread(&AMFTextureConverter::Run, this);
}

void AMFTextureConverter::Shutdown()
{
	Debug("AMFTextureConverter::Shutdown() m_amfConverter->Drain\n");
	m_amfConverter->Drain();
	Debug("AMFTextureConverter::Shutdown() m_thread->join\n");
	m_thread->join();
	Debug("AMFTextureConverter::Shutdown() joined.\n");
	delete m_thread;
	m_thread = NULL;
}

void AMFTextureConverter::Submit(amf::AMFData *data)
{
	while (true)
	{
		Debug("AMFTextureConverter::Submit.\n");
		auto res = m_amfConverter->SubmitInput(data);
		if (res == AMF_INPUT_FULL)
		{
			return;
		}
		else
		{
			break;
		}
	}
}

void AMFTextureConverter::Run()
{
	Debug("Start AMFTextureConverter thread. Thread Id=%d\n", GetCurrentThreadId());
	amf::AMFDataPtr data;
	while (true)
	{
		auto res = m_amfConverter->QueryOutput(&data);
		if (res == AMF_EOF)
		{
			Warn("m_amfConverter->QueryOutput returns AMF_EOF.\n");
			return;
		}

		if (data != NULL)
		{
			m_receiver(data);
		}
		else
		{
			Sleep(1);
		}
	}
}

//
// VideoEncoderVCE
//

VideoEncoderVCE::VideoEncoderVCE(std::shared_ptr<CD3DRender> d3dRender
	, std::shared_ptr<ClientConnection> listener
	, int width, int height)
	: m_d3dRender(d3dRender)
	, m_Listener(listener)
	, m_codec(Settings::Instance().m_codec)
	, m_refreshRate(Settings::Instance().m_refreshRate)
	, m_renderWidth(width)
	, m_renderHeight(height)
	, m_bitrateInMBits(Settings::Instance().mEncodeBitrateMBs)
{
}

VideoEncoderVCE::~VideoEncoderVCE()
{}

void VideoEncoderVCE::Initialize()
{
	Debug("Initializing VideoEncoderVCE.\n");
	AMF_THROW_IF(g_AMFFactory.Init());

	::amf_increase_timer_precision();

	AMF_THROW_IF(g_AMFFactory.GetFactory()->CreateContext(&m_amfContext));
	AMF_THROW_IF(m_amfContext->InitDX11(m_d3dRender->GetDevice()));

	m_encoder = std::make_shared<AMFTextureEncoder>(m_amfContext
		, m_codec, m_renderWidth, m_renderHeight, m_refreshRate, m_bitrateInMBits
		, ENCODER_INPUT_FORMAT, std::bind(&VideoEncoderVCE::Receive, this, std::placeholders::_1));
	m_converter = std::make_shared<AMFTextureConverter>(m_amfContext
		, m_renderWidth, m_renderHeight
		, CONVERTER_INPUT_FORMAT, ENCODER_INPUT_FORMAT
		, std::bind(&AMFTextureEncoder::Submit, m_encoder.get(), std::placeholders::_1));

	m_encoder->Start();
	m_converter->Start();

	Debug("Successfully initialized VideoEncoderVCE.\n");
}

void VideoEncoderVCE::Shutdown()
{
	Debug("Shutting down VideoEncoderVCE.\n");

	m_encoder->Shutdown();
	m_converter->Shutdown();

	amf_restore_timer_precision();

	if (fpOut) {
		fpOut.close();
	}
	Debug("Successfully shutdown VideoEncoderVCE.\n");
}

void VideoEncoderVCE::Transmit(ID3D11Texture2D *pTexture, uint64_t presentationTime, uint64_t targetTimestampNs, bool insertIDR)
{
	amf::AMFSurfacePtr surface;
	// Surface is cached by AMF.

	if (m_Listener) {
		if (m_Listener->GetStatistics()->CheckBitrateUpdated()) {
			m_bitrateInMBits = m_Listener->GetStatistics()->GetBitrate();
			amf_int64 bitRateIn = m_bitrateInMBits * 1000000L; // in bits
			if (m_codec == ALVR_CODEC_H264)
			{
				m_encoder->Get()->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitRateIn);
			}
			else
			{
				m_encoder->Get()->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bitRateIn);
			}
		}
	}

	AMF_THROW_IF(m_amfContext->AllocSurface(amf::AMF_MEMORY_DX11, CONVERTER_INPUT_FORMAT, m_renderWidth, m_renderHeight, &surface));
	ID3D11Texture2D *textureDX11 = (ID3D11Texture2D*)surface->GetPlaneAt(0)->GetNative(); // no reference counting - do not Release()
	m_d3dRender->GetContext()->CopyResource(textureDX11, pTexture);

	amf_pts start_time = amf_high_precision_clock();
	surface->SetProperty(START_TIME_PROPERTY, start_time);
	surface->SetProperty(FRAME_INDEX_PROPERTY, targetTimestampNs);

	ApplyFrameProperties(surface, insertIDR);

	m_converter->Submit(surface);
}

void VideoEncoderVCE::Receive(amf::AMFData *data)
{
	amf_pts current_time = amf_high_precision_clock();
	amf_pts start_time = 0;
	uint64_t targetTimestampNs;
	data->GetProperty(START_TIME_PROPERTY, &start_time);
	data->GetProperty(FRAME_INDEX_PROPERTY, &targetTimestampNs);

	amf::AMFBufferPtr buffer(data); // query for buffer interface

	if (m_Listener) {
		m_Listener->GetStatistics()->EncodeOutput((current_time - start_time) / MICROSEC_TIME);
	}

	char *p = reinterpret_cast<char *>(buffer->GetNative());
	int length = static_cast<int>(buffer->GetSize());

	SkipAUD(&p, &length);

	if (fpOut) {
		fpOut.write(p, length);
	}
	if (m_Listener) {
		m_Listener->SendVideo(reinterpret_cast<uint8_t *>(p), length, targetTimestampNs);
	}
}

void VideoEncoderVCE::ApplyFrameProperties(const amf::AMFSurfacePtr &surface, bool insertIDR) {
	switch (m_codec) {
	case ALVR_CODEC_H264:
		// Disable AUD (NAL Type 9) to produce the same stream format as VideoEncoderNVENC.
		surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_AUD, false);
		if (insertIDR) {
			Debug("Inserting IDR frame for H.264.\n");
			surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
			surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
			surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
		}
		break;
	case ALVR_CODEC_H265:
		// This option is ignored. Maybe a bug on AMD driver.
		surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_AUD, false);
		if (insertIDR) {
			Debug("Inserting IDR frame for H.265.\n");
			// Insert VPS,SPS,PPS
			// These options don't work properly on older AMD driver (Radeon Software 17.7, AMF Runtime 1.4.4)
			// Fixed in 18.9.2 & 1.4.9
			surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
			surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
		}
		break;
	}
}

void VideoEncoderVCE::SkipAUD(char **buffer, int *length) {
	// H.265 encoder always produces AUD NAL even if AMF_VIDEO_ENCODER_HEVC_INSERT_AUD is set. But it is not needed.
	static const int AUD_NAL_SIZE = 7;

	if (m_codec != ALVR_CODEC_H265) {
		return;
	}

	if (*length < AUD_NAL_SIZE + 4) {
		return;
	}

	// Check if start with AUD NAL.
	if (memcmp(*buffer, "\x00\x00\x00\x01\x46", 5) != 0) {
		return;
	}
	// Check if AUD NAL size is AUD_NAL_SIZE bytes.
	if (memcmp(*buffer + AUD_NAL_SIZE, "\x00\x00\x00\x01", 4) != 0) {
		return;
	}
	*buffer += AUD_NAL_SIZE;
	*length -= AUD_NAL_SIZE;
}
