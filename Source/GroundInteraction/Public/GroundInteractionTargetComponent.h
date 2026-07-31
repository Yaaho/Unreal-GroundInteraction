// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WaterInteractionCommon.h"
#include "GroundInteractionTargetComponent.generated.h"

class UGroundInteractionWorldSubsystem;

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class GROUNDINTERACTION_API UGroundInteractionTargetComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	USkeletalMeshComponent* ResolveSourceSkeletalMesh() const;

	void RebuildCaptureTargetInfo();
	void DestroyCaptureTargetInfo();

	const TArray<TObjectPtr<UStaticMeshComponent>>& GetInteractionBodies() const
	{
		return InteractionBodies;
	}

private:
	void RegisterToSubsystem();
	void UnregisterFromSubsystem();

	void BuildProxies(UGroundInteractionWorldSubsystem* Subsystem, USkeletalMeshComponent* SkeletalMeshComp);

	UStaticMeshComponent* CreateProxy(
		UGroundInteractionWorldSubsystem* Subsystem,
		AActor* OwnerActor,
		USkeletalMeshComponent* Parent,
		const FName BoneName,
		UStaticMesh* StaticMesh,
		const FTransform& RelativeTransform,
		const FVector& DesiredWorldSize,
		EWaterSplatShape WaterSplatShape);

private:
	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> SourceSkeletalMesh = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> InteractionBodies;
};
