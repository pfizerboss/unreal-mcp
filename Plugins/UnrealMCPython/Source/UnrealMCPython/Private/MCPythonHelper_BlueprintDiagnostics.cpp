// Copyright (c) 2025 GenOrca (by zenoengine). All Rights Reserved.

#include "MCPythonHelper.h"
#include "MCPythonBlueprint2Internal.h"

#include "EdGraphToken.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Internationalization/Text.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

namespace
{
FString CompileStatus(const UBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        return TEXT("Unknown");
    }
    switch (Blueprint->Status)
    {
    case BS_UpToDate: return TEXT("UpToDate");
    case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
    case BS_Error: return TEXT("Error");
    case BS_Dirty: return TEXT("Dirty");
    case BS_BeingCreated: return TEXT("BeingCreated");
    default: return TEXT("Unknown");
    }
}

void NormalizeDiagnostic(
    const FString& Message,
    const bool bWarning,
    FString& OutCode,
    FString& OutHint)
{
    const FString Lower = Message.ToLower();
    if (Lower.Contains(TEXT("required pin")) ||
        Lower.Contains(TEXT("no value")) ||
        Lower.Contains(TEXT("must be linked")) ||
        Lower.Contains(TEXT("must have a connection")))
    {
        OutCode = TEXT("BP_MISSING_REQUIRED_PIN");
        OutHint = TEXT("Connect the cited pin or assign a valid default value, then compile again.");
    }
    else if (Lower.Contains(TEXT("could not find")) ||
             Lower.Contains(TEXT("unresolved")))
    {
        OutCode = TEXT("BP_UNRESOLVED_MEMBER");
        OutHint = TEXT("Re-inspect the referenced member, replace the stale node, then compile again.");
    }
    else if (Lower.Contains(TEXT("not compatible")) ||
             Lower.Contains(TEXT("type mismatch")) ||
             Lower.Contains(TEXT("incompatible")) ||
             Lower.Contains(TEXT("can't connect pins")))
    {
        OutCode = TEXT("BP_TYPE_MISMATCH");
        OutHint = TEXT("Insert an explicit conversion or change one of the connected pin types.");
    }
    else if (Lower.Contains(TEXT("already exists")) ||
             Lower.Contains(TEXT("duplicate")))
    {
        OutCode = TEXT("BP_DUPLICATE_MEMBER");
        OutHint = TEXT("Rename or remove the duplicate Blueprint member, then compile again.");
    }
    else if (Lower.Contains(TEXT("accessed none")))
    {
        OutCode = TEXT("BP_POSSIBLE_NULL_ACCESS");
        OutHint = TEXT("Validate the referenced object before accessing it.");
    }
    else
    {
        OutCode = bWarning ? TEXT("BP_COMPILE_WARNING") : TEXT("BP_COMPILE_ERROR");
        OutHint = TEXT("Inspect the cited Blueprint node and pins, correct the issue, then compile again.");
    }
}

void ResolveDiagnosticTarget(
    UBlueprint* Blueprint,
    const FTokenizedMessage& Message,
    FString& OutGraphId,
    FString& OutNodeId,
    FString& OutPinId)
{
    const UEdGraphPin* Pin = nullptr;
    const UObject* GraphObject = nullptr;
    for (const TSharedRef<IMessageToken>& Token : Message.GetMessageTokens())
    {
        if (Token->GetType() != EMessageToken::EdGraph)
        {
            continue;
        }
        const FEdGraphToken* EdGraphToken =
            static_cast<const FEdGraphToken*>(&Token.Get());
        if (!Pin)
        {
            Pin = EdGraphToken->GetPin();
        }
        if (!GraphObject)
        {
            GraphObject = EdGraphToken->GetGraphObject();
        }
    }

    const UEdGraphNode* Node = Pin ? Pin->GetOwningNode() : nullptr;
    const UEdGraph* Graph = Node ? Node->GetGraph() : nullptr;
    if (!Node)
    {
        Node = Cast<UEdGraphNode>(GraphObject);
        Graph = Node ? Node->GetGraph() : Cast<UEdGraph>(GraphObject);
    }
    if (Pin)
    {
        OutPinId = UE::MCPython::Blueprint2::DescribePinTarget(
            Blueprint, Pin).Id;
    }
    if (Node)
    {
        OutNodeId = UE::MCPython::Blueprint2::DescribeNodeTarget(
            Blueprint, Node).Id;
    }
    if (Graph)
    {
        OutGraphId = UE::MCPython::Blueprint2::DescribeGraphTarget(
            Blueprint, Graph).Id;
    }
}

TSharedRef<FJsonObject> MakeDiagnostic(
    UBlueprint* Blueprint,
    const TSharedRef<FTokenizedMessage>& Message,
    const bool bWarning)
{
    const FString Text = Message->ToText().ToString();
    FString Code;
    FString Hint;
    NormalizeDiagnostic(Text, bWarning, Code, Hint);

    FString GraphId;
    FString NodeId;
    FString PinId;
    ResolveDiagnosticTarget(
        Blueprint, Message.Get(), GraphId, NodeId, PinId);

    const TSharedRef<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
    Diagnostic->SetStringField(TEXT("code"), Code);
    Diagnostic->SetStringField(
        TEXT("severity"), bWarning ? TEXT("warning") : TEXT("error"));
    Diagnostic->SetStringField(TEXT("message"), Text);
    Diagnostic->SetStringField(TEXT("hint"), Hint);
    Diagnostic->SetStringField(TEXT("graph_id"), GraphId);
    Diagnostic->SetStringField(TEXT("node_id"), NodeId);
    Diagnostic->SetStringField(TEXT("pin_id"), PinId);
    return Diagnostic;
}

