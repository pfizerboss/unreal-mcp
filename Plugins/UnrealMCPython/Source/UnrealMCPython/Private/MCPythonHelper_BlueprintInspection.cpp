// Copyright (c) 2025 GenOrca (by zenoengine). All Rights Reserved.

#include "MCPythonHelper.h"
#include "MCPythonHelperInternal.h"
#include "MCPythonBlueprint2Internal.h"

#include "EdGraphSchema_K2.h"
#include "Components/SceneComponent.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Composite.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

namespace
{
FString BlueprintCompileStatus(const UBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        return TEXT("Unknown");
    }
    switch (Blueprint->Status)
    {
    case BS_Dirty: return TEXT("Dirty");
    case BS_Error: return TEXT("Error");
    case BS_UpToDate: return TEXT("UpToDate");
    case BS_BeingCreated: return TEXT("BeingCreated");
    case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
    default: return TEXT("Unknown");
    }
}

FString TargetIdKind(
    UE::MCPython::Blueprint2::ETargetKind Kind,
    const FString& Id)
{
    using UE::MCPython::Blueprint2::ETargetKind;
    if (Id.StartsWith(TEXT("fallback:")))
    {
        return TEXT("qualified_name_fallback");
    }
    switch (Kind)
    {
    case ETargetKind::Graph: return TEXT("graph_guid");
    case ETargetKind::Node: return TEXT("node_guid");
    case ETargetKind::Pin: return TEXT("pin_guid");
    case ETargetKind::Variable: return TEXT("variable_guid");
    case ETargetKind::Component: return TEXT("scs_variable_guid");
    case ETargetKind::Interface: return TEXT("interface_path");
    }
    return {};
}

void SetTargetMetadata(
    const TSharedPtr<FJsonObject>& Object,
    UE::MCPython::Blueprint2::ETargetKind Kind,
    const FString& Id,
    const FString& Prefix = FString())
{
    const FString IdKindField = Prefix.IsEmpty()
        ? TEXT("id_kind")
        : Prefix + TEXT("_id_kind");
    const FString StableField = Prefix.IsEmpty()
        ? TEXT("stable")
        : Prefix + TEXT("_stable");
    Object->SetStringField(IdKindField, TargetIdKind(Kind, Id));
    Object->SetBoolField(
        StableField,
        !Id.IsEmpty() && !Id.StartsWith(TEXT("fallback:")));
}

void SetTargetMetadata(
    const TSharedPtr<FJsonObject>& Object,
    UE::MCPython::Blueprint2::ETargetKind Kind,
    const UE::MCPython::Blueprint2::FTargetRef& Target,
    const FString& Prefix = FString())
{
    SetTargetMetadata(Object, Kind, Target.Id, Prefix);
    if (!Target.Id.StartsWith(TEXT("fallback:")))
    {
        return;
    }
    const FString FieldPrefix = Prefix.IsEmpty() ? FString() : Prefix + TEXT("_");
    Object->SetStringField(FieldPrefix + TEXT("owner_id"), Target.OwnerId);
    Object->SetStringField(FieldPrefix + TEXT("name"), Target.Name);
    Object->SetStringField(FieldPrefix + TEXT("type_path"), Target.TypePath);
}
}

FString UMCPythonHelper::GetBlueprintGraphInfo(UBlueprint* Blueprint, const FString& GraphName)
{
    if (!Blueprint)
        return MakeJsonError(TEXT("Invalid Blueprint."));

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
        return MakeJsonError(FString::Printf(TEXT("Graph '%s' not found in Blueprint."), *GraphName));

    const UE::MCPython::Blueprint2::FTargetRef GraphTarget =
        UE::MCPython::Blueprint2::DescribeGraphTarget(Blueprint, Graph);
    const FString GraphId = GraphTarget.Id;
    TArray<TSharedPtr<FJsonValue>> NodesArr;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node) continue;

        TSharedPtr<FJsonObject> NodeObj = MakeShareable(new FJsonObject());
        const UE::MCPython::Blueprint2::FTargetRef NodeTarget =
            UE::MCPython::Blueprint2::DescribeNodeTarget(Blueprint, Node);
        const FString NodeId = NodeTarget.Id;
        NodeObj->SetStringField(TEXT("stable_id"), NodeId);
        NodeObj->SetStringField(TEXT("node_id"), NodeId);
        NodeObj->SetStringField(TEXT("graph_id"), GraphId);
        SetTargetMetadata(
            NodeObj,
            UE::MCPython::Blueprint2::ETargetKind::Node,
            NodeTarget);
        SetTargetMetadata(
            NodeObj,
            UE::MCPython::Blueprint2::ETargetKind::Graph,
            GraphTarget,
            TEXT("graph"));
        NodeObj->SetStringField(TEXT("node_name"), Node->GetName());
        NodeObj->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
        NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
        if (!Node->NodeComment.IsEmpty())
            NodeObj->SetStringField(TEXT("comment"), Node->NodeComment);
        NodeObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
        NodeObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);

        TArray<TSharedPtr<FJsonValue>> PinsArr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->bHidden) continue;
            TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject());
            const UE::MCPython::Blueprint2::FTargetRef PinTarget =
                UE::MCPython::Blueprint2::DescribePinTarget(Blueprint, Pin);
            const FString PinId = PinTarget.Id;
            PinObj->SetStringField(TEXT("stable_id"), PinId);
            PinObj->SetStringField(TEXT("pin_id"), PinId);
            PinObj->SetStringField(TEXT("graph_id"), GraphId);
            PinObj->SetStringField(TEXT("node_id"), NodeId);
            SetTargetMetadata(
                PinObj,
                UE::MCPython::Blueprint2::ETargetKind::Pin,
                PinTarget);
            SetTargetMetadata(
                PinObj,
                UE::MCPython::Blueprint2::ETargetKind::Graph,
                GraphTarget,
                TEXT("graph"));
            SetTargetMetadata(
                PinObj,
                UE::MCPython::Blueprint2::ETargetKind::Node,
                NodeTarget,
                TEXT("node"));
            PinObj->SetStringField(TEXT("pin_name"), Pin->GetName());
            FString Friendly = Pin->PinFriendlyName.ToString();
            if (!Friendly.IsEmpty())
                PinObj->SetStringField(TEXT("friendly_name"), Friendly);
            PinObj->SetStringField(TEXT("direction"), (Pin->Direction == EGPD_Input) ? TEXT("Input") : TEXT("Output"));
            PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
            if (Pin->PinType.PinSubCategoryObject.IsValid())
                PinObj->SetStringField(TEXT("sub_type"), Pin->PinType.PinSubCategoryObject->GetName());
            if (!Pin->DefaultValue.IsEmpty())
                PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
            if (Pin->DefaultObject)
                PinObj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());

            // Linked pins
            if (Pin->LinkedTo.Num() > 0)
            {
                TArray<TSharedPtr<FJsonValue>> LinksArr;
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    if (!Linked || !Linked->GetOwningNode()) continue;
                    TSharedPtr<FJsonObject> LinkObj = MakeShareable(new FJsonObject());
                    UEdGraphNode* LinkedNode = Linked->GetOwningNode();
                    UEdGraph* LinkedGraph = LinkedNode->GetGraph();
                    const UE::MCPython::Blueprint2::FTargetRef LinkedGraphTarget =
                        UE::MCPython::Blueprint2::DescribeGraphTarget(
                            Blueprint, LinkedGraph);
                    const UE::MCPython::Blueprint2::FTargetRef LinkedNodeTarget =
                        UE::MCPython::Blueprint2::DescribeNodeTarget(
                            Blueprint, LinkedNode);
                    const UE::MCPython::Blueprint2::FTargetRef LinkedPinTarget =
                        UE::MCPython::Blueprint2::DescribePinTarget(
                            Blueprint, Linked);
                    LinkObj->SetStringField(
                        TEXT("graph_id"),
                        LinkedGraphTarget.Id);
                    LinkObj->SetStringField(
                        TEXT("node_id"),
                        LinkedNodeTarget.Id);
                    LinkObj->SetStringField(
                        TEXT("pin_id"),
                        LinkedPinTarget.Id);
                    SetTargetMetadata(
                        LinkObj,
                        UE::MCPython::Blueprint2::ETargetKind::Graph,
                        LinkedGraphTarget,
                        TEXT("graph"));
                    SetTargetMetadata(
                        LinkObj,
                        UE::MCPython::Blueprint2::ETargetKind::Node,
                        LinkedNodeTarget,
                        TEXT("node"));
                    SetTargetMetadata(
                        LinkObj,
                        UE::MCPython::Blueprint2::ETargetKind::Pin,
                        LinkedPinTarget,
                        TEXT("pin"));
                    LinkObj->SetStringField(TEXT("node_name"), LinkedNode->GetName());
                    LinkObj->SetStringField(TEXT("pin_name"), Linked->GetName());
                    LinksArr.Add(MakeShareable(new FJsonValueObject(LinkObj)));
                }
                PinObj->SetArrayField(TEXT("linked_to"), LinksArr);
            }
            PinsArr.Add(MakeShareable(new FJsonValueObject(PinObj)));
        }
        NodeObj->SetArrayField(TEXT("pins"), PinsArr);
        NodesArr.Add(MakeShareable(new FJsonValueObject(NodeObj)));
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("graph_name"), GraphName);
    Result->SetStringField(TEXT("graph_id"), GraphId);
    SetTargetMetadata(
        Result,
        UE::MCPython::Blueprint2::ETargetKind::Graph,
        GraphTarget);
    Result->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
    Result->SetArrayField(TEXT("nodes"), NodesArr);
    return SerializeJsonObj(Result);
}

