#include "WaterInteractionRendererComponent.h"
#include "GroundInteractionCommon.h"
#include "GroundInteractionSettings.h"
#include "GroundInteractionWorldSubsystem.h"
#include "WaterInteractionCommon.h"
#include "WaterInteractionRendererProxy.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "RenderingThread.h"
#include "SceneView.h"

UWaterInteractionRendererComponent::UWaterInteractionRendererComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UWaterInteractionRendererComponent::OnRegister()
{
	Super::OnRegister();

	CreateRenderProxy();
}

void UWaterInteractionRendererComponent::OnUnregister()
{
	DestroyRenderProxy();

	Super::OnUnregister();
}

void UWaterInteractionRendererComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	DestroyRenderProxy();

	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UWaterInteractionRendererComponent::SendRenderTransform_Concurrent()
{
	Super::SendRenderTransform_Concurrent();

	UpdateRenderProxyTransform();
}

void UWaterInteractionRendererComponent::UpdateRenderProxyTransform()
{
	if (!RenderProxy)
	{
		return;
	}

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();

	if (!Settings || !Settings->bEnableWaterInteraction)
	{
		return;
	}

	const FIntPoint SafeRenderTargetSize = Settings->GetWaterInteractionRenderTargetSize();

	if (SafeRenderTargetSize.X <= 0 || SafeRenderTargetSize.Y <= 0)
	{
		return;
	}

	const FVector InteractionExtentWS = GroundInteractionCommon::MakeSafeInteractionExtent(Settings->WaterInteractionWorldExtent);
	const FVector RawCenterWS = GetComponentLocation();
	const FVector SnappedCenterWS = GroundInteractionCommon::SnapInteractionCenterWS(RawCenterWS, InteractionExtentWS, SafeRenderTargetSize, Settings->WaterInteractionLocationSnapCellCount);

	FWaterInteractionRendererProxy* ProxyForCommand = RenderProxy;

	ENQUEUE_RENDER_COMMAND(WaterInteractionUpdateRendererProxyLocation)(
		[ProxyForCommand, SnappedCenterWS](FRHICommandListImmediate& RHICmdList)
		{
			if (!ProxyForCommand)
			{
				return;
			}

			ProxyForCommand->CurrentSnappedLocation = SnappedCenterWS;
		});

	const FVector BottomInteractionCenterWS = GroundInteractionCommon::GetBottomInteractionCenterWS(SnappedCenterWS, InteractionExtentWS);

	UpdateMPC(BottomInteractionCenterWS, InteractionExtentWS);
}

void UWaterInteractionRendererComponent::InitializeRenderer(
	UGroundInteractionWorldSubsystem* InSubsystem,
	UTextureRenderTarget2D* InHeightRT,
	UMaterialParameterCollection* InMaterialParameterCollection)
{
	OwnerSubsystem = InSubsystem;
	HeightRT = InHeightRT;
	MaterialParameterCollection = InMaterialParameterCollection;

	RenderTargetSize = FIntPoint::ZeroValue;
	UpdateAccumulatorSeconds = 0.0f;

	if (IsRegistered())
	{
		DestroyRenderProxy();
		CreateRenderProxy();
	}

	UpdateRenderProxyTransform();
}