TSharedRef<FJsonObject> MakeWarningRecord(
    const TSharedRef<FJsonObject>& Diagnostic)
{
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(
        TEXT("graph_id"), Diagnostic->GetStringField(TEXT("graph_id")));
    Details->SetStringField(
        TEXT("node_id"), Diagnostic->GetStringField(TEXT("node_id")));
    Details->SetStringField(
        TEXT("pin_id"), Diagnostic->GetStringField(TEXT("pin_id")));
    Details->SetStringField(
        TEXT("hint"), Diagnostic->GetStringField(TEXT("hint")));

    const TSharedRef<FJsonObject> Warning = MakeShared<FJsonObject>();
    Warning->SetStringField(
        TEXT("code"), Diagnostic->GetStringField(TEXT("code")));
    Warning->SetStringField(
        TEXT("message"), Diagnostic->GetStringField(TEXT("message")));
    Warning->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
    Warning->SetObjectField(TEXT("details"), Details);
    return Warning;
}

TSharedRef<FJsonObject> MakeNextAction(
    const FString& Action,
    const FString& AssetPath)
{
    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), AssetPath);
    const TSharedRef<FJsonObject> NextAction = MakeShared<FJsonObject>();
    NextAction->SetStringField(TEXT("domain"), TEXT("blueprint"));
    NextAction->SetStringField(TEXT("action"), Action);
    NextAction->SetObjectField(TEXT("params"), Params);
    return NextAction;
}

TSharedRef<FJsonObject> MakeInspectNextAction(
    const FString& AssetPath,
    const TArray<TSharedPtr<FJsonValue>>& Diagnostics)
{
    const TSharedRef<FJsonObject> NextAction = MakeNextAction(
        TEXT("inspect_blueprint"), AssetPath);
    const TSharedPtr<FJsonObject> Params = NextAction->GetObjectField(
        TEXT("params"));
    TArray<TSharedPtr<FJsonValue>> Queries;

    const TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
    Nodes->SetStringField(TEXT("op"), TEXT("nodes"));
    Nodes->SetStringField(TEXT("detail"), TEXT("detailed"));
    Nodes->SetNumberField(TEXT("limit"), 100);
    Queries.Add(MakeShared<FJsonValueObject>(Nodes));

    FString TargetNodeId;
    for (const TSharedPtr<FJsonValue>& Value : Diagnostics)
    {
        const TSharedPtr<FJsonObject> Diagnostic = Value.IsValid()
            ? Value->AsObject()
            : nullptr;
        if (Diagnostic &&
            Diagnostic->TryGetStringField(TEXT("node_id"), TargetNodeId) &&
            !TargetNodeId.IsEmpty())
        {
            break;
        }
    }
    if (!TargetNodeId.IsEmpty())
    {
        const TSharedRef<FJsonObject> Pins = MakeShared<FJsonObject>();
        Pins->SetStringField(TEXT("op"), TEXT("pins"));
        Pins->SetStringField(TEXT("node_id"), TargetNodeId);
        Pins->SetStringField(TEXT("detail"), TEXT("detailed"));
        Pins->SetNumberField(TEXT("limit"), 100);
        Queries.Add(MakeShared<FJsonValueObject>(Pins));
    }
    Params->SetArrayField(TEXT("queries"), Queries);
    return NextAction;
}

FString InvalidBlueprintResult()
{
    using namespace UE::MCPython::Blueprint2;
    const TSharedRef<FJsonObject> Result = MakeFailure(
        TEXT("INVALID_INPUT"),
        TEXT("params.asset_path"),
        TEXT("Invalid Blueprint."),
        false,
        TEXT("Load a valid Blueprint asset and retry compilation."));
    Result->SetStringField(TEXT("status"), TEXT("Unknown"));
    Result->SetStringField(TEXT("message"), TEXT("Invalid Blueprint."));
    Result->SetArrayField(TEXT("diagnostics"), {});
    return SerializeResult(Result);
}

TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json)
{
    TSharedPtr<FJsonObject> Result;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    return FJsonSerializer::Deserialize(Reader, Result) ? Result : nullptr;
}

TSharedRef<FJsonObject> MakeHealthIssueFromDiagnostic(
    const TSharedRef<FJsonObject>& Diagnostic)
{
    const TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
    for (const FString& Field : {
             TEXT("code"),
             TEXT("severity"),
             TEXT("message"),
             TEXT("hint"),
             TEXT("graph_id"),
             TEXT("node_id"),
             TEXT("pin_id")})
    {
        Issue->SetStringField(Field, Diagnostic->GetStringField(Field));
    }
    Issue->SetStringField(TEXT("member_id"), FString());
    return Issue;
}

