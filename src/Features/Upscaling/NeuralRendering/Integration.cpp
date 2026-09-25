#include "Integration.h"

#include "Renderer.h"
#include "Features/HDRDisplay.h"
#include "Features/Upscaling.h"
#include "Features/Upscaling/FoveatedRender/Bridge.h"
#include "Features/Upscaling/FoveatedRender/Core.h"
#include "Globals.h"
#include "GpuPass.h"
#include "Buffer.h"
#include "Util.h"
#include "Utils/LazyShader.h"

#include <array>

namespace NeuralRendering
{
	namespace
	{
		eastl::unique_ptr<Texture2D> color[2];
		std::uint32_t colorWidth = 0;
		std::uint32_t colorHeight = 0;
		DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
		std::uint32_t lastAppliedFrame = UINT32_MAX;
		bool writebackLogged = false;
		bool flatRouteWasActive = false;
		bool flatFrameGenerationBlockLogged = false;
		bool flatHdrBlockLogged = false;

		// ── Pre-upscale route state ──
		eastl::unique_ptr<Texture2D> preUpscaleLdr;   // display-referred copy handed to Feature 18
		eastl::unique_ptr<Texture2D> preUpscaleHdr;   // staging target; kMAIN is an SRV during the apply pass
		ConstantBuffer* preUpscaleCB = nullptr;
		Util::LazyShader<ID3D11VertexShader> preUpscaleVS;
		// Two permutations of each pass. The encode curve must be identical in both
		// shaders, so they are always selected as a pair. GT7 matches the Post
		// Processing tonemapper this build actually runs; Reinhard is the fallback
		// for setups without those shaders, where the include cannot resolve.
		Util::LazyShader<ID3D11PixelShader> tonemapEncodeGt7PS;
		Util::LazyShader<ID3D11PixelShader> ratioApplyGt7PS;
		Util::LazyShader<ID3D11PixelShader> tonemapEncodeReinhardPS;
		Util::LazyShader<ID3D11PixelShader> ratioApplyReinhardPS;
		bool preUpscaleGt7Unavailable = false;
		std::uint32_t preUpscaleWidth = 0;
		std::uint32_t preUpscaleHeight = 0;
		std::uint32_t lastPreUpscaleFrame = UINT32_MAX;
		bool preUpscaleRouteWasActive = false;
		bool preUpscaleLogged = false;

		struct PreUpscaleData
		{
			float exposure = 1.0f;
			float pad[3]{};
		};

		ID3D11Texture2D* ResolveRenderTargetTexture(
			const RE::BSGraphics::RenderTargetData& target,
			winrt::com_ptr<ID3D11Texture2D>& holder)
		{
			if (target.texture)
				return target.texture;
			auto resolveView = [&](ID3D11View* view) -> ID3D11Texture2D* {
				if (!view)
					return nullptr;
				winrt::com_ptr<ID3D11Resource> resource;
				view->GetResource(resource.put());
				if (!resource || FAILED(resource->QueryInterface(holder.put())))
					return nullptr;
				return holder.get();
			};
			if (auto* texture = resolveView(target.SRV))
				return texture;
			return resolveView(target.RTV);
		}

