// Fill out your copyright notice in the Description page of Project Settings.

#include "GroundInteractionRendererComponent.h"
#include "GroundInteractionWorldSubsystem.h"
#include "GroundInteractionRendererProxy.h"
#include "GroundInteractionSettings.h"
#include "GroundInteractionCommon.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"

UGroundInteractionRendererComponent::UGroundInteractionRendererComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UGroundInteractionRendererComponent::OnRegister()
{
	Super::OnRegister();
	
	CreateRenderProxy();
}

void UGroundInteractionRendererComponent::OnUnregister()
{
	DestroyRenderProxy();

	Super::OnUnregister();
}
void UGroundInteractionRendererComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	DestroyRenderProxy();

	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UGroundInteractionRendererComponent::InitializeRenderer(
	UGroundInteractionWorldSubsystem* InSubsystem,
	UTextureRenderTarget2D* InDepthRT,
	UTextureRenderTarget2D* InFadeRT,
	UMaterialParameterCollection* InMaterialParameterCollection)
{
	check(IsInGameThread());

	OwnerSubsystem = InSubsystem;
	DepthRT = InDepthRT;
	FadeRT = InFadeRT;
	MaterialParameterCollection = InMaterialParameterCollection;

	RenderTargetSize = FIntPoint::ZeroValue;
	CurrentCaptureCenterWS = FVector::ZeroVector;

	if (IsRegistered())
	{
		DestroyRenderProxy();
		CreateRenderProxy();
	}
}

void UGroundInteractionRendererComponent::PrepareFrame_GameThread(
	UGroundInteractionWorldSubsystem* InSubsystem,
	FSceneViewFamily& InViewFamily)
{
	check(IsInGameThread());

	if (!RenderProxy)
	{
		return;
	}

	UGroundInteractionWorldSubsystem* Subsystem =
		InSubsystem ? InSubsystem : OwnerSubsystem.Get();

	if (!Subsystem ||
		!Subsystem->AreAssetsReady())
	{
		return;
	}

	const UGroundInteractionSettings* Settings =
		UGroundInteractionSettings::Get();

	if (!Settings)
	{
		return;
	}

	const FIntPoint NewRenderTargetSize =
		Settings->GetRenderTargetSize();

	bool bRenderTargetChanged = false;

	if (RenderTargetSize != NewRenderTargetSize)
	{
		RenderTargetSize = NewRenderTargetSize;
		ApplyOutputRenderTargets();

		bRenderTargetChanged = true;
	}

	if (RenderTargetSize.X <= 0 ||
		RenderTargetSize.Y <= 0)
	{
		return;
	}

	FVector RawCameraLocationWS = FVector::ZeroVector;

	if (!GetPlayerCameraLocation(RawCameraLocationWS))
	{
		return;
	}

	const FVector CaptureExtentWS =
		GroundInteractionCommon::MakeSafeCaptureExtent(
			Settings->CaptureWorldExtent);

	const FVector SnappedCaptureCenterWS =
		GroundInteractionCommon::SnapCaptureCenterWS(
			RawCameraLocationWS,
			CaptureExtentWS,
			RenderTargetSize);

	const bool bCaptureCenterChanged =
		!CurrentCaptureCenterWS.Equals(
			SnappedCaptureCenterWS,
			KINDA_SMALL_NUMBER);

	// Center Location, Render Target Size 중 아무것도 바뀌지 않았으면 return
	if (!bRenderTargetChanged &&
		!bCaptureCenterChanged)
	{
		return;
	}
	
	CurrentCaptureCenterWS = SnappedCaptureCenterWS;

	TSet<FPrimitiveComponentId> ShowOnlyPrimitives;
	
	// 이 렌더 컴포넌트의 바운드 안에 있는 primitive 목록 컬링
	const TArray<TWeakObjectPtr<UPrimitiveComponent>>& InteractionPrimitives =
		Subsystem->GetInteractionPrimitives();

	const FBox CaptureCullBox(
		CurrentCaptureCenterWS - CaptureExtentWS,
		CurrentCaptureCenterWS + CaptureExtentWS);

	for (const TWeakObjectPtr<UPrimitiveComponent>& PrimitiveWeak :
		InteractionPrimitives)
	{
		UPrimitiveComponent* Primitive =
			PrimitiveWeak.Get();

		if (!IsValid(Primitive) ||
			!Primitive->IsRegistered())
		{
			continue;
		}

		if (Primitive->Bounds.GetBox().Intersect(CaptureCullBox))
		{
			ShowOnlyPrimitives.Add(
				Primitive->GetPrimitiveSceneId());
		}
	}

	FGroundInteractionRendererProxy* ProxyForCommand =
		RenderProxy;

	const FVector CaptureCenterForCommand =
		CurrentCaptureCenterWS;

	const FIntPoint RenderTargetSizeForCommand =
		RenderTargetSize;

	const bool bRenderTargetChangedForCommand =
		bRenderTargetChanged;

	TSet<FPrimitiveComponentId> ShowOnlyPrimitivesForCommand =
		MoveTemp(ShowOnlyPrimitives);

	ENQUEUE_RENDER_COMMAND(GroundInteractionUpdateRendererProxyFrame)(
		[
			ProxyForCommand,
			CaptureCenterForCommand,
			RenderTargetSizeForCommand,
			bRenderTargetChangedForCommand,
			ShowOnlyPrimitivesForCommand = MoveTemp(ShowOnlyPrimitivesForCommand)
		](FRHICommandListImmediate& RHICmdList) mutable
		{
			if (!ProxyForCommand)
			{
				return;
			}

			ProxyForCommand->CurrentSnappedLocation =
				CaptureCenterForCommand;

			ProxyForCommand->RenderTargetSize =
				RenderTargetSizeForCommand;

			ProxyForCommand->ShowOnlyPrimitives =
				MoveTemp(ShowOnlyPrimitivesForCommand);

			if (bRenderTargetChangedForCommand)
			{
				ProxyForCommand->bRenderTargetChanged = true;
			}
		});
}


