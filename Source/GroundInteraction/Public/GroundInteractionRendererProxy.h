// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GroundInteractionRendering.h"
#include "RenderDeferredCleanup.h"
#include "SceneView.h"

class GROUNDINTERACTION_API FGroundInteractionRendererProxy final : public FDeferredCleanupInterface
{
public:
	explicit FGroundInteractionRendererProxy(
		FTextureReferenceRHIRef InDepthTextureReferenceRHI,
		FTextureReferenceRHIRef InFadeTextureReferenceRHI,
		FIntVector InNumCell,
		FVector InCellSpacing,
		const FVector& InInteractionExtentWS,
		float InFadeRiseSpeed,
		float InFadeMaxHeight,
		float InFadeUpdateIntervalSeconds);
	
	virtual ~FGroundInteractionRendererProxy() override = default;

	// SVE에서 호출되는 함수
	void Render(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily);
	
	// GroundInteractionDepth CustomRenderPass 추가 함수
	// 게임 스레드에서 호출
	void QueueGroundInteractionDepthPass_GameThread(
		FSceneViewFamily&InViewFamily,
		const TSet<FPrimitiveComponentId>& InShowOnlyPrimitives,
		const FVector& BottomInteractionCenterWS,
		const FIntPoint& GroundInteractionRenderTargetSize);
	
	// 컴포넌트 파괴 시 호출되는 함수
	void Release();
	
	// 컴포넌트 등록 시 호출되는 함수
	static void RegisterProxy(FSceneInterface* InScene,FGroundInteractionRendererProxy* InProxy);

	// 컴포넌트 해제 시 호출되는 함수
	static void UnregisterProxy(FSceneInterface* InScene,FGroundInteractionRendererProxy* InProxy);
	
	// 프록시 목록을 반환하는 함수
	static TArray<FGroundInteractionRendererProxy*> GetProxies(FSceneInterface* InScene);
	
	
public:
	// FGroundInteractionDepthPass::OnPostRender에서 호출되는 함수
	// custom depth 결과를 다음 Render에서 사용할 capture texture에 보관
	void StoreGroundInteractionDepthCapture(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef DepthPassTexture,
		const FIntPoint& PassRenderTargetSize,
		const FVector& PassBottomInteractionCenterWS);
	
public:
	// RendererComponent에서 세팅하는 값들
	FVector CurrentSnappedLocation = FVector::ZeroVector;
	FIntPoint RenderTargetSize = FIntPoint(512, 512);
	bool bRenderTargetChanged = false;
	
private:
	// DepthFadeTextures 생성 및 RT 사이즈가 바뀌었을 때 초기화하는 함수
	void CheckDepthFadeTextures(FRDGBuilder& GraphBuilder, const FIntPoint& InRenderTargetSize, bool bClearAll);
	
	// Output RT에 결과를 복사하는 함수
	void CopyDepthFadeToOutput(FRDGBuilder& GraphBuilder, int32 DepthFadeTextureId);
	
	// 프록시가 보관하는 texture들의 transient RDG ref 초기화 함수
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

	// Scrolling 패스 후 DepthFadeTexture의 State Index를 진행시키는 함수
	int32 AdvanceDepthFadeTextureId()
	{
		CurrentStateIndex = 1 - CurrentStateIndex;
		return CurrentStateIndex;
	}
	
	// 현재 bottom interaction center를 가져오는 함수
	FVector GetBottomInteractionCenterWS() const
	{
		return FVector(CurrentSnappedLocation.X, CurrentSnappedLocation.Y, CurrentSnappedLocation.Z - InteractionExtentWS.Z);
	}
	
	// 이전 bottom interaction center를 가져오는 함수
	FVector GetPreviousBottomInteractionCenterWS() const
	{
		return FVector(PreviousSnappedLocation.X, PreviousSnappedLocation.Y, PreviousSnappedLocation.Z - InteractionExtentWS.Z);
	}
	
	// 최대 씬 뎁스를 가져오는 함수
	float GetMaxSceneDepth() const
	{
		return FMath::Max(InteractionExtentWS.Z * 2.0f, 1.0f);
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

	// 현재 프록시가 사용하는 interaction 설정.
	FVector InteractionExtentWS = FVector(1000.0f, 1000.0f, 1000.0f);

	float FadeRiseSpeed = 150.0f;
	float FadeMaxHeight = 120.0f;
	float FadeUpdateIntervalSeconds = 1.0f / 60.0f;

	FPlaneTexture DepthFadeTextures[2][2];
	FPlaneTexture CaptureDepthTexture;

	FVector CaptureBottomInteractionCenterWS = FVector::ZeroVector;
	bool bHasDepthCapture = false;

	int32 CurrentStateIndex = 0;

	float FadeUpdateAccumulatorSeconds = 0.0f;

	bool bReleased = false;
	bool bClearDepthFadeTextures = true;
};
