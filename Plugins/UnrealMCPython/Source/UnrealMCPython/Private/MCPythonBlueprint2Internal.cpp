// Copyright (c) 2025 GenOrca. All Rights Reserved.

#include "MCPythonBlueprint2Internal.h"

#include "MCPythonHelperInternal.h"

#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopeLock.h"
#include "Misc/Timespan.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace UE::MCPython::Blueprint2
{
namespace
{
const TCHAR* PrefixForKind(ETargetKind Kind)
{
    switch (Kind)
    {
    case ETargetKind::Graph: return TEXT("graph");
    case ETargetKind::Node: return TEXT("node");
    case ETargetKind::Pin: return TEXT("pin");
    case ETargetKind::Variable: return TEXT("variable");
    case ETargetKind::Component: return TEXT("component");
    case ETargetKind::Interface: return TEXT("interface");
    }
    return TEXT("unknown");
}

FString Sha1(const FString& Value)
{
    FTCHARToUTF8 Utf8(*Value);
    uint8 Digest[FSHA1::DigestSize];
    FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
    return BytesToHex(Digest, FSHA1::DigestSize).ToLower();
}

struct FStoredPaletteAction
{
    FPaletteActionRecord Record;
    FString EditorSessionId;
    FDateTime CreatedAt;
    FDateTime LastUsedAt;
};

struct FStoredPaletteCursor
{
    FPaletteCursorRecord Record;
    FString EditorSessionId;
    FDateTime CreatedAt;
    FDateTime LastUsedAt;
};

struct FStoredPaletteBinding
{
    FPaletteBindingRecord Record;
    FString EditorSessionId;
    FDateTime CreatedAt;
    FDateTime LastUsedAt;
};

struct FPaletteTokenState
{
    TMap<FString, FStoredPaletteAction> Actions;
    TMap<FString, FStoredPaletteCursor> Cursors;
    TMap<FString, FStoredPaletteBinding> Bindings;
    TArray<FString> ActionOrder;
    TArray<FString> CursorOrder;
    TArray<FString> BindingOrder;
    TOptional<FDateTime> TestNow;
    FCriticalSection Mutex;
};

FPaletteTokenState& PaletteTokenState()
{
    static FPaletteTokenState State;
    return State;
}

FDateTime PaletteNow(const FPaletteTokenState& State)
{
    return State.TestNow.IsSet() ? State.TestNow.GetValue() : FDateTime::UtcNow();
}

FString CurrentPaletteSessionId()
{
    return UE::MCPython::GetEditorSessionId().ToString(
        EGuidFormats::DigitsWithHyphensLower);
}

bool IsOpaqueToken(const FString& Token, const FString& Prefix)
{
    if (!Token.StartsWith(Prefix) || Token.Len() != Prefix.Len() + 40)
    {
        return false;
    }
    for (int32 Index = Prefix.Len(); Index < Token.Len(); ++Index)
    {
        const TCHAR Character = Token[Index];
        if (!((Character >= TEXT('0') && Character <= TEXT('9')) ||
              (Character >= TEXT('a') && Character <= TEXT('f'))))
        {
            return false;
        }
    }
    return true;
}

void SetPaletteError(
    FError& OutError,
    const FString& Code,
    const FString& Path,
    const FString& Message,
    const FString& Hint)
{
    OutError = FError{};
    OutError.Code = Code;
    OutError.Path = Path;
    OutError.Message = Message;
    OutError.Hint = Hint;
}

FString CanonicalPaletteContext(const FPaletteContext& Context)
{
    return Context.AssetPath + TEXT("\n") +
        Context.GraphId + TEXT("\n") +
        Context.GraphSchemaPath + TEXT("\n") +
        Context.SourcePinId + TEXT("\n") +
        Context.RequestDigest + TEXT("\n") +
        Context.ResultDigest + TEXT("\n") +
        FString::FromInt(Context.Limit);
}

bool PaletteContextMatches(
    const FPaletteContext& Stored,
    const FPaletteContext& Expected)
{
    auto Matches = [](const FString& StoredValue, const FString& ExpectedValue)
    {
        return ExpectedValue.IsEmpty() || StoredValue == ExpectedValue;
    };
    return Matches(Stored.AssetPath, Expected.AssetPath) &&
        Matches(Stored.GraphId, Expected.GraphId) &&
        Matches(Stored.GraphSchemaPath, Expected.GraphSchemaPath) &&
        Matches(Stored.SourcePinId, Expected.SourcePinId) &&
        Matches(Stored.RequestDigest, Expected.RequestDigest) &&
        Matches(Stored.ResultDigest, Expected.ResultDigest) &&
        (Expected.Limit <= 0 || Stored.Limit == Expected.Limit);
}

template <typename StoredType>
void RemoveExpiredRecords(
    TMap<FString, StoredType>& Records,
    TArray<FString>& Order,
    const FDateTime& Now)
{
    static const FTimespan Lifetime = FTimespan::FromMinutes(30.0);
    TArray<FString> Expired;
    for (const TPair<FString, StoredType>& Pair : Records)
    {
        if (Now - Pair.Value.LastUsedAt > Lifetime)
        {
            Expired.Add(Pair.Key);
        }
    }
    for (const FString& Id : Expired)
    {
        Records.Remove(Id);
        Order.Remove(Id);
    }
}

template <typename StoredType>
void EnforceRecordBound(
    TMap<FString, StoredType>& Records,
    TArray<FString>& Order,
    const int32 Maximum)
{
    while (Records.Num() > Maximum && !Order.IsEmpty())
    {
        const FString Oldest = Order[0];
        Order.RemoveAt(0);
        Records.Remove(Oldest);
    }
}

void PurgeExpiredPaletteRecords(FPaletteTokenState& State, const FDateTime& Now)
{
    RemoveExpiredRecords(State.Actions, State.ActionOrder, Now);
    RemoveExpiredRecords(State.Cursors, State.CursorOrder, Now);
    RemoveExpiredRecords(State.Bindings, State.BindingOrder, Now);
}

FString EscapeCanonicalJsonString(const FString& Value)
{
    FString Result(TEXT("\""));
    for (const TCHAR Character : Value)
    {
        switch (Character)
        {
        case TEXT('"'): Result += TEXT("\\\""); break;
        case TEXT('\\'): Result += TEXT("\\\\"); break;
        case TEXT('\b'): Result += TEXT("\\b"); break;
        case TEXT('\f'): Result += TEXT("\\f"); break;
        case TEXT('\n'): Result += TEXT("\\n"); break;
        case TEXT('\r'): Result += TEXT("\\r"); break;
        case TEXT('\t'): Result += TEXT("\\t"); break;
        default:
            if (Character < 0x20)
            {
                Result += FString::Printf(TEXT("\\u%04x"), Character);
            }
            else
            {
                Result.AppendChar(Character);
            }
            break;
        }
    }
    Result.AppendChar(TEXT('"'));
    return Result;
}

FString CanonicalJsonValue(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid() || Value->IsNull())
    {
        return TEXT("null");
    }
    switch (Value->Type)
    {
    case EJson::String:
        return EscapeCanonicalJsonString(Value->AsString());
    case EJson::Number:
        return FString::SanitizeFloat(Value->AsNumber(), 0);
    case EJson::Boolean:
        return Value->AsBool() ? TEXT("true") : TEXT("false");
    case EJson::Array:
    {
        TArray<FString> Items;
        for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
        {
            Items.Add(CanonicalJsonValue(Item));
        }
        return FString::Printf(TEXT("[%s]"), *FString::Join(Items, TEXT(",")));
    }
    case EJson::Object:
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        TArray<FString> Keys;
        Object->Values.GetKeys(Keys);
        Keys.Sort();
        TArray<FString> Fields;
        for (const FString& Key : Keys)
        {
            Fields.Add(EscapeCanonicalJsonString(Key) + TEXT(":") +
                CanonicalJsonValue(Object->Values[Key]));
        }
        return FString::Printf(TEXT("{%s}"), *FString::Join(Fields, TEXT(",")));
    }
    default:
        return TEXT("null");
    }
}

const FTransaction* TransactionAtIndex(int32 TransactionIndex)
{
    if (!GEditor || !GEditor->Trans || TransactionIndex < 0 ||
        TransactionIndex >= GEditor->Trans->GetQueueLength())
    {
        return nullptr;
    }
    return GEditor->Trans->GetTransaction(TransactionIndex);
}

FGuid TransactionGuidAtIndex(int32 TransactionIndex)
{
    const FTransaction* Transaction = TransactionAtIndex(TransactionIndex);
    return Transaction ? Transaction->GetContext().TransactionId : FGuid();
}

bool IsCurrentTransaction(int32 TransactionIndex, const FGuid& TransactionGuid)
{
    if (!GEditor || !GEditor->Trans || !TransactionGuid.IsValid())
    {
        return false;
    }
    const int32 CurrentUndoIndex =
        GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount() - 1;
    const FTransaction* Transaction = TransactionAtIndex(TransactionIndex);
    return CurrentUndoIndex == TransactionIndex && Transaction &&
        !Transaction->HasExpired() &&
        Transaction->GetContext().TransactionId == TransactionGuid;
}

FString TargetTypePath(const UEdGraph* Graph)
{
    return Graph && Graph->GetSchema()
        ? Graph->GetSchema()->GetClass()->GetPathName()
        : FString();
}

FString TargetTypePath(const UEdGraphNode* Node)
{
    return Node ? Node->GetClass()->GetPathName() : FString();
}

FString PinContainerName(const EPinContainerType ContainerType)
{
    switch (ContainerType)
    {
    case EPinContainerType::None: return TEXT("none");
    case EPinContainerType::Array: return TEXT("array");
    case EPinContainerType::Set: return TEXT("set");
    case EPinContainerType::Map: return TEXT("map");
    }
    return TEXT("unknown");
}

FString CanonicalPinType(const FEdGraphPinType& Type)
{
    const FSimpleMemberReference& Member = Type.PinSubCategoryMemberReference;
    const FEdGraphTerminalType& Terminal = Type.PinValueType;
    const FString MemberGuid = Member.MemberGuid.IsValid()
        ? Member.MemberGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
        : FString();
    return FString::Printf(
        TEXT("category=%s\nsubcategory=%s\nsubcategory_object=%s\n")
        TEXT("member_parent=%s\nmember_name=%s\nmember_guid=%s\n")
        TEXT("container=%s\nvalue_category=%s\nvalue_subcategory=%s\n")
        TEXT("value_subcategory_object=%s\nvalue_const=%d\nvalue_weak=%d\n")
        TEXT("value_wrapper=%d\nreference=%d\nconst=%d\nweak=%d\n")
        TEXT("wrapper=%d\nsingle_precision=%d"),
        *Type.PinCategory.ToString(),
        *Type.PinSubCategory.ToString(),
        *GetPathNameSafe(Type.PinSubCategoryObject.Get()),
        *GetPathNameSafe(Member.MemberParent.Get()),
        *Member.MemberName.ToString(),
        *MemberGuid,
        *PinContainerName(Type.ContainerType),
        *Terminal.TerminalCategory.ToString(),
        *Terminal.TerminalSubCategory.ToString(),
        *GetPathNameSafe(Terminal.TerminalSubCategoryObject.Get()),
        Terminal.bTerminalIsConst ? 1 : 0,
        Terminal.bTerminalIsWeakPointer ? 1 : 0,
        Terminal.bTerminalIsUObjectWrapper ? 1 : 0,
        Type.bIsReference ? 1 : 0,
        Type.bIsConst ? 1 : 0,
        Type.bIsWeakPointer ? 1 : 0,
        Type.bIsUObjectWrapper ? 1 : 0,
        Type.bSerializeAsSinglePrecisionFloat ? 1 : 0);
}

