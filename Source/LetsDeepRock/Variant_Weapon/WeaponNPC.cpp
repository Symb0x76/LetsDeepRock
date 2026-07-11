// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponNPC.h"
#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Attributes/WeaponHealthSet.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameplayTagContainer.h"
#include "TimerManager.h"
#include "WeaponGameplayTags.h"
#include "GameDataSubsystem.h"
#include "AI/EnemyAIController.h"
#include "BrainComponent.h"

AWeaponNPC::AWeaponNPC()
{
	// NPC 自持 ASC：Owner 与 Avatar 都是自己
	AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);

	// NPC 用 Minimal 复制模式即可（不需要把每个 GE 细节复制给所有客户端）
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Minimal);

	HealthSet = CreateDefaultSubobject<UWeaponHealthSet>(TEXT("HealthSet"));

	// AI Controller 配置：使用 EnemyAIController，自动 Possess
	AIControllerClass = AEnemyAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
}

UAbilitySystemComponent* AWeaponNPC::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void AWeaponNPC::BeginPlay()
{
	Super::BeginPlay();

	if (AbilitySystemComponent)
	{
		// NPC 的 ActorInfo 初始化：Owner=Avatar=this
		AbilitySystemComponent->InitAbilityActorInfo(this, this);
	}

	// 仅服务端初始化属性与订阅死亡事件
	if (HasAuthority() && HealthSet)
	{
		// 尝试从 DataTable 读取配置覆盖硬编码默认值
		if (EnemyConfigRowName != NAME_None)
		{
			if (UGameInstance* GI = GetGameInstance())
			{
				if (UGameDataSubsystem* DataSys = GI->GetSubsystem<UGameDataSubsystem>())
				{
					if (const FEnemyConfigRow* Config = DataSys->GetEnemyConfig(EnemyConfigRowName))
					{
						DefaultHealth = Config->MaxHealth;
						DeathLingerTime = Config->DeathLingerTime;
					}
				}
			}
		}

		HealthSet->InitMaxHealth(DefaultHealth);
		HealthSet->InitHealth(DefaultHealth);

		HealthSet->OnHealthDepleted.AddUObject(this, &AWeaponNPC::HandleHealthDepleted);
	}

	// 授予默认能力（服务端）
	if (HasAuthority() && AbilitySystemComponent)
	{
		for (const TSubclassOf<UGameplayAbility>& Ability : DefaultAbilities)
		{
			if (Ability)
			{
				AbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(Ability, 1, INDEX_NONE, this));
			}
		}
	}
}

void AWeaponNPC::HandleHealthDepleted(AActor* OwningActor)
{
	// 阶段 5：NPC 死亡处理——添加 State.Dead Tag + 停止移动/碰撞 + 延迟销毁

	// 1. 添加 State.Dead Tag（供 StateTree / AI 感知使用）
	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->AddLooseGameplayTag(TAG_State_Dead);
	}

	// 2. 停止 AI 逻辑（BehaviorTree 停止运行）
	if (AAIController* AIC = Cast<AAIController>(GetController()))
	{
		if (UBrainComponent* Brain = AIC->GetBrainComponent())
		{
			Brain->StopLogic(TEXT("Dead"));
		}
	}

	// 3. 停止移动
	GetCharacterMovement()->DisableMovement();
	GetCharacterMovement()->StopMovementImmediately();

	// 3. 禁用碰撞（不再阻挡投射物或玩家）
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// 4. 隐藏 NPC（简单死亡表现，后续可替换为死亡动画/布娃娃）
	SetActorHiddenInGame(true);

	// 5. 延迟销毁（给网络复制和客户端特效一点时间）
	// 使用 WeakObjectPtr 避免 Actor 被外部 Destroy 后 lambda 访问悬垂指针
	FTimerHandle DestroyTimerHandle;
	TWeakObjectPtr<AWeaponNPC> WeakThis(this);
	GetWorldTimerManager().SetTimer(
		DestroyTimerHandle,
		[WeakThis]()
		{
			if (WeakThis.IsValid())
			{
				WeakThis->Destroy();
			}
		},
		DeathLingerTime,
		false
	);
}
