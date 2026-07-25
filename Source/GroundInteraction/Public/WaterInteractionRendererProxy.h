#pragma once

#include "CoreMinimal.h"
#include "GroundInteractionRendering.h"
#include "PrimitiveComponentId.h"
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
	void QueueWaterInteractionDepthPass_GameThread(
		FSceneViewFamily& InViewFamily,
		const TSet<FPrimitiveComponentId>& InShowOnlyPrimitives,
		const FVector& BottomInteractionCenterWS,
		const FIntPoint& DepthRenderTargetSize,
		float PulseStrength);
	void Release();

	static void RegisterProxy(FSceneInterface* InScene, FWaterInteractionRendererProxy* InProxy);
	static void UnregisterProxy(FSceneInterface* InScene, FWaterInteractionRendererProxy* InProxy);
	static TArray<FWaterInteractionRendererProxy*> GetProxies(FSceneInterface* InScene);

	void ApplyWaterImpulse(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef DepthPassTexture,
		const FIntPoint& PassRenderTargetSize,
		float PassMaxSceneDepth,
		float PassSceneDepthInvalidEpsilon,
		const FVector& PassBottomInteractionCenterWS,
		float PulseStrength,
		ERHIFeatureLevel::Type FeatureLevel);

public:
	FVector CurrentSnappedLocation = FVector::ZeroVector;
	FIntPoint RenderTargetSize = FIntPoint(512, 512);
	bool bRenderTargetChanged = false;

private:
	void CheckHeightTextures(FRDGBuilder& GraphBuilder, const FIntPoint& InRenderTargetSize, bool bClearAll);
	void ResetHeightTransientRefs();
	void CopyCurrentHeightToOutput(FRDGBuilder& GraphBuilder);
	FVector GetBottomInteractionCenterWS() const;
	float GetMaxSceneDepth() const;
	float GetSceneDepthInvalidEpsilon() const;
	static FMatrix BuildBottomUpOrthoProjection(float HalfExtentX, float HalfExtentY, float MaxDepth);

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

	void AdvanceAfterScrolling()
	{
		AdvanceHeightTextureRoles();
	}

	void AdvanceAfterSimulation()
	{
		AdvanceHeightTextureRoles();
	}

	void ResetHeightTextureRoles()
	{
		PreviousHeightTextureId = 0;
		CurrentHeightTextureId = 1;
		NextHeightTextureId = 2;
	}

	void AdvanceHeightTextureRoles()
	{
		const int32 OldPreviousHeightTextureId = PreviousHeightTextureId;
		const int32 OldCurrentHeightTextureId = CurrentHeightTextureId;
		const int32 OldNextHeightTextureId = NextHeightTextureId;

		PreviousHeightTextureId = OldCurrentHeightTextureId;
		CurrentHeightTextureId = OldNextHeightTextureId;
		NextHeightTextureId = OldPreviousHeightTextureId;
	}

	FTextureReferenceRHIRef OutputHeightTextureReferenceRHI;

	FVector CellSpacing = FVector::OneVector;
	FVector2D PreviousSnappedLocation = FVector2D::ZeroVector;
	bool bHasPreviousLocation = false;

	FVector InteractionExtentWS = FVector(1000.0f, 1000.0f, 1000.0f);

	FPlaneTexture HeightTextures[3];

	int32 PreviousHeightTextureId = 0;
	int32 CurrentHeightTextureId = 1;
	int32 NextHeightTextureId = 2;

	bool bClearHeightTextures = true;
	bool bReleased = false;
};
