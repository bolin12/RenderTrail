#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UE::RenderTrail::Private::AgentProtocol
{
	bool TryParseActionJson(const FString& Content, FString& OutJson,
		TSharedPtr<FJsonObject>& OutAction, bool& bOutRepaired);
}
