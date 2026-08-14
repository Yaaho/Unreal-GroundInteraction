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
	float FadeRiseSpeed = 100.0f; // cm/s

	UPROPERTY(EditAnywhere, Config, Category="GroundInteraction", meta=(ClampMin="0.0"))
	float FadeMaxHeight = 300.0f; // cm
	
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="GroundInteraction", meta=(ClampMin="16", UIMin="128", UIMax="2048"))
	int32 GroundInteractionRenderTargetSize = 512;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="GroundInteraction", meta=(ClampMin="1", UIMin="1", UIMax="32"))
	int32 GroundInteractionLocationSnapCellCount = 8;
	
	// Depth fade 실행 빈도
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="GroundInteraction", meta=(ClampMin="1", UIMin="1", UIMax="120"))
	int32 DepthFadeUpdateRate = 30;
	
	// Depth Update 실행 빈도
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="GroundInteraction", meta=(ClampMin="1", UIMin="1", UIMax="120"))
	int32 GroundInteractionDepthUpdateRate = 30;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction")
	bool bEnableWaterInteraction = false;

	// 해야할 일 내부 시뮬레이션 영역을 출력 RT보다 크게 확장
	// 렌터타겟 영역 밖이 0으로 샘플링되어 또다른 파동을 만드는 문제가 있다.
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction", meta=(EditCondition="bEnableWaterInteraction", EditConditionHides))
	FVector WaterInteractionWorldExtent = FVector(1000.0f, 1000.0f, 1000.0f);

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction", meta=(EditCondition="bEnableWaterInteraction", EditConditionHides, ClampMin="16", UIMin="128", UIMax="2048"))
	int32 WaterInteractionRenderTargetSize = 1024;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction", meta=(EditCondition="bEnableWaterInteraction", EditConditionHides, ClampMin="1", UIMin="1", UIMax="32"))
	int32 WaterInteractionLocationSnapCellCount = 64;
	
	// Water Splat 생성 검사 빈도
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction", meta=(EditCondition="bEnableWaterInteraction", EditConditionHides, ClampMin="1", UIMin="1", UIMax="120", Units="Hz"))
	int32 WaterInteractionUpdateRate = 60;

	// 프리미티브별 water splat 주기 범위(초)
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction", meta=(EditCondition="bEnableWaterInteraction", EditConditionHides, ClampMin="0.0", UIMin="0.0", UIMax="2.0", Units="s"))
	float WaterInteractionSplatIntervalMin = 0.1f;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction", meta=(EditCondition="bEnableWaterInteraction", EditConditionHides, ClampMin="0.0", UIMin="0.0", UIMax="2.0", Units="s"))
	float WaterInteractionSplatIntervalMax = 0.3f;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction", meta=(EditCondition="bEnableWaterInteraction", EditConditionHides, ClampMin="0.0", UIMin="0.0"))
	float WaterInteractionMinVelocity = 5.0f;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction", meta=(EditCondition="bEnableWaterInteraction", EditConditionHides, ClampMin="0.0", UIMin="0.0"))
	float WaterInteractionSurfaceDistanceTolerance = 10.0f;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction", meta=(EditCondition="bEnableWaterInteraction", EditConditionHides, ClampMin="0.0", UIMin="0.0"))
	float WaterInteractionPulseStrength = 1.0f;
	
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction", meta=(EditCondition="bEnableWaterInteraction", EditConditionHides, ClampMin="0.0", UIMin="0.0", UIMax="2000.0"))
	float WaterInteractionAdvectAmount = 100.0f;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction", meta=(EditCondition="bEnableWaterInteraction", EditConditionHides, ClampMin="0.0", UIMin="0.0"))
	float WaterInteractionDamping = 3.0f;

	// Water Height 시뮬레이션의 초당 고정 실행 횟수
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction", meta=(EditCondition="bEnableWaterInteraction", EditConditionHides, ClampMin="1", UIMin="15", UIMax="120", Units="Hz"))
	int32 WaterInteractionSimulationRate = 60;

	// 프레임 저하 시 한 렌더 프레임에서 따라잡을 최대 시뮬레이션 횟수
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category="WaterInteraction", meta=(EditCondition="bEnableWaterInteraction", EditConditionHides, ClampMin="1", UIMin="1", UIMax="8"))
	int32 WaterInteractionMaxSubstepsPerFrame = 4;
	
	FIntPoint GetGroundInteractionRenderTargetSize() const;
	FIntPoint GetWaterInteractionRenderTargetSize() const;
	
	float GetDepthFadeUpdateIntervalSeconds() const;
	float GetDepthUpdateIntervalSeconds() const;
	float GetWaterInteractionUpdateIntervalSeconds() const;
	FVector2D GetWaterInteractionSplatIntervalRange() const;
	
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