TSharedRef<FJsonObject> MakeHealthIssue(
    const FString& Code,
    const FString& Severity,
    const FString& Message,
    const FString& Hint,
    const FString& GraphId = FString(),
    const FString& NodeId = FString(),
    const FString& PinId = FString(),
    const FString& MemberId = FString())
{
    const TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
    Issue->SetStringField(TEXT("code"), Code);
    Issue->SetStringField(TEXT("severity"), Severity);
    Issue->SetStringField(TEXT("message"), Message);
    Issue->SetStringField(TEXT("hint"), Hint);
    Issue->SetStringField(TEXT("graph_id"), GraphId);
    Issue->SetStringField(TEXT("node_id"), NodeId);
    Issue->SetStringField(TEXT("pin_id"), PinId);
    Issue->SetStringField(TEXT("member_id"), MemberId);
    return Issue;
}

FString HealthIssueKey(const TSharedRef<FJsonObject>& Issue)
{
    const TArray<FString> Fields = {
        Issue->GetStringField(TEXT("code")),
        Issue->GetStringField(TEXT("graph_id")),
        Issue->GetStringField(TEXT("node_id")),
        Issue->GetStringField(TEXT("pin_id")),
        Issue->GetStringField(TEXT("member_id")),
    };
    return FString::Join(Fields, TEXT("\x1f"));
}

FString HealthIssueSortKey(const TSharedRef<FJsonObject>& Issue)
{
    const TArray<FString> Fields = {
        Issue->GetStringField(TEXT("graph_id")),
        Issue->GetStringField(TEXT("node_id")),
        Issue->GetStringField(TEXT("pin_id")),
        Issue->GetStringField(TEXT("member_id")),
        Issue->GetStringField(TEXT("code")),
        Issue->GetStringField(TEXT("message")),
        Issue->GetStringField(TEXT("hint")),
    };
    return FString::Join(Fields, TEXT("\x1f"));
}

void NormalizeHealthIssuesImpl(TArray<TSharedPtr<FJsonValue>>& Issues)
{
    Issues.Sort([](
        const TSharedPtr<FJsonValue>& LeftValue,
        const TSharedPtr<FJsonValue>& RightValue)
    {
        const TSharedPtr<FJsonObject> Left = LeftValue->AsObject();
        const TSharedPtr<FJsonObject> Right = RightValue->AsObject();
        const int32 LeftSeverity =
            Left->GetStringField(TEXT("severity")) == TEXT("error") ? 0 : 1;
        const int32 RightSeverity =
            Right->GetStringField(TEXT("severity")) == TEXT("error") ? 0 : 1;
        if (LeftSeverity != RightSeverity)
        {
            return LeftSeverity < RightSeverity;
        }
        return HealthIssueSortKey(Left.ToSharedRef()) <
            HealthIssueSortKey(Right.ToSharedRef());
    });

    TSet<FString> Seen;
    Issues.RemoveAll([&Seen](const TSharedPtr<FJsonValue>& Value)
    {
        const TSharedPtr<FJsonObject> Issue = Value.IsValid()
            ? Value->AsObject()
            : nullptr;
        if (!Issue)
        {
            return true;
        }
        const FString Key = HealthIssueKey(Issue.ToSharedRef());
        if (Seen.Contains(Key))
        {
            return true;
        }
        Seen.Add(Key);
        return false;
    });
}

