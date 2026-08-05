#include "RenderTrailProtocol.h"
#include "IRenderTrailAnalyzerEditorModule.h"
#include "RenderTrailModelBrokerSettings.h"
#include "RenderTrailModelBrokerSettingsCustomization.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IRenderCaptureProvider.h"
#include "Interfaces/IMainFrameModule.h"
#include "Interfaces/IPluginManager.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "UnrealClient.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "ID3D11DynamicRHI.h"
#include "ID3D12DynamicRHI.h"
#include "renderdoc_app.h"
#endif

#define LOCTEXT_NAMESPACE "RenderTrailCaptureEditor"

DEFINE_LOG_CATEGORY_STATIC(LogRenderTrailCaptureEditor, Log, All);

namespace UE::RenderTrail::Private
{
	struct FRawCaptureFile
	{
		FString Path;
		int64 Size = -1;
		FDateTime ModificationTime = FDateTime::MinValue();
	};

	static void Notify(const FText& Text, SNotificationItem::ECompletionState State)
	{
		FNotificationInfo Info(Text);
		Info.ExpireDuration = 5.0f;
		TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(State);
		}
	}

	static FString GetRawCaptureDirectory()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RenderDocCaptures")));
	}

	static bool FindLatestRawCapture(FRawCaptureFile& OutCapture)
	{
		const FString Directory = GetRawCaptureDirectory();
		TArray<FString> FileNames;
		IFileManager::Get().FindFiles(FileNames, *FPaths::Combine(Directory, TEXT("*.rdc")), true, false);

		bool bFound = false;
		for (const FString& FileName : FileNames)
		{
			const FString Path = FPaths::Combine(Directory, FileName);
			const FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
			if (!Stat.bIsValid || Stat.bIsDirectory || Stat.FileSize <= 0)
			{
				continue;
			}
			if (!bFound || Stat.ModificationTime > OutCapture.ModificationTime)
			{
				OutCapture.Path = Path;
				OutCapture.Size = Stat.FileSize;
				OutCapture.ModificationTime = Stat.ModificationTime;
				bFound = true;
			}
		}
		return bFound;
	}

	static FViewport* ResolveCaptureViewport(FString& OutError)
	{
		FViewport* TargetViewport = nullptr;
		if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport
			&& GEngine->GameViewport->Viewport->HasFocus())
		{
			TargetViewport = GEngine->GameViewport->Viewport;
		}

#if WITH_EDITOR
		// Clicking the toolbar moves Slate focus away from the PIE window. When PIE is
		// active, keep the capture target on the GameViewport instead of falling back
		// to the Level Editor viewport merely because the toolbar owns focus.
		if (!TargetViewport && GEditor && GEditor->PlayWorld && GEngine && GEngine->GameViewport
			&& GEngine->GameViewport->Viewport)
		{
			TargetViewport = GEngine->GameViewport->Viewport;
		}

		if (!TargetViewport && GEditor)
		{
			TargetViewport = GEditor->GetActiveViewport();
		}
