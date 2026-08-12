#include "RenderTrailAnalyzerHomeInternal.h"

namespace UE::RenderTrail::Private
{
	void SAnalyzerHome::AddHypothesis(TArray<FString>& Hypotheses, const FString& Hypothesis)
	{
		if (!Hypothesis.IsEmpty() && Hypotheses.Num() < 3)
		{
			Hypotheses.AddUnique(Hypothesis);
		}
	}

	void SAnalyzerHome::AddFailureHypothesis(const FString& Failure, TArray<FString>& Hypotheses)
	{
		if (Failure == TEXT("depth-test"))
			AddHypothesis(Hypotheses, TEXT("深度测试拒绝：检查遮挡顺序、深度输出和当前深度比较方式。"));
		else if (Failure == TEXT("stencil-test"))
			AddHypothesis(Hypotheses, TEXT("模板测试拦截：检查模板参考值、掩码，以及更早写入模板的事件。"));
		else if (Failure == TEXT("shader-discard"))
			AddHypothesis(Hypotheses, TEXT("Shader 主动丢弃：检查透明度遮罩、clip 逻辑及其输入值。"));
		else if (Failure == TEXT("backface-cull"))
			AddHypothesis(Hypotheses, TEXT("背面剔除：检查顶点绕序、镜像变换和材质双面设置。"));
		else if (Failure == TEXT("scissor-clip") || Failure == TEXT("viewport-clip") || Failure == TEXT("depth-clip") || Failure == TEXT("depth-bounds"))
			AddHypothesis(Hypotheses, TEXT("光栅范围裁剪：检查 Viewport、Scissor、投影矩阵和深度范围。"));
		else if (Failure == TEXT("sample-mask"))
			AddHypothesis(Hypotheses, TEXT("采样覆盖不足：检查 MSAA Sample Mask 和 Alpha-to-Coverage。"));
		else if (Failure == TEXT("predication-skipped"))
			AddHypothesis(Hypotheses, TEXT("条件渲染跳过：检查 GPU Predicate 及生成该条件的上游事件。"));
	}

	void SAnalyzerHome::RenderCausalReport()
	{
		LastCandidate.Reset();
		LastSignificantCandidate.Reset();
		bLastCandidateHasDivergence = false;
		if (Samples.IsEmpty())
		{
			SetReportCards(
				TEXT("等待选择关注像素。这里不要求你提供一个“正确”的对照点。"),
				TEXT("最终画面\n↓\n等待选择 P1"),
				TEXT("尚无候选原因。"),
				TEXT("尚未执行 Pixel History 查询。"));
			return;
		}

		if (!bSelectionConfirmed)
		{
			SetReportCards(
				TEXT("选点已就绪，等待确认后开始分析。"),
				TEXT("最终画面\n↓\n已选 P 点\n↓\n等待确认"),
				TEXT("当前不会读取 Pixel History，也不会进行因果判断。"),
				TEXT("选点阶段不会触发后台查询；点击“分析”后才开始读取证据。"));
			return;
		}

		FString Report = FString::Printf(
			TEXT("BOUNDED SINGLE-PIXEL CAUSAL REPORT\n\nReplay target: resource=%d format=%s extent=%dx%d samples=%d\n\nPixel\n"),
			ReplayTargetResourceIndex, ReplayTargetFormat.IsEmpty() ? TEXT("unknown") : *ReplayTargetFormat,
			CurrentPreviewSize.X, CurrentPreviewSize.Y, ReplayTargetSamples);
		bool bAnyPending = false;
		bool bAnyIncompleteEventSummary = false;
		TArray<const FPixelSample*> ReadySamples;
		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			const FPixelSample& Sample = Samples[Index];
			bAnyPending |= Sample.bPending;
			FString State;
			if (Sample.bPending)
			{
				State = TEXT("querying");
			}
			else if (Sample.bFailed)
			{
				State = FString::Printf(TEXT("failed - %s"), *Sample.Error);
			}
			else
			{
				ReadySamples.Add(&Sample);
				bAnyIncompleteEventSummary |= !Sample.bEventSummaryComplete;
				State = FString::Printf(TEXT("%d modifications%s"), Sample.TotalModifications,
					Sample.bTruncated
						? (Sample.bEventSummaryComplete ? TEXT(", detail tail truncated; event summary complete") : TEXT(", event summary incomplete"))
						: TEXT(""));
			}
			Report += FString::Printf(TEXT("- P%d (%d, %d): %s\n"),
				Index + 1, Sample.Pixel.X, Sample.Pixel.Y, *State);
		}

