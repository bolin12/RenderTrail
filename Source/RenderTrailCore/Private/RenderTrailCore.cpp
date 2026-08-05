#include "RenderTrailProtocol.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::RenderTrail
{
	FString GetMetadataPathForCapture(const FString& CapturePath)
	{
		return FPaths::ChangeExtension(CapturePath, MetadataExtension);
	}

	FString FCaptureMetadata::ToJson() const
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schemaVersion"), ProtocolVersion);
		Root->SetStringField(TEXT("capturePath"), CapturePath);
		Root->SetStringField(TEXT("projectName"), ProjectName);
		Root->SetStringField(TEXT("projectDirectory"), ProjectDirectory);
		Root->SetStringField(TEXT("mapName"), MapName);
		Root->SetStringField(TEXT("engineVersion"), EngineVersion);
		Root->SetStringField(TEXT("utcTimestamp"), UtcTimestamp);
		Root->SetStringField(TEXT("frameCounter"), LexToString(FrameCounter));
		Root->SetBoolField(TEXT("isPIE"), bIsPIE);

		FString Result;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
		FJsonSerializer::Serialize(Root, Writer);
		return Result;
	}

	bool FCaptureMetadata::FromJson(const FString& Json, FCaptureMetadata& OutMetadata, FString& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = TEXT("Invalid metadata JSON.");
			return false;
		}

		const int32 SchemaVersion = static_cast<int32>(Root->GetNumberField(TEXT("schemaVersion")));
		if (SchemaVersion != ProtocolVersion)
		{
			OutError = FString::Printf(TEXT("Unsupported metadata schema %d (expected %d)."), SchemaVersion, ProtocolVersion);
			return false;
		}

		OutMetadata.CapturePath = Root->GetStringField(TEXT("capturePath"));
		OutMetadata.ProjectName = Root->GetStringField(TEXT("projectName"));
		OutMetadata.ProjectDirectory = Root->GetStringField(TEXT("projectDirectory"));
		OutMetadata.MapName = Root->GetStringField(TEXT("mapName"));
		OutMetadata.EngineVersion = Root->GetStringField(TEXT("engineVersion"));
		OutMetadata.UtcTimestamp = Root->GetStringField(TEXT("utcTimestamp"));
		LexFromString(OutMetadata.FrameCounter, *Root->GetStringField(TEXT("frameCounter")));
		OutMetadata.bIsPIE = Root->GetBoolField(TEXT("isPIE"));
		return true;
	}

	bool FCaptureMetadata::SaveAdjacent(FString& OutMetadataPath, FString& OutError) const
	{
		OutMetadataPath = GetMetadataPathForCapture(CapturePath);
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutMetadataPath), true))
		{
			OutError = FString::Printf(TEXT("Unable to create metadata directory: %s"), *FPaths::GetPath(OutMetadataPath));
			return false;
		}
		if (!FFileHelper::SaveStringToFile(ToJson(), *OutMetadataPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Unable to write metadata: %s"), *OutMetadataPath);
			return false;
		}
		return true;
	}
}

IMPLEMENT_MODULE(FDefaultModuleImpl, RenderTrailCore)
