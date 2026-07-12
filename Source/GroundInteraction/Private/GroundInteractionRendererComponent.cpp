// Fill out your copyright notice in the Description page of Project Settings.

#include "GroundInteractionRendererComponent.h"
#include "GroundInteractionWorldSubsystem.h"
#include "GroundInteractionRendererProxy.h"
#include "GroundInteractionSettings.h"
#include "GroundInteractionCommon.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "RenderingThread.h"
#include "SceneView.h"

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

void UGroundInteractionRendererComponent::InitializeRenderer(UGroundInteractionWorldSubsystem* InSubsystem,
	UTextureRenderTarget2D* InDepthRT, UTextureRenderTarget2D* InFadeRT, UMaterialParameterCollection* InMaterialParameterCollection)
{
	OwnerSubsystem = InSubsystem;
	DepthRT = InDepthRT;
	FadeRT = InFadeRT;
	MaterialParameterCollection = InMaterialParameterCollection;

	RenderTargetSize = FIntPoint::ZeroValue;
	
	DepthCaptureUpdateAccumulatorSeconds = 0.0f;

	if (IsRegistered())
	{
		DestroyRenderProxy();
		CreateRenderProxy();
	}
	
	SendRenderTransform_Concurrent();
}

void UGroundInteractionRendererComponent::SendRenderTransform_Concurrent()
{
	Super::SendRenderTransform_Concurrent();

	if (!RenderProxy)
	{
		return;
	}

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();

	if (!Settings)
	{
		return;
	}

	const FIntPoint SafeRenderTargetSize = Settings->GetRenderTargetSize();

	if (SafeRenderTargetSize.X <= 0 || SafeRenderTargetSize.Y <= 0)
	{
		return;
	}

	const FVector CaptureExtentWS = GroundInteractionCommon::MakeSafeCaptureExtent(Settings->CaptureWorldExtent);

	const FVector RawCaptureCenterWS = GetComponentLocation();

	const FVector SnappedCaptureCenterWS = GroundInteractionCommon::SnapCaptureCenterWS(RawCaptureCenterWS, CaptureExtentWS, SafeRenderTargetSize);

	FGroundInteractionRendererProxy* ProxyForCommand = RenderProxy;

	ENQUEUE_RENDER_COMMAND(GroundInteractionUpdateRendererProxyLocation)(
		[ProxyForCommand, SnappedCaptureCenterWS](FRHICommandListImmediate& RHICmdList)
		{
			if (!ProxyForCommand)
			{
				return;
			}

			// 프록시에 Snapped Center 위치 설정
			ProxyForCommand->CurrentSnappedLocation = SnappedCaptureCenterWS;
		});

	const FVector CaptureOriginWS = GroundInteractionCommon::GetCaptureOriginWS(SnappedCaptureCenterWS, CaptureExtentWS);
	
	// MPC 에 Snapped Capture Origin 위치 설정
	UpdateMPC(CaptureOriginWS, CaptureExtentWS);
}