void AddSCSHealthIssues(
    UBlueprint* Blueprint,
    TArray<TSharedPtr<FJsonValue>>& Issues)
{
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return;
    }
    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    TSet<USCS_Node*> AllNodeSet;
    TArray<USCS_Node*> Universe;
    for (USCS_Node* Node : SCS->GetAllNodes())
    {
        if (Node)
        {
            AllNodeSet.Add(Node);
            Universe.AddUnique(Node);
        }
    }
    for (USCS_Node* Root : SCS->GetRootNodes())
    {
        if (Root)
        {
            Universe.AddUnique(Root);
        }
    }
    for (int32 Index = 0; Index < Universe.Num(); ++Index)
    {
        for (USCS_Node* Child : Universe[Index]->GetChildNodes())
        {
            if (Child)
            {
                Universe.AddUnique(Child);
            }
        }
    }

    TMap<FGuid, TArray<USCS_Node*>> NodesByGuid;
    for (USCS_Node* Node : Universe)
    {
        if (Node->VariableGuid.IsValid())
        {
            NodesByGuid.FindOrAdd(Node->VariableGuid).Add(Node);
        }
    }
    for (const TPair<FGuid, TArray<USCS_Node*>>& Pair : NodesByGuid)
    {
        if (Pair.Value.Num() < 2)
        {
            continue;
        }
        TArray<FString> Names;
        for (const USCS_Node* Node : Pair.Value)
        {
            Names.Add(Node->GetVariableName().ToString());
        }
        Names.Sort();
        Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
            TEXT("BP_SCS_DUPLICATE_GUID"),
            TEXT("error"),
            FString::Printf(
                TEXT("SCS components share VariableGuid %s: %s."),
                *Pair.Key.ToString(EGuidFormats::DigitsWithHyphensLower),
                *FString::Join(Names, TEXT(", "))),
            TEXT("Assign each SCS component a unique persisted GUID, then inspect the component hierarchy."),
            FString(),
            FString(),
            FString(),
            UE::MCPython::Blueprint2::DescribeComponentTarget(
                Blueprint, Pair.Value[0]).Id)));
    }

    TMap<USCS_Node*, TSet<USCS_Node*>> ParentsByChild;
    for (USCS_Node* Parent : Universe)
    {
        for (USCS_Node* Child : Parent->GetChildNodes())
        {
            if (Child)
            {
                ParentsByChild.FindOrAdd(Child).Add(Parent);
            }
        }
    }
    for (const TPair<USCS_Node*, TSet<USCS_Node*>>& Pair : ParentsByChild)
    {
        if (Pair.Value.Num() > 1)
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                TEXT("BP_SCS_MULTIPLE_PARENTS"),
                TEXT("error"),
                FString::Printf(
                    TEXT("SCS component '%s' has %d parents."),
                    *Pair.Key->GetVariableName().ToString(),
                    Pair.Value.Num()),
                TEXT("Reparent the component under exactly one SCS parent."),
                FString(),
                FString(),
                FString(),
                UE::MCPython::Blueprint2::DescribeComponentTarget(
                    Blueprint, Pair.Key).Id)));
        }
    }

    for (USCS_Node* Node : Universe)
    {
        if (!AllNodeSet.Contains(Node))
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                TEXT("BP_SCS_NODE_MISSING_FROM_ALL_NODES"),
                TEXT("error"),
                FString::Printf(
                    TEXT("SCS component '%s' is referenced by the hierarchy but missing from AllNodes."),
                    *Node->GetVariableName().ToString()),
                TEXT("Rebuild the SCS hierarchy so every root and child is registered in AllNodes."),
                FString(),
                FString(),
                FString(),
                UE::MCPython::Blueprint2::DescribeComponentTarget(
                    Blueprint, Node).Id)));
        }
    }

    TSet<USCS_Node*> Reachable;
    TArray<USCS_Node*> Pending = SCS->GetRootNodes();
    for (int32 Index = 0; Index < Pending.Num(); ++Index)
    {
        USCS_Node* Node = Pending[Index];
        if (!Node || Reachable.Contains(Node))
        {
            continue;
        }
        Reachable.Add(Node);
        for (USCS_Node* Child : Node->GetChildNodes())
        {
            if (Child && !Reachable.Contains(Child))
            {
                Pending.Add(Child);
            }
        }
    }
    for (USCS_Node* Node : AllNodeSet)
    {
        if (!Reachable.Contains(Node))
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                TEXT("BP_SCS_ORPHAN"),
                TEXT("error"),
                FString::Printf(
                    TEXT("SCS component '%s' is registered in AllNodes but unreachable from every root."),
                    *Node->GetVariableName().ToString()),
                TEXT("Attach the component to one valid SCS root or remove the orphaned entry."),
                FString(),
                FString(),
                FString(),
                UE::MCPython::Blueprint2::DescribeComponentTarget(
                    Blueprint, Node).Id)));
        }
    }

    TSet<USCS_Node*> Active;
    TSet<USCS_Node*> Complete;
    TSet<USCS_Node*> ReportedCycles;
    TFunction<void(USCS_Node*)> Visit = [&](USCS_Node* Node)
    {
        if (!Node || Complete.Contains(Node))
        {
            return;
        }
        if (Active.Contains(Node))
        {
            if (!ReportedCycles.Contains(Node))
            {
                ReportedCycles.Add(Node);
                Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                    TEXT("BP_SCS_CYCLE"),
                    TEXT("error"),
                    FString::Printf(
                        TEXT("SCS hierarchy contains a cycle through component '%s'."),
                        *Node->GetVariableName().ToString()),
                    TEXT("Break the component parenting cycle, leaving a directed tree rooted in the SCS root set."),
                    FString(),
                    FString(),
                    FString(),
                    UE::MCPython::Blueprint2::DescribeComponentTarget(
                        Blueprint, Node).Id)));
            }
            return;
        }
        Active.Add(Node);
        for (USCS_Node* Child : Node->GetChildNodes())
        {
            Visit(Child);
        }
        Active.Remove(Node);
        Complete.Add(Node);
    };
    for (USCS_Node* Node : Universe)
    {
        Visit(Node);
    }
}

