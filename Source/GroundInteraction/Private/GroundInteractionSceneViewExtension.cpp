// Fill out your copyright notice in the Description page of Project Settings.

#include "GroundInteractionSceneViewExtension.h"
#include "GroundInteractionWorldSubsystem.h"
#include "GroundInteractionRendererProxy.h"
#include "GroundInteractionRendererComponent.h"

FGroundInteractionSceneViewExtension::FGroundInteractionSceneViewExtension(
	const FAutoRegister& AutoRegister,
	UGroundInteractionWorldSubsystem* InSubsystem,
	UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
	, OwnerSubsystem(InSubsystem)
{
}

bool FGroundInteractionSceneViewExtension::IsActiveThisFrame_Internal(
	const FSceneViewExtensionContext& Context) const
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

void FGroundInteractionSceneViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	if (!InViewFamily.Scene || InViewFamily.Views.Num() == 0)
	{
		return;
	}

	const FSceneView* PrimaryView = InViewFamily.Views[0];
	
	if (!PrimaryView || PrimaryView != &InView)
	{
		return;
	}

	if (InView.bIsSceneCapture || InView.bIsReflectionCapture)
	{
		return;
	}

	UGroundInteractionWorldSubsystem* Subsystem = OwnerSubsystem.Get();

	if (!Subsystem || !Subsystem->AreAssetsReady())
	{
		return;
	}

	UGroundInteractionRendererComponent* RendererComponent = Subsystem->GetRendererComponent();

	if (!RendererComponent)
	{
		return;
	}

	RendererComponent->UpdateAndQueueDepthCapture_GameThread(InViewFamily, InView);
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
}
