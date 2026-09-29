#pragma once

#include "CoreMinimal.h"
#include "Math/Vector2D.h"

struct FClicknetPlayerInput
{
    uint32 SequenceNumber = 0;   // for reconciliation/rollback
    FVector2D MoveAxis = FVector2D::Zero(); // WASD/left-stick
    FVector2D LookAxis = FVector2D::Zero(); // mouse/right-stick delta
    float YawRadians = 0.0f;
    bool bJumpPressed = false;
    bool bFirePressed = false;
};

