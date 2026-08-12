using UnrealBuildTool;

public class RenderTrailAnalyzerEditor : ModuleRules
{
	public RenderTrailAnalyzerEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"ApplicationCore",
			"Core",
			"CoreUObject",
			"DesktopPlatform",
			"HTTP",
			"ImageCore",
			"ImageWrapper",
			"InputCore",
			"Json",
			"PropertyEditor",
			"Projects",
			"RenderTrailCore",
			"Slate",
			"SlateCore",
			"UnrealEd"
		});

	}
}
