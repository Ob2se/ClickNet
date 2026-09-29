#pragma once

#include "box3d/box3d.h"
#include "box3d/math_functions.h"
#include "box3d/collision.h"

struct FBox3DCapsule 
{
	b3Vec3 centerOne = b3Vec3(1.0f, 1.0f, 1.0f);
	b3Vec3 centerTwo = b3Vec3(1.0f, 1.0f, 1.0f);

	float radius = 1.0f;

	static b3Capsule CreateCapsule(const b3Vec3& centerOne, const b3Vec3& centerTwo, float radius);
	void Destroy();
};
