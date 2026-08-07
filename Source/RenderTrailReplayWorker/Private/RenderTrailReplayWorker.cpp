#include "RenderTrailProtocol.h"
#include "RenderTrailReplayEvidence.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Internationalization/Regex.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"

#if RENDERTRAIL_REPLAY_WORKER_PROGRAM
	#define IMPLEMENT_ENCRYPTION_KEY_REGISTRATION()
	#define IMPLEMENT_SIGNING_KEY_REGISTRATION()
	#include "RequiredProgramMainCPPInclude.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Containers/StringConv.h"
#endif

#include <cstdio>
#if RENDERTRAIL_REPLAY_WORKER_PROGRAM
#include <iostream>
#include <string>
#endif

// RenderDoc exposes a global LogType enum, while CoreUObject declares a global
// LogType log category. Keep the third-party name isolated to this include.
#define LogType RenderDocLogType
#include "renderdoc/api/replay/renderdoc_replay.h"
#undef LogType

// renderdoc_tostr.inl is a header-only enum formatter, but its bitfield helper
// expects the numeric DoStringise specializations normally supplied by the
// RenderDoc monolith. Provide the one needed by this isolated replay client.
template <>
rdcstr DoStringise(const uint32_t& Value)
{
	char Buffer[32] = {};
	std::snprintf(Buffer, sizeof(Buffer), "%u", Value);
	return rdcstr(Buffer);
}

#define LogType RenderDocLogType
#include "renderdoc/api/replay/renderdoc_tostr.inl"
#undef LogType

#if RENDERTRAIL_REPLAY_WORKER_PROGRAM
REPLAY_PROGRAM_MARKER()
#endif

DEFINE_LOG_CATEGORY_STATIC(LogRenderTrailReplayWorker, Log, All);

namespace UE::RenderTrail::Private
{
	#if RENDERTRAIL_REPLAY_WORKER_PROGRAM
	static bool GReplayInitialized = false;
	#endif

	// PixelHistory itself may inspect more events internally, but no individual query is allowed to
	// serialise an unbounded frame history across the worker boundary. Keep only the newest evidence.
	static constexpr int32 MaxSerializedPixelModifications = 256;

	static FString FromRdcString(const rdcstr& Value)
	{
		return FString(UTF8_TO_TCHAR(Value.c_str()));
	}

	static FString DescribeRenderDocLoadProgress(float OverallProgress)
	{
		// RenderDoc v1.45 weights LoadProgress as 10% debug resources,
		// 75% initial resource/chunk loading, and 15% frame event replay.
		const float Clamped = FMath::Clamp(OverallProgress, 0.0f, 1.0f);
		if (Clamped < 0.10f)
		{
			return FString::Printf(TEXT("debug resource initialisation %.1f%% (overall %.1f%%)"),
				Clamped * 1000.0f, Clamped * 100.0f);
		}
		if (Clamped < 0.85f)
		{
			return FString::Printf(TEXT("resource/chunk initialisation %.1f%% (overall %.1f%%)"),
				((Clamped - 0.10f) / 0.75f) * 100.0f, Clamped * 100.0f);
		}
		return FString::Printf(TEXT("frame event replay %.1f%% (overall %.1f%%)"),
			((Clamped - 0.85f) / 0.15f) * 100.0f, Clamped * 100.0f);
	}

	template <typename T>
	static FString RenderDocEnumToString(const T& Value)
	{
		return FromRdcString(ToStr(Value));
	}

	static void AddStencilFaceState(const TSharedRef<FJsonObject>& Object, const TCHAR* Prefix,
		const StencilFace& Face)
	{
		Object->SetStringField(FString::Printf(TEXT("%sFail"), Prefix), RenderDocEnumToString(Face.failOperation));
		Object->SetStringField(FString::Printf(TEXT("%sDepthFail"), Prefix), RenderDocEnumToString(Face.depthFailOperation));
		Object->SetStringField(FString::Printf(TEXT("%sPass"), Prefix), RenderDocEnumToString(Face.passOperation));
		Object->SetStringField(FString::Printf(TEXT("%sFunction"), Prefix), RenderDocEnumToString(Face.function));
		Object->SetNumberField(FString::Printf(TEXT("%sReference"), Prefix), Face.reference);
		Object->SetNumberField(FString::Printf(TEXT("%sCompareMask"), Prefix), Face.compareMask);
		Object->SetNumberField(FString::Printf(TEXT("%sWriteMask"), Prefix), Face.writeMask);
	}

