// Copyright Epic Games, Inc. All Rights Reserved.

#include "LetsDeepRockPlayerController.h"
#include "LetsDeepRock.h"

ALetsDeepRockPlayerController::ALetsDeepRockPlayerController()
{
	// Show the mouse cursor for a top-down perspective game.
	// Movement itself is handled by WASD input on the possessed character.
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Default;
}
