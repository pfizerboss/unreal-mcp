// Copyright (c) 2025 GenOrca. All Rights Reserved.

#include "MCPythonHelper.h"

#include "MCPythonBlueprint2Internal.h"
#include "MCPythonBlueprintPaletteInternal.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintActionFilter.h"
#include "BlueprintNodeSignature.h"
#include "BlueprintNodeSpawner.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_StructOperation.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/EngineVersionComparison.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/ObjectKey.h"
#include "UObject/UObjectGlobals.h"

namespace UE::MCPython::Blueprint2::Palette
{
using namespace UE::MCPython::Blueprint2;

struct FPaletteRequest
{
    FString GraphId;
    FString Query;
    FPaletteFilters Filters;
    FString FiltersJson;
    FString Cursor;
    int32 Limit = 50;
};

struct FPaletteSpawnRequest
{
    FString GraphId;
    FString ActionId;
    double PositionX = 0.0;
    double PositionY = 0.0;
    TArray<FString> BindingIds;
};


FString Failure(const FError& Error, const bool bRetryable = false)
{
    return SerializeResult(MakeFailure(
        Error.Code.IsEmpty() ? TEXT("INTERNAL_ERROR") : Error.Code,
        Error.Path,
        Error.Message.IsEmpty() ? TEXT("Blueprint palette request failed.") : Error.Message,
        bRetryable,
        Error.Hint));
}

FString Failure(
    const FString& Code,
    const FString& Path,
    const FString& Message,
    const FString& Hint,
    const bool bRetryable = false)
{
    return SerializeResult(MakeFailure(
        Code, Path, Message, bRetryable, Hint));
}

bool SetInvalid(
    FError& OutError,
    const FString& Path,
    const FString& Message,
    const FString& Hint = TEXT("Use the published Blueprint palette action schema."))
{
    OutError = FError{};
    OutError.Code = TEXT("INVALID_INPUT");
    OutError.Path = Path;
    OutError.Message = Message;
    OutError.Hint = Hint;
    return false;
}

bool SetPrecondition(
    FError& OutError,
    const FString& Path,
    const FString& Message,
    const FString& Hint)
{
    OutError = FError{};
    OutError.Code = TEXT("PRECONDITION_FAILED");
    OutError.Path = Path;
    OutError.Message = Message;
    OutError.Hint = Hint;
    return false;
}

bool ParseObject(
    const FString& Json,
    TSharedPtr<FJsonObject>& OutObject,
    FError& OutError)
{
    OutObject.Reset();
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (Json.IsEmpty() || !FJsonSerializer::Deserialize(Reader, OutObject) ||
        !OutObject.IsValid())
    {
        return SetInvalid(
            OutError,
            TEXT("params"),
            TEXT("Expected one valid JSON object."));
    }
    return true;
}

bool ValidateClosedObject(
    const TSharedRef<FJsonObject>& Object,
    const TSet<FString>& Allowed,
    const FString& Path,
    FError& OutError)
{
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
    {
        if (!Allowed.Contains(Field.Key))
        {
            return SetInvalid(
                OutError,
                Path + TEXT(".") + Field.Key,
                TEXT("Unknown field in closed palette request object."));
        }
    }
    return true;
}

const TSharedPtr<FJsonValue>* FindField(
    const TSharedRef<FJsonObject>& Object,
    const FString& Name)
{
    return Object->Values.Find(Name);
}

bool ReadString(
    const TSharedRef<FJsonObject>& Object,
    const FString& Name,
    const FString& Path,
    const bool bRequired,
    const int32 MaxLength,
    FString& OutValue,
    FError& OutError)
{
    const TSharedPtr<FJsonValue>* Field = FindField(Object, Name);
    if (!Field)
    {
        if (bRequired)
        {
            return SetInvalid(
                OutError, Path, TEXT("Required string field is missing."));
        }
        return true;
    }
    if (!Field->IsValid() || (*Field)->Type != EJson::String)
    {
        return SetInvalid(OutError, Path, TEXT("Expected a JSON string."));
    }
    OutValue = (*Field)->AsString();
    if (OutValue.Len() > MaxLength)
    {
        return SetInvalid(
            OutError,
            Path,
            FString::Printf(TEXT("String exceeds the %d character limit."), MaxLength));
    }
    if (bRequired && OutValue.IsEmpty())
    {
        return SetInvalid(OutError, Path, TEXT("String must not be empty."));
    }
    return true;
}

bool ReadStringArray(
    const TSharedRef<FJsonObject>& Object,
    const FString& Name,
    const FString& Path,
    const int32 MaxItems,
    const int32 MaxLength,
    TArray<FString>& OutValues,
    FError& OutError)
{
    OutValues.Reset();
    const TSharedPtr<FJsonValue>* Field = FindField(Object, Name);
    if (!Field)
    {
        return true;
    }
    if (!Field->IsValid() || (*Field)->Type != EJson::Array)
    {
        return SetInvalid(OutError, Path, TEXT("Expected a JSON array."));
    }
    const TArray<TSharedPtr<FJsonValue>>& Values = (*Field)->AsArray();
    if (Values.Num() > MaxItems)
    {
        return SetInvalid(
            OutError,
            Path,
            FString::Printf(TEXT("Array exceeds the %d item limit."), MaxItems));
    }
    TSet<FString> Unique;
    for (int32 Index = 0; Index < Values.Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& Value = Values[Index];
        const FString ItemPath = FString::Printf(TEXT("%s[%d]"), *Path, Index);
        if (!Value.IsValid() || Value->Type != EJson::String)
        {
            return SetInvalid(OutError, ItemPath, TEXT("Expected a JSON string."));
        }
        FString Item = Value->AsString();
        if (Item.IsEmpty() || Item.Len() > MaxLength)
        {
            return SetInvalid(
                OutError,
                ItemPath,
                FString::Printf(
                    TEXT("String must contain 1 to %d characters."), MaxLength));
        }
        if (Unique.Contains(Item))
        {
            return SetInvalid(OutError, ItemPath, TEXT("Array items must be unique."));
        }
        Unique.Add(Item);
        OutValues.Add(MoveTemp(Item));
    }
    return true;
}

bool ParseFilters(
    const TSharedRef<FJsonObject>& Object,
    FPaletteFilters& OutFilters,
    FError& OutError)
{
    static const TSet<FString> AllowedFields = {
        TEXT("action_kinds"), TEXT("categories"), TEXT("owner_paths"),
        TEXT("pure_only")};
    static const TSet<FString> AllowedKinds = {
        TEXT("function"), TEXT("event"), TEXT("variable"), TEXT("macro"),
        TEXT("delegate"), TEXT("cast"), TEXT("async"),
        TEXT("flow_control"), TEXT("operator"), TEXT("struct"),
        TEXT("other")};
    if (!ValidateClosedObject(Object, AllowedFields, TEXT("params.filters"), OutError))
    {
        return false;
    }

    TArray<FString> Kinds;
    if (!ReadStringArray(
            Object,
            TEXT("action_kinds"),
            TEXT("params.filters.action_kinds"),
            11,
            32,
            Kinds,
            OutError) ||
        !ReadStringArray(
            Object,
            TEXT("categories"),
            TEXT("params.filters.categories"),
            32,
            256,
            OutFilters.CategoryPrefixes,
            OutError))
    {
        return false;
    }
    for (const FString& Kind : Kinds)
    {
        if (!AllowedKinds.Contains(Kind))
        {
            return SetInvalid(
                OutError,
                TEXT("params.filters.action_kinds"),
                FString::Printf(TEXT("Unknown palette action kind '%s'."), *Kind));
        }
        OutFilters.ActionKinds.Add(Kind);
    }

    TArray<FString> OwnerPaths;
    if (!ReadStringArray(
            Object,
            TEXT("owner_paths"),
            TEXT("params.filters.owner_paths"),
            32,
            1024,
            OwnerPaths,
            OutError))
    {
        return false;
    }
    for (const FString& OwnerPath : OwnerPaths)
    {
        OutFilters.OwnerPaths.Add(OwnerPath);
    }

    const TSharedPtr<FJsonValue>* PureOnly = FindField(Object, TEXT("pure_only"));
    if (PureOnly)
    {
        if (!PureOnly->IsValid() || (*PureOnly)->Type != EJson::Boolean)
        {
            return SetInvalid(
                OutError,
                TEXT("params.filters.pure_only"),
                TEXT("Expected a JSON boolean."));
        }
        OutFilters.bPureOnly = (*PureOnly)->AsBool();
    }
    return true;
}

bool ParseSearchRequest(
    const FString& RequestJson,
    FPaletteRequest& OutRequest,
    FError& OutError)
{
    TSharedPtr<FJsonObject> Request;
    if (!ParseObject(RequestJson, Request, OutError))
    {
        return false;
    }
    static const TSet<FString> AllowedFields = {
        TEXT("graph_id"), TEXT("query"), TEXT("filters"), TEXT("cursor"),
        TEXT("limit")};
    if (!ValidateClosedObject(Request.ToSharedRef(), AllowedFields, TEXT("params"), OutError) ||
        !ReadString(
            Request.ToSharedRef(),
            TEXT("graph_id"),
            TEXT("params.graph_id"),
            true,
            128,
            OutRequest.GraphId,
            OutError) ||
        !ReadString(
            Request.ToSharedRef(),
            TEXT("query"),
            TEXT("params.query"),
            false,
            256,
            OutRequest.Query,
            OutError) ||
        !ReadString(
            Request.ToSharedRef(),
            TEXT("cursor"),
            TEXT("params.cursor"),
            false,
            4096,
            OutRequest.Cursor,
            OutError))
    {
        return false;
    }

    const TSharedPtr<FJsonValue>* Limit = FindField(Request.ToSharedRef(), TEXT("limit"));
    if (Limit)
    {
        if (!Limit->IsValid() || (*Limit)->Type != EJson::Number)
        {
            return SetInvalid(OutError, TEXT("params.limit"), TEXT("Expected an integer."));
        }
        const double Number = (*Limit)->AsNumber();
        if (!FMath::IsFinite(Number) || FMath::FloorToDouble(Number) != Number ||
            Number < 1.0 || Number > 200.0)
        {
            return SetInvalid(
                OutError,
                TEXT("params.limit"),
                TEXT("limit must be an integer from 1 through 200."));
        }
        OutRequest.Limit = static_cast<int32>(Number);
    }

    TSharedRef<FJsonObject> Filters = MakeShared<FJsonObject>();
    const TSharedPtr<FJsonValue>* FiltersValue =
        FindField(Request.ToSharedRef(), TEXT("filters"));
    if (FiltersValue)
    {
        if (!FiltersValue->IsValid() || (*FiltersValue)->Type != EJson::Object)
        {
            return SetInvalid(
                OutError,
                TEXT("params.filters"),
                TEXT("Expected a JSON object."));
        }
        Filters = (*FiltersValue)->AsObject().ToSharedRef();
    }
    if (!ParseFilters(Filters, OutRequest.Filters, OutError))
    {
        return false;
    }
    OutRequest.FiltersJson = CanonicalJsonString(
        MakeShared<FJsonValueObject>(Filters));
    return true;
}

bool ReadFinitePositionCoordinate(
    const TSharedRef<FJsonObject>& Position,
    const FString& Name,
    double& OutValue,
    FError& OutError)
{
    const TSharedPtr<FJsonValue>* Field = FindField(Position, Name);
    const FString Path = TEXT("params.position.") + Name;
    if (!Field)
    {
        return SetInvalid(OutError, Path, TEXT("Required number field is missing."));
    }
    if (!Field->IsValid() || (*Field)->Type != EJson::Number)
    {
        return SetInvalid(OutError, Path, TEXT("Expected a JSON number."));
    }
    OutValue = (*Field)->AsNumber();
    if (!FMath::IsFinite(OutValue) ||
        OutValue < -1000000000.0 || OutValue > 1000000000.0)
    {
        return SetInvalid(
            OutError,
            Path,
            TEXT("Graph coordinates must be finite and between -1000000000 and 1000000000."));
    }
    return true;
}

bool ParseSpawnRequest(
    const FString& RequestJson,
    FPaletteSpawnRequest& OutRequest,
    FError& OutError)
{
    TSharedPtr<FJsonObject> Request;
    if (!ParseObject(RequestJson, Request, OutError))
    {
        return false;
    }
    static const TSet<FString> AllowedFields = {
        TEXT("graph_id"), TEXT("action_id"), TEXT("position"),
        TEXT("bindings")};
    if (!ValidateClosedObject(Request.ToSharedRef(), AllowedFields, TEXT("params"), OutError) ||
        !ReadString(
            Request.ToSharedRef(),
            TEXT("graph_id"),
            TEXT("params.graph_id"),
            true,
            128,
            OutRequest.GraphId,
            OutError) ||
        !ReadString(
            Request.ToSharedRef(),
            TEXT("action_id"),
            TEXT("params.action_id"),
            true,
            47,
            OutRequest.ActionId,
            OutError) ||
        !ReadStringArray(
            Request.ToSharedRef(),
            TEXT("bindings"),
            TEXT("params.bindings"),
            32,
            48,
            OutRequest.BindingIds,
            OutError))
    {
        return false;
    }

    const TSharedPtr<FJsonValue>* PositionValue =
        FindField(Request.ToSharedRef(), TEXT("position"));
    if (!PositionValue)
    {
        return SetInvalid(
            OutError,
            TEXT("params.position"),
            TEXT("Required position object is missing."));
    }
    if (!PositionValue->IsValid() || (*PositionValue)->Type != EJson::Object)
    {
        return SetInvalid(
            OutError,
            TEXT("params.position"),
            TEXT("Expected a JSON object."));
    }
    const TSharedRef<FJsonObject> Position =
        (*PositionValue)->AsObject().ToSharedRef();
    static const TSet<FString> PositionFields = {TEXT("x"), TEXT("y")};
    return ValidateClosedObject(
            Position, PositionFields, TEXT("params.position"), OutError) &&
        ReadFinitePositionCoordinate(
            Position, TEXT("x"), OutRequest.PositionX, OutError) &&
        ReadFinitePositionCoordinate(
            Position, TEXT("y"), OutRequest.PositionY, OutError);
}

bool ParseSuggestionRequest(
    const FString& RequestJson,
    FPaletteRequest& OutRequest,
    FString& OutPinId,
    FError& OutError)
{
    TSharedPtr<FJsonObject> Request;
    if (!ParseObject(RequestJson, Request, OutError))
    {
        return false;
    }
    static const TSet<FString> AllowedFields = {
        TEXT("graph_id"), TEXT("pin_id"), TEXT("query"), TEXT("cursor"),
        TEXT("limit")};
    if (!ValidateClosedObject(Request.ToSharedRef(), AllowedFields, TEXT("params"), OutError) ||
        !ReadString(
            Request.ToSharedRef(),
            TEXT("graph_id"),
            TEXT("params.graph_id"),
            true,
            128,
            OutRequest.GraphId,
            OutError) ||
        !ReadString(
            Request.ToSharedRef(),
            TEXT("pin_id"),
            TEXT("params.pin_id"),
            true,
            128,
            OutPinId,
            OutError) ||
        !ReadString(
            Request.ToSharedRef(),
            TEXT("query"),
            TEXT("params.query"),
            false,
            256,
            OutRequest.Query,
            OutError) ||
        !ReadString(
            Request.ToSharedRef(),
            TEXT("cursor"),
            TEXT("params.cursor"),
            false,
            4096,
            OutRequest.Cursor,
            OutError))
    {
        return false;
    }
    const TSharedPtr<FJsonValue>* Limit = FindField(
        Request.ToSharedRef(), TEXT("limit"));
    if (Limit)
    {
        if (!Limit->IsValid() || (*Limit)->Type != EJson::Number)
        {
            return SetInvalid(OutError, TEXT("params.limit"), TEXT("Expected an integer."));
        }
        const double Number = (*Limit)->AsNumber();
        if (!FMath::IsFinite(Number) || FMath::FloorToDouble(Number) != Number ||
            Number < 1.0 || Number > 200.0)
        {
            return SetInvalid(
                OutError,
                TEXT("params.limit"),
                TEXT("limit must be an integer from 1 through 200."));
        }
        OutRequest.Limit = static_cast<int32>(Number);
    }
    OutRequest.FiltersJson = TEXT("{}");
    return true;
}

bool ParseStoredFilters(
    const FString& FiltersJson,
    FPaletteFilters& OutFilters,
    FError& OutError)
{
    TSharedPtr<FJsonObject> Filters;
    if (!ParseObject(FiltersJson, Filters, OutError))
    {
        return false;
    }
    return ParseFilters(Filters.ToSharedRef(), OutFilters, OutError);
}

bool ResolveStableGraph(
    UBlueprint* Blueprint,
    const FString& GraphId,
    UEdGraph*& OutGraph,
    FError& OutError,
    const bool bStaleIsPrecondition)
{
    OutGraph = nullptr;
    FTargetRef Target;
    Target.Id = GraphId;
    FString ResolveError;
    const FResolvedTarget Resolved = ResolveTarget(
        Blueprint, ETargetKind::Graph, Target, ResolveError);
    if (!Resolved.Graph || !Resolved.bStable)
    {
        if (bStaleIsPrecondition)
        {
            return SetPrecondition(
                OutError,
                TEXT("params.action_id"),
                TEXT("The action's stable graph no longer exists."),
                TEXT("Repeat palette search for the current graph."));
        }
        return SetInvalid(
            OutError,
            TEXT("params.graph_id"),
            ResolveError.IsEmpty()
                ? TEXT("A stable graph_id is required.")
                : ResolveError,
            TEXT("Inspect the Blueprint and use its current stable graph ID."));
    }
    if (!Resolved.Graph->GetSchema() ||
        !Resolved.Graph->GetSchema()->IsA<UEdGraphSchema_K2>())
    {
        return SetPrecondition(
            OutError,
            bStaleIsPrecondition ? TEXT("params.action_id") : TEXT("params.graph_id"),
            TEXT("Blueprint palette actions require a K2 graph schema."),
            TEXT("Choose a K2 Blueprint graph and repeat palette search."));
    }
    OutGraph = Resolved.Graph;
    return true;
}

bool ResolveActionSourcePin(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FString& SourcePinId,
    UEdGraphPin*& OutPin,
    FError& OutError)
{
    OutPin = nullptr;
    if (SourcePinId.IsEmpty())
    {
        return true;
    }
    FTargetRef Target;
    Target.Id = SourcePinId;
    FString ResolveError;
    const FResolvedTarget Resolved = ResolveTarget(
        Blueprint, ETargetKind::Pin, Target, ResolveError);
    if (!Resolved.Pin || !Resolved.bStable || !Resolved.Node ||
        Resolved.Node->GetGraph() != Graph)
    {
        return SetPrecondition(
            OutError,
            TEXT("params.action_id"),
            TEXT("The source pin bound to action_id no longer exists in the requested graph."),
            TEXT("Request new pin suggestions and use a current action_id."));
    }
    OutPin = Resolved.Pin;
    return true;
}

UObject* ResolveBindingObject(const FString& ObjectPath)
{
    if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
    {
        return Existing;
    }
    return LoadObject<UObject>(nullptr, *ObjectPath);
}

FProperty* ResolveBindingField(const FString& ObjectPath)
{
    int32 Separator = INDEX_NONE;
    if (!ObjectPath.FindLastChar(TEXT(':'), Separator) ||
        Separator <= 0 || Separator >= ObjectPath.Len() - 1)
    {
        return nullptr;
    }
    const FString OwnerPath = ObjectPath.Left(Separator);
    const FName FieldName(*ObjectPath.Mid(Separator + 1));
    UStruct* Owner = FindObject<UStruct>(nullptr, *OwnerPath);
    if (!Owner)
    {
        Owner = LoadObject<UStruct>(nullptr, *OwnerPath);
    }
    return Owner ? FindFProperty<FProperty>(Owner, FieldName) : nullptr;
}

bool ResolveBindingObjects(
    const TArray<FPaletteBindingRecord>& Records,
    IBlueprintNodeBinder::FBindingSet& OutBindings,
    FError& OutError)
{
    OutBindings.Reset();
    for (int32 Index = 0; Index < Records.Num(); ++Index)
    {
        const FPaletteBindingRecord& Record = Records[Index];
        const FString Path = FString::Printf(TEXT("params.bindings[%d]"), Index);
        if (UObject* Object = ResolveBindingObject(Record.ObjectPath))
        {
            if (Object->GetClass()->GetPathName() != Record.ExpectedClassPath)
            {
                return SetPrecondition(
                    OutError,
                    Path,
                    TEXT("The binding object class has changed."),
                    TEXT("Repeat palette search and use its current binding IDs."));
            }
            OutBindings.Add(FBindingObject(Object));
            continue;
        }
        if (FProperty* Field = ResolveBindingField(Record.ObjectPath))
        {
            if (Record.ExpectedClassPath != TEXT("/Script/CoreUObject.Field") &&
                Record.ExpectedClassPath != Field->GetClass()->GetName())
            {
                return SetPrecondition(
                    OutError,
                    Path,
                    TEXT("The binding field class has changed."),
                    TEXT("Repeat palette search and use its current binding IDs."));
            }
            OutBindings.Add(FBindingObject(Field));
            continue;
        }
        return SetPrecondition(
            OutError,
            Path,
            TEXT("The object or field bound to this binding ID no longer exists."),
            TEXT("Repeat palette search and use its current binding IDs."));
    }
    return true;
}

bool ResolveDynamicBindingObjects(
    const FString& ActionId,
    const TArray<FString>& BindingIds,
    TArray<FPaletteBindingRecord>& OutRecords,
    IBlueprintNodeBinder::FBindingSet& OutBindings,
    FError& OutError)
{
    return ResolvePaletteBindings(
            ActionId, BindingIds, OutRecords, OutError) &&
        ResolveBindingObjects(OutRecords, OutBindings, OutError);
}

UEdGraphNode* GetBoundTemplateNode(
    const FPaletteCandidate& Candidate,
    UEdGraph* Graph,
    const IBlueprintNodeBinder::FBindingSet& Bindings)
{
    return Candidate.Spawner
        ? Candidate.Spawner->GetTemplateNode(Graph, Bindings)
        : nullptr;
}

FString Normalize(const FString& Value)
{
    return Value.TrimStartAndEnd().ToLower();
}

FString MemberPath(FBlueprintActionInfo& Action)
{
    const FFieldVariant Member = Action.GetAssociatedMemberField();
    if (!Member.IsValid())
    {
        return FString();
    }
    if (Member.IsUObject())
    {
        const UObject* Object = Member.ToUObject();
        return Object ? Object->GetPathName() : FString();
    }
    const FField* Field = Member.ToField();
    return Field ? Field->GetPathName() : FString();
}

bool HasInternalMetadata(const UField* Field)
{
    if (!Field)
    {
        return false;
    }
    const UStruct* Struct = Cast<UStruct>(Field);
    return Field->GetBoolMetaData(
            FBlueprintMetadata::MD_BlueprintInternalUseOnly) ||
        (Struct && Struct->GetBoolMetaDataHierarchical(
            FBlueprintMetadata::MD_BlueprintInternalUseOnlyHierarchical));
}

bool IsUnsafeAction(
    FBlueprintActionInfo& Action,
    FBlueprintActionFilter& Filter)
{
    const UClass* NodeClass = Action.GetNodeClass();
    if (!NodeClass || NodeClass->HasAnyClassFlags(
            CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists |
            CLASS_Hidden))
    {
        return true;
    }
    if (HasInternalMetadata(NodeClass) ||
        Action.NodeSpawner->IsTemplateNodeFilteredOut(Filter))
    {
        return true;
    }
    if (const UClass* OwnerClass = Action.GetOwnerClass();
        OwnerClass && (OwnerClass->HasAnyClassFlags(
            CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden) ||
            HasInternalMetadata(OwnerClass)))
    {
        return true;
    }
    if (const UFunction* Function = Action.GetAssociatedFunction();
        Function && (
            Function->HasMetaData(FBlueprintMetadata::MD_DeprecatedFunction) ||
            Function->GetBoolMetaData(
                FBlueprintMetadata::MD_BlueprintInternalUseOnly)))
    {
        return true;
    }
    if (const FProperty* Property = Action.GetAssociatedProperty();
        Property && (
            Property->HasAnyPropertyFlags(CPF_Deprecated) ||
            Property->GetBoolMetaData(
                FBlueprintMetadata::MD_BlueprintInternalUseOnly)))
    {
        return true;
    }
    const UObject* Owner = Action.GetActionOwner();
    if (const UScriptStruct* Struct = Cast<UScriptStruct>(Owner);
        Struct && HasInternalMetadata(Struct))
    {
        return true;
    }
    return false;
}

FString ClassifyAction(FBlueprintActionInfo& Action)
{
    const UClass* NodeClass = Action.GetNodeClass();
    const UFunction* Function = Action.GetAssociatedFunction();
    const FString ClassName = NodeClass ? NodeClass->GetName() : FString();

    if ((NodeClass && NodeClass->IsChildOf<UK2Node_BaseAsyncTask>()) ||
        (Function && Function->HasMetaData(FBlueprintMetadata::MD_Latent)) ||
        ClassName.Contains(TEXT("Async"), ESearchCase::IgnoreCase))
    {
        return TEXT("async");
    }
    if (NodeClass && NodeClass->IsChildOf<UK2Node_Event>())
    {
        return TEXT("event");
    }
    if (NodeClass && NodeClass->IsChildOf<UK2Node_Variable>())
    {
        return TEXT("variable");
    }
    if (NodeClass && NodeClass->IsChildOf<UK2Node_MacroInstance>())
    {
        return TEXT("macro");
    }
    if (ClassName.Contains(TEXT("Delegate"), ESearchCase::IgnoreCase) ||
        Action.GetAssociatedProperty() &&
            Action.GetAssociatedProperty()->IsA<FMulticastDelegateProperty>())
    {
        return TEXT("delegate");
    }
    if ((NodeClass && NodeClass->IsChildOf<UK2Node_DynamicCast>()) ||
        ClassName.Contains(TEXT("DynamicCast"), ESearchCase::IgnoreCase))
    {
        return TEXT("cast");
    }
    static const TArray<FString> FlowMarkers = {
        TEXT("IfThenElse"), TEXT("ExecutionSequence"), TEXT("MultiGate"),
        TEXT("DoOnce"), TEXT("Gate"), TEXT("Switch"), TEXT("ForEach")};
    for (const FString& Marker : FlowMarkers)
    {
        if (ClassName.Contains(Marker, ESearchCase::IgnoreCase))
        {
            return TEXT("flow_control");
        }
    }
    if (ClassName.Contains(TEXT("Operator"), ESearchCase::IgnoreCase) ||
        (Function && Function->HasMetaData(FBlueprintMetadata::MD_CompactNodeTitle)))
    {
        return TEXT("operator");
    }
    if ((NodeClass && NodeClass->IsChildOf<UK2Node_StructOperation>()) ||
        Cast<UScriptStruct>(Action.GetActionOwner()))
    {
        return TEXT("struct");
    }
    if (Function)
    {
        return TEXT("function");
    }
    return TEXT("other");
}

void ParseKeywords(const FString& Text, TArray<FString>& OutKeywords)
{
    TArray<FString> Tokens;
    Text.ParseIntoArrayWS(Tokens);
    TSet<FString> Seen;
    for (FString Token : Tokens)
    {
        Token.TrimStartAndEndInline();
        if (Token.IsEmpty() || Seen.Contains(Token) || OutKeywords.Num() >= 128)
        {
            continue;
        }
        Seen.Add(Token);
        OutKeywords.Add(MoveTemp(Token));
    }
}

bool MatchesFilters(
    const FPaletteCandidate& Candidate,
    const FPaletteFilters& Filters)
{
    if (!Filters.ActionKinds.IsEmpty() &&
        !Filters.ActionKinds.Contains(Candidate.ActionKind))
    {
        return false;
    }
    if (!Filters.OwnerPaths.IsEmpty() &&
        !Filters.OwnerPaths.Contains(Candidate.OwnerPath))
    {
        return false;
    }
    if (Filters.bPureOnly &&
        (!Candidate.bPure.IsSet() || !Candidate.bPure.GetValue()))
    {
        return false;
    }
    if (!Filters.CategoryPrefixes.IsEmpty())
    {
        const FString Category = Normalize(Candidate.Category);
        bool bMatchesCategory = false;
        for (const FString& Prefix : Filters.CategoryPrefixes)
        {
            if (Category.StartsWith(Normalize(Prefix)))
            {
                bMatchesCategory = true;
                break;
            }
        }
        if (!bMatchesCategory)
        {
            return false;
        }
    }
    return true;
}

int32 ScoreCandidate(
    const FPaletteCandidate& Candidate,
    const FString& Query,
    bool& bMatches)
{
    const FString NormalQuery = Normalize(Query);
    if (NormalQuery.IsEmpty())
    {
        bMatches = true;
        return 0;
    }
    const FString Title = Normalize(Candidate.Title);
    const FString Category = Normalize(Candidate.Category);
    const FString Keywords = Normalize(FString::Join(Candidate.Keywords, TEXT(" ")));
    const FString OwnerMember = Normalize(
        Candidate.OwnerPath + TEXT(" ") + Candidate.MemberPath);
    const FString Other = Category + TEXT(" ") + Keywords + TEXT(" ") + OwnerMember;

    TArray<FString> QueryTokens;
    NormalQuery.ParseIntoArrayWS(QueryTokens);
    for (const FString& Token : QueryTokens)
    {
        if (!Title.Contains(Token) && !Other.Contains(Token))
        {
            bMatches = false;
            return 0;
        }
    }

    int32 Score = 0;
    if (Title == NormalQuery)
    {
        Score += 1000;
    }
    else if (Title.StartsWith(NormalQuery))
    {
        Score += 800;
    }
    else if (Title.Contains(NormalQuery))
    {
        Score += 600;
    }
    if (Keywords.Contains(NormalQuery))
    {
        Score += 400;
    }
    if (Category.Contains(NormalQuery))
    {
        Score += 300;
    }
    if (OwnerMember.Contains(NormalQuery))
    {
        Score += 200;
    }
    for (const FString& Token : QueryTokens)
    {
        Score += Title.Contains(Token) ? 20 : 10;
    }
    bMatches = true;
    return Score;
}

void BuildBindingDetails(FPaletteCandidate& Candidate)
{
    Candidate.BindingDetails.Reset();
    for (const FBindingObject& Binding : Candidate.Bindings)
    {
        if (!Binding.IsValid())
        {
            continue;
        }
        FPaletteBindingCandidate Detail;
        Detail.ObjectPath = Binding.GetPathName();
        if (Binding.IsUObject())
        {
            if (const UObject* Object = Binding.Get<UObject>())
            {
                Detail.ClassPath = Object->GetClass()->GetPathName();
            }
        }
        else
        {
            Detail.ClassPath = TEXT("/Script/CoreUObject.Field");
        }
        if (!Detail.ObjectPath.IsEmpty())
        {
            Candidate.BindingDetails.Add(MoveTemp(Detail));
        }
    }
    Candidate.BindingDetails.Sort(
        [](const FPaletteBindingCandidate& Left, const FPaletteBindingCandidate& Right)
        {
            if (Left.ObjectPath != Right.ObjectPath)
            {
                return Left.ObjectPath < Right.ObjectPath;
            }
            return Left.ClassPath < Right.ClassPath;
        });
}

bool BuildCandidates(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    TConstArrayView<UEdGraphPin*> ContextPins,
    const FString& Query,
    const FPaletteFilters& Filters,
    TArray<FPaletteCandidate>& OutCandidates)
{
    OutCandidates.Reset();
    const FBlueprintActionFilter::EFlags FilterFlags =
        static_cast<FBlueprintActionFilter::EFlags>(
            FBlueprintActionFilter::BPFILTER_RejectNonImportedFields |
            FBlueprintActionFilter::BPFILTER_RejectIncompatibleThreadSafety);
    FBlueprintActionFilter NativeFilter(FilterFlags);
    NativeFilter.Context.Blueprints.Add(Blueprint);
    NativeFilter.Context.Graphs.Add(Graph);
    for (UEdGraphPin* ContextPin : ContextPins)
    {
        if (!ContextPin)
        {
            continue;
        }
        NativeFilter.Context.Pins.Add(ContextPin);
        if (UClass* PinClass = Cast<UClass>(
                ContextPin->PinType.PinSubCategoryObject.Get()))
        {
            FBlueprintActionFilter::AddUnique(
                NativeFilter.TargetClasses, PinClass);
        }
        const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(
            Graph->GetSchema());
        UEdGraphNode* OwningNode = ContextPin->GetOwningNodeUnchecked();
        if (K2Schema && OwningNode)
        {
            if (UEdGraphPin* SelfPin = K2Schema->FindSelfPin(
                    *OwningNode, EGPD_Input))
            {
                if (UClass* SelfClass = Cast<UClass>(
                        SelfPin->PinType.PinSubCategoryObject.Get()))
                {
                    FBlueprintActionFilter::AddUnique(
                        NativeFilter.TargetClasses, SelfClass);
                }
            }
        }
    }
    if (Blueprint->SkeletonGeneratedClass)
    {
        FBlueprintActionFilter::AddUnique(
            NativeFilter.TargetClasses, Blueprint->SkeletonGeneratedClass);
    }
    if (Blueprint->GeneratedClass)
    {
        FBlueprintActionFilter::AddUnique(
            NativeFilter.TargetClasses, Blueprint->GeneratedClass);
    }

    TSet<FString> SeenCandidateKeys;
    const FBlueprintActionDatabase::FActionRegistry& Registry =
        FBlueprintActionDatabase::Get().GetAllActions();
    for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Entry : Registry)
    {
        const UObject* Owner = Entry.Key.ResolveObjectPtr();
        if (!Owner)
        {
            continue;
        }
        for (const UBlueprintNodeSpawner* Spawner : Entry.Value)
        {
            if (!Spawner || !Spawner->NodeClass)
            {
                continue;
            }
            FBlueprintActionInfo Action(Owner, Spawner);
            if (NativeFilter.IsFiltered(Action) || IsUnsafeAction(Action, NativeFilter))
            {
                continue;
            }
            const FBlueprintNodeSignature Signature = Spawner->GetSpawnerSignature();
            if (!Signature.IsValid() || Signature.ToString().IsEmpty())
            {
                continue;
            }

            FPaletteCandidate Candidate;
            Candidate.Owner = Owner;
            Candidate.Spawner = Spawner;
            Candidate.Bindings = Action.GetBindings();
            BuildBindingDetails(Candidate);
            Candidate.SpawnerSignature = Signature.ToString();
            Candidate.OwnerPath = Owner->GetPathName();
            Candidate.MemberPath = MemberPath(Action);
            Candidate.NodeClassPath = Spawner->NodeClass->GetPathName();
            Candidate.ActionKind = ClassifyAction(Action);
            if (const UFunction* Function = Action.GetAssociatedFunction())
            {
                Candidate.bPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
            }

            TArray<FString> BindingPaths;
            for (const FPaletteBindingCandidate& Binding : Candidate.BindingDetails)
            {
                BindingPaths.Add(Binding.ObjectPath);
            }
            Candidate.CandidateKey = Candidate.OwnerPath + TEXT("\n") +
                Candidate.SpawnerSignature + TEXT("\n") +
                FString::Join(BindingPaths, TEXT("\n"));
            if (SeenCandidateKeys.Contains(Candidate.CandidateKey))
            {
                continue;
            }

            const FBlueprintActionUiSpec UiSpec = Spawner->GetUiSpec(
                NativeFilter.Context, Candidate.Bindings);
            Candidate.Title = UiSpec.MenuName.ToString().TrimStartAndEnd();
            Candidate.Category = UiSpec.Category.ToString().TrimStartAndEnd();
            Candidate.Tooltip = UiSpec.Tooltip.ToString();
            Candidate.DocumentationLink = UiSpec.DocLink;
            Candidate.DocumentationExcerpt = UiSpec.DocExcerptTag;
            ParseKeywords(UiSpec.Keywords.ToString(), Candidate.Keywords);
            if (Candidate.Title.IsEmpty() || !MatchesFilters(Candidate, Filters))
            {
                continue;
            }
            bool bQueryMatches = false;
            Candidate.Score = ScoreCandidate(Candidate, Query, bQueryMatches);
            if (!bQueryMatches)
            {
                continue;
            }
            Candidate.SortKey = FString::Printf(
                TEXT("%07d\n%s\n%s\n%s"),
                1000000 - Candidate.Score,
                *Normalize(Candidate.Category),
                *Normalize(Candidate.Title),
                *Candidate.CandidateKey);
            SeenCandidateKeys.Add(Candidate.CandidateKey);
            OutCandidates.Add(MoveTemp(Candidate));
        }
    }
    OutCandidates.Sort(
        [](const FPaletteCandidate& Left, const FPaletteCandidate& Right)
        {
            return Left.SortKey < Right.SortKey;
        });
    return true;
}