FString UMCPythonHelper::ListCallableFunctions(UBlueprint* Blueprint, const FString& Filter)
{
    if (!Blueprint)
        return MakeJsonError(TEXT("Invalid Blueprint."));

    UClass* GenClass = Blueprint->GeneratedClass;
    if (!GenClass)
        return MakeJsonError(TEXT("Blueprint has no generated class. Compile it first."));

    TArray<TSharedPtr<FJsonValue>> FuncsArr;
    FString FilterLower = Filter.ToLower();
    TMap<FName, UEdGraph*> LocalFunctionGraphs;
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph)
        {
            LocalFunctionGraphs.Add(Graph->GetFName(), Graph);
        }
    }

    // Collect from the generated class and all parent classes
    for (UClass* Cls = GenClass; Cls; Cls = Cls->GetSuperClass())
    {
        for (TFieldIterator<UFunction> FuncIt(Cls, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
        {
            UFunction* Func = *FuncIt;
            if (!Func || !Func->HasAnyFunctionFlags(FUNC_BlueprintCallable))
                continue;

            FString FuncName = Func->GetName();
            FString ClassName = Cls->GetName();

            if (!FilterLower.IsEmpty())
            {
                if (!FuncName.ToLower().Contains(FilterLower) && !ClassName.ToLower().Contains(FilterLower))
                    continue;
            }

            TSharedPtr<FJsonObject> FuncObj = MakeShareable(new FJsonObject());
            FuncObj->SetStringField(TEXT("function_name"), FuncName);
            FuncObj->SetStringField(TEXT("class_name"), ClassName);
            FuncObj->SetBoolField(TEXT("is_pure"), Func->HasAnyFunctionFlags(FUNC_BlueprintPure));
            FuncObj->SetBoolField(TEXT("is_static"), Func->HasAnyFunctionFlags(FUNC_Static));

            UEdGraph* const* LocalGraph = Cls == GenClass
                ? LocalFunctionGraphs.Find(Func->GetFName())
                : nullptr;
            const bool bTargetable = LocalGraph && *LocalGraph;
            FuncObj->SetBoolField(TEXT("targetable"), bTargetable);
            if (bTargetable)
            {
                const UE::MCPython::Blueprint2::FTargetRef FunctionTarget =
                    UE::MCPython::Blueprint2::DescribeGraphTarget(
                        Blueprint,
                        *LocalGraph);
                FuncObj->SetStringField(TEXT("stable_id"), FunctionTarget.Id);
                FuncObj->SetStringField(TEXT("function_id"), FunctionTarget.Id);
                SetTargetMetadata(
                    FuncObj,
                    UE::MCPython::Blueprint2::ETargetKind::Graph,
                    FunctionTarget);
            }
            else
            {
                FuncObj->SetStringField(TEXT("id_kind"), TEXT("unavailable"));
                FuncObj->SetBoolField(TEXT("stable"), false);
            }

            // Parameters
            TArray<TSharedPtr<FJsonValue>> ParamsArr;
            for (TFieldIterator<FProperty> PropIt(Func); PropIt; ++PropIt)
            {
                FProperty* Prop = *PropIt;
                TSharedPtr<FJsonObject> ParamObj = MakeShareable(new FJsonObject());
                ParamObj->SetStringField(TEXT("name"), Prop->GetName());
                ParamObj->SetStringField(TEXT("type"), Prop->GetCPPType());
                ParamObj->SetBoolField(TEXT("is_return"), Prop->HasAnyPropertyFlags(CPF_ReturnParm));
                ParamObj->SetBoolField(TEXT("is_output"), Prop->HasAnyPropertyFlags(CPF_OutParm));
                ParamsArr.Add(MakeShareable(new FJsonValueObject(ParamObj)));
            }
            FuncObj->SetArrayField(TEXT("parameters"), ParamsArr);
            FuncsArr.Add(MakeShareable(new FJsonValueObject(FuncObj)));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetBoolField(TEXT("success"), true);
    Result->SetNumberField(TEXT("count"), FuncsArr.Num());
    Result->SetArrayField(TEXT("functions"), FuncsArr);
    return SerializeJsonObj(Result);
}

FString UMCPythonHelper::ListBlueprintVariables(UBlueprint* Blueprint)
{
    if (!Blueprint)
        return MakeJsonError(TEXT("Invalid Blueprint."));

    TArray<TSharedPtr<FJsonValue>> VarsArr;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        TSharedPtr<FJsonObject> VarObj = MakeShareable(new FJsonObject());
        const UE::MCPython::Blueprint2::FTargetRef VariableTarget =
            UE::MCPython::Blueprint2::DescribeVariableTarget(Blueprint, Var);
        const FString VariableId = VariableTarget.Id;
        VarObj->SetStringField(TEXT("stable_id"), VariableId);
        VarObj->SetStringField(TEXT("variable_id"), VariableId);
        SetTargetMetadata(
            VarObj,
            UE::MCPython::Blueprint2::ETargetKind::Variable,
            VariableTarget);
        VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
        VarObj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
        if (Var.VarType.PinSubCategoryObject.IsValid())
            VarObj->SetStringField(TEXT("sub_type"), Var.VarType.PinSubCategoryObject->GetName());
        VarObj->SetBoolField(TEXT("is_array"), Var.VarType.IsArray());
        VarObj->SetBoolField(TEXT("is_set"), Var.VarType.IsSet());
        VarObj->SetBoolField(TEXT("is_map"), Var.VarType.IsMap());
        if (!Var.DefaultValue.IsEmpty())
            VarObj->SetStringField(TEXT("default_value"), Var.DefaultValue);
        VarsArr.Add(MakeShareable(new FJsonValueObject(VarObj)));
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetBoolField(TEXT("success"), true);
    Result->SetNumberField(TEXT("count"), VarsArr.Num());
    Result->SetArrayField(TEXT("variables"), VarsArr);
    return SerializeJsonObj(Result);
}

FString UMCPythonHelper::GetBlueprint2Capabilities(UBlueprint* Blueprint)
{
    return UE::MCPython::Blueprint2::SerializeResult(
        UE::MCPython::Blueprint2::BuildCapabilities(Blueprint));
}

FString UMCPythonHelper::GetBlueprintBrief(UBlueprint* Blueprint)
{
    using namespace UE::MCPython::Blueprint2;

    if (!Blueprint)
    {
        return SerializeResult(MakeFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("asset_path"),
            TEXT("Blueprint is required."),
            false,
            TEXT("Provide an existing Blueprint asset.")));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("blueprint_class_path"), Blueprint->GetClass()->GetPathName());
    Data->SetStringField(
        TEXT("parent_class_path"),
        Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());
    Data->SetStringField(
        TEXT("generated_class_path"),
        Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString());
    Data->SetStringField(
        TEXT("skeleton_class_path"),
        Blueprint->SkeletonGeneratedClass
            ? Blueprint->SkeletonGeneratedClass->GetPathName()
            : FString());
    Data->SetStringField(TEXT("compile_status"), BlueprintCompileStatus(Blueprint));

    TArray<TSharedPtr<FJsonValue>> InterfacePaths;
    for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
    {
        if (const UClass* InterfaceClass = Description.Interface.Get())
        {
            InterfacePaths.Add(MakeShared<FJsonValueString>(InterfaceClass->GetPathName()));
        }
    }
    Data->SetArrayField(TEXT("interfaces"), InterfacePaths);

    int32 ComponentCount = 0;
    TArray<TSharedPtr<FJsonValue>> TopLevelComponents;
    if (const USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
    {
        for (const USCS_Node* Node : SCS->GetAllNodes())
        {
            ComponentCount += Node != nullptr ? 1 : 0;
        }
        for (const USCS_Node* Node : SCS->GetRootNodes())
        {
            if (Node)
            {
                TopLevelComponents.Add(MakeShared<FJsonValueString>(
                    Node->GetVariableName().ToString()));
            }
        }
    }
    Data->SetArrayField(TEXT("top_level_components"), TopLevelComponents);

    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);
    TSet<const UEdGraph*> SeenGraphs;
    TArray<TSharedPtr<FJsonValue>> GraphNames;
    int32 NodeCount = 0;
    int32 EventCount = 0;
    for (UEdGraph* Graph : AllGraphs)
    {
        if (!Graph || SeenGraphs.Contains(Graph))
        {
            continue;
        }
        SeenGraphs.Add(Graph);
        if (!UEdGraph::GetOuterGraph(Graph))
        {
            GraphNames.Add(MakeShared<FJsonValueString>(Graph->GetName()));
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            ++NodeCount;
            EventCount += Node->IsA<UK2Node_CustomEvent>() ? 1 : 0;
        }
    }
    Data->SetArrayField(TEXT("graphs"), GraphNames);

    int32 FunctionCount = 0;
    for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        FunctionCount += Graph != nullptr ? 1 : 0;
    }
    int32 MacroCount = 0;
    for (const UEdGraph* Graph : Blueprint->MacroGraphs)
    {
        MacroCount += Graph != nullptr ? 1 : 0;
    }
    int32 DispatcherCount = 0;
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        DispatcherCount +=
            Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate ? 1 : 0;
    }

    TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
    Counts->SetNumberField(TEXT("variables"), Blueprint->NewVariables.Num());
    Counts->SetNumberField(TEXT("components"), ComponentCount);
    Counts->SetNumberField(TEXT("functions"), FunctionCount);
    Counts->SetNumberField(TEXT("macros"), MacroCount);
    Counts->SetNumberField(TEXT("events"), EventCount);
    Counts->SetNumberField(TEXT("dispatchers"), DispatcherCount);
    Counts->SetNumberField(TEXT("interfaces"), InterfacePaths.Num());
    Counts->SetNumberField(TEXT("graphs"), SeenGraphs.Num());
    Counts->SetNumberField(TEXT("nodes"), NodeCount);
    Data->SetObjectField(TEXT("counts"), Counts);
    Data->SetObjectField(TEXT("capabilities"), BuildCapabilities(Blueprint));

    return SerializeResult(MakeSuccess(TEXT("Blueprint brief returned."), Data));
}

namespace
{
using UE::MCPython::Blueprint2::ETargetKind;
using UE::MCPython::Blueprint2::FPageRequest;
using UE::MCPython::Blueprint2::FTargetRef;

struct FInspectQuery
{
    FString Op;
    FString GraphId;
    FString MemberId;
    FString NodeId;
    FString Kind;
    FString NamePattern;
    FString ClassPath;
    FString Cursor;
    int32 Limit = 100;
    bool bDetailed = false;
    FString Digest;
    FString LastId;
};

struct FInspectRecord
{
    enum class ESource
    {
        Ready,
        Overview,
        Variable,
        Component,
        Graph,
        Interface,
        Node,
        Pin,
        Connection,
    };

