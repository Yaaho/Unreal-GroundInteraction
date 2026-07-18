// Fill out your copyright notice in the Description page of Project Settings.

#include "GroundInteractionWorldSubsystem.h"
#include "GroundInteractionSceneViewExtension.h"
#include "GroundInteractionSettings.h"
#include "GroundInteractionRendererComponent.h"
#include "GroundInteractionTargetComponent.h"
#include "WaterInteractionRendererComponent.h"

#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Level.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialParameterCollection.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"

namespace GroundInteractionAssetPaths
{
	// 플러그인에서 사용하는 기본 에셋
	static const FSoftObjectPath DepthRT(TEXT("/GroundInteraction/RT/RT_GroundInteractionDepth.RT_GroundInteractionDepth"));
	
	static const FSoftObjectPath FadeRT(TEXT("/GroundInteraction/RT/RT_GroundInteractionFade.RT_GroundInteractionFade"));

	static const FSoftObjectPath WaterHeightRT(TEXT("/GroundInteraction/RT/RT_WaterinteractionHeight.RT_WaterinteractionHeight"));
	
	static const FSoftObjectPath CubeMesh(TEXT("/GroundInteraction/Mesh/Cube.Cube"));
	
	static const FSoftObjectPath SphereMesh(TEXT("/GroundInteraction/Mesh/Sphere.Sphere"));
	
	static const FSoftObjectPath CapsuleMesh(TEXT("/GroundInteraction/Mesh/Capsule.Capsule"));
}

void UGroundInteractionWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	SceneViewExtension = FSceneViewExtensions::NewExtension<FGroundInteractionSceneViewExtension>(this, GetWorld());

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
	bWaterAssetsReady = false;

	DepthRT = nullptr;
	FadeRT = nullptr;
	WaterHeightRT = nullptr;
	CubeMesh = nullptr;
	SphereMesh = nullptr;
	CapsuleMesh = nullptr;
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


void UGroundInteractionWorldSubsystem::Tick(float DeltaTime)
{
	(void)DeltaTime;

	if (!bAssetsReady || !RendererActor || !RendererComponent)
	{
		return;
	}

	FVector RawCameraLocationWS = FVector::ZeroVector;

	if (!GetPlayerCameraLocation(RawCameraLocationWS))
	{
		return;
	}
	
	// 렌더 액터 위치를 플레이어 위치로 설정
	RendererActor->SetActorLocation(RawCameraLocationWS);
}

TStatId UGroundInteractionWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGroundInteractionWorldSubsystem, STATGROUP_Tickables);
}

