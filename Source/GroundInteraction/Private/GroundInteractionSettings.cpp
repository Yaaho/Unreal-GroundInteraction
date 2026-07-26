// Fill out your copyright notice in the Description page of Project Settings.


#include "GroundInteractionSettings.h"

UGroundInteractionSettings::UGroundInteractionSettings()
{
}

const UGroundInteractionSettings* UGroundInteractionSettings::Get()
{
	return GetDefault<UGroundInteractionSettings>();
}

FIntPoint UGroundInteractionSettings::GetGroundInteractionRenderTargetSize() const
{
	const int32 SafeRenderTargetSize = FMath::Max(GroundInteractionRenderTargetSize, 16);
	return FIntPoint(SafeRenderTargetSize, SafeRenderTargetSize);
}

FIntPoint UGroundInteractionSettings::GetWaterInteractionRenderTargetSize() const
{
	const int32 SafeRenderTargetSize = FMath::Max(WaterInteractionRenderTargetSize, 16);
	return FIntPoint(SafeRenderTargetSize, SafeRenderTargetSize);
}

float UGroundInteractionSettings::GetDepthFadeUpdateIntervalSeconds() const
{
	return 1.0f / float(FMath::Max(DepthFadeUpdateRate, 1));
}

float UGroundInteractionSettings::GetDepthUpdateIntervalSeconds() const
{
	return 1.0f / float(FMath::Max(GroundInteractionDepthUpdateRate, 1));
}

float UGroundInteractionSettings::GetWaterInteractionUpdateIntervalSeconds() const
{
	return 1.0f / float(FMath::Max(WaterInteractionUpdateRate, 1));
}

#if WITH_EDITOR
void UGroundInteractionSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	
	DepthFadeUpdateRate = FMath::Max(DepthFadeUpdateRate, 1);
	GroundInteractionDepthUpdateRate = FMath::Max(GroundInteractionDepthUpdateRate, 1);
	GroundInteractionLocationSnapCellCount = FMath::Max(GroundInteractionLocationSnapCellCount, 1);
	WaterInteractionRenderTargetSize = FMath::Max(WaterInteractionRenderTargetSize, 16);
	WaterInteractionLocationSnapCellCount = FMath::Max(WaterInteractionLocationSnapCellCount, 1);
	WaterInteractionUpdateRate = FMath::Max(WaterInteractionUpdateRate, 1);
	WaterInteractionSplatSpawnProbability = FMath::Clamp(WaterInteractionSplatSpawnProbability, 0.0f, 1.0f);
	WaterInteractionMinVelocity = FMath::Max(WaterInteractionMinVelocity, 0.0f);
	WaterInteractionSurfaceDistanceTolerance = FMath::Max(WaterInteractionSurfaceDistanceTolerance, 0.0f);
	WaterInteractionPulseStrength = FMath::Max(WaterInteractionPulseStrength, 0.0f);

	// GroundInteractionDepthUpdateRate 빈도가  DepthFadeUpdateRate 빈도보다 낮아지지 않도록 함
	if (GroundInteractionDepthUpdateRate < DepthFadeUpdateRate)
	{
		GroundInteractionDepthUpdateRate = DepthFadeUpdateRate;
	}
}
#endif
