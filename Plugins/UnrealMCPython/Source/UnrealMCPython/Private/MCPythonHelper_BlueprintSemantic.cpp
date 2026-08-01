// Copyright (c) 2025 GenOrca. All Rights Reserved.

#include "MCPythonHelper.h"

#include "MCPythonBlueprint2Internal.h"
#include "MCPythonBlueprintPaletteInternal.h"

#include "Algo/Unique.h"
#include "BlueprintNodeSpawner.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_InputKey.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/EngineVersionComparison.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace UE::MCPython::Blueprint2
{
ESemanticConnectedSpawnFailurePoint GSemanticConnectedSpawnFailurePoint =
    ESemanticConnectedSpawnFailurePoint::None;
ESemanticInsertionFailurePoint GSemanticInsertionFailurePoint =
    ESemanticInsertionFailurePoint::None;

void SetSemanticConnectedSpawnFailurePointForTests(
    const ESemanticConnectedSpawnFailurePoint Point)
{
    GSemanticConnectedSpawnFailurePoint = Point;
}

void SetSemanticInsertionFailurePointForTests(
    const ESemanticInsertionFailurePoint Point)
{
    GSemanticInsertionFailurePoint = Point;
}
}
#endif

namespace UE::MCPython::Blueprint2
{
int32 ClassifySemanticPageCandidateBudget(
    const int32 ReturnedBindingCount,
    const int32 CandidateBindingCount)
{
    static constexpr int32 MaximumReturnedBindings = 1024;
    if (CandidateBindingCount < 0 ||
        CandidateBindingCount > MaximumReturnedBindings)
    {
        return -1;
    }
    return ReturnedBindingCount + CandidateBindingCount >
            MaximumReturnedBindings
        ? 0
        : 1;
}

int32 SelectUniqueBestReplacementCandidate(
    const TConstArrayView<int32> FlattenedSemanticCosts)
{
    static constexpr int32 CostWidth = 4;
    if (FlattenedSemanticCosts.IsEmpty() ||
        FlattenedSemanticCosts.Num() % CostWidth != 0)
    {
        return INDEX_NONE;
    }
    auto IsLess = [&](const int32 LeftIndex, const int32 RightIndex)
    {
        for (int32 Component = 0; Component < CostWidth; ++Component)
        {
            const int32 Left = FlattenedSemanticCosts[
                LeftIndex * CostWidth + Component];
            const int32 Right = FlattenedSemanticCosts[
                RightIndex * CostWidth + Component];
            if (Left != Right)
            {
                return Left < Right;
            }
        }
        return false;
    };
    auto IsEqual = [&](const int32 LeftIndex, const int32 RightIndex)
    {
        for (int32 Component = 0; Component < CostWidth; ++Component)
        {
            if (FlattenedSemanticCosts[LeftIndex * CostWidth + Component] !=
                FlattenedSemanticCosts[RightIndex * CostWidth + Component])
            {
                return false;
            }
        }
        return true;
    };

    int32 BestIndex = 0;
    bool bBestIsUnique = true;
    const int32 CandidateCount = FlattenedSemanticCosts.Num() / CostWidth;
    for (int32 CandidateIndex = 1;
         CandidateIndex < CandidateCount;
         ++CandidateIndex)
    {
        if (IsLess(CandidateIndex, BestIndex))
        {
            BestIndex = CandidateIndex;
            bBestIsUnique = true;
        }
        else if (IsEqual(CandidateIndex, BestIndex))
        {
            bBestIsUnique = false;
        }
    }
    return bBestIsUnique ? BestIndex : INDEX_NONE;
}

#if WITH_DEV_AUTOMATION_TESTS
int32 SelectUniqueBestReplacementCandidateForTests(
    const TConstArrayView<int32> FlattenedSemanticCosts)
{
    return SelectUniqueBestReplacementCandidate(FlattenedSemanticCosts);
}
#endif
}

namespace
{
using namespace UE::MCPython::Blueprint2;
using namespace UE::MCPython::Blueprint2::Palette;

struct FConnectionSuggestionRequest
{
    FString GraphId;
    FString SourcePinId;
    FString TargetPinId;
    FString Query;
    FPaletteFilters Filters;
    FString FiltersJson;
    bool bAllowConversion = false;
    FString Cursor;
    int32 Limit = 50;
};

struct FSemanticTemplatePin
{
    UEdGraphPin* Pin = nullptr;
    FString Key;
    FString Name;
    FString Direction;
    TSharedPtr<FJsonObject> Type;
    FString TypeJson;
    int32 Occurrence = 0;
};

struct FSemanticResponse
{
    FString Kind;
    FString Message;
    bool bRequiresConversion = false;
};

struct FSemanticPair
{
    FSemanticTemplatePin Input;
    FSemanticTemplatePin Output;
    FSemanticResponse SourceResponse;
    FSemanticResponse TargetResponse;
    int32 ConversionCount = 0;
    FString Key;
    FString SortKey;
};

struct FSemanticCandidate
{
    FPaletteCandidate Candidate;
    TArray<FSemanticPair> Pairs;
    FString SortKey;
};

struct FConnectedSpawnRequest
{
    FString GraphId;
    FString PinId;
    FString ActionId;
    FString ConnectionBindingId;
    double PositionX = 0.0;
    double PositionY = 0.0;
    bool bAllowConversion = false;
    TArray<FString> BindingIds;
};

struct FInsertionRequest
{
    FString GraphId;
    FString SourcePinId;
    FString TargetPinId;
    FString ActionId;
    FString InputBindingId;
    FString OutputBindingId;
    double PositionX = 0.0;
    double PositionY = 0.0;
    TArray<FString> BindingIds;
};

struct FReplacementPinMappingInput
{
    FString OldPinId;
    FString NewBindingId;
};

struct FReplacementPreviewRequest
{
    FString GraphId;
    FString NodeId;
    FString ActionId;
    TArray<FString> BindingIds;
    TArray<FReplacementPinMappingInput> PinMappings;
    bool bAllowConversion = false;
    bool bAllowLoss = false;
};

FString SemanticFailure(
    const FString& Code,
    const FString& Path,
    const FString& Message,
    const FString& Hint,
    const bool bRetryable = false,
    const TSharedPtr<FJsonObject>& Details = nullptr)
{
    return SerializeResult(MakeFailure(
        Code, Path, Message, bRetryable, Hint, Details));
}

FString UnsupportedSemanticVersion(const FString& Operation)
{
    return SemanticFailure(
        TEXT("UE_VERSION_UNSUPPORTED"),
        TEXT("params"),
        Operation + TEXT(" is not validated for this Unreal Engine version."),
        TEXT("Use the validated UE 5.7 adapter or add an explicit compatibility guard."));
}

FString MissingBlueprint()
{
    return SemanticFailure(
        TEXT("INVALID_INPUT"),
        TEXT("params.asset_path"),
        TEXT("A loaded Blueprint asset is required."),
        TEXT("Pass a valid Blueprint asset path."));
}

TArray<TSharedPtr<FJsonValue>> UnsupportedReplacementMetadata(
    const UEdGraphNode* Node)
{
    TArray<FString> Fields;
    if (Cast<UK2Node_CallFunction>(Node))
    {
        Fields.Append({
            TEXT("defaults_to_pure"),
            TEXT("enum_exec_expansion"),
            TEXT("function_reference")});
    }
    if (Cast<UK2Node_Variable>(Node))
    {
        Fields.Add(TEXT("variable_reference"));
    }
    if (Cast<UK2Node_Event>(Node))
    {
        Fields.Append({
            TEXT("custom_function_name"),
            TEXT("event_reference"),
            TEXT("function_flags"),
            TEXT("internal_event"),
            TEXT("override_function")});
    }
    if (Cast<UK2Node_CustomEvent>(Node))
    {
        Fields.Append({TEXT("call_in_editor"), TEXT("deprecated")});
    }
    if (Cast<UK2Node_InputKey>(Node))
    {
        Fields.Add(TEXT("input_key"));
    }
    Fields.Sort();
    Fields.SetNum(Algo::Unique(Fields));

    TArray<TSharedPtr<FJsonValue>> Result;
    for (const FString& Field : Fields)
    {
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("field"), Field);
        Item->SetStringField(
            TEXT("reason"),
            TEXT("node_class_specific_property_is_not_copied"));
        Result.Add(MakeShared<FJsonValueObject>(Item));
    }
    return Result;
}

bool SetSemanticError(
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
    return false;
}

FString SemanticFailure(const FError& Error)
{
    return SemanticFailure(
        Error.Code.IsEmpty() ? TEXT("INTERNAL_ERROR") : Error.Code,
        Error.Path,
        Error.Message.IsEmpty()
            ? TEXT("Blueprint semantic request failed.")
            : Error.Message,
        Error.Hint);
}

bool ParseJsonObject(
    const FString& Json,
    TSharedPtr<FJsonObject>& OutObject,
    FError& OutError)
{
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (Json.IsEmpty() || !FJsonSerializer::Deserialize(Reader, OutObject) ||
        !OutObject.IsValid())
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params"),
            TEXT("Expected one valid JSON object."),
            TEXT("Use the published Blueprint semantic action schema."));
    }
    return true;
}

bool ParseConnectionSuggestionRequest(
    const FString& Json,
    FConnectionSuggestionRequest& OutRequest,
    FError& OutError)
{
    TSharedPtr<FJsonObject> Object;
    if (!ParseJsonObject(Json, Object, OutError))
    {
        return false;
    }
    static const TSet<FString> Allowed = {
        TEXT("graph_id"), TEXT("source_pin_id"), TEXT("target_pin_id"),
        TEXT("query"), TEXT("filters"), TEXT("allow_conversion"),
        TEXT("cursor"), TEXT("limit")};
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
    {
        if (!Allowed.Contains(Field.Key))
        {
            return SetSemanticError(
                OutError,
                TEXT("INVALID_INPUT"),
                TEXT("params.") + Field.Key,
                TEXT("Unknown field in closed connection suggestion request."),
                TEXT("Remove fields not present in the published action schema."));
        }
    }
    if (!Object->TryGetStringField(TEXT("graph_id"), OutRequest.GraphId) ||
        !Object->TryGetStringField(
            TEXT("source_pin_id"), OutRequest.SourcePinId) ||
        !Object->TryGetStringField(
            TEXT("target_pin_id"), OutRequest.TargetPinId))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params"),
            TEXT("graph_id, source_pin_id, and target_pin_id are required strings."),
            TEXT("Inspect the Blueprint and pass current stable IDs."));
    }
    FGuid SourceGuid;
    FGuid TargetGuid;
    if (!ParseTargetId(OutRequest.SourcePinId, ETargetKind::Pin, SourceGuid) ||
        !ParseTargetId(OutRequest.TargetPinId, ETargetKind::Pin, TargetGuid))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.source_pin_id"),
            TEXT("Both pin IDs must be canonical non-zero persisted pin IDs."),
            TEXT("Inspect the graph again and use returned pin IDs unchanged."));
    }
    if (SourceGuid == TargetGuid)
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.target_pin_id"),
            TEXT("source_pin_id and target_pin_id must identify different pins."),
            TEXT("Choose one output pin and one distinct input pin."));
    }

    if (Object->Values.Contains(TEXT("query")) &&
        (!Object->HasTypedField<EJson::String>(TEXT("query")) ||
            !Object->TryGetStringField(TEXT("query"), OutRequest.Query)))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.query"),
            TEXT("query must be a string when present."),
            TEXT("Pass a palette query string or omit query."));
    }
    if (OutRequest.Query.Len() > 256)
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.query"),
            TEXT("query exceeds the 256 character limit."),
            TEXT("Use a shorter palette query."));
    }
    if (Object->Values.Contains(TEXT("cursor")) &&
        (!Object->HasTypedField<EJson::String>(TEXT("cursor")) ||
            !Object->TryGetStringField(TEXT("cursor"), OutRequest.Cursor)))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.cursor"),
            TEXT("cursor must be a string when present."),
            TEXT("Pass an opaque cursor string or omit cursor."));
    }
    if (Object->Values.Contains(TEXT("allow_conversion")) &&
        (!Object->HasTypedField<EJson::Boolean>(TEXT("allow_conversion")) ||
            !Object->TryGetBoolField(
                TEXT("allow_conversion"), OutRequest.bAllowConversion)))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.allow_conversion"),
            TEXT("allow_conversion must be a Boolean when present."),
            TEXT("Pass true or false, not a string or number."));
    }
    double Limit = 50.0;
    if (Object->Values.Contains(TEXT("limit")) &&
        (!Object->HasTypedField<EJson::Number>(TEXT("limit")) ||
            !Object->TryGetNumberField(TEXT("limit"), Limit)))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.limit"),
            TEXT("limit must be a JSON number when present."),
            TEXT("Pass an integral JSON number from 1 through 200."));
    }
    if (!FMath::IsFinite(Limit) || Limit < 1.0 || Limit > 200.0 ||
        Limit != FMath::TruncToDouble(Limit))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.limit"),
            TEXT("limit must be an integer from 1 through 200."),
            TEXT("Use a bounded semantic page size."));
    }
    OutRequest.Limit = static_cast<int32>(Limit);

    const TSharedPtr<FJsonObject>* Filters = nullptr;
    if (Object->Values.Contains(TEXT("filters")) &&
        (!Object->HasTypedField<EJson::Object>(TEXT("filters")) ||
            !Object->TryGetObjectField(TEXT("filters"), Filters)))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.filters"),
            TEXT("filters must be an object when present."),
            TEXT("Pass a closed palette filters object or omit filters."));
    }
    const TSharedRef<FJsonObject> EmptyFilters = MakeShared<FJsonObject>();
    const TSharedRef<FJsonObject> FilterObject =
        Object->TryGetObjectField(TEXT("filters"), Filters) && Filters &&
            Filters->IsValid()
        ? Filters->ToSharedRef()
        : EmptyFilters;
    OutRequest.FiltersJson = CanonicalJsonString(
        MakeShared<FJsonValueObject>(FilterObject));
    return ParseStoredFilters(
        OutRequest.FiltersJson, OutRequest.Filters, OutError);
}

bool IsOpaqueSemanticToken(const FString& Value, const FString& Prefix)
{
    if (!Value.StartsWith(Prefix) || Value.Len() != Prefix.Len() + 40)
    {
        return false;
    }
    for (int32 Index = Prefix.Len(); Index < Value.Len(); ++Index)
    {
        const TCHAR Character = Value[Index];
        if (!((Character >= TEXT('0') && Character <= TEXT('9')) ||
              (Character >= TEXT('a') && Character <= TEXT('f'))))
        {
            return false;
        }
    }
    return true;
}

bool ParseConnectedSpawnRequest(
    const FString& Json,
    FConnectedSpawnRequest& OutRequest,
    FError& OutError)
{
    TSharedPtr<FJsonObject> Object;
    if (!ParseJsonObject(Json, Object, OutError))
    {
        return false;
    }
    static const TSet<FString> Allowed = {
        TEXT("graph_id"), TEXT("pin_id"), TEXT("action_id"),
        TEXT("connection_binding_id"), TEXT("position"),
        TEXT("allow_conversion"), TEXT("bindings")};
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
    {
        if (!Allowed.Contains(Field.Key))
        {
            return SetSemanticError(
                OutError,
                TEXT("INVALID_INPUT"),
                TEXT("params.") + Field.Key,
                TEXT("Unknown field in closed connected spawn request."),
                TEXT("Remove fields not present in the published action schema."));
        }
    }
    if (!Object->TryGetStringField(TEXT("graph_id"), OutRequest.GraphId) ||
        !Object->TryGetStringField(TEXT("pin_id"), OutRequest.PinId) ||
        !Object->TryGetStringField(TEXT("action_id"), OutRequest.ActionId) ||
        !Object->TryGetStringField(
            TEXT("connection_binding_id"), OutRequest.ConnectionBindingId))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params"),
            TEXT("graph_id, pin_id, action_id, and connection_binding_id are required."),
            TEXT("Use IDs returned by current pin suggestions."));
    }
    FGuid PinGuid;
    if (!ParseTargetId(OutRequest.PinId, ETargetKind::Pin, PinGuid) ||
        !IsOpaqueSemanticToken(OutRequest.ActionId, TEXT("action:")) ||
        !IsOpaqueSemanticToken(
            OutRequest.ConnectionBindingId, TEXT("binding:")))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params"),
            TEXT("Connected spawn IDs must be canonical opaque IDs."),
            TEXT("Use IDs returned by current pin suggestions unchanged."));
    }
    const TSharedPtr<FJsonObject>* Position = nullptr;
    if (!Object->TryGetObjectField(TEXT("position"), Position) ||
        !Position || !Position->IsValid() ||
        (*Position)->Values.Num() != 2 ||
        !(*Position)->Values.Contains(TEXT("x")) ||
        !(*Position)->Values.Contains(TEXT("y")) ||
        !(*Position)->TryGetNumberField(TEXT("x"), OutRequest.PositionX) ||
        !(*Position)->TryGetNumberField(TEXT("y"), OutRequest.PositionY) ||
        !FMath::IsFinite(OutRequest.PositionX) ||
        !FMath::IsFinite(OutRequest.PositionY) ||
        FMath::Abs(OutRequest.PositionX) > 1000000000.0 ||
        FMath::Abs(OutRequest.PositionY) > 1000000000.0)
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.position"),
            TEXT("position must contain only finite bounded x and y coordinates."),
            TEXT("Use coordinates within plus or minus 1,000,000,000."));
    }
    if (Object->Values.Contains(TEXT("allow_conversion")) &&
        (!Object->HasTypedField<EJson::Boolean>(TEXT("allow_conversion")) ||
            !Object->TryGetBoolField(
                TEXT("allow_conversion"), OutRequest.bAllowConversion)))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.allow_conversion"),
            TEXT("allow_conversion must be a Boolean when present."),
            TEXT("Pass true or false, not a string or number."));
    }
    const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
    if (Object->Values.Contains(TEXT("bindings")) &&
        (!Object->HasTypedField<EJson::Array>(TEXT("bindings")) ||
            !Object->TryGetArrayField(TEXT("bindings"), Bindings)))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.bindings"),
            TEXT("bindings must be an array when present."),
            TEXT("Pass the action-owned binding ID array or omit bindings."));
    }
    if (Bindings)
    {
        if (Bindings->Num() > 32)
        {
            return SetSemanticError(
                OutError,
                TEXT("INVALID_INPUT"),
                TEXT("params.bindings"),
                TEXT("At most 32 dynamic binding IDs are accepted."),
                TEXT("Pass exactly the binding IDs returned with the action."));
        }
        TSet<FString> Unique;
        for (const TSharedPtr<FJsonValue>& Binding : *Bindings)
        {
            if (!Binding.IsValid() || Binding->Type != EJson::String ||
                !IsOpaqueSemanticToken(Binding->AsString(), TEXT("binding:")) ||
                Unique.Contains(Binding->AsString()))
            {
                return SetSemanticError(
                    OutError,
                    TEXT("INVALID_INPUT"),
                    TEXT("params.bindings"),
                    TEXT("Dynamic binding IDs must be unique opaque strings."),
                    TEXT("Use each returned action binding exactly once."));
            }
            Unique.Add(Binding->AsString());
            OutRequest.BindingIds.Add(Binding->AsString());
        }
    }
    return true;
}

