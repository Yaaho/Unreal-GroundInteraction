// Fill out your copyright notice in the Description page of Project Settings.


#include "GroundInteractionTargetComponent.h"
#include "GroundInteractionWorldSubsystem.h"
#include "GameFramework/Character.h"

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
		if (UGroundInteractionWorldSubsystem* Sub = World->GetSubsystem<UGroundInteractionWorldSubsystem>())
		{
			Sub->RegisterTarget(this);
		}
	}
}

void UGroundInteractionTargetComponent::UnregisterFromSubsystem()
{
	if (UWorld* World = GetWorld())
	{
		if (UGroundInteractionWorldSubsystem* Sub = World->GetSubsystem<UGroundInteractionWorldSubsystem>())
		{
			Sub->UnregisterTarget(this);
		}
	}
}

