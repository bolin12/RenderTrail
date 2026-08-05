#include "RenderTrailAnalyzerDiagnostics.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogRenderTrailDiagnostics, Log, All);

namespace UE::RenderTrail::Private
{
	void FRenderTrailAnalyzerDiagnostics::LoadConfiguration()
	{
		Options = FRenderTrailDiagnosticsOptions();
		ApplyConfigFile(GetPluginConfigPath(), Options);
		ApplyConfigFile(FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectConfigDir(), TEXT("RenderTrailDiagnostics.ini"))), Options);
	}

	void FRenderTrailAnalyzerDiagnostics::BeginSession(const FString& CapturePath, int64 CaptureSize)
	{
		DiagnosticsFilePath.Empty();
		if (!Options.bEnabled)
		{
			return;
		}

		const FString Directory = FPaths::Combine(FPaths::ProjectLogDir(), TEXT("RenderTrailDiagnostics"));
		IFileManager::Get().MakeDirectory(*Directory, true);
		DiagnosticsFilePath = FPaths::Combine(Directory, FString::Printf(TEXT("RenderTrailDiagnostics_%s_%lld.log"),
			*FPaths::GetBaseFilename(CapturePath), FDateTime::Now().ToUnixTimestamp()));
		WriteRecord(TEXT("session_start"), FString::Printf(
			TEXT("capture=%s\nbytes=%lld\nworkerProtocol=%s\nagentTraffic=%s\nfullEvidencePayload=%s"),
			*CapturePath, CaptureSize,
			Options.bWorkerProtocol ? TEXT("true") : TEXT("false"),
			Options.bAgentTraffic ? TEXT("true") : TEXT("false"),
			Options.bFullEvidencePayload ? TEXT("true") : TEXT("false")), true);
		UE_LOG(LogRenderTrailDiagnostics, Display, TEXT("RenderTrail full diagnostics: %s"), *DiagnosticsFilePath);
	}

	void FRenderTrailAnalyzerDiagnostics::WriteRecord(const FString& Stage, const FString& Detail, bool bReset)
	{
		if (!Options.bEnabled || DiagnosticsFilePath.IsEmpty())
		{
			return;
		}
		const FString Line = FString::Printf(TEXT("[%s] [%s]\n%s\n\n"),
			*FDateTime::Now().ToIso8601(), *Stage, *Detail);
		FFileHelper::SaveStringToFile(Line, *DiagnosticsFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(), bReset ? 0 : (FILEWRITE_Append | FILEWRITE_AllowRead));
	}

	void FRenderTrailAnalyzerDiagnostics::WriteAgentLog(const FString& Stage, const FString& Detail, bool bReset)
	{
		const FString LogDirectory = FPaths::ProjectLogDir();
		IFileManager::Get().MakeDirectory(*LogDirectory, true);
		const FString Line = FString::Printf(TEXT("[%s] [%s]\n%s\n\n"),
			*FDateTime::Now().ToIso8601(), *Stage, *BoundLogText(Detail));
		const FString AgentLogPath = FPaths::Combine(LogDirectory, TEXT("RenderTrailAgent.log"));
		FFileHelper::SaveStringToFile(Line, *AgentLogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(), bReset ? 0 : (FILEWRITE_Append | FILEWRITE_AllowRead));
		if (Options.bAgentTraffic)
		{
			WriteRecord(FString::Printf(TEXT("agent_%s"), *Stage), Detail);
		}
		UE_LOG(LogRenderTrailDiagnostics, Display, TEXT("Agent[%s]: %s"), *Stage, *BoundLogText(Detail, 1200));
	}

	bool FRenderTrailAnalyzerDiagnostics::ParseBool(const FString& Value, bool DefaultValue)
	{
		if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value == TEXT("1")
			|| Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("on"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		if (Value.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Value == TEXT("0")
			|| Value.Equals(TEXT("no"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("off"), ESearchCase::IgnoreCase))
		{
			return false;
		}
		return DefaultValue;
	}

	void FRenderTrailAnalyzerDiagnostics::ApplyConfigFile(const FString& Path, FRenderTrailDiagnosticsOptions& InOutOptions)
	{
		FString Contents;
		if (!IFileManager::Get().FileExists(*Path) || !FFileHelper::LoadFileToString(Contents, *Path))
		{
			return;
		}

		TArray<FString> Lines;
		Contents.ParseIntoArrayLines(Lines, false);
		bool bInSection = false;
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty() || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#")))
			{
				continue;
			}
			if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
			{
				bInSection = Line.Mid(1, Line.Len() - 2).Equals(TEXT("RenderTrailDiagnostics"), ESearchCase::IgnoreCase);
				continue;
			}
			if (!bInSection)
			{
				continue;
			}

			FString Key;
			FString Value;
			if (!Line.Split(TEXT("="), &Key, &Value))
			{
				continue;
			}
			Key.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			if (Key.Equals(TEXT("bEnabled"), ESearchCase::IgnoreCase))
			{
				InOutOptions.bEnabled = ParseBool(Value, InOutOptions.bEnabled);
			}
			else if (Key.Equals(TEXT("bWorkerProtocol"), ESearchCase::IgnoreCase))
			{
				InOutOptions.bWorkerProtocol = ParseBool(Value, InOutOptions.bWorkerProtocol);
			}
			else if (Key.Equals(TEXT("bAgentTraffic"), ESearchCase::IgnoreCase))
			{
				InOutOptions.bAgentTraffic = ParseBool(Value, InOutOptions.bAgentTraffic);
			}
			else if (Key.Equals(TEXT("bFullEvidencePayload"), ESearchCase::IgnoreCase))
			{
				InOutOptions.bFullEvidencePayload = ParseBool(Value, InOutOptions.bFullEvidencePayload);
			}
		}
	}

	FString FRenderTrailAnalyzerDiagnostics::GetPluginConfigPath()
	{
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RenderTrail")))
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(
				Plugin->GetBaseDir(), TEXT("Config"), TEXT("RenderTrailDiagnostics.ini")));
		}
		return FString();
	}

	FString FRenderTrailAnalyzerDiagnostics::BoundLogText(FString Text, int32 MaxChars)
	{
		if (Text.Len() > MaxChars)
		{
			const int32 Omitted = Text.Len() - MaxChars;
			Text.LeftInline(MaxChars);
			Text += FString::Printf(TEXT("\n... [%d chars omitted by RenderTrail log bound]"), Omitted);
		}
		return Text;
	}
}
