// Copyright (c) 2025 GenOrca. All Rights Reserved.

#include "MCPythonHelper.h"

#include "MCPythonBlueprint2Internal.h"
#include "MCPythonBlueprintPaletteInternal.h"

#include "BlueprintNodeSpawner.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/EngineVersionComparison.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace UE::MCPython::Blueprint2
{
ESemanticConnectedSpawnFailurePoint GSemanticConnectedSpawnFailurePoint =
    ESemanticConnectedSpawnFailurePoint::None;

void SetSemanticConnectedSpawnFailurePointForTests(
    const ESemanticConnectedSpawnFailurePoint Point)
{
    GSemanticConnectedSpawnFailurePoint = Point;
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
            if (!ClassifySemanticResponse(
                    Schema->CanCreateConnection(SourcePin, Input.Pin),
                    Request.bAllowConversion,
                    SourceResponse))
            {
                continue;
            }
            for (const FSemanticTemplatePin& Output : Outputs)
            {
                FSemanticResponse TargetResponse;
                if (!ClassifySemanticResponse(
                        Schema->CanCreateConnection(Output.Pin, TargetPin),
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
    const FRollbackResult Rollback = Scope.Rollback();
#if WITH_DEV_AUTOMATION_TESTS
    if (GSemanticConnectedSpawnFailurePoint ==
        ESemanticConnectedSpawnFailurePoint::AfterRollbackResidual)
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
            TEXT("Connected Blueprint spawn rollback left a graph delta."),
            TEXT("Inspect and resnapshot the graph before another mutation."),
            false,
            Details);
    }
    return SemanticFailure(Code, Path, Message, Hint);
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