void UGroundInteractionRendererComponent::UpdateMPC_GameThread(
	const FVector& CaptureOriginWS,
	const FVector& CaptureExtentWS) const
{
	check(IsInGameThread());

	if (!GetWorld() ||
		!MaterialParameterCollection)
	{
		return;
	}

	UKismetMaterialLibrary::SetVectorParameterValue(
		GetWorld(),
		MaterialParameterCollection,
		FName(TEXT("GI_CaptureOriginWS")),
		FLinearColor(
			CaptureOriginWS.X,
			CaptureOriginWS.Y,
			CaptureOriginWS.Z,
			0.0f));

	UKismetMaterialLibrary::SetVectorParameterValue(
		GetWorld(),
		MaterialParameterCollection,
		FName(TEXT("GI_CaptureExtentWS")),
		FLinearColor(
			CaptureExtentWS.X,
			CaptureExtentWS.Y,
			CaptureExtentWS.Z,
			0.0f));

	UKismetMaterialLibrary::SetScalarParameterValue(
		GetWorld(),
		MaterialParameterCollection,
		FName(TEXT("GI_CaptureValid")),
		1.0f);
}

void UGroundInteractionRendererComponent::CreateRenderProxy()
{
	check(IsInGameThread());

	if (RenderProxy)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World ||
		!World->Scene)
	{
		return;
	}

	UGroundInteractionWorldSubsystem* Subsystem =
		OwnerSubsystem.Get();

	if (!Subsystem)
	{
		Subsystem =
			World->GetSubsystem<UGroundInteractionWorldSubsystem>();

		OwnerSubsystem = Subsystem;
	}

	if (!Subsystem ||
		!Subsystem->AreAssetsReady())
	{
		return;
	}

	if (!DepthRT)
	{
		DepthRT = Subsystem->GetDepthRT();
	}

	if (!FadeRT)
	{
		FadeRT = Subsystem->GetFadeRT();
	}

	if (!MaterialParameterCollection)
	{
		MaterialParameterCollection =
			Subsystem->GetMaterialParameterCollection();
	}

	if (!DepthRT ||
		!FadeRT ||
		!MaterialParameterCollection)
	{
		return;
	}

	const UGroundInteractionSettings* Settings =
		UGroundInteractionSettings::Get();

	RenderTargetSize = Settings
		? Settings->GetRenderTargetSize()
		: FIntPoint(512, 512);

	ApplyOutputRenderTargets();

	const FVector CaptureExtent = Settings
		? GroundInteractionCommon::MakeSafeCaptureExtent(
			Settings->CaptureWorldExtent)
		: FVector(1000.0f, 1000.0f, 1000.0f);

	const float FadeRiseSpeed = Settings
		? Settings->FadeRiseSpeed
		: 150.0f;

	const float FadeMaxHeight = Settings
		? Settings->FadeMaxHeight
		: 120.0f;

	const float FadeUpdateIntervalSeconds = Settings
		? Settings->GetDepthFadeUpdateIntervalSeconds()
		: 1.0f / 60.0f;

	const float CaptureUpdateIntervalSeconds = Settings
		? Settings->GetDepthCaptureUpdateIntervalSeconds()
		: 1.0f / 60.0f;

	const FIntVector NumCell(
		FMath::Max(RenderTargetSize.X, 1),
		FMath::Max(RenderTargetSize.Y, 1),
		1);

	const FVector CellSpacing(
		FMath::Max(CaptureExtent.X, 1.0f) * 2.0f / float(NumCell.X),
		FMath::Max(CaptureExtent.Y, 1.0f) * 2.0f / float(NumCell.Y),
		FMath::Max(CaptureExtent.Z, 1.0f) * 2.0f / float(FMath::Max(NumCell.Z, 1)));

	RegisteredScene = World->Scene;

	RenderProxy = new FGroundInteractionRendererProxy(
		this,
		DepthRT->TextureReference.TextureReferenceRHI,
		FadeRT->TextureReference.TextureReferenceRHI,
		NumCell,
		CellSpacing,
		CaptureExtent,
		FadeRiseSpeed,
		FadeMaxHeight,
		FadeUpdateIntervalSeconds,
		CaptureUpdateIntervalSeconds);

	FGroundInteractionRendererProxy* ProxyForCommand =
		RenderProxy;

	FSceneInterface* SceneForCommand =
		RegisteredScene;
	
	ENQUEUE_RENDER_COMMAND(GroundInteractionRegisterRendererProxy)(
		[SceneForCommand, ProxyForCommand](FRHICommandListImmediate& RHICmdList)
		{
			FGroundInteractionRendererProxy::RegisterProxy(
				SceneForCommand,
				ProxyForCommand);
		});
	
	CurrentCaptureCenterWS = FVector(
		TNumericLimits<double>::Max(),
		TNumericLimits<double>::Max(),
		TNumericLimits<double>::Max());
}
void UGroundInteractionRendererComponent::DestroyRenderProxy()
{
	check(IsInGameThread());

	if (!RenderProxy)
	{
		return;
	}

	FGroundInteractionRendererProxy* ProxyForCommand =
		RenderProxy;

	FSceneInterface* SceneForCommand =
		RegisteredScene;

	RenderProxy = nullptr;
	RegisteredScene = nullptr;
	
	if (SceneForCommand)
	{
		ENQUEUE_RENDER_COMMAND(GroundInteractionUnregisterRendererProxy)(
			[SceneForCommand, ProxyForCommand](FRHICommandListImmediate& RHICmdList)
			{
				ProxyForCommand->Release();
				
				FGroundInteractionRendererProxy::UnregisterProxy(
					SceneForCommand,
					ProxyForCommand);
			});
	}

	BeginCleanup(ProxyForCommand);
}

