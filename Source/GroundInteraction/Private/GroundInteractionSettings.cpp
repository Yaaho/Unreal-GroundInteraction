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