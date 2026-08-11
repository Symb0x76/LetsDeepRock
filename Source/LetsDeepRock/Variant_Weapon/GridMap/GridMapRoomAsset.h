// 网格房间模板资产

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "GridMapTypes.h"
#include "GridMapRoomAsset.generated.h"

UENUM(BlueprintType)
enum class EGridMapRoomType : uint8
{
	Connector UMETA(DisplayName = "连接房"),
	Combat UMETA(DisplayName = "战斗房"),
	Objective UMETA(DisplayName = "目标房")
};

UENUM(BlueprintType)
enum class EGridMapRoomStatus : uint8
{
	Draft UMETA(DisplayName = "草稿"),
	Ready UMETA(DisplayName = "就绪")
};

UENUM(BlueprintType)
enum class EGridMapExitDirection : uint8
{
	North UMETA(DisplayName = "北"),
	East UMETA(DisplayName = "东"),
	South UMETA(DisplayName = "南"),
	West UMETA(DisplayName = "西")
};

UENUM(BlueprintType)
enum class EGridMapMarkerType : uint8
{
	PlayerSpawn UMETA(DisplayName = "玩家出生点"),
	Objective UMETA(DisplayName = "目标点"),
	EnemySpawn UMETA(DisplayName = "敌人出生点"),
	Decoration UMETA(DisplayName = "装饰点")
};

USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FGridMapRoomExit
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FName Id;

	/** 出口在边缘上的首格，宽度沿边缘正方向延伸。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FIntPoint Start = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1"))
	int32 Width = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EGridMapExitDirection Direction = EGridMapExitDirection::North;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FGameplayTag ConnectionTag;
};

USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FGridMapRoomMarker
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EGridMapMarkerType Type = EGridMapMarkerType::Decoration;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FIntPoint GridCoord = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float Yaw = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FName Group;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float Weight = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float Radius = 0.0f;
};

UCLASS(BlueprintType)
class LETSDEEPROCK_API UGridMapRoomAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room")
	EGridMapRoomType RoomType = EGridMapRoomType::Connector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room")
	EGridMapRoomStatus Status = EGridMapRoomStatus::Draft;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room")
	FIntPoint Size = FIntPoint(9, 9);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room", meta = (ClampMin = "1.0"))
	float TileSize = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room", meta = (ClampMin = "1"))
	int32 TemplateVersion = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room", meta = (ClampMin = "0.0"))
	float Weight = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room")
	FGameplayTagContainer GameplayTags;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room", meta = (MultiLine = "true"))
	FString Notes;

	/** 行优先稠密格子数据，索引为 Y * Size.X + X。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout")
	TArray<EGridTileType> Tiles;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout")
	TArray<FGridMapRoomExit> Exits;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout")
	TArray<FGridMapRoomMarker> Markers;

	/** 改变尺寸时清空并重建格子，同时重置出口和标记。 */
	UFUNCTION(BlueprintCallable, Category = "Grid Map Room")
	void InitializeRoom(int32 Width, int32 Height, EGridTileType InitialTile = EGridTileType::Empty);

	UFUNCTION(BlueprintPure, Category = "Grid Map Room")
	bool IsInBounds(FIntPoint Coord) const;

	UFUNCTION(BlueprintPure, Category = "Grid Map Room")
	int32 CoordToIndex(FIntPoint Coord) const;

	UFUNCTION(BlueprintPure, Category = "Grid Map Room")
	FIntPoint IndexToCoord(int32 Index) const;

	UFUNCTION(BlueprintPure, Category = "Grid Map Room")
	EGridTileType GetTile(FIntPoint Coord) const;

	UFUNCTION(BlueprintCallable, Category = "Grid Map Room")
	bool SetTile(FIntPoint Coord, EGridTileType TileType);
};
