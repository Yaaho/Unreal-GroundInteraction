// Fill out your copyright notice in the Description page of Project Settings.


#include "GroundInteractionWorldSubsystem.h"
#include "GroundInteractionSceneViewExtension.h"
#include "GroundInteractionSettings.h"
#include "GroundInteractionTargetComponent.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialParameterCollection.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"

namespace GroundInteractionAssetPaths
{
	// 플러그인에서 사용하는 기본 에셋들
	static const FSoftObjectPath DepthRT(
		TEXT("/GroundInteraction/RT/RT_GroundInteractionDepth.RT_GroundInteractionDepth"));

	static const FSoftObjectPath CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));

	static const FSoftObjectPath SphereMesh(
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));

	// 엔진 기본 캡슐 매시는 없어서 실린더를 대신 사용한다.
	static const FSoftObjectPath CylinderMesh(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
}

namespace GroundInteraction
{
	static FVector2D SnapXYToGrid(const FVector2D& XY, const FVector2D& UnitsPerPixel)
	{
		return FVector2D(
			(FMath::FloorToDouble(XY.X / UnitsPerPixel.X) + 0.5) * UnitsPerPixel.X,
			(FMath::FloorToDouble(XY.Y / UnitsPerPixel.Y) + 0.5) * UnitsPerPixel.Y);
	}
}

void UGroundInteractionWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	
	// SVE 생성
	SceneViewExtension = FSceneViewExtensions::NewExtension<FGroundInteractionSceneViewExtension>(this, GetWorld());
	
	// 플러그인 에셋들 비동기 로드 시작
	BeginAsyncLoadAssets();
}

void UGroundInteractionWorldSubsystem::Deinitialize()
{
	for (TPair<TObjectPtr<UGroundInteractionTargetComponent>, FGroundInteractionTargetInfo>& Pair : GroundInteractionTargets)
	{
		for (UStaticMeshComponent* ProxyComp : Pair.Value.InteractionBodies)
		{
			if (IsValid(ProxyComp))
			{
				ProxyComp->DestroyComponent();
			}
		}
	}

	GroundInteractionTargets.Empty();

	bAssetsReady = false;
	bHasPrevCaptureInfo = false;
	PrevCaptureInfo = {};

	DepthRT = nullptr;
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
	
	// 프록시 TMap 에 Target 을 add 함
	GroundInteractionTargets.FindOrAdd(Target);
	
	// 아직 에셋이 로드되기 전이면 엔트리를 빌드하지 않음
	if (!bAssetsReady)
	{
		return;
	}

	RebuildCaptureTargetInfo(Target);
}

void UGroundInteractionWorldSubsystem::UnregisterTarget(UGroundInteractionTargetComponent* Target)
{
	if (!IsValid(Target))
	{
		return;
	}

	DestroyCaptureTargetInfo(Target);
}

