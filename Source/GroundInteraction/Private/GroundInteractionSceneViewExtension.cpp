// Fill out your copyright notice in the Description page of Project Settings.

#include "GroundInteractionSceneViewExtension.h"
#include "GroundInteractionWorldSubsystem.h"
#include "GroundInteractionRendererProxy.h"
#include "GroundInteractionRendererComponent.h"
#include "WaterInteractionRendererComponent.h"
#include "WaterInteractionRendererProxy.h"

#include "Runtime\Engine\Public\SceneView.h"



FGroundInteractionSceneViewExtension::FGroundInteractionSceneViewExtension(const FAutoRegister& AutoRegister, UGroundInteractionWorldSubsystem* InSubsystem, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld), OwnerSubsystem(InSubsystem)
{
}

bool FGroundInteractionSceneViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	if (!FWorldSceneViewExtension::IsActiveThisFrame_Internal(Context))
	{
		return false;
	}

	const UGroundInteractionWorldSubsystem* Subsystem = OwnerSubsystem.Get();

	if (!Subsystem || !Subsystem->AreAssetsReady())
	{
		return false;
	}

	UWorld* CurrentWorld = Subsystem->GetWorld();

	if (!CurrentWorld)
	{
		return false;
	}

	return CurrentWorld->WorldType == EWorldType::PIE || CurrentWorld->WorldType == EWorldType::Game;
}

void FGroundInteractionSceneViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	if (!InViewFamily.Scene || InViewFamily.Views.Num() == 0)
	{
		return;
	}

	const FSceneView* PrimaryView = InViewFamily.Views[0];
	
	if (!PrimaryView || PrimaryView->bIsSceneCapture || PrimaryView->bIsReflectionCapture)
	{
		return;
	}

	UGroundInteractionWorldSubsystem* Subsystem = OwnerSubsystem.Get();

	if (!Subsystem || !Subsystem->AreAssetsReady())
	{
		return;
	}

	if (UGroundInteractionRendererComponent* RendererComponent = Subsystem->GetRendererComponent())
	{
		RendererComponent->UpdateGroundInteractionDepth_GameThread(InViewFamily);
	}

	if (UWaterInteractionRendererComponent* WaterRendererComponent = Subsystem->GetWaterRendererComponent())
	{
		WaterRendererComponent->UpdateWaterInteraction_GameThread(InViewFamily);
	}
}


void FGroundInteractionSceneViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	if (!InViewFamily.Scene || InViewFamily.Views.Num() == 0)
	{
		return;
	}

	const FSceneView* PrimaryView = InViewFamily.Views[0];

	if (!PrimaryView || PrimaryView->bIsSceneCapture || PrimaryView->bIsReflectionCapture)
	{
		return;
	}

	TArray<FGroundInteractionRendererProxy*> Proxies = FGroundInteractionRendererProxy::GetProxies(InViewFamily.Scene);

	for (FGroundInteractionRendererProxy* Proxy : Proxies)
	{
		if (!Proxy)
		{
			continue;
		}

		Proxy->Render(GraphBuilder, InViewFamily);
	}

	TArray<FWaterInteractionRendererProxy*> WaterProxies = FWaterInteractionRendererProxy::GetProxies(InViewFamily.Scene);

	for (FWaterInteractionRendererProxy* Proxy : WaterProxies)
	{
		if (!Proxy)
		{
			continue;
		}

		Proxy->Render(GraphBuilder, InViewFamily);
	}
}