bool BuildCandidatesForSourcePin(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UEdGraphPin* SourcePin,
    const FString& Query,
    const FPaletteFilters& Filters,
    TArray<FPaletteCandidate>& OutCandidates)
{
    if (!SourcePin)
    {
        return BuildCandidates(
            Blueprint,
            Graph,
            TConstArrayView<UEdGraphPin*>(),
            Query,
            Filters,
            OutCandidates);
    }
    UEdGraphPin* ContextPins[] = {SourcePin};
    return BuildCandidates(
        Blueprint,
        Graph,
        MakeArrayView(ContextPins),
        Query,
        Filters,
        OutCandidates);
}

FString ResultDigest(const TArray<FPaletteCandidate>& Candidates)
{
    TArray<TSharedPtr<FJsonValue>> Keys;
    Keys.Reserve(Candidates.Num());
    for (const FPaletteCandidate& Candidate : Candidates)
    {
        Keys.Add(MakeShared<FJsonValueString>(Candidate.CandidateKey));
    }
    return TEXT("sha1:") + Sha1Hex(CanonicalJsonString(
        MakeShared<FJsonValueArray>(Keys)));
}

TArray<FString> CandidateBindingPaths(const FPaletteCandidate& Candidate)
{
    TArray<FString> Paths;
    for (const FPaletteBindingCandidate& Binding : Candidate.BindingDetails)
    {
        Paths.Add(Binding.ObjectPath);
    }
    return Paths;
}