    FString Id;
    FString QualifiedName;
    FString Kind;
    FString Name;
    FString ClassPath;
    TArray<FString> SearchNames;
    TArray<FString> GraphIds;
    TArray<FString> MemberIds;
    TArray<FString> NodeIds;
    TSharedPtr<FJsonObject> Json;
    ESource Source = ESource::Ready;
    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    UEdGraphNode* Node = nullptr;
    UEdGraphPin* Pin = nullptr;
    UEdGraphPin* OtherPin = nullptr;
    const FBPVariableDescription* Variable = nullptr;
    USCS_Node* Component = nullptr;
    const FBPInterfaceDescription* Interface = nullptr;
    FString SourceOp;
    bool bHierarchy = false;
};

FString InspectInvalid(
    const FString& Path,
    const FString& Message,
    const FString& Hint = TEXT("Correct the inspection request and retry."))
{
    using namespace UE::MCPython::Blueprint2;
    return SerializeResult(MakeFailure(
        TEXT("INVALID_INPUT"), Path, Message, false, Hint));
}

TSharedRef<FJsonObject> ClassTypeSummary(const FString& ClassPath)
{
    const TSharedRef<FJsonObject> Type = MakeShared<FJsonObject>();
    Type->SetStringField(TEXT("class_path"), ClassPath);
    return Type;
}

TSharedRef<FJsonObject> PinTypeSummary(const FEdGraphPinType& PinType)
{
    const TSharedRef<FJsonObject> Type = MakeShared<FJsonObject>();
    Type->SetStringField(TEXT("kind"), PinType.PinCategory.ToString());
    Type->SetStringField(TEXT("sub_kind"), PinType.PinSubCategory.ToString());
    Type->SetStringField(
        TEXT("class_path"),
        PinType.PinSubCategoryObject.IsValid()
            ? PinType.PinSubCategoryObject->GetPathName()
            : FString());
    FString Container = TEXT("scalar");
    if (PinType.IsArray())
    {
        Container = TEXT("array");
    }
    else if (PinType.IsSet())
    {
        Container = TEXT("set");
    }
    else if (PinType.IsMap())
    {
        Container = TEXT("map");
    }
    Type->SetStringField(TEXT("container"), Container);
    Type->SetBoolField(TEXT("reference"), PinType.bIsReference);
    Type->SetBoolField(TEXT("const"), PinType.bIsConst);
    return Type;
}

void SetNullableString(
    const TSharedRef<FJsonObject>& Object,
    const FString& Field,
    const FString& Value)
{
    if (Value.IsEmpty())
    {
        Object->SetField(Field, MakeShared<FJsonValueNull>());
    }
    else
    {
        Object->SetStringField(Field, Value);
    }
}

void SetIdentity(
    const TSharedRef<FJsonObject>& Object,
    ETargetKind TargetKind,
    const FTargetRef& Target,
    const FString& Kind,
    const FString& Name,
    const TSharedRef<FJsonObject>& Type,
    const FString& SpecificIdField = FString())
{
    Object->SetStringField(TEXT("id"), Target.Id);
    Object->SetStringField(TEXT("stable_id"), Target.Id);
    if (!SpecificIdField.IsEmpty())
    {
        Object->SetStringField(SpecificIdField, Target.Id);
    }
    SetTargetMetadata(Object, TargetKind, Target);
    Object->SetStringField(TEXT("kind"), Kind);
    Object->SetStringField(TEXT("name"), Name);
    Object->SetObjectField(TEXT("type"), Type);
}

FString NodeKind(const UEdGraphNode* Node)
{
    if (Node && Node->IsA<UK2Node_CustomEvent>())
    {
        return TEXT("custom_event");
    }
    if (Node && Node->IsA<UK2Node_Event>())
    {
        return TEXT("event");
    }
    return TEXT("node");
}

TSharedRef<FJsonObject> MakePinJson(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UEdGraphNode* Node,
    UEdGraphPin* Pin,
    bool bDetailed)
{
    const FTargetRef GraphTarget =
        UE::MCPython::Blueprint2::DescribeGraphTarget(Blueprint, Graph);
    const FTargetRef NodeTarget =
        UE::MCPython::Blueprint2::DescribeNodeTarget(Blueprint, Node);
    const FTargetRef PinTarget =
        UE::MCPython::Blueprint2::DescribePinTarget(Blueprint, Pin);
    const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    SetIdentity(
        Json,
        ETargetKind::Pin,
        PinTarget,
        TEXT("pin"),
        Pin->GetName(),
        PinTypeSummary(Pin->PinType),
        TEXT("pin_id"));
    Json->SetStringField(TEXT("graph_id"), GraphTarget.Id);
    Json->SetStringField(TEXT("node_id"), NodeTarget.Id);
    SetTargetMetadata(Json, ETargetKind::Graph, GraphTarget, TEXT("graph"));
    SetTargetMetadata(Json, ETargetKind::Node, NodeTarget, TEXT("node"));
    Json->SetStringField(
        TEXT("direction"),
        Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
    Json->SetNumberField(TEXT("link_count"), Pin->LinkedTo.Num());
    if (bDetailed)
    {
        Json->SetStringField(TEXT("friendly_name"), Pin->PinFriendlyName.ToString());
        SetNullableString(Json, TEXT("default"), Pin->DefaultValue);
        Json->SetStringField(
            TEXT("default_object_path"),
            Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString());
        SetNullableString(
            Json,
            TEXT("default_text"),
            Pin->DefaultTextValue.IsEmpty()
                ? FString()
                : Pin->DefaultTextValue.ToString());
        TArray<TSharedPtr<FJsonValue>> LinkedPinIds;
        TArray<TSharedPtr<FJsonValue>> LinkedNodeIds;
        TSet<FString> SeenNodeIds;
        for (UEdGraphPin* Linked : Pin->LinkedTo)
        {
            if (Linked)
            {
                LinkedPinIds.Add(MakeShared<FJsonValueString>(
                    UE::MCPython::Blueprint2::DescribePinTarget(
                        Blueprint, Linked).Id));
                if (UEdGraphNode* LinkedNode = Linked->GetOwningNode())
                {
                    const FString LinkedNodeId =
                        UE::MCPython::Blueprint2::DescribeNodeTarget(
                            Blueprint, LinkedNode).Id;
                    if (!SeenNodeIds.Contains(LinkedNodeId))
                    {
                        SeenNodeIds.Add(LinkedNodeId);
                        LinkedNodeIds.Add(MakeShared<FJsonValueString>(
                            LinkedNodeId));
                    }
                }
            }
        }
        LinkedPinIds.Sort([](
            const TSharedPtr<FJsonValue>& A,
            const TSharedPtr<FJsonValue>& B)
        {
            return A->AsString() < B->AsString();
        });
        LinkedNodeIds.Sort([](
            const TSharedPtr<FJsonValue>& A,
            const TSharedPtr<FJsonValue>& B)
        {
            return A->AsString() < B->AsString();
        });
        Json->SetArrayField(TEXT("linked_pin_ids"), LinkedPinIds);
        Json->SetArrayField(TEXT("linked_node_ids"), LinkedNodeIds);
    }
    return Json;
}

FInspectRecord MakeNodeRecord(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UEdGraphNode* Node,
    bool bDetailed,
    bool bMaterialize = true)
{
    const FTargetRef GraphTarget =
        UE::MCPython::Blueprint2::DescribeGraphTarget(Blueprint, Graph);
    const FTargetRef NodeTarget =
        UE::MCPython::Blueprint2::DescribeNodeTarget(Blueprint, Node);
    const FString ClassPath = Node->GetClass()->GetPathName();
    FInspectRecord Record;
    Record.Id = NodeTarget.Id;
    Record.Name = Node->GetName();
    Record.QualifiedName = GraphTarget.Id + TEXT("::") + Record.Name;
    Record.Kind = NodeKind(Node);
    Record.ClassPath = ClassPath;
    Record.SearchNames.Add(
        Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
    Record.GraphIds.Add(GraphTarget.Id);
    Record.MemberIds.Add(GraphTarget.Id);
    Record.MemberIds.Add(NodeTarget.Id);
    Record.NodeIds.Add(NodeTarget.Id);
    FString EventName;
    if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
    {
        EventName = CustomEvent->CustomFunctionName.ToString();
    }
    else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
    {
        const FName ReferencedName = Event->EventReference.GetMemberName();
        EventName = (
            ReferencedName != NAME_None
                ? ReferencedName
                : Event->CustomFunctionName).ToString();
    }
    if (!EventName.IsEmpty())
    {
        Record.SearchNames.Add(EventName);
    }
    if (!bMaterialize)
    {
        return Record;
    }
    Record.Json = MakeShared<FJsonObject>();
    SetIdentity(
        Record.Json.ToSharedRef(),
        ETargetKind::Node,
        NodeTarget,
        Record.Kind,
        Record.Name,
        ClassTypeSummary(ClassPath),
        TEXT("node_id"));
    Record.Json->SetStringField(TEXT("graph_id"), GraphTarget.Id);
    Record.Json->SetStringField(TEXT("class_path"), ClassPath);
    if (!EventName.IsEmpty())
    {
        Record.Json->SetStringField(TEXT("event_name"), EventName);
    }
    SetTargetMetadata(
        Record.Json,
        ETargetKind::Graph,
        GraphTarget,
        TEXT("graph"));
    int32 LinkCount = 0;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        LinkCount += Pin ? Pin->LinkedTo.Num() : 0;
    }
    Record.Json->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
    Record.Json->SetNumberField(TEXT("link_count"), LinkCount);
    if (bDetailed)
    {
        Record.Json->SetStringField(
            TEXT("title"),
            Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
        const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
        Position->SetNumberField(TEXT("x"), Node->NodePosX);
        Position->SetNumberField(TEXT("y"), Node->NodePosY);
        Record.Json->SetObjectField(TEXT("position"), Position);
        Record.Json->SetStringField(TEXT("comment"), Node->NodeComment);
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && !Pin->bHidden)
            {
                Pins.Add(MakeShared<FJsonValueObject>(
                    MakePinJson(Blueprint, Graph, Node, Pin, true)));
            }
        }
        Pins.Sort([](
            const TSharedPtr<FJsonValue>& A,
            const TSharedPtr<FJsonValue>& B)
        {
            return A->AsObject()->GetStringField(TEXT("id")) <
                B->AsObject()->GetStringField(TEXT("id"));
        });
        Record.Json->SetArrayField(TEXT("pins"), Pins);
    }
    return Record;
}

FInspectRecord MakePinRecord(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UEdGraphNode* Node,
    UEdGraphPin* Pin,
    bool bDetailed,
    bool bMaterialize = true)
{
    const FTargetRef GraphTarget =
        UE::MCPython::Blueprint2::DescribeGraphTarget(Blueprint, Graph);
    const FTargetRef NodeTarget =
        UE::MCPython::Blueprint2::DescribeNodeTarget(Blueprint, Node);
    const FTargetRef PinTarget =
        UE::MCPython::Blueprint2::DescribePinTarget(Blueprint, Pin);
    FInspectRecord Record;
    Record.Id = PinTarget.Id;
    Record.Name = Pin->GetName();
    Record.QualifiedName = NodeTarget.Id + TEXT("::") + Record.Name;
    Record.Kind = TEXT("pin");
    Record.ClassPath = Pin->PinType.PinSubCategoryObject.IsValid()
        ? Pin->PinType.PinSubCategoryObject->GetPathName()
        : FString();
    Record.GraphIds.Add(GraphTarget.Id);
    Record.MemberIds.Add(GraphTarget.Id);
    Record.MemberIds.Add(NodeTarget.Id);
    Record.NodeIds.Add(NodeTarget.Id);
    if (!bMaterialize)
    {
        return Record;
    }
    Record.Json = MakePinJson(Blueprint, Graph, Node, Pin, bDetailed);
    return Record;
}

TArray<TSharedPtr<FJsonValue>> SerializeUserPins(
    const UK2Node_EditablePinBase* Node)
{
    TArray<TSharedPtr<FJsonValue>> Parameters;
    if (!Node)
    {
        return Parameters;
    }
    for (const TSharedPtr<FUserPinInfo>& Pin : Node->UserDefinedPins)
    {
        if (!Pin.IsValid())
        {
            continue;
        }
        const TSharedRef<FJsonObject> Parameter = MakeShared<FJsonObject>();
        Parameter->SetStringField(TEXT("name"), Pin->PinName.ToString());
        Parameter->SetObjectField(TEXT("type"), PinTypeSummary(Pin->PinType));
        SetNullableString(
            Parameter,
            TEXT("default"),
            Pin->PinDefaultValue);
        Parameters.Add(MakeShared<FJsonValueObject>(Parameter));
    }
    return Parameters;
}

void SetDeclaredMetadata(
    const TSharedRef<FJsonObject>& Json,
    const FKismetUserDeclaredFunctionMetadata* Source)
{
    Json->SetStringField(
        TEXT("category"), Source ? Source->Category.ToString() : FString());
    Json->SetStringField(
        TEXT("description"), Source ? Source->ToolTip.ToString() : FString());
    Json->SetStringField(
        TEXT("keywords"), Source ? Source->Keywords.ToString() : FString());
    Json->SetStringField(
        TEXT("compact_node_title"),
        Source ? Source->CompactNodeTitle.ToString() : FString());
    Json->SetBoolField(
        TEXT("deprecated"), Source && Source->bIsDeprecated);
    Json->SetStringField(
        TEXT("deprecation_message"),
        Source ? Source->DeprecationMessage : FString());
    Json->SetBoolField(
        TEXT("call_in_editor"), Source && Source->bCallInEditor);
    Json->SetBoolField(
        TEXT("thread_safe"), Source && Source->bThreadSafe);
    Json->SetBoolField(
        TEXT("unsafe_during_actor_construction"),
        Source && Source->bIsUnsafeDuringActorConstruction);
    const TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
    if (Source)
    {
        for (const TPair<FName, FString>& Pair : Source->GetMetaDataMap())
        {
            Values->SetStringField(Pair.Key.ToString(), Pair.Value);
        }
    }
    Json->SetObjectField(TEXT("values"), Values);
}

FString FunctionAccess(int32 Flags)
{
    if ((Flags & FUNC_Private) != 0)
    {
        return TEXT("private");
    }
    if ((Flags & FUNC_Protected) != 0)
    {
        return TEXT("protected");
    }
    return TEXT("public");
}

void AddSourceMemberMetadata(
    const TSharedRef<FJsonObject>& Record,
    UEdGraph* Graph,
    const FString& Kind)
{
    const TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
    if (Kind == TEXT("function"))
    {
        TArray<UK2Node_FunctionEntry*> Entries;
        TArray<UK2Node_FunctionResult*> Results;
        Graph->GetNodesOfClass(Entries);
        Graph->GetNodesOfClass(Results);
        const UK2Node_FunctionEntry* Entry = Entries.IsEmpty()
            ? nullptr
            : Entries[0];
        const UK2Node_FunctionResult* Result = Results.IsEmpty()
            ? nullptr
            : Results[0];
        const int32 Flags = Entry ? Entry->GetExtraFlags() : 0;
        Metadata->SetBoolField(
            TEXT("pure"), (Flags & FUNC_BlueprintPure) != 0);
        Metadata->SetBoolField(TEXT("const"), (Flags & FUNC_Const) != 0);
        Metadata->SetBoolField(TEXT("static"), (Flags & FUNC_Static) != 0);
        Metadata->SetStringField(TEXT("access"), FunctionAccess(Flags));
        Metadata->SetArrayField(TEXT("inputs"), SerializeUserPins(Entry));
        Metadata->SetArrayField(TEXT("outputs"), SerializeUserPins(Result));
        SetDeclaredMetadata(Metadata, Entry ? &Entry->MetaData : nullptr);
    }
    else
    {
        TArray<UK2Node_Tunnel*> Tunnels;
        Graph->GetNodesOfClass(Tunnels);
        const UK2Node_Tunnel* Entry = nullptr;
        const UK2Node_Tunnel* Exit = nullptr;
        for (const UK2Node_Tunnel* Tunnel : Tunnels)
        {
            if (!Tunnel || !Tunnel->IsEditable() ||
                Tunnel->IsA<UK2Node_Composite>())
            {
                continue;
            }
            if (Tunnel->bCanHaveOutputs)
            {
                Entry = Tunnel;
            }
            else if (Tunnel->bCanHaveInputs)
            {
                Exit = Tunnel;
            }
        }
        bool bPure = true;
        for (const UK2Node_Tunnel* Tunnel : {Entry, Exit})
        {
            if (!Tunnel)
            {
                continue;
            }
            for (const UEdGraphPin* Pin : Tunnel->Pins)
            {
                bPure = bPure && (!Pin ||
                    !GetDefault<UEdGraphSchema_K2>()->IsExecPin(*Pin));
            }
        }
        Metadata->SetBoolField(TEXT("pure"), bPure);
        Metadata->SetArrayField(TEXT("inputs"), SerializeUserPins(Entry));
        Metadata->SetArrayField(TEXT("outputs"), SerializeUserPins(Exit));
        const FKismetUserDeclaredFunctionMetadata* Source =
            UK2Node_MacroInstance::GetAssociatedGraphMetadata(Graph);
        SetDeclaredMetadata(Metadata, Source);
    }
    Record->SetObjectField(TEXT("metadata"), Metadata);
}

FInspectRecord MakeGraphRecord(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FString& Kind,
    bool bDetailed,
    bool bMaterialize = true)
{
    const FTargetRef Target =
        UE::MCPython::Blueprint2::DescribeGraphTarget(Blueprint, Graph);
    const FString ClassPath = Graph->GetClass()->GetPathName();
    const FString SchemaPath = Graph->GetSchema()
        ? Graph->GetSchema()->GetClass()->GetPathName()
        : FString();
    FInspectRecord Record;
    Record.Id = Target.Id;
    Record.Name = Graph->GetName();
    Record.QualifiedName = Target.OwnerId + TEXT("::") + Record.Name;
    Record.Kind = Kind;
    Record.ClassPath = ClassPath;
    Record.GraphIds.Add(Target.Id);
    Record.MemberIds.Add(Target.Id);
    if (!bMaterialize)
    {
        return Record;
    }
    Record.Json = MakeShared<FJsonObject>();
    SetIdentity(
        Record.Json.ToSharedRef(),
        ETargetKind::Graph,
        Target,
        Kind,
        Record.Name,
        ClassTypeSummary(ClassPath),
        Kind == TEXT("function") ? TEXT("function_id") : TEXT("macro_id"));
    Record.Json->SetStringField(TEXT("graph_id"), Target.Id);
    Record.Json->SetStringField(TEXT("class_path"), ClassPath);
    Record.Json->SetStringField(TEXT("schema_path"), SchemaPath);
    Record.Json->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
    int32 PinCount = 0;
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        PinCount += Node ? Node->Pins.Num() : 0;
    }
    Record.Json->SetNumberField(TEXT("pin_count"), PinCount);
    if (bDetailed)
    {
        AddSourceMemberMetadata(Record.Json.ToSharedRef(), Graph, Kind);
    }
    return Record;
}