void AddGraphHealthIssues(
    UBlueprint* Blueprint,
    TArray<TSharedPtr<FJsonValue>>& Issues)
{
    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        const UEdGraphSchema_K2* Schema = Graph
            ? Cast<UEdGraphSchema_K2>(Graph->GetSchema())
            : nullptr;
        if (!Graph || !Schema)
        {
            continue;
        }
        const FString GraphId =
            UE::MCPython::Blueprint2::DescribeGraphTarget(
                Blueprint, Graph).Id;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            const FString NodeId =
                UE::MCPython::Blueprint2::DescribeNodeTarget(
                    Blueprint, Node).Id;
            if (const UK2Node_CallFunction* Call =
                    Cast<UK2Node_CallFunction>(Node))
            {
                if (Call->FunctionReference.GetMemberName() != NAME_None &&
                    !Call->GetTargetFunction())
                {
                    Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                        TEXT("BP_UNRESOLVED_MEMBER"),
                        TEXT("error"),
                        FString::Printf(
                            TEXT("Call node references unresolved function '%s'."),
                            *Call->FunctionReference.GetMemberName().ToString()),
                        TEXT("Re-inspect the callable member and replace or retarget the stale call node."),
                        GraphId,
                        NodeId)));
                }
            }
            if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
            {
                if (Variable->VariableReference.GetMemberName() != NAME_None &&
                    !Variable->VariableReference.ResolveMember<FProperty>(
                        Variable->GetBlueprintClassFromNode()))
                {
                    Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                        TEXT("BP_UNRESOLVED_MEMBER"),
                        TEXT("error"),
                        FString::Printf(
                            TEXT("Variable node references unresolved member '%s'."),
                            *Variable->VariableReference.GetMemberName().ToString()),
                        TEXT("Re-inspect Blueprint variables and replace or retarget the stale variable node."),
                        GraphId,
                        NodeId)));
                }
            }

            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Input || Pin->bHidden ||
                    !Pin->LinkedTo.IsEmpty())
                {
                    continue;
                }
                FString SimpleMessage;
                const bool bSimpleValid =
                    Schema->DefaultValueSimpleValidation(
                        Pin->PinType,
                        Pin->PinName,
                        Pin->DefaultValue,
                        Pin->DefaultObject,
                        Pin->DefaultTextValue,
                        &SimpleMessage);
                const FString FullMessage = Schema->IsPinDefaultValid(
                    Pin,
                    Pin->DefaultValue,
                    Pin->DefaultObject,
                    Pin->DefaultTextValue);
                if (bSimpleValid && FullMessage.IsEmpty())
                {
                    continue;
                }
                const FString Message = FullMessage.IsEmpty()
                    ? SimpleMessage
                    : FullMessage;
                const bool bRequired = Pin->PinType.bIsReference ||
                    Pin->PinType.IsContainer() ||
                    Message.Contains(TEXT("must have an input"),
                        ESearchCase::IgnoreCase) ||
                    Message.Contains(TEXT("must be linked"),
                        ESearchCase::IgnoreCase) ||
                    Message.Contains(TEXT("must have a connection"),
                        ESearchCase::IgnoreCase);
                Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                    bRequired
                        ? TEXT("BP_MISSING_REQUIRED_PIN")
                        : TEXT("BP_INVALID_PIN_DEFAULT"),
                    TEXT("error"),
                    Message.IsEmpty()
                        ? FString::Printf(
                            TEXT("Input pin '%s' has no valid connection or default."),
                            *Pin->PinName.ToString())
                        : Message,
                    bRequired
                        ? TEXT("Connect the required input pin or assign a supported default value.")
                        : TEXT("Assign a canonical default compatible with the pin type."),
                    GraphId,
                    NodeId,
                    UE::MCPython::Blueprint2::DescribePinTarget(
                        Blueprint, Pin).Id)));
            }
        }
    }
}

void AddInterfaceHealthIssues(
    UBlueprint* Blueprint,
    TArray<TSharedPtr<FJsonValue>>& Issues)
{
    for (const FBPInterfaceDescription& Description :
         Blueprint->ImplementedInterfaces)
    {
        UClass* InterfaceClass = Description.Interface.Get();
        if (!InterfaceClass)
        {
            continue;
        }
        for (TFieldIterator<UFunction> FunctionIt(
                 InterfaceClass, EFieldIteratorFlags::ExcludeSuper);
             FunctionIt;
             ++FunctionIt)
        {
            UFunction* Function = *FunctionIt;
            if (!Function ||
                !Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
            {
                continue;
            }
            const bool bHasGraph = Description.Graphs.ContainsByPredicate(
                [Function](const UEdGraph* Graph)
                {
                    return Graph && Graph->GetFName() == Function->GetFName();
                });
            const bool bHasEvent =
                FBlueprintEditorUtils::FindOverrideForFunction(
                    Blueprint,
                    InterfaceClass,
                    Function->GetFName()) != nullptr;
            if (!bHasGraph && !bHasEvent)
            {
                Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                    TEXT("BP_MISSING_INTERFACE_IMPLEMENTATION"),
                    TEXT("error"),
                    FString::Printf(
                        TEXT("Interface function '%s.%s' has no implementation graph or event."),
                        *InterfaceClass->GetPathName(),
                        *Function->GetName()),
                    TEXT("Implement the missing interface function as the generated graph or override event."),
                    FString(),
                    FString(),
                    FString(),
                    FString::Printf(
                        TEXT("interface:%s#%s"),
                        *InterfaceClass->GetPathName(),
                        *Function->GetName()))));
            }
        }
    }
}