const FPaletteCandidate* FindExactCandidate(
    const TArray<FPaletteCandidate>& Candidates,
    const FPaletteActionRecord& Record)
{
    return Candidates.FindByPredicate(
        [&Record](const FPaletteCandidate& Item)
        {
            return Item.CandidateKey == Record.CandidateKey &&
                Item.SpawnerSignature == Record.SpawnerSignature &&
                Item.OwnerPath == Record.OwnerPath &&
                CandidateBindingPaths(Item) == Record.BindingPaths;
        });
}

FString RequestDigest(const FPaletteRequest& Request)
{
    TSharedPtr<FJsonObject> Filters;
    FError Ignored;
    ParseObject(Request.FiltersJson, Filters, Ignored);
    const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
    Value->SetStringField(TEXT("query"), Request.Query);
    Value->SetObjectField(
        TEXT("filters"), Filters.IsValid() ? Filters : MakeShared<FJsonObject>());
    return TEXT("sha1:") + CanonicalQueryDigest(Value);
}

FPaletteContext MakeContext(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FPaletteRequest& Request,
    const FString& Digest,
    const FString& SourcePinId = FString())
{
    FPaletteContext Context;
    Context.AssetPath = Blueprint->GetPathName();
    Context.GraphId = Request.GraphId;
    Context.GraphSchemaPath = Graph->GetSchema()->GetClass()->GetPathName();
    Context.Kind = SourcePinId.IsEmpty()
        ? EPaletteContextKind::Graph
        : EPaletteContextKind::Pin;
    Context.SourcePinId = SourcePinId;
    Context.RequestDigest = RequestDigest(Request);
    Context.ResultDigest = Digest;
    Context.Limit = Request.Limit;
    return Context;
}

