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
		float InWaterInteractionMaxDeltaTime);
	
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
	void CheckHeightTextures(FRDGBuilder& GraphBuilder, const FIntPoint& InRenderTargetSize, bool bClearAll);
	void ResetHeightTransientRefs();
	void CopyHeightToOutput(FRDGBuilder& GraphBuilder, int32 SourceHeightTextureId);

	int32 GetPreviousHeightTextureId() const
	{
		return PreviousHeightTextureId;
	}

	int32 GetCurrentHeightTextureId() const
	{
		return CurrentHeightTextureId;
	}

	int32 GetNextHeightTextureId() const
	{
		return NextHeightTextureId;
	}

	void ResetHeightTextureRoles()
	{
		PreviousHeightTextureId = 0;
		CurrentHeightTextureId = 1;
		NextHeightTextureId = 2;
	}

	void AdvanceHeightTextureAfterScrolling()
	{
		const int32 OldPreviousHeightTextureId = PreviousHeightTextureId;
		const int32 OldCurrentHeightTextureId = CurrentHeightTextureId;
		const int32 OldNextHeightTextureId = NextHeightTextureId;

		PreviousHeightTextureId = OldCurrentHeightTextureId;
		CurrentHeightTextureId = OldNextHeightTextureId;
		NextHeightTextureId = OldPreviousHeightTextureId;
	}

	void AdvanceHeightTextureAfterSimulation()
	{
		const int32 OldPreviousHeightTextureId = PreviousHeightTextureId;
		const int32 OldCurrentHeightTextureId = CurrentHeightTextureId;
		const int32 OldNextHeightTextureId = NextHeightTextureId;

		PreviousHeightTextureId = OldNextHeightTextureId;
		CurrentHeightTextureId = OldPreviousHeightTextureId;
		NextHeightTextureId = OldCurrentHeightTextureId;
	}

	FTextureReferenceRHIRef OutputHeightTextureReferenceRHI;

	FVector CellSpacing = FVector::OneVector;
	FVector2D PreviousSnappedLocation = FVector2D::ZeroVector;
	bool bHasPreviousLocation = false;

	FVector InteractionExtentWS = FVector(1000.0f, 1000.0f, 1000.0f);
	float WaterInteractionPulseStrength = 1.0f;
	float WaterInteractionAdvectAmount = 600.0f;
	float WaterInteractionDamping = 3.0f;
	float WaterInteractionMaxDeltaTime = 0.04f;

	FPlaneTexture HeightTextures[3];

	int32 PreviousHeightTextureId = 0;
	int32 CurrentHeightTextureId = 1;
	int32 NextHeightTextureId = 2;

	bool bClearHeightTextures = true;
	bool bReleased = false;
};
