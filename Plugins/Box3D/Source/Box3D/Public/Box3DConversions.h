#pragma once


#include "CoreMinimal.h"
#include "Box3D/math_functions.h"


//since box3d uses its own math types, u cant use them in uproperties without proper reflection
//so we use this class to convert
class FBox3DConversions
{

public:

	static b3Vec3 FVectorToB3Vec3(const FVector& InVector)
	{
		return b3Vec3(InVector.X * .01f, InVector.Y * .01f, InVector.Z * .01f);
	}
	static FVector B3Vec3ToFVector(const b3Vec3& InB3Vec3)
	{
		return FVector(InB3Vec3.x * 100.0f, InB3Vec3.y * 100.0f, InB3Vec3.z * 100.0f);
	}

	static b3Vec3 FVector3fToB3Vec3(const FVector3f& InVector)
	{
		return b3Vec3(InVector.X * .01f, InVector.Y * .01f, InVector.Z * .01f);
	}
	static FVector3f B3Vec3ToFVector3f(const b3Vec3& InB3Vec3)
	{
		return FVector3f(InB3Vec3.x * 100.0f, InB3Vec3.y * 100.0f, InB3Vec3.z * 100.0f);
	}
	static b3Quat FQuatToB3Quat(const FQuat& InQuat)
	{
		return b3Quat(b3Vec3(InQuat.X, InQuat.Y, InQuat.Z), InQuat.W);
	}
	static FQuat B3QuatToFQuat(const b3Quat& InB3Quat)
	{
		return FQuat(InB3Quat.v.x, InB3Quat.v.y, InB3Quat.v.z, InB3Quat.s);
	}

};