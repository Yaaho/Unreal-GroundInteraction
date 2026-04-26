// Fill out your copyright notice in the Description page of Project Settings.


#include "GroundInteractionSceneViewExtension.h"
#include "GroundInteractionWorldSubsystem.h"
#include "GroundInteractionSettings.h"

#include "Engine/TextureRenderTarget2D.h"
#include "RenderTargetPool.h"
#include "Rendering/CustomRenderPass.h"
#include "Runtime/Renderer/Private/SceneRendering.h"

#include "GlobalShader.h"



namespace GroundInteraction
{
	static constexpr int32 ComputeGroupSizeX = 8;
	static constexpr int32 ComputeGroupSizeY = 8;

	static constexpr int32 StateChannel_Depth = 0;
	static constexpr int32 StateChannel_Fade = 1;

	static constexpr EPixelFormat StatePixelFormat = PF_R16F;

	static FIntVector GetGroupCount(const FIntPoint& TextureSize)
	{
		return FIntVector(
			FMath::DivideAndRoundUp(TextureSize.X, ComputeGroupSizeX),
			FMath::DivideAndRoundUp(TextureSize.Y, ComputeGroupSizeY),
			1);
	}

	static float GetMaxSceneDepth(const FGroundInteractionRenderFrameParams& FrameParams)
	{
		return FMath::Max(FrameParams.CaptureInfo.ExtentWS.Z * 2.0f, 1.0f);
	}

	static float GetSceneDepthInvalidEpsilon(float MaxSceneDepth)
	{
		return FMath::Clamp(MaxSceneDepth * 0.0005f, 0.1f, 2.0f);
	}

	static const TCHAR* GetStateDebugName(int32 SwapIndex, int32 ChannelIndex)
	{
		if (SwapIndex == 0 && ChannelIndex == StateChannel_Depth)
		{
			return TEXT("GI_StateRT_0_Depth");
		}

		if (SwapIndex == 0 && ChannelIndex == StateChannel_Fade)
		{
			return TEXT("GI_StateRT_0_Fade");
		}

		if (SwapIndex == 1 && ChannelIndex == StateChannel_Depth)
		{
			return TEXT("GI_StateRT_1_Depth");
		}

		return TEXT("GI_StateRT_1_Fade");
	}

	static FPooledRenderTargetDesc CreateStatePooledTextureDesc(const FIntPoint& TextureSize)
	{
		return FPooledRenderTargetDesc::Create2DDesc(
			TextureSize,
			StatePixelFormat,
			FClearValueBinding::None,
			TexCreate_None,
			TexCreate_ShaderResource | TexCreate_UAV,
			false);
	}

	static void AddClearDepthPass(FRDGBuilder& GraphBuilder, FRDGTextureRef DepthTexture)
	{
		AddClearUAVPass(
			GraphBuilder,
			GraphBuilder.CreateUAV(DepthTexture),
			FVector4f(0.0f, 0.0f, 0.0f, 0.0f));
	}

	static void AddClearFadePass(FRDGBuilder& GraphBuilder, FRDGTextureRef FadeTexture)
	{
		AddClearUAVPass(
			GraphBuilder,
			GraphBuilder.CreateUAV(FadeTexture),
			FVector4f(-32768.0f, 0.0f, 0.0f, 0.0f));
	}

