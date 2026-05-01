// Fill out your copyright notice in the Description page of Project Settings.

#include "GroundInteractionRendererProxy.h"
#include "GroundInteractionRendererComponent.h"

#include "RenderTargetPool.h"
#include "Rendering/CustomRenderPass.h"
#include "Runtime/Renderer/Private/SceneRendering.h"

#include "GlobalShader.h"

static TMap<FSceneInterface*, TArray<FGroundInteractionRendererProxy*>> ProxiesByScene;

static constexpr int32 GROUP_SIZE_X = 8;
static constexpr int32 GROUP_SIZE_Y = 8;
static constexpr int32 GROUP_SIZE_Z = 1;

static constexpr int32 DEPTH_FADE_CHANNEL_DEPTH = 0;
static constexpr int32 DEPTH_FADE_CHANNEL_FADE = 1;

static constexpr EPixelFormat DEPTH_FADE_PIXEL_FORMAT = PF_R16F;


// Prev 렌더 타겟을 읽어서 지정한 픽셀만큼 이동시키는 셰이더
class FMoveCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FMoveCS);
	SHADER_USE_PARAMETER_STRUCT(FMoveCS, FGlobalShader);

	static bool ShouldCompilePermutation(
		const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(
			Parameters.Platform,
			ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(
			Parameters,
			OutEnvironment);

		OutEnvironment.SetDefine(
			TEXT("GI_THREADGROUP_SIZE_X"),
			GROUP_SIZE_X);

		OutEnvironment.SetDefine(
			TEXT("GI_THREADGROUP_SIZE_Y"),
			GROUP_SIZE_Y);

		OutEnvironment.SetDefine(
			TEXT("GI_THREADGROUP_SIZE_Z"),
			GROUP_SIZE_Z);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, NumCells)
		SHADER_PARAMETER(FIntPoint, ScrollPixels)
		SHADER_PARAMETER(float, PrevCaptureViewOriginZ)
		SHADER_PARAMETER(float, CurrentCaptureViewOriginZ)
		SHADER_PARAMETER(float, MaxSceneDepth)
		SHADER_PARAMETER(float, SceneDepthInvalidEpsilon)
		SHADER_PARAMETER(uint32, bHasHistory)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, InputDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, InputFadeTexture)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputFadeTexture)
	END_SHADER_PARAMETER_STRUCT()
};


// Depth Fade 진행시키는 셰이더
class FFadeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFadeCS);
	SHADER_USE_PARAMETER_STRUCT(FFadeCS, FGlobalShader);

	static bool ShouldCompilePermutation(
		const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(
			Parameters.Platform,
			ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(
			Parameters,
			OutEnvironment);

		OutEnvironment.SetDefine(
			TEXT("GI_THREADGROUP_SIZE_X"),
			GROUP_SIZE_X);

		OutEnvironment.SetDefine(
			TEXT("GI_THREADGROUP_SIZE_Y"),
			GROUP_SIZE_Y);

		OutEnvironment.SetDefine(
			TEXT("GI_THREADGROUP_SIZE_Z"),
			GROUP_SIZE_Z);
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


// 캡쳐한 Depth 결과로 업데이트하는 셰이더
class FUpdateDepthCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUpdateDepthCS);
	SHADER_USE_PARAMETER_STRUCT(FUpdateDepthCS, FGlobalShader);

	static bool ShouldCompilePermutation(
		const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(
			Parameters.Platform,
			ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(
			Parameters,
			OutEnvironment);

		OutEnvironment.SetDefine(
			TEXT("GI_THREADGROUP_SIZE_X"),
			GROUP_SIZE_X);

		OutEnvironment.SetDefine(
			TEXT("GI_THREADGROUP_SIZE_Y"),
			GROUP_SIZE_Y);

		OutEnvironment.SetDefine(
			TEXT("GI_THREADGROUP_SIZE_Z"),
			GROUP_SIZE_Z);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, NumCells)
		SHADER_PARAMETER(float, MaxSceneDepth)
		SHADER_PARAMETER(float, SceneDepthInvalidEpsilon)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CurrentDepthTexture)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, FadeTexture)
	END_SHADER_PARAMETER_STRUCT()
};


// 씬 프록시를 캡쳐하는 커스텀 패스
class FDepthCapturePass final : public FCustomRenderPassBase
{
public:
	IMPLEMENT_CUSTOM_RENDER_PASS(FDepthCapturePass);

	FDepthCapturePass(
		const FIntPoint& InRenderTargetSize,
		FGroundInteractionRendererProxy* InOwnerProxy,
		int32 InDepthFadeTextureId,
		float InMaxSceneDepth,
		float InSceneDepthInvalidEpsilon)
		: FCustomRenderPassBase(
			TEXT("DepthCapturePass"),
			FCustomRenderPassBase::ERenderMode::DepthPass,
			FCustomRenderPassBase::ERenderOutput::SceneDepth,
			InRenderTargetSize)
		, OwnerProxy(InOwnerProxy)
		, DepthFadeTextureId(InDepthFadeTextureId)
		, RenderTargetSize(InRenderTargetSize)
		, MaxDepth(InMaxSceneDepth)
		, SceneDepthInvalidEpsilon(InSceneDepthInvalidEpsilon)
	{
	}