FString PinQualificationType(const UEdGraphPin* Pin)
{
    if (!Pin)
    {
        return {};
    }
    int32 MatchingPinOrdinal = 0;
    const UEdGraphNode* OwningNode = Pin->GetOwningNode();
    if (OwningNode)
    {
        for (const UEdGraphPin* Candidate : OwningNode->Pins)
        {
            if (Candidate == Pin)
            {
                break;
            }
            if (Candidate &&
                Candidate->PinName == Pin->PinName &&
                Candidate->Direction == Pin->Direction &&
                CanonicalPinType(Candidate->PinType) ==
                    CanonicalPinType(Pin->PinType))
            {
                ++MatchingPinOrdinal;
            }
        }
    }
    return FString::Printf(
        TEXT("%s\ndirection=%d\nordinal=%d"),
        *CanonicalPinType(Pin->PinType),
        static_cast<int32>(Pin->Direction),
        MatchingPinOrdinal);
}

FResolvedTarget StableGraph(UBlueprint* Blueprint, const FGuid& Guid)
{
    FResolvedTarget Result;
    if (!Blueprint || !Guid.IsValid())
    {
        return Result;
    }
    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        if (Graph && Graph->GraphGuid == Guid)
        {
            Result.Object = Graph;
            Result.Graph = Graph;
            Result.Id = MakeTargetId(ETargetKind::Graph, Guid);
            Result.IdKind = TEXT("graph_guid");
            Result.bStable = true;
            return Result;
        }
    }
    return Result;
}

FResolvedTarget StableNode(UBlueprint* Blueprint, const FGuid& Guid)
{
    FResolvedTarget Result;
    TArray<UEdGraph*> Graphs;
    if (Blueprint)
    {
        Blueprint->GetAllGraphs(Graphs);
    }
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid == Guid)
            {
                Result.Object = Node;
                Result.Graph = Graph;
                Result.Node = Node;
                Result.Id = MakeTargetId(ETargetKind::Node, Guid);
                Result.IdKind = TEXT("node_guid");
                Result.bStable = true;
                return Result;
            }
        }
    }
    return Result;
}

FResolvedTarget StablePin(UBlueprint* Blueprint, const FGuid& Guid)
{
    FResolvedTarget Result;
    TArray<UEdGraph*> Graphs;
    if (Blueprint)
    {
        Blueprint->GetAllGraphs(Graphs);
    }
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->PinId == Guid)
                {
                    Result.Graph = Graph;
                    Result.Node = Node;
                    Result.Pin = Pin;
                    Result.Id = MakeTargetId(ETargetKind::Pin, Guid);
                    Result.IdKind = TEXT("pin_guid");
                    Result.bStable = true;
                    return Result;
                }
            }
        }
    }
    return Result;
}

FResolvedTarget StableVariable(UBlueprint* Blueprint, const FGuid& Guid)
{
    FResolvedTarget Result;
    if (!Blueprint) return Result;
    for (FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        if (Variable.VarGuid == Guid)
        {
            Result.Variable = &Variable;
            Result.Id = MakeTargetId(ETargetKind::Variable, Guid);
            Result.IdKind = TEXT("variable_guid");
            Result.bStable = true;
            return Result;
        }
    }
    return Result;
}

FResolvedTarget StableComponent(UBlueprint* Blueprint, const FGuid& Guid)
{
    FResolvedTarget Result;
    if (!Blueprint || !Blueprint->SimpleConstructionScript) return Result;
    if (USCS_Node* Component =
        Blueprint->SimpleConstructionScript->FindSCSNodeByGuid(Guid);
        Component && Component->VariableGuid == Guid)
    {
        Result.Object = Component;
        Result.Component = Component;
        Result.Id = MakeTargetId(ETargetKind::Component, Guid);
        Result.IdKind = TEXT("scs_variable_guid");
        Result.bStable = true;
    }
    return Result;
}

FResolvedTarget StableInterface(UBlueprint* Blueprint, const FString& Id)
{
    FResolvedTarget Result;
    if (!Blueprint) return Result;
    for (FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
    {
        UClass* InterfaceClass = Description.Interface.Get();
        if (!InterfaceClass) continue;
        const FString Path = InterfaceClass->GetPathName();
        const FString Expected = FString::Printf(TEXT("interface:%s"), *Path);
        if (Id == Expected)
        {
            Result.Object = InterfaceClass;
            Result.Id = Expected;
            Result.IdKind = TEXT("interface_path");
            Result.bStable = true;
            return Result;
        }
    }
    return Result;
}

TSharedPtr<FJsonValue> EmptyArrayValue()
{
    return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
}

bool InvalidInput(
    FError& OutError,
    const FString& Path,
    const FString& Message,
    const FString& Hint = TEXT("Use the canonical Blueprint type schema."))
{
    OutError.Code = TEXT("INVALID_INPUT");
    OutError.Path = Path;
    OutError.Message = Message;
    OutError.Hint = Hint;
    return false;
}

bool ValidateClosedObject(
    const TSharedRef<FJsonObject>& Object,
    std::initializer_list<const TCHAR*> AllowedFields,
    const FString& Path,
    FError& OutError)
{
    TSet<FString> Allowed;
    for (const TCHAR* Field : AllowedFields)
    {
        Allowed.Add(Field);
    }
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
    {
        if (!Allowed.Contains(Field.Key))
        {
            return InvalidInput(
                OutError,
                Path + TEXT(".") + Field.Key,
                FString::Printf(TEXT("Unknown field '%s'."), *Field.Key));
        }
    }
    return true;
}

bool TryGetRequiredString(
    const TSharedRef<FJsonObject>& Object,
    const TCHAR* Field,
    const FString& Path,
    FString& OutValue,
    FError& OutError)
{
    if (!Object->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
    {
        return InvalidInput(
            OutError,
            Path + TEXT(".") + Field,
            FString::Printf(TEXT("'%s' must be a non-empty string."), Field));
    }
    return true;
}

bool TryGetRequiredObject(
    const TSharedRef<FJsonObject>& Object,
    const TCHAR* Field,
    const FString& Path,
    TSharedRef<FJsonObject>& OutValue,
    FError& OutError)
{
    const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
    if (!Value || !Value->IsValid() || (*Value)->Type != EJson::Object)
    {
        return InvalidInput(
            OutError,
            Path + TEXT(".") + Field,
            FString::Printf(TEXT("'%s' must be a type object."), Field));
    }
    OutValue = (*Value)->AsObject().ToSharedRef();
    return true;
}

bool IsContainerKind(const FString& Kind)
{
    return Kind == TEXT("array") || Kind == TEXT("set") ||
        Kind == TEXT("map");
}

bool TryGetKind(
    const TSharedRef<FJsonObject>& Spec,
    const FString& Path,
    FString& OutKind,
    FError& OutError)
{
    return TryGetRequiredString(
        Spec, TEXT("kind"), Path, OutKind, OutError);
}

bool ValidateContainerDepth(
    const TSharedRef<FJsonObject>& Spec,
    const FString& Path,
    int32 Depth,
    FError& OutError)
{
    FString Kind;
    if (!TryGetKind(Spec, Path, Kind, OutError))
    {
        return false;
    }
    if (!IsContainerKind(Kind))
    {
        return true;
    }
    if (Depth >= 8)
    {
        return InvalidInput(
            OutError,
            Path + TEXT(".kind"),
            TEXT("Container nesting cannot exceed eight levels."));
    }

    if (Kind == TEXT("array") || Kind == TEXT("set"))
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        return TryGetRequiredObject(
                Spec, TEXT("item"), Path, Item, OutError) &&
            ValidateContainerDepth(
                Item, Path + TEXT(".item"), Depth + 1, OutError);
    }

    TSharedRef<FJsonObject> Key = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
    return TryGetRequiredObject(Spec, TEXT("key"), Path, Key, OutError) &&
        TryGetRequiredObject(Spec, TEXT("value"), Path, Value, OutError) &&
        ValidateContainerDepth(
            Key, Path + TEXT(".key"), Depth + 1, OutError) &&
        ValidateContainerDepth(
            Value, Path + TEXT(".value"), Depth + 1, OutError);
}

UObject* FindOrLoadTypeObject(const FString& ObjectPath)
{
    if (UObject* Existing = StaticFindObject(
        UObject::StaticClass(), nullptr, *ObjectPath))
    {
        return Existing;
    }
    return LoadObject<UObject>(nullptr, *ObjectPath);
}

TSharedRef<FJsonObject> MakeKindObject(const TCHAR* Kind)
{
    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("kind"), Kind);
    return Result;
}

TSharedRef<FJsonObject> SerializeScalarType(const FEdGraphPinType& Type)
{
    const FName Category = Type.PinCategory;
    if (Category == UEdGraphSchema_K2::PC_Exec)
    {
        return MakeKindObject(TEXT("exec"));
    }
    if (Category == UEdGraphSchema_K2::PC_Boolean)
    {
        return MakeKindObject(TEXT("bool"));
    }
    if (Category == UEdGraphSchema_K2::PC_Byte)
    {
        if (Cast<UEnum>(Type.PinSubCategoryObject.Get()))
        {
            const TSharedRef<FJsonObject> Result = MakeKindObject(TEXT("enum"));
            Result->SetStringField(
                TEXT("type_path"),
                GetPathNameSafe(Type.PinSubCategoryObject.Get()));
            return Result;
        }
        return MakeKindObject(TEXT("byte"));
    }
    if (Category == UEdGraphSchema_K2::PC_Int)
    {
        return MakeKindObject(TEXT("int"));
    }
    if (Category == UEdGraphSchema_K2::PC_Int64)
    {
        return MakeKindObject(TEXT("int64"));
    }
    if (Category == UEdGraphSchema_K2::PC_Real)
    {
        const TSharedRef<FJsonObject> Result = MakeKindObject(TEXT("real"));
        Result->SetStringField(
            TEXT("precision"),
            Type.PinSubCategory == UEdGraphSchema_K2::PC_Float
                ? TEXT("float")
                : TEXT("double"));
        return Result;
    }
    if (Category == UEdGraphSchema_K2::PC_String)
    {
        return MakeKindObject(TEXT("string"));
    }
    if (Category == UEdGraphSchema_K2::PC_Name)
    {
        return MakeKindObject(TEXT("name"));
    }
    if (Category == UEdGraphSchema_K2::PC_Text)
    {
        return MakeKindObject(TEXT("text"));
    }

    const TCHAR* Kind = TEXT("unknown");
    const TCHAR* PathField = TEXT("class_path");
    if (Category == UEdGraphSchema_K2::PC_Enum)
    {
        Kind = TEXT("enum");
        PathField = TEXT("type_path");
    }
    else if (Category == UEdGraphSchema_K2::PC_Struct)
    {
        Kind = TEXT("struct");
        PathField = TEXT("type_path");
    }
    else if (Category == UEdGraphSchema_K2::PC_Object)
    {
        Kind = TEXT("object");
    }
    else if (Category == UEdGraphSchema_K2::PC_Class)
    {
        Kind = TEXT("class");
    }
    else if (Category == UEdGraphSchema_K2::PC_Interface)
    {
        Kind = TEXT("interface");
    }
    else if (Category == UEdGraphSchema_K2::PC_SoftObject)
    {
        Kind = TEXT("soft_object");
    }
    else if (Category == UEdGraphSchema_K2::PC_SoftClass)
    {
        Kind = TEXT("soft_class");
    }

    const TSharedRef<FJsonObject> Result = MakeKindObject(Kind);
    if (FCString::Strcmp(Kind, TEXT("unknown")) != 0)
    {
        Result->SetStringField(
            PathField,
            GetPathNameSafe(Type.PinSubCategoryObject.Get()));
    }
    return Result;
}

FString QuoteImportString(const FString& Value)
{
    FString Escaped = Value.Replace(TEXT("\\"), TEXT("\\\\"));
    Escaped = Escaped.Replace(TEXT("\""), TEXT("\\\""));
    Escaped = Escaped.Replace(TEXT("\n"), TEXT("\\n"));
    Escaped = Escaped.Replace(TEXT("\r"), TEXT("\\r"));
    Escaped = Escaped.Replace(TEXT("\t"), TEXT("\\t"));
    return TEXT("\"") + Escaped + TEXT("\"");
}