bool ParseInsertionRequest(
    const FString& Json,
    FInsertionRequest& OutRequest,
    FError& OutError)
{
    TSharedPtr<FJsonObject> Object;
    if (!ParseJsonObject(Json, Object, OutError))
    {
        return false;
    }
    static const TSet<FString> Allowed = {
        TEXT("graph_id"), TEXT("source_pin_id"), TEXT("target_pin_id"),
        TEXT("action_id"), TEXT("input_binding_id"),
        TEXT("output_binding_id"), TEXT("position"), TEXT("bindings")};
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
    {
        if (!Allowed.Contains(Field.Key))
        {
            return SetSemanticError(
                OutError,
                TEXT("INVALID_INPUT"),
                TEXT("params.") + Field.Key,
                TEXT("Unknown field in closed insertion request."),
                TEXT("Remove fields not present in the published action schema."));
        }
    }
    if (!Object->TryGetStringField(TEXT("graph_id"), OutRequest.GraphId) ||
        !Object->TryGetStringField(
            TEXT("source_pin_id"), OutRequest.SourcePinId) ||
        !Object->TryGetStringField(
            TEXT("target_pin_id"), OutRequest.TargetPinId) ||
        !Object->TryGetStringField(TEXT("action_id"), OutRequest.ActionId) ||
        !Object->TryGetStringField(
            TEXT("input_binding_id"), OutRequest.InputBindingId) ||
        !Object->TryGetStringField(
            TEXT("output_binding_id"), OutRequest.OutputBindingId))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params"),
            TEXT("graph_id, source_pin_id, target_pin_id, action_id, input_binding_id, and output_binding_id are required strings."),
            TEXT("Use one current action and binding pair returned by connection suggestions."));
    }
    FGuid GraphGuid;
    FGuid SourceGuid;
    FGuid TargetGuid;
    if (!ParseTargetId(OutRequest.GraphId, ETargetKind::Graph, GraphGuid) ||
        !ParseTargetId(
            OutRequest.SourcePinId, ETargetKind::Pin, SourceGuid) ||
        !ParseTargetId(
            OutRequest.TargetPinId, ETargetKind::Pin, TargetGuid) ||
        SourceGuid == TargetGuid ||
        !IsOpaqueSemanticToken(OutRequest.ActionId, TEXT("action:")) ||
        !IsOpaqueSemanticToken(
            OutRequest.InputBindingId, TEXT("binding:")) ||
        !IsOpaqueSemanticToken(
            OutRequest.OutputBindingId, TEXT("binding:")) ||
        OutRequest.InputBindingId == OutRequest.OutputBindingId)
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params"),
            TEXT("Insertion IDs must be distinct canonical stable or opaque IDs."),
            TEXT("Inspect the graph and use one returned binding pair unchanged."));
    }
    const TSharedPtr<FJsonObject>* Position = nullptr;
    if (!Object->HasTypedField<EJson::Object>(TEXT("position")) ||
        !Object->TryGetObjectField(TEXT("position"), Position) ||
        !Position || !Position->IsValid() ||
        (*Position)->Values.Num() != 2 ||
        !(*Position)->HasTypedField<EJson::Number>(TEXT("x")) ||
        !(*Position)->HasTypedField<EJson::Number>(TEXT("y")) ||
        !(*Position)->TryGetNumberField(TEXT("x"), OutRequest.PositionX) ||
        !(*Position)->TryGetNumberField(TEXT("y"), OutRequest.PositionY) ||
        !FMath::IsFinite(OutRequest.PositionX) ||
        !FMath::IsFinite(OutRequest.PositionY) ||
        FMath::Abs(OutRequest.PositionX) > 1000000000.0 ||
        FMath::Abs(OutRequest.PositionY) > 1000000000.0)
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.position"),
            TEXT("position must contain only finite bounded numeric x and y coordinates."),
            TEXT("Use coordinates within plus or minus 1,000,000,000."));
    }
    const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
    if (Object->Values.Contains(TEXT("bindings")) &&
        (!Object->HasTypedField<EJson::Array>(TEXT("bindings")) ||
            !Object->TryGetArrayField(TEXT("bindings"), Bindings)))
    {
        return SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            TEXT("params.bindings"),
            TEXT("bindings must be an array when present."),
            TEXT("Pass the action-owned binding ID array or omit bindings."));
    }
    if (Bindings)
    {
        if (Bindings->Num() > 32)
        {
            return SetSemanticError(
                OutError,
                TEXT("INVALID_INPUT"),
                TEXT("params.bindings"),
                TEXT("At most 32 dynamic binding IDs are accepted."),
                TEXT("Pass exactly the binding IDs returned with the action."));
        }
        TSet<FString> Unique;
        for (const TSharedPtr<FJsonValue>& Binding : *Bindings)
        {
            if (!Binding.IsValid() || Binding->Type != EJson::String ||
                !IsOpaqueSemanticToken(Binding->AsString(), TEXT("binding:")) ||
                Unique.Contains(Binding->AsString()))
            {
                return SetSemanticError(
                    OutError,
                    TEXT("INVALID_INPUT"),
                    TEXT("params.bindings"),
                    TEXT("Dynamic binding IDs must be unique opaque strings."),
                    TEXT("Use each returned action binding exactly once."));
            }
            Unique.Add(Binding->AsString());
            OutRequest.BindingIds.Add(Binding->AsString());
        }
    }
    return true;
}

bool ParseReplacementPreviewRequest(
    const FString& Json,
    FReplacementPreviewRequest& OutRequest,
    FError& OutError)
{
    TSharedPtr<FJsonObject> Object;
    if (!ParseJsonObject(Json, Object, OutError))
    {
        return false;
    }
    static const TSet<FString> Allowed = {
        TEXT("graph_id"), TEXT("node_id"), TEXT("action_id"),
        TEXT("bindings"), TEXT("pin_mapping"),
        TEXT("allow_conversion"), TEXT("allow_loss")};
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
    {
        if (!Allowed.Contains(Field.Key))
        {
            return SetSemanticError(
                OutError, TEXT("INVALID_INPUT"), TEXT("params.") + Field.Key,
                TEXT("Unknown field in closed replacement preview request."),
                TEXT("Remove fields not present in the published action schema."));
        }
    }
    if (!Object->TryGetStringField(TEXT("graph_id"), OutRequest.GraphId) ||
        !Object->TryGetStringField(TEXT("node_id"), OutRequest.NodeId) ||
        !Object->TryGetStringField(TEXT("action_id"), OutRequest.ActionId))
    {
        return SetSemanticError(
            OutError, TEXT("INVALID_INPUT"), TEXT("params"),
            TEXT("graph_id, node_id, and action_id are required strings."),
            TEXT("Inspect the graph and use one current graph palette action."));
    }
    FGuid GraphGuid;
    FGuid NodeGuid;
    if (!ParseTargetId(OutRequest.GraphId, ETargetKind::Graph, GraphGuid) ||
        !ParseTargetId(OutRequest.NodeId, ETargetKind::Node, NodeGuid) ||
        !IsOpaqueSemanticToken(OutRequest.ActionId, TEXT("action:")))
    {
        return SetSemanticError(
            OutError, TEXT("INVALID_INPUT"), TEXT("params"),
            TEXT("Replacement preview IDs must be canonical stable or opaque IDs."),
            TEXT("Inspect the graph and repeat palette search."));
    }
    auto ReadBoolean = [&](const FString& Field, bool& OutValue)
    {
        if (!Object->Values.Contains(Field))
        {
            return true;
        }
        if (!Object->HasTypedField<EJson::Boolean>(Field) ||
            !Object->TryGetBoolField(Field, OutValue))
        {
            return SetSemanticError(
                OutError, TEXT("INVALID_INPUT"), TEXT("params.") + Field,
                Field + TEXT(" must be a Boolean when present."),
                TEXT("Pass true or false, not a string or number."));
        }
        return true;
    };
    if (!ReadBoolean(TEXT("allow_conversion"), OutRequest.bAllowConversion) ||
        !ReadBoolean(TEXT("allow_loss"), OutRequest.bAllowLoss))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
    if (Object->Values.Contains(TEXT("bindings")) &&
        (!Object->HasTypedField<EJson::Array>(TEXT("bindings")) ||
            !Object->TryGetArrayField(TEXT("bindings"), Bindings)))
    {
        return SetSemanticError(
            OutError, TEXT("INVALID_INPUT"), TEXT("params.bindings"),
            TEXT("bindings must be an array when present."),
            TEXT("Pass the action-owned binding ID array or omit bindings."));
    }
    TSet<FString> UniqueBindings;
    if (Bindings)
    {
        if (Bindings->Num() > 32)
        {
            return SetSemanticError(
                OutError, TEXT("INVALID_INPUT"), TEXT("params.bindings"),
                TEXT("At most 32 dynamic binding IDs are accepted."),
                TEXT("Pass exactly the binding IDs returned with the action."));
        }
        for (const TSharedPtr<FJsonValue>& Binding : *Bindings)
        {
            if (!Binding.IsValid() || Binding->Type != EJson::String ||
                !IsOpaqueSemanticToken(Binding->AsString(), TEXT("binding:")) ||
                UniqueBindings.Contains(Binding->AsString()))
            {
                return SetSemanticError(
                    OutError, TEXT("INVALID_INPUT"), TEXT("params.bindings"),
                    TEXT("Dynamic binding IDs must be unique opaque strings."),
                    TEXT("Use each returned action binding exactly once."));
            }
            UniqueBindings.Add(Binding->AsString());
            OutRequest.BindingIds.Add(Binding->AsString());
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* PinMappings = nullptr;
    if (Object->Values.Contains(TEXT("pin_mapping")) &&
        (!Object->HasTypedField<EJson::Array>(TEXT("pin_mapping")) ||
            !Object->TryGetArrayField(TEXT("pin_mapping"), PinMappings)))
    {
        return SetSemanticError(
            OutError, TEXT("INVALID_INPUT"), TEXT("params.pin_mapping"),
            TEXT("pin_mapping must be an array when present."),
            TEXT("Pass closed old_pin_id/new_binding_id objects."));
    }
    TSet<FString> UniqueOldPins;
    TSet<FString> UniqueNewBindings;
    if (PinMappings)
    {
        if (PinMappings->Num() > 256)
        {
            return SetSemanticError(
                OutError, TEXT("INVALID_INPUT"), TEXT("params.pin_mapping"),
                TEXT("pin_mapping accepts at most 256 entries."),
                TEXT("Map each visible old pin at most once."));
        }
        for (int32 Index = 0; Index < PinMappings->Num(); ++Index)
        {
            const TSharedPtr<FJsonValue>& Value = (*PinMappings)[Index];
            const TSharedPtr<FJsonObject> Mapping = Value.IsValid() &&
                    Value->Type == EJson::Object
                ? Value->AsObject()
                : nullptr;
            const FString Path = FString::Printf(
                TEXT("params.pin_mapping[%d]"), Index);
            FString OldPinId;
            FString NewBindingId;
            if (!Mapping || Mapping->Values.Num() != 2 ||
                !Mapping->Values.Contains(TEXT("old_pin_id")) ||
                !Mapping->Values.Contains(TEXT("new_binding_id")) ||
                !Mapping->TryGetStringField(TEXT("old_pin_id"), OldPinId) ||
                !Mapping->TryGetStringField(TEXT("new_binding_id"), NewBindingId))
            {
                return SetSemanticError(
                    OutError, TEXT("INVALID_INPUT"), Path,
                    TEXT("Each pin_mapping entry must contain only old_pin_id and new_binding_id strings."),
                    TEXT("Use the published closed mapping object."));
            }
            FGuid OldPinGuid;
            if (!ParseTargetId(OldPinId, ETargetKind::Pin, OldPinGuid) ||
                !IsOpaqueSemanticToken(NewBindingId, TEXT("binding:")) ||
                UniqueOldPins.Contains(OldPinId) ||
                UniqueNewBindings.Contains(NewBindingId))
            {
                return SetSemanticError(
                    OutError, TEXT("INVALID_INPUT"), Path,
                    TEXT("Mapping IDs must be canonical and unique on both sides."),
                    TEXT("Use each old pin and new binding at most once."));
            }
            UniqueOldPins.Add(OldPinId);
            UniqueNewBindings.Add(NewBindingId);
            OutRequest.PinMappings.Add({OldPinId, NewBindingId});
        }
    }
    return true;
}

UEdGraphPin* ResolveSemanticPin(
    UEdGraph* Graph,
    const FString& PinId,
    FError& OutError,
    const FString& Path)
{
    FGuid Guid;
    if (!Graph || !ParseTargetId(PinId, ETargetKind::Pin, Guid))
    {
        SetSemanticError(
            OutError,
            TEXT("INVALID_INPUT"),
            Path,
            TEXT("Expected one canonical persisted pin ID."),
            TEXT("Inspect the graph and use a current pin ID."));
        return nullptr;
    }
    UEdGraphPin* Match = nullptr;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinId == Guid)
            {
                if (Match)
                {
                    SetSemanticError(
                        OutError,
                        TEXT("PRECONDITION_FAILED"),
                        Path,
                        TEXT("The stable pin ID is no longer unique in the graph."),
                        TEXT("Inspect the graph again before retrying."));
                    return nullptr;
                }
                Match = Pin;
            }
        }
    }
    if (!Match)
    {
        SetSemanticError(
            OutError,
            TEXT("PRECONDITION_FAILED"),
            Path,
            TEXT("The stable pin no longer exists in graph_id."),
            TEXT("Inspect the graph again and retry with current IDs."));
    }
    return Match;
}

FString NormalizeSemanticName(const FString& Value)
{
    FString Result = Value;
    Result.TrimStartAndEndInline();
    Result.ToLowerInline();
    return Result;
}

FString TemplatePinKey(const UEdGraphPin* Pin, const int32 Occurrence)
{
    const FString Direction = Pin->Direction == EGPD_Input
        ? TEXT("input")
        : TEXT("output");
    const FString TypeJson = CanonicalJsonString(
        MakeShared<FJsonValueObject>(SerializeTypeSpec(Pin->PinType)));
    return Direction + TEXT("\n") + Pin->PinName.ToString() + TEXT("\n") +
        TypeJson + TEXT("\n") + FString::FromInt(Occurrence);
}

bool ClassifySemanticResponse(
    const FPinConnectionResponse& Response,
    const bool bAllowConversion,
    FSemanticResponse& OutResponse)
{
    OutResponse.Message = Response.Message.ToString();
    switch (Response.Response)
    {
    case CONNECT_RESPONSE_MAKE:
        OutResponse.Kind = TEXT("direct");
        return true;
    case CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE:
        if (!bAllowConversion)
        {
            return false;
        }
        OutResponse.Kind = TEXT("conversion_node");
        OutResponse.bRequiresConversion = true;
        return true;
    case CONNECT_RESPONSE_MAKE_WITH_PROMOTION:
        if (!bAllowConversion)
        {
            return false;
        }
        OutResponse.Kind = TEXT("promotion");
        OutResponse.bRequiresConversion = true;
        return true;
    default:
        return false;
    }
}

bool ClassifyInsertionResponse(
    const FPinConnectionResponse& Response,
    UEdGraphPin* PinA,
    UEdGraphPin* PinB,
    UEdGraphPin* OldSourcePin,
    UEdGraphPin* OldTargetPin,
    const bool bAllowConversion,
    FSemanticResponse& OutResponse)
{
    if (ClassifySemanticResponse(Response, bAllowConversion, OutResponse))
    {
        return true;
    }
    const bool bBreakA =
        Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A ||
        Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB;
    const bool bBreakB =
        Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B ||
        Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB;
    if (!bBreakA && !bBreakB)
    {
        return false;
    }

    bool bBreaksPlannedSource = false;
    bool bBreaksPlannedTarget = false;
    auto ValidateBrokenPin = [&](UEdGraphPin* Pin)
    {
        if (!Pin)
        {
            return false;
        }
        for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            if (Pin == OldSourcePin && LinkedPin == OldTargetPin)
            {
                bBreaksPlannedSource = true;
            }
            else if (Pin == OldTargetPin && LinkedPin == OldSourcePin)
            {
                bBreaksPlannedTarget = true;
            }
            else
            {
                return false;
            }
        }
        return true;
    };
    if ((bBreakA && !ValidateBrokenPin(PinA)) ||
        (bBreakB && !ValidateBrokenPin(PinB)) ||
        (!bBreaksPlannedSource && !bBreaksPlannedTarget))
    {
        return false;
    }
    OutResponse = FSemanticResponse{};
    OutResponse.Message = Response.Message.ToString();
    if (bBreaksPlannedSource && bBreaksPlannedTarget)
    {
        OutResponse.Kind = TEXT("break_planned_both_links");
    }
    else if (bBreaksPlannedSource)
    {
        OutResponse.Kind = TEXT("break_planned_source_link");
    }
    else
    {
        OutResponse.Kind = TEXT("break_planned_target_link");
    }
    return true;
}

