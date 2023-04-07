#include "CEncoder.h"

//SHN 2020-3-6
		CEncoder::CEncoder()
			: m_bExiting(false)
			, m_targetTimestampNs(0)
		{
			m_encodeFinished.Set();
		}

		
			CEncoder::~CEncoder()
		{
			if (m_videoEncoder)
			{
				m_videoEncoder->Shutdown();
				m_videoEncoder.reset();
			}
		}

		void CEncoder::Initialize(std::shared_ptr<CD3DRender> d3dRender, std::shared_ptr<ClientConnection> listener) {
			m_FrameRender = std::make_shared<FrameRender>(d3dRender);
			m_FrameRender->Startup();
			uint32_t encoderWidth, encoderHeight;
			m_FrameRender->GetEncodingResolution(&encoderWidth, &encoderHeight);

			Exception vceException;
			Exception nvencException;
#ifdef ALVR_GPL
			Exception swException;
#endif
			try {
				Debug("Try to use VideoEncoderVCE.\n");
				m_videoEncoder = std::make_shared<VideoEncoderVCE>(d3dRender, listener, encoderWidth, encoderHeight);
				m_videoEncoder->Initialize();
				return;
			}
			catch (Exception e) {
				vceException = e;
			}
			try {
				Debug("Try to use VideoEncoderNVENC.\n");
				m_videoEncoder = std::make_shared<VideoEncoderNVENC>(d3dRender, listener, encoderWidth, encoderHeight);
				m_videoEncoder->Initialize();
				return;
			}
			catch (Exception e) {
				nvencException = e;
			}
#ifdef ALVR_GPL
			try {
				Debug("Try to use VideoEncoderSW.\n");
				m_videoEncoder = std::make_shared<VideoEncoderSW>(d3dRender, listener, encoderWidth, encoderHeight);
				m_videoEncoder->Initialize();
				return;
			}
			catch (Exception e) {
				swException = e;
			}
			throw MakeException("All VideoEncoder are not available. VCE: %s, NVENC: %s, SW: %s", vceException.what(), nvencException.what(), swException.what());
#else
			throw MakeException("All VideoEncoder are not available. VCE: %s, NVENC: %s", vceException.what(), nvencException.what());
#endif
		}
//SHN  :编码前准备
		bool CEncoder::CopyToStaging(ID3D11Texture2D *pTexture[][2], vr::VRTextureBounds_t bounds[][2], int layerCount, bool recentering
			, uint64_t presentationTime, uint64_t targetTimestampNs, const std::string& message, const std::string& debugText)
		{
			m_presentationTime = presentationTime;
			m_targetTimestampNs = targetTimestampNs;
			m_FrameRender->Startup();  // 启动帧渲染相应的模块，是仅针对一帧画面的吗？每一帧都要重新开始初始化一下
			//其实这两个模块写成一个函数也可以 前边都是初始 内容：销毁以前的渲染数据 创建相关3D11Render 里的变量 
			//然后RenderFrame 进行变量的读写    
			m_FrameRender->RenderFrame(pTexture, bounds, layerCount, recentering, message, debugText);
			// SHN：保存深度信息
			m_FrameRender->SaveDepth(presentationTime,targetTimestampNs);
			return true;
		}

		void CEncoder::Run()
		{
			Debug("CEncoder: Start thread. Id=%d\n", GetCurrentThreadId());
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_MOST_URGENT);

			while (!m_bExiting)
			{
				Debug("CEncoder: Waiting for new frame...\n");

				m_newFrameReady.Wait();
				if (m_bExiting)
					break;

				if (m_FrameRender->GetTexture())//执行获取纹理
				{   //这里是在渲染进行完成后 将渲染后的数据 传输给编码器 第一项是纹理（ 双眼的）
					m_videoEncoder->Transmit(m_FrameRender->GetTexture().Get(), m_presentationTime, m_targetTimestampNs, m_scheduler.CheckIDRInsertion());
					//SHN：再传输一个深度图：

				}

				m_encodeFinished.Set();
			}
		}

		void CEncoder::Stop()
		{
			m_bExiting = true;
			m_newFrameReady.Set();
			Join();
			m_FrameRender.reset();
		}

		void CEncoder::NewFrameReady()
		{
			Debug("New Frame Ready\n");
			m_encodeFinished.Reset();
			m_newFrameReady.Set();
		}

		void CEncoder::WaitForEncode()
		{
			m_encodeFinished.Wait();
		}

		void CEncoder::OnStreamStart() {
			m_scheduler.OnStreamStart();
		}

		void CEncoder::OnPacketLoss() {
			m_scheduler.OnPacketLoss();
		}

		void CEncoder::InsertIDR() {
			m_scheduler.InsertIDR();
		}