bool LosslessIntegerText(
    const TSharedPtr<FJsonValue>& Value,
    double Minimum,
    double Maximum,
    const FString& Path,
    FString& OutText,
    FError& OutError)
{
    if (!Value.IsValid() || Value->Type != EJson::Number)
    {
        return InvalidInput(
            OutError, Path, TEXT("Expected a JSON integer."));
    }
    const double Number = Value->AsNumber();
    if (!FMath::IsFinite(Number) || FMath::TruncToDouble(Number) != Number ||
        Number < Minimum || Number > Maximum)
    {
        return InvalidInput(
            OutError,
            Path,
            TEXT("Integer default cannot be represented losslessly."));
    }
    OutText = FString::Printf(TEXT("%.0f"), Number);
    return true;
}

bool JsonToImportText(
    const FEdGraphPinType& Type,
    const TSharedPtr<FJsonValue>& Value,
    const FString& Path,
    bool bNested,
    FString& OutText,
    FError& OutError)
{
    const FName Category = Type.PinCategory;
    if (!Value.IsValid())
    {
        return InvalidInput(OutError, Path, TEXT("Default value is missing."));
    }

    if (Value->IsNull())
    {
        if (Category == UEdGraphSchema_K2::PC_Object ||
            Category == UEdGraphSchema_K2::PC_Class ||
            Category == UEdGraphSchema_K2::PC_Interface ||
            Category == UEdGraphSchema_K2::PC_SoftObject ||
            Category == UEdGraphSchema_K2::PC_SoftClass)
        {
            const bool bSoftReference =
                Category == UEdGraphSchema_K2::PC_SoftObject ||
                Category == UEdGraphSchema_K2::PC_SoftClass;
            OutText = bSoftReference && !bNested ? TEXT("") : TEXT("None");
            return true;
        }
        return InvalidInput(
            OutError,
            Path,
            TEXT("Null is only valid for object and class references."));
    }

    if (Type.ContainerType == EPinContainerType::Array ||
        Type.ContainerType == EPinContainerType::Set)
    {
        if (Value->Type != EJson::Array)
        {
            return InvalidInput(
                OutError, Path, TEXT("Array and set defaults must be JSON arrays."));
        }
        FEdGraphPinType ItemType = Type;
        ItemType.ContainerType = EPinContainerType::None;
        TArray<FString> Items;
        const TArray<TSharedPtr<FJsonValue>>& JsonItems = Value->AsArray();
        for (int32 Index = 0; Index < JsonItems.Num(); ++Index)
        {
            FString ItemText;
            if (!JsonToImportText(
                ItemType,
                JsonItems[Index],
                FString::Printf(TEXT("%s[%d]"), *Path, Index),
                true,
                ItemText,
                OutError))
            {
                return false;
            }
            Items.Add(MoveTemp(ItemText));
        }
        OutText = TEXT("(") + FString::Join(Items, TEXT(",")) + TEXT(")");
        return true;
    }

    if (Type.ContainerType == EPinContainerType::Map)
    {
        if (Value->Type != EJson::Array)
        {
            return InvalidInput(
                OutError,
                Path,
                TEXT("Map defaults must be an ordered JSON array of key/value objects."));
        }
        FEdGraphPinType KeyType = Type;
        KeyType.ContainerType = EPinContainerType::None;
        const FEdGraphPinType ValueType =
            FEdGraphPinType::GetPinTypeForTerminalType(Type.PinValueType);
        TArray<FString> Entries;
        const TArray<TSharedPtr<FJsonValue>>& JsonEntries = Value->AsArray();
        for (int32 Index = 0; Index < JsonEntries.Num(); ++Index)
        {
            const FString EntryPath =
                FString::Printf(TEXT("%s[%d]"), *Path, Index);
            if (!JsonEntries[Index].IsValid() ||
                JsonEntries[Index]->Type != EJson::Object)
            {
                return InvalidInput(
                    OutError,
                    EntryPath,
                    TEXT("Map entries must be key/value objects."));
            }
            const TSharedPtr<FJsonObject> Entry = JsonEntries[Index]->AsObject();
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
                Entry->Values)
            {
                if (Field.Key != TEXT("key") && Field.Key != TEXT("value"))
                {
                    return InvalidInput(
                        OutError,
                        EntryPath + TEXT(".") + Field.Key,
                        TEXT("Map entries only allow key and value."));
                }
            }
            const TSharedPtr<FJsonValue>* KeyValue =
                Entry->Values.Find(TEXT("key"));
            const TSharedPtr<FJsonValue>* MappedValue =
                Entry->Values.Find(TEXT("value"));
            if (!KeyValue)
            {
                return InvalidInput(
                    OutError,
                    EntryPath + TEXT(".key"),
                    TEXT("Map entry key is required."));
            }
            if (!MappedValue)
            {
                return InvalidInput(
                    OutError,
                    EntryPath + TEXT(".value"),
                    TEXT("Map entry value is required."));
            }
            FString KeyText;
            FString ValueText;
            if (!JsonToImportText(
                    KeyType,
                    *KeyValue,
                    EntryPath + TEXT(".key"),
                    true,
                    KeyText,
                    OutError) ||
                !JsonToImportText(
                    ValueType,
                    *MappedValue,
                    EntryPath + TEXT(".value"),
                    true,
                    ValueText,
                    OutError))
            {
                return false;
            }
            Entries.Add(TEXT("(") + KeyText + TEXT(",") +
                ValueText + TEXT(")"));
        }
        OutText = TEXT("(") + FString::Join(Entries, TEXT(",")) + TEXT(")");
        return true;
    }

    if (Category == UEdGraphSchema_K2::PC_Boolean)
    {
        if (Value->Type != EJson::Boolean)
        {
            return InvalidInput(OutError, Path, TEXT("Expected a JSON boolean."));
        }
        OutText = Value->AsBool() ? TEXT("true") : TEXT("false");
        return true;
    }

    if (Category == UEdGraphSchema_K2::PC_Byte)
    {
        if (const UEnum* Enum = Cast<UEnum>(Type.PinSubCategoryObject.Get()))
        {
            if (Value->Type != EJson::String ||
                Enum->GetIndexByNameString(Value->AsString()) == INDEX_NONE)
            {
                return InvalidInput(
                    OutError, Path, TEXT("Expected a valid enum name."));
            }
            OutText = Value->AsString();
            return true;
        }
        return LosslessIntegerText(
            Value, 0.0, 255.0, Path, OutText, OutError);
    }

    if (Category == UEdGraphSchema_K2::PC_Int)
    {
        return LosslessIntegerText(
            Value,
            static_cast<double>(MIN_int32),
            static_cast<double>(MAX_int32),
            Path,
            OutText,
            OutError);
    }

    if (Category == UEdGraphSchema_K2::PC_Int64)
    {
        constexpr double MaxLosslessJsonInteger = 9007199254740991.0;
        return LosslessIntegerText(
            Value,
            -MaxLosslessJsonInteger,
            MaxLosslessJsonInteger,
            Path,
            OutText,
            OutError);
    }

    if (Category == UEdGraphSchema_K2::PC_Real)
    {
        if (Value->Type != EJson::Number ||
            !FMath::IsFinite(Value->AsNumber()))
        {
            return InvalidInput(OutError, Path, TEXT("Expected a finite JSON number."));
        }
        const double Number = Value->AsNumber();
        if (Type.PinSubCategory == UEdGraphSchema_K2::PC_Float &&
            FMath::Abs(Number) > static_cast<double>(MAX_flt))
        {
            return InvalidInput(
                OutError, Path, TEXT("Number is outside the float range."));
        }
        if (Type.PinSubCategory == UEdGraphSchema_K2::PC_Float &&
            Number != 0.0 && static_cast<float>(Number) == 0.0f)
        {
            return InvalidInput(
                OutError,
                Path,
                TEXT("Number underflows the float range and would become zero."));
        }
        OutText = Type.PinSubCategory == UEdGraphSchema_K2::PC_Float
            ? FString::SanitizeFloat(static_cast<float>(Number), 0)
            : FString::SanitizeFloat(Number, 0);
        return true;
    }

    if (Category == UEdGraphSchema_K2::PC_String ||
        Category == UEdGraphSchema_K2::PC_Name ||
        Category == UEdGraphSchema_K2::PC_Text)
    {
        if (Value->Type != EJson::String)
        {
            return InvalidInput(OutError, Path, TEXT("Expected a JSON string."));
        }
        OutText = bNested ? QuoteImportString(Value->AsString()) : Value->AsString();
        return true;
    }

    if (Category == UEdGraphSchema_K2::PC_Enum)
    {
        const UEnum* Enum = Cast<UEnum>(Type.PinSubCategoryObject.Get());
        if (!Enum || Value->Type != EJson::String ||
            Enum->GetIndexByNameString(Value->AsString()) == INDEX_NONE)
        {
            return InvalidInput(
                OutError, Path, TEXT("Expected a valid enum name."));
        }
        OutText = Value->AsString();
        return true;
    }

    if (Category == UEdGraphSchema_K2::PC_Struct)
    {
        if (Value->Type != EJson::Object)
        {
            return InvalidInput(
                OutError, Path, TEXT("Struct defaults must be JSON objects."));
        }
        const UScriptStruct* Struct =
            Cast<UScriptStruct>(Type.PinSubCategoryObject.Get());
        if (!Struct)
        {
            return InvalidInput(
                OutError, Path, TEXT("Blueprint struct type is unresolved."));
        }
        const TSharedPtr<FJsonObject> JsonObject = Value->AsObject();
        TMap<FString, const FProperty*> Properties;
        for (TFieldIterator<FProperty> It(
            Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            Properties.Add(It->GetName(), *It);
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
            JsonObject->Values)
        {
            if (!Properties.Contains(Field.Key))
            {
                return InvalidInput(
                    OutError,
                    Path + TEXT(".") + Field.Key,
                    FString::Printf(
                        TEXT("Unknown field '%s' for struct %s."),
                        *Field.Key,
                        *Struct->GetName()));
            }
        }
        TArray<FString> Fields;
        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        for (TFieldIterator<FProperty> It(
            Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            const FProperty* Property = *It;
            const FString FieldName = Property->GetName();
            const TSharedPtr<FJsonValue>* FieldValue =
                JsonObject->Values.Find(FieldName);
            if (!FieldValue)
            {
                continue;
            }
            FEdGraphPinType FieldType;
            if (!Schema->ConvertPropertyToPinType(Property, FieldType))
            {
                return InvalidInput(
                    OutError,
                    Path + TEXT(".") + FieldName,
                    TEXT("Struct field type is not supported by Blueprint."));
            }
            FString FieldText;
            if (!JsonToImportText(
                FieldType,
                *FieldValue,
                Path + TEXT(".") + FieldName,
                true,
                FieldText,
                OutError))
            {
                return false;
            }
            Fields.Add(FieldName + TEXT("=") + FieldText);
        }
        OutText = TEXT("(") + FString::Join(Fields, TEXT(",")) + TEXT(")");
        return true;
    }

    if (Category == UEdGraphSchema_K2::PC_Object ||
        Category == UEdGraphSchema_K2::PC_Class ||
        Category == UEdGraphSchema_K2::PC_Interface ||
        Category == UEdGraphSchema_K2::PC_SoftObject ||
        Category == UEdGraphSchema_K2::PC_SoftClass)
    {
        if (Value->Type != EJson::String || Value->AsString().IsEmpty())
        {
            return InvalidInput(
                OutError, Path, TEXT("Expected a full Unreal object path."));
        }
        const FString ObjectPath = Value->AsString();
        FText PathReason;
        if (!FPackageName::IsValidObjectPath(ObjectPath, &PathReason))
        {
            return InvalidInput(
                OutError,
                Path,
                FString::Printf(
                    TEXT("Invalid Unreal object path: %s"),
                    *PathReason.ToString()));
        }
        OutText = ObjectPath;
        return true;
    }

    return InvalidInput(
        OutError, Path, TEXT("Blueprint default type is not supported."));
}

bool ValidateScalarDefaultWithSchema(
    const FEdGraphPinType& Type,
    const TSharedPtr<FJsonValue>& JsonValue,
    UObject* Owner,
    const FString& Path,
    FError& OutError)
{
    FString ImportText;
    const bool bNullSoftReference = JsonValue->IsNull() &&
        (Type.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
         Type.PinCategory == UEdGraphSchema_K2::PC_SoftClass);
    if (!bNullSoftReference &&
        !JsonToImportText(
            Type, JsonValue, Path, true, ImportText, OutError))
    {
        return false;
    }

    FString DefaultValue;
    TObjectPtr<UObject> DefaultObject = nullptr;
    FText DefaultTextValue;
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    Schema->GetPinDefaultValuesFromString(
        Type,
        Owner,
        ImportText,
        DefaultValue,
        DefaultObject,
        DefaultTextValue);

    const bool bHardReference =
        Type.PinCategory == UEdGraphSchema_K2::PC_Object ||
        Type.PinCategory == UEdGraphSchema_K2::PC_Class ||
        Type.PinCategory == UEdGraphSchema_K2::PC_Interface;
    if (bHardReference && !JsonValue->IsNull() && !DefaultObject)
    {
        return InvalidInput(
            OutError, Path, TEXT("Unreal object path could not be resolved."));
    }

    FEdGraphPinType ValidationType = Type;
    if (ValidationType.PinCategory == UEdGraphSchema_K2::PC_Enum)
    {
        ValidationType.PinCategory = UEdGraphSchema_K2::PC_Byte;
    }
    FString ValidationMessage;
    if (!Schema->DefaultValueSimpleValidation(
            ValidationType,
            NAME_None,
            DefaultValue,
            DefaultObject,
            DefaultTextValue,
            &ValidationMessage))
    {
        return InvalidInput(OutError, Path, ValidationMessage);
    }
    return true;
}

bool ValidateDefaultRecursively(
    const FEdGraphPinType& Type,
    const TSharedPtr<FJsonValue>& JsonValue,
    UObject* Owner,
    const FString& Path,
    FError& OutError)
{
    if (Type.ContainerType == EPinContainerType::Array ||
        Type.ContainerType == EPinContainerType::Set)
    {
        FEdGraphPinType ItemType = Type;
        ItemType.ContainerType = EPinContainerType::None;
        const TArray<TSharedPtr<FJsonValue>>& Items = JsonValue->AsArray();
        for (int32 Index = 0; Index < Items.Num(); ++Index)
        {
            if (!ValidateDefaultRecursively(
                    ItemType,
                    Items[Index],
                    Owner,
                    FString::Printf(TEXT("%s[%d]"), *Path, Index),
                    OutError))
            {
                return false;
            }
        }
        return true;
    }

    if (Type.ContainerType == EPinContainerType::Map)
    {
        FEdGraphPinType KeyType = Type;
        KeyType.ContainerType = EPinContainerType::None;
        const FEdGraphPinType ValueType =
            FEdGraphPinType::GetPinTypeForTerminalType(Type.PinValueType);
        const TArray<TSharedPtr<FJsonValue>>& Entries = JsonValue->AsArray();
        for (int32 Index = 0; Index < Entries.Num(); ++Index)
        {
            const FString EntryPath =
                FString::Printf(TEXT("%s[%d]"), *Path, Index);
            const TSharedPtr<FJsonObject> Entry = Entries[Index]->AsObject();
            if (!ValidateDefaultRecursively(
                    KeyType,
                    Entry->Values[TEXT("key")],
                    Owner,
                    EntryPath + TEXT(".key"),
                    OutError) ||
                !ValidateDefaultRecursively(
                    ValueType,
                    Entry->Values[TEXT("value")],
                    Owner,
                    EntryPath + TEXT(".value"),
                    OutError))
            {
                return false;
            }
        }
        return true;
    }

    if (Type.PinCategory == UEdGraphSchema_K2::PC_Struct)
    {
        const UScriptStruct* Struct =
            CastChecked<UScriptStruct>(Type.PinSubCategoryObject.Get());
        const TSharedPtr<FJsonObject> Object = JsonValue->AsObject();
        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        for (TFieldIterator<FProperty> It(
            Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            const FString FieldName = It->GetName();
            const TSharedPtr<FJsonValue>* FieldValue =
                Object->Values.Find(FieldName);
            if (!FieldValue)
            {
                continue;
            }
            FEdGraphPinType FieldType;
            if (!Schema->ConvertPropertyToPinType(*It, FieldType))
            {
                return InvalidInput(
                    OutError,
                    Path + TEXT(".") + FieldName,
                    TEXT("Struct field type is not supported by Blueprint."));
            }
            if (!ValidateDefaultRecursively(
                    FieldType,
                    *FieldValue,
                    Owner,
                    Path + TEXT(".") + FieldName,
                    OutError))
            {
                return false;
            }
        }
    }

    return ValidateScalarDefaultWithSchema(
        Type, JsonValue, Owner, Path, OutError);
}

FString UnquoteImportString(const FString& Value)
{
    const FString Trimmed = Value.TrimStartAndEnd();
    if (Trimmed.Len() < 2 || Trimmed[0] != TEXT('"') ||
        Trimmed[Trimmed.Len() - 1] != TEXT('"'))
    {
        return Trimmed;
    }

    FString Result;
    Result.Reserve(Trimmed.Len() - 2);
    bool bEscaped = false;
    for (int32 Index = 1; Index < Trimmed.Len() - 1; ++Index)
    {
        const TCHAR Character = Trimmed[Index];
        if (bEscaped)
        {
            switch (Character)
            {
            case TEXT('n'): Result.AppendChar(TEXT('\n')); break;
            case TEXT('r'): Result.AppendChar(TEXT('\r')); break;
            case TEXT('t'): Result.AppendChar(TEXT('\t')); break;
            default: Result.AppendChar(Character); break;
            }
            bEscaped = false;
        }
        else if (Character == TEXT('\\'))
        {
            bEscaped = true;
        }
        else
        {
            Result.AppendChar(Character);
        }
    }
    if (bEscaped)
    {
        Result.AppendChar(TEXT('\\'));
    }
    return Result;
}

TArray<FString> SplitTopLevelImportText(
    const FString& Value,
    const TCHAR Delimiter)
{
    TArray<FString> Parts;
    int32 Depth = 0;
    int32 Start = 0;
    bool bInQuotes = false;
    bool bEscaped = false;
    for (int32 Index = 0; Index < Value.Len(); ++Index)
    {
        const TCHAR Character = Value[Index];
        if (bInQuotes)
        {
            if (bEscaped)
            {
                bEscaped = false;
            }
            else if (Character == TEXT('\\'))
            {
                bEscaped = true;
            }
            else if (Character == TEXT('"'))
            {
                bInQuotes = false;
            }
            continue;
        }
        if (Character == TEXT('"'))
        {
            bInQuotes = true;
        }
        else if (Character == TEXT('('))
        {
            ++Depth;
        }
        else if (Character == TEXT(')'))
        {
            --Depth;
        }
        else if (Character == Delimiter && Depth == 0)
        {
            Parts.Add(Value.Mid(Start, Index - Start).TrimStartAndEnd());
            Start = Index + 1;
        }
    }
    if (Start < Value.Len())
    {
        Parts.Add(Value.Mid(Start).TrimStartAndEnd());
    }
    else if (Start == 0 && Value.IsEmpty())
    {
        return Parts;
    }
    return Parts;
}

bool StripOuterImportParentheses(
    const FString& Value,
    FString& OutInner)
{
    const FString Trimmed = Value.TrimStartAndEnd();
    if (Trimmed.Len() < 2 || Trimmed[0] != TEXT('(') ||
        Trimmed[Trimmed.Len() - 1] != TEXT(')'))
    {
        return false;
    }
    OutInner = Trimmed.Mid(1, Trimmed.Len() - 2);
    return true;
}

TSharedPtr<FJsonValue> ImportTextToJson(
    const FEdGraphPinType& Type,
    const FString& DefaultValue,
    UObject* DefaultObject,
    const FText& DefaultTextValue,
    const bool bNested)
{
    if (Type.ContainerType == EPinContainerType::Array ||
        Type.ContainerType == EPinContainerType::Set)
    {
        FString Inner;
        if (!StripOuterImportParentheses(DefaultValue, Inner))
        {
            return nullptr;
        }
        FEdGraphPinType ItemType = Type;
        ItemType.ContainerType = EPinContainerType::None;
        TArray<TSharedPtr<FJsonValue>> Items;
        for (const FString& Item : SplitTopLevelImportText(Inner, TEXT(',')))
        {
            const TSharedPtr<FJsonValue> Json = ImportTextToJson(
                ItemType, Item, nullptr, FText::GetEmpty(), true);
            if (!Json.IsValid())
            {
                return nullptr;
            }
            Items.Add(Json);
        }
        return MakeShared<FJsonValueArray>(Items);
    }

    if (Type.ContainerType == EPinContainerType::Map)
    {
        FString Inner;
        if (!StripOuterImportParentheses(DefaultValue, Inner))
        {
            return nullptr;
        }
        FEdGraphPinType KeyType = Type;
        KeyType.ContainerType = EPinContainerType::None;
        const FEdGraphPinType ValueType =
            FEdGraphPinType::GetPinTypeForTerminalType(Type.PinValueType);
        TArray<TSharedPtr<FJsonValue>> Entries;
        for (const FString& EntryText :
             SplitTopLevelImportText(Inner, TEXT(',')))
        {
            FString EntryInner;
            if (!StripOuterImportParentheses(EntryText, EntryInner))
            {
                return nullptr;
            }
            const TArray<FString> Pair =
                SplitTopLevelImportText(EntryInner, TEXT(','));
            if (Pair.Num() != 2)
            {
                return nullptr;
            }
            const TSharedPtr<FJsonValue> Key = ImportTextToJson(
                KeyType, Pair[0], nullptr, FText::GetEmpty(), true);
            const TSharedPtr<FJsonValue> Value = ImportTextToJson(
                ValueType, Pair[1], nullptr, FText::GetEmpty(), true);
            if (!Key.IsValid() || !Value.IsValid())
            {
                return nullptr;
            }
            const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetField(TEXT("key"), Key);
            Entry->SetField(TEXT("value"), Value);
            Entries.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return MakeShared<FJsonValueArray>(Entries);
    }

    const FString Text = DefaultValue.TrimStartAndEnd();
    const FName Category = Type.PinCategory;
    if (Category == UEdGraphSchema_K2::PC_Boolean)
    {
        return MakeShared<FJsonValueBoolean>(Text.Equals(
            TEXT("true"), ESearchCase::IgnoreCase));
    }
    if (Category == UEdGraphSchema_K2::PC_Byte ||
        Category == UEdGraphSchema_K2::PC_Enum)
    {
        if (Type.PinSubCategoryObject.IsValid())
        {
            return MakeShared<FJsonValueString>(UnquoteImportString(Text));
        }
        return MakeShared<FJsonValueNumber>(FCString::Atod(*Text));
    }
    if (Category == UEdGraphSchema_K2::PC_Int ||
        Category == UEdGraphSchema_K2::PC_Int64 ||
        Category == UEdGraphSchema_K2::PC_Real)
    {
        return MakeShared<FJsonValueNumber>(FCString::Atod(*Text));
    }
    if (Category == UEdGraphSchema_K2::PC_String ||
        Category == UEdGraphSchema_K2::PC_Name)
    {
        return MakeShared<FJsonValueString>(UnquoteImportString(Text));
    }
    if (Category == UEdGraphSchema_K2::PC_Text)
    {
        const FString Display = !DefaultTextValue.IsEmpty() && !bNested
            ? DefaultTextValue.ToString()
            : UnquoteImportString(Text);
        return MakeShared<FJsonValueString>(Display);
    }
    if (Category == UEdGraphSchema_K2::PC_Struct)
    {
        FString Inner;
        const UScriptStruct* Struct =
            Cast<UScriptStruct>(Type.PinSubCategoryObject.Get());
        if (!Struct || !StripOuterImportParentheses(Text, Inner))
        {
            return nullptr;
        }
        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        for (const FString& FieldText :
             SplitTopLevelImportText(Inner, TEXT(',')))
        {
            const TArray<FString> Pair =
                SplitTopLevelImportText(FieldText, TEXT('='));
            if (Pair.Num() != 2)
            {
                return nullptr;
            }
            const FProperty* Property =
                FindFProperty<FProperty>(Struct, FName(*Pair[0]));
            FEdGraphPinType FieldType;
            if (!Property || !Schema->ConvertPropertyToPinType(
                    Property, FieldType))
            {
                return nullptr;
            }
            const TSharedPtr<FJsonValue> FieldValue = ImportTextToJson(
                FieldType, Pair[1], nullptr, FText::GetEmpty(), true);
            if (!FieldValue.IsValid())
            {
                return nullptr;
            }
            Object->SetField(Pair[0], FieldValue);
        }
        return MakeShared<FJsonValueObject>(Object);
    }
    if (Category == UEdGraphSchema_K2::PC_Object ||
        Category == UEdGraphSchema_K2::PC_Class ||
        Category == UEdGraphSchema_K2::PC_Interface ||
        Category == UEdGraphSchema_K2::PC_SoftObject ||
        Category == UEdGraphSchema_K2::PC_SoftClass)
    {
        if (DefaultObject && !bNested)
        {
            return MakeShared<FJsonValueString>(DefaultObject->GetPathName());
        }
        const FString Path = UnquoteImportString(Text);
        if (Path.IsEmpty() || Path == TEXT("None"))
        {
            return MakeShared<FJsonValueNull>();
        }
        return MakeShared<FJsonValueString>(Path);
    }
    return nullptr;
}
}

FString MakeTargetId(ETargetKind Kind, const FGuid& Guid)
{
    return FString::Printf(
        TEXT("%s:%s"),
        PrefixForKind(Kind),
        *Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
}

FString MakeQualifiedFallbackId(
    ETargetKind Kind,
    const FString& Owner,
    const FString& Name,
    const FString& TypePath)
{
    return FString::Printf(
        TEXT("fallback:%s:%s"),
        PrefixForKind(Kind),
        *Sha1(Owner + TEXT("\n") + Name + TEXT("\n") + TypePath));
}

FTargetRef DescribeGraphTarget(UBlueprint* Blueprint, const UEdGraph* Graph)
{
    FTargetRef Result;
    if (!Graph)
    {
        return Result;
    }
    Result.OwnerId = Graph->GetOuter()
        ? Graph->GetOuter()->GetPathName()
        : (Blueprint ? Blueprint->GetPathName() : FString());
    Result.Name = Graph->GetName();
    Result.TypePath = TargetTypePath(Graph);
    if (Graph->GraphGuid.IsValid())
    {
        Result.Id = MakeTargetId(ETargetKind::Graph, Graph->GraphGuid);
        return Result;
    }
    Result.Id = MakeQualifiedFallbackId(
        ETargetKind::Graph,
        Result.OwnerId,
        Result.Name,
        Result.TypePath);
    return Result;
}

FTargetRef DescribeNodeTarget(UBlueprint* Blueprint, const UEdGraphNode* Node)
{
    FTargetRef Result;
    if (!Node)
    {
        return Result;
    }
    Result.OwnerId = MakeGraphTargetId(Blueprint, Node->GetGraph());
    Result.Name = Node->GetName();
    Result.TypePath = TargetTypePath(Node);
    if (Node->NodeGuid.IsValid())
    {
        Result.Id = MakeTargetId(ETargetKind::Node, Node->NodeGuid);
        return Result;
    }
    Result.Id = MakeQualifiedFallbackId(
        ETargetKind::Node,
        Result.OwnerId,
        Result.Name,
        Result.TypePath);
    return Result;
}

FTargetRef DescribePinTarget(UBlueprint* Blueprint, const UEdGraphPin* Pin)
{
    FTargetRef Result;
    if (!Pin)
    {
        return Result;
    }
    const UEdGraphNode* OwningNode = Pin->GetOwningNode();
    Result.OwnerId = MakeNodeTargetId(Blueprint, OwningNode);
    Result.Name = Pin->GetName();
    Result.TypePath = PinQualificationType(Pin);
    if (Pin->PinId.IsValid())
    {
        Result.Id = MakeTargetId(ETargetKind::Pin, Pin->PinId);
        return Result;
    }
    Result.Id = MakeQualifiedFallbackId(
        ETargetKind::Pin,
        Result.OwnerId,
        Result.Name,
        Result.TypePath);
    return Result;
}

FTargetRef DescribeVariableTarget(
    UBlueprint* Blueprint,
    const FBPVariableDescription& Variable)
{
    FTargetRef Result;
    Result.OwnerId = Blueprint ? Blueprint->GetPathName() : FString();
    Result.Name = Variable.VarName.ToString();
    Result.TypePath = CanonicalPinType(Variable.VarType);
    Result.Id = Variable.VarGuid.IsValid()
        ? MakeTargetId(ETargetKind::Variable, Variable.VarGuid)
        : MakeQualifiedFallbackId(
            ETargetKind::Variable,
            Result.OwnerId,
            Result.Name,
            Result.TypePath);
    return Result;
}

FTargetRef DescribeComponentTarget(
    UBlueprint* Blueprint,
    const USCS_Node* Component)
{
    FTargetRef Result;
    if (!Component)
    {
        return Result;
    }
    Result.OwnerId = Blueprint ? Blueprint->GetPathName() : FString();
    Result.Name = Component->GetVariableName().ToString();
    const UClass* ComponentClass = Component->ComponentClass
        ? Component->ComponentClass.Get()
        : (Component->ComponentTemplate
            ? Component->ComponentTemplate->GetClass()
            : nullptr);
    Result.TypePath = ComponentClass ? ComponentClass->GetPathName() : FString();
    Result.Id = Component->VariableGuid.IsValid()
        ? MakeTargetId(ETargetKind::Component, Component->VariableGuid)
        : MakeQualifiedFallbackId(
            ETargetKind::Component,
            Result.OwnerId,
            Result.Name,
            Result.TypePath);
    return Result;
}

FString MakeGraphTargetId(UBlueprint* Blueprint, const UEdGraph* Graph)
{
    return DescribeGraphTarget(Blueprint, Graph).Id;
}

FString MakeNodeTargetId(UBlueprint* Blueprint, const UEdGraphNode* Node)
{
    return DescribeNodeTarget(Blueprint, Node).Id;
}

FString MakePinTargetId(UBlueprint* Blueprint, const UEdGraphPin* Pin)
{
    return DescribePinTarget(Blueprint, Pin).Id;
}

bool ParseTargetId(
    const FString& Id,
    ETargetKind ExpectedKind,
    FGuid& OutGuid)
{
    FString Prefix;
    FString Value;
    if (!Id.Split(TEXT(":"), &Prefix, &Value) ||
        Prefix != PrefixForKind(ExpectedKind))
    {
        return false;
    }
    return FGuid::Parse(Value, OutGuid) &&
        OutGuid.IsValid() &&
        Value.Equals(
            OutGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
            ESearchCase::CaseSensitive);
}

FResolvedTarget ResolveTarget(
    UBlueprint* Blueprint,
    ETargetKind Kind,
    const FTargetRef& Target,
    FString& OutError)
{
    OutError.Empty();
    if (!Blueprint)
    {
        OutError = TEXT("Blueprint is required.");
        return {};
    }

    if (Kind == ETargetKind::Interface)
    {
        FResolvedTarget Interface = StableInterface(Blueprint, Target.Id);
        if (Interface.bStable)
        {
            return Interface;
        }
        if (Target.Id.StartsWith(TEXT("interface:")))
        {
            OutError = TEXT("Stable target no longer exists.");
            return {};
        }
    }

    FGuid Guid;
    if (ParseTargetId(Target.Id, Kind, Guid))
    {
        FResolvedTarget Stable;
        switch (Kind)
        {
        case ETargetKind::Graph: Stable = StableGraph(Blueprint, Guid); break;
        case ETargetKind::Node: Stable = StableNode(Blueprint, Guid); break;
        case ETargetKind::Pin: Stable = StablePin(Blueprint, Guid); break;
        case ETargetKind::Variable: Stable = StableVariable(Blueprint, Guid); break;
        case ETargetKind::Component: Stable = StableComponent(Blueprint, Guid); break;
        case ETargetKind::Interface: Stable = StableInterface(Blueprint, Target.Id); break;
        }
        if (Stable.bStable)
        {
            if (Kind == ETargetKind::Node && !Target.OwnerId.IsEmpty() &&
                Stable.Graph && Target.OwnerId !=
                    DescribeNodeTarget(Blueprint, Stable.Node).OwnerId)
            {
                OutError = TEXT("Node does not belong to the requested graph.");
                return {};
            }
            if (Kind == ETargetKind::Pin && !Target.OwnerId.IsEmpty() &&
                Stable.Node && Target.OwnerId !=
                    DescribePinTarget(Blueprint, Stable.Pin).OwnerId)
            {
                OutError = TEXT("Pin does not belong to the requested node.");
                return {};
            }
            return Stable;
        }
        OutError = TEXT("Stable target no longer exists.");
        return {};
    }

    if (!Target.bAllowNameFallback || Target.OwnerId.IsEmpty() ||
        Target.Name.IsEmpty() ||
        Target.TypePath.IsEmpty())
    {
        OutError = TEXT("A stable target id is required.");
        return {};
    }

    auto IsExactFallback = [&Target](const FTargetRef& Candidate)
    {
        return Candidate.Id == Target.Id &&
            Candidate.OwnerId == Target.OwnerId &&
            Candidate.Name == Target.Name &&
            Candidate.TypePath == Target.TypePath &&
            Candidate.Id.StartsWith(TEXT("fallback:"));
    };

    TArray<FResolvedTarget> Matches;
    if (Kind == ETargetKind::Graph)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            const FTargetRef Candidate = DescribeGraphTarget(Blueprint, Graph);
            if (Graph && IsExactFallback(Candidate))
            {
                FResolvedTarget Match;
                Match.Object = Graph;
                Match.Graph = Graph;
                Match.Id = Candidate.Id;
                Match.IdKind = TEXT("qualified_name_fallback");
                Matches.Add(Match);
            }
        }
    }
    else if (Kind == ETargetKind::Node)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                const FTargetRef Candidate = DescribeNodeTarget(Blueprint, Node);
                if (Node && IsExactFallback(Candidate))
                {
                    FResolvedTarget Match;
                    Match.Object = Node;
                    Match.Graph = Graph;
                    Match.Node = Node;
                    Match.Id = Candidate.Id;
                    Match.IdKind = TEXT("qualified_name_fallback");
                    Matches.Add(Match);
                }
            }
        }
    }
    else if (Kind == ETargetKind::Pin)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node)
                {
                    continue;
                }
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    const FTargetRef Candidate = DescribePinTarget(Blueprint, Pin);
                    if (Pin && IsExactFallback(Candidate))
                    {
                        FResolvedTarget Match;
                        Match.Graph = Graph;
                        Match.Node = Node;
                        Match.Pin = Pin;
                        Match.Id = Candidate.Id;
                        Match.IdKind = TEXT("qualified_name_fallback");
                        Matches.Add(Match);
                    }
                }
            }
        }
    }
    else if (Kind == ETargetKind::Variable)
    {
        for (FBPVariableDescription& Variable : Blueprint->NewVariables)
        {
            const FTargetRef Candidate =
                DescribeVariableTarget(Blueprint, Variable);
            if (IsExactFallback(Candidate))
            {
                FResolvedTarget Match;
                Match.Variable = &Variable;
                Match.Id = Candidate.Id;
                Match.IdKind = TEXT("qualified_name_fallback");
                Matches.Add(Match);
            }
        }
    }
    else if (Kind == ETargetKind::Component && Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* Component : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            const FTargetRef Candidate =
                DescribeComponentTarget(Blueprint, Component);
            if (Component && IsExactFallback(Candidate))
            {
                FResolvedTarget Match;
                Match.Object = Component;
                Match.Component = Component;
                Match.Id = Candidate.Id;
                Match.IdKind = TEXT("qualified_name_fallback");
                Matches.Add(Match);
            }
        }
    }
    if (Matches.Num() != 1)
    {
        OutError = Matches.IsEmpty()
            ? TEXT("Fallback target was not found.")
            : TEXT("Fallback target is ambiguous.");
        return {};
    }
    Matches[0].bStable = Kind == ETargetKind::Interface;
    return Matches[0];
}

