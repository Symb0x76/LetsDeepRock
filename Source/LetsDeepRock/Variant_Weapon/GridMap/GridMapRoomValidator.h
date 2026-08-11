// 网格房间模板纯数据校验器

#pragma once

#include "CoreMinimal.h"
#include "GridMapRoomAsset.h"
#include "GridMapRoomValidator.generated.h"

UENUM(BlueprintType)
enum class EGridMapValidationSeverity : uint8
{
	Warning,
	Error
};

USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FGridMapValidationIssue
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	EGridMapValidationSeverity Severity = EGridMapValidationSeverity::Error;

	UPROPERTY(BlueprintReadOnly)
	FName Code;

	UPROPERTY(BlueprintReadOnly)
	FString Message;

	UPROPERTY(BlueprintReadOnly)
	FIntPoint GridCoord = FIntPoint(INDEX_NONE, INDEX_NONE);
};

/** 不依赖 World 或资产注册表的房间模板校验。 */
class LETSDEEPROCK_API FGridMapRoomValidator
{
public:
	static TArray<FGridMapValidationIssue> Validate(const UGridMapRoomAsset& Room);
	static bool IsValidForGeneration(const UGridMapRoomAsset& Room, TArray<FGridMapValidationIssue>* OutIssues = nullptr);
};