void AddMemberCollisionHealthIssues(
    UBlueprint* Blueprint,
    TArray<TSharedPtr<FJsonValue>>& Issues)
{
    struct FNamedMember
    {
        FString Name;
        FString Id;
    };
    TMap<FString, TArray<FNamedMember>> MembersByLowerName;
    const auto AddMember = [&MembersByLowerName](
        const FString& Name,
        const FString& Id)
    {
        if (!Name.IsEmpty())
        {
            MembersByLowerName.FindOrAdd(Name.ToLower()).Add({Name, Id});
        }
    };
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        AddMember(
            Variable.VarName.ToString(),
            UE::MCPython::Blueprint2::DescribeVariableTarget(
                Blueprint, Variable).Id);
    }
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph)
        {
            AddMember(
                Graph->GetName(),
                UE::MCPython::Blueprint2::DescribeGraphTarget(
                    Blueprint, Graph).Id);
        }
    }
    for (UEdGraph* Graph : Blueprint->MacroGraphs)
    {
        if (Graph)
        {
            AddMember(
                Graph->GetName(),
                UE::MCPython::Blueprint2::DescribeGraphTarget(
                    Blueprint, Graph).Id);
        }
    }
    if (Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* Component :
             Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (Component)
            {
                AddMember(
                    Component->GetVariableName().ToString(),
                    UE::MCPython::Blueprint2::DescribeComponentTarget(
                        Blueprint, Component).Id);
            }
        }
    }
    TArray<UK2Node_CustomEvent*> CustomEvents;
    FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, CustomEvents);
    for (UK2Node_CustomEvent* Event : CustomEvents)
    {
        if (Event && Event->CustomFunctionName != NAME_None)
        {
            AddMember(
                Event->CustomFunctionName.ToString(),
                UE::MCPython::Blueprint2::DescribeNodeTarget(
                    Blueprint, Event).Id);
        }
    }

    for (TPair<FString, TArray<FNamedMember>>& Pair : MembersByLowerName)
    {
        if (Pair.Value.Num() < 2)
        {
            continue;
        }
        Pair.Value.Sort([](
            const FNamedMember& Left,
            const FNamedMember& Right)
        {
            return Left.Id == Right.Id
                ? Left.Name < Right.Name
                : Left.Id < Right.Id;
        });
        TArray<FString> Names;
        for (const FNamedMember& Member : Pair.Value)
        {
            Names.Add(Member.Name);
        }
        Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
            TEXT("BP_DUPLICATE_MEMBER"),
            TEXT("error"),
            FString::Printf(
                TEXT("Blueprint members collide case-insensitively: %s."),
                *FString::Join(Names, TEXT(", "))),
            TEXT("Rename one colliding variable, function, macro, event, or component."),
            FString(),
            FString(),
            FString(),
            Pair.Value[0].Id)));
    }
}

void AddVariableDefaultHealthIssues(
    UBlueprint* Blueprint,
    TArray<TSharedPtr<FJsonValue>>& Issues)
{
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        const FEdGraphPinType& Type = Variable.VarType;
        if (Variable.DefaultValue.IsEmpty())
        {
            continue;
        }
        FString DefaultValue = Variable.DefaultValue;
        TObjectPtr<UObject> DefaultObject = nullptr;
        FText DefaultText;
        const bool bHardObject =
            Type.PinCategory == UEdGraphSchema_K2::PC_Object ||
            Type.PinCategory == UEdGraphSchema_K2::PC_Interface;
        const bool bHardClass =
            Type.PinCategory == UEdGraphSchema_K2::PC_Class;
        if (bHardObject || bHardClass)
        {
            Schema->GetPinDefaultValuesFromString(
                Type,
                Blueprint,
                Variable.DefaultValue,
                DefaultValue,
                DefaultObject,
                DefaultText);
            if (!Variable.DefaultValue.IsEmpty() &&
                !Variable.DefaultValue.Equals(
                    TEXT("None"), ESearchCase::IgnoreCase) &&
                !DefaultObject)
            {
                Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                    bHardClass
                        ? TEXT("BP_INVALID_CLASS_DEFAULT")
                        : TEXT("BP_INVALID_OBJECT_DEFAULT"),
                    TEXT("error"),
                    FString::Printf(
                        TEXT("Variable '%s' default path cannot be resolved: %s."),
                        *Variable.VarName.ToString(),
                        *Variable.DefaultValue),
                    bHardClass
                        ? TEXT("Set a loadable class path compatible with the variable type or clear the default.")
                        : TEXT("Set a loadable object path compatible with the variable type or clear the default."),
                    FString(),
                    FString(),
                    FString(),
                    UE::MCPython::Blueprint2::DescribeVariableTarget(
                        Blueprint, Variable).Id)));
                continue;
            }
        }
        else if (Type.PinCategory == UEdGraphSchema_K2::PC_Text)
        {
            const bool bComplexText = FTextStringHelper::IsComplexText(
                *Variable.DefaultValue);
            const TCHAR* TextEnd = FTextStringHelper::ReadFromBuffer(
                *Variable.DefaultValue,
                DefaultText,
                nullptr,
                nullptr,
                bComplexText);
            while (TextEnd && FChar::IsWhitespace(*TextEnd))
            {
                ++TextEnd;
            }
            if (!TextEnd || *TextEnd != TEXT('\0'))
            {
                Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                    TEXT("BP_INVALID_VARIABLE_DEFAULT"),
                    TEXT("error"),
                    FString::Printf(
                        TEXT("Variable '%s' has an invalid serialized text default."),
                        *Variable.VarName.ToString()),
                    TEXT("Set the variable through the canonical Blueprint default schema for its declared type."),
                    FString(),
                    FString(),
                    FString(),
                    UE::MCPython::Blueprint2::DescribeVariableTarget(
                        Blueprint, Variable).Id)));
                continue;
            }
            DefaultValue.Reset();
        }

        const TSharedPtr<FJsonValue> CanonicalJson =
            UE::MCPython::Blueprint2::SerializeDefaultValue(
                Type, DefaultValue, DefaultObject, DefaultText);
        UE::MCPython::Blueprint2::FNormalizedDefault Normalized;
        UE::MCPython::Blueprint2::FError NormalizeError;
        const bool bNormalized = CanonicalJson.IsValid() &&
            UE::MCPython::Blueprint2::NormalizeDefaultValue(
                Type,
                CanonicalJson,
                Blueprint,
                Normalized,
                NormalizeError,
                TEXT("default"));
        FString CanonicalStoredDefault;
        if (bNormalized)
        {
            if (Normalized.DefaultObject)
            {
                CanonicalStoredDefault = Normalized.DefaultObject->GetPathName();
            }
            else if (!Normalized.DefaultTextValue.IsEmpty())
            {
                FTextStringHelper::WriteToBuffer(
                    CanonicalStoredDefault, Normalized.DefaultTextValue);
            }
            else
            {
                CanonicalStoredDefault = Normalized.DefaultValue;
            }
        }
        const bool bCanonicalStoredValue =
            Type.PinCategory == UEdGraphSchema_K2::PC_Text ||
            CanonicalStoredDefault == Variable.DefaultValue;
        if (!bNormalized || !bCanonicalStoredValue)
        {
            const bool bClassPath =
                Type.PinCategory == UEdGraphSchema_K2::PC_Class ||
                Type.PinCategory == UEdGraphSchema_K2::PC_SoftClass;
            const bool bObjectPath =
                Type.PinCategory == UEdGraphSchema_K2::PC_Object ||
                Type.PinCategory == UEdGraphSchema_K2::PC_Interface ||
                Type.PinCategory == UEdGraphSchema_K2::PC_SoftObject;
            Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                bClassPath
                    ? TEXT("BP_INVALID_CLASS_DEFAULT")
                    : bObjectPath
                        ? TEXT("BP_INVALID_OBJECT_DEFAULT")
                        : TEXT("BP_INVALID_VARIABLE_DEFAULT"),
                TEXT("error"),
                FString::Printf(
                    TEXT("Variable '%s' has an invalid default: %s"),
                    *Variable.VarName.ToString(),
                    NormalizeError.Message.IsEmpty()
                        ? TEXT("stored value is not canonical for its declared type")
                        : *NormalizeError.Message),
                TEXT("Set the variable through the canonical Blueprint default schema for its declared type."),
                FString(),
                FString(),
                FString(),
                UE::MCPython::Blueprint2::DescribeVariableTarget(
                    Blueprint, Variable).Id)));
        }
    }
}
}