#endif

		if (!TargetViewport)
		{
			OutError = TEXT("No focused Game Viewport or active Editor Viewport was available for RenderDoc capture.");
		}
		return TargetViewport;
	}

	static bool CaptureCurrentViewport(FString& OutError, TArray<FColor>& OutPreviewPixels, FIntPoint& OutPreviewSize)
	{
		OutPreviewPixels.Reset();
		OutPreviewSize = FIntPoint::ZeroValue;
		FViewport* TargetViewport = ResolveCaptureViewport(OutError);
		if (!TargetViewport)
		{
			return false;
		}
		const FIntPoint TargetSize = TargetViewport->GetSizeXY();
		UE_LOG(LogRenderTrailCaptureEditor, Display,
			TEXT("Selected RenderDoc viewport target: size=%dx%d pie=%s"),
			TargetSize.X, TargetSize.Y,
			(GEditor && GEditor->PlayWorld) ? TEXT("yes") : TEXT("no"));

#if PLATFORM_WINDOWS
		const HMODULE RenderDocModule = ::GetModuleHandleW(L"renderdoc.dll");
		if (!RenderDocModule)
		{
			OutError = TEXT("renderdoc.dll is not loaded in the editor process.");
			return false;
		}

		const pRENDERDOC_GetAPI GetApi = reinterpret_cast<pRENDERDOC_GetAPI>(::GetProcAddress(RenderDocModule, "RENDERDOC_GetAPI"));
		if (!GetApi)
		{
			OutError = TEXT("RENDERDOC_GetAPI is unavailable in the loaded renderdoc.dll.");
			return false;
		}

		RENDERDOC_API_1_0_0* Api = nullptr;
		if (GetApi(eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void**>(&Api)) == 0 || !Api
			|| !Api->StartFrameCapture || !Api->EndFrameCapture)
		{
			OutError = TEXT("The loaded RenderDoc build did not return a compatible frame-capture API.");
			return false;
		}

		RENDERDOC_DevicePointer Device = nullptr;
		switch (RHIGetInterfaceType())
		{
		case ERHIInterfaceType::D3D12:
			Device = GetID3D12DynamicRHI()->RHIGetDevice_NoMGPU();
			break;
		case ERHIInterfaceType::D3D11:
			Device = GetID3D11DynamicRHI()->RHIGetDevice();
			break;
		default:
			break;
		}
		if (!Device)
		{
			OutError = TEXT("RenderTrail viewport capture currently requires a supported D3D11 or D3D12 RHI device.");
			return false;
		}

		// Match the lightweight capture options used by RenderTrail's old path. These
		// options affect capture size/cost, not whether the selected viewport's draw
		// events are recorded.
		Api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, 0);
		Api->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 0);
		Api->SetCaptureOptionU32(eRENDERDOC_Option_SaveAllInitials, 0);

		void* TargetWindow = TargetViewport->GetWindow();
		const bool bViewportHasNativeWindow = TargetWindow != nullptr;
		if (!TargetWindow && FModuleManager::Get().IsModuleLoaded(TEXT("MainFrame")))
		{
			const TSharedPtr<SWindow> MainFrameWindow =
				FModuleManager::LoadModuleChecked<IMainFrameModule>(TEXT("MainFrame")).GetParentWindow();
			if (MainFrameWindow.IsValid() && MainFrameWindow->GetNativeWindow().IsValid())
			{
				TargetWindow = MainFrameWindow->GetNativeWindow()->GetOSWindowHandle();
			}
		}
		if (!TargetWindow)
		{
			OutError = TEXT("The selected viewport is embedded, but the Unreal MainFrame native window is unavailable for RenderDoc capture.");
			return false;
		}
		UE_LOG(LogRenderTrailCaptureEditor, Display,
			TEXT("RenderDoc native capture target resolved from %s window."),
			bViewportHasNativeWindow ? TEXT("viewport") : TEXT("editor main-frame"));

		// Do not use GetActiveWindow() here: a Slate warning/notification window can
		// temporarily own focus. Use the viewport's native window when available, and
		// the Unreal editor main-frame window for embedded Level Editor viewports.
		ENQUEUE_RENDER_COMMAND(RenderTrailStartExplicitCapture)(
			[Api, Device, TargetWindow](FRHICommandListImmediate&)
			{
				Api->StartFrameCapture(Device, TargetWindow);
			});
		TargetViewport->Draw(true);
		ENQUEUE_RENDER_COMMAND(RenderTrailEndExplicitCapture)(
			[Api, Device, TargetWindow](FRHICommandListImmediate& RHICmdList)
			{
				RHICmdList.SubmitAndBlockUntilGPUIdle();
				Api->EndFrameCapture(Device, TargetWindow);
			});

		// Read the same viewport after its captured draw. ReadPixels flushes the queued
		// render work, so this produces a native-size, pixel-addressable preview without
		// creating a RenderDoc ReplayController. The preview is saved only after RenderDoc
		// has assigned the final .rdc path.
		if (TargetSize.X > 0 && TargetSize.Y > 0 && TargetViewport->ReadPixels(OutPreviewPixels)
			&& OutPreviewPixels.Num() == static_cast<int64>(TargetSize.X) * TargetSize.Y)
		{
			OutPreviewSize = TargetSize;
			for (FColor& Pixel : OutPreviewPixels)
			{
				Pixel.A = 255;
			}
			UE_LOG(LogRenderTrailCaptureEditor, Display,
				TEXT("Captured native viewport preview: %dx%d"), TargetSize.X, TargetSize.Y);
		}
		else
		{
			OutPreviewPixels.Reset();
			UE_LOG(LogRenderTrailCaptureEditor, Warning,
				TEXT("RenderDoc capture succeeded, but the native viewport preview readback failed."));
		}
		return true;
