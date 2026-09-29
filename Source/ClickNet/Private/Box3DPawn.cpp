#include "Box3DPawn.h"
#include "CoreMinimal.h"
#include "InputActionValue.h" 
#include "Components/InputComponent.h"
#include "Kismet/GameplayStatics.h"

void ABox3DPawn::ApplyRemoteMovementState(const FVector& Velocity, bool bGrounded)
{
    if (MovementComponent) MovementComponent->ApplyRemoteMovementState(Velocity, bGrounded);
}

ABox3DPawn::ABox3DPawn()
{
	CapsuleComponent = CreateDefaultSubobject<UBox3DCapsuleComponent>(TEXT("Box3DCapsule"));
	RootComponent = CapsuleComponent;
	CapsuleComponent->centerOne = FVector3f(0.0f, 0.0f, -55.0f);
	CapsuleComponent->centerTwo = FVector3f(0.0f, 0.0f, 55.0f);
	CapsuleComponent->radius = 35.0f;
	CapsuleComponent->bCollisionEnabled = false;
	CapsuleComponent->bEnablePhysics = false;

	MovementComponent = CreateDefaultSubobject<UBox3DMovementComponent>(TEXT("MovementComponent"));

}

//needs rename, handles all movement inputs now
void ABox3DPawn::MoveForwardBackward(FVector2D value)
{
	
	MovementComponent->MoveForwardBackward(value.Y, value.X);
}

//can be deleted
void ABox3DPawn::MoveLeftRight(FVector2D value)
{
	FVector2D MovementVector = value;
	MovementComponent->MoveLeftRight(value.X);
}



void ABox3DPawn::LookUpDown(FVector2D value)
{

	AddControllerYawInput(value.X);
	AddControllerPitchInput(-value.Y);



}

void ABox3DPawn::LookLeftRight(FVector2D value)
{
	FVector2D MovementVector = value;
}

void ABox3DPawn::Jump(FVector2D value)
{
	MovementComponent->RequestJump();
}

void ABox3DPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	/*PlayerInputComponent->BindAxis(TEXT("Box3D_MoveForward"), MovementComponent.Get(), &UBox3DMovementComponent::MoveForwardBackwardFromAxis);
	PlayerInputComponent->BindAxis(TEXT("Box3D_MoveRight"), MovementComponent.Get(), &UBox3DMovementComponent::MoveLeftRightFromAxis);*/
	PlayerInputComponent->BindAction(TEXT("Box3D_Jump"), IE_Pressed, MovementComponent.Get(), &UBox3DMovementComponent::RequestJump);
}
