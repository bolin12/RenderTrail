#include "RenderTrailReplayDiagnostics.h"

#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsPlatformMisc.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <dxgi1_4.h>
#include <winevt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace UE::RenderTrail::Private
{
	namespace
	{
		FCriticalSection GWorkerDiagnosticsLock;
		FString GWorkerDiagnosticsPath;
		FString GRenderDocDiagnosticsPath;
		FString GRenderDocLiveDiagnosticsPath;
		double GWorkerDiagnosticsStartSeconds = 0.0;

		FString MiB(uint64 Bytes)
		{
			return FString::Printf(TEXT("%.1f MiB"), static_cast<double>(Bytes) / (1024.0 * 1024.0));
		}

		FString JoinLines(const TArray<FString>& Lines)
		{
			return Lines.IsEmpty() ? TEXT("none") : FString::Join(Lines, TEXT("\n"));
		}

#if PLATFORM_WINDOWS
		FString DescribeVideoMemoryInfo(IDXGIAdapter3* Adapter, DXGI_MEMORY_SEGMENT_GROUP Segment)
		{
			DXGI_QUERY_VIDEO_MEMORY_INFO Info = {};
			const HRESULT Hr = Adapter->QueryVideoMemoryInfo(0, Segment, &Info);
			if (FAILED(Hr))
			{
				return FString::Printf(TEXT("queryFailed=0x%08X"), static_cast<uint32>(Hr));
			}
			return FString::Printf(TEXT("budget=%s currentProcessUsage=%s availableForReservation=%s currentReservation=%s"),
				*MiB(Info.Budget), *MiB(Info.CurrentUsage), *MiB(Info.AvailableForReservation),
				*MiB(Info.CurrentReservation));
		}

		FString BuildDxgiSnapshot()
		{
			IDXGIFactory1* Factory = nullptr;
			const HRESULT FactoryHr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&Factory));
			if (FAILED(FactoryHr) || !Factory)
			{
				return FString::Printf(TEXT("DXGI factory unavailable: hr=0x%08X"), static_cast<uint32>(FactoryHr));
			}

			TArray<FString> Lines;
			for (uint32 Index = 0; ; ++Index)
			{
				IDXGIAdapter1* Adapter = nullptr;
				const HRESULT EnumHr = Factory->EnumAdapters1(Index, &Adapter);
				if (EnumHr == DXGI_ERROR_NOT_FOUND)
				{
					break;
				}
				if (FAILED(EnumHr) || !Adapter)
				{
					Lines.Add(FString::Printf(TEXT("adapter[%u] enumerationFailed=0x%08X"), Index,
						static_cast<uint32>(EnumHr)));
					break;
				}

				DXGI_ADAPTER_DESC1 Desc = {};
				const HRESULT DescHr = Adapter->GetDesc1(&Desc);
				FString Line = FAILED(DescHr)
					? FString::Printf(TEXT("adapter[%u] descriptionFailed=0x%08X"), Index, static_cast<uint32>(DescHr))
					: FString::Printf(
						TEXT("adapter[%u] name='%s' vendor=0x%04X device=0x%04X subSys=0x%08X revision=%u luid=%08X:%08X dedicatedVideo=%s dedicatedSystem=%s sharedSystem=%s flags=0x%X"),
						Index, Desc.Description, Desc.VendorId, Desc.DeviceId, Desc.SubSysId, Desc.Revision,
						static_cast<uint32>(Desc.AdapterLuid.HighPart), Desc.AdapterLuid.LowPart,
						*MiB(Desc.DedicatedVideoMemory), *MiB(Desc.DedicatedSystemMemory),
						*MiB(Desc.SharedSystemMemory), static_cast<uint32>(Desc.Flags));

				LARGE_INTEGER DriverVersion = {};
				const HRESULT DriverHr = Adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &DriverVersion);
				Line += SUCCEEDED(DriverHr)
					? FString::Printf(TEXT(" driverVersionRaw=0x%016llX"),
						static_cast<unsigned long long>(DriverVersion.QuadPart))
					: FString::Printf(TEXT(" driverVersionQueryFailed=0x%08X"), static_cast<uint32>(DriverHr));

				IDXGIAdapter3* Adapter3 = nullptr;
				const HRESULT Adapter3Hr = Adapter->QueryInterface(
					__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&Adapter3));
				if (SUCCEEDED(Adapter3Hr) && Adapter3)
				{
					Line += FString::Printf(TEXT("\n  local: %s\n  nonLocal: %s"),
						*DescribeVideoMemoryInfo(Adapter3, DXGI_MEMORY_SEGMENT_GROUP_LOCAL),
						*DescribeVideoMemoryInfo(Adapter3, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL));
					Adapter3->Release();
				}
				else
				{
					Line += FString::Printf(TEXT(" videoMemoryInfoUnavailable=0x%08X"), static_cast<uint32>(Adapter3Hr));
				}
				Lines.Add(MoveTemp(Line));
				Adapter->Release();
			}
			Factory->Release();
			return JoinLines(Lines);
		}

		FString BuildRelevantProcessSnapshot()
		{
			TArray<FString> Lines;
			FPlatformProcess::FProcEnumerator Enumerator;
			while (Enumerator.MoveNext())
			{
				const FPlatformProcess::FProcEnumInfo Info = Enumerator.GetCurrent();
				const FString Name = Info.GetName();
				const FString Lower = Name.ToLower();
				if (!Lower.Contains(TEXT("unreal")) && !Lower.Contains(TEXT("renderdoc"))
					&& !Lower.Contains(TEXT("qrenderdoc")) && !Lower.Contains(TEXT("rendertrail")))
				{
					continue;
				}
				SIZE_T WorkingSetBytes = 0;
				const bool bHasMemory = FPlatformProcess::GetApplicationMemoryUsage(Info.GetPID(), &WorkingSetBytes);
				Lines.Add(FString::Printf(TEXT("pid=%u parentPid=%u name='%s' workingSet=%s"),
					Info.GetPID(), Info.GetParentPID(), *Name,
					bHasMemory ? *MiB(static_cast<uint64>(WorkingSetBytes)) : TEXT("unavailable")));
			}
			return JoinLines(Lines);
		}
