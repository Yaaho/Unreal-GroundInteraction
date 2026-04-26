// Fill out your copyright notice in the Description page of Project Settings.


#include "GroundInteractionSettings.h"

UGroundInteractionSettings::UGroundInteractionSettings()
{
}

const UGroundInteractionSettings* UGroundInteractionSettings::Get()
{
	return GetDefault<UGroundInteractionSettings>();
}

FName UGroundInteractionSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

FIntPoint UGroundInteractionSettings::GetRenderTargetSize() const
{
	return FIntPoint(
		FMath::Max(RenderTargetSizeX, 16),
		FMath::Max(RenderTargetSizeY, 16));
}

float UGroundInteractionSettings::GetDepthFadeUpdateIntervalSeconds() const
{
	return 1.0f / float(FMath::Max(DepthFadeUpdateRate, 1));
}

float UGroundInteractionSettings::GetDepthCaptureUpdateIntervalSeconds() const
{
	return 1.0f / float(FMath::Max(DepthCaptureUpdateRate, 1));
}

#if WITH_EDITOR
void UGroundInteractionSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	
	DepthFadeUpdateRate = FMath::Max(DepthFadeUpdateRate, 1);
	DepthCaptureUpdateRate = FMath::Max(DepthCaptureUpdateRate, 1);

	// DepthCaptureUpdateRate 빈도가  DepthFadeUpdateRate 빈도보다 낮아지지 않도록 함
	if (DepthCaptureUpdateRate < DepthFadeUpdateRate)
	{
		DepthCaptureUpdateRate = DepthFadeUpdateRate;
	}
}
#endif