#pragma once

#include "Runtime.h"

#include <array>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;

namespace NeuralRendering
{
	class Renderer
	{
	public:
		struct StereoEyeInput
		{
			ID3D11Resource* depth = nullptr;
			ID3D11ShaderResourceView* depthSRV = nullptr;
			// Valid region of `depth`, which is not always the guide size - see
			// CopyDepthGuideCS.hlsl. Zero means "same as the guide extent".
			std::uint32_t depthSourceWidth = 0;
			std::uint32_t depthSourceHeight = 0;
			ID3D11Resource* motionVectors = nullptr;
			std::uint32_t sourceX = 0;
			std::uint32_t sourceY = 0;
			float motionVectorScaleX = 1.0f;
			float motionVectorScaleY = 1.0f;
		};

		static Renderer& Instance();
		~Renderer();

		Renderer(const Renderer&) = delete;
		Renderer& operator=(const Renderer&) = delete;

		/// `depthSourceWidth/Height` describe the valid region of `depth`, which the
		/// post-tonemap route has already upscaled past the guide extent. Zero means
		/// "same as the guide extent" (a straight copy).
		bool Apply(ID3D11Device* device, ID3D11DeviceContext* context, std::uint32_t eyeIndex,
			ID3D11Resource* color, ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
			ID3D11Resource* motionVectors,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight,
			float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning,
			std::uint32_t depthSourceWidth = 0, std::uint32_t depthSourceHeight = 0);
		bool ApplyStereo(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* color,
			const std::array<StereoEyeInput, 2>& eyes,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight, const Tuning& tuning);
		void Reset();
		void ResetHistory();

		[[nodiscard]] bool IsFailureLatched() const;
		[[nodiscard]] std::uint32_t NgxResult() const;
		[[nodiscard]] std::uint64_t SuccessfulFrames() const;
		[[nodiscard]] const char* StatusText() const;

	private:
		Renderer();
		class State;
		State* state_ = nullptr;
	};
}