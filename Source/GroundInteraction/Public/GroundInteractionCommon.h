// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// GroundInteraction의 월드 좌표 스냅 등의 계산을 이곳에 선언
namespace GroundInteractionCommon
{
	FORCEINLINE FIntPoint MakeSafeRenderTargetSize(
		const FIntPoint& InSize)
	{
		return FIntPoint(
			FMath::Max(InSize.X, 1),
			FMath::Max(InSize.Y, 1));
	}

	FORCEINLINE FVector MakeSafeCaptureExtent(
		const FVector& InExtentWS)
	{
		return FVector(
			FMath::Max(InExtentWS.X, 1.0),
			FMath::Max(InExtentWS.Y, 1.0),
			FMath::Max(InExtentWS.Z, 1.0));
	}

	FORCEINLINE FVector2D GetUnitsPerPixel(
		const FVector& CaptureExtentWS,
		const FIntPoint& RenderTargetSize)
	{
		const FVector SafeExtentWS = MakeSafeCaptureExtent(CaptureExtentWS);

		const FIntPoint SafeSize = MakeSafeRenderTargetSize(RenderTargetSize);

		return FVector2D(
			SafeExtentWS.X * 2.0 / static_cast<double>(SafeSize.X),
			SafeExtentWS.Y * 2.0 / static_cast<double>(SafeSize.Y));
	}

	// 모든 capture center는 이 함수로만 만든다.
	FORCEINLINE FVector SnapCaptureCenterWS(
		const FVector& RawCenterWS,
		const FVector& CaptureExtentWS,
		const FIntPoint& RenderTargetSize)
	{
		const FVector SafeExtentWS =
			MakeSafeCaptureExtent(CaptureExtentWS);

		const FIntPoint SafeSize =
			MakeSafeRenderTargetSize(RenderTargetSize);

		const FVector2D UnitsPerPixel =
			GetUnitsPerPixel(SafeExtentWS, SafeSize);

		const double CaptureSnapStep =
			FMath::Max3(
				UnitsPerPixel.X,
				UnitsPerPixel.Y,
				1.0);

		const double SnappedX =
			(FMath::FloorToDouble(RawCenterWS.X / UnitsPerPixel.X)) *
			UnitsPerPixel.X;

		const double SnappedY =
			(FMath::FloorToDouble(RawCenterWS.Y / UnitsPerPixel.Y)) *
			UnitsPerPixel.Y;

		const double SnappedZ =
			(FMath::FloorToDouble(RawCenterWS.Z / CaptureSnapStep)) *
			CaptureSnapStep;

		return FVector(
			SnappedX,
			SnappedY,
			SnappedZ);
	}

	FORCEINLINE FVector GetCaptureOriginWS(
		const FVector& CaptureCenterWS,
		const FVector& CaptureExtentWS)
	{
		const FVector SafeExtentWS =
			MakeSafeCaptureExtent(CaptureExtentWS);

		return FVector(
			CaptureCenterWS.X,
			CaptureCenterWS.Y,
			CaptureCenterWS.Z - SafeExtentWS.Z);
	}

	FORCEINLINE float GetMaxSceneDepth(
		const FVector& CaptureExtentWS)
	{
		const FVector SafeExtentWS =
			MakeSafeCaptureExtent(CaptureExtentWS);

		return FMath::Max(
			SafeExtentWS.Z * 2.0f,
			1.0f);
	}

	FORCEINLINE float GetSceneDepthInvalidEpsilon(
		const FVector& CaptureExtentWS)
	{
		const float MaxDepth =
			GetMaxSceneDepth(CaptureExtentWS);

		return FMath::Clamp(
			MaxDepth * 0.0005f,
			0.1f,
			2.0f);
	}
}
