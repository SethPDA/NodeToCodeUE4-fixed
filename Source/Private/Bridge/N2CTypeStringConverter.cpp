// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "Bridge/N2CTypeStringConverter.h"

#include "EdGraphSchema_K2.h"
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"
#include "Engine/Blueprint.h"
#include "AssetRegistryModule.h"
#include "Modules/ModuleManager.h"

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

// ============================================================================
// Reverse direction: v2 grammar string -> engine type (for the importer, P2)
// ============================================================================

bool FN2CTypeStringConverter::SplitMapArgs(const FString& Args, FString& OutKey, FString& OutValue)
{
	int32 Depth = 0;
	for (int32 i = 0; i < Args.Len(); ++i)
	{
		const TCHAR C = Args[i];
		if (C == TEXT('<'))
		{
			++Depth;
		}
		else if (C == TEXT('>'))
		{
			--Depth;
		}
		else if (C == TEXT(',') && Depth == 0)
		{
			OutKey = Args.Left(i).TrimStartAndEnd();
			OutValue = Args.Mid(i + 1).TrimStartAndEnd();
			return true;
		}
	}
	return false;
}

bool FN2CTypeStringConverter::ParseTerminalType(const FString& TypeString, FName& OutCategory, FName& OutSubCategory, UObject*& OutSubCategoryObject, FString& OutError)
{
	OutSubCategory = NAME_None;
	OutSubCategoryObject = nullptr;

	const FString Trimmed = TypeString.TrimStartAndEnd();
	FString Prefix, Rest;
	if (!Trimmed.Split(TEXT(":"), &Prefix, &Rest))
	{
		const FString Lower = Trimmed.ToLower();
		if (Lower == TEXT("bool")) { OutCategory = UEdGraphSchema_K2::PC_Boolean; return true; }
		if (Lower == TEXT("byte")) { OutCategory = UEdGraphSchema_K2::PC_Byte; return true; }
		if (Lower == TEXT("int")) { OutCategory = UEdGraphSchema_K2::PC_Int; return true; }
		if (Lower == TEXT("int64")) { OutCategory = UEdGraphSchema_K2::PC_Int64; return true; }
		if (Lower == TEXT("float")) { OutCategory = UEdGraphSchema_K2::PC_Float; return true; }
		if (Lower == TEXT("string")) { OutCategory = UEdGraphSchema_K2::PC_String; return true; }
		if (Lower == TEXT("name")) { OutCategory = UEdGraphSchema_K2::PC_Name; return true; }
		if (Lower == TEXT("text")) { OutCategory = UEdGraphSchema_K2::PC_Text; return true; }

		static const TMap<FString, FString> BuiltInStructNames = {
			{TEXT("vector"), TEXT("Vector")}, {TEXT("vector2d"), TEXT("Vector2D")},
			{TEXT("rotator"), TEXT("Rotator")}, {TEXT("transform"), TEXT("Transform")},
			{TEXT("linearcolor"), TEXT("LinearColor")}, {TEXT("intpoint"), TEXT("IntPoint")},
			{TEXT("intvector"), TEXT("IntVector")}
		};
		if (const FString* StructName = BuiltInStructNames.Find(Lower))
		{
			UScriptStruct* Struct = FindObject<UScriptStruct>(ANY_PACKAGE, **StructName);
			if (!Struct)
			{
				OutError = FString::Printf(TEXT("built-in struct '%s' could not be resolved"), **StructName);
				return false;
			}
			OutCategory = UEdGraphSchema_K2::PC_Struct;
			OutSubCategoryObject = Struct;
			return true;
		}

		OutError = FString::Printf(TEXT("unrecognized type '%s'"), *Trimmed);
		return false;
	}

	const FString Kind = Prefix.ToLower();
	if (Kind == TEXT("struct"))
	{
		UScriptStruct* Struct = ResolveStructByNameOrPath(Rest);
		if (!Struct)
		{
			OutError = FString::Printf(TEXT("struct '%s' not found"), *Rest);
			return false;
		}
		OutCategory = UEdGraphSchema_K2::PC_Struct;
		OutSubCategoryObject = Struct;
		return true;
	}
	if (Kind == TEXT("enum"))
	{
		UEnum* Enum = ResolveEnumByNameOrPath(Rest);
		if (!Enum)
		{
			OutError = FString::Printf(TEXT("enum '%s' not found"), *Rest);
			return false;
		}
		OutCategory = UEdGraphSchema_K2::PC_Byte;
		OutSubCategoryObject = Enum;
		return true;
	}

	static const TMap<FString, FName> ObjectLikeKinds = {
		{TEXT("object"), UEdGraphSchema_K2::PC_Object},
		{TEXT("class"), UEdGraphSchema_K2::PC_Class},
		{TEXT("softobject"), UEdGraphSchema_K2::PC_SoftObject},
		{TEXT("softclass"), UEdGraphSchema_K2::PC_SoftClass},
		{TEXT("interface"), UEdGraphSchema_K2::PC_Interface}
	};
	if (const FName* Category = ObjectLikeKinds.Find(Kind))
	{
		UClass* Class = ResolveClassByNameOrPath(Rest);
		if (!Class)
		{
			OutError = FString::Printf(TEXT("class '%s' not found"), *Rest);
			return false;
		}
		OutCategory = *Category;
		OutSubCategoryObject = Class;
		return true;
	}

	OutError = FString::Printf(TEXT("unrecognized type prefix '%s:' in '%s'"), *Prefix, *Trimmed);
	return false;
}

