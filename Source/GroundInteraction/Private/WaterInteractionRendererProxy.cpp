#include "WaterInteractionRendererProxy.h"
#include "GroundInteractionCommon.h"
#include "WaterInteractionCommon.h"

#include "GlobalShader.h"
#include "RenderTargetPool.h"
#include "RenderGraphUtils.h"
#include "Runtime/Engine/Public/SceneInterface.h"
#include "RHIStaticStates.h"

static TMap<FSceneInterface*, TArray<FWaterInteractionRendererProxy*>> WaterProxiesByScene;

DECLARE_GPU_STAT_NAMED(GroundInteractionWater, TEXT("GroundInteraction Water"));
DECLARE_GPU_STAT_NAMED(GroundInteractionWaterClear, TEXT("GroundInteraction Water Clear"));
DECLARE_GPU_STAT_NAMED(GroundInteractionWaterScrolling, TEXT("GroundInteraction Water Scrolling"));
DECLARE_GPU_STAT_NAMED(GroundInteractionWaterSplats, TEXT("GroundInteraction Water Splats"));
DECLARE_GPU_STAT_NAMED(GroundInteractionWaterSimulation, TEXT("GroundInteraction Water Simulation"));
DECLARE_GPU_STAT_NAMED(GroundInteractionWaterOutputCopy, TEXT("GroundInteraction Water Output Copy"));

