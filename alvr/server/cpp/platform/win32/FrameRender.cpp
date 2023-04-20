#include "FrameRender.h"
#include "alvr_server/Utils.h"
#include "alvr_server/Logger.h"
#include "alvr_server/Settings.h"
#include "alvr_server/bindings.h"

extern uint64_t g_DriverTestMode;

using namespace d3d_render_utils;


FrameRender::FrameRender(std::shared_ptr<CD3DRender> pD3DRender)
	: m_pD3DRender(pD3DRender)
{
		FrameRender::SetGpuPriority(m_pD3DRender->GetDevice());
}


FrameRender::~FrameRender()
{
}

bool FrameRender::Startup()
{
	if (m_pStagingTexture) {
		return true;
	}

	//
	// Create staging texture
	// This is input texture of Video Encoder and is render target of both eyes.
	//

	D3D11_TEXTURE2D_DESC compositionTextureDesc;
	ZeroMemory(&compositionTextureDesc, sizeof(compositionTextureDesc));
	compositionTextureDesc.Width = Settings::Instance().m_renderWidth;
	compositionTextureDesc.Height = Settings::Instance().m_renderHeight;
	compositionTextureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	compositionTextureDesc.MipLevels = 1;
	compositionTextureDesc.ArraySize = 1;
	compositionTextureDesc.SampleDesc.Count = 1;
	compositionTextureDesc.Usage = D3D11_USAGE_DEFAULT;
	compositionTextureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	ComPtr<ID3D11Texture2D> compositionTexture;

	if (FAILED(m_pD3DRender->GetDevice()->CreateTexture2D(&compositionTextureDesc, NULL, &compositionTexture)))
	{
		Error("Failed to create staging texture!\n");
		return false;
	}

	HRESULT hr = m_pD3DRender->GetDevice()->CreateRenderTargetView(compositionTexture.Get(), NULL, &m_pRenderTargetView);
	if (FAILED(hr)) {
		Error("CreateRenderTargetView %p %ls\n", hr, GetErrorStr(hr).c_str());
		return false;
	}

	// Create depth stencil texture
	D3D11_TEXTURE2D_DESC descDepth;
	ZeroMemory(&descDepth, sizeof(descDepth));
	descDepth.Width = compositionTextureDesc.Width;
	descDepth.Height = compositionTextureDesc.Height;
	descDepth.MipLevels = 1;
	descDepth.ArraySize = 1;
	descDepth.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	descDepth.SampleDesc.Count = 1;
	descDepth.SampleDesc.Quality = 0;
	descDepth.Usage = D3D11_USAGE_DEFAULT;
	descDepth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	descDepth.CPUAccessFlags = 0;
	descDepth.MiscFlags = 0;
	hr = m_pD3DRender->GetDevice()->CreateTexture2D(&descDepth, nullptr, &m_pDepthStencil);
	if (FAILED(hr)) {
		Error("CreateTexture2D %p %ls\n", hr, GetErrorStr(hr).c_str());
		return false;
	}


	// Create the depth stencil view
	D3D11_DEPTH_STENCIL_VIEW_DESC descDSV;
	ZeroMemory(&descDSV, sizeof(descDSV));
	descDSV.Format = descDepth.Format;
	descDSV.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	descDSV.Texture2D.MipSlice = 0;
	hr = m_pD3DRender->GetDevice()->CreateDepthStencilView(m_pDepthStencil.Get(), &descDSV, &m_pDepthStencilView);
	if (FAILED(hr)) {
		Error("CreateDepthStencilView %p %ls\n", hr, GetErrorStr(hr).c_str());
		return false;
	}

	m_pD3DRender->GetContext()->OMSetRenderTargets(1, m_pRenderTargetView.GetAddressOf(), m_pDepthStencilView.Get());

	D3D11_VIEWPORT viewport;
	viewport.Width = (float)Settings::Instance().m_renderWidth;
	viewport.Height = (float)Settings::Instance().m_renderHeight;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	viewport.TopLeftX = 0;
	viewport.TopLeftY = 0;
	m_pD3DRender->GetContext()->RSSetViewports(1, &viewport);

	//
	// Compile shaders
	//

	std::vector<uint8_t> vshader(FRAME_RENDER_VS_CSO_PTR, FRAME_RENDER_VS_CSO_PTR + FRAME_RENDER_VS_CSO_LEN);
	hr = m_pD3DRender->GetDevice()->CreateVertexShader((const DWORD*)&vshader[0], vshader.size(), NULL, &m_pVertexShader);
	if (FAILED(hr)) {
		Error("CreateVertexShader %p %ls\n", hr, GetErrorStr(hr).c_str());
		return false;
	}

	std::vector<uint8_t> pshader(FRAME_RENDER_PS_CSO_PTR, FRAME_RENDER_PS_CSO_PTR + FRAME_RENDER_PS_CSO_LEN);
	hr = m_pD3DRender->GetDevice()->CreatePixelShader((const DWORD*)&pshader[0], pshader.size(), NULL, &m_pPixelShader);
	if (FAILED(hr)) {
		Error("CreatePixelShader %p %ls\n", hr, GetErrorStr(hr).c_str());
		return false;
	}

	//
	// Create input layout
	//

	// Define the input layout
	D3D11_INPUT_ELEMENT_DESC layout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "VIEW", 0, DXGI_FORMAT_R32_UINT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	UINT numElements = ARRAYSIZE(layout);


	// Create the input layout
	hr = m_pD3DRender->GetDevice()->CreateInputLayout(layout, numElements, &vshader[0],
		vshader.size(), &m_pVertexLayout);
	if (FAILED(hr)) {
		Error("CreateInputLayout %p %ls\n", hr, GetErrorStr(hr).c_str());
		return false;
	}

	// Set the input layout
	m_pD3DRender->GetContext()->IASetInputLayout(m_pVertexLayout.Get());

	//
	// Create vertex buffer
	//

	// Src texture has various geometry and we should use the part of the textures.
	// That part are defined by uv-coordinates of "bounds" passed to IVRDriverDirectModeComponent::SubmitLayer.
	// So we should update uv-coordinates for every frames and layers.
	D3D11_BUFFER_DESC bd;
	ZeroMemory(&bd, sizeof(bd));
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.ByteWidth = sizeof(SimpleVertex) * 8;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	hr = m_pD3DRender->GetDevice()->CreateBuffer(&bd, NULL, &m_pVertexBuffer);
	if (FAILED(hr)) {
		Error("CreateBuffer 1 %p %ls\n", hr, GetErrorStr(hr).c_str());
		return false;
	}

	// Set vertex buffer
	UINT stride = sizeof(SimpleVertex);
	UINT offset = 0;
	m_pD3DRender->GetContext()->IASetVertexBuffers(0, 1, m_pVertexBuffer.GetAddressOf(), &stride, &offset);
	
	//
	// Create index buffer
	//

	WORD indices[] =
	{
		0,1,2,
		0,3,1,

		4,5,6,
		4,7,5
	};

	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.ByteWidth = sizeof(indices);
	bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	bd.CPUAccessFlags = 0;

	D3D11_SUBRESOURCE_DATA InitData;
	ZeroMemory(&InitData, sizeof(InitData));
	InitData.pSysMem = indices;

	hr = m_pD3DRender->GetDevice()->CreateBuffer(&bd, &InitData, &m_pIndexBuffer);
	if (FAILED(hr)) {
		Error("CreateBuffer 2 %p %ls\n", hr, GetErrorStr(hr).c_str());
		return false;
	}

	// Set index buffer
	m_pD3DRender->GetContext()->IASetIndexBuffer(m_pIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);

	// Set primitive topology
	m_pD3DRender->GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Create the sample state
	D3D11_SAMPLER_DESC sampDesc;
	ZeroMemory(&sampDesc, sizeof(sampDesc));
	sampDesc.Filter = D3D11_FILTER_ANISOTROPIC;
	sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	sampDesc.MaxAnisotropy = D3D11_REQ_MAXANISOTROPY;
	sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sampDesc.MinLOD = 0;
	sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
	hr = m_pD3DRender->GetDevice()->CreateSamplerState(&sampDesc, &m_pSamplerLinear);
	if (FAILED(hr)) {
		Error("CreateSamplerState %p %ls\n", hr, GetErrorStr(hr).c_str());
		return false;
	}

	//
	// Create alpha blend state
	// We need alpha blending to support layer.
	//

	// BlendState for first layer.
	// Some VR apps (like StreamVR Home beta) submit the texture that alpha is zero on all pixels.
	// So we need to ignore alpha of first layer.
	D3D11_BLEND_DESC BlendDesc;
	ZeroMemory(&BlendDesc, sizeof(BlendDesc));
	BlendDesc.AlphaToCoverageEnable = FALSE;
	BlendDesc.IndependentBlendEnable = FALSE;
	for (int i = 0; i < 8; i++) {
		BlendDesc.RenderTarget[i].BlendEnable = TRUE;
		BlendDesc.RenderTarget[i].SrcBlend = D3D11_BLEND_ONE;
		BlendDesc.RenderTarget[i].DestBlend = D3D11_BLEND_ZERO;
		BlendDesc.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
		BlendDesc.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
		BlendDesc.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
		BlendDesc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		BlendDesc.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
	}

	hr = m_pD3DRender->GetDevice()->CreateBlendState(&BlendDesc, &m_pBlendStateFirst);
	if (FAILED(hr)) {
		Error("CreateBlendState %p %ls\n", hr, GetErrorStr(hr).c_str());
		return false;
	}

	// BleandState for other layers than first.
	BlendDesc.AlphaToCoverageEnable = FALSE;
	BlendDesc.IndependentBlendEnable = FALSE;
	for (int i = 0; i < 8; i++) {
		BlendDesc.RenderTarget[i].BlendEnable = TRUE;
		BlendDesc.RenderTarget[i].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		BlendDesc.RenderTarget[i].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		BlendDesc.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
		BlendDesc.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
		BlendDesc.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
		BlendDesc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		BlendDesc.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	}

	hr = m_pD3DRender->GetDevice()->CreateBlendState(&BlendDesc, &m_pBlendState);
	if (FAILED(hr)) {
		Error("CreateBlendState %p %ls\n", hr, GetErrorStr(hr).c_str());
		return false;
	}

	m_pStagingTexture = compositionTexture;

	std::vector<uint8_t> quadShaderCSO(QUAD_SHADER_CSO_PTR, QUAD_SHADER_CSO_PTR + QUAD_SHADER_CSO_LEN);
	ComPtr<ID3D11VertexShader> quadVertexShader = CreateVertexShader(m_pD3DRender->GetDevice(), quadShaderCSO);

	enableColorCorrection = Settings::Instance().m_enableColorCorrection;
	if (enableColorCorrection) {
		std::vector<uint8_t> colorCorrectionShaderCSO(COLOR_CORRECTION_CSO_PTR, COLOR_CORRECTION_CSO_PTR + COLOR_CORRECTION_CSO_LEN);

		ComPtr<ID3D11Texture2D> colorCorrectedTexture = CreateTexture(m_pD3DRender->GetDevice(),
			Settings::Instance().m_renderWidth, Settings::Instance().m_renderHeight,
			DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);

		struct ColorCorrection {
			float renderWidth;
			float renderHeight;
			float brightness;
			float contrast;
			float saturation;
			float gamma;
			float sharpening;
			float _align;
		};
		ColorCorrection colorCorrectionStruct = { (float)Settings::Instance().m_renderWidth, (float)Settings::Instance().m_renderHeight,
												  Settings::Instance().m_brightness, Settings::Instance().m_contrast + 1.f,
												  Settings::Instance().m_saturation + 1.f, Settings::Instance().m_gamma,
												  Settings::Instance().m_sharpening };
		ComPtr<ID3D11Buffer> colorCorrectionBuffer = CreateBuffer(m_pD3DRender->GetDevice(), colorCorrectionStruct);

		m_colorCorrectionPipeline = std::make_unique<RenderPipeline>(m_pD3DRender->GetDevice());
		m_colorCorrectionPipeline->Initialize({ m_pStagingTexture.Get() }, quadVertexShader.Get(), colorCorrectionShaderCSO,
											  colorCorrectedTexture.Get(), colorCorrectionBuffer.Get());

		m_pStagingTexture = colorCorrectedTexture;
	}

	enableFFR = Settings::Instance().m_enableFoveatedRendering;
	if (enableFFR) {
		m_ffr = std::make_unique<FFR>(m_pD3DRender->GetDevice());
		m_ffr->Initialize(m_pStagingTexture.Get());

		m_pStagingTexture = m_ffr->GetOutputTexture();
	}

	Debug("Staging Texture created\n");

	return true;
}


