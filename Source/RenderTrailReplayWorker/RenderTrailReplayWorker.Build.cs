using UnrealBuildTool;
using System;
using System.IO;

public class RenderTrailReplayWorker : ModuleRules
{
	public RenderTrailReplayWorker(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.NoPCHs;
		bWarningsAsErrors = false;
		bTreatAsEngineModule = true;
		if (Target.Type != TargetType.Program)
		{
			throw new BuildException("RenderTrailReplayWorker must only be built as the isolated RenderTrailReplayWorker program.");
		}
		PublicIncludePathModuleNames.Add("Launch");
		PublicDefinitions.Add("RENDERTRAIL_REPLAY_WORKER_PROGRAM=1");
		string PluginRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		string ProjectRoot = Target.ProjectFile != null
			? Target.ProjectFile.Directory.FullName
			: Path.GetFullPath(Path.Combine(PluginRoot, "..", ".."));
		string[] RenderDocCandidates =
		{
			Environment.GetEnvironmentVariable("RENDERTRAIL_RENDERDOC_ROOT"),
			Path.Combine(PluginRoot, "ThirdParty", "RenderDoc"),
			Path.Combine(ProjectRoot, "ThirdParty", "RenderDoc"),
			Path.Combine(EngineDirectory, "ThirdParty", "RenderDoc")
		};
		string RenderDocRoot = null;
		foreach (string Candidate in RenderDocCandidates)
		{
			if (!string.IsNullOrWhiteSpace(Candidate) && Directory.Exists(Candidate))
			{
				RenderDocRoot = Path.GetFullPath(Candidate);
				break;
			}
		}
		if (string.IsNullOrWhiteSpace(RenderDocRoot))
		{
			throw new BuildException("RenderTrailReplayWorker requires the version-matched RenderDoc SDK. Set RENDERTRAIL_RENDERDOC_ROOT or provide ThirdParty/RenderDoc in the plugin, project, or Engine.");
		}

		PublicSystemIncludePaths.Add(Path.Combine(RenderDocRoot, "Source"));
		PublicDefinitions.Add("RENDERDOC_PLATFORM_WIN32=1");
		PublicAdditionalLibraries.Add(Path.Combine(RenderDocRoot, "Lib", "Win64", "renderdoc.lib"));
		PublicDelayLoadDLLs.Add("renderdoc.dll");
		PublicSystemLibraries.AddRange(new[] { "dxgi.lib", "wevtapi.lib" });
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"ApplicationCore",
			"Core",
			"CoreUObject",
			"Json",
			"RenderTrailCore",
			"Projects"
		});
	}
}
