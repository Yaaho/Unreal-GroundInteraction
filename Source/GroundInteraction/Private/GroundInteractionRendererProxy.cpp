// Fill out your copyright notice in the Description page of Project Settings.

#include "GroundInteractionRendererProxy.h"
#include "GroundInteractionCommon.h" 

#include "RenderTargetPool.h"
#include "Rendering/CustomRenderPass.h"
#include "Runtime/Renderer/Private/SceneRendering.h"
#include "Runtime\Engine\Public\SceneInterface.h"

#include "GlobalShader.h"

static TMap<FSceneInterface*, TArray<FGroundInteractionRendererProxy*>> ProxiesByScene;

DECLARE_GPU_STAT_NAMED(GroundInteractionGround, TEXT("GroundInteraction Ground"));
DECLARE_GPU_STAT_NAMED(GroundInteractionGroundScrolling, TEXT("GroundInteraction Ground Scrolling"));
DECLARE_GPU_STAT_NAMED(GroundInteractionGroundFade, TEXT("GroundInteraction Ground Fade"));
DECLARE_GPU_STAT_NAMED(GroundInteractionGroundUpdateDepth, TEXT("GroundInteraction Ground Update Depth"));
DECLARE_GPU_STAT_NAMED(GroundInteractionGroundOutputCopy, TEXT("GroundInteraction Ground Output Copy"));

static constexpr int32 GROUND_GROUP_SIZE_X = 8;
static constexpr int32 GROUND_GROUP_SIZE_Y = 8;
static constexpr int32 GROUND_GROUP_SIZE_Z = 1;

static constexpr int32 DEPTH_FADE_CHANNEL_DEPTH = 0;
static constexpr int32 DEPTH_FADE_CHANNEL_FADE = 1;

static constexpr EPixelFormat DEPTH_FADE_PIXEL_FORMAT = PF_R16F;


// Previous 렌더 타겟을 읽어서 지정한 픽셀만큼 이동시키는 셰이더
class FGroundScrollingCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGroundScrollingCS);
	SHADER_USE_PARAMETER_STRUCT(FGroundScrollingCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_X"), GROUND_GROUP_SIZE_X);
		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_Y"), GROUND_GROUP_SIZE_Y);
		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_Z"), GROUND_GROUP_SIZE_Z);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, NumCells)
		SHADER_PARAMETER(FIntPoint, ScrollPixels)
		SHADER_PARAMETER(float, PrevCaptureViewOriginZ)
		SHADER_PARAMETER(float, CurrentCaptureViewOriginZ)
		SHADER_PARAMETER(float, MaxSceneDepth)
		SHADER_PARAMETER(float, SceneDepthInvalidEpsilon)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, InputDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, InputFadeTexture)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputFadeTexture)
	END_SHADER_PARAMETER_STRUCT()
};


// Depth Fade를 진행시키는 셰이더
class FFadeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFadeCS);
	SHADER_USE_PARAMETER_STRUCT(FFadeCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_X"), GROUND_GROUP_SIZE_X);
		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_Y"), GROUND_GROUP_SIZE_Y);
		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_Z"), GROUND_GROUP_SIZE_Z);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, NumCells)
		SHADER_PARAMETER(float, DeltaTimeSeconds)
		SHADER_PARAMETER(float, FadeRiseSpeed)
		SHADER_PARAMETER(float, FadeMaxHeight)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, FadeTexture)
	END_SHADER_PARAMETER_STRUCT()
};


// depth pass 결과로 depth/fade texture를 업데이트하는 셰이더
class FUpdateDepthCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUpdateDepthCS);
	SHADER_USE_PARAMETER_STRUCT(FUpdateDepthCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_X"), GROUND_GROUP_SIZE_X);
		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_Y"), GROUND_GROUP_SIZE_Y);
		OutEnvironment.SetDefine(TEXT("GI_THREADGROUP_SIZE_Z"), GROUND_GROUP_SIZE_Z);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, NumCells)
		SHADER_PARAMETER(FIntPoint, ScrollPixels)
		SHADER_PARAMETER(float, MaxSceneDepth)
		SHADER_PARAMETER(float, SceneDepthInvalidEpsilon)
		SHADER_PARAMETER(float, DepthOffsetZ)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CurrentDepthTexture)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, FadeTexture)
	END_SHADER_PARAMETER_STRUCT()
};


// interaction depth를 렌더링하는 custom render pass
class FGroundInteractionDepthPass final : public FCustomRenderPassBase
{
public:
	IMPLEMENT_CUSTOM_RENDER_PASS(FGroundInteractionDepthPass);

	FGroundInteractionDepthPass(
		const FIntPoint& InRenderTargetSize,
		FGroundInteractionRendererProxy* InOwnerProxy,
		const FVector& InBottomInteractionCenterWS)
		: FCustomRenderPassBase(
			TEXT("GroundInteractionDepthPass"),
			FCustomRenderPassBase::ERenderMode::DepthPass,
			FCustomRenderPassBase::ERenderOutput::SceneDepth,
			InRenderTargetSize)
		, OwnerProxy(InOwnerProxy)
		, RenderTargetSize(InRenderTargetSize)
		, BottomInteractionCenterWS(InBottomInteractionCenterWS)
	{
	}

