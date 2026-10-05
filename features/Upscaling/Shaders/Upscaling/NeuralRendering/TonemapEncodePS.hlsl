// TonemapEncodePS.hlsl — pre-upscale Neural Rendering, pass 1 of 2.
//
// Encodes the render-res subrect of kMAIN (linear scene-referred HDR) into a
// display-referred LDR copy for Feature 18. Reuses UpscaleVS.hlsl for the
// fullscreen triangle.
//
// The destination is allocated at exactly the dynamic render size and kMAIN
// holds this frame's scene in its top-left subrect of the same size, so the
// mapping is 1:1 and Load() is exact — no sampler, no UV scaling, no filtering
// error ahead of the model.

#include "Upscaling/UpscaleVS.hlsl"

#if defined(PSHADER)

#	include "Upscaling/NeuralRendering/PreUpscaleShared.hlsli"

typedef VS_OUTPUT PS_INPUT;

Texture2D<float4> SceneColor : register(t0);

cbuffer PreUpscaleData : register(b0)
{
	float Exposure;
	float3 PreUpscalePad0;
};

float4 main(PS_INPUT input) : SV_Target
{
	int3 coord = int3(int2(input.Position.xy), 0);
	float3 sceneLinear = SceneColor.Load(coord).rgb;
	return float4(PreUpscaleEncode(sceneLinear, Exposure), 1.0);
}

#endif