FString CanonicalQueryDigest(const TSharedRef<FJsonObject>& Query)
{
    return Sha1(CanonicalJsonValue(MakeShared<FJsonValueObject>(Query)));
}

FString CanonicalJsonString(const TSharedPtr<FJsonValue>& Value)
{
    return CanonicalJsonValue(Value);
}

FString Sha1Hex(const FString& Value)
{
    return Sha1(Value);
}

FString RegisterPaletteActionToken(FPaletteActionRecord& Record)
{
    if (Record.Context.AssetPath.IsEmpty() ||
        Record.Context.GraphId.IsEmpty() ||
        Record.Context.GraphSchemaPath.IsEmpty() ||
        Record.Context.RequestDigest.IsEmpty() ||
        Record.Context.ResultDigest.IsEmpty() ||
        Record.Context.Limit <= 0 ||
        Record.CandidateKey.IsEmpty() ||
        Record.SpawnerSignature.IsEmpty() ||
        Record.SortKey.IsEmpty())
    {
        return FString();
    }

    Record.BindingPaths.Sort();
    const FString SessionId = CurrentPaletteSessionId();
    const FString Canonical = SessionId + TEXT("\n") +
        CanonicalPaletteContext(Record.Context) + TEXT("\n") +
        Record.Query + TEXT("\n") +
        Record.FiltersJson + TEXT("\n") +
        Record.CandidateKey + TEXT("\n") +
        Record.SpawnerSignature + TEXT("\n") +
        Record.OwnerPath + TEXT("\n") +
        FString::Join(Record.BindingPaths, TEXT("\n")) + TEXT("\n") +
        Record.SortKey;
    Record.ActionId = TEXT("action:") + Sha1(Canonical);

    FPaletteTokenState& State = PaletteTokenState();
    FScopeLock Lock(&State.Mutex);
    const FDateTime Now = PaletteNow(State);
    PurgeExpiredPaletteRecords(State, Now);
    if (FStoredPaletteAction* Existing = State.Actions.Find(Record.ActionId))
    {
        Existing->Record = Record;
        Existing->LastUsedAt = Now;
        return Record.ActionId;
    }

    FStoredPaletteAction Stored;
    Stored.Record = Record;
    Stored.EditorSessionId = SessionId;
    Stored.CreatedAt = Now;
    Stored.LastUsedAt = Now;
    State.Actions.Add(Record.ActionId, MoveTemp(Stored));
    State.ActionOrder.Add(Record.ActionId);
    EnforceRecordBound(State.Actions, State.ActionOrder, 4096);
    return Record.ActionId;
}

