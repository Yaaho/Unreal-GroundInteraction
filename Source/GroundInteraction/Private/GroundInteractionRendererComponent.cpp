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
	
	DepthUpdateAccumulatorSeconds = 0.0f;

	if (IsRegistered())
	{
		DestroyRenderProxy();
		CreateRenderProxy();
	}
	
	UpdateRenderProxyTransform();
}

void UGroundInteractionRendererComponent::SendRenderTransform_Concurrent()
{
	Super::SendRenderTransform_Concurrent();

	UpdateRenderProxyTransform();
}

void UGroundInteractionRendererComponent::UpdateRenderProxyTransform()
{
	if (!RenderProxy)
	{
		return;
	}

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();

	if (!Settings)
	{
		return;
	}

	const FIntPoint SafeRenderTargetSize = Settings->GetGroundInteractionRenderTargetSize();

	if (SafeRenderTargetSize.X <= 0 || SafeRenderTargetSize.Y <= 0)
	{
		return;
	}

	const FVector InteractionExtentWS = GroundInteractionCommon::MakeSafeInteractionExtent(Settings->GroundInteractionWorldExtent);
	const FVector RawCenterWS = GetComponentLocation();
	const FVector SnappedCenterWS = GroundInteractionCommon::SnapInteractionCenterWS(RawCenterWS, InteractionExtentWS, SafeRenderTargetSize, Settings->GroundInteractionLocationSnapCellCount);

	FGroundInteractionRendererProxy* ProxyForCommand = RenderProxy;

	ENQUEUE_RENDER_COMMAND(GroundInteractionUpdateRendererProxyLocation)(
		[ProxyForCommand, SnappedCenterWS](FRHICommandListImmediate& RHICmdList)
		{
			if (!ProxyForCommand)
			{
				return;
			}

			// 프록시에 snapped center 위치 설정
			ProxyForCommand->CurrentSnappedLocation = SnappedCenterWS;
		});

	const FVector BottomInteractionCenterWS = GroundInteractionCommon::GetBottomInteractionCenterWS(SnappedCenterWS, InteractionExtentWS);
	
	// MPC에 bottom interaction center 위치 설정
	UpdateMPC(BottomInteractionCenterWS, InteractionExtentWS);
}


void UGroundInteractionRendererComponent::UpdateGroundInteractionDepth_GameThread(FSceneViewFamily& InViewFamily)
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

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings || !Subsystem || !Subsystem->AreAssetsReady())
	{
		return;
	}

	const FIntPoint NewRenderTargetSize = Settings->GetGroundInteractionRenderTargetSize();

	bool bRenderTargetChanged = false;

	if (RenderTargetSize != NewRenderTargetSize)
	{
		RenderTargetSize = NewRenderTargetSize;
		ApplyOutputRenderTargets();

		bRenderTargetChanged = true;

		// RT size 변경 시 depth update accumulator 초기화
		DepthUpdateAccumulatorSeconds = 0.0f;
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

	const float DepthUpdateIntervalSeconds = Settings->GetDepthUpdateIntervalSeconds();

	const float DeltaTimeSeconds = FMath::Max(InViewFamily.Time.GetDeltaRealTimeSeconds(), 0.0f);

	DepthUpdateAccumulatorSeconds += DeltaTimeSeconds;

	if (DepthUpdateAccumulatorSeconds < DepthUpdateIntervalSeconds)
	{
		return;
	}

	const int32 DepthUpdateStepCount = FMath::Max(FMath::FloorToInt(DepthUpdateAccumulatorSeconds / DepthUpdateIntervalSeconds), 1);

	DepthUpdateAccumulatorSeconds -= static_cast<float>(DepthUpdateStepCount) * DepthUpdateIntervalSeconds;

	const FVector InteractionExtentWS = GroundInteractionCommon::MakeSafeInteractionExtent(Settings->GroundInteractionWorldExtent);

	const FVector RawCenterWS = GetComponentLocation();

	const FVector SnappedCenterWS = GroundInteractionCommon::SnapInteractionCenterWS(RawCenterWS, InteractionExtentWS, RenderTargetSize, Settings->GroundInteractionLocationSnapCellCount);
	
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

	const FVector BottomInteractionCenterWS = GroundInteractionCommon::GetBottomInteractionCenterWS(SnappedCenterWS, InteractionExtentWS);

	RenderProxy->QueueGroundInteractionDepthPass_GameThread(InViewFamily, ShowOnlyPrimitives, BottomInteractionCenterWS, RenderTargetSize);
}


void UGroundInteractionRendererComponent::UpdateMPC(const FVector& BottomInteractionCenterWS, const FVector& InteractionExtentWS) const
{
	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();

	if (!GetWorld() || !MaterialParameterCollection || !Settings)
	{
		return;
	}

	UKismetMaterialLibrary::SetVectorParameterValue(GetWorld(), MaterialParameterCollection, FName(TEXT("GI_OriginWS")), 
		FLinearColor(BottomInteractionCenterWS.X, BottomInteractionCenterWS.Y, BottomInteractionCenterWS.Z,0.0f));

	UKismetMaterialLibrary::SetVectorParameterValue(GetWorld(), MaterialParameterCollection, FName(TEXT("GI_ExtentWS")),
		FLinearColor(InteractionExtentWS.X, InteractionExtentWS.Y, InteractionExtentWS.Z, 0.0f));

	UKismetMaterialLibrary::SetScalarParameterValue(GetWorld(), MaterialParameterCollection, FName(TEXT("GI_Valid")), 1.0f);
	
	UKismetMaterialLibrary::SetScalarParameterValue(GetWorld(), MaterialParameterCollection, FName(TEXT("GI_FadeSpeed")), 
		FMath::Max(Settings->FadeRiseSpeed, 0.0f));
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
	if (!Settings)
	{
		return;
	}

	RenderTargetSize = Settings->GetGroundInteractionRenderTargetSize();

	ApplyOutputRenderTargets();

	const FVector InteractionExtent = GroundInteractionCommon::MakeSafeInteractionExtent(Settings->GroundInteractionWorldExtent);

	const float FadeRiseSpeed = Settings->FadeRiseSpeed;

	const float FadeMaxHeight = Settings->FadeMaxHeight;

	const float FadeUpdateIntervalSeconds = Settings->GetDepthFadeUpdateIntervalSeconds();

	const FIntVector NumCell(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1), 1);

	const FVector CellSpacing(
		FMath::Max(InteractionExtent.X, 1.0f) * 2.0f / float(NumCell.X),
		FMath::Max(InteractionExtent.Y, 1.0f) * 2.0f / float(NumCell.Y),
		FMath::Max(InteractionExtent.Z, 1.0f) * 2.0f / float(FMath::Max(NumCell.Z, 1)));

	RegisteredScene = World->Scene;

	RenderProxy = new FGroundInteractionRendererProxy(
		DepthRT->TextureReference.TextureReferenceRHI,
		FadeRT->TextureReference.TextureReferenceRHI,
		NumCell,
		CellSpacing,
		InteractionExtent,
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


