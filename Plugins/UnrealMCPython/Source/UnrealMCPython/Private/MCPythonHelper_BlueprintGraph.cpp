// Copyright (c) 2025 GenOrca (by zenoengine). All Rights Reserved.

#include "MCPythonHelper.h"
#include "MCPythonHelperInternal.h"
#include "MCPythonBlueprint2Internal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "Engine/Blueprint.h"
#include "Engine/MemberReference.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EnumLiteral.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Knot.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_Select.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchString.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_InputKey.h"
#include "K2Node_SpawnActorFromClass.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet/KismetMathLibrary.h"
#include "UObject/UnrealType.h"
// ─── Blueprint Graph Helpers (internal) ──────────────────────────────────────

// ─── GetBlueprintGraphInfo ───────────────────────────────────────────────────

// ─── ListCallableFunctions ───────────────────────────────────────────────────

// ─── ListBlueprintVariables ──────────────────────────────────────────────────

// ─── Blueprint Node Creation Helpers ─────────────────────────────────────────

namespace
{
using namespace UE::MCPython::Blueprint2;

FString GraphFailure(
    const FString& Path,
    const FString& Message,
    const FString& Code = TEXT("INVALID_INPUT"))
{
    return SerializeResult(MakeFailure(
        Code,
        Path,
        Message,
        false,
        TEXT("Correct the request using stable Blueprint 2 identifiers and exact Unreal object paths.")));
}

FString GraphFailureWithDetails(
    const FString& Path,
    const FString& Message,
    const TSharedPtr<FJsonObject>& Details,
    const FString& Code = TEXT("INVALID_INPUT"))
{
    return SerializeResult(MakeFailure(
        Code,
        Path,
        Message,
        false,
        TEXT("Correct the request using stable Blueprint 2 identifiers and exact Unreal object paths."),
        Details));
}

FString LegacyConnectFailure(
    const FString& Path,
    const FString& Message,
    const FString& Code = TEXT("INVALID_INPUT"))
{
    const TSharedRef<FJsonObject> Result = MakeFailure(
        Code,
        Path,
        Message,
        false,
        TEXT("Correct the request using stable Blueprint 2 identifiers and exact Unreal object paths."));
    Result->SetStringField(TEXT("message"), Message);
    return SerializeResult(Result);
}

FString LegacyConnectFailureWithDetails(
    const FString& Path,
    const FString& Message,
    const TSharedPtr<FJsonObject>& Details,
    const FString& Code = TEXT("INVALID_INPUT"))
{
    const TSharedRef<FJsonObject> Result = MakeFailure(
        Code,
        Path,
        Message,
        false,
        TEXT("Correct the request using stable Blueprint 2 identifiers and exact Unreal object paths."),
        Details);
    Result->SetStringField(TEXT("message"), Message);
    return SerializeResult(Result);
}

TSharedRef<FJsonObject> CompileNextAction(UBlueprint* Blueprint)
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
bool IsExactK2Graph(const UEdGraph* Graph)
{
    return Graph && Graph->GetSchema() &&
        Graph->GetSchema()->GetClass() == UEdGraphSchema_K2::StaticClass();
}

bool CanCreateNodeClass(const UClass* NodeClass, const UEdGraph* Graph)
{
    const UEdGraphNode* NodeCDO = NodeClass
        ? Cast<UEdGraphNode>(NodeClass->GetDefaultObject())
        : nullptr;
    return NodeCDO && NodeCDO->CanCreateUnderSpecifiedSchema(Graph->GetSchema());
}

bool TryExactMemberPath(
    const FString& Path,
    UClass*& OutOwner,
    FString& OutMember)
{
    int32 Separator = INDEX_NONE;
    if (!Path.FindLastChar(TEXT(':'), Separator) || Separator <= 0 ||
        Separator == Path.Len() - 1)
    {
        return false;
    }
    const FString OwnerPath = Path.Left(Separator);
    OutMember = Path.Mid(Separator + 1);
    OutOwner = FindObject<UClass>(nullptr, *OwnerPath);
    if (!OutOwner)
    {
        OutOwner = LoadObject<UClass>(nullptr, *OwnerPath);
    }
    return OutOwner && OutOwner->GetPathName() == OwnerPath;
}

template <typename TObjectType>
TObjectType* LoadExactObject(const FString& Path)
{
    TObjectType* Object = FindObject<TObjectType>(nullptr, *Path);
    if (!Object)
    {
        Object = LoadObject<TObjectType>(nullptr, *Path);
    }
    return Object && Object->GetPathName() == Path ? Object : nullptr;
}

bool TryFiniteNumber(
    const TSharedRef<FJsonObject>& Object,
    const TCHAR* Field,
    double& OutValue)
{
    return Object->TryGetNumberField(Field, OutValue) && FMath::IsFinite(OutValue);
}

bool HasOnlyFields(
    const TSharedRef<FJsonObject>& Object,
    std::initializer_list<const TCHAR*> Allowed)
{
    TSet<FString> Names;
    for (const TCHAR* Name : Allowed)
    {
        Names.Add(Name);
    }
    for (const auto& Pair : Object->Values)
    {
        if (!Names.Contains(FString(*Pair.Key)))
        {
            return false;
        }
    }
    return true;
}

TArray<TSharedPtr<FJsonValue>> NodePinIds(
    UBlueprint* Blueprint,
    const UEdGraphNode* Node)
{
    TArray<TSharedPtr<FJsonValue>> PinIds;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && !Pin->bHidden)
        {
            PinIds.Add(MakeShared<FJsonValueString>(
                DescribePinTarget(Blueprint, Pin).Id));
        }
    }
    return PinIds;
}

FString NodeAuthoringSuccess(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UEdGraphNode* Node,
    const FString& Summary,
    const bool bLegacy)
{
    const FTargetRef GraphTarget = DescribeGraphTarget(Blueprint, Graph);
    const FTargetRef NodeTarget = DescribeNodeTarget(Blueprint, Node);
    const TArray<TSharedPtr<FJsonValue>> PinIds = NodePinIds(Blueprint, Node);

    const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
    Position->SetNumberField(TEXT("x"), Node->NodePosX);
    Position->SetNumberField(TEXT("y"), Node->NodePosY);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("node_id"), NodeTarget.Id);
    Data->SetStringField(TEXT("graph_id"), GraphTarget.Id);
    Data->SetStringField(TEXT("class_path"), Node->GetClass()->GetPathName());
    Data->SetObjectField(TEXT("position"), Position);
    Data->SetArrayField(TEXT("pin_ids"), PinIds);
    if (const UK2Node_CallFunction* CallNode =
            Cast<UK2Node_CallFunction>(Node))
    {
        if (const UFunction* TargetFunction = CallNode->GetTargetFunction())
        {
            const UClass* OwnerClass = TargetFunction->GetOuterUClass();
            if (OwnerClass)
            {
                Data->SetStringField(
                    TEXT("reference_path"),
                    OwnerClass->GetPathName() + TEXT(":") +
                        TargetFunction->GetName());
            }
        }
    }
    if (const UEdGraphNode_Comment* CommentNode =
            Cast<UEdGraphNode_Comment>(Node))
    {
        const TSharedRef<FJsonObject> Size = MakeShared<FJsonObject>();
        Size->SetNumberField(TEXT("x"), CommentNode->NodeWidth);
        Size->SetNumberField(TEXT("y"), CommentNode->NodeHeight);
        Data->SetStringField(TEXT("comment"), CommentNode->NodeComment);
        Data->SetObjectField(TEXT("size"), Size);
    }

    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(TEXT("graph_id"), GraphTarget.Id);
    Details->SetStringField(TEXT("class_path"), Node->GetClass()->GetPathName());
    const TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
    Change->SetStringField(TEXT("kind"), TEXT("create"));
    Change->SetStringField(TEXT("target_id"), NodeTarget.Id);
    Change->SetObjectField(TEXT("details"), Details);

    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    const TSharedRef<FJsonObject> NextAction = MakeShared<FJsonObject>();
    NextAction->SetStringField(TEXT("domain"), TEXT("blueprint"));
    NextAction->SetStringField(TEXT("action"), TEXT("compile_blueprint"));
    NextAction->SetObjectField(TEXT("params"), Params);

    const TSharedRef<FJsonObject> Result = MakeSuccess(Summary, Data);
    Result->SetArrayField(
        TEXT("changes"), {MakeShared<FJsonValueObject>(Change)});
    Result->SetArrayField(
        TEXT("next_actions"), {MakeShared<FJsonValueObject>(NextAction)});
    if (bLegacy)
    {
        Result->SetStringField(TEXT("node_name"), Node->GetName());
        Result->SetStringField(
            TEXT("node_title"),
            Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
        Result->SetStringField(TEXT("message"), Summary);
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->bHidden)
            {
                continue;
            }
            const TSharedRef<FJsonObject> PinObject = MakeShared<FJsonObject>();
            PinObject->SetStringField(
                TEXT("pin_id"), DescribePinTarget(Blueprint, Pin).Id);
            PinObject->SetStringField(TEXT("pin_name"), Pin->GetName());
            const FString Friendly = Pin->PinFriendlyName.ToString();
            if (!Friendly.IsEmpty())
            {
                PinObject->SetStringField(TEXT("friendly_name"), Friendly);
            }
            PinObject->SetStringField(
                TEXT("direction"),
                Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
            PinObject->SetStringField(
                TEXT("type"), Pin->PinType.PinCategory.ToString());
            Pins.Add(MakeShared<FJsonValueObject>(PinObject));
        }
        Result->SetArrayField(TEXT("pins"), Pins);
    }
    return SerializeResult(Result);
}

FString RollbackGraphFailure(
    FMutationScope& Scope,
    const FString& Path,
    const FString& Message)
{
    const FRollbackResult Rollback = Scope.Rollback();
    if (!Rollback.bSucceeded && !Rollback.bDeferredToWorkflow)
    {
        return GraphFailure(
            TEXT("transaction"),
            TEXT("Blueprint graph mutation rollback failed."),
            TEXT("ROLLBACK_FAILED"));
    }
    return GraphFailure(Path, Message);
}

FString RollbackLegacyConnectFailure(
    FMutationScope& Scope,
    const FString& Path,
    const FString& Message)
{
    const FRollbackResult Rollback = Scope.Rollback();
    if (!Rollback.bSucceeded && !Rollback.bDeferredToWorkflow)
    {
        return LegacyConnectFailure(
            TEXT("transaction"),
            TEXT("Blueprint graph mutation rollback failed."),
            TEXT("ROLLBACK_FAILED"));
    }
    return LegacyConnectFailure(Path, Message);
}

void ModifyPinAndLinks(FMutationScope& Scope, UEdGraphPin* Pin)
{
    if (!Pin)
    {
        return;
    }
    Scope.Modify(Pin->GetOwningNode());
    for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
    {
        if (LinkedPin)
        {
            Scope.Modify(LinkedPin->GetOwningNode());
        }
    }
}

FString CanonicalTypeJson(const FEdGraphPinType& Type)
{
    FString Json;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    FJsonSerializer::Serialize(SerializeTypeSpec(Type), Writer);
    return Json;
}

UFunction* FindExactOperatorFunction(
    const FString& Operator,
    const FEdGraphPinType& OperandType,
    FString& OutError)
{
    static const TMap<FString, FString> Prefixes = {
        {TEXT("add"), TEXT("Add_")},
        {TEXT("subtract"), TEXT("Subtract_")},
        {TEXT("multiply"), TEXT("Multiply_")},
        {TEXT("divide"), TEXT("Divide_")},
        {TEXT("equal"), TEXT("EqualEqual_")},
        {TEXT("not_equal"), TEXT("NotEqual_")},
        {TEXT("less"), TEXT("Less_")},
        {TEXT("less_equal"), TEXT("LessEqual_")},
        {TEXT("greater"), TEXT("Greater_")},
        {TEXT("greater_equal"), TEXT("GreaterEqual_")},
    };
    const FString* Prefix = Prefixes.Find(Operator);
    if (!Prefix)
    {
        OutError = TEXT("Unknown operator name.");
        return nullptr;
    }

    const bool bComparison =
        Operator == TEXT("equal") || Operator == TEXT("not_equal") ||
        Operator == TEXT("less") || Operator == TEXT("less_equal") ||
        Operator == TEXT("greater") || Operator == TEXT("greater_equal");
    FEdGraphPinType ReturnType = OperandType;
    if (bComparison)
    {
        ReturnType.ResetToDefaults();
        ReturnType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    }
    const FString OperandJson = CanonicalTypeJson(OperandType);
    const FString ReturnJson = CanonicalTypeJson(ReturnType);
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    TArray<UFunction*> Matches;
    for (TFieldIterator<UFunction> FunctionIt(
            UKismetMathLibrary::StaticClass(),
            EFieldIteratorFlags::ExcludeSuper);
         FunctionIt;
         ++FunctionIt)
    {
        UFunction* Candidate = *FunctionIt;
        if (!Candidate || !Candidate->GetName().StartsWith(*Prefix))
        {
            continue;
        }
        TArray<const FProperty*> Inputs;
        for (TFieldIterator<FProperty> PropertyIt(Candidate); PropertyIt; ++PropertyIt)
        {
            const FProperty* Parameter = *PropertyIt;
            if (Parameter->HasAnyPropertyFlags(CPF_Parm) &&
                !Parameter->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
            {
                Inputs.Add(Parameter);
            }
        }
        const FProperty* ReturnProperty = Candidate->GetReturnProperty();
        if (Inputs.Num() < 2 || !ReturnProperty)
        {
            continue;
        }
        FEdGraphPinType FirstType;
        FEdGraphPinType SecondType;
        FEdGraphPinType CandidateReturnType;
        if (!Schema->ConvertPropertyToPinType(Inputs[0], FirstType) ||
            !Schema->ConvertPropertyToPinType(Inputs[1], SecondType) ||
            !Schema->ConvertPropertyToPinType(ReturnProperty, CandidateReturnType))
        {
            continue;
        }
        if (CanonicalTypeJson(FirstType) == OperandJson &&
            CanonicalTypeJson(SecondType) == OperandJson &&
            CanonicalTypeJson(CandidateReturnType) == ReturnJson)
        {
            Matches.Add(Candidate);
        }
    }
    if (Matches.Num() != 1)
    {
        OutError = FString::Printf(
            TEXT("Operator resolution expected one exact UKismetMathLibrary candidate, found %d."),
            Matches.Num());
        return nullptr;
    }
    return Matches[0];
}

bool ResolveCommonVariableName(
    UBlueprint* Blueprint,
    const TSharedPtr<FJsonObject>& NodeJson,
    FString& OutName,
    FString& OutError)
{
    FString VariableId;
    if (NodeJson->TryGetStringField(TEXT("variable_id"), VariableId))
    {
        FTargetRef Target;
        Target.Id = VariableId;
        FString ResolveError;
        const FResolvedTarget Resolved = ResolveTarget(
            Blueprint, ETargetKind::Variable, Target, ResolveError);
        if (!Resolved.Variable)
        {
            OutError = FString::Printf(
                TEXT("Variable target could not be resolved: %s"), *ResolveError);
            return false;
        }
        OutName = Resolved.Variable->VarName.ToString();
        return true;
    }
    if (!NodeJson->TryGetStringField(TEXT("variable_name"), OutName) ||
        OutName.IsEmpty())
    {
        OutError = TEXT("Variable node requires 'variable_id' or legacy 'variable_name'.");
        return false;
    }
    return true;
}

TArray<FString> AllowedNodeProperties(const UEdGraphNode* Node)
{
    TArray<FString> Allowed = {
        TEXT("comment"),
        TEXT("comment_bubble_visible"),
        TEXT("enabled_state"),
        TEXT("position"),
        TEXT("pin_defaults"),
    };
    if (Node && Node->IsA<UK2Node_ExecutionSequence>())
    {
        Allowed.Add(TEXT("output_count"));
    }
    else if (Node && Node->IsA<UK2Node_Select>())
    {
        Allowed.Add(TEXT("option_count"));
    }
    else if (Node && (
        Node->IsA<UK2Node_SwitchInteger>() ||
        Node->IsA<UK2Node_SwitchString>() ||
        Node->IsA<UK2Node_SwitchName>()))
    {
        Allowed.Add(TEXT("cases"));
    }
    Allowed.Sort();
    return Allowed;
}

TSharedRef<FJsonObject> AllowedPropertiesDetails(
    const TArray<FString>& Allowed)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FString& Property : Allowed)
    {
        Values.Add(MakeShared<FJsonValueString>(Property));
    }
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetArrayField(TEXT("allowed_properties"), Values);
    return Details;
}

