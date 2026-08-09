#include "RenderTrailAnalyzerEvidence.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace UE::RenderTrail::Private
{
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderTrailTracePolicyTest,
		"RenderTrail.Analyzer.Trace.FocusedPolicy",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FRenderTrailTracePolicyTest::RunTest(const FString& Parameters)
	{
		TArray<FEventSummaryEvidence> Events;
		FEventSummaryEvidence ChangedWriter;
		ChangedWriter.EventId = 10;
		ChangedWriter.ActionKind = TEXT("draw");
		ChangedWriter.PassedFragments = 1;
		ChangedWriter.bChangedTextureValue = true;
		Events.Add(ChangedWriter);

		FEventSummaryEvidence SameValueWriter;
		SameValueWriter.EventId = 11;
		SameValueWriter.ActionKind = TEXT("draw");
		SameValueWriter.PassedFragments = 1;
		SameValueWriter.bChangedTextureValue = false;
		Events.Add(SameValueWriter);

		TestEqual(TEXT("Value-changing writer dominates a later same-value pass"),
			SelectDominatingWriterSummaryIndex(Events, 20, TEXT("color")), 0);

		Events[0].bChangedTextureValue = false;
		TestEqual(TEXT("Latest passed writer is the explicit fallback"),
			SelectDominatingWriterSummaryIndex(Events, 20, TEXT("color")), 1);

		TArray<FEventSummaryEvidence> GeometryEvents;
		FEventSummaryEvidence DepthReset;
		DepthReset.EventId = 1;
		DepthReset.ActionKind = TEXT("clear");
		DepthReset.MarkerPath = TEXT("ClearDepthStencil (SceneDepthZ)");
		DepthReset.PassedFragments = 1;
		DepthReset.bChangedTextureValue = true;
		GeometryEvents.Add(DepthReset);
		FEventSummaryEvidence MeshDepthWriter;
		MeshDepthWriter.EventId = 2;
		MeshDepthWriter.ActionKind = TEXT("draw");
		MeshDepthWriter.MarkerPath = TEXT("PrePass > /Game/Meshes/SM_Cylinder");
		MeshDepthWriter.PassedFragments = 1;
		MeshDepthWriter.bChangedTextureValue = true;
		GeometryEvents.Add(MeshDepthWriter);
		FEventSummaryEvidence StencilReset;
		StencilReset.EventId = 3;
		StencilReset.ActionKind = TEXT("clear");
		StencilReset.MarkerPath = TEXT("ClearStencil (SceneDepthZ)");
		StencilReset.PassedFragments = 1;
		StencilReset.bChangedTextureValue = true;
		GeometryEvents.Add(StencilReset);
		TestEqual(TEXT("Stencil-only clear does not hide the latest depth-owning Mesh draw"),
			SelectDominatingWriterSummaryIndex(GeometryEvents, 10, TEXT("geometry")), 1);

		FEventSummaryEvidence LaterDepthReset = DepthReset;
		LaterDepthReset.EventId = 4;
		GeometryEvents.Add(LaterDepthReset);
		TestEqual(TEXT("A real depth clear remains an ownership reset boundary"),
			SelectDominatingWriterSummaryIndex(GeometryEvents, 10, TEXT("geometry")), 3);

		TestEqual(TEXT("Editor selection resources are overlay evidence"),
			ClassifyResourceTracePurpose(TEXT("Editor.Primitives.Color"), TEXT("SelectionOutlineTexture")),
			FString(TEXT("overlay")));
		TestEqual(TEXT("Depth and visibility resources are geometry evidence"),
			ClassifyResourceTracePurpose(TEXT("SceneDepthZ"), TEXT("SceneDepthTexture")),
			FString(TEXT("geometry")));
		TestEqual(TEXT("Scene color resources remain color evidence"),
			ClassifyResourceTracePurpose(TEXT("SceneColor"), TEXT("ColorTexture")),
			FString(TEXT("color")));

		TestTrue(TEXT("BasePass draw is a bounded scene-source stop"),
			IsSceneSourceEvent(TEXT("draw"), TEXT("BasePass > /Game/Meshes/SM_Cylinder")));
		TestFalse(TEXT("Final composite is not mistaken for the scene source"),
			IsSceneSourceEvent(TEXT("draw"), TEXT("PostProcessing > CompositeEditorPrimitives")));

		const FString ReplayKey = BuildReplayPixelHistoryKey(7, FIntPoint(12, 34), 1, 2, 3, 4);
		TestEqual(TEXT("Replay cache key contains only replay-affecting inputs"),
			ReplayKey, FString(TEXT("7:12:34:1:2:3:4")));

		auto MakeHistory = [](const FString& Purpose, const FString& Resource, uint32 Writer,
			int32 Sample, const FString& Status)
		{
			const TSharedRef<FJsonObject> History = MakeShared<FJsonObject>();
			History->SetStringField(TEXT("tracePurpose"), Purpose);
			History->SetStringField(TEXT("resourceName"), Resource);
			History->SetNumberField(TEXT("selectedWriterEventId"), Writer);
			History->SetNumberField(TEXT("sample"), Sample);
			History->SetStringField(TEXT("branchStatus"), Status);
			History->SetStringField(TEXT("mappingConfidence"), TEXT("confirmed-executed-values"));
			History->SetNumberField(TEXT("collapsedShaderAccessCount"), 2);
			return MakeShared<FJsonValueObject>(History);
		};
		TMap<uint32, FEventContextEvidence> LaneContexts;
		TMap<uint32, int32> LaneDepths;
		FEventContextEvidence ColorConsumer;
		ColorConsumer.EventId = 100;
		ColorConsumer.ResourcePixelHistories.Add(MakeHistory(TEXT("color"), TEXT("Tonemap"), 90, 0,
			TEXT("continued-to-dominating-writer")));
		ColorConsumer.ResourcePixelHistories.Add(MakeHistory(TEXT("color"), TEXT("Tonemap"), 90, 1,
			TEXT("continued-to-dominating-writer")));
		ColorConsumer.ResourcePixelHistories.Add(MakeHistory(TEXT("color"), TEXT("CombineLUTs"), 0, 0,
			TEXT("adaptive-footprint-continued")));
		ColorConsumer.ResourcePixelHistories.Add(MakeHistory(TEXT("color"), TEXT("CombineLUTs"), 0, 0,
			TEXT("no-modification-before-consumer")));
		LaneContexts.Add(ColorConsumer.EventId, ColorConsumer);
		LaneDepths.Add(ColorConsumer.EventId, 1);
		FEventContextEvidence GeometryConsumer;
		GeometryConsumer.EventId = 110;
		GeometryConsumer.ResourcePixelHistories.Add(MakeHistory(TEXT("geometry"), TEXT("SceneDepthZ"), 2, 0,
			TEXT("continued-to-dominating-writer")));
		LaneContexts.Add(GeometryConsumer.EventId, GeometryConsumer);
		LaneDepths.Add(GeometryConsumer.EventId, 0);

		const TArray<FCausalLaneEvidence> Lanes = BuildCausalLaneEvidence(LaneContexts, LaneDepths);
		TestEqual(TEXT("Color and geometry remain separate causal lanes"), Lanes.Num(), 2);
		const FCausalLaneEvidence* ColorLane = Lanes.FindByPredicate([](const FCausalLaneEvidence& Lane)
		{
			return Lane.TracePurpose == TEXT("color");
		});
		TestNotNull(TEXT("Color lane is present"), ColorLane);
		if (ColorLane)
		{
			TestEqual(TEXT("MSAA samples collapse into one producer branch"), ColorLane->Branches.Num(), 2);
			const FCausalLaneBranchEvidence* TonemapBranch = ColorLane->Branches.FindByPredicate(
				[](const FCausalLaneBranchEvidence& Branch) { return Branch.ResourceName == TEXT("Tonemap"); });
			TestNotNull(TEXT("Tonemap branch is present"), TonemapBranch);
			if (TonemapBranch)
			{
				TestEqual(TEXT("Grouped branch retains both MSAA sample indices"), TonemapBranch->Samples.Num(), 2);
				TestEqual(TEXT("Grouped branch retains raw query count"), TonemapBranch->QueryRecordCount, 2);
			}
			const FCausalLaneBranchEvidence* LutBoundary = ColorLane->Branches.FindByPredicate(
				[](const FCausalLaneBranchEvidence& Branch) { return Branch.ResourceName == TEXT("CombineLUTs"); });
			TestNotNull(TEXT("Adaptive LUT boundary is present"), LutBoundary);
			if (LutBoundary)
			{
				TestEqual(TEXT("Terminal no-modification status replaces intermediate adaptive status"),
					LutBoundary->BranchStatus, FString(TEXT("no-modification-before-consumer")));
			}
		}

		auto MakePixelValue = [](double R, double G, double B, double A)
		{
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetBoolField(TEXT("valid"), true);
			TArray<TSharedPtr<FJsonValue>> Components;
			Components.Add(MakeShared<FJsonValueNumber>(R));
			Components.Add(MakeShared<FJsonValueNumber>(G));
			Components.Add(MakeShared<FJsonValueNumber>(B));
			Components.Add(MakeShared<FJsonValueNumber>(A));
			Value->SetArrayField(TEXT("float"), MoveTemp(Components));
			Value->SetNumberField(TEXT("depth"), -1.0);
			Value->SetNumberField(TEXT("stencil"), -1.0);
			return Value;
		};
		auto MakeValueFlowHistory = [&MakePixelValue](const FString& Resource, int32 ResourceIndex,
			const FString& Binding, uint32 Writer, bool bWriterChanged,
			double R, double G, double B, double A, const FString& Status = TEXT("continued-to-dominating-writer"))
		{
			const TSharedRef<FJsonObject> History = MakeShared<FJsonObject>();
			History->SetStringField(TEXT("tracePurpose"), TEXT("color"));
			History->SetStringField(TEXT("resourceName"), Resource);
			History->SetNumberField(TEXT("resourceIndex"), ResourceIndex);
			History->SetStringField(TEXT("shaderBinding"), Binding);
			History->SetNumberField(TEXT("selectedWriterEventId"), Writer);
			History->SetNumberField(TEXT("sample"), 0);
			History->SetStringField(TEXT("branchStatus"), Status);
			History->SetStringField(TEXT("mappingConfidence"), TEXT("confirmed-executed-values"));
			History->SetBoolField(TEXT("executedShaderAccess"), true);
			History->SetObjectField(TEXT("shaderAccessResult"), MakePixelValue(R, G, B, A));
			if (Writer > 0)
			{
				const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
				Event->SetNumberField(TEXT("eventId"), Writer);
				Event->SetStringField(TEXT("actionKind"), Writer == 10203 ? TEXT("dispatch") : TEXT("draw"));
				Event->SetBoolField(TEXT("changedTextureValue"), bWriterChanged);
				Event->SetNumberField(TEXT("passedFragments"), 1);
				Event->SetObjectField(TEXT("firstBefore"), MakePixelValue(0, 0, 0, 0));
				// Compute shaderOut may be zero even though the UAV/resource postMod is non-zero.
				Event->SetObjectField(TEXT("lastShaderOutput"), MakePixelValue(0, 0, 0, 0));
				Event->SetObjectField(TEXT("lastAfter"), MakePixelValue(R, G, B, A));
				TArray<TSharedPtr<FJsonValue>> Summaries;
				Summaries.Add(MakeShared<FJsonValueObject>(Event));
				History->SetArrayField(TEXT("eventSummaries"), MoveTemp(Summaries));
			}
			return MakeShared<FJsonValueObject>(History);
		};
		auto AddInput = [](FEventContextEvidence& Context, int32 ResourceIndex,
			const FString& Name, const FString& Binding, const FString& Access)
		{
			FBoundResourceEvidence Input;
			Input.ResourceIndex = ResourceIndex;
			Input.Name = Name;
			Input.ShaderBinding = Binding;
			Input.Access = Access;
			Input.bTexture = true;
			Context.Inputs.Add(MoveTemp(Input));
		};

		TMap<uint32, FEventContextEvidence> PathContexts;
		TMap<uint32, int32> PathDepths;
		FEventContextEvidence Composite;
		Composite.EventId = 11083;
		AddInput(Composite, 2049, TEXT("SelectionOutlineColor"), TEXT("ColorTexture"), TEXT("read"));
		Composite.ResourcePixelHistories.Add(MakeValueFlowHistory(TEXT("SelectionOutlineColor"), 2049,
			TEXT("ColorTexture"), 10891, false, 0.436, 0.882, 0.419, 0));
		PathContexts.Add(Composite.EventId, Composite);
		PathDepths.Add(Composite.EventId, 0);

		FEventContextEvidence Outline;
		Outline.EventId = 10891;
		AddInput(Outline, 2032, TEXT("Tonemap"), TEXT("ColorTexture"), TEXT("read"));
		Outline.ResourcePixelHistories.Add(MakeValueFlowHistory(TEXT("Tonemap"), 2032,
			TEXT("ColorTexture"), 10823, true, 0.436, 0.882, 0.419, 0));
		PathContexts.Add(Outline.EventId, Outline);
		PathDepths.Add(Outline.EventId, 1);

		FEventContextEvidence Tonemap;
		Tonemap.EventId = 10823;
		AddInput(Tonemap, 2297, TEXT("TSR.Output"), TEXT("ColorTexture"), TEXT("read"));
		Tonemap.ResourcePixelHistories.Add(MakeValueFlowHistory(TEXT("TSR.Output"), 2297,
			TEXT("ColorTexture"), 10203, true, 0.133, 0.977, 0.123, 1));
		PathContexts.Add(Tonemap.EventId, Tonemap);
		PathDepths.Add(Tonemap.EventId, 2);

		FEventContextEvidence ResolveHistory;
		ResolveHistory.EventId = 10203;
		AddInput(ResolveHistory, 1530, TEXT("TSR.History.Color"), TEXT("UpdateHistoryOutputTexture"), TEXT("read"));
		AddInput(ResolveHistory, 2297, TEXT("TSR.Output"), TEXT("SceneColorOutputMip0"), TEXT("read-write"));
		TSharedPtr<FJsonValue> HistoryBoundaryValue = MakeValueFlowHistory(TEXT("TSR.History.Color"), 1530,
			TEXT("UpdateHistoryOutputTexture"), 0, false, 0, 0, 0, 0, TEXT("focused-fallback-pruned"));
		HistoryBoundaryValue->AsObject()->RemoveField(TEXT("shaderAccessResult"));
		HistoryBoundaryValue->AsObject()->SetBoolField(TEXT("executedShaderAccess"), false);
		ResolveHistory.ResourcePixelHistories.Add(HistoryBoundaryValue);
		TSharedPtr<FJsonValue> OutputFeedbackValue = MakeValueFlowHistory(TEXT("TSR.Output"), 2297,
			TEXT("SceneColorOutputMip0"), 0, false, 0, 0, 0, 0, TEXT("no-modification-before-consumer"));
		OutputFeedbackValue->AsObject()->RemoveField(TEXT("shaderAccessResult"));
		OutputFeedbackValue->AsObject()->SetBoolField(TEXT("executedShaderAccess"), false);
		ResolveHistory.ResourcePixelHistories.Add(OutputFeedbackValue);
		PathContexts.Add(ResolveHistory.EventId, ResolveHistory);
		PathDepths.Add(ResolveHistory.EventId, 3);

		const TArray<FCausalLaneEvidence> PathLanes = BuildCausalLaneEvidence(PathContexts, PathDepths);
		const FPrimaryCausalPathEvidence PrimaryPath = BuildPrimaryColorPathEvidence(
			PathLanes, PathContexts, 11083);
		TestEqual(TEXT("Primary path reaches the temporal-history boundary"), PrimaryPath.Branches.Num(), 4);
		if (PrimaryPath.Branches.Num() == 4)
		{
			TestEqual(TEXT("Primary path follows the outline pass-through"), PrimaryPath.Branches[0].ProducerEventId, 10891u);
			TestEqual(TEXT("Primary path follows Tonemap"), PrimaryPath.Branches[1].ProducerEventId, 10823u);
			TestEqual(TEXT("Primary path follows TSR ResolveHistory"), PrimaryPath.Branches[2].ProducerEventId, 10203u);
			TestEqual(TEXT("Read-write TSR output is not mistaken for a compute input"),
				PrimaryPath.Branches[3].ResourceIndex, 1530);
			TestEqual(TEXT("Temporal input is an explicit external-history boundary"),
				PrimaryPath.Branches[3].EdgeRole, FString(TEXT("external-history-boundary")));
			TestTrue(TEXT("Compute producer uses postMod/lastAfter for value matching"),
				PrimaryPath.Branches[2].bProducerValueMatchesExecutedSample);
			TestTrue(TEXT("Compute producer written value remains non-zero"),
				PrimaryPath.Branches[2].ProducerWrittenValue.Contains(TEXT("0.133")));
		}

		FEventContextEvidence SameNameResources;
		SameNameResources.EventId = 200;
		AddInput(SameNameResources, 2068, TEXT("Composite.PrimitivesDepthHistory"), TEXT("PrevHistoryTexture"), TEXT("read"));
		AddInput(SameNameResources, 2298, TEXT("Composite.PrimitivesDepthHistory"), TEXT("CurrentHistoryTexture"), TEXT("read-write"));
		SameNameResources.ResourcePixelHistories.Add(MakeValueFlowHistory(TEXT("Composite.PrimitivesDepthHistory"), 2068,
			TEXT("PrevHistoryTexture"), 0, false, 0, 0, 0, 0, TEXT("no-modification-before-consumer")));
		SameNameResources.ResourcePixelHistories.Add(MakeValueFlowHistory(TEXT("Composite.PrimitivesDepthHistory"), 2298,
			TEXT("CurrentHistoryTexture"), 0, false, 0, 0, 0, 0, TEXT("no-modification-before-consumer")));
		TMap<uint32, FEventContextEvidence> IdentityContexts;
		IdentityContexts.Add(SameNameResources.EventId, SameNameResources);
		TMap<uint32, int32> IdentityDepths;
		IdentityDepths.Add(SameNameResources.EventId, 0);
		const TArray<FCausalLaneEvidence> IdentityLanes = BuildCausalLaneEvidence(IdentityContexts, IdentityDepths);
		const FCausalLaneEvidence* IdentityColorLane = IdentityLanes.FindByPredicate([](const FCausalLaneEvidence& Lane)
		{
			return Lane.TracePurpose == TEXT("color");
		});
		TestNotNull(TEXT("Same-name resource lane exists"), IdentityColorLane);
		if (IdentityColorLane)
		{
			TestEqual(TEXT("Same resource names with different IDs remain separate"), IdentityColorLane->Branches.Num(), 2);
		}
		return true;
	}
}

#endif
