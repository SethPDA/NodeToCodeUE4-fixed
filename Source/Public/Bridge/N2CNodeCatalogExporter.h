// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

/**
 * @file N2CNodeCatalogExporter.h
 * @brief Exports the "node catalog" - what the AI may call - as JSON (docs/BLUEPRINT_CPP_BRIDGE.md §9.3)
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * @class FN2CNodeCatalogExporter
 * @brief Lists BlueprintCallable/BlueprintPure functions the AI may reference, plus real StandardMacros pins
 *
 * Covers, per docs §9.3: a curated list of common engine libraries (resolved by name at runtime,
 * so this module doesn't need to depend on AIModule etc.), every native class in the VoidLine
 * module, and every Blueprint/function library under /Game. CatalogExtraClasses (settings-driven)
 * is deferred to P5, when the Bridge settings category is added.
 */
class FN2CNodeCatalogExporter
{
public:
	static FString ExportCatalog(bool bPrettyPrint = true);

private:
	static void AddClassFunctions(const UClass* Class, TArray<TSharedPtr<FJsonValue>>& OutEntries, TSet<const UClass*>& SeenClasses);
	static TSharedPtr<FJsonObject> BuildFunctionEntry(const UClass* Class, UFunction* Function);

	static void AddCuratedEngineClasses(TArray<TSharedPtr<FJsonValue>>& OutEntries, TSet<const UClass*>& SeenClasses);
	static void AddProjectClasses(TArray<TSharedPtr<FJsonValue>>& OutEntries, TSet<const UClass*>& SeenClasses);
	static void AddGameBlueprintClasses(TArray<TSharedPtr<FJsonValue>>& OutEntries, TSet<const UClass*>& SeenClasses);

	/** Spawns each StandardMacros macro in a transient graph and dumps its real pins (docs §9.3) */
	static TArray<TSharedPtr<FJsonValue>> BuildStandardMacrosCatalog();
	static TSharedPtr<FJsonObject> DumpMacroPins(UEdGraph* MacroGraph);
};