bool TryIntegerInRange(
    const TSharedRef<FJsonObject>& Object,
    const TCHAR* Field,
    int32 Minimum,
    int32 Maximum,
    int32& OutValue)
{
    double Number = 0.0;
    if (!Object->TryGetNumberField(Field, Number) ||
        !FMath::IsFinite(Number) ||
        Number < static_cast<double>(Minimum) ||
        Number > static_cast<double>(Maximum))
    {
        return false;
    }
    const int32 Integer = static_cast<int32>(Number);
    if (Number != static_cast<double>(Integer))
    {
        return false;
    }
    OutValue = Integer;
    return true;
}

struct FPreparedPinDefault
{
    FString PinId;
    FNormalizedDefault Value;
};

struct FNodePropertyPlan
{
    bool bSetComment = false;
    FString Comment;
    bool bSetCommentBubbleVisible = false;
    bool bCommentBubbleVisible = false;
    bool bSetEnabledState = false;
    ENodeEnabledState EnabledState = ENodeEnabledState::Enabled;
    bool bSetPosition = false;
    int32 PositionX = 0;
    int32 PositionY = 0;
    bool bSetOutputCount = false;
    int32 OutputCount = 0;
    bool bSetOptionCount = false;
    int32 OptionCount = 0;
    bool bSetCases = false;
    TArray<int32> IntegerCases;
    TArray<FName> NamedCases;
    TArray<FPreparedPinDefault> PinDefaults;
};

constexpr int32 MaxSwitchCases = 64;

bool ParseEnabledState(
    const FString& Value,
    ENodeEnabledState& OutState)
{
    if (Value == TEXT("enabled"))
    {
        OutState = ENodeEnabledState::Enabled;
        return true;
    }
    if (Value == TEXT("disabled"))
    {
        OutState = ENodeEnabledState::Disabled;
        return true;
    }
    if (Value == TEXT("development_only"))
    {
        OutState = ENodeEnabledState::DevelopmentOnly;
        return true;
    }
    return false;
}

bool ParseSwitchCases(
    const TSharedRef<FJsonObject>& Properties,
    UEdGraphNode* Node,
    FNodePropertyPlan& Plan,
    FString& OutError)
{
    const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
    if (!Properties->TryGetArrayField(TEXT("cases"), Cases) || !Cases)
    {
        OutError = TEXT("cases must be an array.");
        return false;
    }
    if (Cases->Num() > MaxSwitchCases)
    {
        OutError = FString::Printf(
            TEXT("cases must contain at most %d entries."), MaxSwitchCases);
        return false;
    }
    Plan.bSetCases = true;
    if (Node->IsA<UK2Node_SwitchInteger>())
    {
        for (const TSharedPtr<FJsonValue>& Case : *Cases)
        {
            double Number = 0.0;
            if (!Case.IsValid() || !Case->TryGetNumber(Number) ||
                !FMath::IsFinite(Number) ||
                Number < static_cast<double>(MIN_int32) ||
                Number > static_cast<double>(MAX_int32))
            {
                OutError = TEXT("Integer Switch cases must be int32 values.");
                return false;
            }
            const int32 Integer = static_cast<int32>(Number);
            if (Number != static_cast<double>(Integer))
            {
                OutError = TEXT("Integer Switch cases must be int32 values.");
                return false;
            }
            Plan.IntegerCases.Add(Integer);
        }
        for (int32 Index = 1; Index < Plan.IntegerCases.Num(); ++Index)
        {
            const int64 Expected =
                static_cast<int64>(Plan.IntegerCases[0]) + Index;
            if (Expected > MAX_int32 ||
                Plan.IntegerCases[Index] != static_cast<int32>(Expected))
            {
                OutError = TEXT(
                    "Integer Switch cases must be consecutive and ordered.");
                return false;
            }
        }
        return true;
    }

    TSet<FName> Seen;
    for (const TSharedPtr<FJsonValue>& Case : *Cases)
    {
        FString Value;
        if (!Case.IsValid() || !Case->TryGetString(Value) || Value.IsEmpty())
        {
            OutError = TEXT(
                "String and Name Switch cases must be non-empty strings.");
            return false;
        }
        const FName Name(*Value);
        if (Seen.Contains(Name))
        {
            OutError = TEXT("Switch cases must be unique.");
            return false;
        }
        Seen.Add(Name);
        Plan.NamedCases.Add(Name);
    }
    return true;
}

bool ResolveStablePin(
    UBlueprint* Blueprint,
    const FString& PinId,
    FResolvedTarget& OutPin,
    FString& OutError)
{
    FTargetRef Target;
    Target.Id = PinId;
    OutPin = ResolveTarget(Blueprint, ETargetKind::Pin, Target, OutError);
    return OutPin.Pin && OutPin.Node && OutPin.Graph;
}

UEdGraphNode* ResolveConnectionNode(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FString& Reference,
    FString& OutError)
{
    if (Reference.StartsWith(TEXT("node:")))
    {
        FTargetRef Target;
        Target.Id = Reference;
        const FResolvedTarget Resolved = ResolveTarget(
            Blueprint, ETargetKind::Node, Target, OutError);
        if (!Resolved.Node)
        {
            return nullptr;
        }
        if (Resolved.Graph != Graph)
        {
            OutError = TEXT("Stable node target does not belong to the requested graph.");
            return nullptr;
        }
        return Resolved.Node;
    }
    UEdGraphNode* Node = FindBPNodeByName(Graph, Reference);
    if (!Node)
    {
        OutError = FString::Printf(
            TEXT("Exact legacy node name '%s' was not found."), *Reference);
    }
    return Node;
}

UEdGraphPin* ResolveConnectionPin(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UEdGraphNode* Node,
    const FString& Reference,
    FString& OutError)
{
    if (Reference.StartsWith(TEXT("pin:")))
    {
        FResolvedTarget Resolved;
        if (!ResolveStablePin(Blueprint, Reference, Resolved, OutError))
        {
            return nullptr;
        }
        if (Resolved.Graph != Graph || Resolved.Node != Node)
        {
            OutError = TEXT(
                "Stable pin target does not belong to the requested node and graph.");
            return nullptr;
        }
        return Resolved.Pin;
    }
    UEdGraphPin* Pin = FindPinByName(Node, Reference);
    if (!Pin)
    {
        OutError = FString::Printf(
            TEXT("Exact legacy pin name '%s' was not found on node '%s'."),
            *Reference,
            *Node->GetName());
    }
    return Pin;
}

FString ConnectionResponseName(const ECanCreateConnectionResponse Response)
{
    switch (Response)
    {
    case CONNECT_RESPONSE_MAKE: return TEXT("make");
    case CONNECT_RESPONSE_DISALLOW: return TEXT("disallow");
    case CONNECT_RESPONSE_BREAK_OTHERS_A: return TEXT("break_others_a");
    case CONNECT_RESPONSE_BREAK_OTHERS_B: return TEXT("break_others_b");
    case CONNECT_RESPONSE_BREAK_OTHERS_AB: return TEXT("break_others_ab");
    case CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE:
        return TEXT("make_with_conversion_node");
    case CONNECT_RESPONSE_MAKE_WITH_PROMOTION:
        return TEXT("make_with_promotion");
    default: return TEXT("unknown");
    }
}

TSharedRef<FJsonObject> ConnectionDetails(
    UBlueprint* Blueprint,
    UEdGraphPin* OutputPin,
    UEdGraphPin* InputPin,
    const FPinConnectionResponse* Response = nullptr)
{
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(
        TEXT("source_pin_id"), DescribePinTarget(Blueprint, OutputPin).Id);
    Details->SetStringField(
        TEXT("target_pin_id"), DescribePinTarget(Blueprint, InputPin).Id);
    Details->SetObjectField(
        TEXT("source_type"), SerializeTypeSpec(OutputPin->PinType));
    Details->SetObjectField(
        TEXT("target_type"), SerializeTypeSpec(InputPin->PinType));
    if (Response)
    {
        Details->SetStringField(
            TEXT("schema_response"), ConnectionResponseName(Response->Response));
        Details->SetStringField(
            TEXT("schema_message"), Response->Message.ToString());
    }
    return Details;
}

struct FNormalizedConnection
{
    FString SourcePinId;
    FString TargetPinId;
};

FNormalizedConnection NormalizeConnection(
    UBlueprint* Blueprint,
    UEdGraphPin* First,
    UEdGraphPin* Second)
{
    UEdGraphPin* OutputPin = First;
    UEdGraphPin* InputPin = Second;
    if (First && Second && First->Direction == EGPD_Input &&
        Second->Direction == EGPD_Output)
    {
        OutputPin = Second;
        InputPin = First;
    }
    FNormalizedConnection Result;
    Result.SourcePinId = DescribePinTarget(Blueprint, OutputPin).Id;
    Result.TargetPinId = DescribePinTarget(Blueprint, InputPin).Id;
    return Result;
}

FString ConnectionTargetId(const FNormalizedConnection& Connection)
{
    return TEXT("connection:") + Connection.SourcePinId + TEXT("->") +
        Connection.TargetPinId;
}

TSharedRef<FJsonObject> ConnectionChange(
    const FString& Kind,
    const FNormalizedConnection& Connection)
{
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(TEXT("source_pin_id"), Connection.SourcePinId);
    Details->SetStringField(TEXT("target_pin_id"), Connection.TargetPinId);
    const TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
    Change->SetStringField(TEXT("kind"), Kind);
    Change->SetStringField(
        TEXT("target_id"), ConnectionTargetId(Connection));
    Change->SetObjectField(TEXT("details"), Details);
    return Change;
}

TMap<FString, FNormalizedConnection> SnapshotConnections(
    UBlueprint* Blueprint,
    UEdGraph* Graph)
{
    TMap<FString, FNormalizedConnection> Snapshot;
    if (!Blueprint || !Graph)
    {
        return Snapshot;
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
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                if (!Linked || !Linked->GetOwningNode() ||
                    Linked->GetOwningNode()->GetGraph() != Graph)
                {
                    continue;
                }
                const FNormalizedConnection Connection =
                    NormalizeConnection(Blueprint, Pin, Linked);
                Snapshot.Add(ConnectionTargetId(Connection), Connection);
            }
        }
    }
    return Snapshot;
}

TArray<TSharedPtr<FJsonValue>> DiffConnectionChanges(
    const TMap<FString, FNormalizedConnection>& Before,
    const TMap<FString, FNormalizedConnection>& After)
{
    struct FConnectionDelta
    {
        FString Kind;
        FString TargetId;
        FNormalizedConnection Connection;
    };

    TArray<FConnectionDelta> Deltas;
    for (const auto& Existing : Before)
    {
        if (!After.Contains(Existing.Key))
        {
            Deltas.Add({TEXT("delete"), Existing.Key, Existing.Value});
        }
    }
    for (const auto& Current : After)
    {
        if (!Before.Contains(Current.Key))
        {
            Deltas.Add({TEXT("create"), Current.Key, Current.Value});
        }
    }
    Deltas.Sort([](const FConnectionDelta& A, const FConnectionDelta& B)
    {
        const int32 TargetComparison =
            A.TargetId.Compare(B.TargetId, ESearchCase::CaseSensitive);
        return TargetComparison == 0 ? A.Kind < B.Kind : TargetComparison < 0;
    });

    TArray<TSharedPtr<FJsonValue>> Changes;
    for (const FConnectionDelta& Delta : Deltas)
    {
        Changes.Add(MakeShared<FJsonValueObject>(
            ConnectionChange(Delta.Kind, Delta.Connection)));
    }
    return Changes;
}
}

