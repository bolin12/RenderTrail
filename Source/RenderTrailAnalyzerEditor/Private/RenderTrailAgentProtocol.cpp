#include "RenderTrailAgentProtocol.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::RenderTrail::Private::AgentProtocol
{
	namespace
	{
		FString ExtractJsonObject(const FString& Content)
		{
			const int32 FirstBrace = Content.Find(TEXT("{"));
			const int32 LastBrace = Content.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			return FirstBrace != INDEX_NONE && LastBrace >= FirstBrace
				? Content.Mid(FirstBrace, LastBrace - FirstBrace + 1)
				: FString();
		}

		FString RepairJsonBrackets(const FString& Json, bool& bOutRepaired)
		{
			bOutRepaired = false;
			FString Repaired;
			Repaired.Reserve(Json.Len() + 4);
			TArray<TCHAR> BracketStack;
			bool bInString = false;
			bool bEscaped = false;

			for (const TCHAR Character : Json)
			{
				if (bInString)
				{
					Repaired.AppendChar(Character);
					if (bEscaped)
					{
						bEscaped = false;
					}
					else if (Character == TEXT('\\'))
					{
						bEscaped = true;
					}
					else if (Character == TEXT('"'))
					{
						bInString = false;
					}
					continue;
				}

				if (Character == TEXT('"'))
				{
					bInString = true;
					Repaired.AppendChar(Character);
					continue;
				}
				if (Character == TEXT('{') || Character == TEXT('['))
				{
					BracketStack.Add(Character);
					Repaired.AppendChar(Character);
					continue;
				}
				if (Character == TEXT('}') || Character == TEXT(']'))
				{
					if (BracketStack.IsEmpty())
					{
						bOutRepaired = true;
						continue;
					}
					const TCHAR ExpectedOpen = Character == TEXT('}') ? TEXT('{') : TEXT('[');
					if (BracketStack.Last() == ExpectedOpen)
					{
						BracketStack.Pop();
						Repaired.AppendChar(Character);
					}
					else
					{
						// A frequent model error is an extra array closer after a correctly
						// closed object (`..."evidence":"..."}],`). Dropping that unmatched
						// closer preserves the still-open parent object. Closing the parent here
						// would make every following field appear outside the root JSON object.
						bOutRepaired = true;
					}
					continue;
				}
				Repaired.AppendChar(Character);
			}

			while (!BracketStack.IsEmpty())
			{
				Repaired.AppendChar(BracketStack.Pop() == TEXT('{') ? TEXT('}') : TEXT(']'));
				bOutRepaired = true;
			}
			return Repaired;
		}
	}

	bool TryParseActionJson(const FString& Content, FString& OutJson,
		TSharedPtr<FJsonObject>& OutAction, bool& bOutRepaired, FString* OutError)
	{
		if (OutError)
		{
			OutError->Empty();
		}
		OutJson = ExtractJsonObject(Content);
		OutAction.Reset();
		bOutRepaired = false;
		if (OutJson.IsEmpty())
		{
			if (OutError)
			{
				*OutError = TEXT("No complete JSON object delimiters were found in the model content.");
			}
			return false;
		}

		FString LastParseError;
		const auto TryParse = [&OutAction, &LastParseError](const FString& Candidate) -> bool
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Candidate);
			const bool bParsed = FJsonSerializer::Deserialize(Reader, OutAction) && OutAction.IsValid();
			if (!bParsed)
			{
				LastParseError = Reader->GetErrorMessage();
			}
			return bParsed;
		};
		if (TryParse(OutJson))
		{
			return true;
		}

		bool bRepaired = false;
		const FString RepairedJson = RepairJsonBrackets(OutJson, bRepaired);
		if (!bRepaired || !TryParse(RepairedJson))
		{
			if (OutError)
			{
				*OutError = bRepaired
					? FString::Printf(TEXT("JSON remained invalid after bounded closer repair: %s"), *LastParseError)
					: FString::Printf(TEXT("JSON parsing failed and no bounded closer repair applied: %s"), *LastParseError);
			}
			return false;
		}
		OutJson = RepairedJson;
		bOutRepaired = true;
		return true;
	}
}