void CollectTemplatePins(
    UEdGraphNode* TemplateNode,
    TArray<FSemanticTemplatePin>& OutInputs,
    TArray<FSemanticTemplatePin>& OutOutputs)
{
    TMap<FString, int32> Occurrences;
    if (!TemplateNode)
    {
        return;
    }
    for (UEdGraphPin* Pin : TemplateNode->Pins)
    {
        if (!Pin || Pin->bHidden)
        {
            continue;
        }
        FSemanticTemplatePin Descriptor;
        Descriptor.Pin = Pin;
        Descriptor.Name = Pin->PinName.ToString();
        Descriptor.Direction = Pin->Direction == EGPD_Input
            ? TEXT("input")
            : TEXT("output");
        Descriptor.Type = SerializeTypeSpec(Pin->PinType);
        Descriptor.TypeJson = CanonicalJsonString(
            MakeShared<FJsonValueObject>(Descriptor.Type.ToSharedRef()));
        const FString Signature = Descriptor.Direction + TEXT("\n") +
            Descriptor.Name + TEXT("\n") + Descriptor.TypeJson;
        Descriptor.Occurrence = Occurrences.FindOrAdd(Signature)++;
        Descriptor.Key = TemplatePinKey(Pin, Descriptor.Occurrence);
        (Pin->Direction == EGPD_Input ? OutInputs : OutOutputs)
            .Add(MoveTemp(Descriptor));
    }
}

TArray<FSemanticCandidate> BuildSemanticCandidates(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UEdGraphPin* SourcePin,
    UEdGraphPin* TargetPin,
    const FConnectionSuggestionRequest& Request)
{
    TArray<FPaletteCandidate> NativeCandidates;
    const TArray<UEdGraphPin*> ContextPins = {SourcePin, TargetPin};
    BuildCandidates(
        Blueprint,
        Graph,
        ContextPins,
        Request.Query,
        Request.Filters,
        NativeCandidates);
    const UEdGraphSchema* Schema = Graph->GetSchema();
    TArray<FSemanticCandidate> Result;
    Result.Reserve(NativeCandidates.Num());
    for (const FPaletteCandidate& Native : NativeCandidates)
    {
        UEdGraphNode* Template = GetBoundTemplateNode(
            Native, Graph, Native.Bindings);
        TArray<FSemanticTemplatePin> Inputs;
        TArray<FSemanticTemplatePin> Outputs;
        CollectTemplatePins(Template, Inputs, Outputs);

        FSemanticCandidate Semantic;
        Semantic.Candidate = Native;
        for (const FSemanticTemplatePin& Input : Inputs)
        {
            FSemanticResponse SourceResponse;
            if (!ClassifyInsertionResponse(
                    Schema->CanCreateConnection(SourcePin, Input.Pin),
                    SourcePin,
                    Input.Pin,
                    SourcePin,
                    TargetPin,
                    Request.bAllowConversion,
                    SourceResponse))
            {
                continue;
            }
            for (const FSemanticTemplatePin& Output : Outputs)
            {
                FSemanticResponse TargetResponse;
                if (!ClassifyInsertionResponse(
                        Schema->CanCreateConnection(Output.Pin, TargetPin),
                        Output.Pin,
                        TargetPin,
                        SourcePin,
                        TargetPin,
                        Request.bAllowConversion,
                        TargetResponse))
                {
                    continue;
                }
                FSemanticPair Pair;
                Pair.Input = Input;
                Pair.Output = Output;
                Pair.SourceResponse = SourceResponse;
                Pair.TargetResponse = TargetResponse;
                Pair.ConversionCount =
                    (SourceResponse.bRequiresConversion ? 1 : 0) +
                    (TargetResponse.bRequiresConversion ? 1 : 0);
                Pair.Key = Input.Key + TEXT("\n") + Output.Key + TEXT("\n") +
                    SourceResponse.Kind + TEXT("\n") + TargetResponse.Kind;
                Pair.SortKey = FString::Printf(
                    TEXT("%d\n%s\n%s\n%s\n%s\n%08d\n%08d\n%s"),
                    Pair.ConversionCount,
                    *NormalizeSemanticName(Input.Name),
                    *NormalizeSemanticName(Output.Name),
                    *Input.TypeJson,
                    *Output.TypeJson,
                    Input.Occurrence,
                    Output.Occurrence,
                    *Pair.Key);
                Semantic.Pairs.Add(MoveTemp(Pair));
            }
        }
        Semantic.Pairs.Sort([](const FSemanticPair& Left, const FSemanticPair& Right)
        {
            return Left.SortKey < Right.SortKey;
        });
        if (!Semantic.Pairs.IsEmpty())
        {
            TArray<FString> PairKeys;
            for (const FSemanticPair& Pair : Semantic.Pairs)
            {
                PairKeys.Add(Pair.Key);
            }
            Semantic.SortKey = Semantic.Pairs[0].SortKey + TEXT("\n") +
                Native.SortKey + TEXT("\n") + FString::Join(PairKeys, TEXT("\n"));
            Result.Add(MoveTemp(Semantic));
        }
    }
    Result.Sort([](const FSemanticCandidate& Left, const FSemanticCandidate& Right)
    {
        return Left.SortKey < Right.SortKey;
    });
    return Result;
}

FString SemanticResultDigest(const TArray<FSemanticCandidate>& Candidates)
{
    TArray<FString> Values;
    for (const FSemanticCandidate& Candidate : Candidates)
    {
        Values.Add(Candidate.Candidate.CandidateKey);
        for (const FSemanticPair& Pair : Candidate.Pairs)
        {
            Values.Add(Pair.Key);
        }
    }
    return TEXT("sha1:") + Sha1Hex(FString::Join(Values, TEXT("\n")));
}

FPaletteContext MakeConnectionContext(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FConnectionSuggestionRequest& Request,
    const FString& ResultDigestValue)
{
    const TSharedRef<FJsonObject> DigestValue = MakeShared<FJsonObject>();
    DigestValue->SetStringField(TEXT("source_pin_id"), Request.SourcePinId);
    DigestValue->SetStringField(TEXT("target_pin_id"), Request.TargetPinId);
    DigestValue->SetStringField(TEXT("query"), Request.Query);
    TSharedPtr<FJsonObject> Filters;
    FError Ignored;
    ParseJsonObject(Request.FiltersJson, Filters, Ignored);
    DigestValue->SetObjectField(
        TEXT("filters"), Filters.IsValid() ? Filters : MakeShared<FJsonObject>());
    DigestValue->SetBoolField(
        TEXT("allow_conversion"), Request.bAllowConversion);

    FPaletteContext Context;
    Context.AssetPath = Blueprint->GetPathName();
    Context.GraphId = Request.GraphId;
    Context.GraphSchemaPath = Graph->GetSchema()->GetClass()->GetPathName();
    Context.Kind = EPaletteContextKind::Connection;
    Context.SourcePinId = Request.SourcePinId;
    Context.TargetPinId = Request.TargetPinId;
    Context.bAllowConversion = Request.bAllowConversion;
    Context.RequestDigest = TEXT("sha1:") + CanonicalQueryDigest(DigestValue);
    Context.ResultDigest = ResultDigestValue;
    Context.Limit = Request.Limit;
    return Context;
}

TSharedRef<FJsonObject> SerializeSemanticResponse(
    const FSemanticResponse& Response)
{
    const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
    Value->SetStringField(TEXT("kind"), Response.Kind);
    Value->SetStringField(TEXT("message"), Response.Message);
    Value->SetBoolField(
        TEXT("requires_conversion"), Response.bRequiresConversion);
    return Value;
}

bool SerializeSemanticCard(
    const FSemanticCandidate& Semantic,
    const FPaletteContext& Context,
    const FConnectionSuggestionRequest& Request,
    const TSharedRef<FJsonObject>& OutCard,
    int32& OutBindingCount)
{
    const FPaletteCandidate& Candidate = Semantic.Candidate;
    FPaletteActionRecord Action;
    Action.Context = Context;
    Action.Query = Request.Query;
    Action.FiltersJson = Request.FiltersJson;
    Action.CandidateKey = Candidate.CandidateKey;
    Action.SpawnerSignature = Candidate.SpawnerSignature;
    Action.OwnerPath = Candidate.OwnerPath;
    Action.SortKey = Semantic.SortKey;
    for (const FPaletteBindingCandidate& Binding : Candidate.BindingDetails)
    {
        Action.BindingPaths.Add(Binding.ObjectPath);
    }
    const FString ActionId = RegisterPaletteActionToken(Action);
    if (ActionId.IsEmpty())
    {
        return false;
    }

    OutCard->SetStringField(TEXT("action_id"), ActionId);
    OutCard->SetStringField(TEXT("title"), Candidate.Title);
    OutCard->SetStringField(TEXT("category"), Candidate.Category);
    TArray<TSharedPtr<FJsonValue>> Keywords;
    for (const FString& Keyword : Candidate.Keywords)
    {
        Keywords.Add(MakeShared<FJsonValueString>(Keyword));
    }
    OutCard->SetArrayField(TEXT("keywords"), MoveTemp(Keywords));
    OutCard->SetStringField(TEXT("action_kind"), Candidate.ActionKind);
    OutCard->SetStringField(TEXT("node_class_path"), Candidate.NodeClassPath);
    OutCard->SetStringField(TEXT("owner_path"), Candidate.OwnerPath);
    OutCard->SetStringField(TEXT("member_path"), Candidate.MemberPath);
    if (Candidate.bPure.IsSet())
    {
        OutCard->SetBoolField(TEXT("pure"), Candidate.bPure.GetValue());
    }
    else
    {
        OutCard->SetField(TEXT("pure"), MakeShared<FJsonValueNull>());
    }
    OutCard->SetBoolField(TEXT("compatible"), true);
    OutCard->SetStringField(
        TEXT("compatibility_summary"),
        TEXT("Compatible with both stable pins according to the native K2 schema."));
    OutCard->SetBoolField(
        TEXT("requires_binding"), !Candidate.BindingDetails.IsEmpty());

    TArray<TSharedPtr<FJsonValue>> DynamicBindings;
    for (const FPaletteBindingCandidate& Binding : Candidate.BindingDetails)
    {
        FPaletteBindingRecord Record;
        Record.Kind = EPaletteBindingKind::Object;
        Record.ActionId = ActionId;
        Record.ObjectPath = Binding.ObjectPath;
        Record.ExpectedClassPath = Binding.ClassPath;
        const FString BindingId = RegisterPaletteBinding(Record);
        if (BindingId.IsEmpty())
        {
            return false;
        }
        const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
        Value->SetStringField(TEXT("binding_id"), BindingId);
        Value->SetStringField(TEXT("object_path"), Binding.ObjectPath);
        Value->SetStringField(TEXT("class_path"), Binding.ClassPath);
        DynamicBindings.Add(MakeShared<FJsonValueObject>(Value));
    }
    OutCard->SetArrayField(TEXT("bindings"), MoveTemp(DynamicBindings));

    TMap<FString, FString> TemplateBindingIds;
    auto ResolveBindingId = [&](const FSemanticTemplatePin& Pin)
    {
        if (const FString* Existing = TemplateBindingIds.Find(Pin.Key))
        {
            return *Existing;
        }
        FPaletteBindingRecord Record;
        Record.Kind = EPaletteBindingKind::TemplatePin;
        Record.ActionId = ActionId;
        Record.PinName = Pin.Name;
        Record.PinDirection = Pin.Direction;
        Record.PinTypeJson = Pin.TypeJson;
        Record.PinOccurrence = Pin.Occurrence;
        const FString BindingId = RegisterPaletteTemplatePinBinding(Record);
        TemplateBindingIds.Add(Pin.Key, BindingId);
        return BindingId;
    };

    TArray<TSharedPtr<FJsonValue>> PairValues;
    for (int32 Index = 0; Index < Semantic.Pairs.Num(); ++Index)
    {
        const FSemanticPair& Pair = Semantic.Pairs[Index];
        const FString InputBindingId = ResolveBindingId(Pair.Input);
        const FString OutputBindingId = ResolveBindingId(Pair.Output);
        if (InputBindingId.IsEmpty() || OutputBindingId.IsEmpty())
        {
            return false;
        }
        const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
        Value->SetStringField(TEXT("input_binding_id"), InputBindingId);
        Value->SetStringField(TEXT("output_binding_id"), OutputBindingId);
        Value->SetStringField(TEXT("input_pin_name"), Pair.Input.Name);
        Value->SetStringField(TEXT("output_pin_name"), Pair.Output.Name);
        Value->SetObjectField(TEXT("input_type"), Pair.Input.Type.ToSharedRef());
        Value->SetObjectField(TEXT("output_type"), Pair.Output.Type.ToSharedRef());
        Value->SetObjectField(
            TEXT("source_response"),
            SerializeSemanticResponse(Pair.SourceResponse));
        Value->SetObjectField(
            TEXT("target_response"),
            SerializeSemanticResponse(Pair.TargetResponse));
        Value->SetBoolField(
            TEXT("requires_conversion"), Pair.ConversionCount > 0);
        Value->SetNumberField(TEXT("rank"), Index);
        PairValues.Add(MakeShared<FJsonValueObject>(Value));
    }
    OutCard->SetArrayField(TEXT("binding_pairs"), MoveTemp(PairValues));
    OutBindingCount = Candidate.BindingDetails.Num() + TemplateBindingIds.Num();
    return true;
}

TSharedRef<FJsonObject> BuildSuggestionPage(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UEdGraphPin* SourcePin,
    UEdGraphPin* TargetPin,
    const FConnectionSuggestionRequest& Request,
    FError& OutError)
{
    const TArray<FSemanticCandidate> Candidates = BuildSemanticCandidates(
        Blueprint, Graph, SourcePin, TargetPin, Request);
    const FString Digest = SemanticResultDigest(Candidates);
    const FPaletteContext Context = MakeConnectionContext(
        Blueprint, Graph, Request, Digest);

    int32 StartIndex = 0;
    if (!Request.Cursor.IsEmpty())
    {
        FPaletteCursorRecord Cursor;
        if (!ResolvePaletteCursor(Request.Cursor, Context, Cursor, OutError))
        {
            return MakeShared<FJsonObject>();
        }
        while (StartIndex < Candidates.Num() &&
            Candidates[StartIndex].SortKey <= Cursor.LastSortKey)
        {
            ++StartIndex;
        }
        if (StartIndex >= Candidates.Num())
        {
            SetSemanticError(
                OutError,
                TEXT("PRECONDITION_FAILED"),
                TEXT("params.cursor"),
                TEXT("The semantic cursor no longer identifies a continuation item."),
                TEXT("Restart connection suggestions without a cursor."));
            return MakeShared<FJsonObject>();
        }
    }

    int32 ReturnedBindingCount = 0;
    int32 NextIndex = StartIndex;
    TArray<TSharedPtr<FJsonValue>> Items;
    while (NextIndex < Candidates.Num() && Items.Num() < Request.Limit)
    {
        const FSemanticCandidate& Candidate = Candidates[NextIndex];
        TSet<FString> TemplateKeys;
        for (const FSemanticPair& Pair : Candidate.Pairs)
        {
            TemplateKeys.Add(Pair.Input.Key);
            TemplateKeys.Add(Pair.Output.Key);
        }
        const int32 CandidateBindingCount =
            Candidate.Candidate.BindingDetails.Num() + TemplateKeys.Num();
        const int32 BudgetDecision = ClassifySemanticPageCandidateBudget(
            ReturnedBindingCount, CandidateBindingCount);
        if (BudgetDecision < 0)
        {
            SetSemanticError(
                OutError,
                TEXT("OPERATION_FAILED"),
                TEXT("data.items"),
                TEXT("One semantic action requires more capability bindings than the bounded registry can represent."),
                TEXT("Narrow the palette query so every returned action fits the 1,024-binding capability budget."));
            return MakeShared<FJsonObject>();
        }
        if (BudgetDecision == 0)
        {
            break;
        }
        const TSharedRef<FJsonObject> Card = MakeShared<FJsonObject>();
        int32 RegisteredCount = 0;
        if (!SerializeSemanticCard(
                Candidate, Context, Request, Card, RegisteredCount))
        {
            SetSemanticError(
                OutError,
                TEXT("OPERATION_FAILED"),
                TEXT("data.items"),
                TEXT("Failed to register one semantic palette capability."),
                TEXT("Repeat connection suggestions in the current editor session."));
            return MakeShared<FJsonObject>();
        }
        ReturnedBindingCount += RegisteredCount;
        Items.Add(MakeShared<FJsonValueObject>(Card));
        ++NextIndex;
    }

    FString NextCursor;
    if (NextIndex < Candidates.Num() && NextIndex > StartIndex)
    {
        FPaletteCursorRecord Cursor;
        Cursor.Context = Context;
        Cursor.LastSortKey = Candidates[NextIndex - 1].SortKey;
        NextCursor = RegisterPaletteCursor(Cursor);
    }
    const int32 ReturnedCount = Items.Num();
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("graph_id"), Request.GraphId);
    Data->SetStringField(TEXT("source_pin_id"), Request.SourcePinId);
    Data->SetStringField(TEXT("target_pin_id"), Request.TargetPinId);
    Data->SetBoolField(TEXT("allow_conversion"), Request.bAllowConversion);
    Data->SetArrayField(TEXT("items"), MoveTemp(Items));
    Data->SetNumberField(TEXT("total_count"), Candidates.Num());
    Data->SetNumberField(TEXT("returned_count"), ReturnedCount);
    Data->SetStringField(TEXT("next_cursor"), NextCursor);
    Data->SetStringField(TEXT("result_digest"), Digest);
    return Data;
}

UEdGraphPin* FindSpawnedPin(
    UEdGraphNode* Node,
    const FPaletteBindingRecord& Binding)
{
    TMap<FString, int32> Occurrences;
    UEdGraphPin* Match = nullptr;
    if (!Node)
    {
        return nullptr;
    }
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->bHidden)
        {
            continue;
        }
        const FString Direction = Pin->Direction == EGPD_Input
            ? TEXT("input")
            : TEXT("output");
        const FString TypeJson = CanonicalJsonString(
            MakeShared<FJsonValueObject>(SerializeTypeSpec(Pin->PinType)));
        const FString Signature = Direction + TEXT("\n") +
            Pin->PinName.ToString() + TEXT("\n") + TypeJson;
        const int32 Occurrence = Occurrences.FindOrAdd(Signature)++;
        if (Pin->PinName.ToString() == Binding.PinName &&
            Direction == Binding.PinDirection &&
            TypeJson == Binding.PinTypeJson &&
            Occurrence == Binding.PinOccurrence)
        {
            if (Match)
            {
                return nullptr;
            }
            Match = Pin;
        }
    }
    return Match;
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
    TSharedPtr<FJsonValue> Default = SerializeDefaultValue(
        Pin->PinType,
        Pin->DefaultValue,
        Pin->DefaultObject,
        Pin->DefaultTextValue);
    Result->SetField(
        TEXT("default"), Default.IsValid() ? Default : MakeShared<FJsonValueNull>());
    return Result;
}

