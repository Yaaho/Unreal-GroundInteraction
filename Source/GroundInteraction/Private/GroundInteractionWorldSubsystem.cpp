// Fill out your copyright notice in the Description page of Project Settings.

#include "GroundInteractionWorldSubsystem.h"
#include "GroundInteractionCommon.h"
#include "GroundInteractionSceneViewExtension.h"
#include "GroundInteractionSettings.h"
#include "GroundInteractionProxyMeshComponent.h"
#include "GroundInteractionRendererComponent.h"
#include "GroundInteractionTargetComponent.h"
#include "WaterInteractionRendererComponent.h"

#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/Level.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialParameterCollection.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "WaterBodyComponent.h"
#include "WaterBodyManager.h"
#include "WaterSubsystem.h"

#if ENABLE_DRAW_DEBUG
static TAutoConsoleVariable<int32> CVarGroundInteractionDebugDrawProxies(
	TEXT("GroundInteraction.DebugDrawProxies"),
	0,
	TEXT("Draw generated Ground Interaction proxy shapes.\n")
	TEXT("0: Disabled\n")
	TEXT("1: Enabled"),
	ECVF_Cheat);
#endif

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
	WaterSplatScheduleTimes.Empty();
	PendingExternalWaterSplats.Reset();

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

	DrawDebugInteractionProxies();

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

void UGroundInteractionWorldSubsystem::DrawDebugInteractionProxies() const
{
#if ENABLE_DRAW_DEBUG
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (CVarGroundInteractionDebugDrawProxies.GetValueOnGameThread() <= 0)
	{
		return;
	}

	static constexpr int32 NumSegments = 16;
	static constexpr float LifeTime = 0.0f;
	static constexpr float Thickness = 1.0f;
	static constexpr uint8 DepthPriority = SDPG_World;
	const FLinearColor DebugColor(0.0f, 1.0f, 0.25f, 1.0f);

	for (const TWeakObjectPtr<UPrimitiveComponent>& PrimitiveWeak : InteractionPrimitives)
	{
		const UGroundInteractionProxyMeshComponent* Proxy = Cast<UGroundInteractionProxyMeshComponent>(PrimitiveWeak.Get());
		if (!IsValid(Proxy) || !Proxy->IsRegistered())
		{
			continue;
		}

		const UStaticMesh* StaticMesh = Proxy->GetStaticMesh();
		if (!StaticMesh)
		{
			continue;
		}

		const FBoxSphereBounds LocalBounds = StaticMesh->GetBounds();
		const FTransform ComponentTransform = Proxy->GetComponentTransform();
		const FVector WorldCenter = ComponentTransform.TransformPosition(LocalBounds.Origin);
		const FVector WorldScale = ComponentTransform.GetScale3D().GetAbs();
		const FVector WorldExtent = LocalBounds.BoxExtent * WorldScale;
		const FQuat WorldRotation = ComponentTransform.GetRotation();

		if (StaticMesh == CubeMesh)
		{
			DrawDebugBox(
				World,
				WorldCenter,
				WorldExtent,
				WorldRotation,
				DebugColor.ToFColor(true),
				false,
				LifeTime,
				DepthPriority,
				Thickness);
		}
		else if (StaticMesh == SphereMesh)
		{
			const float Radius = LocalBounds.SphereRadius * WorldScale.GetMax();
			DrawDebugSphere(
				World,
				WorldCenter,
				Radius,
				NumSegments,
				DebugColor.ToFColor(true),
				false,
				LifeTime,
				DepthPriority,
				Thickness);
		}
		else if (StaticMesh == CapsuleMesh)
		{
			const float Radius = FMath::Max(WorldExtent.X, WorldExtent.Y);
			const float HalfHeight = FMath::Max(WorldExtent.Z, Radius);
			DrawDebugCapsule(
				World,
				WorldCenter,
				HalfHeight,
				Radius,
				WorldRotation,
				DebugColor.ToFColor(true),
				false,
				LifeTime,
				DepthPriority,
				Thickness);
		}
	}
#endif
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
	WaterSplatScheduleTimes.Remove(TWeakObjectPtr<UPrimitiveComponent>(Primitive));
}

void UGroundInteractionWorldSubsystem::AddWaterSplat(const FVector& PositionWS, const FVector2D& SizeWS)
{
	check(IsInGameThread());

	PendingExternalWaterSplats.Emplace(PositionWS, SizeWS);
}

