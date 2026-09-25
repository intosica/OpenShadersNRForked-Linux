#pragma once

/**
 * @brief Overrides shadow cascade rasterizer states to fix peter-panning and self-shadowing.
 *
 * The vanilla engine shares a single set of rasterizer states across all shadow cascades.
 * This fix clones those states per-cascade and applies tuned depth-bias, depth-bias-clamp,
 * and slope-scaled-bias values to reduce shadow acne on near cascades and peter-panning
 * on far cascades.
 */
struct ShadowmapRasterizerFix : EngineFix
{
	/** @brief Returns the human-readable name of this fix. */
	std::string GetName() override { return "Shadowmap Cascade Rasterizer Fix"; }

	/** @brief Installs the per-cascade rasterizer state hook into the shadow rendering pipeline. */
	void Install() override;

	// gRasterStates is [fill=2][cull=3][depthBias=N][scissor=2] of rasterizer-state pointers.
	// The depth-bias dimension is 12 on flat (SE/AE) but 13 on VR (one extra preset), so the
	// engine indexes with a per-runtime stride. We store flat and stride at runtime: a fixed C
	// array type would bake the wrong stride on one platform and scatter the per-cascade caster
	// states into the wrong slots (the historical VR shadow-cascade flicker).
	static constexpr int kFill = 2, kCull = 3, kScissor = 2, kMaxDepth = 13;
	static constexpr int kMaxStates = kFill * kCull * kMaxDepth * kScissor;  // 156 (VR); flat uses 144
	using RasterStatePtr = ID3D11RasterizerState*;

	static inline int depthDim = 12;  // set to 13 for VR in Install()
	static int StateCount() { return kFill * kCull * depthDim * kScissor; }
	static int StateIndex(int fill, int cull, int depth, int scissor)
	{
		return ((fill * kCull + cull) * depthDim + depth) * kScissor + scissor;
	}

	/**
	 * @brief Clones the global rasterizer state array and applies cascade-specific depth bias settings.
	 * @param inputArray Pointer to the game's global rasterizer state array.
	 * @param cascade Zero-based cascade index whose descriptor values are applied to the cloned states.
	 */
	static void CloneRasterStates(RasterStatePtr* inputArray, int cascade);

	static constexpr uint maxCascades = 3;

	static inline RasterStatePtr* gRasterStates = nullptr;
	static inline RasterStatePtr backupGameRasterStates[kMaxStates] = {};
	static inline RasterStatePtr shadowmapRasterStates[maxCascades][kMaxStates] = {};

	static constexpr int firstCascadeDepthBias = 160;
	static constexpr float firstCascadeDepthBiasClamp = 0.015f;
	static constexpr float firstCascadeSlopeScaleBias = 3.2f;

	static constexpr int secondCascadeDepthBias = 100;
	static constexpr float secondCascadeDepthBiasClamp = 0.015f;
	static constexpr float secondCascadeSlopeScaleBias = 3.8f;

	static constexpr int thirdCascadeDepthBias = 100;
	static constexpr float thirdCascadeDepthBiasClamp = 0.015f;
	static constexpr float thirdCascadeSlopeScaleBias = 3.8f;

	struct ShadowMapRasterizerDescriptor
	{
		int rasterDepthBias;
		float rasterDepthBiasClamp;
		float rasterSlopeScaleBias;
	};
	/**
	 * @brief Applies shadow-map-specific depth bias values to a rasterizer descriptor.
	 * @param outputDesc Rasterizer descriptor whose depth bias fields will be overwritten.
	 * @param desc Shadow map bias parameters to apply.
	 */
	static void GetUpdatedRasterDesc(D3D11_RASTERIZER_DESC& outputDesc, ShadowMapRasterizerDescriptor desc);

	static constexpr ShadowMapRasterizerDescriptor cascadeDescriptors[maxCascades] = {
		{ firstCascadeDepthBias, firstCascadeDepthBiasClamp, firstCascadeSlopeScaleBias },
		{ secondCascadeDepthBias, secondCascadeDepthBiasClamp, secondCascadeSlopeScaleBias },
		{ thirdCascadeDepthBias, thirdCascadeDepthBiasClamp, thirdCascadeSlopeScaleBias }
	};

	struct BSShadowDirectionalLight_RenderShadowmaps_RenderCascade
	{
		static void thunk(RE::BSShadowDirectionalLight* light, void* a_descriptor, void* arg2, uint32_t flags);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