FString SnapshotJson(const TSharedPtr<FJsonObject>& Snapshot)
{
    return Snapshot.IsValid()
        ? CanonicalJsonString(MakeShared<FJsonValueObject>(Snapshot.ToSharedRef()))
        : FString();
}

bool VerifyConnectedSpawnTopology(
    UEdGraphPin* SourcePin,
    UEdGraphPin* ActualPin,
    const FSemanticResponse& Response,
    const TArray<UEdGraphNode*>& AuxiliaryNodes)
{
    if (!SourcePin || !ActualPin)
    {
        return false;
    }
    UEdGraphPin* OutputPin = SourcePin->Direction == EGPD_Output
        ? SourcePin
        : ActualPin;
    UEdGraphPin* InputPin = SourcePin->Direction == EGPD_Output
        ? ActualPin
        : SourcePin;
    if (Response.Kind != TEXT("conversion_node"))
    {
        return OutputPin->LinkedTo.Contains(InputPin) &&
            InputPin->LinkedTo.Contains(OutputPin);
    }

    TSet<UEdGraphNode*> AuxiliarySet;
    for (UEdGraphNode* Node : AuxiliaryNodes)
    {
        if (Node)
        {
            AuxiliarySet.Add(Node);
        }
    }
    if (AuxiliarySet.IsEmpty())
    {
        return false;
    }

    TSet<UEdGraphNode*> GoalNodes;
    for (UEdGraphPin* LinkedPin : InputPin->LinkedTo)
    {
        UEdGraphNode* Owner = LinkedPin
            ? LinkedPin->GetOwningNodeUnchecked()
            : nullptr;
        if (Owner && LinkedPin->Direction == EGPD_Output &&
            AuxiliarySet.Contains(Owner))
        {
            GoalNodes.Add(Owner);
        }
    }

    TArray<UEdGraphNode*> Pending;
    TSet<UEdGraphNode*> Visited;
    for (UEdGraphPin* LinkedPin : OutputPin->LinkedTo)
    {
        UEdGraphNode* Owner = LinkedPin
            ? LinkedPin->GetOwningNodeUnchecked()
            : nullptr;
        if (Owner && LinkedPin->Direction == EGPD_Input &&
            AuxiliarySet.Contains(Owner))
        {
            Pending.Add(Owner);
        }
    }
    while (!Pending.IsEmpty())
    {
        UEdGraphNode* Node = Pending.Pop(EAllowShrinking::No);
        if (!Node || Visited.Contains(Node))
        {
            continue;
        }
        if (GoalNodes.Contains(Node))
        {
            return true;
        }
        Visited.Add(Node);
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output)
            {
                continue;
            }
            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                UEdGraphNode* Next = LinkedPin
                    ? LinkedPin->GetOwningNodeUnchecked()
                    : nullptr;
                if (Next && LinkedPin->Direction == EGPD_Input &&
                    AuxiliarySet.Contains(Next) && !Visited.Contains(Next))
                {
                    Pending.Add(Next);
                }
            }
        }
    }
    return false;
}

TArray<UEdGraphNode*> CollectAuxiliaryPathNodes(
    UEdGraphPin* OutputPin,
    UEdGraphPin* InputPin,
    const TArray<UEdGraphNode*>& AuxiliaryNodes)
{
    TArray<UEdGraphNode*> Result;
    if (!OutputPin || !InputPin || OutputPin->Direction != EGPD_Output ||
        InputPin->Direction != EGPD_Input)
    {
        return Result;
    }

    TSet<UEdGraphNode*> AuxiliarySet;
    for (UEdGraphNode* Node : AuxiliaryNodes)
    {
        if (Node)
        {
            AuxiliarySet.Add(Node);
        }
    }
    if (AuxiliarySet.IsEmpty())
    {
        return Result;
    }

    TSet<UEdGraphNode*> ForwardReachable;
    TArray<UEdGraphNode*> Pending;
    for (UEdGraphPin* LinkedPin : OutputPin->LinkedTo)
    {
        UEdGraphNode* Owner = LinkedPin
            ? LinkedPin->GetOwningNodeUnchecked()
            : nullptr;
        if (Owner && LinkedPin->Direction == EGPD_Input &&
            AuxiliarySet.Contains(Owner))
        {
            Pending.Add(Owner);
        }
    }
    while (!Pending.IsEmpty())
    {
        UEdGraphNode* Node = Pending.Pop(EAllowShrinking::No);
        if (!Node || ForwardReachable.Contains(Node))
        {
            continue;
        }
        ForwardReachable.Add(Node);
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output)
            {
                continue;
            }
            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                UEdGraphNode* Next = LinkedPin
                    ? LinkedPin->GetOwningNodeUnchecked()
                    : nullptr;
                if (Next && LinkedPin->Direction == EGPD_Input &&
                    AuxiliarySet.Contains(Next) &&
                    !ForwardReachable.Contains(Next))
                {
                    Pending.Add(Next);
                }
            }
        }
    }

    TSet<UEdGraphNode*> ReverseReachable;
    Pending.Reset();
    for (UEdGraphPin* LinkedPin : InputPin->LinkedTo)
    {
        UEdGraphNode* Owner = LinkedPin
            ? LinkedPin->GetOwningNodeUnchecked()
            : nullptr;
        if (Owner && LinkedPin->Direction == EGPD_Output &&
            AuxiliarySet.Contains(Owner))
        {
            Pending.Add(Owner);
        }
    }
    while (!Pending.IsEmpty())
    {
        UEdGraphNode* Node = Pending.Pop(EAllowShrinking::No);
        if (!Node || ReverseReachable.Contains(Node))
        {
            continue;
        }
        ReverseReachable.Add(Node);
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input)
            {
                continue;
            }
            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                UEdGraphNode* Previous = LinkedPin
                    ? LinkedPin->GetOwningNodeUnchecked()
                    : nullptr;
                if (Previous && LinkedPin->Direction == EGPD_Output &&
                    AuxiliarySet.Contains(Previous) &&
                    !ReverseReachable.Contains(Previous))
                {
                    Pending.Add(Previous);
                }
            }
        }
    }

    for (UEdGraphNode* Node : ForwardReachable)
    {
        if (ReverseReachable.Contains(Node))
        {
            Result.Add(Node);
        }
    }
    Result.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
    {
        return Left.NodeGuid < Right.NodeGuid;
    });
    return Result;
}

bool ValidateStableConnectedSpawnResult(
    UBlueprint* Blueprint,
    UEdGraphPin* SourcePin,
    UEdGraphNode* NewNode,
    const TArray<UEdGraphNode*>& AuxiliaryNodes)
{
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticConnectedSpawnFailurePoint ==
        ESemanticConnectedSpawnFailurePoint::ZeroVisiblePinGuid)
    {
        return false;
    }
#endif
    if (!Blueprint || !SourcePin || !NewNode ||
        !DescribePinTarget(Blueprint, SourcePin).Id.StartsWith(TEXT("pin:")) ||
        !DescribeNodeTarget(Blueprint, NewNode).Id.StartsWith(TEXT("node:")))
    {
        return false;
    }
    TSet<UEdGraphNode*> AffectedNodes = {NewNode};
    for (UEdGraphNode* Node : AuxiliaryNodes)
    {
        if (!Node ||
            !DescribeNodeTarget(Blueprint, Node).Id.StartsWith(TEXT("node:")))
        {
            return false;
        }
        AffectedNodes.Add(Node);
    }
    for (UEdGraphNode* Node : AffectedNodes)
    {
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin)
            {
                continue;
            }
            if (Node == NewNode && !Pin->bHidden &&
                !DescribePinTarget(Blueprint, Pin).Id.StartsWith(TEXT("pin:")))
            {
                return false;
            }
            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                if (!LinkedPin ||
                    !DescribePinTarget(Blueprint, Pin).Id.StartsWith(TEXT("pin:")) ||
                    !DescribePinTarget(Blueprint, LinkedPin).Id.StartsWith(TEXT("pin:")))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

FString RollbackSemanticFailure(
    FMutationScope& Scope,
    UBlueprint* Blueprint,
    const FString& GraphId,
    const TSharedPtr<FJsonObject>& BeforeSnapshot,
    const FString& Code,
    const FString& Path,
    const FString& Message,
    const FString& Hint,
    const TArray<FString>& AffectedIds,
    const FString& RollbackMessage,
    const bool bInjectResidual)
{
    const FRollbackResult Rollback = Scope.Rollback();
#if WITH_DEV_AUTOMATION_TESTS
    if (bInjectResidual)
    {
        UEdGraph* Graph = nullptr;
        FError InjectionError;
        if (ResolveStableGraph(Blueprint, GraphId, Graph, InjectionError, true) &&
            Graph)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node)
                {
                    ++Node->NodePosX;
                    break;
                }
            }
        }
    }
#endif
    TSharedPtr<FJsonObject> AfterSnapshot;
    FError SnapshotError;
    const bool bAfterSnapshotAvailable = BuildBlueprintGraphSnapshot(
        Blueprint, {GraphId}, AfterSnapshot, SnapshotError);
    const FString BeforeJson = SnapshotJson(BeforeSnapshot);
    const FString AfterJson = SnapshotJson(AfterSnapshot);
    const bool bSnapshotRestored = Rollback.bSucceeded &&
        bAfterSnapshotAvailable && Rollback.ResidualChanges.IsEmpty() &&
        BeforeJson == AfterJson;
    if (!bSnapshotRestored)
    {
        TSet<FString> StableAffectedIds;
        for (const FString& Id : AffectedIds)
        {
            FGuid Guid;
            if (ParseTargetId(Id, ETargetKind::Graph, Guid) ||
                ParseTargetId(Id, ETargetKind::Node, Guid) ||
                ParseTargetId(Id, ETargetKind::Pin, Guid))
            {
                StableAffectedIds.Add(Id);
            }
        }
        TArray<FString> SortedAffectedIds = StableAffectedIds.Array();
        SortedAffectedIds.Sort();
        TArray<TSharedPtr<FJsonValue>> AffectedValues;
        for (const FString& Id : SortedAffectedIds)
        {
            AffectedValues.Add(MakeShared<FJsonValueString>(Id));
        }
        const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(
            TEXT("before_digest"), TEXT("sha1:") + Sha1Hex(BeforeJson));
        Details->SetStringField(
            TEXT("after_digest"), TEXT("sha1:") + Sha1Hex(AfterJson));
        Details->SetArrayField(TEXT("affected_ids"), MoveTemp(AffectedValues));
        Details->SetArrayField(
            TEXT("residual_changes"), Rollback.ResidualChanges);
        Details->SetBoolField(
            TEXT("after_snapshot_available"), bAfterSnapshotAvailable);
        Details->SetBoolField(
            TEXT("deferred_to_workflow"), Rollback.bDeferredToWorkflow);
        return SemanticFailure(
            TEXT("ROLLBACK_FAILED"),
            TEXT("transaction"),
            RollbackMessage,
            TEXT("Inspect and resnapshot the graph before another mutation."),
            false,
            Details);
    }
    return SemanticFailure(Code, Path, Message, Hint);
}

FString RollbackConnectedFailure(
    FMutationScope& Scope,
    UBlueprint* Blueprint,
    const FString& GraphId,
    const TSharedPtr<FJsonObject>& BeforeSnapshot,
    const FString& Code,
    const FString& Path,
    const FString& Message,
    const FString& Hint,
    const TArray<FString>& AffectedIds)
{
    return RollbackSemanticFailure(
        Scope,
        Blueprint,
        GraphId,
        BeforeSnapshot,
        Code,
        Path,
        Message,
        Hint,
        AffectedIds,
        TEXT("Connected Blueprint spawn rollback left a graph delta."),
#if WITH_DEV_AUTOMATION_TESTS
        GSemanticConnectedSpawnFailurePoint ==
            ESemanticConnectedSpawnFailurePoint::AfterRollbackResidual
#else
        false
#endif
    );
}

TArray<TSharedPtr<FJsonValue>> SerializeFinalTopologyEdges(
    UBlueprint* Blueprint,
    UEdGraphNode* NewNode,
    const TArray<UEdGraphNode*>& AuxiliaryNodes,
    const FString& SkipEdgeKey)
{
    TSet<UEdGraphNode*> AffectedNodes = {NewNode};
    for (UEdGraphNode* Node : AuxiliaryNodes)
    {
        if (Node)
        {
            AffectedNodes.Add(Node);
        }
    }
    struct FStableEdge
    {
        FString Key;
        TSharedPtr<FJsonObject> Value;
    };
    TSet<FString> Seen;
    TArray<FStableEdge> Edges;
    for (UEdGraphNode* Node : AffectedNodes)
    {
        if (!Node)
        {
            continue;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin)
            {
                continue;
            }
            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                if (!LinkedPin || Pin->Direction == LinkedPin->Direction)
                {
                    continue;
                }
                UEdGraphPin* OutputPin = Pin->Direction == EGPD_Output
                    ? Pin
                    : LinkedPin;
                UEdGraphPin* InputPin = Pin->Direction == EGPD_Input
                    ? Pin
                    : LinkedPin;
                const FString SourceId =
                    DescribePinTarget(Blueprint, OutputPin).Id;
                const FString TargetId =
                    DescribePinTarget(Blueprint, InputPin).Id;
                const FString Key = SourceId + TEXT("\n") + TargetId;
                if (Key == SkipEdgeKey || Seen.Contains(Key))
                {
                    continue;
                }
                Seen.Add(Key);
                const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
                Response->SetStringField(TEXT("kind"), TEXT("direct"));
                Response->SetStringField(
                    TEXT("message"), TEXT("Final native topology edge."));
                Response->SetBoolField(TEXT("requires_conversion"), false);

                TSet<FString> EdgeAuxiliaryIds;
                for (UEdGraphPin* EdgePin : {OutputPin, InputPin})
                {
                    UEdGraphNode* Owner = EdgePin
                        ? EdgePin->GetOwningNodeUnchecked()
                        : nullptr;
                    if (Owner && AuxiliaryNodes.Contains(Owner))
                    {
                        EdgeAuxiliaryIds.Add(
                            DescribeNodeTarget(Blueprint, Owner).Id);
                    }
                }
                TArray<FString> SortedAuxiliaryIds = EdgeAuxiliaryIds.Array();
                SortedAuxiliaryIds.Sort();
                TArray<TSharedPtr<FJsonValue>> AuxiliaryValues;
                for (const FString& Id : SortedAuxiliaryIds)
                {
                    AuxiliaryValues.Add(MakeShared<FJsonValueString>(Id));
                }

                const TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
                Edge->SetStringField(TEXT("source_pin_id"), SourceId);
                Edge->SetStringField(TEXT("target_pin_id"), TargetId);
                Edge->SetObjectField(TEXT("response"), Response);
                Edge->SetArrayField(
                    TEXT("auxiliary_node_ids"), MoveTemp(AuxiliaryValues));
                Edges.Add({Key, Edge});
            }
        }
    }
    Edges.Sort([](const FStableEdge& Left, const FStableEdge& Right)
    {
        return Left.Key < Right.Key;
    });
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FStableEdge& Edge : Edges)
    {
        Values.Add(MakeShared<FJsonValueObject>(Edge.Value.ToSharedRef()));
    }
    return Values;
}

const FSemanticCandidate* FindExactSemanticCandidate(
    const TArray<FSemanticCandidate>& Candidates,
    const FPaletteActionRecord& Record)
{
    return Candidates.FindByPredicate(
        [&Record](const FSemanticCandidate& Item)
        {
            TArray<FString> BindingPaths;
            for (const FPaletteBindingCandidate& Binding :
                Item.Candidate.BindingDetails)
            {
                BindingPaths.Add(Binding.ObjectPath);
            }
            return Item.Candidate.CandidateKey == Record.CandidateKey &&
                Item.Candidate.SpawnerSignature == Record.SpawnerSignature &&
                Item.Candidate.OwnerPath == Record.OwnerPath &&
                BindingPaths == Record.BindingPaths;
        });
}

bool SemanticPinMatchesBinding(
    const FSemanticTemplatePin& Pin,
    const FPaletteBindingRecord& Binding)
{
    return Pin.Name == Binding.PinName &&
        Pin.Direction == Binding.PinDirection &&
        Pin.TypeJson == Binding.PinTypeJson &&
        Pin.Occurrence == Binding.PinOccurrence;
}

const FSemanticPair* FindExactSemanticPairByBindingIds(
    const FString& ActionId,
    const FSemanticCandidate& Candidate,
    const FString& InputBindingId,
    const FString& OutputBindingId)
{
    return Candidate.Pairs.FindByPredicate(
        [&](const FSemanticPair& Pair)
        {
            auto BindingIdForPin = [&](const FSemanticTemplatePin& Pin)
            {
                FPaletteBindingRecord Record;
                Record.Kind = EPaletteBindingKind::TemplatePin;
                Record.ActionId = ActionId;
                Record.PinName = Pin.Name;
                Record.PinDirection = Pin.Direction;
                Record.PinTypeJson = Pin.TypeJson;
                Record.PinOccurrence = Pin.Occurrence;
                return RegisterPaletteTemplatePinBinding(Record);
            };
            return BindingIdForPin(Pair.Input) == InputBindingId &&
                BindingIdForPin(Pair.Output) == OutputBindingId;
        });
}

TSet<FString> CollectStableGraphEdgeKeys(
    UBlueprint* Blueprint,
    UEdGraph* Graph)
{
    TSet<FString> Result;
    if (!Blueprint || !Graph)
    {
        return Result;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output)
            {
                continue;
            }
            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                if (LinkedPin && LinkedPin->Direction == EGPD_Input)
                {
                    Result.Add(
                        DescribePinTarget(Blueprint, Pin).Id + TEXT("\n") +
                        DescribePinTarget(Blueprint, LinkedPin).Id);
                }
            }
        }
    }
    return Result;
}

