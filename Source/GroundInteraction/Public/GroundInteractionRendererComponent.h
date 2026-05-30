// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "GroundInteractionRendererComponent.generated.h"

class FGroundInteractionRendererProxy;
class UGroundInteractionWorldSubsystem;
class UMaterialParameterCollection;
class UTextureRenderTarget2D;

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class GROUNDINTERACTION_API UGroundInteractionRendererComponent : public USceneComponent
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
	
	// 컴포넌트의 트랜스폼이 변경되었을 때 스냅된 위치를 프록시와 MPC 에 전달
	virtual void SendRenderTransform_Concurrent() override;
	
	// 설정된 프로퍼티가 바뀔때마다 markrenderdynamicdatadirty 를 실행시킨다.
	// renderdynamicdata concurrent 에서 바뀐 컴포넌트의 프로퍼티를 프록시에 반영
	
	// RT size 변경 프록시 전송, 프리미티브 컬링, UpdateDepthPass 실행 등을 담당함
	void UpdateAndQueueDepthCapture_GameThread(FSceneViewFamily& InViewFamily);
	
	// 컴포넌트에서 MPC capture 정보를 갱신
	void UpdateMPC(
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
	
	// Depth Capture 주기 설정
	float DepthCaptureUpdateAccumulatorSeconds = 0.0f;
};
