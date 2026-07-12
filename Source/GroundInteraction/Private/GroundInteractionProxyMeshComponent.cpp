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
			bIsGroundInteractionDepthPass = View->CustomRenderPass->GetTypeName() == GroundInteractionCommon::GetDepthCapturePassTypeName();
		}

		Result.bDrawRelevance = Result.bDrawRelevance && bIsGroundInteractionDepthPass;
		Result.bShadowRelevance = false;

		return Result;
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