	class FGroundInteractionMoveCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGroundInteractionMoveCS);
		SHADER_USE_PARAMETER_STRUCT(FGroundInteractionMoveCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(FIntPoint, TextureSize)
			SHADER_PARAMETER(FIntPoint, ScrollPixels)
			SHADER_PARAMETER(float, PrevCaptureViewOriginZ)
			SHADER_PARAMETER(float, CurrentCaptureViewOriginZ)
			SHADER_PARAMETER(float, MaxSceneDepth)
			SHADER_PARAMETER(float, SceneDepthInvalidEpsilon)
			SHADER_PARAMETER(uint32, bHasHistory)

			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, InputDepthTexture)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, InputFadeTexture)

			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputDepthTexture)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputFadeTexture)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FGroundInteractionFadeCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGroundInteractionFadeCS);
		SHADER_USE_PARAMETER_STRUCT(FGroundInteractionFadeCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(FIntPoint, TextureSize)
			SHADER_PARAMETER(float, DeltaTimeSeconds)
			SHADER_PARAMETER(float, FadeRiseSpeed)
			SHADER_PARAMETER(float, FadeMaxHeight)

			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DepthTexture)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, FadeTexture)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FGroundInteractionUpdateDepthCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGroundInteractionUpdateDepthCS);
		SHADER_USE_PARAMETER_STRUCT(FGroundInteractionUpdateDepthCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(FIntPoint, TextureSize)
			SHADER_PARAMETER(float, MaxSceneDepth)
			SHADER_PARAMETER(float, SceneDepthInvalidEpsilon)

			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CurrentDepthTexture)

			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DepthTexture)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, FadeTexture)
		END_SHADER_PARAMETER_STRUCT()
	};

	IMPLEMENT_GLOBAL_SHADER(FGroundInteractionMoveCS, "/Plugin/GroundInteraction/GroundInteractionMove.usf", "MainCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FGroundInteractionFadeCS, "/Plugin/GroundInteraction/GroundInteractionFade.usf", "MainCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FGroundInteractionUpdateDepthCS, "/Plugin/GroundInteraction/GroundInteractionUpdateDepth.usf", "MainCS", SF_Compute);

	static void AddMovePass(
		FRDGBuilder& GraphBuilder,
		ERHIFeatureLevel::Type FeatureLevel,
		FRDGTextureRef InputDepthTexture,
		FRDGTextureRef InputFadeTexture,
		FRDGTextureRef OutputDepthTexture,
		FRDGTextureRef OutputFadeTexture,
		const FGroundInteractionRenderFrameParams& FrameParams)
	{
		FGroundInteractionMoveCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FGroundInteractionMoveCS::FParameters>();

		const float MaxSceneDepth = GetMaxSceneDepth(FrameParams);

		PassParameters->TextureSize = FrameParams.RenderTargetSize;
		PassParameters->ScrollPixels = FrameParams.ScrollPixels;
		PassParameters->PrevCaptureViewOriginZ = FrameParams.PrevCaptureOriginWS.Z;
		PassParameters->CurrentCaptureViewOriginZ = FrameParams.CaptureInfo.GetCaptureOriginWS().Z;
		PassParameters->MaxSceneDepth = MaxSceneDepth;
		PassParameters->SceneDepthInvalidEpsilon = GetSceneDepthInvalidEpsilon(MaxSceneDepth);
		PassParameters->bHasHistory = FrameParams.bHasPrevCaptureInfo ? 1u : 0u;

		PassParameters->InputDepthTexture = InputDepthTexture;
		PassParameters->InputFadeTexture = InputFadeTexture;
		PassParameters->OutputDepthTexture = GraphBuilder.CreateUAV(OutputDepthTexture);
		PassParameters->OutputFadeTexture = GraphBuilder.CreateUAV(OutputFadeTexture);

		TShaderMapRef<FGroundInteractionMoveCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GroundInteraction.Move"),
			ComputeShader,
			PassParameters,
			GetGroupCount(FrameParams.RenderTargetSize));
	}

	static void AddFadePassInPlace(
		FRDGBuilder& GraphBuilder,
		ERHIFeatureLevel::Type FeatureLevel,
		FRDGTextureRef DepthTexture,
		FRDGTextureRef FadeTexture,
		const FGroundInteractionRenderFrameParams& FrameParams,
		float FadeDeltaTimeSeconds,
		float FadeRiseSpeed,
		float FadeMaxHeight)
	{
		FGroundInteractionFadeCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FGroundInteractionFadeCS::FParameters>();

		PassParameters->TextureSize = FrameParams.RenderTargetSize;
		PassParameters->DeltaTimeSeconds = FadeDeltaTimeSeconds;
		PassParameters->FadeRiseSpeed = FadeRiseSpeed;
		PassParameters->FadeMaxHeight = FadeMaxHeight;
		PassParameters->DepthTexture = GraphBuilder.CreateUAV(DepthTexture);
		PassParameters->FadeTexture = GraphBuilder.CreateUAV(FadeTexture);

		TShaderMapRef<FGroundInteractionFadeCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GroundInteraction.Fade"),
			ComputeShader,
			PassParameters,
			GetGroupCount(FrameParams.RenderTargetSize));
	}

	static void AddUpdateDepthPassInPlace(
		FRDGBuilder& GraphBuilder,
		ERHIFeatureLevel::Type FeatureLevel,
		FRDGTextureRef DepthTexture,
		FRDGTextureRef FadeTexture,
		FRDGTextureRef CurrentDepthTexture,
		const FGroundInteractionRenderFrameParams& FrameParams)
	{
		const float MaxSceneDepth = GetMaxSceneDepth(FrameParams);

		FGroundInteractionUpdateDepthCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FGroundInteractionUpdateDepthCS::FParameters>();

		PassParameters->TextureSize = FrameParams.RenderTargetSize;
		PassParameters->MaxSceneDepth = MaxSceneDepth;
		PassParameters->SceneDepthInvalidEpsilon = GetSceneDepthInvalidEpsilon(MaxSceneDepth);
		PassParameters->CurrentDepthTexture = CurrentDepthTexture;
		PassParameters->DepthTexture = GraphBuilder.CreateUAV(DepthTexture);
		PassParameters->FadeTexture = GraphBuilder.CreateUAV(FadeTexture);

		TShaderMapRef<FGroundInteractionUpdateDepthCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GroundInteraction.UpdateDepth"),
			ComputeShader,
			PassParameters,
			GetGroupCount(FrameParams.RenderTargetSize));
	}

	static void ExecutePreCaptureStatePasses(
		FGroundInteractionSceneViewExtension* Extension,
		ERHIFeatureLevel::Type FeatureLevel,
		const FGroundInteractionRenderFrameParams& FrameParams,
		float FadeRiseSpeed,
		float FadeMaxHeight,
		bool bDoMove,
		bool bDoFade,
		float FadeDeltaTimeSeconds,
		int32 PrevStateIndex,
		int32 CurrStateIndex)
	{
		if (!Extension || !FrameParams.bValid || (!bDoMove && !bDoFade))
		{
			return;
		}

		ENQUEUE_RENDER_COMMAND(GroundInteractionPreCaptureStatePasses)(
		[Extension, FeatureLevel, FrameParams, FadeRiseSpeed, FadeMaxHeight, bDoMove, bDoFade, FadeDeltaTimeSeconds, PrevStateIndex, CurrStateIndex]
			(FRHICommandListImmediate& RHICmdList)
		{
			TRefCountPtr<IPooledRenderTarget> CurrDepthRT =
				Extension->GetStateRT_RenderThread(CurrStateIndex, StateChannel_Depth);

			TRefCountPtr<IPooledRenderTarget> CurrFadeRT =
				Extension->GetStateRT_RenderThread(CurrStateIndex, StateChannel_Fade);

			TRefCountPtr<IPooledRenderTarget> PrevDepthRT;
			TRefCountPtr<IPooledRenderTarget> PrevFadeRT;

			if (bDoMove)
			{
				PrevDepthRT = Extension->GetStateRT_RenderThread(PrevStateIndex, StateChannel_Depth);
				PrevFadeRT = Extension->GetStateRT_RenderThread(PrevStateIndex, StateChannel_Fade);
			}

			if (!CurrDepthRT.IsValid() || !CurrFadeRT.IsValid() ||
				(bDoMove && (!PrevDepthRT.IsValid() || !PrevFadeRT.IsValid())))
			{
				return;
			}

			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef CurrDepthTexture = GraphBuilder.RegisterExternalTexture(
				CurrDepthRT,
				TEXT("GI_CurrDepth"));

			FRDGTextureRef CurrFadeTexture = GraphBuilder.RegisterExternalTexture(
				CurrFadeRT,
				TEXT("GI_CurrFade"));

			FRDGTextureRef PrevDepthTexture = nullptr;
			FRDGTextureRef PrevFadeTexture = nullptr;

			if (bDoMove)
			{
				PrevDepthTexture = GraphBuilder.RegisterExternalTexture(
					PrevDepthRT,
					TEXT("GI_PrevDepth"));

				PrevFadeTexture = GraphBuilder.RegisterExternalTexture(
					PrevFadeRT,
					TEXT("GI_PrevFade"));
			}

			if (!CurrDepthTexture || !CurrFadeTexture ||
				(bDoMove && (!PrevDepthTexture || !PrevFadeTexture)))
			{
				GraphBuilder.Execute();
				return;
			}

			GraphBuilder.UseInternalAccessMode(CurrDepthTexture);
			GraphBuilder.UseInternalAccessMode(CurrFadeTexture);

			if (PrevDepthTexture)
			{
				GraphBuilder.UseInternalAccessMode(PrevDepthTexture);
			}

			if (PrevFadeTexture)
			{
				GraphBuilder.UseInternalAccessMode(PrevFadeTexture);
			}

			if (bDoMove)
			{
				AddMovePass(
					GraphBuilder,
					FeatureLevel,
					PrevDepthTexture,
					PrevFadeTexture,
					CurrDepthTexture,
					CurrFadeTexture,
					FrameParams);
			}

		if (bDoFade)
		{
			AddFadePassInPlace(
				GraphBuilder,
				FeatureLevel,
				CurrDepthTexture,
				CurrFadeTexture,
				FrameParams,
				FadeDeltaTimeSeconds,
				FadeRiseSpeed,
				FadeMaxHeight);
		}

			GraphBuilder.UseExternalAccessMode(CurrDepthTexture, ERHIAccess::SRVMask);
			GraphBuilder.UseExternalAccessMode(CurrFadeTexture, ERHIAccess::SRVMask);

			if (PrevDepthTexture)
			{
				GraphBuilder.UseExternalAccessMode(PrevDepthTexture, ERHIAccess::SRVMask);
			}

			if (PrevFadeTexture)
			{
				GraphBuilder.UseExternalAccessMode(PrevFadeTexture, ERHIAccess::SRVMask);
			}

			GraphBuilder.Execute();
		});
	}

	static void ExecuteCopyStateToOutputPass(
	FGroundInteractionSceneViewExtension* Extension,
	FTextureRenderTargetResource* OutputDepthRTResource,
	FTextureRenderTargetResource* OutputFadeRTResource,
	int32 StateIndex)
	{
		if (!Extension || !OutputDepthRTResource || !OutputFadeRTResource)
		{
			return;
		}

		ENQUEUE_RENDER_COMMAND(GroundInteractionCopyStateToOutput)(
			[Extension, OutputDepthRTResource, OutputFadeRTResource, StateIndex]
			(FRHICommandListImmediate& RHICmdList)
			{
				TRefCountPtr<IPooledRenderTarget> DepthRT =
					Extension->GetStateRT_RenderThread(StateIndex, StateChannel_Depth);

				TRefCountPtr<IPooledRenderTarget> FadeRT =
					Extension->GetStateRT_RenderThread(StateIndex, StateChannel_Fade);

				if (!DepthRT.IsValid() || !FadeRT.IsValid())
				{
					return;
				}

				FRDGBuilder GraphBuilder(RHICmdList);

				FRDGTextureRef StateDepthTexture = GraphBuilder.RegisterExternalTexture(
					DepthRT,
					TEXT("GI_Final_StateDepth"));

				FRDGTextureRef StateFadeTexture = GraphBuilder.RegisterExternalTexture(
					FadeRT,
					TEXT("GI_Final_StateFade"));

				FRDGTextureRef OutputDepthTexture = RegisterExternalTexture(
					GraphBuilder,
					OutputDepthRTResource->GetRenderTargetTexture(),
					TEXT("GI_Output_Depth"));

				FRDGTextureRef OutputFadeTexture = RegisterExternalTexture(
					GraphBuilder,
					OutputFadeRTResource->GetRenderTargetTexture(),
					TEXT("GI_Output_Fade"));

				if (StateDepthTexture && StateFadeTexture && OutputDepthTexture && OutputFadeTexture)
				{
					GraphBuilder.UseInternalAccessMode(StateDepthTexture);
					GraphBuilder.UseInternalAccessMode(StateFadeTexture);
					GraphBuilder.UseInternalAccessMode(OutputDepthTexture);
					GraphBuilder.UseInternalAccessMode(OutputFadeTexture);

					AddCopyTexturePass(GraphBuilder, StateDepthTexture, OutputDepthTexture);
					AddCopyTexturePass(GraphBuilder, StateFadeTexture, OutputFadeTexture);

					GraphBuilder.UseExternalAccessMode(StateDepthTexture, ERHIAccess::SRVMask);
					GraphBuilder.UseExternalAccessMode(StateFadeTexture, ERHIAccess::SRVMask);
					GraphBuilder.UseExternalAccessMode(OutputDepthTexture, ERHIAccess::SRVMask);
					GraphBuilder.UseExternalAccessMode(OutputFadeTexture, ERHIAccess::SRVMask);
				}

				GraphBuilder.Execute();
			});
	}

	class FGroundInteractionDepthPass final : public FCustomRenderPassBase
	{
	public:
		IMPLEMENT_CUSTOM_RENDER_PASS(FGroundInteractionDepthPass);

		FGroundInteractionDepthPass(
			const FIntPoint& InRenderTargetSize,
			FGroundInteractionSceneViewExtension* InOwnerExtension,
			FTextureRenderTargetResource* InOutputDepthRTResource,
			FTextureRenderTargetResource* InOutputFadeRTResource,
			int32 InStateIndex,
			const FGroundInteractionRenderFrameParams& InFrameParams)
			: FCustomRenderPassBase(
				TEXT("GroundInteractionDepthPass"),
				FCustomRenderPassBase::ERenderMode::DepthPass,
				FCustomRenderPassBase::ERenderOutput::SceneDepth,
				InRenderTargetSize)
			, OwnerExtension(InOwnerExtension)
			, OutputDepthRTResource(InOutputDepthRTResource)
			, OutputFadeRTResource(InOutputFadeRTResource)
			, StateIndex(InStateIndex)
			, FrameParams(InFrameParams)
		{
		}

		virtual void OnPreRender(FRDGBuilder& GraphBuilder) override
		{
			const FRDGTextureDesc TextureDesc = FRDGTextureDesc::Create2D(
				RenderTargetSize,
				PF_R32_FLOAT,
				FClearValueBinding::Black,
				TexCreate_RenderTargetable | TexCreate_ShaderResource);

			RenderTargetTexture = GraphBuilder.CreateTexture(
				TextureDesc,
				TEXT("GroundInteractionCapturedDepth"));

			if (Views.Num() > 0 && Views[0] != nullptr)
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
			if (!OwnerExtension || !OutputDepthRTResource || !OutputFadeRTResource ||
				!RenderTargetTexture || Views.Num() == 0 || Views[0] == nullptr)
			{
				return;
			}

			TRefCountPtr<IPooledRenderTarget> DepthRT =
				OwnerExtension->GetStateRT_RenderThread(StateIndex, StateChannel_Depth);

			TRefCountPtr<IPooledRenderTarget> FadeRT =
				OwnerExtension->GetStateRT_RenderThread(StateIndex, StateChannel_Fade);

			if (!DepthRT.IsValid() || !FadeRT.IsValid())
			{
				return;
			}

			FRDGTextureRef StateDepthTexture = GraphBuilder.RegisterExternalTexture(
				DepthRT,
				TEXT("GI_StateDepth_Update"));

			FRDGTextureRef StateFadeTexture = GraphBuilder.RegisterExternalTexture(
				FadeRT,
				TEXT("GI_StateFade_Update"));

			FRDGTextureRef OutputDepthTexture = RegisterExternalTexture(
				GraphBuilder,
				OutputDepthRTResource->GetRenderTargetTexture(),
				TEXT("GI_OutputDepth_FinalCopy"));

			FRDGTextureRef OutputFadeTexture = RegisterExternalTexture(
				GraphBuilder,
				OutputFadeRTResource->GetRenderTargetTexture(),
				TEXT("GI_OutputFade_FinalCopy"));

			if (!StateDepthTexture || !StateFadeTexture || !OutputDepthTexture || !OutputFadeTexture)
			{
				return;
			}

			GraphBuilder.UseInternalAccessMode(StateDepthTexture);
			GraphBuilder.UseInternalAccessMode(StateFadeTexture);
			GraphBuilder.UseInternalAccessMode(OutputDepthTexture);
			GraphBuilder.UseInternalAccessMode(OutputFadeTexture);

			AddUpdateDepthPassInPlace(
				GraphBuilder,
				Views[0]->GetFeatureLevel(),
				StateDepthTexture,
				StateFadeTexture,
				RenderTargetTexture,
				FrameParams);

			AddCopyTexturePass(GraphBuilder, StateDepthTexture, OutputDepthTexture);
			AddCopyTexturePass(GraphBuilder, StateFadeTexture, OutputFadeTexture);

			GraphBuilder.UseExternalAccessMode(StateDepthTexture, ERHIAccess::SRVMask);
			GraphBuilder.UseExternalAccessMode(StateFadeTexture, ERHIAccess::SRVMask);
			GraphBuilder.UseExternalAccessMode(OutputDepthTexture, ERHIAccess::SRVMask);
			GraphBuilder.UseExternalAccessMode(OutputFadeTexture, ERHIAccess::SRVMask);
		}

	private:
		FGroundInteractionSceneViewExtension* OwnerExtension = nullptr;
		FTextureRenderTargetResource* OutputDepthRTResource = nullptr;
		FTextureRenderTargetResource* OutputFadeRTResource = nullptr;
		int32 StateIndex = 0;
		FGroundInteractionRenderFrameParams FrameParams;
	};
}


