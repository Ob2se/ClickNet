#include "Box3DMovementComponent.h"
#include "ClickNetSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

void UBox3DMovementComponent::ApplyRemoteMovementState(const FVector& InVelocity, bool bInGrounded)
{
    if (!GetOwner() || !GetOwner()->ActorHasTag(TEXT("ClickNetRemoteVisual"))) return;
    Velocity = FBox3DConversions::FVectorToB3Vec3(InVelocity);
    bGrounded = bInGrounded;
}

UBox3DMovementComponent::UBox3DMovementComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UBox3DMovementComponent::BeginPlay()
{
    Super::BeginPlay();
    if (GetOwner() && GetOwner()->ActorHasTag(TEXT("ClickNetRemoteVisual")))
    {
        SetComponentTickEnabled(false);
        return;
    }
    SetComponentTickEnabled(true);

    if (const AActor* Owner = GetOwner())
    {
        const FVector Location = Owner->GetActorLocation() * 0.01;
        Position = b3Pos{float(Location.X), float(Location.Y), float(Location.Z)};
        PreviousPosition = Position;
        if (UBox3DCapsuleComponent* CapsuleComponent = Owner->FindComponentByClass<UBox3DCapsuleComponent>())
        {
            Capsule = CapsuleComponent->GenerateCapsuleForMover();
        }
        UE_LOG(LogTemp, Display, TEXT("Box3D mover BeginPlay: owner=%s capsuleRadius=%.3f position=(%.2f, %.2f, %.2f)"),
            *Owner->GetName(), Capsule.radius, Position.x, Position.y, Position.z);
    }
}

void UBox3DMovementComponent::MoveForwardBackward(float valueY, float valueX)
{
    fMoveForwardBack = valueY;
    fMoveLeftRight = valueX;
}

void UBox3DMovementComponent::MoveLeftRight(float Value)
{
    fMoveLeftRight = Value;
    RightInputAge = 0.0f;
}

void UBox3DMovementComponent::MoveForwardBackwardFromAxis(float Value)
{
    fAxisForwardBack = Value;
}

void UBox3DMovementComponent::MoveLeftRightFromAxis(float Value)
{
    fAxisLeftRight = Value;
}

void UBox3DMovementComponent::RequestJump()
{
    bJumpRequested = true;
}

void UBox3DMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (GetOwner() && GetOwner()->ActorHasTag(TEXT("ClickNetRemoteVisual"))) return;


    TimeAccumulator = FMath::Min(TimeAccumulator + FMath::Clamp(DeltaTime, 0.0f, 0.25f), 0.25f);
    constexpr float Step = 1.0f / 60.0f;
    for (int32 Steps = 0; Steps < 4 && TimeAccumulator >= Step; ++Steps)
    {
        SimulateStep(Step);
        TimeAccumulator -= Step;
    }
    if (AActor* Owner = GetOwner(); Owner && Capsule.radius > 0.0f)
    {
        const float Alpha = FMath::Clamp(TimeAccumulator / Step, 0.0f, 1.0f);
        const FVector Previous(PreviousPosition.x, PreviousPosition.y, PreviousPosition.z);
        const FVector Current(Position.x, Position.y, Position.z);
        Owner->SetActorLocation((FMath::Lerp(Previous, Current, Alpha) + VisualCorrectionMeters) * 100.0);
    }
}