FInspectRecord MakeVariableRecord(
    UBlueprint* Blueprint,
    const FBPVariableDescription& Variable,
    const FString& Op,
    bool bDetailed,
    bool bMaterialize = true)
{
    const FTargetRef Target =
        UE::MCPython::Blueprint2::DescribeVariableTarget(Blueprint, Variable);
    const bool bDispatcher =
        Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
    FInspectRecord Record;
    Record.Id = Target.Id;
    Record.Name = Variable.VarName.ToString();
    Record.QualifiedName = Blueprint->GetPathName() + TEXT("::") + Record.Name;
    Record.Kind = bDispatcher ? TEXT("dispatcher") : TEXT("variable");
    Record.ClassPath = Variable.VarType.PinSubCategoryObject.IsValid()
        ? Variable.VarType.PinSubCategoryObject->GetPathName()
        : FString();
    Record.MemberIds.Add(Target.Id);
    if (!bMaterialize)
    {
        return Record;
    }
    Record.Json = MakeShared<FJsonObject>();
    SetIdentity(
        Record.Json.ToSharedRef(),
        ETargetKind::Variable,
        Target,
        Record.Kind,
        Record.Name,
        PinTypeSummary(Variable.VarType),
        bDispatcher ? TEXT("dispatcher_id") : TEXT("variable_id"));
    Record.Json->SetBoolField(TEXT("has_default"), !Variable.DefaultValue.IsEmpty());
    Record.Json->SetNumberField(
        TEXT("metadata_count"), Variable.MetaDataArray.Num());
    if (Op == TEXT("variable_defaults") || bDetailed)
    {
        SetNullableString(
            Record.Json.ToSharedRef(), TEXT("default"), Variable.DefaultValue);
    }
    if (bDetailed)
    {
        Record.Json->SetStringField(TEXT("friendly_name"), Variable.FriendlyName);
        Record.Json->SetStringField(TEXT("category"), Variable.Category.ToString());
        Record.Json->SetStringField(
            TEXT("property_flags"),
            FString::Printf(
                TEXT("%llu"),
                static_cast<unsigned long long>(Variable.PropertyFlags)));
        Record.Json->SetStringField(
            TEXT("rep_notify_function"), Variable.RepNotifyFunc.ToString());
        const TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
        for (const FBPVariableMetaDataEntry& Entry : Variable.MetaDataArray)
        {
            Metadata->SetStringField(
                Entry.DataKey.ToString(), Entry.DataValue);
        }
        Record.Json->SetObjectField(TEXT("metadata"), Metadata);
    }
    return Record;
}

USCS_Node* FindComponentParent(
    const TArray<USCS_Node*>& Components,
    USCS_Node* Component)
{
    for (USCS_Node* Candidate : Components)
    {
        if (Candidate && Candidate->GetChildNodes().Contains(Component))
        {
            return Candidate;
        }
    }
    return nullptr;
}