		bool EnsureColorResources(ID3D11Resource* source, std::uint32_t width, std::uint32_t height)
		{
			winrt::com_ptr<ID3D11Texture2D> sourceTexture;
			if (!source || FAILED(source->QueryInterface(sourceTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			if (color[0] && colorWidth == width && colorHeight == height && colorFormat == sourceDesc.Format)
				return true;
			const std::uint32_t resourceCount = globals::game::isVR ? 2u : 1u;
			for (std::uint32_t eye = 0; eye < resourceCount; ++eye) {
				color[eye] = Upscaling::CreateTextureFromSource(source, width, height, false, true, true,
					eye == 0 ? "NeuralRendering::LdrColorLeft" : "NeuralRendering::LdrColorRight");
				if (!color[eye])
					return false;
			}
			if (!globals::game::isVR)
				color[1].reset();
			colorWidth = width;
			colorHeight = height;
			colorFormat = sourceDesc.Format;
			return true;
		}

		bool EnsurePreUpscaleResources(ID3D11Resource* sceneColor, std::uint32_t width, std::uint32_t height)
		{
			if (preUpscaleLdr && preUpscaleHdr && preUpscaleWidth == width && preUpscaleHeight == height)
				return true;

			winrt::com_ptr<ID3D11Texture2D> sceneTexture;
			if (!sceneColor || FAILED(sceneColor->QueryInterface(sceneTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC sceneDesc{};
			sceneTexture->GetDesc(&sceneDesc);

			preUpscaleLdr.reset();
			preUpscaleHdr.reset();
			preUpscaleWidth = preUpscaleHeight = 0;

			// R8G8B8A8_UNORM deliberately: it is what the post-tonemap route feeds the
			// model via kFRAMEBUFFER on flat SDR, so the model sees the same precision
			// and encoding it does today. Only the resolution changes.
			D3D11_TEXTURE2D_DESC ldrDesc{};
			ldrDesc.Width = width;
			ldrDesc.Height = height;
			ldrDesc.MipLevels = 1;
			ldrDesc.ArraySize = 1;
			ldrDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			ldrDesc.SampleDesc.Count = 1;
			ldrDesc.Usage = D3D11_USAGE_DEFAULT;
			ldrDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

			D3D11_TEXTURE2D_DESC hdrDesc = sceneDesc;
			hdrDesc.Width = width;
			hdrDesc.Height = height;
			hdrDesc.MipLevels = 1;
			hdrDesc.ArraySize = 1;
			hdrDesc.SampleDesc.Count = 1;
			hdrDesc.SampleDesc.Quality = 0;
			hdrDesc.Usage = D3D11_USAGE_DEFAULT;
			hdrDesc.CPUAccessFlags = 0;
			hdrDesc.MiscFlags = 0;
			hdrDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

			try {
				preUpscaleLdr = eastl::make_unique<Texture2D>(ldrDesc, "NeuralRendering::PreUpscaleLdr");
				D3D11_SHADER_RESOURCE_VIEW_DESC ldrSrv{};
				ldrSrv.Format = ldrDesc.Format;
				ldrSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				ldrSrv.Texture2D.MipLevels = 1;
				preUpscaleLdr->CreateSRV(ldrSrv);
				D3D11_RENDER_TARGET_VIEW_DESC ldrRtv{};
				ldrRtv.Format = ldrDesc.Format;
				ldrRtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				preUpscaleLdr->CreateRTV(ldrRtv);

				preUpscaleHdr = eastl::make_unique<Texture2D>(hdrDesc, "NeuralRendering::PreUpscaleHdr");
				D3D11_RENDER_TARGET_VIEW_DESC hdrRtv{};
				hdrRtv.Format = hdrDesc.Format;
				hdrRtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				preUpscaleHdr->CreateRTV(hdrRtv);
			} catch (const std::exception& e) {
				logger::error("[DLSSNR] pre-upscale resource creation failed: {}", e.what());
				preUpscaleLdr.reset();
				preUpscaleHdr.reset();
				return false;
			}

			preUpscaleWidth = width;
			preUpscaleHeight = height;
			// Guide and color extents changed under Feature 18; drop the history so the
			// first frame at the new size doesn't reproject against the old one.
			Renderer::Instance().ResetHistory();
			logger::info("[DLSSNR] pre-upscale resources {}x{} hdrFormat={}", width, height,
				static_cast<std::uint32_t>(hdrDesc.Format));
			return true;
		}

		void DrawPreUpscalePass(ID3D11DeviceContext* context, ID3D11VertexShader* vertexShader,
			ID3D11PixelShader* pixelShader, ID3D11RenderTargetView* target,
			ID3D11ShaderResourceView* const* sources, UINT sourceCount, ID3D11Buffer* constants,
			std::uint32_t width, std::uint32_t height)
		{
			context->IASetInputLayout(nullptr);
			context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
			context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			context->VSSetShader(vertexShader, nullptr, 0);
			context->PSSetShader(pixelShader, nullptr, 0);
			context->GSSetShader(nullptr, nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);

			context->PSSetShaderResources(0, sourceCount, sources);
			context->PSSetConstantBuffers(0, 1, &constants);

			context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
			context->OMSetDepthStencilState(nullptr, 0);
			context->RSSetState(nullptr);

			D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
			context->RSSetViewports(1, &viewport);

			ID3D11RenderTargetView* targets[]{ target };
			context->OMSetRenderTargets(1, targets, nullptr);
			context->Draw(3, 0);
		}

		// CS quality mode -> NVSDK_NGX_PerfQuality_Value. The two enums are ordered
		// differently, so this cannot be a cast: CS runs NativeAA..UltraPerformance as
		// 0..4, while NGX is MaxPerf=0, Balanced=1, MaxQuality=2, UltraPerformance=3,
		// UltraQuality=4, DLAA=5.
		std::uint32_t ResolvePerformanceQuality(const FoveatedRender::Settings& settings)
		{
			if (settings.neuralRenderingQualityContract != 0) {
				// UI order: Follow, DLAA, Quality, Balanced, Performance, Ultra Perf, Ultra Quality
				constexpr std::uint32_t kExplicit[]{ 2u, 5u, 2u, 1u, 0u, 3u, 4u };
				return kExplicit[std::min<std::uint32_t>(settings.neuralRenderingQualityContract, 6u)];
			}
			switch (globals::features::upscaling.settings.qualityMode) {
			case 0: return 5u;  // NativeAA / DLAA
			case 1: return 2u;  // Quality      -> MaxQuality
			case 2: return 1u;  // Balanced
			case 3: return 0u;  // Performance  -> MaxPerf
			case 4: return 3u;  // UltraPerformance
			default: return 2u;
			}
		}

		Tuning GetTuning(const FoveatedRender::Settings& settings)
		{
			return {
				settings.neuralRenderingIntensity,
				settings.neuralRenderingLocalTone,
				settings.neuralRenderingLocalStructure,
				settings.neuralRenderingSkinStructure,
				settings.neuralRenderingStyle,
				settings.neuralRenderingAutoMask,
				settings.neuralRenderingUICorrection,
				ResolvePerformanceQuality(settings),
				settings.neuralRenderingOutputPreset,
			};
		}

		bool ApplyFlatLdr(Upscaling& upscaling, FoveatedRender& foveated)
		{
			const bool frameGenerationConfigured = upscaling.IsFrameGenerationConfiguredForSession();
			const bool hdrConfigured = globals::features::hdrDisplay.loaded &&
				globals::features::hdrDisplay.settings.enableHDR;
			auto* renderer = globals::game::renderer;
			winrt::com_ptr<ID3D11Texture2D> framebufferHolder;
			ID3D11Texture2D* framebuffer = nullptr;
			if (renderer) {
				auto& target = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
				framebuffer = ResolveRenderTargetTexture(target, framebufferHolder);
			}
			// The two flat routes are mutually exclusive: both evaluate slot 0 and both
			// write the scene, so running them together would apply the model twice.
			// Frame generation is no longer refused: the interop now adopts the sidecar's
			// D3D12 device rather than creating a second one alongside it. Still logged
			// once, because the combination is new and this is the first thing to suspect
			// if the session misbehaves.
			const bool routeActive = upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS &&
				foveated.settings.neuralRenderingEnabled && !foveated.settings.neuralRenderingPreUpscale &&
				!hdrConfigured;
			if (routeActive && frameGenerationConfigured && !flatFrameGenerationBlockLogged) {
				logger::info("[DLSSNR] Running alongside Frame Generation on the shared sidecar device");
				flatFrameGenerationBlockLogged = true;
			}
			if (!routeActive) {
				if (foveated.settings.neuralRenderingEnabled && hdrConfigured && !flatHdrBlockLogged) {
					logger::warn("[DLSSNR] Flat route blocked: HDR Display is not supported by the LDR integration");
					flatHdrBlockLogged = true;
				}
				if (flatRouteWasActive)
					Reset();
				return false;
			}
			flatRouteWasActive = true;

			const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
			if (lastAppliedFrame == frame)
				return true;
			auto* context = globals::d3d::context;
			if (!renderer || !context || !globals::d3d::device || !upscaling.motionVectorCopyTexture)
				return false;

			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			if (!framebuffer || !depth.texture || !depth.depthSRV || !upscaling.motionVectorCopyTexture->resource)
				return false;

			D3D11_TEXTURE2D_DESC totalDesc{};
			framebuffer->GetDesc(&totalDesc);
			if (!EnsureColorResources(framebuffer, totalDesc.Width, totalDesc.Height))
				return false;

			// The guide extent must follow the MOTION VECTORS, not the allocation.
			// motionVectorCopyTexture is a display-res allocation, but EncodeTexturesCS
			// only dispatches over the render extent, so only that subrect holds this
			// frame's vectors - and they are in render-res scale. Taking the extent and
			// the MVec scale from the allocation (as this route used to) overstates both
			// by the DLSS ratio at anything other than DLAA, which misdirects every
			// reprojection the model makes.
			//
			// ignoreLock: PerformUpscaling() sets dynamicResolutionLock after
			// UpscaleDepth(), and this route runs after that, so the unlocked query
			// would hand back the display size.
			const auto renderSize = Util::ConvertToDynamic(globals::state->screenSize, true);
			const auto guideWidth = static_cast<std::uint32_t>(renderSize.x);
			const auto guideHeight = static_cast<std::uint32_t>(renderSize.y);
			if (guideWidth == 0 || guideHeight == 0)
				return false;

			CS_GPU_PASS("NeuralRendering::FlatLdrBeforeUI");
			ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* savedDSV = nullptr;
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
			context->OMSetRenderTargets(0, nullptr, nullptr);
			context->CopyResource(color[0]->resource.get(), framebuffer);

			// Depth, unlike the motion vectors, IS valid at display res here - UpscaleDepth()
			// has already run - so it is handed over with its own source extent and gets
			// downsampled into the render-res guide rather than cropped to the corner.
			const bool succeeded = Renderer::Instance().Apply(globals::d3d::device, context, 0,
				color[0]->resource.get(), depth.texture, depth.depthSRV,
				upscaling.motionVectorCopyTexture->resource.get(), guideWidth, guideHeight,
				totalDesc.Width, totalDesc.Height, static_cast<float>(guideWidth),
				static_cast<float>(guideHeight), GetTuning(foveated.settings),
				totalDesc.Width, totalDesc.Height);
			if (succeeded) {
				context->CopyResource(framebuffer, color[0]->resource.get());
				lastAppliedFrame = frame;
				if (!writebackLogged) {
					logger::info("[DLSSNR] Flat LDR kFRAMEBUFFER output written before UI guides={}x{} color={}x{}",
						guideWidth, guideHeight, totalDesc.Width, totalDesc.Height);
					writebackLogged = true;
				}
			}

			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
			for (auto*& rtv : savedRTVs)
				if (rtv) rtv->Release();
			if (savedDSV) savedDSV->Release();
			return succeeded;
		}
	}

	bool ApplyFlatPreUpscale()
	{
		if (globals::game::isVR)
			return false;

		auto& upscaling = globals::features::upscaling;
		auto& foveated = upscaling.foveatedRender;

		const bool hdrConfigured = globals::features::hdrDisplay.loaded &&
			globals::features::hdrDisplay.settings.enableHDR;
		// Frame generation is not a prerequisite either way - see ApplyFlatLdr. HDR
		// Display still is: the route rewrites kMAIN in scene-linear space, which the
		// HDR output path assumes it owns.
		const bool routeActive = upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS &&
			foveated.settings.neuralRenderingEnabled && foveated.settings.neuralRenderingPreUpscale &&
			!hdrConfigured;
		if (!routeActive) {
			if (preUpscaleRouteWasActive)
				Reset();
			return false;
		}
		preUpscaleRouteWasActive = true;

		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
		if (lastPreUpscaleFrame == frame)
			return true;

		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		if (!renderer || !context || !globals::d3d::device || !upscaling.motionVectorCopyTexture ||
			!upscaling.motionVectorCopyTexture->resource)
			return false;

		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		if (!main.texture || !main.SRV || !depth.texture || !depth.depthSRV)
			return false;

		// kMAIN, the depth target and the motion vector copy are all display-res
		// allocations holding this frame's scene in a top-left subrect of the dynamic
		// render size. Everything below runs at that size, so color and guides are
		// pixel-aligned 1:1 — unlike the post-tonemap route, where the color has been
		// upscaled but the guides have not.
		const auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);
		const auto renderWidth = static_cast<std::uint32_t>(renderSize.x);
		const auto renderHeight = static_cast<std::uint32_t>(renderSize.y);
		if (renderWidth == 0 || renderHeight == 0)
			return false;

		if (!EnsurePreUpscaleResources(main.texture, renderWidth, renderHeight))
			return false;

		auto* vertexShader = preUpscaleVS.Get(L"Data\\Shaders\\Upscaling\\UpscaleVS.hlsl",
			{ { "VSHADER", "" } }, "vs_5_0", "main", "NeuralRendering::PreUpscaleVS");
		const wchar_t* encodePath = L"Data\\Shaders\\Upscaling\\NeuralRendering\\TonemapEncodePS.hlsl";
		const wchar_t* applyPath = L"Data\\Shaders\\Upscaling\\NeuralRendering\\RatioApplyPS.hlsl";
		ID3D11PixelShader* encodeShader = nullptr;
		ID3D11PixelShader* applyShader = nullptr;
		// Curve choice only changes what the model is shown, never the fidelity of
		// the transfer - see Settings::neuralRenderingPreUpscaleCurve. The GT7
		// permutation can still fail on setups without the Post Processing shaders,
		// in which case it latches off and Reinhard is used regardless of the setting.
		const bool wantGt7 = foveated.settings.neuralRenderingPreUpscaleCurve == 0;
		if (wantGt7 && !preUpscaleGt7Unavailable) {
			encodeShader = tonemapEncodeGt7PS.Get(encodePath, { { "PSHADER", "" }, { "PREUPSCALE_GT7", "" } },
				"ps_5_0", "main", "NeuralRendering::TonemapEncodePS(GT7)");
			applyShader = ratioApplyGt7PS.Get(applyPath, { { "PSHADER", "" }, { "PREUPSCALE_GT7", "" } },
				"ps_5_0", "main", "NeuralRendering::RatioApplyPS(GT7)");
			// Both or neither: a mixed pair would have RatioApplyPS reconstruct a
			// different encode than the model was actually handed, breaking the
			// unchanged-pixel-stays-unchanged invariant the transfer depends on.
			if (!encodeShader || !applyShader) {
				preUpscaleGt7Unavailable = true;
				encodeShader = nullptr;
				applyShader = nullptr;
				logger::warn("[DLSSNR] GT7 encode unavailable (Post Processing shaders missing?); "
					"falling back to Reinhard - expect more contrast than the reference");
			}
		}
		if (!encodeShader) {
			encodeShader = tonemapEncodeReinhardPS.Get(encodePath, { { "PSHADER", "" } },
				"ps_5_0", "main", "NeuralRendering::TonemapEncodePS");
			applyShader = ratioApplyReinhardPS.Get(applyPath, { { "PSHADER", "" } },
				"ps_5_0", "main", "NeuralRendering::RatioApplyPS");
		}
		if (!vertexShader || !encodeShader || !applyShader)
			return false;

		if (!preUpscaleCB)
			preUpscaleCB = new ConstantBuffer(ConstantBufferDesc<PreUpscaleData>(), "NeuralRendering::PreUpscaleCB");
		PreUpscaleData constants{};
		constants.exposure = foveated.settings.neuralRenderingPreUpscaleExposure;
		preUpscaleCB->Update(constants);
		ID3D11Buffer* constantBuffer = preUpscaleCB->CB();

		CS_GPU_PASS("NeuralRendering::FlatPreUpscale");
		bool succeeded = false;
		{
			Util::FullscreenPassScope stateScope(context);
			ID3D11ShaderResourceView* nullSRVs[2]{};

			ID3D11ShaderResourceView* encodeSources[]{ main.SRV };
			DrawPreUpscalePass(context, vertexShader, encodeShader, preUpscaleLdr->rtv.get(),
				encodeSources, 1, constantBuffer, renderWidth, renderHeight);

			// Feature 18 copies out of preUpscaleLdr and back into it, so it must not
			// still be bound as a render target here.
			context->OMSetRenderTargets(0, nullptr, nullptr);
			context->PSSetShaderResources(0, 2, nullSRVs);

			succeeded = Renderer::Instance().Apply(globals::d3d::device, context, 0,
				preUpscaleLdr->resource.get(), depth.texture, depth.depthSRV,
				upscaling.motionVectorCopyTexture->resource.get(),
				renderWidth, renderHeight, renderWidth, renderHeight,
				static_cast<float>(renderWidth), static_cast<float>(renderHeight),
				GetTuning(foveated.settings));

			if (succeeded) {
				ID3D11ShaderResourceView* applySources[]{ main.SRV, preUpscaleLdr->srv.get() };
				DrawPreUpscalePass(context, vertexShader, applyShader, preUpscaleHdr->rtv.get(),
					applySources, 2, constantBuffer, renderWidth, renderHeight);
				context->OMSetRenderTargets(0, nullptr, nullptr);
				context->PSSetShaderResources(0, 2, nullSRVs);

				const D3D11_BOX sceneBox{ 0, 0, 0, renderWidth, renderHeight, 1 };
				context->CopySubresourceRegion(main.texture, 0, 0, 0, 0,
					preUpscaleHdr->resource.get(), 0, &sceneBox);
			}

			// FullscreenPassScope restores PS constant buffer slot 1, not slot 0.
			ID3D11Buffer* nullConstants = nullptr;
			context->PSSetConstantBuffers(0, 1, &nullConstants);
		}

		if (succeeded) {
			lastPreUpscaleFrame = frame;
			if (!preUpscaleLogged) {
				logger::info("[DLSSNR] Flat pre-upscale kMAIN output written before DLSS render={}x{} exposure={:.2f}",
					renderWidth, renderHeight, foveated.settings.neuralRenderingPreUpscaleExposure);
				preUpscaleLogged = true;
			}
		}
		return succeeded;
	}

	bool ApplyFoveatedLdr()
	{
		auto& upscaling = globals::features::upscaling;
		auto& foveated = upscaling.foveatedRender;
		if (!globals::game::isVR)
			return ApplyFlatLdr(upscaling, foveated);
		if (!globals::game::isVR || !FoveatedRenderImpl::Bridge::IsRouteActive() ||
			upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS ||
			foveated.GetDlssMode() != FoveatedRender::DlssMode::kDefault ||
			!foveated.settings.neuralRenderingEnabled || upscaling.IsFrameGenerationActive())
			return false;

		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
		const std::uint32_t guideFrame = FoveatedRenderImpl::Core::neuralGuidesFrame;
		if (lastAppliedFrame == frame || (guideFrame != frame && !(frame > 0 && guideFrame == frame - 1)))
			return false;

		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		if (!renderer || !context || !globals::d3d::device ||
			!FoveatedRenderImpl::Core::vrSubrectDepth[0] || !FoveatedRenderImpl::Core::vrSubrectDepth[1] ||
			!FoveatedRenderImpl::Core::vrSubrectMotionVectors[0] || !FoveatedRenderImpl::Core::vrSubrectMotionVectors[1])
			return false;
		auto& total = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTOTAL];
		if (!total.texture)
			return false;

		D3D11_TEXTURE2D_DESC totalDesc{};
		total.texture->GetDesc(&totalDesc);
		const auto& leftUV = foveated.subrectController.GetUV();
		const auto& rightUV = foveated.subrectController.GetRightEyeUV();
		if (leftUV.w != rightUV.w || leftUV.h != rightUV.h)
			return false;
		const std::uint32_t eyeWidth = totalDesc.Width / 2;
		const std::uint32_t outWidth = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(eyeWidth * leftUV.w));
		const std::uint32_t outHeight = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(totalDesc.Height * leftUV.h));

		CS_GPU_PASS("NeuralRendering::FoveatedLdrBeforeUI");
		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* savedDSV = nullptr;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		const Util::Subrect::UVRegion* eyeUVs[2]{ &leftUV, &rightUV };
		std::array<Renderer::StereoEyeInput, 2> inputs{};
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			const auto& uv = *eyeUVs[eye];
			const std::uint32_t x = (eye ? eyeWidth : 0) + static_cast<std::uint32_t>(eyeWidth * uv.x);
			const std::uint32_t y = static_cast<std::uint32_t>(totalDesc.Height * uv.y);
			float motionScaleX = 1.0f;
			float motionScaleY = 1.0f;
			FoveatedRenderImpl::Bridge::ComputeMvecScale(eye, motionScaleX, motionScaleY);
			inputs[eye] = {
				.depth = FoveatedRenderImpl::Core::vrSubrectDepth[eye]->resource.get(),
				.depthSRV = FoveatedRenderImpl::Core::vrSubrectDepth[eye]->srv.get(),
				.motionVectors = FoveatedRenderImpl::Core::vrSubrectMotionVectors[eye]->resource.get(),
				.sourceX = x,
				.sourceY = y,
				.motionVectorScaleX = motionScaleX * FoveatedRenderImpl::Core::vrSubrectInW,
				.motionVectorScaleY = motionScaleY * FoveatedRenderImpl::Core::vrSubrectInH,
			};
		}
		const bool succeeded = Renderer::Instance().ApplyStereo(globals::d3d::device, context,
			total.texture, inputs, FoveatedRenderImpl::Core::vrSubrectInW, FoveatedRenderImpl::Core::vrSubrectInH,
			outWidth, outHeight, GetTuning(foveated.settings));
		if (succeeded) {
			lastAppliedFrame = frame;
			if (!writebackLogged) {
				logger::info("[DLSSNR] LDR output written before UI composite size={}x{} batchedAsync=true", outWidth, outHeight);
				writebackLogged = true;
			}
		}

		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
		for (auto*& rtv : savedRTVs)
			if (rtv) rtv->Release();
		if (savedDSV) savedDSV->Release();
		return succeeded;
	}

	void Reset()
	{
		Renderer::Instance().Reset();
		color[0].reset();
		color[1].reset();
		colorWidth = colorHeight = 0;
		colorFormat = DXGI_FORMAT_UNKNOWN;
		lastAppliedFrame = UINT32_MAX;
		writebackLogged = false;
		flatRouteWasActive = false;
		flatFrameGenerationBlockLogged = false;
		flatHdrBlockLogged = false;

		preUpscaleLdr.reset();
		preUpscaleHdr.reset();
		delete preUpscaleCB;
		preUpscaleCB = nullptr;
		preUpscaleVS.Reset();
		tonemapEncodeGt7PS.Reset();
		ratioApplyGt7PS.Reset();
		tonemapEncodeReinhardPS.Reset();
		ratioApplyReinhardPS.Reset();
		preUpscaleGt7Unavailable = false;
		preUpscaleWidth = preUpscaleHeight = 0;
		lastPreUpscaleFrame = UINT32_MAX;
		preUpscaleRouteWasActive = false;
		preUpscaleLogged = false;
	}
}