	virtual void OnPreRender(FRDGBuilder& GraphBuilder) override
	{
		const FIntPoint SafeSize(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1));

		const bool bNeedsAllocateRenderTarget = 
			!DepthPooledTarget.IsValid() || DepthPooledTarget->GetDesc().Extent != SafeSize || DepthPooledTarget->GetDesc().Format != PF_R32_FLOAT;

		if (bNeedsAllocateRenderTarget)
		{
			FPooledRenderTargetDesc Desc = FPooledRenderTargetDesc::Create2DDesc(
				SafeSize,
				PF_R32_FLOAT,
				FClearValueBinding::Black,
				TexCreate_None,
				TexCreate_RenderTargetable | TexCreate_ShaderResource,
				false);

			Desc.DebugName = TEXT("GI_GroundInteractionDepth");

			GRenderTargetPool.FindFreeElement(GraphBuilder.RHICmdList, Desc, DepthPooledTarget, TEXT("GI_GroundInteractionDepth"));
		}

		if (!DepthPooledTarget.IsValid())
		{
			RenderTargetTexture = nullptr;
			PreRenderGraphBuilder = nullptr;
			return;
		}

		RenderTargetTexture = GraphBuilder.RegisterExternalTexture(DepthPooledTarget, TEXT("GI_GroundInteractionDepth_PreRender"));

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
			DepthPassTexture = GraphBuilder.RegisterExternalTexture(DepthPooledTarget, TEXT("GI_GroundInteractionDepth_PostRender"));
		}

		if (!DepthPassTexture)
		{
			return;
		}
		
		OwnerProxy->StoreGroundInteractionDepthCapture(GraphBuilder, DepthPassTexture, RenderTargetSize, BottomInteractionCenterWS);
	}

private:
	FGroundInteractionRendererProxy* OwnerProxy = nullptr;
	FIntPoint RenderTargetSize = FIntPoint(1, 1);
	FVector BottomInteractionCenterWS = FVector::ZeroVector;

	TRefCountPtr<IPooledRenderTarget> DepthPooledTarget;
	
	FRDGBuilder* PreRenderGraphBuilder = nullptr;
};



IMPLEMENT_GLOBAL_SHADER(FGroundScrollingCS, "/Plugin/GroundInteraction/GroundInteractionGroundScrolling.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFadeCS, "/Plugin/GroundInteraction/GroundInteractionFade.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUpdateDepthCS, "/Plugin/GroundInteraction/GroundInteractionUpdateDepth.usf", "MainCS", SF_Compute);


static void AddGroundScrollingPass(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FRDGTextureSRVRef PreviousDepthSRV,
	FRDGTextureSRVRef PreviousFadeSRV,
	FRDGTextureUAVRef CurrentDepthUAV,
	FRDGTextureUAVRef CurrentFadeUAV,
	const FIntPoint& RenderTargetSize,
	const FIntPoint& ScrollPixels,
	float PrevCaptureViewOriginZ,
	float CurrentCaptureViewOriginZ,
	float MaxSceneDepth,
	float SceneDepthInvalidEpsilon)
{
	FGroundScrollingCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGroundScrollingCS::FParameters>();

	const FIntPoint NumCells(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1));

	PassParameters->NumCells = NumCells;
	PassParameters->ScrollPixels = ScrollPixels;
	PassParameters->PrevCaptureViewOriginZ = PrevCaptureViewOriginZ;
	PassParameters->CurrentCaptureViewOriginZ = CurrentCaptureViewOriginZ;
	PassParameters->MaxSceneDepth = MaxSceneDepth;
	PassParameters->SceneDepthInvalidEpsilon = SceneDepthInvalidEpsilon;

	PassParameters->InputDepthTexture = PreviousDepthSRV;
	PassParameters->InputFadeTexture = PreviousFadeSRV;
	PassParameters->OutputDepthTexture = CurrentDepthUAV;
	PassParameters->OutputFadeTexture = CurrentFadeUAV;

	TShaderMapRef<FGroundScrollingCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));

	const FIntVector DispatchCount = FComputeShaderUtils::GetGroupCount(
			FIntVector(NumCells.X, NumCells.Y, 1),
			FIntVector(GROUND_GROUP_SIZE_X, GROUND_GROUP_SIZE_Y, GROUND_GROUP_SIZE_Z));

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GroundInteraction.GroundScrolling"), ComputeShader, PassParameters, DispatchCount);
}


