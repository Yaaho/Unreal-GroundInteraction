// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"
#include "GroundInteractionWorldSubsystem.generated.h"

class FGroundInteractionSceneViewExtension;
class USceneComponent;
class UPrimitiveComponent;
class UGroundInteractionRendererComponent;
class UGroundInteractionTargetComponent;
class UWaterInteractionRendererComponent;
struct FStreamableHandle;

struct FInteractionPrimitiveRuntimeState
{
	FVector PreviousLocationWS = FVector::ZeroVector;
	FVector CurrentLocationWS = FVector::ZeroVector;
	FVector VelocityWS = FVector::ZeroVector;
	FBoxSphereBounds BoundsWS = FBoxSphereBounds(ForceInit);
	bool bHasPreviousLocation = false;

	double NextWaterPulseTimeSeconds = 0.0;
	uint64 LastWaterPulseFrameNumber = MAX_uint64;
};

UCLASS()
class GROUNDINTERACTION_API UGroundInteractionWorldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()
	
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;
	
	void RegisterTarget(UGroundInteractionTargetComponent* Target);
	void UnregisterTarget(UGroundInteractionTargetComponent* Target);
	
	void RegisterInteractionPrimitive(UPrimitiveComponent* Primitive);
	void UnregisterInteractionPrimitive(UPrimitiveComponent* Primitive);
	
	const TArray<TWeakObjectPtr<UPrimitiveComponent>>& GetInteractionPrimitives() const
	{
		return InteractionPrimitives;
	}

	void UpdateInteractionPrimitiveStates_GameThread(double CurrentTimeSeconds);
	const FInteractionPrimitiveRuntimeState* FindInteractionPrimitiveState(const UPrimitiveComponent* Primitive) const;
	bool CanWaterInteractionPulse_GameThread(const UPrimitiveComponent* Primitive, double CurrentTimeSeconds) const;
	void ConsumeWaterInteractionPulse_GameThread(const UPrimitiveComponent* Primitive, double CurrentTimeSeconds, float PulseIntervalSeconds, float PulseRandomIntervalSeconds);
	
	UGroundInteractionRendererComponent* GetRendererComponent() const
	{
		return RendererComponent;
	}

	UWaterInteractionRendererComponent* GetWaterRendererComponent() const
	{
		return WaterRendererComponent;
	}

	UTextureRenderTarget2D* GetDepthRT() const
	{
		return DepthRT;
	}

	UTextureRenderTarget2D* GetFadeRT() const
	{
		return FadeRT;
	}

	UTextureRenderTarget2D* GetWaterHeightRT() const
	{
		return WaterHeightRT;
	}

	UStaticMesh* GetCubeMesh() const
	{
		return CubeMesh;
	}

	UStaticMesh* GetSphereMesh() const
	{
		return SphereMesh;
	}

	UStaticMesh* GetCapsuleMesh() const
	{
		return CapsuleMesh;
	}

	UMaterialParameterCollection* GetMaterialParameterCollection() const
	{
		return MaterialParameterCollection;
	}

	bool AreAssetsReady() const
	{
		return bAssetsReady;
	}

	bool AreWaterAssetsReady() const
	{
		return bWaterAssetsReady;
	}
	
	const TSharedPtr<FGroundInteractionSceneViewExtension, ESPMode::ThreadSafe>& GetSceneViewExtension() const
	{
		return SceneViewExtension;
	}

private:
	void BeginAsyncLoadAssets();
	void OnAssetsLoaded();

	void CreateOrUpdateRendererActor();
	void CreateOrUpdateGroundRendererComponent();
	void CreateOrUpdateWaterRendererComponent();
	void DestroyRendererComponent();
	
	bool GetPlayerCameraLocation(FVector& OutCameraLocationWS) const;

private:
	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UGroundInteractionTargetComponent>> RegisteredTargets;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UPrimitiveComponent>> InteractionPrimitives;

	TMap<FObjectKey, FInteractionPrimitiveRuntimeState> InteractionPrimitiveStates;
	uint64 LastPrimitiveStateUpdateFrameNumber = MAX_uint64;
	double LastPrimitiveStateUpdateTimeSeconds = -1.0;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DepthRT = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> FadeRT = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> WaterHeightRT = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CubeMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> SphereMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CapsuleMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> MaterialParameterCollection = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<AActor> RendererActor = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> RendererRootComponent = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UGroundInteractionRendererComponent> RendererComponent = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UWaterInteractionRendererComponent> WaterRendererComponent = nullptr;

	bool bAssetsReady = false;
	bool bWaterAssetsReady = false;

	TSharedPtr<FStreamableHandle> AssetLoadHandle;
	TSharedPtr<FGroundInteractionSceneViewExtension, ESPMode::ThreadSafe> SceneViewExtension;
};
