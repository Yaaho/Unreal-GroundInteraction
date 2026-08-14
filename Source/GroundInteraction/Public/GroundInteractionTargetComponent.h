// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WaterInteractionCommon.h"
#include "GroundInteractionTargetComponent.generated.h"

class UGroundInteractionWorldSubsystem;
class UPhysicsAsset;
class USkeletalMeshComponent;

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class GROUNDINTERACTION_API UGroundInteractionTargetComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGroundInteractionTargetComponent();

	UFUNCTION(BlueprintCallable, Category="Ground Interaction|Source")
	void SetInteractionPhysicsAssetOverride(UPhysicsAsset* NewPhysicsAsset);

	UFUNCTION(BlueprintPure, Category="Ground Interaction|Source")
	UPhysicsAsset* GetInteractionPhysicsAssetOverride() const;

	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

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
	UPhysicsAsset* ResolveInteractionPhysicsAsset(const USkeletalMeshComponent* SkeletalMeshComp) const;

	void BuildProxies(UGroundInteractionWorldSubsystem* Subsystem, USkeletalMeshComponent* SkeletalMeshComp);

	UStaticMeshComponent* CreateProxy(
		UGroundInteractionWorldSubsystem* Subsystem,
		AActor* OwnerActor,
		USkeletalMeshComponent* Parent,
		const FName BoneName,
		UStaticMesh* StaticMesh,
		const FTransform& RelativeTransform,
		const FVector& BodyLocalSize,
		EWaterSplatShape WaterSplatShape);

private:
	// Ground Interaction 콜리전 용도로 사용할 피직스 에셋 
	// 비어 있으면 Source Skeletal Mesh의 Physics Asset을 사용한다.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Ground Interaction|Source", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UPhysicsAsset> GroundInteractionPhysicsAssetOverride = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> SourceSkeletalMesh = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> InteractionBodies;

	float SourceMeshWaitElapsedSeconds = 0.0f;
};
