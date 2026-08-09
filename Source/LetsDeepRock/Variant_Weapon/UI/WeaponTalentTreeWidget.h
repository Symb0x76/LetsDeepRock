// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "WeaponTalentTreeWidget.generated.h"

class UBorder;
class UCanvasPanel;
class UImage;
class UTextBlock;
class UWeaponTalentTreeWidget;

UENUM()
enum class EWeaponTalentNodeShape : uint8
{
	Circle,
	Square,
	Hub,
	Pentagon,
	SmallPentagon
};

/** UButton 的可靠无参点击转发器。 */
UCLASS()
class UWeaponTalentNodeButton : public UButton
{
	GENERATED_BODY()

public:
	void InitializeNode(int32 InNodeId, UWeaponTalentTreeWidget* InOwner);

private:
	UFUNCTION()
	void HandleClicked();

	int32 NodeId = INDEX_NONE;

	UPROPERTY()
	TObjectPtr<UWeaponTalentTreeWidget> OwnerWidget = nullptr;
};

USTRUCT()
struct FWeaponTalentNode
{
	GENERATED_BODY()

	int32 Id = INDEX_NONE;
	EWeaponTalentNodeShape Shape = EWeaponTalentNodeShape::Circle;
	FVector2D Position = FVector2D::ZeroVector;
	TArray<int32> Parents;
	int32 MutexGroup = 0;
	bool bActivated = false;

	UPROPERTY()
	TObjectPtr<UWeaponTalentNodeButton> Button = nullptr;
};

USTRUCT()
struct FWeaponTalentConnection
{
	GENERATED_BODY()

	int32 FromId = INDEX_NONE;
	int32 ToId = INDEX_NONE;
	bool bMutexHint = false;

	UPROPERTY()
	TObjectPtr<UImage> Image = nullptr;
};

/** 不依赖蓝图资产的纯 C++ 天赋树页面。 */
UCLASS()
class UWeaponTalentTreeWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Talent")
	void ShowTalentTree();

	UFUNCTION(BlueprintCallable, Category = "Talent")
	void CloseTalentTree();

	/** 由 UWeaponTalentNodeButton 转发。 */
	void HandleNodeClicked(int32 NodeId);

protected:
	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void BuildLayout();
	void InitializeTopology();
	int32 AddNode(EWeaponTalentNodeShape Shape, const FVector2D& Position, const TArray<int32>& Parents = {}, int32 MutexGroup = 0);
	void AddConnection(int32 FromId, int32 ToId, bool bMutexHint = false);
	void CreateConnectionWidget(FWeaponTalentConnection& Connection);
	void AddMutexHintLines();
	void CreateNodeWidget(FWeaponTalentNode& Node);
	void RefreshVisuals();
	bool AreParentsActivated(const FWeaponTalentNode& Node) const;
	bool IsMutexExcluded(const FWeaponTalentNode& Node) const;
	bool CanActivate(const FWeaponTalentNode& Node) const;
	FText GetPlaceholderTooltip(int32 NodeId) const;
	float GetNodeSize(EWeaponTalentNodeShape Shape) const;

	UFUNCTION()
	void HandleCloseClicked();

	UPROPERTY()
	TMap<int32, FWeaponTalentNode> Nodes;

	UPROPERTY()
	TArray<FWeaponTalentConnection> Connections;

	UPROPERTY()
	TObjectPtr<UCanvasPanel> TreeCanvas = nullptr;

	UPROPERTY()
	TObjectPtr<UTextBlock> TalentPointsText = nullptr;

	int32 AvailableTalentPoints = 999;
	int32 NextNodeId = 1;
};
