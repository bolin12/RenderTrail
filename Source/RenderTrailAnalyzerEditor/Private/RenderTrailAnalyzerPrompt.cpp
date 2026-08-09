#include "RenderTrailAnalyzerPrompt.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogRenderTrailAnalyzerPrompt, Log, All);

namespace UE::RenderTrail::Private
{
	static bool LoadAgentPromptIni(const FString& Path, FString& OutPrompt, int32& OutLineCount)
	{
		OutPrompt.Empty();
		OutLineCount = 0;
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *Path))
		{
			return false;
		}

		TArray<FString> Lines;
		Contents.ParseIntoArrayLines(Lines, false);
		bool bInPromptSection = false;
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty() || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#")))
			{
				continue;
			}
			if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
			{
				bInPromptSection = Line.Mid(1, Line.Len() - 2).Equals(TEXT("RenderTrailAgentPrompt"), ESearchCase::IgnoreCase);
				continue;
			}
			if (!bInPromptSection)
			{
				continue;
			}

			int32 Separator = INDEX_NONE;
			if (Line.StartsWith(TEXT("+Line="))) Separator = 6;
			else if (Line.StartsWith(TEXT("Line="))) Separator = 5;
			if (Separator != INDEX_NONE)
			{
				FString Value = Line.Mid(Separator).TrimStartAndEnd();
				if (!Value.IsEmpty())
				{
					if (!OutPrompt.IsEmpty()) OutPrompt += TEXT("\n");
					OutPrompt += Value;
					++OutLineCount;
				}
			}
			else if (Line.StartsWith(TEXT("Prompt=")))
			{
				OutPrompt = Line.Mid(7).TrimStartAndEnd();
			}
		}
		return !OutPrompt.IsEmpty();
	}

	FString LoadRenderTrailAgentSystemPrompt()
	{
		const FString ProjectOverridePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectConfigDir(), TEXT("RenderTrailAgentPrompt.ini")));
		TArray<FString> IniPromptPaths = { ProjectOverridePath };
		FString PluginOverridePath;
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RenderTrail")))
		{
			PluginOverridePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				Plugin->GetBaseDir(), TEXT("Config"), TEXT("RenderTrailAgentPrompt.ini")));
			IniPromptPaths.Add(PluginOverridePath);
		}

		for (const FString& PromptPath : IniPromptPaths)
		{
			FString Prompt;
			int32 PromptLineCount = 0;
			if (IFileManager::Get().FileExists(*PromptPath) && LoadAgentPromptIni(PromptPath, Prompt, PromptLineCount))
			{
				UE_LOG(LogRenderTrailAnalyzerPrompt, Display,
					TEXT("Loaded Agent system prompt from INI: path='%s' lines=%d chars=%d"),
					*PromptPath, PromptLineCount, Prompt.Len());
				return Prompt;
			}
		}

		TArray<FString> LegacyPromptPaths = {
			FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("RenderTrailAgentPrompt.txt"))
		};
		if (!PluginOverridePath.IsEmpty())
		{
			LegacyPromptPaths.Add(FPaths::Combine(FPaths::GetPath(PluginOverridePath), TEXT("RenderTrailAgentPrompt.txt")));
		}
		for (const FString& PromptPath : LegacyPromptPaths)
		{
			FString Prompt;
			if (IFileManager::Get().FileExists(*PromptPath) && FFileHelper::LoadFileToString(Prompt, *PromptPath))
			{
				Prompt.TrimStartAndEndInline();
				if (!Prompt.IsEmpty())
				{
					UE_LOG(LogRenderTrailAnalyzerPrompt, Display,
						TEXT("Loaded legacy Agent prompt: path='%s' chars=%d"), *PromptPath, Prompt.Len());
					return Prompt;
				}
			}
		}

		UE_LOG(LogRenderTrailAnalyzerPrompt, Warning,
			TEXT("Agent prompt was not found or empty; using the safe fallback. Project='%s' plugin='%s'."),
			*ProjectOverridePath, *PluginOverridePath);
		return TEXT("You are RenderTrail's read-only selected-pixel forensics agent. Respond in Chinese with exactly one finish JSON object. Use only supplied RenderDoc evidence. Distinguish the final physical writer from significant upstream color formation, never treat a resource binding or discard as proof of pixel contribution, never invent shader algorithms or UE asset attribution, and list chain breaks in unknowns. Treat causalLanes.color, causalLanes.geometry, and causalLanes.overlay as parallel DAGs and never concatenate them into one chronology; each lane step must be a consumer <- resource <- producer edge. Follow deterministicEventContextIndex writer/consumer edges and cover critical, asset-marker, scene-raster, Nanite, depth, and boundary roles; selectedForDetail is detail availability, not existence or importance.");
	}
}
