#include "WaterInteractionRendererComponent.h"
#include "GroundInteractionCommon.h"
#include "WaterInteractionRendererProxy.h"
#include "GroundInteractionSettings.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "NiagaraComponent.h"
#include "PrimitiveComponentId.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "SceneViewOwner.h"

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

void UWaterInteractionRendererComponent::InitializeRenderer(UNiagaraComponent* InWaterSplatComponent,
	UTextureRenderTarget2D* InHeightRT, UMaterialParameterCollection* InMaterialParameterCollection)
{
	WaterSplatComponent = InWaterSplatComponent;
	HeightRT = InHeightRT;
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

void UWaterInteractionRendererComponent::UpdateWaterInteractionDepth_GameThread(FSceneViewFamily& InViewFamily)
{
	check(IsInGameThread());

	if (!RenderProxy)
	{
		return;
	}

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings || !Settings->bEnableWaterInteraction)
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

		DepthUpdateAccumulatorSeconds = 0.0f;
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

	const float DepthUpdateIntervalSeconds = Settings->GetWaterInteractionUpdateIntervalSeconds();
	const float DeltaTimeSeconds = FMath::Max(InViewFamily.Time.GetDeltaRealTimeSeconds(), 0.0f);

	DepthUpdateAccumulatorSeconds += DeltaTimeSeconds;

	if (DepthUpdateAccumulatorSeconds < DepthUpdateIntervalSeconds)
	{
		return;
	}

	const int32 DepthUpdateStepCount = FMath::Max(FMath::FloorToInt(DepthUpdateAccumulatorSeconds / DepthUpdateIntervalSeconds), 1);

	DepthUpdateAccumulatorSeconds -= static_cast<float>(DepthUpdateStepCount) * DepthUpdateIntervalSeconds;

	UNiagaraComponent* NiagaraComponent = WaterSplatComponent.Get();
	if (!IsValid(NiagaraComponent) || !NiagaraComponent->IsRegistered())
	{
		return;
	}

	const AActor* NiagaraOwner = NiagaraComponent->GetOwner();
	if (!IsValid(NiagaraOwner))
	{
		return;
	}

	const FPrimitiveComponentId NiagaraPrimitiveId = NiagaraComponent->GetPrimitiveSceneId();
	if (!NiagaraPrimitiveId.IsValid())
	{
		return;
	}

	const FVector InteractionExtentWS = GroundInteractionCommon::MakeSafeInteractionExtent(Settings->WaterInteractionWorldExtent);
	const FVector RawCenterWS = GetComponentLocation();
	const FVector SnappedCenterWS = GroundInteractionCommon::SnapInteractionCenterWS(RawCenterWS, InteractionExtentWS, RenderTargetSize, Settings->WaterInteractionLocationSnapCellCount);

	TSet<FPrimitiveComponentId> ShowOnlyPrimitives;
	ShowOnlyPrimitives.Add(NiagaraPrimitiveId);

	const FVector BottomInteractionCenterWS = GroundInteractionCommon::GetBottomInteractionCenterWS(SnappedCenterWS, InteractionExtentWS);

	const FSceneViewOwner ViewOwner(NiagaraOwner);
	RenderProxy->QueueWaterInteractionDepthPass_GameThread(InViewFamily, ViewOwner, ShowOnlyPrimitives, BottomInteractionCenterWS, RenderTargetSize, Settings->WaterInteractionPulseStrength);
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
