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
			"Engine",
			"LevelEditor",
			"MainFrame",
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
