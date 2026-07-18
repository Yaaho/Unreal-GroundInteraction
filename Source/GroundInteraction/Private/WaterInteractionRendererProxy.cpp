#include "WaterInteractionRendererProxy.h"

#include "RenderingThread.h"

static TMap<FSceneInterface*, TArray<FWaterInteractionRendererProxy*>> WaterProxiesByScene;

FWaterInteractionRendererProxy::FWaterInteractionRendererProxy(
	FTextureReferenceRHIRef InHeightTextureReferenceRHI,
	FIntVector InNumCell,
	FVector InCellSpacing,
	const FVector& InInteractionExtentWS)
	: CurrentSnappedLocation(FVector::ZeroVector)
	, RenderTargetSize(
		FMath::Max(InNumCell.X, 1),
		FMath::Max(InNumCell.Y, 1))
	, bRenderTargetChanged(false)
	, OutputHeightTextureReferenceRHI(InHeightTextureReferenceRHI)
	, CellSpacing(InCellSpacing)
	, PreviousSnappedLocation(FVector::ZeroVector)
	, bHasPreviousLocation(false)
	, InteractionExtentWS(
		FMath::Max(InInteractionExtentWS.X, 1.0),
		FMath::Max(InInteractionExtentWS.Y, 1.0),
		FMath::Max(InInteractionExtentWS.Z, 1.0))
	, bReleased(false)
{
}

void FWaterInteractionRendererProxy::Render(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	(void)GraphBuilder;
	(void)InViewFamily;

	if (bReleased)
	{
		return;
	}
}

void FWaterInteractionRendererProxy::Release()
{
	check(IsInRenderingThread());

	bReleased = true;
}

void FWaterInteractionRendererProxy::RegisterProxy(FSceneInterface* InScene, FWaterInteractionRendererProxy* InProxy)
{
	check(IsInRenderingThread());

	if (!InScene || !InProxy)
	{
		return;
	}

	WaterProxiesByScene.FindOrAdd(InScene).AddUnique(InProxy);
}

void FWaterInteractionRendererProxy::UnregisterProxy(FSceneInterface* InScene, FWaterInteractionRendererProxy* InProxy)
{
	check(IsInRenderingThread());

	if (!InScene || !InProxy)
	{
		return;
	}

	TArray<FWaterInteractionRendererProxy*>* Proxies = WaterProxiesByScene.Find(InScene);

	if (!Proxies)
	{
		return;
	}

	Proxies->RemoveSwap(InProxy);

	if (Proxies->Num() == 0)
	{
		WaterProxiesByScene.Remove(InScene);
	}
}

TArray<FWaterInteractionRendererProxy*> FWaterInteractionRendererProxy::GetProxies(FSceneInterface* InScene)
{
	check(IsInRenderingThread());

	if (!InScene)
	{
		return {};
	}

	const TArray<FWaterInteractionRendererProxy*>* Proxies = WaterProxiesByScene.Find(InScene);

	return Proxies ? *Proxies : TArray<FWaterInteractionRendererProxy*>();
}
