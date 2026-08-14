// Fill out your copyright notice in the Description page of Project Settings.


#include "GroundInteractionTargetComponent.h"
#include "GroundInteractionProxyMeshComponent.h"
#include "GroundInteractionWorldSubsystem.h"
#include "WaterInteractionCommon.h"

#include "Engine/World.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Character.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"

DEFINE_LOG_CATEGORY_STATIC(LogGroundInteractionTarget, Log, All);

static constexpr float SourceMeshWaitTickIntervalSeconds = 1.0f;
static constexpr float SourceMeshWaitTimeoutSeconds = 30.0f;

UGroundInteractionTargetComponent::UGroundInteractionTargetComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickInterval = SourceMeshWaitTickIntervalSeconds;
}

void UGroundInteractionTargetComponent::SetInteractionPhysicsAssetOverride(UPhysicsAsset* NewPhysicsAsset)
{
	GroundInteractionPhysicsAssetOverride = NewPhysicsAsset;

	// 같은 Asset을 다시 전달한 경우에도 Source Mesh 변경 후 명시적인 새로고침 용도로 사용할 수 있다.
	if (IsRegistered())
	{
		RebuildCaptureTargetInfo();
	}
}

UPhysicsAsset* UGroundInteractionTargetComponent::GetInteractionPhysicsAssetOverride() const
{
	return GroundInteractionPhysicsAssetOverride.Get();
}

void UGroundInteractionTargetComponent::OnRegister()
{
	Super::OnRegister();
	RegisterToSubsystem();
}

void UGroundInteractionTargetComponent::OnUnregister()
{
	DestroyCaptureTargetInfo();
	UnregisterFromSubsystem();
	Super::OnUnregister();
}

void UGroundInteractionTargetComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	DestroyCaptureTargetInfo();
	UnregisterFromSubsystem();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UGroundInteractionTargetComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!IsRegistered())
	{
		SetComponentTickEnabled(false);
		return;
	}

	USkeletalMeshComponent* SkeletalMeshComp = ResolveSourceSkeletalMesh();
	if (!IsValid(SkeletalMeshComp))
	{
		SetComponentTickEnabled(false);
		return;
	}

	// Source Mesh 가 비동기로 로드되길 기다린다.
	if (!IsValid(SkeletalMeshComp->GetSkeletalMeshAsset()))
	{
		SourceMeshWaitElapsedSeconds += DeltaTime;
		if (SourceMeshWaitElapsedSeconds >= SourceMeshWaitTimeoutSeconds)
		{
			// 대기 시간이 SourceMeshWaitTimeoutSeconds 를 넘어가면 틱 중단 후 리턴
			SetComponentTickEnabled(false);
		}

		return;
	}

	RebuildCaptureTargetInfo();
}

#if WITH_EDITOR
void UGroundInteractionTargetComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (IsRegistered()
		&& PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(
			UGroundInteractionTargetComponent,
			GroundInteractionPhysicsAssetOverride))
	{
		RebuildCaptureTargetInfo();
	}
}
#endif

USkeletalMeshComponent* UGroundInteractionTargetComponent::ResolveSourceSkeletalMesh() const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return nullptr;
	}

	if (const ACharacter* Character = Cast<ACharacter>(Owner))
	{
		if (USkeletalMeshComponent* CharacterMesh = Character->GetMesh())
		{
			return CharacterMesh;
		}
	}

	if (USkeletalMeshComponent* FoundMesh = Owner->FindComponentByClass<USkeletalMeshComponent>())
	{
		return FoundMesh;
	}

	return nullptr;
}

void UGroundInteractionTargetComponent::RegisterToSubsystem()
{
	if (UWorld* World = GetWorld())
	{
		if (UGroundInteractionWorldSubsystem* Subsystem = World->GetSubsystem<UGroundInteractionWorldSubsystem>())
		{
			Subsystem->RegisterTarget(this);
		}
	}
}

