#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ClickNetRemoteMovementReceiver.generated.h"

UINTERFACE(MinimalAPI)
class UClickNetRemoteMovementReceiver : public UInterface
{
    GENERATED_BODY()
};

// Velocity uses Unreal centimeters per second. Implemented by game pawns so
// the networking plugin does not need a dependency on the game module.
class CLICKNETSUBSYSTEMPLUGIN_API IClickNetRemoteMovementReceiver
{
    GENERATED_BODY()

public:
    virtual void ApplyRemoteMovementState(const FVector& Velocity, bool bGrounded) = 0;
};