static void AddFadePass(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FRDGTextureUAVRef DepthTextureUAV,
	FRDGTextureUAVRef FadeTextureUAV,
	const FIntPoint& RenderTargetSize,
	float FadeDeltaTimeSeconds,
	float FadeRiseSpeed,
	float FadeMaxHeight)
{
	FFadeCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFadeCS::FParameters>();

	const FIntPoint NumCells(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1));

	PassParameters->NumCells = NumCells;
	PassParameters->DeltaTimeSeconds = FadeDeltaTimeSeconds;
	PassParameters->FadeRiseSpeed = FadeRiseSpeed;
	PassParameters->FadeMaxHeight = FadeMaxHeight;
	PassParameters->DepthTexture = DepthTextureUAV;
	PassParameters->FadeTexture = FadeTextureUAV;

	TShaderMapRef<FFadeCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));

	const FIntVector DispatchCount = FComputeShaderUtils::GetGroupCount(
			FIntVector(NumCells.X, NumCells.Y, 1),
			FIntVector(GROUND_GROUP_SIZE_X, GROUND_GROUP_SIZE_Y, GROUND_GROUP_SIZE_Z));

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GroundInteraction.Fade"), ComputeShader, PassParameters, DispatchCount);
}


static void AddUpdateDepthPass(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FRDGTextureUAVRef DepthTextureUAV,
	FRDGTextureUAVRef FadeTextureUAV,
	FRDGTextureRef DepthPassTexture,
	const FIntPoint& RenderTargetSize,
	const FIntPoint& ScrollPixels,
	float MaxSceneDepth,
	float SceneDepthInvalidEpsilon,
	float DepthOffsetZ)
{
	FUpdateDepthCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FUpdateDepthCS::FParameters>();

	const FIntPoint NumCells(FMath::Max(RenderTargetSize.X, 1), FMath::Max(RenderTargetSize.Y, 1));

	PassParameters->NumCells = NumCells;
	PassParameters->ScrollPixels = ScrollPixels;
	PassParameters->MaxSceneDepth = MaxSceneDepth;
	PassParameters->SceneDepthInvalidEpsilon = SceneDepthInvalidEpsilon;
	PassParameters->DepthOffsetZ = DepthOffsetZ;
	PassParameters->CurrentDepthTexture = DepthPassTexture;
	PassParameters->DepthTexture = DepthTextureUAV;
	PassParameters->FadeTexture = FadeTextureUAV;

	TShaderMapRef<FUpdateDepthCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));

	const FIntVector DispatchCount = FComputeShaderUtils::GetGroupCount(
		FIntVector(NumCells.X, NumCells.Y, 1),
		FIntVector(GROUND_GROUP_SIZE_X, GROUND_GROUP_SIZE_Y, GROUND_GROUP_SIZE_Z));
	
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GroundInteraction.UpdateDepth"), ComputeShader, PassParameters, DispatchCount);
}


FGroundInteractionRendererProxy::FGroundInteractionRendererProxy(
	FTextureReferenceRHIRef InDepthTextureReferenceRHI,
	FTextureReferenceRHIRef InFadeTextureReferenceRHI,
	FIntVector InNumCell,
	FVector InCellSpacing,
	const FVector& InInteractionExtentWS,
	float InFadeRiseSpeed,
	float InFadeMaxHeight,
	float InFadeUpdateIntervalSeconds)
	: CurrentSnappedLocation(FVector::ZeroVector)
	, RenderTargetSize(
		FMath::Max(InNumCell.X, 1),
		FMath::Max(InNumCell.Y, 1))
	, bRenderTargetChanged(false)
	, OutputDepthTextureReferenceRHI(InDepthTextureReferenceRHI)
	, OutputFadeTextureReferenceRHI(InFadeTextureReferenceRHI)
	, CellSpacing(InCellSpacing)
	, PreviousSnappedLocation(FVector::ZeroVector)
	, bHasPreviousCapture(false)
	, InteractionExtentWS(
		FMath::Max(InInteractionExtentWS.X, 1.0),
		FMath::Max(InInteractionExtentWS.Y, 1.0),
		FMath::Max(InInteractionExtentWS.Z, 1.0))
	, FadeRiseSpeed(FMath::Max(InFadeRiseSpeed, 0.0f))
	, FadeMaxHeight(FMath::Max(InFadeMaxHeight, 0.0f))
	, FadeUpdateIntervalSeconds(FMath::Max(InFadeUpdateIntervalSeconds, KINDA_SMALL_NUMBER))
	, DepthFadeTextures()
	, CurrentStateIndex(0)
	, FadeUpdateAccumulatorSeconds(0.0f)
	, bReleased(false)
	, bClearDepthFadeTextures(true)
{
}


