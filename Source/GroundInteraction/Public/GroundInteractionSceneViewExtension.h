// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class UGroundInteractionWorldSubsystem;

class FGroundInteractionSceneViewExtension : public FWorldSceneViewExtension
{
public:
	FGroundInteractionSceneViewExtension(
		const FAutoRegister& AutoRegister,
		UGroundInteractionWorldSubsystem* InSubsystem,
		UWorld* InWorld);

	virtual ~FGroundInteractionSceneViewExtension() override = default;

	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
	
	TRefCountPtr<IPooledRenderTarget> GetStateRT_RenderThread(int32 SwapIndex, int32 ChannelIndex) const;

private:
	static FMatrix BuildBottomUpOrthoProjection(float HalfExtentX, float HalfExtentY, float MaxDepth);

	TWeakObjectPtr<UGroundInteractionWorldSubsystem> OwnerSubsystem;
	
	// StateRT[Swap][Channel]
	// Channel 0 = Depth R16F
	// Channel 1 = Fade  R16F
	TRefCountPtr<IPooledRenderTarget> StateRT[2][2];
	
	// 현재 프레임의 StateRT 인덱스를 저장하는 변수
	int32 CurrentStateIndex = 0;
	
	// Fade Update 의 누적 시간
	float FadeUpdateAccumulatorSeconds = 0.0f;
	
	// Update Depth 의 누적 시간
	float CaptureUpdateAccumulatorSeconds = 0.0f;
};