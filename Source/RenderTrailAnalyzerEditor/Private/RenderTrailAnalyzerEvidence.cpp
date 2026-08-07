#include "RenderTrailAnalyzerEvidence.h"

namespace UE::RenderTrail::Private
{
	FPixelValueEvidence ParsePixelValue(const TSharedPtr<FJsonObject>& Value)
	{
		FPixelValueEvidence Result;
		bool bRenderDocValueValid = false;
		if (!Value.IsValid() || !Value->TryGetBoolField(TEXT("valid"), bRenderDocValueValid) || !bRenderDocValueValid)
		{
			Result.Text = TEXT("<unavailable>");
			return Result;
		}

		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		if (!Value->TryGetArrayField(TEXT("float"), Components) || !Components || Components->Num() < 4)
		{
			Result.Text = TEXT("<unavailable>");
			return Result;
		}

		TArray<FString> Text;
		double* Channels[] = { &Result.R, &Result.G, &Result.B, &Result.A };
		Result.bValid = true;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const TSharedPtr<FJsonValue>& Component = (*Components)[Index];
			if (!Component.IsValid() || Component->Type != EJson::Number)
			{
				Result.bValid = false;
				Text.Add(TEXT("NaN"));
				continue;
			}
			*Channels[Index] = Component->AsNumber();
			Text.Add(FString::Printf(TEXT("%.6g"), *Channels[Index]));
		}