	virtual void OnPreRender(FRDGBuilder& GraphBuilder) override
	{
		const FIntPoint SafeSize(
			FMath::Max(RenderTargetSize.X, 1),
			FMath::Max(RenderTargetSize.Y, 1));

		const bool bNeedsAlloc =
			!CapturedDepthPooledTarget.IsValid() ||
			CapturedDepthPooledTarget->GetDesc().Extent != SafeSize ||
			CapturedDepthPooledTarget->GetDesc().Format != PF_R32_FLOAT;

		if (bNeedsAlloc)
		{
			FPooledRenderTargetDesc Desc =
				FPooledRenderTargetDesc::Create2DDesc(
					SafeSize,
					PF_R32_FLOAT,
					FClearValueBinding::Black,
					TexCreate_None,
					TexCreate_RenderTargetable | TexCreate_ShaderResource,
					false);

			Desc.DebugName = TEXT("GI_CapturedDepth");

			GRenderTargetPool.FindFreeElement(
				GraphBuilder.RHICmdList,
				Desc,
				CapturedDepthPooledTarget,
				TEXT("GI_CapturedDepth"));
		}

		if (!CapturedDepthPooledTarget.IsValid())
		{
			RenderTargetTexture = nullptr;
			PreRenderGraphBuilder = nullptr;
			return;
		}

		RenderTargetTexture =
			GraphBuilder.RegisterExternalTexture(
				CapturedDepthPooledTarget,
				TEXT("GI_CapturedDepth_PreRender"));

		PreRenderGraphBuilder = &GraphBuilder;

		if (RenderTargetTexture &&
			Views.Num() > 0 &&
			Views[0] != nullptr)
		{
			AddClearRenderTargetPass(
				GraphBuilder,
				RenderTargetTexture,
				FLinearColor::Black,
				Views[0]->UnscaledViewRect);
		}
	}

	virtual void OnPostRender(FRDGBuilder& GraphBuilder) override
	{
		if (!OwnerProxy ||
			!CapturedDepthPooledTarget.IsValid() ||
			Views.Num() == 0 ||
			Views[0] == nullptr)
		{
			return;
		}

		FRDGTextureRef CapturedDepthTexture = nullptr;

		if (PreRenderGraphBuilder == &GraphBuilder &&
			RenderTargetTexture)
		{
			CapturedDepthTexture =
				RenderTargetTexture;
		}
		else
		{
			CapturedDepthTexture =
				GraphBuilder.RegisterExternalTexture(
					CapturedDepthPooledTarget,
					TEXT("GI_CapturedDepth_PostRender"));
		}

		if (!CapturedDepthTexture)
		{
			return;
		}

		OwnerProxy->UpdateCapturedDepth(
			GraphBuilder,
			CapturedDepthTexture,
			DepthFadeTextureId,
			RenderTargetSize,
			MaxDepth,
			SceneDepthInvalidEpsilon,
			Views[0]->GetFeatureLevel());
	}

private:
	FGroundInteractionRendererProxy* OwnerProxy = nullptr;
	int32 DepthFadeTextureId = 0;
	FIntPoint RenderTargetSize = FIntPoint(1, 1);
	float MaxDepth = 1.0f;
	float SceneDepthInvalidEpsilon = 0.1f;

	TRefCountPtr<IPooledRenderTarget> CapturedDepthPooledTarget;

	// 비교용 포인터다. 역참조하지 않는다.
	FRDGBuilder* PreRenderGraphBuilder = nullptr;
};


IMPLEMENT_GLOBAL_SHADER(
	FMoveCS,
	"/Plugin/GroundInteraction/GroundInteractionMove.usf", "MainCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(
	FFadeCS, "/Plugin/GroundInteraction/GroundInteractionFade.usf", "MainCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(
	FUpdateDepthCS, "/Plugin/GroundInteraction/GroundInteractionUpdateDepth.usf", "MainCS", SF_Compute);


static void AddMovePass(
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
	float SceneDepthInvalidEpsilon,
	bool bHasHistory)
{
	FMoveCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FMoveCS::FParameters>();

	const FIntPoint NumCells(
		FMath::Max(RenderTargetSize.X, 1),
		FMath::Max(RenderTargetSize.Y, 1));

	PassParameters->NumCells = NumCells;
	PassParameters->ScrollPixels = ScrollPixels;
	PassParameters->PrevCaptureViewOriginZ = PrevCaptureViewOriginZ;
	PassParameters->CurrentCaptureViewOriginZ = CurrentCaptureViewOriginZ;
	PassParameters->MaxSceneDepth = MaxSceneDepth;
	PassParameters->SceneDepthInvalidEpsilon = SceneDepthInvalidEpsilon;
	PassParameters->bHasHistory = bHasHistory ? 1u : 0u;

	PassParameters->InputDepthTexture = PreviousDepthSRV;
	PassParameters->InputFadeTexture = PreviousFadeSRV;
	PassParameters->OutputDepthTexture = CurrentDepthUAV;
	PassParameters->OutputFadeTexture = CurrentFadeUAV;

	TShaderMapRef<FMoveCS> ComputeShader(
		GetGlobalShaderMap(FeatureLevel));

	const FIntVector DispatchCount =
		FComputeShaderUtils::GetGroupCount(
			FIntVector(
				NumCells.X,
				NumCells.Y,
				1),
			FIntVector(
				GROUP_SIZE_X,
				GROUP_SIZE_Y,
				GROUP_SIZE_Z));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GroundInteraction.Move"),
		ComputeShader,
		PassParameters,
		DispatchCount);
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
	FFadeCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFadeCS::FParameters>();

	const FIntPoint NumCells(
		FMath::Max(RenderTargetSize.X, 1),
		FMath::Max(RenderTargetSize.Y, 1));

	PassParameters->NumCells = NumCells;
	PassParameters->DeltaTimeSeconds = FadeDeltaTimeSeconds;
	PassParameters->FadeRiseSpeed = FadeRiseSpeed;
	PassParameters->FadeMaxHeight = FadeMaxHeight;
	PassParameters->DepthTexture = DepthTextureUAV;
	PassParameters->FadeTexture = FadeTextureUAV;

	TShaderMapRef<FFadeCS> ComputeShader(
		GetGlobalShaderMap(FeatureLevel));

	const FIntVector DispatchCount =
		FComputeShaderUtils::GetGroupCount(
			FIntVector(
				NumCells.X,
				NumCells.Y,
				1),
			FIntVector(
				GROUP_SIZE_X,
				GROUP_SIZE_Y,
				GROUP_SIZE_Z));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GroundInteraction.Fade"),
		ComputeShader,
		PassParameters,
		DispatchCount);
}


