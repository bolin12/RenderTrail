#include "RenderTrailModelBrokerSettingsCustomization.h"

#include "RenderTrailModelBrokerSettings.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "PropertyHandle.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchableComboBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "RenderTrailModelBrokerSettingsCustomization"

namespace UE::RenderTrail::Private
{
	static FString GetDiscoveryModelsUrl(const URenderTrailOwnedModelSettings* Settings)
	{
		const FString EnvironmentEndpoint = FPlatformMisc::GetEnvironmentVariable(TEXT("RENDERTRAIL_MODEL_ENDPOINT")).TrimStartAndEnd();
		return EnvironmentEndpoint.IsEmpty()
			? Settings->GetModelsUrl()
			: URenderTrailOwnedModelSettings::MakeModelsUrl(EnvironmentEndpoint);
	}

	static FString GetDiscoveryApiKey(const URenderTrailOwnedModelSettings* Settings)
	{
		const FString EnvironmentApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("RENDERTRAIL_MODEL_API_KEY")).TrimStartAndEnd();
		return EnvironmentApiKey.IsEmpty() ? Settings->ApiKey.TrimStartAndEnd() : EnvironmentApiKey;
	}
}

TSharedRef<IDetailCustomization> FRenderTrailModelBrokerSettingsCustomization::MakeInstance()
{
	return MakeShareable(new FRenderTrailModelBrokerSettingsCustomization());
}

void FRenderTrailModelBrokerSettingsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	ProviderHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(URenderTrailOwnedModelSettings, Provider));
	BaseUrlHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(URenderTrailOwnedModelSettings, BaseUrlOverride));
	ModelHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(URenderTrailOwnedModelSettings, Model));
	ProviderHandle->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FRenderTrailModelBrokerSettingsCustomization::ResetDiscoveredModels));
	BaseUrlHandle->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FRenderTrailModelBrokerSettingsCustomization::ResetDiscoveredModels));

	StatusText = LOCTEXT("InitialStatus", "按供应商要求填写 API Key，然后获取这把 Key 实际可用的模型。");
	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(TEXT("Model Broker"));

	Category.AddCustomRow(LOCTEXT("ResolvedBaseUrlSearch", "Resolved Base URL"))
	.NameContent()
	[
		SNew(STextBlock)
		.Font(DetailBuilder.GetDetailFont())
		.Text(LOCTEXT("ResolvedBaseUrl", "实际 Base URL"))
		.ToolTipText(LOCTEXT("ResolvedBaseUrlTooltip", "The effective provider base URL. Chat Completions and Models paths are appended automatically."))
	]
	.ValueContent()
	.MinDesiredWidth(480.0f)
	[
		SNew(STextBlock)
		.Text(this, &FRenderTrailModelBrokerSettingsCustomization::GetResolvedBaseUrlText)
		.ToolTipText(this, &FRenderTrailModelBrokerSettingsCustomization::GetResolvedBaseUrlText)
	];

	Category.AddCustomRow(LOCTEXT("AvailableModelsSearch", "Available Models Fetch Select Flash Pro"))
	.NameContent()
	[
		SNew(STextBlock)
		.Font(DetailBuilder.GetDetailFont())
		.Text(LOCTEXT("AvailableModels", "可用模型"))
		.ToolTipText(LOCTEXT("AvailableModelsTooltip", "Fetch the provider's live model list, then search and select a model. The selection updates Model ID."))
	]
	.ValueContent()
	.MinDesiredWidth(480.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SButton)
			.Text(this, &FRenderTrailModelBrokerSettingsCustomization::GetFetchButtonText)
			.IsEnabled(this, &FRenderTrailModelBrokerSettingsCustomization::CanFetchModels)
			.ToolTipText(LOCTEXT("FetchModelsTooltip", "GET the OpenAI-compatible /models endpoint using the configured API Key."))
			.OnClicked(this, &FRenderTrailModelBrokerSettingsCustomization::FetchModels)
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SAssignNew(ModelComboBox, SSearchableComboBox)
			.OptionsSource(&ModelOptions)
			.SearchVisibility(EVisibility::Visible)
			.OnGenerateWidget(this, &FRenderTrailModelBrokerSettingsCustomization::MakeModelOptionWidget)
			.OnSelectionChanged(this, &FRenderTrailModelBrokerSettingsCustomization::HandleModelSelected)
			[
				SNew(STextBlock)
				.Text(this, &FRenderTrailModelBrokerSettingsCustomization::GetSelectedModelText)
			]
		]
	];

	Category.AddCustomRow(LOCTEXT("ModelDiscoveryStatusSearch", "Model Discovery Status"))
	.WholeRowContent()
	[
		SNew(STextBlock)
		.Text(this, &FRenderTrailModelBrokerSettingsCustomization::GetStatusText)
		.AutoWrapText(true)
	];
}

