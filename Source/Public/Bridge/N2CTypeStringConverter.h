// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

/**
 * @file N2CTypeStringConverter.h
 * @brief Converts between UE4.27 pin/property types and the N2C Graph v2 type grammar (docs/BLUEPRINT_CPP_BRIDGE.md §7.7)
 */

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

/**
 * @class FN2CTypeStringConverter
 * @brief One-way conversion from engine pin types to v2 type strings (e.g. "array<object:BP_Enemy>")
 *
 * Used by the v2 exporter, the Blueprint summary exporter and the node catalog exporter so all
 * three produce the same type spelling, and by the v2 importer (P2) to go the other way when
 * creating declarations and local variables.
 */
class FN2CTypeStringConverter
{
public:
	/** Convert a live pin/property type to its v2 grammar string, e.g. "map<int,object:StaticMeshComponent>" */
	static FString PinTypeToString(const FEdGraphPinType& PinType);

	/** Convert a UFunction parameter/return property to its v2 grammar string */
	static FString PropertyToString(const FProperty* Property);

	/**
	 * Short display name for a class reference in the v2 grammar: the bare class name, with the
	 * trailing "_C" stripped for Blueprint-generated classes (docs §7.7: "Blueprint classes use the
	 * asset name without _C").
	 */
	static FString GetClassShortName(const UClass* Class);

	/**
	 * Parse a v2 grammar type string (docs §7.7) into an engine pin type, e.g. "array<object:Actor>".
	 * Used by the importer for declarations, local variables and event/custom_event parameters.
	 * @return false with OutError set if the string doesn't parse or a referenced class/struct/enum
	 *         can't be resolved.
	 */
	static bool StringToPinType(const FString& TypeString, FEdGraphPinType& OutPinType, FString& OutError);

	/**
	 * Resolve a class by full path ("/Script/Engine.Actor"), short native name ("Actor", with or
	 * without the "U"/"A"/"AActor"-style prefix), or Blueprint asset name ("BP_Enemy", resolved to
	 * its generated class via the asset registry). Returns nullptr if nothing matches.
	 */
	static UClass* ResolveClassByNameOrPath(const FString& NameOrPath);

	/** Resolve a struct by full path, short native name, or user-defined struct asset name. */
	static UScriptStruct* ResolveStructByNameOrPath(const FString& NameOrPath);

	/** Resolve an enum by full path, short native name, or user-defined enum asset name. */
	static UEnum* ResolveEnumByNameOrPath(const FString& NameOrPath);

private:
	/** Convert everything except the container wrapper (used for both the main type and map values) */
	static FString TerminalTypeToString(const FName& Category, const FName& SubCategory, const UObject* SubCategoryObject);

	/** Parse a single (non-container) terminal type string into category/subcategory/object */
	static bool ParseTerminalType(const FString& TypeString, FName& OutCategory, FName& OutSubCategory, UObject*& OutSubCategoryObject, FString& OutError);

	/** Split "K,V" at the top-level comma (not one nested inside a map<...> key or value) */
	static bool SplitMapArgs(const FString& Args, FString& OutKey, FString& OutValue);
};
