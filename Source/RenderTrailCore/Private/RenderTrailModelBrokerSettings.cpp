#include "RenderTrailModelBrokerSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(RenderTrailModelBrokerSettings)

#include "Misc/ConfigCacheIni.h"

namespace UE::RenderTrail::Private
{
	static FString TrimUrl(FString Url)
	{
		Url = Url.TrimStartAndEnd();
		if (!Url.IsEmpty() && !Url.Contains(TEXT("://")))
		{
			Url = TEXT("http://") + Url;
		}
		while (Url.EndsWith(TEXT("/")))
		{
			Url.LeftChopInline(1);
		}
		return Url;
	}
}

FString URenderTrailOwnedModelSettings::GetDefaultBaseUrl(const ERenderTrailModelProvider InProvider)
{
	switch (InProvider)
	{
	case ERenderTrailModelProvider::OpenAI:
		return TEXT("https://api.openai.com/v1");
	case ERenderTrailModelProvider::GoogleGemini:
		return TEXT("https://generativelanguage.googleapis.com/v1beta/openai");
	case ERenderTrailModelProvider::OpenRouter:
		return TEXT("https://openrouter.ai/api/v1");
	case ERenderTrailModelProvider::DeepSeek:
		return TEXT("https://api.deepseek.com");
	case ERenderTrailModelProvider::Groq:
		return TEXT("https://api.groq.com/openai/v1");
	case ERenderTrailModelProvider::CustomOpenAICompatible:
	case ERenderTrailModelProvider::LocalOpenAICompatible:
	default:
		return FString();
	}
}

FString URenderTrailOwnedModelSettings::NormalizeBaseUrl(const FString& InBaseOrEndpoint)
{
	FString Url = UE::RenderTrail::Private::TrimUrl(InBaseOrEndpoint);
	const FString CompletionSuffix = TEXT("/chat/completions");
	if (Url.EndsWith(CompletionSuffix, ESearchCase::IgnoreCase))
	{
		Url.LeftChopInline(CompletionSuffix.Len());
	}
	return UE::RenderTrail::Private::TrimUrl(MoveTemp(Url));
}

FString URenderTrailOwnedModelSettings::MakeChatCompletionsUrl(const FString& InBaseOrEndpoint)
{
	const FString BaseUrl = NormalizeBaseUrl(InBaseOrEndpoint);
	return BaseUrl.IsEmpty() ? FString() : BaseUrl + TEXT("/chat/completions");
}

FString URenderTrailOwnedModelSettings::MakeModelsUrl(const FString& InBaseOrEndpoint)
{
	const FString BaseUrl = NormalizeBaseUrl(InBaseOrEndpoint);
	return BaseUrl.IsEmpty() ? FString() : BaseUrl + TEXT("/models");
}

FString URenderTrailOwnedModelSettings::GetResolvedBaseUrl() const
{
	return BaseUrlOverride.TrimStartAndEnd().IsEmpty()
		? NormalizeBaseUrl(GetDefaultBaseUrl(Provider))
		: NormalizeBaseUrl(BaseUrlOverride);
}

FString URenderTrailOwnedModelSettings::GetChatCompletionsUrl() const
{
	return MakeChatCompletionsUrl(GetResolvedBaseUrl());
}

FString URenderTrailOwnedModelSettings::GetModelsUrl() const
{
	return MakeModelsUrl(GetResolvedBaseUrl());
}

FString URenderTrailOwnedModelSettings::GetProviderDisplayName() const
{
	if (const UEnum* ProviderEnum = StaticEnum<ERenderTrailModelProvider>())
	{
		return ProviderEnum->GetDisplayNameTextByValue(static_cast<int64>(Provider)).ToString();
	}
	return TEXT("Unknown");
}

void URenderTrailOwnedModelSettings::PostInitProperties()
{
	Super::PostInitProperties();
	if (!GConfig)
	{
		return;
	}

	// Migrate settings written by the old CaptureEditor-owned prototype.
	static const TCHAR* LegacyCaptureSection = TEXT("/Script/RenderTrailCaptureEditor.RenderTrailModelBrokerSettings");
	static const TCHAR* LegacyBrokerSection = TEXT("/Script/RenderTrailModelBroker.RenderTrailOwnedModelSettings");
	const TCHAR* LegacySections[] = { LegacyCaptureSection, LegacyBrokerSection };

	for (const TCHAR* LegacySection : LegacySections)
	{
		FString LegacyValue;
		if (BaseUrlOverride.IsEmpty()
			&& GConfig->GetString(LegacySection, TEXT("BaseUrlOverride"), LegacyValue, GEditorPerProjectIni)
			&& !LegacyValue.Contains(TEXT("/mcp"), ESearchCase::IgnoreCase))
		{
			BaseUrlOverride = LegacyValue;
		}
		if (Model.IsEmpty() && GConfig->GetString(LegacySection, TEXT("Model"), LegacyValue, GEditorPerProjectIni))
		{
			Model = LegacyValue;
		}
		if (ApiKey.IsEmpty() && GConfig->GetString(LegacySection, TEXT("ApiKey"), LegacyValue, GEditorPerProjectIni))
		{
			ApiKey = LegacyValue;
		}
		int32 LegacyMaxTokens = 0;
		if (MaxOutputTokens == 8192
			&& GConfig->GetInt(LegacySection, TEXT("MaxOutputTokens"), LegacyMaxTokens, GEditorPerProjectIni))
		{
			MaxOutputTokens = LegacyMaxTokens;
		}
		FString LegacyProvider;
		if (GConfig->GetString(LegacySection, TEXT("Provider"), LegacyProvider, GEditorPerProjectIni))
		{
			if (const UEnum* ProviderEnum = StaticEnum<ERenderTrailModelProvider>())
			{
				const int64 Value = ProviderEnum->GetValueByNameString(LegacyProvider);
				if (Value != INDEX_NONE)
				{
					Provider = static_cast<ERenderTrailModelProvider>(Value);
				}
			}
		}
	}
}