bool VerifyInsertionTopology(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UEdGraphPin* SourcePin,
    UEdGraphPin* TargetPin,
    UEdGraphPin* ActualInput,
    UEdGraphPin* ActualOutput,
    const FSemanticResponse& SourceResponse,
    const FSemanticResponse& TargetResponse,
    const TArray<UEdGraphNode*>& AuxiliaryNodes,
    const TSet<FString>& EdgesBefore)
{
    if (!Blueprint || !Graph || !SourcePin || !TargetPin ||
        !ActualInput || !ActualOutput ||
        SourcePin->LinkedTo.Contains(TargetPin) ||
        TargetPin->LinkedTo.Contains(SourcePin) ||
        !VerifyConnectedSpawnTopology(
            SourcePin, ActualInput, SourceResponse, AuxiliaryNodes) ||
        !VerifyConnectedSpawnTopology(
            ActualOutput, TargetPin, TargetResponse, AuxiliaryNodes))
    {
        return false;
    }
    const FString OldEdgeKey =
        DescribePinTarget(Blueprint, SourcePin).Id + TEXT("\n") +
        DescribePinTarget(Blueprint, TargetPin).Id;
    const TSet<FString> EdgesAfter = CollectStableGraphEdgeKeys(Blueprint, Graph);
    for (const FString& Edge : EdgesBefore)
    {
        if (Edge != OldEdgeKey && !EdgesAfter.Contains(Edge))
        {
            return false;
        }
    }
    return !EdgesAfter.Contains(OldEdgeKey);
}

FString InsertionSuccess(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FInsertionRequest& Request,
    UEdGraphPin* SourcePin,
    UEdGraphPin* TargetPin,
    UEdGraphNode* NewNode,
    UEdGraphPin* ActualInput,
    UEdGraphPin* ActualOutput,
    const FSemanticResponse& SourceResponse,
    const FSemanticResponse& TargetResponse,
    const TArray<UEdGraphNode*>& AuxiliaryNodes)
{
    TArray<TSharedPtr<FJsonValue>> PinIds;
    TArray<TSharedPtr<FJsonValue>> Pins;
    for (const UEdGraphPin* Pin : NewNode->Pins)
    {
        if (!Pin || Pin->bHidden)
        {
            continue;
        }
        const TSharedRef<FJsonObject> PinValue = SerializeSpawnPin(Blueprint, Pin);
        PinIds.Add(MakeShared<FJsonValueString>(
            PinValue->GetStringField(TEXT("id"))));
        Pins.Add(MakeShared<FJsonValueObject>(PinValue));
    }
    TArray<FString> AuxiliaryIds;
    for (UEdGraphNode* Node : AuxiliaryNodes)
    {
        AuxiliaryIds.Add(DescribeNodeTarget(Blueprint, Node).Id);
    }
    AuxiliaryIds.Sort();
    TArray<TSharedPtr<FJsonValue>> AuxiliaryValues;
    for (const FString& Id : AuxiliaryIds)
    {
        AuxiliaryValues.Add(MakeShared<FJsonValueString>(Id));
    }

    TArray<TSharedPtr<FJsonValue>> Connections;
    TSet<FString> ConnectionKeys;
    auto AddConnection = [&](UEdGraphPin* OutputPin,
                             UEdGraphPin* InputPin,
                             const FSemanticResponse& Response)
    {
        const FString SourceId = DescribePinTarget(Blueprint, OutputPin).Id;
        const FString TargetId = DescribePinTarget(Blueprint, InputPin).Id;
        const FString Key = SourceId + TEXT("\n") + TargetId;
        if (ConnectionKeys.Contains(Key))
        {
            return;
        }
        ConnectionKeys.Add(Key);
        const TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
        Edge->SetStringField(TEXT("source_pin_id"), SourceId);
        Edge->SetStringField(TEXT("target_pin_id"), TargetId);
        Edge->SetObjectField(
            TEXT("response"), SerializeSemanticResponse(Response));
        TArray<FString> PathAuxiliaryIds;
        for (UEdGraphNode* AuxiliaryNode : CollectAuxiliaryPathNodes(
                OutputPin, InputPin, AuxiliaryNodes))
        {
            PathAuxiliaryIds.Add(
                DescribeNodeTarget(Blueprint, AuxiliaryNode).Id);
        }
        PathAuxiliaryIds.Sort();
        TArray<TSharedPtr<FJsonValue>> PathAuxiliaryValues;
        for (const FString& Id : PathAuxiliaryIds)
        {
            PathAuxiliaryValues.Add(MakeShared<FJsonValueString>(Id));
        }
        Edge->SetArrayField(
            TEXT("auxiliary_node_ids"), MoveTemp(PathAuxiliaryValues));
        Connections.Add(MakeShared<FJsonValueObject>(Edge));
    };
    AddConnection(SourcePin, ActualInput, SourceResponse);
    AddConnection(ActualOutput, TargetPin, TargetResponse);
    for (const TSharedPtr<FJsonValue>& EdgeValue : SerializeFinalTopologyEdges(
            Blueprint, NewNode, AuxiliaryNodes, FString()))
    {
        const TSharedPtr<FJsonObject> Edge = EdgeValue->AsObject();
        const FString Key = Edge->GetStringField(TEXT("source_pin_id")) +
            TEXT("\n") + Edge->GetStringField(TEXT("target_pin_id"));
        if (!ConnectionKeys.Contains(Key))
        {
            ConnectionKeys.Add(Key);
            Connections.Add(EdgeValue);
        }
    }

    const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
    Position->SetNumberField(TEXT("x"), NewNode->NodePosX);
    Position->SetNumberField(TEXT("y"), NewNode->NodePosY);
    const TSharedRef<FJsonObject> ReplacedConnection = MakeShared<FJsonObject>();
    ReplacedConnection->SetStringField(
        TEXT("source_pin_id"), DescribePinTarget(Blueprint, SourcePin).Id);
    ReplacedConnection->SetStringField(
        TEXT("target_pin_id"), DescribePinTarget(Blueprint, TargetPin).Id);

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("graph_id"), Request.GraphId);
    Data->SetStringField(TEXT("action_id"), Request.ActionId);
    Data->SetStringField(TEXT("input_binding_id"), Request.InputBindingId);
    Data->SetStringField(TEXT("output_binding_id"), Request.OutputBindingId);
    Data->SetStringField(
        TEXT("node_id"), DescribeNodeTarget(Blueprint, NewNode).Id);
    Data->SetStringField(TEXT("class_path"), NewNode->GetClass()->GetPathName());
    Data->SetObjectField(TEXT("position"), Position);
    Data->SetArrayField(TEXT("pin_ids"), MoveTemp(PinIds));
    Data->SetArrayField(TEXT("pins"), MoveTemp(Pins));
    Data->SetArrayField(TEXT("auxiliary_node_ids"), MoveTemp(AuxiliaryValues));
    Data->SetObjectField(TEXT("replaced_connection"), ReplacedConnection);
    Data->SetArrayField(TEXT("connections"), MoveTemp(Connections));
    Data->SetBoolField(TEXT("transaction_recorded"), true);
    Data->SetBoolField(TEXT("saved"), false);
    const FString NodeId = DescribeNodeTarget(Blueprint, NewNode).Id;
    auto MakeChange = [](const FString& Kind,
                         const FString& TargetId,
                         const TSharedRef<FJsonObject>& Details)
    {
        const TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
        Change->SetStringField(TEXT("kind"), Kind);
        Change->SetStringField(TEXT("target_id"), TargetId);
        Change->SetObjectField(TEXT("details"), Details);
        return MakeShared<FJsonValueObject>(Change);
    };
    auto MakeConnectionChange = [&](const FString& Kind,
                                    UEdGraphPin* OutputPin,
                                    UEdGraphPin* InputPin)
    {
        const FString SourceId = DescribePinTarget(Blueprint, OutputPin).Id;
        const FString TargetId = DescribePinTarget(Blueprint, InputPin).Id;
        const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("source_pin_id"), SourceId);
        Details->SetStringField(TEXT("target_pin_id"), TargetId);
        return MakeChange(
            Kind,
            TargetId,
            Details);
    };
    const TSharedRef<FJsonObject> CreateDetails = MakeShared<FJsonObject>();
    CreateDetails->SetStringField(TEXT("graph_id"), Request.GraphId);
    CreateDetails->SetStringField(
        TEXT("class_path"), NewNode->GetClass()->GetPathName());
    CreateDetails->SetStringField(TEXT("action_id"), Request.ActionId);
    TArray<TSharedPtr<FJsonValue>> Changes = {
        MakeChange(TEXT("create"), NodeId, CreateDetails),
        MakeConnectionChange(TEXT("delete"), SourcePin, TargetPin),
        MakeConnectionChange(TEXT("create"), SourcePin, ActualInput),
        MakeConnectionChange(TEXT("create"), ActualOutput, TargetPin)};

    auto MakeNextAction = [&](const FString& Action)
    {
        const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
        const TSharedRef<FJsonObject> Next = MakeShared<FJsonObject>();
        Next->SetStringField(TEXT("domain"), TEXT("blueprint"));
        Next->SetStringField(TEXT("action"), Action);
        Next->SetObjectField(TEXT("params"), Params);
        return MakeShared<FJsonValueObject>(Next);
    };
    const TSharedRef<FJsonObject> Result = MakeSuccess(
        TEXT("Inserted one native Blueprint palette action into an existing edge."),
        Data);
    Result->SetArrayField(TEXT("changes"), MoveTemp(Changes));
    Result->SetArrayField(
        TEXT("next_actions"),
        {MakeNextAction(TEXT("snapshot_blueprint_graph")),
         MakeNextAction(TEXT("compile_blueprint")),
         MakeNextAction(TEXT("get_blueprint_health"))});
    return SerializeResult(Result);
}

FString ConnectedSpawnSuccess(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FConnectedSpawnRequest& Request,
    UEdGraphPin* SourcePin,
    UEdGraphNode* NewNode,
    UEdGraphPin* NewPin,
    const FSemanticResponse& Response,
    const TArray<UEdGraphNode*>& AuxiliaryNodes)
{
    TArray<TSharedPtr<FJsonValue>> PinIds;
    TArray<TSharedPtr<FJsonValue>> Pins;
    for (const UEdGraphPin* Pin : NewNode->Pins)
    {
        if (!Pin || Pin->bHidden)
        {
            continue;
        }
        const TSharedRef<FJsonObject> PinValue = SerializeSpawnPin(Blueprint, Pin);
        PinIds.Add(MakeShared<FJsonValueString>(
            PinValue->GetStringField(TEXT("id"))));
        Pins.Add(MakeShared<FJsonValueObject>(PinValue));
    }
    TArray<FString> AuxiliaryIds;
    for (UEdGraphNode* Node : AuxiliaryNodes)
    {
        AuxiliaryIds.Add(DescribeNodeTarget(Blueprint, Node).Id);
    }
    AuxiliaryIds.Sort();
    TArray<TSharedPtr<FJsonValue>> AuxiliaryValues;
    for (const FString& Id : AuxiliaryIds)
    {
        AuxiliaryValues.Add(MakeShared<FJsonValueString>(Id));
    }

    UEdGraphPin* OutputPin = SourcePin->Direction == EGPD_Output
        ? SourcePin
        : NewPin;
    UEdGraphPin* InputPin = SourcePin->Direction == EGPD_Output
        ? NewPin
        : SourcePin;
    const TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
    Edge->SetStringField(
        TEXT("source_pin_id"), DescribePinTarget(Blueprint, OutputPin).Id);
    Edge->SetStringField(
        TEXT("target_pin_id"), DescribePinTarget(Blueprint, InputPin).Id);
    Edge->SetObjectField(TEXT("response"), SerializeSemanticResponse(Response));
    Edge->SetArrayField(TEXT("auxiliary_node_ids"), AuxiliaryValues);
    const FString RequestedEdgeKey =
        Edge->GetStringField(TEXT("source_pin_id")) + TEXT("\n") +
        Edge->GetStringField(TEXT("target_pin_id"));
    TArray<TSharedPtr<FJsonValue>> Connections = {
        MakeShared<FJsonValueObject>(Edge)};
    Connections.Append(SerializeFinalTopologyEdges(
        Blueprint,
        NewNode,
        AuxiliaryNodes,
        Response.bRequiresConversion ? FString() : RequestedEdgeKey));

    const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
    Position->SetNumberField(TEXT("x"), NewNode->NodePosX);
    Position->SetNumberField(TEXT("y"), NewNode->NodePosY);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("graph_id"), Request.GraphId);
    Data->SetStringField(TEXT("action_id"), Request.ActionId);
    Data->SetStringField(
        TEXT("source_pin_id"), DescribePinTarget(Blueprint, SourcePin).Id);
    Data->SetStringField(
        TEXT("connection_binding_id"), Request.ConnectionBindingId);
    Data->SetStringField(
        TEXT("node_id"), DescribeNodeTarget(Blueprint, NewNode).Id);
    Data->SetStringField(TEXT("class_path"), NewNode->GetClass()->GetPathName());
    Data->SetObjectField(TEXT("position"), Position);
    Data->SetArrayField(TEXT("pin_ids"), MoveTemp(PinIds));
    Data->SetArrayField(TEXT("pins"), MoveTemp(Pins));
    Data->SetArrayField(TEXT("auxiliary_node_ids"), MoveTemp(AuxiliaryValues));
    Data->SetArrayField(TEXT("connections"), MoveTemp(Connections));
    Data->SetBoolField(TEXT("transaction_recorded"), true);
    Data->SetBoolField(TEXT("saved"), false);
    return SerializeResult(MakeSuccess(
        TEXT("Spawned and connected one native Blueprint palette action."),
        Data));
}
}

FString UMCPythonHelper::SuggestBlueprintNodesForConnection(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
#if UE_VERSION_NEWER_THAN(5, 7, 99) || UE_VERSION_OLDER_THAN(5, 7, 0)
    return UnsupportedSemanticVersion(TEXT("Blueprint connection suggestions"));
#else
    if (!Blueprint)
    {
        return MissingBlueprint();
    }
    FConnectionSuggestionRequest Request;
    FError Error;
    if (!ParseConnectionSuggestionRequest(RequestJson, Request, Error))
    {
        return SemanticFailure(Error);
    }
    UEdGraph* Graph = nullptr;
    if (!ResolveStableGraph(
            Blueprint, Request.GraphId, Graph, Error, false))
    {
        return SemanticFailure(Error);
    }
    UEdGraphPin* SourcePin = ResolveSemanticPin(
        Graph, Request.SourcePinId, Error, TEXT("params.source_pin_id"));
    UEdGraphPin* TargetPin = ResolveSemanticPin(
        Graph, Request.TargetPinId, Error, TEXT("params.target_pin_id"));
    if (!SourcePin || !TargetPin)
    {
        return SemanticFailure(Error);
    }
    if (SourcePin->Direction != EGPD_Output ||
        TargetPin->Direction != EGPD_Input)
    {
        return SemanticFailure(
            TEXT("INVALID_INPUT"),
            TEXT("params.source_pin_id"),
            TEXT("source_pin_id must be output and target_pin_id must be input."),
            TEXT("Inspect pin directions and retry with output-to-input order."));
    }
    const bool bSourceExec =
        SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
    const bool bTargetExec =
        TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
    if (bSourceExec != bTargetExec)
    {
        return SemanticFailure(
            TEXT("INVALID_INPUT"),
            TEXT("params.target_pin_id"),
            TEXT("Execution and data pins cannot share one bridge request."),
            TEXT("Choose two execution pins or two compatible data pins."));
    }
    const TSharedRef<FJsonObject> Data = BuildSuggestionPage(
        Blueprint, Graph, SourcePin, TargetPin, Request, Error);
    if (!Error.Code.IsEmpty())
    {
        return SemanticFailure(Error);
    }
    return SerializeResult(MakeSuccess(
        FString::Printf(
            TEXT("Returned %d semantic Blueprint connection actions."),
            Data->GetIntegerField(TEXT("returned_count"))),
        Data));
#endif
}

FString UMCPythonHelper::AddBlueprintConnectedActionNode(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
#if UE_VERSION_NEWER_THAN(5, 7, 99) || UE_VERSION_OLDER_THAN(5, 7, 0)
    return UnsupportedSemanticVersion(TEXT("Blueprint connected spawn"));
#else
    if (!Blueprint)
    {
        return MissingBlueprint();
    }
    FConnectedSpawnRequest Request;
    FError Error;
    if (!ParseConnectedSpawnRequest(RequestJson, Request, Error))
    {
        return SemanticFailure(Error);
    }
    UEdGraph* Graph = nullptr;
    if (!ResolveStableGraph(Blueprint, Request.GraphId, Graph, Error, true))
    {
        return SemanticFailure(Error);
    }
    UEdGraphPin* SourcePin = ResolveSemanticPin(
        Graph, Request.PinId, Error, TEXT("params.pin_id"));
    if (!SourcePin)
    {
        return SemanticFailure(Error);
    }

    FPaletteContextExpectation Expected;
    Expected.AssetPath = Blueprint->GetPathName();
    Expected.GraphId = Request.GraphId;
    Expected.GraphSchemaPath = Graph->GetSchema()->GetClass()->GetPathName();
    Expected.Kind = EPaletteContextKind::Pin;
    Expected.SourcePinId = Request.PinId;
    FPaletteActionRecord Action;
    if (!ResolvePaletteActionToken(
            Request.ActionId, Expected, Action, Error))
    {
        return SemanticFailure(Error);
    }

    FPaletteFilters Filters;
    if (!ParseStoredFilters(Action.FiltersJson, Filters, Error))
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The action's stored filter context is no longer valid."),
            TEXT("Repeat pin suggestions and use a current action."));
    }
    TArray<FPaletteCandidate> Candidates;
    const TArray<UEdGraphPin*> ContextPins = {SourcePin};
    BuildCandidates(
        Blueprint, Graph, ContextPins, Action.Query, Filters, Candidates);
    if (ResultDigest(Candidates) != Action.Context.ResultDigest)
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The native pin action result set has changed."),
            TEXT("Repeat pin suggestions and use a current action."));
    }
    const FPaletteCandidate* Candidate = FindExactCandidate(Candidates, Action);
    if (!Candidate)
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The selected native pin action is no longer available."),
            TEXT("Repeat pin suggestions and choose a current action."));
    }

    TArray<FPaletteBindingRecord> DynamicRecords;
    IBlueprintNodeBinder::FBindingSet DynamicBindings;
    if (!ResolveDynamicBindingObjects(
            Request.ActionId,
            Request.BindingIds,
            DynamicRecords,
            DynamicBindings,
            Error))
    {
        return SemanticFailure(Error);
    }
    TArray<FString> DynamicPaths;
    for (const FPaletteBindingRecord& Record : DynamicRecords)
    {
        DynamicPaths.Add(Record.ObjectPath);
    }
    DynamicPaths.Sort();
    if (DynamicPaths != Action.BindingPaths)
    {
        return SemanticFailure(
            TEXT("INVALID_INPUT"),
            TEXT("params.bindings"),
            TEXT("bindings must exactly match the selected action."),
            TEXT("Pass the complete bindings array returned with the action."));
    }
    FPaletteBindingRecord TemplateBinding;
    if (!ResolvePaletteTemplatePinBinding(
            Request.ActionId,
            Request.ConnectionBindingId,
            TemplateBinding,
            Error))
    {
        return SemanticFailure(Error);
    }
    UEdGraphNode* TemplateNode = GetBoundTemplateNode(
        *Candidate, Graph, DynamicBindings);
    UEdGraphPin* TemplatePin = FindSpawnedPin(TemplateNode, TemplateBinding);
    if (!TemplatePin)
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.connection_binding_id"),
            TEXT("The selected template pin shape has changed."),
            TEXT("Repeat pin suggestions and select a current binding."));
    }
    FSemanticResponse Preflight;
    if (!ClassifySemanticResponse(
            Graph->GetSchema()->CanCreateConnection(SourcePin, TemplatePin),
            Request.bAllowConversion,
            Preflight))
    {
        return SemanticFailure(
            Request.bAllowConversion
                ? TEXT("PRECONDITION_FAILED")
                : TEXT("INVALID_INPUT"),
            TEXT("params.connection_binding_id"),
            TEXT("The selected template pin is not connectable under the requested conversion policy."),
            TEXT("Repeat pin suggestions or explicitly allow a reported conversion."));
    }

    TSharedPtr<FJsonObject> BeforeSnapshot;
    if (!BuildBlueprintGraphSnapshot(
            Blueprint, {Request.GraphId}, BeforeSnapshot, Error))
    {
        return SemanticFailure(Error);
    }
    TSet<UEdGraphNode*> NodesBefore;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node)
        {
            NodesBefore.Add(Node);
        }
    }

    FMutationScope Scope(NSLOCTEXT(
        "MCPython", "AddBlueprintConnectedActionNode",
        "Add connected Blueprint palette node"));
    if (!Scope.IsValid())
    {
        return SemanticFailure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Could not begin a connected Blueprint transaction."),
            TEXT("Close any conflicting editor transaction and retry."));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    Scope.Modify(SourcePin->GetOwningNode());
    for (UEdGraphPin* LinkedPin : SourcePin->LinkedTo)
    {
        if (LinkedPin)
        {
            Scope.Modify(LinkedPin->GetOwningNode());
        }
    }

    UEdGraphNode* NewNode = Candidate->Spawner->Invoke(
        Graph,
        DynamicBindings,
        FVector2D(Request.PositionX, Request.PositionY));
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticConnectedSpawnFailurePoint ==
            ESemanticConnectedSpawnFailurePoint::OutOfGraphResult &&
        NewNode)
    {
        Graph->Nodes.Remove(NewNode);
    }
