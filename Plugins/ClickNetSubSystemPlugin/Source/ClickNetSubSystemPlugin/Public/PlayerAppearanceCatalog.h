#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameFramework/Pawn.h"
#include "PlayerAppearanceCatalog.generated.h"


UCLASS(BlueprintType)
class CLICKNETSUBSYSTEMPLUGIN_API UPlayerAppearanceCatalog : public UDataAsset
{
    GENERATED_BODY()

public:

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Appearance")
    TMap<int32, TSubclassOf<APawn>> AppearanceClasses;
};