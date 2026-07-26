// Fill out your copyright notice in the Description page of Project Settings.

#include "GroundInteractionWorldSubsystem.h"
#include "GroundInteractionCommon.h"
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
#include "GameFramework/Actor.h"
#include "Materials/MaterialParameterCollection.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterfaceArrayFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "WaterBodyComponent.h"
#include "WaterBodyManager.h"
#include "WaterSubsystem.h"

namespace GroundInteractionAssetPaths
{
	// 플러그인에서 사용하는 기본 에셋
	static const FSoftObjectPath DepthRT(TEXT("/GroundInteraction/RT/RT_GroundInteractionDepth.RT_GroundInteractionDepth"));
	
	static const FSoftObjectPath FadeRT(TEXT("/GroundInteraction/RT/RT_GroundInteractionFade.RT_GroundInteractionFade"));

	static const FSoftObjectPath WaterHeightRT(TEXT("/GroundInteraction/RT/RT_WaterinteractionHeight.RT_WaterinteractionHeight"));

	static const FSoftObjectPath WaterSplatSystem(TEXT("/GroundInteraction/Water/FX/NS_WaterSplatCapture.NS_WaterSplatCapture"));
	
	static const FSoftObjectPath CubeMesh(TEXT("/GroundInteraction/Mesh/Cube.Cube"));
	
	static const FSoftObjectPath SphereMesh(TEXT("/GroundInteraction/Mesh/Sphere.Sphere"));
	
	static const FSoftObjectPath CapsuleMesh(TEXT("/GroundInteraction/Mesh/Capsule.Capsule"));
}

namespace GroundInteractionNiagaraParameters
{
	static const FName SplatPositions(TEXT("User.SplatPositions"));
	static const FName SplatSizes(TEXT("User.SplatSizes"));
	static const FName SplatLifetime(TEXT("User.SplatLifetime"));
	static const FName SplatCount(TEXT("User.SplatCount"));
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
	PendingWaterSplatPositions.Reset();
	PendingWaterSplatSizes.Reset();

	bAssetsReady = false;
	bWaterAssetsReady = false;

	DepthRT = nullptr;
	FadeRT = nullptr;
	WaterHeightRT = nullptr;
	WaterSplatSystem = nullptr;
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
		if (IsValid(WaterSplatComponent) && WaterSplatComponent->IsRegistered())
		{
			WaterSplatComponent->SetVariableInt(GroundInteractionNiagaraParameters::SplatCount, 0);
		}

		return;
	}
	
	// 렌더 액터 위치를 플레이어 위치로 설정
	RendererActor->SetActorLocation(RawCameraLocationWS);

	UpdateWaterSplats();
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

void UGroundInteractionWorldSubsystem::AddWaterSplat(const FVector& PositionWS, const FVector2D& SizeWS)
{
	check(IsInGameThread());

	PendingWaterSplatPositions.Add(PositionWS);
	PendingWaterSplatSizes.Add(SizeWS);
}

void UGroundInteractionWorldSubsystem::UpdateWaterSplats()
{
	check(IsInGameThread());

	if (!IsValid(WaterSplatComponent) || !WaterSplatComponent->IsRegistered())
	{
		return;
	}

	WaterSplatComponent->SetVariableInt(GroundInteractionNiagaraParameters::SplatCount, 0);

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings || !Settings->bEnableWaterInteraction || !bWaterAssetsReady || !RendererActor)
	{
		return;
	}

	TArray<FVector> SplatPositions;
	TArray<FVector2D> SplatSizes;

	const int32 MaxSplatCount = InteractionPrimitives.Num() + PendingWaterSplatPositions.Num();
	SplatPositions.Reserve(MaxSplatCount);
	SplatSizes.Reserve(MaxSplatCount);

	const float SpawnProbability = FMath::Clamp(Settings->WaterInteractionSplatSpawnProbability, 0.0f, 1.0f);

	if (SpawnProbability > 0.0f)
	{
		const float MinVelocitySq = FMath::Square(FMath::Max(Settings->WaterInteractionMinVelocity, 0.0f));
		const FVector InteractionExtentWS = GroundInteractionCommon::MakeSafeInteractionExtent(Settings->WaterInteractionWorldExtent);
		const FIntPoint RenderTargetSize = Settings->GetWaterInteractionRenderTargetSize();
		const FVector SnappedCenterWS = GroundInteractionCommon::SnapInteractionCenterWS(
			RendererActor->GetActorLocation(),
			InteractionExtentWS,
			RenderTargetSize,
			Settings->WaterInteractionLocationSnapCellCount);
		const FBox InteractionBounds = FBox::BuildAABB(SnappedCenterWS, InteractionExtentWS);

		for (const TWeakObjectPtr<UPrimitiveComponent>& PrimitiveWeak : InteractionPrimitives)
		{
			UPrimitiveComponent* Primitive = PrimitiveWeak.Get();
			if (!IsValid(Primitive) || !Primitive->IsRegistered())
			{
				continue;
			}

			const FVector VelocityWS = GetWaterSplatVelocityWS(Primitive);
			if (VelocityWS.SizeSquared() < MinVelocitySq)
			{
				continue;
			}

			if (!InteractionBounds.Intersect(Primitive->Bounds.GetBox()))
			{
				continue;
			}

			if (SpawnProbability < 1.0f && FMath::FRand() >= SpawnProbability)
			{
				continue;
			}

			FVector SplatPositionWS = FVector::ZeroVector;
			FVector2D SplatSizeWS = FVector2D::ZeroVector;
			if (!TryCalculateWaterContact(Primitive, Settings, SplatPositionWS, SplatSizeWS))
			{
				continue;
			}

			SplatPositions.Add(SplatPositionWS);
			SplatSizes.Add(SplatSizeWS);
		}
	}

	check(PendingWaterSplatPositions.Num() == PendingWaterSplatSizes.Num());

	SplatPositions.Append(PendingWaterSplatPositions);
	SplatSizes.Append(PendingWaterSplatSizes);

	if (SplatPositions.IsEmpty())
	{
		return;
	}

	UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayPosition(WaterSplatComponent, GroundInteractionNiagaraParameters::SplatPositions, SplatPositions);

	UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayVector2D(WaterSplatComponent, GroundInteractionNiagaraParameters::SplatSizes, SplatSizes);

	const float SplatLifetimeSeconds = Settings->GetWaterInteractionUpdateIntervalSeconds() * 1.1f;
	WaterSplatComponent->SetVariableFloat(GroundInteractionNiagaraParameters::SplatLifetime, SplatLifetimeSeconds);
	WaterSplatComponent->SetVariableInt(GroundInteractionNiagaraParameters::SplatCount, SplatPositions.Num());

	PendingWaterSplatPositions.Reset();
	PendingWaterSplatSizes.Reset();
}