bool FN2CTypeStringConverter::StringToPinType(const FString& TypeString, FEdGraphPinType& OutPinType, FString& OutError)
{
	OutPinType = FEdGraphPinType();
	const FString Trimmed = TypeString.TrimStartAndEnd();

	// Tri-state: unset = "this prefix didn't match, try the next one"
	auto ParseContainer = [&](const TCHAR* Prefix, EPinContainerType ContainerType) -> TOptional<bool>
	{
		if (!Trimmed.StartsWith(Prefix) || !Trimmed.EndsWith(TEXT(">")))
		{
			return TOptional<bool>();
		}
		const int32 PrefixLen = FCString::Strlen(Prefix);
		const FString Inner = Trimmed.Mid(PrefixLen, Trimmed.Len() - PrefixLen - 1);
		OutPinType.ContainerType = ContainerType;

		if (ContainerType == EPinContainerType::Map)
		{
			FString KeyStr, ValueStr;
			if (!SplitMapArgs(Inner, KeyStr, ValueStr))
			{
				OutError = FString::Printf(TEXT("map<...> requires a key and a value type, got '%s'"), *Inner);
				return TOptional<bool>(false);
			}
			UObject* KeyObj = nullptr;
			if (!ParseTerminalType(KeyStr, OutPinType.PinCategory, OutPinType.PinSubCategory, KeyObj, OutError))
			{
				return TOptional<bool>(false);
			}
			OutPinType.PinSubCategoryObject = KeyObj;

			UObject* ValueObj = nullptr;
			if (!ParseTerminalType(ValueStr, OutPinType.PinValueType.TerminalCategory, OutPinType.PinValueType.TerminalSubCategory, ValueObj, OutError))
			{
				return TOptional<bool>(false);
			}
			OutPinType.PinValueType.TerminalSubCategoryObject = ValueObj;
		}
		else
		{
			UObject* SubObj = nullptr;
			if (!ParseTerminalType(Inner, OutPinType.PinCategory, OutPinType.PinSubCategory, SubObj, OutError))
			{
				return TOptional<bool>(false);
			}
			OutPinType.PinSubCategoryObject = SubObj;
		}
		return TOptional<bool>(true);
	};

	if (TOptional<bool> Result = ParseContainer(TEXT("array<"), EPinContainerType::Array)) { return Result.GetValue(); }
	if (TOptional<bool> Result = ParseContainer(TEXT("set<"), EPinContainerType::Set)) { return Result.GetValue(); }
	if (TOptional<bool> Result = ParseContainer(TEXT("map<"), EPinContainerType::Map)) { return Result.GetValue(); }

	UObject* SubObj = nullptr;
	if (!ParseTerminalType(Trimmed, OutPinType.PinCategory, OutPinType.PinSubCategory, SubObj, OutError))
	{
		return false;
	}
	OutPinType.PinSubCategoryObject = SubObj;
	return true;
}