		if (bAnyPending)
		{
			Report += TEXT("\nWaiting for bounded RenderDoc queries. No causal claim is made yet.");
			SetReportCards(
				TEXT("正在查询所选像素的 Pixel History；结果返回前不做因果判断。"),
				TEXT("最终画面\n↓\n正在收集 P 点的事件证据…"),
				TEXT("等待查询完成。"),
				Report);
			return;
		}

		if (ReadySamples.IsEmpty())
		{
			Report += TEXT("\nThe selected pixel query failed. The chain is broken before pixel evidence.");
			SetReportCards(
				TEXT("当前像素查询失败，目前没有 Pixel History 可以支持结论。"),
				TEXT("最终画面\n↓\nPixel History 不可用\n↓\n■ 因果链中断"),
				TEXT("可能是目标纹理、API 或当前截帧不支持 Pixel History；尚未证明任何渲染原因。"),
				Report);
			return;
		}

		if (bAnyIncompleteEventSummary)
		{
			Report += TEXT("\nEvent summary is incomplete for at least one point; the deterministic chain is stopped instead of guessing from a truncated tail.\n");
			SetReportCards(
				TEXT("Pixel History 返回了不完整的事件摘要，当前不会继续生成溯源结论。"),
				TEXT("最终画面\n↓\n事件摘要不完整\n↓\n■ 溯源暂停"),
				TEXT("需要重新编译并使用支持完整 eventSummaries 的 Replay Worker；这不是 Agent 可以补齐的缺口。"),
				Report);
			return;
		}

		TMap<uint64, TArray<FEventEvidence>> AggregatedBySample;
		for (const FPixelSample* Sample : ReadySamples)
		{
			AggregatedBySample.Add(Sample->Id, AggregateEvents(*Sample));
		}

		const FPixelSample* BaseSample = ReadySamples[0];
		const TArray<FEventEvidence>* BaseEvents = AggregatedBySample.Find(BaseSample->Id);

		FCausalCandidate Candidate;
		bool bHasCandidate = false;
		int32 FinalWriterIndex = INDEX_NONE;
		if (BaseEvents)
		{
			for (int32 EventIndex = BaseEvents->Num() - 1; EventIndex >= 0; --EventIndex)
			{
				const FEventEvidence& Event = (*BaseEvents)[EventIndex];
				if (Event.ActionKind == TEXT("present")
					|| (Event.PassedFragments <= 0 && !(Event.bDirectShaderWrite && Event.bChangedTextureValue)))
				{
					continue;
				}
				Candidate.Event = Event;
				Candidate.SampleCoverage = 1;
				bHasCandidate = true;
				FinalWriterIndex = EventIndex;
				break;
			}
		}

		if (bHasCandidate)
		{
			LastCandidate = Candidate;
			bLastCandidateHasDivergence = false;
			if (Candidate.Event.ColorDeltaMax >= 0.0
				&& Candidate.Event.ColorDeltaMax < SignificantColorDeltaThreshold && BaseEvents)
			{
				for (int32 EventIndex = FinalWriterIndex - 1; EventIndex >= 0; --EventIndex)
				{
					const FEventEvidence& Event = (*BaseEvents)[EventIndex];
					if (!IsConfirmedPixelWriter(Event) || Event.ActionKind == TEXT("present")
						|| Event.ColorDeltaMax < SignificantColorDeltaThreshold)
					{
						continue;
					}
					FCausalCandidate SignificantCandidate;
					SignificantCandidate.Event = Event;
					SignificantCandidate.SampleCoverage = 1;
					LastSignificantCandidate = MoveTemp(SignificantCandidate);
					break;
				}
			}
		}
		EnsureRelevantEventContexts();
		EnsureCandidateShaderDebug();

