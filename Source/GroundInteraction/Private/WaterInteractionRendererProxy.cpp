#include "WaterInteractionRendererProxy.h"
#include "GroundInteractionCommon.h"

#include "GlobalShader.h"
#include "RenderTargetPool.h"
#include "RenderGraphUtils.h"
#include "Rendering/CustomRenderPass.h"
#include "Runtime/Renderer/Private/SceneRendering.h"
#include "Runtime/Engine/Public/SceneInterface.h"

static TMap<FSceneInterface*, TArray<FWaterInteractionRendererProxy*>> WaterProxiesByScene;

// Keep these names unique because Unreal's Unity build can concatenate this
// source file with GroundInteractionRendererProxy.cpp into one translation unit.
static constexpr int32 WATER_GROUP_SIZE_X = 8;
static constexpr int32 WATER_GROUP_SIZE_Y = 8;
static constexpr int32 WATER_GROUP_SIZE_Z = 1;

static constexpr EPixelFormat WATER_HEIGHT_PIXEL_FORMAT = PF_R16F;

class FWaterScrollingCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FWaterScrollingCS);
	SHADER_USE_PARAMETER_STRUCT(FWaterScrollingCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_X"), WATER_GROUP_SIZE_X);
		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_Y"), WATER_GROUP_SIZE_Y);
		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_Z"), WATER_GROUP_SIZE_Z);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, NumCells)
		SHADER_PARAMETER(FIntPoint, ScrollPixels)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, InputHeightTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputHeightTexture)
	END_SHADER_PARAMETER_STRUCT()
};

class FApplyWaterImpulseCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FApplyWaterImpulseCS);
	SHADER_USE_PARAMETER_STRUCT(FApplyWaterImpulseCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_X"), WATER_GROUP_SIZE_X);
		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_Y"), WATER_GROUP_SIZE_Y);
		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_Z"), WATER_GROUP_SIZE_Z);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, NumCells)
		SHADER_PARAMETER(FIntPoint, ScrollPixels)
		SHADER_PARAMETER(float, MaxSceneDepth)
		SHADER_PARAMETER(float, SceneDepthInvalidEpsilon)
		SHADER_PARAMETER(float, DepthOffsetZ)
		SHADER_PARAMETER(float, PulseStrength)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CurrentDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, HeightTexture)
	END_SHADER_PARAMETER_STRUCT()
};

class FWaterInteractionDepthPass final : public FCustomRenderPassBase
{
public:
	IMPLEMENT_CUSTOM_RENDER_PASS(FWaterInteractionDepthPass);

	FWaterInteractionDepthPass(
		const FIntPoint& InRenderTargetSize,
		FWaterInteractionRendererProxy* InOwnerProxy,
		const FVector& InBottomInteractionCenterWS,
		float InMaxSceneDepth,
		float InSceneDepthInvalidEpsilon,
		float InPulseStrength)
		: FCustomRenderPassBase(
			TEXT("WaterInteractionDepthPass"),
			FCustomRenderPassBase::ERenderMode::DepthPass,
			FCustomRenderPassBase::ERenderOutput::SceneDepth,
			InRenderTargetSize)
		, OwnerProxy(InOwnerProxy)
		, RenderTargetSize(InRenderTargetSize)
		, BottomInteractionCenterWS(InBottomInteractionCenterWS)
		, MaxDepth(InMaxSceneDepth)
		, SceneDepthInvalidEpsilon(InSceneDepthInvalidEpsilon)
		, PulseStrength(InPulseStrength)
	{
	}

	virtual void OnPreRender(FRDGBuilder& GraphBuilder) override
	{
		const FIntPoint SafeSize(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1));
		const bool bNeedsAllocateRenderTarget = !DepthPooledTarget.IsValid() || DepthPooledTarget->GetDesc().Extent != SafeSize || DepthPooledTarget->GetDesc().Format != PF_R32_FLOAT;

		if (bNeedsAllocateRenderTarget)
		{
			FPooledRenderTargetDesc Desc = FPooledRenderTargetDesc::Create2DDesc(
				SafeSize,
				PF_R32_FLOAT,
				FClearValueBinding::Black,
				TexCreate_None,
				TexCreate_RenderTargetable | TexCreate_ShaderResource,
				false);

			Desc.DebugName = TEXT("WI_WaterInteractionDepth");
			GRenderTargetPool.FindFreeElement(GraphBuilder.RHICmdList, Desc, DepthPooledTarget, TEXT("WI_WaterInteractionDepth"));
		}