bool UGroundInteractionWorldSubsystem::IsTickable() const
{
	const UWorld* World = GetWorld();

	return World && !World->IsNetMode(NM_DedicatedServer) && bAssetsReady && RendererActor && RendererComponent;
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

void UGroundInteractionWorldSubsystem::BeginAsyncLoadAssets()
{
	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings)
	{
		return;
	}

	TArray<FSoftObjectPath> AssetsToLoad;
	AssetsToLoad.Reserve(7);

	AssetsToLoad.Add(GroundInteractionAssetPaths::DepthRT);
	AssetsToLoad.Add(GroundInteractionAssetPaths::FadeRT);
	AssetsToLoad.Add(GroundInteractionAssetPaths::CubeMesh);
	AssetsToLoad.Add(GroundInteractionAssetPaths::SphereMesh);
	AssetsToLoad.Add(GroundInteractionAssetPaths::CapsuleMesh);

	if (Settings->bEnableWaterInteraction)
	{
		AssetsToLoad.Add(GroundInteractionAssetPaths::WaterHeightRT);
	}

	if (!Settings->MaterialParameterCollection.IsNull())
	{
		AssetsToLoad.Add(Settings->MaterialParameterCollection.ToSoftObjectPath());
	}

	AssetLoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(AssetsToLoad, FStreamableDelegate::CreateUObject(this, &UGroundInteractionWorldSubsystem::OnAssetsLoaded));
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

	WaterHeightRT = nullptr;
	if (Settings->bEnableWaterInteraction)
	{
		WaterHeightRT = Cast<UTextureRenderTarget2D>(GroundInteractionAssetPaths::WaterHeightRT.ResolveObject());
		if (!WaterHeightRT)
		{
			WaterHeightRT = Cast<UTextureRenderTarget2D>(GroundInteractionAssetPaths::WaterHeightRT.TryLoad());
		}
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

	CapsuleMesh = Cast<UStaticMesh>(GroundInteractionAssetPaths::CapsuleMesh.ResolveObject());
	if (!CapsuleMesh)
	{
		CapsuleMesh = Cast<UStaticMesh>(GroundInteractionAssetPaths::CapsuleMesh.TryLoad());
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
		CapsuleMesh != nullptr &&
		MaterialParameterCollection != nullptr;

	bWaterAssetsReady = Settings->bEnableWaterInteraction && WaterHeightRT != nullptr;

	if (!bAssetsReady)
	{
		return;
	}

	CreateOrUpdateRendererActor();
	CreateOrUpdateGroundRendererComponent();
	CreateOrUpdateWaterRendererComponent();

	for (const TWeakObjectPtr<UGroundInteractionTargetComponent>& TargetWeak : RegisteredTargets)
	{
		if (UGroundInteractionTargetComponent* Target = TargetWeak.Get())
		{
			Target->RebuildCaptureTargetInfo();
		}
	}
}

void UGroundInteractionWorldSubsystem::CreateOrUpdateRendererActor()
{
	UWorld* World = GetWorld();
	if (!World || !bAssetsReady)
	{
		return;
	}

	if (!RendererActor)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Name = MakeUniqueObjectName(World->PersistentLevel, AActor::StaticClass(), TEXT("GroundInteractionRendererActor"));

		SpawnParams.ObjectFlags |= RF_Transient;

		RendererActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);

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

	if (!RendererRootComponent)
	{
		RendererRootComponent = NewObject<USceneComponent>(RendererActor, TEXT("InteractionRendererRoot"), RF_Transient);

		RendererActor->SetRootComponent(RendererRootComponent);
		RendererActor->AddInstanceComponent(RendererRootComponent);
	}

	if (!RendererRootComponent->IsRegistered())
	{
		RendererRootComponent->RegisterComponent();
	}
}

void UGroundInteractionWorldSubsystem::CreateOrUpdateGroundRendererComponent()
{
	if (!RendererActor || !RendererRootComponent || !bAssetsReady)
	{
		return;
	}

	if (!RendererComponent)
	{
		RendererComponent = NewObject<UGroundInteractionRendererComponent>(RendererActor, TEXT("GroundInteractionRendererComponent"), RF_Transient);

		RendererComponent->SetupAttachment(RendererRootComponent);
		
		RendererActor->AddInstanceComponent(RendererComponent);
	}

	if (!RendererComponent->IsRegistered())
	{
		RendererComponent->RegisterComponent();
	}
	
	RendererComponent->InitializeRenderer(this, DepthRT, FadeRT, MaterialParameterCollection);
}

void UGroundInteractionWorldSubsystem::CreateOrUpdateWaterRendererComponent()
{
	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings || !Settings->bEnableWaterInteraction || !bWaterAssetsReady)
	{
		if (WaterRendererComponent)
		{
			WaterRendererComponent->DestroyComponent();
			WaterRendererComponent = nullptr;
		}

		return;
	}

	if (!RendererActor || !RendererRootComponent)
	{
		return;
	}

	if (!WaterRendererComponent)
	{
		WaterRendererComponent = NewObject<UWaterInteractionRendererComponent>(RendererActor, TEXT("WaterInteractionRendererComponent"), RF_Transient);

		WaterRendererComponent->SetupAttachment(RendererRootComponent);

		RendererActor->AddInstanceComponent(WaterRendererComponent);
	}

	if (!WaterRendererComponent->IsRegistered())
	{
		WaterRendererComponent->RegisterComponent();
	}

	WaterRendererComponent->InitializeRenderer(this, WaterHeightRT, MaterialParameterCollection);
}

void UGroundInteractionWorldSubsystem::DestroyRendererComponent()
{
	if (WaterRendererComponent)
	{
		WaterRendererComponent->DestroyComponent();
		WaterRendererComponent = nullptr;
	}

	if (RendererComponent)
	{
		RendererComponent->DestroyComponent();
		RendererComponent = nullptr;
	}

	if (RendererRootComponent)
	{
		RendererRootComponent->DestroyComponent();
		RendererRootComponent = nullptr;
	}

	if (RendererActor)
	{
		RendererActor->Destroy();
		RendererActor = nullptr;
	}
}

bool UGroundInteractionWorldSubsystem::GetPlayerCameraLocation(FVector& OutCameraLocationWS) const
{
	const UWorld* World = GetWorld();

	if (!World || World->IsNetMode(NM_DedicatedServer))
	{
		return false;
	}

	APlayerController* PlayerController = World->GetFirstPlayerController();

	if (!PlayerController || !PlayerController->PlayerCameraManager)
	{
		return false;
	}

	OutCameraLocationWS = PlayerController->PlayerCameraManager->GetCameraLocation();

	return true;
}