#endif
    auto RollbackFailure = [&](const FString& Code,
                               const FString& Path,
                               const FString& Message,
                               const FString& Hint)
    {
        TArray<FString> AffectedIds = {Request.GraphId, Request.PinId};
        if (SourcePin && SourcePin->GetOwningNodeUnchecked())
        {
            AffectedIds.Add(DescribeNodeTarget(
                Blueprint, SourcePin->GetOwningNodeUnchecked()).Id);
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node || (Node != NewNode && NodesBefore.Contains(Node)))
            {
                continue;
            }
            AffectedIds.Add(DescribeNodeTarget(Blueprint, Node).Id);
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && !Pin->bHidden)
                {
                    AffectedIds.Add(DescribePinTarget(Blueprint, Pin).Id);
                }
            }
        }
        return RollbackConnectedFailure(
            Scope,
            Blueprint,
            Request.GraphId,
            BeforeSnapshot,
            Code,
            Path,
            Message,
            Hint,
            AffectedIds);
    };
    if (!NewNode || NodesBefore.Contains(NewNode) ||
        NewNode->GetGraph() != Graph || !Graph->Nodes.Contains(NewNode))
    {
        return RollbackFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The native action did not create one new node in graph_id."),
            TEXT("Repeat pin suggestions or choose a non-singleton action."));
    }
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticConnectedSpawnFailurePoint ==
            ESemanticConnectedSpawnFailurePoint::AfterInvoke ||
        GSemanticConnectedSpawnFailurePoint ==
            ESemanticConnectedSpawnFailurePoint::AfterRollbackResidual)
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("test.failure_point"),
            TEXT("Injected connected-spawn failure after native invocation."),
            TEXT("Disable the automation failure point before retrying."));
    }
#endif
    Scope.Modify(NewNode);
    if (NewNode->GetClass()->GetPathName() != Candidate->NodeClassPath)
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The native spawner returned an unexpected node class."),
            TEXT("Repeat pin suggestions and report the action signature."));
    }
    UEdGraphPin* ActualPin = FindSpawnedPin(NewNode, TemplateBinding);
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticConnectedSpawnFailurePoint ==
        ESemanticConnectedSpawnFailurePoint::MissingActualPin)
    {
        ActualPin = nullptr;
    }
#endif
    if (!ActualPin)
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("params.connection_binding_id"),
            TEXT("The spawned node no longer has the selected template pin."),
            TEXT("Repeat pin suggestions and choose a current binding."));
    }
    FSemanticResponse ActualResponse;
    const bool bActualCompatible = ClassifySemanticResponse(
            Graph->GetSchema()->CanCreateConnection(SourcePin, ActualPin),
            Request.bAllowConversion,
            ActualResponse);
    bool bConnectionCreated = false;
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticConnectedSpawnFailurePoint !=
        ESemanticConnectedSpawnFailurePoint::BeforeTryCreate)
#endif
    {
        bConnectionCreated = bActualCompatible &&
            Graph->GetSchema()->TryCreateConnection(SourcePin, ActualPin);
    }
    if (!bActualCompatible || !bConnectionCreated)
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("params.connection_binding_id"),
            TEXT("K2 schema failed to create the preflight-approved connection."),
            TEXT("Repeat pin suggestions against the current graph state."));
    }

    TArray<UEdGraphNode*> AuxiliaryNodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && Node != NewNode && !NodesBefore.Contains(Node))
        {
            AuxiliaryNodes.Add(Node);
        }
    }
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticConnectedSpawnFailurePoint ==
        ESemanticConnectedSpawnFailurePoint::AfterTryCreateBreakTopology)
    {
        const TArray<UEdGraphPin*> Links = SourcePin->LinkedTo;
        for (UEdGraphPin* LinkedPin : Links)
        {
            UEdGraphNode* Owner = LinkedPin
                ? LinkedPin->GetOwningNodeUnchecked()
                : nullptr;
            if (Owner && (Owner == NewNode || !NodesBefore.Contains(Owner)))
            {
                SourcePin->BreakLinkTo(LinkedPin);
            }
        }
    }
#endif
    if (!VerifyConnectedSpawnTopology(
            SourcePin, ActualPin, ActualResponse, AuxiliaryNodes))
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("result.connections"),
            TEXT("The requested endpoints are not connected by the final native topology."),
            TEXT("Inspect the graph before retrying."));
    }
    if (!ValidateStableConnectedSpawnResult(
            Blueprint, SourcePin, NewNode, AuxiliaryNodes))
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("result"),
            TEXT("Spawned semantic targets or topology edges did not receive stable IDs."),
            TEXT("Retry after the graph has finished loading."));
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    return ConnectedSpawnSuccess(
        Blueprint,
        Graph,
        Request,
        SourcePin,
        NewNode,
        ActualPin,
        ActualResponse,
        AuxiliaryNodes);
#endif
}

FString UMCPythonHelper::InsertBlueprintActionNode(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
#if UE_VERSION_NEWER_THAN(5, 7, 99) || UE_VERSION_OLDER_THAN(5, 7, 0)
    return UnsupportedSemanticVersion(TEXT("Blueprint action insertion"));
#else
    if (!Blueprint)
    {
        return MissingBlueprint();
    }
    FInsertionRequest Request;
    FError Error;
    if (!ParseInsertionRequest(RequestJson, Request, Error))
    {
        return SemanticFailure(Error);
    }
    UEdGraph* Graph = nullptr;
    if (!ResolveStableGraph(Blueprint, Request.GraphId, Graph, Error, true))
    {
        return SemanticFailure(Error);
    }
    UEdGraphPin* SourcePin = ResolveSemanticPin(
        Graph, Request.SourcePinId, Error, TEXT("params.source_pin_id"));
    UEdGraphPin* TargetPin = ResolveSemanticPin(
        Graph, Request.TargetPinId, Error, TEXT("params.target_pin_id"));
    if (!SourcePin || !TargetPin)
    {
        return SemanticFailure(Error);
    }
    if (SourcePin->Direction != EGPD_Output ||
        TargetPin->Direction != EGPD_Input)
    {
        return SemanticFailure(
            TEXT("INVALID_INPUT"),
            TEXT("params.source_pin_id"),
            TEXT("source_pin_id must be output and target_pin_id must be input."),
            TEXT("Inspect pin directions and retry with output-to-input order."));
    }
    if (!SourcePin->LinkedTo.Contains(TargetPin) ||
        !TargetPin->LinkedTo.Contains(SourcePin))
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.target_pin_id"),
            TEXT("The named pins no longer share the exact direct edge."),
            TEXT("Inspect the graph and request current connection suggestions."));
    }

    FPaletteContextExpectation Expected;
    Expected.AssetPath = Blueprint->GetPathName();
    Expected.GraphId = Request.GraphId;
    Expected.GraphSchemaPath = Graph->GetSchema()->GetClass()->GetPathName();
    Expected.Kind = EPaletteContextKind::Connection;
    Expected.SourcePinId = Request.SourcePinId;
    Expected.TargetPinId = Request.TargetPinId;
    FPaletteActionRecord Action;
    if (!ResolvePaletteActionToken(Request.ActionId, Expected, Action, Error))
    {
        return SemanticFailure(Error);
    }

    FConnectionSuggestionRequest SuggestionRequest;
    SuggestionRequest.GraphId = Request.GraphId;
    SuggestionRequest.SourcePinId = Request.SourcePinId;
    SuggestionRequest.TargetPinId = Request.TargetPinId;
    SuggestionRequest.Query = Action.Query;
    SuggestionRequest.FiltersJson = Action.FiltersJson;
    SuggestionRequest.bAllowConversion = Action.Context.bAllowConversion;
    SuggestionRequest.Limit = Action.Context.Limit;
    if (!ParseStoredFilters(
            Action.FiltersJson, SuggestionRequest.Filters, Error))
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The action's stored filter context is no longer valid."),
            TEXT("Repeat connection suggestions and use a current action."));
    }
    const TArray<FSemanticCandidate> Candidates = BuildSemanticCandidates(
        Blueprint, Graph, SourcePin, TargetPin, SuggestionRequest);
    if (SemanticResultDigest(Candidates) != Action.Context.ResultDigest)
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The native connection action result set has changed."),
            TEXT("Repeat connection suggestions and use a current action."));
    }
    const FSemanticCandidate* Candidate = FindExactSemanticCandidate(
        Candidates, Action);
    if (!Candidate)
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The selected native connection action is no longer available."),
            TEXT("Repeat connection suggestions and choose a current action."));
    }

    TArray<FPaletteBindingRecord> DynamicRecords;
    IBlueprintNodeBinder::FBindingSet DynamicBindings;
    if (!ResolveDynamicBindingObjects(
            Request.ActionId,
            Request.BindingIds,
            DynamicRecords,
            DynamicBindings,
            Error))
    {
        return SemanticFailure(Error);
    }
    TArray<FString> DynamicPaths;
    for (const FPaletteBindingRecord& Record : DynamicRecords)
    {
        DynamicPaths.Add(Record.ObjectPath);
    }
    DynamicPaths.Sort();
    if (DynamicPaths != Action.BindingPaths)
    {
        return SemanticFailure(
            TEXT("INVALID_INPUT"),
            TEXT("params.bindings"),
            TEXT("bindings must exactly match the selected action."),
            TEXT("Pass the complete bindings array returned with the action."));
    }

    FPaletteBindingRecord InputBinding;
    if (!ResolvePaletteTemplatePinBinding(
            Request.ActionId,
            Request.InputBindingId,
            InputBinding,
            Error))
    {
        Error.Path = TEXT("params.input_binding_id");
        return SemanticFailure(Error);
    }
    FPaletteBindingRecord OutputBinding;
    if (!ResolvePaletteTemplatePinBinding(
            Request.ActionId,
            Request.OutputBindingId,
            OutputBinding,
            Error))
    {
        Error.Path = TEXT("params.output_binding_id");
        return SemanticFailure(Error);
    }
    if (InputBinding.PinDirection != TEXT("input") ||
        OutputBinding.PinDirection != TEXT("output"))
    {
        return SemanticFailure(
            TEXT("INVALID_INPUT"),
            TEXT("params.input_binding_id"),
            TEXT("The selected binding IDs are not in input/output order."),
            TEXT("Use one binding pair exactly as returned by connection suggestions."));
    }
    const FSemanticPair* Pair = FindExactSemanticPairByBindingIds(
        Request.ActionId,
        *Candidate,
        Request.InputBindingId,
        Request.OutputBindingId);
    if (!Pair)
    {
        return SemanticFailure(
            TEXT("INVALID_INPUT"),
            TEXT("params.input_binding_id"),
            TEXT("The selected input and output bindings are not one returned pair."),
            TEXT("Use both binding IDs from the same current binding_pairs record."));
    }
    if (!SemanticPinMatchesBinding(Pair->Input, InputBinding) ||
        !SemanticPinMatchesBinding(Pair->Output, OutputBinding))
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.input_binding_id"),
            TEXT("The stored template-pin capability no longer matches its current pair."),
            TEXT("Repeat connection suggestions and use a refreshed binding pair."));
    }

    UEdGraphNode* TemplateNode = GetBoundTemplateNode(
        Candidate->Candidate, Graph, DynamicBindings);
    UEdGraphPin* TemplateInput = FindSpawnedPin(TemplateNode, InputBinding);
    UEdGraphPin* TemplateOutput = FindSpawnedPin(TemplateNode, OutputBinding);
    if (!TemplateInput || !TemplateOutput)
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.input_binding_id"),
            TEXT("The selected template pin pair has changed."),
            TEXT("Repeat connection suggestions and select a current pair."));
    }
    FSemanticResponse PreflightSource;
    FSemanticResponse PreflightTarget;
    if (!ClassifyInsertionResponse(
            Graph->GetSchema()->CanCreateConnection(SourcePin, TemplateInput),
            SourcePin,
            TemplateInput,
            SourcePin,
            TargetPin,
            Action.Context.bAllowConversion,
            PreflightSource) ||
        !ClassifyInsertionResponse(
            Graph->GetSchema()->CanCreateConnection(TemplateOutput, TargetPin),
            TemplateOutput,
            TargetPin,
            SourcePin,
            TargetPin,
            Action.Context.bAllowConversion,
            PreflightTarget) ||
        PreflightSource.Kind != Pair->SourceResponse.Kind ||
        PreflightTarget.Kind != Pair->TargetResponse.Kind)
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.input_binding_id"),
            TEXT("The selected template pair no longer has its advertised schema responses."),
            TEXT("Repeat connection suggestions against the current graph state."));
    }

    TSharedPtr<FJsonObject> BeforeSnapshot;
    if (!BuildBlueprintGraphSnapshot(
            Blueprint, {Request.GraphId}, BeforeSnapshot, Error))
    {
        return SemanticFailure(Error);
    }
    const TSet<FString> EdgesBefore = CollectStableGraphEdgeKeys(
        Blueprint, Graph);
    TSet<UEdGraphNode*> NodesBefore;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node)
        {
            NodesBefore.Add(Node);
        }
    }

    FMutationScope Scope(NSLOCTEXT(
        "MCPython", "InsertBlueprintActionNode",
        "Insert Blueprint palette node into connection"));
    if (!Scope.IsValid())
    {
        return SemanticFailure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Could not begin a Blueprint insertion transaction."),
            TEXT("Close any conflicting editor transaction and retry."));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    TSet<UEdGraphNode*> ModifiedNodes;
    auto ModifyPinOwner = [&](UEdGraphPin* Pin)
    {
        UEdGraphNode* Owner = Pin ? Pin->GetOwningNodeUnchecked() : nullptr;
        if (Owner && !ModifiedNodes.Contains(Owner))
        {
            ModifiedNodes.Add(Owner);
            Scope.Modify(Owner);
        }
    };
    ModifyPinOwner(SourcePin);
    ModifyPinOwner(TargetPin);
    for (UEdGraphPin* LinkedPin : SourcePin->LinkedTo)
    {
        ModifyPinOwner(LinkedPin);
    }
    for (UEdGraphPin* LinkedPin : TargetPin->LinkedTo)
    {
        ModifyPinOwner(LinkedPin);
    }

    UEdGraphNode* NewNode = Candidate->Candidate.Spawner->Invoke(
        Graph,
        DynamicBindings,
        FVector2D(Request.PositionX, Request.PositionY));
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticInsertionFailurePoint ==
            ESemanticInsertionFailurePoint::OutOfGraphResult &&
        NewNode)
    {
        Graph->Nodes.Remove(NewNode);
    }
#endif
    auto RollbackFailure = [&](const FString& Code,
                               const FString& Path,
                               const FString& Message,
                               const FString& Hint)
    {
        TArray<FString> AffectedIds = {
            Request.GraphId, Request.SourcePinId, Request.TargetPinId};
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node || (Node != NewNode && NodesBefore.Contains(Node)))
            {
                continue;
            }
            AffectedIds.Add(DescribeNodeTarget(Blueprint, Node).Id);
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && !Pin->bHidden)
                {
                    AffectedIds.Add(DescribePinTarget(Blueprint, Pin).Id);
                }
            }
        }
        return RollbackSemanticFailure(
            Scope,
            Blueprint,
            Request.GraphId,
            BeforeSnapshot,
            Code,
            Path,
            Message,
            Hint,
            AffectedIds,
            TEXT("Blueprint insertion rollback left a graph delta."),
#if WITH_DEV_AUTOMATION_TESTS
            GSemanticInsertionFailurePoint ==
                ESemanticInsertionFailurePoint::AfterRollbackResidual
#else
            false
#endif
        );
    };
    int32 AddedAfterInvoke = 0;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        AddedAfterInvoke += Node && !NodesBefore.Contains(Node) ? 1 : 0;
    }
    if (!NewNode || NodesBefore.Contains(NewNode) ||
        NewNode->GetGraph() != Graph || !Graph->Nodes.Contains(NewNode) ||
        AddedAfterInvoke != 1)
    {
        return RollbackFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The native action did not create exactly one requested node in graph_id."),
            TEXT("Repeat connection suggestions or choose a non-singleton action."));
    }
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticInsertionFailurePoint ==
            ESemanticInsertionFailurePoint::AfterInvoke ||
        GSemanticInsertionFailurePoint ==
            ESemanticInsertionFailurePoint::AfterRollbackResidual)
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("test.failure_point"),
            TEXT("Injected insertion failure after native invocation."),
            TEXT("Disable the automation failure point before retrying."));
    }
