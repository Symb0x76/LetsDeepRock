// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class LetsDeepRock : ModuleRules
{
	public LetsDeepRock(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"NetCore",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"NavigationSystem",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"Niagara",
			"UMG",
			"Slate",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"OnlineSubsystem",
			"OnlineSubsystemUtils"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"Networking",
			"Sockets"
		});

		// 阶段 1 起会用到 GameplayAbilities 类型的头文件，保持公共依赖即可

		PublicIncludePaths.AddRange(new string[] {
			"LetsDeepRock",
			"LetsDeepRock/Variant_Strategy",
			"LetsDeepRock/Variant_Strategy/UI",
			"LetsDeepRock/Variant_TwinStick",
			"LetsDeepRock/Variant_TwinStick/AI",
			"LetsDeepRock/Variant_TwinStick/Gameplay",
			"LetsDeepRock/Variant_TwinStick/UI",
			"LetsDeepRock/Variant_Weapon",
			"LetsDeepRock/Variant_Weapon/Abilities",
			"LetsDeepRock/Variant_Weapon/Attributes",
			"LetsDeepRock/Variant_Weapon/Weapons",
			"LetsDeepRock/Variant_Weapon/GridMap",
			"LetsDeepRock/Variant_Weapon/AI",
			"LetsDeepRock/Variant_Weapon/AI/Tasks",
			"LetsDeepRock/Variant_Weapon/AI/Services",
			"LetsDeepRock/Variant_Weapon/AI/Decorators",
			"LetsDeepRock/Data"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
