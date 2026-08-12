#include "RenderTrailAnalyzerHomeInternal.h"

namespace UE::RenderTrail::Private
{
	void SAnalyzerHome::AddAgentMessage(const FString& Role, const FString& Content)
	{
		AddAgentMessage(Role, Content, FString());
	}

	void SAnalyzerHome::AddAgentMessage(const FString& Role, const FString& Content, const FString& ReasoningContent)
	{
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("role"), Role);
		Message->SetStringField(TEXT("content"), Content);
		if (!ReasoningContent.IsEmpty())
		{
			Message->SetStringField(TEXT("reasoning_content"), ReasoningContent);
		}
		AgentMessages.Add(MakeShared<FJsonValueObject>(Message));
	}

	TArray<TSharedPtr<FJsonObject>> SAnalyzerHome::SelectAgentResourceHistoriesForCoverage(
		const FEventContextEvidence& Context) const
	{
		struct FHistoryCandidate
		{
			TSharedPtr<FJsonObject> History;
			int32 OriginalIndex = INDEX_NONE;
			int64 Score = 0;
			bool bRequired = false;
			bool bConfirmedWriter = false;
			bool bAssetOrSceneEvent = false;
			bool bNanite = false;
			bool bDepth = false;
			bool bSceneColorOrGBuffer = false;
			bool bBoundary = false;
			bool bConfirmedMapping = false;
		};

		TArray<FHistoryCandidate> Candidates;
		for (int32 HistoryIndex = 0; HistoryIndex < Context.ResourcePixelHistories.Num(); ++HistoryIndex)
		{
			const TSharedPtr<FJsonObject> History = Context.ResourcePixelHistories[HistoryIndex].IsValid()
				? Context.ResourcePixelHistories[HistoryIndex]->AsObject() : nullptr;
			if (!History.IsValid())
			{
				continue;
			}
			FHistoryCandidate Candidate;
			Candidate.History = History;
			Candidate.OriginalIndex = HistoryIndex;
			FString ResourceName;
			FString ShaderBinding;
			FString BranchStatus;
			FString MappingConfidence;
			History->TryGetStringField(TEXT("resourceName"), ResourceName);
			History->TryGetStringField(TEXT("shaderBinding"), ShaderBinding);
			History->TryGetStringField(TEXT("branchStatus"), BranchStatus);
			History->TryGetStringField(TEXT("mappingConfidence"), MappingConfidence);
			History->TryGetBoolField(TEXT("requiredForAgent"), Candidate.bRequired);
			const FString Searchable = ResourceName + TEXT(" ") + ShaderBinding;
			Candidate.bNanite = Searchable.Contains(TEXT("Nanite"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("VisBuffer"), ESearchCase::IgnoreCase);
			Candidate.bDepth = Searchable.Contains(TEXT("Depth"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("HZB"), ESearchCase::IgnoreCase);
			Candidate.bSceneColorOrGBuffer = Searchable.Contains(TEXT("SceneColor"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("GBuffer"), ESearchCase::IgnoreCase);
			Candidate.bBoundary = !BranchStatus.IsEmpty()
				&& BranchStatus != TEXT("continued-to-dominating-writer")
				&& BranchStatus != TEXT("continued-to-pixel-writer")
				&& BranchStatus != TEXT("adaptive-footprint-continued")
				&& BranchStatus != TEXT("no-modification-before-consumer");
			Candidate.bConfirmedMapping = MappingConfidence == TEXT("confirmed-executed-values");

			double SelectedWriterEventId = 0.0;
			Candidate.bConfirmedWriter = History->TryGetNumberField(
				TEXT("selectedWriterEventId"), SelectedWriterEventId) && SelectedWriterEventId > 0.0;
			const TArray<TSharedPtr<FJsonValue>>* EventSummaries = nullptr;
			if (History->TryGetArrayField(TEXT("eventSummaries"), EventSummaries) && EventSummaries)
			{
				for (const TSharedPtr<FJsonValue>& EventValue : *EventSummaries)
				{
					const TSharedPtr<FJsonObject> Event = EventValue.IsValid() ? EventValue->AsObject() : nullptr;
					if (!Event.IsValid())
					{
						continue;
					}
					FString Marker;
					Event->TryGetStringField(TEXT("markerPath"), Marker);
					Candidate.bAssetOrSceneEvent |= Marker.Contains(TEXT("BasePass"), ESearchCase::IgnoreCase)
						|| Marker.Contains(TEXT("PrePass"), ESearchCase::IgnoreCase)
						|| Marker.Contains(TEXT("GBuffer"), ESearchCase::IgnoreCase)
						|| Marker.Contains(TEXT("/Game/"), ESearchCase::IgnoreCase)
						|| Marker.Contains(TEXT("/Engine/"), ESearchCase::IgnoreCase)
						|| Marker.Contains(TEXT(" SM_"), ESearchCase::IgnoreCase);
				}
			}

			Candidate.Score += Candidate.bRequired ? 150000 : 0;
			Candidate.Score += Candidate.bConfirmedWriter ? 130000 : 0;
			Candidate.Score += Candidate.bAssetOrSceneEvent ? 110000 : 0;
			Candidate.Score += Candidate.bConfirmedMapping ? 100000 : 0;
			Candidate.Score += Candidate.bNanite ? 90000 : 0;
			Candidate.Score += Candidate.bDepth ? 80000 : 0;
			Candidate.Score += Candidate.bSceneColorOrGBuffer ? 70000 : 0;
			Candidate.Score += Candidate.bBoundary ? 60000 : 0;
			Candidate.Score -= HistoryIndex;
			Candidates.Add(MoveTemp(Candidate));
		}

		Candidates.Sort([](const FHistoryCandidate& A, const FHistoryCandidate& B)
		{
			return A.Score == B.Score ? A.OriginalIndex < B.OriginalIndex : A.Score > B.Score;
		});
		TArray<TSharedPtr<FJsonObject>> Selected;
		auto AddBest = [&Candidates, &Selected](TFunctionRef<bool(const FHistoryCandidate&)> Predicate)
		{
			for (const FHistoryCandidate& Candidate : Candidates)
			{
				if (Predicate(Candidate) && !Selected.Contains(Candidate.History))
				{
					Selected.Add(Candidate.History);
					return;
				}
			}
		};
		AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bRequired; });
		AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bConfirmedWriter; });
		AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bAssetOrSceneEvent; });
		AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bNanite; });
		AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bDepth; });
		AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bSceneColorOrGBuffer; });
		AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bBoundary; });
		AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bConfirmedMapping; });
		for (const FHistoryCandidate& Candidate : Candidates)
		{
			if (Selected.Num() >= MaxAgentResourceHistoriesPerContext)
			{
				break;
			}
			Selected.AddUnique(Candidate.History);
		}
		if (Selected.Num() > MaxAgentResourceHistoriesPerContext)
		{
			Selected.SetNum(MaxAgentResourceHistoriesPerContext, EAllowShrinking::No);
		}
		return Selected;
	}

	TArray<int32> SAnalyzerHome::SelectAgentBoundResourcesForCoverage(
		const TArray<FBoundResourceEvidence>& Resources, int32 MaxResources)
	{
		struct FResourceCandidate
		{
			int32 Index = INDEX_NONE;
			int64 Score = 0;
			bool bSceneColor = false;
			bool bDepth = false;
			bool bGBuffer = false;
			bool bNanite = false;
			bool bGpuSceneOrMaterial = false;
		};
		TArray<FResourceCandidate> Candidates;
		for (int32 Index = 0; Index < Resources.Num(); ++Index)
		{
			const FBoundResourceEvidence& Resource = Resources[Index];
			const FString Searchable = Resource.Name + TEXT(" ") + Resource.ShaderBinding;
			FResourceCandidate Candidate;
			Candidate.Index = Index;
			Candidate.bSceneColor = Searchable.Contains(TEXT("SceneColor"), ESearchCase::IgnoreCase);
			Candidate.bDepth = Searchable.Contains(TEXT("Depth"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("HZB"), ESearchCase::IgnoreCase);
			Candidate.bGBuffer = Searchable.Contains(TEXT("GBuffer"), ESearchCase::IgnoreCase);
			Candidate.bNanite = Searchable.Contains(TEXT("Nanite"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("VisBuffer"), ESearchCase::IgnoreCase);
			Candidate.bGpuSceneOrMaterial = Searchable.Contains(TEXT("GPUScene"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("PrimitiveData"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("Material"), ESearchCase::IgnoreCase);
			Candidate.Score += Candidate.bSceneColor ? 100000 : 0;
			Candidate.Score += Candidate.bDepth ? 95000 : 0;
			Candidate.Score += Candidate.bGBuffer ? 90000 : 0;
			Candidate.Score += Candidate.bNanite ? 85000 : 0;
			Candidate.Score += Candidate.bGpuSceneOrMaterial ? 80000 : 0;
			Candidate.Score += Resource.bTexture ? 10000 : 0;
			Candidate.Score -= Index;
			Candidates.Add(Candidate);
		}
		Candidates.Sort([](const FResourceCandidate& A, const FResourceCandidate& B)
		{
			return A.Score == B.Score ? A.Index < B.Index : A.Score > B.Score;
		});
		TArray<int32> Selected;
		auto AddBest = [&Candidates, &Selected](TFunctionRef<bool(const FResourceCandidate&)> Predicate)
		{
			for (const FResourceCandidate& Candidate : Candidates)
			{
				if (Predicate(Candidate) && !Selected.Contains(Candidate.Index))
				{
					Selected.Add(Candidate.Index);
					return;
				}
			}
		};
		AddBest([](const FResourceCandidate& Candidate) { return Candidate.bSceneColor; });
		AddBest([](const FResourceCandidate& Candidate) { return Candidate.bDepth; });
		AddBest([](const FResourceCandidate& Candidate) { return Candidate.bGBuffer; });
		AddBest([](const FResourceCandidate& Candidate) { return Candidate.bNanite; });
		AddBest([](const FResourceCandidate& Candidate) { return Candidate.bGpuSceneOrMaterial; });
		for (const FResourceCandidate& Candidate : Candidates)
		{
			if (Selected.Num() >= MaxResources)
			{
				break;
			}
			Selected.AddUnique(Candidate.Index);
		}
		if (Selected.Num() > MaxResources)
		{
			Selected.SetNum(MaxResources, EAllowShrinking::No);
		}
		return Selected;
	}

	TArray<TSharedPtr<FJsonObject>> SAnalyzerHome::SelectAgentResourceProvenanceForCoverage(
		const FEventContextEvidence& Context) const
	{
		struct FProvenanceCandidate
		{
			TSharedPtr<FJsonObject> Provenance;
			int32 OriginalIndex = INDEX_NONE;
			int64 Score = 0;
			bool bAsset = false;
			bool bVisBuffer = false;
			bool bGpuScene = false;
			bool bNanite = false;
			bool bSceneStage = false;
			bool bConfirmedProducer = false;
			bool bChainBreak = false;
			bool bConfirmedMapping = false;
		};
		TArray<FProvenanceCandidate> Candidates;
		for (int32 Index = 0; Index < Context.ResourceProvenance.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Provenance = Context.ResourceProvenance[Index].IsValid()
				? Context.ResourceProvenance[Index]->AsObject() : nullptr;
			if (!Provenance.IsValid())
			{
				continue;
			}
			FString ResourceName;
			FString ShaderBinding;
			FString ProducerMarker;
			FString ProducerStatus;
			FString ChainBreak;
			FString MappingConfidence;
			bool bProducerFound = false;
			Provenance->TryGetStringField(TEXT("resource"), ResourceName);
			Provenance->TryGetStringField(TEXT("shaderBinding"), ShaderBinding);
			Provenance->TryGetStringField(TEXT("producerMarkerPath"), ProducerMarker);
			Provenance->TryGetStringField(TEXT("producerStatus"), ProducerStatus);
			Provenance->TryGetStringField(TEXT("chainBreak"), ChainBreak);
			Provenance->TryGetStringField(TEXT("mappingConfidence"), MappingConfidence);
			Provenance->TryGetBoolField(TEXT("producerFound"), bProducerFound);
			const FString Searchable = ResourceName + TEXT(" ") + ShaderBinding + TEXT(" ") + ProducerMarker;
			FProvenanceCandidate Candidate;
			Candidate.Provenance = Provenance;
			Candidate.OriginalIndex = Index;
			Candidate.bAsset = Searchable.Contains(TEXT("/Game/"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT(" SM_"), ESearchCase::IgnoreCase);
			Candidate.bVisBuffer = Searchable.Contains(TEXT("VisBuffer"), ESearchCase::IgnoreCase);
			Candidate.bGpuScene = Searchable.Contains(TEXT("GPUScene"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("PrimitiveData"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("InstanceSceneData"), ESearchCase::IgnoreCase);
			Candidate.bNanite = Searchable.Contains(TEXT("Nanite"), ESearchCase::IgnoreCase);
			Candidate.bSceneStage = Searchable.Contains(TEXT("BasePass"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("PrePass"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("GBuffer"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("SceneDepth"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("SceneColor"), ESearchCase::IgnoreCase);
			Candidate.bConfirmedProducer = bProducerFound && ProducerStatus == TEXT("confirmed-resource-write");
			Candidate.bChainBreak = !ChainBreak.IsEmpty() || (!ProducerStatus.IsEmpty()
				&& ProducerStatus != TEXT("confirmed-resource-write"));
			Candidate.bConfirmedMapping = MappingConfidence == TEXT("confirmed-executed-values");
			Candidate.Score += Candidate.bAsset ? 120000 : 0;
			Candidate.Score += Candidate.bVisBuffer ? 115000 : 0;
			Candidate.Score += Candidate.bGpuScene ? 110000 : 0;
			Candidate.Score += Candidate.bNanite ? 100000 : 0;
			Candidate.Score += Candidate.bSceneStage ? 95000 : 0;
			Candidate.Score += Candidate.bConfirmedMapping ? 90000 : 0;
			Candidate.Score += Candidate.bConfirmedProducer ? 80000 : 0;
			Candidate.Score += Candidate.bChainBreak ? 50000 : 0;
			Candidate.Score -= Index;
			Candidates.Add(MoveTemp(Candidate));
		}
		Candidates.Sort([](const FProvenanceCandidate& A, const FProvenanceCandidate& B)
		{
			return A.Score == B.Score ? A.OriginalIndex < B.OriginalIndex : A.Score > B.Score;
		});
		TArray<TSharedPtr<FJsonObject>> Selected;
		auto AddBest = [&Candidates, &Selected](TFunctionRef<bool(const FProvenanceCandidate&)> Predicate)
		{
			for (const FProvenanceCandidate& Candidate : Candidates)
			{
				if (Predicate(Candidate) && !Selected.Contains(Candidate.Provenance))
				{
					Selected.Add(Candidate.Provenance);
					return;
				}
			}
		};
		AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bAsset; });
		AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bVisBuffer; });
		AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bGpuScene; });
		AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bNanite; });
		AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bSceneStage; });
		AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bConfirmedMapping; });
		AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bConfirmedProducer; });
		AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bChainBreak; });
		for (const FProvenanceCandidate& Candidate : Candidates)
		{
			if (Selected.Num() >= MaxAgentBoundResourcesPerContext)
			{
				break;
			}
			Selected.AddUnique(Candidate.Provenance);
		}
		if (Selected.Num() > MaxAgentBoundResourcesPerContext)
		{
			Selected.SetNum(MaxAgentBoundResourcesPerContext, EAllowShrinking::No);
		}
		return Selected;
	}

	TSharedRef<FJsonObject> SAnalyzerHome::BuildCompactResourceHistoryForAgent(const TSharedPtr<FJsonObject>& History) const
	{
		TSharedRef<FJsonObject> Compact = MakeShared<FJsonObject>();
		if (!History.IsValid())
		{
			Compact->SetStringField(TEXT("branchStatus"), TEXT("invalid-history"));
			return Compact;
		}
		static const TCHAR* StringFields[] = {
			TEXT("resourceName"), TEXT("shaderBinding"), TEXT("coordinateMapping"), TEXT("mappingConfidence"),
			TEXT("coordinateEvidence"), TEXT("branchStatus"), TEXT("detail"), TEXT("error"), TEXT("shaderAccessDisassembly"),
			TEXT("tracePurpose"), TEXT("selectedWriterReason"), TEXT("resetBoundaryReason"),
			TEXT("adaptiveNextTraceKey") };
		for (const TCHAR* Field : StringFields)
		{
			FString Value;
			if (History->TryGetStringField(Field, Value))
			{
				Compact->SetStringField(Field, Value.Left(800));
			}
		}
		static const TCHAR* NumberFields[] = {
			TEXT("consumerEventId"), TEXT("resourceIndex"), TEXT("x"), TEXT("y"), TEXT("mip"), TEXT("slice"),
			TEXT("sample"), TEXT("totalModifications"), TEXT("totalEvents"), TEXT("shaderAccessInstruction"),
			TEXT("queueToResponseSeconds"), TEXT("selectedWriterEventId"), TEXT("dominatedWriterCount"),
			TEXT("resetBoundaryEventId"), TEXT("collapsedShaderAccessCount"), TEXT("adaptiveAttempt"),
			TEXT("adaptiveCandidateCount"), TEXT("adaptiveCandidatesRemaining"), TEXT("adaptiveNextX"), TEXT("adaptiveNextY") };
		for (const TCHAR* Field : NumberFields)
		{
			double Value = 0.0;
			if (History->TryGetNumberField(Field, Value))
			{
				Compact->SetNumberField(Field, Value);
			}
		}
		static const TCHAR* BoolFields[] = {
			TEXT("requiredForAgent"), TEXT("detailTailTruncated"), TEXT("shaderAccessObserved"),
			TEXT("shaderCoordinateValuesMatched"), TEXT("shaderFootprintDerivedFromExecutedValues"),
			TEXT("executedShaderAccess") };
		for (const TCHAR* Field : BoolFields)
		{
			bool bValue = false;
			if (History->TryGetBoolField(Field, bValue))
			{
				Compact->SetBoolField(Field, bValue);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Aliases = nullptr;
		if (History->TryGetArrayField(TEXT("shaderBindingAliases"), Aliases) && Aliases)
		{
			Compact->SetArrayField(TEXT("shaderBindingAliases"), *Aliases);
		}
		const TArray<TSharedPtr<FJsonValue>>* WriterIds = nullptr;
		if (History->TryGetArrayField(TEXT("confirmedWriterEventIds"), WriterIds) && WriterIds)
		{
			Compact->SetArrayField(TEXT("confirmedWriterEventIds"), *WriterIds);
		}
		const TSharedPtr<FJsonObject>* ShaderAccessResult = nullptr;
		if (History->TryGetObjectField(TEXT("shaderAccessResult"), ShaderAccessResult) && ShaderAccessResult)
		{
			Compact->SetObjectField(TEXT("shaderAccessResult"), *ShaderAccessResult);
		}

		const TArray<TSharedPtr<FJsonValue>>* EventSummaries = nullptr;
		TArray<TSharedPtr<FJsonValue>> CompactEvents;
		if (History->TryGetArrayField(TEXT("eventSummaries"), EventSummaries) && EventSummaries)
		{
			TArray<int32> SelectedIndices;
			if (EventSummaries->Num() <= MaxAgentEventsPerResourceHistory)
			{
				for (int32 Index = 0; Index < EventSummaries->Num(); ++Index)
				{
					SelectedIndices.Add(Index);
				}
			}
			else
			{
				struct FScoredEventIndex
				{
					int32 Index = INDEX_NONE;
					int32 Score = 0;
				};
				TArray<FScoredEventIndex> ScoredIndices;
				for (int32 Index = 0; Index < EventSummaries->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> Event = (*EventSummaries)[Index].IsValid()
						? (*EventSummaries)[Index]->AsObject() : nullptr;
					if (!Event.IsValid())
					{
						continue;
					}
					FString Marker;
					FString ActionKind;
					bool bChanged = false;
					double Passed = 0.0;
					Event->TryGetStringField(TEXT("markerPath"), Marker);
					Event->TryGetStringField(TEXT("actionKind"), ActionKind);
					Event->TryGetBoolField(TEXT("changedTextureValue"), bChanged);
					Event->TryGetNumberField(TEXT("passedFragments"), Passed);
					int32 Score = Index;
					Score += bChanged ? 80 : 0;
					Score += Passed > 0.0 ? 40 : 0;
					Score += (ActionKind == TEXT("draw") || ActionKind == TEXT("dispatch")) ? 30 : 0;
					Score += (Marker.Contains(TEXT("BasePass")) || Marker.Contains(TEXT("PrePass"))
						|| Marker.Contains(TEXT("DepthPass")) || Marker.Contains(TEXT("GBuffer"))) ? 220 : 0;
					Score += Marker.Contains(TEXT("Nanite")) ? 140 : 0;
					Score += (Marker.Contains(TEXT("SM_")) || Marker.Contains(TEXT("/Game/"))
						|| Marker.Contains(TEXT("/Engine/"))) ? 100 : 0;
					Score -= ActionKind == TEXT("clear") ? 160 : 0;
					FScoredEventIndex Scored;
					Scored.Index = Index;
					Scored.Score = Score;
					ScoredIndices.Add(MoveTemp(Scored));
				}
				ScoredIndices.Sort([](const FScoredEventIndex& A, const FScoredEventIndex& B)
				{
					return A.Score == B.Score ? A.Index > B.Index : A.Score > B.Score;
				});
				double SelectedWriterEventId = 0.0;
				History->TryGetNumberField(TEXT("selectedWriterEventId"), SelectedWriterEventId);
				for (int32 Index = 0; Index < EventSummaries->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> Event = (*EventSummaries)[Index].IsValid()
						? (*EventSummaries)[Index]->AsObject() : nullptr;
					double EventId = 0.0;
					if (Event.IsValid() && Event->TryGetNumberField(TEXT("eventId"), EventId)
						&& static_cast<uint32>(EventId) == static_cast<uint32>(SelectedWriterEventId))
					{
						SelectedIndices.AddUnique(Index);
						break;
					}
				}
				SelectedIndices.AddUnique(EventSummaries->Num() - 1);
				for (const FScoredEventIndex& Scored : ScoredIndices)
				{
					SelectedIndices.AddUnique(Scored.Index);
					if (SelectedIndices.Num() >= MaxAgentEventsPerResourceHistory)
					{
						break;
					}
				}
				SelectedIndices.Sort();
			}
			for (const int32 Index : SelectedIndices)
			{
				const TSharedPtr<FJsonObject> Event = (*EventSummaries)[Index].IsValid()
					? (*EventSummaries)[Index]->AsObject() : nullptr;
				if (!Event.IsValid())
				{
					continue;
				}
				TSharedRef<FJsonObject> CompactEvent = MakeShared<FJsonObject>();
				double Number = 0.0;
				FString TextValue;
				bool bValue = false;
				if (Event->TryGetNumberField(TEXT("eventId"), Number)) CompactEvent->SetNumberField(TEXT("eventId"), Number);
				if (Event->TryGetNumberField(TEXT("passedFragments"), Number)) CompactEvent->SetNumberField(TEXT("passedFragments"), Number);
				if (Event->TryGetNumberField(TEXT("rejectedFragments"), Number)) CompactEvent->SetNumberField(TEXT("rejectedFragments"), Number);
				if (Event->TryGetStringField(TEXT("action"), TextValue)) CompactEvent->SetStringField(TEXT("action"), TextValue);
				if (Event->TryGetStringField(TEXT("actionKind"), TextValue)) CompactEvent->SetStringField(TEXT("actionKind"), TextValue);
				if (Event->TryGetStringField(TEXT("markerPath"), TextValue)) CompactEvent->SetStringField(TEXT("marker"), CompactMarkerPath(TextValue));
				if (Event->TryGetBoolField(TEXT("changedTextureValue"), bValue)) CompactEvent->SetBoolField(TEXT("changedTextureValue"), bValue);
				if (Event->TryGetBoolField(TEXT("directShaderWrite"), bValue)) CompactEvent->SetBoolField(TEXT("directShaderWrite"), bValue);
				if (Event->TryGetBoolField(TEXT("hasPrimitiveEvidence"), bValue)) CompactEvent->SetBoolField(TEXT("hasPrimitiveEvidence"), bValue);
				if (Event->TryGetNumberField(TEXT("lastPrimitiveId"), Number)) CompactEvent->SetNumberField(TEXT("lastPrimitiveId"), Number);
				const TArray<TSharedPtr<FJsonValue>>* FailureReasons = nullptr;
				if (Event->TryGetArrayField(TEXT("failureReasons"), FailureReasons) && FailureReasons)
				{
					CompactEvent->SetArrayField(TEXT("failureReasons"), *FailureReasons);
				}
				const TSharedPtr<FJsonObject>* PixelValue = nullptr;
				if (Event->TryGetObjectField(TEXT("firstBefore"), PixelValue) && PixelValue) CompactEvent->SetObjectField(TEXT("firstBefore"), *PixelValue);
				if (Event->TryGetObjectField(TEXT("lastShaderOutput"), PixelValue) && PixelValue) CompactEvent->SetObjectField(TEXT("lastShaderOutput"), *PixelValue);
				if (Event->TryGetObjectField(TEXT("lastAfter"), PixelValue) && PixelValue) CompactEvent->SetObjectField(TEXT("lastAfter"), *PixelValue);
				CompactEvents.Add(MakeShared<FJsonValueObject>(CompactEvent));
			}
		}
		Compact->SetStringField(TEXT("eventSummarySelection"),
			TEXT("causal-priority-selection; full ordered history remains in technical evidence"));
		Compact->SetArrayField(TEXT("selectedEventSummaries"), MoveTemp(CompactEvents));
		return Compact;
	}

	TSharedRef<FJsonObject> SAnalyzerHome::BuildCompactShaderDebugForAgent(const TSharedPtr<FJsonObject>& Trace) const
	{
		TSharedRef<FJsonObject> Compact = MakeShared<FJsonObject>();
		if (!Trace.IsValid())
		{
			return Compact;
		}
		static const TCHAR* StringFields[] = { TEXT("stage"), TEXT("debugStatus") };
		for (const TCHAR* Field : StringFields)
		{
			FString Value;
			if (Trace->TryGetStringField(Field, Value)) Compact->SetStringField(Field, Value);
		}
		static const TCHAR* NumberFields[] = { TEXT("eventId"), TEXT("x"), TEXT("y"), TEXT("sample"),
			TEXT("instructionInfoCount"), TEXT("sourceVariableMappingCount"), TEXT("stepCount"), TEXT("textureAccessCount") };
		for (const TCHAR* Field : NumberFields)
		{
			double Value = 0.0;
			if (Trace->TryGetNumberField(Field, Value)) Compact->SetNumberField(Field, Value);
		}
		bool bCompleted = false;
		if (Trace->TryGetBoolField(TEXT("completed"), bCompleted)) Compact->SetBoolField(TEXT("completed"), bCompleted);
		const TArray<TSharedPtr<FJsonValue>>* Accesses = nullptr;
		TArray<TSharedPtr<FJsonValue>> CompactAccesses;
		if (Trace->TryGetArrayField(TEXT("textureAccesses"), Accesses) && Accesses)
		{
			for (int32 Index = 0; Index < Accesses->Num() && Index < MaxAgentTextureAccesses; ++Index)
			{
				const TSharedPtr<FJsonObject> Access = (*Accesses)[Index].IsValid() ? (*Accesses)[Index]->AsObject() : nullptr;
				if (!Access.IsValid()) continue;
				TSharedRef<FJsonObject> CompactAccess = MakeShared<FJsonObject>();
				double Number = 0.0;
				FString Disassembly;
				if (Access->TryGetNumberField(TEXT("instruction"), Number)) CompactAccess->SetNumberField(TEXT("instruction"), Number);
				if (Access->TryGetStringField(TEXT("disassembly"), Disassembly)) CompactAccess->SetStringField(TEXT("disassembly"), Disassembly.Left(600));
				const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
				TArray<TSharedPtr<FJsonValue>> CompactVariables;
				if (Access->TryGetArrayField(TEXT("variables"), Variables) && Variables)
				{
					for (const TSharedPtr<FJsonValue>& VariableValue : *Variables)
					{
						const TSharedPtr<FJsonObject> Variable = VariableValue.IsValid() ? VariableValue->AsObject() : nullptr;
						if (!Variable.IsValid()) continue;
						TSharedRef<FJsonObject> CompactVariable = MakeShared<FJsonObject>();
						FString Name;
						if (Variable->TryGetStringField(TEXT("name"), Name)) CompactVariable->SetStringField(TEXT("name"), Name);
						const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
						if (Variable->TryGetArrayField(TEXT("values"), Values) && Values) CompactVariable->SetArrayField(TEXT("values"), *Values);
						CompactVariables.Add(MakeShared<FJsonValueObject>(CompactVariable));
					}
				}
				CompactAccess->SetArrayField(TEXT("variables"), MoveTemp(CompactVariables));
				CompactAccesses.Add(MakeShared<FJsonValueObject>(CompactAccess));
			}
		}
		Compact->SetArrayField(TEXT("textureAccesses"), MoveTemp(CompactAccesses));
		return Compact;
	}

	TSharedRef<FJsonObject> SAnalyzerHome::BuildCompactResourceProvenanceForAgent(
		const TSharedPtr<FJsonObject>& Provenance) const
	{
		TSharedRef<FJsonObject> Compact = MakeShared<FJsonObject>();
		if (!Provenance.IsValid())
		{
			return Compact;
		}
		static const TCHAR* StringFields[] = {
			TEXT("resource"), TEXT("shaderBinding"), TEXT("readEvidence"), TEXT("pixelContribution"),
			TEXT("pixelTraceStatus"), TEXT("dimensionRelation"), TEXT("coordinateMapping"),
			TEXT("producerUsage"), TEXT("producerStatus"), TEXT("relation"), TEXT("producerAction"),
			TEXT("producerKind"), TEXT("chainBreak") };
		for (const TCHAR* Field : StringFields)
		{
			FString Value;
			if (Provenance->TryGetStringField(Field, Value))
			{
				Compact->SetStringField(Field, Value.Left(800));
			}
		}
		static const TCHAR* NumberFields[] = {
			TEXT("resourceIndex"), TEXT("producerEventId"), TEXT("invalidatingEventId") };
		for (const TCHAR* Field : NumberFields)
		{
			double Value = 0.0;
			if (Provenance->TryGetNumberField(Field, Value))
			{
				Compact->SetNumberField(Field, Value);
			}
		}
		bool bProducerFound = false;
		if (Provenance->TryGetBoolField(TEXT("producerFound"), bProducerFound))
		{
			Compact->SetBoolField(TEXT("producerFound"), bProducerFound);
		}
		return Compact;
	}

	FString SAnalyzerHome::BuildAgentPrefilterEvidence() const
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("evidenceType"), TEXT("prefiltered_pixel_forensics"));
		Root->SetStringField(TEXT("capture"), FPaths::GetCleanFilename(GetCapturePath()));
		Root->SetStringField(TEXT("ruleSummary"), LastReportSummary);
		Root->SetStringField(TEXT("boundedCausalPath"), LastReportCausalPath);
		TSharedRef<FJsonObject> ReplayTarget = MakeShared<FJsonObject>();
		ReplayTarget->SetNumberField(TEXT("resourceIndex"), ReplayTargetResourceIndex);
		ReplayTarget->SetNumberField(TEXT("width"), CurrentPreviewSize.X);
		ReplayTarget->SetNumberField(TEXT("height"), CurrentPreviewSize.Y);
		ReplayTarget->SetNumberField(TEXT("samples"), ReplayTargetSamples);
		ReplayTarget->SetStringField(TEXT("format"), ReplayTargetFormat);
		Root->SetObjectField(TEXT("replayTarget"), ReplayTarget);

		FString MetadataJson;
		UE::RenderTrail::FCaptureMetadata Metadata;
		FString MetadataError;
		if (FFileHelper::LoadFileToString(MetadataJson, *UE::RenderTrail::GetMetadataPathForCapture(GetCapturePath()))
			&& UE::RenderTrail::FCaptureMetadata::FromJson(MetadataJson, Metadata, MetadataError))
		{
			TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
			Context->SetStringField(TEXT("project"), Metadata.ProjectName);
			Context->SetStringField(TEXT("map"), Metadata.MapName);
			Context->SetStringField(TEXT("engine"), Metadata.EngineVersion);
			Context->SetBoolField(TEXT("pie"), Metadata.bIsPIE);
			Root->SetObjectField(TEXT("ueContext"), Context);
		}

		TArray<TSharedPtr<FJsonValue>> SampleValues;
		for (int32 SampleIndex = 0; SampleIndex < Samples.Num(); ++SampleIndex)
		{
			const FPixelSample& Sample = Samples[SampleIndex];
			TSharedRef<FJsonObject> SampleJson = MakeShared<FJsonObject>();
			const FString Label = FString::Printf(TEXT("P%d"), SampleIndex + 1);
			SampleJson->SetStringField(TEXT("label"), Label);
			SampleJson->SetStringField(TEXT("role"), TEXT("point_of_interest"));
			SampleJson->SetNumberField(TEXT("x"), Sample.Pixel.X);
			SampleJson->SetNumberField(TEXT("y"), Sample.Pixel.Y);
			SampleJson->SetNumberField(TEXT("modificationCount"), Sample.TotalModifications);
			const TArray<FEventEvidence> Events = AggregateEvents(Sample);
			const FString FinalObservedValue = !Sample.Modifications.IsEmpty()
				? Sample.Modifications.Last().After
				: (!Events.IsEmpty() ? Events.Last().After : TEXT("unavailable"));
			SampleJson->SetStringField(TEXT("finalObservedValue"), FinalObservedValue);
			SampleJson->SetBoolField(TEXT("detailTailTruncated"), Sample.bTruncated);

			TArray<TSharedPtr<FJsonValue>> EventValues;
			const int32 First = FMath::Max(0, Events.Num() - MaxAgentPrefilterEventsPerSample);
			for (int32 Index = Events.Num() - 1; Index >= First; --Index)
			{
				const FEventEvidence& Event = Events[Index];
				TSharedRef<FJsonObject> EventJson = MakeShared<FJsonObject>();
				EventJson->SetNumberField(TEXT("eventId"), Event.EventId);
				EventJson->SetStringField(TEXT("kind"), Event.ActionKind);
				EventJson->SetStringField(TEXT("action"), Event.Action);
				EventJson->SetStringField(TEXT("marker"), CompactMarkerPath(Event.MarkerPath));
				EventJson->SetNumberField(TEXT("actionFlags"), Event.ActionFlags);
				EventJson->SetStringField(TEXT("result"), DescribeEventResult(Event));
				EventJson->SetNumberField(TEXT("passedFragments"), Event.PassedFragments);
				EventJson->SetNumberField(TEXT("rejectedFragments"), Event.RejectedFragments);
				AddColorDeltaJson(EventJson, Event);
				EventValues.Add(MakeShared<FJsonValueObject>(EventJson));
			}
			SampleJson->SetArrayField(TEXT("latestRelevantEvents"), EventValues);
			TArray<TSharedPtr<FJsonValue>> CompleteEventChain;
			const int32 AgentChainFirst = FMath::Max(0, Events.Num() - MaxAgentEventChainPerSample);
			CompleteEventChain.Reserve(Events.Num() - AgentChainFirst);
			for (int32 EventIndex = AgentChainFirst; EventIndex < Events.Num(); ++EventIndex)
			{
				const FEventEvidence& Event = Events[EventIndex];
				TSharedRef<FJsonObject> EventJson = MakeShared<FJsonObject>();
				EventJson->SetNumberField(TEXT("eventId"), Event.EventId);
				EventJson->SetStringField(TEXT("kind"), Event.ActionKind);
				EventJson->SetStringField(TEXT("action"), Event.Action);
				EventJson->SetStringField(TEXT("marker"), CompactMarkerPath(Event.MarkerPath));
				EventJson->SetNumberField(TEXT("actionFlags"), Event.ActionFlags);
				EventJson->SetStringField(TEXT("semantics"), ClassifySemantics(Event));
				EventJson->SetStringField(TEXT("result"), DescribeEventResult(Event));
				EventJson->SetStringField(TEXT("before"), Event.Before);
				EventJson->SetStringField(TEXT("shaderOutput"), Event.ShaderOutput);
				EventJson->SetStringField(TEXT("after"), Event.After);
				EventJson->SetNumberField(TEXT("passedFragments"), Event.PassedFragments);
				EventJson->SetNumberField(TEXT("rejectedFragments"), Event.RejectedFragments);
				EventJson->SetBoolField(TEXT("directShaderWrite"), Event.bDirectShaderWrite);
				EventJson->SetBoolField(TEXT("changedTextureValue"), Event.bChangedTextureValue);
				EventJson->SetBoolField(TEXT("hasPrimitiveEvidence"), Event.bHasPrimitiveEvidence);
				EventJson->SetNumberField(TEXT("primitiveId"), Event.PrimitiveId);
				AddColorDeltaJson(EventJson, Event);
				TArray<TSharedPtr<FJsonValue>> FailureValues;
				for (const FString& Failure : Event.FailureReasons)
				{
					FailureValues.Add(MakeShared<FJsonValueString>(Failure));
				}
				EventJson->SetArrayField(TEXT("failureReasons"), MoveTemp(FailureValues));
				CompleteEventChain.Add(MakeShared<FJsonValueObject>(EventJson));
			}
			SampleJson->SetArrayField(TEXT("completeEventChain"), MoveTemp(CompleteEventChain));
			SampleJson->SetBoolField(TEXT("eventChainComplete"), Sample.bEventSummaryComplete && AgentChainFirst == 0);
			SampleJson->SetNumberField(TEXT("eventChainEventCount"), Events.Num());
			SampleJson->SetNumberField(TEXT("eventChainStartIndex"), AgentChainFirst);
			TSharedRef<FJsonObject> AgentCausalGraph =
				BuildPixelCausalGraph(Sample, Events, EventContexts, MaxDisplayedFrontierResources);
			const TArray<TSharedPtr<FJsonValue>>* WriterHops = nullptr;
			if (AgentCausalGraph->TryGetArrayField(TEXT("targetWriterHops"), WriterHops) && WriterHops)
			{
				for (const TSharedPtr<FJsonValue>& HopValue : *WriterHops)
				{
					const TSharedPtr<FJsonObject> Hop = HopValue.IsValid() ? HopValue->AsObject() : nullptr;
					if (!Hop.IsValid()) continue;
					const TArray<TSharedPtr<FJsonValue>>* Histories = nullptr;
					if (Hop->TryGetArrayField(TEXT("resourcePixelHistories"), Histories) && Histories)
					{
						Hop->SetNumberField(TEXT("resourcePixelHistoryCount"), Histories->Num());
						Hop->RemoveField(TEXT("resourcePixelHistories"));
					}
				}
			}
			SampleJson->SetObjectField(TEXT("causalGraph"), AgentCausalGraph);
			SampleValues.Add(MakeShared<FJsonValueObject>(SampleJson));
		}
		Root->SetArrayField(TEXT("samples"), SampleValues);

		const TArray<FCausalLaneEvidence> CausalLanes =
			BuildCausalLaneEvidence(EventContexts, EventContextDepths);
		TArray<TSharedPtr<FJsonValue>> CausalLaneValues;
		for (const FCausalLaneEvidence& Lane : CausalLanes)
		{
			const TSharedRef<FJsonObject> LaneJson = MakeShared<FJsonObject>();
			LaneJson->SetStringField(TEXT("kind"), Lane.TracePurpose);
			LaneJson->SetStringField(TEXT("meaning"),
				Lane.TracePurpose == TEXT("geometry")
					? TEXT("geometry/visibility ownership; not an RGB chronology")
					: (Lane.TracePurpose == TEXT("overlay")
						? TEXT("editor overlay/composite resources; separate from scene color and geometry")
						: TEXT("color-resource producer DAG; structural edges do not alone prove final pixel contribution")));
			LaneJson->SetNumberField(TEXT("groupedBranchCount"), Lane.Branches.Num());
			LaneJson->SetNumberField(TEXT("evidenceRecordCount"), Lane.EvidenceRecordCount);
			LaneJson->SetNumberField(TEXT("queryRecordCount"), Lane.QueryRecordCount);
			LaneJson->SetNumberField(TEXT("confirmedProducerCount"), Lane.ConfirmedProducerCount);
			LaneJson->SetNumberField(TEXT("resetBoundaryCount"), Lane.ResetBoundaryCount);
			LaneJson->SetNumberField(TEXT("unresolvedBoundaryCount"), Lane.UnresolvedBoundaryCount);
			LaneJson->SetStringField(TEXT("selectionPolicy"),
				TEXT("all collected contexts; duplicate MSAA samples and adaptive coordinates grouped by consumer/resource/producer"));

			TArray<TSharedPtr<FJsonValue>> BranchValues;
			const int32 IncludedBranches = FMath::Min(Lane.Branches.Num(), MaxAgentCausalLaneBranches);
			for (int32 BranchIndex = 0; BranchIndex < IncludedBranches; ++BranchIndex)
			{
				const FCausalLaneBranchEvidence& Branch = Lane.Branches[BranchIndex];
				const TSharedRef<FJsonObject> BranchJson = MakeShared<FJsonObject>();
				BranchJson->SetNumberField(TEXT("consumerEventId"), Branch.ConsumerEventId);
				BranchJson->SetNumberField(TEXT("producerEventId"), Branch.ProducerEventId);
				BranchJson->SetNumberField(TEXT("resetBoundaryEventId"), Branch.ResetBoundaryEventId);
				BranchJson->SetNumberField(TEXT("resourceIndex"), Branch.ResourceIndex);
				BranchJson->SetNumberField(TEXT("reverseDepth"), Branch.ReverseDepth);
				BranchJson->SetStringField(TEXT("resource"), Branch.ResourceName);
				BranchJson->SetStringField(TEXT("shaderBinding"), Branch.ShaderBinding);
				BranchJson->SetStringField(TEXT("resourceAccess"), Branch.ResourceAccess);
				BranchJson->SetStringField(TEXT("branchStatus"), Branch.BranchStatus);
				BranchJson->SetStringField(TEXT("mappingConfidence"), Branch.MappingConfidence);
				BranchJson->SetStringField(TEXT("edgeRole"), Branch.EdgeRole);
				BranchJson->SetStringField(TEXT("edgeConfidence"), Branch.EdgeConfidence);
				BranchJson->SetStringField(TEXT("executedSampleValue"), Branch.ExecutedSampleValue);
				BranchJson->SetStringField(TEXT("producerBeforeValue"), Branch.ProducerBeforeValue);
				BranchJson->SetStringField(TEXT("producerShaderOutputValue"), Branch.ProducerShaderOutputValue);
				BranchJson->SetStringField(TEXT("producerWrittenValue"), Branch.ProducerWrittenValue);
				BranchJson->SetStringField(TEXT("producerWrittenValueSource"), TEXT("Pixel History lastAfter/postMod"));
				BranchJson->SetStringField(TEXT("producerActionKind"), Branch.ProducerActionKind);
				BranchJson->SetBoolField(TEXT("executedShaderAccess"), Branch.bExecutedShaderAccess);
				BranchJson->SetBoolField(TEXT("producerChangedValue"), Branch.bProducerChangedValue);
				BranchJson->SetBoolField(TEXT("producerValueMatchesExecutedSample"), Branch.bProducerValueMatchesExecutedSample);
				BranchJson->SetNumberField(TEXT("evidenceRecordCount"), Branch.EvidenceRecordCount);
				BranchJson->SetNumberField(TEXT("queryRecordCount"), Branch.QueryRecordCount);
				BranchJson->SetNumberField(TEXT("collapsedShaderAccessCount"), Branch.CollapsedShaderAccessCount);
				TArray<TSharedPtr<FJsonValue>> SampleIndices;
				for (const int32 Sample : Branch.Samples)
				{
					SampleIndices.Add(MakeShared<FJsonValueNumber>(Sample));
				}
				BranchJson->SetArrayField(TEXT("samples"), MoveTemp(SampleIndices));
				if (const FEventContextEvidence* Consumer = EventContexts.Find(Branch.ConsumerEventId))
				{
					BranchJson->SetStringField(TEXT("consumerAction"), Consumer->Action);
					BranchJson->SetStringField(TEXT("consumerMarker"), CompactMarkerPath(Consumer->MarkerPath));
				}
				if (const FEventContextEvidence* Producer = EventContexts.Find(Branch.ProducerEventId))
				{
					BranchJson->SetStringField(TEXT("producerAction"), Producer->Action);
					BranchJson->SetStringField(TEXT("producerMarker"), CompactMarkerPath(Producer->MarkerPath));
					BranchJson->SetStringField(TEXT("producerShaderEntry"), Producer->ShaderEntry);
				}
				BranchValues.Add(MakeShared<FJsonValueObject>(BranchJson));
			}
			LaneJson->SetNumberField(TEXT("includedBranchCount"), IncludedBranches);
			LaneJson->SetArrayField(TEXT("branches"), MoveTemp(BranchValues));
			CausalLaneValues.Add(MakeShared<FJsonValueObject>(LaneJson));
		}
		Root->SetNumberField(TEXT("causalLaneCount"), CausalLanes.Num());
		Root->SetArrayField(TEXT("causalLanes"), MoveTemp(CausalLaneValues));
		Root->SetStringField(TEXT("causalLanePolicy"),
			TEXT("color, geometry, and overlay are parallel evidence lanes; never concatenate them into one chronological process"));
		const uint32 PrimaryRootEventId = LastCandidate.IsSet() ? LastCandidate->Event.EventId : 0;
		const FPrimaryCausalPathEvidence PrimaryPath = BuildPrimaryColorPathEvidence(
			CausalLanes, EventContexts, PrimaryRootEventId);
		const TSharedRef<FJsonObject> PrimaryPathJson = MakeShared<FJsonObject>();
		PrimaryPathJson->SetNumberField(TEXT("rootEventId"), PrimaryPath.RootEventId);
		PrimaryPathJson->SetStringField(TEXT("stopReason"), PrimaryPath.StopReason);
		PrimaryPathJson->SetBoolField(TEXT("reachedConfirmedSceneSource"), PrimaryPath.bReachedConfirmedSceneSource);
		PrimaryPathJson->SetBoolField(TEXT("reachedExplicitBoundary"), PrimaryPath.bReachedExplicitBoundary);
		TArray<TSharedPtr<FJsonValue>> PrimaryPathEdges;
		for (const FCausalLaneBranchEvidence& Branch : PrimaryPath.Branches)
		{
			const TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
			Edge->SetNumberField(TEXT("consumerEventId"), Branch.ConsumerEventId);
			Edge->SetNumberField(TEXT("producerEventId"), Branch.ProducerEventId);
			Edge->SetNumberField(TEXT("resourceIndex"), Branch.ResourceIndex);
			Edge->SetStringField(TEXT("resource"), Branch.ResourceName);
			Edge->SetStringField(TEXT("lane"), Branch.TracePurpose);
			Edge->SetStringField(TEXT("edgeRole"), Branch.EdgeRole);
			Edge->SetStringField(TEXT("edgeConfidence"), Branch.EdgeConfidence);
			Edge->SetStringField(TEXT("executedSampleValue"), Branch.ExecutedSampleValue);
			Edge->SetStringField(TEXT("producerWrittenValue"), Branch.ProducerWrittenValue);
			Edge->SetStringField(TEXT("branchStatus"), Branch.BranchStatus);
			PrimaryPathEdges.Add(MakeShared<FJsonValueObject>(Edge));
		}
		PrimaryPathJson->SetArrayField(TEXT("edges"), MoveTemp(PrimaryPathEdges));
		Root->SetObjectField(TEXT("primaryColorPath"), PrimaryPathJson);

		if (LastCandidate.IsSet())
		{
			const FCausalCandidate& Candidate = LastCandidate.GetValue();
			TSharedRef<FJsonObject> CandidateJson = MakeShared<FJsonObject>();
			CandidateJson->SetNumberField(TEXT("eventId"), Candidate.Event.EventId);
			CandidateJson->SetStringField(TEXT("action"), Candidate.Event.Action);
			CandidateJson->SetStringField(TEXT("kind"), Candidate.Event.ActionKind);
			CandidateJson->SetStringField(TEXT("semantics"), ClassifySemantics(Candidate.Event));
			CandidateJson->SetStringField(TEXT("marker"), CompactMarkerPath(Candidate.Event.MarkerPath));
			CandidateJson->SetStringField(TEXT("result"), DescribeEventResult(Candidate.Event));
			CandidateJson->SetStringField(TEXT("causalRole"), TEXT("final-writer"));
			AddColorDeltaJson(CandidateJson, Candidate.Event);
			CandidateJson->SetBoolField(TEXT("pointDivergence"), bLastCandidateHasDivergence);
			CandidateJson->SetNumberField(TEXT("sampleCoverage"), Candidate.SampleCoverage);
			Root->SetObjectField(TEXT("candidate"), CandidateJson);
			Root->SetObjectField(TEXT("finalWriter"), CandidateJson);
		}
		if (LastSignificantCandidate.IsSet())
		{
			const FCausalCandidate& Candidate = LastSignificantCandidate.GetValue();
			TSharedRef<FJsonObject> CandidateJson = MakeShared<FJsonObject>();
			CandidateJson->SetNumberField(TEXT("eventId"), Candidate.Event.EventId);
			CandidateJson->SetStringField(TEXT("action"), Candidate.Event.Action);
			CandidateJson->SetStringField(TEXT("kind"), Candidate.Event.ActionKind);
			CandidateJson->SetStringField(TEXT("semantics"), ClassifySemantics(Candidate.Event));
			CandidateJson->SetStringField(TEXT("marker"), CompactMarkerPath(Candidate.Event.MarkerPath));
			CandidateJson->SetStringField(TEXT("result"), DescribeEventResult(Candidate.Event));
			CandidateJson->SetStringField(TEXT("causalRole"), TEXT("significant-upstream-writer-candidate"));
			AddColorDeltaJson(CandidateJson, Candidate.Event);
			Root->SetObjectField(TEXT("significantWriterCandidate"), CandidateJson);
		}

		const FAgentContextCoverageSelection ContextSelection = BuildAgentContextCoverageSelection();
		TArray<TSharedPtr<FJsonValue>> DeterministicContexts;
		const int32 ContextCount = ContextSelection.DetailedEventIds.Num();
		for (int32 ContextIndex = 0; ContextIndex < ContextCount; ++ContextIndex)
		{
			const uint32 ContextEventId = ContextSelection.DetailedEventIds[ContextIndex];
			const FEventContextEvidence& Context = EventContexts.FindChecked(ContextEventId);
			const int32 ReverseDepth = EventContextDepths.FindRef(Context.EventId);
			const FAgentContextCoverageEvidence& Coverage =
				ContextSelection.CoverageByEventId.FindChecked(ContextEventId);
			TSharedRef<FJsonObject> ContextJson = MakeShared<FJsonObject>();
			ContextJson->SetNumberField(TEXT("eventId"), Context.EventId);
			ContextJson->SetStringField(TEXT("action"), Context.Action);
			ContextJson->SetStringField(TEXT("actionKind"), Context.ActionKind);
			ContextJson->SetStringField(TEXT("marker"), CompactMarkerPath(Context.MarkerPath));
			ContextJson->SetNumberField(TEXT("reverseDepth"), ReverseDepth);
			if (Coverage.CausalDistance != MAX_int32)
			{
				ContextJson->SetNumberField(TEXT("causalDistanceFromCritical"), Coverage.CausalDistance);
			}
			ContextJson->SetNumberField(TEXT("coveragePriorityScore"),
				static_cast<double>(Coverage.PriorityScore));
			ContextJson->SetNumberField(TEXT("observedPassedFragments"), Coverage.PassedFragments);
			ContextJson->SetNumberField(TEXT("observedRejectedFragments"), Coverage.RejectedFragments);
			ContextJson->SetBoolField(TEXT("observedTextureValueChange"), Coverage.bChangedTextureValue);
			TArray<TSharedPtr<FJsonValue>> CoverageRoles;
			for (const FString& Role : GetAgentContextCoverageRoles(Coverage))
			{
				CoverageRoles.Add(MakeShared<FJsonValueString>(Role));
			}
			ContextJson->SetArrayField(TEXT("coverageRoles"), MoveTemp(CoverageRoles));
			TArray<TSharedPtr<FJsonValue>> SelectionReasons;
			if (const TArray<FString>* Reasons = ContextSelection.SelectionReasons.Find(ContextEventId))
			{
				for (const FString& Reason : *Reasons)
				{
					SelectionReasons.Add(MakeShared<FJsonValueString>(Reason));
				}
			}
			ContextJson->SetArrayField(TEXT("detailSelectionReasons"), MoveTemp(SelectionReasons));
			ContextJson->SetStringField(TEXT("shaderStage"), Context.ShaderStage);
			ContextJson->SetStringField(TEXT("shaderEntry"), Context.ShaderEntry);
			ContextJson->SetStringField(TEXT("shaderDebugStatus"), Context.ShaderDebugStatus);
			ContextJson->SetBoolField(TEXT("shaderDebuggable"), Context.bShaderDebuggable);
			ContextJson->SetBoolField(TEXT("sourceSymbols"), Context.bSourceDebugInfo);
			ContextJson->SetStringField(TEXT("shaderEncoding"), Context.ShaderEncoding);
			ContextJson->SetNumberField(TEXT("inputSignatureCount"), Context.ShaderInputSignatureCount);
			ContextJson->SetNumberField(TEXT("outputSignatureCount"), Context.ShaderOutputSignatureCount);
			ContextJson->SetNumberField(TEXT("constantBlockCount"), Context.ShaderConstantBlockCount);
			ContextJson->SetNumberField(TEXT("samplerCount"), Context.ShaderSamplerCount);
			ContextJson->SetNumberField(TEXT("readOnlyResourceCount"), Context.ShaderReadOnlyResourceCount);
			ContextJson->SetNumberField(TEXT("readWriteResourceCount"), Context.ShaderReadWriteResourceCount);
			ContextJson->SetBoolField(TEXT("fixedFunctionStateAvailable"), Context.PipelineState.IsValid());
			if (IsCriticalAgentEvent(Context.EventId) && Context.PipelineState.IsValid())
			{
				ContextJson->SetObjectField(TEXT("fixedFunctionState"), Context.PipelineState);
			}
			if (Context.ShaderDebugTrace.IsValid())
			{
				ContextJson->SetObjectField(TEXT("shaderDebugTrace"),
					BuildCompactShaderDebugForAgent(Context.ShaderDebugTrace));
			}
			TArray<TSharedPtr<FJsonValue>> Inputs;
			for (const int32 InputIndex : SelectAgentBoundResourcesForCoverage(
				Context.Inputs, MaxAgentBoundResourcesPerContext))
			{
				const FBoundResourceEvidence& Input = Context.Inputs[InputIndex];
				TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
				Resource->SetNumberField(TEXT("resourceIndex"), Input.ResourceIndex);
				Resource->SetStringField(TEXT("name"), Input.Name);
				Resource->SetStringField(TEXT("format"), Input.Format);
				Resource->SetStringField(TEXT("stage"), Input.Stage);
				Resource->SetStringField(TEXT("access"), Input.Access);
				Resource->SetStringField(TEXT("shaderBinding"), Input.ShaderBinding);
				Resource->SetNumberField(TEXT("width"), Input.Width);
				Resource->SetNumberField(TEXT("height"), Input.Height);
				Resource->SetNumberField(TEXT("samples"), Input.Samples);
				Inputs.Add(MakeShared<FJsonValueObject>(Resource));
			}
			ContextJson->SetArrayField(TEXT("inputs"), MoveTemp(Inputs));
			ContextJson->SetNumberField(TEXT("inputCount"), Context.Inputs.Num());
			ContextJson->SetStringField(TEXT("boundResourceSelectionPolicy"), TEXT("semantic-coverage"));
			TArray<TSharedPtr<FJsonValue>> Outputs;
			for (const int32 OutputIndex : SelectAgentBoundResourcesForCoverage(
				Context.Outputs, MaxAgentBoundResourcesPerContext))
			{
				const FBoundResourceEvidence& Output = Context.Outputs[OutputIndex];
				TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
				Resource->SetNumberField(TEXT("resourceIndex"), Output.ResourceIndex);
				Resource->SetStringField(TEXT("name"), Output.Name);
				Resource->SetStringField(TEXT("format"), Output.Format);
				Resource->SetStringField(TEXT("stage"), Output.Stage);
				Resource->SetStringField(TEXT("access"), Output.Access);
				Resource->SetNumberField(TEXT("width"), Output.Width);
				Resource->SetNumberField(TEXT("height"), Output.Height);
				Outputs.Add(MakeShared<FJsonValueObject>(Resource));
			}
			ContextJson->SetArrayField(TEXT("outputs"), MoveTemp(Outputs));
			ContextJson->SetNumberField(TEXT("outputCount"), Context.Outputs.Num());

			TArray<TSharedPtr<FJsonValue>> CompactProvenance;
			for (const TSharedPtr<FJsonObject>& Provenance :
				SelectAgentResourceProvenanceForCoverage(Context))
			{
				CompactProvenance.Add(MakeShared<FJsonValueObject>(
					BuildCompactResourceProvenanceForAgent(Provenance)));
			}
			ContextJson->SetArrayField(TEXT("resourceProvenance"), MoveTemp(CompactProvenance));
			ContextJson->SetNumberField(TEXT("resourceProvenanceCount"), Context.ResourceProvenance.Num());
			ContextJson->SetStringField(TEXT("resourceProvenanceSelectionPolicy"), TEXT("semantic-coverage"));

			TArray<TSharedPtr<FJsonValue>> CompactHistories;
			for (const TSharedPtr<FJsonObject>& History : SelectAgentResourceHistoriesForCoverage(Context))
			{
				CompactHistories.Add(MakeShared<FJsonValueObject>(
					BuildCompactResourceHistoryForAgent(History)));
			}
			ContextJson->SetArrayField(TEXT("resourcePixelHistories"), MoveTemp(CompactHistories));
			ContextJson->SetNumberField(TEXT("resourcePixelHistoryCount"), Context.ResourcePixelHistories.Num());
			ContextJson->SetStringField(TEXT("resourcePixelHistorySelectionPolicy"), TEXT("causal-coverage"));
			DeterministicContexts.Add(MakeShared<FJsonValueObject>(ContextJson));
		}
		Root->SetArrayField(TEXT("deterministicEventContexts"), MoveTemp(DeterministicContexts));

		TArray<uint32> ContextIndexIds;
		ContextSelection.CoverageByEventId.GenerateKeyArray(ContextIndexIds);
		ContextIndexIds.Sort([&ContextSelection](uint32 A, uint32 B)
		{
			const FAgentContextCoverageEvidence& CoverageA =
				ContextSelection.CoverageByEventId.FindChecked(A);
			const FAgentContextCoverageEvidence& CoverageB =
				ContextSelection.CoverageByEventId.FindChecked(B);
			const bool bReachableA = CoverageA.CausalDistance != MAX_int32;
			const bool bReachableB = CoverageB.CausalDistance != MAX_int32;
			if (bReachableA != bReachableB)
			{
				return bReachableA;
			}
			if (bReachableA && CoverageA.CausalDistance != CoverageB.CausalDistance)
			{
				return CoverageA.CausalDistance < CoverageB.CausalDistance;
			}
			return CoverageA.ReverseDepth == CoverageB.ReverseDepth
				? A > B : CoverageA.ReverseDepth < CoverageB.ReverseDepth;
		});

		TSharedRef<FJsonObject> ContextIndexSummary = MakeShared<FJsonObject>();
		int32 PixelWriterContextCount = 0;
		int32 AssetMarkerContextCount = 0;
		int32 SceneRasterContextCount = 0;
		int32 NaniteContextCount = 0;
		int32 DepthContextCount = 0;
		int32 BoundaryContextCount = 0;
		TArray<TSharedPtr<FJsonValue>> ContextIndex;
		ContextIndex.Reserve(ContextIndexIds.Num());
		for (const uint32 ContextEventId : ContextIndexIds)
		{
			const FEventContextEvidence& Context = EventContexts.FindChecked(ContextEventId);
			const FAgentContextCoverageEvidence& Coverage =
				ContextSelection.CoverageByEventId.FindChecked(ContextEventId);
			PixelWriterContextCount += Coverage.bReferencedPixelWriter ? 1 : 0;
			AssetMarkerContextCount += Coverage.bAssetMarker ? 1 : 0;
			SceneRasterContextCount += Coverage.bSceneRaster ? 1 : 0;
			NaniteContextCount += Coverage.bNanite ? 1 : 0;
			DepthContextCount += Coverage.bDepthStage ? 1 : 0;
			BoundaryContextCount += Coverage.bBranchBoundary || Coverage.bUnresolvedProducer ? 1 : 0;

			TSharedRef<FJsonObject> IndexEntry = MakeShared<FJsonObject>();
			IndexEntry->SetNumberField(TEXT("eventId"), ContextEventId);
			IndexEntry->SetNumberField(TEXT("reverseDepth"), Coverage.ReverseDepth);
			if (Coverage.CausalDistance != MAX_int32)
			{
				IndexEntry->SetNumberField(TEXT("causalDistanceFromCritical"), Coverage.CausalDistance);
			}
			IndexEntry->SetBoolField(TEXT("selectedForDetail"),
				ContextSelection.DetailedEventIds.Contains(ContextEventId));
			IndexEntry->SetBoolField(TEXT("referencedAsPixelWriter"), Coverage.bReferencedPixelWriter);
			IndexEntry->SetNumberField(TEXT("passedFragments"), Coverage.PassedFragments);
			IndexEntry->SetNumberField(TEXT("rejectedFragments"), Coverage.RejectedFragments);
			IndexEntry->SetBoolField(TEXT("changedTextureValue"), Coverage.bChangedTextureValue);
			IndexEntry->SetStringField(TEXT("shaderEntry"), Context.ShaderEntry);
			TArray<FString> Roles = GetAgentContextCoverageRoles(Coverage);
			TArray<TSharedPtr<FJsonValue>> RoleValues;
			for (const FString& Role : Roles)
			{
				RoleValues.Add(MakeShared<FJsonValueString>(Role));
			}
			IndexEntry->SetArrayField(TEXT("coverageRoles"), MoveTemp(RoleValues));
			const bool bMeaningfulMarker = ContextSelection.DetailedEventIds.Contains(ContextEventId)
				|| Coverage.bAssetMarker || Coverage.bSceneRaster || Coverage.bNanite
				|| Coverage.bDepthStage || Coverage.bBranchBoundary || Coverage.bUnresolvedProducer;
			if (bMeaningfulMarker)
			{
				IndexEntry->SetStringField(TEXT("markerHint"),
					CompactMarkerPath(Context.MarkerPath).Left(MaxAgentContextIndexMarkerChars));
			}

			auto SetBoundedEventIds = [&IndexEntry](const TCHAR* Field, const TCHAR* CountField,
				const TArray<uint32>& EventIds)
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (int32 Index = 0; Index < EventIds.Num() && Index < MaxAgentContextIndexLinks; ++Index)
				{
					Values.Add(MakeShared<FJsonValueNumber>(EventIds[Index]));
				}
				IndexEntry->SetArrayField(Field, MoveTemp(Values));
				IndexEntry->SetNumberField(CountField, EventIds.Num());
			};
			SetBoundedEventIds(TEXT("producerEventIds"), TEXT("producerEventCount"),
				Coverage.ProducerEventIds);
			SetBoundedEventIds(TEXT("downstreamConsumerEventIds"), TEXT("downstreamConsumerEventCount"),
				Coverage.DownstreamConsumerEventIds);
			ContextIndex.Add(MakeShared<FJsonValueObject>(IndexEntry));
		}
		ContextIndexSummary->SetNumberField(TEXT("total"), ContextIndex.Num());
		ContextIndexSummary->SetNumberField(TEXT("selectedForDetail"), ContextCount);
		ContextIndexSummary->SetNumberField(TEXT("referencedPixelWriters"), PixelWriterContextCount);
		ContextIndexSummary->SetNumberField(TEXT("assetMarkers"), AssetMarkerContextCount);
		ContextIndexSummary->SetNumberField(TEXT("sceneRaster"), SceneRasterContextCount);
		ContextIndexSummary->SetNumberField(TEXT("nanite"), NaniteContextCount);
		ContextIndexSummary->SetNumberField(TEXT("depth"), DepthContextCount);
		ContextIndexSummary->SetNumberField(TEXT("chainBoundaries"), BoundaryContextCount);
		Root->SetStringField(TEXT("deterministicEventContextSelectionPolicy"),
			TEXT("causal-coverage: critical + confirmed writers + asset markers + scene raster + Nanite + depth + boundaries + depth diversity"));
		Root->SetObjectField(TEXT("deterministicEventContextIndexSummary"), ContextIndexSummary);
		Root->SetArrayField(TEXT("deterministicEventContextIndex"), MoveTemp(ContextIndex));
		Root->SetNumberField(TEXT("deterministicEventContextCount"), EventContexts.Num());
		Root->SetNumberField(TEXT("deterministicEventContextsIncluded"), ContextCount);
		Root->SetBoolField(TEXT("agentEvidenceIsFullTrace"), false);
		Root->SetStringField(TEXT("fullTraceSnapshotPath"), FullTraceSnapshotPath);
		Root->SetStringField(TEXT("fullTraceRecordsJsonlPath"), FullTraceJsonlPath);
		Root->SetNumberField(TEXT("fullTraceRecordCount"), FullTraceRecordCount);
		Root->SetBoolField(TEXT("criticalDeterministicCollectionComplete"),
			!HasPendingCriticalDeterministicQueries());
		Root->SetBoolField(TEXT("backgroundDeterministicCollectionPending"),
			HasPendingBackgroundDeterministicQueries());
		Root->SetNumberField(TEXT("pendingCriticalEventContexts"), GetPendingCriticalContextCount());
		Root->SetNumberField(TEXT("pendingRequiredResourcePixelHistories"),
			GetPendingRequiredResourceHistoryCount());
		Root->SetNumberField(TEXT("pendingBackgroundResourcePixelHistories"),
			GetPendingBackgroundResourceHistoryCount());
		Root->SetBoolField(TEXT("deterministicContextCollectionComplete"),
			PendingEventContextIds.IsEmpty() && PendingResourcePixelHistoryByRequest.IsEmpty()
				&& PendingShaderDebugByRequest.IsEmpty()
				&& BudgetDeferredResourcePixelHistoryRequests.IsEmpty()
				&& BudgetDeferredEventContextDepths.IsEmpty());
		Root->SetNumberField(TEXT("deterministicContextFailureCount"), FailedEventContextIds.Num());
		Root->SetNumberField(TEXT("shaderDebugFailureCount"), FailedShaderDebugIds.Num());
		Root->SetNumberField(TEXT("resourcePixelHistoryFailureCount"), FailedResourcePixelHistoryKeys.Num());
		Root->SetNumberField(TEXT("resourcePixelHistoryQueryCount"), ResourcePixelHistoryQueriesSubmitted);
		Root->SetNumberField(TEXT("resourcePixelHistoryBranchCount"), GetDiscoveredResourceHistoryCount());
		Root->SetNumberField(TEXT("resourcePixelHistoryBudgetDeferredCount"),
			GetBudgetDeferredResourceHistoryCount());
		Root->SetNumberField(TEXT("resourcePixelHistoryBudgetDeferredCandidateCount"),
			BudgetDeferredResourcePixelHistoryRequests.Num());
		Root->SetNumberField(TEXT("resourcePixelHistoryQueryLimit"), ResourcePixelHistoryQueryLimit);
		Root->SetNumberField(TEXT("deterministicContextBudgetDeferredCount"),
			BudgetDeferredEventContextDepths.Num());
		Root->SetNumberField(TEXT("deterministicContextLimit"), DeterministicContextLimit);
		Root->SetBoolField(TEXT("automaticDeepTraceEnabled"), true);
		Root->SetNumberField(TEXT("agentEventChainLimitPerSample"), MaxAgentEventChainPerSample);
		return SerializeJson(Root);
	}

	bool SAnalyzerHome::AgentEvidenceContainsEvent(uint32 EventId) const
	{
		for (const FPixelSample& Sample : Samples)
		{
			if (Sample.Modifications.ContainsByPredicate([EventId](const FPixelModificationEvidence& Item) { return Item.EventId == EventId; }))
			{
				return true;
			}
		}
		return false;
	}

	FString SAnalyzerHome::BuildAgentEventObservation(uint32 EventId) const
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("tool"), TEXT("inspect_event"));
		Root->SetNumberField(TEXT("eventId"), EventId);
		TArray<TSharedPtr<FJsonValue>> Outcomes;
		for (int32 SampleIndex = 0; SampleIndex < Samples.Num(); ++SampleIndex)
		{
			const FPixelSample& Sample = Samples[SampleIndex];
			const FString Label = FString::Printf(TEXT("P%d"), SampleIndex + 1);
			const TArray<FEventEvidence> Events = AggregateEvents(Sample);
			if (const FEventEvidence* Event = FindEvent(Events, EventId))
			{
				TSharedRef<FJsonObject> Outcome = MakeShared<FJsonObject>();
				Outcome->SetStringField(TEXT("sample"), Label);
				Outcome->SetStringField(TEXT("action"), Event->Action);
				Outcome->SetStringField(TEXT("kind"), Event->ActionKind);
				Outcome->SetStringField(TEXT("marker"), CompactMarkerPath(Event->MarkerPath));
				Outcome->SetStringField(TEXT("result"), DescribeEventResult(*Event));
				Outcome->SetStringField(TEXT("before"), Event->Before);
				Outcome->SetStringField(TEXT("shaderOutput"), Event->ShaderOutput);
				Outcome->SetStringField(TEXT("after"), Event->After);
				Outcomes.Add(MakeShared<FJsonValueObject>(Outcome));
			}
		}
		Root->SetArrayField(TEXT("sampleOutcomes"), Outcomes);

		if (const FEventContextEvidence* Context = EventContexts.Find(EventId))
		{
			TSharedRef<FJsonObject> Pipeline = MakeShared<FJsonObject>();
			Pipeline->SetStringField(TEXT("shaderStage"), Context->ShaderStage);
			Pipeline->SetStringField(TEXT("shaderEntry"), Context->ShaderEntry);
			Pipeline->SetBoolField(TEXT("shaderDebuggable"), Context->bShaderDebuggable);
			Pipeline->SetBoolField(TEXT("sourceSymbols"), Context->bSourceDebugInfo);
			TSharedRef<FJsonObject> Reflection = MakeShared<FJsonObject>();
			Reflection->SetStringField(TEXT("encoding"), Context->ShaderEncoding);
			Reflection->SetNumberField(TEXT("inputSignatureCount"), Context->ShaderInputSignatureCount);
			Reflection->SetNumberField(TEXT("outputSignatureCount"), Context->ShaderOutputSignatureCount);
			Reflection->SetNumberField(TEXT("constantBlockCount"), Context->ShaderConstantBlockCount);
			Reflection->SetNumberField(TEXT("samplerCount"), Context->ShaderSamplerCount);
			Reflection->SetNumberField(TEXT("readOnlyResourceCount"), Context->ShaderReadOnlyResourceCount);
			Reflection->SetNumberField(TEXT("readWriteResourceCount"), Context->ShaderReadWriteResourceCount);
			Pipeline->SetObjectField(TEXT("shaderReflection"), Reflection);
			Pipeline->SetStringField(TEXT("algorithmEvidence"),
				(Context->bShaderDebuggable && Context->bSourceDebugInfo)
					? TEXT("source/debug information exists; instruction trace not executed")
					: TEXT("not proven from reflection and bindings"));
			TArray<TSharedPtr<FJsonValue>> Inputs;
			for (int32 Index = 0; Index < Context->Inputs.Num() && Index < MaxDisplayedFrontierResources; ++Index)
			{
				const FBoundResourceEvidence& Input = Context->Inputs[Index];
				TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
				Resource->SetStringField(TEXT("name"), Input.Name);
				Resource->SetStringField(TEXT("shaderBinding"), Input.ShaderBinding);
				Resource->SetStringField(TEXT("stage"), Input.Stage);
				Resource->SetStringField(TEXT("access"), Input.Access);
				Resource->SetNumberField(TEXT("width"), Input.Width);
				Resource->SetNumberField(TEXT("height"), Input.Height);
				Resource->SetNumberField(TEXT("samples"), Input.Samples);
				Inputs.Add(MakeShared<FJsonValueObject>(Resource));
			}
			Pipeline->SetArrayField(TEXT("usedInputs"), Inputs);
			TArray<TSharedPtr<FJsonValue>> Outputs;
			for (int32 Index = 0; Index < Context->Outputs.Num() && Index < MaxDisplayedFrontierResources; ++Index)
			{
				const FBoundResourceEvidence& Output = Context->Outputs[Index];
				TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
				Resource->SetStringField(TEXT("name"), Output.Name);
				Resource->SetStringField(TEXT("stage"), Output.Stage);
				Resource->SetStringField(TEXT("access"), Output.Access);
				Resource->SetNumberField(TEXT("width"), Output.Width);
				Resource->SetNumberField(TEXT("height"), Output.Height);
				Outputs.Add(MakeShared<FJsonValueObject>(Resource));
			}
			Pipeline->SetArrayField(TEXT("usedOutputs"), Outputs);
			Pipeline->SetArrayField(TEXT("resourceProvenance"), Context->ResourceProvenance);
			Pipeline->SetArrayField(TEXT("resourcePixelHistories"), Context->ResourcePixelHistories);
			if (Context->PipelineState.IsValid())
			{
				Pipeline->SetObjectField(TEXT("fixedFunctionState"), Context->PipelineState);
			}
			if (Context->ShaderDebugTrace.IsValid())
			{
				Pipeline->SetObjectField(TEXT("shaderDebugTrace"), Context->ShaderDebugTrace);
			}
			Root->SetObjectField(TEXT("pipeline"), Pipeline);
		}
		else
		{
			Root->SetStringField(TEXT("pipeline"), FailedEventContextIds.Contains(EventId) ? TEXT("query_failed") : TEXT("not_loaded"));
		}
		return SerializeJson(Root);
	}

	const FPixelSample* SAnalyzerHome::FindAgentSample(const FString& Label) const
	{
		if (!Label.StartsWith(TEXT("P"), ESearchCase::IgnoreCase))
		{
			return nullptr;
		}
		int32 OneBasedIndex = 0;
		if (!LexTryParseString(OneBasedIndex, *Label.Mid(1)) || !Samples.IsValidIndex(OneBasedIndex - 1))
		{
			return nullptr;
		}
		return &Samples[OneBasedIndex - 1];
	}

	FString SAnalyzerHome::BuildAgentSampleObservation(const FString& Label) const
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("tool"), TEXT("inspect_sample"));
		Root->SetStringField(TEXT("sample"), Label);
		const FPixelSample* Sample = FindAgentSample(Label);
		if (!Sample)
		{
			Root->SetStringField(TEXT("error"), TEXT("unknown sample label"));
			return SerializeJson(Root);
		}

		Root->SetNumberField(TEXT("x"), Sample->Pixel.X);
		Root->SetNumberField(TEXT("y"), Sample->Pixel.Y);
		Root->SetStringField(TEXT("finalObservedValue"), Sample->Modifications.IsEmpty() ? TEXT("unavailable") : Sample->Modifications.Last().After);
		const TArray<FEventEvidence> Events = AggregateEvents(*Sample);
		TArray<TSharedPtr<FJsonValue>> History;
		const int32 First = FMath::Max(0, Events.Num() - MaxDisplayedTraceHops);
		for (int32 Index = Events.Num() - 1; Index >= First; --Index)
		{
			const FEventEvidence& Event = Events[Index];
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("eventId"), Event.EventId);
			Item->SetStringField(TEXT("kind"), Event.ActionKind);
			Item->SetStringField(TEXT("action"), Event.Action);
			Item->SetStringField(TEXT("marker"), CompactMarkerPath(Event.MarkerPath));
			Item->SetStringField(TEXT("result"), DescribeEventResult(Event));
			History.Add(MakeShared<FJsonValueObject>(Item));
		}
		Root->SetArrayField(TEXT("latestFirstHistory"), History);
		return SerializeJson(Root);
	}
}
