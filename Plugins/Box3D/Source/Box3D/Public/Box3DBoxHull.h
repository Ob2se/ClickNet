#pragma once

#include "box3d/box3d.h"
#include "box3d/math_functions.h"
#include "box3d/collision.h"

struct FBox3DBoxHull
{
	float x = 1.0f;
	float y = 1.0f;
	float z = 1.0f;

	float size = 1.0f;

	static b3BoxHull CreateBox(float x, float y, float z);
	static b3BoxHull CreateCube(float size);

	void Destroy();
};