		FString CausalPath = TEXT("最终画面");
		TArray<FString> Hypotheses;
		for (const FPixelSample* Sample : ReadySamples)
		{
			const int32 DisplayIndex = Samples.IndexOfByPredicate([Sample](const FPixelSample& Item)
			{
				return Item.Id == Sample->Id;
			});
			const TArray<FEventEvidence>* Events = AggregatedBySample.Find(Sample->Id);
			const FEventEvidence* LatestWriter = nullptr;
			if (Events)
			{
				for (int32 EventIndex = Events->Num() - 1; EventIndex >= 0; --EventIndex)
				{
					const FEventEvidence& Event = (*Events)[EventIndex];
					if (Event.PassedFragments > 0 || (Event.bDirectShaderWrite && Event.bChangedTextureValue))
					{
						LatestWriter = &Event;
						break;
					}
				}
			}
			if (LatestWriter)
			{
				Report += FString::Printf(TEXT("- P%d latest observed writer: EID %u [%s] %s.\n"),
					DisplayIndex + 1, LatestWriter->EventId, *LatestWriter->ActionKind, *LatestWriter->Action);
				CausalPath += FString::Printf(TEXT("\n↓\nP%d (%d,%d) ← EID %u · %s"),
					DisplayIndex + 1, Sample->Pixel.X, Sample->Pixel.Y, LatestWriter->EventId, *LatestWriter->Action);
			}
			else
			{
				Report += FString::Printf(TEXT("- P%d has no confirmed write in the selected target history.\n"), DisplayIndex + 1);
				CausalPath += FString::Printf(TEXT("\n↓\nP%d (%d,%d) ← 未确认写入"), DisplayIndex + 1, Sample->Pixel.X, Sample->Pixel.Y);
				AddHypothesis(Hypotheses, FString::Printf(
					TEXT("P%d 没有确认写入：可能只保留了 Clear/背景、没有光栅覆盖，或目标不支持完整 Pixel History；这不等于已经证明对象被剔除。"),
					DisplayIndex + 1));
			}
		}