TSharedRef<FJsonObject> VectorJson(const FVector& Value)
{
    const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("x"), Value.X);
    Json->SetNumberField(TEXT("y"), Value.Y);
    Json->SetNumberField(TEXT("z"), Value.Z);
    return Json;
}

FInspectRecord MakeComponentRecord(
    UBlueprint* Blueprint,
    const TArray<USCS_Node*>& Components,
    USCS_Node* Component,
    bool bHierarchy,
    bool bDetailed,
    bool bMaterialize = true)
{
    const FTargetRef Target =
        UE::MCPython::Blueprint2::DescribeComponentTarget(Blueprint, Component);
    const UClass* ComponentClass = Component->ComponentClass
        ? Component->ComponentClass.Get()
        : (Component->ComponentTemplate
            ? Component->ComponentTemplate->GetClass()
            : nullptr);
    const FString ClassPath = ComponentClass
        ? ComponentClass->GetPathName()
        : FString();
    USCS_Node* Parent = FindComponentParent(Components, Component);
    FInspectRecord Record;
    Record.Id = Target.Id;
    Record.Name = Component->GetVariableName().ToString();
    Record.QualifiedName = Blueprint->GetPathName() + TEXT("::") + Record.Name;
    Record.Kind = TEXT("component");
    Record.ClassPath = ClassPath;
    Record.MemberIds.Add(Target.Id);
    if (!bMaterialize)
    {
        return Record;
    }
    Record.Json = MakeShared<FJsonObject>();
    SetIdentity(
        Record.Json.ToSharedRef(),
        ETargetKind::Component,
        Target,
        Record.Kind,
        Record.Name,
        ClassTypeSummary(ClassPath),
        TEXT("component_id"));
    Record.Json->SetStringField(TEXT("class_path"), ClassPath);
    Record.Json->SetStringField(
        TEXT("parent_id"),
        Parent
            ? UE::MCPython::Blueprint2::DescribeComponentTarget(
                Blueprint, Parent).Id
            : FString());
    Record.Json->SetNumberField(
        TEXT("child_count"), Component->GetChildNodes().Num());
    if (bHierarchy || bDetailed)
    {
        TArray<TSharedPtr<FJsonValue>> ChildIds;
        for (USCS_Node* Child : Component->GetChildNodes())
        {
            if (Child)
            {
                ChildIds.Add(MakeShared<FJsonValueString>(
                    UE::MCPython::Blueprint2::DescribeComponentTarget(
                        Blueprint, Child).Id));
            }
        }
        ChildIds.Sort([](
            const TSharedPtr<FJsonValue>& A,
            const TSharedPtr<FJsonValue>& B)
        {
            return A->AsString() < B->AsString();
        });
        Record.Json->SetArrayField(TEXT("child_ids"), ChildIds);
    }
    if (bDetailed)
    {
        const TSharedRef<FJsonObject> Defaults = MakeShared<FJsonObject>();
        Defaults->SetStringField(
            TEXT("template_path"),
            Component->ComponentTemplate
                ? Component->ComponentTemplate->GetPathName()
                : FString());
        if (const USceneComponent* Scene =
            Cast<USceneComponent>(Component->ComponentTemplate))
        {
            Defaults->SetObjectField(
                TEXT("relative_location"),
                VectorJson(Scene->GetRelativeLocation()));
            const FRotator Rotation = Scene->GetRelativeRotation();
            const TSharedRef<FJsonObject> RotationJson = MakeShared<FJsonObject>();
            RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
            RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
            RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
            Defaults->SetObjectField(TEXT("relative_rotation"), RotationJson);
            Defaults->SetObjectField(
                TEXT("relative_scale"),
                VectorJson(Scene->GetRelativeScale3D()));
        }
        Record.Json->SetObjectField(TEXT("defaults"), Defaults);
    }
    return Record;
}

FInspectRecord MakeInterfaceRecord(
    UBlueprint* Blueprint,
    const FBPInterfaceDescription& Description,
    bool bDetailed,
    bool bMaterialize = true)
{
    const UClass* InterfaceClass = Description.Interface.Get();
    const FString ClassPath = InterfaceClass
        ? InterfaceClass->GetPathName()
        : FString();
    FTargetRef Target;
    Target.Id = TEXT("interface:") + ClassPath;
    Target.OwnerId = Blueprint->GetPathName();
    Target.Name = InterfaceClass ? InterfaceClass->GetName() : FString();
    Target.TypePath = ClassPath;
    FInspectRecord Record;
    Record.Id = Target.Id;
    Record.Name = Target.Name;
    Record.QualifiedName = ClassPath;
    Record.Kind = TEXT("interface");
    Record.ClassPath = ClassPath;
    Record.MemberIds.Add(Target.Id);
    if (!bMaterialize)
    {
        return Record;
    }
    Record.Json = MakeShared<FJsonObject>();
    SetIdentity(
        Record.Json.ToSharedRef(),
        ETargetKind::Interface,
        Target,
        Record.Kind,
        Record.Name,
        ClassTypeSummary(ClassPath),
        TEXT("interface_id"));
    Record.Json->SetStringField(TEXT("class_path"), ClassPath);
    Record.Json->SetNumberField(
        TEXT("graph_count"), Description.Graphs.Num());
    if (bDetailed)
    {
        TArray<TSharedPtr<FJsonValue>> GraphIds;
        for (UEdGraph* Graph : Description.Graphs)
        {
            if (Graph)
            {
                GraphIds.Add(MakeShared<FJsonValueString>(
                    UE::MCPython::Blueprint2::DescribeGraphTarget(
                        Blueprint, Graph).Id));
            }
        }
        GraphIds.Sort([](
            const TSharedPtr<FJsonValue>& A,
            const TSharedPtr<FJsonValue>& B)
        {
            return A->AsString() < B->AsString();
        });
        Record.Json->SetArrayField(TEXT("graph_ids"), GraphIds);
    }
    return Record;
}

FInspectRecord MakeConnectionRecord(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UEdGraphPin* Source,
    UEdGraphPin* Target,
    bool bDetailed,
    bool bMaterialize = true)
{
    UEdGraphNode* SourceNode = Source->GetOwningNode();
    UEdGraphNode* TargetNode = Target->GetOwningNode();
    const FTargetRef GraphTarget =
        UE::MCPython::Blueprint2::DescribeGraphTarget(Blueprint, Graph);
    const FTargetRef SourceNodeTarget =
        UE::MCPython::Blueprint2::DescribeNodeTarget(Blueprint, SourceNode);
    const FTargetRef TargetNodeTarget =
        UE::MCPython::Blueprint2::DescribeNodeTarget(Blueprint, TargetNode);
    const FTargetRef SourceTarget =
        UE::MCPython::Blueprint2::DescribePinTarget(Blueprint, Source);
    const FTargetRef TargetTarget =
        UE::MCPython::Blueprint2::DescribePinTarget(Blueprint, Target);
    FInspectRecord Record;
    Record.Id = TEXT("connection:") + SourceTarget.Id + TEXT("->") + TargetTarget.Id;
    Record.Name = Source->GetName() + TEXT(" -> ") + Target->GetName();
    Record.QualifiedName = SourceNodeTarget.Id + TEXT("::") +
        Source->GetName() + TEXT("->") + TargetNodeTarget.Id + TEXT("::") +
        Target->GetName();
    Record.Kind = TEXT("connection");
    Record.GraphIds.Add(GraphTarget.Id);
    Record.MemberIds.Add(GraphTarget.Id);
    Record.MemberIds.Add(SourceNodeTarget.Id);
    Record.MemberIds.Add(TargetNodeTarget.Id);
    Record.NodeIds.Add(SourceNodeTarget.Id);
    Record.NodeIds.Add(TargetNodeTarget.Id);
    if (!bMaterialize)
    {
        return Record;
    }
    Record.Json = MakeShared<FJsonObject>();
    Record.Json->SetStringField(TEXT("id"), Record.Id);
    Record.Json->SetStringField(TEXT("stable_id"), Record.Id);
    Record.Json->SetStringField(TEXT("id_kind"), TEXT("pin_pair"));
    Record.Json->SetBoolField(
        TEXT("stable"),
        !SourceTarget.Id.StartsWith(TEXT("fallback:")) &&
        !TargetTarget.Id.StartsWith(TEXT("fallback:")));
    Record.Json->SetStringField(TEXT("kind"), Record.Kind);
    Record.Json->SetStringField(TEXT("name"), Record.Name);
    Record.Json->SetObjectField(TEXT("type"), ClassTypeSummary(TEXT("connection")));
    Record.Json->SetStringField(TEXT("graph_id"), GraphTarget.Id);
    Record.Json->SetStringField(TEXT("source_node_id"), SourceNodeTarget.Id);
    Record.Json->SetStringField(TEXT("source_pin_id"), SourceTarget.Id);
    Record.Json->SetStringField(TEXT("target_node_id"), TargetNodeTarget.Id);
    Record.Json->SetStringField(TEXT("target_pin_id"), TargetTarget.Id);
    if (bDetailed)
    {
        Record.Json->SetStringField(TEXT("source_pin_name"), Source->GetName());
        Record.Json->SetStringField(TEXT("target_pin_name"), Target->GetName());
    }
    return Record;
}

TArray<UEdGraph*> UniqueBlueprintGraphs(UBlueprint* Blueprint)
{
    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);
    TSet<UEdGraph*> Seen;
    TArray<UEdGraph*> Result;
    for (UEdGraph* Graph : AllGraphs)
    {
        if (Graph && !Seen.Contains(Graph))
        {
            Seen.Add(Graph);
            Result.Add(Graph);
        }
    }
    return Result;
}

