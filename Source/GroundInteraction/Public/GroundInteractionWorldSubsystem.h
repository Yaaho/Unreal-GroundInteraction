// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GroundInteractionWorldSubsystem.generated.h"

class FGroundInteractionSceneViewExtension;
class USceneComponent;
class UPrimitiveComponent;
class UGroundInteractionRendererComponent;
class UGroundInteractionSettings;
class UGroundInteractionTargetComponent;
class UNiagaraComponent;
class UNiagaraSystem;
class UWaterInteractionRendererComponent;
struct FStreamableHandle;

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

	void AddWaterSplat(const FVector& PositionWS, const FVector2D& SizeWS);
	
	const TArray<TWeakObjectPtr<UPrimitiveComponent>>& GetInteractionPrimitives() const
	{
		return InteractionPrimitives;
	}
	
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
	void CreateOrUpdateWaterSplatComponent();
	void CreateOrUpdateWaterRendererComponent();
	void DestroyRendererComponent();

	void UpdateWaterSplats();
	FVector GetWaterSplatVelocityWS(const UPrimitiveComponent* Primitive) const;
	bool TryCalculateWaterContact(const UPrimitiveComponent* Primitive, const UGroundInteractionSettings* Settings, FVector& OutPositionWS, FVector2D& OutSizeWS) const;
	
	bool GetPlayerCameraLocation(FVector& OutCameraLocationWS) const;

private:
	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UGroundInteractionTargetComponent>> RegisteredTargets;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UPrimitiveComponent>> InteractionPrimitives;

	TArray<FVector> PendingWaterSplatPositions;
	TArray<FVector2D> PendingWaterSplatSizes;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DepthRT = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> FadeRT = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> WaterHeightRT = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraSystem> WaterSplatSystem = nullptr;

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
	TObjectPtr<UNiagaraComponent> WaterSplatComponent = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UWaterInteractionRendererComponent> WaterRendererComponent = nullptr;

	bool bAssetsReady = false;
	bool bWaterAssetsReady = false;

	TSharedPtr<FStreamableHandle> AssetLoadHandle;
	TSharedPtr<FGroundInteractionSceneViewExtension, ESPMode::ThreadSafe> SceneViewExtension;
};
