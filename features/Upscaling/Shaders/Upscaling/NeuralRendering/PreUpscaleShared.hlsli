// PreUpscaleShared.hlsli — display-referred encode for the pre-upscale Neural
// Rendering route.
//
// Feature 18 is a display-referred model: the post-tonemap route hands it
// kFRAMEBUFFER, an 8-bit gamma-encoded SDR buffer. Running the pass before DLSS
// means running it on kMAIN, which is linear scene-referred HDR (FP16,
// pre-exposure, unbounded). Handing that to the model directly would put it far
// outside the range its tone and structure priors were built for.
//
// Instead the route encodes a display-referred copy for the model, and transfers
// the model's *change* back onto the HDR buffer as a ratio (see RatioApplyPS).
// Because the ratio divides out the encode, the exact curve is not critical —
// it only has to land values in a range the model is happy with. Exposure is
// user-tunable because the engine's own adaptation runs later, in ISHDR, and is
// not available to us here.

#ifndef NEURALRENDERING_PREUPSCALESHARED_HLSLI
#define NEURALRENDERING_PREUPSCALESHARED_HLSLI

// The curve's job is to show the model something with roughly the contrast of
// the final image. It does NOT have to be the inverse of anything: the ratio
// transfer divides the encode out, so a pixel the model leaves alone is
// unchanged whatever curve is used here. What the curve does affect is what the
// model *decides* - a flat curve reads as a washed-out image and provokes a
// large local-tone correction, which then compounds with the real tonemapper
// downstream and shows up as excess contrast.
//
// So match the real one. This build's post chain runs GT7 (Post Processing ->
// Color Grading), so use it directly when it is reachable. PREUPSCALE_GT7 is
// defined by the C++ side, which falls back to the Reinhard permutation if the
// Post Processing shaders are not installed and the include cannot resolve.
#if defined(PREUPSCALE_GT7)
#	include "PostProcessing/ColorGrading/Include/GT7ToneMapping.hlsli"
#endif

float3 PreUpscaleTonemap(float3 a_linear, float a_exposure)
{
	float3 exposed = max(a_linear * a_exposure, 0.0);
#if defined(PREUPSCALE_GT7)
	// Returns display-linear SDR in [0, 1]; the gamma encode is applied below.
	return saturate(GT7ToneMappingSDR(exposed));
#else
	return exposed / (1.0 + exposed);
#endif
}

// Scene-linear HDR -> gamma-encoded display-referred, matching the 8-bit
// gamma-encoded buffer the post-tonemap route feeds the model.
float3 PreUpscaleEncode(float3 a_linear, float a_exposure)
{
	return pow(saturate(PreUpscaleTonemap(a_linear, a_exposure)), 1.0 / 2.2);
}

// Round to the 8-bit grid of the texture the model reads, so an unchanged pixel
// compares exactly equal instead of differing by its own quantization error.
float3 PreUpscaleQuantize(float3 a_encoded)
{
	return round(saturate(a_encoded) * 255.0) / 255.0;
}

// Convert a ratio measured between two ENCODED values into a multiplier for
// scene-linear light. Equivalent to taking the ratio after expanding both sides,
// but conditioned far better: the division happens between two similar values on
// the same quantization grid, rather than between two expanded near-zero ones.
float3 PreUpscaleExpandGain(float3 a_encodedGain)
{
	return pow(max(a_encodedGain, 0.0), 2.2);
}

#endif