void FGroundInteractionRendererProxy::Render(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	check(IsInRenderingThread());

	if (bReleased || !InViewFamily.Scene || InViewFamily.Views.Num() == 0)
	{
		return;
	}

	const FSceneView* PrimaryView = InViewFamily.Views[0];

	if (!PrimaryView || PrimaryView->bIsSceneCapture || PrimaryView->bIsReflectionCapture)
	{
		return;
	}

	if (!OutputDepthTextureReferenceRHI.IsValid() || !OutputFadeTextureReferenceRHI.IsValid())
	{
		return;
	}

	if (RenderTargetSize.X <= 0 || RenderTargetSize.Y <= 0)
	{
		return;
	}

	RDG_EVENT_SCOPE_STAT(GraphBuilder, GroundInteractionGround, "GroundInteraction::Ground");
	
	// 새 RDG graph 진입 전 이전 graph의 transient RDG ref를 비운다.
	ResetDepthFadeTransientRefs();
	
	// 렌더 타겟 사이즈가 바뀌었으면 이전 상태를 버리는 변수들을 초기화한다.
	if (bRenderTargetChanged)
	{
		bRenderTargetChanged = false;
		CurrentStateIndex = 0;
		FadeUpdateAccumulatorSeconds = 0.0f;

		bHasPreviousCapture = false;
		PreviousSnappedLocation = FVector::ZeroVector;
		bHasDepthCapture = false;
		CaptureBottomInteractionCenterWS = FVector::ZeroVector;

		bClearDepthFadeTextures = true;
	}
	
	// 렌더 타겟 크기가 바뀌었으면 DepthFadeTextures가 초기화된다.
	CheckDepthFadeTextures(GraphBuilder, RenderTargetSize, bClearDepthFadeTextures);

	bClearDepthFadeTextures = false;

	// 이번 Render 호출에서 사용할 delta time.
	// Fade Pass 실행 여부를 판단하는 accumulator 누적에 사용한다.
	const float DeltaTimeSeconds = FMath::Max(InViewFamily.Time.GetDeltaRealTimeSeconds(), 0.0f);

	// 현재 프레임의 bottom interaction center 위치
	const FVector CurrentBottomInteractionCenterWS = GetBottomInteractionCenterWS();

	// 이전 bottom interaction center 위치
	const FVector PreviousBottomInteractionCenterWS = bHasPreviousCapture ? GetPreviousBottomInteractionCenterWS() : CurrentBottomInteractionCenterWS;
	
	// 이전 프레임 중심 위치와 현재 프레임 중심 위치를 비교해 몇 픽셀만큼 이동했는지 저장한다.
	FIntPoint ScrollPixels = FIntPoint::ZeroValue;
	
	if (bHasPreviousCapture)
	{
		const FVector2D UnitsPerPixel = GroundInteractionCommon::GetUnitsPerPixel(InteractionExtentWS, RenderTargetSize);
		ScrollPixels.X = FMath::RoundToInt((CurrentSnappedLocation.X - PreviousSnappedLocation.X) / UnitsPerPixel.X);
		ScrollPixels.Y =- FMath::RoundToInt((CurrentSnappedLocation.Y - PreviousSnappedLocation.Y) / UnitsPerPixel.Y);
	}
	
	// ---- Scrolling Pass 실행 ----
	if (bHasPreviousCapture && ScrollPixels != FIntPoint::ZeroValue)
	{
		FPlaneTexture& PreviousDepth = DepthFadeTextures[GetPrevDepthFadeTextureId()][DEPTH_FADE_CHANNEL_DEPTH];
		FPlaneTexture& PreviousFade = DepthFadeTextures[GetPrevDepthFadeTextureId()][DEPTH_FADE_CHANNEL_FADE];
		FPlaneTexture& CurrentDepth = DepthFadeTextures[GetCurrentDepthFadeTextureId()][DEPTH_FADE_CHANNEL_DEPTH];
		FPlaneTexture& CurrentFade = DepthFadeTextures[GetCurrentDepthFadeTextureId()][DEPTH_FADE_CHANNEL_FADE];

		FRDGTextureRef PreviousDepthTexture = PreviousDepth.GetOrCreateTexture(GraphBuilder, TEXT("GroundScrolling_PreviousDepth"));
		FRDGTextureRef PreviousFadeTexture = PreviousFade.GetOrCreateTexture(GraphBuilder, TEXT("GroundScrolling_PreviousFade"));
		
		FRDGTextureSRVRef PreviousDepthSRV = PreviousDepth.GetOrCreateSRV(GraphBuilder, TEXT("GroundScrolling_PreviousDepth"));
		FRDGTextureSRVRef PreviousFadeSRV = PreviousFade.GetOrCreateSRV(GraphBuilder, TEXT("GroundScrolling_PreviousFade"));

		FRDGTextureRef CurrentDepthTexture = CurrentDepth.GetOrCreateTexture(GraphBuilder, TEXT("GroundScrolling_CurrentDepth"));
		FRDGTextureRef CurrentFadeTexture = CurrentFade.GetOrCreateTexture(GraphBuilder, TEXT("GroundScrolling_CurrentFade"));

		FRDGTextureUAVRef CurrentDepthUAV = CurrentDepth.GetOrCreateUAV(GraphBuilder, TEXT("GroundScrolling_CurrentDepth"));
		FRDGTextureUAVRef CurrentFadeUAV = CurrentFade.GetOrCreateUAV(GraphBuilder, TEXT("GroundScrolling_CurrentFade"));

		if (PreviousDepthTexture && PreviousFadeTexture &&
			PreviousDepthSRV && PreviousFadeSRV &&
			CurrentDepthTexture && CurrentFadeTexture &&
			CurrentDepthUAV && CurrentFadeUAV)
		{
			GraphBuilder.UseInternalAccessMode(PreviousDepthTexture);
			GraphBuilder.UseInternalAccessMode(PreviousFadeTexture);
			GraphBuilder.UseInternalAccessMode(CurrentDepthTexture);
			GraphBuilder.UseInternalAccessMode(CurrentFadeTexture);

			RDG_EVENT_SCOPE_STAT(GraphBuilder, GroundInteractionGroundScrolling, "GroundInteraction::GroundScrolling");

			AddGroundScrollingPass(
				GraphBuilder,
				PrimaryView->GetFeatureLevel(),
				PreviousDepthSRV,
				PreviousFadeSRV,
				CurrentDepthUAV,
				CurrentFadeUAV,
				RenderTargetSize,
				ScrollPixels,
				PreviousBottomInteractionCenterWS.Z,
				CurrentBottomInteractionCenterWS.Z,
				GetMaxSceneDepth(),
				GetSceneDepthInvalidEpsilon());

			AdvanceDepthFadeTextureId();

			// 이전 snapped location 변경
			PreviousSnappedLocation = CurrentSnappedLocation;
		}
	}

	// ---- Fade Pass 실행 ----
	// FadeUpdateAccumulatorSeconds가 FadeUpdateIntervalSeconds보다 커졌을 때 실행한다.
	FadeUpdateAccumulatorSeconds += DeltaTimeSeconds;

	float FadeDeltaTimeSeconds = 0.0f;

	if (FadeUpdateAccumulatorSeconds >= FadeUpdateIntervalSeconds)
	{
		const int32 FadeStepCount = FMath::Max(FMath::FloorToInt(FadeUpdateAccumulatorSeconds / FadeUpdateIntervalSeconds), 1);
		FadeDeltaTimeSeconds = static_cast<float>(FadeStepCount) * FadeUpdateIntervalSeconds;
		FadeUpdateAccumulatorSeconds -= FadeDeltaTimeSeconds;
	}

	if (FadeDeltaTimeSeconds > 0.0f)
	{
		FPlaneTexture& Depth = DepthFadeTextures[CurrentStateIndex][DEPTH_FADE_CHANNEL_DEPTH];
		FPlaneTexture& Fade = DepthFadeTextures[CurrentStateIndex][DEPTH_FADE_CHANNEL_FADE];

		FRDGTextureRef DepthTexture = Depth.GetOrCreateTexture(GraphBuilder, TEXT("Fade_Depth"));
		FRDGTextureRef FadeTexture = Fade.GetOrCreateTexture(GraphBuilder, TEXT("Fade_Fade"));

		FRDGTextureUAVRef DepthUAV = Depth.GetOrCreateUAV(GraphBuilder, TEXT("Fade_Depth"));
		FRDGTextureUAVRef FadeUAV = Fade.GetOrCreateUAV(GraphBuilder, TEXT("Fade_Fade"));

		if (DepthTexture && FadeTexture && DepthUAV && FadeUAV)
		{
			GraphBuilder.UseInternalAccessMode(DepthTexture);
			GraphBuilder.UseInternalAccessMode(FadeTexture);

			RDG_EVENT_SCOPE_STAT(GraphBuilder, GroundInteractionGroundFade, "GroundInteraction::GroundFade");

			AddFadePass(
				GraphBuilder,
				PrimaryView->GetFeatureLevel(),
				DepthUAV,
				FadeUAV,
				RenderTargetSize,
				FadeDeltaTimeSeconds,
				FadeRiseSpeed,
				FadeMaxHeight);
		}
	}

	// ---- Update Depth Pass 실행 ----
	// 이전 custom depth pass에서 보관한 접촉 결과를 Current Texture 에 반영한다.
	if (bHasDepthCapture && CaptureDepthTexture.AllocatedSize == RenderTargetSize)
	{
		FPlaneTexture& Depth = DepthFadeTextures[CurrentStateIndex][DEPTH_FADE_CHANNEL_DEPTH];
		FPlaneTexture& Fade = DepthFadeTextures[CurrentStateIndex][DEPTH_FADE_CHANNEL_FADE];

		FRDGTextureRef DepthTexture = Depth.GetOrCreateTexture(GraphBuilder, TEXT("UpdateDepth_Depth"));
		FRDGTextureRef FadeTexture = Fade.GetOrCreateTexture(GraphBuilder, TEXT("UpdateDepth_Fade"));
		FRDGTextureRef CapturedDepthTexture = CaptureDepthTexture.GetOrCreateTexture(GraphBuilder, TEXT("UpdateDepth_CapturedDepth"));

		FRDGTextureUAVRef DepthUAV = Depth.GetOrCreateUAV(GraphBuilder, TEXT("UpdateDepth_Depth"));
		FRDGTextureUAVRef FadeUAV = Fade.GetOrCreateUAV(GraphBuilder, TEXT("UpdateDepth_Fade"));

		if (DepthTexture && FadeTexture && CapturedDepthTexture && DepthUAV && FadeUAV)
		{
			const FVector2D UnitsPerPixel = GroundInteractionCommon::GetUnitsPerPixel(InteractionExtentWS, RenderTargetSize);

			const FIntPoint CaptureScrollPixels(
				FMath::RoundToInt((CurrentBottomInteractionCenterWS.X - CaptureBottomInteractionCenterWS.X) / UnitsPerPixel.X),
				-FMath::RoundToInt((CurrentBottomInteractionCenterWS.Y - CaptureBottomInteractionCenterWS.Y) / UnitsPerPixel.Y));

			const float CaptureDepthOffsetZ = static_cast<float>(CaptureBottomInteractionCenterWS.Z - CurrentBottomInteractionCenterWS.Z);

			GraphBuilder.UseInternalAccessMode(DepthTexture);
			GraphBuilder.UseInternalAccessMode(FadeTexture);
			GraphBuilder.UseInternalAccessMode(CapturedDepthTexture);

			RDG_EVENT_SCOPE_STAT(GraphBuilder, GroundInteractionGroundUpdateDepth, "GroundInteraction::GroundUpdateDepth");

			AddUpdateDepthPass(
				GraphBuilder,
				PrimaryView->GetFeatureLevel(),
				DepthUAV,
				FadeUAV,
				CapturedDepthTexture,
				RenderTargetSize,
				CaptureScrollPixels,
				GetMaxSceneDepth(),
				GetSceneDepthInvalidEpsilon(),
				CaptureDepthOffsetZ);

			GraphBuilder.SetTextureAccessFinal(CapturedDepthTexture, ERHIAccess::SRVMask);

			if (!bHasPreviousCapture)
			{
				bHasPreviousCapture = true;
				PreviousSnappedLocation = CurrentSnappedLocation;
			}
		}
	}

	CopyDepthFadeToOutput(GraphBuilder, CurrentStateIndex);
}


