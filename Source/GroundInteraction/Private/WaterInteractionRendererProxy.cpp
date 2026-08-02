#include "WaterInteractionRendererProxy.h"
#include "GroundInteractionCommon.h"
#include "WaterInteractionCommon.h"

#include "GlobalShader.h"
#include "RenderTargetPool.h"
#include "RenderGraphUtils.h"
#include "Runtime/Engine/Public/SceneInterface.h"
#include "RHIStaticStates.h"

static TMap<FSceneInterface*, TArray<FWaterInteractionRendererProxy*>> WaterProxiesByScene;

// Keep these names unique because Unreal's Unity build can concatenate this
// source file with GroundInteractionRendererProxy.cpp into one translation unit.
static constexpr int32 WATER_GROUP_SIZE_X = 8;
static constexpr int32 WATER_GROUP_SIZE_Y = 8;
static constexpr int32 WATER_GROUP_SIZE_Z = 1;
static constexpr int32 WATER_MAX_SPLATS_PER_PASS = WATER_GROUP_SIZE_X * WATER_GROUP_SIZE_Y * WATER_GROUP_SIZE_Z;

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
		OutEnvironment.SetDefine(TEXT("WI_THREADGROUP_SIZE_X"), WATER_GROUP_SIZE_X);
		OutEnvironment.SetDefine(TEXT("WI_THREADGROUP_SIZE_Y"), WATER_GROUP_SIZE_Y);
		OutEnvironment.SetDefine(TEXT("WI_THREADGROUP_SIZE_Z"), WATER_GROUP_SIZE_Z);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, NumCells)
		SHADER_PARAMETER(FIntPoint, ScrollPixels)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, InputHeightTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputHeightTexture)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FWaterScrollingCS, "/Plugin/GroundInteraction/GroundInteractionWaterScrolling.usf", "MainCS", SF_Compute);

class FWaterHeightSimulationCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FWaterHeightSimulationCS);
	SHADER_USE_PARAMETER_STRUCT(FWaterHeightSimulationCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("WI_THREADGROUP_SIZE_X"), WATER_GROUP_SIZE_X);
		OutEnvironment.SetDefine(TEXT("WI_THREADGROUP_SIZE_Y"), WATER_GROUP_SIZE_Y);
		OutEnvironment.SetDefine(TEXT("WI_THREADGROUP_SIZE_Z"), WATER_GROUP_SIZE_Z);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, NumCells)
		SHADER_PARAMETER(float, DeltaTimeSeconds)
		SHADER_PARAMETER(float, AdvectAmount)
		SHADER_PARAMETER(float, Damping)

		SHADER_PARAMETER_SAMPLER(SamplerState, PreviousHeightSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, PreviousHeightTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, CurrentHeightTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, NextHeightTexture)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FWaterHeightSimulationCS, "/Plugin/GroundInteraction/GroundInteractionWaterHeightSimulation.usf", "MainCS", SF_Compute);

class FApplyWaterSplatsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FApplyWaterSplatsCS);
	SHADER_USE_PARAMETER_STRUCT(FApplyWaterSplatsCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("WI_THREADGROUP_SIZE_X"), WATER_GROUP_SIZE_X);
		OutEnvironment.SetDefine(TEXT("WI_THREADGROUP_SIZE_Y"), WATER_GROUP_SIZE_Y);
		OutEnvironment.SetDefine(TEXT("WI_THREADGROUP_SIZE_Z"), WATER_GROUP_SIZE_Z);
		OutEnvironment.SetDefine(TEXT("WI_MAX_SPLATS_PER_PASS"), WATER_MAX_SPLATS_PER_PASS);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, NumCells)
		SHADER_PARAMETER(FVector2f, InteractionExtentXY)
		SHADER_PARAMETER(uint32, NumWaterSplats)
		SHADER_PARAMETER(float, PulseStrength)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FWaterSplatRelative>, WaterSplats)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, HeightTexture)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FApplyWaterSplatsCS, "/Plugin/GroundInteraction/GroundInteractionApplyWaterSplats.usf", "MainCS", SF_Compute);

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