FReply FRenderTrailModelBrokerSettingsCustomization::FetchModels()
{
	const URenderTrailOwnedModelSettings* Settings = GetDefault<URenderTrailOwnedModelSettings>();
	const FString ModelsUrl = UE::RenderTrail::Private::GetDiscoveryModelsUrl(Settings);
	if (ModelsUrl.IsEmpty())
	{
		StatusText = LOCTEXT("MissingBaseUrl", "请先选择预设供应商，或填写 Base URL 覆盖。");
		return FReply::Handled();
	}

	bFetchingModels = true;
	StatusText = FText::Format(LOCTEXT("FetchingModels", "正在从 {0} 获取模型……"), FText::FromString(ModelsUrl));
	ModelOptions.Reset();
	if (ModelComboBox.IsValid())
	{
		ModelComboBox->ClearSelection();
		ModelComboBox->RefreshOptions();
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(ModelsUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	const FString ApiKey = UE::RenderTrail::Private::GetDiscoveryApiKey(Settings);
	if (!ApiKey.IsEmpty())
	{
		Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + ApiKey);
	}
	Request->SetTimeout(30.0f);

	const TWeakPtr<FRenderTrailModelBrokerSettingsCustomization> WeakThis =
		StaticCastSharedRef<FRenderTrailModelBrokerSettingsCustomization>(AsShared());
	Request->OnProcessRequestComplete().BindLambda(
		[WeakThis](FHttpRequestPtr, FHttpResponsePtr Response, const bool bSucceeded)
		{
			if (const TSharedPtr<FRenderTrailModelBrokerSettingsCustomization> Pinned = WeakThis.Pin())
			{
				Pinned->HandleModelsResponse(
					bSucceeded && Response.IsValid(),
					Response.IsValid() ? Response->GetResponseCode() : 0,
					Response.IsValid() ? Response->GetContentAsString() : FString());
			}
		});

	if (!Request->ProcessRequest())
	{
		bFetchingModels = false;
		StatusText = LOCTEXT("FetchStartFailed", "无法启动模型列表请求。");
	}
	return FReply::Handled();
}

void FRenderTrailModelBrokerSettingsCustomization::HandleModelsResponse(
	const bool bSucceeded,
	const int32 ResponseCode,
	FString ResponseBody)
{
	bFetchingModels = false;
	if (!bSucceeded)
	{
		StatusText = LOCTEXT("FetchFailed", "模型列表请求失败或超时。");
		return;
	}
	if (ResponseCode < 200 || ResponseCode >= 300)
	{
		ResponseBody.ReplaceInline(TEXT("\r"), TEXT(" "));
		ResponseBody.ReplaceInline(TEXT("\n"), TEXT(" "));
		ResponseBody.LeftInline(240);
		StatusText = FText::Format(
			LOCTEXT("FetchHttpError", "Model discovery returned HTTP {0}: {1}"),
			FText::AsNumber(ResponseCode), FText::FromString(ResponseBody));
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	const TArray<TSharedPtr<FJsonValue>>* Data = nullptr;
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()
		|| !Root->TryGetArrayField(TEXT("data"), Data) || !Data)
	{
		StatusText = LOCTEXT("InvalidModelsJson", "供应商没有返回 OpenAI-compatible Models JSON（data[].id）。");
		return;
	}

	TSet<FString> UniqueModels;
	for (const TSharedPtr<FJsonValue>& Value : *Data)
	{
		const TSharedPtr<FJsonObject> ModelObject = Value.IsValid() ? Value->AsObject() : nullptr;
		FString ModelId;
		if (ModelObject.IsValid() && ModelObject->TryGetStringField(TEXT("id"), ModelId))
		{
			ModelId = ModelId.TrimStartAndEnd();
			if (!ModelId.IsEmpty())
			{
				UniqueModels.Add(MoveTemp(ModelId));
			}
		}
	}

	TArray<FString> SortedModels = UniqueModels.Array();
	SortedModels.Sort([](const FString& Left, const FString& Right)
	{
		return Left.Compare(Right, ESearchCase::IgnoreCase) < 0;
	});
	for (FString& ModelId : SortedModels)
	{
		ModelOptions.Add(MakeShared<FString>(MoveTemp(ModelId)));
	}

	if (ModelComboBox.IsValid())
	{
		ModelComboBox->RefreshOptions();
		FString CurrentModel;
		ModelHandle->GetValue(CurrentModel);
		const TSharedPtr<FString>* CurrentOption = ModelOptions.FindByPredicate(
			[&CurrentModel](const TSharedPtr<FString>& Option)
			{
				return Option.IsValid() && Option->Equals(CurrentModel, ESearchCase::CaseSensitive);
			});
		if (CurrentOption)
		{
			ModelComboBox->SetSelectedItem(*CurrentOption, ESelectInfo::Direct);
		}
	}

	StatusText = ModelOptions.IsEmpty()
		? LOCTEXT("NoModels", "请求成功，但供应商没有返回模型 ID。")
		: FText::Format(LOCTEXT("ModelsFound", "找到 {0} 个模型。可以搜索 flash、pro 或任意模型 ID 后选择。"), FText::AsNumber(ModelOptions.Num()));
}

void FRenderTrailModelBrokerSettingsCustomization::HandleModelSelected(
	TSharedPtr<FString> Selection,
	ESelectInfo::Type)
{
	if (!Selection.IsValid() || !ModelHandle.IsValid())
	{
		return;
	}
	ModelHandle->SetValue(*Selection);
	GetMutableDefault<URenderTrailOwnedModelSettings>()->SaveConfig();
	StatusText = FText::Format(LOCTEXT("ModelSelected", "已选择模型：{0}"), FText::FromString(*Selection));
}

void FRenderTrailModelBrokerSettingsCustomization::ResetDiscoveredModels()
{
	ModelOptions.Reset();
	if (ModelComboBox.IsValid())
	{
		ModelComboBox->ClearSelection();
		ModelComboBox->RefreshOptions();
	}
	StatusText = LOCTEXT("ProviderChanged", "供应商或 Base URL 已改变，请重新获取模型列表。");
}

TSharedRef<SWidget> FRenderTrailModelBrokerSettingsCustomization::MakeModelOptionWidget(TSharedPtr<FString> Option) const
{
	return SNew(STextBlock).Text(FText::FromString(Option.IsValid() ? *Option : FString()));
}

FText FRenderTrailModelBrokerSettingsCustomization::GetSelectedModelText() const
{
	FString CurrentModel;
	if (ModelHandle.IsValid() && ModelHandle->GetValue(CurrentModel) == FPropertyAccess::Success
		&& !CurrentModel.TrimStartAndEnd().IsEmpty())
	{
		return FText::FromString(CurrentModel);
	}
	return ModelOptions.IsEmpty()
		? LOCTEXT("FetchModelsFirst", "请先获取模型")
		: LOCTEXT("SelectModel", "选择模型……");
}

FText FRenderTrailModelBrokerSettingsCustomization::GetResolvedBaseUrlText() const
{
	const FString BaseUrl = GetDefault<URenderTrailOwnedModelSettings>()->GetResolvedBaseUrl();
	return BaseUrl.IsEmpty() ? LOCTEXT("NoResolvedBaseUrl", "尚未配置") : FText::FromString(BaseUrl);
}

FText FRenderTrailModelBrokerSettingsCustomization::GetFetchButtonText() const
{
	return bFetchingModels ? LOCTEXT("FetchingButton", "获取中……") : LOCTEXT("FetchButton", "获取模型");
}

FText FRenderTrailModelBrokerSettingsCustomization::GetStatusText() const
{
	return StatusText;
}

bool FRenderTrailModelBrokerSettingsCustomization::CanFetchModels() const
{
	return !bFetchingModels
		&& !UE::RenderTrail::Private::GetDiscoveryModelsUrl(GetDefault<URenderTrailOwnedModelSettings>()).IsEmpty();
}

#undef LOCTEXT_NAMESPACE
