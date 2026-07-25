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
	
	// 컴포넌트 transform 변경 시 interaction 위치를 프록시와 MPC에 전달
	virtual void SendRenderTransform_Concurrent() override;
	
	// 프로퍼티 변경까지 반영해야 하면 MarkRenderDynamicDataDirty를 호출한다.
	// RenderDynamicData_Concurrent에서 바뀐 컴포넌트 프로퍼티를 프록시에 반영한다.
	
	// RT size 변경 전달, 프리미티브 컬렉션, UpdateDepthPass 실행을 담당
	void UpdateGroundInteractionDepth_GameThread(FSceneViewFamily& InViewFamily);
	
	// 컴포넌트에서 MPC interaction 정보를 갱신
	void UpdateMPC(const FVector& BottomInteractionCenterWS, const FVector& InteractionExtentWS) const;
	
private:
	void CreateRenderProxy();
	void DestroyRenderProxy();
	void UpdateRenderProxyTransform();
	void ApplyOutputRenderTargets();

	bool InitOutputRenderTarget(UTextureRenderTarget2D* RenderTarget, const FIntPoint& Size, const FLinearColor& ClearColor) const;

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
	
	// Depth update 주기 누적값
	float DepthUpdateAccumulatorSeconds = 0.0f;
};
