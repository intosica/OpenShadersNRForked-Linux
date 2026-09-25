// CopyDepthGuideCS.hlsl — converts the engine depth target into the typed
// single-channel guide Feature 18 consumes.
//
// The source's VALID region is not always its allocated size, and is not always
// the same size as the guide:
//
//   pre-upscale route  - depth holds the render-res subrect, guide is render res
//                        -> 1:1, this is a straight copy
//   post-tonemap route - UpscaleDepth() has already run, so depth is valid at
//                        display res, while the motion vectors it is paired with
//                        are only valid over the render-res subrect. The guide
//                        extent has to follow the motion vectors, so depth is
//                        downsampled to match
//
// Hence the explicit source extent: the caller knows which case it is in, and
// GetDimensions cannot distinguish a valid subrect from the allocation around it.
//
// Nearest sampling, not linear: averaging depth across a silhouette fabricates
// surfaces at intermediate depths that exist nowhere in the scene. A min-depth
// (closest-of-footprint) reduction would preserve silhouettes better still and
// is the obvious next refinement if edge behaviour needs it.

Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> DestinationDepth : register(u0);

cbuffer DepthGuideData : register(b0)
{
	uint2 SourceExtent;  // valid region of SourceDepth, in texels
	uint2 GuideExtent;   // size of DestinationDepth
};

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	if (any(dispatchThreadID.xy >= GuideExtent))
		return;

	float2 ratio = float2(SourceExtent) / float2(GuideExtent);
	uint2 sourceCoord = min(uint2((float2(dispatchThreadID.xy) + 0.5) * ratio), SourceExtent - 1);

	DestinationDepth[dispatchThreadID.xy] = SourceDepth[sourceCoord];
}