bool UGroundInteractionWorldSubsystem::BuildRenderFrameParams(
	const FSceneView* PrimaryView,
	const FIntPoint& RTSize,
	FGroundInteractionRenderFrameParams& OutParams) const
{
	OutParams = {};

	if (!PrimaryView || RTSize.X <= 0 || RTSize.Y <= 0)
	{
		return false;
	}

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings)
	{
		return false;
	}

	const FVector CaptureExtent(
		FMath::Max(Settings->CaptureWorldExtent.X, 1.0f),
		FMath::Max(Settings->CaptureWorldExtent.Y, 1.0f),
		FMath::Max(Settings->CaptureWorldExtent.Z, 1.0f));

	// XY 스냅 단위: CaptureWorldExtent는 half extent이므로 실제 크기는 * 2.
	const FVector2D UnitsPerPixel(
		CaptureExtent.X * 2.0f / float(RTSize.X),
		CaptureExtent.Y * 2.0f / float(RTSize.Y));

	const float CaptureSnapStep = FMath::Max3(
		UnitsPerPixel.X,
		UnitsPerPixel.Y,
		1.0);

	const FVector2D SnappedXY = GroundInteraction::SnapXYToGrid(
		FVector2D(PrimaryView->ViewLocation.X, PrimaryView->ViewLocation.Y),
		UnitsPerPixel);

	const float SnappedZ =
		(FMath::FloorToDouble(PrimaryView->ViewLocation.Z / CaptureSnapStep) + 0.5) * CaptureSnapStep;

	OutParams.bValid = true;
	OutParams.RenderTargetSize = RTSize;

	OutParams.CaptureInfo.bValid = true;
	OutParams.CaptureInfo.CenterWS = FVector(
		SnappedXY.X,
		SnappedXY.Y,
		SnappedZ);

	OutParams.CaptureInfo.ExtentWS = CaptureExtent;

	const FVector CurrentCaptureOriginWS = OutParams.CaptureInfo.GetCaptureOriginWS();

	OutParams.bHasPrevCaptureInfo = bHasPrevCaptureInfo;
	OutParams.PrevCaptureOriginWS =
		bHasPrevCaptureInfo
			? PrevCaptureInfo.GetCaptureOriginWS()
			: CurrentCaptureOriginWS;

	if (bHasPrevCaptureInfo)
	{
		OutParams.ScrollPixels.X = FMath::RoundToInt(
			(OutParams.CaptureInfo.CenterWS.X - PrevCaptureInfo.CenterWS.X) / UnitsPerPixel.X);

		OutParams.ScrollPixels.Y = -FMath::RoundToInt(
			(OutParams.CaptureInfo.CenterWS.Y - PrevCaptureInfo.CenterWS.Y) / UnitsPerPixel.Y);
	}
	
	// 스냅된 위치가 바뀌었을 때만 Move 패스 실행
	OutParams.bShouldMove = !CurrentCaptureOriginWS.Equals(OutParams.PrevCaptureOriginWS, KINDA_SMALL_NUMBER);

	OutParams.DeltaTimeSeconds =
		PrimaryView->Family
			? PrimaryView->Family->Time.GetDeltaWorldTimeSeconds()
			: (GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.0f);

	return true;
}

void UGroundInteractionWorldSubsystem::CommitRenderFrame(const FGroundInteractionRenderFrameParams& FrameParams)
{
	if (!FrameParams.bValid || !FrameParams.CaptureInfo.bValid)
	{
		return;
	}

	PrevCaptureInfo = FrameParams.CaptureInfo;
	bHasPrevCaptureInfo = true;
}


void UGroundInteractionWorldSubsystem::BuildShowOnlyPrimitives(
	const FGroundInteractionCaptureInfo& CaptureInfo,
	TSet<FPrimitiveComponentId>& OutPrimitiveIds) const
{
	OutPrimitiveIds.Reset();

	if (!bAssetsReady || !CaptureInfo.bValid)
	{
		return;
	}

	const FBox CaptureBox(
		CaptureInfo.CenterWS - CaptureInfo.ExtentWS,
		CaptureInfo.CenterWS + CaptureInfo.ExtentWS);

	for (const TPair<TObjectPtr<UGroundInteractionTargetComponent>, FGroundInteractionTargetInfo>& Pair : GroundInteractionTargets)
	{
		for (UStaticMeshComponent* ProxyComp : Pair.Value.InteractionBodies)
		{
			if (!IsValid(ProxyComp) || !ProxyComp->IsRegistered())
			{
				continue;
			}

			const FBox ProxyBox = ProxyComp->Bounds.GetBox();
			if (ProxyBox.Intersect(CaptureBox))
			{
				OutPrimitiveIds.Add(ProxyComp->GetPrimitiveSceneId());
			}
		}
	}
}

void UGroundInteractionWorldSubsystem::UpdateMPC(const FGroundInteractionCaptureInfo& Region) const
{
	if (!GetWorld() || !MaterialParameterCollection)
	{
		return;
	}

	const FVector CaptureOriginWS = Region.GetCaptureOriginWS();
	
	// X, Y 는 중심 위치
	// Z 는 캡쳐의 Bottom Oigin 이다.
	UKismetMaterialLibrary::SetVectorParameterValue(
		GetWorld(),
		MaterialParameterCollection,
		FName(TEXT("GI_CaptureOriginWS")),
		FLinearColor(CaptureOriginWS.X, CaptureOriginWS.Y, CaptureOriginWS.Z, 0.0f));

	UKismetMaterialLibrary::SetVectorParameterValue(
		GetWorld(),
		MaterialParameterCollection,
		FName(TEXT("GI_CaptureExtentWS")),
		FLinearColor(Region.ExtentWS.X, Region.ExtentWS.Y, Region.ExtentWS.Z, 0.0f));

	UKismetMaterialLibrary::SetScalarParameterValue(
		GetWorld(),
		MaterialParameterCollection,
		FName(TEXT("GI_CaptureValid")),
		Region.bValid ? 1.0f : 0.0f);
}