TSharedRef<FJsonObject> SerializeBinding(
    const FString& ActionId,
    const FPaletteBindingCandidate& Binding)
{
    FPaletteBindingRecord Record;
    Record.ActionId = ActionId;
    Record.ObjectPath = Binding.ObjectPath;
    Record.ExpectedClassPath = Binding.ClassPath;
    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("binding_id"), RegisterPaletteBinding(Record));
    Result->SetStringField(TEXT("object_path"), Binding.ObjectPath);
    Result->SetStringField(TEXT("class_path"), Binding.ClassPath);
    return Result;
}

TSharedRef<FJsonObject> SerializeCard(
    const FPaletteCandidate& Candidate,
    const FPaletteContext& Context,
    const FString& Query,
    const FString& FiltersJson,
    const FString& ExistingActionId = FString())
{
    FPaletteActionRecord Record;
    Record.Context = Context;
    Record.Query = Query;
    Record.FiltersJson = FiltersJson;
    Record.CandidateKey = Candidate.CandidateKey;
    Record.SpawnerSignature = Candidate.SpawnerSignature;
    Record.OwnerPath = Candidate.OwnerPath;
    Record.SortKey = Candidate.SortKey;
    for (const FPaletteBindingCandidate& Binding : Candidate.BindingDetails)
    {
        Record.BindingPaths.Add(Binding.ObjectPath);
    }
    const FString ActionId = ExistingActionId.IsEmpty()
        ? RegisterPaletteActionToken(Record)
        : ExistingActionId;

    const TSharedRef<FJsonObject> Card = MakeShared<FJsonObject>();
    Card->SetStringField(TEXT("action_id"), ActionId);
    Card->SetStringField(TEXT("title"), Candidate.Title);
    Card->SetStringField(TEXT("category"), Candidate.Category);
    TArray<TSharedPtr<FJsonValue>> Keywords;
    for (const FString& Keyword : Candidate.Keywords)
    {
        Keywords.Add(MakeShared<FJsonValueString>(Keyword));
    }
    Card->SetArrayField(TEXT("keywords"), MoveTemp(Keywords));
    Card->SetStringField(TEXT("action_kind"), Candidate.ActionKind);
    Card->SetStringField(TEXT("node_class_path"), Candidate.NodeClassPath);
    Card->SetStringField(TEXT("owner_path"), Candidate.OwnerPath);
    Card->SetStringField(TEXT("member_path"), Candidate.MemberPath);
    if (Candidate.bPure.IsSet())
    {
        Card->SetBoolField(TEXT("pure"), Candidate.bPure.GetValue());
    }
    else
    {
        Card->SetField(TEXT("pure"), MakeShared<FJsonValueNull>());
    }
    Card->SetBoolField(TEXT("compatible"), true);
    Card->SetStringField(
        TEXT("compatibility_summary"),
        Context.SourcePinId.IsEmpty()
            ? TEXT("Available in the requested graph context.")
            : TEXT("Compatible according to the current native action filter."));
    Card->SetBoolField(
        TEXT("requires_binding"), !Candidate.BindingDetails.IsEmpty());
    TArray<TSharedPtr<FJsonValue>> Bindings;
    for (const FPaletteBindingCandidate& Binding : Candidate.BindingDetails)
    {
        Bindings.Add(MakeShared<FJsonValueObject>(
            SerializeBinding(ActionId, Binding)));
    }
    Card->SetArrayField(TEXT("bindings"), MoveTemp(Bindings));
    return Card;
}

