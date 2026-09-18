// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

/**
 * @file N2CBridgeTests.cpp
 * @brief Automation tests for the Blueprint <-> C++ bridge (N2C Graph v2, see docs/BLUEPRINT_CPP_BRIDGE.md)
 */

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Bridge/N2CGraphExporterV2.h"
#include "Bridge/N2CBlueprintSummaryExporter.h"
#include "Bridge/N2CNodeCatalogExporter.h"
#include "Bridge/N2CTypeStringConverter.h"
#include "Bridge/N2CGraphDocument.h"
#include "Bridge/N2CGraphImporter.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Editor.h"
#include "UObject/Package.h"
#include "AssetRegistryModule.h"

namespace N2CBridgeTests
{
	/** A fresh transient test Blueprint (AActor parent), per docs §16 - never touches real assets. */
	static UBlueprint* CreateTransientTestBlueprint(const TCHAR* Name)
	{
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), Name),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
	}

	/** Full path to Source/Private/Tests/Fixtures, or an empty string if the plugin can't be resolved. */
	static FString GetFixturesDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NodeToCode"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/Private/Tests/Fixtures"));
	}
}

/**
 * P0 smoke test (docs/BLUEPRINT_CPP_BRIDGE.md §15): confirms the "NodeToCode.Bridge" automation
 * filter is wired up and that the N2C Graph v2 example fixture (§7.9) is present and parses as
 * valid JSON. It does not exercise a v2 parser -- that arrives with the importer in P2.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CBridgeFixtureLoadsTest, "NodeToCode.Bridge.FixtureLoads", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CBridgeFixtureLoadsTest::RunTest(const FString& Parameters)
{
	const FString FixturesDir = N2CBridgeTests::GetFixturesDir();
	if (!TestFalse(TEXT("NodeToCode plugin base directory should resolve"), FixturesDir.IsEmpty()))
	{
		return false;
	}

	const FString FixturePath = FPaths::Combine(FixturesDir, TEXT("N2CGraphV2_Example.json"));
	if (!TestTrue(FString::Printf(TEXT("Fixture file should exist: %s"), *FixturePath), FPaths::FileExists(FixturePath)))
	{
		return false;
	}

	FString FixtureText;
	if (!TestTrue(TEXT("Fixture file should load"), FFileHelper::LoadFileToString(FixtureText, *FixturePath)))
	{
		return false;
	}

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FixtureText);
	if (!TestTrue(TEXT("Fixture should parse as valid JSON"), FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid()))
	{
		return false;
	}

	FString Format;
	JsonObject->TryGetStringField(TEXT("format"), Format);
	TestEqual(TEXT("Fixture format should be n2c.graph"), Format, FString(TEXT("n2c.graph")));

	int32 Version = 0;
	JsonObject->TryGetNumberField(TEXT("version"), Version);
	TestEqual(TEXT("Fixture version should be 2"), Version, 2);

	const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
	if (TestTrue(TEXT("Fixture should have a graphs array"), JsonObject->TryGetArrayField(TEXT("graphs"), Graphs) && Graphs != nullptr))
	{
		TestEqual(TEXT("Fixture should declare 2 graphs (EventGraph + GetDamageMultiplier)"), Graphs->Num(), 2);
	}

	return true;
}

/**
 * P1 (docs §15): a map pin's key AND value type must both be visible and in the right slot -
 * the exact spot the v1 translator gets backwards (docs §18.2, N2CArrayProcessor.cpp:36-72,
 * key/value swapped). Self-contained: builds the pin type by hand rather than depending on a
 * specific node in a specific asset having a map pin.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CTypeConverterMapTypeTest, "NodeToCode.Bridge.TypeConverter.MapType", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CTypeConverterMapTypeTest::RunTest(const FString& Parameters)
{
	FEdGraphPinType MapPinType;
	MapPinType.PinCategory = UEdGraphSchema_K2::PC_Int; // key type
	MapPinType.ContainerType = EPinContainerType::Map;
	MapPinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Object; // value type
	MapPinType.PinValueType.TerminalSubCategoryObject = AActor::StaticClass();

	const FString TypeString = FN2CTypeStringConverter::PinTypeToString(MapPinType);
	TestEqual(TEXT("map<key,value> should put the key type first and the value type second"),
		TypeString, FString(TEXT("map<int,object:Actor>")));

	return true;
}

/**
 * P1 acceptance test (docs §15): exporting BP_Grid_Revealed's GenerateBoxLocation_FOR_BREAK must
 * produce pin-level links with internal names, the same link count as the live graph's LinkedTo
 * pairs, Branch/Loop outputs that can be told apart, and map value types in the verbose "types" map.
 * Read-only: never modifies or saves the asset (docs §16).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CBridgeExportRoundTripTest, "NodeToCode.Bridge.ExportRoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FN2CBridgeExportRoundTripTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, TEXT("/Game/PuzzleMechanics/Grid/BP_Grid_Revealed"));
	if (!TestNotNull(TEXT("BP_Grid_Revealed should load"), Blueprint))
	{
		return false;
	}

	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == TEXT("GenerateBoxLocation_FOR_BREAK"))
		{
			TargetGraph = Graph;
			break;
		}
	}
	if (!TestNotNull(TEXT("GenerateBoxLocation_FOR_BREAK function graph should exist"), TargetGraph))
	{
		return false;
	}

	// Ground truth, computed directly from the live graph: one entry per LinkedTo pair whose
	// source is an output pin and whose target is also a K2 node (matches what the exporter does).
	int32 ExpectedLinkCount = 0;
	for (UEdGraphNode* GraphNode : TargetGraph->Nodes)
	{
		UK2Node* K2 = Cast<UK2Node>(GraphNode);
		if (!K2)
		{
			continue;
		}
		for (UEdGraphPin* Pin : K2->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked && Cast<UK2Node>(Linked->GetOwningNode()))
				{
					++ExpectedLinkCount;
				}
			}
		}
	}

	const FString JsonOutput = FN2CGraphExporterV2::ExportGraph(TargetGraph, TSet<UEdGraphNode*>(), /*bPrettyPrint=*/false);
	if (!TestFalse(TEXT("Export should produce output"), JsonOutput.IsEmpty()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootJson;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonOutput);
	if (!TestTrue(TEXT("Export should be valid JSON"), FJsonSerializer::Deserialize(Reader, RootJson) && RootJson.IsValid()))
	{
		return false;
	}

	FString Format;
	RootJson->TryGetStringField(TEXT("format"), Format);
	TestEqual(TEXT("format should be n2c.graph"), Format, FString(TEXT("n2c.graph")));

	const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
	if (!TestTrue(TEXT("Document should have a graphs array"), RootJson->TryGetArrayField(TEXT("graphs"), Graphs) && Graphs && Graphs->Num() == 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> GraphJson = (*Graphs)[0]->AsObject();
	if (!TestTrue(TEXT("graphs[0] should be an object"), GraphJson.IsValid()))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
	if (!TestTrue(TEXT("Graph should have a links array"), GraphJson->TryGetArrayField(TEXT("links"), Links) && Links))
	{
		return false;
	}
	TestEqual(TEXT("Exported link count should equal the live graph's output-pin LinkedTo count"), Links->Num(), ExpectedLinkCount);

	// Every link endpoint must be "id.PinName" - i.e. pin-level, not the v1 node-to-node pairs.
	int32 BadLinkEndpoints = 0;
	for (const TSharedPtr<FJsonValue>& LinkValue : *Links)
	{
		const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
		if (!LinkValue->TryGetArray(Pair) || !Pair || Pair->Num() != 2)
		{
			++BadLinkEndpoints;
			continue;
		}
		for (const TSharedPtr<FJsonValue>& EndpointValue : *Pair)
		{
			FString Endpoint;
			if (!EndpointValue->TryGetString(Endpoint) || !Endpoint.Contains(TEXT(".")))
			{
				++BadLinkEndpoints;
			}
		}
	}
	TestEqual(TEXT("Every link endpoint should be a pin-level \"id.PinName\" reference"), BadLinkEndpoints, 0);

	// Branch and Loop outputs can be told apart: collect every exec-output pin name used as a
	// link source by a "branch" or "macro" node, and confirm no leftover display-style names
	// ("True"/"False") slipped in in place of the real internal names ("then"/"else").
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	GraphJson->TryGetArrayField(TEXT("nodes"), Nodes);
	TestNotNull(TEXT("Graph should have a nodes array"), Nodes);

	TSet<FString> BranchNodeIds;
	TSet<FString> MacroNodeIds;
	bool bFoundBranch = false;
	bool bFoundMacro = false;
	if (Nodes)
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
		{
			const TSharedPtr<FJsonObject> NodeJson = NodeValue->AsObject();
			if (!NodeJson.IsValid())
			{
				continue;
			}
			FString Id, Kind;
			NodeJson->TryGetStringField(TEXT("id"), Id);
			NodeJson->TryGetStringField(TEXT("kind"), Kind);
			if (Kind == TEXT("branch"))
			{
				BranchNodeIds.Add(Id);
				bFoundBranch = true;
			}
			else if (Kind == TEXT("macro"))
			{
				MacroNodeIds.Add(Id);
				bFoundMacro = true;
			}
		}
	}
	TestTrue(TEXT("GenerateBoxLocation_FOR_BREAK should contain at least one Branch node"), bFoundBranch);
	TestTrue(TEXT("GenerateBoxLocation_FOR_BREAK should contain at least one loop macro node"), bFoundMacro);

	static const TSet<FString> ValidBranchExecPinNames = { TEXT("then"), TEXT("else"), TEXT("execute") };
	int32 BadBranchPinNames = 0;
	for (const TSharedPtr<FJsonValue>& LinkValue : *Links)
	{
		const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
		if (!LinkValue->TryGetArray(Pair) || !Pair || Pair->Num() != 2)
		{
			continue;
		}
		FString From;
		(*Pair)[0]->TryGetString(From);
		int32 DotIndex = INDEX_NONE;
		if (!From.FindChar(TEXT('.'), DotIndex))
		{
			continue;
		}
		const FString SourceId = From.Left(DotIndex);
		const FString SourcePin = From.Mid(DotIndex + 1);
		if (BranchNodeIds.Contains(SourceId) && !ValidBranchExecPinNames.Contains(SourcePin))
		{
			++BadBranchPinNames;
		}
	}
	TestEqual(TEXT("Branch exec outputs should use internal pin names (then/else), not display names (True/False)"), BadBranchPinNames, 0);

	// Map value types are present: BP_Grid_Revealed's BlueprintMapLibrary calls (Map_Keys/Map_Find/
	// Map_Add/Map_Clear) resolve a wildcard TargetMap pin to a real map<key,value> type.
	bool bFoundMapType = false;
	const TSharedPtr<FJsonObject>* TypesJson = nullptr;
	if (GraphJson->TryGetObjectField(TEXT("types"), TypesJson) && TypesJson && TypesJson->IsValid())
	{
		for (const auto& Pair : (*TypesJson)->Values)
		{
			FString TypeString;
			if (Pair.Value->TryGetString(TypeString) && TypeString.StartsWith(TEXT("map<")))
			{
				bFoundMapType = true;
				break;
			}
		}
	}
	TestTrue(TEXT("The verbose \"types\" map should include at least one map<key,value> entry"), bFoundMapType);

	return true;
}

/**
 * The catalog loads (and therefore compiles) every /Game Blueprint to list its BlueprintCallable
 * functions. Some pre-existing project content doesn't compile cleanly (unrelated to NodeToCode or
 * this test, e.g. a Blueprint referencing a since-deleted class); that noise shouldn't fail a test
 * about StandardMacros pin names. SuppressLogs() disables the automation framework's "any logged
 * error/warning fails the test" behaviour for this test only.
 */
class FN2CQuietAutomationTestBase : public FAutomationTestBase
{
public:
	FN2CQuietAutomationTestBase(const FString& InName, const bool bInComplexTask)
		: FAutomationTestBase(InName, bInComplexTask)
	{
	}
	virtual bool SuppressLogs() override { return true; }
};

/**
 * P1: the node catalog must list the real StandardMacros pins (docs §9.3), settling the pin-name
 * table in §7.5 rather than guessing at cosmetic display names.
 */
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FN2CNodeCatalogStandardMacrosTest, FN2CQuietAutomationTestBase, "NodeToCode.Bridge.NodeCatalog.StandardMacros", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CNodeCatalogStandardMacrosTest::RunTest(const FString& Parameters)
{
	const FString JsonOutput = FN2CNodeCatalogExporter::ExportCatalog(/*bPrettyPrint=*/false);
	if (!TestFalse(TEXT("Catalog export should produce output"), JsonOutput.IsEmpty()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootJson;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonOutput);
	if (!TestTrue(TEXT("Catalog should be valid JSON"), FJsonSerializer::Deserialize(Reader, RootJson) && RootJson.IsValid()))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Macros = nullptr;
	if (!TestTrue(TEXT("Catalog should have a macros array"), RootJson->TryGetArrayField(TEXT("macros"), Macros) && Macros))
	{
		return false;
	}

	TSharedPtr<FJsonObject> ForEachLoopJson;
	for (const TSharedPtr<FJsonValue>& MacroValue : *Macros)
	{
		const TSharedPtr<FJsonObject> MacroJson = MacroValue->AsObject();
		FString MacroName;
		if (MacroJson.IsValid() && MacroJson->TryGetStringField(TEXT("macro"), MacroName) && MacroName == TEXT("ForEachLoop"))
		{
			ForEachLoopJson = MacroJson;
			break;
		}
	}
	if (!TestTrue(TEXT("Catalog should list the ForEachLoop macro"), ForEachLoopJson.IsValid()))
	{
		return false;
	}

	auto ContainsPinName = [](const TSharedPtr<FJsonObject>& MacroJson, const TCHAR* Field, const TCHAR* PinName) -> bool
	{
		const TArray<TSharedPtr<FJsonValue>>* PinArray = nullptr;
		if (!MacroJson->TryGetArrayField(Field, PinArray) || !PinArray)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& PinValue : *PinArray)
		{
			FString Name;
			if (PinValue->TryGetString(Name) && Name == PinName)
			{
				return true;
			}
		}
		return false;
	};

	TestTrue(TEXT("ForEachLoop inputs should include \"Array\""), ContainsPinName(ForEachLoopJson, TEXT("inputs"), TEXT("Array")));
	TestTrue(TEXT("ForEachLoop outputs should include \"LoopBody\""), ContainsPinName(ForEachLoopJson, TEXT("outputs"), TEXT("LoopBody")));
	TestTrue(TEXT("ForEachLoop outputs should include \"Array Element\""), ContainsPinName(ForEachLoopJson, TEXT("outputs"), TEXT("Array Element")));
	TestTrue(TEXT("ForEachLoop outputs should include \"Array Index\""), ContainsPinName(ForEachLoopJson, TEXT("outputs"), TEXT("Array Index")));
	TestTrue(TEXT("ForEachLoop outputs should include \"Completed\""), ContainsPinName(ForEachLoopJson, TEXT("outputs"), TEXT("Completed")));

	return true;
}

/**
 * P1 smoke test: Copy Blueprint Summary shouldn't crash and should produce the documented envelope.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CBlueprintSummarySmokeTest, "NodeToCode.Bridge.BlueprintSummary.Smoke", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FN2CBlueprintSummarySmokeTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, TEXT("/Game/PuzzleMechanics/Grid/BP_Grid_Revealed"));
	if (!TestNotNull(TEXT("BP_Grid_Revealed should load"), Blueprint))
	{
		return false;
	}

	const FString JsonOutput = FN2CBlueprintSummaryExporter::ExportSummary(Blueprint, /*bPrettyPrint=*/false);
	if (!TestFalse(TEXT("Summary export should produce output"), JsonOutput.IsEmpty()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootJson;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonOutput);
	if (!TestTrue(TEXT("Summary should be valid JSON"), FJsonSerializer::Deserialize(Reader, RootJson) && RootJson.IsValid()))
	{
		return false;
	}

	FString Format;
	RootJson->TryGetStringField(TEXT("format"), Format);
	TestEqual(TEXT("format should be n2c.bpsummary"), Format, FString(TEXT("n2c.bpsummary")));

	return true;
}

