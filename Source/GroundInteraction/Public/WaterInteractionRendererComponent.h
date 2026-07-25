#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "PrimitiveComponentId.h"
#include "WaterInteractionRendererComponent.generated.h"

class FWaterInteractionRendererProxy;
class FSceneViewFamily;
class UGroundInteractionSettings;
class UGroundInteractionWorldSubsystem;
class UMaterialParameterCollection;
class UPrimitiveComponent;
class UTextureRenderTarget2D;
struct FInteractionPrimitiveRuntimeState;

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

	void UpdateWaterInteractionDepth_GameThread(FSceneViewFamily& InViewFamily);

private:
	void CreateRenderProxy();
	void DestroyRenderProxy();
	void UpdateRenderProxyTransform();
	void ApplyOutputRenderTarget();
	void UpdateMPC(const FVector& BottomInteractionCenterWS, const FVector& InteractionExtentWS) const;
	void BuildWaterDepthPassShowOnlyPrimitives(TSet<FPrimitiveComponentId>& OutShowOnlyPrimitives, TArray<UPrimitiveComponent*>& OutSelectedPrimitives, UGroundInteractionWorldSubsystem* Subsystem, const UGroundInteractionSettings* Settings, const FVector& InteractionCenterWS, const FVector& InteractionExtentWS, double CurrentTimeSeconds) const;
	bool IsPrimitiveNearWaterSurface(const FInteractionPrimitiveRuntimeState& PrimitiveState, const UGroundInteractionSettings* Settings) const;

	bool InitOutputRenderTarget(UTextureRenderTarget2D* RenderTarget, const FIntPoint& Size, const FLinearColor& ClearColor) const;

private:
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> HeightRT = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> MaterialParameterCollection = nullptr;

	TWeakObjectPtr<UGroundInteractionWorldSubsystem> OwnerSubsystem;

	FWaterInteractionRendererProxy* RenderProxy = nullptr;
	FSceneInterface* RegisteredScene = nullptr;
	FIntPoint RenderTargetSize = FIntPoint::ZeroValue;

	float DepthUpdateAccumulatorSeconds = 0.0f;
};