bool ResolvePaletteActionToken(
    const FString& ActionId,
    const FPaletteContext& Expected,
    FPaletteActionRecord& OutRecord,
    FError& OutError)
{
    OutRecord = FPaletteActionRecord{};
    OutError = FError{};
    if (!IsOpaqueToken(ActionId, TEXT("action:")))
    {
        SetPaletteError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.action_id"),
            TEXT("action_id must be an opaque action token returned by palette search."),
            TEXT("Repeat the palette search and use the returned action_id unchanged."));
        return false;
    }

    FPaletteTokenState& State = PaletteTokenState();
    FScopeLock Lock(&State.Mutex);
    const FDateTime Now = PaletteNow(State);
    PurgeExpiredPaletteRecords(State, Now);
    FStoredPaletteAction* Stored = State.Actions.Find(ActionId);
    if (!Stored)
    {
        SetPaletteError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.action_id"),
            TEXT("action_id is unknown, expired, evicted, or tampered."),
            TEXT("Repeat the palette search and use a current action_id."));
        return false;
    }
    if (Stored->EditorSessionId != CurrentPaletteSessionId() ||
        !PaletteContextMatches(Stored->Record.Context, Expected))
    {
        SetPaletteError(
            OutError,
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("action_id no longer matches the current editor, asset, graph, pin, or palette context."),
            TEXT("Repeat the palette search in the current context."));
        return false;
    }

    Stored->LastUsedAt = Now;
    OutRecord = Stored->Record;
    return true;
}