void UGroundInteractionRendererComponent::ApplyOutputRenderTargets()
{
	check(IsInGameThread());

	if (!DepthRT ||
		!FadeRT)
	{
		return;
	}

	InitOutputRenderTarget(
		DepthRT,
		RenderTargetSize,
		FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));

	InitOutputRenderTarget(
		FadeRT,
		RenderTargetSize,
		FLinearColor(-32768.0f, 0.0f, 0.0f, 0.0f));
}

bool UGroundInteractionRendererComponent::InitOutputRenderTarget(
	UTextureRenderTarget2D* RenderTarget,
	const FIntPoint& Size,
	const FLinearColor& ClearColor) const
{
	check(IsInGameThread());

	if (!RenderTarget)
	{
		return false;
	}

	const FIntPoint SafeSize(
		FMath::Max(Size.X, 16),
		FMath::Max(Size.Y, 16));

	const bool bNeedsInit =
		RenderTarget->SizeX != SafeSize.X ||
		RenderTarget->SizeY != SafeSize.Y ||
		RenderTarget->RenderTargetFormat != RTF_R16f ||
		RenderTarget->SRGB ||
		RenderTarget->bAutoGenerateMips ||
		!RenderTarget->bCanCreateUAV ||
		RenderTarget->AddressX != TA_Clamp ||
		RenderTarget->AddressY != TA_Clamp ||
		RenderTarget->Filter != TF_Nearest ||
		!RenderTarget->ClearColor.Equals(ClearColor);

	if (!bNeedsInit)
	{
		return false;
	}

	RenderTarget->RenderTargetFormat = RTF_R16f;
	RenderTarget->ClearColor = ClearColor;
	RenderTarget->SRGB = false;
	RenderTarget->bAutoGenerateMips = false;
	RenderTarget->bCanCreateUAV = true;
	RenderTarget->AddressX = TA_Clamp;
	RenderTarget->AddressY = TA_Clamp;
	RenderTarget->Filter = TF_Nearest;
	RenderTarget->InitAutoFormat(SafeSize.X, SafeSize.Y);
	RenderTarget->UpdateResourceImmediate(true);

	return true;
}

bool UGroundInteractionRendererComponent::GetPlayerCameraLocation(
	FVector& OutCameraLocationWS) const
{
	check(IsInGameThread());

	UWorld* World = GetWorld();
	if (!World ||
		World->IsNetMode(NM_DedicatedServer))
	{
		return false;
	}

	APlayerController* PlayerController =
		World->GetFirstPlayerController();

	if (!PlayerController ||
		!PlayerController->PlayerCameraManager)
	{
		return false;
	}

	OutCameraLocationWS =
		PlayerController->PlayerCameraManager->GetCameraLocation();

	return true;
}