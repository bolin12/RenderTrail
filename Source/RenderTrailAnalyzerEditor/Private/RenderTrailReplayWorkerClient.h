#pragma once

#include "CoreMinimal.h"

namespace UE::RenderTrail::Private
{
	struct FRenderTrailReplayWorkerPollResult
	{
		TArray<FString> OutputLines;
		FString ErrorChunk;
		FString PartialOutput;
		FString ExitDetail;
		bool bExited = false;
	};

	struct FRenderTrailReplayWorkerStopResult
	{
		double ElapsedSeconds = 0.0;
		bool bHadProcess = false;
		bool bWasRunning = false;
		bool bShutdownWritten = false;
		bool bExitedGracefully = false;
		bool bForcedTermination = false;
	};

	class FRenderTrailReplayWorkerClient final
	{
	public:
		~FRenderTrailReplayWorkerClient();

		static FString GetDefaultExecutablePath();

		bool Launch(const FString& WorkerPath, const FString& CapturePath, const FString& PreviewPath,
			bool bFullDiagnostics, FString& OutCommandLine, FString& OutError);
		FRenderTrailReplayWorkerStopResult Stop();
		FRenderTrailReplayWorkerPollResult Poll();
		bool Write(const FString& Payload);

		bool IsRunning();

	private:
		void ClosePipes();
		void DrainOutput(TArray<FString>& OutLines);

		FProcHandle ProcessHandle;
		void* StdOutRead = nullptr;
		void* StdOutWrite = nullptr;
		void* StdInRead = nullptr;
		void* StdInWrite = nullptr;
		void* StdErrRead = nullptr;
		void* StdErrWrite = nullptr;
		FString OutputBuffer;
		FString ErrorBuffer;
		bool bExitReported = false;
	};
}
