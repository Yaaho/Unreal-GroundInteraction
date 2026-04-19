// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GroundInteractionTargetComponent.generated.h"


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
	
private:
	void RegisterToSubsystem();
	void UnregisterFromSubsystem();
};
