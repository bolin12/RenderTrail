#include "RenderTrailReplayWorkerClient.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

namespace UE::RenderTrail::Private
{
	FRenderTrailReplayWorkerClient::~FRenderTrailReplayWorkerClient()
	{
		Stop();
	}

	FString FRenderTrailReplayWorkerClient::GetDefaultExecutablePath()
	{
		const FString BinaryName = TEXT("RenderTrailReplayWorker.exe");
		const FString ProjectBinary = FPaths::Combine(
			FPaths::ProjectDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory(), BinaryName);
		if (FPaths::FileExists(ProjectBinary))
		{
			return FPaths::ConvertRelativePathToFull(ProjectBinary);
		}
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::EngineDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory(), BinaryName));
	}

	bool FRenderTrailReplayWorkerClient::Launch(const FString& WorkerPath, const FString& CapturePath,
		const FString& PreviewPath, bool bFullDiagnostics, FString& OutCommandLine, FString& OutError)
	{
		Stop();
		OutError.Empty();
		if (!FPlatformProcess::CreatePipe(StdOutRead, StdOutWrite, false)
			|| !FPlatformProcess::CreatePipe(StdInRead, StdInWrite, true)
			|| !FPlatformProcess::CreatePipe(StdErrRead, StdErrWrite, false))
		{
			OutError = TEXT("Could not create Replay Worker pipes.");
			Stop();
			return false;
		}

		const FString WorkerBaseDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory()));
		const FString FullDiagnosticsArgument = bFullDiagnostics ? TEXT(" -RenderTrailFullDiagnostics") : TEXT("");
		OutCommandLine = FString::Printf(TEXT("-basedir=\"%s\" -Server -Capture=\"%s\" -Preview=\"%s\"%s"),
			*WorkerBaseDir, *CapturePath, *PreviewPath, *FullDiagnosticsArgument);

		ProcessHandle = FPlatformProcess::CreateProc(*WorkerPath, *OutCommandLine, false, true, true, nullptr, 0,
			*WorkerBaseDir, StdOutWrite, StdInRead, StdErrWrite);
		if (!ProcessHandle.IsValid())
		{
			OutError = TEXT("Failed to launch isolated Replay Worker.");
			Stop();
			return false;
		}

		FPlatformProcess::ClosePipe(nullptr, StdOutWrite);
		StdOutWrite = nullptr;
		FPlatformProcess::ClosePipe(StdInRead, nullptr);
		StdInRead = nullptr;
		FPlatformProcess::ClosePipe(nullptr, StdErrWrite);
		StdErrWrite = nullptr;
		OutputBuffer.Empty();
		ErrorBuffer.Empty();
		bExitReported = false;
		return true;
	}

	FRenderTrailReplayWorkerStopResult FRenderTrailReplayWorkerClient::Stop()
	{
		FRenderTrailReplayWorkerStopResult Result;
		const double StopStartSeconds = FPlatformTime::Seconds();
		if (ProcessHandle.IsValid())
		{
			Result.bHadProcess = true;
			Result.bWasRunning = FPlatformProcess::IsProcRunning(ProcessHandle);
			if (StdInWrite && FPlatformProcess::IsProcRunning(ProcessHandle))
			{
				Result.bShutdownWritten = FPlatformProcess::WritePipe(StdInWrite, TEXT("{\"command\":\"shutdown\"}"));
				for (int32 Attempt = 0; Attempt < 20 && FPlatformProcess::IsProcRunning(ProcessHandle); ++Attempt)
				{
					FPlatformProcess::Sleep(0.05f);
				}
			}
			if (FPlatformProcess::IsProcRunning(ProcessHandle))
			{
				Result.bForcedTermination = true;
				FPlatformProcess::TerminateProc(ProcessHandle, true);
			}
			else if (Result.bWasRunning)
			{
				Result.bExitedGracefully = true;
			}
			FPlatformProcess::CloseProc(ProcessHandle);
			ProcessHandle.Reset();
		}
		ClosePipes();
		OutputBuffer.Empty();
		ErrorBuffer.Empty();
		bExitReported = false;
		Result.ElapsedSeconds = FPlatformTime::Seconds() - StopStartSeconds;
		return Result;
	}

	FRenderTrailReplayWorkerPollResult FRenderTrailReplayWorkerClient::Poll()
	{
		FRenderTrailReplayWorkerPollResult Result;
		if (!ProcessHandle.IsValid())
		{
			return Result;
		}

		if (StdOutRead)
		{
			OutputBuffer += FPlatformProcess::ReadPipe(StdOutRead);
			DrainOutput(Result.OutputLines);
		}
		if (StdErrRead)
		{
			Result.ErrorChunk = FPlatformProcess::ReadPipe(StdErrRead);
			ErrorBuffer += Result.ErrorChunk;
			if (ErrorBuffer.Len() > 16384)
			{
				ErrorBuffer.RightChopInline(ErrorBuffer.Len() - 16384, EAllowShrinking::No);
			}
		}

		if (!FPlatformProcess::IsProcRunning(ProcessHandle) && !bExitReported)
		{
			bExitReported = true;
			if (StdOutRead)
			{
				OutputBuffer += FPlatformProcess::ReadPipe(StdOutRead);
				DrainOutput(Result.OutputLines);
			}
			Result.bExited = true;
			Result.PartialOutput = OutputBuffer;
			Result.ExitDetail = ErrorBuffer.IsEmpty()
				? TEXT("Replay Worker exited before reporting ready.")
				: ErrorBuffer.Right(2000);
		}
		return Result;
	}

	bool FRenderTrailReplayWorkerClient::Write(const FString& Payload)
	{
		return IsRunning() && StdInWrite && FPlatformProcess::WritePipe(StdInWrite, Payload);
	}

	bool FRenderTrailReplayWorkerClient::IsRunning()
	{
		return ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcessHandle);
	}

	void FRenderTrailReplayWorkerClient::ClosePipes()
	{
		FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);
		FPlatformProcess::ClosePipe(StdInRead, StdInWrite);
		FPlatformProcess::ClosePipe(StdErrRead, StdErrWrite);
		StdOutRead = StdOutWrite = StdInRead = StdInWrite = StdErrRead = StdErrWrite = nullptr;
	}

	void FRenderTrailReplayWorkerClient::DrainOutput(TArray<FString>& OutLines)
	{
		int32 NewlineIndex = INDEX_NONE;
		while (OutputBuffer.FindChar(TEXT('\n'), NewlineIndex))
		{
			FString Line = OutputBuffer.Left(NewlineIndex);
			OutputBuffer.RightChopInline(NewlineIndex + 1, EAllowShrinking::No);
			Line.TrimStartAndEndInline();
			if (!Line.IsEmpty())
			{
				OutLines.Add(MoveTemp(Line));
			}
		}
	}
}
