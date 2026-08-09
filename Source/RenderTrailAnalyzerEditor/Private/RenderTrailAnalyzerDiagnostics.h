#pragma once

#include "CoreMinimal.h"

namespace UE::RenderTrail::Private
{
	struct FRenderTrailDiagnosticsOptions
	{
		bool bEnabled = true;
		bool bWorkerProtocol = true;
		bool bAgentTraffic = true;
		bool bFullEvidencePayload = false;
		bool bGpuCrashDiagnostics = false;
		bool bFastReplay = true;
		bool bRenderDocDRED = false;
	};

	class FRenderTrailAnalyzerDiagnostics final
	{
	public:
		void LoadConfiguration();
		void BeginSession(const FString& CapturePath, int64 CaptureSize);
		void WriteRecord(const FString& Stage, const FString& Detail, bool bReset = false);
		void WriteAgentLog(const FString& Stage, const FString& Detail, bool bReset = false);

		const FRenderTrailDiagnosticsOptions& GetOptions() const { return Options; }
		bool HasSession() const { return !DiagnosticsFilePath.IsEmpty(); }
		const FString& GetWorkerDiagnosticsFilePath() const { return WorkerDiagnosticsFilePath; }

	private:
		static bool ParseBool(const FString& Value, bool DefaultValue);
		static void ApplyConfigFile(const FString& Path, FRenderTrailDiagnosticsOptions& InOutOptions);
		static FString GetPluginConfigPath();
		static FString BoundLogText(FString Text, int32 MaxChars = 16000);

		FRenderTrailDiagnosticsOptions Options;
		FString DiagnosticsFilePath;
		FString WorkerDiagnosticsFilePath;
	};
}
