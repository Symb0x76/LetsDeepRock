using UnrealBuildTool;

public class LetsDeepRockEditor : ModuleRules
{
	public LetsDeepRockEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			// 运行时模块：房间资产与校验器（Editor 单向依赖 Runtime）
			"LetsDeepRock",

			"AssetRegistry",
			"AssetDefinition",
			"AssetTools",
			"BlueprintGraph",
			"EditorFramework",
			"EditorStyle",
			"GameplayTags",
			"InputCore",
			"Json",
			"PropertyEditor",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UnrealEd"
		});
	}
}
