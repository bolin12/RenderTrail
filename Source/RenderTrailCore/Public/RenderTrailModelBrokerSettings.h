#pragma once

#include "Engine/DeveloperSettings.h"

#include "RenderTrailModelBrokerSettings.generated.h"

/** Providers exposing an OpenAI-compatible Chat Completions endpoint. */
UENUM()
enum class ERenderTrailModelProvider : uint8
{
	OpenAI UMETA(DisplayName="OpenAI"),
	GoogleGemini UMETA(DisplayName="Google Gemini"),
	OpenRouter UMETA(DisplayName="OpenRouter"),
	DeepSeek UMETA(DisplayName="DeepSeek"),
	Groq UMETA(DisplayName="Groq"),
	CustomOpenAICompatible UMETA(DisplayName="Custom OpenAI-compatible"),
	LocalOpenAICompatible UMETA(DisplayName="Local OpenAI-compatible")
};

/** RenderTrail-owned model connection settings. No MCP server is required. */
UCLASS(config=EditorPerProjectUserSettings, meta=(DisplayName="RenderTrail Model Broker"))
class RENDERTRAILCORE_API URenderTrailOwnedModelSettings final : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(DisplayName="Provider"))
	ERenderTrailModelProvider Provider = ERenderTrailModelProvider::CustomOpenAICompatible;

	/** Base URL or full /chat/completions URL. */
	UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(DisplayName="Base URL"))
	FString BaseUrlOverride;

	/** Provider model identifier. */
	UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(DisplayName="Model"))
	FString Model;

	/** Optional bearer token. Stored in per-project editor settings. */
	UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(DisplayName="API Key", PasswordField=true))
	FString ApiKey;

	/** Maximum output tokens sent by RenderTrail's bounded Agent loop. */
	UPROPERTY(Config, EditAnywhere, Category="Limits", meta=(ClampMin="128", ClampMax="8192", UIMin="128", UIMax="8192", DisplayName="Max Output Tokens"))
	int32 MaxOutputTokens = 8192;

	/** DeepSeek V4 only. The Agent remains stable with this disabled by default. */
	UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(DisplayName="Enable Thinking"))
	bool bEnableThinking = false;

	static FString GetDefaultBaseUrl(ERenderTrailModelProvider InProvider);
	static FString NormalizeBaseUrl(const FString& InBaseOrEndpoint);
	static FString MakeChatCompletionsUrl(const FString& InBaseOrEndpoint);
	static FString MakeModelsUrl(const FString& InBaseOrEndpoint);

	FString GetResolvedBaseUrl() const;
	FString GetChatCompletionsUrl() const;
	FString GetModelsUrl() const;
	FString GetProviderDisplayName() const;

	virtual void PostInitProperties() override;

	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("RenderTrail Model Broker"); }
};
