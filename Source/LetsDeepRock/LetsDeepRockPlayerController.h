// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
//#include "Templates/SubclassOf.h"
#include "GameFramework/PlayerController.h"
#include "LetsDeepRockPlayerController.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, Log, All);

/**
 *  Player controller for a top-down perspective game.
 *  Movement is driven by WASD input handled on the possessed character.
 */
UCLASS(abstract)
class ALetsDeepRockPlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	/** Constructor */
	ALetsDeepRockPlayerController();
};