static void AddUpdateDepthPass(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FRDGTextureUAVRef DepthTextureUAV,
	FRDGTextureUAVRef FadeTextureUAV,
	FRDGTextureRef CapturedDepthTexture,
	const FIntPoint& RenderTargetSize,
	float MaxSceneDepth,
	float SceneDepthInvalidEpsilon)
{
	FUpdateDepthCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FUpdateDepthCS::FParameters>();

	const FIntPoint NumCells(
		FMath::Max(RenderTargetSize.X, 1),
		FMath::Max(RenderTargetSize.Y, 1));

	PassParameters->NumCells = NumCells;
	PassParameters->MaxSceneDepth = MaxSceneDepth;
	PassParameters->SceneDepthInvalidEpsilon = SceneDepthInvalidEpsilon;
	PassParameters->CurrentDepthTexture = CapturedDepthTexture;
	PassParameters->DepthTexture = DepthTextureUAV;
	PassParameters->FadeTexture = FadeTextureUAV;

	TShaderMapRef<FUpdateDepthCS> ComputeShader(
		GetGlobalShaderMap(FeatureLevel));

	const FIntVector DispatchCount =
		FComputeShaderUtils::GetGroupCount(
			FIntVector(
				NumCells.X,
				NumCells.Y,
				1),
			FIntVector(
				GROUP_SIZE_X,
				GROUP_SIZE_Y,
				GROUP_SIZE_Z));
	
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GroundInteraction.UpdateDepth"),
		ComputeShader,
		PassParameters,
		DispatchCount);
}


bool FPlaneTexture::CheckPooledTarget(
	FRHICommandListImmediate& RHICmdList,
	const FIntPoint& InSize,
	EPixelFormat InFormat,
	const TCHAR* DebugName)
{
	const FIntPoint SafeSize(
		FMath::Max(InSize.X, 1),
		FMath::Max(InSize.Y, 1));

	const bool bNeedsAlloc =
		!PooledTarget.IsValid() ||
		AllocatedSize != SafeSize ||
		AllocatedFormat != InFormat;

	if (!bNeedsAlloc)
	{
		return false;
	}

	FPooledRenderTargetDesc Desc =
		FPooledRenderTargetDesc::Create2DDesc(
			SafeSize,
			InFormat,
			FClearValueBinding::None,
			TexCreate_None,
			TexCreate_ShaderResource | TexCreate_UAV,
			false);

	Desc.DebugName = DebugName;

	GRenderTargetPool.FindFreeElement(
		RHICmdList,
		Desc,
		PooledTarget,
		DebugName);

	AllocatedSize = SafeSize;
	AllocatedFormat = InFormat;

	ResetTransientRefs();

	return true;
}

FRDGTextureRef FPlaneTexture::GetOrCreateTexture(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name)
{
	if (!PooledTarget.IsValid())
	{
		return nullptr;
	}

	if (!TransientTextureRef)
	{
		TransientTextureRef =
			GraphBuilder.RegisterExternalTexture(
				PooledTarget,
				Name);
	}

	return TransientTextureRef;
}

FRDGTextureSRVRef FPlaneTexture::GetOrCreateSRV(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name)
{
	FRDGTextureRef TextureRef =
		GetOrCreateTexture(
			GraphBuilder,
			Name);

	if (!TextureRef)
	{
		return nullptr;
	}

	if (!TransientSRVRef)
	{
		TransientSRVRef =
			GraphBuilder.CreateSRV(
				FRDGTextureSRVDesc::Create(TextureRef));
	}

	return TransientSRVRef;
}

FRDGTextureUAVRef FPlaneTexture::GetOrCreateUAV(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name)
{
	FRDGTextureRef TextureRef =
		GetOrCreateTexture(
			GraphBuilder,
			Name);

	if (!TextureRef)
	{
		return nullptr;
	}

	if (!TransientUAVRef)
	{
		TransientUAVRef =
			GraphBuilder.CreateUAV(TextureRef);
	}

	return TransientUAVRef;
}


FGroundInteractionRendererProxy::FGroundInteractionRendererProxy(
	UGroundInteractionRendererComponent* InRendererComponent,
	FTextureReferenceRHIRef InDepthTextureReferenceRHI,
	FTextureReferenceRHIRef InFadeTextureReferenceRHI,
	FIntVector InNumCell,
	FVector InCellSpacing,
	const FVector& InCaptureExtentWS,
	float InFadeRiseSpeed,
	float InFadeMaxHeight,
	float InFadeUpdateIntervalSeconds,
	float InCaptureUpdateIntervalSeconds)
	: RendererComponent(InRendererComponent)
	, OutputDepthTextureReferenceRHI(InDepthTextureReferenceRHI)
	, OutputFadeTextureReferenceRHI(InFadeTextureReferenceRHI)
	, RenderTargetSize(
		FMath::Max(InNumCell.X, 1),
		FMath::Max(InNumCell.Y, 1))
	, CellSpacing(InCellSpacing)
	, CaptureExtentWS(
		FMath::Max(InCaptureExtentWS.X, 1.0),
		FMath::Max(InCaptureExtentWS.Y, 1.0),
		FMath::Max(InCaptureExtentWS.Z, 1.0))
	, FadeRiseSpeed(FMath::Max(InFadeRiseSpeed, 0.0f))
	, FadeMaxHeight(FMath::Max(InFadeMaxHeight, 0.0f))
	, FadeUpdateIntervalSeconds(FMath::Max(InFadeUpdateIntervalSeconds, KINDA_SMALL_NUMBER))
	, CaptureUpdateIntervalSeconds(FMath::Max(InCaptureUpdateIntervalSeconds, KINDA_SMALL_NUMBER))
{
	CurrentSnappedLocation = FVector::ZeroVector;
	PreviousSnappedLocation = FVector::ZeroVector;

	ShowOnlyPrimitives.Reset();

	bRenderTargetChanged = false;
	bHasPreviousCapture = false;

	CurrentStateIndex = 0;

	FadeUpdateAccumulatorSeconds = 0.0f;
	CaptureUpdateAccumulatorSeconds = 0.0f;

	bReleased = false;
	bClearDepthFadeTextures = true;
}


