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
			"AssetRegistry",
			"BlueprintGraph",
			"InputCore",
			"Json"
		});
	}
}
