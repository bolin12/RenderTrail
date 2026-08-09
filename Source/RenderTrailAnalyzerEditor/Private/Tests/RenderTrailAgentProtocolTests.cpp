#include "RenderTrailAgentProtocol.h"
#include "RenderTrailAnalyzerEvidence.h"
#include "RenderTrailProtocol.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace UE::RenderTrail::Private
{
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderTrailAgentProtocolTest,
		"RenderTrail.Analyzer.Agent.Protocol",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FRenderTrailAgentProtocolTest::RunTest(const FString& Parameters)
	{
		FString Json;
		TSharedPtr<FJsonObject> Action;
		bool bRepaired = false;

		const bool bParsedValid = AgentProtocol::TryParseActionJson(
			TEXT("result: {\"action\":\"finish\",\"confidence\":\"high\"}"), Json, Action, bRepaired);
		TestTrue(TEXT("Extracts a single action object from compatible wrapper text"), bParsedValid);
		TestFalse(TEXT("Valid JSON is not marked repaired"), bRepaired);
		if (Action.IsValid())
		{
			TestEqual(TEXT("Parsed action"), Action->GetStringField(TEXT("action")), FString(TEXT("finish")));
		}

		Action.Reset();
		const bool bParsedExtraCloser = AgentProtocol::TryParseActionJson(
			TEXT("{\"action\":\"finish\",\"influence\":{\"eventId\":7}] ,\"confidence\":\"low\"}"),
			Json, Action, bRepaired);
		TestTrue(TEXT("Drops an extra array closer without prematurely closing the root object"), bParsedExtraCloser);
		TestTrue(TEXT("Extra closer JSON is marked repaired"), bRepaired);
		if (Action.IsValid())
		{
			TestEqual(TEXT("Fields after the repaired closer remain in the root object"),
				Action->GetStringField(TEXT("confidence")), FString(TEXT("low")));
		}

		Action.Reset();
		const bool bParsedRepaired = AgentProtocol::TryParseActionJson(
			TEXT("{\"action\":\"finish\",\"unknowns\":[{\"name\":\"gap\"]}"), Json, Action, bRepaired);
		TestTrue(TEXT("Repairs a bounded mismatched closer before strict parsing"), bParsedRepaired);
		TestTrue(TEXT("Malformed compatible JSON is marked repaired"), bRepaired);
		if (Action.IsValid())
		{
			TestEqual(TEXT("Repaired action"), Action->GetStringField(TEXT("action")), FString(TEXT("finish")));
		}

		Action.Reset();
		TestFalse(TEXT("Rejects text without a JSON action object"),
			AgentProtocol::TryParseActionJson(TEXT("no structured response"), Json, Action, bRepaired));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderTrailPreviewPathTest,
		"RenderTrail.Analyzer.Preview.AuthoritativePathSeparation",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FRenderTrailPreviewPathTest::RunTest(const FString& Parameters)
	{
		const FString Capture = TEXT("D:/Project/Saved/RenderDocCaptures/frame.rdc");
		const FString NativePreview = UE::RenderTrail::GetPreviewPathForCapture(Capture);
		const FString ReplayPreview = UE::RenderTrail::GetReplayPreviewPathForCapture(Capture);
		TestNotEqual(TEXT("Native preview and authoritative RenderDoc final RT use different files"),
			NativePreview, ReplayPreview);
		TestTrue(TEXT("Native preview keeps the legacy PNG name"), NativePreview.EndsWith(TEXT("frame.png")));
		TestTrue(TEXT("Authoritative preview has an explicit RenderDoc suffix"),
			ReplayPreview.EndsWith(TEXT("frame.renderdoc.png")));
		TestEqual(TEXT("Both previews stay in the same preview directory"),
			FPaths::GetPath(NativePreview), FPaths::GetPath(ReplayPreview));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderTrailAgentContextCoverageTest,
		"RenderTrail.Analyzer.Agent.ContextCausalCoverage",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FRenderTrailAgentContextCoverageTest::RunTest(const FString& Parameters)
	{
		auto MakeContext = [](uint32 EventId, const FString& Marker, const FString& Shader)
		{
			FEventContextEvidence Context;
			Context.EventId = EventId;
			Context.ActionKind = TEXT("draw");
			Context.MarkerPath = Marker;
			Context.ShaderEntry = Shader;
			return Context;
		};
		auto MakeHistory = [](const TArray<uint32>& WriterIds, uint32 SelectedWriterId,
			const TArray<uint32>& PassedChangedIds, const FString& BranchStatus)
		{
			TSharedRef<FJsonObject> History = MakeShared<FJsonObject>();
			History->SetStringField(TEXT("branchStatus"), BranchStatus);
			if (SelectedWriterId > 0)
			{
				History->SetNumberField(TEXT("selectedWriterEventId"), SelectedWriterId);
			}
			TArray<TSharedPtr<FJsonValue>> Writers;
			for (const uint32 WriterId : WriterIds)
			{
				Writers.Add(MakeShared<FJsonValueNumber>(WriterId));
			}
			History->SetArrayField(TEXT("confirmedWriterEventIds"), MoveTemp(Writers));
			TArray<TSharedPtr<FJsonValue>> EventSummaries;
			for (const uint32 EventId : PassedChangedIds)
			{
				TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
				Event->SetNumberField(TEXT("eventId"), EventId);
				Event->SetNumberField(TEXT("passedFragments"), 1);
				Event->SetNumberField(TEXT("rejectedFragments"), 0);
				Event->SetBoolField(TEXT("changedTextureValue"), true);
				EventSummaries.Add(MakeShared<FJsonValueObject>(Event));
			}
			History->SetArrayField(TEXT("eventSummaries"), MoveTemp(EventSummaries));
			return MakeShared<FJsonValueObject>(History);
		};

		TMap<uint32, FEventContextEvidence> Contexts;
		Contexts.Add(100, MakeContext(100, TEXT("PostProcessing > Composite"), TEXT("CompositePS")));
		Contexts.Add(99, MakeContext(99, TEXT("PostProcessing > RecentNoise"), TEXT("NoisePS")));
		Contexts.Add(98, MakeContext(98, TEXT("PostProcessing > RecentNoise2"), TEXT("NoisePS")));
		Contexts.Add(90, MakeContext(90, TEXT("PostProcessing > SceneColorResolve"), TEXT("ResolvePS")));
		Contexts.Add(40, MakeContext(40, TEXT("Unresolved resource branch"), TEXT("")));
		Contexts.Add(30, MakeContext(30, TEXT("Nanite > MicropolyRasterize > VisBuffer"), TEXT("MicropolyRasterize")));
		Contexts.Add(20, MakeContext(20,
			TEXT("BasePass > /Game/Meshes/SM_Cylinder.SM_Cylinder (1 instances)"), TEXT("BasePassPS")));
		Contexts.FindChecked(100).ResourcePixelHistories.Add(
			MakeHistory({90, 30}, 90, {90, 30}, TEXT("continued-to-dominating-writer")));
		Contexts.FindChecked(90).ResourcePixelHistories.Add(
			MakeHistory({20}, 20, {20}, TEXT("continued-to-dominating-writer")));
		Contexts.FindChecked(40).ResourcePixelHistories.Add(
			MakeHistory({}, 0, {}, TEXT("query-failed")));

		TMap<uint32, int32> Depths;
		Depths.Add(100, 0);
		Depths.Add(99, 1);
		Depths.Add(98, 1);
		Depths.Add(90, 1);
		Depths.Add(40, 3);
		Depths.Add(30, 3);
		Depths.Add(20, 5);
		TSet<uint32> CriticalIds = {100};

		const FAgentContextCoverageSelection Selection =
			SelectAgentContextsForCausalCoverage(Contexts, Depths, CriticalIds, 4);
		TestEqual(TEXT("Keeps the requested detail budget"), Selection.DetailedEventIds.Num(), 4);
		TestTrue(TEXT("Keeps the critical final writer"), Selection.DetailedEventIds.Contains(100));
		TestTrue(TEXT("Keeps a deep BasePass asset writer"), Selection.DetailedEventIds.Contains(20));
		TestTrue(TEXT("Keeps a Nanite visibility writer"), Selection.DetailedEventIds.Contains(30));
		TestTrue(TEXT("Keeps an explicit unresolved boundary"), Selection.DetailedEventIds.Contains(40));
		TestFalse(TEXT("Does not let recent shallow noise crowd out causal coverage"),
			Selection.DetailedEventIds.Contains(99));
		const FAgentContextCoverageEvidence* AssetCoverage = Selection.CoverageByEventId.Find(20);
		TestNotNull(TEXT("Indexes the deep asset writer"), AssetCoverage);
		if (AssetCoverage)
		{
			TestEqual(TEXT("Indexes causal distance through the intermediate resolve"),
				AssetCoverage->CausalDistance, 2);
			TestTrue(TEXT("Classifies the asset marker"), AssetCoverage->bAssetMarker);
			TestTrue(TEXT("Classifies the BasePass stage"), AssetCoverage->bSceneRaster);
		}
		const FAgentContextCoverageEvidence* VisibilityCoverage = Selection.CoverageByEventId.Find(30);
		TestNotNull(TEXT("Indexes the unselected visible writer"), VisibilityCoverage);
		if (VisibilityCoverage)
		{
			TestTrue(TEXT("Unselected visible writer remains classified as Nanite evidence"),
				VisibilityCoverage->bNanite);
			TestFalse(TEXT("Unselected visible writer is not promoted to the causal parent"),
				VisibilityCoverage->bReferencedPixelWriter);
		}
		return true;
	}
}

#endif