FVector UGroundInteractionWorldSubsystem::GetWaterSplatVelocityWS(const UPrimitiveComponent* Primitive) const
{
	if (!IsValid(Primitive))
	{
		return FVector::ZeroVector;
	}

	const auto IsUsableVelocity = [](const FVector& VelocityWS)
	{
		return !VelocityWS.ContainsNaN() && !VelocityWS.IsNearlyZero();
	};

	FVector VelocityWS = Primitive->GetComponentVelocity();
	if (IsUsableVelocity(VelocityWS))
	{
		return VelocityWS;
	}

	for (const USceneComponent* AttachParent = Primitive->GetAttachParent(); AttachParent; AttachParent = AttachParent->GetAttachParent())
	{
		VelocityWS = AttachParent->GetComponentVelocity();
		if (IsUsableVelocity(VelocityWS))
		{
			return VelocityWS;
		}
	}

	if (const AActor* Owner = Primitive->GetOwner())
	{
		VelocityWS = Owner->GetVelocity();
		if (IsUsableVelocity(VelocityWS))
		{
			return VelocityWS;
		}
	}

	return FVector::ZeroVector;
}

bool UGroundInteractionWorldSubsystem::TryCalculateWaterContact(const UPrimitiveComponent* Primitive, const UGroundInteractionSettings* Settings, FVector& OutPositionWS, FVector2D& OutSizeWS) const
{
	if (!IsValid(Primitive) || !Settings)
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FWaterBodyManager* WaterBodyManager = UWaterSubsystem::GetWaterBodyManager(World);
	if (!WaterBodyManager)
	{
		return false;
	}

	const FBoxSphereBounds PrimitiveBoundsWS = Primitive->Bounds;
	const double SurfaceDistanceTolerance = FMath::Max(static_cast<double>(Settings->WaterInteractionSurfaceDistanceTolerance), 0.0);
	const double QueryDistance = FMath::Max(PrimitiveBoundsWS.SphereRadius, 0.0) + SurfaceDistanceTolerance;
	const FBox PrimitiveBounds = PrimitiveBoundsWS.GetBox();
	bool bContactFound = false;

	WaterBodyManager->ForEachWaterBodyComponent(
		[&](UWaterBodyComponent* WaterBodyComponent)
		{
			if (!IsValid(WaterBodyComponent) || !WaterBodyComponent->IsRegistered())
			{
				return true;
			}

			if (!WaterBodyComponent->Bounds.GetBox().ExpandBy(QueryDistance).Intersect(PrimitiveBounds))
			{
				return true;
			}

			const EWaterBodyQueryFlags QueryFlags = EWaterBodyQueryFlags::ComputeLocation | EWaterBodyQueryFlags::ComputeNormal;
			const TValueOrError<FWaterBodyQueryResult, EWaterBodyQueryError> QueryResult =
				WaterBodyComponent->TryQueryWaterInfoClosestToWorldLocation(PrimitiveBoundsWS.Origin, QueryFlags);

			if (!QueryResult.HasValue())
			{
				return true;
			}

			const FWaterBodyQueryResult& Query = QueryResult.GetValue();
			if (Query.IsInExclusionVolume())
			{
				return true;
			}

			FVector SurfaceNormalWS = Query.GetWaterSurfaceNormal();
			if (!SurfaceNormalWS.Normalize())
			{
				SurfaceNormalWS = FVector::UpVector;
			}

			const double SignedDistanceToSurface = FVector::DotProduct(
				PrimitiveBoundsWS.Origin - Query.GetWaterSurfaceLocation(),
				SurfaceNormalWS);

			if (FMath::Abs(SignedDistanceToSurface) > QueryDistance)
			{
				return true;
			}

			OutPositionWS = Query.GetWaterSurfaceLocation();
			OutSizeWS = FVector2D(PrimitiveBoundsWS.BoxExtent.X * 2.0, PrimitiveBoundsWS.BoxExtent.Y * 2.0);
			bContactFound = true;

			return false;
		});

	return bContactFound;
}

