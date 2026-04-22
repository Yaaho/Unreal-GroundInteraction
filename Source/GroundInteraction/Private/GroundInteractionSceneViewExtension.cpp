// Fill out your copyright notice in the Description page of Project Settings.


#include "GroundInteractionSceneViewExtension.h"
#include "GroundInteractionWorldSubsystem.h"
#include "GroundInteractionSettings.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Rendering/CustomRenderPass.h"
#include "Runtime/Renderer/Private/SceneRendering.h"

#include "GlobalShader.h"


namespace GroundInteraction
{
	static constexpr int32 ComputeGroupSizeX = 8;
	static constexpr int32 ComputeGroupSizeY = 8;
	
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
	
	static FRDGTextureDesc CreateStateTextureDesc(const FIntPoint& TextureSize, EPixelFormat Format)
	{
		return FRDGTextureDesc::Create2D(
			TextureSize,
			Format,
			FClearValueBinding(FLinearColor(0.0f, -32768.0f, 0.0f, 0.0f)),
			TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable);
	}
	
	// 텍스쳐를 스냅된 중심 위치로 Move 하는 컴퓨스 셰이더 패스
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
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, InputStateTexture)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutputStateTexture)
		END_SHADER_PARAMETER_STRUCT()
	};
	
	// 캡쳐한 결과로 텍스쳐를 업데이트하는 컴퓨트 셰이더 패스
	class FGroundInteractionUpdateDepthCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGroundInteractionUpdateDepthCS);
		SHADER_USE_PARAMETER_STRUCT(FGroundInteractionUpdateDepthCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(FIntPoint, TextureSize)
			SHADER_PARAMETER(float, MaxSceneDepth)
			SHADER_PARAMETER(float, SceneDepthInvalidEpsilon)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, InputStateTexture)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CurrentDepthTexture)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutputStateTexture)
		END_SHADER_PARAMETER_STRUCT()
	};
	
	// 폴리지 상호작용을 위한 Depth Fade 가 일어나는 패스
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
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, InputStateTexture)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutputStateTexture)
		END_SHADER_PARAMETER_STRUCT()
	};
	
	IMPLEMENT_GLOBAL_SHADER(FGroundInteractionMoveCS, "/Plugin/GroundInteraction/GroundInteractionMove.usf", "MainCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FGroundInteractionUpdateDepthCS, "/Plugin/GroundInteraction/GroundInteractionUpdateDepth.usf", "MainCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FGroundInteractionFadeCS, "/Plugin/GroundInteraction/GroundInteractionFade.usf", "MainCS", SF_Compute);
	
	
	
	static FRDGTextureRef AddMovePass(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FRDGTextureRef InputStateTexture,
	const FGroundInteractionRenderFrameParams& FrameParams)
	{
		FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(
			CreateStateTextureDesc(FrameParams.RenderTargetSize, InputStateTexture->Desc.Format),
			TEXT("GI_MovedState"));

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
		PassParameters->InputStateTexture = InputStateTexture;
		PassParameters->OutputStateTexture = GraphBuilder.CreateUAV(OutputTexture);

		TShaderMapRef<FGroundInteractionMoveCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GroundInteraction.Move"),
			ComputeShader,
			PassParameters,
			GetGroupCount(FrameParams.RenderTargetSize));

		return OutputTexture;
	}

	static FRDGTextureRef AddUpdateDepthPass(
		FRDGBuilder& GraphBuilder,
		ERHIFeatureLevel::Type FeatureLevel,
		FRDGTextureRef InputStateTexture,
		FRDGTextureRef CurrentDepthTexture,
		const FGroundInteractionRenderFrameParams& FrameParams)
	{
		const FIntPoint TextureSize = FrameParams.RenderTargetSize;
		const float MaxSceneDepth = GetMaxSceneDepth(FrameParams);

		FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(
			CreateStateTextureDesc(TextureSize, InputStateTexture->Desc.Format),
			TEXT("GI_UpdatedState"));

		FGroundInteractionUpdateDepthCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FGroundInteractionUpdateDepthCS::FParameters>();
	
		PassParameters->TextureSize = TextureSize;
		PassParameters->MaxSceneDepth = MaxSceneDepth;
		PassParameters->SceneDepthInvalidEpsilon = GetSceneDepthInvalidEpsilon(MaxSceneDepth);
		PassParameters->InputStateTexture = InputStateTexture;
		PassParameters->CurrentDepthTexture = CurrentDepthTexture;
		PassParameters->OutputStateTexture = GraphBuilder.CreateUAV(OutputTexture);

		TShaderMapRef<FGroundInteractionUpdateDepthCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GroundInteraction.UpdateDepth"),
			ComputeShader,
			PassParameters,
			GetGroupCount(TextureSize));

		return OutputTexture;
	}

	static FRDGTextureRef AddFadePass(
		FRDGBuilder& GraphBuilder,
		ERHIFeatureLevel::Type FeatureLevel,
		FRDGTextureRef InputStateTexture,
		const FGroundInteractionRenderFrameParams& FrameParams,
		float FadeRiseSpeed,
		float FadeMaxHeight)
	{
		FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(
			CreateStateTextureDesc(FrameParams.RenderTargetSize, InputStateTexture->Desc.Format),
			TEXT("GI_FadedState"));

		FGroundInteractionFadeCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FGroundInteractionFadeCS::FParameters>();

		PassParameters->TextureSize = FrameParams.RenderTargetSize;
		PassParameters->DeltaTimeSeconds = FrameParams.DeltaTimeSeconds;
		PassParameters->FadeRiseSpeed = FadeRiseSpeed;
		PassParameters->FadeMaxHeight = FadeMaxHeight;
		PassParameters->InputStateTexture = InputStateTexture;
		PassParameters->OutputStateTexture = GraphBuilder.CreateUAV(OutputTexture);

		TShaderMapRef<FGroundInteractionFadeCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GroundInteraction.Fade"),
			ComputeShader,
			PassParameters,
			GetGroupCount(FrameParams.RenderTargetSize));

		return OutputTexture;
	}
	
	
	
	static void ExecuteMovePass(
		FTextureRenderTargetResource* StateRTResource,
		ERHIFeatureLevel::Type FeatureLevel,
		const FGroundInteractionRenderFrameParams& FrameParams)
	{
		if (!StateRTResource || !FrameParams.bValid)
		{
			return;
		}

		ENQUEUE_RENDER_COMMAND(GroundInteractionMovePass)(
			[StateRTResource, FeatureLevel, FrameParams](FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				FRDGTextureRef ExternalStateTexture = RegisterExternalTexture(
					GraphBuilder,
					StateRTResource->GetRenderTargetTexture(),
					TEXT("GI_ExternalState_Move"));

				if (ExternalStateTexture)
				{
					GraphBuilder.UseInternalAccessMode(ExternalStateTexture);

					FRDGTextureRef MovedTexture = AddMovePass(
						GraphBuilder,
						FeatureLevel,
						ExternalStateTexture,
						FrameParams);

					AddCopyTexturePass(GraphBuilder, MovedTexture, ExternalStateTexture);
					GraphBuilder.UseExternalAccessMode(ExternalStateTexture, ERHIAccess::SRVMask);
				}

				GraphBuilder.Execute();
			});
	}

	static void ExecuteFadePass(
		FTextureRenderTargetResource* StateRTResource,
		ERHIFeatureLevel::Type FeatureLevel,
		const FGroundInteractionRenderFrameParams& FrameParams,
		float FadeRiseSpeed,
		float FadeMaxHeight)
	{
		if (!StateRTResource || !FrameParams.bValid)
		{
			return;
		}

		ENQUEUE_RENDER_COMMAND(GroundInteractionFadePass)(
			[StateRTResource, FeatureLevel, FrameParams, FadeRiseSpeed, FadeMaxHeight](FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				FRDGTextureRef ExternalStateTexture = RegisterExternalTexture(
					GraphBuilder,
					StateRTResource->GetRenderTargetTexture(),
					TEXT("GI_ExternalState_Fade"));

				if (ExternalStateTexture)
				{
					GraphBuilder.UseInternalAccessMode(ExternalStateTexture);

					FRDGTextureRef FadedTexture = AddFadePass(
						GraphBuilder,
						FeatureLevel,
						ExternalStateTexture,
						FrameParams,
						FadeRiseSpeed,
						FadeMaxHeight);

					AddCopyTexturePass(GraphBuilder, FadedTexture, ExternalStateTexture);
					GraphBuilder.UseExternalAccessMode(ExternalStateTexture, ERHIAccess::SRVMask);
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
			FTextureRenderTargetResource* InStateRTResource,
			const FGroundInteractionRenderFrameParams& InFrameParams)
		: FCustomRenderPassBase(
			TEXT("GroundInteractionDepthPass"),
			FCustomRenderPassBase::ERenderMode::DepthPass,
			FCustomRenderPassBase::ERenderOutput::SceneDepth,
			InRenderTargetSize)
		, StateRTResource(InStateRTResource)
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
			if (!StateRTResource || !RenderTargetTexture || Views.Num() == 0 || Views[0] == nullptr)
			{
				return;
			}

			FRDGTextureRef ExternalStateTexture = RegisterExternalTexture(
				GraphBuilder,
				StateRTResource->GetRenderTargetTexture(),
				TEXT("GI_ExternalState_Update"));

			if (!ExternalStateTexture)
			{
				return;
			}

			GraphBuilder.UseInternalAccessMode(ExternalStateTexture);

			FRDGTextureRef UpdatedTexture = AddUpdateDepthPass(
				GraphBuilder,
				Views[0]->GetFeatureLevel(),
				ExternalStateTexture,
				RenderTargetTexture,
				FrameParams);

			AddCopyTexturePass(GraphBuilder, UpdatedTexture, ExternalStateTexture);
			GraphBuilder.UseExternalAccessMode(ExternalStateTexture, ERHIAccess::SRVMask);
		}

	private:
		FTextureRenderTargetResource* StateRTResource = nullptr;
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

	UTextureRenderTarget2D* OutputRT = Subsystem->GetDepthRT();
	if (!OutputRT)
	{
		return;
	}

	FTextureRenderTargetResource* OutputRTResource = Subsystem->GetDepthRTResource_GameThread();
	if (!OutputRTResource)
	{
		return;
	}

	const FIntPoint RTSize(
		OutputRT->SizeX > 0 ? OutputRT->SizeX : 512,
		OutputRT->SizeY > 0 ? OutputRT->SizeY : 512);

	FGroundInteractionRenderFrameParams FrameParams;
	if (!Subsystem->BuildRenderFrameParams(PrimaryView, RTSize, FrameParams))
	{
		return;
	}

	Subsystem->UpdateMPC(FrameParams.CaptureInfo);

	if (FrameParams.bShouldMove)
	{
		GroundInteraction::ExecuteMovePass(
			OutputRTResource,
			PrimaryView->GetFeatureLevel(),
			FrameParams);
	}

	GroundInteraction::ExecuteFadePass(
		OutputRTResource,
		PrimaryView->GetFeatureLevel(),
		FrameParams,
		Settings->FadeRiseSpeed,
		Settings->FadeMaxHeight);

	TSet<FPrimitiveComponentId> ShowOnlyPrimitives;
	Subsystem->BuildShowOnlyPrimitives(FrameParams.CaptureInfo, ShowOnlyPrimitives);

	if (ShowOnlyPrimitives.Num() == 0)
	{
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
		OutputRTResource,
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