#include "WaterInteractionRendererComponent.h"
#include "GroundInteractionCommon.h"
#include "GroundInteractionWorldSubsystem.h"
#include "WaterInteractionRendererProxy.h"
#include "GroundInteractionSettings.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "RenderingThread.h"

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

	const bool bRenderTargetSizeChanged = RenderTargetSize != SafeRenderTargetSize;

	if (bRenderTargetSizeChanged)
	{
		RenderTargetSize = SafeRenderTargetSize;
		ApplyOutputRenderTarget();
	}

	const FVector InteractionExtentWS = GroundInteractionCommon::MakeSafeInteractionExtent(Settings->WaterInteractionWorldExtent);
	const FVector RawCenterWS = GetComponentLocation();
	const FVector SnappedCenterWS = GroundInteractionCommon::SnapInteractionCenterWS(RawCenterWS, InteractionExtentWS, SafeRenderTargetSize, Settings->WaterInteractionLocationSnapCellCount);

	FWaterInteractionRendererProxy* ProxyForCommand = RenderProxy;

	ENQUEUE_RENDER_COMMAND(WaterInteractionUpdateRendererProxyLocation)(
		[ProxyForCommand, SnappedCenterWS, SafeRenderTargetSize, bRenderTargetSizeChanged](FRHICommandListImmediate& RHICmdList)
		{
			if (!ProxyForCommand)
			{
				return;
			}

			ProxyForCommand->CurrentSnappedLocation = SnappedCenterWS;
			ProxyForCommand->RenderTargetSize = SafeRenderTargetSize;
			ProxyForCommand->bRenderTargetChanged |= bRenderTargetSizeChanged;
		});

	const FVector BottomInteractionCenterWS = GroundInteractionCommon::GetBottomInteractionCenterWS(SnappedCenterWS, InteractionExtentWS);

	UpdateMPC(BottomInteractionCenterWS, InteractionExtentWS);
}

void UWaterInteractionRendererComponent::InitializeRenderer(UGroundInteractionWorldSubsystem* InSubsystem,
	UTextureRenderTarget2D* InHeightRT, UMaterialParameterCollection* InMaterialParameterCollection)
{
	OwnerSubsystem = InSubsystem;
	HeightRT = InHeightRT;
	MaterialParameterCollection = InMaterialParameterCollection;

	RenderTargetSize = FIntPoint::ZeroValue;

	if (IsRegistered())
	{
		DestroyRenderProxy();
		CreateRenderProxy();
	}

	SendRenderTransform_Concurrent();
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

	UGroundInteractionWorldSubsystem* Subsystem = OwnerSubsystem.Get();

	if (!Subsystem)
	{
		Subsystem = World->GetSubsystem<UGroundInteractionWorldSubsystem>();

		OwnerSubsystem = Subsystem;
	}

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings || !Settings->bEnableWaterInteraction || !Subsystem || !Subsystem->AreWaterAssetsReady())
	{
		return;
	}

	if (!HeightRT)
	{
		HeightRT = Subsystem->GetWaterHeightRT();
	}

	if (!MaterialParameterCollection)
	{
		MaterialParameterCollection = Subsystem->GetMaterialParameterCollection();
	}

	if (!HeightRT || !MaterialParameterCollection)
	{
		return;
	}

	RenderTargetSize = Settings->GetWaterInteractionRenderTargetSize();
	ApplyOutputRenderTarget();

	const FVector InteractionExtent = GroundInteractionCommon::MakeSafeInteractionExtent(Settings->WaterInteractionWorldExtent);
	const FIntVector NumCell(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1), 1);
	const FVector CellSpacing(
		FMath::Max(InteractionExtent.X, 1.0f) * 2.0f / float(NumCell.X),
		FMath::Max(InteractionExtent.Y, 1.0f) * 2.0f / float(NumCell.Y),
		FMath::Max(InteractionExtent.Z, 1.0f) * 2.0f / float(FMath::Max(NumCell.Z, 1)));

	RegisteredScene = World->Scene;
	RenderProxy = new FWaterInteractionRendererProxy(HeightRT->TextureReference.TextureReferenceRHI, NumCell, CellSpacing, InteractionExtent);

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

	UKismetMaterialLibrary::SetVectorParameterValue(
		GetWorld(),
		MaterialParameterCollection,
		FName(TEXT("WI_OriginWS")),
		FLinearColor(
			BottomInteractionCenterWS.X,
			BottomInteractionCenterWS.Y,
			BottomInteractionCenterWS.Z,
			0.0f));

	UKismetMaterialLibrary::SetVectorParameterValue(
		GetWorld(),
		MaterialParameterCollection,
		FName(TEXT("WI_ExtentWS")),
		FLinearColor(
			InteractionExtentWS.X,
			InteractionExtentWS.Y,
			InteractionExtentWS.Z,
			0.0f));

	UKismetMaterialLibrary::SetScalarParameterValue(
		GetWorld(),
		MaterialParameterCollection,
		FName(TEXT("WI_Valid")),
		1.0f);
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