	static TSharedRef<FJsonObject> BuildD3D12PipelineState(const D3D12Pipe::State& State)
	{
		const TSharedRef<FJsonObject> Pipeline = MakeShared<FJsonObject>();
		Pipeline->SetStringField(TEXT("api"), TEXT("D3D12"));
		Pipeline->SetStringField(TEXT("primitiveTopology"), RenderDocEnumToString(State.inputAssembly.topology));
		Pipeline->SetNumberField(TEXT("inputLayoutCount"), static_cast<double>(State.inputAssembly.layouts.size()));
		Pipeline->SetNumberField(TEXT("vertexBufferCount"), static_cast<double>(State.inputAssembly.vertexBuffers.size()));
		Pipeline->SetBoolField(TEXT("indexBufferBound"), State.inputAssembly.indexBuffer.resourceId != ResourceId());

		const TSharedRef<FJsonObject> Rasterizer = MakeShared<FJsonObject>();
		Rasterizer->SetStringField(TEXT("fillMode"), RenderDocEnumToString(State.rasterizer.state.fillMode));
		Rasterizer->SetStringField(TEXT("cullMode"), RenderDocEnumToString(State.rasterizer.state.cullMode));
		Rasterizer->SetBoolField(TEXT("frontCounterClockwise"), State.rasterizer.state.frontCCW);
		Rasterizer->SetBoolField(TEXT("depthClip"), State.rasterizer.state.depthClip);
		Rasterizer->SetNumberField(TEXT("sampleMask"), State.rasterizer.sampleMask);
		Rasterizer->SetNumberField(TEXT("viewportCount"), static_cast<double>(State.rasterizer.viewports.size()));
		Rasterizer->SetNumberField(TEXT("scissorCount"), static_cast<double>(State.rasterizer.scissors.size()));
		TArray<TSharedPtr<FJsonValue>> Viewports;
		for (const Viewport& ViewportState : State.rasterizer.viewports)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetBoolField(TEXT("enabled"), ViewportState.enabled);
			Item->SetNumberField(TEXT("x"), ViewportState.x);
			Item->SetNumberField(TEXT("y"), ViewportState.y);
			Item->SetNumberField(TEXT("width"), ViewportState.width);
			Item->SetNumberField(TEXT("height"), ViewportState.height);
			Item->SetNumberField(TEXT("minDepth"), ViewportState.minDepth);
			Item->SetNumberField(TEXT("maxDepth"), ViewportState.maxDepth);
			Viewports.Add(MakeShared<FJsonValueObject>(Item));
		}
		Rasterizer->SetArrayField(TEXT("viewports"), MoveTemp(Viewports));
		TArray<TSharedPtr<FJsonValue>> Scissors;
		for (const Scissor& ScissorState : State.rasterizer.scissors)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetBoolField(TEXT("enabled"), ScissorState.enabled);
			Item->SetNumberField(TEXT("x"), ScissorState.x);
			Item->SetNumberField(TEXT("y"), ScissorState.y);
			Item->SetNumberField(TEXT("width"), ScissorState.width);
			Item->SetNumberField(TEXT("height"), ScissorState.height);
			Scissors.Add(MakeShared<FJsonValueObject>(Item));
		}
		Rasterizer->SetArrayField(TEXT("scissors"), MoveTemp(Scissors));
		Pipeline->SetObjectField(TEXT("rasterizer"), Rasterizer);

		const D3D12Pipe::DepthStencilState& DepthStencilState = State.outputMerger.depthStencilState;
		const TSharedRef<FJsonObject> DepthStencil = MakeShared<FJsonObject>();
		DepthStencil->SetBoolField(TEXT("depthEnable"), DepthStencilState.depthEnable);
		DepthStencil->SetBoolField(TEXT("depthWrites"), DepthStencilState.depthWrites);
		DepthStencil->SetStringField(TEXT("depthFunction"), RenderDocEnumToString(DepthStencilState.depthFunction));
		DepthStencil->SetBoolField(TEXT("depthBoundsEnable"), DepthStencilState.depthBoundsEnable);
		DepthStencil->SetBoolField(TEXT("stencilEnable"), DepthStencilState.stencilEnable);
		DepthStencil->SetBoolField(TEXT("depthReadOnly"), State.outputMerger.depthReadOnly);
		DepthStencil->SetBoolField(TEXT("stencilReadOnly"), State.outputMerger.stencilReadOnly);
		AddStencilFaceState(DepthStencil, TEXT("front"), DepthStencilState.frontFace);
		AddStencilFaceState(DepthStencil, TEXT("back"), DepthStencilState.backFace);
		Pipeline->SetObjectField(TEXT("depthStencil"), DepthStencil);

		const D3D12Pipe::BlendState& BlendState = State.outputMerger.blendState;
		const TSharedRef<FJsonObject> Blend = MakeShared<FJsonObject>();
		Blend->SetBoolField(TEXT("alphaToCoverage"), BlendState.alphaToCoverage);
		Blend->SetBoolField(TEXT("independentBlend"), BlendState.independentBlend);
		Blend->SetNumberField(TEXT("targetCount"), static_cast<double>(BlendState.blends.size()));
		TArray<TSharedPtr<FJsonValue>> Targets;
		for (int32 Index = 0; Index < static_cast<int32>(BlendState.blends.size()); ++Index)
		{
			const ColorBlend& Target = BlendState.blends[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("slot"), Index);
			Item->SetBoolField(TEXT("enabled"), Target.enabled);
			Item->SetBoolField(TEXT("logicOperationEnabled"), Target.logicOperationEnabled);
			Item->SetStringField(TEXT("colorSource"), RenderDocEnumToString(Target.colorBlend.source));
			Item->SetStringField(TEXT("colorDestination"), RenderDocEnumToString(Target.colorBlend.destination));
			Item->SetStringField(TEXT("colorOperation"), RenderDocEnumToString(Target.colorBlend.operation));
			Item->SetStringField(TEXT("alphaSource"), RenderDocEnumToString(Target.alphaBlend.source));
			Item->SetStringField(TEXT("alphaDestination"), RenderDocEnumToString(Target.alphaBlend.destination));
			Item->SetStringField(TEXT("alphaOperation"), RenderDocEnumToString(Target.alphaBlend.operation));
			Item->SetNumberField(TEXT("writeMask"), Target.writeMask);
			Targets.Add(MakeShared<FJsonValueObject>(Item));
		}
		Blend->SetArrayField(TEXT("targets"), MoveTemp(Targets));
		Pipeline->SetObjectField(TEXT("blend"), Blend);

		Pipeline->SetNumberField(TEXT("renderTargetCount"), static_cast<double>(State.outputMerger.renderTargets.size()));
		Pipeline->SetBoolField(TEXT("predicationEnabled"), State.predication.resourceId != ResourceId());
		return Pipeline;
	}

	static FString ResultMessage(const ResultDetails& Result)
	{
		return Result.internal_msg
			? FromRdcString(*Result.internal_msg)
			: FString::Printf(TEXT("RenderDoc ResultCode %u"), static_cast<uint32>(Result.code));
	}

	static FString SerializeJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Object, Writer);
		return Json;
	}

	static FString DescribeAction(const ActionDescription& Action, const SDFile& StructuredFile)
	{
		const rdcstr StructuredName = Action.GetName(StructuredFile);
		if (!StructuredName.empty())
		{
			return FromRdcString(StructuredName);
		}
		if (static_cast<bool>(Action.flags & ActionFlags::Present))
		{
			return TEXT("Present");
		}
		if (static_cast<bool>(Action.flags & ActionFlags::Drawcall))
		{
			return TEXT("Draw call");
		}
		if (static_cast<bool>(Action.flags & ActionFlags::Clear))
		{
			return TEXT("Clear");
		}
		if (static_cast<bool>(Action.flags & ActionFlags::Resolve))
		{
			return TEXT("Resolve");
		}
		if (static_cast<bool>(Action.flags & ActionFlags::Copy))
		{
			return TEXT("Copy");
		}
		if (static_cast<bool>(Action.flags & (ActionFlags::Dispatch | ActionFlags::MeshDispatch | ActionFlags::DispatchRay)))
		{
			return TEXT("Dispatch");
		}
		return FString::Printf(TEXT("Event %u"), Action.eventId);
	}

	static FString DescribeActionKind(const ActionDescription& Action)
	{
		if (static_cast<bool>(Action.flags & ActionFlags::Present))
		{
			return TEXT("present");
		}
		if (static_cast<bool>(Action.flags & ActionFlags::Drawcall))
		{
			return TEXT("draw");
		}
		if (static_cast<bool>(Action.flags & (ActionFlags::Dispatch | ActionFlags::MeshDispatch | ActionFlags::DispatchRay)))
		{
			return TEXT("dispatch");
		}
		if (static_cast<bool>(Action.flags & ActionFlags::Copy))
		{
			return TEXT("copy");
		}
		if (static_cast<bool>(Action.flags & ActionFlags::Resolve))
		{
			return TEXT("resolve");
		}
		if (static_cast<bool>(Action.flags & ActionFlags::Clear))
		{
			return TEXT("clear");
		}
		return TEXT("other");
	}

	static FString DescribeActionPath(const ActionDescription& Action, const SDFile& StructuredFile)
	{
		TArray<FString> Components;
		for (const ActionDescription* Parent = Action.parent; Parent; Parent = Parent->parent)
		{
			FString Name = FromRdcString(Parent->customName);
			if (Name.IsEmpty())
			{
				Name = DescribeAction(*Parent, StructuredFile);
			}
			if (!Name.IsEmpty())
			{
				Components.Insert(MoveTemp(Name), 0);
			}
		}
		return FString::Join(Components, TEXT(" > "));
	}

	static void IndexActions(const rdcarray<ActionDescription>& Actions, const SDFile& StructuredFile,
		TMap<uint32, FString>& Names, TMap<uint32, FString>& Kinds, TMap<uint32, FString>& Paths,
		TMap<uint32, uint32>& Flags)
	{
		for (const ActionDescription& Action : Actions)
		{
			Names.Add(Action.eventId, DescribeAction(Action, StructuredFile));
			Kinds.Add(Action.eventId, DescribeActionKind(Action));
			Paths.Add(Action.eventId, DescribeActionPath(Action, StructuredFile));
			Flags.Add(Action.eventId, static_cast<uint32>(Action.flags));
			IndexActions(Action.children, StructuredFile, Names, Kinds, Paths, Flags);
		}
	}

	static void FlattenActions(const rdcarray<ActionDescription>& Actions, TArray<const ActionDescription*>& OutActions)
	{
		for (const ActionDescription& Action : Actions)
		{
			OutActions.Add(&Action);
			FlattenActions(Action.children, OutActions);
		}
	}

	static ResourceId FirstOutput(const ActionDescription& Action)
	{
		for (const ResourceId& Output : Action.outputs)
		{
			if (Output != ResourceId())
			{
				return Output;
			}
		}
		return ResourceId();
	}

	static TArray<TSharedPtr<FJsonValue>> NumberArray(const float* Values, int32 Count)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (FMath::IsFinite(Values[Index]))
			{
				Result.Add(MakeShared<FJsonValueNumber>(Values[Index]));
			}
			else
			{
				Result.Add(MakeShared<FJsonValueNull>());
			}
		}
		return Result;
	}

	static TArray<TSharedPtr<FJsonValue>> NumberArray(const uint32_t* Values, int32 Count)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Result.Add(MakeShared<FJsonValueNumber>(static_cast<double>(Values[Index])));
		}
		return Result;
	}

	static TSharedRef<FJsonObject> ModificationValueToJson(const ModificationValue& Value)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("valid"), Value.IsValid());
		Object->SetArrayField(TEXT("float"), NumberArray(Value.col.floatValue.data(), 4));
		Object->SetArrayField(TEXT("uint"), NumberArray(Value.col.uintValue.data(), 4));
		if (FMath::IsFinite(Value.depth))
		{
			Object->SetNumberField(TEXT("depth"), Value.depth);
		}
		else
		{
			Object->SetField(TEXT("depth"), MakeShared<FJsonValueNull>());
		}
		Object->SetNumberField(TEXT("stencil"), Value.stencil);
		return Object;
	}

	static TArray<TSharedPtr<FJsonValue>> GetFailureReasons(const PixelModification& Modification)
	{
		TArray<TSharedPtr<FJsonValue>> Reasons;
		auto Add = [&Reasons](bool bFailed, const TCHAR* Reason)
		{
			if (bFailed)
			{
				Reasons.Add(MakeShared<FJsonValueString>(Reason));
			}
		};
		Add(Modification.sampleMasked, TEXT("sample-mask"));
		Add(Modification.backfaceCulled, TEXT("backface-cull"));
		Add(Modification.depthClipped, TEXT("depth-clip"));
		Add(Modification.depthBoundsFailed, TEXT("depth-bounds"));
		Add(Modification.viewClipped, TEXT("viewport-clip"));
		Add(Modification.scissorClipped, TEXT("scissor-clip"));
		Add(Modification.shaderDiscarded, TEXT("shader-discard"));
		Add(Modification.depthTestFailed, TEXT("depth-test"));
		Add(Modification.stencilTestFailed, TEXT("stencil-test"));
		Add(Modification.predicationSkipped, TEXT("predication-skipped"));
		return Reasons;
	}

	class FReplaySession
	{
	public:
		explicit FReplaySession(TFunction<void(const FString&)> InMessageCallback, bool InFullDiagnostics)
			: MessageCallback(MoveTemp(InMessageCallback))
			, bFullDiagnostics(InFullDiagnostics)
		{
		}

		~FReplaySession()
		{
			Shutdown();
		}

		bool Open(const FString& InCapturePath, const FString& InPreviewPath,
			TFunctionRef<void(const FString& Phase, double ElapsedSeconds)> Progress, FString& OutError)
		{
			#if RENDERTRAIL_REPLAY_WORKER_PROGRAM
			if (GReplayInitialized)
			{
				OutError = TEXT("Replay Worker accepts one capture per process; restart the worker before opening another capture.");
				return false;
			}
			#endif
			const double LoadStartSeconds = FPlatformTime::Seconds();
			SessionStartSeconds = LoadStartSeconds;
			auto Report = [&Progress, LoadStartSeconds](const FString& Phase)
			{
				Progress(Phase, FPlatformTime::Seconds() - LoadStartSeconds);
			};
			CapturePath = FPaths::ConvertRelativePathToFull(InCapturePath);
			PreviewPath = FPaths::ConvertRelativePathToFull(InPreviewPath);
			Report(TEXT("Validating replay inputs"));
			if (!FPaths::FileExists(CapturePath))
			{
				OutError = FString::Printf(TEXT("Capture does not exist: %s"), *CapturePath);
				return false;
			}

			const FString RenderDocDll = TEXT("C:/Program Files/RenderDoc/renderdoc.dll");
			if (!FPaths::FileExists(RenderDocDll))
			{
				OutError = FString::Printf(TEXT("RenderDoc replay DLL was not found: %s"), *RenderDocDll);
				return false;
			}
			Report(TEXT("Loading RenderDoc replay DLL"));
			const double DllLoadStartSeconds = FPlatformTime::Seconds();
			DllHandle = FPlatformProcess::GetDllHandle(*RenderDocDll);
			if (!DllHandle)
			{
				OutError = FString::Printf(TEXT("Failed to load RenderDoc replay DLL: %s"), *RenderDocDll);
				return false;
			}
			const TCHAR* RequiredExports[] = {
				TEXT("RENDERDOC_AllocArrayMem"), TEXT("RENDERDOC_FreeArrayMem"), TEXT("RENDERDOC_GetVersionString"),
				TEXT("RENDERDOC_GetCommitHash"), TEXT("RENDERDOC_InitialiseReplay"), TEXT("RENDERDOC_OpenCaptureFile"),
				TEXT("RENDERDOC_ResourceFormatName"), TEXT("RENDERDOC_ShutdownReplay")};
			for (const TCHAR* Export : RequiredExports)
			{
				if (!FPlatformProcess::GetDllExport(DllHandle, Export))
				{
					OutError = FString::Printf(TEXT("Installed renderdoc.dll is missing replay export %s"), Export);
					return false;
				}
			}
			ResourceFormatName = reinterpret_cast<FResourceFormatName>(
				FPlatformProcess::GetDllExport(DllHandle, TEXT("RENDERDOC_ResourceFormatName")));

			RenderDocVersion = UTF8_TO_TCHAR(RENDERDOC_GetVersionString());
			RenderDocCommit = UTF8_TO_TCHAR(RENDERDOC_GetCommitHash());
			Report(FString::Printf(TEXT("RenderDoc replay DLL loaded: version=%s commit=%s duration=%.3fs"),
				*RenderDocVersion, *RenderDocCommit, FPlatformTime::Seconds() - DllLoadStartSeconds));
			Report(TEXT("Initialising RenderDoc replay runtime and enumerating GPUs"));
			const double ReplayInitStartSeconds = FPlatformTime::Seconds();
			GlobalEnvironment Environment;
			Environment.enumerateGPUs = true;
			rdcarray<rdcstr> ReplayArguments;
			RENDERDOC_InitialiseReplay(Environment, ReplayArguments);
			bReplayInitialized = true;
			#if RENDERTRAIL_REPLAY_WORKER_PROGRAM
			GReplayInitialized = true;
			#endif
			Report(FString::Printf(TEXT("RenderDoc replay runtime initialised: duration=%.3fs"),
				FPlatformTime::Seconds() - ReplayInitStartSeconds));

			Report(TEXT("Creating capture file handle"));
			ICaptureFile* File = RENDERDOC_OpenCaptureFile();
			if (!File)
			{
				OutError = TEXT("RENDERDOC_OpenCaptureFile returned null.");
				return false;
			}

			const int64 CaptureBytes = IFileManager::Get().FileSize(*CapturePath);
			Report(FString::Printf(TEXT("Opening RDC container: bytes=%lld"), CaptureBytes));
			const double OpenFileStartSeconds = FPlatformTime::Seconds();
			const FTCHARToUTF8 CaptureUtf8(*CapturePath);
			const ResultDetails OpenFileResult = File->OpenFile(rdcstr(CaptureUtf8.Get()), "rdc", nullptr);
			if (!OpenFileResult.OK())
			{
				OutError = FString::Printf(TEXT("Could not open capture: %s"), *ResultMessage(OpenFileResult));
				File->Shutdown();
				return false;
			}
			Report(FString::Printf(TEXT("RDC container opened: duration=%.3fs"),
				FPlatformTime::Seconds() - OpenFileStartSeconds));

			// A native-size preview captured by the editor is pixel-addressable and must
			// not be overwritten by RenderDoc's window thumbnail while Replay is opening.
			// Legacy/external captures without that cache still get the embedded thumbnail
			// as a provisional, non-authoritative preview.
			if (IsPixelExactPreviewValid())
			{
				bPreviewCached = true;
				Report(TEXT("Preserving cached native preview while Replay opens"));
			}
			else
			{
				const Thumbnail EmbeddedThumbnail = File->GetThumbnail(FileType::PNG, 8192);
				if (!EmbeddedThumbnail.data.empty() && EmbeddedThumbnail.width > 0 && EmbeddedThumbnail.height > 0)
				{
					TArray64<uint8> ThumbnailBytes;
					ThumbnailBytes.SetNumUninitialized(static_cast<int64>(EmbeddedThumbnail.data.size()));
					FMemory::Memcpy(ThumbnailBytes.GetData(), EmbeddedThumbnail.data.data(), EmbeddedThumbnail.data.size());
					IFileManager::Get().MakeDirectory(*FPaths::GetPath(PreviewPath), true);
					if (FFileHelper::SaveArrayToFile(ThumbnailBytes, *PreviewPath))
					{
						bEmbeddedThumbnailExported = true;
						Report(FString::Printf(TEXT("Fast capture thumbnail exported: %ux%u"),
							EmbeddedThumbnail.width, EmbeddedThumbnail.height));

						const TSharedRef<FJsonObject> PreviewObject = MakeShared<FJsonObject>();
						PreviewObject->SetStringField(TEXT("type"), TEXT("preview"));
						PreviewObject->SetStringField(TEXT("previewPath"), PreviewPath);
						PreviewObject->SetNumberField(TEXT("width"), EmbeddedThumbnail.width);
						PreviewObject->SetNumberField(TEXT("height"), EmbeddedThumbnail.height);
						PreviewObject->SetStringField(TEXT("source"), TEXT("embedded_capture_thumbnail"));
						Emit(PreviewObject);
					}
				}
			}

			Report(TEXT("RenderDoc OpenCapture begin: creating device, restoring resources, and replaying initial events"));
			const double OpenCaptureStartSeconds = FPlatformTime::Seconds();
			FCriticalSection ReplayProgressLock;
			int32 LastProgressBucket = INDEX_NONE;
			double LastProgressReportSeconds = OpenCaptureStartSeconds;
			float LastOverallProgress = 0.0f;
			const RENDERDOC_ProgressCallback ReplayProgress =
				[&Report, &ReplayProgressLock, &LastProgressBucket, &LastProgressReportSeconds, &LastOverallProgress](float Value)
			{
				FScopeLock Lock(&ReplayProgressLock);
				const float Clamped = FMath::Clamp(Value, 0.0f, 1.0f);
				LastOverallProgress = Clamped;
				const int32 Bucket = FMath::FloorToInt(Clamped * 20.0f);
				const double Now = FPlatformTime::Seconds();
				if (LastProgressBucket == INDEX_NONE || Bucket > LastProgressBucket
					|| Bucket < LastProgressBucket || Now - LastProgressReportSeconds >= 10.0)
				{
					Report(FString::Printf(TEXT("RenderDoc OpenCapture progress: %s"),
						*DescribeRenderDocLoadProgress(Clamped)));
					LastProgressBucket = Bucket;
					LastProgressReportSeconds = Now;
				}
			};
			const rdcpair<ResultDetails, IReplayController*> OpenCaptureResult =
				File->OpenCapture(ReplayOptions(), ReplayProgress);
			const double OpenCaptureDuration = FPlatformTime::Seconds() - OpenCaptureStartSeconds;
			Report(FString::Printf(TEXT("RenderDoc OpenCapture returned: success=%s duration=%.3fs lastProgress=%s result=%s"),
				OpenCaptureResult.first.OK() && OpenCaptureResult.second ? TEXT("true") : TEXT("false"),
				OpenCaptureDuration, *DescribeRenderDocLoadProgress(LastOverallProgress),
				*ResultMessage(OpenCaptureResult.first)));
			if (!OpenCaptureResult.first.OK() || !OpenCaptureResult.second)
			{
				OutError = FString::Printf(TEXT("Could not create replay: %s"), *ResultMessage(OpenCaptureResult.first));
				File->Shutdown();
				return false;
			}
			Controller = OpenCaptureResult.second;
			File->Shutdown();
			Report(TEXT("Replay controller created; reading API properties, actions, textures, and resources"));
			const double MetadataStartSeconds = FPlatformTime::Seconds();

			const APIProperties Properties = Controller->GetAPIProperties();
			bPixelHistorySupported = Properties.pixelHistory;
			bShaderDebuggingSupported = Properties.shaderDebugging;

			const rdcarray<ActionDescription>& Actions = Controller->GetRootActions();
			TArray<const ActionDescription*> FlatActions;
			FlattenActions(Actions, FlatActions);
			const ActionDescription* LastAction = FlatActions.IsEmpty() ? nullptr : FlatActions.Last();
			FinalEventId = LastAction ? LastAction->eventId : 0;

			const rdcarray<TextureDescription>& Textures = Controller->GetTextures();
			const rdcarray<ResourceDescription>& Resources = Controller->GetResources();
			Report(FString::Printf(TEXT("Replay metadata read: rootActions=%llu flatActions=%d textures=%llu resources=%llu duration=%.3fs"),
				static_cast<unsigned long long>(Actions.size()), FlatActions.Num(),
				static_cast<unsigned long long>(Textures.size()), static_cast<unsigned long long>(Resources.size()),
				FPlatformTime::Seconds() - MetadataStartSeconds));
			auto IsTexture = [&Textures](ResourceId Candidate)
			{
				for (const TextureDescription& Texture : Textures)
				{
					if (Texture.resourceId == Candidate)
					{
						return true;
					}
				}
				return false;
			};
			auto ResolveTexture = [&Resources, &IsTexture](ResourceId Candidate)
			{
				if (IsTexture(Candidate))
				{
					return Candidate;
				}
				for (const ResourceDescription& Resource : Resources)
				{
					if (Resource.resourceId != Candidate)
					{
						continue;
					}
					for (const ResourceId& Parent : Resource.parentResources)
					{
						if (IsTexture(Parent))
						{
							return Parent;
						}
					}
					for (const ResourceId& Derived : Resource.derivedResources)
					{
						if (IsTexture(Derived))
						{
							return Derived;
						}
					}
				}
				return ResourceId();
			};
			auto TextureArea = [&Textures](ResourceId Candidate) -> uint64
			{
				for (const TextureDescription& Texture : Textures)
				{
					if (Texture.resourceId == Candidate)
					{
						return static_cast<uint64>(Texture.width) * Texture.height;
					}
				}
				return 0;
			};

			// A capture can contain several Present actions when more than one editor,
			// PIE, notification, or auxiliary window rendered during the frame. Prefer
			// the largest actual Present output rather than blindly taking the latest
			// Present, which can be a small warning/notification window.
			uint64 LargestPresentArea = 0;
			uint32 LargestPresentEventId = 0;
			for (int32 Index = FlatActions.Num() - 1; Index >= 0; --Index)
			{
				const ActionDescription& Action = *FlatActions[Index];
				if (!static_cast<bool>(Action.flags & ActionFlags::Present))
				{
					continue;
				}

				ResourceId Candidate = ResolveTexture(Action.copyDestination != ResourceId() ? Action.copyDestination : Action.copySource);
				if (Candidate == ResourceId())
				{
					Candidate = ResolveTexture(FirstOutput(Action));
				}
				const uint64 CandidateArea = TextureArea(Candidate);
				if (Candidate != ResourceId() && CandidateArea >= LargestPresentArea)
				{
					TargetTexture = Candidate;
					LargestPresentArea = CandidateArea;
					LargestPresentEventId = Action.eventId;
				}
			}
			if (TargetTexture != ResourceId())
			{
				FinalEventId = LargestPresentEventId;
			}
			// Some injected captures end before DXGI Present. A largest swap-buffer texture
			// is the next best evidence-backed target in that case; do not select a small
			// auxiliary window merely because it appears first in RenderDoc's resource list.
			if (TargetTexture == ResourceId())
			{
				uint64 LargestSwapBufferArea = 0;
				for (const TextureDescription& Texture : Textures)
				{
					if (static_cast<bool>(Texture.creationFlags & TextureCategory::SwapBuffer))
					{
						const uint64 CandidateArea = static_cast<uint64>(Texture.width) * Texture.height;
						if (CandidateArea >= LargestSwapBufferArea)
						{
							TargetTexture = Texture.resourceId;
							LargestSwapBufferArea = CandidateArea;
						}
					}
				}
			}
			// UE captures can also expose the backbuffer only as the last color output. Walk backwards
			// through real actions and use the most recent output/copy target that is a known texture.
			if (TargetTexture == ResourceId())
			{
				for (int32 Index = FlatActions.Num() - 1; Index >= 0; --Index)
				{
					const ActionDescription& Action = *FlatActions[Index];
					ResourceId Candidate;
					for (const ResourceId& Output : Action.outputs)
					{
						Candidate = ResolveTexture(Output);
						if (Candidate != ResourceId())
						{
							break;
						}
					}
					if (Candidate == ResourceId())
					{
						Candidate = ResolveTexture(Action.copyDestination != ResourceId() ? Action.copyDestination : Action.copySource);
					}
					if (Candidate != ResourceId())
					{
						TargetTexture = Candidate;
						FinalEventId = Action.eventId;
						break;
					}
				}
			}
			// Last-resort capture compatibility: choose the largest color target. This remains a
			// structured RenderDoc resource selection and is surfaced to the UI as a fallback target.
			if (TargetTexture == ResourceId())
			{
				uint64 LargestArea = 0;
				for (const TextureDescription& Texture : Textures)
				{
					const uint64 Area = static_cast<uint64>(Texture.width) * Texture.height;
					if (static_cast<bool>(Texture.creationFlags & TextureCategory::ColorTarget) && Area > LargestArea)
					{
						LargestArea = Area;
						TargetTexture = Texture.resourceId;
					}
				}
			}
			if (TargetTexture == ResourceId())
			{
				OutError = TEXT("No swap-chain/final-present texture was found in the capture.");
				return false;
			}

			for (const TextureDescription& Texture : Textures)
			{
				if (Texture.resourceId == TargetTexture)
				{
					Width = Texture.width;
					Height = Texture.height;
					TargetCompType = Texture.format.compType;
					TargetSamples = FMath::Max(1U, Texture.msSamp);
					rdcstr FormatName;
					if (ResourceFormatName)
					{
						ResourceFormatName(Texture.format, FormatName);
					}
					TargetFormat = FormatName.empty() ? TEXT("unknown") : FromRdcString(FormatName);
					break;
				}
			}
			for (const ResourceDescription& Resource : Resources)
			{
				if (Resource.resourceId == TargetTexture)
				{
					TargetName = FromRdcString(Resource.name);
					TargetResourceIndex = static_cast<int32>(&Resource - Resources.data());
					break;
				}
			}
			if (Width == 0 || Height == 0)
			{
				OutError = TEXT("Final texture has invalid dimensions.");
				return false;
			}
			Report(FString::Printf(TEXT("Target texture identified: %s %ux%u; action labels deferred"),
				TargetName.IsEmpty() ? TEXT("<unnamed>") : *TargetName, Width, Height));

			if (!bEmbeddedThumbnailExported && IsPixelExactPreviewValid())
			{
				bPreviewCached = true;
				Report(TEXT("Native preview remained available during Replay initialisation"));
			}

			if (FinalEventId > 0)
			{
				const double SetEventStartSeconds = FPlatformTime::Seconds();
				Report(FString::Printf(TEXT("SetFrameEvent begin: eventId=%u force=true"), FinalEventId));
				Controller->SetFrameEvent(FinalEventId, true);
				Report(FString::Printf(TEXT("SetFrameEvent complete: eventId=%u duration=%.3fs"),
					FinalEventId, FPlatformTime::Seconds() - SetEventStartSeconds));
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(PreviewPath), true);
			TextureSave Save;
			Save.resourceId = TargetTexture;
			Save.destType = FileType::PNG;
			Save.mip = 0;
			Save.alpha = AlphaMapping::Discard;
			const FTCHARToUTF8 PreviewUtf8(*PreviewPath);
			const double SaveTextureStartSeconds = FPlatformTime::Seconds();
			Report(FString::Printf(TEXT("SaveTexture begin: target=%s %ux%u eventId=%u"),
				TargetName.IsEmpty() ? TEXT("<unnamed>") : *TargetName, Width, Height, FinalEventId));
			const ResultDetails SaveResult = Controller->SaveTexture(Save, rdcstr(PreviewUtf8.Get()));
			Report(FString::Printf(TEXT("SaveTexture returned: success=%s duration=%.3fs result=%s"),
				SaveResult.OK() ? TEXT("true") : TEXT("false"),
				FPlatformTime::Seconds() - SaveTextureStartSeconds, *ResultMessage(SaveResult)));
			if (!SaveResult.OK())
			{
				OutError = FString::Printf(TEXT("Could not export preview: %s"), *ResultMessage(SaveResult));
				return false;
			}
			bPreviewCached = false;
			Report(TEXT("Preview exported"));
			return true;
		}

		void EmitReady() const
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("type"), TEXT("ready"));
			Object->SetStringField(TEXT("capturePath"), CapturePath);
			Object->SetStringField(TEXT("previewPath"), PreviewPath);
			Object->SetNumberField(TEXT("width"), Width);
			Object->SetNumberField(TEXT("height"), Height);
			Object->SetNumberField(TEXT("finalEventId"), FinalEventId);
			Object->SetNumberField(TEXT("targetResourceIndex"), TargetResourceIndex);
			Object->SetNumberField(TEXT("targetSamples"), TargetSamples);
			Object->SetStringField(TEXT("targetName"), TargetName);
			Object->SetStringField(TEXT("targetFormat"), TargetFormat);
			Object->SetStringField(TEXT("renderDocVersion"), RenderDocVersion);
			Object->SetStringField(TEXT("renderDocCommit"), RenderDocCommit);
			Object->SetBoolField(TEXT("pixelHistorySupported"), bPixelHistorySupported);
			Object->SetBoolField(TEXT("shaderDebuggingSupported"), bShaderDebuggingSupported);
			Object->SetBoolField(TEXT("previewCached"), bPreviewCached);
			Object->SetBoolField(TEXT("actionIndexReady"), bActionIndexBuilt);
			Object->SetBoolField(TEXT("fullDiagnostics"), bFullDiagnostics);
			Emit(Object);
		}

		void QueryPixelHistory(uint32 X, uint32 Y, int32 ResourceIndex, uint32 Mip, uint32 Slice,
			uint32 SampleIndex, uint32 BeforeEventId, int32 TypeCastOverride, const FString& RequestId)
		{
			if (!Controller)
			{
				EmitError(TEXT("pixel_history"), TEXT("Replay session is not open."), RequestId);
				return;
			}
			if (!bPixelHistorySupported)
			{
				EmitError(TEXT("pixel_history"), TEXT("This replay API/driver does not support Pixel History."), RequestId);
				return;
			}
			ResourceId TextureId = TargetTexture;
			FString TextureName = TargetName;
			const TextureDescription* TextureDescriptionValue = nullptr;
			CompType TypeCast = TypeCastOverride >= 0
				? static_cast<CompType>(TypeCastOverride) : TargetCompType;
			const rdcarray<ResourceDescription>& Resources = Controller->GetResources();
			const rdcarray<TextureDescription>& Textures = Controller->GetTextures();
			if (ResourceIndex >= 0)
			{
				if (ResourceIndex >= static_cast<int32>(Resources.size()))
				{
					EmitError(TEXT("pixel_history"), FString::Printf(TEXT("Resource index %d is invalid."), ResourceIndex), RequestId);
					return;
				}
				TextureId = Resources[ResourceIndex].resourceId;
				TextureName = FromRdcString(Resources[ResourceIndex].name);
			}
			else
			{
				ResourceIndex = TargetResourceIndex;
			}
			for (const TextureDescription& Texture : Textures)
			{
				if (Texture.resourceId == TextureId)
				{
					TextureDescriptionValue = &Texture;
					if (TypeCastOverride < 0)
					{
						TypeCast = Texture.format.compType;
					}
					break;
				}
			}
			if (!TextureDescriptionValue)
			{
				EmitError(TEXT("pixel_history"), FString::Printf(TEXT("Resource %d is not a texture."), ResourceIndex), RequestId);
				return;
			}
			if (Mip >= TextureDescriptionValue->mips || Slice >= TextureDescriptionValue->arraysize
				|| SampleIndex >= FMath::Max(1U, TextureDescriptionValue->msSamp))
			{
				EmitError(TEXT("pixel_history"), FString::Printf(
					TEXT("Subresource mip=%u slice=%u sample=%u is invalid for resource %d (mips=%u slices=%u samples=%u)."),
					Mip, Slice, SampleIndex, ResourceIndex, TextureDescriptionValue->mips,
					TextureDescriptionValue->arraysize, FMath::Max(1U, TextureDescriptionValue->msSamp)), RequestId);
				return;
			}
			const uint32 QueryWidth = FMath::Max(1U, TextureDescriptionValue->width >> Mip);
			const uint32 QueryHeight = FMath::Max(1U, TextureDescriptionValue->height >> Mip);
			if (X >= QueryWidth || Y >= QueryHeight)
			{
				EmitError(TEXT("pixel_history"), FString::Printf(TEXT("Pixel (%u, %u) is outside resource %d size %ux%u."),
					X, Y, ResourceIndex, QueryWidth, QueryHeight), RequestId);
				return;
			}
			FString ActionIndexError;
			if (!EnsureActionIndex(ActionIndexError))
			{
				EmitError(TEXT("action_index"), ActionIndexError, RequestId);
				return;
			}

			const double QueryStartSeconds = FPlatformTime::Seconds();
			EmitDiagnostic(TEXT("pixel_history"), TEXT("begin"), QueryStartSeconds,
				FString::Printf(TEXT("requestId=%s resource=%d name=%s pixel=(%u,%u) sub=(%u,%u,%u) beforeEvent=%u"),
					*RequestId, ResourceIndex, TextureName.IsEmpty() ? TEXT("<unnamed>") : *TextureName,
					X, Y, Mip, Slice, SampleIndex, BeforeEventId), RequestId);
			const double ReplayCallStartSeconds = FPlatformTime::Seconds();
			EmitDiagnostic(TEXT("pixel_history.replay"), TEXT("begin"), ReplayCallStartSeconds,
				TEXT("Calling IReplayController::PixelHistory"), RequestId);
			const rdcarray<PixelModification> Modifications = Controller->PixelHistory(
				TextureId, X, Y, Subresource(Mip, Slice, SampleIndex), TypeCast);
			TArray<const PixelModification*> RelevantModifications;
			RelevantModifications.Reserve(static_cast<int32>(Modifications.size()));
			for (const PixelModification& Modification : Modifications)
			{
				if (BeforeEventId == 0 || Modification.eventId < BeforeEventId)
				{
					RelevantModifications.Add(&Modification);
				}
			}
			const int32 RawTotal = static_cast<int32>(Modifications.size());
			const int32 Total = RelevantModifications.Num();
			EmitDiagnostic(TEXT("pixel_history.replay"), TEXT("end"), ReplayCallStartSeconds,
				FString::Printf(TEXT("rawModifications=%d relevantModifications=%d"), RawTotal, Total), RequestId);
			const int32 First = bFullDiagnostics ? 0 : FMath::Max(0, Total - MaxSerializedPixelModifications);
			TArray<TSharedPtr<FJsonValue>> JsonModifications;
			JsonModifications.Reserve(Total - First);
			TMap<uint32, TSharedPtr<FJsonObject>> EventSummaryById;
			TArray<uint32> EventSummaryOrder;
			for (int32 Index = 0; Index < Total; ++Index)
			{
				const PixelModification& Modification = *RelevantModifications[Index];
				TSharedPtr<FJsonObject>* ExistingSummary = EventSummaryById.Find(Modification.eventId);
				if (!ExistingSummary)
				{
					const TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
					Summary->SetNumberField(TEXT("eventId"), Modification.eventId);
					Summary->SetStringField(TEXT("action"), ActionNames.FindRef(Modification.eventId));
					Summary->SetStringField(TEXT("actionKind"), ActionKinds.FindRef(Modification.eventId));
					Summary->SetStringField(TEXT("markerPath"), ActionPaths.FindRef(Modification.eventId));
					Summary->SetNumberField(TEXT("actionFlags"), ActionFlagsByEvent.FindRef(Modification.eventId));
					Summary->SetNumberField(TEXT("passedFragments"), 0);
					Summary->SetNumberField(TEXT("rejectedFragments"), 0);
					Summary->SetBoolField(TEXT("directShaderWrite"), false);
					Summary->SetBoolField(TEXT("unboundPixelShader"), false);
					Summary->SetBoolField(TEXT("changedTextureValue"), false);
					Summary->SetBoolField(TEXT("hasPrimitiveEvidence"), false);
					Summary->SetObjectField(TEXT("firstBefore"), ModificationValueToJson(Modification.preMod));
					TArray<TSharedPtr<FJsonValue>> EmptyFailureReasons;
					Summary->SetArrayField(TEXT("failureReasons"), MoveTemp(EmptyFailureReasons));
					EventSummaryOrder.Add(Modification.eventId);
					EventSummaryById.Add(Modification.eventId, Summary);
					ExistingSummary = EventSummaryById.Find(Modification.eventId);
				}

				const TSharedPtr<FJsonObject> Summary = *ExistingSummary;
				const bool bPassed = GetFailureReasons(Modification).IsEmpty();
				Summary->SetNumberField(TEXT("passedFragments"), Summary->GetNumberField(TEXT("passedFragments")) + (bPassed ? 1 : 0));
				Summary->SetNumberField(TEXT("rejectedFragments"), Summary->GetNumberField(TEXT("rejectedFragments")) + (bPassed ? 0 : 1));
				Summary->SetBoolField(TEXT("directShaderWrite"), Summary->GetBoolField(TEXT("directShaderWrite")) || Modification.directShaderWrite);
				Summary->SetBoolField(TEXT("unboundPixelShader"), Summary->GetBoolField(TEXT("unboundPixelShader")) || Modification.unboundPS);
				Summary->SetBoolField(TEXT("changedTextureValue"), Summary->GetBoolField(TEXT("changedTextureValue")) || !(Modification.preMod == Modification.postMod));
				Summary->SetBoolField(TEXT("hasPrimitiveEvidence"), true);
				Summary->SetObjectField(TEXT("lastShaderOutput"), ModificationValueToJson(Modification.shaderOut));
				Summary->SetObjectField(TEXT("lastAfter"), ModificationValueToJson(Modification.postMod));
				Summary->SetNumberField(TEXT("lastPrimitiveId"), Modification.primitiveID);
				Summary->SetNumberField(TEXT("lastFragmentIndex"), Modification.fragIndex);

				TArray<TSharedPtr<FJsonValue>> FailureReasons = Summary->GetArrayField(TEXT("failureReasons"));
				for (const TSharedPtr<FJsonValue>& Failure : GetFailureReasons(Modification))
				{
					const FString FailureText = Failure->AsString();
					bool bAlreadyPresent = false;
					for (const TSharedPtr<FJsonValue>& ExistingFailure : FailureReasons)
					{
						if (ExistingFailure.IsValid() && ExistingFailure->AsString() == FailureText)
						{
							bAlreadyPresent = true;
							break;
						}
					}
					if (!bAlreadyPresent)
					{
						FailureReasons.Add(MakeShared<FJsonValueString>(FailureText));
					}
				}
				Summary->SetArrayField(TEXT("failureReasons"), MoveTemp(FailureReasons));
			}
			TArray<TSharedPtr<FJsonValue>> JsonEventSummaries;
			JsonEventSummaries.Reserve(EventSummaryOrder.Num());
			for (const uint32 EventId : EventSummaryOrder)
			{
				if (const TSharedPtr<FJsonObject>* Summary = EventSummaryById.Find(EventId))
				{
					if (Summary->IsValid())
					{
						JsonEventSummaries.Add(MakeShared<FJsonValueObject>((*Summary).ToSharedRef()));
					}
				}
			}
			for (int32 Index = First; Index < Total; ++Index)
			{
				const PixelModification& Modification = *RelevantModifications[Index];
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("eventId"), Modification.eventId);
				Item->SetStringField(TEXT("action"), ActionNames.FindRef(Modification.eventId));
				Item->SetStringField(TEXT("actionKind"), ActionKinds.FindRef(Modification.eventId));
				Item->SetStringField(TEXT("markerPath"), ActionPaths.FindRef(Modification.eventId));
				Item->SetNumberField(TEXT("actionFlags"), ActionFlagsByEvent.FindRef(Modification.eventId));
				Item->SetBoolField(TEXT("directShaderWrite"), Modification.directShaderWrite);
				Item->SetBoolField(TEXT("unboundPixelShader"), Modification.unboundPS);
				Item->SetBoolField(TEXT("changedTextureValue"), !(Modification.preMod == Modification.postMod));
				Item->SetNumberField(TEXT("fragmentIndex"), Modification.fragIndex);
				Item->SetNumberField(TEXT("primitiveId"), Modification.primitiveID);
				Item->SetObjectField(TEXT("before"), ModificationValueToJson(Modification.preMod));
				Item->SetObjectField(TEXT("shaderOutput"), ModificationValueToJson(Modification.shaderOut));
				Item->SetObjectField(TEXT("after"), ModificationValueToJson(Modification.postMod));
				TArray<TSharedPtr<FJsonValue>> FailureReasons = GetFailureReasons(Modification);
				Item->SetBoolField(TEXT("passed"), FailureReasons.IsEmpty());
				Item->SetArrayField(TEXT("failureReasons"), MoveTemp(FailureReasons));
				JsonModifications.Add(MakeShared<FJsonValueObject>(Item));
			}

			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("type"), TEXT("pixel_history"));
			Object->SetStringField(TEXT("requestId"), RequestId);
			Object->SetNumberField(TEXT("x"), X);
			Object->SetNumberField(TEXT("y"), Y);
			Object->SetNumberField(TEXT("resourceIndex"), ResourceIndex);
			Object->SetStringField(TEXT("resourceName"), TextureName);
			Object->SetNumberField(TEXT("mip"), Mip);
			Object->SetNumberField(TEXT("slice"), Slice);
			Object->SetNumberField(TEXT("sample"), SampleIndex);
			Object->SetNumberField(TEXT("beforeEventId"), BeforeEventId);
			Object->SetNumberField(TEXT("rawModifications"), RawTotal);
			Object->SetNumberField(TEXT("totalModifications"), Total);
			Object->SetNumberField(TEXT("returnedModifications"), Total - First);
			Object->SetBoolField(TEXT("truncated"), First > 0);
			Object->SetBoolField(TEXT("fullDiagnostics"), bFullDiagnostics);
			Object->SetNumberField(TEXT("totalEvents"), EventSummaryOrder.Num());
			Object->SetArrayField(TEXT("eventSummaries"), MoveTemp(JsonEventSummaries));
			Object->SetArrayField(TEXT("modifications"), MoveTemp(JsonModifications));
			EmitDiagnostic(TEXT("pixel_history"), TEXT("end"), QueryStartSeconds,
				FString::Printf(TEXT("events=%d returnedModifications=%d"), EventSummaryOrder.Num(), Total - First), RequestId);
			Emit(Object);
		}

		void QueryEventContext(uint32 EventId, const FString& RequestId)
		{
			if (!Controller)
			{
				EmitError(TEXT("event_context"), TEXT("Replay session is not open."), RequestId);
				return;
			}
			FString ActionIndexError;
			if (!EnsureActionIndex(ActionIndexError))
			{
				EmitError(TEXT("action_index"), ActionIndexError, RequestId);
				return;
			}
			if (!ActionNames.Contains(EventId))
			{
				EmitError(TEXT("event_context"), FString::Printf(TEXT("Event %u is not a GPU action."), EventId), RequestId);
				return;
			}

			const double QueryStartSeconds = FPlatformTime::Seconds();
			EmitDiagnostic(TEXT("event_context"), TEXT("begin"), QueryStartSeconds,
				FString::Printf(TEXT("requestId=%s eventId=%u"), *RequestId, EventId), RequestId);
			const double SetEventStartSeconds = FPlatformTime::Seconds();
			EmitDiagnostic(TEXT("event_context.set_frame_event"), TEXT("begin"), SetEventStartSeconds,
				FString::Printf(TEXT("eventId=%u force=true"), EventId), RequestId);
			Controller->SetFrameEvent(EventId, true);
			EmitDiagnostic(TEXT("event_context.set_frame_event"), TEXT("end"), SetEventStartSeconds,
				FString::Printf(TEXT("eventId=%u"), EventId), RequestId);
			const double CollectStartSeconds = FPlatformTime::Seconds();
			EmitDiagnostic(TEXT("event_context.collect"), TEXT("begin"), CollectStartSeconds,
				TEXT("Collecting descriptors, resources, provenance, pipeline, and shader reflection"), RequestId);
			const FString Kind = ActionKinds.FindRef(EventId);
			const ShaderStage Stage = Kind == TEXT("dispatch") ? ShaderStage::Compute : ShaderStage::Pixel;
			const TCHAR* StageName = Stage == ShaderStage::Compute ? TEXT("compute") : TEXT("pixel");
			const rdcarray<ResourceDescription>& Resources = Controller->GetResources();
			const rdcarray<TextureDescription>& Textures = Controller->GetTextures();
			const ShaderReflection* BindingShader = nullptr;
			if (const D3D12Pipe::State* D3D12State = Controller->GetD3D12PipelineState())
			{
				BindingShader = Stage == ShaderStage::Compute
					? D3D12State->computeShader.reflection : D3D12State->pixelShader.reflection;
			}
			else if (const D3D11Pipe::State* D3D11State = Controller->GetD3D11PipelineState())
			{
				BindingShader = Stage == ShaderStage::Compute
					? D3D11State->computeShader.reflection : D3D11State->pixelShader.reflection;
			}
			else if (const GLPipe::State* GLState = Controller->GetGLPipelineState())
			{
				BindingShader = Stage == ShaderStage::Compute
					? GLState->computeShader.reflection : GLState->fragmentShader.reflection;
			}
			else if (const VKPipe::State* VulkanState = Controller->GetVulkanPipelineState())
			{
				BindingShader = Stage == ShaderStage::Compute
					? VulkanState->computeShader.reflection : VulkanState->fragmentShader.reflection;
			}

			auto ResourceToJson = [this, &Resources, &Textures](ResourceId Id, const FString& StageText, const FString& Access)
			{
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				FString Name;
				int32 ResourceIndex = INDEX_NONE;
				for (int32 Index = 0; Index < static_cast<int32>(Resources.size()); ++Index)
				{
					const ResourceDescription& Resource = Resources[Index];
					if (Resource.resourceId == Id)
					{
						Name = FromRdcString(Resource.name);
						ResourceIndex = Index;
						break;
					}
				}
				Item->SetNumberField(TEXT("resourceIndex"), ResourceIndex);
				Item->SetStringField(TEXT("name"), Name.IsEmpty() ? TEXT("Unnamed resource") : Name);
				Item->SetStringField(TEXT("stage"), StageText);
				Item->SetStringField(TEXT("access"), Access);
				bool bTexture = false;
				for (const TextureDescription& Texture : Textures)
				{
					if (Texture.resourceId == Id)
					{
						bTexture = true;
						rdcstr FormatName;
						if (ResourceFormatName)
						{
							ResourceFormatName(Texture.format, FormatName);
						}
						Item->SetStringField(TEXT("format"),
							FormatName.empty() ? TEXT("unknown") : FromRdcString(FormatName));
						Item->SetNumberField(TEXT("width"), Texture.width);
						Item->SetNumberField(TEXT("height"), Texture.height);
						Item->SetNumberField(TEXT("depth"), Texture.depth);
						Item->SetNumberField(TEXT("mips"), Texture.mips);
						Item->SetNumberField(TEXT("samples"), Texture.msSamp);
						break;
					}
				}
				Item->SetBoolField(TEXT("texture"), bTexture);
				return Item;
			};

			TArray<TSharedPtr<FJsonValue>> Inputs;
			TArray<ResourceId> InputResources;
			TArray<FString> InputShaderBindings;
			for (const DescriptorAccess& Access : Controller->GetDescriptorAccess())
			{
				if (Access.stage != Stage || Access.staticallyUnused
					|| (!IsReadOnlyDescriptor(Access.type) && !IsReadWriteDescriptor(Access.type)))
				{
					continue;
				}
				rdcarray<DescriptorRange> Ranges;
				Ranges.push_back(DescriptorRange(Access));
				const rdcarray<Descriptor> Descriptors = Controller->GetDescriptors(Access.descriptorStore, Ranges);
				if (Descriptors.empty())
				{
					continue;
				}
				const Descriptor& DescriptorValue = Descriptors[0];
				const ResourceId Resource = DescriptorValue.resource;
				if (Resource == ResourceId()
					|| (!bFullDiagnostics && Inputs.Num() >= MaxSerializedEventResources))
				{
					continue;
				}

				FString ShaderBinding;
				if (BindingShader && Access.index != DescriptorAccess::NoShaderBinding)
				{
					if (IsReadWriteDescriptor(Access.type)
						&& Access.index < BindingShader->readWriteResources.size())
					{
						ShaderBinding = FromRdcString(BindingShader->readWriteResources[Access.index].name);
					}
					else if (Access.index < BindingShader->readOnlyResources.size())
					{
						ShaderBinding = FromRdcString(BindingShader->readOnlyResources[Access.index].name);
					}
				}

				const TSharedRef<FJsonObject> Input = ResourceToJson(Resource, StageName,
					IsReadWriteDescriptor(Access.type) ? TEXT("read-write") : TEXT("read"));
				Input->SetStringField(TEXT("shaderBinding"), ShaderBinding);
				Input->SetNumberField(TEXT("bindingIndex"), Access.index);
				Input->SetNumberField(TEXT("arrayElement"), Access.arrayElement);
				Input->SetNumberField(TEXT("firstMip"), DescriptorValue.firstMip);
				Input->SetNumberField(TEXT("firstSlice"), DescriptorValue.firstSlice);
				Input->SetNumberField(TEXT("typeCast"), static_cast<int32>(DescriptorValue.format.compType));
				rdcstr DescriptorFormatName;
				if (ResourceFormatName)
				{
					ResourceFormatName(DescriptorValue.format, DescriptorFormatName);
				}
				Input->SetStringField(TEXT("descriptorFormat"), DescriptorFormatName.empty()
					? TEXT("unknown") : FromRdcString(DescriptorFormatName));
				Inputs.Add(MakeShared<FJsonValueObject>(Input));
				InputResources.Add(Resource);
				InputShaderBindings.Add(ShaderBinding);
			}

			TArray<TSharedPtr<FJsonValue>> Outputs;
			TArray<ResourceId> SeenOutputs;
			TArray<const ActionDescription*> FlatActions;
			FlattenActions(Controller->GetRootActions(), FlatActions);
			const ActionDescription* SelectedAction = nullptr;
			for (int32 ActionIndex = 0; ActionIndex < FlatActions.Num(); ++ActionIndex)
			{
				const ActionDescription* Action = FlatActions[ActionIndex];
				if (Action && Action->eventId == EventId)
				{
					SelectedAction = Action;
					break;
				}
			}
			if (SelectedAction)
			{
				for (const ResourceId& Output : SelectedAction->outputs)
				{
					if (Output != ResourceId() && !SeenOutputs.Contains(Output)
						&& (bFullDiagnostics || Outputs.Num() < MaxSerializedEventResources))
					{
						SeenOutputs.Add(Output);
						Outputs.Add(MakeShared<FJsonValueObject>(ResourceToJson(Output, TEXT("output-merger"), TEXT("color-write"))));
					}
				}
				if (SelectedAction->depthOut != ResourceId() && !SeenOutputs.Contains(SelectedAction->depthOut)
					&& (bFullDiagnostics || Outputs.Num() < MaxSerializedEventResources))
				{
					Outputs.Add(MakeShared<FJsonValueObject>(ResourceToJson(SelectedAction->depthOut, TEXT("output-merger"), TEXT("depth-stencil"))));
				}
			}

			TArray<TSharedPtr<FJsonValue>> ResourceProvenance;
			for (int32 InputIndex = 0; InputIndex < InputResources.Num(); ++InputIndex)
			{
				const ResourceId& InputResource = InputResources[InputIndex];
				const FString& ShaderBinding = InputShaderBindings[InputIndex];
				FString ResourceName;
				int32 ResourceIndex = INDEX_NONE;
				for (int32 Index = 0; Index < static_cast<int32>(Resources.size()); ++Index)
				{
					const ResourceDescription& Resource = Resources[Index];
					if (Resource.resourceId == InputResource)
					{
						ResourceName = FromRdcString(Resource.name);
						ResourceIndex = Index;
						break;
					}
				}

				const TextureDescription* InputTexture = nullptr;
				const TextureDescription* OutputTexture = nullptr;
				for (const TextureDescription& Texture : Textures)
				{
					if (Texture.resourceId == InputResource)
					{
						InputTexture = &Texture;
					}
					if (SelectedAction)
					{
						for (const ResourceId& OutputResource : SelectedAction->outputs)
						{
							if (Texture.resourceId == OutputResource)
							{
								OutputTexture = &Texture;
								break;
							}
						}
					}
				}

				ResourceProvenance.Add(MakeShared<FJsonValueObject>(BuildResourceProvenance(
					*Controller, InputResource, EventId, ResourceName, ShaderBinding, ResourceIndex, InputTexture, OutputTexture,
					ActionNames, ActionKinds, ActionPaths)));
			}

			bool bShaderDebuggable = false;
			bool bSourceDebugInfo = false;
			FString ShaderEntry;
			FString ShaderDebugStatus;
			FString ShaderEncoding;
			int32 ShaderInputSignatureCount = 0;
			int32 ShaderOutputSignatureCount = 0;
			int32 ShaderConstantBlockCount = 0;
			int32 ShaderSamplerCount = 0;
			int32 ShaderReadOnlyResourceCount = 0;
			int32 ShaderReadWriteResourceCount = 0;
			TSharedPtr<FJsonObject> PipelineState = MakeShared<FJsonObject>();
			PipelineState->SetBoolField(TEXT("fixedFunctionCaptured"), false);
			const ShaderReflection* Shader = nullptr;
			if (const D3D12Pipe::State* D3D12State = Controller->GetD3D12PipelineState())
			{
				PipelineState = BuildD3D12PipelineState(*D3D12State);
				PipelineState->SetBoolField(TEXT("fixedFunctionCaptured"), true);
				Shader = Stage == ShaderStage::Compute ? D3D12State->computeShader.reflection : D3D12State->pixelShader.reflection;
			}
			else if (const D3D11Pipe::State* D3D11State = Controller->GetD3D11PipelineState())
			{
				PipelineState->SetStringField(TEXT("api"), TEXT("D3D11"));
				PipelineState->SetStringField(TEXT("captureNote"), TEXT("D3D11 fixed-function snapshot is not serialized yet."));
				Shader = Stage == ShaderStage::Compute ? D3D11State->computeShader.reflection : D3D11State->pixelShader.reflection;
			}
			else if (const GLPipe::State* GLState = Controller->GetGLPipelineState())
			{
				PipelineState->SetStringField(TEXT("api"), TEXT("OpenGL"));
				PipelineState->SetStringField(TEXT("captureNote"), TEXT("OpenGL fixed-function snapshot is not serialized yet."));
				Shader = Stage == ShaderStage::Compute ? GLState->computeShader.reflection : GLState->fragmentShader.reflection;
			}
			else if (const VKPipe::State* VulkanState = Controller->GetVulkanPipelineState())
			{
				PipelineState->SetStringField(TEXT("api"), TEXT("Vulkan"));
				PipelineState->SetStringField(TEXT("captureNote"), TEXT("Vulkan fixed-function snapshot is not serialized yet."));
				Shader = Stage == ShaderStage::Compute ? VulkanState->computeShader.reflection : VulkanState->fragmentShader.reflection;
			}
			if (Shader)
			{
				bShaderDebuggable = Shader->debugInfo.debuggable;
				bSourceDebugInfo = Shader->debugInfo.sourceDebugInformation;
				ShaderEntry = FromRdcString(Shader->entryPoint);
				ShaderDebugStatus = FromRdcString(Shader->debugInfo.debugStatus);
				ShaderEncoding = RenderDocEnumToString(Shader->encoding);
				ShaderInputSignatureCount = static_cast<int32>(Shader->inputSignature.size());
				ShaderOutputSignatureCount = static_cast<int32>(Shader->outputSignature.size());
				ShaderConstantBlockCount = static_cast<int32>(Shader->constantBlocks.size());
				ShaderSamplerCount = static_cast<int32>(Shader->samplers.size());
				ShaderReadOnlyResourceCount = static_cast<int32>(Shader->readOnlyResources.size());
				ShaderReadWriteResourceCount = static_cast<int32>(Shader->readWriteResources.size());
			}

			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("type"), TEXT("event_context"));
			Object->SetStringField(TEXT("requestId"), RequestId);
			Object->SetNumberField(TEXT("eventId"), EventId);
			Object->SetStringField(TEXT("action"), ActionNames.FindRef(EventId));
			Object->SetStringField(TEXT("actionKind"), Kind);
			Object->SetStringField(TEXT("markerPath"), ActionPaths.FindRef(EventId));
			Object->SetStringField(TEXT("shaderStage"), StageName);
			Object->SetStringField(TEXT("shaderEntry"), ShaderEntry);
			Object->SetBoolField(TEXT("shaderDebuggable"), bShaderDebuggable);
			Object->SetBoolField(TEXT("sourceDebugInfo"), bSourceDebugInfo);
			Object->SetStringField(TEXT("shaderDebugStatus"), ShaderDebugStatus);
			Object->SetStringField(TEXT("shaderEncoding"), ShaderEncoding);
			Object->SetNumberField(TEXT("shaderInputSignatureCount"), ShaderInputSignatureCount);
			Object->SetNumberField(TEXT("shaderOutputSignatureCount"), ShaderOutputSignatureCount);
			Object->SetNumberField(TEXT("shaderConstantBlockCount"), ShaderConstantBlockCount);
			Object->SetNumberField(TEXT("shaderSamplerCount"), ShaderSamplerCount);
			Object->SetNumberField(TEXT("shaderReadOnlyResourceCount"), ShaderReadOnlyResourceCount);
			Object->SetNumberField(TEXT("shaderReadWriteResourceCount"), ShaderReadWriteResourceCount);
			Object->SetBoolField(TEXT("fullDiagnostics"), bFullDiagnostics);
			Object->SetArrayField(TEXT("inputs"), MoveTemp(Inputs));
			Object->SetArrayField(TEXT("outputs"), MoveTemp(Outputs));
			Object->SetArrayField(TEXT("resourceProvenance"), MoveTemp(ResourceProvenance));
			Object->SetObjectField(TEXT("pipelineState"), PipelineState);
			EmitDiagnostic(TEXT("event_context.collect"), TEXT("end"), CollectStartSeconds,
				FString::Printf(TEXT("inputs=%d outputs=%d provenance=%d"),
					InputResources.Num(), SeenOutputs.Num(), Object->GetArrayField(TEXT("resourceProvenance")).Num()), RequestId);
			Emit(Object);

			if (FinalEventId > 0)
			{
				const double RestoreStartSeconds = FPlatformTime::Seconds();
				EmitDiagnostic(TEXT("event_context.restore_frame_event"), TEXT("begin"), RestoreStartSeconds,
					FString::Printf(TEXT("eventId=%u force=true"), FinalEventId), RequestId);
				Controller->SetFrameEvent(FinalEventId, true);
				EmitDiagnostic(TEXT("event_context.restore_frame_event"), TEXT("end"), RestoreStartSeconds,
					FString::Printf(TEXT("eventId=%u"), FinalEventId), RequestId);
			}
			EmitDiagnostic(TEXT("event_context"), TEXT("end"), QueryStartSeconds,
				FString::Printf(TEXT("eventId=%u"), EventId), RequestId);
		}

		void QueryShaderDebug(uint32 EventId, uint32 X, uint32 Y, int32 SampleIndex, uint32 PrimitiveId, bool bHasPrimitive,
			const FString& RequestId)
		{
			if (!Controller)
			{
				EmitError(TEXT("shader_debug"), TEXT("Replay session is not open."), RequestId);
				return;
			}
			if (!bShaderDebuggingSupported)
			{
				EmitError(TEXT("shader_debug"), TEXT("This replay API/driver does not support shader debugging."), RequestId);
				return;
			}
			FString ActionIndexError;
			if (!EnsureActionIndex(ActionIndexError))
			{
				EmitError(TEXT("action_index"), ActionIndexError, RequestId);
				return;
			}
			if (!ActionNames.Contains(EventId) || ActionKinds.FindRef(EventId) != TEXT("draw"))
			{
				EmitError(TEXT("shader_debug"), TEXT("shader_debug requires a draw event."), RequestId);
				return;
			}
			if (X >= Width || Y >= Height)
			{
				EmitError(TEXT("shader_debug"), FString::Printf(TEXT("Pixel (%u, %u) is outside %ux%u."), X, Y, Width, Height), RequestId);
				return;
			}

			const double QueryStartSeconds = FPlatformTime::Seconds();
			EmitDiagnostic(TEXT("shader_debug"), TEXT("begin"), QueryStartSeconds,
				FString::Printf(TEXT("requestId=%s eventId=%u pixel=(%u,%u) sample=%d primitive=%s%u"),
					*RequestId, EventId, X, Y, SampleIndex,
					bHasPrimitive ? TEXT("") : TEXT("unselected:"), PrimitiveId), RequestId);
			const double SetEventStartSeconds = FPlatformTime::Seconds();
			EmitDiagnostic(TEXT("shader_debug.set_frame_event"), TEXT("begin"), SetEventStartSeconds,
				FString::Printf(TEXT("eventId=%u force=true"), EventId), RequestId);
			Controller->SetFrameEvent(EventId, true);
			EmitDiagnostic(TEXT("shader_debug.set_frame_event"), TEXT("end"), SetEventStartSeconds,
				FString::Printf(TEXT("eventId=%u"), EventId), RequestId);
			const ShaderReflection* DebugShader = nullptr;
			ResourceId DebugPipeline;
			if (const D3D12Pipe::State* D3D12State = Controller->GetD3D12PipelineState())
			{
				DebugShader = D3D12State->pixelShader.reflection;
				DebugPipeline = D3D12State->pipelineResourceId;
			}
			else if (const D3D11Pipe::State* D3D11State = Controller->GetD3D11PipelineState())
			{
				DebugShader = D3D11State->pixelShader.reflection;
			}
			const double DisassembleStartSeconds = FPlatformTime::Seconds();
			EmitDiagnostic(TEXT("shader_debug.disassemble"), TEXT("begin"), DisassembleStartSeconds,
				TEXT("Calling IReplayController::DisassembleShader"), RequestId);
			const FString ShaderDisassembly = DebugShader
				? FromRdcString(Controller->DisassembleShader(DebugPipeline, DebugShader, rdcstr()))
				: FString();
			EmitDiagnostic(TEXT("shader_debug.disassemble"), TEXT("end"), DisassembleStartSeconds,
				FString::Printf(TEXT("characters=%d shaderAvailable=%s"), ShaderDisassembly.Len(), DebugShader ? TEXT("true") : TEXT("false")), RequestId);
			DebugPixelInputs Inputs;
			if (SampleIndex >= 0)
			{
				Inputs.sample = static_cast<uint32>(SampleIndex);
			}
			if (bHasPrimitive)
			{
				Inputs.primitive = PrimitiveId;
			}
			const double DebugPixelStartSeconds = FPlatformTime::Seconds();
			EmitDiagnostic(TEXT("shader_debug.debug_pixel"), TEXT("begin"), DebugPixelStartSeconds,
				TEXT("Calling IReplayController::DebugPixel"), RequestId);
			ShaderDebugTrace* Trace = Controller->DebugPixel(X, Y, Inputs);
			EmitDiagnostic(TEXT("shader_debug.debug_pixel"), Trace ? TEXT("end") : TEXT("error"), DebugPixelStartSeconds,
				Trace ? TEXT("traceCreated=true") : TEXT("traceCreated=false"), RequestId);
			if (!Trace)
			{
				EmitError(TEXT("shader_debug"), TEXT("RenderDoc could not create a pixel shader debug trace for this event/pixel."), RequestId);
				if (FinalEventId > 0)
				{
					Controller->SetFrameEvent(FinalEventId, true);
				}
				return;
			}

			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("type"), TEXT("shader_debug"));
			Object->SetStringField(TEXT("requestId"), RequestId);
			Object->SetNumberField(TEXT("eventId"), EventId);
			Object->SetNumberField(TEXT("x"), X);
			Object->SetNumberField(TEXT("y"), Y);
			Object->SetNumberField(TEXT("sample"), SampleIndex);
			Object->SetNumberField(TEXT("primitiveId"), PrimitiveId);
			Object->SetBoolField(TEXT("primitiveSelected"), bHasPrimitive);
			Object->SetStringField(TEXT("stage"), RenderDocEnumToString(Trace->stage));
			Object->SetNumberField(TEXT("instructionInfoCount"), static_cast<double>(Trace->instInfo.size()));
			Object->SetNumberField(TEXT("sourceVariableMappingCount"), static_cast<double>(Trace->sourceVars.size()));
			Object->SetNumberField(TEXT("inputCount"), static_cast<double>(Trace->inputs.size()));
			Object->SetNumberField(TEXT("constantBlockCount"), static_cast<double>(Trace->constantBlocks.size()));
			Object->SetNumberField(TEXT("readOnlyResourceCount"), static_cast<double>(Trace->readOnlyResources.size()));
			Object->SetNumberField(TEXT("readWriteResourceCount"), static_cast<double>(Trace->readWriteResources.size()));
			Object->SetBoolField(TEXT("fullDiagnostics"), bFullDiagnostics);

			TFunction<TSharedRef<FJsonObject>(const ShaderVariable&, int32)> SerializeShaderVariable;
			SerializeShaderVariable = [&SerializeShaderVariable](const ShaderVariable& Variable, int32 Depth)
			{
				const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetStringField(TEXT("name"), FromRdcString(Variable.name));
				Json->SetStringField(TEXT("type"), RenderDocEnumToString(Variable.type));
				Json->SetNumberField(TEXT("rows"), Variable.rows);
				Json->SetNumberField(TEXT("columns"), Variable.columns);
				Json->SetNumberField(TEXT("memberCount"), Variable.members.size());
				const int32 ValueCount = FMath::Clamp(static_cast<int32>(Variable.rows) * static_cast<int32>(Variable.columns), 0, 16);
				TArray<TSharedPtr<FJsonValue>> Values;
				auto AddNumber = [&Values](double Value)
				{
					if (FMath::IsFinite(Value))
					{
						Values.Add(MakeShared<FJsonValueNumber>(Value));
					}
					else
					{
						Values.Add(MakeShared<FJsonValueNull>());
					}
				};
				for (int32 Index = 0; Index < ValueCount; ++Index)
				{
					switch (Variable.type)
					{
					case VarType::Float:
						AddNumber(Variable.value.f32v[Index]);
						break;
					case VarType::Double:
						AddNumber(Variable.value.f64v[Index]);
						break;
					case VarType::Half:
						AddNumber(static_cast<float>(Variable.value.f16v[Index]));
						break;
					case VarType::SInt:
						AddNumber(Variable.value.s32v[Index]);
						break;
					case VarType::UInt:
					case VarType::Enum:
					case VarType::Bool:
						AddNumber(Variable.value.u32v[Index]);
						break;
					case VarType::SShort:
						AddNumber(Variable.value.s16v[Index]);
						break;
					case VarType::UShort:
						AddNumber(Variable.value.u16v[Index]);
						break;
					case VarType::SLong:
						AddNumber(static_cast<double>(Variable.value.s64v[Index]));
						break;
					case VarType::ULong:
						AddNumber(static_cast<double>(Variable.value.u64v[Index]));
						break;
					case VarType::SByte:
						AddNumber(Variable.value.s8v[Index]);
						break;
					case VarType::UByte:
						AddNumber(Variable.value.u8v[Index]);
						break;
					default:
						Index = ValueCount;
						break;
					}
				}
				Json->SetArrayField(TEXT("values"), MoveTemp(Values));
				TArray<TSharedPtr<FJsonValue>> Members;
				if (Depth < 8)
				{
					Members.Reserve(static_cast<int32>(Variable.members.size()));
					for (const ShaderVariable& Member : Variable.members)
					{
						Members.Add(MakeShared<FJsonValueObject>(SerializeShaderVariable(Member, Depth + 1)));
					}
				}
				Json->SetArrayField(TEXT("members"), MoveTemp(Members));
				return Json;
			};

			auto SerializeVariableNames = [this](const rdcarray<ShaderVariable>& Variables)
			{
				TArray<TSharedPtr<FJsonValue>> Names;
				for (const ShaderVariable& Variable : Variables)
				{
					if (!Variable.name.empty())
					{
						Names.Add(MakeShared<FJsonValueString>(FromRdcString(Variable.name)));
					}
					if (!bFullDiagnostics && Names.Num() >= 32)
					{
						break;
					}
				}
				return Names;
			};
			auto SerializeVariableValues = [this, &SerializeShaderVariable](const rdcarray<ShaderVariable>& Variables)
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const ShaderVariable& Variable : Variables)
				{
					Values.Add(MakeShared<FJsonValueObject>(SerializeShaderVariable(Variable, 0)));
					if (!bFullDiagnostics && Values.Num() >= 32)
					{
						break;
					}
				}
				return Values;
			};
			Object->SetArrayField(TEXT("inputVariables"), SerializeVariableNames(Trace->inputs));
			Object->SetArrayField(TEXT("constantBlockVariables"), SerializeVariableNames(Trace->constantBlocks));
			Object->SetArrayField(TEXT("inputVariableValues"), SerializeVariableValues(Trace->inputs));
			Object->SetArrayField(TEXT("constantBlockVariableValues"), SerializeVariableValues(Trace->constantBlocks));
			Object->SetArrayField(TEXT("readOnlyResourceVariables"), SerializeVariableValues(Trace->readOnlyResources));
			Object->SetArrayField(TEXT("readWriteResourceVariables"), SerializeVariableValues(Trace->readWriteResources));
			TArray<TSharedPtr<FJsonValue>> SourceVariableNames;
			for (const SourceVariableMapping& Mapping : Trace->sourceVars)
			{
				if (!Mapping.name.empty())
				{
					SourceVariableNames.Add(MakeShared<FJsonValueString>(FromRdcString(Mapping.name)));
				}
				if (!bFullDiagnostics && SourceVariableNames.Num() >= 32)
				{
					break;
				}
			}
			Object->SetArrayField(TEXT("sourceVariableNames"), MoveTemp(SourceVariableNames));
			TArray<TSharedPtr<FJsonValue>> InstructionSourceSamples;
			for (const InstructionSourceInfo& Info : Trace->instInfo)
			{
				const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
				Source->SetNumberField(TEXT("instruction"), Info.instruction);
				Source->SetNumberField(TEXT("disassemblyLine"), Info.lineInfo.disassemblyLine);
				Source->SetNumberField(TEXT("fileIndex"), Info.lineInfo.fileIndex);
				Source->SetNumberField(TEXT("lineStart"), Info.lineInfo.lineStart);
				Source->SetNumberField(TEXT("lineEnd"), Info.lineInfo.lineEnd);
				Source->SetNumberField(TEXT("columnStart"), Info.lineInfo.colStart);
				Source->SetNumberField(TEXT("columnEnd"), Info.lineInfo.colEnd);
				InstructionSourceSamples.Add(MakeShared<FJsonValueObject>(Source));
				if (!bFullDiagnostics && InstructionSourceSamples.Num() >= 32)
				{
					break;
				}
			}
			Object->SetArrayField(TEXT("instructionSourceSamples"), MoveTemp(InstructionSourceSamples));

			auto SerializeStateSnapshot = [this, &SerializeShaderVariable](const ShaderDebugState& State)
			{
				const TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
				Snapshot->SetNumberField(TEXT("stepIndex"), State.stepIndex);
				Snapshot->SetNumberField(TEXT("nextInstruction"), State.nextInstruction);
				Snapshot->SetStringField(TEXT("flags"), RenderDocEnumToString(State.flags));
				Snapshot->SetNumberField(TEXT("changeCount"), State.changes.size());
				TArray<TSharedPtr<FJsonValue>> ChangedVariables;
				for (const ShaderVariableChange& Change : State.changes)
				{
					const rdcstr& Name = !Change.after.name.empty() ? Change.after.name : Change.before.name;
					if (!Name.empty())
					{
						const TSharedRef<FJsonObject> Changed = MakeShared<FJsonObject>();
						Changed->SetStringField(TEXT("name"), FromRdcString(Name));
						Changed->SetObjectField(TEXT("before"), SerializeShaderVariable(Change.before, 0));
						Changed->SetObjectField(TEXT("after"), SerializeShaderVariable(Change.after, 0));
						ChangedVariables.Add(MakeShared<FJsonValueObject>(Changed));
					}
					if (!bFullDiagnostics && ChangedVariables.Num() >= 32)
					{
						break;
					}
				}
				Snapshot->SetArrayField(TEXT("changedVariables"), MoveTemp(ChangedVariables));
				TArray<TSharedPtr<FJsonValue>> Callstack;
				for (const rdcstr& Function : State.callstack)
				{
					Callstack.Add(MakeShared<FJsonValueString>(FromRdcString(Function)));
					if (!bFullDiagnostics && Callstack.Num() >= 16)
					{
						break;
					}
				}
				Snapshot->SetArrayField(TEXT("callstack"), MoveTemp(Callstack));
				return Snapshot;
			};

			TArray<TSharedPtr<FJsonValue>> StepFlags;
			TArray<TSharedPtr<FJsonValue>> TraceStateSamples;
			TArray<TSharedPtr<FJsonValue>> TextureAccesses;
			TArray<FIntPoint> ObservedInstructionLines;
			TMap<uint32, int32> DisassemblyLineByInstruction;
			TMap<uint32, FString> DisassemblyTextByInstruction;
			TArray<FString> DisassemblyLines;
			ShaderDisassembly.ParseIntoArrayLines(DisassemblyLines, false);
			for (const InstructionSourceInfo& Info : Trace->instInfo)
			{
				const int32 DisassemblyLine = static_cast<int32>(Info.lineInfo.disassemblyLine);
				DisassemblyLineByInstruction.Add(Info.instruction, DisassemblyLine);
				if (DisassemblyLines.IsValidIndex(DisassemblyLine))
				{
					DisassemblyTextByInstruction.Add(Info.instruction, DisassemblyLines[DisassemblyLine].TrimStartAndEnd());
				}
			}
			TMap<FString, ShaderVariable> LiveVariables;
			TSet<uint32> SerializedTextureInstructions;
			auto ResolveTextureInstruction = [&DisassemblyTextByInstruction](uint32 EventInstruction)
			{
				for (uint32 Offset = 0; Offset <= 12 && Offset <= EventInstruction; ++Offset)
				{
					const uint32 CandidateInstruction = EventInstruction - Offset;
					const FString CandidateText = DisassemblyTextByInstruction.FindRef(CandidateInstruction);
					if (CandidateText.Contains(TEXT("textureLoad"), ESearchCase::IgnoreCase)
						|| CandidateText.Contains(TEXT("textureSample"), ESearchCase::IgnoreCase)
						|| CandidateText.Contains(TEXT("textureGather"), ESearchCase::IgnoreCase)
						|| CandidateText.Contains(TEXT("sample"), ESearchCase::IgnoreCase)
						|| CandidateText.Contains(TEXT("OpImage"), ESearchCase::IgnoreCase))
					{
						return CandidateInstruction;
					}
				}
				return EventInstruction;
			};
			uint32 NextInstructionBeforeState = 0;
			TSharedPtr<FJsonObject> FirstStateSnapshot;
			TSharedPtr<FJsonObject> LastStateSnapshot;
			int32 StepCount = 0;
			int32 TotalVariableChanges = 0;
			int32 MaxCallstackDepth = 0;
			bool bCompleted = Trace->debugger != nullptr;
			if (Trace->debugger)
			{
				const double ContinueStartSeconds = FPlatformTime::Seconds();
				EmitDiagnostic(TEXT("shader_debug.continue"), TEXT("begin"), ContinueStartSeconds,
					FString::Printf(TEXT("stepLimit=%d"),
						bFullDiagnostics ? FullDiagnosticsShaderDebugSteps : MaxShaderDebugSteps), RequestId);
				while (StepCount < (bFullDiagnostics ? FullDiagnosticsShaderDebugSteps : MaxShaderDebugSteps))
				{
					const rdcarray<ShaderDebugState> States = Controller->ContinueDebug(Trace->debugger);
					if (States.empty())
					{
						break;
					}
					for (const ShaderDebugState& State : States)
					{
						const uint32 ExecutedInstruction = NextInstructionBeforeState;
						NextInstructionBeforeState = State.nextInstruction;
						++StepCount;
						if (const int32* DisassemblyLine = DisassemblyLineByInstruction.Find(State.nextInstruction))
						{
							ObservedInstructionLines.AddUnique(FIntPoint(
								static_cast<int32>(State.nextInstruction), *DisassemblyLine));
						}
						TotalVariableChanges += static_cast<int32>(State.changes.size());
						for (const ShaderVariableChange& Change : State.changes)
						{
							if (!Change.after.name.empty())
							{
								LiveVariables.Add(FromRdcString(Change.after.name), Change.after);
							}
							else if (!Change.before.name.empty())
							{
								LiveVariables.Remove(FromRdcString(Change.before.name));
							}
						}
						if (static_cast<bool>(State.flags & ShaderEvents::SampleLoadGather)
							&& TextureAccesses.Num() < MaxSerializedTextureAccesses)
						{
							const uint32 TextureInstruction = ResolveTextureInstruction(ExecutedInstruction);
							if (!SerializedTextureInstructions.Contains(TextureInstruction))
							{
								SerializedTextureInstructions.Add(TextureInstruction);
								const TSharedRef<FJsonObject> Access = MakeShared<FJsonObject>();
								Access->SetNumberField(TEXT("stepIndex"), State.stepIndex);
								Access->SetNumberField(TEXT("instruction"), TextureInstruction);
								Access->SetNumberField(TEXT("eventInstruction"), ExecutedInstruction);
								Access->SetNumberField(TEXT("nextInstruction"), State.nextInstruction);
								const FString InstructionText = DisassemblyTextByInstruction.FindRef(TextureInstruction);
								Access->SetStringField(TEXT("disassembly"), InstructionText);
								if (const int32* SourceLine = DisassemblyLineByInstruction.Find(TextureInstruction))
								{
									Access->SetNumberField(TEXT("disassemblyLine"), *SourceLine);
								}

								TSet<FString> ReferencedNames;
								FRegexMatcher VariableMatcher(FRegexPattern(TEXT("(_+[A-Za-z0-9_\\.]+)")), InstructionText);
								while (VariableMatcher.FindNext())
								{
									ReferencedNames.Add(VariableMatcher.GetCaptureGroup(1));
								}
								TArray<TSharedPtr<FJsonValue>> Variables;
								for (const FString& Name : ReferencedNames)
								{
									if (const ShaderVariable* Variable = LiveVariables.Find(Name))
									{
										Variables.Add(MakeShared<FJsonValueObject>(SerializeShaderVariable(*Variable, 0)));
									}
								}
								Access->SetArrayField(TEXT("variables"), MoveTemp(Variables));
								TextureAccesses.Add(MakeShared<FJsonValueObject>(Access));
							}
						}
						MaxCallstackDepth = FMath::Max(MaxCallstackDepth, static_cast<int32>(State.callstack.size()));
						LastStateSnapshot = SerializeStateSnapshot(State);
						if (!FirstStateSnapshot.IsValid())
						{
							FirstStateSnapshot = LastStateSnapshot;
						}
						const FString FlagText = RenderDocEnumToString(State.flags);
						bool bKnownFlag = false;
						for (const TSharedPtr<FJsonValue>& ExistingFlag : StepFlags)
						{
							if (ExistingFlag.IsValid() && ExistingFlag->AsString() == FlagText)
							{
								bKnownFlag = true;
								break;
							}
						}
						if (!bKnownFlag)
						{
							StepFlags.Add(MakeShared<FJsonValueString>(FlagText));
						}
						if (StepCount >= (bFullDiagnostics ? FullDiagnosticsShaderDebugSteps : MaxShaderDebugSteps))
						{
							bCompleted = false;
							break;
						}
					}
					if (StepCount >= (bFullDiagnostics ? FullDiagnosticsShaderDebugSteps : MaxShaderDebugSteps))
					{
						break;
					}
				}
				EmitDiagnostic(TEXT("shader_debug.continue"), TEXT("end"), ContinueStartSeconds,
					FString::Printf(TEXT("steps=%d completed=%s"), StepCount, bCompleted ? TEXT("true") : TEXT("false")), RequestId);
			}
			Object->SetNumberField(TEXT("stepCount"), StepCount);
			Object->SetBoolField(TEXT("completed"), bCompleted);
			Object->SetNumberField(TEXT("totalVariableChanges"), TotalVariableChanges);
			Object->SetNumberField(TEXT("maxCallstackDepth"), MaxCallstackDepth);
			if (FirstStateSnapshot.IsValid())
			{
				TraceStateSamples.Add(MakeShared<FJsonValueObject>(FirstStateSnapshot.ToSharedRef()));
			}
			if (LastStateSnapshot.IsValid() && LastStateSnapshot != FirstStateSnapshot)
			{
				TraceStateSamples.Add(MakeShared<FJsonValueObject>(LastStateSnapshot.ToSharedRef()));
			}
			Object->SetArrayField(TEXT("traceStateSamples"), MoveTemp(TraceStateSamples));
			Object->SetArrayField(TEXT("stepFlags"), MoveTemp(StepFlags));
			Object->SetArrayField(TEXT("textureAccesses"), MoveTemp(TextureAccesses));
			Object->SetObjectField(TEXT("shaderCodeEvidence"),
				BuildShaderCodeEvidence(ShaderDisassembly, ObservedInstructionLines));
			Controller->FreeTrace(Trace);
			Emit(Object);
			if (FinalEventId > 0)
			{
				const double RestoreStartSeconds = FPlatformTime::Seconds();
				EmitDiagnostic(TEXT("shader_debug.restore_frame_event"), TEXT("begin"), RestoreStartSeconds,
					FString::Printf(TEXT("eventId=%u force=true"), FinalEventId), RequestId);
				Controller->SetFrameEvent(FinalEventId, true);
				EmitDiagnostic(TEXT("shader_debug.restore_frame_event"), TEXT("end"), RestoreStartSeconds,
					FString::Printf(TEXT("eventId=%u"), FinalEventId), RequestId);
			}
			EmitDiagnostic(TEXT("shader_debug"), TEXT("end"), QueryStartSeconds,
				FString::Printf(TEXT("eventId=%u steps=%d"), EventId, StepCount), RequestId);
		}

		void Shutdown()
		{
			if (Controller)
			{
				const double ShutdownStartSeconds = FPlatformTime::Seconds();
				EmitDiagnostic(TEXT("replay_controller_shutdown"), TEXT("begin"), ShutdownStartSeconds,
					TEXT("Calling IReplayController::Shutdown"));
				Controller->Shutdown();
				Controller = nullptr;
				EmitDiagnostic(TEXT("replay_controller_shutdown"), TEXT("end"), ShutdownStartSeconds,
					TEXT("Replay controller released"));
			}
			ActionNames.Empty();
			ActionKinds.Empty();
			ActionPaths.Empty();
			ActionFlagsByEvent.Empty();
			bActionIndexBuilt = false;
			bPreviewCached = false;
			// RenderDoc replay must be shut down exactly once, at process exit. In
			// particular, do not initialise/shutdown/re-initialise it for each capture.
			// The editor never links this module; the worker process owns the complete
			// replay lifetime.
			// The editor's RenderDocPlugin may own the same DLL. Keep the module loaded
			// for the editor lifetime instead of unloading it behind that plugin's back.
			DllHandle = nullptr;
		}

	public:
		void Emit(const TSharedRef<FJsonObject>& Object) const
		{
			Object->SetNumberField(TEXT("protocolVersion"), ReplayWorkerProtocolVersion);
			if (MessageCallback)
			{
				MessageCallback(SerializeJson(Object));
			}
		}

		void EmitError(const FString& Stage, const FString& Message, const FString& RequestId = FString()) const
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("type"), TEXT("error"));
			Object->SetStringField(TEXT("stage"), Stage);
			Object->SetStringField(TEXT("message"), Message);
			if (!RequestId.IsEmpty())
			{
				Object->SetStringField(TEXT("requestId"), RequestId);
			}
			Emit(Object);
		}

		void EmitDiagnostic(const FString& Stage, const FString& State, double StageStartSeconds,
			const FString& Detail, const FString& RequestId = FString()) const
		{
			const double Now = FPlatformTime::Seconds();
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("type"), TEXT("diagnostic"));
			Object->SetStringField(TEXT("stage"), Stage);
			Object->SetStringField(TEXT("state"), State);
			Object->SetStringField(TEXT("detail"), Detail);
			Object->SetNumberField(TEXT("stageElapsedSeconds"), FMath::Max(0.0, Now - StageStartSeconds));
			Object->SetNumberField(TEXT("sessionElapsedSeconds"),
				SessionStartSeconds > 0.0 ? FMath::Max(0.0, Now - SessionStartSeconds) : 0.0);
			if (!RequestId.IsEmpty())
			{
				Object->SetStringField(TEXT("requestId"), RequestId);
			}
			Emit(Object);
		}

		void EmitProgress(const FString& Phase, double ElapsedSeconds) const
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("type"), TEXT("progress"));
			Object->SetStringField(TEXT("phase"), Phase);
			Object->SetNumberField(TEXT("elapsedSeconds"), ElapsedSeconds);
			Emit(Object);
		}

	private:
		using FResourceFormatName = void (RENDERDOC_CC *)(const ResourceFormat&, rdcstr&);

		bool IsPreviewCacheValid() const
		{
			if (!IFileManager::Get().FileExists(*PreviewPath))
			{
				return false;
			}
			const FDateTime CaptureTimestamp = IFileManager::Get().GetTimeStamp(*CapturePath);
			const FDateTime PreviewTimestamp = IFileManager::Get().GetTimeStamp(*PreviewPath);
			if (CaptureTimestamp == FDateTime::MinValue() || PreviewTimestamp == FDateTime::MinValue())
			{
				return false;
			}
			return PreviewTimestamp >= CaptureTimestamp;
		}

		bool IsPixelExactPreviewValid() const
		{
			if (!IsPreviewCacheValid())
			{
				return false;
			}
			FString MetadataJson;
			FCaptureMetadata Metadata;
			FString MetadataError;
			return FFileHelper::LoadFileToString(
					MetadataJson, *GetMetadataPathForCapture(CapturePath))
				&& FCaptureMetadata::FromJson(MetadataJson, Metadata, MetadataError)
				&& Metadata.bPreviewPixelExact
				&& Metadata.PreviewWidth > 0
				&& Metadata.PreviewHeight > 0
				&& !Metadata.PreviewPath.IsEmpty()
				&& FPaths::IsSamePath(
					FPaths::ConvertRelativePathToFull(Metadata.PreviewPath), PreviewPath);
		}

		bool EnsureActionIndex(FString& OutError)
		{
			if (bActionIndexBuilt)
			{
				return true;
			}
			if (!Controller)
			{
				OutError = TEXT("Replay session is not open.");
				return false;
			}

			const double StartSeconds = FPlatformTime::Seconds();
			EmitDiagnostic(TEXT("action_index"), TEXT("begin"), StartSeconds,
				TEXT("Reading root actions and structured file labels"));
			const rdcarray<ActionDescription>& Actions = Controller->GetRootActions();
			IndexActions(Actions, Controller->GetStructuredFile(), ActionNames, ActionKinds, ActionPaths, ActionFlagsByEvent);
			bActionIndexBuilt = true;
			EmitDiagnostic(TEXT("action_index"), TEXT("end"), StartSeconds,
				FString::Printf(TEXT("events=%d"), ActionNames.Num()));
			UE_LOG(LogRenderTrailReplayWorker, Display,
				TEXT("Deferred action index built: events=%d elapsed=%.3fs"),
				ActionNames.Num(), FPlatformTime::Seconds() - StartSeconds);
			return true;
		}

		void* DllHandle = nullptr;
		FResourceFormatName ResourceFormatName = nullptr;
		IReplayController* Controller = nullptr;
		bool bReplayInitialized = false;
		bool bPixelHistorySupported = false;
		bool bShaderDebuggingSupported = false;
		FString CapturePath;
		FString PreviewPath;
		FString TargetName;
		FString TargetFormat;
		FString RenderDocVersion;
		FString RenderDocCommit;
		TFunction<void(const FString&)> MessageCallback;
		double SessionStartSeconds = 0.0;
		ResourceId TargetTexture;
		CompType TargetCompType = CompType::Typeless;
		int32 TargetResourceIndex = INDEX_NONE;
		uint32 TargetSamples = 1;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 FinalEventId = 0;
		bool bPreviewCached = false;
		bool bEmbeddedThumbnailExported = false;
		bool bActionIndexBuilt = false;
		bool bFullDiagnostics = false;
		TMap<uint32, FString> ActionNames;
		TMap<uint32, FString> ActionKinds;
		TMap<uint32, FString> ActionPaths;
		TMap<uint32, uint32> ActionFlagsByEvent;
		static constexpr int32 MaxSerializedEventResources = 16;
		static constexpr int32 MaxSerializedTextureAccesses = 64;
		static constexpr int32 MaxShaderDebugSteps = 4096;
		static constexpr int32 FullDiagnosticsShaderDebugSteps = 65536;
	};
}

