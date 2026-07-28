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

// ─── ConnectBlueprintPins UFUNCTION ──────────────────────────────────────────

FString UMCPythonHelper::ConnectBlueprintPins(UBlueprint* Blueprint, const FString& GraphName,
    const FString& SourceNodeName, const FString& SourcePinName,
    const FString& TargetNodeName, const FString& TargetPinName)
{
    if (!Blueprint)
        return MakeJsonError(TEXT("Invalid Blueprint."));

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
        return MakeJsonError(FString::Printf(TEXT("Graph '%s' not found."), *GraphName));

    UEdGraphNode* SourceNode = FindBPNodeByName(Graph, SourceNodeName);
    if (!SourceNode)
        return MakeJsonError(FString::Printf(TEXT("Source node '%s' not found."), *SourceNodeName));

    UEdGraphNode* TargetNode = FindBPNodeByName(Graph, TargetNodeName);
    if (!TargetNode)
        return MakeJsonError(FString::Printf(TEXT("Target node '%s' not found."), *TargetNodeName));

    UEdGraphPin* SourcePin = FindPinByName(SourceNode, SourcePinName);
    if (!SourcePin)
    {
        TArray<FString> PinNames;
        for (UEdGraphPin* P : SourceNode->Pins) { if (P && !P->bHidden) PinNames.Add(P->GetName()); }
        return MakeJsonError(FString::Printf(TEXT("Pin '%s' not found on node '%s'. Available: %s"),
            *SourcePinName, *SourceNodeName, *FString::Join(PinNames, TEXT(", "))));
    }

    UEdGraphPin* TargetPin = FindPinByName(TargetNode, TargetPinName);
    if (!TargetPin)
    {
        TArray<FString> PinNames;
        for (UEdGraphPin* P : TargetNode->Pins) { if (P && !P->bHidden) PinNames.Add(P->GetName()); }
        return MakeJsonError(FString::Printf(TEXT("Pin '%s' not found on node '%s'. Available: %s"),
            *TargetPinName, *TargetNodeName, *FString::Join(PinNames, TEXT(", "))));
    }

    // Verify directions are compatible (output -> input)
    if (SourcePin->Direction == TargetPin->Direction)
        return MakeJsonError(FString::Printf(TEXT("Cannot connect pins with same direction (%s)."),
            SourcePin->Direction == EGPD_Input ? TEXT("both Input") : TEXT("both Output")));

    // Check if connection is allowed by the schema and handle BREAK_OTHERS
    const UEdGraphSchema* Schema = Graph->GetSchema();
    if (!Schema)
        return MakeJsonError(TEXT("Graph schema is unavailable."));
    const FPinConnectionResponse Response =
        Schema->CanCreateConnection(SourcePin, TargetPin);
    if (Response.Response == CONNECT_RESPONSE_DISALLOW)
        return MakeJsonError(FString::Printf(TEXT("Connection not allowed: %s"), *Response.Message.ToString()));

    FMutationScope Scope(NSLOCTEXT(
        "MCPython", "ConnectBlueprintPins", "Connect Blueprint pins"));
    if (!Scope.IsValid())
        return MakeJsonError(TEXT("Could not begin a Blueprint graph transaction."));
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    ModifyPinAndLinks(Scope, SourcePin);
    ModifyPinAndLinks(Scope, TargetPin);

    // Break existing connections when schema requires it (e.g. exec output already connected)
    if (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A)
        SourcePin->BreakAllPinLinks();
    else if (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B)
        TargetPin->BreakAllPinLinks();

    SourcePin->MakeLinkTo(TargetPin);

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    return MakeJsonSuccess(FString::Printf(TEXT("Connected %s.%s -> %s.%s"),
        *SourceNodeName, *SourcePinName, *TargetNodeName, *TargetPinName));
}

// ─── RemoveBlueprintNode UFUNCTION ───────────────────────────────────────────

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
