// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GroundInteractionWorldSubsystem.generated.h"

class FGroundInteractionSceneViewExtension;
class UGroundInteractionRendererComponent;
class UGroundInteractionTargetComponent;
class FSceneViewFamily;
struct FStreamableHandle;

UCLASS()
class GROUNDINTERACTION_API UGroundInteractionWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
	
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	void RegisterTarget(UGroundInteractionTargetComponent* Target);
	void UnregisterTarget(UGroundInteractionTargetComponent* Target);
	
	void RegisterInteractionPrimitive(UPrimitiveComponent* Primitive);
	void UnregisterInteractionPrimitive(UPrimitiveComponent* Primitive);
	
	const TArray<TWeakObjectPtr<UPrimitiveComponent>>& GetInteractionPrimitives() const
	{
		return InteractionPrimitives;
	}
	
	// BeginRenderViewFamily 에서 해당 함수가 호출됨
	// 게임스레드에서 렌더 컴포넌트를 업데이트하는 함수
	void PrepareRendererFrame_GameThread(
		FSceneViewFamily& InViewFamily);

	UTextureRenderTarget2D* GetDepthRT() const
	{
		return DepthRT;
	}

	UTextureRenderTarget2D* GetFadeRT() const
	{
		return FadeRT;
	}

	UStaticMesh* GetCubeMesh() const
	{
		return CubeMesh;
	}

	UStaticMesh* GetSphereMesh() const
	{
		return SphereMesh;
	}

	UStaticMesh* GetCylinderMesh() const
	{
		return CylinderMesh;
	}

	UMaterialParameterCollection* GetMaterialParameterCollection() const
	{
		return MaterialParameterCollection;
	}

	bool AreAssetsReady() const
	{
		return bAssetsReady;
	}
	
	const TSharedPtr<FGroundInteractionSceneViewExtension, ESPMode::ThreadSafe>& GetSceneViewExtension() const
	{
		return SceneViewExtension;
	}

private:
	void BeginAsyncLoadAssets();
	void OnAssetsLoaded();

	void CreateOrUpdateRendererComponent();
	void DestroyRendererComponent();

private:
	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UGroundInteractionTargetComponent>> RegisteredTargets;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UPrimitiveComponent>> InteractionPrimitives;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DepthRT = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> FadeRT = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CubeMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> SphereMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CylinderMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> MaterialParameterCollection = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<AActor> RendererActor = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UGroundInteractionRendererComponent> RendererComponent = nullptr;

	bool bAssetsReady = false;

	TSharedPtr<FStreamableHandle> AssetLoadHandle;
	TSharedPtr<FGroundInteractionSceneViewExtension, ESPMode::ThreadSafe> SceneViewExtension;
};
