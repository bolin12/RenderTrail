using UnrealBuildTool;
using System.IO;

public class RenderTrailCaptureEditor : ModuleRules
{
	public RenderTrailCaptureEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source", "ThirdParty", "RenderDoc"));
		PrivateIncludePathModuleNames.AddRange(new[] { "D3D11RHI", "D3D12RHI" });
		AddEngineThirdPartyPrivateStaticDependencies(Target, "RenderDoc");

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"DeveloperSettings",
			"Engine",
			"HTTP",
			"Json",
			"JsonUtilities",
			"LevelEditor",
			"MainFrame",
			"PropertyEditor",
			"RHI",
			"RenderTrailAnalyzerEditor",
			"RenderTrailCore",
			"RenderCore",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UnrealEd"
		});
	}
}