void FGroundInteractionRendererProxy::QueueGroundInteractionDepthPass_GameThread(FSceneViewFamily& InViewFamily, const TSet<FPrimitiveComponentId>& InShowOnlyPrimitives, const FVector& BottomInteractionCenterWS, const FIntPoint& DepthRenderTargetSize)
{
	check(IsInGameThread());

	if (!InViewFamily.Scene)
	{
		return;
	}

	const FIntPoint SafeRenderTargetSize(FMath::Max(DepthRenderTargetSize.X, 1), FMath::Max(DepthRenderTargetSize.Y, 1));

	const float HalfExtentX = FMath::Max(InteractionExtentWS.X, 1.0f);
	const float HalfExtentY = FMath::Max(InteractionExtentWS.Y, 1.0f);

	const FVector ViewLookAt = BottomInteractionCenterWS + FVector(0.0f, 0.0f, 1.0f);

	FSceneInterface::FCustomRenderPassRendererInput PassInput;
	PassInput.ViewLocation = BottomInteractionCenterWS;

	PassInput.ViewRotationMatrix = FLookAtMatrix(BottomInteractionCenterWS, ViewLookAt, FVector(0.0f, 1.0f, 0.0f));

	PassInput.ViewRotationMatrix = PassInput.ViewRotationMatrix.RemoveTranslation();

	PassInput.ViewRotationMatrix.RemoveScaling();

	const float MaxDepth = GetMaxSceneDepth();

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

	PassInput.CustomRenderPass = new FGroundInteractionDepthPass(SafeRenderTargetSize, this, BottomInteractionCenterWS);
	
	InViewFamily.Scene->AddCustomRenderPass(&InViewFamily, PassInput);
}


