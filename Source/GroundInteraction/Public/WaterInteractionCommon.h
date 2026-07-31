// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class UGroundInteractionProxyMeshComponent;

enum class EWaterSplatShape : uint8
{
	Sphere = 0,
	Box = 1
};

struct FWaterSplatWorld
{
	FVector2d AbsoluteCenterXY = FVector2d(0.0, 0.0);
	FVector2d HalfSizeXY = FVector2d(0.0, 0.0);

	float RotationCos = 1.0f;
	float RotationSin = 0.0f;
	uint32 ShapeType = static_cast<uint32>(EWaterSplatShape::Sphere);
	uint32 Padding = 0;
};

struct FWaterSplatRelative
{
	FVector2f RelativeCenterXY = FVector2f(0.0f, 0.0f);
	FVector2f HalfSizeXY = FVector2f(0.0f, 0.0f);

	float RotationCos = 1.0f;
	float RotationSin = 0.0f;
	uint32 ShapeType = static_cast<uint32>(EWaterSplatShape::Sphere);
	uint32 Padding = 0;
};

static_assert(sizeof(FWaterSplatRelative) == 32, "FWaterSplatRelative must match the 32-byte HLSL StructuredBuffer element layout.");

namespace WaterInteractionCommon
{
	GROUNDINTERACTION_API bool TryMakeWaterSplatWorld(
		const UGroundInteractionProxyMeshComponent* Proxy,
		const FVector& WaterSurfacePositionWS,
		const FVector& WaterSurfaceNormalWS,
		FWaterSplatWorld& OutSplat);
}
