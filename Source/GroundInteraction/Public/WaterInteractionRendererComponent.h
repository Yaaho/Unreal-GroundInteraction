#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "WaterInteractionRendererComponent.generated.h"

class FWaterInteractionRendererProxy;
class FSceneViewFamily;
class UGroundInteractionWorldSubsystem;
class UMaterialParameterCollection;
class UTextureRenderTarget2D;

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class GROUNDINTERACTION_API UWaterInteractionRendererComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UWaterInteractionRendererComponent();

	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
	virtual void SendRenderTransform_Concurrent() override;

	void InitializeRenderer(
		UGroundInteractionWorldSubsystem* InSubsystem,
		UTextureRenderTarget2D* InHeightRT,
		UMaterialParameterCollection* InMaterialParameterCollection);

	void UpdateWaterInteraction_GameThread(FSceneViewFamily& InViewFamily);

private:
	void CreateRenderProxy();
	void DestroyRenderProxy();
	void UpdateRenderProxyTransform();
	void ApplyOutputRenderTarget();
	void UpdateMPC(const FVector& BottomInteractionCenterWS, const FVector& InteractionExtentWS) const;

	bool InitOutputRenderTarget(UTextureRenderTarget2D* RenderTarget, const FIntPoint& Size, const FLinearColor& ClearColor) const;

private:
	TWeakObjectPtr<UGroundInteractionWorldSubsystem> OwnerSubsystem;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> HeightRT = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> MaterialParameterCollection = nullptr;

	FWaterInteractionRendererProxy* RenderProxy = nullptr;
	FSceneInterface* RegisteredScene = nullptr;
	FIntPoint RenderTargetSize = FIntPoint::ZeroValue;

	float UpdateAccumulatorSeconds = 0.0f;
};