#endif
    Scope.Modify(NewNode);
    if (NewNode->GetClass()->GetPathName() !=
        Candidate->Candidate.NodeClassPath)
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("params.action_id"),
            TEXT("The native spawner returned an unexpected node class."),
            TEXT("Repeat connection suggestions and report the action signature."));
    }
    UEdGraphPin* ActualInput = FindSpawnedPin(NewNode, InputBinding);
    UEdGraphPin* ActualOutput = FindSpawnedPin(NewNode, OutputBinding);
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticInsertionFailurePoint ==
        ESemanticInsertionFailurePoint::MissingActualInput)
    {
        ActualInput = nullptr;
    }
    if (GSemanticInsertionFailurePoint ==
        ESemanticInsertionFailurePoint::MissingActualOutput)
    {
        ActualOutput = nullptr;
    }
#endif
    if (!ActualInput || !ActualOutput)
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("params.input_binding_id"),
            TEXT("The spawned node no longer has the selected pin pair."),
            TEXT("Repeat connection suggestions and choose a current pair."));
    }
    FSemanticResponse ActualSource;
    FSemanticResponse ActualTarget;
    if (!ClassifyInsertionResponse(
            Graph->GetSchema()->CanCreateConnection(SourcePin, ActualInput),
            SourcePin,
            ActualInput,
            SourcePin,
            TargetPin,
            Action.Context.bAllowConversion,
            ActualSource) ||
        !ClassifyInsertionResponse(
            Graph->GetSchema()->CanCreateConnection(ActualOutput, TargetPin),
            ActualOutput,
            TargetPin,
            SourcePin,
            TargetPin,
            Action.Context.bAllowConversion,
            ActualTarget) ||
        ActualSource.Kind != Pair->SourceResponse.Kind ||
        ActualTarget.Kind != Pair->TargetResponse.Kind)
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("params.input_binding_id"),
            TEXT("The spawned pin pair does not match its preflight schema responses."),
            TEXT("Repeat connection suggestions against the current graph state."));
    }

#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticInsertionFailurePoint ==
        ESemanticInsertionFailurePoint::BeforeBreak)
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("test.failure_point"),
            TEXT("Injected insertion failure before removing the old edge."),
            TEXT("Disable the automation failure point before retrying."));
    }
#endif
    Graph->GetSchema()->BreakSinglePinLink(SourcePin, TargetPin);
    if (SourcePin->LinkedTo.Contains(TargetPin) ||
        TargetPin->LinkedTo.Contains(SourcePin))
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("params.target_pin_id"),
            TEXT("K2 schema failed to remove the named old edge."),
            TEXT("Inspect the graph before retrying."));
    }
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticInsertionFailurePoint ==
        ESemanticInsertionFailurePoint::AfterBreak)
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("test.failure_point"),
            TEXT("Injected insertion failure after removing the old edge."),
            TEXT("Disable the automation failure point before retrying."));
    }
#endif
    bool bFirstConnectionCreated = false;
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticInsertionFailurePoint !=
        ESemanticInsertionFailurePoint::BeforeFirstConnect)
#endif
    {
        bFirstConnectionCreated = Graph->GetSchema()->TryCreateConnection(
            SourcePin, ActualInput);
    }
    if (!bFirstConnectionCreated)
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("params.input_binding_id"),
            TEXT("K2 schema failed to create the source-to-input connection."),
            TEXT("Repeat connection suggestions against the current graph state."));
    }
    bool bSecondConnectionCreated = false;
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticInsertionFailurePoint !=
        ESemanticInsertionFailurePoint::BeforeSecondConnect)
#endif
    {
        bSecondConnectionCreated = Graph->GetSchema()->TryCreateConnection(
            ActualOutput, TargetPin);
    }
    if (!bSecondConnectionCreated)
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("params.output_binding_id"),
            TEXT("K2 schema failed to create the output-to-target connection."),
            TEXT("Repeat connection suggestions against the current graph state."));
    }

    TArray<UEdGraphNode*> AuxiliaryNodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && Node != NewNode && !NodesBefore.Contains(Node))
        {
            AuxiliaryNodes.Add(Node);
        }
    }
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticInsertionFailurePoint ==
        ESemanticInsertionFailurePoint::AfterConnectionsBreakTopology)
    {
        SourcePin->BreakLinkTo(ActualInput);
    }
#endif
    AuxiliaryNodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
    {
        return Left.NodeGuid < Right.NodeGuid;
    });
    if (!VerifyInsertionTopology(
            Blueprint,
            Graph,
            SourcePin,
            TargetPin,
            ActualInput,
            ActualOutput,
            ActualSource,
            ActualTarget,
            AuxiliaryNodes,
            EdgesBefore))
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("result.connections"),
            TEXT("The insertion changed unrelated topology or did not create both replacement paths."),
            TEXT("Inspect and resnapshot the graph before retrying."));
    }
    bool bStableResult = ValidateStableConnectedSpawnResult(
            Blueprint, SourcePin, NewNode, AuxiliaryNodes) &&
        DescribePinTarget(Blueprint, TargetPin).Id.StartsWith(TEXT("pin:"));
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticInsertionFailurePoint ==
        ESemanticInsertionFailurePoint::ZeroVisiblePinGuid)
    {
        bStableResult = false;
    }
#endif
    if (!bStableResult)
    {
        return RollbackFailure(
            TEXT("OPERATION_FAILED"),
            TEXT("result"),
            TEXT("Inserted semantic targets or topology edges did not receive stable IDs."),
            TEXT("Retry after the graph has finished loading."));
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    return InsertionSuccess(
        Blueprint,
        Graph,
        Request,
        SourcePin,
        TargetPin,
        NewNode,
        ActualInput,
        ActualOutput,
        ActualSource,
        ActualTarget,
        AuxiliaryNodes);
#endif
}