void UGroundInteractionRendererComponent::UpdateDepthCapture_GameThread(FSceneViewFamily& InViewFamily)
{
	check(IsInGameThread());

	if (!RenderProxy)
	{
		return;
	}

	UGroundInteractionWorldSubsystem* Subsystem = OwnerSubsystem.Get();

	if (!Subsystem)
	{
		if (UWorld* World = GetWorld())
		{
			Subsystem = World->GetSubsystem<UGroundInteractionWorldSubsystem>();

			OwnerSubsystem = Subsystem;
		}
	}

	if (!Subsystem || !Subsystem->AreAssetsReady())
	{
		return;
	}

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();

	if (!Settings)
	{
		return;
	}

	const FIntPoint NewRenderTargetSize = Settings->GetRenderTargetSize();

	bool bRenderTargetChanged = false;

	if (RenderTargetSize != NewRenderTargetSize)
	{
		RenderTargetSize = NewRenderTargetSize;
		ApplyOutputRenderTargets();

		bRenderTargetChanged = true;

		// RT size 변경 시 capture accumulator 초기화
		DepthCaptureUpdateAccumulatorSeconds = 0.0f;

		SendRenderTransform_Concurrent();
	}

	if (RenderTargetSize.X <= 0 || RenderTargetSize.Y <= 0)
	{
		return;
	}

	if (bRenderTargetChanged)
	{
		FGroundInteractionRendererProxy* ProxyForCommand = RenderProxy;

		const FIntPoint RenderTargetSizeForCommand = RenderTargetSize;

		ENQUEUE_RENDER_COMMAND(GroundInteractionUpdateRendererProxyRenderTargetSize)(
			[ProxyForCommand, RenderTargetSizeForCommand](FRHICommandListImmediate& RHICmdList)
			{
				if (!ProxyForCommand)
				{
					return;
				}
				
				ProxyForCommand->RenderTargetSize = RenderTargetSizeForCommand;
				ProxyForCommand->bRenderTargetChanged = true;
			});
	}

	const float CaptureUpdateIntervalSeconds = Settings->GetDepthCaptureUpdateIntervalSeconds();

	const float DeltaTimeSeconds = FMath::Max(InViewFamily.Time.GetDeltaRealTimeSeconds(), 0.0f);

	DepthCaptureUpdateAccumulatorSeconds += DeltaTimeSeconds;

	if (DepthCaptureUpdateAccumulatorSeconds < CaptureUpdateIntervalSeconds)
	{
		return;
	}

	const int32 CaptureStepCount = FMath::Max(FMath::FloorToInt(DepthCaptureUpdateAccumulatorSeconds / CaptureUpdateIntervalSeconds), 1);

	DepthCaptureUpdateAccumulatorSeconds -= static_cast<float>(CaptureStepCount) * CaptureUpdateIntervalSeconds;

	const FVector CaptureExtentWS = GroundInteractionCommon::MakeSafeCaptureExtent(Settings->CaptureWorldExtent);

	const FVector RawCaptureCenterWS = GetComponentLocation();

	const FVector SnappedCaptureCenterWS = GroundInteractionCommon::SnapCaptureCenterWS(RawCaptureCenterWS, CaptureExtentWS, RenderTargetSize);
	
	TSet<FPrimitiveComponentId> ShowOnlyPrimitives;

	const TArray<TWeakObjectPtr<UPrimitiveComponent>>& InteractionPrimitives = Subsystem->GetInteractionPrimitives();

	for (const TWeakObjectPtr<UPrimitiveComponent>& PrimitiveWeak : InteractionPrimitives)
	{
		UPrimitiveComponent* Primitive = PrimitiveWeak.Get();

		if (!IsValid(Primitive) || !Primitive->IsRegistered())
		{
			continue;
		}

		ShowOnlyPrimitives.Add(Primitive->GetPrimitiveSceneId());
	}

	if (ShowOnlyPrimitives.Num() <= 0)
	{
		return;
	}

	const FVector CaptureViewLocation = GroundInteractionCommon::GetCaptureOriginWS(SnappedCaptureCenterWS, CaptureExtentWS);

	RenderProxy->QueueDepthCapture_GameThread(InViewFamily, ShowOnlyPrimitives, CaptureViewLocation, RenderTargetSize);
}


