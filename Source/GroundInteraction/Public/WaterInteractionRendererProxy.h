#pragma once

#include "CoreMinimal.h"
#include "GroundInteractionRendering.h"
#include "WaterInteractionCommon.h"
#include "RenderDeferredCleanup.h"
#include "SceneView.h"

class GROUNDINTERACTION_API FWaterInteractionRendererProxy final : public FDeferredCleanupInterface
{
public:
	explicit FWaterInteractionRendererProxy(
		FTextureReferenceRHIRef InHeightTextureReferenceRHI,
		FIntVector InNumCell,
		FVector InCellSpacing,
		const FVector& InInteractionExtentWS,
		float InWaterInteractionPulseStrength,
		float InWaterInteractionAdvectAmount,
		float InWaterInteractionDamping,
		int32 InWaterInteractionSimulationRate,
		int32 InWaterInteractionMaxSubstepsPerFrame);
	
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
	TArray<FWaterSplatWorld> PendingWaterSplats;

private:
	bool CheckHeightTextures(FRDGBuilder& GraphBuilder, const FIntPoint& InRenderTargetSize, bool bClearAll);
	void ResetHeightTransientRefs();
	void CopyHeightToOutput(FRDGBuilder& GraphBuilder, int32 SourceHeightTextureId);

	int32 GetCurrentHeightTextureId() const
	{
		return CurrentHeightTextureId;
	}

	int32 GetPreviousHeightTextureId() const
	{
		return PreviousHeightTextureId;
	}

	int32 GetNextHeightTextureId() const
	{
		return NextHeightTextureId;
	}

	void ResetHeightTextureRoles()
	{
		CurrentHeightTextureId = 0;
		PreviousHeightTextureId = 1;
		NextHeightTextureId = 2;
	}

	void AdvanceHeightTextureAfterScrolling()
	{
		const int32 OldCurrentHeightTextureId = CurrentHeightTextureId;
		const int32 OldPreviousHeightTextureId = PreviousHeightTextureId;
		const int32 OldNextHeightTextureId = NextHeightTextureId;

		CurrentHeightTextureId = OldPreviousHeightTextureId;
		PreviousHeightTextureId = OldNextHeightTextureId;
		NextHeightTextureId = OldCurrentHeightTextureId;
	}

	void AdvanceHeightTextureAfterSimulation()
	{
		const int32 OldCurrentHeightTextureId = CurrentHeightTextureId;
		const int32 OldPreviousHeightTextureId = PreviousHeightTextureId;
		const int32 OldNextHeightTextureId = NextHeightTextureId;

		CurrentHeightTextureId = OldNextHeightTextureId;
		PreviousHeightTextureId = OldCurrentHeightTextureId;
		NextHeightTextureId = OldPreviousHeightTextureId;
	}

	FTextureReferenceRHIRef OutputHeightTextureReferenceRHI;

	FVector CellSpacing = FVector::OneVector;
	FVector2D PreviousSnappedLocation = FVector2D::ZeroVector;
	bool bHasPreviousLocation = false;

	FVector InteractionExtentWS = FVector(1000.0f, 1000.0f, 1000.0f);
	float WaterInteractionPulseStrength = 1.0f;
	float WaterInteractionAdvectAmount = 600.0f;
	float WaterInteractionDamping = 3.0f;
	double WaterInteractionFixedDeltaTime = 1.0 / 60.0;
	int32 WaterInteractionMaxSubstepsPerFrame = 4;
	double SimulationAccumulatorSeconds = 0.0;
	double WaterRenderTimeRemainingSeconds = 0.0;

	FPlaneTexture HeightTextures[3];

	int32 CurrentHeightTextureId = 0;
	int32 PreviousHeightTextureId = 1;
	int32 NextHeightTextureId = 2;

	bool bClearHeightTextures = true;
	bool bReleased = false;
};