void UE::MCPython::Blueprint2::CollectStructuralHealthIssues(
    UBlueprint* Blueprint,
    TArray<TSharedPtr<FJsonValue>>& OutIssues)
{
    AddSCSHealthIssues(Blueprint, OutIssues);
    if (!Blueprint)
    {
        return;
    }
    AddGraphHealthIssues(Blueprint, OutIssues);
    AddInterfaceHealthIssues(Blueprint, OutIssues);
    AddMemberCollisionHealthIssues(Blueprint, OutIssues);
    AddVariableDefaultHealthIssues(Blueprint, OutIssues);
}

void UE::MCPython::Blueprint2::NormalizeHealthIssues(
    TArray<TSharedPtr<FJsonValue>>& Issues)
{
    NormalizeHealthIssuesImpl(Issues);
}

FString UMCPythonHelper::CompileBlueprint(UBlueprint* Blueprint)
{
    using namespace UE::MCPython::Blueprint2;
    if (!Blueprint)
    {
        return InvalidBlueprintResult();
    }

    FCompilerResultsLog Results;
    Results.bSilentMode = true;
    Results.bAnnotateMentionedNodes = false;
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint,
        EBlueprintCompileOptions::None,
        &Results);

    TArray<TSharedPtr<FJsonValue>> Diagnostics;
    TArray<TSharedPtr<FJsonValue>> WarningRecords;
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
    for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
    {
        const EMessageSeverity::Type Severity = Message->GetSeverity();
        const bool bWarning = Severity == EMessageSeverity::Warning ||
            Severity == EMessageSeverity::PerformanceWarning;
        const bool bError = Severity == EMessageSeverity::Error;
        if (!bWarning && !bError)
        {
            continue;
        }
        const TSharedRef<FJsonObject> Diagnostic = MakeDiagnostic(
            Blueprint, Message, bWarning);
        Diagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
        if (bWarning)
        {
            ++WarningCount;
            WarningRecords.Add(MakeShared<FJsonValueObject>(
                MakeWarningRecord(Diagnostic)));
        }
        else
        {
            ++ErrorCount;
        }
    }

    const FString Status = CompileStatus(Blueprint);
    const bool bHasErrors = ErrorCount > 0 || Blueprint->Status == BS_Error;
    const FString LegacyMessage = bHasErrors
        ? TEXT("Blueprint compilation failed. Check the output log for details.")
        : TEXT("Blueprint compiled successfully.");
    const FString Summary = bHasErrors
        ? FString::Printf(
            TEXT("Blueprint compilation failed with %d error(s) and %d warning(s)."),
            ErrorCount,
            WarningCount)
        : WarningCount > 0
            ? FString::Printf(
                TEXT("Blueprint compiled with %d warning(s)."), WarningCount)
            : TEXT("Blueprint compiled successfully.");

    TSharedRef<FJsonObject> Result = bHasErrors
        ? MakeFailure(
            TEXT("COMPILE_FAILED"),
            FString(),
            LegacyMessage,
            false,
            TEXT("Inspect the structured diagnostics, repair the cited nodes or pins, and compile again."))
        : MakeSuccess(Summary);
    Result->SetBoolField(TEXT("success"), !bHasErrors);
    Result->SetStringField(TEXT("status"), Status);
    Result->SetStringField(TEXT("message"), LegacyMessage);
    Result->SetStringField(TEXT("summary"), Summary);

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("result_status"), Status);
    Data->SetNumberField(TEXT("error_count"), ErrorCount);
    Data->SetNumberField(TEXT("warning_count"), WarningCount);
    Data->SetNumberField(TEXT("diagnostic_count"), Diagnostics.Num());
    Result->SetObjectField(TEXT("data"), Data);
    Result->SetArrayField(TEXT("diagnostics"), Diagnostics);
    Result->SetArrayField(TEXT("warnings"), WarningRecords);

    const FString AssetPath = Blueprint->GetPathName();
    TArray<TSharedPtr<FJsonValue>> NextActions;
    if (bHasErrors)
    {
        NextActions.Add(MakeShared<FJsonValueObject>(
            MakeInspectNextAction(AssetPath, Diagnostics)));
        NextActions.Add(MakeShared<FJsonValueObject>(
            MakeNextAction(TEXT("get_blueprint_health"), AssetPath)));
    }
    else
    {
        NextActions.Add(MakeShared<FJsonValueObject>(
            MakeNextAction(TEXT("get_blueprint_health"), AssetPath)));
        NextActions.Add(MakeShared<FJsonValueObject>(
            MakeNextAction(TEXT("snapshot_blueprint_graph"), AssetPath)));
    }
    Result->SetArrayField(TEXT("next_actions"), NextActions);
    return SerializeResult(Result);
}