FString RegisterPaletteCursor(FPaletteCursorRecord& Record)
{
    if (Record.Context.AssetPath.IsEmpty() ||
        Record.Context.GraphId.IsEmpty() ||
        Record.Context.GraphSchemaPath.IsEmpty() ||
        Record.Context.RequestDigest.IsEmpty() ||
        Record.Context.ResultDigest.IsEmpty() ||
        Record.Context.Limit <= 0 ||
        Record.LastSortKey.IsEmpty())
    {
        return FString();
    }

    const FString SessionId = CurrentPaletteSessionId();
    Record.CursorId = TEXT("palette-cursor:") + Sha1(
        SessionId + TEXT("\n") + CanonicalPaletteContext(Record.Context) +
        TEXT("\n") + Record.LastSortKey);

    FPaletteTokenState& State = PaletteTokenState();
    FScopeLock Lock(&State.Mutex);
    const FDateTime Now = PaletteNow(State);
    PurgeExpiredPaletteRecords(State, Now);
    if (FStoredPaletteCursor* Existing = State.Cursors.Find(Record.CursorId))
    {
        Existing->Record = Record;
        Existing->LastUsedAt = Now;
        return Record.CursorId;
    }

    FStoredPaletteCursor Stored;
    Stored.Record = Record;
    Stored.EditorSessionId = SessionId;
    Stored.CreatedAt = Now;
    Stored.LastUsedAt = Now;
    State.Cursors.Add(Record.CursorId, MoveTemp(Stored));
    State.CursorOrder.Add(Record.CursorId);
    EnforceRecordBound(State.Cursors, State.CursorOrder, 1024);
    return Record.CursorId;
}

bool ResolvePaletteCursor(
    const FString& Cursor,
    const FPaletteContext& Expected,
    FPaletteCursorRecord& OutRecord,
    FError& OutError)
{
    OutRecord = FPaletteCursorRecord{};
    OutError = FError{};
    if (!IsOpaqueToken(Cursor, TEXT("palette-cursor:")))
    {
        SetPaletteError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.cursor"),
            TEXT("cursor must be an opaque palette cursor returned by the previous page."),
            TEXT("Restart palette search without a cursor."));
        return false;
    }

    FPaletteTokenState& State = PaletteTokenState();
    FScopeLock Lock(&State.Mutex);
    const FDateTime Now = PaletteNow(State);
    PurgeExpiredPaletteRecords(State, Now);
    FStoredPaletteCursor* Stored = State.Cursors.Find(Cursor);
    if (!Stored)
    {
        SetPaletteError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.cursor"),
            TEXT("cursor is unknown, expired, evicted, or tampered."),
            TEXT("Restart palette search without a cursor."));
        return false;
    }
    if (Stored->EditorSessionId != CurrentPaletteSessionId() ||
        !PaletteContextMatches(Stored->Record.Context, Expected))
    {
        SetPaletteError(
            OutError,
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.cursor"),
            TEXT("cursor no longer matches the current request or palette result set."),
            TEXT("Restart palette search without a cursor."));
        return false;
    }

    Stored->LastUsedAt = Now;
    OutRecord = Stored->Record;
    return true;
}

FString RegisterPaletteBinding(FPaletteBindingRecord& Record)
{
    if (!IsOpaqueToken(Record.ActionId, TEXT("action:")) ||
        Record.ObjectPath.IsEmpty() ||
        Record.ExpectedClassPath.IsEmpty())
    {
        return FString();
    }

    const FString SessionId = CurrentPaletteSessionId();
    Record.BindingId = TEXT("binding:") + Sha1(
        SessionId + TEXT("\n") + Record.ActionId + TEXT("\n") +
        Record.ObjectPath + TEXT("\n") + Record.ExpectedClassPath);

    FPaletteTokenState& State = PaletteTokenState();
    FScopeLock Lock(&State.Mutex);
    const FDateTime Now = PaletteNow(State);
    PurgeExpiredPaletteRecords(State, Now);
    if (FStoredPaletteBinding* Existing = State.Bindings.Find(Record.BindingId))
    {
        Existing->Record = Record;
        Existing->LastUsedAt = Now;
        return Record.BindingId;
    }

    FStoredPaletteBinding Stored;
    Stored.Record = Record;
    Stored.EditorSessionId = SessionId;
    Stored.CreatedAt = Now;
    Stored.LastUsedAt = Now;
    State.Bindings.Add(Record.BindingId, MoveTemp(Stored));
    State.BindingOrder.Add(Record.BindingId);
    EnforceRecordBound(State.Bindings, State.BindingOrder, 1024);
    return Record.BindingId;
}

bool ResolvePaletteBindings(
    const FString& ActionId,
    const TArray<FString>& BindingIds,
    TArray<FPaletteBindingRecord>& OutRecords,
    FError& OutError)
{
    OutRecords.Reset();
    OutError = FError{};
    if (!IsOpaqueToken(ActionId, TEXT("action:")) || BindingIds.Num() > 32)
    {
        SetPaletteError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.bindings"),
            TEXT("bindings require one valid action_id and at most 32 opaque binding IDs."),
            TEXT("Use only binding IDs returned with the selected palette action."));
        return false;
    }

    TSet<FString> UniqueIds;
    FPaletteTokenState& State = PaletteTokenState();
    FScopeLock Lock(&State.Mutex);
    const FDateTime Now = PaletteNow(State);
    PurgeExpiredPaletteRecords(State, Now);
    for (const FString& BindingId : BindingIds)
    {
        if (!IsOpaqueToken(BindingId, TEXT("binding:")) ||
            UniqueIds.Contains(BindingId))
        {
            SetPaletteError(
                OutError,
                TEXT("INVALID_INPUT"),
                TEXT("params.bindings"),
                TEXT("binding IDs must be unique opaque values returned by palette search."),
                TEXT("Remove duplicate or modified binding IDs."));
            OutRecords.Reset();
            return false;
        }
        UniqueIds.Add(BindingId);

        FStoredPaletteBinding* Stored = State.Bindings.Find(BindingId);
        if (!Stored || Stored->EditorSessionId != CurrentPaletteSessionId() ||
            Stored->Record.ActionId != ActionId)
        {
            SetPaletteError(
                OutError,
                TEXT("INVALID_INPUT"),
                TEXT("params.bindings"),
                TEXT("binding ID is unknown, expired, or belongs to another action."),
                TEXT("Describe or search the action again and use its binding IDs."));
            OutRecords.Reset();
            return false;
        }
        Stored->LastUsedAt = Now;
        OutRecords.Add(Stored->Record);
    }
    return true;
}

#if WITH_DEV_AUTOMATION_TESTS
void ResetPaletteTokenStateForTests()
{
    FPaletteTokenState& State = PaletteTokenState();
    FScopeLock Lock(&State.Mutex);
    State.Actions.Reset();
    State.Cursors.Reset();
    State.Bindings.Reset();
    State.ActionOrder.Reset();
    State.CursorOrder.Reset();
    State.BindingOrder.Reset();
}

