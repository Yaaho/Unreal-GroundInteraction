// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GroundInteractionRendererComponent.generated.h"

class FGroundInteractionRendererProxy;
class UGroundInteractionWorldSubsystem;
class UMaterialParameterCollection;
class UTextureRenderTarget2D;
class FSceneViewFamily;

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class GROUNDINTERACTION_API UGroundInteractionRendererComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGroundInteractionRendererComponent();
	
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
	
	void InitializeRenderer(
		UGroundInteractionWorldSubsystem* InSubsystem,
		UTextureRenderTarget2D* InDepthRT,
		UTextureRenderTarget2D* InFadeRT,
		UMaterialParameterCollection* InMaterialParameterCollection);
	
	// 렌더 컴포넌트 프레임 준비 함수.
	void PrepareFrame_GameThread(
		UGroundInteractionWorldSubsystem* InSubsystem,
		FSceneViewFamily& InViewFamily);
	
	// 프록시에서 호출되는 MPC 설정 함수
	void UpdateMPC_GameThread(
		const FVector& CaptureOriginWS,
		const FVector& CaptureExtentWS) const;
	
private:
	void CreateRenderProxy();
	void DestroyRenderProxy();

	void ApplyOutputRenderTargets();

	bool InitOutputRenderTarget(
		UTextureRenderTarget2D* RenderTarget,
		const FIntPoint& Size,
		const FLinearColor& ClearColor) const;

	bool GetPlayerCameraLocation(
		FVector& OutCameraLocationWS) const;

private:
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DepthRT = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> FadeRT = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> MaterialParameterCollection = nullptr;
	
	TWeakObjectPtr<UGroundInteractionWorldSubsystem> OwnerSubsystem;

	FGroundInteractionRendererProxy* RenderProxy = nullptr;
	FSceneInterface* RegisteredScene = nullptr;
	
	FIntPoint RenderTargetSize = FIntPoint::ZeroValue;
	
	FVector CurrentCaptureCenterWS = FVector::ZeroVector;
};