FString UMCPythonHelper::GetBlueprintHealth(UBlueprint* Blueprint)
{
    using namespace UE::MCPython::Blueprint2;
    if (!Blueprint)
    {
        return InvalidBlueprintResult();
    }

    const TSharedPtr<FJsonObject> CompileResult = ParseJsonObject(
        CompileBlueprint(Blueprint));
    if (!CompileResult)
    {
        return SerializeResult(MakeFailure(
            TEXT("INTERNAL_ERROR"),
            TEXT("compile"),
            TEXT("Blueprint compilation returned invalid JSON."),
            false,
            TEXT("Inspect the Unreal editor log and retry the health check.")));
    }

    const bool bCompileSucceeded =
        CompileResult->GetBoolField(TEXT("success"));
    const FString CompileStatus =
        CompileResult->GetStringField(TEXT("status"));
    const TArray<TSharedPtr<FJsonValue>>& Diagnostics =
        CompileResult->GetArrayField(TEXT("diagnostics"));
    TArray<TSharedPtr<FJsonValue>> Issues;
    for (const TSharedPtr<FJsonValue>& Value : Diagnostics)
    {
        const TSharedPtr<FJsonObject> Diagnostic = Value.IsValid()
            ? Value->AsObject()
            : nullptr;
        if (!Diagnostic)
        {
            continue;
        }
        const TSharedRef<FJsonObject> Issue = MakeHealthIssueFromDiagnostic(
            Diagnostic.ToSharedRef());
        Issues.Add(MakeShared<FJsonValueObject>(Issue));
    }

    CollectStructuralHealthIssues(Blueprint, Issues);
    NormalizeHealthIssues(Issues);
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
    for (const TSharedPtr<FJsonValue>& Value : Issues)
    {
        const TSharedPtr<FJsonObject> Issue = Value->AsObject();
        if (Issue->GetStringField(TEXT("severity")) == TEXT("error"))
        {
            ++ErrorCount;
        }
        else
        {
            ++WarningCount;
        }
    }

    const bool bHealthy = ErrorCount == 0;
    const FString Summary = bHealthy
        ? WarningCount > 0
            ? FString::Printf(
                TEXT("Blueprint health check found %d warning(s)."),
                WarningCount)
            : TEXT("Blueprint health check found no issues.")
        : FString::Printf(
            TEXT("Blueprint health check found %d error(s) and %d warning(s)."),
            ErrorCount,
            WarningCount);
    TSharedRef<FJsonObject> Result = bCompileSucceeded
        ? MakeSuccess(Summary)
        : MakeFailure(
            TEXT("COMPILE_FAILED"),
            FString(),
            TEXT("Blueprint compilation failed during the health check."),
            false,
            TEXT("Repair the cited Blueprint issues, then run the health check again."));

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetBoolField(TEXT("healthy"), bHealthy);
    Data->SetStringField(TEXT("compile_status"), CompileStatus);
    Data->SetNumberField(TEXT("issue_count"), Issues.Num());
    Data->SetNumberField(TEXT("error_count"), ErrorCount);
    Data->SetNumberField(TEXT("warning_count"), WarningCount);
    Data->SetArrayField(TEXT("issues"), Issues);
    Result->SetObjectField(TEXT("data"), Data);

    const TArray<TSharedPtr<FJsonValue>>* CompileWarnings = nullptr;
    if (CompileResult->TryGetArrayField(TEXT("warnings"), CompileWarnings) &&
        CompileWarnings)
    {
        Result->SetArrayField(TEXT("warnings"), *CompileWarnings);
    }
    const TArray<TSharedPtr<FJsonValue>>* NextActions = nullptr;
    if (CompileResult->TryGetArrayField(TEXT("next_actions"), NextActions) &&
        NextActions)
    {
        Result->SetArrayField(TEXT("next_actions"), *NextActions);
    }
    return SerializeResult(Result);
}