		if (!DepthPooledTarget.IsValid())
		{
			RenderTargetTexture = nullptr;
			PreRenderGraphBuilder = nullptr;
			return;
		}

		RenderTargetTexture = GraphBuilder.RegisterExternalTexture(DepthPooledTarget, TEXT("WI_WaterInteractionDepth_PreRender"));
		PreRenderGraphBuilder = &GraphBuilder;

		if (RenderTargetTexture && Views.Num() > 0 && Views[0] != nullptr)
		{
			AddClearRenderTargetPass(GraphBuilder, RenderTargetTexture, FLinearColor::Black, Views[0]->UnscaledViewRect);
		}
	}

	virtual void OnPostRender(FRDGBuilder& GraphBuilder) override
	{
		if (!OwnerProxy || !DepthPooledTarget.IsValid() || Views.Num() == 0 || Views[0] == nullptr)
		{
			return;
		}

		FRDGTextureRef DepthPassTexture = nullptr;

		if (PreRenderGraphBuilder == &GraphBuilder && RenderTargetTexture)
		{
			DepthPassTexture = RenderTargetTexture;
		}
		else
		{
			DepthPassTexture = GraphBuilder.RegisterExternalTexture(DepthPooledTarget, TEXT("WI_WaterInteractionDepth_PostRender"));
		}

		if (!DepthPassTexture)
		{
			return;
		}

		OwnerProxy->ApplyWaterImpulse(GraphBuilder, DepthPassTexture, RenderTargetSize, MaxDepth, SceneDepthInvalidEpsilon, BottomInteractionCenterWS, PulseStrength, Views[0]->GetFeatureLevel());
	}

private:
	FWaterInteractionRendererProxy* OwnerProxy = nullptr;
	FIntPoint RenderTargetSize = FIntPoint(1, 1);
	FVector BottomInteractionCenterWS = FVector::ZeroVector;
	float MaxDepth = 1.0f;
	float SceneDepthInvalidEpsilon = 0.1f;
	float PulseStrength = 0.0f;
	TRefCountPtr<IPooledRenderTarget> DepthPooledTarget;
	FRDGBuilder* PreRenderGraphBuilder = nullptr;
};

IMPLEMENT_GLOBAL_SHADER(FWaterScrollingCS, "/Plugin/GroundInteraction/GroundInteractionWaterScrolling.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FApplyWaterImpulseCS, "/Plugin/GroundInteraction/GroundInteractionApplyWaterImpulse.usf", "MainCS", SF_Compute);

static void AddWaterScrollingPass(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FRDGTextureSRVRef InputHeightTextureSRV,
	FRDGTextureUAVRef OutputHeightTextureUAV,
	const FIntPoint& RenderTargetSize,
	const FIntPoint& ScrollPixels)
{
	FWaterScrollingCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FWaterScrollingCS::FParameters>();
	const FIntPoint NumCells(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1));

	PassParameters->NumCells = NumCells;
	PassParameters->ScrollPixels = ScrollPixels;
	PassParameters->InputHeightTexture = InputHeightTextureSRV;
	PassParameters->OutputHeightTexture = OutputHeightTextureUAV;

	TShaderMapRef<FWaterScrollingCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
	const FIntVector DispatchCount = FComputeShaderUtils::GetGroupCount(FIntVector(NumCells.X, NumCells.Y, 1), FIntVector(WATER_GROUP_SIZE_X, WATER_GROUP_SIZE_Y, WATER_GROUP_SIZE_Z));

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GroundInteraction.WaterScrolling"), ComputeShader, PassParameters, DispatchCount);
}