static UEdGraphNode* CreateBPNodeFromJson(UEdGraph* Graph, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeJson, FString& OutError)
{
    FString NodeType;
    if (!NodeJson->TryGetStringField(TEXT("type"), NodeType))
    {
        OutError = TEXT("Node JSON missing 'type' field.");
        return nullptr;
    }

    double PosXd = 0, PosYd = 0;
    NodeJson->TryGetNumberField(TEXT("pos_x"), PosXd);
    NodeJson->TryGetNumberField(TEXT("pos_y"), PosYd);
    if (!FMath::IsFinite(PosXd) || !FMath::IsFinite(PosYd) ||
        PosXd < MIN_int32 || PosXd > MAX_int32 ||
        PosYd < MIN_int32 || PosYd > MAX_int32)
    {
        OutError = TEXT("Node position must contain finite int32 coordinates.");
        return nullptr;
    }
    int32 PosX = (int32)PosXd;
    int32 PosY = (int32)PosYd;

    UEdGraphNode* NewNode = nullptr;

    if (NodeType == TEXT("CallFunction"))
    {
        FString TargetClass, FunctionName;
        if (!NodeJson->TryGetStringField(TEXT("function_name"), FunctionName))
        {
            OutError = TEXT("CallFunction node missing 'function_name'.");
            return nullptr;
        }
        NodeJson->TryGetStringField(TEXT("target"), TargetClass);

        // Find the UFunction
        UFunction* TargetFunc = nullptr;
        if (!TargetClass.IsEmpty())
        {
            UClass* Cls = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *TargetClass));
            if (!Cls)
                Cls = FindFirstObject<UClass>(*TargetClass, EFindFirstObjectOptions::NativeFirst);
            if (Cls)
                TargetFunc = Cls->FindFunctionByName(FName(*FunctionName));
        }

        if (!TargetFunc)
        {
            // Search in the Blueprint's generated class hierarchy
            for (UClass* Cls = Blueprint->GeneratedClass; Cls && !TargetFunc; Cls = Cls->GetSuperClass())
            {
                TargetFunc = Cls->FindFunctionByName(FName(*FunctionName));
            }
        }

        if (!TargetFunc)
        {
            OutError = FString::Printf(TEXT("Function '%s' not found (target: '%s')."), *FunctionName, *TargetClass);
            return nullptr;
        }

        FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
        UK2Node_CallFunction* FuncNode = Creator.CreateNode(false);
        FuncNode->SetFromFunction(TargetFunc);
        FuncNode->NodePosX = PosX;
        FuncNode->NodePosY = PosY;
        Creator.Finalize();
        NewNode = FuncNode;
    }
    else if (NodeType == TEXT("Event"))
    {
        FString EventName;
        UClass* EventClass = nullptr;
        FString FunctionPath;
        if (NodeJson->TryGetStringField(TEXT("function_path"), FunctionPath))
        {
            if (!TryExactMemberPath(FunctionPath, EventClass, EventName))
            {
                OutError = TEXT("Event 'function_path' must be an exact ClassPath:FunctionName path.");
                return nullptr;
            }
        }
        else if (!NodeJson->TryGetStringField(TEXT("event_name"), EventName))
        {
            OutError = TEXT("Event node requires 'function_path' or legacy 'event_name'.");
            return nullptr;
        }

        if (!EventClass)
        {
            EventClass = Blueprint->GeneratedClass
                ? Blueprint->GeneratedClass
                : Blueprint->ParentClass;
        }
        UFunction* EventFunc = EventClass ? EventClass->FindFunctionByName(FName(*EventName)) : nullptr;

        if (!EventFunc ||
            (!FunctionPath.IsEmpty() && EventFunc->GetOuterUClass() != EventClass))
        {
            OutError = FString::Printf(TEXT("Event function '%s' not found in class hierarchy."), *EventName);
            return nullptr;
        }

        FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
        UK2Node_Event* EventNode = Creator.CreateNode(false);
        EventNode->EventReference.SetExternalMember(FName(*EventName), EventClass);
        EventNode->bOverrideFunction = true;
        EventNode->NodePosX = PosX;
        EventNode->NodePosY = PosY;
        Creator.Finalize();
        NewNode = EventNode;
    }
    else if (NodeType == TEXT("CustomEvent"))
    {
        FString EventName;
        if (!NodeJson->TryGetStringField(TEXT("event_name"), EventName))
        {
            OutError = TEXT("CustomEvent node missing 'event_name'.");
            return nullptr;
        }

        FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
        UK2Node_CustomEvent* CustomNode = Creator.CreateNode(false);
        if (!CustomNode)
        {
            OutError = FString::Printf(TEXT("Failed to create CustomEvent '%s'."), *EventName);
            return nullptr;
        }
        CustomNode->CustomFunctionName = FName(*EventName);
        CustomNode->NodePosX = PosX;
        CustomNode->NodePosY = PosY;
        Creator.Finalize();
        NewNode = CustomNode;
    }
    else if (NodeType == TEXT("CastTo"))
    {
        FString CastClass;
        const bool bExactClassPath =
            NodeJson->TryGetStringField(TEXT("class_path"), CastClass);
        if (!bExactClassPath &&
            !NodeJson->TryGetStringField(TEXT("cast_class"), CastClass))
        {
            OutError = TEXT("CastTo node requires 'class_path' or legacy 'cast_class'.");
            return nullptr;
        }
        UClass* TargetClass = bExactClassPath
            ? LoadExactObject<UClass>(CastClass)
            : LoadClass<UObject>(nullptr, *CastClass);
        if (!TargetClass && !bExactClassPath)
            TargetClass = LoadClass<UObject>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *CastClass));
        if (!TargetClass && !bExactClassPath)
            TargetClass = LoadClass<UObject>(nullptr, *FString::Printf(TEXT("/Script/AIModule.%s"), *CastClass));
        if (!TargetClass)
        {
            OutError = FString::Printf(TEXT("CastTo: class '%s' not found."), *CastClass);
            return nullptr;
        }
        FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
        UK2Node_DynamicCast* CastNode = Creator.CreateNode(false);
        CastNode->TargetType = TargetClass;
        CastNode->NodePosX = PosX;
        CastNode->NodePosY = PosY;
        Creator.Finalize();
        NewNode = CastNode;
    }
    else if (NodeType == TEXT("Branch"))
    {
        FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
        UK2Node_IfThenElse* BranchNode = Creator.CreateNode(false);
        BranchNode->NodePosX = PosX;
        BranchNode->NodePosY = PosY;
        Creator.Finalize();
        NewNode = BranchNode;
    }
    else if (NodeType == TEXT("Sequence"))
    {
        double OutputCountValue = 2.0;
        NodeJson->TryGetNumberField(TEXT("output_count"), OutputCountValue);
        const int32 OutputCount = static_cast<int32>(OutputCountValue);
        if (!FMath::IsFinite(OutputCountValue) ||
            OutputCountValue != OutputCount || OutputCount < 2 || OutputCount > 64)
        {
            OutError = TEXT("Sequence output_count must be an integer from 2 to 64.");
            return nullptr;
        }
        FGraphNodeCreator<UK2Node_ExecutionSequence> Creator(*Graph);
        UK2Node_ExecutionSequence* SeqNode = Creator.CreateNode(false);
        SeqNode->NodePosX = PosX;
        SeqNode->NodePosY = PosY;
        Creator.Finalize();
        int32 ExistingOutputs = 0;
        for (const UEdGraphPin* Pin : SeqNode->Pins)
        {
            ExistingOutputs += Pin && Pin->Direction == EGPD_Output ? 1 : 0;
        }
        while (ExistingOutputs < OutputCount)
        {
            SeqNode->AddInputPin();
            ++ExistingOutputs;
        }
        NewNode = SeqNode;
    }
    else if (NodeType == TEXT("VariableGet"))
    {
        FString VarName;
        if (!ResolveCommonVariableName(Blueprint, NodeJson, VarName, OutError))
        {
            return nullptr;
        }

        FString VarClass;
        const bool bHasExternalClass = NodeJson->TryGetStringField(TEXT("variable_class"), VarClass) && !VarClass.IsEmpty();

        FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
        UK2Node_VariableGet* GetNode = Creator.CreateNode(false);

        if (bHasExternalClass)
        {
            UClass* OwnerClass = LoadClass<UObject>(nullptr, *VarClass);
            if (!OwnerClass)
                OwnerClass = LoadClass<UObject>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *VarClass));
            if (OwnerClass)
                GetNode->VariableReference.SetExternalMember(FName(*VarName), OwnerClass);
            else
                GetNode->VariableReference.SetSelfMember(FName(*VarName));
        }
        else
        {
            GetNode->VariableReference.SetSelfMember(FName(*VarName));
        }

        GetNode->NodePosX = PosX;
        GetNode->NodePosY = PosY;
        Creator.Finalize();
        NewNode = GetNode;
    }
    else if (NodeType == TEXT("VariableSet"))
    {
        FString VarName;
        if (!ResolveCommonVariableName(Blueprint, NodeJson, VarName, OutError))
        {
            return nullptr;
        }

        FString VarClass;
        const bool bHasExternalClass = NodeJson->TryGetStringField(TEXT("variable_class"), VarClass) && !VarClass.IsEmpty();

        FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
        UK2Node_VariableSet* SetNode = Creator.CreateNode(false);

        if (bHasExternalClass)
        {
            UClass* OwnerClass = LoadClass<UObject>(nullptr, *VarClass);
            if (!OwnerClass)
                OwnerClass = LoadClass<UObject>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *VarClass));
            if (OwnerClass)
                SetNode->VariableReference.SetExternalMember(FName(*VarName), OwnerClass);
            else
                SetNode->VariableReference.SetSelfMember(FName(*VarName));
        }
        else
        {
            SetNode->VariableReference.SetSelfMember(FName(*VarName));
        }

        SetNode->NodePosX = PosX;
        SetNode->NodePosY = PosY;
        Creator.Finalize();
        NewNode = SetNode;
    }
    else if (NodeType == TEXT("Operator"))
    {
        FString Operator;
        if (!NodeJson->TryGetStringField(TEXT("operator"), Operator) ||
            Operator.IsEmpty())
        {
            OutError = TEXT("Operator node missing 'operator'.");
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* TypeObject = nullptr;
        if (!NodeJson->TryGetObjectField(TEXT("operand_type"), TypeObject) ||
            !TypeObject || !TypeObject->IsValid())
        {
            OutError = TEXT("Operator node requires canonical 'operand_type'.");
            return nullptr;
        }
        FEdGraphPinType OperandType;
        FError TypeError;
        if (!ParseTypeSpec(
                TypeObject->ToSharedRef(),
                OperandType,
                TypeError,
                TEXT("node_json.operand_type")))
        {
            OutError = TypeError.Message;
            return nullptr;
        }
        UFunction* OperatorFunction = FindExactOperatorFunction(
            Operator, OperandType, OutError);
        if (!OperatorFunction)
        {
            return nullptr;
        }
        FGraphNodeCreator<UK2Node_PromotableOperator> Creator(*Graph);
        UK2Node_PromotableOperator* OperatorNode = Creator.CreateNode(false);
        OperatorNode->SetFromFunction(OperatorFunction);
        OperatorNode->NodePosX = PosX;
        OperatorNode->NodePosY = PosY;
        Creator.Finalize();
        NewNode = OperatorNode;
    }
    else if (NodeType == TEXT("Select"))
    {
        double OptionCountValue = 0.0;
        const TSharedPtr<FJsonObject>* TypeObject = nullptr;
        if (!NodeJson->TryGetNumberField(
                TEXT("option_count"), OptionCountValue) ||
            !NodeJson->TryGetObjectField(TEXT("value_type"), TypeObject) ||
            !TypeObject || !TypeObject->IsValid())
        {
            OutError = TEXT("Select node requires 'option_count' and canonical 'value_type'.");
            return nullptr;
        }
        const int32 OptionCount = static_cast<int32>(OptionCountValue);
        if (!FMath::IsFinite(OptionCountValue) ||
            OptionCountValue != OptionCount || OptionCount < 2 || OptionCount > 64)
        {
            OutError = TEXT("Select option_count must be an integer from 2 to 64.");
            return nullptr;
        }
        FEdGraphPinType ValueType;
        FError TypeError;
        if (!ParseTypeSpec(
                TypeObject->ToSharedRef(),
                ValueType,
                TypeError,
                TEXT("node_json.value_type")))
        {
            OutError = TypeError.Message;
            return nullptr;
        }
        FGraphNodeCreator<UK2Node_Select> Creator(*Graph);
        UK2Node_Select* SelectNode = Creator.CreateNode(false);
        SelectNode->NodePosX = PosX;
        SelectNode->NodePosY = PosY;
        Creator.Finalize();
        TArray<UEdGraphPin*> OptionPins;
        SelectNode->GetOptionPins(OptionPins);
        while (OptionPins.Num() < OptionCount)
        {
            SelectNode->AddInputPin();
            SelectNode->GetOptionPins(OptionPins);
        }
        if (UEdGraphPin* ReturnPin = SelectNode->GetReturnValuePin())
        {
            ReturnPin->PinType = ValueType;
        }
        for (UEdGraphPin* OptionPin : OptionPins)
        {
            if (OptionPin)
            {
                OptionPin->PinType = ValueType;
            }
        }
        NewNode = SelectNode;
    }
    else if (NodeType == TEXT("Switch"))
    {
        FString SwitchKind;
        if (!NodeJson->TryGetStringField(TEXT("switch_kind"), SwitchKind))
        {
            OutError = TEXT("Switch node missing 'switch_kind'.");
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
        NodeJson->TryGetArrayField(TEXT("cases"), Cases);
        if (SwitchKind == TEXT("enum"))
        {
            FString EnumPath;
            if (!NodeJson->TryGetStringField(TEXT("enum_path"), EnumPath))
            {
                OutError = TEXT("Enum Switch requires 'enum_path'.");
                return nullptr;
            }
            UEnum* Enum = LoadExactObject<UEnum>(EnumPath);
            if (!Enum)
            {
                OutError = TEXT("Switch enum_path must resolve to an exact UEnum.");
                return nullptr;
            }
            if (Cases)
            {
                OutError = TEXT("Enum Switch derives its cases from enum_path and does not accept 'cases'.");
                return nullptr;
            }
            FGraphNodeCreator<UK2Node_SwitchEnum> Creator(*Graph);
            UK2Node_SwitchEnum* SwitchNode = Creator.CreateNode(false);
            SwitchNode->SetEnum(Enum);
            SwitchNode->NodePosX = PosX;
            SwitchNode->NodePosY = PosY;
            Creator.Finalize();
            NewNode = SwitchNode;
        }
        else if (SwitchKind == TEXT("int"))
        {
            TArray<int32> IntegerCases;
            if (Cases)
            {
                for (const TSharedPtr<FJsonValue>& Case : *Cases)
                {
                    double Value = 0.0;
                    if (!Case.IsValid() || !Case->TryGetNumber(Value) ||
                        !FMath::IsFinite(Value) || Value != static_cast<int32>(Value))
                    {
                        OutError = TEXT("Integer Switch cases must be int32 values.");
                        return nullptr;
                    }
                    IntegerCases.Add(static_cast<int32>(Value));
                }
                for (int32 Index = 1; Index < IntegerCases.Num(); ++Index)
                {
                    if (IntegerCases[Index] != IntegerCases[0] + Index)
                    {
                        OutError = TEXT("Integer Switch cases must be consecutive and ordered.");
                        return nullptr;
                    }
                }
            }
            FGraphNodeCreator<UK2Node_SwitchInteger> Creator(*Graph);
            UK2Node_SwitchInteger* SwitchNode = Creator.CreateNode(false);
            if (!IntegerCases.IsEmpty())
            {
                SwitchNode->StartIndex = IntegerCases[0];
            }
            SwitchNode->NodePosX = PosX;
            SwitchNode->NodePosY = PosY;
            Creator.Finalize();
            for (int32 Index = 0; Index < IntegerCases.Num(); ++Index)
            {
                SwitchNode->AddPinToSwitchNode();
            }
            NewNode = SwitchNode;
        }
        else if (SwitchKind == TEXT("string") || SwitchKind == TEXT("name"))
        {
            TArray<FName> PinNames;
            if (Cases)
            {
                TSet<FName> Seen;
                for (const TSharedPtr<FJsonValue>& Case : *Cases)
                {
                    FString Value;
                    if (!Case.IsValid() || !Case->TryGetString(Value) || Value.IsEmpty())
                    {
                        OutError = TEXT("String and Name Switch cases must be non-empty strings.");
                        return nullptr;
                    }
                    const FName PinName(*Value);
                    if (Seen.Contains(PinName))
                    {
                        OutError = TEXT("Switch cases must be unique.");
                        return nullptr;
                    }
                    Seen.Add(PinName);
                    PinNames.Add(PinName);
                }
            }
            if (SwitchKind == TEXT("string"))
            {
                FGraphNodeCreator<UK2Node_SwitchString> Creator(*Graph);
                UK2Node_SwitchString* SwitchNode = Creator.CreateNode(false);
                SwitchNode->PinNames = PinNames;
                SwitchNode->NodePosX = PosX;
                SwitchNode->NodePosY = PosY;
                Creator.Finalize();
                NewNode = SwitchNode;
            }
            else
            {
                FGraphNodeCreator<UK2Node_SwitchName> Creator(*Graph);
                UK2Node_SwitchName* SwitchNode = Creator.CreateNode(false);
                SwitchNode->PinNames = PinNames;
                SwitchNode->NodePosX = PosX;
                SwitchNode->NodePosY = PosY;
                Creator.Finalize();
                NewNode = SwitchNode;
            }
        }
        else
        {
            OutError = TEXT("switch_kind must be enum, int, string, or name.");
            return nullptr;
        }
    }
    else if (NodeType == TEXT("Reroute"))
    {
        FGraphNodeCreator<UK2Node_Knot> Creator(*Graph);
        UK2Node_Knot* KnotNode = Creator.CreateNode(false);
        KnotNode->NodePosX = PosX;
        KnotNode->NodePosY = PosY;
        Creator.Finalize();
        NewNode = KnotNode;
    }
    else if (NodeType == TEXT("Comment"))
    {
        FString Text;
        double SizeX = 0.0;
        double SizeY = 0.0;
        if (!NodeJson->TryGetStringField(TEXT("text"), Text) ||
            !NodeJson->TryGetNumberField(TEXT("size_x"), SizeX) ||
            !NodeJson->TryGetNumberField(TEXT("size_y"), SizeY) ||
            !FMath::IsFinite(SizeX) || !FMath::IsFinite(SizeY) ||
            SizeX <= 0.0 || SizeY <= 0.0 ||
            SizeX > MAX_int32 || SizeY > MAX_int32)
        {
            OutError = TEXT("Comment requires text and positive finite size_x/size_y.");
            return nullptr;
        }
        FGraphNodeCreator<UEdGraphNode_Comment> Creator(*Graph);
        UEdGraphNode_Comment* CommentNode = Creator.CreateNode(false);
        CommentNode->NodePosX = PosX;
        CommentNode->NodePosY = PosY;
        Creator.Finalize();
        CommentNode->NodeComment = Text;
        CommentNode->NodeWidth = static_cast<int32>(SizeX);
        CommentNode->NodeHeight = static_cast<int32>(SizeY);
        NewNode = CommentNode;
    }
    else if (NodeType == TEXT("MakeStruct") || NodeType == TEXT("BreakStruct"))
    {
        FString StructPath;
        if (!NodeJson->TryGetStringField(TEXT("struct_path"), StructPath))
        {
            OutError = TEXT("Struct node missing 'struct_path'.");
            return nullptr;
        }
        UScriptStruct* Struct = LoadExactObject<UScriptStruct>(StructPath);
        if (!Struct)
        {
            OutError = TEXT("struct_path must resolve to an exact UScriptStruct.");
            return nullptr;
        }
        if (NodeType == TEXT("MakeStruct"))
        {
            FGraphNodeCreator<UK2Node_MakeStruct> Creator(*Graph);
            UK2Node_MakeStruct* StructNode = Creator.CreateNode(false);
            StructNode->StructType = Struct;
            StructNode->NodePosX = PosX;
            StructNode->NodePosY = PosY;
            Creator.Finalize();
            NewNode = StructNode;
        }
        else
        {
            FGraphNodeCreator<UK2Node_BreakStruct> Creator(*Graph);
            UK2Node_BreakStruct* StructNode = Creator.CreateNode(false);
            StructNode->StructType = Struct;
            StructNode->NodePosX = PosX;
            StructNode->NodePosY = PosY;
            Creator.Finalize();
            NewNode = StructNode;
        }
    }
    else if (NodeType == TEXT("MacroInstance"))
    {
        FString MacroName;
        if (!NodeJson->TryGetStringField(TEXT("macro_name"), MacroName))
        {
            OutError = TEXT("MacroInstance node missing 'macro_name'.");
            return nullptr;
        }

        // Search for macro graph in the Blueprint and its parents
        UEdGraph* MacroGraph = nullptr;
        for (UBlueprint* SearchBP = Blueprint; SearchBP && !MacroGraph; SearchBP = Cast<UBlueprint>(SearchBP->ParentClass->ClassGeneratedBy))
        {
            for (UEdGraph* MGraph : SearchBP->MacroGraphs)
            {
                if (MGraph && MGraph->GetName() == MacroName)
                {
                    MacroGraph = MGraph;
                    break;
                }
            }
            if (!SearchBP->ParentClass || !SearchBP->ParentClass->ClassGeneratedBy)
                break;
        }

        // Also search engine-level macros (e.g., ForEachLoop)
        if (!MacroGraph)
        {
            UBlueprint* MacroLibBP = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
            if (MacroLibBP)
            {
                for (UEdGraph* MGraph : MacroLibBP->MacroGraphs)
                {
                    if (MGraph && MGraph->GetName() == MacroName)
                    {
                        MacroGraph = MGraph;
                        break;
                    }
                }
            }
        }

        if (!MacroGraph)
        {
            OutError = FString::Printf(TEXT("Macro '%s' not found."), *MacroName);
            return nullptr;
        }

        FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
        UK2Node_MacroInstance* MacroNode = Creator.CreateNode(false);
        MacroNode->SetMacroGraph(MacroGraph);
        MacroNode->NodePosX = PosX;
        MacroNode->NodePosY = PosY;
        Creator.Finalize();
        NewNode = MacroNode;
    }
    else if (NodeType == TEXT("InputKey"))
    {
        FString KeyName;
        if (!NodeJson->TryGetStringField(TEXT("key_name"), KeyName))
        {
            OutError = TEXT("InputKey node missing 'key_name'.");
            return nullptr;
        }
        FGraphNodeCreator<UK2Node_InputKey> Creator(*Graph);
        UK2Node_InputKey* KeyNode = Creator.CreateNode(false);
        KeyNode->InputKey = FKey(*KeyName);
        KeyNode->NodePosX = PosX;
        KeyNode->NodePosY = PosY;
        Creator.Finalize();
        NewNode = KeyNode;
    }
    else if (NodeType == TEXT("SpawnActor"))
    {
        FGraphNodeCreator<UK2Node_SpawnActorFromClass> Creator(*Graph);
        UK2Node_SpawnActorFromClass* SpawnNode = Creator.CreateNode(false);
        SpawnNode->NodePosX = PosX;
        SpawnNode->NodePosY = PosY;
        Creator.Finalize();
        NewNode = SpawnNode;
    }
    else
    {
        OutError = FString::Printf(TEXT("Unknown node type '%s'. Supported: CallFunction, Event, CustomEvent, CastTo, Branch, Sequence, VariableGet, VariableSet, Operator, Select, Switch, Reroute, Comment, MakeStruct, BreakStruct, MacroInstance, InputKey, SpawnActor."), *NodeType);
        return nullptr;
    }

    // Set pin defaults if specified
    if (NewNode && NodeJson->HasField(TEXT("pin_defaults")))
    {
        const TSharedPtr<FJsonObject>& PinDefaults = NodeJson->GetObjectField(TEXT("pin_defaults"));
        for (auto& Pair : PinDefaults->Values)
        {
            // *Pair.Key yields const TCHAR* on both UE 5.7 (FString key) and 5.8 (UE::FSharedString key).
            UEdGraphPin* Pin = FindPinByName(NewNode, FString(*Pair.Key), EGPD_Input);
            if (Pin)
            {
                FString Value;
                if (Pair.Value->TryGetString(Value))
                {
                    Pin->DefaultValue = Value;
                }
            }
        }
    }

    return NewNode;
}

FString UMCPythonHelper::AddReflectedBlueprintNode(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    using namespace UE::MCPython::Blueprint2;

    if (!Blueprint)
    {
        return GraphFailure(
            TEXT("asset_path"),
            TEXT("Blueprint is required."),
            TEXT("PRECONDITION_FAILED"));
    }

    TSharedPtr<FJsonObject> Request;
    const TSharedRef<TJsonReader<>> Reader =
        TJsonReaderFactory<>::Create(RequestJson);
    if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
    {
        return GraphFailure(
            TEXT("request"), TEXT("Request must be one valid JSON object."));
    }
    if (!HasOnlyFields(
            Request.ToSharedRef(),
            {TEXT("graph_id"), TEXT("member_kind"), TEXT("member_path"),
             TEXT("position")}))
    {
        return GraphFailure(
            TEXT("request"), TEXT("Request contains an unknown field."));
    }

    FString GraphId;
    FString MemberKind;
    FString MemberPath;
    if (!Request->TryGetStringField(TEXT("graph_id"), GraphId) ||
        GraphId.IsEmpty())
    {
        return GraphFailure(
            TEXT("graph_id"), TEXT("graph_id must be a stable graph ID."));
    }
    if (!Request->TryGetStringField(TEXT("member_kind"), MemberKind) ||
        MemberKind.IsEmpty())
    {
        return GraphFailure(
            TEXT("member_kind"), TEXT("member_kind is required."));
    }
    static const TSet<FString> MemberKinds = {
        TEXT("function"), TEXT("property_get"), TEXT("property_set"),
        TEXT("cast_to"), TEXT("enum_literal"), TEXT("make_struct"),
        TEXT("break_struct")};
    if (!MemberKinds.Contains(MemberKind))
    {
        return GraphFailure(
            TEXT("member_kind"),
            TEXT("member_kind must be one of function, property_get, property_set, cast_to, enum_literal, make_struct, or break_struct."));
    }
    if (!Request->TryGetStringField(TEXT("member_path"), MemberPath) ||
        MemberPath.IsEmpty())
    {
        return GraphFailure(
            TEXT("member_path"), TEXT("member_path is required."));
    }

    const TSharedPtr<FJsonObject>* PositionObject = nullptr;
    if (!Request->TryGetObjectField(TEXT("position"), PositionObject) ||
        !PositionObject || !PositionObject->IsValid() ||
        !HasOnlyFields(PositionObject->ToSharedRef(), {TEXT("x"), TEXT("y")}))
    {
        return GraphFailure(
            TEXT("position"), TEXT("position must contain only numeric x and y."));
    }
    double PositionX = 0.0;
    double PositionY = 0.0;
    if (!TryFiniteNumber(
            PositionObject->ToSharedRef(), TEXT("x"), PositionX) ||
        !TryFiniteNumber(
            PositionObject->ToSharedRef(), TEXT("y"), PositionY))
    {
        return GraphFailure(
            TEXT("position"), TEXT("position.x and position.y must be finite numbers."));
    }
    if (PositionX < MIN_int32 || PositionX > MAX_int32 ||
        PositionY < MIN_int32 || PositionY > MAX_int32)
    {
        return GraphFailure(
            TEXT("position"), TEXT("position must fit in Unreal graph coordinates."));
    }

    FTargetRef GraphTarget;
    GraphTarget.Id = GraphId;
    FString ResolveError;
    const FResolvedTarget Resolved = ResolveTarget(
        Blueprint, ETargetKind::Graph, GraphTarget, ResolveError);
    UEdGraph* Graph = Resolved.Graph;
    if (!Graph)
    {
        return GraphFailure(TEXT("graph_id"), ResolveError);
    }
    if (!IsExactK2Graph(Graph))
    {
        return GraphFailure(
            TEXT("graph_id"),
            TEXT("Reflected nodes require the exact UE 5.7 K2 graph schema."),
            TEXT("PRECONDITION_FAILED"));
    }

    UFunction* Function = nullptr;
    FProperty* Property = nullptr;
    UClass* TargetClass = nullptr;
    UEnum* TargetEnum = nullptr;
    UScriptStruct* TargetStruct = nullptr;
    UClass* MemberOwner = nullptr;
    FString MemberName;

    if (MemberKind == TEXT("function"))
    {
        if (!TryExactMemberPath(MemberPath, MemberOwner, MemberName))
        {
            return GraphFailure(
                TEXT("member_path"),
                TEXT("Function member_path must be an exact ClassPath:FunctionName path."));
        }
        Function = MemberOwner->FindFunctionByName(FName(*MemberName));
        if (!Function || Function->GetOuterUClass() != MemberOwner)
        {
            return GraphFailure(
                TEXT("member_path"), TEXT("Exact reflected function was not found."));
        }
        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        FText Reason;
        const UClass* ContextClass = Blueprint->SkeletonGeneratedClass
            ? Blueprint->SkeletonGeneratedClass.Get()
            : (Blueprint->GeneratedClass
                ? Blueprint->GeneratedClass.Get()
                : Blueprint->ParentClass.Get());
        const uint32 FunctionTypes =
            UEdGraphSchema_K2::FT_Pure |
            UEdGraphSchema_K2::FT_Imperative |
            UEdGraphSchema_K2::FT_Const |
            UEdGraphSchema_K2::FT_Protected;
        if (!UEdGraphSchema_K2::CanUserKismetCallFunction(Function) ||
            !Schema->CanFunctionBeUsedInGraph(
                ContextClass, Function, Graph, FunctionTypes, false, &Reason))
        {
            return GraphFailure(
                TEXT("member_path"),
                Reason.IsEmpty()
                    ? TEXT("Function cannot be called from this Blueprint graph.")
                    : Reason.ToString());
        }
        if (!CanCreateNodeClass(UK2Node_CallFunction::StaticClass(), Graph))
        {
            return GraphFailure(
                TEXT("member_kind"),
                TEXT("CallFunction nodes cannot be created under this schema."));
        }
    }
    else if (MemberKind == TEXT("property_get") ||
             MemberKind == TEXT("property_set"))
    {
        if (!TryExactMemberPath(MemberPath, MemberOwner, MemberName))
        {
            return GraphFailure(
                TEXT("member_path"),
                TEXT("Property member_path must be an exact ClassPath:PropertyName path."));
        }
        Property = FindFProperty<FProperty>(MemberOwner, FName(*MemberName));
        if (!Property || Property->GetOwnerClass() != MemberOwner)
        {
            return GraphFailure(
                TEXT("member_path"), TEXT("Exact reflected property was not found."));
        }
        if (!Property->HasAllPropertyFlags(CPF_BlueprintVisible))
        {
            return GraphFailure(
                TEXT("member_path"), TEXT("Property is not Blueprint-visible."));
        }
        if (MemberKind == TEXT("property_set") &&
            Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly | CPF_EditConst))
        {
            return GraphFailure(
                TEXT("member_path"), TEXT("Property is not writable from Blueprint."));
        }
        const UClass* NodeClass = MemberKind == TEXT("property_get")
            ? UK2Node_VariableGet::StaticClass()
            : UK2Node_VariableSet::StaticClass();
        if (!CanCreateNodeClass(NodeClass, Graph))
        {
            return GraphFailure(
                TEXT("member_kind"),
                TEXT("Property node cannot be created under this schema."));
        }
    }
    else if (MemberKind == TEXT("cast_to"))
    {
        TargetClass = LoadExactObject<UClass>(MemberPath);
        if (!TargetClass)
        {
            return GraphFailure(
                TEXT("member_path"), TEXT("cast_to member_path must resolve to an exact UClass."));
        }
        if (!CanCreateNodeClass(UK2Node_DynamicCast::StaticClass(), Graph))
        {
            return GraphFailure(
                TEXT("member_kind"),
                TEXT("DynamicCast nodes cannot be created under this schema."));
        }
    }
    else if (MemberKind == TEXT("enum_literal"))
    {
        TargetEnum = LoadExactObject<UEnum>(MemberPath);
        if (!TargetEnum)
        {
            return GraphFailure(
                TEXT("member_path"), TEXT("enum_literal member_path must resolve to an exact UEnum."));
        }
        if (!CanCreateNodeClass(UK2Node_EnumLiteral::StaticClass(), Graph))
        {
            return GraphFailure(
                TEXT("member_kind"),
                TEXT("EnumLiteral nodes cannot be created under this schema."));
        }
    }
    else
    {
        TargetStruct = LoadExactObject<UScriptStruct>(MemberPath);
        if (!TargetStruct)
        {
            return GraphFailure(
                TEXT("member_path"), TEXT("Struct member_path must resolve to an exact UScriptStruct."));
        }
        const UClass* NodeClass = MemberKind == TEXT("make_struct")
            ? UK2Node_MakeStruct::StaticClass()
            : UK2Node_BreakStruct::StaticClass();
        if (!CanCreateNodeClass(NodeClass, Graph))
        {
            return GraphFailure(
                TEXT("member_kind"),
                TEXT("Struct node cannot be created under this schema."));
        }
    }

    FMutationScope Scope(NSLOCTEXT(
        "MCPython", "AddReflectedBlueprintNode", "Add reflected Blueprint node"));
    if (!Scope.IsValid())
    {
        return GraphFailure(
            TEXT("transaction"),
            TEXT("Could not begin a Blueprint graph transaction."),
            TEXT("TRANSACTION_FAILED"));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);

    const int32 NodeX = static_cast<int32>(PositionX);
    const int32 NodeY = static_cast<int32>(PositionY);
    UEdGraphNode* NewNode = nullptr;
    if (Function)
    {
        FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
        UK2Node_CallFunction* Node = Creator.CreateNode(false);
        Node->SetFromFunction(Function);
        Node->NodePosX = NodeX;
        Node->NodePosY = NodeY;
        Creator.Finalize();
        NewNode = Node;
    }
    else if (Property && MemberKind == TEXT("property_get"))
    {
        FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
        UK2Node_VariableGet* Node = Creator.CreateNode(false);
        Node->VariableReference.SetFromField<FProperty>(Property, false);
        Node->NodePosX = NodeX;
        Node->NodePosY = NodeY;
        Creator.Finalize();
        NewNode = Node;
    }
    else if (Property)
    {
        FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
        UK2Node_VariableSet* Node = Creator.CreateNode(false);
        Node->VariableReference.SetFromField<FProperty>(Property, false);
        Node->NodePosX = NodeX;
        Node->NodePosY = NodeY;
        Creator.Finalize();
        NewNode = Node;
    }
    else if (TargetClass)
    {
        FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
        UK2Node_DynamicCast* Node = Creator.CreateNode(false);
        Node->TargetType = TargetClass;
        Node->NodePosX = NodeX;
        Node->NodePosY = NodeY;
        Creator.Finalize();
        NewNode = Node;
    }
    else if (TargetEnum)
    {
        FGraphNodeCreator<UK2Node_EnumLiteral> Creator(*Graph);
        UK2Node_EnumLiteral* Node = Creator.CreateNode(false);
        Node->Enum = TargetEnum;
        Node->NodePosX = NodeX;
        Node->NodePosY = NodeY;
        Creator.Finalize();
        NewNode = Node;
    }
    else if (MemberKind == TEXT("make_struct"))
    {
        FGraphNodeCreator<UK2Node_MakeStruct> Creator(*Graph);
        UK2Node_MakeStruct* Node = Creator.CreateNode(false);
        Node->StructType = TargetStruct;
        Node->NodePosX = NodeX;
        Node->NodePosY = NodeY;
        Creator.Finalize();
        NewNode = Node;
    }
    else if (MemberKind == TEXT("break_struct"))
    {
        FGraphNodeCreator<UK2Node_BreakStruct> Creator(*Graph);
        UK2Node_BreakStruct* Node = Creator.CreateNode(false);
        Node->StructType = TargetStruct;
        Node->NodePosX = NodeX;
        Node->NodePosY = NodeY;
        Creator.Finalize();
        NewNode = Node;
    }

    if (!NewNode)
    {
        return RollbackGraphFailure(
            Scope, TEXT("member_kind"), TEXT("Reflected node creation failed."));
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    return NodeAuthoringSuccess(
        Blueprint,
        Graph,
        NewNode,
        TEXT("Reflected Blueprint node added."),
        false);
}

// ─── AddBlueprintNode UFUNCTION ──────────────────────────────────────────────

FString UMCPythonHelper::AddBlueprintNode(UBlueprint* Blueprint, const FString& GraphName, const FString& NodeJson)
{
    using namespace UE::MCPython::Blueprint2;

    if (!Blueprint)
        return MakeJsonError(TEXT("Invalid Blueprint."));

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
        return MakeJsonError(FString::Printf(TEXT("Graph '%s' not found."), *GraphName));

    TSharedPtr<FJsonObject> JsonObj;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(NodeJson);
    if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
        return MakeJsonError(TEXT("Failed to parse NodeJson."));

    FMutationScope Scope(NSLOCTEXT(
        "MCPython", "AddBlueprintNode", "Add Blueprint graph node"));
    if (!Scope.IsValid())
        return MakeJsonError(TEXT("Could not begin a Blueprint graph transaction."));
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);

    FString Error;
    UEdGraphNode* NewNode = CreateBPNodeFromJson(Graph, Blueprint, JsonObj, Error);
    if (!NewNode)
    {
        const FRollbackResult Rollback = Scope.Rollback();
        if (!Rollback.bSucceeded && !Rollback.bDeferredToWorkflow)
        {
            return GraphFailure(
                TEXT("transaction"),
                TEXT("Blueprint graph mutation rollback failed."),
                TEXT("ROLLBACK_FAILED"));
        }
        return MakeJsonError(Error);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    return NodeAuthoringSuccess(
        Blueprint,
        Graph,
        NewNode,
        FString::Printf(
            TEXT("Node '%s' added to graph '%s'."),
            *NewNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString(),
            *GraphName),
        true);
}

// ─── SetBlueprintNodeProperties UFUNCTION ────────────────────────────────────

FString UMCPythonHelper::SetBlueprintNodeProperties(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    using namespace UE::MCPython::Blueprint2;

    if (!Blueprint)
    {
        return GraphFailure(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    TSharedPtr<FJsonObject> Request;
    const TSharedRef<TJsonReader<>> Reader =
        TJsonReaderFactory<>::Create(RequestJson);
    if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
    {
        return GraphFailure(TEXT("params"), TEXT("Request must be a JSON object."));
    }
    if (!HasOnlyFields(Request.ToSharedRef(), {TEXT("node_id"), TEXT("properties")}))
    {
        return GraphFailure(
            TEXT("params"), TEXT("Only node_id and properties are accepted."));
    }

    FString NodeId;
    const TSharedPtr<FJsonObject>* PropertiesPointer = nullptr;
    if (!Request->TryGetStringField(TEXT("node_id"), NodeId) ||
        NodeId.IsEmpty())
    {
        return GraphFailure(
            TEXT("params.node_id"), TEXT("A stable node_id is required."));
    }
    if (!Request->TryGetObjectField(
            TEXT("properties"), PropertiesPointer) ||
        !PropertiesPointer || !PropertiesPointer->IsValid() ||
        (*PropertiesPointer)->Values.IsEmpty())
    {
        return GraphFailure(
            TEXT("params.properties"),
            TEXT("properties must be a non-empty object."));
    }
    const TSharedRef<FJsonObject> Properties =
        PropertiesPointer->ToSharedRef();

    FTargetRef NodeTarget;
    NodeTarget.Id = NodeId;
    FString ResolveError;
    const FResolvedTarget Resolved = ResolveTarget(
        Blueprint, ETargetKind::Node, NodeTarget, ResolveError);
    UEdGraphNode* Node = Resolved.Node;
    UEdGraph* Graph = Resolved.Graph;
    if (!Node || !Graph)
    {
        return GraphFailure(
            TEXT("params.node_id"),
            FString::Printf(TEXT("Node target could not be resolved: %s"), *ResolveError));
    }
    if (!IsExactK2Graph(Graph))
    {
        return GraphFailure(
            TEXT("params.node_id"), TEXT("Node must belong to an exact K2 graph."));
    }
    const UEdGraphSchema_K2* Schema =
        CastChecked<UEdGraphSchema_K2>(Graph->GetSchema());

    // The concrete class determines the allowlist before any class-specific
    // value is read or validated.
    const TArray<FString> Allowed = AllowedNodeProperties(Node);
    for (const auto& Property : Properties->Values)
    {
        if (!Allowed.Contains(Property.Key))
        {
            return GraphFailureWithDetails(
                TEXT("params.properties.") + Property.Key,
                FString::Printf(
                    TEXT("Property '%s' is not writable for node class '%s'."),
                    *Property.Key,
                    *Node->GetClass()->GetPathName()),
                AllowedPropertiesDetails(Allowed));
        }
    }

    FNodePropertyPlan Plan;
    if (Properties->HasField(TEXT("comment")))
    {
        if (!Properties->TryGetStringField(TEXT("comment"), Plan.Comment))
        {
            return GraphFailure(
                TEXT("params.properties.comment"), TEXT("comment must be a string."));
        }
        Plan.bSetComment = true;
    }
    if (Properties->HasField(TEXT("comment_bubble_visible")))
    {
        if (!Properties->TryGetBoolField(
                TEXT("comment_bubble_visible"), Plan.bCommentBubbleVisible))
        {
            return GraphFailure(
                TEXT("params.properties.comment_bubble_visible"),
                TEXT("comment_bubble_visible must be a boolean."));
        }
        Plan.bSetCommentBubbleVisible = true;
    }
    if (Properties->HasField(TEXT("enabled_state")))
    {
        FString State;
        if (!Properties->TryGetStringField(TEXT("enabled_state"), State) ||
            !ParseEnabledState(State, Plan.EnabledState))
        {
            return GraphFailure(
                TEXT("params.properties.enabled_state"),
                TEXT("enabled_state must be enabled, disabled, or development_only."));
        }
        Plan.bSetEnabledState = true;
    }
    if (Properties->HasField(TEXT("position")))
    {
        const TSharedPtr<FJsonObject>* PositionPointer = nullptr;
        if (!Properties->TryGetObjectField(
                TEXT("position"), PositionPointer) ||
            !PositionPointer || !PositionPointer->IsValid() ||
            !HasOnlyFields(PositionPointer->ToSharedRef(), {TEXT("x"), TEXT("y")}) ||
            !TryIntegerInRange(
                PositionPointer->ToSharedRef(), TEXT("x"),
                MIN_int32, MAX_int32, Plan.PositionX) ||
            !TryIntegerInRange(
                PositionPointer->ToSharedRef(), TEXT("y"),
                MIN_int32, MAX_int32, Plan.PositionY))
        {
            return GraphFailure(
                TEXT("params.properties.position"),
                TEXT("position must contain only finite int32 x and y values."));
        }
        Plan.bSetPosition = true;
    }
    if (Properties->HasField(TEXT("output_count")))
    {
        if (!TryIntegerInRange(
                Properties, TEXT("output_count"), 2, 64, Plan.OutputCount))
        {
            return GraphFailure(
                TEXT("params.properties.output_count"),
                TEXT("output_count must be an integer from 2 to 64."));
        }
        Plan.bSetOutputCount = true;
    }
    if (Properties->HasField(TEXT("option_count")))
    {
        if (!TryIntegerInRange(
                Properties, TEXT("option_count"), 2, 64, Plan.OptionCount))
        {
            return GraphFailure(
                TEXT("params.properties.option_count"),
                TEXT("option_count must be an integer from 2 to 64."));
        }
        Plan.bSetOptionCount = true;
        UK2Node_Select* SelectNode = CastChecked<UK2Node_Select>(Node);
        TArray<UEdGraphPin*> Options;
        SelectNode->GetOptionPins(Options);
        if (Plan.OptionCount > Options.Num() && !SelectNode->CanAddPin())
        {
            return GraphFailure(
                TEXT("params.properties.option_count"),
                TEXT("option_count cannot be increased for this fixed-shape Select node."));
        }
        if (Plan.OptionCount < Options.Num() &&
            !SelectNode->CanRemoveOptionPinToNode())
        {
            return GraphFailure(
                TEXT("params.properties.option_count"),
                TEXT("option_count cannot be decreased for this fixed-shape Select node."));
        }
    }
    if (Properties->HasField(TEXT("cases")))
    {
        FString CasesError;
        if (!ParseSwitchCases(Properties, Node, Plan, CasesError))
        {
            return GraphFailure(
                TEXT("params.properties.cases"), CasesError);
        }
    }

    if (Properties->HasField(TEXT("pin_defaults")))
    {
        const TSharedPtr<FJsonObject>* DefaultsPointer = nullptr;
        if (!Properties->TryGetObjectField(
                TEXT("pin_defaults"), DefaultsPointer) ||
            !DefaultsPointer || !DefaultsPointer->IsValid() ||
            (*DefaultsPointer)->Values.IsEmpty())
        {
            return GraphFailure(
                TEXT("params.properties.pin_defaults"),
                TEXT("pin_defaults must be a non-empty object keyed by stable pin ID."));
        }
        for (const auto& Default : (*DefaultsPointer)->Values)
        {
            FResolvedTarget PinTarget;
            FString PinError;
            if (!ResolveStablePin(Blueprint, Default.Key, PinTarget, PinError))
            {
                return GraphFailure(
                    TEXT("params.properties.pin_defaults.") + Default.Key,
                    FString::Printf(
                        TEXT("Pin target could not be resolved: %s"), *PinError));
            }
            if (PinTarget.Node != Node || PinTarget.Graph != Graph)
            {
                return GraphFailure(
                    TEXT("params.properties.pin_defaults.") + Default.Key,
                    TEXT("Pin target does not belong to the requested node."));
            }
            if (PinTarget.Pin->Direction != EGPD_Input ||
                PinTarget.Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                return GraphFailure(
                    TEXT("params.properties.pin_defaults.") + Default.Key,
                    TEXT("Only non-exec input pin defaults are writable."));
            }
            if (PinTarget.Pin->bDefaultValueIsReadOnly ||
                PinTarget.Pin->bDefaultValueIsIgnored)
            {
                return GraphFailure(
                    TEXT("params.properties.pin_defaults.") + Default.Key,
                    TEXT("Pin default is read-only or ignored by the node."));
            }
            if (Plan.bSetOptionCount)
            {
                if (UK2Node_Select* SelectNode = Cast<UK2Node_Select>(Node))
                {
                    TArray<UEdGraphPin*> Options;
                    SelectNode->GetOptionPins(Options);
                    const int32 OptionIndex = Options.IndexOfByKey(PinTarget.Pin);
                    if (OptionIndex != INDEX_NONE && OptionIndex >= Plan.OptionCount)
                    {
                        return GraphFailure(
                            TEXT("params.properties.pin_defaults.") + Default.Key,
                            TEXT("Pin target would be removed by option_count."));
                    }
                }
            }
            FPreparedPinDefault Prepared;
            Prepared.PinId = Default.Key;
            FError DefaultError;
            if (!NormalizeDefaultValue(
                    PinTarget.Pin->PinType,
                    Default.Value,
                    Node,
                    Prepared.Value,
                    DefaultError,
                    TEXT("params.properties.pin_defaults.") + Default.Key))
            {
                return SerializeResult(MakeFailure(
                    DefaultError.Code.IsEmpty()
                        ? FString(TEXT("INVALID_INPUT"))
                        : DefaultError.Code,
                    DefaultError.Path,
                    DefaultError.Message,
                    false,
                    TEXT("Provide a value compatible with the canonical pin type.")));
            }
            const FString PinDefaultError = Schema->IsPinDefaultValid(
                PinTarget.Pin,
                Prepared.Value.DefaultValue,
                Prepared.Value.DefaultObject,
                Prepared.Value.DefaultTextValue);
            if (!PinDefaultError.IsEmpty())
            {
                return GraphFailure(
                    TEXT("params.properties.pin_defaults.") + Default.Key,
                    FString::Printf(
                        TEXT("Pin default is not writable: %s"),
                        *PinDefaultError));
            }
            Plan.PinDefaults.Add(MoveTemp(Prepared));
        }
    }

    FMutationScope Scope(NSLOCTEXT(
        "MCPython", "SetBlueprintNodeProperties", "Set Blueprint node properties"));
    if (!Scope.IsValid())
    {
        return GraphFailure(
            TEXT("transaction"),
            TEXT("Could not begin a Blueprint graph transaction."),
            TEXT("TRANSACTION_FAILED"));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    Scope.Modify(Node);
    for (UEdGraphPin* Pin : Node->Pins)
    {
        ModifyPinAndLinks(Scope, Pin);
    }
    bool bChangedPinShape = false;
    if (Plan.bSetOutputCount)
    {
        UK2Node_ExecutionSequence* Sequence =
            CastChecked<UK2Node_ExecutionSequence>(Node);
        auto OutputPins = [Sequence]()
        {
            TArray<UEdGraphPin*> Result;
            for (UEdGraphPin* Pin : Sequence->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Output &&
                    Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    Result.Add(Pin);
                }
            }
            return Result;
        };
        TArray<UEdGraphPin*> Outputs = OutputPins();
        while (Outputs.Num() < Plan.OutputCount)
        {
            const int32 PreviousCount = Outputs.Num();
            Sequence->AddInputPin();
            Outputs = OutputPins();
            if (Outputs.Num() <= PreviousCount)
            {
                return RollbackGraphFailure(
                    Scope,
                    TEXT("params.properties.output_count"),
                    TEXT("Sequence node did not add an output pin."));
            }
        }
        while (Outputs.Num() > Plan.OutputCount)
        {
            const int32 PreviousCount = Outputs.Num();
            Schema->BreakPinLinks(*Outputs.Last(), true);
            Sequence->RemovePinFromExecutionNode(Outputs.Last());
            Outputs = OutputPins();
            if (Outputs.Num() >= PreviousCount)
            {
                return RollbackGraphFailure(
                    Scope,
                    TEXT("params.properties.output_count"),
                    TEXT("Sequence node did not remove an output pin."));
            }
        }
        bChangedPinShape = true;
    }
    if (Plan.bSetOptionCount)
    {
        UK2Node_Select* SelectNode = CastChecked<UK2Node_Select>(Node);
        TArray<UEdGraphPin*> Options;
        SelectNode->GetOptionPins(Options);
        while (Options.Num() < Plan.OptionCount)
        {
            if (!SelectNode->CanAddPin())
            {
                return RollbackGraphFailure(
                    Scope,
                    TEXT("params.properties.option_count"),
                    TEXT("Select node can no longer add an option pin."));
            }
            const int32 PreviousCount = Options.Num();
            SelectNode->AddInputPin();
            SelectNode->GetOptionPins(Options);
            if (Options.Num() <= PreviousCount)
            {
                return RollbackGraphFailure(
                    Scope,
                    TEXT("params.properties.option_count"),
                    TEXT("Select node did not add an option pin."));
            }
        }
        while (Options.Num() > Plan.OptionCount)
        {
            if (!SelectNode->CanRemoveOptionPinToNode())
            {
                return RollbackGraphFailure(
                    Scope,
                    TEXT("params.properties.option_count"),
                    TEXT("Select node can no longer remove an option pin."));
            }
            const int32 PreviousCount = Options.Num();
            SelectNode->RemoveOptionPinToNode();
            SelectNode->GetOptionPins(Options);
            if (Options.Num() >= PreviousCount)
            {
                return RollbackGraphFailure(
                    Scope,
                    TEXT("params.properties.option_count"),
                    TEXT("Select node did not remove an option pin."));
            }
        }
        bChangedPinShape = true;
    }
    if (Plan.bSetCases)
    {
        if (UK2Node_SwitchInteger* Switch = Cast<UK2Node_SwitchInteger>(Node))
        {
            if (!Plan.IntegerCases.IsEmpty() &&
                Switch->StartIndex != Plan.IntegerCases[0])
            {
                Switch->StartIndex = Plan.IntegerCases[0];
                Switch->ReconstructNode();
            }
            auto CasePins = [Switch]()
            {
                TArray<UEdGraphPin*> Result;
                UEdGraphPin* DefaultPin = Switch->GetDefaultPin();
                for (UEdGraphPin* Pin : Switch->Pins)
                {
                    if (Pin && Pin != DefaultPin &&
                        Pin->Direction == EGPD_Output &&
                        Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                    {
                        Result.Add(Pin);
                    }
                }
                return Result;
            };
            TArray<UEdGraphPin*> Current = CasePins();
            while (Current.Num() < Plan.IntegerCases.Num())
            {
                const int32 PreviousCount = Current.Num();
                Switch->AddPinToSwitchNode();
                Current = CasePins();
                if (Current.Num() <= PreviousCount)
                {
                    return RollbackGraphFailure(
                        Scope,
                        TEXT("params.properties.cases"),
                        TEXT("Integer Switch did not add a case pin."));
                }
            }
            while (Current.Num() > Plan.IntegerCases.Num())
            {
                const int32 PreviousCount = Current.Num();
                Schema->BreakPinLinks(*Current.Last(), true);
                Switch->RemovePinFromSwitchNode(Current.Last());
                Current = CasePins();
                if (Current.Num() >= PreviousCount)
                {
                    return RollbackGraphFailure(
                        Scope,
                        TEXT("params.properties.cases"),
                        TEXT("Integer Switch did not remove a case pin."));
                }
            }
        }
        else if (UK2Node_SwitchString* StringSwitch =
            Cast<UK2Node_SwitchString>(Node))
        {
            StringSwitch->PinNames = Plan.NamedCases;
            StringSwitch->ReconstructNode();
        }
        else if (UK2Node_SwitchName* NameSwitch =
            Cast<UK2Node_SwitchName>(Node))
        {
            NameSwitch->PinNames = Plan.NamedCases;
            NameSwitch->ReconstructNode();
        }
        bChangedPinShape = true;
    }

    if (bChangedPinShape)
    {
        FString ReResolveError;
        const FResolvedTarget ReResolved = ResolveTarget(
            Blueprint, ETargetKind::Node, NodeTarget, ReResolveError);
        if (!ReResolved.Node || ReResolved.Graph != Graph)
        {
            return RollbackGraphFailure(
                Scope,
                TEXT("params.node_id"),
                FString::Printf(
                    TEXT("Node could not be re-resolved after reconstruction: %s"),
                    *ReResolveError));
        }
        Node = ReResolved.Node;
    }

    if (Plan.bSetComment)
    {
        Node->NodeComment = Plan.Comment;
    }
#if WITH_EDITORONLY_DATA
    if (Plan.bSetCommentBubbleVisible)
    {
        Node->bCommentBubbleVisible = Plan.bCommentBubbleVisible;
    }
#endif
    if (Plan.bSetEnabledState)
    {
        Node->SetEnabledState(Plan.EnabledState);
    }
    if (Plan.bSetPosition)
    {
        Node->NodePosX = Plan.PositionX;
        Node->NodePosY = Plan.PositionY;
    }

    for (const FPreparedPinDefault& Prepared : Plan.PinDefaults)
    {
        FResolvedTarget PinTarget;
        FString PinError;
        if (!ResolveStablePin(Blueprint, Prepared.PinId, PinTarget, PinError) ||
            PinTarget.Node != Node || PinTarget.Graph != Graph)
        {
            return RollbackGraphFailure(
                Scope,
                TEXT("params.properties.pin_defaults.") + Prepared.PinId,
                FString::Printf(
                    TEXT("Pin could not be re-resolved after reconstruction: %s"),
                    *PinError));
        }
        UEdGraphPin* Pin = PinTarget.Pin;
        if (Pin->PinType.IsContainer())
        {
            Schema->TrySetDefaultValue(*Pin, Prepared.Value.DefaultValue);
        }
        else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
            Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
            Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Interface)
        {
            Schema->TrySetDefaultObject(*Pin, Prepared.Value.DefaultObject);
        }
        else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
        {
            Schema->TrySetDefaultText(*Pin, Prepared.Value.DefaultTextValue);
        }
        else
        {
            Schema->TrySetDefaultValue(*Pin, Prepared.Value.DefaultValue);
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    const FTargetRef GraphTarget = DescribeGraphTarget(Blueprint, Graph);
    const FTargetRef CurrentNodeTarget = DescribeNodeTarget(Blueprint, Node);
    const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
    Position->SetNumberField(TEXT("x"), Node->NodePosX);
    Position->SetNumberField(TEXT("y"), Node->NodePosY);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("graph_id"), GraphTarget.Id);
    Data->SetStringField(TEXT("node_id"), CurrentNodeTarget.Id);
    Data->SetStringField(TEXT("class_path"), Node->GetClass()->GetPathName());
    Data->SetObjectField(TEXT("position"), Position);
    Data->SetArrayField(TEXT("pin_ids"), NodePinIds(Blueprint, Node));
    Data->SetObjectField(TEXT("properties"), Properties);

    const TSharedRef<FJsonObject> ChangeDetails = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> PropertyNames;
    for (const auto& Property : Properties->Values)
    {
        PropertyNames.Add(MakeShared<FJsonValueString>(Property.Key));
    }
    PropertyNames.Sort([](
        const TSharedPtr<FJsonValue>& A,
        const TSharedPtr<FJsonValue>& B)
    {
        return A->AsString() < B->AsString();
    });
    ChangeDetails->SetArrayField(TEXT("properties"), PropertyNames);
    const TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
    Change->SetStringField(TEXT("kind"), TEXT("update"));
    Change->SetStringField(TEXT("target_id"), CurrentNodeTarget.Id);
    Change->SetObjectField(TEXT("details"), ChangeDetails);

    const TSharedRef<FJsonObject> Result = MakeSuccess(
        TEXT("Blueprint node properties updated."), Data);
    Result->SetArrayField(
        TEXT("changes"), {MakeShared<FJsonValueObject>(Change)});
    Result->SetArrayField(
        TEXT("next_actions"),
        {MakeShared<FJsonValueObject>(CompileNextAction(Blueprint))});
    return SerializeResult(Result);
}

// ─── ConnectBlueprintPins UFUNCTION ──────────────────────────────────────────

FString UMCPythonHelper::ConnectBlueprintPins(UBlueprint* Blueprint, const FString& GraphName,
    const FString& SourceNodeName, const FString& SourcePinName,
    const FString& TargetNodeName, const FString& TargetPinName)
{
    using namespace UE::MCPython::Blueprint2;

    if (!Blueprint)
    {
        return LegacyConnectFailure(
            TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
    {
        return LegacyConnectFailure(
            TEXT("graph_name"),
            FString::Printf(TEXT("Graph '%s' not found."), *GraphName));
    }
    if (!IsExactK2Graph(Graph))
    {
        return LegacyConnectFailure(
            TEXT("graph_name"), TEXT("Graph must use the exact K2 schema."));
    }

    FString ResolveError;
    UEdGraphNode* SourceNode = ResolveConnectionNode(
        Blueprint, Graph, SourceNodeName, ResolveError);
    if (!SourceNode)
    {
        return LegacyConnectFailure(
            TEXT("source_node"),
            FString::Printf(TEXT("Source node could not be resolved: %s"), *ResolveError));
    }
    UEdGraphNode* TargetNode = ResolveConnectionNode(
        Blueprint, Graph, TargetNodeName, ResolveError);
    if (!TargetNode)
    {
        return LegacyConnectFailure(
            TEXT("target_node"),
            FString::Printf(TEXT("Target node could not be resolved: %s"), *ResolveError));
    }
    UEdGraphPin* SourcePin = ResolveConnectionPin(
        Blueprint, Graph, SourceNode, SourcePinName, ResolveError);
    if (!SourcePin)
    {
        return LegacyConnectFailure(
            TEXT("source_pin"),
            FString::Printf(TEXT("Source pin could not be resolved: %s"), *ResolveError));
    }
    UEdGraphPin* TargetPin = ResolveConnectionPin(
        Blueprint, Graph, TargetNode, TargetPinName, ResolveError);
    if (!TargetPin)
    {
        return LegacyConnectFailure(
            TEXT("target_pin"),
            FString::Printf(TEXT("Target pin could not be resolved: %s"), *ResolveError));
    }
    if (SourcePin == TargetPin || SourcePin->Direction == TargetPin->Direction)
    {
        return LegacyConnectFailureWithDetails(
            TEXT("source_pin"),
            TEXT("Connection direction requires exactly one input pin and one output pin."),
            ConnectionDetails(Blueprint, SourcePin, TargetPin));
    }

    UEdGraphPin* OutputPin = SourcePin->Direction == EGPD_Output
        ? SourcePin
        : TargetPin;
    UEdGraphPin* InputPin = SourcePin->Direction == EGPD_Input
        ? SourcePin
        : TargetPin;
    const UEdGraphSchema_K2* Schema =
        CastChecked<UEdGraphSchema_K2>(Graph->GetSchema());
    const FPinConnectionResponse Response =
        Schema->CanCreateConnection(OutputPin, InputPin);
    const bool bAllowed =
        Response.Response == CONNECT_RESPONSE_MAKE ||
        Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A ||
        Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B ||
        Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB ||
        Response.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE ||
        Response.Response == CONNECT_RESPONSE_MAKE_WITH_PROMOTION;
    if (!bAllowed)
    {
        return LegacyConnectFailureWithDetails(
            TEXT("source_pin"),
            FString::Printf(
                TEXT("Connection not allowed: %s"), *Response.Message.ToString()),
            ConnectionDetails(Blueprint, OutputPin, InputPin, &Response));
    }

    TSet<FGuid> BeforeNodeGuids;
    for (UEdGraphNode* Existing : Graph->Nodes)
    {
        if (Existing && Existing->NodeGuid.IsValid())
        {
            BeforeNodeGuids.Add(Existing->NodeGuid);
        }
    }
    const TMap<FString, FNormalizedConnection> BeforeConnections =
        SnapshotConnections(Blueprint, Graph);

    FMutationScope Scope(NSLOCTEXT(
        "MCPython", "ConnectBlueprintPins", "Connect Blueprint pins"));
    if (!Scope.IsValid())
    {
        return LegacyConnectFailure(
            TEXT("transaction"),
            TEXT("Could not begin a Blueprint graph transaction."),
            TEXT("TRANSACTION_FAILED"));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    ModifyPinAndLinks(Scope, OutputPin);
    ModifyPinAndLinks(Scope, InputPin);
    if (!Schema->TryCreateConnection(OutputPin, InputPin))
    {
        return RollbackLegacyConnectFailure(
            Scope,
            TEXT("source_pin"),
            TEXT("K2 schema failed to create the preflight-approved connection."));
    }

    TArray<FString> InsertedNodeIds;
    for (UEdGraphNode* Current : Graph->Nodes)
    {
        if (!Current)
        {
            continue;
        }
        if (Current->NodeGuid.IsValid() &&
            !BeforeNodeGuids.Contains(Current->NodeGuid))
        {
            InsertedNodeIds.Add(DescribeNodeTarget(Blueprint, Current).Id);
        }
    }
    InsertedNodeIds.Sort();
    TArray<TSharedPtr<FJsonValue>> InsertedValues;
    for (const FString& InsertedNodeId : InsertedNodeIds)
    {
        InsertedValues.Add(MakeShared<FJsonValueString>(InsertedNodeId));
    }

    const FNormalizedConnection Connection =
        NormalizeConnection(Blueprint, OutputPin, InputPin);
    const TMap<FString, FNormalizedConnection> AfterConnections =
        SnapshotConnections(Blueprint, Graph);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(
        TEXT("graph_id"), DescribeGraphTarget(Blueprint, Graph).Id);
    Data->SetStringField(TEXT("source_pin_id"), Connection.SourcePinId);
    Data->SetStringField(TEXT("target_pin_id"), Connection.TargetPinId);
    Data->SetStringField(
        TEXT("schema_response"), ConnectionResponseName(Response.Response));
    Data->SetArrayField(TEXT("inserted_conversion_node_ids"), InsertedValues);

    const FString Message = FString::Printf(
        TEXT("Connected %s.%s -> %s.%s"),
        *SourceNodeName,
        *SourcePinName,
        *TargetNodeName,
        *TargetPinName);
    const TSharedRef<FJsonObject> Result = MakeSuccess(Message, Data);
    Result->SetArrayField(
        TEXT("changes"),
        DiffConnectionChanges(BeforeConnections, AfterConnections));
    Result->SetArrayField(
        TEXT("next_actions"),
        {MakeShared<FJsonValueObject>(CompileNextAction(Blueprint))});
    Result->SetStringField(TEXT("message"), Message);
    Result->SetStringField(TEXT("source_node"), SourceNodeName);
    Result->SetStringField(TEXT("source_pin"), SourcePinName);
    Result->SetStringField(TEXT("target_node"), TargetNodeName);
    Result->SetStringField(TEXT("target_pin"), TargetPinName);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    return SerializeResult(Result);
}

// ─── RemoveBlueprintNode UFUNCTION ───────────────────────────────────────────

FString UMCPythonHelper::DisconnectBlueprintPins(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    using namespace UE::MCPython::Blueprint2;

    if (!Blueprint)
    {
        return GraphFailure(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    TSharedPtr<FJsonObject> Request;
    const TSharedRef<TJsonReader<>> Reader =
        TJsonReaderFactory<>::Create(RequestJson);
    if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
    {
        return GraphFailure(TEXT("params"), TEXT("Request must be a JSON object."));
    }
    if (!HasOnlyFields(
            Request.ToSharedRef(),
            {TEXT("pin_id"), TEXT("source_pin_id"), TEXT("target_pin_id")}))
    {
        return GraphFailure(
            TEXT("params"),
            TEXT("Only pin_id or source_pin_id plus target_pin_id are accepted."));
    }
    FString PinId;
    FString SourcePinId;
    FString TargetPinId;
    if (Request->HasField(TEXT("pin_id")) &&
        !Request->TryGetStringField(TEXT("pin_id"), PinId))
    {
        return GraphFailure(
            TEXT("params.pin_id"), TEXT("pin_id must be a string."));
    }
    if (Request->HasField(TEXT("source_pin_id")) &&
        !Request->TryGetStringField(TEXT("source_pin_id"), SourcePinId))
    {
        return GraphFailure(
            TEXT("params.source_pin_id"),
            TEXT("source_pin_id must be a string."));
    }
    if (Request->HasField(TEXT("target_pin_id")) &&
        !Request->TryGetStringField(TEXT("target_pin_id"), TargetPinId))
    {
        return GraphFailure(
            TEXT("params.target_pin_id"),
            TEXT("target_pin_id must be a string."));
    }
    const bool bBreakAll = !PinId.IsEmpty() &&
        SourcePinId.IsEmpty() && TargetPinId.IsEmpty();
    const bool bBreakPair = PinId.IsEmpty() &&
        !SourcePinId.IsEmpty() && !TargetPinId.IsEmpty();
    if (bBreakAll == bBreakPair)
    {
        return GraphFailure(
            TEXT("params"),
            TEXT("Provide either pin_id alone or both source_pin_id and target_pin_id."));
    }

    FResolvedTarget FirstTarget;
    FResolvedTarget SecondTarget;
    FString ResolveError;
    if (!ResolveStablePin(
            Blueprint,
            bBreakAll ? PinId : SourcePinId,
            FirstTarget,
            ResolveError))
    {
        return GraphFailure(
            bBreakAll ? TEXT("params.pin_id") : TEXT("params.source_pin_id"),
            FString::Printf(TEXT("Pin target could not be resolved: %s"), *ResolveError));
    }
    UEdGraph* Graph = FirstTarget.Graph;
    if (!IsExactK2Graph(Graph))
    {
        return GraphFailure(
            bBreakAll ? TEXT("params.pin_id") : TEXT("params.source_pin_id"),
            TEXT("Pin must belong to an exact K2 graph."));
    }
    if (bBreakPair)
    {
        if (!ResolveStablePin(Blueprint, TargetPinId, SecondTarget, ResolveError))
        {
            return GraphFailure(
                TEXT("params.target_pin_id"),
                FString::Printf(
                    TEXT("Pin target could not be resolved: %s"), *ResolveError));
        }
        if (SecondTarget.Graph != Graph)
        {
            return GraphFailure(
                TEXT("params.target_pin_id"),
                TEXT("Exact pin pair must belong to the same graph."));
        }
        if (FirstTarget.Pin == SecondTarget.Pin ||
            FirstTarget.Pin->Direction == SecondTarget.Pin->Direction)
        {
            return GraphFailureWithDetails(
                TEXT("params.source_pin_id"),
                TEXT("Exact pin pair requires one input pin and one output pin."),
                ConnectionDetails(
                    Blueprint, FirstTarget.Pin, SecondTarget.Pin));
        }
        if (!FirstTarget.Pin->LinkedTo.Contains(SecondTarget.Pin))
        {
            UEdGraphPin* OutputPin = FirstTarget.Pin->Direction == EGPD_Output
                ? FirstTarget.Pin
                : SecondTarget.Pin;
            UEdGraphPin* InputPin = FirstTarget.Pin->Direction == EGPD_Input
                ? FirstTarget.Pin
                : SecondTarget.Pin;
            return GraphFailureWithDetails(
                TEXT("params.target_pin_id"),
                TEXT("The exact pin pair is not linked."),
                ConnectionDetails(Blueprint, OutputPin, InputPin),
                TEXT("PRECONDITION_FAILED"));
        }
    }

    TArray<FNormalizedConnection> Removed;
    if (bBreakAll)
    {
        for (UEdGraphPin* Linked : FirstTarget.Pin->LinkedTo)
        {
            if (Linked)
            {
                Removed.Add(NormalizeConnection(
                    Blueprint, FirstTarget.Pin, Linked));
            }
        }
    }
    else
    {
        Removed.Add(NormalizeConnection(
            Blueprint, FirstTarget.Pin, SecondTarget.Pin));
    }
    Removed.Sort([](
        const FNormalizedConnection& A,
        const FNormalizedConnection& B)
    {
        return ConnectionTargetId(A) < ConnectionTargetId(B);
    });

    FMutationScope Scope(NSLOCTEXT(
        "MCPython", "DisconnectBlueprintPins", "Disconnect Blueprint pins"));
    if (!Scope.IsValid())
    {
        return GraphFailure(
            TEXT("transaction"),
            TEXT("Could not begin a Blueprint graph transaction."),
            TEXT("TRANSACTION_FAILED"));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    ModifyPinAndLinks(Scope, FirstTarget.Pin);
    if (bBreakPair)
    {
        ModifyPinAndLinks(Scope, SecondTarget.Pin);
    }
    const UEdGraphSchema_K2* Schema =
        CastChecked<UEdGraphSchema_K2>(Graph->GetSchema());
    if (bBreakAll)
    {
        Schema->BreakPinLinks(*FirstTarget.Pin, true);
    }
    else
    {
        Schema->BreakSinglePinLink(FirstTarget.Pin, SecondTarget.Pin);
    }
    if (!Removed.IsEmpty())
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }

    TArray<TSharedPtr<FJsonValue>> Changes;
    for (const FNormalizedConnection& Connection : Removed)
    {
        Changes.Add(MakeShared<FJsonValueObject>(
            ConnectionChange(TEXT("delete"), Connection)));
    }
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(
        TEXT("graph_id"), DescribeGraphTarget(Blueprint, Graph).Id);
    Data->SetNumberField(TEXT("removed_connection_count"), Removed.Num());
    const TSharedRef<FJsonObject> Result = MakeSuccess(
        FString::Printf(
            TEXT("Disconnected %d Blueprint pin connection%s."),
            Removed.Num(),
            Removed.Num() == 1 ? TEXT("") : TEXT("s")),
        Data);
    Result->SetArrayField(TEXT("changes"), Changes);
    if (!Removed.IsEmpty())
    {
        Result->SetArrayField(
            TEXT("next_actions"),
            {MakeShared<FJsonValueObject>(CompileNextAction(Blueprint))});
    }
    return SerializeResult(Result);
}

FString UMCPythonHelper::RemoveBlueprintNode(UBlueprint* Blueprint, const FString& GraphName,
    const FString& NodeName)
{
    if (!Blueprint)
        return MakeJsonError(TEXT("Invalid Blueprint."));

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
        return MakeJsonError(FString::Printf(TEXT("Graph '%s' not found."), *GraphName));

    UEdGraphNode* Node = FindBPNodeByName(Graph, NodeName);
    if (!Node)
        return MakeJsonError(FString::Printf(TEXT("Node '%s' not found in graph '%s'."), *NodeName, *GraphName));

    FMutationScope Scope(NSLOCTEXT(
        "MCPython", "RemoveBlueprintNode", "Remove Blueprint node"));
    if (!Scope.IsValid())
        return MakeJsonError(TEXT("Could not begin a Blueprint graph transaction."));
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    Scope.Modify(Node);

    // Break all pin connections first
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin)
        {
            ModifyPinAndLinks(Scope, Pin);
            Pin->BreakAllPinLinks();
        }
    }

    Graph->RemoveNode(Node);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    return MakeJsonSuccess(FString::Printf(TEXT("Node '%s' removed from graph '%s'."), *NodeName, *GraphName));
}

// ─── BuildBlueprintGraph UFUNCTION ───────────────────────────────────────────

static void LayoutBPGraphNodes(const TMap<FString, UEdGraphNode*>& NodeMap,
    const TArray<TSharedPtr<FJsonValue>>& Connections)
{
    // Simple left-to-right layout based on execution flow
    // Assign columns based on connection depth
    TMap<FString, int32> NodeColumns;
    TSet<FString> Visited;

    // Find nodes with no incoming exec connections (roots)
    TSet<FString> HasIncoming;
    for (auto& ConnVal : Connections)
    {
        const TSharedPtr<FJsonObject>& Conn = ConnVal->AsObject();
        if (!Conn.IsValid()) continue;
        FString TargetNodeId;
        if (Conn->TryGetStringField(TEXT("target_node"), TargetNodeId))
            HasIncoming.Add(TargetNodeId);
    }

    // Assign column 0 to roots, then propagate
    int32 Col = 0;
    for (auto& Pair : NodeMap)
    {
        if (!HasIncoming.Contains(Pair.Key))
            NodeColumns.Add(Pair.Key, 0);
    }

    // Propagate columns through connections
    for (auto& ConnVal : Connections)
    {
        const TSharedPtr<FJsonObject>& Conn = ConnVal->AsObject();
        if (!Conn.IsValid()) continue;
        FString SourceId, TargetId;
        Conn->TryGetStringField(TEXT("source_node"), SourceId);
        Conn->TryGetStringField(TEXT("target_node"), TargetId);

        int32* SourceCol = NodeColumns.Find(SourceId);
        int32 SC = SourceCol ? *SourceCol : 0;
        int32* TargetCol = NodeColumns.Find(TargetId);
        if (!TargetCol || *TargetCol <= SC)
            NodeColumns.Add(TargetId, SC + 1);
    }

    // Count nodes per column for Y positioning
    TMap<int32, int32> ColumnRowCount;
    const float XStep = 400.0f;
    const float YStep = 200.0f;

    for (auto& Pair : NodeMap)
    {
        int32* ColPtr = NodeColumns.Find(Pair.Key);
        int32 C = ColPtr ? *ColPtr : 0;
        int32* RowPtr = ColumnRowCount.Find(C);
        int32 Row = RowPtr ? *RowPtr : 0;

        Pair.Value->NodePosX = (int32)(C * XStep);
        Pair.Value->NodePosY = (int32)(Row * YStep);

        ColumnRowCount.Add(C, Row + 1);
    }
}

FString UMCPythonHelper::BuildBlueprintGraph(UBlueprint* Blueprint, const FString& GraphName,
    const FString& GraphJson)
{
    if (!Blueprint)
        return MakeJsonError(TEXT("Invalid Blueprint."));

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
        return MakeJsonError(FString::Printf(TEXT("Graph '%s' not found."), *GraphName));

    // Parse JSON
    TSharedPtr<FJsonObject> JsonObj;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(GraphJson);
    if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
        return MakeJsonError(TEXT("Failed to parse GraphJson."));

    const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
    if (!JsonObj->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
        return MakeJsonError(TEXT("GraphJson requires a 'nodes' array."));

    const TArray<TSharedPtr<FJsonValue>>& NodesArr = *Nodes;
    TArray<TSharedPtr<FJsonValue>> ConnectionsArr;
    if (JsonObj->HasField(TEXT("connections")))
    {
        const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
        if (!JsonObj->TryGetArrayField(TEXT("connections"), Connections) ||
            !Connections)
        {
            return MakeJsonError(
                TEXT("GraphJson 'connections' must be an array."));
        }
        ConnectionsArr = *Connections;
    }

    FMutationScope Scope(NSLOCTEXT(
        "MCPython", "BuildBlueprintGraph", "Build Blueprint graph"));
    if (!Scope.IsValid())
        return MakeJsonError(TEXT("Could not begin a Blueprint graph transaction."));
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);

    // Remove existing user-created nodes (keep root/entry nodes)
    TArray<UEdGraphNode*> NodesToRemove;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node) continue;
        // Keep entry points (function entry, etc.) but remove user nodes
        // For EventGraph, we typically remove all non-essential nodes
        if (!Node->IsA<UK2Node_Event>())
        {
            NodesToRemove.Add(Node);
        }
    }
    for (UEdGraphNode* Node : NodesToRemove)
    {
        Scope.Modify(Node);
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin)
            {
                ModifyPinAndLinks(Scope, Pin);
                Pin->BreakAllPinLinks();
            }
        }
        Graph->RemoveNode(Node);
    }

    // Create nodes from JSON
    TMap<FString, UEdGraphNode*> NodeMap; // id -> node
    TArray<FString> CreationErrors;

    for (auto& NodeVal : NodesArr)
    {
        if (!NodeVal.IsValid() || NodeVal->Type != EJson::Object)
        {
            CreationErrors.Add(TEXT("Each node must be a JSON object."));
            continue;
        }
        const TSharedPtr<FJsonObject>& NodeObj = NodeVal->AsObject();

        FString NodeId;
        if (!NodeObj->TryGetStringField(TEXT("id"), NodeId) ||
            NodeId.IsEmpty())
        {
            CreationErrors.Add(TEXT("Node requires a non-empty 'id' field."));
            continue;
        }
        if (NodeMap.Contains(NodeId))
        {
            CreationErrors.Add(FString::Printf(
                TEXT("Duplicate node id '%s'."), *NodeId));
            continue;
        }

        FString Error;
        UEdGraphNode* NewNode = CreateBPNodeFromJson(Graph, Blueprint, NodeObj, Error);
        if (NewNode)
        {
            Scope.Modify(NewNode);
            for (UEdGraphPin* Pin : NewNode->Pins)
            {
                ModifyPinAndLinks(Scope, Pin);
            }
            NodeMap.Add(NodeId, NewNode);
        }
        else
        {
            CreationErrors.Add(FString::Printf(TEXT("Node '%s': %s"), *NodeId, *Error));
        }
    }

    // Connect pins
    TArray<FString> ConnectionErrors;
    for (auto& ConnVal : ConnectionsArr)
    {
        if (!ConnVal.IsValid() || ConnVal->Type != EJson::Object)
        {
            ConnectionErrors.Add(
                TEXT("Each connection must be a JSON object."));
            continue;
        }
        const TSharedPtr<FJsonObject>& ConnObj = ConnVal->AsObject();

        FString SourceNodeId, SourcePinName, TargetNodeId, TargetPinName;
        ConnObj->TryGetStringField(TEXT("source_node"), SourceNodeId);
        ConnObj->TryGetStringField(TEXT("source_pin"), SourcePinName);
        ConnObj->TryGetStringField(TEXT("target_node"), TargetNodeId);
        ConnObj->TryGetStringField(TEXT("target_pin"), TargetPinName);

        UEdGraphNode** SourceNodePtr = NodeMap.Find(SourceNodeId);
        UEdGraphNode** TargetNodePtr = NodeMap.Find(TargetNodeId);

        if (!SourceNodePtr || !*SourceNodePtr)
        {
            ConnectionErrors.Add(FString::Printf(TEXT("Source node '%s' not found."), *SourceNodeId));
            continue;
        }
        if (!TargetNodePtr || !*TargetNodePtr)
        {
            ConnectionErrors.Add(FString::Printf(TEXT("Target node '%s' not found."), *TargetNodeId));
            continue;
        }

        UEdGraphPin* SourcePin = FindPinByName(*SourceNodePtr, SourcePinName);
        UEdGraphPin* TargetPin = FindPinByName(*TargetNodePtr, TargetPinName);

        if (!SourcePin)
        {
            ConnectionErrors.Add(FString::Printf(TEXT("Pin '%s' not found on '%s'."), *SourcePinName, *SourceNodeId));
            continue;
        }
        if (!TargetPin)
        {
            ConnectionErrors.Add(FString::Printf(TEXT("Pin '%s' not found on '%s'."), *TargetPinName, *TargetNodeId));
            continue;
        }

        ModifyPinAndLinks(Scope, SourcePin);
        ModifyPinAndLinks(Scope, TargetPin);
        SourcePin->MakeLinkTo(TargetPin);
    }

    if (CreationErrors.IsEmpty() && ConnectionErrors.IsEmpty())
    {
        LayoutBPGraphNodes(NodeMap, ConnectionsArr);
    }

    // Build result
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetBoolField(TEXT("success"), CreationErrors.Num() == 0 && ConnectionErrors.Num() == 0);
    Result->SetNumberField(TEXT("nodes_created"), NodeMap.Num());
    Result->SetNumberField(TEXT("connections_made"), ConnectionsArr.Num() - ConnectionErrors.Num());

    FString Message = FString::Printf(TEXT("Built graph '%s': %d nodes, %d connections."),
        *GraphName, NodeMap.Num(), ConnectionsArr.Num() - ConnectionErrors.Num());
    if (CreationErrors.Num() > 0 || ConnectionErrors.Num() > 0)
        Message += TEXT(" Some errors occurred.");
    Result->SetStringField(TEXT("message"), Message);

    if (CreationErrors.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> ErrArr;
        for (const FString& Err : CreationErrors)
            ErrArr.Add(MakeShareable(new FJsonValueString(Err)));
        Result->SetArrayField(TEXT("creation_errors"), ErrArr);
    }
    if (ConnectionErrors.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> ErrArr;
        for (const FString& Err : ConnectionErrors)
            ErrArr.Add(MakeShareable(new FJsonValueString(Err)));
        Result->SetArrayField(TEXT("connection_errors"), ErrArr);
    }

    // Return node_id -> node_name mapping for reference
    TSharedPtr<FJsonObject> MapObj = MakeShareable(new FJsonObject());
    for (auto& Pair : NodeMap)
    {
        MapObj->SetStringField(Pair.Key, Pair.Value->GetName());
    }
    Result->SetObjectField(TEXT("node_id_to_name"), MapObj);

    if (CreationErrors.Num() > 0 || ConnectionErrors.Num() > 0)
    {
        const FRollbackResult Rollback = Scope.Rollback();
        if (!Rollback.bSucceeded && !Rollback.bDeferredToWorkflow)
        {
            return GraphFailure(
                TEXT("transaction"),
                TEXT("Blueprint graph mutation rollback failed."),
                TEXT("ROLLBACK_FAILED"));
        }
        Result->SetBoolField(TEXT("rolled_back"), Rollback.bSucceeded);
        Result->SetBoolField(
            TEXT("rollback_deferred_to_workflow"),
            Rollback.bDeferredToWorkflow);
        return SerializeJsonObj(Result);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    return SerializeJsonObj(Result);
}

// ─── SetBlueprintNodePosition UFUNCTION ──────────────────────────────────────

FString UMCPythonHelper::SetBlueprintNodePosition(UBlueprint* Blueprint,
    const FString& GraphName, const FString& NodeName, float PosX, float PosY)
{
    if (!Blueprint)
        return MakeJsonError(TEXT("Invalid Blueprint."));

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
        return MakeJsonError(FString::Printf(TEXT("Graph '%s' not found."), *GraphName));

    UEdGraphNode* Node = FindBPNodeByName(Graph, NodeName);
    if (!Node)
        return MakeJsonError(FString::Printf(TEXT("Node '%s' not found in graph '%s'."), *NodeName, *GraphName));

    FMutationScope Scope(NSLOCTEXT(
        "MCPython", "SetBlueprintNodePosition", "Move Blueprint node"));
    if (!Scope.IsValid())
        return MakeJsonError(TEXT("Could not begin a Blueprint graph transaction."));
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    Scope.Modify(Node);
    Node->NodePosX = (int32)PosX;
    Node->NodePosY = (int32)PosY;

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetStringField(TEXT("node"), NodeName);
    R->SetNumberField(TEXT("pos_x"), PosX);
    R->SetNumberField(TEXT("pos_y"), PosY);
    return SerializeJsonObj(R);
}

// ─── SetBlueprintNodePinDefault ──────────────────────────────────────────────

FString UMCPythonHelper::SetBlueprintNodePinDefault(UBlueprint* Blueprint,
    const FString& GraphName, const FString& NodeName,
    const FString& PinName, const FString& Value)
{
    if (!Blueprint)
        return MakeJsonError(TEXT("Invalid Blueprint."));

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
        return MakeJsonError(FString::Printf(TEXT("Graph '%s' not found."), *GraphName));

    UEdGraphNode* Node = FindBPNodeByName(Graph, NodeName);
    if (!Node)
        return MakeJsonError(FString::Printf(TEXT("Node '%s' not found."), *NodeName));

    UEdGraphPin* Pin = FindPinByName(Node, PinName, EGPD_Input);
    if (!Pin)
    {
        TArray<FString> Names;
        for (UEdGraphPin* P : Node->Pins) { if (P && !P->bHidden && P->Direction == EGPD_Input) Names.Add(P->GetName()); }
        return MakeJsonError(FString::Printf(TEXT("Input pin '%s' not found. Available: %s"), *PinName, *FString::Join(Names, TEXT(", "))));
    }

    UObject* DefaultObject = nullptr;
    // For object-type pins, try loading the asset before beginning mutation.
    if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
        Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
        Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
        Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
    {
        DefaultObject = StaticLoadObject(UObject::StaticClass(), nullptr, *Value);
        if (!DefaultObject)
            return MakeJsonError(FString::Printf(TEXT("Could not load asset: %s"), *Value));
    }

    FMutationScope Scope(NSLOCTEXT(
        "MCPython", "SetBlueprintNodePinDefault", "Set Blueprint pin default"));
    if (!Scope.IsValid())
        return MakeJsonError(TEXT("Could not begin a Blueprint graph transaction."));
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    Scope.Modify(Node);

    if (DefaultObject)
    {
        Pin->DefaultObject = DefaultObject;
        Pin->DefaultValue = TEXT("");
    }
    else
    {
        Pin->DefaultValue = Value;
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetStringField(TEXT("message"), FString::Printf(TEXT("Set pin '%s' on '%s' to '%s'."), *PinName, *NodeName, *Value));
    return SerializeJsonObj(R);
}
