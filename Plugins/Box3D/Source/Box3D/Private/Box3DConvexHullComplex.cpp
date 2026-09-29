#include "Box3DConvexHullComplex.h"
#include "Box3DConversions.h"

b3MeshDef* FBox3DConvexHullComplex::CreateMeshDef(UStaticMesh* MeshToConvex)
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


	//if theres no vertex positions dont even continue
	if (!VertexPositions.IsValid()) return nullptr;

	//we have to create a map here because box3d expects its indices to be in order compared to the vertex array
	//but mesh description does not guarantee that order, so we use this to map ids to indices then add those indices to a new array
	TMap<FVertexID, int32_t> VertexIDToIndex;

	int NumVertices = MeshDescription->Vertices().Num();

	
	//create an array of b3Vec3 to hold the vertices, reserve size based on the number of vertices in the mesh description
	TArray<b3Vec3> Vertices;
	Vertices.Reserve(NumVertices);

	//loop through the vertex positions and convert them to b3Vec3 and add them to the array
	for (const FVertexID VertexID : MeshDescription->Vertices().GetElementIDs())
	{

		const FVector3f Position = VertexPositions.Get(VertexID);
		VertexIDToIndex.Add(VertexID, Vertices.Num()); // record its new slot

		Vertices.Emplace(FBox3DConversions::FVector3fToB3Vec3(Position));
	}

	//get the number of triangles in the mesh description and reserve space for the indices array
	int32 NumTriangles = MeshDescription->Triangles().Num();
	TArray<int32_t> Indices;
	Indices.Reserve(NumTriangles * 3);

	//loop through the triangles and get their vertex ids, then look up the corresponding index in the vertex array and add it to the indices array
	for (const FTriangleID TriangleID : MeshDescription->Triangles().GetElementIDs())
	{
		TArrayView<const FVertexID> TriVerts = MeshDescription->GetTriangleVertices(TriangleID);

		for (FVertexID VID : TriVerts)
		{
			Indices.Emplace(VertexIDToIndex[VID]); // look up the packed index, not the raw FVertexID
		}
	}

	//create the mesh definition and fill it with the vertex and index data
	b3MeshDef* meshDef = new b3MeshDef();
	meshDef->vertices = Vertices.GetData();
	meshDef->vertexCount = Vertices.Num();
	meshDef->indices = Indices.GetData();
	meshDef->triangleCount = Indices.Num() / 3;
	meshDef->weldVertices = true;
	meshDef->identifyEdges = true;


	return meshDef;
#endif
}