FString UMCPythonHelper::PreviewBlueprintActionReplacement(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
#if UE_VERSION_NEWER_THAN(5, 7, 99) || UE_VERSION_OLDER_THAN(5, 7, 0)
    return UnsupportedSemanticVersion(TEXT("Blueprint action replacement preview"));
#else
    if (!Blueprint)
    {
        return MissingBlueprint();
    }
    FReplacementPreviewRequest Request;
    FError Error;
    if (!ParseReplacementPreviewRequest(RequestJson, Request, Error))
    {
        return SemanticFailure(Error);
    }
    UEdGraph* Graph = nullptr;
    if (!ResolveStableGraph(Blueprint, Request.GraphId, Graph, Error, true))
    {
        return SemanticFailure(Error);
    }
    FGuid NodeGuid;
    ParseTargetId(Request.NodeId, ETargetKind::Node, NodeGuid);
    UEdGraphNode* TargetNode = nullptr;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && Node->NodeGuid == NodeGuid)
        {
            if (TargetNode)
            {
                return SemanticFailure(
                    TEXT("PRECONDITION_FAILED"), TEXT("params.node_id"),
                    TEXT("The stable node ID is no longer unique in the graph."),
                    TEXT("Inspect the graph again before retrying."));
            }
            TargetNode = Node;
        }
    }
    if (!TargetNode)
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"), TEXT("params.node_id"),
            TEXT("The stable node no longer exists in graph_id."),
            TEXT("Inspect the graph again and use a current node ID."));
    }
    for (UEdGraphPin* Pin : TargetNode->Pins)
    {
        if (Pin && !Pin->bHidden &&
            !DescribePinTarget(Blueprint, Pin).Id.StartsWith(TEXT("pin:")))
        {
            return SemanticFailure(
                TEXT("PRECONDITION_FAILED"), TEXT("params.node_id"),
                TEXT("Every considered target-node pin requires a persisted stable ID."),
                TEXT("Wait for the graph to finish loading and inspect it again."));
        }
    }

    FPaletteContextExpectation Expected;
    Expected.AssetPath = Blueprint->GetPathName();
    Expected.GraphId = Request.GraphId;
    Expected.GraphSchemaPath = Graph->GetSchema()->GetClass()->GetPathName();
    Expected.Kind = EPaletteContextKind::Graph;
    FPaletteActionRecord Action;
    if (!ResolvePaletteActionToken(Request.ActionId, Expected, Action, Error))
    {
        return SemanticFailure(Error);
    }
    FPaletteFilters Filters;
    if (!ParseStoredFilters(Action.FiltersJson, Filters, Error))
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"), TEXT("params.action_id"),
            TEXT("The action's stored graph search context is no longer valid."),
            TEXT("Repeat palette search and use a current graph action."));
    }
    TArray<FPaletteCandidate> Candidates;
    BuildCandidates(
        Blueprint, Graph, TConstArrayView<UEdGraphPin*>(),
        Action.Query, Filters, Candidates);
    const FString ActionResultDigest = ResultDigest(Candidates);
    if (ActionResultDigest != Action.Context.ResultDigest)
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"), TEXT("params.action_id"),
            TEXT("The native graph action result set has changed."),
            TEXT("Repeat palette search and use a current action."));
    }
    const FPaletteCandidate* Candidate = FindExactCandidate(Candidates, Action);
    if (!Candidate)
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"), TEXT("params.action_id"),
            TEXT("The selected native graph action is no longer available."),
            TEXT("Repeat palette search and choose a current action."));
    }
    TArray<FPaletteBindingRecord> DynamicRecords;
    IBlueprintNodeBinder::FBindingSet DynamicBindings;
    if (!ResolveDynamicBindingObjects(
            Request.ActionId, Request.BindingIds, DynamicRecords,
            DynamicBindings, Error))
    {
        return SemanticFailure(Error);
    }
    TArray<FString> DynamicPaths;
    for (const FPaletteBindingRecord& Record : DynamicRecords)
    {
        DynamicPaths.Add(Record.ObjectPath);
    }
    DynamicPaths.Sort();
    if (DynamicPaths != Action.BindingPaths)
    {
        return SemanticFailure(
            TEXT("INVALID_INPUT"), TEXT("params.bindings"),
            TEXT("bindings must exactly match the selected graph action."),
            TEXT("Pass the complete bindings array returned with the action."));
    }
    Candidate->Spawner->PrimeDefaultUiSpec(Graph);
    UEdGraphNode* TemplateNode = GetBoundTemplateNode(
        *Candidate, Graph, DynamicBindings);
    if (!TemplateNode)
    {
        return SemanticFailure(
            TEXT("PRECONDITION_FAILED"), TEXT("params.action_id"),
            TEXT("The selected action no longer exposes a bound template node."),
            TEXT("Repeat palette search and choose a current action."));
    }
    if (TemplateNode->Pins.IsEmpty())
    {
        TemplateNode->AllocateDefaultPins();
    }
    TArray<FSemanticTemplatePin> TemplateInputs;
    TArray<FSemanticTemplatePin> TemplateOutputs;
    CollectTemplatePins(TemplateNode, TemplateInputs, TemplateOutputs);
    TArray<FSemanticTemplatePin> TemplatePins = TemplateInputs;
    TemplatePins.Append(TemplateOutputs);
    struct FRegisteredTemplatePin
    {
        FSemanticTemplatePin Descriptor;
        FString BindingId;
    };
    TArray<FRegisteredTemplatePin> RegisteredPins;
    for (const FSemanticTemplatePin& Pin : TemplatePins)
    {
        FPaletteBindingRecord Record;
        Record.Kind = EPaletteBindingKind::TemplatePin;
        Record.ActionId = Request.ActionId;
        Record.PinName = Pin.Name;
        Record.PinDirection = Pin.Direction;
        Record.PinTypeJson = Pin.TypeJson;
        Record.PinOccurrence = Pin.Occurrence;
        const FString BindingId = RegisterPaletteTemplatePinBinding(Record);
        if (BindingId.IsEmpty())
        {
            return SemanticFailure(
                TEXT("INTERNAL_ERROR"), TEXT("result.mappings"),
                TEXT("Failed to register one replacement template pin."),
                TEXT("Repeat palette search in the current editor session."));
        }
        RegisteredPins.Add({Pin, BindingId});
    }

    TArray<UEdGraphPin*> OldPins;
    for (UEdGraphPin* Pin : TargetNode->Pins)
    {
        if (Pin && !Pin->bHidden)
        {
            OldPins.Add(Pin);
        }
    }
    OldPins.Sort([&](const UEdGraphPin& Left, const UEdGraphPin& Right)
    {
        return DescribePinTarget(Blueprint, &Left).Id <
            DescribePinTarget(Blueprint, &Right).Id;
    });

    auto NormalizedPinName = [](const FString& Value)
    {
        FString Result = Value;
        Result.TrimStartAndEndInline();
        Result.ToLowerInline();
        Result.ReplaceInline(TEXT(" "), TEXT(""));
        Result.ReplaceInline(TEXT("_"), TEXT(""));
        return Result;
    };
    auto IsExecPin = [](const UEdGraphPin* Pin)
    {
        return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
    };
    auto HasWritableDefault = [](const UEdGraphPin* Pin)
    {
        return Pin && Pin->Direction == EGPD_Input && Pin->LinkedTo.IsEmpty() &&
            !Pin->bDefaultValueIsIgnored &&
            (!Pin->DefaultValue.IsEmpty() || Pin->DefaultObject ||
                !Pin->DefaultTextValue.IsEmpty());
    };
    auto FindOldPin = [&](const FString& PinId) -> UEdGraphPin*
    {
        for (UEdGraphPin* Pin : OldPins)
        {
            if (DescribePinTarget(Blueprint, Pin).Id == PinId)
            {
                return Pin;
            }
        }
        return nullptr;
    };
    auto FindRegisteredPin = [&](const FString& BindingId)
        -> const FRegisteredTemplatePin*
    {
        return RegisteredPins.FindByPredicate(
            [&](const FRegisteredTemplatePin& Item)
            {
                return Item.BindingId == BindingId;
            });
    };
    auto SortedExternalLinks = [&](const UEdGraphPin* Pin)
    {
        TArray<UEdGraphPin*> Links = Pin ? Pin->LinkedTo : TArray<UEdGraphPin*>();
        Links.Sort([&](const UEdGraphPin& Left, const UEdGraphPin& Right)
        {
            return DescribePinTarget(Blueprint, &Left).Id <
                DescribePinTarget(Blueprint, &Right).Id;
        });
        return Links;
    };
    auto ValidatePair = [&](UEdGraphPin* OldPin,
                            const FRegisteredTemplatePin& NewPin,
                            TArray<FSemanticResponse>& OutResponses,
                            FString& OutReason)
    {
        OutResponses.Reset();
        if (!OldPin || !NewPin.Descriptor.Pin ||
            (OldPin->Direction == EGPD_Input ? TEXT("input") : TEXT("output")) !=
                NewPin.Descriptor.Direction ||
            IsExecPin(OldPin) != IsExecPin(NewPin.Descriptor.Pin))
        {
            OutReason = TEXT("direction_or_exec_category_mismatch");
            return false;
        }
        const FString OldTypeJson = CanonicalJsonString(
            MakeShared<FJsonValueObject>(SerializeTypeSpec(OldPin->PinType)));
        const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(
            Graph->GetSchema());
        const bool bDirectTypeCompatible = OldPin->Direction == EGPD_Input
            ? K2Schema && K2Schema->ArePinTypesCompatible(
                OldPin->PinType,
                NewPin.Descriptor.Pin->PinType,
                Blueprint->GeneratedClass)
            : K2Schema && K2Schema->ArePinTypesCompatible(
                NewPin.Descriptor.Pin->PinType,
                OldPin->PinType,
                Blueprint->GeneratedClass);
        if (OldTypeJson != NewPin.Descriptor.TypeJson &&
            OldPin->LinkedTo.IsEmpty() && !bDirectTypeCompatible)
        {
            OutReason = TEXT("unlinked_pin_type_is_not_exact");
            return false;
        }
        for (UEdGraphPin* LinkedPin : SortedExternalLinks(OldPin))
        {
            if (!LinkedPin ||
                !DescribePinTarget(Blueprint, LinkedPin).Id.StartsWith(TEXT("pin:")))
            {
                OutReason = TEXT("external_link_is_not_stable");
                return false;
            }
            FSemanticResponse Response;
            if (!ClassifySemanticResponse(
                    Graph->GetSchema()->CanCreateConnection(
                        NewPin.Descriptor.Pin, LinkedPin),
                    Request.bAllowConversion,
                    Response))
            {
                OutReason = TEXT("external_link_is_incompatible");
                return false;
            }
            OutResponses.Add(Response);
        }
        if (HasWritableDefault(OldPin))
        {
            FString ValidationMessage;
            if (!K2Schema || !K2Schema->DefaultValueSimpleValidation(
                    OldPin->PinType,
                    OldPin->PinName,
                    OldPin->DefaultValue,
                    OldPin->DefaultObject,
                    OldPin->DefaultTextValue,
                    &ValidationMessage))
            {
                OutReason = TEXT("source_default_is_invalid");
                return false;
            }
            const TSharedPtr<FJsonValue> Value = SerializeDefaultValue(
                OldPin->PinType, OldPin->DefaultValue, OldPin->DefaultObject,
                OldPin->DefaultTextValue);
            FNormalizedDefault Normalized;
            FError DefaultError;
            if (!Value.IsValid() || !NormalizeDefaultValue(
                    NewPin.Descriptor.Pin->PinType,
                    Value,
                    Blueprint,
                    Normalized,
                    DefaultError,
                    TEXT("params.pin_mapping")))
            {
                OutReason = TEXT("default_is_incompatible");
                return false;
            }
        }
        OutReason = OldTypeJson == NewPin.Descriptor.TypeJson
            ? TEXT("exact_type")
            : TEXT("native_compatible_type");
        return true;
    };

    struct FSelectedMapping
    {
        UEdGraphPin* OldPin = nullptr;
        const FRegisteredTemplatePin* NewPin = nullptr;
        FString Origin;
        FString Reason;
        TArray<FSemanticResponse> Responses;
    };
    TArray<FSelectedMapping> SelectedMappings;
    TSet<FString> ConsumedOldPins;
    TSet<FString> ConsumedNewBindings;
    for (const FReplacementPinMappingInput& Explicit : Request.PinMappings)
    {
        UEdGraphPin* OldPin = FindOldPin(Explicit.OldPinId);
        const FRegisteredTemplatePin* NewPin =
            FindRegisteredPin(Explicit.NewBindingId);
        TArray<FSemanticResponse> Responses;
        FString Reason;
        if (!OldPin || !NewPin ||
            !ValidatePair(OldPin, *NewPin, Responses, Reason))
        {
            return SemanticFailure(
                TEXT("INVALID_INPUT"), TEXT("params.pin_mapping"),
                TEXT("One explicit replacement mapping is not valid for the current node and action."),
                TEXT("Use current old pin IDs and template bindings with compatible directions, links, and defaults."));
        }
        SelectedMappings.Add(
            {OldPin, NewPin, TEXT("explicit"), TEXT("explicit_mapping"), Responses});
        ConsumedOldPins.Add(Explicit.OldPinId);
        ConsumedNewBindings.Add(Explicit.NewBindingId);
    }

    for (UEdGraphPin* OldPin : OldPins)
    {
        const FString OldPinId = DescribePinTarget(Blueprint, OldPin).Id;
        if (ConsumedOldPins.Contains(OldPinId))
        {
            continue;
        }
        const FString OldTypeJson = CanonicalJsonString(
            MakeShared<FJsonValueObject>(SerializeTypeSpec(OldPin->PinType)));
        struct FSemanticCost
        {
            int32 Type = MAX_int32;
            int32 Name = MAX_int32;
            int32 Container = MAX_int32;
            int32 Qualifier = MAX_int32;
            bool operator==(const FSemanticCost& Other) const
            {
                return Type == Other.Type && Name == Other.Name &&
                    Container == Other.Container && Qualifier == Other.Qualifier;
            }
            bool operator<(const FSemanticCost& Other) const
            {
                if (Type != Other.Type) return Type < Other.Type;
                if (Name != Other.Name) return Name < Other.Name;
                if (Container != Other.Container)
                    return Container < Other.Container;
                return Qualifier < Other.Qualifier;
            }
        };
        TArray<const FRegisteredTemplatePin*> EligiblePins;
        TArray<FSemanticCost> EligibleCosts;
        TArray<int32> FlattenedCosts;
        TMap<FString, TArray<FSemanticResponse>> ResponsesByBinding;
        for (const FRegisteredTemplatePin& NewPin : RegisteredPins)
        {
            if (ConsumedNewBindings.Contains(NewPin.BindingId))
            {
                continue;
            }
            TArray<FSemanticResponse> Responses;
            FString Reason;
            if (!ValidatePair(OldPin, NewPin, Responses, Reason))
            {
                continue;
            }
            int32 TypeCost = NewPin.Descriptor.TypeJson == OldTypeJson ? 0 : 1;
            for (const FSemanticResponse& Response : Responses)
            {
                if (Response.bRequiresConversion)
                {
                    TypeCost = 2;
                    break;
                }
            }
            if (TypeCost == 2 && !Request.bAllowConversion)
            {
                continue;
            }
            FSemanticCost Cost;
            Cost.Type = TypeCost;
            Cost.Name =
                NormalizedPinName(OldPin->PinName.ToString()) ==
                    NormalizedPinName(NewPin.Descriptor.Name)
                ? 0
                : 1;
            Cost.Container = OldPin->PinType.ContainerType ==
                    NewPin.Descriptor.Pin->PinType.ContainerType
                ? 0
                : 1;
            Cost.Qualifier =
                OldPin->PinType.bIsReference ==
                        NewPin.Descriptor.Pin->PinType.bIsReference &&
                    OldPin->PinType.bIsConst ==
                        NewPin.Descriptor.Pin->PinType.bIsConst
                ? 0
                : 1;
            EligiblePins.Add(&NewPin);
            EligibleCosts.Add(Cost);
            FlattenedCosts.Append(
                {Cost.Type, Cost.Name, Cost.Container, Cost.Qualifier});
            ResponsesByBinding.Add(NewPin.BindingId, MoveTemp(Responses));
        }
        const int32 BestIndex = SelectUniqueBestReplacementCandidate(
            FlattenedCosts);
        if (BestIndex != INDEX_NONE)
        {
            const FRegisteredTemplatePin* NewPin = EligiblePins[BestIndex];
            const FSemanticCost& BestCost = EligibleCosts[BestIndex];
            SelectedMappings.Add({
                OldPin,
                NewPin,
                TEXT("inferred"),
                BestCost.Type == 0 && BestCost.Name == 0
                    ? TEXT("unique_exact_type_and_normalized_name")
                    : BestCost.Type == 0
                        ? TEXT("unique_exact_type")
                        : BestCost.Type == 1
                            ? TEXT("unique_direct_compatible_type")
                            : TEXT("unique_conversion_compatible_type"),
                ResponsesByBinding.FindChecked(NewPin->BindingId)});
            ConsumedOldPins.Add(OldPinId);
            ConsumedNewBindings.Add(NewPin->BindingId);
        }
    }
    SelectedMappings.Sort([&](const FSelectedMapping& Left,
                              const FSelectedMapping& Right)
    {
        return DescribePinTarget(Blueprint, Left.OldPin).Id <
            DescribePinTarget(Blueprint, Right.OldPin).Id;
    });

    TArray<TSharedPtr<FJsonValue>> MappingValues;
    TArray<TSharedPtr<FJsonValue>> RetainedConnections;
    TArray<TSharedPtr<FJsonValue>> RetainedDefaults;
    TArray<TSharedPtr<FJsonValue>> LostConnections;
    TArray<TSharedPtr<FJsonValue>> LostDefaults;
    FReplacementPlanRecord Plan;
    Plan.AssetPath = Blueprint->GetPathName();
    Plan.GraphId = Request.GraphId;
    Plan.GraphSchemaPath = Graph->GetSchema()->GetClass()->GetPathName();
    Plan.NodeId = Request.NodeId;
    Plan.ActionId = Request.ActionId;
    Plan.ActionResultDigest = ActionResultDigest;
    Plan.DynamicBindingIds = Request.BindingIds;
    Plan.PositionX = TargetNode->NodePosX;
    Plan.PositionY = TargetNode->NodePosY;
    Plan.Comment = TargetNode->NodeComment;
    Plan.bCommentBubbleVisible = TargetNode->bCommentBubbleVisible;
    Plan.EnabledState = static_cast<uint8>(TargetNode->GetDesiredEnabledState());
    Plan.bAllowConversion = Request.bAllowConversion;
    Plan.bAllowLoss = Request.bAllowLoss;

    TSet<FString> MappedOldIds;
    for (const FSelectedMapping& Mapping : SelectedMappings)
    {
        const FString OldPinId = DescribePinTarget(Blueprint, Mapping.OldPin).Id;
        MappedOldIds.Add(OldPinId);
        FReplacementMappingRecord MappingRecord;
        MappingRecord.OldPinId = OldPinId;
        MappingRecord.NewBindingId = Mapping.NewPin->BindingId;
        MappingRecord.Origin = Mapping.Origin;
        MappingRecord.Reason = Mapping.Reason;
        Plan.Mappings.Add(MappingRecord);
        const TSharedRef<FJsonObject> MappingJson = MakeShared<FJsonObject>();
        MappingJson->SetStringField(TEXT("old_pin_id"), OldPinId);
        MappingJson->SetStringField(
            TEXT("new_binding_id"), Mapping.NewPin->BindingId);
        MappingJson->SetStringField(
            TEXT("new_pin_name"), Mapping.NewPin->Descriptor.Name);
        MappingJson->SetStringField(TEXT("origin"), Mapping.Origin);
        MappingJson->SetStringField(TEXT("reason"), Mapping.Reason);
        MappingValues.Add(MakeShared<FJsonValueObject>(MappingJson));

        const TArray<UEdGraphPin*> SortedLinks =
            SortedExternalLinks(Mapping.OldPin);
        for (int32 LinkIndex = 0;
             LinkIndex < SortedLinks.Num(); ++LinkIndex)
        {
            UEdGraphPin* LinkedPin = SortedLinks[LinkIndex];
            const FString LinkedPinId = DescribePinTarget(Blueprint, LinkedPin).Id;
            const FSemanticResponse& Response = Mapping.Responses[LinkIndex];
            FReplacementConnectionRecord Record;
            Record.OldPinId = OldPinId;
            Record.NewBindingId = Mapping.NewPin->BindingId;
            Record.LinkedPinId = LinkedPinId;
            Record.ResponseKind = Response.Kind;
            Record.ResponseMessage = Response.Message;
            Plan.Connections.Add(Record);
            const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("old_pin_id"), OldPinId);
            Json->SetStringField(
                TEXT("new_binding_id"), Mapping.NewPin->BindingId);
            Json->SetStringField(TEXT("linked_pin_id"), LinkedPinId);
            Json->SetObjectField(
                TEXT("response"), SerializeSemanticResponse(Response));
            RetainedConnections.Add(MakeShared<FJsonValueObject>(Json));
        }
        if (HasWritableDefault(Mapping.OldPin))
        {
            const TSharedPtr<FJsonValue> Value = SerializeDefaultValue(
                Mapping.OldPin->PinType,
                Mapping.OldPin->DefaultValue,
                Mapping.OldPin->DefaultObject,
                Mapping.OldPin->DefaultTextValue);
            const FString CanonicalValue = CanonicalJsonString(Value);
            Plan.Defaults.Add(
                {OldPinId, Mapping.NewPin->BindingId, CanonicalValue});
            const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("old_pin_id"), OldPinId);
            Json->SetStringField(
                TEXT("new_binding_id"), Mapping.NewPin->BindingId);
            Json->SetField(TEXT("value"), Value);
            RetainedDefaults.Add(MakeShared<FJsonValueObject>(Json));
        }
    }
    for (UEdGraphPin* OldPin : OldPins)
    {
        const FString OldPinId = DescribePinTarget(Blueprint, OldPin).Id;
        if (MappedOldIds.Contains(OldPinId))
        {
            continue;
        }
        for (UEdGraphPin* LinkedPin : SortedExternalLinks(OldPin))
        {
            const FString LinkedPinId = DescribePinTarget(Blueprint, LinkedPin).Id;
            const FString Reason = TEXT("no_unique_compatible_mapping");
            Plan.LostConnections.Add({OldPinId, LinkedPinId, Reason});
            const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("old_pin_id"), OldPinId);
            Json->SetStringField(TEXT("linked_pin_id"), LinkedPinId);
            Json->SetStringField(TEXT("reason"), Reason);
            LostConnections.Add(MakeShared<FJsonValueObject>(Json));
        }
        if (HasWritableDefault(OldPin))
        {
            const TSharedPtr<FJsonValue> Value = SerializeDefaultValue(
                OldPin->PinType, OldPin->DefaultValue,
                OldPin->DefaultObject, OldPin->DefaultTextValue);
            const FString Reason = TEXT("no_unique_compatible_mapping");
            Plan.LostDefaults.Add(
                {OldPinId, CanonicalJsonString(Value), Reason});
            const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("old_pin_id"), OldPinId);
            Json->SetField(TEXT("value"), Value);
            Json->SetStringField(TEXT("reason"), Reason);
            LostDefaults.Add(MakeShared<FJsonValueObject>(Json));
        }
    }
    Plan.LossCount = Plan.LostConnections.Num() + Plan.LostDefaults.Num();

    const TSharedRef<FJsonObject> FocusedSnapshot = MakeShared<FJsonObject>();
    FocusedSnapshot->SetStringField(TEXT("node_id"), Request.NodeId);
    FocusedSnapshot->SetStringField(
        TEXT("class_path"), TargetNode->GetClass()->GetPathName());
    const TSharedRef<FJsonObject> SnapshotPosition = MakeShared<FJsonObject>();
    SnapshotPosition->SetNumberField(TEXT("x"), TargetNode->NodePosX);
    SnapshotPosition->SetNumberField(TEXT("y"), TargetNode->NodePosY);
    FocusedSnapshot->SetObjectField(TEXT("position"), SnapshotPosition);
    FocusedSnapshot->SetStringField(TEXT("comment"), TargetNode->NodeComment);
    FocusedSnapshot->SetBoolField(
        TEXT("comment_bubble_visible"), TargetNode->bCommentBubbleVisible);
    auto EnabledStateString = [](const ENodeEnabledState State)
    {
        switch (State)
        {
        case ENodeEnabledState::Disabled: return FString(TEXT("disabled"));
        case ENodeEnabledState::DevelopmentOnly:
            return FString(TEXT("development_only"));
        default: return FString(TEXT("enabled"));
        }
    };
    FocusedSnapshot->SetStringField(
        TEXT("enabled_state"),
        EnabledStateString(TargetNode->GetDesiredEnabledState()));
    TArray<TSharedPtr<FJsonValue>> SnapshotPins;
    for (UEdGraphPin* Pin : OldPins)
    {
        const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(
            TEXT("pin_id"), DescribePinTarget(Blueprint, Pin).Id);
        Json->SetStringField(TEXT("name"), Pin->PinName.ToString());
        Json->SetStringField(
            TEXT("direction"),
            Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        Json->SetObjectField(TEXT("type"), SerializeTypeSpec(Pin->PinType));
        TSharedPtr<FJsonValue> Default = SerializeDefaultValue(
            Pin->PinType, Pin->DefaultValue, Pin->DefaultObject,
            Pin->DefaultTextValue);
        Json->SetField(
            TEXT("default"), Default.IsValid() ? Default : MakeShared<FJsonValueNull>());
        TArray<FString> LinkedIds;
        for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            LinkedIds.Add(DescribePinTarget(Blueprint, LinkedPin).Id);
        }
        LinkedIds.Sort();
        TArray<TSharedPtr<FJsonValue>> LinkedValues;
        for (const FString& Id : LinkedIds)
        {
            LinkedValues.Add(MakeShared<FJsonValueString>(Id));
        }
        Json->SetArrayField(TEXT("linked_pin_ids"), MoveTemp(LinkedValues));
        SnapshotPins.Add(MakeShared<FJsonValueObject>(Json));
    }
    FocusedSnapshot->SetArrayField(TEXT("pins"), MoveTemp(SnapshotPins));
    Plan.NodeSnapshotDigest = TEXT("sha1:") + Sha1Hex(CanonicalJsonString(
        MakeShared<FJsonValueObject>(FocusedSnapshot)));
    const FString PlanId = RegisterReplacementPlan(Plan);
    if (PlanId.IsEmpty())
    {
        return SemanticFailure(
            TEXT("INTERNAL_ERROR"), TEXT("result.replacement_plan_id"),
            TEXT("Failed to register the deterministic replacement plan."),
            TEXT("Inspect stable IDs and repeat preview."));
    }

    const TSharedRef<FJsonObject> SelectedAction = MakeShared<FJsonObject>();
    SelectedAction->SetStringField(TEXT("action_id"), Request.ActionId);
    SelectedAction->SetStringField(TEXT("title"), Candidate->Title);
    SelectedAction->SetStringField(
        TEXT("node_class_path"), Candidate->NodeClassPath);
    SelectedAction->SetStringField(TEXT("owner_path"), Candidate->OwnerPath);
    SelectedAction->SetStringField(TEXT("member_path"), Candidate->MemberPath);
    const TSharedRef<FJsonObject> TargetSummary = MakeShared<FJsonObject>();
    TargetSummary->SetStringField(TEXT("node_id"), Request.NodeId);
    TargetSummary->SetStringField(
        TEXT("class_path"), TargetNode->GetClass()->GetPathName());
    TargetSummary->SetStringField(
        TEXT("title"),
        TargetNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
    TargetSummary->SetObjectField(TEXT("position"), SnapshotPosition);
    TargetSummary->SetStringField(TEXT("comment"), TargetNode->NodeComment);
    TargetSummary->SetBoolField(
        TEXT("comment_bubble_visible"), TargetNode->bCommentBubbleVisible);
    TargetSummary->SetStringField(
        TEXT("enabled_state"),
        EnabledStateString(TargetNode->GetDesiredEnabledState()));

    TArray<TSharedPtr<FJsonValue>> Warnings;
    if (Plan.LossCount > 0)
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Replacement would lose %d connection/default item(s)."),
            Plan.LossCount)));
    }
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("replacement_plan_id"), PlanId);
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("graph_id"), Request.GraphId);
    Data->SetStringField(
        TEXT("node_snapshot_digest"), Plan.NodeSnapshotDigest);
    Data->SetStringField(
        TEXT("action_result_digest"), ActionResultDigest);
    Data->SetObjectField(TEXT("selected_action"), SelectedAction);
    Data->SetObjectField(TEXT("target_node"), TargetSummary);
    Data->SetArrayField(TEXT("mappings"), MoveTemp(MappingValues));
    Data->SetArrayField(
        TEXT("retained_connections"), MoveTemp(RetainedConnections));
    Data->SetArrayField(TEXT("retained_defaults"), MoveTemp(RetainedDefaults));
    Data->SetArrayField(
        TEXT("unmapped_connections"), MoveTemp(LostConnections));
    Data->SetArrayField(TEXT("unmapped_defaults"), MoveTemp(LostDefaults));
    Data->SetArrayField(
        TEXT("unsupported_metadata"),
        UnsupportedReplacementMetadata(TargetNode));
    Data->SetNumberField(TEXT("loss_count"), Plan.LossCount);
    Data->SetArrayField(TEXT("warnings"), MoveTemp(Warnings));
    Data->SetBoolField(
        TEXT("applicable"), Request.bAllowLoss || Plan.LossCount == 0);
    Data->SetBoolField(TEXT("allow_conversion"), Request.bAllowConversion);
    Data->SetBoolField(TEXT("allow_loss"), Request.bAllowLoss);
    return SerializeResult(MakeSuccess(
        TEXT("Built a read-only snapshot-bound Blueprint replacement preview."),
        Data));
#endif
}