struct FConnectionBindingSuggestion
{
    FString BindingId;
    FString PinName;
    FString Direction;
    FString TypeJson;
    TSharedRef<FJsonObject> Type = MakeShared<FJsonObject>();
    FString ResponseKind;
    FString ResponseMessage;
    bool bRequiresConversion = false;
    int32 Occurrence = 0;
};

bool ClassifyConnectionResponse(
    const FPinConnectionResponse& Response,
    FString& OutKind,
    bool& bOutRequiresConversion)
{
    bOutRequiresConversion = false;
    switch (Response.Response)
    {
    case CONNECT_RESPONSE_MAKE:
        OutKind = TEXT("direct");
        return true;
    case CONNECT_RESPONSE_BREAK_OTHERS_A:
        OutKind = TEXT("break_planned_source_link");
        return true;
    case CONNECT_RESPONSE_BREAK_OTHERS_B:
        OutKind = TEXT("break_planned_target_link");
        return true;
    case CONNECT_RESPONSE_BREAK_OTHERS_AB:
        OutKind = TEXT("break_planned_both_links");
        return true;
    case CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE:
        OutKind = TEXT("conversion_node");
        bOutRequiresConversion = true;
        return true;
    case CONNECT_RESPONSE_MAKE_WITH_PROMOTION:
        OutKind = TEXT("promotion");
        bOutRequiresConversion = true;
        return true;
    default:
        return false;
    }
}

void BuildConnectionBindingSuggestions(
    const FPaletteCandidate& Candidate,
    UEdGraph* Graph,
    UEdGraphPin* SourcePin,
    TArray<FConnectionBindingSuggestion>& OutSuggestions)
{
    OutSuggestions.Reset();
    UEdGraphNode* TemplateNode = GetBoundTemplateNode(
        Candidate, Graph, Candidate.Bindings);
    const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
    TMap<FString, int32> Occurrences;
    if (TemplateNode && SourcePin && Schema)
    {
        for (UEdGraphPin* TemplatePin : TemplateNode->Pins)
        {
            if (!TemplatePin || TemplatePin->bHidden ||
                TemplatePin->Direction == SourcePin->Direction ||
                OutSuggestions.Num() >= 256)
            {
                continue;
            }
            const FPinConnectionResponse Response =
                Schema->CanCreateConnection(SourcePin, TemplatePin);
            FString ResponseKind;
            bool bRequiresConversion = false;
            if (!ClassifyConnectionResponse(
                    Response, ResponseKind, bRequiresConversion))
            {
                continue;
            }

            FConnectionBindingSuggestion Suggestion;
            Suggestion.PinName = TemplatePin->PinName.ToString();
            Suggestion.Direction = TemplatePin->Direction == EGPD_Input
                ? TEXT("input")
                : TEXT("output");
            Suggestion.Type = SerializeTypeSpec(TemplatePin->PinType);
            Suggestion.TypeJson = CanonicalJsonString(
                MakeShared<FJsonValueObject>(Suggestion.Type));
            Suggestion.ResponseKind = MoveTemp(ResponseKind);
            Suggestion.ResponseMessage = Response.Message.ToString();
            Suggestion.bRequiresConversion = bRequiresConversion;
            const FString Signature = Suggestion.PinName + TEXT("\n") +
                Suggestion.Direction + TEXT("\n") + Suggestion.TypeJson;
            Suggestion.Occurrence = Occurrences.FindOrAdd(Signature)++;

            OutSuggestions.Add(MoveTemp(Suggestion));
        }
    }
}

bool AddConnectionBindings(
    const FString& ActionId,
    TArray<FConnectionBindingSuggestion>& Suggestions,
    const TSharedRef<FJsonObject>& Card)
{
    for (FConnectionBindingSuggestion& Suggestion : Suggestions)
    {
        FPaletteBindingRecord Binding;
        Binding.Kind = EPaletteBindingKind::TemplatePin;
        Binding.ActionId = ActionId;
        Binding.PinName = Suggestion.PinName;
        Binding.PinDirection = Suggestion.Direction;
        Binding.PinTypeJson = Suggestion.TypeJson;
        Binding.PinOccurrence = Suggestion.Occurrence;
        Suggestion.BindingId = RegisterPaletteTemplatePinBinding(Binding);
        if (Suggestion.BindingId.IsEmpty())
        {
            return false;
        }
    }

    Suggestions.Sort([](
        const FConnectionBindingSuggestion& Left,
        const FConnectionBindingSuggestion& Right)
    {
        if (Left.bRequiresConversion != Right.bRequiresConversion)
        {
            return !Left.bRequiresConversion;
        }
        const FString LeftName = Normalize(Left.PinName);
        const FString RightName = Normalize(Right.PinName);
        if (LeftName != RightName)
        {
            return LeftName < RightName;
        }
        if (Left.TypeJson != Right.TypeJson)
        {
            return Left.TypeJson < Right.TypeJson;
        }
        if (Left.Occurrence != Right.Occurrence)
        {
            return Left.Occurrence < Right.Occurrence;
        }
        return Left.BindingId < Right.BindingId;
    });

    TArray<TSharedPtr<FJsonValue>> Values;
    Values.Reserve(Suggestions.Num());
    for (int32 Index = 0; Index < Suggestions.Num(); ++Index)
    {
        const FConnectionBindingSuggestion& Suggestion = Suggestions[Index];
        const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
        Response->SetStringField(TEXT("kind"), Suggestion.ResponseKind);
        Response->SetStringField(TEXT("message"), Suggestion.ResponseMessage);
        Response->SetBoolField(
            TEXT("requires_conversion"), Suggestion.bRequiresConversion);
        const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
        Value->SetStringField(TEXT("binding_id"), Suggestion.BindingId);
        Value->SetStringField(TEXT("pin_name"), Suggestion.PinName);
        Value->SetStringField(TEXT("direction"), Suggestion.Direction);
        Value->SetObjectField(TEXT("type"), Suggestion.Type);
        Value->SetObjectField(TEXT("response"), Response);
        Value->SetNumberField(TEXT("rank"), Index);
        Values.Add(MakeShared<FJsonValueObject>(Value));
    }
    Card->SetArrayField(TEXT("connection_bindings"), MoveTemp(Values));
    return true;
}