void FGroundInteractionRendererProxy::Render(
	FRDGBuilder& GraphBuilder,
	FSceneViewFamily& InViewFamily)
{
	check(IsInRenderingThread());

	if (bReleased ||
		!InViewFamily.Scene ||
		InViewFamily.Views.Num() == 0)
	{
		return;
	}

	const FSceneView* PrimaryView =
		InViewFamily.Views[0];

	if (!PrimaryView ||
		PrimaryView->bIsSceneCapture ||
		PrimaryView->bIsReflectionCapture)
	{
		return;
	}

	if (!OutputDepthTextureReferenceRHI.IsValid() ||
		!OutputFadeTextureReferenceRHI.IsValid())
	{
		return;
	}

	if (RenderTargetSize.X <= 0 ||
		RenderTargetSize.Y <= 0)
	{
		return;
	}
	
	
	// 새 RDG graph 진입 시 이전 graph의 transient RDG ref를 비운다.
	ResetDepthFadeTransientRefs();

	
	
	// 렌더 타겟 사이즈가 바뀌었으면 이전 상태를 나타내는 변수들을 초기화한다.
	if (bRenderTargetChanged)
	{
		bRenderTargetChanged = false;

		CurrentStateIndex = 0;

		FadeUpdateAccumulatorSeconds = 0.0f;
		CaptureUpdateAccumulatorSeconds = 0.0f;

		bHasPreviousCapture = false;
		PreviousSnappedLocation = FVector::ZeroVector;

		bClearDepthFadeTextures = true;
	}
	
	// 렌더 타겟 크기가 바뀌었으면 DepthFadeTextures 가 초기화된다.
	CheckDepthFadeTextures(
	GraphBuilder,
	RenderTargetSize,
	bClearDepthFadeTextures);

	bClearDepthFadeTextures = false;
	
	
	
	
	// 이번 Render 호출에서 사용할 delta time.
	// Fade Pass 와 Capture Pass 실행 여부를 나타내는 accumulator 누적에 사용한다.
	const float DeltaTimeSeconds =
	FMath::Max(
		InViewFamily.Time.GetDeltaRealTimeSeconds(),
		0.0f);

	// 현재 프레임의 스냅된 센터 위치
	const FVector CurrentCaptureOriginWS =
		GetCaptureOriginWS();

	// 이전 프레임의 스냅된 센터 위치
	const FVector PreviousCaptureOriginWS =
		bHasPreviousCapture
			? GetPreviousCaptureOriginWS()
			: CurrentCaptureOriginWS;
	
	// 이전 프레임 센터 위치와 현재 프레임 센터 위치를 비교해 몇 픽셀만큼 움직여야 하는지 Offset 을 저장하는 변수
	FIntPoint CaptureScrollPixels = FIntPoint::ZeroValue;
	
	if (bHasPreviousCapture)
	{
		const FVector2D UnitsPerPixel(
			CaptureExtentWS.X * 2.0 / static_cast<double>(RenderTargetSize.X),
			CaptureExtentWS.Y * 2.0 / static_cast<double>(RenderTargetSize.Y));

		CaptureScrollPixels.X =
			FMath::RoundToInt(
				(CurrentSnappedLocation.X - PreviousSnappedLocation.X) /
				UnitsPerPixel.X);

		CaptureScrollPixels.Y =
			-FMath::RoundToInt(
				(CurrentSnappedLocation.Y - PreviousSnappedLocation.Y) /
				UnitsPerPixel.Y);
	}
	
	// MPC 에서 Center 위치 재설정 여부를 나타내는 변수
	bool bIsMoved = false;
	
	// ---- Move Pass 실행 ----
	// 이전 프레임 캡쳐 결과가 있고, CaptureScrollPixels 값이 0 이 아니면 중심 위치를 이동시겨야 하므로 Move Pass 를 실행한다.
	if (bHasPreviousCapture && CaptureScrollPixels != FIntPoint::ZeroValue)
	{
		FPlaneTexture& PreviousDepth =
			DepthFadeTextures[GetPrevDepthFadeTextureId()][DEPTH_FADE_CHANNEL_DEPTH];

		FPlaneTexture& PreviousFade =
			DepthFadeTextures[GetPrevDepthFadeTextureId()][DEPTH_FADE_CHANNEL_FADE];

		FPlaneTexture& CurrentDepth =
			DepthFadeTextures[GetCurrentDepthFadeTextureId()][DEPTH_FADE_CHANNEL_DEPTH];

		FPlaneTexture& CurrentFade =
			DepthFadeTextures[GetCurrentDepthFadeTextureId()][DEPTH_FADE_CHANNEL_FADE];

		FRDGTextureRef PreviousDepthTexture =
			PreviousDepth.GetOrCreateTexture(
				GraphBuilder,
				TEXT("Move_PreviousDepth"));

		FRDGTextureRef PreviousFadeTexture =
			PreviousFade.GetOrCreateTexture(
				GraphBuilder,
				TEXT("Move_PreviousFade"));

		FRDGTextureSRVRef PreviousDepthSRV =
			PreviousDepth.GetOrCreateSRV(
				GraphBuilder,
				TEXT("Move_PreviousDepth"));

		FRDGTextureSRVRef PreviousFadeSRV =
			PreviousFade.GetOrCreateSRV(
				GraphBuilder,
				TEXT("Move_PreviousFade"));

		FRDGTextureRef CurrentDepthTexture =
			CurrentDepth.GetOrCreateTexture(
				GraphBuilder,
				TEXT("Move_CurrentDepth"));

		FRDGTextureRef CurrentFadeTexture =
			CurrentFade.GetOrCreateTexture(
				GraphBuilder,
				TEXT("Move_CurrentFade"));

		FRDGTextureUAVRef CurrentDepthUAV =
			CurrentDepth.GetOrCreateUAV(
				GraphBuilder,
				TEXT("Move_CurrentDepth"));

		FRDGTextureUAVRef CurrentFadeUAV =
			CurrentFade.GetOrCreateUAV(
				GraphBuilder,
				TEXT("Move_CurrentFade"));

		if (PreviousDepthTexture &&
			PreviousFadeTexture &&
			PreviousDepthSRV &&
			PreviousFadeSRV &&
			CurrentDepthTexture &&
			CurrentFadeTexture &&
			CurrentDepthUAV &&
			CurrentFadeUAV)
		{
			GraphBuilder.UseInternalAccessMode(PreviousDepthTexture);
			GraphBuilder.UseInternalAccessMode(PreviousFadeTexture);
			GraphBuilder.UseInternalAccessMode(CurrentDepthTexture);
			GraphBuilder.UseInternalAccessMode(CurrentFadeTexture);

			AddMovePass(
				GraphBuilder,
				PrimaryView->GetFeatureLevel(),
				PreviousDepthSRV,
				PreviousFadeSRV,
				CurrentDepthUAV,
				CurrentFadeUAV,
				RenderTargetSize,
				CaptureScrollPixels,
				PreviousCaptureOriginWS.Z,
				CurrentCaptureOriginWS.Z,
				GetMaxSceneDepth(),
				GetSceneDepthInvalidEpsilon(),
				true);

			GraphBuilder.UseExternalAccessMode(
				PreviousDepthTexture,
				ERHIAccess::SRVMask);

			GraphBuilder.UseExternalAccessMode(
				PreviousFadeTexture,
				ERHIAccess::SRVMask);

			GraphBuilder.UseExternalAccessMode(
				CurrentDepthTexture,
				ERHIAccess::SRVMask);

			GraphBuilder.UseExternalAccessMode(
				CurrentFadeTexture,
				ERHIAccess::SRVMask);

			AdvanceDepthFadeTextureId();
			bIsMoved = true; 
			// 스냅된 센터 위치 변경
			PreviousSnappedLocation = CurrentSnappedLocation;
		}
	}
	
	
	
	
	
	// ---- Fade Pass 실행 ----
	// FadeUpdateAccumulatorSeconds 가 FadeUpdateIntervalSeconds 다 커졌을 때 실행된다.
	FadeUpdateAccumulatorSeconds += DeltaTimeSeconds;

	float FadeDeltaTimeSeconds = 0.0f;

	if (FadeUpdateAccumulatorSeconds >= FadeUpdateIntervalSeconds)
	{
		const int32 FadeStepCount =
			FMath::Max(
				FMath::FloorToInt(
					FadeUpdateAccumulatorSeconds / FadeUpdateIntervalSeconds),
				1);

		FadeDeltaTimeSeconds =
			static_cast<float>(FadeStepCount) * FadeUpdateIntervalSeconds;

		FadeUpdateAccumulatorSeconds -=
			FadeDeltaTimeSeconds;
	}
	
	bool bIsFaded = false;

	if (FadeDeltaTimeSeconds > 0.0f)
	{
		FPlaneTexture& Depth =
			DepthFadeTextures[CurrentStateIndex][DEPTH_FADE_CHANNEL_DEPTH];

		FPlaneTexture& Fade =
			DepthFadeTextures[CurrentStateIndex][DEPTH_FADE_CHANNEL_FADE];

		FRDGTextureRef DepthTexture =
			Depth.GetOrCreateTexture(
				GraphBuilder,
				TEXT("Fade_Depth"));

		FRDGTextureRef FadeTexture =
			Fade.GetOrCreateTexture(
				GraphBuilder,
				TEXT("Fade_Fade"));

		FRDGTextureUAVRef DepthUAV =
			Depth.GetOrCreateUAV(
				GraphBuilder,
				TEXT("Fade_Depth"));

		FRDGTextureUAVRef FadeUAV =
			Fade.GetOrCreateUAV(
				GraphBuilder,
				TEXT("Fade_Fade"));

		if (DepthTexture &&
			FadeTexture &&
			DepthUAV &&
			FadeUAV)
		{
			GraphBuilder.UseInternalAccessMode(DepthTexture);
			GraphBuilder.UseInternalAccessMode(FadeTexture);

			AddFadePass(
				GraphBuilder,
				PrimaryView->GetFeatureLevel(),
				DepthUAV,
				FadeUAV,
				RenderTargetSize,
				FadeDeltaTimeSeconds,
				FadeRiseSpeed,
				FadeMaxHeight);

			GraphBuilder.UseExternalAccessMode(
				DepthTexture,
				ERHIAccess::SRVMask);

			GraphBuilder.UseExternalAccessMode(
				FadeTexture,
				ERHIAccess::SRVMask);
			
			bIsFaded = true;
		}
	}
	
	
	
	
	
	
	// ---- Capture 후 Update Depth 하는 부분 ----
	// CaptureUpdateAccumulatorSeconds 가 CaptureUpdateIntervalSeconds 보다 커졌을 때 실행된다.
	CaptureUpdateAccumulatorSeconds += DeltaTimeSeconds;

	float CaptureDeltaTimeSeconds = 0.0f;

	if (CaptureUpdateAccumulatorSeconds >= CaptureUpdateIntervalSeconds)
	{
		const int32 CaptureStepCount =
			FMath::Max(
				FMath::FloorToInt(
					CaptureUpdateAccumulatorSeconds / CaptureUpdateIntervalSeconds),
				1);

		CaptureDeltaTimeSeconds =
			static_cast<float>(CaptureStepCount) * CaptureUpdateIntervalSeconds;

		CaptureUpdateAccumulatorSeconds -=
			CaptureDeltaTimeSeconds;
	}

	// 이번 Render 호출 시작 시점 기준으로 첫 캡쳐인지 저장한다.
	const bool bIsFirstCapture = !bHasPreviousCapture;

	// 이전 캡쳐 히스토리가 없다면 이번 프레임엔 캡쳐를 한 번 시도한다.
	if (bIsFirstCapture)
	{
		CaptureDeltaTimeSeconds = CaptureUpdateIntervalSeconds;
		CaptureUpdateAccumulatorSeconds = 0.0f;
	}

	bool bIsCaptured = false;

	// ---- Capture Pass 실행 ----
	// Capture rate 가 도달했고, 컴포넌트에서 넘겨준 ShowOnlyPrimitives 가 있을 때만 캡쳐 패스를 등록한다.
	if (CaptureDeltaTimeSeconds > 0.0f &&
		ShowOnlyPrimitives.Num() > 0)
	{
		const float HalfExtentX =
			FMath::Max(CaptureExtentWS.X, 1.0f);

		const float HalfExtentY =
			FMath::Max(CaptureExtentWS.Y, 1.0f);

		const FVector CaptureViewLocation =
			CurrentCaptureOriginWS;

		const FVector CaptureLookAt =
			CaptureViewLocation + FVector(0.0f, 0.0f, 1.0f);

		FSceneInterface::FCustomRenderPassRendererInput PassInput;
		PassInput.ViewLocation = CaptureViewLocation;

		PassInput.ViewRotationMatrix =
			FLookAtMatrix(
				CaptureViewLocation,
				CaptureLookAt,
				FVector(0.0f, 1.0f, 0.0f));

		PassInput.ViewRotationMatrix =
			PassInput.ViewRotationMatrix.RemoveTranslation();

		PassInput.ViewRotationMatrix.RemoveScaling();

		const float MaxDepth =
			GetMaxSceneDepth();

		PassInput.ProjectionMatrix =
			BuildBottomUpOrthoProjection(
				HalfExtentX,
				HalfExtentY,
				MaxDepth);

		PassInput.ViewActor = nullptr;
		PassInput.ShowOnlyPrimitives = ShowOnlyPrimitives;

		PassInput.bIsSceneCapture = true;
		PassInput.bUseMainViewFamilyShowFlags = false;

		PassInput.EngineShowFlags = FEngineShowFlags(ESFIM_Game);
		PassInput.EngineShowFlags.SetFog(false);
		PassInput.EngineShowFlags.SetAtmosphere(false);
		PassInput.EngineShowFlags.SetSkyLighting(false);
		PassInput.EngineShowFlags.SetPostProcessing(false);
		PassInput.EngineShowFlags.SetAntiAliasing(false);
		PassInput.EngineShowFlags.SetDynamicShadows(false);
		PassInput.EngineShowFlags.SetTranslucency(false);

		PassInput.CustomRenderPass =
			new FDepthCapturePass(
				RenderTargetSize,
				this,
				CurrentStateIndex,
				MaxDepth,
				GetSceneDepthInvalidEpsilon());

		InViewFamily.Scene->AddCustomRenderPass(
			&InViewFamily,
			PassInput);

		bIsCaptured = true;

		// Capture pass 가 등록된 경우
		// UpdateCapturedDepth() 안에서 UpdateDepth + CopyDepthFadeToOutput 이 실행되므로
		// 이 Render() 함수 안에서 CopyDepthFadeToOutput 을 하지 않는다.
		// 첫번째 캡쳐는 Move Pass 가 없으므로 여기서 PreviousSnappedLocation 를 설정한다.
		
		if (bIsFirstCapture)
		{
			bHasPreviousCapture = true;
			PreviousSnappedLocation = CurrentSnappedLocation;
		}
	}

	// Capture Pass 가 실행되지 않은 경우에만 현재 DepthFadeTexture 를 Output RT 에 복사한다.
	// Capture Pass 가 실행됐으면 UpdateCapturedDepth() 에서 CopyDepthFadeToOutput 을 실행하므로 여기서 실행할 필요 없다.
	if (!bIsCaptured && (bIsMoved || bIsFaded || !bHasPreviousCapture))
	{
		CopyDepthFadeToOutput(
			GraphBuilder,
			CurrentStateIndex);
	}
	
	
	
	
	// Move Pass 성공 시 OR 첫 캡쳐 실행 시 MPC 의 중심 위치 갱신
	if (bIsMoved || (bIsFirstCapture && bIsCaptured))
	{
		TWeakObjectPtr<UGroundInteractionRendererComponent> RendererComponentWeak =
			RendererComponent;

		const FVector CaptureOriginForMPC =
			CurrentCaptureOriginWS;

		const FVector CaptureExtentForMPC =
			CaptureExtentWS;

		AsyncTask(
			ENamedThreads::GameThread,
			[
				RendererComponentWeak,
				CaptureOriginForMPC,
				CaptureExtentForMPC
			]()
			{
				if (UGroundInteractionRendererComponent* Component =
					RendererComponentWeak.Get())
				{
					Component->UpdateMPC_GameThread(
						CaptureOriginForMPC,
						CaptureExtentForMPC);
				}
			});
	}
}



