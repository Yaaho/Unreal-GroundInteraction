// Fill out your copyright notice in the Description page of Project Settings.

#include "GroundInteractionWorldSubsystem.h"
#include "GroundInteractionSceneViewExtension.h"
#include "GroundInteractionSettings.h"
#include "GroundInteractionRendererComponent.h"
#include "GroundInteractionTargetComponent.h"

#include "Components/PrimitiveComponent.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialParameterCollection.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"

namespace GroundInteractionAssetPaths
{
	// 플러그인에서 사용하는 기본 에셋들
	static const FSoftObjectPath DepthRT(
		TEXT("/GroundInteraction/RT/RT_GroundInteractionDepth.RT_GroundInteractionDepth"));
	
	static const FSoftObjectPath FadeRT(
		TEXT("/GroundInteraction/RT/RT_GroundInteractionFade.RT_GroundInteractionFade"));
	
	static const FSoftObjectPath CubeMesh(
		TEXT("/GroundInteraction/Mesh/Cube.Cube"));
	
	static const FSoftObjectPath SphereMesh(
		TEXT("/GroundInteraction/Mesh/Sphere.Sphere"));
	
	// 엔진 기본 캡슐 매시는 없어서 실린더를 대신 사용한다.
	static const FSoftObjectPath CylinderMesh(
		TEXT("/GroundInteraction/Mesh/Capsule.Capsule"));
}

void UGroundInteractionWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	SceneViewExtension =
		FSceneViewExtensions::NewExtension<FGroundInteractionSceneViewExtension>(this, GetWorld());

	BeginAsyncLoadAssets();
}

void UGroundInteractionWorldSubsystem::Deinitialize()
{
	DestroyRendererComponent();

	for (const TWeakObjectPtr<UGroundInteractionTargetComponent>& TargetWeak : RegisteredTargets)
	{
		if (UGroundInteractionTargetComponent* Target = TargetWeak.Get())
		{
			Target->DestroyCaptureTargetInfo();
		}
	}

	RegisteredTargets.Empty();
	InteractionPrimitives.Empty();

	bAssetsReady = false;

	DepthRT = nullptr;
	FadeRT = nullptr;
	CubeMesh = nullptr;
	SphereMesh = nullptr;
	CylinderMesh = nullptr;
	MaterialParameterCollection = nullptr;

	if (AssetLoadHandle.IsValid())
	{
		AssetLoadHandle->CancelHandle();
		AssetLoadHandle.Reset();
	}

	if (SceneViewExtension.IsValid())
	{
		SceneViewExtension.Reset();
	}

	Super::Deinitialize();
}

void UGroundInteractionWorldSubsystem::RegisterTarget(UGroundInteractionTargetComponent* Target)
{
	if (!IsValid(Target))
	{
		return;
	}

	RegisteredTargets.AddUnique(Target);

	if (bAssetsReady)
	{
		Target->RebuildCaptureTargetInfo();
	}
}

void UGroundInteractionWorldSubsystem::UnregisterTarget(UGroundInteractionTargetComponent* Target)
{
	if (!Target)
	{
		return;
	}

	RegisteredTargets.Remove(Target);
}

void UGroundInteractionWorldSubsystem::RegisterInteractionPrimitive(UPrimitiveComponent* Primitive)
{
	if (!IsValid(Primitive))
	{
		return;
	}

	InteractionPrimitives.AddUnique(Primitive);
}

void UGroundInteractionWorldSubsystem::UnregisterInteractionPrimitive(UPrimitiveComponent* Primitive)
{
	if (!Primitive)
	{
		return;
	}

	InteractionPrimitives.Remove(Primitive);
}

void UGroundInteractionWorldSubsystem::PrepareRendererFrame_GameThread(
	FSceneViewFamily& InViewFamily)
{
	check(IsInGameThread());

	if (!bAssetsReady ||
		!RendererComponent)
	{
		return;
	}

	RendererComponent->PrepareFrame_GameThread(
		this,
		InViewFamily);
}

void UGroundInteractionWorldSubsystem::BeginAsyncLoadAssets()
{
	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings)
	{
		return;
	}

	TArray<FSoftObjectPath> AssetsToLoad;
	AssetsToLoad.Reserve(6);

	AssetsToLoad.Add(GroundInteractionAssetPaths::DepthRT);
	AssetsToLoad.Add(GroundInteractionAssetPaths::FadeRT);
	AssetsToLoad.Add(GroundInteractionAssetPaths::CubeMesh);
	AssetsToLoad.Add(GroundInteractionAssetPaths::SphereMesh);
	AssetsToLoad.Add(GroundInteractionAssetPaths::CylinderMesh);

	if (!Settings->MaterialParameterCollection.IsNull())
	{
		AssetsToLoad.Add(Settings->MaterialParameterCollection.ToSoftObjectPath());
	}

	AssetLoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
		AssetsToLoad,
		FStreamableDelegate::CreateUObject(this, &UGroundInteractionWorldSubsystem::OnAssetsLoaded));
}

