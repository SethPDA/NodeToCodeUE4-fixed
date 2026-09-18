// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "Bridge/N2CNodeCatalogExporter.h"
#include "Bridge/N2CTypeStringConverter.h"
#include "Utils/N2CLogger.h"

#include "Engine/Blueprint.h"
#include "K2Node_MacroInstance.h"
#include "EdGraphSchema_K2.h"

#include "AssetRegistryModule.h"
#include "UObject/UObjectIterator.h"
#include "Modules/ModuleManager.h"

#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

TSharedPtr<FJsonObject> FN2CNodeCatalogExporter::BuildFunctionEntry(const UClass* Class, UFunction* Function)
{
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("class"), Class->GetPathName());
	Entry->SetStringField(TEXT("function"), Function->GetName());
	Entry->SetBoolField(TEXT("pure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
	Entry->SetBoolField(TEXT("static"), Function->HasAnyFunctionFlags(FUNC_Static));
	Entry->SetBoolField(TEXT("latent"), Function->HasMetaData(TEXT("Latent")));

	TArray<TSharedPtr<FJsonValue>> Inputs;
	TArray<TSharedPtr<FJsonValue>> Outputs;
	for (TFieldIterator<FProperty> PropIt(Function); PropIt && PropIt->HasAnyPropertyFlags(CPF_Parm); ++PropIt)
	{
		FProperty* Param = *PropIt;

		TArray<TSharedPtr<FJsonValue>> Pair;
		Pair.Add(MakeShared<FJsonValueString>(Param->HasAnyPropertyFlags(CPF_ReturnParm) ? TEXT("ReturnValue") : Param->GetName()));
		Pair.Add(MakeShared<FJsonValueString>(FN2CTypeStringConverter::PropertyToString(Param)));
		TSharedPtr<FJsonValueArray> PairValue = MakeShared<FJsonValueArray>(Pair);

		if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			Outputs.Add(PairValue);
			continue;
		}

		// A non-const out param is a genuine output; everything else (including const-ref) is an input
		const bool bIsOut = Param->HasAnyPropertyFlags(CPF_OutParm) && !Param->HasAnyPropertyFlags(CPF_ConstParm);
		(bIsOut ? Outputs : Inputs).Add(PairValue);
	}
	Entry->SetArrayField(TEXT("inputs"), Inputs);
	Entry->SetArrayField(TEXT("outputs"), Outputs);

	const FString Category = Function->GetMetaData(TEXT("Category"));
	if (!Category.IsEmpty())
	{
		Entry->SetStringField(TEXT("category"), Category);
	}
	const FString Tooltip = Function->GetToolTipText().ToString();
	if (!Tooltip.IsEmpty())
	{
		Entry->SetStringField(TEXT("tooltip"), Tooltip);
	}

	return Entry;
}

void FN2CNodeCatalogExporter::AddClassFunctions(const UClass* Class, TArray<TSharedPtr<FJsonValue>>& OutEntries, TSet<const UClass*>& SeenClasses)
{
	if (!Class || SeenClasses.Contains(Class))
	{
		return;
	}
	SeenClasses.Add(Class);

	for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
	{
		UFunction* Function = *FuncIt;
		if (!Function || !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
		{
			continue;
		}
		OutEntries.Add(MakeShared<FJsonValueObject>(BuildFunctionEntry(Class, Function)));
	}
}

void FN2CNodeCatalogExporter::AddCuratedEngineClasses(TArray<TSharedPtr<FJsonValue>>& OutEntries, TSet<const UClass*>& SeenClasses)
{
	// Resolved by short name at runtime (rather than #included headers) so this module doesn't
	// need to depend on AIModule etc. just for the catalog. A class not found here just means the
	// owning module isn't loaded in this project - not an error.
	static const TArray<FString> CuratedClassNames = {
		TEXT("KismetSystemLibrary"), TEXT("KismetMathLibrary"), TEXT("KismetStringLibrary"),
		TEXT("KismetArrayLibrary"), TEXT("BlueprintMapLibrary"), TEXT("BlueprintSetLibrary"),
		TEXT("GameplayStatics"), TEXT("AIBlueprintHelperLibrary"), TEXT("BTFunctionLibrary"),
		TEXT("BlackboardComponent")
	};

	for (const FString& ClassName : CuratedClassNames)
	{
		UClass* Class = FindObject<UClass>(ANY_PACKAGE, *ClassName);
		if (!Class)
		{
			FN2CLogger::Get().LogWarning(FString::Printf(TEXT("Node catalog: curated class '%s' not found (its module may not be loaded)"), *ClassName));
			continue;
		}
		AddClassFunctions(Class, OutEntries, SeenClasses);
	}
}

void FN2CNodeCatalogExporter::AddProjectClasses(TArray<TSharedPtr<FJsonValue>>& OutEntries, TSet<const UClass*>& SeenClasses)
{
	static const FName VoidLinePackageName(TEXT("/Script/VoidLine"));
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class || Class->GetOutermost()->GetFName() != VoidLinePackageName)
		{
			continue;
		}
		AddClassFunctions(Class, OutEntries, SeenClasses);
	}
}

