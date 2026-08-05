using UnrealBuildTool;

[SupportedPlatforms("Win64")]
[SupportedConfigurations(UnrealTargetConfiguration.Debug, UnrealTargetConfiguration.Development)]
public class RenderTrailReplayWorkerTarget : TargetRules
{
	public RenderTrailReplayWorkerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Program;
		LinkType = TargetLinkType.Monolithic;
		BuildEnvironment = TargetBuildEnvironment.Unique;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		LaunchModuleName = "RenderTrailReplayWorker";

		bBuildDeveloperTools = false;
		bBuildWithEditorOnlyData = false;
		bCompileAgainstEngine = false;
		bCompileAgainstCoreUObject = true;
		bCompileAgainstApplicationCore = true;
		bCompileWithPluginSupport = true;
		bIncludePluginsForTargetPlatforms = true;
		bUsesSlate = false;
		bWithLiveCoding = false;
		bIsBuildingConsoleApplication = true;
		bUseLoggingInShipping = true;

		AdditionalPlugins.Add("RenderTrail");
	}
}