void UGroundInteractionRendererComponent::UpdateMPC(const FVector& CaptureOriginWS, const FVector& CaptureExtentWS) const
{
	if (!GetWorld() || !MaterialParameterCollection)
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
	if (RenderProxy)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || !World->Scene)
	{
		return;
	}

	UGroundInteractionWorldSubsystem* Subsystem = OwnerSubsystem.Get();

	if (!Subsystem)
	{
		Subsystem = World->GetSubsystem<UGroundInteractionWorldSubsystem>();

		OwnerSubsystem = Subsystem;
	}

	if (!Subsystem || !Subsystem->AreAssetsReady())
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
		MaterialParameterCollection = Subsystem->GetMaterialParameterCollection();
	}

	if (!DepthRT || !FadeRT || !MaterialParameterCollection)
	{
		return;
	}

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();

	RenderTargetSize = Settings ? Settings->GetRenderTargetSize() : FIntPoint(512, 512);

	ApplyOutputRenderTargets();

	const FVector CaptureExtent = Settings ? GroundInteractionCommon::MakeSafeCaptureExtent(Settings->CaptureWorldExtent) : FVector(1000.0f, 1000.0f, 1000.0f);

	const float FadeRiseSpeed = Settings ? Settings->FadeRiseSpeed : 150.0f;

	const float FadeMaxHeight = Settings ? Settings->FadeMaxHeight : 120.0f;

	const float FadeUpdateIntervalSeconds = Settings ? Settings->GetDepthFadeUpdateIntervalSeconds() : 1.0f / 60.0f;

	const FIntVector NumCell(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1), 1);

	const FVector CellSpacing(
		FMath::Max(CaptureExtent.X, 1.0f) * 2.0f / float(NumCell.X),
		FMath::Max(CaptureExtent.Y, 1.0f) * 2.0f / float(NumCell.Y),
		FMath::Max(CaptureExtent.Z, 1.0f) * 2.0f / float(FMath::Max(NumCell.Z, 1)));

	RegisteredScene = World->Scene;

	RenderProxy = new FGroundInteractionRendererProxy(
		DepthRT->TextureReference.TextureReferenceRHI,
		FadeRT->TextureReference.TextureReferenceRHI,
		NumCell,
		CellSpacing,
		CaptureExtent,
		FadeRiseSpeed,
		FadeMaxHeight,
		FadeUpdateIntervalSeconds);

	FGroundInteractionRendererProxy* ProxyForCommand = RenderProxy;

	FSceneInterface* SceneForCommand = RegisteredScene;
	
	ENQUEUE_RENDER_COMMAND(GroundInteractionRegisterRendererProxy)(
		[SceneForCommand, ProxyForCommand](FRHICommandListImmediate& RHICmdList)
		{
			FGroundInteractionRendererProxy::RegisterProxy(SceneForCommand, ProxyForCommand);
		});
}


void UGroundInteractionRendererComponent::DestroyRenderProxy()
{
	if (!RenderProxy)
	{
		return;
	}

	FGroundInteractionRendererProxy* ProxyForCommand = RenderProxy;
	FSceneInterface* SceneForCommand = RegisteredScene;

	RenderProxy = nullptr;
	RegisteredScene = nullptr;
	
	if (SceneForCommand)
	{
		ENQUEUE_RENDER_COMMAND(GroundInteractionUnregisterRendererProxy)(
			[SceneForCommand, ProxyForCommand](FRHICommandListImmediate& RHICmdList)
			{
				ProxyForCommand->Release();
				
				FGroundInteractionRendererProxy::UnregisterProxy(SceneForCommand, ProxyForCommand);
			});
	}

	BeginCleanup(ProxyForCommand);
}

void UGroundInteractionRendererComponent::ApplyOutputRenderTargets()
{
	if (!DepthRT || !FadeRT)
	{
		return;
	}

	InitOutputRenderTarget(DepthRT, RenderTargetSize, FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
	InitOutputRenderTarget(FadeRT, RenderTargetSize, FLinearColor(-32768.0f, 0.0f, 0.0f, 0.0f));
}

bool UGroundInteractionRendererComponent::InitOutputRenderTarget(UTextureRenderTarget2D* RenderTarget, const FIntPoint& Size, const FLinearColor& ClearColor) const
{
	if (!RenderTarget)
	{
		return false;
	}

	const FIntPoint SafeSize(FMath::Max(Size.X, 16), FMath::Max(Size.Y, 16));

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
