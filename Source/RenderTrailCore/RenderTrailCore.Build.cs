using UnrealBuildTool;

public class RenderTrailCore : ModuleRules
{
	public RenderTrailCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.NoPCHs;
		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"DeveloperSettings",
			"Json",
			"Projects"
		});
	}
}