/**
 * P2 acceptance test (docs §15): the §7.9 example document - the exact JSON in the bridge doc's own
 * spec - must import into a fresh transient Blueprint with 0 errors and compile. This exercises
 * declarations (2 variables, 1 function), event/variable_get/branch/call_function/macro nodes, and
 * pin-level linking, end to end.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CImporterSpecExampleTest, "NodeToCode.Bridge.Import.SpecExample", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CImporterSpecExampleTest::RunTest(const FString& Parameters)
{
	const FString FixturesDir = N2CBridgeTests::GetFixturesDir();
	const FString FixturePath = FPaths::Combine(FixturesDir, TEXT("N2CGraphV2_Example.json"));
	FString FixtureText;
	if (!TestTrue(TEXT("Fixture file should load"), FFileHelper::LoadFileToString(FixtureText, *FixturePath)))
	{
		return false;
	}

	UBlueprint* TestBlueprint = N2CBridgeTests::CreateTransientTestBlueprint(TEXT("N2CTestBP_SpecExample"));
	if (!TestNotNull(TEXT("Transient test Blueprint should be created"), TestBlueprint))
	{
		return false;
	}

	FN2CImportOptions Options;
	Options.Mode = EN2CImportMode::InsertIntoGraph;
	Options.bCreateDeclarations = true;
	Options.bCompileAfter = true;

	// The document has 2 graphs (EventGraph + GetDamageMultiplier); pass no explicit target so each
	// is matched to the Blueprint's own graphs by name (§8.3 step 2) - EventGraph already exists,
	// GetDamageMultiplier is created fresh by "declarations.functions".
	const FN2CImportResult Result = FN2CGraphImporter::Import(FixtureText, TestBlueprint, nullptr, Options);

	TestTrue(Result.Report.ToText(), Result.bSuccess);
	TestEqual(TEXT("Import should report 0 errors"), Result.Report.CountErrors(), 0);

	return true;
}

/**
 * P2 acceptance test (docs §15): a misspelled function name should produce a "did you mean"
 * suggestion, and a misspelled pin name should list the node's real pins - the exact report shape
 * documented at §8.4.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CImporterTypoSuggestionsTest, "NodeToCode.Bridge.Import.TypoSuggestions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CImporterTypoSuggestionsTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBlueprint = N2CBridgeTests::CreateTransientTestBlueprint(TEXT("N2CTestBP_Typo"));
	if (!TestNotNull(TEXT("Transient test Blueprint should be created"), TestBlueprint))
	{
		return false;
	}

	const FString TypoJson = TEXT(R"({
		"format": "n2c.graph", "version": 2,
		"graphs": [{
			"name": "EventGraph", "kind": "event_graph",
			"nodes": [
				{"id": "begin", "kind": "event", "event": "ReceiveBeginPlay"},
				{"id": "loop", "kind": "macro", "macro": "ForEachLoop"},
				{"id": "tostr", "kind": "call_function", "function": "Conv_VectorToStrng", "class": "KismetStringLibrary"}
			],
			"links": [
				["begin.then", "loop.Exec"],
				["loop.ArrayElem", "tostr.InVec"]
			]
		}]
	})");

	FN2CImportOptions Options;
	Options.Mode = EN2CImportMode::ValidateOnly;
	const FN2CImportResult Result = FN2CGraphImporter::Import(TypoJson, TestBlueprint, TestBlueprint->UbergraphPages.Num() > 0 ? TestBlueprint->UbergraphPages[0] : nullptr, Options);

	TestFalse(TEXT("A document with typos should not succeed"), Result.bSuccess);

	bool bFoundFunctionSuggestion = false;
	bool bFoundPinList = false;
	for (const FN2CImportReportEntry& Entry : Result.Report.Entries)
	{
		if (Entry.Level != EN2CReportLevel::Error)
		{
			continue;
		}
		if (Entry.Message.Contains(TEXT("Conv_VectorToStrng")) && Entry.Message.Contains(TEXT("Conv_VectorToString")))
		{
			bFoundFunctionSuggestion = true;
		}
		if (Entry.Message.Contains(TEXT("ArrayElem")) && Entry.Message.Contains(TEXT("not found")) &&
			Entry.Message.Contains(TEXT("Array Element")))
		{
			bFoundPinList = true;
		}
	}

	TestTrue(FString::Printf(TEXT("Report should suggest 'Conv_VectorToString' for the typo'd function. Report:\n%s"), *Result.Report.ToText()), bFoundFunctionSuggestion);
	TestTrue(FString::Printf(TEXT("Report should list the real pins for the typo'd link. Report:\n%s"), *Result.Report.ToText()), bFoundPinList);

	return true;
}

/**
 * P2 acceptance test (docs §15, §17): one Ctrl+Z (GEditor->UndoTransaction) removes the whole
 * import, since it all happens inside a single FScopedTransaction (§8.3 step 3).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CImporterUndoTest, "NodeToCode.Bridge.Import.UndoRemovesWholeImport", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CImporterUndoTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEditor should be available in this automation context"), GEditor))
	{
		return false;
	}

	UBlueprint* TestBlueprint = N2CBridgeTests::CreateTransientTestBlueprint(TEXT("N2CTestBP_Undo"));
	if (!TestNotNull(TEXT("Transient test Blueprint should be created"), TestBlueprint) || TestBlueprint->UbergraphPages.Num() == 0)
	{
		return false;
	}
	UEdGraph* EventGraph = TestBlueprint->UbergraphPages[0];
	const int32 NodeCountBefore = EventGraph->Nodes.Num();

	const FString SimpleJson = TEXT(R"({
		"format": "n2c.graph", "version": 2,
		"graphs": [{
			"name": "EventGraph", "kind": "event_graph",
			"nodes": [
				{"id": "begin", "kind": "event", "event": "ReceiveBeginPlay"},
				{"id": "print", "kind": "call_function", "function": "PrintString", "class": "KismetSystemLibrary"}
			],
			"links": [["begin.then", "print.execute"]]
		}]
	})");

	FN2CImportOptions Options;
	Options.Mode = EN2CImportMode::InsertIntoGraph;
	Options.bCompileAfter = false;
	const FN2CImportResult Result = FN2CGraphImporter::Import(SimpleJson, TestBlueprint, EventGraph, Options);

	if (!TestTrue(Result.Report.ToText(), Result.bSuccess))
	{
		return false;
	}
	const int32 NodeCountAfterImport = EventGraph->Nodes.Num();
	TestTrue(TEXT("Import should have added at least one node"), NodeCountAfterImport > NodeCountBefore);

	GEditor->UndoTransaction();

	const int32 NodeCountAfterUndo = EventGraph->Nodes.Num();
	TestEqual(TEXT("A single undo should remove the whole import"), NodeCountAfterUndo, NodeCountBefore);

	return true;
}

namespace N2CBridgeTests
{
	/** Reduces an exported v2 document's first graph to id-independent (kind + discriminator) node
	 * signatures and (fromSignature.pin -> toSignature.pin) link signatures, so two exports of
	 * equivalent content can be compared even if node ids differ (docs §9.1: "Ids and positions may
	 * differ"). Returns false if the JSON doesn't parse as a v2 document.
	 */
	static bool BuildRoundTripSignatures(const FString& JsonText, TArray<FString>& OutNodeSignatures, TArray<FString>& OutLinkSignatures)
	{
		TSharedPtr<FJsonObject> RootJson;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, RootJson) || !RootJson.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!RootJson->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs || Graphs->Num() == 0)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> GraphJson = (*Graphs)[0]->AsObject();
		if (!GraphJson.IsValid())
		{
			return false;
		}

		TMap<FString, FString> IdToSignature;
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (GraphJson->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
		{
			static const TCHAR* DiscriminatorFields[] = { TEXT("function"), TEXT("variable"), TEXT("macro"), TEXT("struct"), TEXT("enum"), TEXT("event"), TEXT("name") };
			for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
			{
				const TSharedPtr<FJsonObject> NodeJson = NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
				if (!NodeJson.IsValid())
				{
					continue;
				}
				FString Id, Kind;
				NodeJson->TryGetStringField(TEXT("id"), Id);
				NodeJson->TryGetStringField(TEXT("kind"), Kind);

				FString Discriminator;
				for (const TCHAR* Field : DiscriminatorFields)
				{
					FString Value;
					if (NodeJson->TryGetStringField(Field, Value) && !Value.IsEmpty())
					{
						Discriminator = Value;
						break;
					}
				}

				const FString Signature = Kind + TEXT(":") + Discriminator;
				IdToSignature.Add(Id, Signature);
				OutNodeSignatures.Add(Signature);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
		if (GraphJson->TryGetArrayField(TEXT("links"), Links) && Links)
		{
			for (const TSharedPtr<FJsonValue>& LinkValue : *Links)
			{
				const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
				if (!LinkValue.IsValid() || !LinkValue->TryGetArray(Pair) || !Pair || Pair->Num() != 2)
				{
					continue;
				}
				FString From, To;
				(*Pair)[0]->TryGetString(From);
				(*Pair)[1]->TryGetString(To);

				auto ToRefSignature = [&IdToSignature](const FString& Ref) -> FString
				{
					int32 DotIndex = INDEX_NONE;
					if (!Ref.FindChar(TEXT('.'), DotIndex))
					{
						return Ref;
					}
					const FString Id = Ref.Left(DotIndex);
					const FString Pin = Ref.Mid(DotIndex + 1);
					const FString* Sig = IdToSignature.Find(Id);
					return (Sig ? *Sig : Id) + TEXT(".") + Pin;
				};

				OutLinkSignatures.Add(ToRefSignature(From) + TEXT(" -> ") + ToRefSignature(To));
			}
		}

		return true;
	}
}

/**
 * P2 acceptance test (docs §15): export -> clear -> import -> export on a duplicate of
 * BP_Grid_Revealed's GenerateBoxLocation_FOR_BREAK function must give the same set of nodes and
 * links (ids and positions may differ, §9.1). Never touches the original asset (docs §16): works on
 * an in-memory duplicate under /Game/__N2CTest/ that is never saved and is discarded at the end.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CImporterRoundTripTest, "NodeToCode.Bridge.Import.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FN2CImporterRoundTripTest::RunTest(const FString& Parameters)
{
	UBlueprint* SourceBlueprint = LoadObject<UBlueprint>(nullptr, TEXT("/Game/PuzzleMechanics/Grid/BP_Grid_Revealed"));
	if (!TestNotNull(TEXT("BP_Grid_Revealed should load"), SourceBlueprint))
	{
		return false;
	}

	UPackage* DuplicatePackage = CreatePackage(TEXT("/Game/__N2CTest/BP_Grid_Revealed_P2RoundTrip"));
	UBlueprint* DuplicatedBlueprint = Cast<UBlueprint>(StaticDuplicateObject(SourceBlueprint, DuplicatePackage, TEXT("BP_Grid_Revealed_P2RoundTrip")));
	if (!TestNotNull(TEXT("Blueprint duplication should succeed"), DuplicatedBlueprint))
	{
		return false;
	}
	DuplicatedBlueprint->SetFlags(RF_Standalone | RF_Public);
	FAssetRegistryModule::AssetCreated(DuplicatedBlueprint);
	FKismetEditorUtilities::CompileBlueprint(DuplicatedBlueprint);

	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* Graph : DuplicatedBlueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == TEXT("GenerateBoxLocation_FOR_BREAK"))
		{
			TargetGraph = Graph;
			break;
		}
	}
	if (!TestNotNull(TEXT("GenerateBoxLocation_FOR_BREAK should exist on the duplicate"), TargetGraph))
	{
		return false;
	}

	const FString JsonA = FN2CGraphExporterV2::ExportGraph(TargetGraph, TSet<UEdGraphNode*>(), /*bPrettyPrint=*/false);
	if (!TestFalse(TEXT("First export should produce output"), JsonA.IsEmpty()))
	{
		return false;
	}

	// "Clear" the graph: remove everything except the intrinsic Entry/Result nodes, which the
	// importer only ever reuses, never (re-)creates (§8.3 step 6).
	TArray<UEdGraphNode*> NodesToRemove;
	for (UEdGraphNode* Node : TargetGraph->Nodes)
	{
		if (!Node->IsA<UK2Node_FunctionEntry>() && !Node->IsA<UK2Node_FunctionResult>())
		{
			NodesToRemove.Add(Node);
		}
	}
	for (UEdGraphNode* Node : NodesToRemove)
	{
		FBlueprintEditorUtils::RemoveNode(DuplicatedBlueprint, Node, /*bDontRecompile=*/true);
	}

	FN2CImportOptions Options;
	Options.Mode = EN2CImportMode::InsertIntoGraph;
	Options.bCompileAfter = true;
	const FN2CImportResult ImportResult = FN2CGraphImporter::Import(JsonA, DuplicatedBlueprint, TargetGraph, Options);
	TestTrue(ImportResult.Report.ToText(), ImportResult.bSuccess);

	const FString JsonB = FN2CGraphExporterV2::ExportGraph(TargetGraph, TSet<UEdGraphNode*>(), /*bPrettyPrint=*/false);
	if (!TestFalse(TEXT("Second export should produce output"), JsonB.IsEmpty()))
	{
		return false;
	}

	TArray<FString> NodeSignaturesA, LinkSignaturesA, NodeSignaturesB, LinkSignaturesB;
	const bool bParsedA = N2CBridgeTests::BuildRoundTripSignatures(JsonA, NodeSignaturesA, LinkSignaturesA);
	const bool bParsedB = N2CBridgeTests::BuildRoundTripSignatures(JsonB, NodeSignaturesB, LinkSignaturesB);
	TestTrue(TEXT("First export should parse as a v2 document"), bParsedA);
	TestTrue(TEXT("Second export should parse as a v2 document"), bParsedB);

	if (bParsedA && bParsedB)
	{
		NodeSignaturesA.Sort();
		NodeSignaturesB.Sort();
		LinkSignaturesA.Sort();
		LinkSignaturesB.Sort();

		TestEqual(TEXT("Round trip should produce the same number of nodes"), NodeSignaturesB.Num(), NodeSignaturesA.Num());
		TestEqual(TEXT("Round trip should produce the same number of links"), LinkSignaturesB.Num(), LinkSignaturesA.Num());

		if (NodeSignaturesA.Num() == NodeSignaturesB.Num())
		{
			int32 MismatchedNodes = 0;
			FString FirstMismatch;
			for (int32 i = 0; i < NodeSignaturesA.Num(); ++i)
			{
				if (NodeSignaturesA[i] != NodeSignaturesB[i])
				{
					++MismatchedNodes;
					if (FirstMismatch.IsEmpty())
					{
						FirstMismatch = FString::Printf(TEXT("'%s' vs '%s'"), *NodeSignaturesA[i], *NodeSignaturesB[i]);
					}
				}
			}
			TestEqual(FString::Printf(TEXT("Round trip should produce the same set of nodes (first mismatch: %s)"), *FirstMismatch), MismatchedNodes, 0);
		}

		if (LinkSignaturesA.Num() == LinkSignaturesB.Num())
		{
			int32 MismatchedLinks = 0;
			FString FirstMismatch;
			for (int32 i = 0; i < LinkSignaturesA.Num(); ++i)
			{
				if (LinkSignaturesA[i] != LinkSignaturesB[i])
				{
					++MismatchedLinks;
					if (FirstMismatch.IsEmpty())
					{
						FirstMismatch = FString::Printf(TEXT("'%s' vs '%s'"), *LinkSignaturesA[i], *LinkSignaturesB[i]);
					}
				}
			}
			TestEqual(FString::Printf(TEXT("Round trip should produce the same set of links (first mismatch: %s)"), *FirstMismatch), MismatchedLinks, 0);
		}
	}

	// Cleanup (§16): never saved to disk, but drop it from the in-memory asset registry / object
	// graph so this transient test asset doesn't linger for the rest of the process.
	FAssetRegistryModule::AssetDeleted(DuplicatedBlueprint);
	DuplicatedBlueprint->ClearFlags(RF_Standalone | RF_Public);
	DuplicatedBlueprint->MarkPendingKill();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
