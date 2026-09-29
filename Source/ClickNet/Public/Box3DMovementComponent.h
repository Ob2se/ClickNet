#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Box3D/Public/Box3DCapsuleComponent.h"
#include "Box3D/Public/Box3DConversions.h"
#include "Box3DMovementComponent.generated.h"

UCLASS(ClassGroup = (Box3D), meta = (BlueprintSpawnableComponent))
class CLICKNET_API UBox3DMovementComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBox3DMovementComponent();
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	void RequestJump();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D Movement", meta = (ClampMin = "0.0"))
	float MoveSpeed = 5.0f; // meters per second

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D Movement", meta = (ClampMin = "0.0"))
	float JumpSpeed = 5.0f; // meters per second

	void MoveForwardBackward(float valueY, float valueX);
	void MoveLeftRight(float value);
	void MoveForwardBackwardFromAxis(float value);
	void MoveLeftRightFromAxis(float value);


	void SimulateStep(float FixedDeltaTime);

	// Remote visuals receive server motion without running local simulation.
	void ApplyRemoteMovementState(const FVector& InVelocity, bool bInGrounded);

	UFUNCTION(BlueprintPure, Category = "Box3D Movement")
	FVector Box3DGetVelocity() const { return FBox3DConversions::B3Vec3ToFVector(Velocity); }

	UFUNCTION(BlueprintPure, Category = "Box3D Movement")
	bool Box3DIsFalling() const { return !bGrounded; }

protected:
	virtual void BeginPlay() override;

private:
    struct FPredictedMove
    {
        uint32 Tick = 0;
        b3Vec3 DesiredVelocity = b3Vec3(0.0f, 0.0f, 0.0f);
        bool bJump = false;
        bool bCommandSent = false;
    };
    TArray<FPredictedMove> PendingMoves;
    uint32 NextInputTick = 1;
    bool bWasConnected = false;
    FVector VisualCorrectionMeters = FVector::ZeroVector;
    b3Pos PreviousPosition = b3Pos{0.0f, 0.0f, 0.0f};
    float LargestCorrectionMeters = 0.0f;
    int32 LargestAckLeadTicks = 0;
    int32 CorrectionCount = 0;
    int32 DeferredSnapshotCount = 0;

	float fMoveForwardBack = 0.0f;
	float fMoveLeftRight = 0.0f;
	float ForwardInputAge = 1.0f;
	float RightInputAge = 1.0f;
	float fAxisForwardBack = 0.0f;
	float fAxisLeftRight = 0.0f;

	b3Vec3 Velocity = b3Vec3(0.0f, 0.0f, 0.0f);


	float TimeAccumulator = 0.0f;
	bool bJumpRequested = false;
	bool bGrounded = false;
	bool bLoggedFirstStep = false;
	float DebugElapsed = 0.0f;

	b3Pos Position = b3Pos{0.0f, 0.0f, 0.0f};
	b3Capsule Capsule{};


};