void UWaterInteractionRendererComponent::UpdateWaterInteraction_GameThread(FSceneViewFamily& InViewFamily)
{
	check(IsInGameThread());

	if (!RenderProxy)
	{
		return;
	}

	UGroundInteractionWorldSubsystem* Subsystem = OwnerSubsystem.Get();

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings || !Settings->bEnableWaterInteraction || !Subsystem || !Subsystem->AreWaterAssetsReady())
	{
		return;
	}

	const FIntPoint NewRenderTargetSize = Settings->GetWaterInteractionRenderTargetSize();
	bool bRenderTargetChanged = false;

	if (RenderTargetSize != NewRenderTargetSize)
	{
		RenderTargetSize = NewRenderTargetSize;
		ApplyOutputRenderTarget();
		bRenderTargetChanged = true;
		UpdateAccumulatorSeconds = 0.0f;
	}

	if (RenderTargetSize.X <= 0 || RenderTargetSize.Y <= 0)
	{
		return;
	}

	if (bRenderTargetChanged)
	{
		FWaterInteractionRendererProxy* ProxyForCommand = RenderProxy;
		const FIntPoint RenderTargetSizeForCommand = RenderTargetSize;

		ENQUEUE_RENDER_COMMAND(WaterInteractionUpdateRendererProxyRenderTargetSize)(
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

	if (Settings->WaterInteractionPulseStrength <= 0.0f)
	{
		return;
	}

	const float UpdateIntervalSeconds = Settings->GetWaterInteractionUpdateIntervalSeconds();
	const float DeltaTimeSeconds = FMath::Max(InViewFamily.Time.GetDeltaRealTimeSeconds(), 0.0f);

	UpdateAccumulatorSeconds += DeltaTimeSeconds;
	if (UpdateAccumulatorSeconds < UpdateIntervalSeconds)
	{
		return;
	}

	const int32 UpdateStepCount = FMath::Max(FMath::FloorToInt(UpdateAccumulatorSeconds / UpdateIntervalSeconds), 1);
	UpdateAccumulatorSeconds -= static_cast<float>(UpdateStepCount) * UpdateIntervalSeconds;

	const FVector InteractionExtentWS = GroundInteractionCommon::MakeSafeInteractionExtent(Settings->WaterInteractionWorldExtent);
	const FVector SnappedCenterWS = GroundInteractionCommon::SnapInteractionCenterWS(GetComponentLocation(), InteractionExtentWS, RenderTargetSize, Settings->WaterInteractionLocationSnapCellCount);

	const FBox InteractionBoundsWS = FBox::BuildAABB(SnappedCenterWS, InteractionExtentWS);
	TArray<FWaterSplatWorld> WaterSplats = Subsystem->GenerateWaterSplats();

	WaterSplats.RemoveAll([&InteractionBoundsWS](const FWaterSplatWorld& WaterSplat)
	{
		const double AbsRotationCos = FMath::Abs(static_cast<double>(WaterSplat.RotationCos));
		const double AbsRotationSin = FMath::Abs(static_cast<double>(WaterSplat.RotationSin));
		const FVector2d AxisAlignedHalfSize(AbsRotationCos * WaterSplat.HalfSizeXY.X + AbsRotationSin * WaterSplat.HalfSizeXY.Y, AbsRotationSin * WaterSplat.HalfSizeXY.X + AbsRotationCos * WaterSplat.HalfSizeXY.Y);
		const FVector2d SplatMin = WaterSplat.AbsoluteCenterXY - AxisAlignedHalfSize;
		const FVector2d SplatMax = WaterSplat.AbsoluteCenterXY + AxisAlignedHalfSize;

		return SplatMax.X < InteractionBoundsWS.Min.X || SplatMin.X > InteractionBoundsWS.Max.X || SplatMax.Y < InteractionBoundsWS.Min.Y || SplatMin.Y > InteractionBoundsWS.Max.Y;
	});

	if (WaterSplats.IsEmpty())
	{
		return;
	}

	FWaterInteractionRendererProxy* ProxyForCommand = RenderProxy;
	ENQUEUE_RENDER_COMMAND(WaterInteractionAppendWaterSplats)(
		[ProxyForCommand, WaterSplatsForCommand = MoveTemp(WaterSplats)](FRHICommandListImmediate& RHICmdList) mutable
		{
			if (!ProxyForCommand)
			{
				return;
			}
			
			ProxyForCommand->PendingWaterSplats.Append(MoveTemp(WaterSplatsForCommand));
		});
}

void UWaterInteractionRendererComponent::CreateRenderProxy()
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

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings || !Settings->bEnableWaterInteraction)
	{
		return;
	}

	if (!HeightRT || !MaterialParameterCollection)
	{
		return;
	}

	RenderTargetSize = Settings->GetWaterInteractionRenderTargetSize();
	ApplyOutputRenderTarget();

	const FVector InteractionExtent = GroundInteractionCommon::MakeSafeInteractionExtent(Settings->WaterInteractionWorldExtent);
	const FIntVector NumCell(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1), 1);
	const FVector CellSpacing(FMath::Max(InteractionExtent.X, 1.0f) * 2.0f / float(NumCell.X), FMath::Max(InteractionExtent.Y, 1.0f) * 2.0f / float(NumCell.Y), FMath::Max(InteractionExtent.Z, 1.0f) * 2.0f / float(FMath::Max(NumCell.Z, 1)));

	RegisteredScene = World->Scene;
	RenderProxy = new FWaterInteractionRendererProxy(HeightRT->TextureReference.TextureReferenceRHI, NumCell, CellSpacing, InteractionExtent, Settings->WaterInteractionPulseStrength);

	FWaterInteractionRendererProxy* ProxyForCommand = RenderProxy;
	FSceneInterface* SceneForCommand = RegisteredScene;

	ENQUEUE_RENDER_COMMAND(WaterInteractionRegisterRendererProxy)(
		[SceneForCommand, ProxyForCommand](FRHICommandListImmediate& RHICmdList)
		{
			FWaterInteractionRendererProxy::RegisterProxy(SceneForCommand, ProxyForCommand);
		});
}

