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

#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node.h"

namespace N2CBridgeTests
{
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

#endif // WITH_DEV_AUTOMATION_TESTS