FGroundInteractionSceneViewExtension::FGroundInteractionSceneViewExtension(
	const FAutoRegister& AutoRegister,
	UGroundInteractionWorldSubsystem* InSubsystem,
	UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
	, OwnerSubsystem(InSubsystem)
{
}

bool FGroundInteractionSceneViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	if (!FWorldSceneViewExtension::IsActiveThisFrame_Internal(Context))
	{
		return false;
	}

	const UGroundInteractionWorldSubsystem* Subsystem = OwnerSubsystem.Get();
	if (!Subsystem || !Subsystem->AreAssetsReady())
	{
		return false;
	}

	UWorld* CurrentWorld = Subsystem->GetWorld();
	if (!CurrentWorld)
	{
		return false;
	}

	return CurrentWorld->WorldType == EWorldType::PIE || CurrentWorld->WorldType == EWorldType::Game;
}

TRefCountPtr<IPooledRenderTarget> FGroundInteractionSceneViewExtension::GetStateRT_RenderThread(
	int32 SwapIndex,
	int32 ChannelIndex) const
{
	if (SwapIndex < 0 || SwapIndex >= 2 || ChannelIndex < 0 || ChannelIndex >= 2)
	{
		return nullptr;
	}

	return StateRT[SwapIndex][ChannelIndex];
}

void FGroundInteractionSceneViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	UGroundInteractionWorldSubsystem* Subsystem = OwnerSubsystem.Get();
	if (!Subsystem || !Subsystem->AreAssetsReady() || !InViewFamily.Scene || InViewFamily.Views.Num() == 0)
	{
		return;
	}

	const FSceneView* PrimaryView = InViewFamily.Views[0];
	if (!PrimaryView || PrimaryView->bIsSceneCapture || PrimaryView->bIsReflectionCapture)
	{
		return;
	}

	const UGroundInteractionSettings* Settings = UGroundInteractionSettings::Get();
	if (!Settings)
	{
		return;
	}

	UTextureRenderTarget2D* OutputDepthRT = Subsystem->GetDepthRT();
	UTextureRenderTarget2D* OutputFadeRT = Subsystem->GetFadeRT();

	if (!OutputDepthRT || !OutputFadeRT)
	{
		return;
	}

	const bool bOutputRTChanged = Subsystem->ApplyOutputRenderTargetSettings_GameThread();
	if (bOutputRTChanged)
	{
		CurrentStateIndex = 0;
		FadeUpdateAccumulatorSeconds = 0.0f;
		CaptureUpdateAccumulatorSeconds = 0.0f;
		Subsystem->ResetHistory();
	}

	FTextureRenderTargetResource* OutputDepthRTResource = Subsystem->GetDepthRTResource_GameThread();
	FTextureRenderTargetResource* OutputFadeRTResource = Subsystem->GetFadeRTResource_GameThread();

	if (!OutputDepthRTResource || !OutputFadeRTResource)
	{
		return;
	}

	const FIntPoint RTSize(
		OutputDepthRT->SizeX > 0 ? OutputDepthRT->SizeX : 512,
		OutputDepthRT->SizeY > 0 ? OutputDepthRT->SizeY : 512);

	FGroundInteractionRenderFrameParams FrameParams;
	if (!Subsystem->BuildRenderFrameParams(PrimaryView, RTSize, FrameParams))
	{
		return;
	}

	Subsystem->UpdateMPC(FrameParams.CaptureInfo);
	
	// Update rate는 현실 시간 기준으로 계산한다.
	// 현재 BuildRenderFrameParams()의 DeltaTimeSeconds는 World time이므로 여기서는 쓰지 않는다.
	const float DeltaRealTimeSeconds = FMath::Max(
		InViewFamily.Time.GetDeltaRealTimeSeconds(),
		0.0f);

	// Fade tick
	const float FadeUpdateIntervalSeconds = Settings->GetDepthFadeUpdateIntervalSeconds();

	FadeUpdateAccumulatorSeconds += DeltaRealTimeSeconds;

	bool bDoFade = false;
	float FadeDeltaTimeSeconds = 0.0f;

	if (FadeUpdateAccumulatorSeconds >= FadeUpdateIntervalSeconds)
	{
		const int32 FadeStepCount = FMath::Max(
			FMath::FloorToInt(FadeUpdateAccumulatorSeconds / FadeUpdateIntervalSeconds),
			1);

		FadeDeltaTimeSeconds = float(FadeStepCount) * FadeUpdateIntervalSeconds;
		FadeUpdateAccumulatorSeconds -= FadeDeltaTimeSeconds;
		bDoFade = true;
	}

	// Capture / UpdateDepth tick
	const float CaptureUpdateIntervalSeconds = Settings->GetDepthCaptureUpdateIntervalSeconds();

	CaptureUpdateAccumulatorSeconds += DeltaRealTimeSeconds;

	bool bDoCaptureUpdate = false;

	if (CaptureUpdateAccumulatorSeconds >= CaptureUpdateIntervalSeconds)
	{
		const int32 CaptureStepCount = FMath::Max(
			FMath::FloorToInt(CaptureUpdateAccumulatorSeconds / CaptureUpdateIntervalSeconds),
			1);

		CaptureUpdateAccumulatorSeconds -= float(CaptureStepCount) * CaptureUpdateIntervalSeconds;
		bDoCaptureUpdate = true;
	}

	// bForceCaptureUpdate 변수 대신 기존 history 여부로 강제 capture.
	// 첫 프레임 또는 ResetHistory() 직후에는 반드시 capture/update가 돈다.
	if (!FrameParams.bHasPrevCaptureInfo)
	{
		bDoCaptureUpdate = true;
		CaptureUpdateAccumulatorSeconds = 0.0f;
	}

	const bool bDoMove = FrameParams.bShouldMove;

	const int32 PrevStateIndex = CurrentStateIndex;
	const int32 CurrStateIndex = bDoMove
		? (1 - CurrentStateIndex)
		: CurrentStateIndex;

	if (bDoMove)
	{
		CurrentStateIndex = CurrStateIndex;
	}

	ENQUEUE_RENDER_COMMAND(GroundInteractionCreateStateRTsIfNeeded)(
		[this, RTSize](FRHICommandListImmediate& RHICmdList)
		{
			bool bAllocatedAny = false;

			for (int32 SwapIndex = 0; SwapIndex < 2; ++SwapIndex)
			{
				for (int32 ChannelIndex = 0; ChannelIndex < 2; ++ChannelIndex)
				{
					const bool bNeedsAlloc =
						!StateRT[SwapIndex][ChannelIndex].IsValid() ||
						StateRT[SwapIndex][ChannelIndex]->GetDesc().Extent != RTSize ||
						StateRT[SwapIndex][ChannelIndex]->GetDesc().Format != GroundInteraction::StatePixelFormat;

					if (!bNeedsAlloc)
					{
						continue;
					}

					const TCHAR* DebugName =
						GroundInteraction::GetStateDebugName(SwapIndex, ChannelIndex);

					FPooledRenderTargetDesc Desc =
						GroundInteraction::CreateStatePooledTextureDesc(RTSize);

					Desc.DebugName = DebugName;

					GRenderTargetPool.FindFreeElement(
						RHICmdList,
						Desc,
						StateRT[SwapIndex][ChannelIndex],
						DebugName);

					bAllocatedAny = true;
				}
			}

			if (bAllocatedAny)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				for (int32 SwapIndex = 0; SwapIndex < 2; ++SwapIndex)
				{
					TRefCountPtr<IPooledRenderTarget> DepthRT =
						StateRT[SwapIndex][GroundInteraction::StateChannel_Depth];

					TRefCountPtr<IPooledRenderTarget> FadeRT =
						StateRT[SwapIndex][GroundInteraction::StateChannel_Fade];

					if (!DepthRT.IsValid() || !FadeRT.IsValid())
					{
						continue;
					}

					FRDGTextureRef DepthTexture = GraphBuilder.RegisterExternalTexture(
						DepthRT,
						TEXT("GI_StateDepth_Clear"));

					FRDGTextureRef FadeTexture = GraphBuilder.RegisterExternalTexture(
						FadeRT,
						TEXT("GI_StateFade_Clear"));

					if (DepthTexture && FadeTexture)
					{
						GraphBuilder.UseInternalAccessMode(DepthTexture);
						GraphBuilder.UseInternalAccessMode(FadeTexture);

						GroundInteraction::AddClearDepthPass(GraphBuilder, DepthTexture);
						GroundInteraction::AddClearFadePass(GraphBuilder, FadeTexture);

						GraphBuilder.UseExternalAccessMode(DepthTexture, ERHIAccess::SRVMask);
						GraphBuilder.UseExternalAccessMode(FadeTexture, ERHIAccess::SRVMask);
					}
				}

				GraphBuilder.Execute();
			}
		});

	GroundInteraction::ExecutePreCaptureStatePasses(
		this,
		PrimaryView->GetFeatureLevel(),
		FrameParams,
		Settings->FadeRiseSpeed,
		Settings->FadeMaxHeight,
		bDoMove,
		bDoFade,
		FadeDeltaTimeSeconds,
		PrevStateIndex,
		CurrStateIndex);
	
	
	const bool bStateChangedBeforeCapture = bDoMove || bDoFade;

	if (!bDoCaptureUpdate)
	{
		if (bStateChangedBeforeCapture)
		{
			GroundInteraction::ExecuteCopyStateToOutputPass(
				this,
				OutputDepthRTResource,
				OutputFadeRTResource,
				CurrStateIndex);
		}

		Subsystem->CommitRenderFrame(FrameParams);
		return;
	}
	
	TSet<FPrimitiveComponentId> ShowOnlyPrimitives;
	Subsystem->BuildShowOnlyPrimitives(FrameParams.CaptureInfo, ShowOnlyPrimitives);

	if (ShowOnlyPrimitives.Num() == 0)
	{
		if (bStateChangedBeforeCapture)
		{
			GroundInteraction::ExecuteCopyStateToOutputPass(
				this,
				OutputDepthRTResource,
				OutputFadeRTResource,
				CurrStateIndex);
		}

		Subsystem->CommitRenderFrame(FrameParams);
		return;
	}

	const float HalfExtentX = FMath::Max(FrameParams.CaptureInfo.ExtentWS.X, 1.0f);
	const float HalfExtentY = FMath::Max(FrameParams.CaptureInfo.ExtentWS.Y, 1.0f);

	const FVector CaptureOriginWS = FrameParams.CaptureInfo.GetCaptureOriginWS();
	const FVector ViewLocation = CaptureOriginWS;
	const FVector LookAt = ViewLocation + FVector(0.0f, 0.0f, 1.0f);

	FSceneInterface::FCustomRenderPassRendererInput PassInput;
	PassInput.ViewLocation = ViewLocation;
	PassInput.ViewRotationMatrix = FLookAtMatrix(ViewLocation, LookAt, FVector(0.0f, 1.0f, 0.0f));
	PassInput.ViewRotationMatrix = PassInput.ViewRotationMatrix.RemoveTranslation();
	PassInput.ViewRotationMatrix.RemoveScaling();

	const float MaxDepth = FrameParams.CaptureInfo.ExtentWS.Z * 2.0f;
	PassInput.ProjectionMatrix = BuildBottomUpOrthoProjection(HalfExtentX, HalfExtentY, MaxDepth);

	PassInput.ViewActor = nullptr;
	PassInput.ShowOnlyPrimitives = MoveTemp(ShowOnlyPrimitives);

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

	PassInput.CustomRenderPass = new GroundInteraction::FGroundInteractionDepthPass(
		RTSize,
		this,
		OutputDepthRTResource,
		OutputFadeRTResource,
		CurrStateIndex,
		FrameParams);

	InViewFamily.Scene->AddCustomRenderPass(&InViewFamily, PassInput);

	Subsystem->CommitRenderFrame(FrameParams);
}

FMatrix FGroundInteractionSceneViewExtension::BuildBottomUpOrthoProjection(
	float HalfExtentX,
	float HalfExtentY,
	float MaxDepth)
{
	check((int32)ERHIZBuffer::IsInverted);

	const float SafeMaxDepth = FMath::Max(MaxDepth, 1.0f);
	const float ZScale = 1.0f / SafeMaxDepth;
	const float ZOffset = 0.0f;

	return FReversedZOrthoMatrix(
		HalfExtentX,
		HalfExtentY,
		ZScale,
		ZOffset);
}