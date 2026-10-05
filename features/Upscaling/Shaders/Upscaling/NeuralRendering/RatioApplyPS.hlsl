// RatioApplyPS.hlsl — pre-upscale Neural Rendering, pass 2 of 2.
//
// Transfers Feature 18's change back onto the linear HDR scene. Rather than
// writing the model's LDR output over kMAIN (which would destroy HDR range
// before DLSS and the engine's own tonemapper ever see it), this recomputes the
// encode the model was given and applies the ratio between its output and that
// input as a multiplier on the original linear value.
//
// The invariant that matters: a pixel the model did not change must come out
// bit-for-bit unchanged. Two things are needed for that, and both are about
// doing the arithmetic in the domain where the quantization actually lives.
//
//   1. The model read an 8-bit texture, so its input must be quantized the same
//      way before it is used as a denominator. Comparing the model's quantized
//      output against a full-precision input makes every pixel look changed.
//
//   2. The ratio is taken in the ENCODED domain, where one code value is a
//      uniform 1/255, and expanded to a linear gain afterwards. Taking it in
//      linear space instead needs a denominator floor, and any floor large
//      enough to be safe near black is enormous relative to real shadow values
//      — a 1/255 floor applied to linear values is ~600x too big and crushes
//      everything below roughly 8% brightness.
//
// The additive one-code epsilon makes the degenerate case correct too: as both
// operands approach black the gain approaches 1, leaving near-black alone
// instead of amplifying quantization noise into it.
//
// Reads and writes are 1:1 with the render-res subrect (see TonemapEncodePS);
// the destination is a staging target because kMAIN is bound as an SRV here and
// cannot also be the render target.

#include "Upscaling/UpscaleVS.hlsl"

#if defined(PSHADER)

#	include "Upscaling/NeuralRendering/PreUpscaleShared.hlsli"

typedef VS_OUTPUT PS_INPUT;

Texture2D<float4> SceneColor : register(t0);
Texture2D<float4> NeuralColor : register(t1);

cbuffer PreUpscaleData : register(b0)
{
	float Exposure;
	float3 PreUpscalePad0;
};

static const float kCodeStep = 1.0 / 255.0;  // one 8-bit code value
static const float kMinGain = 0.25;
static const float kMaxGain = 4.0;

float4 main(PS_INPUT input) : SV_Target
{
	int3 coord = int3(int2(input.Position.xy), 0);
	float4 scene = SceneColor.Load(coord);

	// Quantize to match the R8G8B8A8_UNORM texture the model actually read.
	float3 modelInput = PreUpscaleQuantize(PreUpscaleEncode(scene.rgb, Exposure));
	float3 modelOutput = NeuralColor.Load(coord).rgb;

	float3 encodedGain = (modelOutput + kCodeStep) / (modelInput + kCodeStep);
	float3 gain = clamp(PreUpscaleExpandGain(encodedGain), kMinGain, kMaxGain);

	return float4(scene.rgb * gain, scene.a);
}

#endif
