// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GroundInteractionWorldSubsystem.generated.h"


class FGroundInteractionSceneViewExtension;
class UGroundInteractionTargetComponent;
struct FStreamableHandle;

// 현재 캡쳐 영역의 정보를 저장하는 구조체
USTRUCT()
struct FGroundInteractionCaptureInfo
{
	GENERATED_BODY()

	UPROPERTY()
	bool bValid = false;
	
	// 캡처 중심 위치
	UPROPERTY()
	FVector CenterWS = FVector::ZeroVector;
	
	// 캡쳐 프러스텀의 바운드
	UPROPERTY()
	FVector ExtentWS = FVector(1000.0f, 1000.0f, 1000.0f);
	
	// 캡쳐 View 의 위치
	FVector GetCaptureOriginWS() const
	{
		return FVector(CenterWS.X, CenterWS.Y, CenterWS.Z - ExtentWS.Z);
	}
};

// UGroundInteractionTargetComponent 의 스켈레탈 매시와 거가 붙은 인터랙션 프록시들을 저장하는 구조체
USTRUCT()
struct FGroundInteractionTargetInfo
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> Skeletal = nullptr;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> InteractionBodies;
};

struct FGroundInteractionRenderFrameParams
{
	bool bValid = false;
	bool bHasPrevCaptureInfo = false;
	bool bShouldMove = false;

	FGroundInteractionCaptureInfo CaptureInfo;
	FVector PrevCaptureOriginWS = FVector::ZeroVector;

	FIntPoint RenderTargetSize = FIntPoint::ZeroValue;
	FIntPoint ScrollPixels = FIntPoint::ZeroValue;

	float DeltaTimeSeconds = 0.0f;
};

/**
 * 
 */
UCLASS()
class GROUNDINTERACTION_API UGroundInteractionWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
	
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	void RegisterTarget(UGroundInteractionTargetComponent* Target);
	void UnregisterTarget(UGroundInteractionTargetComponent* Target);
	
	bool BuildRenderFrameParams(
		const FSceneView* PrimaryView,
		const FIntPoint& RTSize,
		FGroundInteractionRenderFrameParams& OutParams) const;

	void CommitRenderFrame(const FGroundInteractionRenderFrameParams& FrameParams);

	void BuildShowOnlyPrimitives(
		const FGroundInteractionCaptureInfo& CaptureInfo,
		TSet<FPrimitiveComponentId>& OutPrimitiveIds) const;

	void UpdateMPC(const FGroundInteractionCaptureInfo& Region) const;
	
	UTextureRenderTarget2D* GetDepthRT() const
	{
		return DepthRT;
	}
	
	UTextureRenderTarget2D* GetFadeRT() const
	{
		return FadeRT;
	}
	
	// 게임 스레드에서 RT 리소스를 pass 에 넘기기 위한 함수
	FTextureRenderTargetResource* GetDepthRTResource_GameThread() const;
	FTextureRenderTargetResource* GetFadeRTResource_GameThread() const;
	
	bool ApplyOutputRenderTargetSettings_GameThread();
	void ResetHistory();
	
	bool AreAssetsReady() const
	{
		return bAssetsReady;
	}

	const TSharedPtr<FGroundInteractionSceneViewExtension, ESPMode::ThreadSafe>& GetSceneViewExtension() const
	{
		return SceneViewExtension;
	}

private:
	// 서브시스템 초기화 시점에 필요한 에셋들을 비동기 로드 요청하는 함수
	void BeginAsyncLoadAssets();
	// 에셋 로드가 완료되면 호출되는 함수
	void OnAssetsLoaded();
	
	bool InitOutputRenderTarget_GameThread(UTextureRenderTarget2D* RenderTarget, const FIntPoint& Size, const FLinearColor& ClearColor) const;
	
	// 캡쳐 대상 항목을 빋드하는 함수
	void RebuildCaptureTargetInfo(UGroundInteractionTargetComponent* Target);
	void DestroyCaptureTargetInfo(UGroundInteractionTargetComponent* Target);

	void BuildProxies(FGroundInteractionTargetInfo& TargetInfo, USkeletalMeshComponent* SkeletalMeshComp);

	UStaticMeshComponent* CreateProxy(
		AActor* OwnerActor,
		USkeletalMeshComponent* Parent,
		const FName BoneName,
		UStaticMesh* StaticMesh,
		const FTransform& RelativeTransform,
		const FVector& DesiredWorldSize);

private:
	UPROPERTY(Transient)
	TMap<TObjectPtr<UGroundInteractionTargetComponent>, FGroundInteractionTargetInfo> GroundInteractionTargets;
	
	// Depth 캡쳐 결과를 저장하는 렌더 타겟
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DepthRT = nullptr;
	
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> FadeRT = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CubeMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> SphereMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CylinderMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> MaterialParameterCollection = nullptr;

	// 에셋 로드 완료 여부
	bool bAssetsReady = false;
	
	FGroundInteractionCaptureInfo PrevCaptureInfo;
	bool bHasPrevCaptureInfo = false;

	TSharedPtr<FStreamableHandle> AssetLoadHandle;
	TSharedPtr<FGroundInteractionSceneViewExtension, ESPMode::ThreadSafe> SceneViewExtension;
};