void UWaterInteractionRendererComponent::DestroyRenderProxy()
{
	if (!RenderProxy)
	{
		return;
	}

	FWaterInteractionRendererProxy* ProxyForCommand = RenderProxy;
	FSceneInterface* SceneForCommand = RegisteredScene;

	RenderProxy = nullptr;
	RegisteredScene = nullptr;

	if (SceneForCommand)
	{
		ENQUEUE_RENDER_COMMAND(WaterInteractionUnregisterRendererProxy)(
			[SceneForCommand, ProxyForCommand](FRHICommandListImmediate& RHICmdList)
			{
				ProxyForCommand->Release();
				FWaterInteractionRendererProxy::UnregisterProxy(SceneForCommand, ProxyForCommand);
			});
	}

	BeginCleanup(ProxyForCommand);
}

void UWaterInteractionRendererComponent::UpdateMPC(const FVector& BottomInteractionCenterWS, const FVector& InteractionExtentWS) const
{
	if (!GetWorld() || !MaterialParameterCollection)
	{
		return;
	}

	UKismetMaterialLibrary::SetVectorParameterValue(GetWorld(), MaterialParameterCollection, FName(TEXT("WI_OriginWS")), FLinearColor(BottomInteractionCenterWS.X, BottomInteractionCenterWS.Y, BottomInteractionCenterWS.Z, 0.0f));
	UKismetMaterialLibrary::SetVectorParameterValue(GetWorld(), MaterialParameterCollection, FName(TEXT("WI_ExtentWS")), FLinearColor(InteractionExtentWS.X, InteractionExtentWS.Y, InteractionExtentWS.Z, 0.0f));

	UKismetMaterialLibrary::SetScalarParameterValue(GetWorld(), MaterialParameterCollection, FName(TEXT("WI_Valid")), 1.0f);
}

void UWaterInteractionRendererComponent::ApplyOutputRenderTarget()
{
	if (!HeightRT)
	{
		return;
	}

	InitOutputRenderTarget(HeightRT, RenderTargetSize, FLinearColor::Black);
}

bool UWaterInteractionRendererComponent::InitOutputRenderTarget(UTextureRenderTarget2D* RenderTarget, const FIntPoint& Size, const FLinearColor& ClearColor) const
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