static void AddWaterImpulsePass(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FRDGTextureUAVRef HeightTextureUAV,
	FRDGTextureRef DepthPassTexture,
	const FIntPoint& RenderTargetSize,
	const FIntPoint& ScrollPixels,
	float MaxSceneDepth,
	float SceneDepthInvalidEpsilon,
	float DepthOffsetZ,
	float PulseStrength)
{
	FApplyWaterImpulseCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplyWaterImpulseCS::FParameters>();
	const FIntPoint NumCells(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1));

	PassParameters->NumCells = NumCells;
	PassParameters->ScrollPixels = ScrollPixels;
	PassParameters->MaxSceneDepth = MaxSceneDepth;
	PassParameters->SceneDepthInvalidEpsilon = SceneDepthInvalidEpsilon;
	PassParameters->DepthOffsetZ = DepthOffsetZ;
	PassParameters->PulseStrength = FMath::Max(PulseStrength, 0.0f);
	PassParameters->CurrentDepthTexture = DepthPassTexture;
	PassParameters->HeightTexture = HeightTextureUAV;

	TShaderMapRef<FApplyWaterImpulseCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
	const FIntVector DispatchCount = FComputeShaderUtils::GetGroupCount(FIntVector(NumCells.X, NumCells.Y, 1), FIntVector(WATER_GROUP_SIZE_X, WATER_GROUP_SIZE_Y, WATER_GROUP_SIZE_Z));

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GroundInteraction.ApplyWaterImpulse"), ComputeShader, PassParameters, DispatchCount);
}

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
	, PreviousSnappedLocation(FVector2D::ZeroVector)
	, bHasPreviousLocation(false)
	, InteractionExtentWS(
		FMath::Max(InInteractionExtentWS.X, 1.0),
		FMath::Max(InInteractionExtentWS.Y, 1.0),
		FMath::Max(InInteractionExtentWS.Z, 1.0))
	, HeightTextures()
	, PreviousHeightTextureId(0)
	, CurrentHeightTextureId(1)
	, NextHeightTextureId(2)
	, bClearHeightTextures(true)
	, bReleased(false)
{
}

void FWaterInteractionRendererProxy::Render(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	if (bReleased)
	{
		return;
	}

	if (!InViewFamily.Scene || InViewFamily.Views.Num() == 0)
	{
		return;
	}

	const FSceneView* PrimaryView = InViewFamily.Views[0];

	if (!PrimaryView || PrimaryView->bIsSceneCapture || PrimaryView->bIsReflectionCapture)
	{
		return;
	}

	if (!OutputHeightTextureReferenceRHI.IsValid())
	{
		return;
	}

	if (RenderTargetSize.X <= 0 || RenderTargetSize.Y <= 0)
	{
		return;
	}

	ResetHeightTransientRefs();

	if (bRenderTargetChanged)
	{
		bRenderTargetChanged = false;
		bHasPreviousLocation = false;
		PreviousSnappedLocation = FVector2D::ZeroVector;

		ResetHeightTextureRoles();
		bClearHeightTextures = true;
	}

	CheckHeightTextures(GraphBuilder, RenderTargetSize, bClearHeightTextures);

	bClearHeightTextures = false;

	const FVector2D CurrentSnappedLocationXY(CurrentSnappedLocation.X, CurrentSnappedLocation.Y);

	if (!bHasPreviousLocation)
	{
		bHasPreviousLocation = true;
		PreviousSnappedLocation = CurrentSnappedLocationXY;
	}
	else
	{
		const FVector2D UnitsPerPixel = GroundInteractionCommon::GetUnitsPerPixel(InteractionExtentWS, RenderTargetSize);
		const FIntPoint ScrollPixels(
			FMath::RoundToInt((CurrentSnappedLocationXY.X - PreviousSnappedLocation.X) / UnitsPerPixel.X),
			-FMath::RoundToInt((CurrentSnappedLocationXY.Y - PreviousSnappedLocation.Y) / UnitsPerPixel.Y));

		if (ScrollPixels != FIntPoint::ZeroValue)
		{
			FPlaneTexture& PreviousHeight = HeightTextures[GetPreviousHeightTextureId()];
			FPlaneTexture& CurrentHeight = HeightTextures[GetCurrentHeightTextureId()];
			FPlaneTexture& NextHeight = HeightTextures[GetNextHeightTextureId()];

			FRDGTextureRef PreviousHeightTexture = PreviousHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterScrolling_PreviousHeight"));
			FRDGTextureRef CurrentHeightTexture = CurrentHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterScrolling_CurrentHeight"));
			FRDGTextureRef NextHeightTexture = NextHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterScrolling_NextHeight"));

			FRDGTextureSRVRef PreviousHeightSRV = PreviousHeight.GetOrCreateSRV(GraphBuilder, TEXT("WaterScrolling_PreviousHeight"));
			FRDGTextureSRVRef CurrentHeightSRV = CurrentHeight.GetOrCreateSRV(GraphBuilder, TEXT("WaterScrolling_CurrentHeight"));

			FRDGTextureUAVRef CurrentHeightUAV = CurrentHeight.GetOrCreateUAV(GraphBuilder, TEXT("WaterScrolling_CurrentHeight"));
			FRDGTextureUAVRef NextHeightUAV = NextHeight.GetOrCreateUAV(GraphBuilder, TEXT("WaterScrolling_NextHeight"));

			if (PreviousHeightTexture && CurrentHeightTexture && NextHeightTexture &&
				PreviousHeightSRV && CurrentHeightSRV &&
				CurrentHeightUAV && NextHeightUAV)
			{
				GraphBuilder.UseInternalAccessMode(PreviousHeightTexture);
				GraphBuilder.UseInternalAccessMode(CurrentHeightTexture);
				GraphBuilder.UseInternalAccessMode(NextHeightTexture);

				AddWaterScrollingPass(
					GraphBuilder,
					PrimaryView->GetFeatureLevel(),
					CurrentHeightSRV,
					NextHeightUAV,
					RenderTargetSize,
					ScrollPixels);

				AddWaterScrollingPass(
					GraphBuilder,
					PrimaryView->GetFeatureLevel(),
					PreviousHeightSRV,
					CurrentHeightUAV,
					RenderTargetSize,
					ScrollPixels);

				AdvanceAfterScrolling();
				PreviousSnappedLocation = CurrentSnappedLocationXY;
			}
		}
		else
		{
			PreviousSnappedLocation = CurrentSnappedLocationXY;
		}
	}

	// ---- Water Height Simulation Pass 실행 ----
	// AddWaterHeightSimulationPass(...);
	// AdvanceAfterSimulation();

	CopyCurrentHeightToOutput(GraphBuilder);
}