void FN2CNodeCatalogExporter::AddGameBlueprintClasses(TArray<TSharedPtr<FJsonValue>>& OutEntries, TSet<const UClass*>& SeenClasses)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> BlueprintAssets;
	AssetRegistry.GetAssets(Filter, BlueprintAssets);

	FN2CLogger::Get().Log(
		FString::Printf(TEXT("Node catalog: scanning %d /Game Blueprint asset(s)"), BlueprintAssets.Num()),
		EN2CLogSeverity::Info);

	for (const FAssetData& AssetData : BlueprintAssets)
	{
		UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
		if (Blueprint && Blueprint->GeneratedClass)
		{
			AddClassFunctions(Blueprint->GeneratedClass, OutEntries, SeenClasses);
		}
	}
}

TSharedPtr<FJsonObject> FN2CNodeCatalogExporter::DumpMacroPins(UEdGraph* MacroGraph)
{
	if (!MacroGraph)
	{
		return nullptr;
	}

	// Spawn a real MacroInstance node so the pins we dump match what a placed node would
	// actually expose (display renaming / wildcard resolution included), not just the macro
	// graph's raw tunnel pins (docs §9.3: "spawn each macro in a transient graph").
	UEdGraph* TempGraph = NewObject<UEdGraph>(GetTransientPackage(), NAME_None, RF_Transient);
	TempGraph->Schema = UEdGraphSchema_K2::StaticClass();

	UK2Node_MacroInstance* MacroNode = NewObject<UK2Node_MacroInstance>(TempGraph);
	TempGraph->AddNode(MacroNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	MacroNode->CreateNewGuid();
	MacroNode->PostPlacedNewNode();
	MacroNode->SetMacroGraph(MacroGraph); // must precede AllocateDefaultPins
	MacroNode->AllocateDefaultPins();

	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("macro"), MacroGraph->GetName());

	TArray<TSharedPtr<FJsonValue>> Inputs;
	TArray<TSharedPtr<FJsonValue>> Outputs;
	for (UEdGraphPin* Pin : MacroNode->Pins)
	{
		if (!Pin)
		{
			continue;
		}
		TSharedPtr<FJsonValueString> NameValue = MakeShared<FJsonValueString>(Pin->PinName.ToString());
		(Pin->Direction == EGPD_Input ? Inputs : Outputs).Add(NameValue);
	}
	Json->SetArrayField(TEXT("inputs"), Inputs);
	Json->SetArrayField(TEXT("outputs"), Outputs);

	TempGraph->MarkPendingKill();

	return Json;
}

TArray<TSharedPtr<FJsonValue>> FN2CNodeCatalogExporter::BuildStandardMacrosCatalog()
{
	TArray<TSharedPtr<FJsonValue>> MacroEntries;

	UBlueprint* StandardMacrosBlueprint = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
	if (!StandardMacrosBlueprint)
	{
		FN2CLogger::Get().LogWarning(TEXT("Node catalog: could not load StandardMacros - macro pin names will be missing from the catalog"));
		return MacroEntries;
	}

	for (UEdGraph* MacroGraph : StandardMacrosBlueprint->MacroGraphs)
	{
		if (TSharedPtr<FJsonObject> MacroJson = DumpMacroPins(MacroGraph))
		{
			MacroEntries.Add(MakeShared<FJsonValueObject>(MacroJson));
		}
	}

	return MacroEntries;
}

FString FN2CNodeCatalogExporter::ExportCatalog(bool bPrettyPrint)
{
	TArray<TSharedPtr<FJsonValue>> Entries;
	TSet<const UClass*> SeenClasses;

	AddCuratedEngineClasses(Entries, SeenClasses);
	AddProjectClasses(Entries, SeenClasses);
	AddGameBlueprintClasses(Entries, SeenClasses);

	TSharedPtr<FJsonObject> RootJson = MakeShared<FJsonObject>();
	RootJson->SetStringField(TEXT("format"), TEXT("n2c.catalog"));
	RootJson->SetNumberField(TEXT("version"), 1);
	RootJson->SetArrayField(TEXT("functions"), Entries);
	RootJson->SetArrayField(TEXT("macros"), BuildStandardMacrosCatalog());

	FString OutputString;
	if (bPrettyPrint)
	{
		TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutputString, 1);
		FJsonSerializer::Serialize(RootJson.ToSharedRef(), Writer);
	}
	else
	{
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
		FJsonSerializer::Serialize(RootJson.ToSharedRef(), Writer);
	}

	return OutputString;
}