#endif
	}

	void InitializeWorkerDiagnostics(const FString& InWorkerLogPath)
	{
		FScopeLock Lock(&GWorkerDiagnosticsLock);
		GWorkerDiagnosticsPath = InWorkerLogPath.IsEmpty()
			? FString()
			: FPaths::ConvertRelativePathToFull(InWorkerLogPath);
		GRenderDocDiagnosticsPath = GWorkerDiagnosticsPath.IsEmpty()
			? FString()
			: FPaths::ChangeExtension(GWorkerDiagnosticsPath, TEXT("renderdoc.log"));
		GRenderDocLiveDiagnosticsPath = GWorkerDiagnosticsPath.IsEmpty()
			? FString()
			: FPaths::ChangeExtension(GWorkerDiagnosticsPath, TEXT("renderdoc.live.log"));
		GWorkerDiagnosticsStartSeconds = FPlatformTime::Seconds();
		if (!GWorkerDiagnosticsPath.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(GWorkerDiagnosticsPath), true);
			const FString Header = FString::Printf(TEXT("[%s] [worker_diagnostics_start] elapsed=0.000 thread=%u\npath=%s\nrenderDocLog=%s\nrenderDocLiveLog=%s\n\n"),
				*FDateTime::Now().ToIso8601(), FPlatformTLS::GetCurrentThreadId(),
				*GWorkerDiagnosticsPath, *GRenderDocDiagnosticsPath, *GRenderDocLiveDiagnosticsPath);
			FFileHelper::SaveStringToFile(Header, *GWorkerDiagnosticsPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_AllowRead);
		}
	}

	const FString& GetWorkerDiagnosticsPath()
	{
		return GWorkerDiagnosticsPath;
	}

	const FString& GetRenderDocDiagnosticsPath()
	{
		return GRenderDocDiagnosticsPath;
	}

	const FString& GetRenderDocLiveDiagnosticsPath()
	{
		return GRenderDocLiveDiagnosticsPath;
	}

	void WriteWorkerDiagnostic(const FString& Stage, const FString& Detail)
	{
		FScopeLock Lock(&GWorkerDiagnosticsLock);
		if (GWorkerDiagnosticsPath.IsEmpty())
		{
			return;
		}
		FString BoundedDetail = Detail;
		if (BoundedDetail.Len() > 131072)
		{
			BoundedDetail.LeftInline(131072, EAllowShrinking::No);
			BoundedDetail += TEXT("\n... [worker diagnostic record truncated at 131072 characters]");
		}
		const FString Record = FString::Printf(TEXT("[%s] [%s] elapsed=%.3f thread=%u\n%s\n\n"),
			*FDateTime::Now().ToIso8601(), *Stage,
			FMath::Max(0.0, FPlatformTime::Seconds() - GWorkerDiagnosticsStartSeconds),
			FPlatformTLS::GetCurrentThreadId(), *BoundedDetail);
		FFileHelper::SaveStringToFile(Record, *GWorkerDiagnosticsPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(),
			FILEWRITE_Append | FILEWRITE_AllowRead);
	}

	FString BuildWorkerMemorySnapshot()
	{
		const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
		return FString::Printf(
			TEXT("processPhysical=%s processPeakPhysical=%s processVirtual=%s processPeakVirtual=%s systemAvailablePhysical=%s systemAvailableVirtual=%s"),
			*MiB(Stats.UsedPhysical), *MiB(Stats.PeakUsedPhysical), *MiB(Stats.UsedVirtual),
			*MiB(Stats.PeakUsedVirtual), *MiB(Stats.AvailablePhysical), *MiB(Stats.AvailableVirtual));
	}

	FString BuildWorkerGpuMemorySnapshot()
	{
#if PLATFORM_WINDOWS
		return BuildDxgiSnapshot();
#else
		return TEXT("GPU memory snapshot is only implemented on Windows.");
#endif
	}

	FString BuildWorkerSystemSnapshot()
	{
		FString OsLabel;
		FString OsVersion;
		FPlatformMisc::GetOSVersions(OsLabel, OsVersion);
		TArray<FString> Sections;
		Sections.Add(FString::Printf(TEXT("processId=%u executable='%s' baseDir='%s' workingDir='%s'"),
			FPlatformProcess::GetCurrentProcessId(), FPlatformProcess::ExecutablePath(),
			FPlatformProcess::BaseDir(), *FPlatformProcess::GetCurrentWorkingDirectory()));
		Sections.Add(FString::Printf(TEXT("os='%s' version='%s' cpu='%s' primaryGpu='%s'"),
			*OsLabel, *OsVersion, *FPlatformMisc::GetCPUBrand(), *FPlatformMisc::GetPrimaryGPUBrand()));
		Sections.Add(BuildWorkerMemorySnapshot());
#if PLATFORM_WINDOWS
		const FTDRInfo Tdr = FWindowsPlatformMisc::GetTDRInfo();
		Sections.Add(FString::Printf(TEXT("tdrLevel=%s tdrDelaySeconds=%u tdrDdiDelaySeconds=%u"),
			LexToString(Tdr.Level), Tdr.Delay, Tdr.DdiDelay));
		Sections.Add(FString::Printf(TEXT("relevantProcesses:\n%s"), *BuildRelevantProcessSnapshot()));
		Sections.Add(FString::Printf(TEXT("dxgiAdaptersAndBudgets:\n%s"), *BuildDxgiSnapshot()));
#endif
		return FString::Join(Sections, TEXT("\n"));
	}

	FString BuildRecentGpuEventSnapshot(int32 LookbackSeconds, int32 MaxEvents)
	{
#if PLATFORM_WINDOWS
		const FString QueryText = FString::Printf(
			TEXT("*[System[(Provider[@Name='nvlddmkm'] or Provider[@Name='Display'] or Provider[@Name='Microsoft-Windows-DxgKrnl']) and TimeCreated[timediff(@SystemTime) <= %d]]]"),
			FMath::Max(1, LookbackSeconds) * 1000);
		EVT_HANDLE Query = EvtQuery(nullptr, L"System", *QueryText,
			EvtQueryChannelPath | EvtQueryReverseDirection);
		if (!Query)
		{
			return FString::Printf(TEXT("Windows Event Log query failed: error=%u query=%s"),
				static_cast<uint32>(GetLastError()), *QueryText);
		}

		TArray<FString> Events;
		for (int32 Index = 0; Index < FMath::Max(1, MaxEvents); ++Index)
		{
			EVT_HANDLE Event = nullptr;
			DWORD Returned = 0;
			if (!EvtNext(Query, 1, &Event, 0, 0, &Returned) || Returned == 0)
			{
				const DWORD Error = GetLastError();
				if (Error != ERROR_NO_MORE_ITEMS)
				{
					Events.Add(FString::Printf(TEXT("EvtNext failed: error=%u"), static_cast<uint32>(Error)));
				}
				break;
			}

			DWORD BufferUsed = 0;
			DWORD PropertyCount = 0;
			EvtRender(nullptr, Event, EvtRenderEventXml, 0, nullptr, &BufferUsed, &PropertyCount);
			if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && BufferUsed > sizeof(wchar_t))
			{
				TArray<wchar_t> Buffer;
				Buffer.SetNumZeroed((BufferUsed / sizeof(wchar_t)) + 1);
				if (EvtRender(nullptr, Event, EvtRenderEventXml, BufferUsed, Buffer.GetData(),
					&BufferUsed, &PropertyCount))
				{
					FString Xml(Buffer.GetData());
					Xml.ReplaceInline(TEXT("\r"), TEXT(""));
					if (Xml.Len() > 16384)
					{
						Xml.LeftInline(16384, EAllowShrinking::No);
						Xml += TEXT("... [event XML truncated]");
					}
					Events.Add(MoveTemp(Xml));
				}
				else
				{
					Events.Add(FString::Printf(TEXT("EvtRender failed: error=%u"),
						static_cast<uint32>(GetLastError())));
				}
			}
			EvtClose(Event);
		}
		EvtClose(Query);
		return Events.IsEmpty() ? TEXT("No recent NVIDIA/Display/DxgKrnl events in the requested window.")
			: FString::Join(Events, TEXT("\n--- gpu-event ---\n"));