TArray<FWaterSplatWorld> UGroundInteractionWorldSubsystem::GenerateWaterSplats(double CurrentTimeSeconds)
{
	check(IsInGameThread());

	TArray<FWaterSplatWorld> WaterSplats;

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings || !Settings->bEnableWaterInteraction || !bWaterAssetsReady || !IsValid(WaterRendererComponent) || !WaterRendererComponent->IsRegistered())
	{
		return WaterSplats;
	}

	const FIntPoint RenderTargetSize = Settings->GetWaterInteractionRenderTargetSize();
	if (RenderTargetSize.X <= 0 || RenderTargetSize.Y <= 0)
	{
		return WaterSplats;
	}

	const FVector InteractionExtentWS = GroundInteractionCommon::MakeSafeInteractionExtent(Settings->WaterInteractionWorldExtent);
	const FVector SnappedCenterWS = GroundInteractionCommon::SnapInteractionCenterWS(
		WaterRendererComponent->GetComponentLocation(),
		InteractionExtentWS,
		RenderTargetSize,
		Settings->WaterInteractionLocationSnapCellCount);
	const FBox InteractionBoundsWS = FBox::BuildAABB(SnappedCenterWS, InteractionExtentWS);

	WaterSplats.Reserve(InteractionPrimitives.Num() + PendingExternalWaterSplats.Num());

	if (!FMath::IsFinite(CurrentTimeSeconds))
	{
		return WaterSplats;
	}

	for (auto ScheduleIt = WaterSplatScheduleTimes.CreateIterator(); ScheduleIt; ++ScheduleIt)
	{
		if (!ScheduleIt.Key().IsValid())
		{
			ScheduleIt.RemoveCurrent();
		}
	}

	const FVector2D SplatIntervalRange = Settings->GetWaterInteractionSplatIntervalRange();
	const float MinVelocitySq = FMath::Square(FMath::Max(Settings->WaterInteractionMinVelocity, 0.0f));

	for (int32 PrimitiveIndex = InteractionPrimitives.Num() - 1; PrimitiveIndex >= 0; --PrimitiveIndex)
	{
		const TWeakObjectPtr<UPrimitiveComponent> PrimitiveWeak = InteractionPrimitives[PrimitiveIndex];
		UGroundInteractionProxyMeshComponent* Proxy = Cast<UGroundInteractionProxyMeshComponent>(PrimitiveWeak.Get());
		if (!IsValid(Proxy))
		{
			WaterSplatScheduleTimes.Remove(PrimitiveWeak);
			InteractionPrimitives.RemoveAtSwap(PrimitiveIndex);
			continue;
		}

		if (!Proxy->IsRegistered())
		{
			continue;
		}

		double& NextAttemptTimeSeconds = WaterSplatScheduleTimes.FindOrAdd(PrimitiveWeak, CurrentTimeSeconds);

		if (CurrentTimeSeconds < NextAttemptTimeSeconds)
		{
			continue;
		}

		// 성공 여부와 관계없이 다음 시도를 먼저 예약해 실패한 프리미티브의 매 업데이트 재검사를 막는다.
		NextAttemptTimeSeconds = CurrentTimeSeconds + static_cast<double>(FMath::FRandRange(SplatIntervalRange.X, SplatIntervalRange.Y));

		if (GetWaterSplatVelocityWS(Proxy).SizeSquared() < MinVelocitySq)
		{
			continue;
		}

		if (!InteractionBoundsWS.Intersect(Proxy->Bounds.GetBox()))
		{
			continue;
		}

		FVector WaterSurfacePositionWS = FVector::ZeroVector;
		FVector WaterSurfaceNormalWS = FVector::UpVector;
		if (!TryCalculateWaterContact(Proxy, Settings, WaterSurfacePositionWS, WaterSurfaceNormalWS))
		{
			continue;
		}

		FWaterSplatWorld Splat;
		if (WaterInteractionCommon::TryMakeWaterSplatWorld(Proxy, WaterSurfacePositionWS, WaterSurfaceNormalWS, Splat))
		{
			WaterSplats.Add(Splat);
		}
	}

	for (const TPair<FVector, FVector2D>& ExternalSplat : PendingExternalWaterSplats)
	{
		if (ExternalSplat.Key.ContainsNaN() || ExternalSplat.Value.ContainsNaN())
		{
			continue;
		}

		const FVector2d HalfSizeXY(FMath::Abs(ExternalSplat.Value.X) * 0.5, FMath::Abs(ExternalSplat.Value.Y) * 0.5);
		if (HalfSizeXY.X <= UE_DOUBLE_SMALL_NUMBER || HalfSizeXY.Y <= UE_DOUBLE_SMALL_NUMBER)
		{
			continue;
		}

		FWaterSplatWorld& Splat = WaterSplats.AddDefaulted_GetRef();
		Splat.AbsoluteCenterXY = FVector2d(ExternalSplat.Key.X, ExternalSplat.Key.Y);
		Splat.HalfSizeXY = HalfSizeXY;
	}

	PendingExternalWaterSplats.Reset();

	return WaterSplats;
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

bool UGroundInteractionWorldSubsystem::TryCalculateWaterContact(const UPrimitiveComponent* Primitive, const UGroundInteractionSettings* Settings, FVector& OutPositionWS, FVector& OutNormalWS) const
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
			OutNormalWS = SurfaceNormalWS;
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

	bWaterAssetsReady =
		Settings->bEnableWaterInteraction &&
		WaterHeightRT != nullptr;

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

	if (!RendererActor || !RendererRootComponent || !WaterHeightRT)
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