void FGroundInteractionRendererProxy::Release()
{
	check(IsInRenderingThread());

	bReleased = true;
}

void FGroundInteractionRendererProxy::RegisterProxy(
	FSceneInterface* InScene,
	FGroundInteractionRendererProxy* InProxy)
{
	check(IsInRenderingThread());

	if (!InScene || !InProxy)
	{
		return;
	}

	ProxiesByScene.FindOrAdd(InScene).AddUnique(InProxy);
}

void FGroundInteractionRendererProxy::UnregisterProxy(
	FSceneInterface* InScene,
	FGroundInteractionRendererProxy* InProxy)
{
	check(IsInRenderingThread());

	if (!InScene || !InProxy)
	{
		return;
	}

	TArray<FGroundInteractionRendererProxy*>* Proxies =
		ProxiesByScene.Find(InScene);

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

TArray<FGroundInteractionRendererProxy*> FGroundInteractionRendererProxy::GetProxies(
	FSceneInterface* InScene)
{
	check(IsInRenderingThread());

	if (!InScene)
	{
		return {};
	}

	const TArray<FGroundInteractionRendererProxy*>* Proxies =
		ProxiesByScene.Find(InScene);

	return Proxies ? *Proxies : TArray<FGroundInteractionRendererProxy*>();
}


void FGroundInteractionRendererProxy::UpdateCapturedDepth(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef CapturedDepthTexture,
	int32 DepthFadeTextureId,
	const FIntPoint& PassRenderTargetSize,
	float PassMaxSceneDepth,
	float PassSceneDepthInvalidEpsilon,
	ERHIFeatureLevel::Type FeatureLevel)
{
	check(IsInRenderingThread());

	if (bReleased ||
		!CapturedDepthTexture ||
		DepthFadeTextureId < 0 ||
		DepthFadeTextureId >= 2)
	{
		return;
	}

	// CustomRenderPass 의 OnPostRender 는 Render() 와 다른 GraphBuilder 로 들어올 수 있으므로
	// 이전 RDG graph 에서 만들어진 transient ref 를 비운다.
	ResetDepthFadeTransientRefs();

	FPlaneTexture& Depth =
		DepthFadeTextures[DepthFadeTextureId][DEPTH_FADE_CHANNEL_DEPTH];

	FPlaneTexture& Fade =
		DepthFadeTextures[DepthFadeTextureId][DEPTH_FADE_CHANNEL_FADE];

	FRDGTextureRef DepthTexture =
		Depth.GetOrCreateTexture(
			GraphBuilder,
			TEXT("UpdateDepth_Depth"));

	FRDGTextureRef FadeTexture =
		Fade.GetOrCreateTexture(
			GraphBuilder,
			TEXT("UpdateDepth_Fade"));

	FRDGTextureUAVRef DepthUAV =
		Depth.GetOrCreateUAV(
			GraphBuilder,
			TEXT("UpdateDepth_Depth"));

	FRDGTextureUAVRef FadeUAV =
		Fade.GetOrCreateUAV(
			GraphBuilder,
			TEXT("UpdateDepth_Fade"));

	if (!DepthTexture ||
		!FadeTexture ||
		!DepthUAV ||
		!FadeUAV)
	{
		return;
	}

	GraphBuilder.UseInternalAccessMode(DepthTexture);
	GraphBuilder.UseInternalAccessMode(FadeTexture);

	AddUpdateDepthPass(
		GraphBuilder,
		FeatureLevel,
		DepthUAV,
		FadeUAV,
		CapturedDepthTexture,
		PassRenderTargetSize,
		PassMaxSceneDepth,
		PassSceneDepthInvalidEpsilon);

	// Capture pass 가 실행된 프레임은 여기서 output RT 로 복사한다.
	// Capture Pass 가 실행되면 Render() 함수 함수 안에서는 CopyDepthFadeToOutput() 가 호출되지 않는다.
	CopyDepthFadeToOutput(
		GraphBuilder,
		DepthFadeTextureId);

	GraphBuilder.UseExternalAccessMode(
		DepthTexture,
		ERHIAccess::SRVMask);

	GraphBuilder.UseExternalAccessMode(
		FadeTexture,
		ERHIAccess::SRVMask);
}


void FGroundInteractionRendererProxy::CheckDepthFadeTextures(
	FRDGBuilder& GraphBuilder,
	const FIntPoint& InRenderTargetSize,
	bool bClearAll)
{
	const FIntPoint SafeSize(
		FMath::Max(InRenderTargetSize.X, 1),
		FMath::Max(InRenderTargetSize.Y, 1));

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

			bAllocatedAny |=
				DepthFadeTextures[TextureId][ChannelIndex]
					.CheckPooledTarget(
						GraphBuilder.RHICmdList,
						SafeSize,
						DEPTH_FADE_PIXEL_FORMAT,
						DebugName);
		}
	}

	if (!bAllocatedAny && !bClearAll)
	{
		return;
	}

	// 새로 할당됐거나 clear 요청이 있으면,
	// 같은 GraphBuilder 안에서 다시 등록하기 전에 transient ref를 비운다.
	ResetDepthFadeTransientRefs();

	for (int32 TextureId = 0; TextureId < 2; ++TextureId)
	{
		FPlaneTexture& Depth =
			DepthFadeTextures[TextureId][DEPTH_FADE_CHANNEL_DEPTH];

		FPlaneTexture& Fade =
			DepthFadeTextures[TextureId][DEPTH_FADE_CHANNEL_FADE];

		FRDGTextureRef DepthTexture =
			Depth.GetOrCreateTexture(
				GraphBuilder,
				TEXT("DepthFade_ClearDepth"));

		FRDGTextureRef FadeTexture =
			Fade.GetOrCreateTexture(
				GraphBuilder,
				TEXT("DepthFade_ClearFade"));

		FRDGTextureUAVRef DepthUAV =
			Depth.GetOrCreateUAV(
				GraphBuilder,
				TEXT("DepthFade_ClearDepth"));

		FRDGTextureUAVRef FadeUAV =
			Fade.GetOrCreateUAV(
				GraphBuilder,
				TEXT("DepthFade_ClearFade"));

		if (!DepthTexture ||
			!FadeTexture ||
			!DepthUAV ||
			!FadeUAV)
		{
			continue;
		}

		GraphBuilder.UseInternalAccessMode(DepthTexture);
		GraphBuilder.UseInternalAccessMode(FadeTexture);

		AddClearUAVPass(
			GraphBuilder,
			DepthUAV,
			FVector4f(0.0f, 0.0f, 0.0f, 0.0f));

		AddClearUAVPass(
			GraphBuilder,
			FadeUAV,
			FVector4f(-32768.0f, 0.0f, 0.0f, 0.0f));

		GraphBuilder.UseExternalAccessMode(
			DepthTexture,
			ERHIAccess::SRVMask);

		GraphBuilder.UseExternalAccessMode(
			FadeTexture,
			ERHIAccess::SRVMask);
	}
}