void FGroundInteractionRendererProxy::Release()
{
	check(IsInRenderingThread());

	bReleased = true;
}

void FGroundInteractionRendererProxy::RegisterProxy(FSceneInterface* InScene, FGroundInteractionRendererProxy* InProxy)
{
	check(IsInRenderingThread());

	if (!InScene || !InProxy)
	{
		return;
	}

	ProxiesByScene.FindOrAdd(InScene).AddUnique(InProxy);
}

void FGroundInteractionRendererProxy::UnregisterProxy(FSceneInterface* InScene, FGroundInteractionRendererProxy* InProxy)
{
	check(IsInRenderingThread());

	if (!InScene || !InProxy)
	{
		return;
	}

	TArray<FGroundInteractionRendererProxy*>* Proxies = ProxiesByScene.Find(InScene);

	if (!Proxies)
	{
		return;
	}

	Proxies->RemoveSwap(InProxy);

	if (Proxies->Num() == 0)
	{
		ProxiesByScene.Remove(InScene);
	}
}

TArray<FGroundInteractionRendererProxy*> FGroundInteractionRendererProxy::GetProxies(FSceneInterface* InScene)
{
	check(IsInRenderingThread());

	if (!InScene)
	{
		return {};
	}

	const TArray<FGroundInteractionRendererProxy*>* Proxies = ProxiesByScene.Find(InScene);

	return Proxies ? *Proxies : TArray<FGroundInteractionRendererProxy*>();
}