FTextureRenderTargetResource* UGroundInteractionWorldSubsystem::GetDepthRTResource_GameThread() const
{
	if (!DepthRT)
	{
		return nullptr;
	}

	return DepthRT->GameThread_GetRenderTargetResource();
}


void UGroundInteractionWorldSubsystem::BeginAsyncLoadAssets()
{
	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings)
	{
		return;
	}

	TArray<FSoftObjectPath> AssetsToLoad;
	AssetsToLoad.Reserve(5);

	AssetsToLoad.Add(GroundInteractionAssetPaths::DepthRT);
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
		(DepthRT != nullptr) &&
		(CubeMesh != nullptr) &&
		(SphereMesh != nullptr) &&
		(CylinderMesh != nullptr) &&
		(MaterialParameterCollection != nullptr);

	if (!bAssetsReady)
	{
		return;
	}
	
	if (DepthRT)
	{
		const int32 RTSizeX = FMath::Max(DepthRT->SizeX, 1);
		const int32 RTSizeY = FMath::Max(DepthRT->SizeY, 1);

		DepthRT->RenderTargetFormat = RTF_RG16f;
		DepthRT->ClearColor = FLinearColor(0.0f, -32768.0f, 0.0f, 0.0f);
		DepthRT->SRGB = false;
		DepthRT->bAutoGenerateMips = false;
		DepthRT->AddressX = TA_Clamp;
		DepthRT->AddressY = TA_Clamp;
		DepthRT->Filter = TF_Nearest;
		DepthRT->InitAutoFormat(RTSizeX, RTSizeY);
		DepthRT->UpdateResourceImmediate(true);
	}
	
	// 에셋 로드 완료 시 엔트리 전부 리빌드
	for (TPair<TObjectPtr<UGroundInteractionTargetComponent>, FGroundInteractionTargetInfo>& Pair : GroundInteractionTargets)
	{
		if (IsValid(Pair.Key))
		{
			RebuildCaptureTargetInfo(Pair.Key);
		}
	}
}


void UGroundInteractionWorldSubsystem::RebuildCaptureTargetInfo(UGroundInteractionTargetComponent* Target)
{
	if (!IsValid(Target) || !bAssetsReady)
	{
		return;
	}

	FGroundInteractionTargetInfo& TargetInfo = GroundInteractionTargets.FindOrAdd(Target);
	
	// 엔트리 리빌드
	for (UStaticMeshComponent* ProxyComp : TargetInfo.InteractionBodies)
	{
		if (IsValid(ProxyComp))
		{
			ProxyComp->DestroyComponent();
		}
	}
	TargetInfo.InteractionBodies.Reset();

	USkeletalMeshComponent* SkeletalMeshComp = Target->ResolveSourceSkeletalMesh();
	TargetInfo.Skeletal = SkeletalMeshComp;

	if (!IsValid(SkeletalMeshComp))
	{
		return;
	}

	BuildProxies(TargetInfo, SkeletalMeshComp);
}

void UGroundInteractionWorldSubsystem::DestroyCaptureTargetInfo(UGroundInteractionTargetComponent* Target)
{
	FGroundInteractionTargetInfo* ExistingTargetInfo = GroundInteractionTargets.Find(Target);
	if (!ExistingTargetInfo)
	{
		return;
	}

	for (UStaticMeshComponent* ProxyComp : ExistingTargetInfo->InteractionBodies)
	{
		if (IsValid(ProxyComp))
		{
			ProxyComp->DestroyComponent();
		}
	}

	GroundInteractionTargets.Remove(Target);
}

