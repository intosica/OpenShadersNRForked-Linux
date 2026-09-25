#pragma once

namespace NeuralRendering
{
	/** Runs DLSS Neural Rendering on the LDR foveated regions immediately before UI composite. */
	bool ApplyFoveatedLdr();

	/**
	 * Flat-only: runs DLSS Neural Rendering before the DLSS upscale, at the dynamic
	 * render size, instead of after the engine tonemapper at display size.
	 *
	 * Feature 18's cost scales with the pixel count it is created at, so evaluating
	 * at render resolution is materially cheaper — at DLSS Quality that is 1.64 MP
	 * against 3.69 MP for a 1440p display. The trade is that kMAIN here is linear
	 * scene-referred HDR rather than the display-referred buffer the model expects,
	 * so the pass encodes a display-referred copy and transfers the model's change
	 * back as a ratio (TonemapEncodePS / RatioApplyPS), and DLSS then accumulates a
	 * generative result into its temporal history.
	 *
	 * Mutually exclusive with ApplyFoveatedLdr; gated behind
	 * Settings::neuralRenderingPreUpscale, which defaults off.
	 */
	bool ApplyFlatPreUpscale();

	/** Releases all runtime and shared-resource state. */
	void Reset();
}