void FGroundInteractionRendererProxy::StoreGroundInteractionDepthCapture(FRDGBuilder& GraphBuilder, FRDGTextureRef DepthPassTexture, const FIntPoint& PassRenderTargetSize, const FVector& PassBottomInteractionCenterWS)
{
	check(IsInRenderingThread());

	if (bReleased || !DepthPassTexture)
	{
		return;
	}

	const FIntPoint SafeRenderTargetSize(FMath::Max(PassRenderTargetSize.X, 1), FMath::Max(PassRenderTargetSize.Y, 1));

	// CustomRenderPass의 OnPostRender는 Render()와 다른 GraphBuilder로 들어올 수 있으므로
	// 이전 RDG graph에서 만들어진 capture texture의 transient ref를 비운다.
	CaptureDepthTexture.ResetTransientRefs();
	CaptureDepthTexture.CheckPooledTarget(GraphBuilder.RHICmdList, SafeRenderTargetSize, PF_R32_FLOAT, TEXT("GI_CapturedInteractionDepth"));

	FRDGTextureRef CapturedDepthTexture = CaptureDepthTexture.GetOrCreateTexture(GraphBuilder, TEXT("Store_CapturedInteractionDepth"));

	if (!CapturedDepthTexture)
	{
		bHasDepthCapture = false;
		return;
	}

	GraphBuilder.UseInternalAccessMode(CapturedDepthTexture);
	AddCopyTexturePass(GraphBuilder, DepthPassTexture, CapturedDepthTexture);
	GraphBuilder.SetTextureAccessFinal(CapturedDepthTexture, ERHIAccess::SRVMask);

	CaptureBottomInteractionCenterWS = PassBottomInteractionCenterWS;
	bHasDepthCapture = true;
}

void FGroundInteractionRendererProxy::CheckDepthFadeTextures(FRDGBuilder& GraphBuilder, const FIntPoint& InRenderTargetSize, bool bClearAll)
{
	const FIntPoint SafeSize(FMath::Max(InRenderTargetSize.X, 1), FMath::Max(InRenderTargetSize.Y, 1));

	bool bAllocatedAny = false;

	for (int32 TextureId = 0; TextureId < 2; ++TextureId)
	{
		for (int32 ChannelIndex = 0; ChannelIndex < 2; ++ChannelIndex)
		{
			const TCHAR* DebugName = nullptr;

			if (TextureId == 0 && ChannelIndex == DEPTH_FADE_CHANNEL_DEPTH)
			{
				DebugName = TEXT("DepthFade_0_Depth");
			}
			else if (TextureId == 0 && ChannelIndex == DEPTH_FADE_CHANNEL_FADE)
			{
				DebugName = TEXT("DepthFade_0_Fade");
			}
			else if (TextureId == 1 && ChannelIndex == DEPTH_FADE_CHANNEL_DEPTH)
			{
				DebugName = TEXT("DepthFade_1_Depth");
			}
			else
			{
				DebugName = TEXT("DepthFade_1_Fade");
			}

			bAllocatedAny |= DepthFadeTextures[TextureId][ChannelIndex].CheckPooledTarget(GraphBuilder.RHICmdList, SafeSize, DEPTH_FADE_PIXEL_FORMAT, DebugName);
		}
	}

	if (!bAllocatedAny && !bClearAll)
	{
		return;
	}

	// 새로 할당했거나 clear 요청이 있으면 같은 GraphBuilder 안에서 다시 등록하기 전에 transient ref를 비운다.
	ResetDepthFadeTransientRefs();

	for (int32 TextureId = 0; TextureId < 2; ++TextureId)
	{
		FPlaneTexture& Depth = DepthFadeTextures[TextureId][DEPTH_FADE_CHANNEL_DEPTH];
		FPlaneTexture& Fade = DepthFadeTextures[TextureId][DEPTH_FADE_CHANNEL_FADE];

		FRDGTextureRef DepthTexture = Depth.GetOrCreateTexture(GraphBuilder, TEXT("DepthFade_ClearDepth"));
		FRDGTextureRef FadeTexture = Fade.GetOrCreateTexture(GraphBuilder, TEXT("DepthFade_ClearFade"));

		FRDGTextureUAVRef DepthUAV = Depth.GetOrCreateUAV(GraphBuilder, TEXT("DepthFade_ClearDepth"));
		FRDGTextureUAVRef FadeUAV = Fade.GetOrCreateUAV(GraphBuilder, TEXT("DepthFade_ClearFade"));

		if (!DepthTexture || !FadeTexture || !DepthUAV || !FadeUAV)
		{
			continue;
		}

		GraphBuilder.UseInternalAccessMode(DepthTexture);
		GraphBuilder.UseInternalAccessMode(FadeTexture);

		AddClearUAVPass(GraphBuilder, DepthUAV, FVector4f(0.0f, 0.0f, 0.0f, 0.0f));
		AddClearUAVPass(GraphBuilder, FadeUAV, FVector4f(-32768.0f, 0.0f, 0.0f, 0.0f));

		GraphBuilder.SetTextureAccessFinal(DepthTexture, ERHIAccess::SRVMask);
		GraphBuilder.SetTextureAccessFinal(FadeTexture, ERHIAccess::SRVMask);
	}
}


