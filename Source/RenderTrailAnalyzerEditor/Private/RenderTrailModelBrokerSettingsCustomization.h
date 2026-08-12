#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IPropertyHandle;
class SSearchableComboBox;

/** Adds provider model discovery and selection to the Project Settings page. */
class FRenderTrailModelBrokerSettingsCustomization final : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	FReply FetchModels();
	void HandleModelsResponse(bool bSucceeded, int32 ResponseCode, FString ResponseBody);
	void HandleModelSelected(TSharedPtr<FString> Selection, ESelectInfo::Type SelectInfo);
	void ResetDiscoveredModels();

	TSharedRef<SWidget> MakeModelOptionWidget(TSharedPtr<FString> Option) const;
	FText GetSelectedModelText() const;
	FText GetResolvedBaseUrlText() const;
	FText GetFetchButtonText() const;
	FText GetStatusText() const;
	bool CanFetchModels() const;

	TSharedPtr<IPropertyHandle> ProviderHandle;
	TSharedPtr<IPropertyHandle> BaseUrlHandle;
	TSharedPtr<IPropertyHandle> ModelHandle;
	TArray<TSharedPtr<FString>> ModelOptions;
	TSharedPtr<SSearchableComboBox> ModelComboBox;
	FText StatusText;
	bool bFetchingModels = false;
};