void FWaterInteractionRendererProxy::QueueWaterInteractionDepthPass_GameThread(FSceneViewFamily& InViewFamily, const TSet<FPrimitiveComponentId>& InShowOnlyPrimitives, const FVector& BottomInteractionCenterWS, const FIntPoint& DepthRenderTargetSize, float PulseStrength)
{
	check(IsInGameThread());

	if (!InViewFamily.Scene || InShowOnlyPrimitives.Num() <= 0)
	{
		return;
	}

	const FIntPoint SafeRenderTargetSize(FMath::Max(DepthRenderTargetSize.X, 1), FMath::Max(DepthRenderTargetSize.Y, 1));
	const float HalfExtentX = FMath::Max(InteractionExtentWS.X, 1.0f);
	const float HalfExtentY = FMath::Max(InteractionExtentWS.Y, 1.0f);
	const FVector ViewLookAt = BottomInteractionCenterWS + FVector(0.0f, 0.0f, 1.0f);
	const float MaxDepth = GetMaxSceneDepth();

	FSceneInterface::FCustomRenderPassRendererInput PassInput;
	PassInput.ViewLocation = BottomInteractionCenterWS;
	PassInput.ViewRotationMatrix = FLookAtMatrix(BottomInteractionCenterWS, ViewLookAt, FVector(0.0f, 1.0f, 0.0f));
	PassInput.ViewRotationMatrix = PassInput.ViewRotationMatrix.RemoveTranslation();
	PassInput.ViewRotationMatrix.RemoveScaling();
	PassInput.ProjectionMatrix = BuildBottomUpOrthoProjection(HalfExtentX, HalfExtentY, MaxDepth);
	PassInput.ViewActor = nullptr;
	PassInput.ShowOnlyPrimitives = InShowOnlyPrimitives;
	PassInput.bUseMainViewFamilyShowFlags = false;
	PassInput.EngineShowFlags = FEngineShowFlags(ESFIM_Game);
	PassInput.EngineShowFlags.SetFog(false);
	PassInput.EngineShowFlags.SetAtmosphere(false);
	PassInput.EngineShowFlags.SetSkyLighting(false);
	PassInput.EngineShowFlags.SetPostProcessing(false);
	PassInput.EngineShowFlags.SetAntiAliasing(false);
	PassInput.EngineShowFlags.SetDecals(false);
	PassInput.EngineShowFlags.SetDynamicShadows(false);
	PassInput.EngineShowFlags.SetTranslucency(false);
	PassInput.CustomRenderPass = new FWaterInteractionDepthPass(SafeRenderTargetSize, this, BottomInteractionCenterWS, MaxDepth, GetSceneDepthInvalidEpsilon(), PulseStrength);

	InViewFamily.Scene->AddCustomRenderPass(&InViewFamily, PassInput);
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

void FWaterInteractionRendererProxy::ApplyWaterImpulse(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef DepthPassTexture,
	const FIntPoint& PassRenderTargetSize,
	float PassMaxSceneDepth,
	float PassSceneDepthInvalidEpsilon,
	const FVector& PassBottomInteractionCenterWS,
	float PulseStrength,
	ERHIFeatureLevel::Type FeatureLevel)
{
	check(IsInRenderingThread());

	if (bReleased || !DepthPassTexture)
	{
		return;
	}

	ResetHeightTransientRefs();
	CheckHeightTextures(GraphBuilder, PassRenderTargetSize, bClearHeightTextures);
	bClearHeightTextures = false;

	const int32 CurrentHeightTextureIdForUpdate = GetCurrentHeightTextureId();

	if (CurrentHeightTextureIdForUpdate < 0 || CurrentHeightTextureIdForUpdate >= 3)
	{
		return;
	}

	FPlaneTexture& CurrentHeight = HeightTextures[CurrentHeightTextureIdForUpdate];
	FRDGTextureRef CurrentHeightTexture = CurrentHeight.GetOrCreateTexture(GraphBuilder, TEXT("ApplyWaterImpulse_Height"));
	FRDGTextureUAVRef CurrentHeightUAV = CurrentHeight.GetOrCreateUAV(GraphBuilder, TEXT("ApplyWaterImpulse_Height"));

	if (!CurrentHeightTexture || !CurrentHeightUAV)
	{
		return;
	}

	const FIntPoint SafeRenderTargetSize(FMath::Max(PassRenderTargetSize.X, 1), FMath::Max(PassRenderTargetSize.Y, 1));
	const FVector CurrentBottomInteractionCenterWS = GetBottomInteractionCenterWS();
	const FVector2D UnitsPerPixel = GroundInteractionCommon::GetUnitsPerPixel(InteractionExtentWS, SafeRenderTargetSize);
	const FIntPoint ScrollPixels(
		FMath::RoundToInt((CurrentBottomInteractionCenterWS.X - PassBottomInteractionCenterWS.X) / UnitsPerPixel.X),
		-FMath::RoundToInt((CurrentBottomInteractionCenterWS.Y - PassBottomInteractionCenterWS.Y) / UnitsPerPixel.Y));
	const float DepthOffsetZ = static_cast<float>(PassBottomInteractionCenterWS.Z - CurrentBottomInteractionCenterWS.Z);

	GraphBuilder.UseInternalAccessMode(CurrentHeightTexture);

	AddWaterImpulsePass(
		GraphBuilder,
		FeatureLevel,
		CurrentHeightUAV,
		DepthPassTexture,
		SafeRenderTargetSize,
		ScrollPixels,
		PassMaxSceneDepth,
		PassSceneDepthInvalidEpsilon,
		DepthOffsetZ,
		PulseStrength);

	GraphBuilder.SetTextureAccessFinal(CurrentHeightTexture, ERHIAccess::SRVMask);

	if (!bHasPreviousLocation)
	{
		bHasPreviousLocation = true;
		PreviousSnappedLocation = FVector2D(CurrentSnappedLocation.X, CurrentSnappedLocation.Y);
	}
}

void FWaterInteractionRendererProxy::CheckHeightTextures(FRDGBuilder& GraphBuilder, const FIntPoint& InRenderTargetSize, bool bClearAll)
{
	check(IsInRenderingThread());

	const FIntPoint SafeSize(FMath::Max(InRenderTargetSize.X, 1), FMath::Max(InRenderTargetSize.Y, 1));

	bool bAllocatedAny = false;

	for (int32 TextureId = 0; TextureId < 3; ++TextureId)
	{
		const TCHAR* DebugName = nullptr;

		if (TextureId == 0)
		{
			DebugName = TEXT("WaterHeight_0");
		}
		else if (TextureId == 1)
		{
			DebugName = TEXT("WaterHeight_1");
		}
		else
		{
			DebugName = TEXT("WaterHeight_2");
		}

		bAllocatedAny |= HeightTextures[TextureId].CheckPooledTarget(GraphBuilder.RHICmdList, SafeSize, WATER_HEIGHT_PIXEL_FORMAT, DebugName);
	}

	if (!bAllocatedAny && !bClearAll)
	{
		return;
	}

	ResetHeightTransientRefs();

	for (int32 TextureId = 0; TextureId < 3; ++TextureId)
	{
		FPlaneTexture& Height = HeightTextures[TextureId];

		FRDGTextureRef HeightTexture = Height.GetOrCreateTexture(GraphBuilder, TEXT("WaterHeight_Clear"));
		FRDGTextureUAVRef HeightUAV = Height.GetOrCreateUAV(GraphBuilder, TEXT("WaterHeight_Clear"));

		if (!HeightTexture || !HeightUAV)
		{
			continue;
		}

		GraphBuilder.UseInternalAccessMode(HeightTexture);

		AddClearUAVPass(GraphBuilder, HeightUAV, FVector4f(0.0f, 0.0f, 0.0f, 0.0f));

		GraphBuilder.SetTextureAccessFinal(HeightTexture, ERHIAccess::SRVMask);
	}
}

void FWaterInteractionRendererProxy::ResetHeightTransientRefs()
{
	check(IsInRenderingThread());

	for (int32 TextureId = 0; TextureId < 3; ++TextureId)
	{
		HeightTextures[TextureId].ResetTransientRefs();
	}
}

void FWaterInteractionRendererProxy::CopyCurrentHeightToOutput(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());

	if (bReleased)
	{
		return;
	}

	FTextureRHIRef OutputHeightTextureRHI = nullptr;
	if (OutputHeightTextureReferenceRHI.IsValid())
	{
		if (FRHITexture* Texture = OutputHeightTextureReferenceRHI->GetReferencedTexture())
		{
			OutputHeightTextureRHI = FTextureRHIRef(Texture);
		}
	}

	if (!OutputHeightTextureRHI.IsValid())
	{
		return;
	}

	const int32 CurrentHeightTextureIdForCopy = GetCurrentHeightTextureId();

	if (CurrentHeightTextureIdForCopy < 0 || CurrentHeightTextureIdForCopy >= 3)
	{
		return;
	}

	FPlaneTexture& CurrentHeight = HeightTextures[CurrentHeightTextureIdForCopy];

	FRDGTextureRef CurrentHeightTexture = CurrentHeight.GetOrCreateTexture(GraphBuilder, TEXT("Copy_WaterHeight"));
	FRDGTextureRef OutputHeightTexture = RegisterExternalTexture(GraphBuilder, OutputHeightTextureRHI, TEXT("Output_WaterHeight"));

	if (!CurrentHeightTexture || !OutputHeightTexture)
	{
		return;
	}

	GraphBuilder.UseInternalAccessMode(CurrentHeightTexture);
	GraphBuilder.UseInternalAccessMode(OutputHeightTexture);

	AddCopyTexturePass(GraphBuilder, CurrentHeightTexture, OutputHeightTexture);

	GraphBuilder.UseExternalAccessMode(OutputHeightTexture, ERHIAccess::SRVMask);
}

FVector FWaterInteractionRendererProxy::GetBottomInteractionCenterWS() const
{
	return GroundInteractionCommon::GetBottomInteractionCenterWS(CurrentSnappedLocation, InteractionExtentWS);
}

float FWaterInteractionRendererProxy::GetMaxSceneDepth() const
{
	return GroundInteractionCommon::GetMaxSceneDepth(InteractionExtentWS);
}

float FWaterInteractionRendererProxy::GetSceneDepthInvalidEpsilon() const
{
	return GroundInteractionCommon::GetSceneDepthInvalidEpsilon(InteractionExtentWS);
}

FMatrix FWaterInteractionRendererProxy::BuildBottomUpOrthoProjection(float HalfExtentX, float HalfExtentY, float MaxDepth)
{
	const float SafeHalfExtentX = FMath::Max(HalfExtentX, 1.0f);
	const float SafeHalfExtentY = FMath::Max(HalfExtentY, 1.0f);
	const float SafeMaxDepth = FMath::Max(MaxDepth, 1.0f);
	const float ZScale = 1.0f / SafeMaxDepth;
	const float ZOffset = 0.0f;

	return FReversedZOrthoMatrix(SafeHalfExtentX, SafeHalfExtentY, ZScale, ZOffset);
}
