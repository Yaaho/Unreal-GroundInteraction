#include "GroundInteractionRendering.h"

#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"

bool FPlaneTexture::CheckPooledTarget(FRHICommandListImmediate& RHICmdList, const FIntPoint& InSize, EPixelFormat InFormat, const TCHAR* DebugName)
{
	const FIntPoint SafeSize(FMath::Max(InSize.X, 1), FMath::Max(InSize.Y, 1));

	const bool bNeedsAllocateRenderTarget = !PooledTarget.IsValid() || AllocatedSize != SafeSize || AllocatedFormat != InFormat;

	if (!bNeedsAllocateRenderTarget)
	{
		return false;
	}

	FPooledRenderTargetDesc Desc = FPooledRenderTargetDesc::Create2DDesc(
		SafeSize,
		InFormat,
		FClearValueBinding::None,
		TexCreate_None,
		TexCreate_ShaderResource | TexCreate_UAV,
		false);

	Desc.DebugName = DebugName;

	GRenderTargetPool.FindFreeElement(RHICmdList, Desc, PooledTarget, DebugName);

	AllocatedSize = SafeSize;
	AllocatedFormat = InFormat;

	ResetTransientRefs();

	return true;
}

FRDGTextureRef FPlaneTexture::GetOrCreateTexture(FRDGBuilder& GraphBuilder, const TCHAR* Name)
{
	if (!PooledTarget.IsValid())
	{
		return nullptr;
	}

	if (!TransientTextureRef)
	{
		TransientTextureRef = GraphBuilder.RegisterExternalTexture(PooledTarget, Name);
	}

	return TransientTextureRef;
}

FRDGTextureSRVRef FPlaneTexture::GetOrCreateSRV(FRDGBuilder& GraphBuilder, const TCHAR* Name)
{
	FRDGTextureRef TextureRef = GetOrCreateTexture(GraphBuilder, Name);

	if (!TextureRef)
	{
		return nullptr;
	}

	if (!TransientSRVRef)
	{
		TransientSRVRef = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TextureRef));
	}

	return TransientSRVRef;
}

FRDGTextureUAVRef FPlaneTexture::GetOrCreateUAV(FRDGBuilder& GraphBuilder, const TCHAR* Name)
{
	FRDGTextureRef TextureRef = GetOrCreateTexture(GraphBuilder, Name);

	if (!TextureRef)
	{
		return nullptr;
	}

	if (!TransientUAVRef)
	{
		TransientUAVRef = GraphBuilder.CreateUAV(TextureRef);
	}

	return TransientUAVRef;
}