FInspectRecord MakeOverviewRecord(
    UBlueprint* Blueprint,
    bool bMaterialize = true)
{
    FInspectRecord Record;
    Record.Id = Blueprint->GetPathName();
    Record.Name = Blueprint->GetName();
    Record.QualifiedName = Blueprint->GetPathName();
    Record.Kind = TEXT("blueprint");
    Record.ClassPath = Blueprint->GetClass()->GetPathName();
    if (!bMaterialize)
    {
        return Record;
    }
    const TArray<UEdGraph*> Graphs = UniqueBlueprintGraphs(Blueprint);
    int32 NodeCount = 0;
    int32 PinCount = 0;
    int32 EventCount = 0;
    int32 ConnectionCount = 0;
    for (const UEdGraph* Graph : Graphs)
    {
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            ++NodeCount;
            EventCount += Node->IsA<UK2Node_Event>() ||
                Node->IsA<UK2Node_CustomEvent>() ? 1 : 0;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin)
                {
                    continue;
                }
                ++PinCount;
                if (Pin->Direction == EGPD_Output)
                {
                    ConnectionCount += Pin->LinkedTo.Num();
                }
            }
        }
    }
    int32 ComponentCount = 0;
    if (const USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
    {
        ComponentCount = SCS->GetAllNodes().Num();
    }
    int32 DispatcherCount = 0;
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        DispatcherCount +=
            Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate ? 1 : 0;
    }
    Record.Json = MakeShared<FJsonObject>();
    Record.Json->SetStringField(TEXT("id"), Record.Id);
    Record.Json->SetStringField(TEXT("stable_id"), Record.Id);
    Record.Json->SetStringField(TEXT("id_kind"), TEXT("asset_path"));
    Record.Json->SetBoolField(TEXT("stable"), true);
    Record.Json->SetStringField(TEXT("kind"), Record.Kind);
    Record.Json->SetStringField(TEXT("name"), Record.Name);
    Record.Json->SetObjectField(TEXT("type"), ClassTypeSummary(Record.ClassPath));
    Record.Json->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Record.Json->SetStringField(TEXT("compile_status"), BlueprintCompileStatus(Blueprint));
    const TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
    Counts->SetNumberField(TEXT("variables"), Blueprint->NewVariables.Num());
    Counts->SetNumberField(TEXT("components"), ComponentCount);
    Counts->SetNumberField(TEXT("functions"), Blueprint->FunctionGraphs.Num());
    Counts->SetNumberField(TEXT("macros"), Blueprint->MacroGraphs.Num());
    Counts->SetNumberField(TEXT("events"), EventCount);
    Counts->SetNumberField(TEXT("dispatchers"), DispatcherCount);
    Counts->SetNumberField(TEXT("interfaces"), Blueprint->ImplementedInterfaces.Num());
    Counts->SetNumberField(TEXT("graphs"), Graphs.Num());
    Counts->SetNumberField(TEXT("nodes"), NodeCount);
    Counts->SetNumberField(TEXT("pins"), PinCount);
    Counts->SetNumberField(TEXT("connections"), ConnectionCount);
    Record.Json->SetObjectField(TEXT("counts"), Counts);
    return Record;
}

TSharedRef<FJsonObject> MaterializeInspectRecord(
    const FInspectRecord& Record,
    bool bDetailed)
{
    switch (Record.Source)
    {
    case FInspectRecord::ESource::Overview:
        return MakeOverviewRecord(Record.Blueprint).Json.ToSharedRef();
    case FInspectRecord::ESource::Variable:
        return MakeVariableRecord(
            Record.Blueprint,
            *Record.Variable,
            Record.SourceOp,
            bDetailed).Json.ToSharedRef();
    case FInspectRecord::ESource::Component:
        {
            const TArray<USCS_Node*> Components =
                Record.Blueprint->SimpleConstructionScript
                    ? Record.Blueprint->SimpleConstructionScript->GetAllNodes()
                    : TArray<USCS_Node*>();
            return MakeComponentRecord(
                Record.Blueprint,
                Components,
                Record.Component,
                Record.bHierarchy,
                bDetailed).Json.ToSharedRef();
        }
    case FInspectRecord::ESource::Graph:
        return MakeGraphRecord(
            Record.Blueprint,
            Record.Graph,
            Record.Kind,
            bDetailed).Json.ToSharedRef();
    case FInspectRecord::ESource::Interface:
        return MakeInterfaceRecord(
            Record.Blueprint,
            *Record.Interface,
            bDetailed).Json.ToSharedRef();
    case FInspectRecord::ESource::Node:
        return MakeNodeRecord(
            Record.Blueprint,
            Record.Graph,
            Record.Node,
            bDetailed).Json.ToSharedRef();
    case FInspectRecord::ESource::Pin:
        return MakePinRecord(
            Record.Blueprint,
            Record.Graph,
            Record.Node,
            Record.Pin,
            bDetailed).Json.ToSharedRef();
    case FInspectRecord::ESource::Connection:
        return MakeConnectionRecord(
            Record.Blueprint,
            Record.Graph,
            Record.Pin,
            Record.OtherPin,
            bDetailed).Json.ToSharedRef();
    default:
        return Record.Json.ToSharedRef();
    }
}

bool ContainsId(const TArray<FString>& Ids, const FString& Expected)
{
    return Ids.ContainsByPredicate([&Expected](const FString& Value)
    {
        return Value.Equals(Expected, ESearchCase::CaseSensitive);
    });
}

bool MatchesNamePattern(const FString& Value, const FString& Pattern)
{
    if (Pattern.Contains(TEXT("*")) || Pattern.Contains(TEXT("?")))
    {
        return Value.MatchesWildcard(Pattern, ESearchCase::IgnoreCase);
    }
    return Value.Contains(Pattern, ESearchCase::IgnoreCase);
}

bool RecordMatches(const FInspectRecord& Record, const FInspectQuery& Query)
{
    if (!Query.GraphId.IsEmpty() && !ContainsId(Record.GraphIds, Query.GraphId))
    {
        return false;
    }
    if (!Query.MemberId.IsEmpty() &&
        !ContainsId(Record.MemberIds, Query.MemberId))
    {
        return false;
    }
    if (!Query.NodeId.IsEmpty() && !ContainsId(Record.NodeIds, Query.NodeId))
    {
        return false;
    }
    if (!Query.Kind.IsEmpty() &&
        !Record.Kind.Equals(Query.Kind, ESearchCase::IgnoreCase))
    {
        return false;
    }
    if (!Query.NamePattern.IsEmpty() &&
        !MatchesNamePattern(Record.Name, Query.NamePattern) &&
        !MatchesNamePattern(Record.QualifiedName, Query.NamePattern) &&
        !Record.SearchNames.ContainsByPredicate([&Query](const FString& Name)
        {
            return MatchesNamePattern(Name, Query.NamePattern);
        }))
    {
        return false;
    }
    return Query.ClassPath.IsEmpty() ||
        Record.ClassPath.Equals(Query.ClassPath, ESearchCase::IgnoreCase);
}

TArray<FInspectRecord> BuildInspectRecords(
    UBlueprint* Blueprint,
    const FInspectQuery& Query)
{
    TArray<FInspectRecord> Records;
    if (Query.Op == TEXT("overview"))
    {
        FInspectRecord Record = MakeOverviewRecord(Blueprint, false);
        Record.Source = FInspectRecord::ESource::Overview;
        Record.Blueprint = Blueprint;
        Records.Add(MoveTemp(Record));
    }
    else if (
        Query.Op == TEXT("variables") ||
        Query.Op == TEXT("variable_defaults") ||
        Query.Op == TEXT("dispatchers"))
    {
        for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
        {
            const bool bDispatcher =
                Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
            if (Query.Op == TEXT("dispatchers") && !bDispatcher)
            {
                continue;
            }
            FInspectRecord Record = MakeVariableRecord(
                Blueprint, Variable, Query.Op, false, false);
            Record.Source = FInspectRecord::ESource::Variable;
            Record.Blueprint = Blueprint;
            Record.Variable = &Variable;
            Record.SourceOp = Query.Op;
            Records.Add(MoveTemp(Record));
        }
    }
    else if (
        Query.Op == TEXT("components") ||
        Query.Op == TEXT("component_hierarchy"))
    {
        if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
        {
            const TArray<USCS_Node*> Components = SCS->GetAllNodes();
            for (USCS_Node* Component : Components)
            {
                if (Component)
                {
                    FInspectRecord Record = MakeComponentRecord(
                        Blueprint,
                        Components,
                        Component,
                        Query.Op == TEXT("component_hierarchy"),
                        false,
                        false);
                    Record.Source = FInspectRecord::ESource::Component;
                    Record.Blueprint = Blueprint;
                    Record.Component = Component;
                    Record.bHierarchy =
                        Query.Op == TEXT("component_hierarchy");
                    Records.Add(MoveTemp(Record));
                }
            }
        }
    }
    else if (Query.Op == TEXT("functions") || Query.Op == TEXT("macros"))
    {
        const TArray<UEdGraph*>& Graphs = Query.Op == TEXT("functions")
            ? Blueprint->FunctionGraphs
            : Blueprint->MacroGraphs;
        for (UEdGraph* Graph : Graphs)
        {
            if (Graph)
            {
                FInspectRecord Record = MakeGraphRecord(
                    Blueprint,
                    Graph,
                    Query.Op == TEXT("functions")
                        ? TEXT("function")
                        : TEXT("macro"),
                    false,
                    false);
                Record.Source = FInspectRecord::ESource::Graph;
                Record.Blueprint = Blueprint;
                Record.Graph = Graph;
                Records.Add(MoveTemp(Record));
            }
        }
    }
    else if (Query.Op == TEXT("interfaces"))
    {
        for (const FBPInterfaceDescription& Description :
            Blueprint->ImplementedInterfaces)
        {
            if (Description.Interface.Get())
            {
                FInspectRecord Record = MakeInterfaceRecord(
                    Blueprint, Description, false, false);
                Record.Source = FInspectRecord::ESource::Interface;
                Record.Blueprint = Blueprint;
                Record.Interface = &Description;
                Records.Add(MoveTemp(Record));
            }
        }
    }
    else
    {
        for (UEdGraph* Graph : UniqueBlueprintGraphs(Blueprint))
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node)
                {
                    continue;
                }
                if (Query.Op == TEXT("events"))
                {
                    if (Node->IsA<UK2Node_Event>() ||
                        Node->IsA<UK2Node_CustomEvent>())
                    {
                        FInspectRecord Record = MakeNodeRecord(
                            Blueprint, Graph, Node, false, false);
                        Record.Source = FInspectRecord::ESource::Node;
                        Record.Blueprint = Blueprint;
                        Record.Graph = Graph;
                        Record.Node = Node;
                        Records.Add(MoveTemp(Record));
                    }
                    continue;
                }
                if (Query.Op == TEXT("nodes"))
                {
                    FInspectRecord Record = MakeNodeRecord(
                        Blueprint, Graph, Node, false, false);
                    Record.Source = FInspectRecord::ESource::Node;
                    Record.Blueprint = Blueprint;
                    Record.Graph = Graph;
                    Record.Node = Node;
                    Records.Add(MoveTemp(Record));
                    continue;
                }
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin || Pin->bHidden)
                    {
                        continue;
                    }
                    if (Query.Op == TEXT("pins"))
                    {
                        FInspectRecord Record = MakePinRecord(
                            Blueprint, Graph, Node, Pin, false, false);
                        Record.Source = FInspectRecord::ESource::Pin;
                        Record.Blueprint = Blueprint;
                        Record.Graph = Graph;
                        Record.Node = Node;
                        Record.Pin = Pin;
                        Records.Add(MoveTemp(Record));
                    }
                    else if (
                        Query.Op == TEXT("connections") &&
                        Pin->Direction == EGPD_Output)
                    {
                        for (UEdGraphPin* Linked : Pin->LinkedTo)
                        {
                            if (Linked && Linked->GetOwningNode())
                            {
                                FInspectRecord Record = MakeConnectionRecord(
                                    Blueprint, Graph, Pin, Linked, false, false);
                                Record.Source = FInspectRecord::ESource::Connection;
                                Record.Blueprint = Blueprint;
                                Record.Graph = Graph;
                                Record.Pin = Pin;
                                Record.OtherPin = Linked;
                                Records.Add(MoveTemp(Record));
                            }
                        }
                    }
                }
            }
        }
    }
    Records.RemoveAll([&Query](const FInspectRecord& Record)
    {
        return !RecordMatches(Record, Query);
    });
    Records.Sort([](const FInspectRecord& A, const FInspectRecord& B)
    {
        const int32 IdComparison = A.Id.Compare(B.Id, ESearchCase::CaseSensitive);
        return IdComparison == 0
            ? A.QualifiedName < B.QualifiedName
            : IdComparison < 0;
    });
    return Records;
}