void FGroundInteractionRendererProxy::CopyDepthFadeToOutput(FRDGBuilder& GraphBuilder, int32 DepthFadeTextureId)
{
	check(IsInRenderingThread());

	if (bReleased || DepthFadeTextureId < 0 || DepthFadeTextureId >= 2)
	{
		return;
	}

	FTextureRHIRef OutputDepthTextureRHI = nullptr;
	if (OutputDepthTextureReferenceRHI.IsValid())
	{
		if (FRHITexture* Texture = OutputDepthTextureReferenceRHI->GetReferencedTexture())
		{
			OutputDepthTextureRHI = FTextureRHIRef(Texture);
		}
	}

	FTextureRHIRef OutputFadeTextureRHI = nullptr;
	if (OutputFadeTextureReferenceRHI.IsValid())
	{
		if (FRHITexture* Texture = OutputFadeTextureReferenceRHI->GetReferencedTexture())
		{
			OutputFadeTextureRHI = FTextureRHIRef(Texture);
		}
	}

	if (!OutputDepthTextureRHI.IsValid() || !OutputFadeTextureRHI.IsValid())
	{
		return;
	}
	
	FPlaneTexture& Depth = DepthFadeTextures[DepthFadeTextureId][DEPTH_FADE_CHANNEL_DEPTH];
	FPlaneTexture& Fade = DepthFadeTextures[DepthFadeTextureId][DEPTH_FADE_CHANNEL_FADE];

	FRDGTextureRef DepthTexture = Depth.GetOrCreateTexture(GraphBuilder, TEXT("Copy_Depth"));
	FRDGTextureRef FadeTexture = Fade.GetOrCreateTexture(GraphBuilder, TEXT("Copy_Fade"));

	FRDGTextureRef OutputDepthTexture = RegisterExternalTexture(GraphBuilder, OutputDepthTextureRHI, TEXT("Output_Depth"));
	FRDGTextureRef OutputFadeTexture = RegisterExternalTexture(GraphBuilder, OutputFadeTextureRHI, TEXT("Output_Fade"));

	if (!DepthTexture || !FadeTexture || !OutputDepthTexture || !OutputFadeTexture)
	{
		return;
	}

	GraphBuilder.UseInternalAccessMode(DepthTexture);
	GraphBuilder.UseInternalAccessMode(FadeTexture);
	GraphBuilder.UseInternalAccessMode(OutputDepthTexture);
	GraphBuilder.UseInternalAccessMode(OutputFadeTexture);

	RDG_EVENT_SCOPE_STAT(GraphBuilder, GroundInteractionGroundOutputCopy, "GroundInteraction::GroundOutputCopy");

	AddCopyTexturePass(GraphBuilder, DepthTexture, OutputDepthTexture);
	AddCopyTexturePass(GraphBuilder, FadeTexture, OutputFadeTexture);

	GraphBuilder.UseExternalAccessMode(OutputDepthTexture, ERHIAccess::SRVMask);
	GraphBuilder.UseExternalAccessMode(OutputFadeTexture, ERHIAccess::SRVMask);
}

void FGroundInteractionRendererProxy::ResetDepthFadeTransientRefs()
{
	check(IsInRenderingThread());

	for (int32 TextureId = 0; TextureId < 2; ++TextureId)
	{
		for (int32 ChannelIndex = 0; ChannelIndex < 2; ++ChannelIndex)
		{
			DepthFadeTextures[TextureId][ChannelIndex].ResetTransientRefs();
		}
	}

	CaptureDepthTexture.ResetTransientRefs();
}


FMatrix FGroundInteractionRendererProxy::BuildBottomUpOrthoProjection(float HalfExtentX,  float HalfExtentY, float MaxDepth)
{
	const float SafeHalfExtentX = FMath::Max(HalfExtentX, 1.0f);

	const float SafeHalfExtentY = FMath::Max(HalfExtentY, 1.0f);

	const float SafeMaxDepth = FMath::Max(MaxDepth, 1.0f);

	const float ZScale = 1.0f / SafeMaxDepth;
	const float ZOffset = 0.0f;

	return FReversedZOrthoMatrix(SafeHalfExtentX, SafeHalfExtentY, ZScale, ZOffset);
}


