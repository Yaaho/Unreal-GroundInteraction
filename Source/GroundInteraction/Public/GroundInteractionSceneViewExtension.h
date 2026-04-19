// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class UGroundInteractionWorldSubsystem;

class FGroundInteractionSceneViewExtension : public FWorldSceneViewExtension
{
public:
	FGroundInteractionSceneViewExtension (
		const FAutoRegister& AutoRegister,
		UGroundInteractionWorldSubsystem* InSubsystem,
		UWorld* InWorld);

	virtual ~FGroundInteractionSceneViewExtension() override = default;

	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;

private:
	static FMatrix BuildBottomUpOrthoProjection(float HalfExtentX, float HalfExtentY, float MaxDepth);
	
	TWeakObjectPtr<UGroundInteractionWorldSubsystem> OwnerSubsystem;
};