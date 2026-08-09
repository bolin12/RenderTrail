#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"

#include <atomic>

class FRunnableThread;

namespace UE::RenderTrail::Private
{
	void InitializeWorkerDiagnostics(const FString& InWorkerLogPath);
	const FString& GetWorkerDiagnosticsPath();
	const FString& GetRenderDocDiagnosticsPath();
	const FString& GetRenderDocLiveDiagnosticsPath();
	void WriteWorkerDiagnostic(const FString& Stage, const FString& Detail);

	FString BuildWorkerMemorySnapshot();
	FString BuildWorkerGpuMemorySnapshot();
	FString BuildWorkerSystemSnapshot();
	FString BuildRecentGpuEventSnapshot(int32 LookbackSeconds = 300, int32 MaxEvents = 8);

	class FOpenCaptureWatchdog final : public FRunnable
	{
	public:
		explicit FOpenCaptureWatchdog(double InOpenCaptureStartSeconds);
		~FOpenCaptureWatchdog() override;

		void Start();
		void NotifyProgress(float OverallProgress);
		void Finish();

		void Stop() override;
		uint32 Run() override;

	private:
		double OpenCaptureStartSeconds = 0.0;
		std::atomic<bool> bStopRequested{false};
		std::atomic<float> LastOverallProgress{0.0f};
		std::atomic<double> LastProgressCallbackSeconds{0.0};
		std::atomic<double> LastProgressChangeSeconds{0.0};
		FRunnableThread* Thread = nullptr;
	};
}