		FString Summary;
		if (bHasCandidate)
		{
			const FString Semantics = ClassifySemantics(Candidate.Event);
			const bool bDeltaKnown = Candidate.Event.ColorDeltaMax >= 0.0;
			const bool bMinorFinalAdjustment = bDeltaKnown
				&& Candidate.Event.ColorDeltaMax < SignificantColorDeltaThreshold;
			if (!bDeltaKnown)
			{
				Summary = FString::Printf(TEXT("EID %u 是 P1 的最后物理写入者，但 Before/After 数值不完整，当前不能判断它是末端微调还是主要颜色形成事件。"),
					Candidate.Event.EventId);
				CausalPath += FString::Printf(TEXT("\n↓\n最后写入 EID %u · %s（颜色差值不可用）"),
					Candidate.Event.EventId, *Candidate.Event.Action);
			}
			else if (bMinorFinalAdjustment)
			{
				Summary = LastSignificantCandidate.IsSet()
					? FString::Printf(TEXT("EID %u 是 P1 的最后物理写入者，但最大颜色变化仅 %.6g，属于末端微调；更早的 EID %u 是当前最近的显著写入候选，颜色形成原因仍需沿资源依赖继续证明。"),
						Candidate.Event.EventId, Candidate.Event.ColorDeltaMax, LastSignificantCandidate->Event.EventId)
					: FString::Printf(TEXT("EID %u 是 P1 的最后物理写入者，但最大颜色变化仅 %.6g，属于末端微调；当前链中尚未找到可确认的显著上游写入。"),
						Candidate.Event.EventId, Candidate.Event.ColorDeltaMax);
				CausalPath += FString::Printf(TEXT("\n↓\n最后写入 EID %u · %s（末端微调，Δmax=%.6g）"),
					Candidate.Event.EventId, *Candidate.Event.Action, Candidate.Event.ColorDeltaMax);
				if (LastSignificantCandidate.IsSet())
				{
					CausalPath += FString::Printf(TEXT("\n↓ 继续反向追踪\n显著写入候选 EID %u · %s（Δmax=%.6g）"),
						LastSignificantCandidate->Event.EventId, *LastSignificantCandidate->Event.Action,
						LastSignificantCandidate->Event.ColorDeltaMax);
				}
			}
			else
			{
				Summary = FString::Printf(TEXT("EID %u 是 P1 的最后物理写入者，并造成显著颜色变化（Δmax=%.6g）；它是当前主要写入候选，但跨 RT、材质和物体归属仍只接受显式证据。"),
					Candidate.Event.EventId, Candidate.Event.ColorDeltaMax);
				CausalPath += FString::Printf(TEXT("\n↓\n最后且显著写入 EID %u · %s（Δmax=%.6g）"),
					Candidate.Event.EventId, *Candidate.Event.Action, Candidate.Event.ColorDeltaMax);
			}
			CausalPath += FString::Printf(TEXT("\n↓\n■ 当前反向追踪边界：%s"),
				Semantics == TEXT("resample/nonlinear")
					? TEXT("发生缩放/重采样，不能假定同坐标继续上溯")
					: TEXT("输入资源仅按 producer 关系有界展开；像素贡献与坐标映射必须另行证明"));
			Report += FString::Printf(
				TEXT("\nFinal writer\n- EID %u [%s / %s] %s\n- result: %s\n- color change: %s；deltaMax=%.9g；deltaL1=%.9g\n"),
				Candidate.Event.EventId, *Candidate.Event.ActionKind, *Semantics, *Candidate.Event.Action,
				*DescribeEventResult(Candidate.Event), *ClassifyColorDelta(Candidate.Event),
				Candidate.Event.ColorDeltaMax, Candidate.Event.ColorDeltaL1);
			if (LastSignificantCandidate.IsSet())
			{
				const FEventEvidence& Significant = LastSignificantCandidate->Event;
				Report += FString::Printf(TEXT("\nSignificant upstream writer candidate\n- EID %u [%s / %s] %s\n- color change: %s；deltaMax=%.9g；deltaL1=%.9g\n"),
					Significant.EventId, *Significant.ActionKind, *ClassifySemantics(Significant), *Significant.Action,
					*ClassifyColorDelta(Significant), Significant.ColorDeltaMax, Significant.ColorDeltaL1);
			}

			for (const FString& Failure : Candidate.Event.FailureReasons)
			{
				AddFailureHypothesis(Failure, Hypotheses);
			}
			if (Candidate.Event.bUnboundPixelShader)
			{
				AddHypothesis(Hypotheses, TEXT("该事件没有可确认的像素着色器输出；检查 PS/RT 绑定，但不要据此推断 Mesh 身份。"));
			}
			if (Candidate.Event.bDirectShaderWrite)
			{
				AddHypothesis(Hypotheses, Candidate.Event.bChangedTextureValue
					? TEXT("UAV/Storage 写入改变了像素；当前证据指向 Dispatch/直接写入路径。")
					: TEXT("UAV/Storage 事件没有改变数值，可能只是命中历史中的干扰项。"));
			}
			else if (Candidate.Event.PassedFragments > 0 && Candidate.Event.ActionKind == TEXT("draw"))
			{
				AddHypothesis(Hypotheses, Semantics == TEXT("scene-write")
					? TEXT("场景 Draw 确认写入：可检查已证明的 Shader 输出、资源绑定以及可用的 UE Marker/Mesh 映射。")
					: TEXT("后处理 Draw 写入最终目标：优先看绑定输入，不能仅凭全屏 Draw 名称判断根因。"));
			}

			Report += TEXT("\nEvent Context（Shader、资源与 Pipeline）\n");
			if (const FEventContextEvidence* Context = EventContexts.Find(Candidate.Event.EventId))
			{
				Report += FString::Printf(TEXT("- Shader %s%s | debuggable: %s | source symbols: %s\n"),
					*Context->ShaderStage,
					Context->ShaderEntry.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" entry %s"), *Context->ShaderEntry),
					Context->bShaderDebuggable ? TEXT("yes") : TEXT("no"),
					Context->bSourceDebugInfo ? TEXT("yes") : TEXT("no"));
				if (!Context->TraceStopReason.IsEmpty())
				{
					Report += FString::Printf(TEXT("- Focused trace stop: %s (scene source Pipeline/Shader evidence retained)\n"),
						*Context->TraceStopReason);
				}
				Report += FString::Printf(TEXT("- Shader reflection：encoding=%s；inputSig=%d；outputSig=%d；constantBlocks=%d；samplers=%d；RO=%d；RW=%d\n"),
					Context->ShaderEncoding.IsEmpty() ? TEXT("unknown") : *Context->ShaderEncoding,
					Context->ShaderInputSignatureCount, Context->ShaderOutputSignatureCount,
					Context->ShaderConstantBlockCount, Context->ShaderSamplerCount,
					Context->ShaderReadOnlyResourceCount, Context->ShaderReadWriteResourceCount);
				int32 ShownInputs = 0;
				for (const FBoundResourceEvidence& Input : Context->Inputs)
				{
					if (ShownInputs++ >= MaxDisplayedFrontierResources)
					{
						break;
					}
					Report += Input.bTexture
						? FString::Printf(TEXT("- Bound input candidate: %s binding=%s [%s, %s] %s %dx%d samples=%d；像素贡献尚未由绑定本身证明\n"),
							*Input.Name, Input.ShaderBinding.IsEmpty() ? TEXT("unknown") : *Input.ShaderBinding,
							*Input.Stage, *Input.Access, *Input.Format, Input.Width, Input.Height, Input.Samples)
						: FString::Printf(TEXT("- Bound input candidate: %s [%s, %s] buffer/resource；像素贡献尚未证明\n"),
							*Input.Name, *Input.Stage, *Input.Access);
				}
				int32 ShownOutputs = 0;
				for (const FBoundResourceEvidence& Output : Context->Outputs)
				{
					if (ShownOutputs++ >= MaxDisplayedFrontierResources)
					{
						break;
					}
					Report += Output.bTexture
						? FString::Printf(TEXT("- Output: %s [%s, %s] %dx%d\n"),
							*Output.Name, *Output.Stage, *Output.Access, Output.Width, Output.Height)
						: FString::Printf(TEXT("- Output: %s [%s, %s] buffer/resource\n"),
							*Output.Name, *Output.Stage, *Output.Access);
				}
				for (const TSharedPtr<FJsonValue>& ProvenanceValue : Context->ResourceProvenance)
				{
					const TSharedPtr<FJsonObject> Provenance = ProvenanceValue.IsValid() ? ProvenanceValue->AsObject() : nullptr;
					if (!Provenance.IsValid())
					{
						continue;
					}
					FString ResourceName;
					FString ProducerAction;
					FString ProducerKind;
					FString ProducerStatus;
					FString ProducerUsage;
					FString CoordinateMapping;
					FString PixelTraceStatus;
					FString ChainBreak;
					bool bProducerFound = false;
					double ProducerEventId = 0.0;
					double InvalidatingEventId = 0.0;
					Provenance->TryGetStringField(TEXT("resource"), ResourceName);
					Provenance->TryGetStringField(TEXT("producerAction"), ProducerAction);
					Provenance->TryGetStringField(TEXT("producerKind"), ProducerKind);
					Provenance->TryGetStringField(TEXT("producerStatus"), ProducerStatus);
					Provenance->TryGetStringField(TEXT("producerUsage"), ProducerUsage);
					Provenance->TryGetStringField(TEXT("coordinateMapping"), CoordinateMapping);
					Provenance->TryGetStringField(TEXT("pixelTraceStatus"), PixelTraceStatus);
					Provenance->TryGetStringField(TEXT("chainBreak"), ChainBreak);
					Provenance->TryGetBoolField(TEXT("producerFound"), bProducerFound);
					Provenance->TryGetNumberField(TEXT("producerEventId"), ProducerEventId);
					Provenance->TryGetNumberField(TEXT("invalidatingEventId"), InvalidatingEventId);
					if (ProducerStatus == TEXT("invalidated"))
					{
						Report += FString::Printf(TEXT("- Resource chain break: %s 在 EID %u 被 Discard；Discard 不是 producer。%s\n"),
							*ResourceName, static_cast<uint32>(InvalidatingEventId), ChainBreak.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" %s"), *ChainBreak));
					}
					else if (bProducerFound)
					{
						Report += FString::Printf(TEXT("- Resource producer edge: %s ← EID %u [%s/%s] %s；%s；pixel trace=%s；该资源关系不自动证明像素贡献\n"), *ResourceName,
							static_cast<uint32>(ProducerEventId), *ProducerKind, *ProducerUsage, *ProducerAction, *CoordinateMapping,
							PixelTraceStatus.IsEmpty() ? TEXT("unknown") : *PixelTraceStatus);
					}
					else
					{
						Report += FString::Printf(TEXT("- Resource chain break: %s ← 未找到此前有效写入；%s；%s\n"),
							*ResourceName, *CoordinateMapping, *ChainBreak);
					}
				}

				if (!Context->ResourcePixelHistories.IsEmpty())
				{
					Report += TEXT("\nResource/sample Pixel History branches\n");
					for (const TSharedPtr<FJsonValue>& HistoryValue : Context->ResourcePixelHistories)
					{
						const TSharedPtr<FJsonObject> History = HistoryValue.IsValid() ? HistoryValue->AsObject() : nullptr;
						if (!History.IsValid())
						{
							continue;
						}
						FString ResourceName;
						FString ShaderBinding;
						FString BranchStatus;
						FString Mapping;
						FString MappingConfidence;
						FString TracePurpose;
						FString CoordinateEvidence;
						double X = 0.0;
						double Y = 0.0;
						double SampleIndex = 0.0;
						double TotalEvents = 0.0;
						double SelectedWriterEventId = 0.0;
						double DominatedWriterCount = 0.0;
						History->TryGetStringField(TEXT("resourceName"), ResourceName);
						History->TryGetStringField(TEXT("shaderBinding"), ShaderBinding);
						History->TryGetStringField(TEXT("branchStatus"), BranchStatus);
						History->TryGetStringField(TEXT("coordinateMapping"), Mapping);
						History->TryGetStringField(TEXT("mappingConfidence"), MappingConfidence);
						History->TryGetStringField(TEXT("tracePurpose"), TracePurpose);
						History->TryGetStringField(TEXT("coordinateEvidence"), CoordinateEvidence);
						History->TryGetNumberField(TEXT("x"), X);
						History->TryGetNumberField(TEXT("y"), Y);
						History->TryGetNumberField(TEXT("sample"), SampleIndex);
						History->TryGetNumberField(TEXT("totalEvents"), TotalEvents);
						History->TryGetNumberField(TEXT("selectedWriterEventId"), SelectedWriterEventId);
						History->TryGetNumberField(TEXT("dominatedWriterCount"), DominatedWriterCount);
						Report += FString::Printf(TEXT("- [%s] %s/%s pixel=(%d,%d) sample=%d mapping=%s confidence=%s events=%d selectedWriter=EID %u visibleAlternatives=%d status=%s\n"),
							TracePurpose.IsEmpty() ? TEXT("unknown") : *TracePurpose,
							*ResourceName, ShaderBinding.IsEmpty() ? TEXT("unknown-binding") : *ShaderBinding,
							static_cast<int32>(X), static_cast<int32>(Y), static_cast<int32>(SampleIndex),
							Mapping.IsEmpty() ? TEXT("unknown") : *Mapping,
							MappingConfidence.IsEmpty() ? TEXT("unknown") : *MappingConfidence,
							static_cast<int32>(TotalEvents), static_cast<uint32>(SelectedWriterEventId),
							static_cast<int32>(DominatedWriterCount),
							BranchStatus.IsEmpty() ? TEXT("unknown") : *BranchStatus);
						if (!CoordinateEvidence.IsEmpty())
						{
							Report += FString::Printf(TEXT("  coordinate evidence: %s\n"), *CoordinateEvidence);
						}

						const TArray<TSharedPtr<FJsonValue>>* EventSummaries = nullptr;
						if (History->TryGetArrayField(TEXT("eventSummaries"), EventSummaries) && EventSummaries)
						{
							Report += TEXT("  Returned Pixel History events (complete; passed/changed draws first)\n");
							auto AppendHistoryEvents = [&Report, EventSummaries](bool bPassedOrChangedGroup)
							{
								for (const TSharedPtr<FJsonValue>& EventValue : *EventSummaries)
								{
									const TSharedPtr<FJsonObject> Event = EventValue.IsValid() ? EventValue->AsObject() : nullptr;
									if (!Event.IsValid())
									{
										continue;
									}
									double EventId = 0.0;
									double PassedFragments = 0.0;
									double RejectedFragments = 0.0;
									double PrimitiveId = 0.0;
									bool bChanged = false;
									FString Action;
									FString ActionKind;
									FString MarkerPath;
									Event->TryGetNumberField(TEXT("eventId"), EventId);
									Event->TryGetNumberField(TEXT("passedFragments"), PassedFragments);
									Event->TryGetNumberField(TEXT("rejectedFragments"), RejectedFragments);
									Event->TryGetNumberField(TEXT("lastPrimitiveId"), PrimitiveId);
									Event->TryGetBoolField(TEXT("changedTextureValue"), bChanged);
									Event->TryGetStringField(TEXT("action"), Action);
									Event->TryGetStringField(TEXT("actionKind"), ActionKind);
									Event->TryGetStringField(TEXT("markerPath"), MarkerPath);
									const bool bPassedOrChanged = PassedFragments > 0.0 || bChanged;
									if (bPassedOrChanged != bPassedOrChangedGroup)
									{
										continue;
									}
									TArray<FString> FailureReasons;
									const TArray<TSharedPtr<FJsonValue>>* Failures = nullptr;
									if (Event->TryGetArrayField(TEXT("failureReasons"), Failures) && Failures)
									{
										for (const TSharedPtr<FJsonValue>& Failure : *Failures)
										{
											if (Failure.IsValid() && Failure->Type == EJson::String)
											{
												FailureReasons.Add(Failure->AsString());
											}
										}
									}
									Report += FString::Printf(
										TEXT("    %s EID %u [%s] passed=%d rejected=%d changed=%s primitive=%u action=%s\n      marker=%s%s\n"),
										bPassedOrChanged ? TEXT("[PASS/WRITE]") : TEXT("[REJECTED]"),
										static_cast<uint32>(EventId), ActionKind.IsEmpty() ? TEXT("unknown") : *ActionKind,
										static_cast<int32>(PassedFragments), static_cast<int32>(RejectedFragments),
										bChanged ? TEXT("true") : TEXT("false"), static_cast<uint32>(PrimitiveId),
										Action.IsEmpty() ? TEXT("<unnamed>") : *Action, *CompactMarkerPath(MarkerPath),
										FailureReasons.IsEmpty()
											? TEXT("")
											: *FString::Printf(TEXT("\n      failures=%s"), *FString::Join(FailureReasons, TEXT(","))));
								}
							};
							AppendHistoryEvents(true);
							AppendHistoryEvents(false);
						}
					}
				}

				Report += TEXT("\nPipeline 固定功能状态\n");
				Report += EvidenceFormatting::FormatPipelineState(Context->PipelineState);
				Report += TEXT("\nShader 算法证据\n");
				if (Context->ShaderDebugTrace.IsValid())
				{
					Report += EvidenceFormatting::FormatShaderDebugTrace(Context->ShaderDebugTrace);
				}
				else if (Context->bShaderDebuggable && Context->bSourceDebugInfo)
				{
					Report += TEXT("- Shader has debug/source information; this report currently has reflection and bindings only, not an instruction-level trace.\n");
				}
				else
				{
					Report += TEXT("- Algorithm is not proven: entry name and bound input do not identify the math performed by Main. Shader source/debug trace is unavailable for this event.\n");
				}
				if (!Context->ShaderDebugStatus.IsEmpty())
				{
					Report += FString::Printf(TEXT("- Shader debug status: %s\n"), *Context->ShaderDebugStatus);
				}
			}
			else if (FailedEventContextIds.Contains(Candidate.Event.EventId))
			{
				Report += TEXT("- Event-context query failed; the chain remains broken here.\n");
			}
			else
			{
				Report += TEXT("- Querying this event's used inputs and shader-debug capability.\n");
			}
		}
		else
		{
			Summary = TEXT("Pixel History 没有提供可用的非 Present 事件；当前只能确认最终目标处的因果链中断。");
			CausalPath += TEXT("\n↓\n未找到可追踪 GPU 写入\n↓\n■ 因果链中断");
			AddHypothesis(Hypotheses,
				TEXT("当前目标可能只有背景/Clear、没有可见 Fragment，或该 API/资源的 Pixel History 不完整；不能据此断言具体剔除原因。"));
		}

		if (Hypotheses.IsEmpty())
		{
			AddHypothesis(Hypotheses,
				TEXT("当前证据只说明像素实际经历的 GPU 过程；没有设计期参考值时，不判断颜色“应该”是什么。"));
		}
		FString Suspects;
			Report += TEXT("\n证据支持的可能性\n");
		for (int32 Index = 0; Index < Hypotheses.Num(); ++Index)
		{
			Report += FString::Printf(TEXT("%d. %s\n"), Index + 1, *Hypotheses[Index]);
			Suspects += FString::Printf(TEXT("%d  %s%s"), Index + 1, *Hypotheses[Index],
				Index + 1 < Hypotheses.Num() ? TEXT("\n\n") : TEXT(""));
		}

		for (const FPixelSample* Sample : ReadySamples)
		{
			const int32 DisplayIndex = Samples.IndexOfByPredicate([Sample](const FPixelSample& Item)
			{
				return Item.Id == Sample->Id;
			});
			if (const TArray<FEventEvidence>* Events = AggregatedBySample.Find(Sample->Id))
			{
				Report += FString::Printf(TEXT("\nPass / event chain P%d (marker-derived; showing latest %d/%d events)\n"),
					DisplayIndex + 1, FMath::Min(MaxDisplayedTraceHops, Events->Num()), Events->Num());
				Report += TEXT("Pass boundary is inferred from RenderDoc action/marker data; a generic Slate ElementBatch marker is not a guaranteed RenderGraph pass name.\n");
				const int32 First = FMath::Max(0, Events->Num() - MaxDisplayedTraceHops);
				int32 Hop = 0;
				for (int32 EventIndex = Events->Num() - 1; EventIndex >= First; --EventIndex)
				{
					const FEventEvidence& Event = (*Events)[EventIndex];
					Report += FString::Printf(TEXT("%d. EID %u [%s] %s\n   marker/pass: %s\n   semantic: %s | flags=%u | result: %s\n   pixel: before=%s | shaderOutput=%s | after=%s\n   change: %s | deltaMax=%.9g | deltaL1=%.9g\n"),
						++Hop, Event.EventId, *Event.ActionKind, *Event.Action,
						*CompactMarkerPath(Event.MarkerPath), *ClassifySemantics(Event), Event.ActionFlags, *DescribeEventResult(Event),
						*Event.Before, *Event.ShaderOutput, *Event.After,
						*ClassifyColorDelta(Event), Event.ColorDeltaMax, Event.ColorDeltaL1);
					if (const FEventContextEvidence* Context = EventContexts.Find(Event.EventId))
					{
						Report += FString::Printf(TEXT("   context: shader=%s%s; inputs=%d; outputs=%d; fixedFunction=%s; debugTrace=%s\n"),
							*Context->ShaderStage,
							Context->ShaderEntry.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("/%s"), *Context->ShaderEntry),
							Context->Inputs.Num(), Context->Outputs.Num(),
							Context->PipelineState.IsValid() ? TEXT("available") : TEXT("unavailable"),
							Context->ShaderDebugTrace.IsValid() ? TEXT("available") : TEXT("not-run"));
					}
				}
			}
		}

		const FAgentContextCoverageSelection ContextSelection = BuildAgentContextCoverageSelection();
		if (!ContextSelection.DetailedEventIds.IsEmpty())
		{
			Report += FString::Printf(TEXT("\nAgent causal-coverage detail selection (%d/%d contexts; full index is included in Agent evidence)\n"),
				ContextSelection.DetailedEventIds.Num(), EventContexts.Num());
			for (const uint32 EventId : ContextSelection.DetailedEventIds)
			{
				const FEventContextEvidence& Context = EventContexts.FindChecked(EventId);
				const FAgentContextCoverageEvidence& Coverage =
					ContextSelection.CoverageByEventId.FindChecked(EventId);
				const FString Roles = FString::Join(GetAgentContextCoverageRoles(Coverage), TEXT(","));
				const TArray<FString>* Reasons = ContextSelection.SelectionReasons.Find(EventId);
				Report += FString::Printf(TEXT("- depth=%d causalDistance=%s EID %u [%s] %s\n  roles=%s；selectedBecause=%s\n  marker/pass: %s\n  pixelEvidence: passed=%d rejected=%d changed=%s\n  shader: %s%s；inputs=%d；outputs=%d；resource/sample branches=%d；pipeline=%s\n"),
					Coverage.ReverseDepth,
					Coverage.CausalDistance == MAX_int32 ? TEXT("unlinked")
						: *FString::FromInt(Coverage.CausalDistance),
					EventId, *Context.ActionKind, *Context.Action, *Roles,
					Reasons ? *FString::Join(*Reasons, TEXT(",")) : TEXT("coverage-score-fill"),
					*CompactMarkerPath(Context.MarkerPath),
					Coverage.PassedFragments, Coverage.RejectedFragments,
					Coverage.bChangedTextureValue ? TEXT("true") : TEXT("false"),
					*Context.ShaderStage,
					Context.ShaderEntry.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("/%s"), *Context.ShaderEntry),
					Context.Inputs.Num(), Context.Outputs.Num(), Context.ResourcePixelHistories.Num(),
					Context.PipelineState.IsValid() ? TEXT("available") : TEXT("unavailable"));
			}
		}

		const FString HistoryCoverage = BuildHistoryCoverageText();
		Report += TEXT("\n") + HistoryCoverage + TEXT("\n");
		if (!FullTraceSnapshotPath.IsEmpty())
		{
			Report += FString::Printf(
				TEXT("\n全量追踪（未经过 Agent 裁剪）\n- 实时原始记录：%s\n- 完整快照：%s\n- 当前记录：%d（请求 %d / 响应 %d）\n- 注意：Agent 只读取最多 %d/%d 个上下文；它的回答不是全量追踪。\n"),
				*FullTraceJsonlPath, *FullTraceSnapshotPath, FullTraceRecordCount,
				FullTraceRequestCount, FullTraceResponseCount,
				ContextSelection.DetailedEventIds.Num(), EventContexts.Num());
		}
		if (!BudgetDeferredResourcePixelHistoryRequests.IsEmpty()
			|| !BudgetDeferredEventContextDepths.IsEmpty())
		{
			Summary += FString::Printf(TEXT(" 自动深追已达到安全上限：仍有 %d 个资源组（%d 个候选坐标）和 %d 个 producer 上下文被预算截断；这不表示没有更早原因。"),
				GetBudgetDeferredResourceHistoryCount(), BudgetDeferredResourcePixelHistoryRequests.Num(),
				BudgetDeferredEventContextDepths.Num());
		}

		Report += FString::Printf(TEXT("\nConfidence: %s. Applies only to the observed GPU chain, not design intent or engine-side cause.\n"),
			ReadySamples.Num() >= 2 ? TEXT("medium") : TEXT("low"));
		SetReportCards(Summary, CausalPath, Suspects, Report);
	}
}