void SetSourcePinField(
    const TSharedRef<FJsonObject>& Object,
    const FString& SourcePinId)
{
    if (SourcePinId.IsEmpty())
    {
        Object->SetField(TEXT("source_pin_id"), MakeShared<FJsonValueNull>());
    }
    else
    {
        Object->SetStringField(TEXT("source_pin_id"), SourcePinId);
    }
}

TSharedRef<FJsonObject> SearchPalette(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UEdGraphPin* SourcePin,
    const FString& SourcePinId,
    const FPaletteRequest& Request,
    FError& OutError)
{
    TArray<FPaletteCandidate> Candidates;
    BuildCandidatesForSourcePin(
        Blueprint, Graph, SourcePin, Request.Query, Request.Filters, Candidates);
    const FString Digest = ResultDigest(Candidates);
    const FPaletteContext Context = MakeContext(
        Blueprint, Graph, Request, Digest, SourcePinId);

    int32 StartIndex = 0;
    if (!Request.Cursor.IsEmpty())
    {
        FPaletteCursorRecord CursorRecord;
        if (!ResolvePaletteCursor(
                Request.Cursor, Context, CursorRecord, OutError))
        {
            return MakeShared<FJsonObject>();
        }
        while (StartIndex < Candidates.Num() &&
            Candidates[StartIndex].SortKey <= CursorRecord.LastSortKey)
        {
            ++StartIndex;
        }
        if (StartIndex >= Candidates.Num())
        {
            SetPrecondition(
                OutError,
                TEXT("params.cursor"),
                TEXT("The cursor no longer identifies a continuation item."),
                TEXT("Restart palette search without a cursor."));
            return MakeShared<FJsonObject>();
        }
    }

    TArray<TSharedPtr<FJsonValue>> Items;
    Items.Reserve(FMath::Min(Request.Limit, Candidates.Num() - StartIndex));
    static constexpr int32 MaximumReturnedBindings = 1024;
    int32 ReturnedBindingCount = 0;
    int32 NextCandidateIndex = StartIndex;
    while (NextCandidateIndex < Candidates.Num() &&
        Items.Num() < Request.Limit)
    {
        const int32 Index = NextCandidateIndex;
        TArray<FConnectionBindingSuggestion> ConnectionBindings;
        if (SourcePin)
        {
            BuildConnectionBindingSuggestions(
                Candidates[Index], Graph, SourcePin, ConnectionBindings);
            if (ConnectionBindings.IsEmpty())
            {
                ++NextCandidateIndex;
                continue;
            }
        }

        const int32 CandidateBindingCount =
            Candidates[Index].BindingDetails.Num() + ConnectionBindings.Num();
        if (CandidateBindingCount > MaximumReturnedBindings)
        {
            ++NextCandidateIndex;
            continue;
        }
        if (ReturnedBindingCount + CandidateBindingCount >
            MaximumReturnedBindings)
        {
            break;
        }

        ++NextCandidateIndex;
        const TSharedRef<FJsonObject> Card = SerializeCard(
            Candidates[Index],
            Context,
            Request.Query,
            Request.FiltersJson);
        if (SourcePin)
        {
            if (!AddConnectionBindings(
                    Card->GetStringField(TEXT("action_id")),
                    ConnectionBindings,
                    Card))
            {
                OutError.Code = TEXT("OPERATION_FAILED");
                OutError.Path = TEXT("data.items");
                OutError.Message = TEXT(
                    "Failed to register one compatible template-pin binding.");
                OutError.Hint = TEXT(
                    "Repeat pin suggestions in the current editor session.");
                return MakeShared<FJsonObject>();
            }
        }
        ReturnedBindingCount += CandidateBindingCount;
        Items.Add(MakeShared<FJsonValueObject>(Card));
    }

    FString NextCursor;
    if (NextCandidateIndex < Candidates.Num() &&
        NextCandidateIndex > StartIndex)
    {
        FPaletteCursorRecord CursorRecord;
        CursorRecord.Context = Context;
        CursorRecord.LastSortKey =
            Candidates[NextCandidateIndex - 1].SortKey;
        NextCursor = RegisterPaletteCursor(CursorRecord);
    }

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("graph_id"), Request.GraphId);
    SetSourcePinField(Data, SourcePinId);
    const int32 ReturnedCount = Items.Num();
    Data->SetArrayField(TEXT("items"), MoveTemp(Items));
    Data->SetNumberField(TEXT("total_count"), Candidates.Num());
    Data->SetNumberField(TEXT("returned_count"), ReturnedCount);
    Data->SetStringField(TEXT("next_cursor"), NextCursor);
    Data->SetStringField(TEXT("result_digest"), Digest);
    return Data;
}

TSharedPtr<FJsonValue> TemplatePinDefault(const UEdGraphPin* Pin)
{
    if (!Pin)
    {
        return MakeShared<FJsonValueNull>();
    }
    TSharedPtr<FJsonValue> Result = SerializeDefaultValue(
        Pin->PinType,
        Pin->DefaultValue,
        Pin->DefaultObject,
        Pin->DefaultTextValue);
    return Result.IsValid() ? Result : MakeShared<FJsonValueNull>();
}

void AddTemplatePins(
    const FPaletteCandidate& Candidate,
    UEdGraph* Graph,
    const TSharedRef<FJsonObject>& Data)
{
    Candidate.Spawner->PrimeDefaultUiSpec(Graph);
    UEdGraphNode* TemplateNode = Candidate.Spawner->GetCachedTemplateNode();
    TArray<TSharedPtr<FJsonValue>> Pins;
    if (TemplateNode)
    {
        for (const UEdGraphPin* Pin : TemplateNode->Pins)
        {
            if (!Pin || Pin->bHidden || Pins.Num() >= 256)
            {
                continue;
            }
            const TSharedRef<FJsonObject> PinObject = MakeShared<FJsonObject>();
            PinObject->SetStringField(TEXT("name"), Pin->PinName.ToString());
            PinObject->SetStringField(
                TEXT("direction"),
                Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
            PinObject->SetObjectField(TEXT("type"), SerializeTypeSpec(Pin->PinType));
            PinObject->SetField(TEXT("default"), TemplatePinDefault(Pin));
            Pins.Add(MakeShared<FJsonValueObject>(PinObject));
        }
    }
    Data->SetBoolField(TEXT("pin_preview_available"), TemplateNode != nullptr);
    Data->SetArrayField(TEXT("template_pins"), MoveTemp(Pins));
}

FString RollbackPaletteFailure(
    FMutationScope& Scope,
    const FString& Code,
    const FString& Path,
    const FString& Message,
    const FString& Hint)
{
    const FRollbackResult Rollback = Scope.Rollback();
    if (!Rollback.bSucceeded && !Rollback.bDeferredToWorkflow)
    {
        return Failure(
            TEXT("ROLLBACK_FAILED"),
            TEXT("transaction"),
            TEXT("Blueprint palette mutation rollback failed."),
            TEXT("Inspect the graph before attempting another mutation."));
    }
    return Failure(Code, Path, Message, Hint);
}

bool GraphMatchesNodeSnapshot(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const TMap<UEdGraphNode*, FString>& Snapshot)
{
    if (!Blueprint || !Graph || Graph->Nodes.Num() != Snapshot.Num())
    {
        return false;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        const FString* ExpectedId = Node ? Snapshot.Find(Node) : nullptr;
        if (!ExpectedId || DescribeNodeTarget(Blueprint, Node).Id != *ExpectedId)
        {
            return false;
        }
    }
    return true;
}

TSharedRef<FJsonObject> SerializeSpawnPin(
    UBlueprint* Blueprint,
    const UEdGraphPin* Pin)
{
    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("id"), DescribePinTarget(Blueprint, Pin).Id);
    Result->SetStringField(TEXT("name"), Pin->PinName.ToString());
    Result->SetStringField(
        TEXT("direction"),
        Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
    Result->SetObjectField(TEXT("type"), SerializeTypeSpec(Pin->PinType));
    Result->SetField(TEXT("default"), TemplatePinDefault(Pin));
    return Result;
}

TSharedRef<FJsonObject> MakeCompileNextAction(UBlueprint* Blueprint)
{
    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(
        TEXT("asset_path"), Blueprint ? Blueprint->GetPathName() : FString());
    const TSharedRef<FJsonObject> NextAction = MakeShared<FJsonObject>();
    NextAction->SetStringField(TEXT("domain"), TEXT("blueprint"));
    NextAction->SetStringField(TEXT("action"), TEXT("compile_blueprint"));
    NextAction->SetObjectField(TEXT("params"), Params);
    return NextAction;
}

FString SpawnSuccess(
    UBlueprint* Blueprint,
    const FString& GraphId,
    const FString& ActionId,
    UEdGraphNode* Node,
    const TArray<TPair<FString, UEdGraphNode*>>& AuxiliaryNodes)
{
    const FString NodeId = DescribeNodeTarget(Blueprint, Node).Id;
    TArray<TSharedPtr<FJsonValue>> PinIds;
    TArray<TSharedPtr<FJsonValue>> Pins;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->bHidden)
        {
            continue;
        }
        const TSharedRef<FJsonObject> PinObject = SerializeSpawnPin(Blueprint, Pin);
        PinIds.Add(MakeShared<FJsonValueString>(
            PinObject->GetStringField(TEXT("id"))));
        Pins.Add(MakeShared<FJsonValueObject>(PinObject));
    }

    TArray<TSharedPtr<FJsonValue>> AuxiliaryIds;
    for (const TPair<FString, UEdGraphNode*>& Auxiliary : AuxiliaryNodes)
    {
        AuxiliaryIds.Add(MakeShared<FJsonValueString>(Auxiliary.Key));
    }
    const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
    Position->SetNumberField(TEXT("x"), Node->NodePosX);
    Position->SetNumberField(TEXT("y"), Node->NodePosY);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("graph_id"), GraphId);
    Data->SetStringField(TEXT("action_id"), ActionId);
    Data->SetStringField(TEXT("node_id"), NodeId);
    Data->SetStringField(TEXT("class_path"), Node->GetClass()->GetPathName());
    Data->SetObjectField(TEXT("position"), Position);
    Data->SetArrayField(TEXT("pin_ids"), MoveTemp(PinIds));
    Data->SetArrayField(TEXT("pins"), MoveTemp(Pins));
    Data->SetArrayField(TEXT("auxiliary_node_ids"), MoveTemp(AuxiliaryIds));

    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(TEXT("graph_id"), GraphId);
    Details->SetStringField(TEXT("class_path"), Node->GetClass()->GetPathName());
    Details->SetStringField(TEXT("action_id"), ActionId);
    const TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
    Change->SetStringField(TEXT("kind"), TEXT("create"));
    Change->SetStringField(TEXT("target_id"), NodeId);
    Change->SetObjectField(TEXT("details"), Details);

    const TSharedRef<FJsonObject> Result = MakeSuccess(
        FString::Printf(
            TEXT("Spawned Blueprint palette node '%s'."),
            *Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString()),
        Data);
    Result->SetArrayField(
        TEXT("changes"), {MakeShared<FJsonValueObject>(Change)});
    Result->SetArrayField(
        TEXT("next_actions"),
        {MakeShared<FJsonValueObject>(MakeCompileNextAction(Blueprint))});
    return SerializeResult(Result);
}