namespace UE::RenderTrail::Replay
{
	using FMessageCallback = TFunction<void(const FString&)>;
	static TUniquePtr<Private::FReplaySession> GSession;
	void Shutdown();

	bool OpenCapture(const FString& CapturePath, const FString& PreviewPath, bool bFullDiagnostics,
		FMessageCallback MessageCallback, FString& OutError)
	{
		Shutdown();
		GSession = MakeUnique<Private::FReplaySession>(MoveTemp(MessageCallback), bFullDiagnostics);
		Private::FReplaySession* Session = GSession.Get();
		const auto ReportProgress = [Session](const FString& Phase, double ElapsedSeconds)
		{
			Session->EmitProgress(Phase, ElapsedSeconds);
		};
		if (!Session->Open(CapturePath, PreviewPath, ReportProgress, OutError))
		{
			GSession.Reset();
			return false;
		}
		Session->EmitReady();
		return true;
	}

	void Shutdown()
	{
		if (GSession)
		{
			GSession->Shutdown();
			GSession.Reset();
		}
	}

	void ShutdownRenderDoc()
	{
		#if RENDERTRAIL_REPLAY_WORKER_PROGRAM
		if (Private::GReplayInitialized)
		{
			RENDERDOC_ShutdownReplay();
			Private::GReplayInitialized = false;
		}
		#endif
	}

