#include "Box3DBoxHull.h"

b3BoxHull FBox3DBoxHull::CreateBox(float x, float y, float z)
{
	return b3MakeBoxHull(x,y,z);
}

b3BoxHull FBox3DBoxHull::CreateCube(float size)
{
	return b3MakeCubeHull(size);
}
