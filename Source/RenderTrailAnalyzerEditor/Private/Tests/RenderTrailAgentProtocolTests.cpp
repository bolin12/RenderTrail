#include "RenderTrailAgentProtocol.h"

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
}

#endif
