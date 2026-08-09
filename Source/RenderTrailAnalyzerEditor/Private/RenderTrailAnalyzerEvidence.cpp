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

	FAgentContextCoverageSelection SelectAgentContextsForCausalCoverage(
		const TMap<uint32, FEventContextEvidence>& EventContexts,
		const TMap<uint32, int32>& EventContextDepths,
		const TSet<uint32>& CriticalEventIds,
		int32 MaxDetailedContexts)
	{
		FAgentContextCoverageSelection Result;
		MaxDetailedContexts = FMath::Max(0, MaxDetailedContexts);
		for (const TPair<uint32, FEventContextEvidence>& Pair : EventContexts)
		{
			const FEventContextEvidence& Context = Pair.Value;
			FAgentContextCoverageEvidence& Coverage = Result.CoverageByEventId.Add(Pair.Key);
			Coverage.EventId = Pair.Key;
			Coverage.ReverseDepth = EventContextDepths.FindRef(Pair.Key);
			Coverage.bCritical = CriticalEventIds.Contains(Pair.Key);

			FString Searchable = Context.MarkerPath + TEXT(" ") + Context.Action + TEXT(" ") + Context.ShaderEntry;
			for (const FBoundResourceEvidence& Resource : Context.Inputs)
			{
				Searchable += TEXT(" ") + Resource.Name + TEXT(" ") + Resource.ShaderBinding;
			}
			for (const FBoundResourceEvidence& Resource : Context.Outputs)
			{
				Searchable += TEXT(" ") + Resource.Name;
			}
			auto Contains = [&Searchable](const TCHAR* Token)
			{
				return Searchable.Contains(Token, ESearchCase::IgnoreCase);
			};
			Coverage.bAssetMarker = Contains(TEXT("/Game/")) || Contains(TEXT("/Engine/"))
				|| Contains(TEXT(" SM_")) || Contains(TEXT(" SK_"))
				|| Contains(TEXT(" SkeletalMesh_"));
			Coverage.bSceneRaster = Contains(TEXT("BasePass")) || Contains(TEXT("PrePass"))
				|| Contains(TEXT("DepthPass")) || Contains(TEXT("GBuffer"));
			Coverage.bNanite = Contains(TEXT("Nanite")) || Contains(TEXT("MicropolyRasterize"))
				|| Contains(TEXT("VisBuffer"));
			Coverage.bDepthStage = Contains(TEXT("SceneDepth")) || Contains(TEXT("DepthPass"))
				|| Contains(TEXT("PrePass")) || Contains(TEXT("HZB")) || Contains(TEXT("EmitSceneDepth"));
		}

		for (const TPair<uint32, FEventContextEvidence>& Pair : EventContexts)
		{
			FAgentContextCoverageEvidence& ConsumerCoverage =
				Result.CoverageByEventId.FindChecked(Pair.Key);
			for (const TSharedPtr<FJsonValue>& HistoryValue : Pair.Value.ResourcePixelHistories)
			{
				const TSharedPtr<FJsonObject> History = HistoryValue.IsValid() ? HistoryValue->AsObject() : nullptr;
				if (!History.IsValid())
				{
					continue;
				}
				FString BranchStatus;
				History->TryGetStringField(TEXT("branchStatus"), BranchStatus);
				if (!BranchStatus.IsEmpty()
					&& BranchStatus != TEXT("continued-to-dominating-writer")
					&& BranchStatus != TEXT("continued-to-pixel-writer")
					&& BranchStatus != TEXT("adaptive-footprint-continued")
					&& BranchStatus != TEXT("no-modification-before-consumer"))
				{
					ConsumerCoverage.bBranchBoundary = true;
				}

				double SelectedWriterEventIdValue = 0.0;
				if (History->TryGetNumberField(TEXT("selectedWriterEventId"), SelectedWriterEventIdValue))
				{
					const uint32 WriterEventId = static_cast<uint32>(SelectedWriterEventIdValue);
					if (WriterEventId != 0 && WriterEventId != Pair.Key)
					{
						ConsumerCoverage.ProducerEventIds.AddUnique(WriterEventId);
						if (FAgentContextCoverageEvidence* WriterCoverage =
							Result.CoverageByEventId.Find(WriterEventId))
						{
							WriterCoverage->bReferencedPixelWriter = true;
							WriterCoverage->DownstreamConsumerEventIds.AddUnique(Pair.Key);
						}
						else
						{
							ConsumerCoverage.bUnresolvedProducer = true;
						}
					}
				}

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
						double EventIdValue = 0.0;
						if (!Event->TryGetNumberField(TEXT("eventId"), EventIdValue))
						{
							continue;
						}
						FAgentContextCoverageEvidence* EventCoverage =
							Result.CoverageByEventId.Find(static_cast<uint32>(EventIdValue));
						if (!EventCoverage)
						{
							continue;
						}
						double Passed = 0.0;
						double Rejected = 0.0;
						bool bChanged = false;
						Event->TryGetNumberField(TEXT("passedFragments"), Passed);
						Event->TryGetNumberField(TEXT("rejectedFragments"), Rejected);
						Event->TryGetBoolField(TEXT("changedTextureValue"), bChanged);
						EventCoverage->PassedFragments = FMath::Max(
							EventCoverage->PassedFragments, static_cast<int32>(Passed));
						EventCoverage->RejectedFragments = FMath::Max(
							EventCoverage->RejectedFragments, static_cast<int32>(Rejected));
						EventCoverage->bChangedTextureValue |= bChanged;
					}
				}
			}
		}

		TArray<uint32> CausalQueue;
		for (const uint32 CriticalEventId : CriticalEventIds)
		{
			if (FAgentContextCoverageEvidence* Coverage = Result.CoverageByEventId.Find(CriticalEventId))
			{
				Coverage->CausalDistance = 0;
				CausalQueue.Add(CriticalEventId);
			}
		}
		for (int32 QueueIndex = 0; QueueIndex < CausalQueue.Num(); ++QueueIndex)
		{
			const uint32 ConsumerEventId = CausalQueue[QueueIndex];
			const FAgentContextCoverageEvidence& Consumer =
				Result.CoverageByEventId.FindChecked(ConsumerEventId);
			for (const uint32 ProducerEventId : Consumer.ProducerEventIds)
			{
				FAgentContextCoverageEvidence* Producer = Result.CoverageByEventId.Find(ProducerEventId);
				if (Producer && Producer->CausalDistance > Consumer.CausalDistance + 1)
				{
					Producer->CausalDistance = Consumer.CausalDistance + 1;
					CausalQueue.Add(ProducerEventId);
				}
			}
		}

		for (TPair<uint32, FAgentContextCoverageEvidence>& Pair : Result.CoverageByEventId)
		{
			FAgentContextCoverageEvidence& Coverage = Pair.Value;
			Coverage.ProducerEventIds.Sort([](uint32 A, uint32 B) { return A > B; });
			Coverage.DownstreamConsumerEventIds.Sort([](uint32 A, uint32 B) { return A > B; });
			Coverage.PriorityScore = Coverage.bCritical ? 1000000 : 0;
			if (Coverage.CausalDistance != MAX_int32)
			{
				Coverage.PriorityScore += FMath::Max<int64>(0, 220000 - Coverage.CausalDistance * 10000LL);
			}
			Coverage.PriorityScore += Coverage.bReferencedPixelWriter ? 120000 : 0;
			Coverage.PriorityScore += Coverage.bChangedTextureValue ? 90000 : 0;
			Coverage.PriorityScore += Coverage.PassedFragments > 0 ? 70000 : 0;
			Coverage.PriorityScore += Coverage.bAssetMarker ? 60000 : 0;
			Coverage.PriorityScore += Coverage.bSceneRaster ? 50000 : 0;
			Coverage.PriorityScore += Coverage.bNanite ? 45000 : 0;
			Coverage.PriorityScore += Coverage.bDepthStage ? 35000 : 0;
			Coverage.PriorityScore += Coverage.bBranchBoundary || Coverage.bUnresolvedProducer ? 25000 : 0;
			Coverage.PriorityScore += FMath::Min(Coverage.ReverseDepth, 32) * 250LL;
			if (Coverage.RejectedFragments > 0 && Coverage.PassedFragments == 0
				&& !Coverage.bChangedTextureValue)
			{
				Coverage.PriorityScore -= 30000;
			}
		}

		auto AddSelected = [&Result, MaxDetailedContexts](uint32 EventId, const FString& Reason)
		{
			if (!Result.CoverageByEventId.Contains(EventId))
			{
				return false;
			}
			if (Result.DetailedEventIds.Contains(EventId))
			{
				Result.SelectionReasons.FindOrAdd(EventId).AddUnique(Reason);
				return true;
			}
			if (Result.DetailedEventIds.Num() >= MaxDetailedContexts)
			{
				return false;
			}
			Result.DetailedEventIds.Add(EventId);
			Result.SelectionReasons.FindOrAdd(EventId).Add(Reason);
			return true;
		};

		TArray<uint32> RankedIds;
		Result.CoverageByEventId.GenerateKeyArray(RankedIds);
		RankedIds.Sort([&Result](uint32 A, uint32 B)
		{
			const FAgentContextCoverageEvidence& CoverageA = Result.CoverageByEventId.FindChecked(A);
			const FAgentContextCoverageEvidence& CoverageB = Result.CoverageByEventId.FindChecked(B);
			return CoverageA.PriorityScore == CoverageB.PriorityScore
				? A > B : CoverageA.PriorityScore > CoverageB.PriorityScore;
		});

		for (const uint32 EventId : RankedIds)
		{
			if (Result.CoverageByEventId.FindChecked(EventId).bCritical)
			{
				AddSelected(EventId, TEXT("critical-final-or-significant-writer"));
			}
		}

		auto AddBestCoverage = [&RankedIds, &Result, &AddSelected](
			const FString& Reason, TFunctionRef<bool(const FAgentContextCoverageEvidence&)> Predicate,
			bool bRequireUnselected = false)
		{
			for (const uint32 EventId : RankedIds)
			{
				const FAgentContextCoverageEvidence& Coverage = Result.CoverageByEventId.FindChecked(EventId);
				if (Predicate(Coverage)
					&& (!bRequireUnselected || !Result.DetailedEventIds.Contains(EventId)))
				{
					AddSelected(EventId, Reason);
					return;
				}
			}
		};

		AddBestCoverage(TEXT("asset-marker-pixel-writer"), [](const FAgentContextCoverageEvidence& Coverage)
		{
			return Coverage.bAssetMarker && (Coverage.PassedFragments > 0 || Coverage.bChangedTextureValue);
		});
		AddBestCoverage(TEXT("basepass-prepass-gbuffer-coverage"), [](const FAgentContextCoverageEvidence& Coverage)
		{
			return Coverage.bSceneRaster;
		});
		AddBestCoverage(TEXT("nanite-visibility-or-raster-coverage"), [](const FAgentContextCoverageEvidence& Coverage)
		{
			return Coverage.bNanite;
		});
		AddBestCoverage(TEXT("nanite-writer-diversity"), [](const FAgentContextCoverageEvidence& Coverage)
		{
			return Coverage.bNanite && (Coverage.PassedFragments > 0 || Coverage.bChangedTextureValue);
		}, true);
		AddBestCoverage(TEXT("depth-chain-coverage"), [](const FAgentContextCoverageEvidence& Coverage)
		{
			return Coverage.bDepthStage;
		});
		AddBestCoverage(TEXT("explicit-chain-boundary"), [](const FAgentContextCoverageEvidence& Coverage)
		{
			return Coverage.bBranchBoundary || Coverage.bUnresolvedProducer;
		});
		AddBestCoverage(TEXT("direct-confirmed-writer-hop"), [](const FAgentContextCoverageEvidence& Coverage)
		{
			return Coverage.CausalDistance == 1;
		}, true);
		AddBestCoverage(TEXT("second-confirmed-writer-hop"), [](const FAgentContextCoverageEvidence& Coverage)
		{
			return Coverage.CausalDistance == 1;
		}, true);
		AddBestCoverage(TEXT("deeper-confirmed-writer-hop"), [](const FAgentContextCoverageEvidence& Coverage)
		{
			return Coverage.CausalDistance >= 2 && Coverage.CausalDistance != MAX_int32;
		}, true);
		AddBestCoverage(TEXT("deep-frontier-coverage"), [](const FAgentContextCoverageEvidence& Coverage)
		{
			return Coverage.ReverseDepth >= 4 && (Coverage.bReferencedPixelWriter || Coverage.bAssetMarker
				|| Coverage.bSceneRaster || Coverage.bNanite || Coverage.bDepthStage);
		}, true);
		AddBestCoverage(TEXT("asset-marker-rejection-or-candidate"), [](const FAgentContextCoverageEvidence& Coverage)
		{
			return Coverage.bAssetMarker;
		}, true);

		for (const uint32 EventId : RankedIds)
		{
			if (Result.DetailedEventIds.Num() >= MaxDetailedContexts)
			{
				break;
			}
			AddSelected(EventId, TEXT("coverage-score-fill"));
		}
		return Result;
	}

	TArray<FCausalLaneEvidence> BuildCausalLaneEvidence(
		const TMap<uint32, FEventContextEvidence>& EventContexts,
		const TMap<uint32, int32>& EventContextDepths)
	{
		auto StatusRank = [](const FString& Status)
		{
			if (Status.Contains(TEXT("failed"), ESearchCase::IgnoreCase)) return 90;
			if (Status.Contains(TEXT("budget"), ESearchCase::IgnoreCase)
				|| Status.Contains(TEXT("limit"), ESearchCase::IgnoreCase)
				|| Status.Contains(TEXT("pruned"), ESearchCase::IgnoreCase)) return 80;
			if (Status == TEXT("geometry-reset-boundary")) return 70;
			if (Status == TEXT("no-modification-before-consumer")) return 60;
			if (Status == TEXT("continued-to-dominating-writer")) return 50;
			if (Status == TEXT("adaptive-footprint-continued")) return 10;
			return 20;
		};
		auto ConfidenceRank = [](const FString& Confidence)
		{
			if (Confidence == TEXT("confirmed-executed-values")) return 50;
			if (Confidence.Contains(TEXT("confirmed-executed-uv"), ESearchCase::IgnoreCase)) return 40;
			if (Confidence == TEXT("same-extent")) return 30;
			if (Confidence == TEXT("candidate")) return 10;
			return 0;
		};
		auto AppendDistinctValue = [](FString& Existing, const FString& Value)
		{
			if (Value.IsEmpty() || Existing == Value || Existing.Contains(Value, ESearchCase::CaseSensitive))
			{
				return;
			}
			if (Existing.IsEmpty())
			{
				Existing = Value;
			}
			else if (!Existing.Contains(TEXT(" | "), ESearchCase::CaseSensitive))
			{
				Existing += TEXT(" | ") + Value;
			}
		};
		auto ParseAccessValue = [](const TSharedPtr<FJsonObject>& Value)
		{
			FPixelValueEvidence Result;
			if (!Value.IsValid())
			{
				return Result;
			}
			const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
			if (!Value->TryGetArrayField(TEXT("float"), Components) || !Components || Components->IsEmpty())
			{
				return Result;
			}
			double* Channels[] = { &Result.R, &Result.G, &Result.B, &Result.A };
			TArray<FString> Text;
			Result.bValid = true;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				const double Component = Components->IsValidIndex(Index) && (*Components)[Index].IsValid()
					&& (*Components)[Index]->Type == EJson::Number ? (*Components)[Index]->AsNumber() : 0.0;
				*Channels[Index] = Component;
				Text.Add(FString::Printf(TEXT("%.6g"), Component));
			}
			Result.Text = FString::Printf(TEXT("RGBA (%s)"), *FString::Join(Text, TEXT(", ")));
			return Result;
		};
		auto IsNeutralColor = [](const FPixelValueEvidence& Value)
		{
			return Value.bValid && FMath::Abs(Value.R) <= KINDA_SMALL_NUMBER
				&& FMath::Abs(Value.G) <= KINDA_SMALL_NUMBER
				&& FMath::Abs(Value.B) <= KINDA_SMALL_NUMBER;
		};

		TMap<FString, FCausalLaneEvidence> LanesByPurpose;
		TMap<FString, TMap<FString, int32>> BranchIndicesByPurpose;
		for (const TPair<uint32, FEventContextEvidence>& Pair : EventContexts)
		{
			const FEventContextEvidence& Context = Pair.Value;
			for (const TSharedPtr<FJsonValue>& HistoryValue : Context.ResourcePixelHistories)
			{
				const TSharedPtr<FJsonObject> History = HistoryValue.IsValid() ? HistoryValue->AsObject() : nullptr;
				if (!History.IsValid())
				{
					continue;
				}

				FString Purpose = TEXT("color");
				FString ResourceName = TEXT("unknown-resource");
				FString ShaderBinding;
				FString BranchStatus;
				FString MappingConfidence;
				double ResourceIndex = INDEX_NONE;
				double ProducerEventId = 0.0;
				double ResetBoundaryEventId = 0.0;
				double Sample = 0.0;
				double CollapsedAccessCount = 0.0;
				History->TryGetStringField(TEXT("tracePurpose"), Purpose);
				History->TryGetStringField(TEXT("resourceName"), ResourceName);
				History->TryGetStringField(TEXT("shaderBinding"), ShaderBinding);
				History->TryGetStringField(TEXT("branchStatus"), BranchStatus);
				History->TryGetStringField(TEXT("mappingConfidence"), MappingConfidence);
				History->TryGetNumberField(TEXT("resourceIndex"), ResourceIndex);
				History->TryGetNumberField(TEXT("selectedWriterEventId"), ProducerEventId);
				History->TryGetNumberField(TEXT("resetBoundaryEventId"), ResetBoundaryEventId);
				History->TryGetNumberField(TEXT("sample"), Sample);
				History->TryGetNumberField(TEXT("collapsedShaderAccessCount"), CollapsedAccessCount);
				if (Purpose.IsEmpty())
				{
					Purpose = TEXT("color");
				}
				const bool bBoundaryOnlyRecord = BranchStatus.Contains(TEXT("pruned"), ESearchCase::IgnoreCase)
					|| BranchStatus.Contains(TEXT("budget-exhausted"), ESearchCase::IgnoreCase)
					|| BranchStatus.Contains(TEXT("sample-limit"), ESearchCase::IgnoreCase)
					|| BranchStatus.Contains(TEXT("resource-limit"), ESearchCase::IgnoreCase);

				FCausalLaneEvidence& Lane = LanesByPurpose.FindOrAdd(Purpose);
				Lane.TracePurpose = Purpose;
				++Lane.EvidenceRecordCount;
				Lane.QueryRecordCount += bBoundaryOnlyRecord ? 0 : 1;
				const uint32 ProducerId = static_cast<uint32>(FMath::Max(0.0, ProducerEventId));
				const uint32 ResetId = static_cast<uint32>(FMath::Max(0.0, ResetBoundaryEventId));
				const int32 StableResourceIndex = static_cast<int32>(ResourceIndex);
				FString ResourceAccess;
				for (const FBoundResourceEvidence& Input : Context.Inputs)
				{
					if ((StableResourceIndex != INDEX_NONE && Input.ResourceIndex == StableResourceIndex)
						|| (!ShaderBinding.IsEmpty() && Input.ShaderBinding == ShaderBinding))
					{
						ResourceAccess = Input.Access;
						break;
					}
				}

				bool bExecutedShaderAccess = false;
				History->TryGetBoolField(TEXT("executedShaderAccess"), bExecutedShaderAccess);
				const TSharedPtr<FJsonObject>* AccessValueJson = nullptr;
				const FPixelValueEvidence AccessValue = History->TryGetObjectField(TEXT("shaderAccessResult"), AccessValueJson)
					&& AccessValueJson ? ParseAccessValue(*AccessValueJson) : FPixelValueEvidence();

				FPixelValueEvidence ProducerBefore;
				FPixelValueEvidence ProducerShaderOutput;
				FPixelValueEvidence ProducerWritten;
				FString ProducerActionKind;
				bool bProducerChangedValue = false;
				const TArray<TSharedPtr<FJsonValue>>* EventSummaries = nullptr;
				if (ProducerId > 0 && History->TryGetArrayField(TEXT("eventSummaries"), EventSummaries) && EventSummaries)
				{
					for (const TSharedPtr<FJsonValue>& EventValue : *EventSummaries)
					{
						const TSharedPtr<FJsonObject> Event = EventValue.IsValid() ? EventValue->AsObject() : nullptr;
						double EventId = 0.0;
						if (!Event.IsValid() || !Event->TryGetNumberField(TEXT("eventId"), EventId)
							|| static_cast<uint32>(EventId) != ProducerId)
						{
							continue;
						}
						Event->TryGetStringField(TEXT("actionKind"), ProducerActionKind);
						Event->TryGetBoolField(TEXT("changedTextureValue"), bProducerChangedValue);
						const TSharedPtr<FJsonObject>* PixelValue = nullptr;
						if (Event->TryGetObjectField(TEXT("firstBefore"), PixelValue) && PixelValue)
						{
							ProducerBefore = ParsePixelValue(*PixelValue);
						}
						if (Event->TryGetObjectField(TEXT("lastShaderOutput"), PixelValue) && PixelValue)
						{
							ProducerShaderOutput = ParsePixelValue(*PixelValue);
						}
						if (Event->TryGetObjectField(TEXT("lastAfter"), PixelValue) && PixelValue)
						{
							// postMod/lastAfter is the authoritative written resource value for draw,
							// copy and compute/UAV paths. Compute shaderOut is not an RT output value.
							ProducerWritten = ParsePixelValue(*PixelValue);
						}
						break;
					}
				}
				const bool bValueMatch = AccessValue.bValid && ProducerWritten.bValid
					&& ComputeColorDeltaMax(AccessValue, ProducerWritten) <= SignificantColorDeltaThreshold;
				const FString BranchKey = FString::Printf(TEXT("%u|%d|%s|%s|%u|%u"),
					Context.EventId, StableResourceIndex, *ResourceName, *ShaderBinding, ProducerId, ResetId);
				TMap<FString, int32>& BranchIndices = BranchIndicesByPurpose.FindOrAdd(Purpose);
				int32* ExistingIndex = BranchIndices.Find(BranchKey);
				if (!ExistingIndex)
				{
					FCausalLaneBranchEvidence Branch;
					Branch.TracePurpose = Purpose;
					Branch.ConsumerEventId = Context.EventId;
					Branch.ProducerEventId = ProducerId;
					Branch.ResetBoundaryEventId = ResetId;
					Branch.ResourceIndex = StableResourceIndex;
					Branch.ReverseDepth = EventContextDepths.FindRef(Context.EventId);
					Branch.ResourceName = ResourceName;
					Branch.ShaderBinding = ShaderBinding;
					Branch.ResourceAccess = ResourceAccess;
					Branch.BranchStatus = BranchStatus;
					Branch.MappingConfidence = MappingConfidence;
					Branch.ExecutedSampleValue = AccessValue.Text;
					Branch.ProducerBeforeValue = ProducerBefore.Text;
					Branch.ProducerShaderOutputValue = ProducerShaderOutput.Text;
					Branch.ProducerWrittenValue = ProducerWritten.Text;
					Branch.ProducerActionKind = ProducerActionKind;
					Branch.Samples.Add(static_cast<int32>(Sample));
					Branch.EvidenceRecordCount = 1;
					Branch.QueryRecordCount = bBoundaryOnlyRecord ? 0 : 1;
					Branch.CollapsedShaderAccessCount = static_cast<int32>(CollapsedAccessCount);
					Branch.bExecutedShaderAccess = bExecutedShaderAccess;
					Branch.bHasExecutedSampleValue = AccessValue.bValid;
					Branch.bExecutedSampleValueNeutral = IsNeutralColor(AccessValue);
					Branch.bProducerChangedValue = bProducerChangedValue;
					Branch.bProducerValueMatchesExecutedSample = bValueMatch;
					const int32 NewIndex = Lane.Branches.Add(MoveTemp(Branch));
					BranchIndices.Add(BranchKey, NewIndex);
					continue;
				}

				FCausalLaneBranchEvidence& Branch = Lane.Branches[*ExistingIndex];
				Branch.Samples.AddUnique(static_cast<int32>(Sample));
				++Branch.EvidenceRecordCount;
				Branch.QueryRecordCount += bBoundaryOnlyRecord ? 0 : 1;
				Branch.CollapsedShaderAccessCount = FMath::Max(Branch.CollapsedShaderAccessCount,
					static_cast<int32>(CollapsedAccessCount));
				AppendDistinctValue(Branch.ExecutedSampleValue, AccessValue.Text);
				AppendDistinctValue(Branch.ProducerBeforeValue, ProducerBefore.Text);
				AppendDistinctValue(Branch.ProducerShaderOutputValue, ProducerShaderOutput.Text);
				AppendDistinctValue(Branch.ProducerWrittenValue, ProducerWritten.Text);
				Branch.bExecutedShaderAccess |= bExecutedShaderAccess;
				if (AccessValue.bValid)
				{
					Branch.bExecutedSampleValueNeutral = Branch.bHasExecutedSampleValue
						? (Branch.bExecutedSampleValueNeutral && IsNeutralColor(AccessValue))
						: IsNeutralColor(AccessValue);
					Branch.bHasExecutedSampleValue = true;
				}
				Branch.bProducerChangedValue |= bProducerChangedValue;
				Branch.bProducerValueMatchesExecutedSample |= bValueMatch;
				if (Branch.ProducerActionKind.IsEmpty()) Branch.ProducerActionKind = ProducerActionKind;
				if (Branch.ResourceAccess.IsEmpty()) Branch.ResourceAccess = ResourceAccess;
				if (StatusRank(BranchStatus) > StatusRank(Branch.BranchStatus))
				{
					Branch.BranchStatus = BranchStatus;
				}
				if (ConfidenceRank(MappingConfidence) > ConfidenceRank(Branch.MappingConfidence))
				{
					Branch.MappingConfidence = MappingConfidence;
				}
			}
		}

		TArray<FCausalLaneEvidence> Result;
		static const TCHAR* OrderedPurposes[] = { TEXT("color"), TEXT("geometry"), TEXT("overlay") };
		for (const TCHAR* Purpose : OrderedPurposes)
		{
			FCausalLaneEvidence* Lane = LanesByPurpose.Find(Purpose);
			if (!Lane)
			{
				continue;
			}
			for (FCausalLaneBranchEvidence& Branch : Lane->Branches)
			{
				Branch.Samples.Sort();
				const bool bConsumerOutputFeedback = Branch.ResourceAccess.Contains(TEXT("write"), ESearchCase::IgnoreCase);
				const bool bConfirmedMapping = Branch.MappingConfidence.StartsWith(TEXT("confirmed-executed-"));
				if (bConsumerOutputFeedback)
				{
					Branch.EdgeRole = TEXT("consumer-read-write-output");
				}
				else if (Branch.ResetBoundaryEventId > 0)
				{
					Branch.EdgeRole = TEXT("reset-boundary");
				}
				else if (Branch.TracePurpose == TEXT("geometry") && Branch.ProducerEventId > 0)
				{
					Branch.EdgeRole = TEXT("geometry-owner");
				}
				else if (Branch.bHasExecutedSampleValue && Branch.bExecutedSampleValueNeutral)
				{
					Branch.EdgeRole = TEXT("neutral-input");
				}
				else if (Branch.ProducerEventId > 0 && Branch.bProducerChangedValue)
				{
					Branch.EdgeRole = TEXT("value-changing-producer");
				}
				else if (Branch.ProducerEventId > 0)
				{
					Branch.EdgeRole = TEXT("pass-through-producer");
				}
				else if (Branch.ResourceName.Contains(TEXT("History"), ESearchCase::IgnoreCase))
				{
					Branch.EdgeRole = TEXT("external-history-boundary");
				}
				else if (Branch.BranchStatus.Contains(TEXT("pruned"), ESearchCase::IgnoreCase)
					|| Branch.BranchStatus.Contains(TEXT("budget"), ESearchCase::IgnoreCase))
				{
					Branch.EdgeRole = TEXT("budget-boundary");
				}
				else
				{
					Branch.EdgeRole = TEXT("unresolved-boundary");
				}

				if (Branch.ResetBoundaryEventId > 0 || (Branch.TracePurpose == TEXT("geometry")
					&& Branch.ProducerEventId > 0 && bConfirmedMapping))
				{
					Branch.EdgeConfidence = TEXT("confirmed");
				}
				else if (Branch.ProducerEventId > 0 && bConfirmedMapping && Branch.bExecutedShaderAccess
					&& Branch.bHasExecutedSampleValue && (Branch.bProducerValueMatchesExecutedSample
						|| Branch.EdgeRole == TEXT("neutral-input")))
				{
					Branch.EdgeConfidence = TEXT("confirmed-value-flow");
				}
				else if (Branch.ProducerEventId > 0 && bConfirmedMapping && Branch.bExecutedShaderAccess)
				{
					Branch.EdgeConfidence = TEXT("strong-executed-read");
				}
				else if (Branch.ProducerEventId > 0)
				{
					Branch.EdgeConfidence = TEXT("partial-structural");
				}
				else
				{
					Branch.EdgeConfidence = TEXT("blocked");
				}
				if (Branch.ProducerEventId > 0) ++Lane->ConfirmedProducerCount;
				else if (Branch.ResetBoundaryEventId > 0) ++Lane->ResetBoundaryCount;
				else ++Lane->UnresolvedBoundaryCount;
			}
			Lane->Branches.Sort([](const FCausalLaneBranchEvidence& A, const FCausalLaneBranchEvidence& B)
			{
				if (A.ReverseDepth != B.ReverseDepth) return A.ReverseDepth < B.ReverseDepth;
				const bool bAProducer = A.ProducerEventId > 0;
				const bool bBProducer = B.ProducerEventId > 0;
				if (bAProducer != bBProducer) return bAProducer;
				if (A.ConsumerEventId != B.ConsumerEventId) return A.ConsumerEventId > B.ConsumerEventId;
				return A.ResourceName < B.ResourceName;
			});
			Result.Add(MoveTemp(*Lane));
		}
		return Result;
	}

	FPrimaryCausalPathEvidence BuildPrimaryColorPathEvidence(
		const TArray<FCausalLaneEvidence>& Lanes,
		const TMap<uint32, FEventContextEvidence>& EventContexts,
		uint32 RootEventId,
		int32 MaxHops)
	{
		FPrimaryCausalPathEvidence Result;
		Result.RootEventId = RootEventId;
		if (RootEventId == 0)
		{
			Result.StopReason = TEXT("missing-final-writer");
			return Result;
		}

		TArray<const FCausalLaneBranchEvidence*> AllBranches;
		for (const FCausalLaneEvidence& Lane : Lanes)
		{
			for (const FCausalLaneBranchEvidence& Branch : Lane.Branches)
			{
				AllBranches.Add(&Branch);
			}
		}
		TSet<uint32> VisitedEvents;
		uint32 ConsumerEventId = RootEventId;
		for (int32 Hop = 0; Hop < FMath::Max(1, MaxHops); ++Hop)
		{
			if (VisitedEvents.Contains(ConsumerEventId))
			{
				Result.StopReason = TEXT("cycle-rejected");
				Result.bReachedExplicitBoundary = true;
				break;
			}
			VisitedEvents.Add(ConsumerEventId);

			const FCausalLaneBranchEvidence* Best = nullptr;
			int32 BestScore = MIN_int32;
			for (const FCausalLaneBranchEvidence* Branch : AllBranches)
			{
				if (!Branch || Branch->ConsumerEventId != ConsumerEventId || Branch->TracePurpose == TEXT("geometry")
					|| Branch->ProducerEventId == Branch->ConsumerEventId)
				{
					continue;
				}
				int32 Score = 0;
				Score += Branch->ProducerEventId > 0 ? 100 : 0;
				Score += Branch->EdgeConfidence.StartsWith(TEXT("confirmed")) ? 90
					: (Branch->EdgeConfidence.StartsWith(TEXT("strong")) ? 65 : 0);
				Score += Branch->EdgeRole == TEXT("value-changing-producer") ? 90 : 0;
				Score += Branch->EdgeRole == TEXT("pass-through-producer") ? 80 : 0;
				Score += Branch->EdgeRole == TEXT("external-history-boundary") ? 70 : 0;
				Score -= Branch->EdgeRole == TEXT("neutral-input") ? 260 : 0;
				Score -= Branch->EdgeRole == TEXT("consumer-read-write-output") ? 400 : 0;
				Score -= Branch->ResourceName.Contains(TEXT("BlackDummy"), ESearchCase::IgnoreCase) ? 300 : 0;
				Score += Branch->ResourceName.Contains(TEXT("TSR.Output"), ESearchCase::IgnoreCase) ? 130 : 0;
				Score += Branch->ResourceName.Contains(TEXT("SceneColor"), ESearchCase::IgnoreCase) ? 120 : 0;
				Score += Branch->ResourceName.Contains(TEXT("Tonemap"), ESearchCase::IgnoreCase) ? 110 : 0;
				Score += Branch->ResourceName.Contains(TEXT("SelectionOutlineColor"), ESearchCase::IgnoreCase) ? 100 : 0;
				Score += Branch->ResourceName.Contains(TEXT("History.Color"), ESearchCase::IgnoreCase) ? 95 : 0;
				Score += Branch->ResourceName.Contains(TEXT("Bloom"), ESearchCase::IgnoreCase) ? 20 : 0;
				if (!Best || Score > BestScore || (Score == BestScore && Branch->ResourceIndex < Best->ResourceIndex))
				{
					Best = Branch;
					BestScore = Score;
				}
			}
			if (!Best)
			{
				if (const FEventContextEvidence* Context = EventContexts.Find(ConsumerEventId);
					Context && IsSceneSourceEvent(Context->ActionKind, Context->MarkerPath))
				{
					Result.bReachedConfirmedSceneSource = true;
					Result.StopReason = TEXT("scene-source-reached");
				}
				else
				{
					Result.StopReason = TEXT("no-upstream-color-edge");
				}
				break;
			}
			Result.Branches.Add(*Best);
			if (Best->ProducerEventId == 0)
			{
				Result.bReachedExplicitBoundary = true;
				Result.StopReason = Best->EdgeRole;
				break;
			}
			ConsumerEventId = Best->ProducerEventId;
		}
		if (Result.StopReason.IsEmpty())
		{
			Result.StopReason = TEXT("maximum-hop-boundary");
			Result.bReachedExplicitBoundary = true;
		}
		return Result;
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

	int32 SelectDominatingWriterSummaryIndex(const TArray<FEventSummaryEvidence>& Events,
		uint32 ConsumerEventId, const FString& TracePurpose)
	{
		if (TracePurpose == TEXT("geometry"))
		{
			for (int32 Index = Events.Num() - 1; Index >= 0; --Index)
			{
				const FEventSummaryEvidence& Event = Events[Index];
				if (Event.EventId == 0 || Event.EventId == ConsumerEventId
					|| (ConsumerEventId > 0 && Event.EventId >= ConsumerEventId))
				{
					continue;
				}
				if (Event.ActionKind == TEXT("clear"))
				{
					const bool bStencilOnlyClear = Event.MarkerPath.Contains(TEXT("ClearStencil"), ESearchCase::IgnoreCase)
						&& !Event.MarkerPath.Contains(TEXT("ClearDepthStencil"), ESearchCase::IgnoreCase);
					if (bStencilOnlyClear)
					{
						continue;
					}
					return Index;
				}
				if (Event.ActionKind == TEXT("draw") && Event.PassedFragments > 0
					&& Event.bChangedTextureValue)
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}

		int32 LatestPassedIndex = INDEX_NONE;
		for (int32 Index = Events.Num() - 1; Index >= 0; --Index)
		{
			const FEventSummaryEvidence& Event = Events[Index];
			if (Event.EventId == 0 || Event.EventId == ConsumerEventId
				|| (ConsumerEventId > 0 && Event.EventId >= ConsumerEventId))
			{
				continue;
			}
			if (Event.bChangedTextureValue)
			{
				return Index;
			}
			if (LatestPassedIndex == INDEX_NONE && Event.PassedFragments > 0)
			{
				LatestPassedIndex = Index;
			}
		}
		return LatestPassedIndex;
	}

	FString ClassifyResourceTracePurpose(const FString& ResourceName, const FString& ShaderBinding)
	{
		const FString Searchable = ResourceName + TEXT(" ") + ShaderBinding;
		if (Searchable.Contains(TEXT("Editor.Primitives"), ESearchCase::IgnoreCase)
			|| Searchable.Contains(TEXT("EditorPrimitives"), ESearchCase::IgnoreCase)
			|| Searchable.Contains(TEXT("SelectionOutline"), ESearchCase::IgnoreCase))
		{
			return TEXT("overlay");
		}
		if (Searchable.Contains(TEXT("Depth"), ESearchCase::IgnoreCase)
			|| Searchable.Contains(TEXT("Stencil"), ESearchCase::IgnoreCase)
			|| Searchable.Contains(TEXT("VisBuffer"), ESearchCase::IgnoreCase)
			|| Searchable.Contains(TEXT("ShadingMask"), ESearchCase::IgnoreCase)
			|| Searchable.Contains(TEXT("GPUScene"), ESearchCase::IgnoreCase))
		{
			return TEXT("geometry");
		}
		return TEXT("color");
	}

	bool IsSceneSourceEvent(const FString& ActionKind, const FString& MarkerPath)
	{
		if (ActionKind != TEXT("draw"))
		{
			return false;
		}
		return MarkerPath.Contains(TEXT("BasePass"), ESearchCase::IgnoreCase)
			|| MarkerPath.Contains(TEXT("PrePass"), ESearchCase::IgnoreCase)
			|| MarkerPath.Contains(TEXT("DepthPass"), ESearchCase::IgnoreCase)
			|| MarkerPath.Contains(TEXT("GBuffer"), ESearchCase::IgnoreCase)
			|| MarkerPath.Contains(TEXT("VisBuffer"), ESearchCase::IgnoreCase)
			|| MarkerPath.Contains(TEXT("NaniteRaster"), ESearchCase::IgnoreCase)
			|| MarkerPath.Contains(TEXT("Nanite Raster"), ESearchCase::IgnoreCase)
			|| MarkerPath.Contains(TEXT("RenderViewEditorPrimitives"), ESearchCase::IgnoreCase)
			|| MarkerPath.Contains(TEXT("/Game/"), ESearchCase::IgnoreCase)
			|| MarkerPath.Contains(TEXT(" SM_"), ESearchCase::IgnoreCase)
			|| MarkerPath.Contains(TEXT(" SK_"), ESearchCase::IgnoreCase);
	}

	FString BuildReplayPixelHistoryKey(int32 ResourceIndex, const FIntPoint& Pixel,
		int32 Mip, int32 Slice, int32 Sample, int32 TypeCast)
	{
		return FString::Printf(TEXT("%d:%d:%d:%d:%d:%d:%d"), ResourceIndex, Pixel.X, Pixel.Y,
			Mip, Slice, Sample, TypeCast);
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

	static FString ExtractMarkerIdentifier(const FString& MarkerPath, const TArray<FString>& Prefixes)
	{
		for (const FString& Prefix : Prefixes)
		{
			int32 SearchFrom = 0;
			while (SearchFrom < MarkerPath.Len())
			{
				const int32 Start = MarkerPath.Find(Prefix, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
				if (Start == INDEX_NONE)
				{
					break;
				}
				const bool bTokenBoundary = Start == 0
					|| (!FChar::IsAlnum(MarkerPath[Start - 1]) && MarkerPath[Start - 1] != TEXT('_'));
				if (!bTokenBoundary)
				{
					SearchFrom = Start + Prefix.Len();
					continue;
				}
				int32 End = Start + Prefix.Len();
				while (End < MarkerPath.Len())
				{
					const TCHAR Character = MarkerPath[End];
					if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-')
						&& Character != TEXT('.') && Character != TEXT('/'))
					{
						break;
					}
					++End;
				}
				return MarkerPath.Mid(Start, End - Start);
			}
		}
		return FString();
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
			const FString MaterialCandidate = ExtractMarkerIdentifier(Event.MarkerPath,
				{ TEXT("MI_"), TEXT("M_"), TEXT("WorldGridMaterial") });
			const FString MeshCandidate = ExtractMarkerIdentifier(Event.MarkerPath,
				{ TEXT("SM_"), TEXT("SK_"), TEXT("SkeletalMesh_") });
			UnrealAttribution->SetStringField(TEXT("material"),
				MaterialCandidate.IsEmpty() ? TEXT("unknown") : MaterialCandidate);
			UnrealAttribution->SetStringField(TEXT("mesh"), MeshCandidate.IsEmpty() ? TEXT("unknown") : MeshCandidate);
			UnrealAttribution->SetStringField(TEXT("actor"), TEXT("unknown"));
			UnrealAttribution->SetStringField(TEXT("status"),
				(MaterialCandidate.IsEmpty() && MeshCandidate.IsEmpty())
					? TEXT("requires-explicit-UE-marker-or-shader-map-evidence")
					: TEXT("marker-derived-candidate-not-actor-proof"));
			UnrealAttribution->SetStringField(TEXT("materialConfidence"),
				MaterialCandidate.IsEmpty() ? TEXT("unknown") : TEXT("candidate-from-marker"));
			UnrealAttribution->SetStringField(TEXT("meshConfidence"),
				MeshCandidate.IsEmpty() ? TEXT("unknown") : TEXT("candidate-from-marker"));
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
