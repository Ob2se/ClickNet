#include "Box3DCapsule.h"
#include "box3d.h"
#include "collision.h"

b3Capsule FBox3DCapsule::CreateCapsule(const b3Vec3& centerOne, const b3Vec3& centerTwo, float radius)
{
	b3Capsule b3CapsuleShape;
	b3CapsuleShape.center1 = centerOne;
	b3CapsuleShape.center2 = centerTwo;
	b3CapsuleShape.radius = radius;

	return b3CapsuleShape;
}
