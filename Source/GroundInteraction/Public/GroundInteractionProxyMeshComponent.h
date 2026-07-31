// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "WaterInteractionCommon.h"
#include "GroundInteractionProxyMeshComponent.generated.h"

UCLASS(ClassGroup=(Custom))
class GROUNDINTERACTION_API UGroundInteractionProxyMeshComponent : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UGroundInteractionProxyMeshComponent(const FObjectInitializer& ObjectInitializer);

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual bool IsHLODRelevant() const override { return false; }

	void SetWaterSplatShape(EWaterSplatShape InShape)
	{
		WaterSplatShape = InShape;
	}

	EWaterSplatShape GetWaterSplatShape() const
	{
		return WaterSplatShape;
	}

private:
	EWaterSplatShape WaterSplatShape = EWaterSplatShape::Sphere;
};
