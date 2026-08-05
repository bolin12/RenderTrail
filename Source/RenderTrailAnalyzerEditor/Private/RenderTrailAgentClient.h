#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IHttpRequest.h"
#include "Templates/SharedPointer.h"

namespace UE::RenderTrail::Private
{
	struct FRenderTrailAgentRequest
	{
		FString Endpoint;
		FString Model;
		FString ApiKey;
		TArray<TSharedPtr<FJsonValue>> Messages;
		int32 MaxOutputTokens = 8192;
		bool bIncludeThinking = false;
		bool bEnableThinking = false;
	};

	struct FRenderTrailAgentResponse
	{
		FString Content;
		FString ReasoningContent;
		FString FinishReason = TEXT("unknown");
		FString RawBody;
		FString Error;
		int32 HttpStatus = 0;

		bool IsSuccess() const { return Error.IsEmpty(); }
	};

	class FRenderTrailAgentClient final : public TSharedFromThis<FRenderTrailAgentClient>
	{
	public:
		using FCompletion = TFunction<void(FRenderTrailAgentResponse&&)>;

		~FRenderTrailAgentClient();

		void Submit(const FRenderTrailAgentRequest& Request, FCompletion&& Completion);
		void Cancel();
		bool IsRunning() const;

	private:
		void HandleComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded, uint64 Generation);
		void CompleteFailure(uint64 Generation, FString Error);
		static bool ExtractAssistantContent(const TSharedPtr<FJsonObject>& Message, FString& OutContent);

		FHttpRequestPtr ActiveRequest;
		FCompletion ActiveCompletion;
		uint64 RequestGeneration = 0;
	};
}
