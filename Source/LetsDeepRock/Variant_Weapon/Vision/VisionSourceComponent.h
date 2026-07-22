// 核心视野系统 - 视野来源组件

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VisionSourceComponent.generated.h"

UENUM(BlueprintType)
enum class EVisionSourceShape : uint8
{
	Cone UMETA(DisplayName = "锥形"),
	Circle UMETA(DisplayName = "圆形")
};

/** 可挂在任意 Actor 上的本地视野来源。 */
UCLASS(ClassGroup = (Vision), meta = (BlueprintSpawnableComponent))
class LETSDEEPROCK_API UVisionSourceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVisionSourceComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vision")
	EVisionSourceShape Shape = EVisionSourceShape::Cone;

	/** 视野距离（世界单位）；0 表示延伸到地图边界。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vision", meta = (ClampMin = "0.0"))
	float Range = 0.0f;

	/** 锥形半角；60 表示完整夹角 120 度。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vision", meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float HalfAngle = 60.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vision")
	bool bActive = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vision")
	int32 TeamId = 0;

	UFUNCTION(BlueprintCallable, Category = "Vision")
	void ConfigureCone(float InRange, float InHalfAngle);

	UFUNCTION(BlueprintCallable, Category = "Vision")
	void ConfigureCircle(float InRange);

	UFUNCTION(BlueprintPure, Category = "Vision")
	FVector GetVisionLocation() const;

	UFUNCTION(BlueprintPure, Category = "Vision")
	FVector GetVisionDirection() const;
};
