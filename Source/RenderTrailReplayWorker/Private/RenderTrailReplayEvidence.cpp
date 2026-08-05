#include "RenderTrailReplayEvidence.h"

// RenderDoc exposes a global LogType enum, while CoreUObject declares a global
// LogType log category. Keep the third-party name isolated to this include.
#define LogType RenderDocLogType
#include "renderdoc/api/replay/renderdoc_replay.h"
#undef LogType

namespace UE::RenderTrail::Private
{
	static bool IsResourceWriteUsage(ResourceUsage Usage)
	{
		switch (Usage)
		{
		case ResourceUsage::StreamOut:
		case ResourceUsage::VS_RWResource:
		case ResourceUsage::HS_RWResource:
		case ResourceUsage::DS_RWResource:
		case ResourceUsage::GS_RWResource:
		case ResourceUsage::PS_RWResource:
		case ResourceUsage::CS_RWResource:
		case ResourceUsage::TS_RWResource:
		case ResourceUsage::MS_RWResource:
		case ResourceUsage::All_RWResource:
		case ResourceUsage::ColorTarget:
		case ResourceUsage::DepthStencilTarget:
		case ResourceUsage::Clear:
		case ResourceUsage::GenMips:
		case ResourceUsage::Resolve:
		case ResourceUsage::ResolveDst:
		case ResourceUsage::Copy:
		case ResourceUsage::CopyDst:
		case ResourceUsage::CPUWrite:
			return true;
		default:
			return false;
		}
	}

	static FString DescribeResourceUsage(ResourceUsage Usage)
	{
		switch (Usage)
		{
		case ResourceUsage::Discard: return TEXT("Discard");
		case ResourceUsage::Clear: return TEXT("Clear");
		case ResourceUsage::Copy: return TEXT("Copy");
		case ResourceUsage::CopyDst: return TEXT("CopyDst");
		case ResourceUsage::Resolve: return TEXT("Resolve");
		case ResourceUsage::ResolveDst: return TEXT("ResolveDst");
		case ResourceUsage::GenMips: return TEXT("GenMips");
		case ResourceUsage::ColorTarget: return TEXT("ColorTarget");
		case ResourceUsage::DepthStencilTarget: return TEXT("DepthStencilTarget");
		case ResourceUsage::CPUWrite: return TEXT("CPUWrite");
		case ResourceUsage::StreamOut: return TEXT("StreamOut");
		case ResourceUsage::VS_RWResource: return TEXT("VS_RWResource");
		case ResourceUsage::HS_RWResource: return TEXT("HS_RWResource");
		case ResourceUsage::DS_RWResource: return TEXT("DS_RWResource");
		case ResourceUsage::GS_RWResource: return TEXT("GS_RWResource");
		case ResourceUsage::PS_RWResource: return TEXT("PS_RWResource");
		case ResourceUsage::CS_RWResource: return TEXT("CS_RWResource");
		case ResourceUsage::TS_RWResource: return TEXT("TS_RWResource");
		case ResourceUsage::MS_RWResource: return TEXT("MS_RWResource");
		case ResourceUsage::All_RWResource: return TEXT("All_RWResource");
		default: return TEXT("Unknown");
		}
	}

	static FString DescribeResourceRelation(ResourceUsage Usage)
	{
		switch (Usage)
		{
		case ResourceUsage::Clear: return TEXT("clears-resource");
		case ResourceUsage::Copy:
		case ResourceUsage::CopyDst: return TEXT("copies-into-resource");
		case ResourceUsage::Resolve:
		case ResourceUsage::ResolveDst: return TEXT("resolves-into-resource");
		case ResourceUsage::GenMips: return TEXT("generates-resource-mips");
		case ResourceUsage::DepthStencilTarget: return TEXT("writes-depth-stencil-resource");
		case ResourceUsage::CPUWrite: return TEXT("cpu-writes-resource");
		default: return TEXT("writes-resource");
		}
	}

