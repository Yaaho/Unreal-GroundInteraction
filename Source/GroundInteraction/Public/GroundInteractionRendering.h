#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "PixelFormat.h"
#include "RHICommandList.h"

struct IPooledRenderTarget;

// 프록시 내부 렌더 타겟 데이터 구조체
struct GROUNDINTERACTION_API FPlaneTexture
{
	TRefCountPtr<IPooledRenderTarget> PooledTarget;

	FRDGTextureRef TransientTextureRef = nullptr;
	FRDGTextureSRVRef TransientSRVRef = nullptr;
	FRDGTextureUAVRef TransientUAVRef = nullptr;

	FIntPoint AllocatedSize = FIntPoint::ZeroValue;
	EPixelFormat AllocatedFormat = PF_Unknown;

	// 새 RDG graph 시작 전 transient ref 비움
	void ResetTransientRefs()
	{
		TransientTextureRef = nullptr;
		TransientSRVRef = nullptr;
		TransientUAVRef = nullptr;
	}

	// 필요한 크기/포맷의 pooled render target 보장
	bool CheckPooledTarget(FRHICommandListImmediate& RHICmdList, const FIntPoint& InSize, EPixelFormat InFormat, const TCHAR* DebugName);

	// pooled target을 현재 RDG graph의 texture로 등록
	FRDGTextureRef GetOrCreateTexture(FRDGBuilder& GraphBuilder, const TCHAR* Name);

	// 현재 RDG graph의 SRV 반환
	FRDGTextureSRVRef GetOrCreateSRV(FRDGBuilder& GraphBuilder, const TCHAR* Name);

	// 현재 RDG graph의 UAV 반환
	FRDGTextureUAVRef GetOrCreateUAV(FRDGBuilder& GraphBuilder, const TCHAR* Name);
};
