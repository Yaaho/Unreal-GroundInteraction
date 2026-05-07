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
	
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
	
	// 프록시의 렌더 함수 호출
	virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;

private:
	TWeakObjectPtr<UGroundInteractionWorldSubsystem> OwnerSubsystem;
};