	void QueryPixelHistory(uint32 X, uint32 Y, int32 ResourceIndex, uint32 Mip, uint32 Slice,
		uint32 SampleIndex, uint32 BeforeEventId, int32 TypeCastOverride, const FString& RequestId)
	{
		if (GSession)
		{
			GSession->QueryPixelHistory(X, Y, ResourceIndex, Mip, Slice, SampleIndex,
				BeforeEventId, TypeCastOverride, RequestId);
		}
	}

	void QueryEventContext(uint32 EventId, const FString& RequestId)
	{
		if (GSession)
		{
			GSession->QueryEventContext(EventId, RequestId);
		}
	}

	void QueryShaderDebug(uint32 EventId, uint32 X, uint32 Y, int32 SampleIndex, uint32 PrimitiveId, bool bHasPrimitive,
		const FString& RequestId)
	{
		if (GSession)
		{
			GSession->QueryShaderDebug(EventId, X, Y, SampleIndex, PrimitiveId, bHasPrimitive, RequestId);
		}
	}

	bool IsOpen()
	{
		return GSession.IsValid();
	}
}

#if RENDERTRAIL_REPLAY_WORKER_PROGRAM

namespace
{
	static void WriteProtocolLine(const FString& Line)
	{
		const FTCHARToUTF8 Utf8(*Line);
		std::fwrite(Utf8.Get(), 1, Utf8.Length(), stdout);
		std::fputc('\n', stdout);
		std::fflush(stdout);
	}

