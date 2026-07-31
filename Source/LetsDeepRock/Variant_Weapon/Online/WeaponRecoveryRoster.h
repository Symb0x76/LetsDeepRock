#pragma once

#include "CoreMinimal.h"
#include "GameFramework/OnlineReplStructs.h"
#include "WeaponRecoveryRoster.generated.h"

/** Connection readiness for one stable player inside the recoverable match roster. */
UENUM()
enum class EWeaponRecoveryPlayerStatus : uint8
{
	Pending,
	Ready,
	Departed,
};

/** Server-owned roster record; host role and gameplay alive/dead state are separate dimensions. */
USTRUCT()
struct LETSDEEPROCK_API FWeaponRecoveryRosterEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FUniqueNetIdRepl PlayerId;

	UPROPERTY()
	int32 OriginalSlot = INDEX_NONE;

	UPROPERTY()
	EWeaponRecoveryPlayerStatus Status = EWeaponRecoveryPlayerStatus::Pending;

	/** Server World time when Pending expires; zero for Ready/Departed. */
	double PendingDeadlineServerTime = 0.0;
};

/** Portable recovery form of one roster entry; Pending time is relative across Worlds. */
USTRUCT()
struct LETSDEEPROCK_API FWeaponRecoveryRosterSnapshot
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FUniqueNetIdRepl PlayerId;

	UPROPERTY(SaveGame)
	int32 OriginalSlot = INDEX_NONE;

	UPROPERTY(SaveGame)
	EWeaponRecoveryPlayerStatus Status = EWeaponRecoveryPlayerStatus::Pending;

	/** Remaining grace at Capture; meaningful only while Status is Pending. */
	UPROPERTY(SaveGame)
	float RemainingGraceSeconds = 0.0f;
};