static void AddApplyWaterSplatsPass(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FRDGTextureUAVRef HeightTextureUAV,
	const FIntPoint& RenderTargetSize,
	const FVector& InteractionExtentWS,
	float PulseStrength,
	const FWaterSplatRelative* WaterSplats,
	uint32 NumWaterSplats)
{
	check(HeightTextureUAV);
	check(WaterSplats);
	check(NumWaterSplats > 0 && NumWaterSplats <= WATER_MAX_SPLATS_PER_PASS);

	const uint64 WaterSplatDataSize = sizeof(FWaterSplatRelative) * NumWaterSplats;
	FWaterSplatRelative* WaterSplatData = GraphBuilder.AllocPODArray<FWaterSplatRelative>(NumWaterSplats);
	FMemory::Memcpy(WaterSplatData, WaterSplats, WaterSplatDataSize);

	const FRDGBufferDesc WaterSplatBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FWaterSplatRelative), NumWaterSplats);
	FRDGBufferRef WaterSplatBuffer = GraphBuilder.CreateBuffer(WaterSplatBufferDesc, TEXT("WaterSplatsBuffer"), ERDGBufferFlags::None);
	GraphBuilder.QueueBufferUpload(WaterSplatBuffer, WaterSplatData, WaterSplatDataSize, ERDGInitialDataFlags::NoCopy);

	const FIntPoint NumCells(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1));
	FApplyWaterSplatsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplyWaterSplatsCS::FParameters>();
	PassParameters->NumCells = NumCells;
	PassParameters->InteractionExtentXY = FVector2f(static_cast<float>(FMath::Max(InteractionExtentWS.X, 1.0)), static_cast<float>(FMath::Max(InteractionExtentWS.Y, 1.0)));
	PassParameters->NumWaterSplats = NumWaterSplats;
	PassParameters->PulseStrength = FMath::Max(PulseStrength, 0.0f);
	PassParameters->WaterSplats = GraphBuilder.CreateSRV(WaterSplatBuffer);
	PassParameters->HeightTexture = HeightTextureUAV;

	TShaderMapRef<FApplyWaterSplatsCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
	const FIntVector DispatchCount = FComputeShaderUtils::GetGroupCount(FIntVector(NumCells.X, NumCells.Y, 1), FIntVector(WATER_GROUP_SIZE_X, WATER_GROUP_SIZE_Y, WATER_GROUP_SIZE_Z));

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GroundInteraction.ApplyWaterSplats(%u)", NumWaterSplats), ComputeShader, PassParameters, DispatchCount);
}

static void AddWaterHeightSimulationPass(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FRDGTextureSRVRef PreviousHeightTextureSRV,
	FRDGTextureSRVRef CurrentHeightTextureSRV,
	FRDGTextureUAVRef NextHeightTextureUAV,
	const FIntPoint& RenderTargetSize,
	float DeltaTimeSeconds,
	float AdvectAmount,
	float Damping)
{
	FWaterHeightSimulationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FWaterHeightSimulationCS::FParameters>();
	const FIntPoint NumCells(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1));

	PassParameters->NumCells = NumCells;
	PassParameters->DeltaTimeSeconds = DeltaTimeSeconds;
	PassParameters->AdvectAmount = AdvectAmount;
	PassParameters->Damping = Damping;
	PassParameters->PreviousHeightSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->PreviousHeightTexture = PreviousHeightTextureSRV;
	PassParameters->CurrentHeightTexture = CurrentHeightTextureSRV;
	PassParameters->NextHeightTexture = NextHeightTextureUAV;

	TShaderMapRef<FWaterHeightSimulationCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
	const FIntVector DispatchCount = FComputeShaderUtils::GetGroupCount(FIntVector(NumCells.X, NumCells.Y, 1), FIntVector(WATER_GROUP_SIZE_X, WATER_GROUP_SIZE_Y, WATER_GROUP_SIZE_Z));

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GroundInteraction.WaterHeightSimulation"), ComputeShader, PassParameters, DispatchCount);
}

