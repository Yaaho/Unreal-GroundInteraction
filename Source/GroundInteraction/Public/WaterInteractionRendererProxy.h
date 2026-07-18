#pragma once

#include "CoreMinimal.h"
#include "GroundInteractionRendering.h"
#include "RenderDeferredCleanup.h"
#include "SceneView.h"

class GROUNDINTERACTION_API FWaterInteractionRendererProxy final : public FDeferredCleanupInterface
{
public:
	explicit FWaterInteractionRendererProxy(
		FTextureReferenceRHIRef InHeightTextureReferenceRHI,
		FIntVector InNumCell,
		FVector InCellSpacing,
		const FVector& InInteractionExtentWS);
	
	virtual ~FWaterInteractionRendererProxy() override = default;

	void Render(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily);
	void Release();

	static void RegisterProxy(FSceneInterface* InScene, FWaterInteractionRendererProxy* InProxy);
	static void UnregisterProxy(FSceneInterface* InScene, FWaterInteractionRendererProxy* InProxy);
	static TArray<FWaterInteractionRendererProxy*> GetProxies(FSceneInterface* InScene);

public:
	FVector CurrentSnappedLocation = FVector::ZeroVector;
	FIntPoint RenderTargetSize = FIntPoint(512, 512);
	bool bRenderTargetChanged = false;

private:
	FTextureReferenceRHIRef OutputHeightTextureReferenceRHI;

	FVector CellSpacing = FVector::OneVector;
	FVector PreviousSnappedLocation = FVector::ZeroVector;
	bool bHasPreviousLocation = false;

	FVector InteractionExtentWS = FVector(1000.0f, 1000.0f, 1000.0f);

	bool bReleased = false;
};
