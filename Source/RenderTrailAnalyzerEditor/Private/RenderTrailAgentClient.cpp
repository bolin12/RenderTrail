#include "RenderTrailAgentClient.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::RenderTrail::Private
{
	namespace
	{
		FString SerializeJson(const TSharedRef<FJsonObject>& Object)
		{
			FString Result;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
			FJsonSerializer::Serialize(Object, Writer);
			return Result;
		}
	}

	FRenderTrailAgentClient::~FRenderTrailAgentClient()
	{
		Cancel();
	}

	void FRenderTrailAgentClient::Submit(const FRenderTrailAgentRequest& Request, FCompletion&& Completion)
	{
		Cancel();
		const uint64 Generation = ++RequestGeneration;
		ActiveCompletion = MoveTemp(Completion);

		const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("model"), Request.Model);
		Body->SetArrayField(TEXT("messages"), Request.Messages);
		Body->SetNumberField(TEXT("max_tokens"), Request.MaxOutputTokens);
		if (Request.bIncludeThinking)
		{
			const TSharedRef<FJsonObject> Thinking = MakeShared<FJsonObject>();
			Thinking->SetStringField(TEXT("type"), Request.bEnableThinking ? TEXT("enabled") : TEXT("disabled"));
			Body->SetObjectField(TEXT("thinking"), Thinking);
		}

		ActiveRequest = FHttpModule::Get().CreateRequest();
		ActiveRequest->SetURL(Request.Endpoint);
		ActiveRequest->SetVerb(TEXT("POST"));
		ActiveRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		ActiveRequest->SetHeader(TEXT("Accept"), TEXT("application/json"));
		if (!Request.ApiKey.IsEmpty())
		{
			ActiveRequest->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Request.ApiKey);
		}
		ActiveRequest->SetContentAsString(SerializeJson(Body));
		ActiveRequest->SetTimeout(120.0f);
		ActiveRequest->SetActivityTimeout(120.0f);

		const TWeakPtr<FRenderTrailAgentClient> WeakThis = AsShared();
		ActiveRequest->OnProcessRequestComplete().BindLambda(
			[WeakThis, Generation](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
			{
				if (const TSharedPtr<FRenderTrailAgentClient> Pinned = WeakThis.Pin())
				{
					Pinned->HandleComplete(HttpRequest, HttpResponse, bSucceeded, Generation);
				}
			});

		const FHttpRequestPtr RequestToStart = ActiveRequest;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[WeakThis, RequestToStart, Generation](float)
			{
				const TSharedPtr<FRenderTrailAgentClient> Pinned = WeakThis.Pin();
				if (!Pinned.IsValid() || Pinned->RequestGeneration != Generation || Pinned->ActiveRequest != RequestToStart)
				{
					return false;
				}
				if (!RequestToStart->ProcessRequest())
				{
					Pinned->CompleteFailure(Generation, TEXT("RenderTrail Model Broker request could not be queued."));
				}
				return false;
			}));
	}

	void FRenderTrailAgentClient::Cancel()
	{
		++RequestGeneration;
		if (ActiveRequest.IsValid())
		{
			const FHttpRequestPtr RequestToCancel = ActiveRequest;
			RequestToCancel->OnProcessRequestComplete().Unbind();
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[RequestToCancel](float)
				{
					RequestToCancel->CancelRequest();
					return false;
				}));
		}
		ActiveRequest.Reset();
		ActiveCompletion = nullptr;
	}

	bool FRenderTrailAgentClient::IsRunning() const
	{
		return ActiveRequest.IsValid();
	}

	void FRenderTrailAgentClient::HandleComplete(FHttpRequestPtr Request, FHttpResponsePtr Response,
		bool bSucceeded, uint64 Generation)
	{
		if (RequestGeneration != Generation || Request != ActiveRequest)
		{
			return;
		}

		FCompletion Completion = MoveTemp(ActiveCompletion);
		ActiveRequest.Reset();
		FRenderTrailAgentResponse Result;
		if (!bSucceeded || !Response.IsValid())
		{
			Result.Error = TEXT("RenderTrail model request failed or timed out.");
			Completion(MoveTemp(Result));
			return;
		}

		Result.HttpStatus = Response->GetResponseCode();
		Result.RawBody = Response->GetContentAsString();
		if (Result.HttpStatus < 200 || Result.HttpStatus >= 300)
		{
			FString ErrorBody = Result.RawBody;
			ErrorBody.LeftInline(600);
			Result.Error = FString::Printf(TEXT("RenderTrail model endpoint returned HTTP %d: %s"),
				Result.HttpStatus, *ErrorBody);
			Completion(MoveTemp(Result));
			return;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Result.RawBody);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			Result.Error = TEXT("RenderTrail model endpoint returned invalid JSON.");
			Completion(MoveTemp(Result));
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
		if (!Root->TryGetArrayField(TEXT("choices"), Choices) || !Choices || Choices->IsEmpty())
		{
			Result.Error = TEXT("RenderTrail model endpoint returned no choices.");
			Completion(MoveTemp(Result));
			return;
		}

		const TSharedPtr<FJsonObject> Choice = (*Choices)[0].IsValid() ? (*Choices)[0]->AsObject() : nullptr;
		const TSharedPtr<FJsonObject>* Message = nullptr;
		if (!Choice.IsValid() || !Choice->TryGetObjectField(TEXT("message"), Message) || !Message || !Message->IsValid())
		{
			Result.Error = TEXT("RenderTrail model response is missing choices[0].message.");
			Completion(MoveTemp(Result));
			return;
		}

		ExtractAssistantContent(*Message, Result.Content);
		(*Message)->TryGetStringField(TEXT("reasoning_content"), Result.ReasoningContent);
		Choice->TryGetStringField(TEXT("finish_reason"), Result.FinishReason);
		if (Result.Content.TrimStartAndEnd().IsEmpty())
		{
			Result.Error = FString::Printf(
				TEXT("RenderTrail model returned empty assistant content (finish_reason=%s, reasoning_chars=%d)."),
				*Result.FinishReason, Result.ReasoningContent.Len());
		}
		Completion(MoveTemp(Result));
	}

	void FRenderTrailAgentClient::CompleteFailure(uint64 Generation, FString Error)
	{
		if (RequestGeneration != Generation)
		{
			return;
		}
		FCompletion Completion = MoveTemp(ActiveCompletion);
		ActiveRequest.Reset();
		FRenderTrailAgentResponse Result;
		Result.Error = MoveTemp(Error);
		Completion(MoveTemp(Result));
	}

	bool FRenderTrailAgentClient::ExtractAssistantContent(const TSharedPtr<FJsonObject>& Message, FString& OutContent)
	{
		OutContent.Empty();
		if (!Message.IsValid())
		{
			return false;
		}
		if (Message->TryGetStringField(TEXT("content"), OutContent))
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
		if (!Message->TryGetArrayField(TEXT("content"), Parts) || !Parts)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& PartValue : *Parts)
		{
			if (!PartValue.IsValid())
			{
				continue;
			}
			if (PartValue->Type == EJson::String)
			{
				OutContent += PartValue->AsString();
				continue;
			}
			const TSharedPtr<FJsonObject> Part = PartValue->AsObject();
			FString Text;
			if (Part.IsValid() && (Part->TryGetStringField(TEXT("text"), Text)
				|| Part->TryGetStringField(TEXT("content"), Text)))
			{
				OutContent += Text;
			}
		}
		return true;
	}
}