UClass* FN2CTypeStringConverter::ResolveClassByNameOrPath(const FString& NameOrPath)
{
	if (NameOrPath.IsEmpty())
	{
		return nullptr;
	}

	if (NameOrPath.StartsWith(TEXT("/")))
	{
		if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *NameOrPath))
		{
			return Found;
		}
		if (UClass* Loaded = LoadObject<UClass>(nullptr, *NameOrPath))
		{
			return Loaded;
		}
		if (UClass* Loaded = LoadObject<UClass>(nullptr, *(NameOrPath + TEXT("_C"))))
		{
			return Loaded;
		}
		if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *NameOrPath))
		{
			return Blueprint->GeneratedClass;
		}
		return nullptr;
	}

	if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *NameOrPath))
	{
		return Found;
	}
	// Accept the C++ naming convention (AActor, UObject, IInterface) as well as the bare name
	if (NameOrPath.Len() > 1 && (NameOrPath[0] == TEXT('A') || NameOrPath[0] == TEXT('U') || NameOrPath[0] == TEXT('I')))
	{
		if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *NameOrPath.RightChop(1)))
		{
			return Found;
		}
	}
	// A Blueprint-generated class always ends in _C
	if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *(NameOrPath + TEXT("_C"))))
	{
		return Found;
	}

	// Last resort: search the asset registry for a Blueprint asset with this name and load it
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> BlueprintAssets;
	AssetRegistryModule.Get().GetAssetsByClass(UBlueprint::StaticClass()->GetFName(), BlueprintAssets, true);
	for (const FAssetData& AssetData : BlueprintAssets)
	{
		if (AssetData.AssetName.ToString() == NameOrPath)
		{
			if (UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset()))
			{
				return Blueprint->GeneratedClass;
			}
		}
	}

	return nullptr;
}

UScriptStruct* FN2CTypeStringConverter::ResolveStructByNameOrPath(const FString& NameOrPath)
{
	if (NameOrPath.IsEmpty())
	{
		return nullptr;
	}

	if (NameOrPath.StartsWith(TEXT("/")))
	{
		if (UScriptStruct* Found = FindObject<UScriptStruct>(ANY_PACKAGE, *NameOrPath))
		{
			return Found;
		}
		return LoadObject<UScriptStruct>(nullptr, *NameOrPath);
	}

	if (UScriptStruct* Found = FindObject<UScriptStruct>(ANY_PACKAGE, *NameOrPath))
	{
		return Found;
	}
	if (NameOrPath.Len() > 1 && NameOrPath[0] == TEXT('F'))
	{
		if (UScriptStruct* Found = FindObject<UScriptStruct>(ANY_PACKAGE, *NameOrPath.RightChop(1)))
		{
			return Found;
		}
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> StructAssets;
	AssetRegistryModule.Get().GetAssetsByClass(FName(TEXT("UserDefinedStruct")), StructAssets, true);
	for (const FAssetData& AssetData : StructAssets)
	{
		if (AssetData.AssetName.ToString() == NameOrPath)
		{
			return Cast<UScriptStruct>(AssetData.GetAsset());
		}
	}

	return nullptr;
}

UEnum* FN2CTypeStringConverter::ResolveEnumByNameOrPath(const FString& NameOrPath)
{
	if (NameOrPath.IsEmpty())
	{
		return nullptr;
	}

	if (NameOrPath.StartsWith(TEXT("/")))
	{
		if (UEnum* Found = FindObject<UEnum>(ANY_PACKAGE, *NameOrPath))
		{
			return Found;
		}
		return LoadObject<UEnum>(nullptr, *NameOrPath);
	}

	if (UEnum* Found = FindObject<UEnum>(ANY_PACKAGE, *NameOrPath))
	{
		return Found;
	}
	if (NameOrPath.Len() > 1 && NameOrPath[0] == TEXT('E'))
	{
		if (UEnum* Found = FindObject<UEnum>(ANY_PACKAGE, *NameOrPath.RightChop(1)))
		{
			return Found;
		}
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> EnumAssets;
	AssetRegistryModule.Get().GetAssetsByClass(FName(TEXT("UserDefinedEnum")), EnumAssets, true);
	for (const FAssetData& AssetData : EnumAssets)
	{
		if (AssetData.AssetName.ToString() == NameOrPath)
		{
			return Cast<UEnum>(AssetData.GetAsset());
		}
	}

	return nullptr;
}
