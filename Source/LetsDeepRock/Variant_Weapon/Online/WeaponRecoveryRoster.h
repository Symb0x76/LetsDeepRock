#pragma once

#include "CoreMinimal.h"
#include "GameFramework/OnlineReplStructs.h"
#include "WeaponRecoveryRoster.generated.h"

/** Connection readiness for one stable player inside the recoverable match roster. */
UENUM(BlueprintType)
enum class EWeaponRecoveryPlayerStatus : uint8
{
	Pending,
	Ready,
	Departed,
};

/** Server-owned roster record; host role and gameplay alive/dead state are separate dimensions. */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponRecoveryRosterEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Recovery")
	FUniqueNetIdRepl PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "Recovery")
	int32 OriginalSlot = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "Recovery")
	EWeaponRecoveryPlayerStatus Status = EWeaponRecoveryPlayerStatus::Pending;

	/** Server World time when Pending expires; zero for Ready/Departed. */
	double PendingDeadlineServerTime = 0.0;
};