bool ReadOptionalQueryString(
    const TSharedRef<FJsonObject>& QueryObject,
    const FString& Field,
    const FString& Path,
    FString& OutValue,
    FString& OutFailure)
{
    if (!QueryObject->HasField(Field))
    {
        OutValue.Empty();
        return true;
    }
    if (!QueryObject->TryGetStringField(Field, OutValue))
    {
        OutFailure = InspectInvalid(
            Path, FString::Printf(TEXT("'%s' must be a string."), *Field));
        return false;
    }
    if (Field != TEXT("cursor") && OutValue.IsEmpty())
    {
        OutFailure = InspectInvalid(
            Path, FString::Printf(TEXT("'%s' must not be empty."), *Field));
        return false;
    }
    return true;
}

bool IsInspectIdKind(const FString& Kind)
{
    return Kind == TEXT("graph") ||
        Kind == TEXT("node") ||
        Kind == TEXT("pin") ||
        Kind == TEXT("variable") ||
        Kind == TEXT("component");
}

bool IsAsciiLetter(TCHAR Character)
{
    return (Character >= TEXT('A') && Character <= TEXT('Z')) ||
        (Character >= TEXT('a') && Character <= TEXT('z'));
}

bool IsAsciiDigit(TCHAR Character)
{
    return Character >= TEXT('0') && Character <= TEXT('9');
}

bool IsLowerHex(const FString& Value, int32 ExpectedLength)
{
    if (Value.Len() != ExpectedLength)
    {
        return false;
    }
    for (const TCHAR Character : Value)
    {
        if (!((Character >= TEXT('0') && Character <= TEXT('9')) ||
              (Character >= TEXT('a') && Character <= TEXT('f'))))
        {
            return false;
        }
    }
    return true;
}

bool IsStableInspectId(const FString& Id)
{
    if (Id.StartsWith(TEXT("fallback:")))
    {
        TArray<FString> Parts;
        Id.ParseIntoArray(Parts, TEXT(":"), false);
        return Parts.Num() == 3 &&
            Parts[0] == TEXT("fallback") &&
            IsInspectIdKind(Parts[1]) &&
            IsLowerHex(Parts[2], 40);
    }
    if (Id.StartsWith(TEXT("interface:")))
    {
        const FString Path = Id.RightChop(10);
        const int32 Slash = Path.Find(
            TEXT("/"),
            ESearchCase::CaseSensitive,
            ESearchDir::FromStart,
            1);
        int32 Dot = INDEX_NONE;
        if (!Path.StartsWith(TEXT("/")) ||
            Slash <= 1 ||
            !Path.FindLastChar(TEXT('.'), Dot) ||
            Dot <= Slash + 1 ||
            Dot + 1 >= Path.Len())
        {
            return false;
        }
        const FString Module = Path.Mid(1, Slash - 1);
        if (Module.IsEmpty() || !IsAsciiLetter(Module[0]))
        {
            return false;
        }
        for (const TCHAR Character : Module)
        {
            if (!IsAsciiLetter(Character) && !IsAsciiDigit(Character) &&
                Character != TEXT('_'))
            {
                return false;
            }
        }
        for (int32 Index = Slash + 1; Index < Path.Len(); ++Index)
        {
            const TCHAR Character = Path[Index];
            if (FChar::IsWhitespace(Character) || Character == TEXT(':') ||
                (Index > Dot &&
                 (Character == TEXT('/') || Character == TEXT('.'))))
            {
                return false;
            }
        }
        return true;
    }
    int32 Separator = INDEX_NONE;
    if (!Id.FindChar(TEXT(':'), Separator))
    {
        return false;
    }
    const FString Kind = Id.Left(Separator);
    const FString GuidText = Id.Mid(Separator + 1);
    FGuid Guid;
    return IsInspectIdKind(Kind) &&
        GuidText.Len() == 36 &&
        GuidText == GuidText.ToLower() &&
        FGuid::Parse(GuidText, Guid);
}

bool ValidateInspectFilter(
    const FString& Value,
    const FString& Path,
    bool bClassPath,
    FString& OutFailure)
{
    if (Value.IsEmpty())
    {
        return true;
    }
    const bool bValid = bClassPath
        ? Value.Len() >= 9 && Value.StartsWith(TEXT("/Script/"))
        : IsStableInspectId(Value);
    if (bValid)
    {
        return true;
    }
    OutFailure = InspectInvalid(
        Path,
        bClassPath
            ? TEXT("'class_path' must be a full /Script/ class path.")
            : TEXT("Stable ID filter has an invalid format."));
    return false;
}

bool ParseInspectQueries(
    UBlueprint* Blueprint,
    const TSharedRef<FJsonObject>& Request,
    TArray<FInspectQuery>& OutQueries,
    FString& OutFailure)
{
    const TArray<TSharedPtr<FJsonValue>>* QueryValues = nullptr;
    TArray<TSharedPtr<FJsonValue>> EmptyQueries;
    if (Request->HasField(TEXT("queries")))
    {
        if (!Request->TryGetArrayField(TEXT("queries"), QueryValues) ||
            !QueryValues)
        {
            OutFailure = InspectInvalid(
                TEXT("queries"), TEXT("'queries' must be an array."));
            return false;
        }
    }
    else
    {
        QueryValues = &EmptyQueries;
    }
    if (QueryValues->Num() > 32)
    {
        OutFailure = InspectInvalid(
            TEXT("queries"),
            TEXT("'queries' must contain at most 32 query objects."));
        return false;
    }
    FString TopLevelCursor;
    if (Request->HasField(TEXT("cursor")) &&
        !Request->TryGetStringField(TEXT("cursor"), TopLevelCursor))
    {
        OutFailure = InspectInvalid(
            TEXT("cursor"), TEXT("'cursor' must be a string."));
        return false;
    }
    const int32 QueryCount = QueryValues->IsEmpty() ? 1 : QueryValues->Num();
    if (!TopLevelCursor.IsEmpty() && QueryCount != 1)
    {
        OutFailure = InspectInvalid(
            TEXT("cursor"),
            TEXT("A top-level cursor is valid only when exactly one query is submitted."),
            TEXT("Move each cursor into its matching query for multi-query pagination."));
        return false;
    }
    static const TSet<FString> SupportedOps = {
        TEXT("overview"), TEXT("variables"), TEXT("variable_defaults"),
        TEXT("components"), TEXT("component_hierarchy"), TEXT("functions"),
        TEXT("macros"), TEXT("events"), TEXT("dispatchers"),
        TEXT("interfaces"), TEXT("nodes"), TEXT("pins"),
        TEXT("connections")};
    static const TSet<FString> SupportedFields = {
        TEXT("op"), TEXT("graph_id"), TEXT("member_id"), TEXT("node_id"),
        TEXT("kind"), TEXT("name_pattern"), TEXT("class_path"), TEXT("detail"),
        TEXT("limit"), TEXT("cursor")};
    TArray<TSharedPtr<FJsonValue>> DefaultQueries;
    if (QueryValues->IsEmpty())
    {
        const TSharedRef<FJsonObject> Overview = MakeShared<FJsonObject>();
        Overview->SetStringField(TEXT("op"), TEXT("overview"));
        DefaultQueries.Add(MakeShared<FJsonValueObject>(Overview));
        QueryValues = &DefaultQueries;
    }
    for (int32 Index = 0; Index < QueryValues->Num(); ++Index)
    {
        const FString QueryPath = FString::Printf(TEXT("queries[%d]"), Index);
        const TSharedPtr<FJsonValue>& QueryValue = (*QueryValues)[Index];
        if (!QueryValue.IsValid() || QueryValue->Type != EJson::Object)
        {
            OutFailure = InspectInvalid(
                QueryPath, TEXT("Each query must be a JSON object."));
            return false;
        }
        const TSharedPtr<FJsonObject> QueryObject = QueryValue->AsObject();
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
            QueryObject->Values)
        {
            if (!SupportedFields.Contains(Field.Key))
            {
                OutFailure = InspectInvalid(
                    QueryPath + TEXT(".") + Field.Key,
                    FString::Printf(
                        TEXT("Unknown inspection query field: %s"),
                        *Field.Key));
                return false;
            }
        }
        FInspectQuery Query;
        if (!QueryObject->TryGetStringField(TEXT("op"), Query.Op) ||
            !SupportedOps.Contains(Query.Op))
        {
            OutFailure = InspectInvalid(
                QueryPath + TEXT(".op"),
                TEXT("'op' must be one of the supported Blueprint inspection operations."));
            return false;
        }
        FString Detail = TEXT("compact");
        if (QueryObject->HasField(TEXT("detail")) &&
            (!QueryObject->TryGetStringField(TEXT("detail"), Detail) ||
             (Detail != TEXT("compact") && Detail != TEXT("detailed"))))
        {
            OutFailure = InspectInvalid(
                QueryPath + TEXT(".detail"),
                TEXT("'detail' must be either 'compact' or 'detailed'."));
            return false;
        }
        Query.bDetailed = Detail == TEXT("detailed");
        if (QueryObject->HasField(TEXT("limit")))
        {
            double Limit = 0.0;
            if (!QueryObject->TryGetNumberField(TEXT("limit"), Limit) ||
                !FMath::IsFinite(Limit) || Limit != FMath::FloorToDouble(Limit) ||
                Limit < 1.0 || Limit > 500.0)
            {
                OutFailure = InspectInvalid(
                    QueryPath + TEXT(".limit"),
                    TEXT("'limit' must be an integer between 1 and 500."));
                return false;
            }
            Query.Limit = static_cast<int32>(Limit);
        }
        if (!ReadOptionalQueryString(
                QueryObject.ToSharedRef(), TEXT("graph_id"),
                QueryPath + TEXT(".graph_id"), Query.GraphId, OutFailure) ||
            !ReadOptionalQueryString(
                QueryObject.ToSharedRef(), TEXT("member_id"),
                QueryPath + TEXT(".member_id"), Query.MemberId, OutFailure) ||
            !ReadOptionalQueryString(
                QueryObject.ToSharedRef(), TEXT("node_id"),
                QueryPath + TEXT(".node_id"), Query.NodeId, OutFailure) ||
            !ReadOptionalQueryString(
                QueryObject.ToSharedRef(), TEXT("kind"),
                QueryPath + TEXT(".kind"), Query.Kind, OutFailure) ||
            !ReadOptionalQueryString(
                QueryObject.ToSharedRef(), TEXT("name_pattern"),
                QueryPath + TEXT(".name_pattern"), Query.NamePattern, OutFailure) ||
            !ReadOptionalQueryString(
                QueryObject.ToSharedRef(), TEXT("class_path"),
                QueryPath + TEXT(".class_path"), Query.ClassPath, OutFailure) ||
            !ReadOptionalQueryString(
                QueryObject.ToSharedRef(), TEXT("cursor"),
                QueryPath + TEXT(".cursor"), Query.Cursor, OutFailure))
        {
            return false;
        }
        if (!ValidateInspectFilter(
                Query.GraphId,
                QueryPath + TEXT(".graph_id"),
                false,
                OutFailure) ||
            !ValidateInspectFilter(
                Query.MemberId,
                QueryPath + TEXT(".member_id"),
                false,
                OutFailure) ||
            !ValidateInspectFilter(
                Query.NodeId,
                QueryPath + TEXT(".node_id"),
                false,
                OutFailure) ||
            !ValidateInspectFilter(
                Query.ClassPath,
                QueryPath + TEXT(".class_path"),
                true,
                OutFailure))
        {
            return false;
        }
        if (!TopLevelCursor.IsEmpty())
        {
            if (!Query.Cursor.IsEmpty())
            {
                OutFailure = InspectInvalid(
                    QueryPath + TEXT(".cursor"),
                    TEXT("Specify either the top-level cursor or the query cursor, not both."));
                return false;
            }
            Query.Cursor = TopLevelCursor;
        }
        const TSharedRef<FJsonObject> Canonical = MakeShared<FJsonObject>();
        Canonical->SetStringField(TEXT("op"), Query.Op);
        Canonical->SetNumberField(TEXT("limit"), Query.Limit);
        Canonical->SetStringField(TEXT("detail"), Detail);
        if (!Query.GraphId.IsEmpty())
        {
            Canonical->SetStringField(TEXT("graph_id"), Query.GraphId);
        }
        if (!Query.MemberId.IsEmpty())
        {
            Canonical->SetStringField(TEXT("member_id"), Query.MemberId);
        }
        if (!Query.NodeId.IsEmpty())
        {
            Canonical->SetStringField(TEXT("node_id"), Query.NodeId);
        }
        if (!Query.Kind.IsEmpty())
        {
            Canonical->SetStringField(TEXT("kind"), Query.Kind);
        }
        if (!Query.NamePattern.IsEmpty())
        {
            Canonical->SetStringField(TEXT("name_pattern"), Query.NamePattern);
        }
        if (!Query.ClassPath.IsEmpty())
        {
            Canonical->SetStringField(TEXT("class_path"), Query.ClassPath);
        }
        Query.Digest = UE::MCPython::Blueprint2::CanonicalQueryDigest(Canonical);
        if (!Query.Cursor.IsEmpty())
        {
            FPageRequest Page;
            FString CursorError;
            if (!UE::MCPython::Blueprint2::DecodeCursor(
                    Query.Cursor,
                    Blueprint->GetPathName(),
                    Query.Digest,
                    Page,
                    CursorError))
            {
                OutFailure = InspectInvalid(
                    QueryPath + TEXT(".cursor"), CursorError,
                    TEXT("Use the cursor returned by this exact query in the current editor session."));
                return false;
            }
            Query.LastId = Page.LastId;
        }
        OutQueries.Add(MoveTemp(Query));
    }
    return true;
}
}

