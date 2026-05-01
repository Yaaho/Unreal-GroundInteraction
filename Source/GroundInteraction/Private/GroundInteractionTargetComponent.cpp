// Fill out your copyright notice in the Description page of Project Settings.


#include "GroundInteractionTargetComponent.h"
#include "GroundInteractionWorldSubsystem.h"
#include "GameFramework/Character.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"


void UGroundInteractionTargetComponent::OnRegister()
{
	Super::OnRegister();
	RegisterToSubsystem();
}

void UGroundInteractionTargetComponent::OnUnregister()
{
	UnregisterFromSubsystem();
	Super::OnUnregister();
}

void UGroundInteractionTargetComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	UnregisterFromSubsystem();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

#if WITH_EDITOR
void UGroundInteractionTargetComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	
	UnregisterFromSubsystem();
	RegisterToSubsystem();
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
		if (UGroundInteractionWorldSubsystem* Subsystem =
			World->GetSubsystem<UGroundInteractionWorldSubsystem>())
		{
			Subsystem->RegisterTarget(this);
		}
	}
}

void UGroundInteractionTargetComponent::UnregisterFromSubsystem()
{
	if (UWorld* World = GetWorld())
	{
		if (UGroundInteractionWorldSubsystem* Subsystem =
			World->GetSubsystem<UGroundInteractionWorldSubsystem>())
		{
			Subsystem->UnregisterTarget(this);
		}
	}
}

void UGroundInteractionTargetComponent::RebuildCaptureTargetInfo()
{
	DestroyCaptureTargetInfo();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UGroundInteractionWorldSubsystem* Subsystem =
		World->GetSubsystem<UGroundInteractionWorldSubsystem>();

	if (!Subsystem || !Subsystem->AreAssetsReady())
	{
		return;
	}

	SourceSkeletalMesh = ResolveSourceSkeletalMesh();

	if (!IsValid(SourceSkeletalMesh))
	{
		return;
	}

	BuildProxies(Subsystem, SourceSkeletalMesh);
}

void UGroundInteractionTargetComponent::DestroyCaptureTargetInfo()
{
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

	UStaticMesh* CubeMesh = Subsystem->GetCubeMesh();
	UStaticMesh* SphereMesh = Subsystem->GetSphereMesh();
	UStaticMesh* CylinderMesh = Subsystem->GetCylinderMesh();

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
				Subsystem,
				OwnerActor,
				SkeletalMeshComp,
				BoneName,
				CubeMesh,
				RelativeTransform,
				DesiredWorldSize))
			{
				InteractionBodies.Add(Proxy);
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
				Subsystem,
				OwnerActor,
				SkeletalMeshComp,
				BoneName,
				SphereMesh,
				RelativeTransform,
				DesiredWorldSize))
			{
				InteractionBodies.Add(Proxy);
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
				Subsystem,
				OwnerActor,
				SkeletalMeshComp,
				BoneName,
				CylinderMesh,
				RelativeTransform,
				DesiredWorldSize))
			{
				InteractionBodies.Add(Proxy);
			}
		}
	}
}

UStaticMeshComponent* UGroundInteractionTargetComponent::CreateProxy(
	UGroundInteractionWorldSubsystem* Subsystem,
	AActor* OwnerActor,
	USkeletalMeshComponent* Parent,
	const FName BoneName,
	UStaticMesh* StaticMesh,
	const FTransform& RelativeTransform,
	const FVector& DesiredWorldSize)
{
	if (!Subsystem || !OwnerActor || !Parent || !StaticMesh)
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
	
	// 이 인터렉션 타겟 프록시 컴포넌트는 SparsePixelate 에서 제외된다.
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

	Subsystem->RegisterInteractionPrimitive(ProxyComp);

	return ProxyComp;
}

