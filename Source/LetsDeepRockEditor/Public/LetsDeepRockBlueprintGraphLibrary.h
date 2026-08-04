#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LetsDeepRockBlueprintGraphLibrary.generated.h"

UCLASS()
class LETSDEEPROCKEDITOR_API ULetsDeepRockBlueprintGraphLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "LetsDeepRock|Editor|Blueprint Graph", meta = (CPP_Default_GraphName = "", CPP_Default_bIncludePins = "true"))
	static FString InspectBlueprintGraphJson(const FString& AssetPath, const FString& GraphName, bool bIncludePins);

	UFUNCTION(BlueprintCallable, Category = "LetsDeepRock|Editor|Blueprint Graph")
	static FString SearchBlueprintNodesJson(const TArray<FString>& Paths, const TArray<FString>& Keywords, const TArray<FString>& NodeClasses);
};
