// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject\SoftObjectPtr.h"
#include "Engine/DeveloperSettings.h"
#include "GroundInteractionSettings.generated.h"

/**
 * 
 */
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Ground Interaction"))
class GROUNDINTERACTION_API UGroundInteractionSettings : public UDeveloperSettings
{
	GENERATED_BODY()
	
public:
	UGroundInteractionSettings();

	static const UGroundInteractionSettings* Get();
	
public:
	// 캡쳐 영역 정보를 머티리얼에 전달할 MPC 에셋
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Assets")
	TSoftObjectPtr<UMaterialParameterCollection> MaterialParameterCollection;
	
	// 텍스쳐가 가리키는 월드 범위
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="GroundInteraction")
	FVector GroundInteractionWorldExtent = FVector(1000.0f, 1000.0f, 1000.0f);

	UPROPERTY(EditAnywhere, Config, Category="GroundInteraction", meta=(ClampMin="0.0"))
	float FadeRiseSpeed = 150.0f; // cm/s

	UPROPERTY(EditAnywhere, Config, Category="GroundInteraction", meta=(ClampMin="0.0"))
	float FadeMaxHeight = 120.0f; // cm
	
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="GroundInteraction", meta=(ClampMin="16", UIMin="128", UIMax="2048"))
	int32 GroundInteractionRenderTargetSize = 512;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="GroundInteraction", meta=(ClampMin="1", UIMin="1", UIMax="32"))
	int32 GroundInteractionLocationSnapCellCount = 1;
	
	// Depth fade 실행 빈도
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="GroundInteraction", meta=(ClampMin="1", UIMin="1", UIMax="120"))
	int32 DepthFadeUpdateRate = 60;
	
	// Depth Update 실행 빈도
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="GroundInteraction", meta=(ClampMin="1", UIMin="1", UIMax="120"))
	int32 GroundInteractionDepthUpdateRate = 60;
	
	FIntPoint GetGroundInteractionRenderTargetSize() const;
	
	float GetDepthFadeUpdateIntervalSeconds() const;
	float GetDepthUpdateIntervalSeconds() const;
	
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
