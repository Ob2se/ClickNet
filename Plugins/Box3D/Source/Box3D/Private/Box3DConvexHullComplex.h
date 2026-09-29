#pragma once
#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "box3d/box3d.h"
#include "box3d/math_functions.h"
#include "box3d/collision.h"

struct FBox3DConvexHullComplex
{




	static b3MeshDef* CreateMeshDef(UStaticMesh* MeshToConvex);




	void Destroy();
};