FString UMCPythonHelper::InspectBlueprint(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    using namespace UE::MCPython::Blueprint2;
    if (!Blueprint)
    {
        return SerializeResult(MakeFailure(
            TEXT("PRECONDITION_FAILED"),
            TEXT("asset_path"),
            TEXT("Blueprint is required."),
            false,
            TEXT("Provide an existing Blueprint asset.")));
    }
    TSharedPtr<FJsonObject> Request;
    const TSharedRef<TJsonReader<>> Reader =
        TJsonReaderFactory<>::Create(RequestJson);
    if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
    {
        return InspectInvalid(
            TEXT("request"), TEXT("Inspection request must be a JSON object."));
    }
    TArray<FInspectQuery> Queries;
    FString Failure;
    if (!ParseInspectQueries(
            Blueprint, Request.ToSharedRef(), Queries, Failure))
    {
        return Failure;
    }

    TArray<TArray<FInspectRecord>> QueryRecords;
    QueryRecords.Reserve(Queries.Num());
    for (int32 Index = 0; Index < Queries.Num(); ++Index)
    {
        TArray<FInspectRecord> Records = BuildInspectRecords(
            Blueprint, Queries[Index]);
        if (!Queries[Index].LastId.IsEmpty() &&
            !Records.ContainsByPredicate([&Queries, Index](const FInspectRecord& Record)
            {
                return Record.Id.Equals(
                    Queries[Index].LastId, ESearchCase::CaseSensitive);
            }))
        {
            return InspectInvalid(
                FString::Printf(TEXT("queries[%d].cursor"), Index),
                TEXT("Cursor is stale because its last returned stable ID no longer exists."),
                TEXT("Restart pagination from the first page of this query."));
        }
        QueryRecords.Add(MoveTemp(Records));
    }

    TArray<TSharedPtr<FJsonValue>> Results;
    for (int32 Index = 0; Index < Queries.Num(); ++Index)
    {
        const FInspectQuery& Query = Queries[Index];
        const TArray<FInspectRecord>& Records = QueryRecords[Index];
        int32 Start = 0;
        if (!Query.LastId.IsEmpty())
        {
            Start = Records.IndexOfByPredicate([&Query](const FInspectRecord& Record)
            {
                return Record.Id.Equals(
                    Query.LastId, ESearchCase::CaseSensitive);
            }) + 1;
        }
        const int32 End = FMath::Min(Start + Query.Limit, Records.Num());
        TArray<TSharedPtr<FJsonValue>> Items;
        for (int32 RecordIndex = Start; RecordIndex < End; ++RecordIndex)
        {
            const TSharedRef<FJsonObject> Materialized =
                MaterializeInspectRecord(
                    Records[RecordIndex], Query.bDetailed);
            Items.Add(MakeShared<FJsonValueObject>(Materialized));
        }
        FString NextCursor;
        if (End < Records.Num() && End > Start)
        {
            FPageRequest Page;
            Page.Limit = Query.Limit;
            Page.LastId = Records[End - 1].Id;
            Page.QueryDigest = Query.Digest;
            NextCursor = EncodeCursor(Blueprint->GetPathName(), Page);
        }
        const TSharedRef<FJsonObject> QueryResult = MakeShared<FJsonObject>();
        QueryResult->SetStringField(TEXT("op"), Query.Op);
        QueryResult->SetStringField(
            TEXT("detail"), Query.bDetailed ? TEXT("detailed") : TEXT("compact"));
        QueryResult->SetNumberField(TEXT("total_count"), Records.Num());
        QueryResult->SetNumberField(TEXT("returned_count"), Items.Num());
        QueryResult->SetArrayField(TEXT("items"), Items);
        QueryResult->SetStringField(TEXT("next_cursor"), NextCursor);
        Results.Add(MakeShared<FJsonValueObject>(QueryResult));
    }
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetArrayField(TEXT("results"), Results);
    return SerializeResult(MakeSuccess(
        FString::Printf(
            TEXT("Blueprint inspection returned %d query result%s."),
            Results.Num(), Results.Num() == 1 ? TEXT("") : TEXT("s")),
        Data));
}

FString UMCPythonHelper::ListBlueprintComponents(UBlueprint* Blueprint)
{
    if (!Blueprint)
        return MakeJsonError(TEXT("Invalid Blueprint."));

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    if (!SCS)
        return MakeJsonError(TEXT("Blueprint has no SimpleConstructionScript."));

    TArray<TSharedPtr<FJsonValue>> ComponentsArr;
    for (USCS_Node* Node : SCS->GetAllNodes())
    {
        if (!Node) continue;
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        const UE::MCPython::Blueprint2::FTargetRef ComponentTarget =
            UE::MCPython::Blueprint2::DescribeComponentTarget(Blueprint, Node);
        const FString ComponentId = ComponentTarget.Id;
        Obj->SetStringField(TEXT("stable_id"), ComponentId);
        Obj->SetStringField(TEXT("component_id"), ComponentId);
        SetTargetMetadata(
            Obj,
            UE::MCPython::Blueprint2::ETargetKind::Component,
            ComponentTarget);
        Obj->SetStringField(TEXT("variable_name"), Node->GetVariableName().ToString());
        if (Node->ComponentTemplate)
        {
            Obj->SetStringField(TEXT("class"), Node->ComponentTemplate->GetClass()->GetName());
            Obj->SetBoolField(TEXT("is_native"), false);
        }
        else
        {
            Obj->SetStringField(TEXT("class"), TEXT("Unknown"));
            Obj->SetBoolField(TEXT("is_native"), false);
        }
        USCS_Node* Parent = nullptr;
        for (USCS_Node* Candidate : SCS->GetAllNodes())
        {
            if (Candidate && Candidate->GetChildNodes().Contains(Node))
            {
                Parent = Candidate;
                break;
            }
        }
        Obj->SetStringField(TEXT("parent"), Parent ? Parent->GetVariableName().ToString() : TEXT(""));
        ComponentsArr.Add(MakeShareable(new FJsonValueObject(Obj)));
    }

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetArrayField(TEXT("components"), ComponentsArr);
    R->SetNumberField(TEXT("count"), ComponentsArr.Num());
    return SerializeJsonObj(R);
}
