// Fill out your copyright notice in the Description page of Project Settings.

#include "WaterInteractionCommon.h"

#include "GroundInteractionProxyMeshComponent.h"

#include "Engine/StaticMesh.h"

namespace
{
	bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool IsFiniteVector2D(const FVector2d& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y);
	}

	void CanonicalizeDirection(FVector2d& Direction)
	{
		if (Direction.X < 0.0 || (Direction.X == 0.0 && Direction.Y < 0.0))
		{
			Direction *= -1.0;
		}
	}

	FVector TransformEllipsoidVector(const TStaticArray<FVector, 3>& HalfAxes, const FVector& UnitSphereVector)
	{
		return HalfAxes[0] * UnitSphereVector.X + HalfAxes[1] * UnitSphereVector.Y + HalfAxes[2] * UnitSphereVector.Z;
	}
}

bool WaterInteractionCommon::TryMakeWaterSplatGT(
	const UGroundInteractionProxyMeshComponent* Proxy,
	const FVector& WaterSurfacePositionWS,
	const FVector& WaterSurfaceNormalWS,
	FWaterSplatGT& OutSplat)
{
	check(IsInGameThread());

	if (!IsValid(Proxy) || !Proxy->IsRegistered() || !IsFiniteVector(WaterSurfacePositionWS) || !IsFiniteVector(WaterSurfaceNormalWS))
	{
		return false;
	}

	FVector SurfaceNormalWS = WaterSurfaceNormalWS;
	if (!SurfaceNormalWS.Normalize())
	{
		return false;
	}

	const UStaticMesh* StaticMesh = Proxy->GetStaticMesh();
	if (!IsValid(StaticMesh))
	{
		return false;
	}

	const FTransform& ComponentToWorld = Proxy->GetComponentTransform();
	if (ComponentToWorld.ContainsNaN())
	{
		return false;
	}

	const FBoxSphereBounds LocalBounds = StaticMesh->GetBounds();
	const FVector LocalCenter = LocalBounds.Origin;
	const FVector LocalHalfExtent = LocalBounds.BoxExtent;

	if (!IsFiniteVector(LocalCenter) || !IsFiniteVector(LocalHalfExtent) ||
		LocalHalfExtent.X <= UE_DOUBLE_SMALL_NUMBER || LocalHalfExtent.Y <= UE_DOUBLE_SMALL_NUMBER || LocalHalfExtent.Z <= UE_DOUBLE_SMALL_NUMBER)
	{
		return false;
	}

	const FVector ShapeCenterWS = ComponentToWorld.TransformPosition(LocalCenter);
	TStaticArray<FVector, 3> HalfAxesWS;
	HalfAxesWS[0] = ComponentToWorld.TransformVector(FVector(LocalHalfExtent.X, 0.0, 0.0));
	HalfAxesWS[1] = ComponentToWorld.TransformVector(FVector(0.0, LocalHalfExtent.Y, 0.0));
	HalfAxesWS[2] = ComponentToWorld.TransformVector(FVector(0.0, 0.0, LocalHalfExtent.Z));

	const double ShapeScale = FMath::Max3(HalfAxesWS[0].Size(), HalfAxesWS[1].Size(), HalfAxesWS[2].Size());
	const double SizeTolerance = FMath::Max(ShapeScale * 1.0e-8, 1.0e-6);
	const double SizeToleranceSquared = FMath::Square(SizeTolerance);

	if (!IsFiniteVector(ShapeCenterWS) || !IsFiniteVector(HalfAxesWS[0]) || !IsFiniteVector(HalfAxesWS[1]) ||
		!IsFiniteVector(HalfAxesWS[2]) || !FMath::IsFinite(ShapeScale) ||
		HalfAxesWS[0].SizeSquared() <= SizeToleranceSquared || HalfAxesWS[1].SizeSquared() <= SizeToleranceSquared ||
		HalfAxesWS[2].SizeSquared() <= SizeToleranceSquared)
	{
		return false;
	}

	const FBoxSphereBounds& WorldBounds = Proxy->Bounds;
	if (!IsFiniteVector(WorldBounds.Origin) || !IsFiniteVector(WorldBounds.BoxExtent))
	{
		return false;
	}

	const double BoundsPlaneDistance = FVector::DotProduct(SurfaceNormalWS, WorldBounds.Origin - WaterSurfacePositionWS);
	const double BoundsPlaneRadius =
		FMath::Abs(SurfaceNormalWS.X) * WorldBounds.BoxExtent.X +
		FMath::Abs(SurfaceNormalWS.Y) * WorldBounds.BoxExtent.Y +
		FMath::Abs(SurfaceNormalWS.Z) * WorldBounds.BoxExtent.Z;

	if (FMath::Abs(BoundsPlaneDistance) > BoundsPlaneRadius + SizeTolerance)
	{
		return false;
	}

	FWaterSplatGT Splat;
	const EWaterSplatShape ShapeType = Proxy->GetWaterSplatShape();

	switch (ShapeType)
	{
	case EWaterSplatShape::Sphere:
		{
			const FVector UnitSpherePlaneNormal(
				FVector::DotProduct(HalfAxesWS[0], SurfaceNormalWS),
				FVector::DotProduct(HalfAxesWS[1], SurfaceNormalWS),
				FVector::DotProduct(HalfAxesWS[2], SurfaceNormalWS));
			const double UnitSpherePlaneNormalSquared = UnitSpherePlaneNormal.SizeSquared();
			const double PlaneOffset = FVector::DotProduct(SurfaceNormalWS, ShapeCenterWS - WaterSurfacePositionWS);

			if (!IsFiniteVector(UnitSpherePlaneNormal) || !FMath::IsFinite(UnitSpherePlaneNormalSquared) ||
				UnitSpherePlaneNormalSquared <= UE_DOUBLE_SMALL_NUMBER)
			{
				return false;
			}

			const double NormalizedDistanceSquared = FMath::Square(PlaneOffset) / UnitSpherePlaneNormalSquared;

			if (!FMath::IsFinite(NormalizedDistanceSquared) || NormalizedDistanceSquared >= 1.0)
			{
				return false;
			}

			const FVector UnitSphereCenter = UnitSpherePlaneNormal * (-PlaneOffset / UnitSpherePlaneNormalSquared);
			const FVector SectionNormal = UnitSpherePlaneNormal.GetSafeNormal();
			const FVector ReferenceAxis = FMath::Abs(SectionNormal.Z) < 0.999 ? FVector::UpVector : FVector::YAxisVector;
			const FVector SectionAxis0 = FVector::CrossProduct(SectionNormal, ReferenceAxis).GetSafeNormal();
			const FVector SectionAxis1 = FVector::CrossProduct(SectionNormal, SectionAxis0).GetSafeNormal();
			const double SectionScale = FMath::Sqrt(FMath::Max(1.0 - NormalizedDistanceSquared, 0.0));

			if (SectionAxis0.IsNearlyZero() || SectionAxis1.IsNearlyZero() || SectionScale <= UE_DOUBLE_SMALL_NUMBER)
			{
				return false;
			}

			const FVector SectionCenterWS = ShapeCenterWS + TransformEllipsoidVector(HalfAxesWS, UnitSphereCenter);
			const FVector EllipseVector0WS = TransformEllipsoidVector(HalfAxesWS, SectionAxis0) * SectionScale;
			const FVector EllipseVector1WS = TransformEllipsoidVector(HalfAxesWS, SectionAxis1) * SectionScale;

			const double MatrixXX = FMath::Square(EllipseVector0WS.X) + FMath::Square(EllipseVector1WS.X);
			const double MatrixXY = EllipseVector0WS.X * EllipseVector0WS.Y + EllipseVector1WS.X * EllipseVector1WS.Y;
			const double MatrixYY = FMath::Square(EllipseVector0WS.Y) + FMath::Square(EllipseVector1WS.Y);
			const double Trace = MatrixXX + MatrixYY;
			const double Discriminant = FMath::Sqrt(FMath::Max(FMath::Square(MatrixXX - MatrixYY) + 4.0 * FMath::Square(MatrixXY), 0.0));
			const double MaxEigenvalue = FMath::Max((Trace + Discriminant) * 0.5, 0.0);
			const double Determinant = FMath::Max(MatrixXX * MatrixYY - FMath::Square(MatrixXY), 0.0);
			const double MinEigenvalue = MaxEigenvalue > UE_DOUBLE_SMALL_NUMBER ? Determinant / MaxEigenvalue : 0.0;

			if (!FMath::IsFinite(MaxEigenvalue) || !FMath::IsFinite(MinEigenvalue) ||
				MaxEigenvalue <= SizeToleranceSquared || MinEigenvalue <= SizeToleranceSquared)
			{
				return false;
			}

			FVector2d Direction(1.0, 0.0);
			const double CircularTolerance = FMath::Max(MaxEigenvalue * 1.0e-8, SizeToleranceSquared);

			if (MaxEigenvalue - MinEigenvalue > CircularTolerance)
			{
				if (FMath::Abs(MatrixXY) > SizeToleranceSquared)
				{
					Direction = FVector2d(MaxEigenvalue - MatrixYY, MatrixXY);
				}
				else if (MatrixYY > MatrixXX)
				{
					Direction = FVector2d(0.0, 1.0);
				}

				if (!Direction.Normalize())
				{
					return false;
				}

				CanonicalizeDirection(Direction);
			}

			Splat.AbsoluteCenterXY = FVector2d(SectionCenterWS.X, SectionCenterWS.Y);
			Splat.HalfSizeXY = FVector2d(FMath::Sqrt(MaxEigenvalue), FMath::Sqrt(MinEigenvalue));
			Splat.RotationCos = static_cast<float>(Direction.X);
			Splat.RotationSin = static_cast<float>(Direction.Y);
			break;
		}

	case EWaterSplatShape::Box:
		{
			TStaticArray<FVector, 8> CornerOffsets;
			TStaticArray<double, 8> PlaneDistances;
			const FVector ShapeCenterToSurface = ShapeCenterWS - WaterSurfacePositionWS;

			for (int32 CornerIndex = 0; CornerIndex < CornerOffsets.Num(); ++CornerIndex)
			{
				const double SignX = (CornerIndex & 1) != 0 ? 1.0 : -1.0;
				const double SignY = (CornerIndex & 2) != 0 ? 1.0 : -1.0;
				const double SignZ = (CornerIndex & 4) != 0 ? 1.0 : -1.0;

				CornerOffsets[CornerIndex] = HalfAxesWS[0] * SignX + HalfAxesWS[1] * SignY + HalfAxesWS[2] * SignZ;
				PlaneDistances[CornerIndex] = FVector::DotProduct(SurfaceNormalWS, ShapeCenterToSurface + CornerOffsets[CornerIndex]);
			}

			const double PlaneTolerance = SizeTolerance;
			TArray<FVector> IntersectionOffsets;
			IntersectionOffsets.Reserve(12);

			for (int32 CornerIndex = 0; CornerIndex < CornerOffsets.Num(); ++CornerIndex)
			{
				if (FMath::Abs(PlaneDistances[CornerIndex]) <= PlaneTolerance)
				{
					IntersectionOffsets.Add(CornerOffsets[CornerIndex]);
				}
			}

			static constexpr int32 EdgeBits[] = { 1, 2, 4 };

			for (int32 CornerIndex = 0; CornerIndex < CornerOffsets.Num(); ++CornerIndex)
			{
				for (const int32 EdgeBit : EdgeBits)
				{
					const int32 OtherCornerIndex = CornerIndex ^ EdgeBit;
					if (CornerIndex >= OtherCornerIndex)
					{
						continue;
					}

					const double Distance0 = PlaneDistances[CornerIndex];
					const double Distance1 = PlaneDistances[OtherCornerIndex];
					const bool bCrossesPlane = (Distance0 < -PlaneTolerance && Distance1 > PlaneTolerance) ||
						(Distance0 > PlaneTolerance && Distance1 < -PlaneTolerance);

					if (!bCrossesPlane)
					{
						continue;
					}

					const double Alpha = Distance0 / (Distance0 - Distance1);
					IntersectionOffsets.Add(FMath::Lerp(CornerOffsets[CornerIndex], CornerOffsets[OtherCornerIndex], Alpha));
				}
			}

			if (IntersectionOffsets.Num() < 3)
			{
				return false;
			}

			FVector2d Direction = FVector2d::ZeroVector;
			double BestDirectionLengthSquared = 0.0;

			for (const FVector& HalfAxisWS : HalfAxesWS)
			{
				const FVector2d ProjectedAxis(HalfAxisWS.X, HalfAxisWS.Y);
				const double ProjectedLengthSquared = ProjectedAxis.SizeSquared();

				if (ProjectedLengthSquared > BestDirectionLengthSquared)
				{
					Direction = ProjectedAxis;
					BestDirectionLengthSquared = ProjectedLengthSquared;
				}
			}

			if (BestDirectionLengthSquared <= SizeToleranceSquared || !Direction.Normalize())
			{
				return false;
			}

			FVector2d PerpendicularDirection(-Direction.Y, Direction.X);
			const FVector2d ProjectionOrigin(IntersectionOffsets[0].X, IntersectionOffsets[0].Y);
			double MinU = TNumericLimits<double>::Max();
			double MaxU = TNumericLimits<double>::Lowest();
			double MinV = TNumericLimits<double>::Max();
			double MaxV = TNumericLimits<double>::Lowest();

			for (const FVector& IntersectionOffset : IntersectionOffsets)
			{
				const FVector2d RelativePoint = FVector2d(IntersectionOffset.X, IntersectionOffset.Y) - ProjectionOrigin;
				const double ProjectionU = FVector2d::DotProduct(RelativePoint, Direction);
				const double ProjectionV = FVector2d::DotProduct(RelativePoint, PerpendicularDirection);

				MinU = FMath::Min(MinU, ProjectionU);
				MaxU = FMath::Max(MaxU, ProjectionU);
				MinV = FMath::Min(MinV, ProjectionV);
				MaxV = FMath::Max(MaxV, ProjectionV);
			}

			const double SizeU = MaxU - MinU;
			const double SizeV = MaxV - MinV;
			if (!FMath::IsFinite(SizeU) || !FMath::IsFinite(SizeV) || SizeU <= SizeTolerance || SizeV <= SizeTolerance)
			{
				return false;
			}

			Splat.AbsoluteCenterXY = FVector2d(ShapeCenterWS.X, ShapeCenterWS.Y) + ProjectionOrigin +
				Direction * ((MinU + MaxU) * 0.5) +
				PerpendicularDirection * ((MinV + MaxV) * 0.5);
			Splat.HalfSizeXY = FVector2d(SizeU * 0.5, SizeV * 0.5);

			if (Splat.HalfSizeXY.Y > Splat.HalfSizeXY.X)
			{
				Swap(Splat.HalfSizeXY.X, Splat.HalfSizeXY.Y);
				Direction = PerpendicularDirection;
			}

			CanonicalizeDirection(Direction);
			Splat.RotationCos = static_cast<float>(Direction.X);
			Splat.RotationSin = static_cast<float>(Direction.Y);
			break;
		}

	default:
		return false;
	}

	if (!IsFiniteVector2D(Splat.AbsoluteCenterXY) || !IsFiniteVector2D(Splat.HalfSizeXY) ||
		!FMath::IsFinite(Splat.RotationCos) || !FMath::IsFinite(Splat.RotationSin))
	{
		return false;
	}

	Splat.ShapeType = static_cast<uint32>(ShapeType);
	Splat.Padding = 0;
	OutSplat = Splat;
	return true;
}