void UGroundInteractionWorldSubsystem::BuildProxies(
	FGroundInteractionTargetInfo& TargetInfo,
	USkeletalMeshComponent* SkeletalMeshComp)
{
	if (!IsValid(SkeletalMeshComp) || !bAssetsReady)
	{
		return;
	}

	UPhysicsAsset* PhysicsAsset = SkeletalMeshComp->GetPhysicsAsset();
	if (!PhysicsAsset)
	{
		return;
	}

	AActor* OwnerActor = SkeletalMeshComp->GetOwner();
	if (!OwnerActor)
	{
		return;
	}

	for (USkeletalBodySetup* BodySetup : PhysicsAsset->SkeletalBodySetups)
	{
		if (!BodySetup)
		{
			continue;
		}

		const FName BoneName = BodySetup->BoneName;
		const FKAggregateGeom& AggGeom = BodySetup->AggGeom;

		for (const FKBoxElem& BoxElem : AggGeom.BoxElems)
		{
			const FTransform RelativeTransform(
				BoxElem.Rotation.Quaternion(),
				BoxElem.Center,
				FVector::OneVector);

			const FVector DesiredWorldSize(BoxElem.X, BoxElem.Y, BoxElem.Z);

			if (UStaticMeshComponent* Proxy = CreateProxy(
				OwnerActor,
				SkeletalMeshComp,
				BoneName,
				CubeMesh,
				RelativeTransform,
				DesiredWorldSize))
			{
				TargetInfo.InteractionBodies.Add(Proxy);
			}
		}

		for (const FKSphereElem& SphereElem : AggGeom.SphereElems)
		{
			const FTransform RelativeTransform(
				FQuat::Identity,
				SphereElem.Center,
				FVector::OneVector);

			const float Diameter = SphereElem.Radius * 2.0f;
			const FVector DesiredWorldSize(Diameter, Diameter, Diameter);

			if (UStaticMeshComponent* Proxy = CreateProxy(
				OwnerActor,
				SkeletalMeshComp,
				BoneName,
				SphereMesh,
				RelativeTransform,
				DesiredWorldSize))
			{
				TargetInfo.InteractionBodies.Add(Proxy);
			}
		}

		for (const FKSphylElem& SphylElem : AggGeom.SphylElems)
		{
			const FTransform RelativeTransform(
				SphylElem.Rotation.Quaternion(),
				SphylElem.Center,
				FVector::OneVector);

			const float Diameter = SphylElem.Radius * 2.0f;
			const float FullHeight = SphylElem.Length + Diameter;
			const FVector DesiredWorldSize(Diameter, Diameter, FullHeight);

			if (UStaticMeshComponent* Proxy = CreateProxy(
				OwnerActor,
				SkeletalMeshComp,
				BoneName,
				CylinderMesh,
				RelativeTransform,
				DesiredWorldSize))
			{
				TargetInfo.InteractionBodies.Add(Proxy);
			}
		}
	}
}

UStaticMeshComponent* UGroundInteractionWorldSubsystem::CreateProxy(
	AActor* OwnerActor,
	USkeletalMeshComponent* Parent,
	const FName BoneName,
	UStaticMesh* StaticMesh,
	const FTransform& RelativeTransform,
	const FVector& DesiredWorldSize)
{
	if (!OwnerActor || !Parent || !StaticMesh)
	{
		return nullptr;
	}

	UStaticMeshComponent* ProxyComp = NewObject<UStaticMeshComponent>(
		OwnerActor,
		MakeUniqueObjectName(OwnerActor, UStaticMeshComponent::StaticClass(), TEXT("GIProxy")));

	if (!ProxyComp)
	{
		return nullptr;
	}

	OwnerActor->AddInstanceComponent(ProxyComp);

	ProxyComp->SetMobility(EComponentMobility::Movable);
	ProxyComp->SetStaticMesh(StaticMesh);
	ProxyComp->SetupAttachment(Parent, BoneName);
	ProxyComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ProxyComp->SetGenerateOverlapEvents(false);
	ProxyComp->SetCastShadow(false);
	ProxyComp->SetVisibleInSceneCaptureOnly(true);
	ProxyComp->SetReceivesDecals(false);
	ProxyComp->bCastDynamicShadow = false;
	ProxyComp->bAffectDistanceFieldLighting = false;
	ProxyComp->ComponentTags.Add(TEXT("ExcludeFromSparsePixelate"));

	const FVector SourceFullSize = StaticMesh->GetBounds().BoxExtent * 2.0f;
	const FVector SafeSourceSize(
		FMath::Max(SourceFullSize.X, 1.0f),
		FMath::Max(SourceFullSize.Y, 1.0f),
		FMath::Max(SourceFullSize.Z, 1.0f));

	const FVector RelativeScale3D(
		DesiredWorldSize.X / SafeSourceSize.X,
		DesiredWorldSize.Y / SafeSourceSize.Y,
		DesiredWorldSize.Z / SafeSourceSize.Z);

	FTransform FinalRelativeTransform = RelativeTransform;
	FinalRelativeTransform.SetScale3D(RelativeScale3D);
	ProxyComp->SetRelativeTransform(FinalRelativeTransform);
	
	ProxyComp->RegisterComponent();

	return ProxyComp;
}