void UGroundInteractionWorldSubsystem::BeginAsyncLoadAssets()
{
	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings)
	{
		return;
	}

	TArray<FSoftObjectPath> AssetsToLoad;
	AssetsToLoad.Reserve(8);

	AssetsToLoad.Add(GroundInteractionAssetPaths::DepthRT);
	AssetsToLoad.Add(GroundInteractionAssetPaths::FadeRT);
	AssetsToLoad.Add(GroundInteractionAssetPaths::CubeMesh);
	AssetsToLoad.Add(GroundInteractionAssetPaths::SphereMesh);
	AssetsToLoad.Add(GroundInteractionAssetPaths::CapsuleMesh);

	if (Settings->bEnableWaterInteraction)
	{
		AssetsToLoad.Add(GroundInteractionAssetPaths::WaterHeightRT);
		AssetsToLoad.Add(GroundInteractionAssetPaths::WaterSplatSystem);
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
	WaterSplatSystem = nullptr;

	if (Settings->bEnableWaterInteraction)
	{
		WaterHeightRT = Cast<UTextureRenderTarget2D>(GroundInteractionAssetPaths::WaterHeightRT.ResolveObject());
		if (!WaterHeightRT)
		{
			WaterHeightRT = Cast<UTextureRenderTarget2D>(GroundInteractionAssetPaths::WaterHeightRT.TryLoad());
		}

		WaterSplatSystem = Cast<UNiagaraSystem>(GroundInteractionAssetPaths::WaterSplatSystem.ResolveObject());
		if (!WaterSplatSystem)
		{
			WaterSplatSystem = Cast<UNiagaraSystem>(GroundInteractionAssetPaths::WaterSplatSystem.TryLoad());
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

	bWaterAssetsReady =
		Settings->bEnableWaterInteraction &&
		WaterHeightRT != nullptr &&
		WaterSplatSystem != nullptr;

	if (!bAssetsReady)
	{
		return;
	}

	CreateOrUpdateRendererActor();
	CreateOrUpdateGroundRendererComponent();
	CreateOrUpdateWaterSplatComponent();
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

void UGroundInteractionWorldSubsystem::CreateOrUpdateWaterSplatComponent()
{
	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings || !Settings->bEnableWaterInteraction || !bWaterAssetsReady || !RendererActor || !RendererRootComponent || !WaterSplatSystem)
	{
		return;
	}

	if (WaterSplatComponent)
	{
		return;
	}

	WaterSplatComponent = NewObject<UNiagaraComponent>(RendererActor, TEXT("WaterSplatComponent"), RF_Transient);
	if (!WaterSplatComponent)
	{
		return;
	}

	WaterSplatComponent->SetAutoActivate(false);
	WaterSplatComponent->SetAsset(WaterSplatSystem);
	WaterSplatComponent->SetupAttachment(RendererRootComponent);
	RendererActor->AddInstanceComponent(WaterSplatComponent);

	WaterSplatComponent->SetAutoDestroy(false);
	WaterSplatComponent->SetOnlyOwnerSee(true);
	WaterSplatComponent->SetCastShadow(false);
	WaterSplatComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WaterSplatComponent->SetGenerateOverlapEvents(false);
	WaterSplatComponent->RegisterComponent();

	WaterSplatComponent->SetVariableInt(GroundInteractionNiagaraParameters::SplatCount, 0);
	WaterSplatComponent->Activate(false);
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

	if (!RendererActor || !RendererRootComponent || !IsValid(WaterSplatComponent) || !WaterSplatComponent->IsRegistered())
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

	WaterRendererComponent->InitializeRenderer(WaterSplatComponent, WaterHeightRT, MaterialParameterCollection);
}

void UGroundInteractionWorldSubsystem::DestroyRendererComponent()
{
	if (WaterRendererComponent)
	{
		WaterRendererComponent->DestroyComponent();
		WaterRendererComponent = nullptr;
	}

	if (WaterSplatComponent)
	{
		WaterSplatComponent->DestroyComponent();
		WaterSplatComponent = nullptr;
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
