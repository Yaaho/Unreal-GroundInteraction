// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
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

	virtual FName GetCategoryName() const override;
	
public:
	// 캡쳐 영역 정보를 머티리얼에 전달할 MPC 에셋
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Assets")
	TSoftObjectPtr<UMaterialParameterCollection> MaterialParameterCollection;
	
	// 텍스쳐가 가리키는 월드 범위
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Capture")
	FVector CaptureWorldExtent = FVector(1000.0f, 1000.0f, 1000.0f);

	UPROPERTY(EditAnywhere, Config, Category="Fade", meta=(ClampMin="0.0"))
	float FadeRiseSpeed = 150.0f; // cm/s

	UPROPERTY(EditAnywhere, Config, Category="Fade", meta=(ClampMin="0.0"))
	float FadeMaxHeight = 120.0f; // cm
};
