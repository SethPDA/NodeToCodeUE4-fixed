// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "Bridge/N2CTypeStringConverter.h"

#include "EdGraphSchema_K2.h"
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"

FString FN2CTypeStringConverter::GetClassShortName(const UClass* Class)
{
	if (!Class)
	{
		return FString();
	}

	FString Name = Class->GetName();
	if (Name.EndsWith(TEXT("_C")))
	{
		Name.LeftChopInline(2);
	}
	return Name;
}

FString FN2CTypeStringConverter::TerminalTypeToString(const FName& Category, const FName& SubCategory, const UObject* SubCategoryObject)
{
	if (Category == UEdGraphSchema_K2::PC_Exec)
	{
		return TEXT("exec");
	}
	if (Category == UEdGraphSchema_K2::PC_Boolean)
	{
		return TEXT("bool");
	}
	if (Category == UEdGraphSchema_K2::PC_Byte)
	{
		// A byte pin whose SubCategoryObject is a UEnum is a genuine user/native enum, not a raw byte
		if (const UEnum* Enum = Cast<UEnum>(SubCategoryObject))
		{
			return FString::Printf(TEXT("enum:%s"), *Enum->GetName());
		}
		return TEXT("byte");
	}
	if (Category == UEdGraphSchema_K2::PC_Int)
	{
		return TEXT("int");
	}
	if (Category == UEdGraphSchema_K2::PC_Int64)
	{
		return TEXT("int64");
	}
	if (Category == UEdGraphSchema_K2::PC_Float)
	{
		// UE 4.27 has no PC_Double/PC_Real - every floating point pin is PC_Float
		return TEXT("float");
	}
	if (Category == UEdGraphSchema_K2::PC_Name)
	{
		return TEXT("name");
	}
	if (Category == UEdGraphSchema_K2::PC_String)
	{
		return TEXT("string");
	}
	if (Category == UEdGraphSchema_K2::PC_Text)
	{
		return TEXT("text");
	}
	if (Category == UEdGraphSchema_K2::PC_Struct)
	{
		if (const UScriptStruct* Struct = Cast<UScriptStruct>(SubCategoryObject))
		{
			static const TSet<FString> BuiltInStructs = {
				TEXT("Vector"), TEXT("Vector2D"), TEXT("Rotator"), TEXT("Transform"),
				TEXT("LinearColor"), TEXT("IntPoint"), TEXT("IntVector")
			};
			const FString StructName = Struct->GetName();
			if (BuiltInStructs.Contains(StructName))
			{
				return StructName.ToLower();
			}
			return FString::Printf(TEXT("struct:%s"), *StructName);
		}
		return TEXT("struct:Unknown");
	}
	if (Category == UEdGraphSchema_K2::PC_Object)
	{
		return FString::Printf(TEXT("object:%s"), *GetClassShortName(Cast<UClass>(SubCategoryObject)));
	}
	if (Category == UEdGraphSchema_K2::PC_Class)
	{
		return FString::Printf(TEXT("class:%s"), *GetClassShortName(Cast<UClass>(SubCategoryObject)));
	}
	if (Category == UEdGraphSchema_K2::PC_SoftObject)
	{
		return FString::Printf(TEXT("softobject:%s"), *GetClassShortName(Cast<UClass>(SubCategoryObject)));
	}
	if (Category == UEdGraphSchema_K2::PC_SoftClass)
	{
		return FString::Printf(TEXT("softclass:%s"), *GetClassShortName(Cast<UClass>(SubCategoryObject)));
	}
	if (Category == UEdGraphSchema_K2::PC_Interface)
	{
		return FString::Printf(TEXT("interface:%s"), *GetClassShortName(Cast<UClass>(SubCategoryObject)));
	}
	if (Category == UEdGraphSchema_K2::PC_Delegate || Category == UEdGraphSchema_K2::PC_MCDelegate)
	{
		// Not part of the v2 declarations grammar (§7.7); informational only
		return FString::Printf(TEXT("delegate:%s"), SubCategoryObject ? *SubCategoryObject->GetName() : TEXT("Unknown"));
	}
	if (Category == UEdGraphSchema_K2::PC_Wildcard)
	{
		return TEXT("wildcard");
	}

	// Fallback: lowercase the raw category name rather than silently dropping type info
	return Category.ToString().ToLower();
}

FString FN2CTypeStringConverter::PinTypeToString(const FEdGraphPinType& PinType)
{
	const FString TerminalString = TerminalTypeToString(PinType.PinCategory, PinType.PinSubCategory, PinType.PinSubCategoryObject.Get());

	switch (PinType.ContainerType)
	{
	case EPinContainerType::Array:
		return FString::Printf(TEXT("array<%s>"), *TerminalString);
	case EPinContainerType::Set:
		return FString::Printf(TEXT("set<%s>"), *TerminalString);
	case EPinContainerType::Map:
		{
			const FString ValueString = TerminalTypeToString(
				PinType.PinValueType.TerminalCategory,
				PinType.PinValueType.TerminalSubCategory,
				PinType.PinValueType.TerminalSubCategoryObject.Get());
			return FString::Printf(TEXT("map<%s,%s>"), *TerminalString, *ValueString);
		}
	default:
		return TerminalString;
	}
}

FString FN2CTypeStringConverter::PropertyToString(const FProperty* Property)
{
	if (!Property)
	{
		return TEXT("unknown");
	}

	FEdGraphPinType PinType;
	if (!GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, PinType))
	{
		return TEXT("unknown");
	}

	return PinTypeToString(PinType);
}