UBlueprint* ResolveBlueprintAsset(const FString& AssetPath)
{
    if (UBlueprint* Existing = FindObject<UBlueprint>(nullptr, *AssetPath))
    {
        return Existing;
    }
    return LoadObject<UBlueprint>(nullptr, *AssetPath);
}
}

using namespace UE::MCPython::Blueprint2::Palette;

FString UMCPythonHelper::SearchBlueprintNodeActions(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
#if UE_VERSION_NEWER_THAN(5, 7, 99) || UE_VERSION_OLDER_THAN(5, 7, 0)
    return Failure(
        TEXT("UE_VERSION_UNSUPPORTED"),
        TEXT("params"),
        TEXT("Blueprint palette search is not validated for this Unreal Engine version."),
        TEXT("Use the validated UE 5.7 adapter or add an explicit compatibility guard."));
#else
    if (!Blueprint)
    {
        return Failure(
            TEXT("INVALID_INPUT"),
            TEXT("params.asset_path"),
            TEXT("A loaded Blueprint asset is required."),
            TEXT("Pass a valid Blueprint asset path."));
    }
    FPaletteRequest Request;
    FError Error;
    if (!ParseSearchRequest(RequestJson, Request, Error))
    {
        return Failure(Error);
    }
    UEdGraph* Graph = nullptr;
    if (!ResolveStableGraph(
            Blueprint, Request.GraphId, Graph, Error, false))
    {
        return Failure(Error);
    }
    const TSharedRef<FJsonObject> Data = SearchPalette(
        Blueprint, Graph, nullptr, FString(), Request, Error);
    if (!Error.Code.IsEmpty())
    {
        return Failure(Error);
    }
    return SerializeResult(MakeSuccess(
        FString::Printf(
            TEXT("Returned %d compatible Blueprint palette actions."),
            Data->GetIntegerField(TEXT("returned_count"))),
        Data));
#endif
}

FString UMCPythonHelper::DescribeBlueprintNodeAction(
    const FString& RequestJson)
{
#if UE_VERSION_NEWER_THAN(5, 7, 99) || UE_VERSION_OLDER_THAN(5, 7, 0)
    return Failure(
        TEXT("UE_VERSION_UNSUPPORTED"),
        TEXT("params"),
        TEXT("Blueprint palette description is not validated for this Unreal Engine version."),
        TEXT("Use the validated UE 5.7 adapter or add an explicit compatibility guard."));
#else
    TSharedPtr<FJsonObject> RequestObject;
    FError Error;
    if (!ParseObject(RequestJson, RequestObject, Error))
    {
        return Failure(Error);
    }
    static const TSet<FString> AllowedFields = {TEXT("action_id")};
    FString ActionId;
    if (!ValidateClosedObject(
            RequestObject.ToSharedRef(), AllowedFields, TEXT("params"), Error) ||
        !ReadString(
            RequestObject.ToSharedRef(),
            TEXT("action_id"),
            TEXT("params.action_id"),
            true,
            47,
            ActionId,
            Error))
    {
        return Failure(Error);
    }

    FPaletteContextExpectation AnyContext;
    FPaletteActionRecord ActionRecord;
    if (!ResolvePaletteActionToken(
            ActionId, AnyContext, ActionRecord, Error))
    {
        return Failure(Error);
    }
    UBlueprint* Blueprint = ResolveBlueprintAsset(ActionRecord.Context.AssetPath);
    if (!Blueprint)
    {
        return Failure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The Blueprint bound to action_id is no longer loaded or available."),
            TEXT("Repeat palette search for a current Blueprint asset."));
    }
    UEdGraph* Graph = nullptr;
    if (!ResolveStableGraph(
            Blueprint, ActionRecord.Context.GraphId, Graph, Error, true))
    {
        return Failure(Error);
    }
    if (Graph->GetSchema()->GetClass()->GetPathName() !=
        ActionRecord.Context.GraphSchemaPath)
    {
        return Failure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The action's graph schema has changed."),
            TEXT("Repeat palette search for the current graph."));
    }

    FPaletteFilters Filters;
    if (!ParseStoredFilters(ActionRecord.FiltersJson, Filters, Error))
    {
        return Failure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The action's stored search context is no longer valid."),
            TEXT("Repeat palette search."));
    }
    UEdGraphPin* SourcePin = nullptr;
    if (!ResolveActionSourcePin(
            Blueprint,
            Graph,
            ActionRecord.Context.SourcePinId,
            SourcePin,
            Error))
    {
        return Failure(Error);
    }
    TArray<FPaletteCandidate> Candidates;
    BuildCandidatesForSourcePin(
        Blueprint, Graph, SourcePin, ActionRecord.Query, Filters, Candidates);
    if (ResultDigest(Candidates) != ActionRecord.Context.ResultDigest)
    {
        return Failure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The native Blueprint action result set has changed."),
            TEXT("Repeat palette search and describe the new action_id."));
    }
    const FPaletteCandidate* Candidate = Candidates.FindByPredicate(
        [&ActionRecord](const FPaletteCandidate& Item)
        {
            TArray<FString> BindingPaths;
            for (const FPaletteBindingCandidate& Binding : Item.BindingDetails)
            {
                BindingPaths.Add(Binding.ObjectPath);
            }
            return Item.CandidateKey == ActionRecord.CandidateKey &&
                Item.SpawnerSignature == ActionRecord.SpawnerSignature &&
                Item.OwnerPath == ActionRecord.OwnerPath &&
                BindingPaths == ActionRecord.BindingPaths;
        });
    if (!Candidate)
    {
        return Failure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The native Blueprint action is no longer available."),
            TEXT("Repeat palette search and select a current action_id."));
    }

    const TSharedRef<FJsonObject> Data = SerializeCard(
        *Candidate,
        ActionRecord.Context,
        ActionRecord.Query,
        ActionRecord.FiltersJson,
        ActionId);
    Data->SetStringField(TEXT("asset_path"), ActionRecord.Context.AssetPath);
    Data->SetStringField(TEXT("graph_id"), ActionRecord.Context.GraphId);
    SetSourcePinField(Data, ActionRecord.Context.SourcePinId);
    Data->SetStringField(TEXT("tooltip"), Candidate->Tooltip);
    Data->SetStringField(
        TEXT("documentation_link"), Candidate->DocumentationLink);
    Data->SetStringField(
        TEXT("documentation_excerpt"), Candidate->DocumentationExcerpt);
    TArray<TSharedPtr<FJsonValue>> Restrictions = {
        MakeShared<FJsonValueString>(TEXT("editor_session_scoped")),
        MakeShared<FJsonValueString>(TEXT("graph_context_bound"))};
    if (!Candidate->BindingDetails.IsEmpty())
    {
        Restrictions.Add(MakeShared<FJsonValueString>(
            TEXT("requires_returned_bindings")));
    }
    Data->SetArrayField(TEXT("restrictions"), MoveTemp(Restrictions));
    AddTemplatePins(*Candidate, Graph, Data);
    return SerializeResult(MakeSuccess(
        FString::Printf(TEXT("Described Blueprint action '%s'."), *Candidate->Title),
        Data));
#endif
}

