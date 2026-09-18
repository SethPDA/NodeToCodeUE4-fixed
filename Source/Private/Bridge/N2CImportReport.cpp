// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "Bridge/N2CImportReport.h"

#include "Dom/JsonValue.h"

void FN2CImportReport::AddOk(const FString& Scope, const FString& Ref, const FString& Message)
{
	FN2CImportReportEntry Entry;
	Entry.Level = EN2CReportLevel::OK;
	Entry.Scope = Scope;
	Entry.Ref = Ref;
	Entry.Message = Message;
	Entries.Add(MoveTemp(Entry));
}

void FN2CImportReport::AddWarning(const FString& Scope, const FString& Ref, const FString& Message, const TArray<FString>& Choices)
{
	FN2CImportReportEntry Entry;
	Entry.Level = EN2CReportLevel::Warning;
	Entry.Scope = Scope;
	Entry.Ref = Ref;
	Entry.Message = Message;
	Entry.Choices = Choices;
	Entries.Add(MoveTemp(Entry));
}

void FN2CImportReport::AddError(const FString& Scope, const FString& Ref, const FString& Message, const TArray<FString>& Choices)
{
	FN2CImportReportEntry Entry;
	Entry.Level = EN2CReportLevel::Error;
	Entry.Scope = Scope;
	Entry.Ref = Ref;
	Entry.Message = Message;
	Entry.Choices = Choices;
	Entries.Add(MoveTemp(Entry));
}

bool FN2CImportReport::HasErrors() const
{
	return CountErrors() > 0;
}

int32 FN2CImportReport::CountErrors() const
{
	int32 Count = 0;
	for (const FN2CImportReportEntry& Entry : Entries)
	{
		if (Entry.Level == EN2CReportLevel::Error)
		{
			++Count;
		}
	}
	return Count;
}

int32 FN2CImportReport::CountWarnings() const
{
	int32 Count = 0;
	for (const FN2CImportReportEntry& Entry : Entries)
	{
		if (Entry.Level == EN2CReportLevel::Warning)
		{
			++Count;
		}
	}
	return Count;
}

FString FN2CImportReport::ToText() const
{
	FString Out;
	Out += FString::Printf(TEXT("N2C IMPORT REPORT - %d error(s), %d warning(s) - target %s - mode %s\n"),
		CountErrors(), CountWarnings(), *TargetDescription, *ModeDescription);

	for (const FN2CImportReportEntry& Entry : Entries)
	{
		const TCHAR* LevelTag = TEXT("[OK]   ");
		if (Entry.Level == EN2CReportLevel::Warning)
		{
			LevelTag = TEXT("[WARN] ");
		}
		else if (Entry.Level == EN2CReportLevel::Error)
		{
			LevelTag = TEXT("[ERROR]");
		}

		Out += FString::Printf(TEXT("%s %s %s : %s"), LevelTag, *Entry.Scope, *Entry.Ref, *Entry.Message);
		if (Entry.Choices.Num() > 0)
		{
			Out += TEXT(" ") + FString::Join(Entry.Choices, TEXT(", "));
		}
		Out += TEXT("\n");
	}

	return Out;
}

TSharedPtr<FJsonObject> FN2CImportReport::ToJsonObject() const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("target"), TargetDescription);
	Root->SetStringField(TEXT("mode"), ModeDescription);
	Root->SetNumberField(TEXT("errors"), CountErrors());
	Root->SetNumberField(TEXT("warnings"), CountWarnings());

	TArray<TSharedPtr<FJsonValue>> EntriesJson;
	for (const FN2CImportReportEntry& Entry : Entries)
	{
		TSharedPtr<FJsonObject> EntryJson = MakeShared<FJsonObject>();
		switch (Entry.Level)
		{
		case EN2CReportLevel::OK: EntryJson->SetStringField(TEXT("level"), TEXT("ok")); break;
		case EN2CReportLevel::Warning: EntryJson->SetStringField(TEXT("level"), TEXT("warning")); break;
		case EN2CReportLevel::Error: EntryJson->SetStringField(TEXT("level"), TEXT("error")); break;
		}
		EntryJson->SetStringField(TEXT("scope"), Entry.Scope);
		EntryJson->SetStringField(TEXT("ref"), Entry.Ref);
		EntryJson->SetStringField(TEXT("message"), Entry.Message);
		if (Entry.Choices.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ChoicesJson;
			for (const FString& Choice : Entry.Choices)
			{
				ChoicesJson.Add(MakeShared<FJsonValueString>(Choice));
			}
			EntryJson->SetArrayField(TEXT("choices"), ChoicesJson);
		}
		EntriesJson.Add(MakeShared<FJsonValueObject>(EntryJson));
	}
	Root->SetArrayField(TEXT("entries"), EntriesJson);

	return Root;
}