		Result.bHasDepth = Value->HasTypedField<EJson::Number>(TEXT("depth"));
		if (Result.bHasDepth)
		{
			Result.Depth = Value->GetNumberField(TEXT("depth"));
		}
		double Stencil = 0.0;
		Value->TryGetNumberField(TEXT("stencil"), Stencil);
		Result.Stencil = static_cast<int32>(Stencil);
		Result.Text = FString::Printf(TEXT("RGBA (%s)  depth %.6g  stencil %d"), *FString::Join(Text, TEXT(", ")),
			Result.bHasDepth ? Result.Depth : 0.0, Result.Stencil);
		return Result;
	}

	double ComputeColorDeltaMax(const FPixelValueEvidence& Before, const FPixelValueEvidence& After)
	{
		if (!Before.bValid || !After.bValid)
		{
			return -1.0;
		}
		return FMath::Max(
			FMath::Max(FMath::Abs(After.R - Before.R), FMath::Abs(After.G - Before.G)),
			FMath::Max(FMath::Abs(After.B - Before.B), FMath::Abs(After.A - Before.A)));
	}

	double ComputeColorDeltaL1(const FPixelValueEvidence& Before, const FPixelValueEvidence& After)
	{
		if (!Before.bValid || !After.bValid)
		{
			return -1.0;
		}
		return FMath::Abs(After.R - Before.R) + FMath::Abs(After.G - Before.G)
			+ FMath::Abs(After.B - Before.B) + FMath::Abs(After.A - Before.A);
	}

	FString ClassifyColorDelta(const FEventEvidence& Event)
	{
		if (Event.ColorDeltaMax < 0.0)
		{
			return TEXT("unknown");
		}
		if (Event.ColorDeltaMax <= KINDA_SMALL_NUMBER)
		{
			return TEXT("no-change");
		}
		return Event.ColorDeltaMax < SignificantColorDeltaThreshold
			? TEXT("minor-terminal-adjustment")
			: TEXT("significant-change");
	}

	void AddColorDeltaJson(const TSharedRef<FJsonObject>& Json, const FEventEvidence& Event)
	{
		Json->SetStringField(TEXT("changeMagnitude"), ClassifyColorDelta(Event));
		if (Event.ColorDeltaMax >= 0.0)
		{
			Json->SetNumberField(TEXT("colorDeltaMax"), Event.ColorDeltaMax);
			Json->SetNumberField(TEXT("colorDeltaL1"), Event.ColorDeltaL1);
		}
	}

	FBoundResourceEvidence ParseBoundResource(const TSharedPtr<FJsonObject>& Json)
	{
		FBoundResourceEvidence Resource;
		if (!Json.IsValid())
		{
			return Resource;
		}
		Json->TryGetStringField(TEXT("name"), Resource.Name);
		Json->TryGetStringField(TEXT("format"), Resource.Format);
		Json->TryGetStringField(TEXT("stage"), Resource.Stage);
		Json->TryGetStringField(TEXT("access"), Resource.Access);
		Json->TryGetStringField(TEXT("shaderBinding"), Resource.ShaderBinding);
		Json->TryGetBoolField(TEXT("texture"), Resource.bTexture);
		double Number = 0.0;
		if (Json->TryGetNumberField(TEXT("resourceIndex"), Number))
			Resource.ResourceIndex = static_cast<int32>(Number);
		if (Json->TryGetNumberField(TEXT("width"), Number))
			Resource.Width = static_cast<int32>(Number);
		if (Json->TryGetNumberField(TEXT("height"), Number))
			Resource.Height = static_cast<int32>(Number);
		if (Json->TryGetNumberField(TEXT("samples"), Number))
			Resource.Samples = static_cast<int32>(Number);
		if (Json->TryGetNumberField(TEXT("bindingIndex"), Number))
			Resource.BindingIndex = static_cast<int32>(Number);
		if (Json->TryGetNumberField(TEXT("arrayElement"), Number))
			Resource.ArrayElement = static_cast<int32>(Number);
		if (Json->TryGetNumberField(TEXT("firstMip"), Number))
			Resource.FirstMip = static_cast<int32>(Number);
		if (Json->TryGetNumberField(TEXT("firstSlice"), Number))
			Resource.FirstSlice = static_cast<int32>(Number);
		if (Json->TryGetNumberField(TEXT("typeCast"), Number))
			Resource.TypeCast = static_cast<int32>(Number);
		return Resource;
	}

	TArray<FEventEvidence> AggregateEvents(const FPixelSample& Sample)
	{
		TArray<FEventEvidence> Events;
		if (Sample.bEventSummaryComplete)
		{
			Events.Reserve(Sample.EventSummaries.Num());
			for (const FEventSummaryEvidence& Summary : Sample.EventSummaries)
			{
				FEventEvidence Event;
				Event.EventId = Summary.EventId;
				Event.Action = Summary.Action;
				Event.ActionKind = Summary.ActionKind;
				Event.MarkerPath = Summary.MarkerPath;
				Event.ActionFlags = Summary.ActionFlags;
				Event.PassedFragments = Summary.PassedFragments;
				Event.RejectedFragments = Summary.RejectedFragments;
				Event.bDirectShaderWrite = Summary.bDirectShaderWrite;
				Event.bUnboundPixelShader = Summary.bUnboundPixelShader;
				Event.bChangedTextureValue = Summary.bChangedTextureValue;
				Event.bHasPrimitiveEvidence = Summary.bHasPrimitiveEvidence;
				Event.PrimitiveId = Summary.PrimitiveId;
				Event.FailureReasons = Summary.FailureReasons;
				Event.Before = Summary.Before;
				Event.ShaderOutput = Summary.ShaderOutput;
				Event.After = Summary.After;
				Event.BeforeValue = Summary.BeforeValue;
				Event.ShaderOutputValue = Summary.ShaderOutputValue;
				Event.AfterValue = Summary.AfterValue;
				Event.ColorDeltaMax = ComputeColorDeltaMax(Event.BeforeValue, Event.AfterValue);
				Event.ColorDeltaL1 = ComputeColorDeltaL1(Event.BeforeValue, Event.AfterValue);
				Events.Add(MoveTemp(Event));
			}
			return Events;
		}

		TMap<uint32, int32> EventIndex;
		for (int32 ModificationIndex = 0; ModificationIndex < Sample.Modifications.Num(); ++ModificationIndex)
		{
			const FPixelModificationEvidence& Modification = Sample.Modifications[ModificationIndex];
			int32* ExistingIndex = EventIndex.Find(Modification.EventId);
			if (!ExistingIndex)
			{
				FEventEvidence Event;
				Event.EventId = Modification.EventId;
				Event.Action = Modification.Action;
				Event.ActionKind = Modification.ActionKind;
				Event.MarkerPath = Modification.MarkerPath;
				Event.ActionFlags = Modification.ActionFlags;
				Event.Before = Modification.Before;
				Event.BeforeValue = Modification.BeforeValue;
				EventIndex.Add(Event.EventId, Events.Add(MoveTemp(Event)));
				ExistingIndex = EventIndex.Find(Modification.EventId);
			}
			FEventEvidence& Event = Events[*ExistingIndex];
			Event.PassedFragments += Modification.bPassed ? 1 : 0;
			Event.RejectedFragments += Modification.bPassed ? 0 : 1;
			Event.LastModificationIndex = ModificationIndex;
			Event.bDirectShaderWrite |= Modification.bDirectShaderWrite;
			Event.bUnboundPixelShader |= Modification.bUnboundPixelShader;
			Event.bChangedTextureValue |= Modification.bChangedTextureValue;
			Event.bHasPrimitiveEvidence = true;
			Event.PrimitiveId = Modification.PrimitiveId;
			Event.ShaderOutput = Modification.ShaderOutput;
			Event.After = Modification.After;
			Event.ShaderOutputValue = Modification.ShaderOutputValue;
			Event.AfterValue = Modification.AfterValue;
			for (const FString& Failure : Modification.FailureReasons)
			{
				Event.FailureReasons.AddUnique(Failure);
			}
		}
		for (FEventEvidence& Event : Events)
		{
			Event.ColorDeltaMax = ComputeColorDeltaMax(Event.BeforeValue, Event.AfterValue);
			Event.ColorDeltaL1 = ComputeColorDeltaL1(Event.BeforeValue, Event.AfterValue);
		}
		return Events;
	}

	const FEventEvidence* FindEvent(const TArray<FEventEvidence>& Events, uint32 EventId)
	{
		return Events.FindByPredicate([EventId](const FEventEvidence& Event) { return Event.EventId == EventId; });
	}

	FString DescribeEventResult(const FEventEvidence& Event)
	{
		if (Event.bDirectShaderWrite)
		{
			return Event.bChangedTextureValue ? TEXT("potential UAV/shader write changed value") : TEXT("potential UAV/shader write, no value change");
		}
		if (Event.PassedFragments > 0)
		{
			return FString::Printf(TEXT("%d fragment%s wrote; %d rejected"), Event.PassedFragments,
				Event.PassedFragments == 1 ? TEXT("") : TEXT("s"), Event.RejectedFragments);
		}
		return Event.FailureReasons.IsEmpty()
			? TEXT("did not write")
			: FString::Printf(TEXT("rejected: %s"), *FString::Join(Event.FailureReasons, TEXT(", ")));
	}

	bool IsConfirmedPixelWriter(const FEventEvidence& Event)
	{
		return Event.PassedFragments > 0 || (Event.bDirectShaderWrite && Event.bChangedTextureValue);
	}

	FString ClassifySemantics(const FEventEvidence& Event)
	{
		if (Event.bDirectShaderWrite || Event.ActionKind == TEXT("dispatch"))
		{
			return TEXT("compute/UAV");
		}
		if (Event.ActionKind == TEXT("copy") || Event.ActionKind == TEXT("resolve"))
		{
			return TEXT("copy/resolve");
		}
		if (Event.ActionKind == TEXT("clear"))
		{
			return TEXT("clear/load");
		}
		FString SemanticContext = Event.Action;
		TArray<FString> PathComponents;
		Event.MarkerPath.ParseIntoArray(PathComponents, TEXT(" > "), true);
		const int32 FirstRelevantPath = FMath::Max(0, PathComponents.Num() - 3);
		for (int32 Index = FirstRelevantPath; Index < PathComponents.Num(); ++Index)
		{
			SemanticContext += TEXT(" ") + PathComponents[Index];
		}
		const FString Context = SemanticContext.ToLower();
		if (Context.Contains(TEXT("temporalsuperresolution")) || Context.Contains(TEXT("upscale"))
			|| Context.Contains(TEXT("downsample")) || Context.Contains(TEXT("upsample"))
			|| Context.Contains(TEXT("resample")))
		{
			return TEXT("resample/nonlinear");
		}
		if (Context.Contains(TEXT("postprocessing")) || Context.Contains(TEXT("tonemap"))
			|| Context.Contains(TEXT("bloom")) || Context.Contains(TEXT("composite"))
			|| Context.Contains(TEXT("screenpass")))
		{
			return TEXT("post-process");
		}
		return Event.ActionKind == TEXT("draw") ? TEXT("scene-write") : TEXT("unclassified");
	}

	FString CompactMarkerPath(const FString& MarkerPath)
	{
		TArray<FString> Components;
		MarkerPath.ParseIntoArray(Components, TEXT(" > "), true);
		if (Components.Num() <= 4)
		{
			return MarkerPath;
		}
		return FString::Printf(TEXT("%s > ... > %s > %s > %s"), *Components[0],
			*Components[Components.Num() - 3], *Components[Components.Num() - 2], *Components[Components.Num() - 1]);
	}

	TSharedRef<FJsonObject> BuildPixelCausalGraph(
		const FPixelSample& Sample,
		const TArray<FEventEvidence>& Events,
		const TMap<uint32, FEventContextEvidence>& EventContexts,
		int32 MaxDisplayedFrontierResources)
	{
		const TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
		Graph->SetNumberField(TEXT("schemaVersion"), 1);
		Graph->SetStringField(TEXT("direction"), TEXT("reverse-from-final-pixel"));
		Graph->SetNumberField(TEXT("maxReverseHops"), MaxCausalGraphHops);

		const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("nodeType"), TEXT("pixel-state"));
		Target->SetNumberField(TEXT("x"), Sample.Pixel.X);
		Target->SetNumberField(TEXT("y"), Sample.Pixel.Y);
		Target->SetNumberField(TEXT("mip"), 0);
		Target->SetNumberField(TEXT("slice"), 0);
		Target->SetStringField(TEXT("value"), !Events.IsEmpty() ? Events.Last().After : TEXT("unavailable"));
		Graph->SetObjectField(TEXT("target"), Target);

		TArray<TSharedPtr<FJsonValue>> WriterHops;
		int32 WriterHop = 0;
		for (int32 EventIndex = Events.Num() - 1; EventIndex >= 0 && WriterHop < MaxCausalGraphHops; --EventIndex)
		{
			const FEventEvidence& Event = Events[EventIndex];
			if (!IsConfirmedPixelWriter(Event) || Event.ActionKind == TEXT("present"))
			{
				continue;
			}

			const TSharedRef<FJsonObject> Hop = MakeShared<FJsonObject>();
			Hop->SetNumberField(TEXT("hop"), WriterHop);
			Hop->SetStringField(TEXT("nodeType"), TEXT("gpu-event"));
			Hop->SetStringField(TEXT("relation"), TEXT("writes-target-pixel"));
			Hop->SetStringField(TEXT("causalRole"), WriterHop == 0 ? TEXT("final-writer") : TEXT("earlier-target-writer"));
			Hop->SetStringField(TEXT("confidence"), TEXT("confirmed"));
			Hop->SetNumberField(TEXT("eventId"), Event.EventId);
			Hop->SetStringField(TEXT("action"), Event.Action);
			Hop->SetStringField(TEXT("kind"), Event.ActionKind);
			Hop->SetStringField(TEXT("passMarker"), CompactMarkerPath(Event.MarkerPath));
			Hop->SetStringField(TEXT("semantics"), ClassifySemantics(Event));
			Hop->SetStringField(TEXT("before"), Event.Before);
			Hop->SetStringField(TEXT("shaderOutput"), Event.ShaderOutput);
			Hop->SetStringField(TEXT("after"), Event.After);
			AddColorDeltaJson(Hop, Event);

			if (const FEventContextEvidence* Context = EventContexts.Find(Event.EventId))
			{
				Hop->SetBoolField(TEXT("eventContextAvailable"), true);
				const TSharedRef<FJsonObject> Shader = MakeShared<FJsonObject>();
				Shader->SetStringField(TEXT("stage"), Context->ShaderStage);
				Shader->SetStringField(TEXT("entry"), Context->ShaderEntry);
				Shader->SetStringField(TEXT("encoding"), Context->ShaderEncoding);
				Shader->SetBoolField(TEXT("debugTraceAvailable"), Context->ShaderDebugTrace.IsValid());
				Shader->SetStringField(TEXT("codeEvidence"), Context->ShaderDebugTrace.IsValid()
					? TEXT("executed-debug-trace") : TEXT("reflection-and-bindings-only"));
				Hop->SetObjectField(TEXT("shader"), Shader);
				Hop->SetBoolField(TEXT("fixedFunctionStateAvailable"), Context->PipelineState.IsValid());

				TArray<TSharedPtr<FJsonValue>> Inputs;
				for (int32 InputIndex = 0; InputIndex < Context->Inputs.Num() && InputIndex < MaxDisplayedFrontierResources; ++InputIndex)
				{
					const FBoundResourceEvidence& Input = Context->Inputs[InputIndex];
					const TSharedRef<FJsonObject> InputJson = MakeShared<FJsonObject>();
					InputJson->SetStringField(TEXT("nodeType"), TEXT("resource-version-candidate"));
					InputJson->SetNumberField(TEXT("resourceIndex"), Input.ResourceIndex);
					InputJson->SetStringField(TEXT("name"), Input.Name);
					InputJson->SetStringField(TEXT("format"), Input.Format);
					InputJson->SetStringField(TEXT("access"), Input.Access);
					InputJson->SetStringField(TEXT("shaderBinding"), Input.ShaderBinding);
					InputJson->SetNumberField(TEXT("width"), Input.Width);
					InputJson->SetNumberField(TEXT("height"), Input.Height);
					InputJson->SetNumberField(TEXT("samples"), Input.Samples);
					InputJson->SetStringField(TEXT("pixelContribution"), TEXT("not-proven-from-binding-alone"));
					Inputs.Add(MakeShared<FJsonValueObject>(InputJson));
				}
				Hop->SetArrayField(TEXT("boundInputCandidates"), MoveTemp(Inputs));
				Hop->SetArrayField(TEXT("resourceDependencies"), Context->ResourceProvenance);
				Hop->SetArrayField(TEXT("resourcePixelHistories"), Context->ResourcePixelHistories);
			}
			else
			{
				Hop->SetBoolField(TEXT("eventContextAvailable"), false);
			}

			const TSharedRef<FJsonObject> UnrealAttribution = MakeShared<FJsonObject>();
			UnrealAttribution->SetStringField(TEXT("material"), TEXT("unknown"));
			UnrealAttribution->SetStringField(TEXT("mesh"), TEXT("unknown"));
			UnrealAttribution->SetStringField(TEXT("actor"), TEXT("unknown"));
			UnrealAttribution->SetStringField(TEXT("status"), TEXT("requires-explicit-UE-marker-or-shader-map-evidence"));
			UnrealAttribution->SetStringField(TEXT("markerEvidence"), CompactMarkerPath(Event.MarkerPath));
			Hop->SetObjectField(TEXT("ueAttribution"), UnrealAttribution);
			WriterHops.Add(MakeShared<FJsonValueObject>(Hop));
			++WriterHop;
		}
		Graph->SetArrayField(TEXT("targetWriterHops"), MoveTemp(WriterHops));

		TArray<TSharedPtr<FJsonValue>> ResourceEdges;
		TArray<TSharedPtr<FJsonValue>> ChainBreaks;
		for (const TPair<uint32, FEventContextEvidence>& Pair : EventContexts)
		{
			const FEventContextEvidence& Context = Pair.Value;
			for (const TSharedPtr<FJsonValue>& ProvenanceValue : Context.ResourceProvenance)
			{
				const TSharedPtr<FJsonObject> Provenance = ProvenanceValue.IsValid() ? ProvenanceValue->AsObject() : nullptr;
				if (!Provenance.IsValid())
				{
					continue;
				}
				const TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
				Edge->SetStringField(TEXT("edgeType"), TEXT("resource-dependency"));
				Edge->SetNumberField(TEXT("consumerEventId"), Context.EventId);
				FString TextValue;
				double NumberValue = 0.0;
				bool BoolValue = false;
				if (Provenance->TryGetStringField(TEXT("resource"), TextValue)) Edge->SetStringField(TEXT("resource"), TextValue);
				if (Provenance->TryGetNumberField(TEXT("resourceIndex"), NumberValue)) Edge->SetNumberField(TEXT("resourceIndex"), NumberValue);
				if (Provenance->TryGetStringField(TEXT("relation"), TextValue)) Edge->SetStringField(TEXT("relation"), TextValue);
				if (Provenance->TryGetStringField(TEXT("producerStatus"), TextValue)) Edge->SetStringField(TEXT("producerStatus"), TextValue);
				if (Provenance->TryGetStringField(TEXT("producerUsage"), TextValue)) Edge->SetStringField(TEXT("producerUsage"), TextValue);
				if (Provenance->TryGetStringField(TEXT("coordinateMapping"), TextValue)) Edge->SetStringField(TEXT("coordinateMapping"), TextValue);
				if (Provenance->TryGetStringField(TEXT("pixelContribution"), TextValue)) Edge->SetStringField(TEXT("pixelContribution"), TextValue);
				if (Provenance->TryGetStringField(TEXT("pixelTraceStatus"), TextValue)) Edge->SetStringField(TEXT("pixelTraceStatus"), TextValue);
				if (Provenance->TryGetBoolField(TEXT("producerFound"), BoolValue)) Edge->SetBoolField(TEXT("producerFound"), BoolValue);
				if (Provenance->TryGetNumberField(TEXT("producerEventId"), NumberValue)) Edge->SetNumberField(TEXT("producerEventId"), NumberValue);
				if (Provenance->TryGetStringField(TEXT("chainBreak"), TextValue) && !TextValue.IsEmpty())
				{
					Edge->SetStringField(TEXT("chainBreak"), TextValue);
					if (ChainBreaks.Num() < MaxCausalGraphBreaks)
					{
						const TSharedRef<FJsonObject> Break = MakeShared<FJsonObject>();
						Break->SetNumberField(TEXT("eventId"), Context.EventId);
						Break->SetStringField(TEXT("reason"), TextValue);
						ChainBreaks.Add(MakeShared<FJsonValueObject>(Break));
					}
				}
				ResourceEdges.Add(MakeShared<FJsonValueObject>(Edge));
				if (ResourceEdges.Num() >= MaxCausalGraphResourceEdges)
				{
					break;
				}
			}
			if (ResourceEdges.Num() >= MaxCausalGraphResourceEdges)
			{
				break;
			}
		}
		Graph->SetArrayField(TEXT("resourceEdges"), MoveTemp(ResourceEdges));
		Graph->SetArrayField(TEXT("chainBreaks"), MoveTemp(ChainBreaks));
		Graph->SetStringField(TEXT("tracePolicy"), TEXT("bound inputs are candidates; recurse only through bounded producer evidence and never assume same-coordinate mapping"));
		return Graph;
	}
}