void UGroundInteractionTargetComponent::UnregisterFromSubsystem()
{
	if (UWorld* World = GetWorld())
	{
		if (UGroundInteractionWorldSubsystem* Subsystem = World->GetSubsystem<UGroundInteractionWorldSubsystem>())
		{
			Subsystem->UnregisterTarget(this);
		}
	}
}

UPhysicsAsset* UGroundInteractionTargetComponent::ResolveInteractionPhysicsAsset(const USkeletalMeshComponent* SkeletalMeshComp) const
{
	if (IsValid(GroundInteractionPhysicsAssetOverride))
	{
		return GroundInteractionPhysicsAssetOverride.Get();
	}

	return IsValid(SkeletalMeshComp) ? SkeletalMeshComp->GetPhysicsAsset() : nullptr;
}

void UGroundInteractionTargetComponent::RebuildCaptureTargetInfo()
{
	DestroyCaptureTargetInfo();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UGroundInteractionWorldSubsystem* Subsystem = World->GetSubsystem<UGroundInteractionWorldSubsystem>();

	if (!Subsystem || !Subsystem->AreAssetsReady())
	{
		return;
	}

	SourceSkeletalMesh = ResolveSourceSkeletalMesh();

	if (!IsValid(SourceSkeletalMesh))
	{
		return;
	}
	
	if (!IsValid(SourceSkeletalMesh->GetSkeletalMeshAsset()))
	{
		SetComponentTickEnabled(true);
		return;
	}

	BuildProxies(Subsystem, SourceSkeletalMesh);
}

void UGroundInteractionTargetComponent::DestroyCaptureTargetInfo()
{
	SetComponentTickEnabled(false);
	SourceMeshWaitElapsedSeconds = 0.0f;

	UGroundInteractionWorldSubsystem* Subsystem = nullptr;

	if (UWorld* World = GetWorld())
	{
		Subsystem = World->GetSubsystem<UGroundInteractionWorldSubsystem>();
	}

	for (UStaticMeshComponent* ProxyComp : InteractionBodies)
	{
		if (Subsystem && IsValid(ProxyComp))
		{
			Subsystem->UnregisterInteractionPrimitive(ProxyComp);
		}

		if (IsValid(ProxyComp))
		{
			ProxyComp->DestroyComponent();
		}
	}

	InteractionBodies.Reset();
	SourceSkeletalMesh = nullptr;
}