void UBox3DMovementComponent::SimulateStep(float FixedDeltaTime)
{
    UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
    UClickNetSubsystem* CNSubsystem = GameInstance ? GameInstance->GetSubsystem<UClickNetSubsystem>() : nullptr;
    AActor* Owner = GetOwner();
    if (!CNSubsystem || !Owner || Capsule.radius <= 0.0f)
    {
        return;
    }

    const bool bConnected = CNSubsystem->IsConnectedToServer();
    if (bConnected != bWasConnected)
    {
        PendingMoves.Reset();
        NextInputTick = 1;
        VisualCorrectionMeters = FVector::ZeroVector;
        bWasConnected = bConnected;
    }
    if (bConnected)
    {
        clicknet::wire::PlayerState Snapshot;
        const bool bHasSnapshot = CNSubsystem->ConsumePlayerState(Snapshot);
        const bool bMissingCommand = bHasSnapshot
            && CNSubsystem->HasPendingInputThrough(Snapshot.acknowledgedInputTick);
        if (bMissingCommand) ++DeferredSnapshotCount;
        // A held-input step is not confirmation of a turn still in flight.
        // Keep prediction/history until the server receives that command and
        // reconstructs its steps, instead of briefly pulling back to old input.
        if (bHasSnapshot && !bMissingCommand)
        {
            // Compare states at the same simulation point. The actor is rendered
            // between fixed steps and can be almost one step behind Position.
            // Using its location here reintroduces that delay on every snapshot.
            const FVector PredictedBeforeMeters(Position.x, Position.y, Position.z);
            const uint32 LastSimulatedTick = NextInputTick - 1;
            if (Snapshot.acknowledgedInputTick > LastSimulatedTick)
            {
                LargestAckLeadTicks = FMath::Max(LargestAckLeadTicks,
                    static_cast<int32>(Snapshot.acknowledgedInputTick - LastSimulatedTick));
                // The server can keep ticking while this client's fixed-step
                // accumulator is paused or capped. Old sequence numbers would
                // be acknowledged immediately, making turns feel delayed.
                PendingMoves.Reset();
                NextInputTick = Snapshot.acknowledgedInputTick + 1;
            }
            clicknet::MoverState ServerState;
            ServerState.position = b3Pos{Snapshot.px, Snapshot.py, Snapshot.pz};
            ServerState.velocity = b3Vec3(Snapshot.vx, Snapshot.vy, Snapshot.vz);
            ServerState.grounded = Snapshot.grounded;
            CNSubsystem->ResetLocalMoverState(ServerState);
            Position = ServerState.position;
            Velocity = ServerState.velocity;
            bGrounded = ServerState.grounded;

            PendingMoves.RemoveAll([&](const FPredictedMove& Move)
            {
                return Move.Tick <= Snapshot.acknowledgedInputTick;
            });
            for (const FPredictedMove& Move : PendingMoves)
            {
                Velocity.x = Move.DesiredVelocity.x;
                Velocity.y = Move.DesiredVelocity.y;
                CNSubsystem->SimulateLocalMover(Position, Capsule, Velocity, FixedDeltaTime,
                    bGrounded, Move.bJump, JumpSpeed);
            }
            CNSubsystem->PlaceLocalMoverProxyAtPredictedPosition();
            const FVector CorrectedMeters(Position.x, Position.y, Position.z);
            const FVector SimulationCorrection = PredictedBeforeMeters - CorrectedMeters;
            const float CorrectionMeters = SimulationCorrection.Size();
            LargestCorrectionMeters = FMath::Max(LargestCorrectionMeters, CorrectionMeters);
            ++CorrectionCount;
            // Keep the actor and its camera visually continuous while the
            // authoritative mover/proxy take the corrected physics position.
            VisualCorrectionMeters = CorrectionMeters < 3.0f
                ? (VisualCorrectionMeters + SimulationCorrection).GetClampedToMaxSize(1.0f)
                : FVector::ZeroVector;
        }
    }

    const FVector Forward = Owner->GetActorForwardVector().GetSafeNormal2D();
    const FVector Right = Owner->GetActorRightVector().GetSafeNormal2D();
    const FVector Input = (Forward * (fMoveForwardBack + fAxisForwardBack)
        + Right * (fMoveLeftRight + fAxisLeftRight)).GetClampedToMaxSize(1.0);
    Velocity.x = float(Input.X) * MoveSpeed;
    Velocity.y = float(Input.Y) * MoveSpeed;

    const bool bJump = bJumpRequested;
    bJumpRequested = false;

    PreviousPosition = Position;
    if (!CNSubsystem->SimulateLocalMover(Position, Capsule, Velocity, FixedDeltaTime,
        bGrounded, bJump, JumpSpeed))
    {
        return;
    }

    if (bConnected)
    {
        if (PendingMoves.Num() >= 128) PendingMoves.RemoveAt(0, PendingMoves.Num() - 127);
        FPredictedMove& Move = PendingMoves.AddDefaulted_GetRef();
        Move.Tick = NextInputTick++;
        Move.DesiredVelocity = b3Vec3(float(Input.X) * MoveSpeed, float(Input.Y) * MoveSpeed, 0.0f);
        Move.bJump = bJump;

        FClicknetPlayerInput NetworkInput;
        NetworkInput.SequenceNumber = Move.Tick;
        NetworkInput.MoveAxis = FVector2D(Input.X, Input.Y);
        NetworkInput.YawRadians = FMath::DegreesToRadians(Owner->GetActorRotation().Yaw);
        NetworkInput.bJumpPressed = bJump;
        CNSubsystem->SendInput(NetworkInput);
        Move.bCommandSent = CNSubsystem->GetLastSentInputTick() == Move.Tick;
    }

    if (!bLoggedFirstStep)
    {
        bLoggedFirstStep = true;
        UE_LOG(LogTemp, Display, TEXT("Box3D mover first step: owner=%s worldBodies=%d"),
            *Owner->GetName(), CNSubsystem->GetImportedBodyCount());
    }

    VisualCorrectionMeters *= FMath::Exp(-FixedDeltaTime / 0.10f);
    DebugElapsed += FixedDeltaTime;
    if (DebugElapsed >= 1.0f)
    {
        DebugElapsed = 0.0f;
        UE_LOG(LogTemp, Display, TEXT("Box3D mover: position=(%.2f, %.2f, %.2f) velocity=(%.2f, %.2f, %.2f) grounded=%d input=(%.2f, %.2f) snapshots=%d maxCorrection=%.3fm ackLead=%d deferred=%d"),
            Position.x, Position.y, Position.z, Velocity.x, Velocity.y, Velocity.z,
            bGrounded, fMoveForwardBack + fAxisForwardBack, fMoveLeftRight + fAxisLeftRight,
            CorrectionCount, LargestCorrectionMeters, LargestAckLeadTicks, DeferredSnapshotCount);
        CorrectionCount = 0;
        DeferredSnapshotCount = 0;
        LargestCorrectionMeters = 0.0f;
        LargestAckLeadTicks = 0;
    }
}