FString UMCPythonHelper::AddBlueprintActionNode(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
#if UE_VERSION_NEWER_THAN(5, 7, 99) || UE_VERSION_OLDER_THAN(5, 7, 0)
    return Failure(
        TEXT("UE_VERSION_UNSUPPORTED"),
        TEXT("params"),
        TEXT("Blueprint palette spawning is not validated for this Unreal Engine version."),
        TEXT("Use the validated UE 5.7 adapter or add an explicit compatibility guard."));
#else
    using namespace UE::MCPython::Blueprint2;

    if (!Blueprint)
    {
        return Failure(
            TEXT("INVALID_INPUT"),
            TEXT("params.asset_path"),
            TEXT("A loaded Blueprint asset is required."),
            TEXT("Pass a valid Blueprint asset path."));
    }
    FPaletteSpawnRequest Request;
    FError Error;
    if (!ParseSpawnRequest(RequestJson, Request, Error))
    {
        return Failure(Error);
    }

    FPaletteContextExpectation ExpectedContext;
    ExpectedContext.AssetPath = Blueprint->GetPathName();
    ExpectedContext.GraphId = Request.GraphId;
    FPaletteActionRecord ActionRecord;
    if (!ResolvePaletteActionToken(
            Request.ActionId, ExpectedContext, ActionRecord, Error))
    {
        return Failure(Error);
    }

    UEdGraph* Graph = nullptr;
    if (!ResolveStableGraph(
            Blueprint, Request.GraphId, Graph, Error, true))
    {
        return Failure(Error);
    }
    if (Graph->GetSchema()->GetClass()->GetPathName() !=
        ActionRecord.Context.GraphSchemaPath)
    {
        return Failure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The action's graph schema has changed."),
            TEXT("Repeat palette search for the current graph."));
    }
    UEdGraphPin* SourcePin = nullptr;
    if (!ResolveActionSourcePin(
            Blueprint,
            Graph,
            ActionRecord.Context.SourcePinId,
            SourcePin,
            Error))
    {
        return Failure(Error);
    }

    FPaletteFilters Filters;
    if (!ParseStoredFilters(ActionRecord.FiltersJson, Filters, Error))
    {
        return Failure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The action's stored search context is no longer valid."),
            TEXT("Repeat palette search."));
    }
    TArray<FPaletteCandidate> Candidates;
    BuildCandidatesForSourcePin(
        Blueprint,
        Graph,
        SourcePin,
        ActionRecord.Query,
        Filters,
        Candidates);
    if (ResultDigest(Candidates) != ActionRecord.Context.ResultDigest)
    {
        return Failure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The native Blueprint action result set has changed."),
            TEXT("Repeat palette search and use a current action_id."));
    }
    const FPaletteCandidate* Candidate = FindExactCandidate(
        Candidates, ActionRecord);
    if (!Candidate)
    {
        return Failure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The native Blueprint action is no longer available."),
            TEXT("Repeat palette search and select a current action_id."));
    }

    TArray<FPaletteBindingRecord> BindingRecords;
    IBlueprintNodeBinder::FBindingSet ResolvedBindings;
    if (!ResolveDynamicBindingObjects(
            Request.ActionId,
            Request.BindingIds,
            BindingRecords,
            ResolvedBindings,
            Error))
    {
        return Failure(Error);
    }
    TArray<FString> BindingPaths;
    for (const FPaletteBindingRecord& Binding : BindingRecords)
    {
        BindingPaths.Add(Binding.ObjectPath);
    }
    BindingPaths.Sort();
    if (BindingPaths != ActionRecord.BindingPaths)
    {
        return Failure(
            TEXT("INVALID_INPUT"),
            TEXT("params.bindings"),
            TEXT("bindings must contain exactly the IDs returned for this action."),
            TEXT("Search again and pass the selected action's complete bindings array."));
    }
    TMap<UEdGraphNode*, FString> NodesBefore;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node)
        {
            NodesBefore.Add(Node, DescribeNodeTarget(Blueprint, Node).Id);
        }
    }

    FMutationScope Scope(NSLOCTEXT(
        "MCPython", "AddBlueprintActionNode", "Add Blueprint palette node"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Could not begin a Blueprint palette transaction."),
            TEXT("Close any conflicting editor transaction and retry."));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);

    UEdGraphNode* NewNode = Candidate->Spawner->Invoke(
        Graph,
        ResolvedBindings,
        FVector2D(Request.PositionX, Request.PositionY));
    if (!NewNode)
    {
        return RollbackPaletteFailure(
            Scope,
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The native Blueprint action did not create a node."),
            TEXT("Repeat palette search or choose another compatible action."));
    }
    if (NodesBefore.Contains(NewNode))
    {
        const FRollbackResult Rollback = Scope.Rollback();
        if (!Rollback.bSucceeded && !Rollback.bDeferredToWorkflow &&
            !GraphMatchesNodeSnapshot(Blueprint, Graph, NodesBefore))
        {
            return Failure(
                TEXT("ROLLBACK_FAILED"),
                TEXT("transaction"),
                TEXT("Blueprint palette singleton rollback left a graph delta."),
                TEXT("Inspect the graph before attempting another mutation."));
        }
        return Failure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The action resolved to an existing singleton node."),
            TEXT("Choose an action that can create a new node in this graph."));
    }
    if (NewNode->GetGraph() != Graph || !Graph->Nodes.Contains(NewNode))
    {
        return RollbackPaletteFailure(
            Scope,
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The action resolved to an existing or out-of-graph singleton node."),
            TEXT("Choose an action that can create a new node in this graph."));
    }
    if (NewNode->GetClass()->GetPathName() != Candidate->NodeClassPath)
    {
        return RollbackPaletteFailure(
            Scope,
            TEXT("INTERNAL_ERROR"),
            TEXT("params.action_id"),
            TEXT("The native spawner returned an unexpected node class."),
            TEXT("Repeat palette search and report the action signature."));
    }

    const FString NewNodeId = DescribeNodeTarget(Blueprint, NewNode).Id;
    if (!NewNodeId.StartsWith(TEXT("node:")))
    {
        return RollbackPaletteFailure(
            Scope,
            TEXT("INTERNAL_ERROR"),
            TEXT("result.node_id"),
            TEXT("The spawned node did not receive a stable node ID."),
            TEXT("Retry after the graph has finished loading."));
    }
    for (const UEdGraphPin* Pin : NewNode->Pins)
    {
        if (Pin && !Pin->bHidden &&
            !DescribePinTarget(Blueprint, Pin).Id.StartsWith(TEXT("pin:")))
        {
            return RollbackPaletteFailure(
                Scope,
                TEXT("INTERNAL_ERROR"),
                TEXT("result.pin_ids"),
                TEXT("A visible spawned pin did not receive a stable pin ID."),
                TEXT("Retry after the graph has finished loading."));
        }
    }

    TArray<TPair<FString, UEdGraphNode*>> AuxiliaryNodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || Node == NewNode || NodesBefore.Contains(Node))
        {
            continue;
        }
        const FString NodeId = DescribeNodeTarget(Blueprint, Node).Id;
        if (!NodeId.StartsWith(TEXT("node:")))
        {
            return RollbackPaletteFailure(
                Scope,
                TEXT("INTERNAL_ERROR"),
                TEXT("result.auxiliary_node_ids"),
                TEXT("An auxiliary spawned node did not receive a stable node ID."),
                TEXT("Retry after the graph has finished loading."));
        }
        AuxiliaryNodes.Emplace(NodeId, Node);
    }
    AuxiliaryNodes.Sort(
        [](const TPair<FString, UEdGraphNode*>& Left,
           const TPair<FString, UEdGraphNode*>& Right)
        {
            return Left.Key < Right.Key;
        });

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    return SpawnSuccess(
        Blueprint,
        Request.GraphId,
        Request.ActionId,
        NewNode,
        AuxiliaryNodes);
#endif
}

FString UMCPythonHelper::SuggestBlueprintNodesForPin(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
#if UE_VERSION_NEWER_THAN(5, 7, 99) || UE_VERSION_OLDER_THAN(5, 7, 0)
    return Failure(
        TEXT("UE_VERSION_UNSUPPORTED"),
        TEXT("params.pin_id"),
        TEXT("Pin-context Blueprint palette suggestions are not validated for this Unreal Engine version."),
        TEXT("Use the validated UE 5.7 adapter or add an explicit compatibility guard."));
#else
    if (!Blueprint)
    {
        return Failure(
            TEXT("INVALID_INPUT"),
            TEXT("params.asset_path"),
            TEXT("A loaded Blueprint asset is required."),
            TEXT("Pass a valid Blueprint asset path."));
    }

    FPaletteRequest Request;
    FString PinId;
    FError Error;
    if (!ParseSuggestionRequest(RequestJson, Request, PinId, Error))
    {
        return Failure(Error);
    }

    UEdGraph* Graph = nullptr;
    if (!ResolveStableGraph(
            Blueprint, Request.GraphId, Graph, Error, false))
    {
        return Failure(Error);
    }

    FTargetRef PinTarget;
    PinTarget.Id = PinId;
    FString ResolveError;
    const FResolvedTarget Resolved = ResolveTarget(
        Blueprint, ETargetKind::Pin, PinTarget, ResolveError);
    if (!Resolved.Pin || !Resolved.Node || !Resolved.bStable)
    {
        return Failure(
            TEXT("INVALID_INPUT"),
            TEXT("params.pin_id"),
            ResolveError.IsEmpty()
                ? TEXT("A stable pin_id is required.")
                : ResolveError,
            TEXT("Inspect the Blueprint and use a current stable pin ID."));
    }
    if (Resolved.Node->GetGraph() != Graph)
    {
        return Failure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.pin_id"),
            TEXT("The source pin does not belong to the requested graph."),
            TEXT("Use a pin_id from graph_id and request new suggestions."));
    }

    const TSharedRef<FJsonObject> Data = SearchPalette(
        Blueprint, Graph, Resolved.Pin, PinId, Request, Error);
    if (!Error.Code.IsEmpty())
    {
        return Failure(Error);
    }
    return SerializeResult(MakeSuccess(
        FString::Printf(
            TEXT("Returned %d Blueprint actions compatible according to the current native action filter."),
            Data->GetIntegerField(TEXT("returned_count"))),
        Data));
#endif
}
