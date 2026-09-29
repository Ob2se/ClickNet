#include "Box3DSphere.h"


b3Sphere FBox3DSphere::CreateSphere(const b3Vec3& center, float radius)
{
	b3Sphere b3SphereShape;
	b3SphereShape.center = center;
	b3SphereShape.radius = radius;
	return b3SphereShape;
}