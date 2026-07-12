// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "PixelFormat.h"
#include "RenderDeferredCleanup.h"
#include "RHICommandList.h"
#include "SceneView.h"

struct IPooledRenderTarget;

// 프록시 내부 렌더 타겟에 쓰이는 구조체
struct FPlaneTexture
{
	TRefCountPtr<IPooledRenderTarget> PooledTarget;

	FRDGTextureRef TransientTextureRef = nullptr;
	FRDGTextureSRVRef TransientSRVRef = nullptr;
	FRDGTextureUAVRef TransientUAVRef = nullptr;

	FIntPoint AllocatedSize = FIntPoint::ZeroValue;
	EPixelFormat AllocatedFormat = PF_Unknown;
	
	// 새 RDG 그래프 시작 전 transient ref 비움
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


class GROUNDINTERACTION_API FGroundInteractionRendererProxy final : public FDeferredCleanupInterface
{
public:
	explicit FGroundInteractionRendererProxy(
		FTextureReferenceRHIRef InDepthTextureReferenceRHI,
		FTextureReferenceRHIRef InFadeTextureReferenceRHI,
		FIntVector InNumCell,
		FVector InCellSpacing,
		const FVector& InCaptureExtentWS,
		float InFadeRiseSpeed,
		float InFadeMaxHeight,
		float InFadeUpdateIntervalSeconds);
	
	virtual ~FGroundInteractionRendererProxy() override = default;

	// SVE 에서 호출되는 함수
	void Render(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily);
	
	// Depth Capture CustomRenderPass 추가 함수
	// 게임 스레드에서 호출됨
	void QueueDepthCapture_GameThread(
		FSceneViewFamily&InViewFamily,
		const TSet<FPrimitiveComponentId>& InShowOnlyPrimitives,
		const FVector& CaptureViewLocation,
		const FIntPoint& CaptureRenderTargetSize);
	
	// 컴포넌트 파괴 시 호출되는 함수
	void Release();
	
	// 컴포넌트 등록 시 호출되는 함수
	static void RegisterProxy(FSceneInterface* InScene,FGroundInteractionRendererProxy* InProxy);

	// 컴포넌트 해제 시 호출되는 함수
	static void UnregisterProxy(FSceneInterface* InScene,FGroundInteractionRendererProxy* InProxy);
	
	// 프록시 목록을 반환하는 함수
	static TArray<FGroundInteractionRendererProxy*> GetProxies(FSceneInterface* InScene);
	
	
public:
	// FDepthCapturePass::OnPostRender 에서 호출되는 함수
	// custom depth capture 결과를 CurrentStateIndex texture 에 반영
	void UpdateCapturedDepth(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef CapturedDepthTexture,
		const FIntPoint& PassRenderTargetSize,
		float PassMaxSceneDepth,
		float PassSceneDepthInvalidEpsilon,
		const FVector& PassCaptureViewLocation,
		ERHIFeatureLevel::Type FeatureLevel);
	
public:
	// RendererComponent 에서 세팅하는 값들
	FVector CurrentSnappedLocation = FVector::ZeroVector;
	FIntPoint RenderTargetSize = FIntPoint(512, 512);
	bool bRenderTargetChanged = false;
	
private:
	// DepthFadeTextures 생성 및 RT 사이즈가 바뀌었을 시 초기화 하는 함수
	void CheckDepthFadeTextures(FRDGBuilder& GraphBuilder, const FIntPoint& InRenderTargetSize, bool bClearAll);
	
	// Output RT 에 결과를 복사하는 함수
	void CopyDepthFadeToOutput(FRDGBuilder& GraphBuilder, int32 DepthFadeTextureId);
	
	// DepthFadeTextures 의 transient RDG ref 초기화 함수
	void ResetDepthFadeTransientRefs();
	
	
	// 이전 프레임의 DepthFadeTexture ID 가져오는 함수
	int32 GetPrevDepthFadeTextureId() const
	{
		return CurrentStateIndex;
	}

	// 현재 프레임의 DepthFadeTexture ID 가져오는 함수
	int32 GetCurrentDepthFadeTextureId() const
	{
		return 1 - CurrentStateIndex;
	}

	// Move 패스 후 DepthFadeTexture 의 State Index 를 진행시키는 함수
	int32 AdvanceDepthFadeTextureId()
	{
		CurrentStateIndex = 1 - CurrentStateIndex;
		return CurrentStateIndex;
	}
	
	// 캡쳐 월드 위치를 가져오는 함수
	FVector GetCaptureOriginWS() const
	{
		return FVector(CurrentSnappedLocation.X, CurrentSnappedLocation.Y, CurrentSnappedLocation.Z - CaptureExtentWS.Z);
	}
	
	// 이전 프레임의 캡쳐 위치를 가져오는 함수
	FVector GetPreviousCaptureOriginWS() const
	{
		return FVector(PreviousSnappedLocation.X,PreviousSnappedLocation.Y, PreviousSnappedLocation.Z - CaptureExtentWS.Z);
	}
	
	// 최대 씬댑스를 가져오는 함수
	float GetMaxSceneDepth() const
	{
		return FMath::Max(CaptureExtentWS.Z * 2.0f, 1.0f);
	}

	float GetSceneDepthInvalidEpsilon() const
	{
		return FMath::Clamp(GetMaxSceneDepth() * 0.0005f, 0.1f, 2.0f);
	}

	static FMatrix BuildBottomUpOrthoProjection(float HalfExtentX, float HalfExtentY, float MaxDepth);
	
private:
	FTextureReferenceRHIRef OutputDepthTextureReferenceRHI;
	FTextureReferenceRHIRef OutputFadeTextureReferenceRHI;

	FVector CellSpacing = FVector::OneVector;

	// 프록시가 자체적으로 유지하는 렌더 히스토리.
	FVector PreviousSnappedLocation = FVector::ZeroVector;
	bool bHasPreviousCapture = false;

	// 현재 프록시가 사용하는 capture 설정.
	FVector CaptureExtentWS = FVector(1000.0f, 1000.0f, 1000.0f);

	float FadeRiseSpeed = 150.0f;
	float FadeMaxHeight = 120.0f;
	float FadeUpdateIntervalSeconds = 1.0f / 60.0f;

	FPlaneTexture DepthFadeTextures[2][2];

	int32 CurrentStateIndex = 0;

	float FadeUpdateAccumulatorSeconds = 0.0f;

	bool bReleased = false;
	bool bClearDepthFadeTextures = true;
};