	static FString Utf8ToFString(const std::string& Value)
	{
		const FUTF8ToTCHAR Converted(Value.c_str());
		return FString(Converted.Get());
	}

	static void WriteProtocolError(const TCHAR* Stage, const FString& Message, const FString& RequestId = FString())
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("type"), TEXT("error"));
		Object->SetStringField(TEXT("stage"), Stage);
		Object->SetStringField(TEXT("message"), Message);
		if (!RequestId.IsEmpty())
		{
			Object->SetStringField(TEXT("requestId"), RequestId);
		}
		Object->SetNumberField(TEXT("protocolVersion"), UE::RenderTrail::ReplayWorkerProtocolVersion);
		WriteProtocolLine(UE::RenderTrail::Private::SerializeJson(Object));
	}

	static void WriteProtocolDiagnostic(const TCHAR* Stage, const TCHAR* State,
		double StageStartSeconds, const FString& Detail)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("type"), TEXT("diagnostic"));
		Object->SetStringField(TEXT("stage"), Stage);
		Object->SetStringField(TEXT("state"), State);
		Object->SetStringField(TEXT("detail"), Detail);
		Object->SetNumberField(TEXT("stageElapsedSeconds"),
			FMath::Max(0.0, FPlatformTime::Seconds() - StageStartSeconds));
		Object->SetNumberField(TEXT("protocolVersion"), UE::RenderTrail::ReplayWorkerProtocolVersion);
		WriteProtocolLine(UE::RenderTrail::Private::SerializeJson(Object));
	}

	static int32 ReadUInt32(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		double Value = 0.0;
		return Object.IsValid() && Object->TryGetNumberField(Field, Value)
			? static_cast<int32>(FMath::Max(0.0, Value))
			: 0;
	}

	static int32 ReadInt32(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 DefaultValue = 0)
	{
		double Value = 0.0;
		return Object.IsValid() && Object->TryGetNumberField(Field, Value)
			? static_cast<int32>(Value)
			: DefaultValue;
	}
}

	IMPLEMENT_APPLICATION(RenderTrailReplayWorker, "RenderTrailReplayWorker");

	INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
	{
		int32 ReturnCode = EXIT_SUCCESS;
		const double PreInitStartSeconds = FPlatformTime::Seconds();
		GEngineLoop.PreInit(ArgC, ArgV);
		WriteProtocolDiagnostic(TEXT("worker_preinit"), TEXT("end"), PreInitStartSeconds,
			TEXT("GEngineLoop.PreInit completed"));

	FString CapturePath;
	FString PreviewPath;
	FParse::Value(FCommandLine::Get(), TEXT("Capture="), CapturePath);
	FParse::Value(FCommandLine::Get(), TEXT("Preview="), PreviewPath);
	const bool bFullDiagnostics = FParse::Param(FCommandLine::Get(), TEXT("RenderTrailFullDiagnostics"));
	if (CapturePath.IsEmpty() || PreviewPath.IsEmpty())
	{
		WriteProtocolError(TEXT("startup"), TEXT("Replay Worker requires -Capture= and -Preview= command line arguments."));
		ReturnCode = EXIT_FAILURE;
	}
	else
	{
		FString OpenError;
		const bool bOpened = UE::RenderTrail::Replay::OpenCapture(
			CapturePath,
			PreviewPath,
			bFullDiagnostics,
			[](const FString& JsonLine)
			{
				WriteProtocolLine(JsonLine);
			},
			OpenError);
		if (!bOpened)
		{
			WriteProtocolError(TEXT("open_capture"), OpenError);
			ReturnCode = EXIT_FAILURE;
		}
		else
		{
			std::string InputLine;
			while (std::getline(std::cin, InputLine))
			{
				const FString JsonLine = Utf8ToFString(InputLine);
				TSharedPtr<FJsonObject> Request;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonLine);
				if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
				{
					WriteProtocolError(TEXT("protocol"), TEXT("Replay Worker received a malformed JSON request."));
					continue;
				}

				FString Command;
				Request->TryGetStringField(TEXT("command"), Command);
				if (Command == TEXT("shutdown"))
				{
					break;
				}

				FString RequestId;
				Request->TryGetStringField(TEXT("requestId"), RequestId);
				if (Command == TEXT("pixel_history"))
				{
					UE::RenderTrail::Replay::QueryPixelHistory(
						static_cast<uint32>(ReadUInt32(Request, TEXT("x"))),
						static_cast<uint32>(ReadUInt32(Request, TEXT("y"))),
						ReadInt32(Request, TEXT("resourceIndex"), INDEX_NONE),
						static_cast<uint32>(ReadUInt32(Request, TEXT("mip"))),
						static_cast<uint32>(ReadUInt32(Request, TEXT("slice"))),
						static_cast<uint32>(ReadUInt32(Request, TEXT("sample"))),
						static_cast<uint32>(ReadUInt32(Request, TEXT("beforeEventId"))),
						ReadInt32(Request, TEXT("typeCast"), INDEX_NONE),
						RequestId);
				}
				else if (Command == TEXT("event_context"))
				{
					UE::RenderTrail::Replay::QueryEventContext(
						static_cast<uint32>(ReadUInt32(Request, TEXT("eventId"))), RequestId);
				}
				else if (Command == TEXT("shader_debug"))
				{
					bool bHasPrimitive = false;
					Request->TryGetBoolField(TEXT("hasPrimitive"), bHasPrimitive);
					UE::RenderTrail::Replay::QueryShaderDebug(
						static_cast<uint32>(ReadUInt32(Request, TEXT("eventId"))),
						static_cast<uint32>(ReadUInt32(Request, TEXT("x"))),
						static_cast<uint32>(ReadUInt32(Request, TEXT("y"))),
						ReadInt32(Request, TEXT("sample"), -1),
						static_cast<uint32>(ReadUInt32(Request, TEXT("primitiveId"))),
						bHasPrimitive,
						RequestId);
				}
				else
				{
					WriteProtocolError(TEXT("protocol"), FString::Printf(TEXT("Unknown Replay Worker command: %s"), *Command), RequestId);
				}
			}
		}
	}

	const double SessionShutdownStartSeconds = FPlatformTime::Seconds();
	WriteProtocolDiagnostic(TEXT("worker_session_shutdown"), TEXT("begin"), SessionShutdownStartSeconds,
		TEXT("Releasing Replay Controller and session state"));
	UE::RenderTrail::Replay::Shutdown();
	WriteProtocolDiagnostic(TEXT("worker_session_shutdown"), TEXT("end"), SessionShutdownStartSeconds,
		TEXT("Replay session released"));
	const double RenderDocShutdownStartSeconds = FPlatformTime::Seconds();
	WriteProtocolDiagnostic(TEXT("renderdoc_runtime_shutdown"), TEXT("begin"), RenderDocShutdownStartSeconds,
		TEXT("Calling RENDERDOC_ShutdownReplay"));
	UE::RenderTrail::Replay::ShutdownRenderDoc();
	WriteProtocolDiagnostic(TEXT("renderdoc_runtime_shutdown"), TEXT("end"), RenderDocShutdownStartSeconds,
		TEXT("RenderDoc replay runtime shut down"));
	FEngineLoop::AppPreExit();
	FModuleManager::Get().UnloadModulesAtShutdown();
	FEngineLoop::AppExit();
	FPlatformMisc::RequestExit(false);
	return ReturnCode;
}

#else

IMPLEMENT_MODULE(FDefaultModuleImpl, RenderTrailReplayWorker)

#endif
