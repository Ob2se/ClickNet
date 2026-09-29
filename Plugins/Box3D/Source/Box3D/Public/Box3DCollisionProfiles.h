#pragma once


#include "CoreMinimal.h"

//collision profiles for Box3D collisions, bitwise, so 0 is none, 1 is world static, 2 is world dynamic, 4 is pawn, etc.
UENUM(BlueprintType, Meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EBox3DCollisionProfile : uint8
{
	None = 0ULL,
	WorldStatic = 1ULL << 0,
	WorldDynamic = 1ULL << 1,
	Pawn = 1ULL << 2,
};