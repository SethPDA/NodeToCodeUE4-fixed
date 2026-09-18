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
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