void SetPaletteTokenClockForTests(const TOptional<FDateTime>& Now)
{
    FPaletteTokenState& State = PaletteTokenState();
    FScopeLock Lock(&State.Mutex);
    State.TestNow = Now;
}
#endif

FString EncodeCursor(const FString& AssetPath, const FPageRequest& Page)
{
    const TSharedRef<FJsonObject> Cursor = MakeShared<FJsonObject>();
    Cursor->SetStringField(TEXT("asset_path"), AssetPath);
    Cursor->SetStringField(
        TEXT("editor_session"),
        UE::MCPython::GetEditorSessionId().ToString(
            EGuidFormats::DigitsWithHyphensLower));
    Cursor->SetStringField(TEXT("last_id"), Page.LastId);
    Cursor->SetStringField(TEXT("query_digest"), Page.QueryDigest);
    FString Serialized;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    FJsonSerializer::Serialize(Cursor, Writer);
    return FBase64::Encode(Serialized, EBase64Mode::UrlSafe);
}

bool DecodeCursor(
    const FString& Cursor,
    const FString& ExpectedAssetPath,
    const FString& ExpectedQueryDigest,
    FPageRequest& OutPage,
    FString& OutError)
{
    FString Decoded;
    if (!FBase64::Decode(Cursor, Decoded, EBase64Mode::UrlSafe))
    {
        OutError = TEXT("Cursor is not valid base64url.");
        return false;
    }
    TSharedPtr<FJsonObject> Object;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Decoded);
    if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
    {
        OutError = TEXT("Cursor does not contain a JSON object.");
        return false;
    }
    FString AssetPath;
    FString EditorSession;
    FString LastId;
    FString QueryDigest;
    if (!Object->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Object->TryGetStringField(TEXT("editor_session"), EditorSession) ||
        !Object->TryGetStringField(TEXT("last_id"), LastId) ||
        !Object->TryGetStringField(TEXT("query_digest"), QueryDigest))
    {
        OutError = TEXT("Cursor is missing required fields.");
        return false;
    }
    const FString CurrentSession = UE::MCPython::GetEditorSessionId().ToString(
        EGuidFormats::DigitsWithHyphensLower);
    if (AssetPath != ExpectedAssetPath || QueryDigest != ExpectedQueryDigest ||
        EditorSession != CurrentSession)
    {
        OutError = TEXT("Cursor does not match the asset, query, or editor session.");
        return false;
    }
    OutPage.LastId = LastId;
    OutPage.QueryDigest = QueryDigest;
    OutError.Empty();
    return true;
}

TSharedRef<FJsonObject> MakeSuccess(
    const FString& Summary,
    const TSharedPtr<FJsonObject>& Data)
{
    const FString TraceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("status"), TEXT("succeeded"));
    Result->SetStringField(TEXT("summary"), Summary);
    Result->SetObjectField(TEXT("data"), Data.IsValid() ? Data : MakeShared<FJsonObject>());
    Result->SetField(TEXT("changes"), EmptyArrayValue());
    Result->SetField(TEXT("warnings"), EmptyArrayValue());
    Result->SetField(TEXT("errors"), EmptyArrayValue());
    Result->SetField(TEXT("next_actions"), EmptyArrayValue());
    Result->SetStringField(TEXT("trace_id"), TraceId);
    return Result;
}

TSharedRef<FJsonObject> MakeFailure(
    const FString& Code,
    const FString& Path,
    const FString& Message,
    bool bRetryable,
    const FString& Hint,
    const TSharedPtr<FJsonObject>& Details)
{
    static const TSet<FString> ApprovedCodes = {
        TEXT("INVALID_INPUT"), TEXT("UNKNOWN_ACTION"), TEXT("CONFLICT"),
        TEXT("PRECONDITION_FAILED"), TEXT("CONFIRMATION_REQUIRED"),
        TEXT("CONFIRMATION_EXPIRED"), TEXT("PLUGIN_REQUIRED"),
        TEXT("UE_VERSION_UNSUPPORTED"), TEXT("UE_UNAVAILABLE"),
        TEXT("TIMEOUT"), TEXT("COMPILE_FAILED"), TEXT("VERIFICATION_FAILED"),
        TEXT("TRANSACTION_FAILED"), TEXT("ROLLBACK_FAILED"),
        TEXT("INTERNAL_ERROR")};
    const FString SafeCode = ApprovedCodes.Contains(Code)
        ? Code
        : FString(TEXT("INTERNAL_ERROR"));
    const FString TraceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
    Error->SetStringField(TEXT("code"), SafeCode);
    if (Path.IsEmpty())
    {
        Error->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
    }
    else
    {
        Error->SetStringField(TEXT("path"), Path);
    }
    Error->SetStringField(TEXT("message"), Message);
    Error->SetBoolField(TEXT("retryable"), bRetryable);
    Error->SetStringField(TEXT("hint"), Hint);
    Error->SetObjectField(
        TEXT("details"), Details.IsValid() ? Details : MakeShared<FJsonObject>());
    Error->SetStringField(TEXT("trace_id"), TraceId);

    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(TEXT("status"), TEXT("failed"));
    Result->SetStringField(TEXT("summary"), Message);
    Result->SetObjectField(TEXT("data"), MakeShared<FJsonObject>());
    Result->SetField(TEXT("changes"), EmptyArrayValue());
    Result->SetField(TEXT("warnings"), EmptyArrayValue());
    Result->SetArrayField(
        TEXT("errors"), {MakeShared<FJsonValueObject>(Error)});
    Result->SetField(TEXT("next_actions"), EmptyArrayValue());
    Result->SetStringField(TEXT("trace_id"), TraceId);
    return Result;
}

FString SerializeResult(const TSharedRef<FJsonObject>& Result)
{
    FString Serialized;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    FJsonSerializer::Serialize(Result, Writer);
    return Serialized;
}

bool ParseTypeSpec(
    const TSharedRef<FJsonObject>& Spec,
    FEdGraphPinType& OutType,
    FError& OutError,
    const FString& Path,
    int32 Depth)
{
    OutType.ResetToDefaults();
    OutError = FError{};

    if (Depth == 0 && !ValidateContainerDepth(Spec, Path, 0, OutError))
    {
        return false;
    }

    FString Kind;
    if (!TryGetKind(Spec, Path, Kind, OutError))
    {
        return false;
    }

    if (IsContainerKind(Kind) && Depth >= 8)
    {
        return InvalidInput(
            OutError,
            Path + TEXT(".kind"),
            TEXT("Container nesting cannot exceed eight levels."));
    }

    auto SetPrimitive = [&OutType](const FName Category)
    {
        OutType.PinCategory = Category;
    };
    if (Kind == TEXT("bool") || Kind == TEXT("byte") ||
        Kind == TEXT("int") || Kind == TEXT("int64") ||
        Kind == TEXT("string") || Kind == TEXT("name") ||
        Kind == TEXT("text"))
    {
        if (!ValidateClosedObject(Spec, {TEXT("kind")}, Path, OutError))
        {
            return false;
        }
        if (Kind == TEXT("bool")) SetPrimitive(UEdGraphSchema_K2::PC_Boolean);
        else if (Kind == TEXT("byte")) SetPrimitive(UEdGraphSchema_K2::PC_Byte);
        else if (Kind == TEXT("int")) SetPrimitive(UEdGraphSchema_K2::PC_Int);
        else if (Kind == TEXT("int64")) SetPrimitive(UEdGraphSchema_K2::PC_Int64);
        else if (Kind == TEXT("string")) SetPrimitive(UEdGraphSchema_K2::PC_String);
        else if (Kind == TEXT("name")) SetPrimitive(UEdGraphSchema_K2::PC_Name);
        else SetPrimitive(UEdGraphSchema_K2::PC_Text);
        return true;
    }

    if (Kind == TEXT("real"))
    {
        if (!ValidateClosedObject(
            Spec, {TEXT("kind"), TEXT("precision")}, Path, OutError))
        {
            return false;
        }
        FString Precision;
        if (!TryGetRequiredString(
            Spec, TEXT("precision"), Path, Precision, OutError))
        {
            return false;
        }
        if (Precision != TEXT("float") && Precision != TEXT("double"))
        {
            return InvalidInput(
                OutError,
                Path + TEXT(".precision"),
                TEXT("Real precision must be 'float' or 'double'."));
        }
        OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
        OutType.PinSubCategory = Precision == TEXT("float")
            ? UEdGraphSchema_K2::PC_Float
            : UEdGraphSchema_K2::PC_Double;
        return true;
    }

    if (Kind == TEXT("enum") || Kind == TEXT("struct"))
    {
        if (!ValidateClosedObject(
            Spec, {TEXT("kind"), TEXT("type_path")}, Path, OutError))
        {
            return false;
        }
        FString TypePath;
        if (!TryGetRequiredString(
            Spec, TEXT("type_path"), Path, TypePath, OutError))
        {
            return false;
        }
        UObject* TypeObject = FindOrLoadTypeObject(TypePath);
        if (Kind == TEXT("enum") && !Cast<UEnum>(TypeObject))
        {
            return InvalidInput(
                OutError,
                Path + TEXT(".type_path"),
                TEXT("Enum type_path must resolve to a UEnum."));
        }
        if (Kind == TEXT("struct") && !Cast<UScriptStruct>(TypeObject))
        {
            return InvalidInput(
                OutError,
                Path + TEXT(".type_path"),
                TEXT("Struct type_path must resolve to a UScriptStruct."));
        }
        OutType.PinCategory = Kind == TEXT("enum")
            ? UEdGraphSchema_K2::PC_Byte
            : UEdGraphSchema_K2::PC_Struct;
        OutType.PinSubCategoryObject = TypeObject;
        return true;
    }

    if (Kind == TEXT("object") || Kind == TEXT("class") ||
        Kind == TEXT("interface") || Kind == TEXT("soft_object") ||
        Kind == TEXT("soft_class"))
    {
        if (!ValidateClosedObject(
            Spec, {TEXT("kind"), TEXT("class_path")}, Path, OutError))
        {
            return false;
        }
        FString ClassPath;
        if (!TryGetRequiredString(
            Spec, TEXT("class_path"), Path, ClassPath, OutError))
        {
            return false;
        }
        UClass* Class = Cast<UClass>(FindOrLoadTypeObject(ClassPath));
        if (!Class)
        {
            return InvalidInput(
                OutError,
                Path + TEXT(".class_path"),
                TEXT("class_path must resolve to a UClass."));
        }
        if (Kind == TEXT("interface") &&
            !Class->HasAnyClassFlags(CLASS_Interface))
        {
            return InvalidInput(
                OutError,
                Path + TEXT(".class_path"),
                TEXT("Interface class_path must resolve to an interface class."));
        }
        if (Kind == TEXT("object")) OutType.PinCategory = UEdGraphSchema_K2::PC_Object;
        else if (Kind == TEXT("class")) OutType.PinCategory = UEdGraphSchema_K2::PC_Class;
        else if (Kind == TEXT("interface")) OutType.PinCategory = UEdGraphSchema_K2::PC_Interface;
        else if (Kind == TEXT("soft_object")) OutType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
        else OutType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
        OutType.PinSubCategoryObject = Class;
        return true;
    }

    if (Kind == TEXT("array") || Kind == TEXT("set"))
    {
        if (!ValidateClosedObject(
            Spec, {TEXT("kind"), TEXT("item")}, Path, OutError))
        {
            return false;
        }
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        if (!TryGetRequiredObject(
            Spec, TEXT("item"), Path, Item, OutError))
        {
            return false;
        }
        FString ItemKind;
        if (!TryGetKind(Item, Path + TEXT(".item"), ItemKind, OutError))
        {
            return false;
        }
        if (IsContainerKind(ItemKind))
        {
            return InvalidInput(
                OutError,
                Path + TEXT(".item.kind"),
                TEXT("Unreal pin types cannot contain implicit nested containers."),
                TEXT("Wrap the nested container in a Blueprint struct."));
        }
        if (!ParseTypeSpec(
            Item, OutType, OutError, Path + TEXT(".item"), Depth + 1))
        {
            return false;
        }
        OutType.ContainerType = Kind == TEXT("array")
            ? EPinContainerType::Array
            : EPinContainerType::Set;
        return true;
    }

    if (Kind == TEXT("map"))
    {
        if (!ValidateClosedObject(
            Spec,
            {TEXT("kind"), TEXT("key"), TEXT("value")},
            Path,
            OutError))
        {
            return false;
        }
        TSharedRef<FJsonObject> Key = MakeShared<FJsonObject>();
        TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
        if (!TryGetRequiredObject(Spec, TEXT("key"), Path, Key, OutError) ||
            !TryGetRequiredObject(Spec, TEXT("value"), Path, Value, OutError))
        {
            return false;
        }
        FString KeyKind;
        if (!TryGetKind(Key, Path + TEXT(".key"), KeyKind, OutError))
        {
            return false;
        }
        if (IsContainerKind(KeyKind))
        {
            return InvalidInput(
                OutError,
                Path + TEXT(".key.kind"),
                TEXT("Map keys cannot be containers."));
        }
        FString ValueKind;
        if (!TryGetKind(Value, Path + TEXT(".value"), ValueKind, OutError))
        {
            return false;
        }
        if (IsContainerKind(ValueKind))
        {
            return InvalidInput(
                OutError,
                Path + TEXT(".value.kind"),
                TEXT("Unreal pin types cannot contain implicit nested containers."),
                TEXT("Wrap the nested container in a Blueprint struct."));
        }
        FEdGraphPinType KeyType;
        FEdGraphPinType ValueType;
        if (!ParseTypeSpec(
                Key, KeyType, OutError, Path + TEXT(".key"), Depth + 1) ||
            !ParseTypeSpec(
                Value, ValueType, OutError, Path + TEXT(".value"), Depth + 1))
        {
            return false;
        }
        OutType = KeyType;
        OutType.ContainerType = EPinContainerType::Map;
        OutType.PinValueType = FEdGraphTerminalType::FromPinType(ValueType);
        return true;
    }

    return InvalidInput(
        OutError,
        Path + TEXT(".kind"),
        FString::Printf(TEXT("Unknown Blueprint type kind '%s'."), *Kind));
}