#else
		return TEXT("GPU event snapshot is only implemented on Windows.");
#endif
	}

	FOpenCaptureWatchdog::FOpenCaptureWatchdog(double InOpenCaptureStartSeconds)
		: OpenCaptureStartSeconds(InOpenCaptureStartSeconds)
	{
		LastProgressCallbackSeconds.store(InOpenCaptureStartSeconds, std::memory_order_relaxed);
		LastProgressChangeSeconds.store(InOpenCaptureStartSeconds, std::memory_order_relaxed);
	}

	FOpenCaptureWatchdog::~FOpenCaptureWatchdog()
	{
		Finish();
	}

	void FOpenCaptureWatchdog::Start()
	{
		if (!Thread && !GetWorkerDiagnosticsPath().IsEmpty())
		{
			Thread = FRunnableThread::Create(this, TEXT("RenderTrailOpenCaptureWatchdog"), 0, TPri_BelowNormal);
			if (!Thread)
			{
				WriteWorkerDiagnostic(TEXT("open_capture.watchdog"), TEXT("Failed to create watchdog thread."));
			}
		}
	}

	void FOpenCaptureWatchdog::NotifyProgress(float OverallProgress)
	{
		const float Clamped = FMath::Clamp(OverallProgress, 0.0f, 1.0f);
		const float Previous = LastOverallProgress.exchange(Clamped, std::memory_order_relaxed);
		const double Now = FPlatformTime::Seconds();
		LastProgressCallbackSeconds.store(Now, std::memory_order_relaxed);
		if (!FMath::IsNearlyEqual(Previous, Clamped, 0.0001f))
		{
			LastProgressChangeSeconds.store(Now, std::memory_order_relaxed);
		}
	}

	void FOpenCaptureWatchdog::Finish()
	{
		Stop();
		if (Thread)
		{
			Thread->WaitForCompletion();
			delete Thread;
			Thread = nullptr;
		}
	}

	void FOpenCaptureWatchdog::Stop()
	{
		bStopRequested.store(true, std::memory_order_relaxed);
	}

	uint32 FOpenCaptureWatchdog::Run()
	{
		double LastHeartbeatSeconds = OpenCaptureStartSeconds;
		double LastGpuMemorySnapshotSeconds = OpenCaptureStartSeconds;
		while (!bStopRequested.load(std::memory_order_relaxed))
		{
			FPlatformProcess::Sleep(0.25f);
			const double Now = FPlatformTime::Seconds();
			if (Now - LastHeartbeatSeconds >= 5.0)
			{
				const double ProgressAge = FMath::Max(0.0,
					Now - LastProgressCallbackSeconds.load(std::memory_order_relaxed));
				const double ProgressValueAge = FMath::Max(0.0,
					Now - LastProgressChangeSeconds.load(std::memory_order_relaxed));
				WriteWorkerDiagnostic(TEXT("open_capture.heartbeat"), FString::Printf(
					TEXT("openCaptureElapsed=%.3fs overallProgress=%.1f%% progressCallbackAge=%.3fs progressValueAge=%.3fs progressStalled=%s %s"),
					Now - OpenCaptureStartSeconds,
					LastOverallProgress.load(std::memory_order_relaxed) * 100.0f,
					ProgressAge, ProgressValueAge, ProgressValueAge >= 30.0 ? TEXT("true") : TEXT("false"),
					*BuildWorkerMemorySnapshot()));
				LastHeartbeatSeconds = Now;
			}
			if (Now - LastGpuMemorySnapshotSeconds >= 15.0)
			{
				WriteWorkerDiagnostic(TEXT("open_capture.gpu_memory"), BuildWorkerGpuMemorySnapshot());
				LastGpuMemorySnapshotSeconds = Now;
			}
		}
		return 0;
	}
}