FWaterInteractionRendererProxy::FWaterInteractionRendererProxy(
	FTextureReferenceRHIRef InHeightTextureReferenceRHI,
	FIntVector InNumCell,
	FVector InCellSpacing,
	const FVector& InInteractionExtentWS,
	float InWaterInteractionPulseStrength,
	float InWaterInteractionAdvectAmount,
	float InWaterInteractionDamping,
	float InWaterInteractionMaxDeltaTime)
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
	, WaterInteractionPulseStrength(FMath::Max(InWaterInteractionPulseStrength, 0.0f))
	, WaterInteractionAdvectAmount(FMath::Max(InWaterInteractionAdvectAmount, 0.0f))
	, WaterInteractionDamping(FMath::Max(InWaterInteractionDamping, 0.0f))
	, WaterInteractionMaxDeltaTime(FMath::Max(InWaterInteractionMaxDeltaTime, KINDA_SMALL_NUMBER))
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
	if (bReleased || !InViewFamily.Scene || InViewFamily.Views.Num() == 0)
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

	const FVector2d CurrentSnappedLocationXY(CurrentSnappedLocation.X, CurrentSnappedLocation.Y);

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

				AdvanceHeightTextureAfterScrolling();
				PreviousSnappedLocation = CurrentSnappedLocationXY;
			}
		}
		else
		{
			PreviousSnappedLocation = CurrentSnappedLocationXY;
		}
	}

	// ---- Water Splat pass 실행 ----
	TArray<FWaterSplatWorld> WaterSplatsWorld = MoveTemp(PendingWaterSplats);
	TArray<FWaterSplatRelative> WaterSplats;
	WaterSplats.Reserve(WaterSplatsWorld.Num());

	for (const FWaterSplatWorld& WaterSplatWorld : WaterSplatsWorld)
	{
		const FVector2d RelativeCenterXY = WaterSplatWorld.AbsoluteCenterXY - CurrentSnappedLocationXY;
		FWaterSplatRelative& WaterSplat = WaterSplats.AddDefaulted_GetRef();
		WaterSplat.RelativeCenterXY = FVector2f(static_cast<float>(RelativeCenterXY.X), static_cast<float>(RelativeCenterXY.Y));
		WaterSplat.HalfSizeXY = FVector2f(static_cast<float>(WaterSplatWorld.HalfSizeXY.X), static_cast<float>(WaterSplatWorld.HalfSizeXY.Y));
		WaterSplat.RotationCos = WaterSplatWorld.RotationCos;
		WaterSplat.RotationSin = WaterSplatWorld.RotationSin;
		WaterSplat.ShapeType = WaterSplatWorld.ShapeType;
		WaterSplat.Padding = WaterSplatWorld.Padding;
	}

	if (!WaterSplats.IsEmpty())
	{
		FPlaneTexture& CurrentHeight = HeightTextures[GetCurrentHeightTextureId()];
		FRDGTextureRef CurrentHeightTexture = CurrentHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterSplats_CurrentHeight"));
		FRDGTextureUAVRef CurrentHeightUAV = CurrentHeight.GetOrCreateUAV(GraphBuilder, TEXT("WaterSplats_CurrentHeight"));

		if (CurrentHeightTexture && CurrentHeightUAV)
		{
			GraphBuilder.UseInternalAccessMode(CurrentHeightTexture);

			for (int32 StartIndex = 0; StartIndex < WaterSplats.Num(); StartIndex += WATER_MAX_SPLATS_PER_PASS)
			{
				const int32 NumWaterSplatsInPass = FMath::Min(WaterSplats.Num() - StartIndex, WATER_MAX_SPLATS_PER_PASS);
				AddApplyWaterSplatsPass(
					GraphBuilder,
					PrimaryView->GetFeatureLevel(),
					CurrentHeightUAV,
					RenderTargetSize,
					InteractionExtentWS,
					WaterInteractionPulseStrength,
					WaterSplats.GetData() + StartIndex,
					static_cast<uint32>(NumWaterSplatsInPass));
			}
		}
	}

	// ---- Water Height Simulation Pass 실행 ----
	const float SimulationDeltaTime = FMath::Clamp(
		InViewFamily.Time.GetDeltaRealTimeSeconds(),
		0.0f,
		WaterInteractionMaxDeltaTime);

	if (SimulationDeltaTime > 0.0f)
	{
		const int32 SimulationOutputHeightTextureId = GetNextHeightTextureId();

		FPlaneTexture& PreviousHeight = HeightTextures[GetPreviousHeightTextureId()];
		FPlaneTexture& CurrentHeight = HeightTextures[GetCurrentHeightTextureId()];
		FPlaneTexture& NextHeight = HeightTextures[SimulationOutputHeightTextureId];

		FRDGTextureRef PreviousHeightTexture = PreviousHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterHeightSimulation_PreviousHeight"));
		FRDGTextureRef CurrentHeightTexture = CurrentHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterHeightSimulation_CurrentHeight"));
		FRDGTextureRef NextHeightTexture = NextHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterHeightSimulation_NextHeight"));

		FRDGTextureSRVRef PreviousHeightSRV = PreviousHeight.GetOrCreateSRV(GraphBuilder, TEXT("WaterHeightSimulation_PreviousHeight"));
		FRDGTextureSRVRef CurrentHeightSRV = CurrentHeight.GetOrCreateSRV(GraphBuilder, TEXT("WaterHeightSimulation_CurrentHeight"));
		FRDGTextureUAVRef NextHeightUAV = NextHeight.GetOrCreateUAV(GraphBuilder, TEXT("WaterHeightSimulation_NextHeight"));

		if (PreviousHeightTexture && CurrentHeightTexture && NextHeightTexture &&
			PreviousHeightSRV && CurrentHeightSRV && NextHeightUAV)
		{
			GraphBuilder.UseInternalAccessMode(PreviousHeightTexture);
			GraphBuilder.UseInternalAccessMode(CurrentHeightTexture);
			GraphBuilder.UseInternalAccessMode(NextHeightTexture);

			AddWaterHeightSimulationPass(
				GraphBuilder,
				PrimaryView->GetFeatureLevel(),
				PreviousHeightSRV,
				CurrentHeightSRV,
				NextHeightUAV,
				RenderTargetSize,
				SimulationDeltaTime,
				WaterInteractionAdvectAmount,
				WaterInteractionDamping);

			CopyHeightToOutput(GraphBuilder, SimulationOutputHeightTextureId);
			AdvanceHeightTextureAfterSimulation();
		}
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

void FWaterInteractionRendererProxy::CopyHeightToOutput(FRDGBuilder& GraphBuilder, int32 SourceHeightTextureId)
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

	if (SourceHeightTextureId < 0 || SourceHeightTextureId >= 3)
	{
		return;
	}

	FPlaneTexture& SourceHeight = HeightTextures[SourceHeightTextureId];

	FRDGTextureRef SourceHeightTexture = SourceHeight.GetOrCreateTexture(GraphBuilder, TEXT("Copy_WaterHeight"));
	FRDGTextureRef OutputHeightTexture = RegisterExternalTexture(GraphBuilder, OutputHeightTextureRHI, TEXT("Output_WaterHeight"));

	if (!SourceHeightTexture || !OutputHeightTexture)
	{
		return;
	}

	GraphBuilder.UseInternalAccessMode(SourceHeightTexture);
	GraphBuilder.UseInternalAccessMode(OutputHeightTexture);

	AddCopyTexturePass(GraphBuilder, SourceHeightTexture, OutputHeightTexture);

	GraphBuilder.UseExternalAccessMode(OutputHeightTexture, ERHIAccess::SRVMask);
}