TSharedRef<FJsonObject> SerializeTypeSpec(const FEdGraphPinType& Type)
{
    if (Type.ContainerType == EPinContainerType::None)
    {
        return SerializeScalarType(Type);
    }

    if (Type.ContainerType == EPinContainerType::Array ||
        Type.ContainerType == EPinContainerType::Set)
    {
        FEdGraphPinType ItemType = Type;
        ItemType.ContainerType = EPinContainerType::None;
        const TCHAR* Kind = Type.ContainerType == EPinContainerType::Array
            ? TEXT("array")
            : TEXT("set");
        const TSharedRef<FJsonObject> Result = MakeKindObject(Kind);
        Result->SetObjectField(TEXT("item"), SerializeScalarType(ItemType));
        return Result;
    }

    FEdGraphPinType KeyType = Type;
    KeyType.ContainerType = EPinContainerType::None;
    const FEdGraphPinType ValueType =
        FEdGraphPinType::GetPinTypeForTerminalType(Type.PinValueType);
    const TSharedRef<FJsonObject> Result = MakeKindObject(TEXT("map"));
    Result->SetObjectField(TEXT("key"), SerializeScalarType(KeyType));
    Result->SetObjectField(TEXT("value"), SerializeScalarType(ValueType));
    return Result;
}

TSharedPtr<FJsonValue> SerializeDefaultValue(
    const FEdGraphPinType& Type,
    const FString& DefaultValue,
    UObject* DefaultObject,
    const FText& DefaultTextValue)
{
    return ImportTextToJson(
        Type, DefaultValue, DefaultObject, DefaultTextValue, false);
}

bool NormalizeDefaultValue(
    const FEdGraphPinType& Type,
    const TSharedPtr<FJsonValue>& JsonValue,
    UObject* Owner,
    FNormalizedDefault& OutDefault,
    FError& OutError,
    const FString& Path)
{
    OutDefault = FNormalizedDefault{};
    OutError = FError{};
    FString ImportText;
    if (!JsonToImportText(
        Type,
        JsonValue,
        Path,
        false,
        ImportText,
        OutError))
    {
        return false;
    }

    if (!ValidateDefaultRecursively(
            Type, JsonValue, Owner, Path, OutError))
    {
        return false;
    }

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    Schema->GetPinDefaultValuesFromString(
        Type,
        Owner,
        ImportText,
        OutDefault.DefaultValue,
        OutDefault.DefaultObject,
        OutDefault.DefaultTextValue);

    if (Type.IsContainer())
    {
        OutDefault.DefaultValue = ImportText;
        OutDefault.DefaultObject = nullptr;
        OutDefault.DefaultTextValue = FText::GetEmpty();
    }

    if (!Type.IsContainer() && !JsonValue->IsNull() &&
        (Type.PinCategory == UEdGraphSchema_K2::PC_Object ||
         Type.PinCategory == UEdGraphSchema_K2::PC_Class ||
         Type.PinCategory == UEdGraphSchema_K2::PC_Interface) &&
        !OutDefault.DefaultObject)
    {
        OutDefault = FNormalizedDefault{};
        return InvalidInput(
            OutError, Path, TEXT("Unreal object path could not be resolved."));
    }

    FEdGraphPinType ValidationType = Type;
    if (ValidationType.PinCategory == UEdGraphSchema_K2::PC_Enum)
    {
        ValidationType.PinCategory = UEdGraphSchema_K2::PC_Byte;
    }
    FString ValidationMessage;
    if (!Schema->DefaultValueSimpleValidation(
        ValidationType,
        NAME_None,
        OutDefault.DefaultValue,
        OutDefault.DefaultObject,
        OutDefault.DefaultTextValue,
        &ValidationMessage))
    {
        OutDefault = FNormalizedDefault{};
        return InvalidInput(OutError, Path, ValidationMessage);
    }
    return true;
}

bool SupportsCompilerTokens()
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 7
    return true;
#else
    return false;
#endif
}

TSharedRef<FJsonObject> BuildCapabilities(UBlueprint* Blueprint)
{
    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("api_version"), 2);
    Result->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    auto StringArray = [](std::initializer_list<const TCHAR*> Values)
    {
        TArray<TSharedPtr<FJsonValue>> ResultValues;
        for (const TCHAR* Value : Values)
        {
            ResultValues.Add(MakeShared<FJsonValueString>(Value));
        }
        return ResultValues;
    };
    Result->SetArrayField(TEXT("scalar_kinds"), StringArray({
        TEXT("bool"), TEXT("byte"), TEXT("int"), TEXT("int64"),
        TEXT("real"), TEXT("string"), TEXT("name"), TEXT("text"),
        TEXT("enum"), TEXT("struct"), TEXT("object"), TEXT("class"),
        TEXT("interface"), TEXT("soft_object"), TEXT("soft_class")}));
    Result->SetArrayField(
        TEXT("container_kinds"),
        StringArray({TEXT("array"), TEXT("set"), TEXT("map")}));
    Result->SetArrayField(TEXT("node_families"), StringArray({
        TEXT("event"), TEXT("call_function"), TEXT("variable"),
        TEXT("branch"), TEXT("cast"), TEXT("construct_object"),
        TEXT("dynamic_cast"), TEXT("macro"), TEXT("interface_message")}));

    bool bHasGraph = false;
    bool bHasK2Graph = false;
    bool bAllGraphsUseK2Schema = true;
    if (Blueprint)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (const UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            bHasGraph = true;
            const bool bUsesK2Schema = Graph->GetSchema() &&
                Graph->GetSchema()->IsA<UEdGraphSchema_K2>();
            bHasK2Graph |= bUsesK2Schema;
            bAllGraphsUseK2Schema &= bUsesK2Schema;
        }
    }
    bAllGraphsUseK2Schema &= bHasGraph;
    const bool bHasSCS = Blueprint && Blueprint->SimpleConstructionScript;
    const bool bSupportsCompilerTokens = SupportsCompilerTokens();
    Result->SetBoolField(TEXT("supports_k2_schema"), true);
    Result->SetBoolField(TEXT("supports_scs_operations"), true);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 7
    Result->SetBoolField(TEXT("supports_blueprint_node_palette"), true);
#else
    Result->SetBoolField(TEXT("supports_blueprint_node_palette"), false);
#endif
    Result->SetBoolField(
        TEXT("has_palette_compatible_graphs"), bHasK2Graph);
    Result->SetBoolField(
        TEXT("supports_compiler_tokens"),
        bSupportsCompilerTokens);
    Result->SetBoolField(TEXT("has_k2_graphs"), bHasK2Graph);
    Result->SetBoolField(
        TEXT("all_graphs_k2_schema"),
        bAllGraphsUseK2Schema);
    Result->SetBoolField(TEXT("k2_schema"), bAllGraphsUseK2Schema);
    Result->SetBoolField(TEXT("compiler_tokens"), bSupportsCompilerTokens);
    Result->SetBoolField(TEXT("has_scs"), bHasSCS);
    Result->SetBoolField(TEXT("scs_operations"), bHasSCS);
    return Result;
}

bool IsSupportedBlueprintSelectionEditor(const FName& EditorName)
{
    return EditorName == TEXT("BlueprintEditor") ||
        EditorName == TEXT("WidgetBlueprintEditor") ||
        EditorName == TEXT("AnimationBlueprintEditor");
}

FMutationScope::FMutationScope(const FText& Description)
    : bWorkflowOwned(UE::MCPython::HasActiveWorkflowTransaction())
{
    if (bWorkflowOwned)
    {
        return;
    }
    if (GEditor && !GEditor->Trans)
    {
        GEditor->Trans = GEditor->CreateTrans();
    }
    if (!GEditor || !GEditor->Trans || GEditor->Trans->IsActive())
    {
        return;
    }
    LocalTransaction = MakeUnique<FScopedTransaction>(Description);
    if (!LocalTransaction->IsOutstanding())
    {
        LocalTransaction.Reset();
        return;
    }
    TransactionIndex = GEditor->Trans->GetQueueLength() - 1;
    TransactionGuid = TransactionGuidAtIndex(TransactionIndex);
    if (!TransactionGuid.IsValid())
    {
        LocalTransaction->Cancel();
        LocalTransaction.Reset();
        TransactionIndex = INDEX_NONE;
    }
}

FMutationScope::~FMutationScope() = default;

bool FMutationScope::IsValid() const
{
    return bWorkflowOwned ||
        (LocalTransaction.IsValid() && LocalTransaction->IsOutstanding() &&
         TransactionGuid.IsValid());
}

void FMutationScope::Modify(UObject* Object)
{
    if (IsValid() && Object)
    {
        Object->Modify();
    }
}

FRollbackResult FMutationScope::Rollback()
{
    FRollbackResult Result;
    if (bWorkflowOwned)
    {
        Result.bSucceeded = true;
        Result.bDeferredToWorkflow = true;
        return Result;
    }
    if (!LocalTransaction.IsValid())
    {
        return Result;
    }

    LocalTransaction.Reset();
    const bool bUndoSucceeded =
        IsCurrentTransaction(TransactionIndex, TransactionGuid) &&
        GEditor->UndoTransaction();
    Result.bSucceeded = bUndoSucceeded;
    if (!bUndoSucceeded)
    {
        const TSharedRef<FJsonObject> Residual = MakeShared<FJsonObject>();
        Residual->SetStringField(TEXT("code"), TEXT("ROLLBACK_FAILED"));
        Residual->SetNumberField(TEXT("transaction_index"), TransactionIndex);
        Residual->SetStringField(
            TEXT("transaction_guid"),
            TransactionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        Result.ResidualChanges.Add(MakeShared<FJsonValueObject>(Residual));
    }
    TransactionIndex = INDEX_NONE;
    TransactionGuid.Invalidate();
    return Result;
}
}
