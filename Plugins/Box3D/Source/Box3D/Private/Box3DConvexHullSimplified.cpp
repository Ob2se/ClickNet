#include "Box3DConvexHullSimplified.h"
#include "Box3DConversions.h"
#include "MeshAttributes.h"
#include "MeshAttributeArray.h"
#include "MeshDescription.h"

// Create a convex hull from a static mesh and return the hull data
b3HullData* FBox3DConvexHullSimplified::CreateHullData(UStaticMesh* MeshToConvex, int maxVertexCount)
{
#if !WITH_EDITOR
	return nullptr;
#else
	
	//if no mesh return null data
	if (!MeshToConvex) return nullptr;
	

	//check if source model is valid and get the mesh description
	FMeshDescription* MeshDescription = nullptr;
	if (MeshToConvex->IsSourceModelValid(0))
	{
		MeshDescription = MeshToConvex->GetMeshDescription(0);
	}

	//return if no mesh description is found
	if (!MeshDescription) return nullptr;

	//get vertex positions from mesh description
	TVertexAttributesRef<FVector3f> VertexPositions = MeshDescription->GetVertexPositions();

	//if there is valid vertex positions
	if (VertexPositions.IsValid())
	{
		//create an array of b3Vec3 to hold the vertices, reserve size based on the number of vertices in the mesh description
		TArray<b3Vec3> Verticies;
		Verticies.Reserve(MeshDescription->Vertices().Num());

		//loop through the vertex positions and convert them to b3Vec3 and add them to the array
		for (const FVertexID VertexID : MeshDescription->Vertices().GetElementIDs())
		{ 

			const FVector3f Position = VertexPositions.Get(VertexID);

			Verticies.Emplace(FBox3DConversions::FVector3fToB3Vec3(Position));
		}

		
		//create hull data with the verticies and supplied max vertex count
		b3HullData* HullData = b3CreateHull(Verticies.GetData(), Verticies.Num(), maxVertexCount);

		//return that hull data
		return HullData;

	}

	
	//no valid vertex positions found, return null
	return nullptr;
#endif
}