#else
		OutError = TEXT("Explicit viewport capture is currently implemented for Windows only.");
		return false;
#endif
	}
}

class FRenderTrailCaptureEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		PropertyEditor.RegisterCustomClassLayout(
			URenderTrailOwnedModelSettings::StaticClass()->GetFName(),
			FOnGetDetailCustomizationInstance::CreateStatic(&FRenderTrailModelBrokerSettingsCustomization::MakeInstance));
		PropertyEditor.NotifyCustomizationModuleChanged();

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FRenderTrailCaptureEditorModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		if (FModuleManager::Get().IsModuleLoaded(TEXT("PropertyEditor")))
		{
			FPropertyEditorModule& PropertyEditor = FModuleManager::GetModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
			PropertyEditor.UnregisterCustomClassLayout(URenderTrailOwnedModelSettings::StaticClass()->GetFName());
			PropertyEditor.NotifyCustomizationModuleChanged();
		}
		if (CapturePollHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(CapturePollHandle);
			CapturePollHandle.Reset();
		}
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

private:

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		UToolMenu* PlayToolbar = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar"));
		FToolMenuSection& PlaySection = PlayToolbar->FindOrAddSection(TEXT("Play"));
		PlaySection.AddEntry(FToolMenuEntry::InitToolBarButton(
			TEXT("RenderTrailCaptureAndInspectToolbar"),
			FUIAction(FExecuteAction::CreateRaw(this, &FRenderTrailCaptureEditorModule::CaptureAndAnalyze)),
			LOCTEXT("CaptureAndAnalyzeToolbarLabel", "RenderTrail"),
			LOCTEXT("CaptureAndAnalyzeToolbarTooltip", "Recover the latest completed RenderDoc capture, or capture the focused Game/Editor viewport and inspect it in RenderTrail."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("LevelViewport.HighResScreenshot"))));
	}

	void CaptureAndAnalyze()
	{
		if (bCapturePending)
		{
			UE::RenderTrail::Private::Notify(LOCTEXT("CaptureAlreadyPending", "A RenderTrail capture is already pending."), SNotificationItem::CS_Pending);
			return;
		}

		// A previous editor run may have crashed after RenderDoc finished the file but before the
		// analyzer launched. Claiming that file first makes the capture useful without repeating it.
		UE::RenderTrail::Private::FRawCaptureFile RecoverableCapture;
		if (UE::RenderTrail::Private::FindLatestRawCapture(RecoverableCapture)
			&& !FPaths::FileExists(UE::RenderTrail::GetMetadataPathForCapture(RecoverableCapture.Path)))
		{
			UE_LOG(LogRenderTrailCaptureEditor, Display,
				TEXT("Recovering completed unclaimed capture: %s (%lld bytes)"),
				*RecoverableCapture.Path, RecoverableCapture.Size);
			FinalizeCapture(RecoverableCapture.Path, MakeCaptureMetadata(RecoverableCapture.Path), true);
			return;
		}

		if (!IRenderCaptureProvider::IsAvailable())
		{
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoCaptureProvider", "No render capture provider is available. Enable RenderDocPlugin and start the editor with RenderDoc attached."));
			return;
		}

		BaselineCapture = UE::RenderTrail::Private::FRawCaptureFile();
		UE::RenderTrail::Private::FindLatestRawCapture(BaselineCapture);
		PendingMetadata = MakeCaptureMetadata(FString());
		StableCandidatePath.Empty();
		StableCandidateSize = -1;
		StableCandidatePolls = 0;

		FString TriggerError;
		PendingPreviewPixels.Reset();
		PendingPreviewSize = FIntPoint::ZeroValue;
		if (!UE::RenderTrail::Private::CaptureCurrentViewport(
			TriggerError, PendingPreviewPixels, PendingPreviewSize))
		{
			UE_LOG(LogRenderTrailCaptureEditor, Error, TEXT("Safe RenderDoc trigger failed: %s"), *TriggerError);
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TriggerError));
			return;
		}
		bCapturePending = true;
		UE_LOG(LogRenderTrailCaptureEditor, Display,
			TEXT("Requested active-viewport RenderDoc capture. Baseline=%s Size=%lld Time=%s"),
			*BaselineCapture.Path, BaselineCapture.Size, *BaselineCapture.ModificationTime.ToIso8601());
		UE::RenderTrail::Private::Notify(LOCTEXT("CaptureRequested", "RenderDoc is capturing the active viewport. Waiting for a stable .rdc file…"), SNotificationItem::CS_Pending);

		if (CapturePollHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(CapturePollHandle);
		}
		const double Deadline = FPlatformTime::Seconds() + 90.0;
		CapturePollHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this, Deadline](float)
			{
				UE::RenderTrail::Private::FRawCaptureFile Candidate;
				if (UE::RenderTrail::Private::FindLatestRawCapture(Candidate))
				{
					const bool bNewCapture = BaselineCapture.Path.IsEmpty()
						|| Candidate.Path != BaselineCapture.Path
						|| Candidate.ModificationTime > BaselineCapture.ModificationTime
						|| Candidate.Size != BaselineCapture.Size;
					if (bNewCapture)
					{
						if (StableCandidatePath == Candidate.Path && StableCandidateSize == Candidate.Size)
						{
							++StableCandidatePolls;
						}
						else
						{
							StableCandidatePath = Candidate.Path;
							StableCandidateSize = Candidate.Size;
							StableCandidatePolls = 1;
							UE_LOG(LogRenderTrailCaptureEditor, Display,
								TEXT("Observed new RenderDoc capture candidate: %s (%lld bytes)"),
								*Candidate.Path, Candidate.Size);
						}

						// Four unchanged 250 ms samples avoid opening a capture while RenderDoc is still writing it.
						if (StableCandidatePolls >= 4)
						{
							CapturePollHandle.Reset();
							bCapturePending = false;
							FinalizeCapture(Candidate.Path, PendingMetadata, false);
							return false;
						}
					}
				}
				if (FPlatformTime::Seconds() >= Deadline)
				{
					CapturePollHandle.Reset();
					bCapturePending = false;
					UE_LOG(LogRenderTrailCaptureEditor, Error, TEXT("Timed out waiting for a new stable RenderDoc capture in %s."), *UE::RenderTrail::Private::GetRawCaptureDirectory());
					UE::RenderTrail::Private::Notify(LOCTEXT("CaptureTimeout", "Timed out waiting for a new stable RenderDoc capture file."), SNotificationItem::CS_Fail);
					return false;
				}
				return true;
			}), 0.25f);
	}

	UE::RenderTrail::FCaptureMetadata MakeCaptureMetadata(const FString& CapturePath) const
	{
		UE::RenderTrail::FCaptureMetadata Metadata;
		Metadata.CapturePath = CapturePath;
		Metadata.ProjectName = FApp::GetProjectName();
		Metadata.ProjectDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		Metadata.EngineVersion = FEngineVersion::Current().ToString();
		Metadata.UtcTimestamp = FDateTime::UtcNow().ToIso8601();
		Metadata.FrameCounter = GFrameCounter;
		Metadata.bIsPIE = GEditor && GEditor->PlayWorld != nullptr;
		if (GEditor)
		{
			if (const UWorld* World = GEditor->GetEditorWorldContext().World())
			{
				Metadata.MapName = World->GetPathName();
			}
		}
		return Metadata;
	}

	void FinalizeCapture(const FString& CapturePath, UE::RenderTrail::FCaptureMetadata Metadata, bool bRecovered)
	{
		Metadata.CapturePath = CapturePath;
		if (!bRecovered && !PendingPreviewPixels.IsEmpty()
			&& PendingPreviewSize.X > 0 && PendingPreviewSize.Y > 0)
		{
			const FString PreviewPath = UE::RenderTrail::GetPreviewPathForCapture(CapturePath);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(PreviewPath), true);
			TArray64<uint8> CompressedPreview;
			FImageUtils::PNGCompressImageArray(
				PendingPreviewSize.X,
				PendingPreviewSize.Y,
				TArrayView64<const FColor>(PendingPreviewPixels.GetData(), PendingPreviewPixels.Num()),
				CompressedPreview);
			if (!CompressedPreview.IsEmpty() && FFileHelper::SaveArrayToFile(CompressedPreview, *PreviewPath))
			{
				Metadata.PreviewPath = PreviewPath;
				Metadata.PreviewWidth = PendingPreviewSize.X;
				Metadata.PreviewHeight = PendingPreviewSize.Y;
				Metadata.bPreviewPixelExact = true;
				UE_LOG(LogRenderTrailCaptureEditor, Display,
					TEXT("Saved native viewport preview for immediate pixel selection: %s (%dx%d)"),
					*PreviewPath, PendingPreviewSize.X, PendingPreviewSize.Y);
			}
			else
			{
				UE_LOG(LogRenderTrailCaptureEditor, Warning,
					TEXT("Could not save native viewport preview: %s"), *PreviewPath);
			}
		}
		PendingPreviewPixels.Reset();
		PendingPreviewSize = FIntPoint::ZeroValue;
		FString MetadataPath;
		FString MetadataError;
		if (!Metadata.SaveAdjacent(MetadataPath, MetadataError))
		{
			UE_LOG(LogRenderTrailCaptureEditor, Warning, TEXT("%s"), *MetadataError);
		}
		UE_LOG(LogRenderTrailCaptureEditor, Display,
			TEXT("%s RenderDoc capture ready for offline analysis: %s; metadata: %s"),
			bRecovered ? TEXT("Recovered") : TEXT("Completed"), *CapturePath, *MetadataPath);
		UE::RenderTrail::Private::Notify(
			bRecovered ? LOCTEXT("CaptureRecovered", "Recovered the latest completed .rdc; opening offline analysis.")
				: LOCTEXT("CaptureCompleted", "RenderDoc capture is stable; opening offline analysis."),
			SNotificationItem::CS_Success);
		LaunchAnalyzer(CapturePath);
	}

	void LaunchAnalyzer(FString CapturePath)
	{
		IRenderTrailAnalyzerEditorModule::Get().OpenCapture(CapturePath);
	}

	FTSTicker::FDelegateHandle CapturePollHandle;
	UE::RenderTrail::Private::FRawCaptureFile BaselineCapture;
	UE::RenderTrail::FCaptureMetadata PendingMetadata;
	FString StableCandidatePath;
	TArray<FColor> PendingPreviewPixels;
	FIntPoint PendingPreviewSize = FIntPoint::ZeroValue;
	int64 StableCandidateSize = -1;
	int32 StableCandidatePolls = 0;
	bool bCapturePending = false;
};

IMPLEMENT_MODULE(FRenderTrailCaptureEditorModule, RenderTrailCaptureEditor)

#undef LOCTEXT_NAMESPACE
