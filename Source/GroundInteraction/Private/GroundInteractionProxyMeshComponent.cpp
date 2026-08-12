// Fill out your copyright notice in the Description page of Project Settings.

#include "GroundInteractionProxyMeshComponent.h"

#include "GroundInteractionCommon.h"

#include "Rendering/CustomRenderPass.h"
#include "SceneView.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshSceneProxy.h"

class FGroundInteractionProxyMeshSceneProxy final : public FStaticMeshSceneProxy
{
public:
	explicit FGroundInteractionProxyMeshSceneProxy(UGroundInteractionProxyMeshComponent* Component)
		: FStaticMeshSceneProxy(Component, true)
	{
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result = FStaticMeshSceneProxy::GetViewRelevance(View);

		bool bIsGroundInteractionDepthPass = false;
		if (View && View->CustomRenderPass)
		{
			const FName DepthPassTypeName = View->CustomRenderPass->GetTypeName();
			bIsGroundInteractionDepthPass = DepthPassTypeName == GroundInteractionCommon::GetGroundDepthPassTypeName();
		}

		Result.bDrawRelevance = Result.bDrawRelevance && bIsGroundInteractionDepthPass;
		Result.bShadowRelevance = false;

		return Result;
	}

	virtual bool GetMeshElement(
		int32 LODIndex,
		int32 BatchIndex,
		int32 SectionIndex,
		uint8 InDepthPriorityGroup,
		bool bUseSelectionOutline,
		bool bAllowPreCulledIndices,
		FMeshBatch& OutMeshBatch) const override
	{
		const bool bResult = FStaticMeshSceneProxy::GetMeshElement(
			LODIndex,
			BatchIndex,
			SectionIndex,
			InDepthPriorityGroup,
			bUseSelectionOutline,
			bAllowPreCulledIndices,
			OutMeshBatch);

		if (bResult)
		{
			// 캐릭터 좌우 반전에 의해 매시가 음수 스케일이 되어도 Depth 가 잘 렌더링 되도록 
			// Ground Interaction 프록시의 양면을 depth pass에 렌더링한다.  
			OutMeshBatch.bDisableBackfaceCulling = true;
		}

		return bResult;
	}

	SIZE_T GetTypeHash() const
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}
};

UGroundInteractionProxyMeshComponent::UGroundInteractionProxyMeshComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	SetCastShadow(false);
	SetReceivesDecals(false);

	bCastDynamicShadow = false;
	bAffectDistanceFieldLighting = false;
	bSelectable = false;
}

FPrimitiveSceneProxy* UGroundInteractionProxyMeshComponent::CreateSceneProxy()
{
	UStaticMesh* Mesh = GetStaticMesh();
	if (!Mesh || !Mesh->GetRenderData())
	{
		return nullptr;
	}

	return new FGroundInteractionProxyMeshSceneProxy(this);
}
