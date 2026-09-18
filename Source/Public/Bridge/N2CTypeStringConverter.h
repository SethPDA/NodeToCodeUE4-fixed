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
 * three produce the same type spelling. The reverse direction (string -> FEdGraphPinType) belongs
 * to the importer (P2) and is not implemented here.
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

private:
	/** Convert everything except the container wrapper (used for both the main type and map values) */
	static FString TerminalTypeToString(const FName& Category, const FName& SubCategory, const UObject* SubCategoryObject);
};