void FGroundInteractionRendererProxy::CopyDepthFadeToOutput(
	FRDGBuilder& GraphBuilder,
	int32 DepthFadeTextureId)
{
	check(IsInRenderingThread());

	if (bReleased ||
		DepthFadeTextureId < 0 ||
		DepthFadeTextureId >= 2)
	{
		return;
	}

	FTextureRHIRef OutputDepthTextureRHI = nullptr;
	if (OutputDepthTextureReferenceRHI.IsValid())
	{
		if (FRHITexture* Texture =
			OutputDepthTextureReferenceRHI->GetReferencedTexture())
		{
			OutputDepthTextureRHI = FTextureRHIRef(Texture);
		}
	}

	FTextureRHIRef OutputFadeTextureRHI = nullptr;
	if (OutputFadeTextureReferenceRHI.IsValid())
	{
		if (FRHITexture* Texture =
			OutputFadeTextureReferenceRHI->GetReferencedTexture())
		{
			OutputFadeTextureRHI = FTextureRHIRef(Texture);
		}
	}

	if (!OutputDepthTextureRHI.IsValid() ||
		!OutputFadeTextureRHI.IsValid())
	{
		return;
	}
	
	FPlaneTexture& Depth =
		DepthFadeTextures[DepthFadeTextureId][DEPTH_FADE_CHANNEL_DEPTH];

	FPlaneTexture& Fade =
		DepthFadeTextures[DepthFadeTextureId][DEPTH_FADE_CHANNEL_FADE];

	FRDGTextureRef DepthTexture =
		Depth.GetOrCreateTexture(
			GraphBuilder,
			TEXT("Copy_Depth"));

	FRDGTextureRef FadeTexture =
		Fade.GetOrCreateTexture(
			GraphBuilder,
			TEXT("Copy_Fade"));

	FRDGTextureRef OutputDepthTexture =
		RegisterExternalTexture(
			GraphBuilder,
			OutputDepthTextureRHI,
			TEXT("Output_Depth"));

	FRDGTextureRef OutputFadeTexture =
		RegisterExternalTexture(
			GraphBuilder,
			OutputFadeTextureRHI,
			TEXT("Output_Fade"));

	if (!DepthTexture ||
		!FadeTexture ||
		!OutputDepthTexture ||
		!OutputFadeTexture)
	{
		return;
	}

	GraphBuilder.UseInternalAccessMode(DepthTexture);
	GraphBuilder.UseInternalAccessMode(FadeTexture);
	GraphBuilder.UseInternalAccessMode(OutputDepthTexture);
	GraphBuilder.UseInternalAccessMode(OutputFadeTexture);

	AddCopyTexturePass(
		GraphBuilder,
		DepthTexture,
		OutputDepthTexture);

	AddCopyTexturePass(
		GraphBuilder,
		FadeTexture,
		OutputFadeTexture);

	GraphBuilder.UseExternalAccessMode(
		DepthTexture,
		ERHIAccess::SRVMask);

	GraphBuilder.UseExternalAccessMode(
		FadeTexture,
		ERHIAccess::SRVMask);

	GraphBuilder.UseExternalAccessMode(
		OutputDepthTexture,
		ERHIAccess::SRVMask);

	GraphBuilder.UseExternalAccessMode(
		OutputFadeTexture,
		ERHIAccess::SRVMask);
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
}


FMatrix FGroundInteractionRendererProxy::BuildBottomUpOrthoProjection(
	float HalfExtentX,
	float HalfExtentY,
	float MaxDepth)
{
	check((int32)ERHIZBuffer::IsInverted);

	const float SafeHalfExtentX =
		FMath::Max(HalfExtentX, 1.0f);

	const float SafeHalfExtentY =
		FMath::Max(HalfExtentY, 1.0f);

	const float SafeMaxDepth =
		FMath::Max(MaxDepth, 1.0f);

	const float ZScale = 1.0f / SafeMaxDepth;
	const float ZOffset = 0.0f;

	return FReversedZOrthoMatrix(
		SafeHalfExtentX,
		SafeHalfExtentY,
		ZScale,
		ZOffset);
}