static constexpr int32 WATER_GROUP_SIZE_X = 8;
static constexpr int32 WATER_GROUP_SIZE_Y = 8;
static constexpr int32 WATER_GROUP_SIZE_Z = 1;
static constexpr int32 WATER_MAX_SPLATS_PER_PASS = WATER_GROUP_SIZE_X * WATER_GROUP_SIZE_Y * WATER_GROUP_SIZE_Z;
static constexpr double WATER_RENDER_IDLE_DELAY_SECONDS = 10.0;

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
		SHADER_PARAMETER(FVector2f, InteractionExtentXY)
		SHADER_PARAMETER(float, DeltaTimeSeconds)
		SHADER_PARAMETER(float, AdvectAmount)
		SHADER_PARAMETER(float, Damping)

		SHADER_PARAMETER_SAMPLER(SamplerState, CurrentHeightSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, CurrentHeightTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, PreviousHeightTexture)
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
	FRDGTextureSRVRef CurrentHeightTextureSRV,
	FRDGTextureSRVRef PreviousHeightTextureSRV,
	FRDGTextureUAVRef NextHeightTextureUAV,
	const FIntPoint& RenderTargetSize,
	const FVector2f& InteractionExtentXY,
	float DeltaTimeSeconds,
	float AdvectAmount,
	float Damping)
{
	FWaterHeightSimulationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FWaterHeightSimulationCS::FParameters>();
	const FIntPoint NumCells(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1));

	PassParameters->NumCells = NumCells;
	PassParameters->InteractionExtentXY = InteractionExtentXY;
	PassParameters->DeltaTimeSeconds = DeltaTimeSeconds;
	PassParameters->AdvectAmount = AdvectAmount;
	PassParameters->Damping = Damping;
	PassParameters->CurrentHeightSampler = TStaticSamplerState<SF_Point, AM_Border, AM_Border, AM_Border, 0, 1, 0>::GetRHI();
	PassParameters->CurrentHeightTexture = CurrentHeightTextureSRV;
	PassParameters->PreviousHeightTexture = PreviousHeightTextureSRV;
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
	int32 InWaterInteractionSimulationRate,
	int32 InWaterInteractionMaxSubstepsPerFrame)
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
	, WaterInteractionFixedDeltaTime(1.0 / static_cast<double>(FMath::Max(InWaterInteractionSimulationRate, 1)))
	, WaterInteractionMaxSubstepsPerFrame(FMath::Max(InWaterInteractionMaxSubstepsPerFrame, 1))
	, SimulationAccumulatorSeconds(0.0)
	, WaterRenderTimeRemainingSeconds(0.0)
	, HeightTextures()
	, CurrentHeightTextureId(0)
	, PreviousHeightTextureId(1)
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

	const double FrameDeltaTimeSeconds = FMath::Max(InViewFamily.Time.GetDeltaRealTimeSeconds(), 0.0);

	// Splat 이 일정 기간 없을 때 Render() 전체 과정 조기 리턴하여 스킵
	if (PendingWaterSplats.IsEmpty())
	{
		WaterRenderTimeRemainingSeconds = FMath::Max(WaterRenderTimeRemainingSeconds - FrameDeltaTimeSeconds, 0.0);

		if (WaterRenderTimeRemainingSeconds <= 0.0)
		{
			SimulationAccumulatorSeconds = 0.0;
			return;
		}
	}
	else
	{
		WaterRenderTimeRemainingSeconds = WATER_RENDER_IDLE_DELAY_SECONDS;
	}

	RDG_EVENT_SCOPE_STAT(GraphBuilder, GroundInteractionWater, "GroundInteraction::Water");

	ResetHeightTransientRefs();

	if (bRenderTargetChanged)
	{
		bRenderTargetChanged = false;
		bHasPreviousLocation = false;
		PreviousSnappedLocation = FVector2D::ZeroVector;

		ResetHeightTextureRoles();
		bClearHeightTextures = true;
		SimulationAccumulatorSeconds = 0.0;
	}

	bool bShouldUpdateOutputHeight = CheckHeightTextures(GraphBuilder, RenderTargetSize, bClearHeightTextures);

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
			FPlaneTexture& CurrentHeight = HeightTextures[GetCurrentHeightTextureId()];
			FPlaneTexture& PreviousHeight = HeightTextures[GetPreviousHeightTextureId()];
			FPlaneTexture& NextHeight = HeightTextures[GetNextHeightTextureId()];

			FRDGTextureRef CurrentHeightTexture = CurrentHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterScrolling_CurrentHeight"));
			FRDGTextureRef PreviousHeightTexture = PreviousHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterScrolling_PreviousHeight"));
			FRDGTextureRef NextHeightTexture = NextHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterScrolling_NextHeight"));

			FRDGTextureSRVRef CurrentHeightSRV = CurrentHeight.GetOrCreateSRV(GraphBuilder, TEXT("WaterScrolling_CurrentHeight"));
			FRDGTextureSRVRef PreviousHeightSRV = PreviousHeight.GetOrCreateSRV(GraphBuilder, TEXT("WaterScrolling_PreviousHeight"));

			FRDGTextureUAVRef PreviousHeightUAV = PreviousHeight.GetOrCreateUAV(GraphBuilder, TEXT("WaterScrolling_PreviousHeight"));
			FRDGTextureUAVRef NextHeightUAV = NextHeight.GetOrCreateUAV(GraphBuilder, TEXT("WaterScrolling_NextHeight"));

			if (CurrentHeightTexture && PreviousHeightTexture && NextHeightTexture &&
				CurrentHeightSRV && PreviousHeightSRV &&
				PreviousHeightUAV && NextHeightUAV)
			{
				RDG_EVENT_SCOPE_STAT(GraphBuilder, GroundInteractionWaterScrolling, "GroundInteraction::WaterScrolling");

				GraphBuilder.UseInternalAccessMode(CurrentHeightTexture);
				GraphBuilder.UseInternalAccessMode(PreviousHeightTexture);
				GraphBuilder.UseInternalAccessMode(NextHeightTexture);

				AddWaterScrollingPass(
					GraphBuilder,
					PrimaryView->GetFeatureLevel(),
					PreviousHeightSRV,
					NextHeightUAV,
					RenderTargetSize,
					ScrollPixels);

				AddWaterScrollingPass(
					GraphBuilder,
					PrimaryView->GetFeatureLevel(),
					CurrentHeightSRV,
					PreviousHeightUAV,
					RenderTargetSize,
					ScrollPixels);

				AdvanceHeightTextureAfterScrolling();
				bShouldUpdateOutputHeight = true;
				PreviousSnappedLocation = CurrentSnappedLocationXY;
			}
		}
		else
		{
			PreviousSnappedLocation = CurrentSnappedLocationXY;
		}
	}
	
	const double FixedDeltaTimeSeconds = FMath::Max(static_cast<double>(WaterInteractionFixedDeltaTime), static_cast<double>(KINDA_SMALL_NUMBER));
	const double MaxAccumulatedTimeSeconds = FixedDeltaTimeSeconds * static_cast<double>(WaterInteractionMaxSubstepsPerFrame);
	SimulationAccumulatorSeconds = FMath::Min(SimulationAccumulatorSeconds + FrameDeltaTimeSeconds, MaxAccumulatedTimeSeconds);

	const int32 SimulationSubstepCount = FMath::Min(FMath::FloorToInt(SimulationAccumulatorSeconds / FixedDeltaTimeSeconds), WaterInteractionMaxSubstepsPerFrame);

	// 시뮬레이션을 실행할 만큼 델타 타임이 누적되지 않았으면 스크롤링한 결과를 렌더 타겟에 복사하기만 한 후 리턴
	if (SimulationSubstepCount <= 0)
	{
		if (bShouldUpdateOutputHeight)
		{
			CopyHeightToOutput(GraphBuilder, GetCurrentHeightTextureId());
		}

		return;
	}

	// ---- Water Splat Pass 실행 ----
	// 시뮬레이션이 실행되는 프레임때만 Splat Pass 가 실행됨
	if (!PendingWaterSplats.IsEmpty())
	{
		FPlaneTexture& PreviousHeight = HeightTextures[GetPreviousHeightTextureId()];
		FRDGTextureRef PreviousHeightTexture = PreviousHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterSplats_PreviousHeight"));
		FRDGTextureUAVRef PreviousHeightUAV = PreviousHeight.GetOrCreateUAV(GraphBuilder, TEXT("WaterSplats_PreviousHeight"));

		if (PreviousHeightTexture && PreviousHeightUAV)
		{
			TArray<FWaterSplatRelative> WaterSplats;
			WaterSplats.Reserve(PendingWaterSplats.Num());

			for (const FWaterSplatWorld& WaterSplatWorld : PendingWaterSplats)
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

			GraphBuilder.UseInternalAccessMode(PreviousHeightTexture);

			{
				RDG_EVENT_SCOPE_STAT(GraphBuilder, GroundInteractionWaterSplats, "GroundInteraction::WaterSplats");

				for (int32 StartIndex = 0; StartIndex < WaterSplats.Num(); StartIndex += WATER_MAX_SPLATS_PER_PASS)
				{
					const int32 NumWaterSplatsInPass = FMath::Min(WaterSplats.Num() - StartIndex, WATER_MAX_SPLATS_PER_PASS);
					AddApplyWaterSplatsPass(
						GraphBuilder,
						PrimaryView->GetFeatureLevel(),
						PreviousHeightUAV,
						RenderTargetSize,
						InteractionExtentWS,
						WaterInteractionPulseStrength,
						WaterSplats.GetData() + StartIndex,
						static_cast<uint32>(NumWaterSplatsInPass));
				}
			}

			PendingWaterSplats.Reset();
		}
	}

	int32 ExecutedSimulationSubstepCount = 0;

	// ---- Water Height Simulation 실행 ----
	for (int32 SubstepIndex = 0; SubstepIndex < SimulationSubstepCount; ++SubstepIndex)
	{
		FPlaneTexture& CurrentHeight = HeightTextures[GetCurrentHeightTextureId()];
		FPlaneTexture& PreviousHeight = HeightTextures[GetPreviousHeightTextureId()];
		FPlaneTexture& NextHeight = HeightTextures[GetNextHeightTextureId()];

		FRDGTextureRef CurrentHeightTexture = CurrentHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterHeightSimulation_CurrentHeight"));
		FRDGTextureRef PreviousHeightTexture = PreviousHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterHeightSimulation_PreviousHeight"));
		FRDGTextureRef NextHeightTexture = NextHeight.GetOrCreateTexture(GraphBuilder, TEXT("WaterHeightSimulation_NextHeight"));

		FRDGTextureSRVRef CurrentHeightSRV = CurrentHeight.GetOrCreateSRV(GraphBuilder, TEXT("WaterHeightSimulation_CurrentHeight"));
		FRDGTextureSRVRef PreviousHeightSRV = PreviousHeight.GetOrCreateSRV(GraphBuilder, TEXT("WaterHeightSimulation_PreviousHeight"));
		FRDGTextureUAVRef NextHeightUAV = NextHeight.GetOrCreateUAV(GraphBuilder, TEXT("WaterHeightSimulation_NextHeight"));

		if (!CurrentHeightTexture || !PreviousHeightTexture || !NextHeightTexture ||
			!CurrentHeightSRV || !PreviousHeightSRV || !NextHeightUAV)
		{
			break;
		}

		GraphBuilder.UseInternalAccessMode(CurrentHeightTexture);
		GraphBuilder.UseInternalAccessMode(PreviousHeightTexture);
		GraphBuilder.UseInternalAccessMode(NextHeightTexture);

		{
			RDG_EVENT_SCOPE_STAT(GraphBuilder, GroundInteractionWaterSimulation, "GroundInteraction::WaterSimulation");

			AddWaterHeightSimulationPass(
				GraphBuilder,
				PrimaryView->GetFeatureLevel(),
				CurrentHeightSRV,
				PreviousHeightSRV,
				NextHeightUAV,
				RenderTargetSize,
				FVector2f(static_cast<float>(InteractionExtentWS.X), static_cast<float>(InteractionExtentWS.Y)),
				static_cast<float>(FixedDeltaTimeSeconds),
				WaterInteractionAdvectAmount,
				WaterInteractionDamping);
		}

		AdvanceHeightTextureAfterSimulation();
		bShouldUpdateOutputHeight = true;
		++ExecutedSimulationSubstepCount;
	}

	SimulationAccumulatorSeconds = FMath::Max(SimulationAccumulatorSeconds - FixedDeltaTimeSeconds * static_cast<double>(ExecutedSimulationSubstepCount), 0.0);

	if (bShouldUpdateOutputHeight)
	{
		CopyHeightToOutput(GraphBuilder, GetCurrentHeightTextureId());
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

bool FWaterInteractionRendererProxy::CheckHeightTextures(FRDGBuilder& GraphBuilder, const FIntPoint& InRenderTargetSize, bool bClearAll)
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
		return false;
	}

	RDG_EVENT_SCOPE_STAT(GraphBuilder, GroundInteractionWaterClear, "GroundInteraction::WaterClear");

	ResetHeightTransientRefs();

	bool bClearedAny = false;

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
		bClearedAny = true;

		GraphBuilder.SetTextureAccessFinal(HeightTexture, ERHIAccess::SRVMask);
	}

	return bClearedAny;
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

	RDG_EVENT_SCOPE_STAT(GraphBuilder, GroundInteractionWaterOutputCopy, "GroundInteraction::WaterOutputCopy");

	AddCopyTexturePass(GraphBuilder, SourceHeightTexture, OutputHeightTexture);

	GraphBuilder.UseExternalAccessMode(OutputHeightTexture, ERHIAccess::SRVMask);
}
