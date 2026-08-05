#include "IRenderTrailAnalyzerEditorModule.h"
#include "RenderTrailAnalyzerHome.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Docking/SDockTab.h"

namespace UE::RenderTrail::Private
{
	static const FName AnalyzerTabId(TEXT("RenderTrailAnalyzer"));

	class FRenderTrailAnalyzerEditorModule final : public IRenderTrailAnalyzerEditorModule
	{
	public:
		void StartupModule() override
		{
			FGlobalTabmanager::Get()->RegisterNomadTabSpawner(AnalyzerTabId,
				FOnSpawnTab::CreateRaw(this, &FRenderTrailAnalyzerEditorModule::SpawnAnalyzerTab))
				.SetDisplayName(FText::FromString(TEXT("RenderTrail Analyzer")))
				.SetTooltipText(FText::FromString(TEXT("Inspect RenderDoc captures with bounded pixel-local evidence.")));
		}

		void ShutdownModule() override
		{
			AnalyzerWidget.Reset();
			if (FSlateApplication::IsInitialized())
			{
				FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(AnalyzerTabId);
			}
		}

		void OpenCapture(const FString& CapturePath) override
		{
			PendingCapturePath = FPaths::ConvertRelativePathToFull(CapturePath);
			const TSharedPtr<SRenderTrailAnalyzerHome> ExistingWidget = AnalyzerWidget.Pin();
			FGlobalTabmanager::Get()->TryInvokeTab(AnalyzerTabId);
			if (ExistingWidget.IsValid())
			{
				ExistingWidget->OpenCapture(PendingCapturePath);
			}
		}

	private:
		TSharedRef<SDockTab> SpawnAnalyzerTab(const FSpawnTabArgs&)
		{
			TSharedRef<SRenderTrailAnalyzerHome> Widget =
				SNew(SRenderTrailAnalyzerHome).InitialCapture(PendingCapturePath);
			AnalyzerWidget = Widget;
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				[
					Widget
				];
		}

		FString PendingCapturePath;
		TWeakPtr<SRenderTrailAnalyzerHome> AnalyzerWidget;
	};
}

IMPLEMENT_MODULE(UE::RenderTrail::Private::FRenderTrailAnalyzerEditorModule, RenderTrailAnalyzerEditor)
