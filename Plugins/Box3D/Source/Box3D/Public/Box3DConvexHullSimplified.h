#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "box3d/box3d.h"
#include "box3d/math_functions.h"
#include "box3d/collision.h"

struct FBox3DConvexHullSimplified
{
	

	static b3HullData* CreateHullData(UStaticMesh* MeshToConvex, int maxVertexCount);




	void Destroy();
};