bool FrameRender::RenderFrame(ID3D11Texture2D *pTexture[][2], vr::VRTextureBounds_t bounds[][2], vr::HmdVector3_t frameGazeDirection,int layerCount, bool recentering, const std::string &message, const std::string& debugText)
{
	// Set render target
	m_pD3DRender->GetContext()->OMSetRenderTargets(1, m_pRenderTargetView.GetAddressOf(), m_pDepthStencilView.Get());

	// Set viewport
	D3D11_VIEWPORT viewport;
	viewport.Width = (float)Settings::Instance().m_renderWidth;
	viewport.Height = (float)Settings::Instance().m_renderHeight;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	viewport.TopLeftX = 0;
	viewport.TopLeftY = 0;
	m_pD3DRender->GetContext()->RSSetViewports(1, &viewport);
    //Info("Viewport Width X Height=(%f x %f)\n",viewport.Width,viewport.Height);
	// Clear the back buffer
	m_pD3DRender->GetContext()->ClearRenderTargetView(m_pRenderTargetView.Get(), DirectX::Colors::MidnightBlue);

	// Overlay recentering texture on top of all layers.
	int recenterLayer = -1;
	if (recentering) {
		recenterLayer = layerCount;
		layerCount++;
	}

	for (int i = 0; i < layerCount; i++) {
		ID3D11Texture2D *textures[2];
		vr::VRTextureBounds_t bound[2];

		if (i == recenterLayer) {
			textures[0] = (ID3D11Texture2D *)m_recenterTexture.Get();
			textures[1] = (ID3D11Texture2D *)m_recenterTexture.Get();
			bound[0].uMin = bound[0].vMin = bound[1].uMin = bound[1].vMin = 0.0f;
			bound[0].uMax = bound[0].vMax = bound[1].uMax = bound[1].vMax = 1.0f;
		}
		else {
			textures[0] = pTexture[i][0];
			textures[1] = pTexture[i][1];
			bound[0] = bounds[i][0];
			bound[1] = bounds[i][1];
		}
		if (textures[0] == NULL || textures[1] == NULL) {
			Debug("Ignore NULL layer. layer=%d/%d%s%s\n", i, layerCount
				, recentering ? L" (recentering)" : L"", !message.empty() ? L" (message)" : L"");
			continue;
		}

		D3D11_TEXTURE2D_DESC srcDesc;
		textures[0]->GetDesc(&srcDesc);


		//Info("RenderFrame layer=%d/%d %dx%d %d%s%s\n", i, layerCount, srcDesc.Width, srcDesc.Height, srcDesc.Format
		//	, recentering ? L" (recentering)" : L"", !message.empty() ? L" (message)" : L"");
		if (true)
		{
		//根据眼动数据计算Center_x,Center_y	
		struct GazePoint
		{  UINT x;
		   UINT y;	
		} GazePoint[2];
          UINT Zx = srcDesc.Width /(tanf(0.942478)+tanf(0.698132));
	      UINT Zy = srcDesc.Height/(tanf(0.733038)+tanf(0.942478)); 
	   //应该是一个Z但是目前所有的尺寸算出来的都差一些
          double angle_x = atanf(-1.0*frameGazeDirection.v[0]/frameGazeDirection.v[2]);
	      double angle_y = atanf(1.0*frameGazeDirection.v[1]/frameGazeDirection.v[2]);
          GazePoint[0].y = GazePoint[1].y = Zy*(tanf(0.733038)+tanf(angle_y));
	      GazePoint[0].x = Zx*(tanf(0.942478)+tanf(angle_x));
	      GazePoint[1].x = Zx*(tanf(0.698132)+tanf(angle_x));
        //UINT Center_X = srcDesc.Width/2, Center_Y = srcDesc.Height/2; 
	   // UINT W = srcDesc.Width/16, H = srcDesc.Height/16; 
		struct visualformat
		{
			UINT width ;
			UINT height;
		}  vformat[2];
		vformat[0].width  = vformat[1].width  = srcDesc.Width/16;
		vformat[0].height = vformat[1].height = srcDesc.Height/16;
		//超出边界重置可视化区域的尺寸,GazePoint是中心 所以 加1/2的宽高和边界比较
        if (GazePoint[0].x + vformat[0].width/2 >srcDesc.Width)
        vformat[0].width  = 2*(srcDesc.Width  - GazePoint[0].x);
        if (GazePoint[0].y +vformat[0].height/2 >srcDesc.Height)
	    vformat[0].height = 2*(srcDesc.Height - GazePoint[0].y); //Left

		if (GazePoint[1].x + vformat[1].width/2 >srcDesc.Width)
        vformat[1].width  = 2*(srcDesc.Width  - GazePoint[1].x);
        if (GazePoint[1].y +vformat[1].height/2 >srcDesc.Height)
	    vformat[1].height = 2*(srcDesc.Height - GazePoint[1].y); //right	





        D3D11_BOX srcRegion[2];
		srcRegion[0].front  =  srcRegion[1].front = 0;
	    srcRegion[0].back   =  srcRegion[1].back  = 1;
	    srcRegion[0].left   =  srcRegion[1].left  = 0;
        srcRegion[0].top    =  srcRegion[1].top   = 0;
	    srcRegion[0].right  =  vformat[0].width;
		srcRegion[1].right  =  vformat[1].width;
	    srcRegion[0].bottom =  vformat[0].height;
		srcRegion[1].left   =  vformat[1].height;
			
          //如果分辨率所发生了改变则需要重新创建纹理
          if ( (m_GazepointWidth != srcDesc.Width) || (m_GazepointHeight != srcDesc.Height) )
		  {
			CreateGazepointTexture(srcDesc);//创建/更新 可视化区域
			m_pD3DRender->GetContext()->CopySubresourceRegion(textures[0],0,GazePoint[0].x-vformat[0].width/2,GazePoint[0].y-vformat[0].height/2,0,GazepointTexture.Get(),0,&srcRegion[0]);
            m_pD3DRender->GetContext()->CopySubresourceRegion(textures[1],0,GazePoint[1].y-vformat[1].width/2,GazePoint[1].y-vformat[1].height/2,0,GazepointTexture.Get(),0,&srcRegion[1]);
		  }		  
           else
		  {
			m_pD3DRender->GetContext()->CopySubresourceRegion(textures[0],0,GazePoint[0].x-vformat[1].width/2,GazePoint[0].y-vformat[0].height/2,0,GazepointTexture.Get(),0,&srcRegion[0]);
            m_pD3DRender->GetContext()->CopySubresourceRegion(textures[1],0,GazePoint[1].y-vformat[1].width/2,GazePoint[1].y-vformat[1].height/2,0,GazepointTexture.Get(),0,&srcRegion[1]);
		  }
		}
		
		D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Format = srcDesc.Format;
		SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		SRVDesc.Texture2D.MostDetailedMip = 0;
		SRVDesc.Texture2D.MipLevels = 1;

		ComPtr<ID3D11ShaderResourceView> pShaderResourceView[2];

		HRESULT hr = m_pD3DRender->GetDevice()->CreateShaderResourceView(textures[0], &SRVDesc, pShaderResourceView[0].ReleaseAndGetAddressOf());
		if (FAILED(hr)) {
			Error("CreateShaderResourceView %p %ls\n", hr, GetErrorStr(hr).c_str());
			return false;
		}
		hr = m_pD3DRender->GetDevice()->CreateShaderResourceView(textures[1], &SRVDesc, pShaderResourceView[1].ReleaseAndGetAddressOf());
		if (FAILED(hr)) {
			Error("CreateShaderResourceView %p %ls\n", hr, GetErrorStr(hr).c_str());
			return false;
		}

		if (i == 0) {
			m_pD3DRender->GetContext()->OMSetBlendState(m_pBlendStateFirst.Get(), NULL, 0xffffffff);
		}
		else {
			m_pD3DRender->GetContext()->OMSetBlendState(m_pBlendState.Get(), NULL, 0xffffffff);
		}
		
		// Clear the depth buffer to 1.0 (max depth)
		// We need clear depth buffer to correctly render layers.
		m_pD3DRender->GetContext()->ClearDepthStencilView(m_pDepthStencilView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

		//
		// Update uv-coordinates in vertex buffer according to bounds.
		//

		SimpleVertex vertices[] =
		{
			// Left View
		{ DirectX::XMFLOAT3(-1.0f, -1.0f, 0.5f), DirectX::XMFLOAT2(bound[0].uMin, bound[0].vMax), 0 },
		{ DirectX::XMFLOAT3(0.0f,  1.0f, 0.5f), DirectX::XMFLOAT2(bound[0].uMax, bound[0].vMin), 0 },
		{ DirectX::XMFLOAT3(0.0f, -1.0f, 0.5f), DirectX::XMFLOAT2(bound[0].uMax, bound[0].vMax), 0 },
		{ DirectX::XMFLOAT3(-1.0f,  1.0f, 0.5f), DirectX::XMFLOAT2(bound[0].uMin, bound[0].vMin), 0 },
		// Right View
		{ DirectX::XMFLOAT3(0.0f, -1.0f, 0.5f), DirectX::XMFLOAT2(bound[1].uMin, bound[1].vMax), 1 },
		{ DirectX::XMFLOAT3(1.0f,  1.0f, 0.5f), DirectX::XMFLOAT2(bound[1].uMax, bound[1].vMin), 1 },
		{ DirectX::XMFLOAT3(1.0f, -1.0f, 0.5f), DirectX::XMFLOAT2(bound[1].uMax, bound[1].vMax), 1 },
		{ DirectX::XMFLOAT3(0.0f,  1.0f, 0.5f), DirectX::XMFLOAT2(bound[1].uMin, bound[1].vMin), 1 },
		};

		// TODO: Which is better? UpdateSubresource or Map
		//m_pD3DRender->GetContext()->UpdateSubresource(m_pVertexBuffer.Get(), 0, nullptr, &vertices, 0, 0);

		D3D11_MAPPED_SUBRESOURCE mapped = { 0 };
		hr = m_pD3DRender->GetContext()->Map(m_pVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr)) {
			Error("Map %p %ls\n", hr, GetErrorStr(hr).c_str());
			return false;
		}
		memcpy(mapped.pData, vertices, sizeof(vertices));

		m_pD3DRender->GetContext()->Unmap(m_pVertexBuffer.Get(), 0);

		// Set the input layout
		m_pD3DRender->GetContext()->IASetInputLayout(m_pVertexLayout.Get());

		//
		// Set buffers
		//

		UINT stride = sizeof(SimpleVertex);
		UINT offset = 0;
		m_pD3DRender->GetContext()->IASetVertexBuffers(0, 1, m_pVertexBuffer.GetAddressOf(), &stride, &offset);

		m_pD3DRender->GetContext()->IASetIndexBuffer(m_pIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
		m_pD3DRender->GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		//
		// Set shaders
		//

		m_pD3DRender->GetContext()->VSSetShader(m_pVertexShader.Get(), nullptr, 0);
		m_pD3DRender->GetContext()->PSSetShader(m_pPixelShader.Get(), nullptr, 0);

		ID3D11ShaderResourceView *shaderResourceView[2] = { pShaderResourceView[0].Get(), pShaderResourceView[1].Get() };
		m_pD3DRender->GetContext()->PSSetShaderResources(0, 2, shaderResourceView);

		m_pD3DRender->GetContext()->PSSetSamplers(0, 1, m_pSamplerLinear.GetAddressOf());
		
		//
		// Draw
		//

		m_pD3DRender->GetContext()->DrawIndexed(VERTEX_INDEX_COUNT, 0, 0);
	}

	if (enableColorCorrection) {
		m_colorCorrectionPipeline->Render();
	}

	if (enableFFR) {
		m_ffr->Render();
	}

	m_pD3DRender->GetContext()->Flush();

	return true;
}

void FrameRender::CreateGazepointTexture(D3D11_TEXTURE2D_DESC m_srcDesc)
{
        m_GazepointWidth = m_srcDesc.Width;
	    m_GazepointHeight = m_srcDesc.Height;
	    D3D11_TEXTURE2D_DESC gazeDesc;
	    gazeDesc.Width = m_srcDesc.Width/16;
	    gazeDesc.Height = m_srcDesc.Height/16;	
	    gazeDesc.Format = m_srcDesc.Format;
	    gazeDesc.Usage = D3D11_USAGE_DEFAULT;
	    gazeDesc.MipLevels = 1;
	    gazeDesc.ArraySize = 1;
	    gazeDesc.SampleDesc.Count = 1;
	    gazeDesc.CPUAccessFlags = 0;
	    gazeDesc.MiscFlags = 0;
	    gazeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;//绑定为常量缓冲区，可以与任何其他绑定标志组合
        //初始化纹理数据 
	    const UINT pixelSize = 4; // 
        const UINT rowPitch = gazeDesc.Width* pixelSize;
        const UINT textureSize = rowPitch * gazeDesc.Height;
        std::vector<float> pixels(textureSize / sizeof(float));
        for (UINT y = 0; y < gazeDesc.Height; ++y)
        {
          for (UINT x = 0; x < gazeDesc.Width; ++x)
          {
            UINT pixelIndex = y * rowPitch / sizeof(float) + x * pixelSize / sizeof(float);
            pixels[pixelIndex + 0] = 0.0f; // 红色通道
            pixels[pixelIndex + 1] = 0.0f; // 绿色通道
            pixels[pixelIndex + 2] = 1.0f; // 蓝色通道
            pixels[pixelIndex + 3] = 0.5f; // 透明度通道
          }
        }
		D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = pixels.data();
        initData.SysMemPitch = rowPitch;
        initData.SysMemSlicePitch = textureSize;
		//创建2D纹理对象
	    HRESULT hr = m_pD3DRender->GetDevice()->CreateTexture2D(&gazeDesc,&initData,&GazepointTexture);
		if (FAILED(hr))
		{
			Info("CreateTexture2D failed :GazepointTexture hr = %x\n", hr);
		}
		
}

ComPtr<ID3D11Texture2D> FrameRender::GetTexture()
{
	return m_pStagingTexture;
}

void FrameRender::GetEncodingResolution(uint32_t *width, uint32_t *height) {
	if (enableFFR) {
		m_ffr->GetOptimizedResolution(width, height);
	}
	else {
		*width = Settings::Instance().m_renderWidth;
		*height = Settings::Instance().m_renderHeight;
	}
	
}
