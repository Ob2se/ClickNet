#pragma once

#include "box3d/box3d.h"
#include "box3d/math_functions.h"
#include "box3d/collision.h"

struct FBox3DSphere
{
	b3Vec3 center = b3Vec3(1.0f, 1.0f, 1.0f);

	float radius = 1.0f;

	static b3Sphere CreateSphere(const b3Vec3& center, float radius);
	void Destroy();
};