	TSharedRef<FJsonObject> BuildResourceProvenance(
		IReplayController& Controller,
		const ResourceId& InputResource,
		uint32 ConsumerEventId,
		const FString& ResourceName,
		int32 ResourceIndex,
		const TextureDescription* InputTexture,
		const TextureDescription* OutputTexture,
		const TMap<uint32, FString>& ActionNames,
		const TMap<uint32, FString>& ActionKinds,
		const TMap<uint32, FString>& ActionPaths)
	{
		const TSharedRef<FJsonObject> Provenance = MakeShared<FJsonObject>();
		Provenance->SetStringField(TEXT("resource"), ResourceName.IsEmpty() ? TEXT("Unnamed resource") : ResourceName);
		Provenance->SetNumberField(TEXT("resourceIndex"), ResourceIndex);
		Provenance->SetStringField(TEXT("readEvidence"), TEXT("event-descriptor-access"));
		Provenance->SetStringField(TEXT("pixelContribution"), TEXT("not-proven-from-binding-or-descriptor-access-alone"));
		Provenance->SetStringField(TEXT("pixelTraceStatus"), TEXT("blocked-until-sample-coordinate-and-value-are-proven"));

		if (InputTexture && OutputTexture)
		{
			const bool bSameDimensions = InputTexture->width == OutputTexture->width
				&& InputTexture->height == OutputTexture->height;
			Provenance->SetStringField(TEXT("dimensionRelation"), bSameDimensions ? TEXT("same-size") : TEXT("different-size"));
			Provenance->SetStringField(TEXT("coordinateMapping"), bSameDimensions
				? TEXT("not-proven; dimensions match but executed sample/load coordinates are unavailable")
				: TEXT("not-proven; input/output dimensions differ and resampling or remapping may occur"));
		}
		else
		{
			Provenance->SetStringField(TEXT("dimensionRelation"), TEXT("unknown"));
			Provenance->SetStringField(TEXT("coordinateMapping"), TEXT("not-proven; input/output texture relationship is incomplete"));
		}

		bool bFoundProducer = false;
		FString ProducerStatus = TEXT("not-found");
		FString ChainBreak = TEXT("no prior write usage was found for this bound resource");
		const rdcarray<EventUsage> Usages = Controller.GetUsage(InputResource);
		const EventUsage* PreviousRelevantUsage = nullptr;
		for (const EventUsage& Usage : Usages)
		{
			if (Usage.eventId < ConsumerEventId
				&& (Usage.usage == ResourceUsage::Discard || IsResourceWriteUsage(Usage.usage)))
			{
				if (!PreviousRelevantUsage || Usage.eventId > PreviousRelevantUsage->eventId)
				{
					PreviousRelevantUsage = &Usage;
				}
			}
		}

		if (PreviousRelevantUsage)
		{
			Provenance->SetStringField(TEXT("producerUsage"), DescribeResourceUsage(PreviousRelevantUsage->usage));
			if (PreviousRelevantUsage->usage == ResourceUsage::Discard)
			{
				ProducerStatus = TEXT("invalidated");
				Provenance->SetStringField(TEXT("relation"), TEXT("invalidates-resource"));
				Provenance->SetNumberField(TEXT("invalidatingEventId"), PreviousRelevantUsage->eventId);
				Provenance->SetStringField(TEXT("invalidatingAction"), ActionNames.FindRef(PreviousRelevantUsage->eventId));
				ChainBreak = TEXT("resource contents were discarded before this read; discard is not a producer");
			}
			else
			{
				bFoundProducer = true;
				ProducerStatus = TEXT("confirmed-resource-write");
				ChainBreak.Empty();
				Provenance->SetStringField(TEXT("relation"), DescribeResourceRelation(PreviousRelevantUsage->usage));
				Provenance->SetNumberField(TEXT("producerEventId"), PreviousRelevantUsage->eventId);
				Provenance->SetStringField(TEXT("producerAction"), ActionNames.FindRef(PreviousRelevantUsage->eventId));
				Provenance->SetStringField(TEXT("producerKind"), ActionKinds.FindRef(PreviousRelevantUsage->eventId));
				Provenance->SetStringField(TEXT("producerMarkerPath"), ActionPaths.FindRef(PreviousRelevantUsage->eventId));
			}
		}

		Provenance->SetBoolField(TEXT("producerFound"), bFoundProducer);
		Provenance->SetStringField(TEXT("producerStatus"), ProducerStatus);
		Provenance->SetStringField(TEXT("chainBreak"), ChainBreak);
		return Provenance;
	}

	TSharedRef<FJsonObject> BuildShaderCodeEvidence(
		const FString& Disassembly,
		const TArray<FIntPoint>& ObservedInstructionLines,
		int32 MaxLines)
	{
		const TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
		Evidence->SetStringField(TEXT("kind"), TEXT("executed-disassembly-window"));
		Evidence->SetStringField(TEXT("mapping"), TEXT("DebugPixel nextInstruction mapped through ShaderDebugTrace.instInfo"));
		if (Disassembly.IsEmpty() || ObservedInstructionLines.IsEmpty())
		{
			Evidence->SetStringField(TEXT("status"), TEXT("unavailable"));
			Evidence->SetArrayField(TEXT("lines"), {});
			return Evidence;
		}

		TArray<FString> DisassemblyLines;
		Disassembly.ParseIntoArrayLines(DisassemblyLines, false);
		TArray<TSharedPtr<FJsonValue>> Lines;
		TSet<FIntPoint> Seen;
		for (const FIntPoint& Mapping : ObservedInstructionLines)
		{
			if (Seen.Contains(Mapping) || Lines.Num() >= MaxLines)
			{
				continue;
			}
			Seen.Add(Mapping);
			int32 LineIndex = Mapping.Y;
			if (!DisassemblyLines.IsValidIndex(LineIndex) && DisassemblyLines.IsValidIndex(LineIndex - 1))
			{
				--LineIndex;
			}
			if (!DisassemblyLines.IsValidIndex(LineIndex))
			{
				continue;
			}
			const TSharedRef<FJsonObject> Line = MakeShared<FJsonObject>();
			Line->SetNumberField(TEXT("instruction"), Mapping.X);
			Line->SetNumberField(TEXT("disassemblyLine"), Mapping.Y);
			Line->SetStringField(TEXT("text"), DisassemblyLines[LineIndex].TrimStartAndEnd());
			Lines.Add(MakeShared<FJsonValueObject>(Line));
		}
		Evidence->SetStringField(TEXT("status"), Lines.IsEmpty() ? TEXT("mapping-unavailable") : TEXT("available"));
		Evidence->SetArrayField(TEXT("lines"), MoveTemp(Lines));
		return Evidence;
	}
}