void UGroundInteractionTargetComponent::BuildProxies(
	UGroundInteractionWorldSubsystem* Subsystem,
	USkeletalMeshComponent* SkeletalMeshComp)
{
	if (!Subsystem || !IsValid(SkeletalMeshComp) || !Subsystem->AreAssetsReady())
	{
		return;
	}

	UPhysicsAsset* PhysicsAsset = ResolveInteractionPhysicsAsset(SkeletalMeshComp);
	if (!PhysicsAsset)
	{
		return;
	}

	AActor* OwnerActor = SkeletalMeshComp->GetOwner();
	if (!OwnerActor)
	{
		return;
	}

	UStaticMesh* CubeMesh = Subsystem->GetCubeMesh();
	UStaticMesh* SphereMesh = Subsystem->GetSphereMesh();
	UStaticMesh* CylinderMesh = Subsystem->GetCapsuleMesh();

	for (USkeletalBodySetup* BodySetup : PhysicsAsset->SkeletalBodySetups)
	{
		if (!BodySetup)
		{
			continue;
		}

		const FName BoneName = BodySetup->BoneName;
		if (BoneName.IsNone() || SkeletalMeshComp->GetBoneIndex(BoneName) == INDEX_NONE)
		{
			// GroundInteraction Physics Asset 에 지정된 본이 Source Skeletal Mesh 에 없다면 프록시 생성 건너뜀
			continue;
		}

		const FKAggregateGeom& AggGeom = BodySetup->AggGeom;

		for (const FKBoxElem& BoxElem : AggGeom.BoxElems)
		{
			const FTransform RelativeTransform(BoxElem.Rotation.Quaternion(), BoxElem.Center, FVector::OneVector);
			const FVector BodyLocalSize(BoxElem.X, BoxElem.Y, BoxElem.Z);

			if (UStaticMeshComponent* Proxy = CreateProxy(Subsystem, OwnerActor, SkeletalMeshComp, BoneName, CubeMesh, RelativeTransform, BodyLocalSize, EWaterSplatShape::Box))
			{
				InteractionBodies.Add(Proxy);
			}
		}

		for (const FKSphereElem& SphereElem : AggGeom.SphereElems)
		{
			const FTransform RelativeTransform(FQuat::Identity, SphereElem.Center, FVector::OneVector);
			const float Diameter = SphereElem.Radius * 2.0f;
			const FVector BodyLocalSize(Diameter, Diameter, Diameter);

			if (UStaticMeshComponent* Proxy = CreateProxy(Subsystem, OwnerActor, SkeletalMeshComp, BoneName, SphereMesh, RelativeTransform, BodyLocalSize, EWaterSplatShape::Sphere))
			{
				InteractionBodies.Add(Proxy);
			}
		}

		for (const FKSphylElem& SphylElem : AggGeom.SphylElems)
		{
			const FTransform RelativeTransform(SphylElem.Rotation.Quaternion(), SphylElem.Center, FVector::OneVector);

			const float Diameter = SphylElem.Radius * 2.0f;
			const float FullHeight = SphylElem.Length + Diameter;
			const FVector BodyLocalSize(Diameter, Diameter, FullHeight);

			if (UStaticMeshComponent* Proxy = CreateProxy(Subsystem, OwnerActor, SkeletalMeshComp, BoneName, CylinderMesh, RelativeTransform, BodyLocalSize, EWaterSplatShape::Sphere))
			{
				InteractionBodies.Add(Proxy);
			}
		}
	}
}

UStaticMeshComponent* UGroundInteractionTargetComponent::CreateProxy(UGroundInteractionWorldSubsystem* Subsystem, AActor* OwnerActor, 
	USkeletalMeshComponent* Parent, const FName BoneName, UStaticMesh* StaticMesh,
	const FTransform& RelativeTransform, const FVector& BodyLocalSize, EWaterSplatShape WaterSplatShape)
{
	if (!Subsystem || !OwnerActor || !Parent || !StaticMesh)
	{
		return nullptr;
	}

	const FName ProxyName = MakeUniqueObjectName(OwnerActor, UGroundInteractionProxyMeshComponent::StaticClass(), TEXT("GIProxy"));
	UGroundInteractionProxyMeshComponent* ProxyComp = NewObject<UGroundInteractionProxyMeshComponent>(OwnerActor, UGroundInteractionProxyMeshComponent::StaticClass(), ProxyName, RF_Transient | RF_DuplicateTransient);

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
	ProxyComp->SetReceivesDecals(false);
	ProxyComp->bCastDynamicShadow = false;
	ProxyComp->bAffectDistanceFieldLighting = false;
	
	const FVector SourceFullSize = StaticMesh->GetBounds().BoxExtent * 2.0f;
	const FVector SafeSourceSize(FMath::Max(SourceFullSize.X, 1.0f), FMath::Max(SourceFullSize.Y, 1.0f),FMath::Max(SourceFullSize.Z, 1.0f));
	const FVector RelativeScale3D(BodyLocalSize.X / SafeSourceSize.X, BodyLocalSize.Y / SafeSourceSize.Y, BodyLocalSize.Z / SafeSourceSize.Z);

	FTransform FinalRelativeTransform = RelativeTransform;
	FinalRelativeTransform.SetScale3D(RelativeScale3D);

	ProxyComp->SetWaterSplatShape(WaterSplatShape);
	ProxyComp->SetRelativeTransform(FinalRelativeTransform);
	ProxyComp->RegisterComponent();

	Subsystem->RegisterInteractionPrimitive(ProxyComp);

	return ProxyComp;
}