void UGroundInteractionWorldSubsystem::OnAssetsLoaded()
{
	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings)
	{
		return;
	}

	DepthRT = Cast<UTextureRenderTarget2D>(GroundInteractionAssetPaths::DepthRT.ResolveObject());
	if (!DepthRT)
	{
		DepthRT = Cast<UTextureRenderTarget2D>(GroundInteractionAssetPaths::DepthRT.TryLoad());
	}

	FadeRT = Cast<UTextureRenderTarget2D>(GroundInteractionAssetPaths::FadeRT.ResolveObject());
	if (!FadeRT)
	{
		FadeRT = Cast<UTextureRenderTarget2D>(GroundInteractionAssetPaths::FadeRT.TryLoad());
	}

	CubeMesh = Cast<UStaticMesh>(GroundInteractionAssetPaths::CubeMesh.ResolveObject());
	if (!CubeMesh)
	{
		CubeMesh = Cast<UStaticMesh>(GroundInteractionAssetPaths::CubeMesh.TryLoad());
	}

	SphereMesh = Cast<UStaticMesh>(GroundInteractionAssetPaths::SphereMesh.ResolveObject());
	if (!SphereMesh)
	{
		SphereMesh = Cast<UStaticMesh>(GroundInteractionAssetPaths::SphereMesh.TryLoad());
	}

	CylinderMesh = Cast<UStaticMesh>(GroundInteractionAssetPaths::CylinderMesh.ResolveObject());
	if (!CylinderMesh)
	{
		CylinderMesh = Cast<UStaticMesh>(GroundInteractionAssetPaths::CylinderMesh.TryLoad());
	}

	if (!Settings->MaterialParameterCollection.IsNull())
	{
		MaterialParameterCollection = Settings->MaterialParameterCollection.Get();

		if (!MaterialParameterCollection)
		{
			MaterialParameterCollection = Settings->MaterialParameterCollection.LoadSynchronous();
		}
	}

	bAssetsReady =
		DepthRT != nullptr &&
		FadeRT != nullptr &&
		CubeMesh != nullptr &&
		SphereMesh != nullptr &&
		CylinderMesh != nullptr &&
		MaterialParameterCollection != nullptr;

	if (!bAssetsReady)
	{
		return;
	}

	CreateOrUpdateRendererComponent();

	for (const TWeakObjectPtr<UGroundInteractionTargetComponent>& TargetWeak : RegisteredTargets)
	{
		if (UGroundInteractionTargetComponent* Target = TargetWeak.Get())
		{
			Target->RebuildCaptureTargetInfo();
		}
	}
}

void UGroundInteractionWorldSubsystem::CreateOrUpdateRendererComponent()
{
	UWorld* World = GetWorld();
	if (!World || !bAssetsReady)
	{
		return;
	}

	if (!RendererActor)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Name = MakeUniqueObjectName(
			World->PersistentLevel,
			AActor::StaticClass(),
			TEXT("GroundInteractionRendererActor"));

		SpawnParams.ObjectFlags |= RF_Transient;

		RendererActor = World->SpawnActor<AActor>(
			AActor::StaticClass(),
			FTransform::Identity,
			SpawnParams);

		if (RendererActor)
		{
			RendererActor->SetActorHiddenInGame(true);
			RendererActor->SetActorEnableCollision(false);
		}
	}

	if (!RendererActor)
	{
		return;
	}

	if (!RendererComponent)
	{
		RendererComponent = NewObject<UGroundInteractionRendererComponent>(
			RendererActor,
			TEXT("GroundInteractionRendererComponent"),
			RF_Transient);

		RendererActor->AddInstanceComponent(RendererComponent);
	}

	RendererComponent->InitializeRenderer(
		this,
		DepthRT,
		FadeRT,
		MaterialParameterCollection);

	if (!RendererComponent->IsRegistered())
	{
		RendererComponent->RegisterComponent();
	}
}

void UGroundInteractionWorldSubsystem::DestroyRendererComponent()
{
	if (RendererComponent)
	{
		RendererComponent->DestroyComponent();
		RendererComponent = nullptr;
	}

	if (RendererActor)
	{
		RendererActor->Destroy();
		RendererActor = nullptr;
	}
}
