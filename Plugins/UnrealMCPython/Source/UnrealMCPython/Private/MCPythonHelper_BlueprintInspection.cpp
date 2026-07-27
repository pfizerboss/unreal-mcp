// Copyright (c) 2025 GenOrca (by zenoengine). All Rights Reserved.

#include "MCPythonHelper.h"
#include "MCPythonHelperInternal.h"
#include "MCPythonBlueprint2Internal.h"

#include "EdGraphSchema_K2.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_CustomEvent.h"
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
}

FString UMCPythonHelper::GetBlueprintGraphInfo(UBlueprint* Blueprint, const FString& GraphName)
{
    if (!Blueprint)
        return MakeJsonError(TEXT("Invalid Blueprint."));

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
        return MakeJsonError(FString::Printf(TEXT("Graph '%s' not found in Blueprint."), *GraphName));

    const FString GraphId = UE::MCPython::Blueprint2::MakeGraphTargetId(
        Blueprint, Graph);
    TArray<TSharedPtr<FJsonValue>> NodesArr;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node) continue;

        TSharedPtr<FJsonObject> NodeObj = MakeShareable(new FJsonObject());
        const FString NodeId = UE::MCPython::Blueprint2::MakeNodeTargetId(
            Blueprint, Node);
        NodeObj->SetStringField(TEXT("stable_id"), NodeId);
        NodeObj->SetStringField(TEXT("node_id"), NodeId);
        NodeObj->SetStringField(TEXT("graph_id"), GraphId);
        SetTargetMetadata(
            NodeObj,
            UE::MCPython::Blueprint2::ETargetKind::Node,
            NodeId);
        SetTargetMetadata(
            NodeObj,
            UE::MCPython::Blueprint2::ETargetKind::Graph,
            GraphId,
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
            const FString PinId = UE::MCPython::Blueprint2::MakePinTargetId(
                Blueprint, Pin);
            PinObj->SetStringField(TEXT("stable_id"), PinId);
            PinObj->SetStringField(TEXT("pin_id"), PinId);
            PinObj->SetStringField(TEXT("graph_id"), GraphId);
            PinObj->SetStringField(TEXT("node_id"), NodeId);
            SetTargetMetadata(
                PinObj,
                UE::MCPython::Blueprint2::ETargetKind::Pin,
                PinId);
            SetTargetMetadata(
                PinObj,
                UE::MCPython::Blueprint2::ETargetKind::Graph,
                GraphId,
                TEXT("graph"));
            SetTargetMetadata(
                PinObj,
                UE::MCPython::Blueprint2::ETargetKind::Node,
                NodeId,
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
                    LinkObj->SetStringField(
                        TEXT("graph_id"),
                        UE::MCPython::Blueprint2::MakeGraphTargetId(
                            Blueprint, LinkedGraph));
                    LinkObj->SetStringField(
                        TEXT("node_id"),
                        UE::MCPython::Blueprint2::MakeNodeTargetId(
                            Blueprint, LinkedNode));
                    LinkObj->SetStringField(
                        TEXT("pin_id"),
                        UE::MCPython::Blueprint2::MakePinTargetId(
                            Blueprint, Linked));
                    SetTargetMetadata(
                        LinkObj,
                        UE::MCPython::Blueprint2::ETargetKind::Graph,
                        LinkObj->GetStringField(TEXT("graph_id")),
                        TEXT("graph"));
                    SetTargetMetadata(
                        LinkObj,
                        UE::MCPython::Blueprint2::ETargetKind::Node,
                        LinkObj->GetStringField(TEXT("node_id")),
                        TEXT("node"));
                    SetTargetMetadata(
                        LinkObj,
                        UE::MCPython::Blueprint2::ETargetKind::Pin,
                        LinkObj->GetStringField(TEXT("pin_id")),
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
        GraphId);
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
        const FString TypePath = Var.VarType.PinSubCategoryObject.IsValid()
            ? Var.VarType.PinSubCategoryObject->GetPathName()
            : Var.VarType.PinCategory.ToString();
        const FString VariableId = Var.VarGuid.IsValid()
            ? UE::MCPython::Blueprint2::MakeTargetId(
                UE::MCPython::Blueprint2::ETargetKind::Variable,
                Var.VarGuid)
            : UE::MCPython::Blueprint2::MakeQualifiedFallbackId(
                UE::MCPython::Blueprint2::ETargetKind::Variable,
                Blueprint->GetPathName(),
                Var.VarName.ToString(),
                TypePath);
        VarObj->SetStringField(TEXT("stable_id"), VariableId);
        VarObj->SetStringField(TEXT("variable_id"), VariableId);
        SetTargetMetadata(
            VarObj,
            UE::MCPython::Blueprint2::ETargetKind::Variable,
            VariableId);
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
        const UClass* ComponentClass = Node->ComponentClass
            ? Node->ComponentClass.Get()
            : (Node->ComponentTemplate
                ? Node->ComponentTemplate->GetClass()
                : nullptr);
        const FString TypePath = ComponentClass
            ? ComponentClass->GetPathName()
            : FString();
        const FString ComponentId = Node->VariableGuid.IsValid()
            ? UE::MCPython::Blueprint2::MakeTargetId(
                UE::MCPython::Blueprint2::ETargetKind::Component,
                Node->VariableGuid)
            : UE::MCPython::Blueprint2::MakeQualifiedFallbackId(
                UE::MCPython::Blueprint2::ETargetKind::Component,
                Blueprint->GetPathName(),
                Node->GetVariableName().ToString(),
                TypePath);
        Obj->SetStringField(TEXT("stable_id"), ComponentId);
        Obj->SetStringField(TEXT("component_id"), ComponentId);
        SetTargetMetadata(
            Obj,
            UE::MCPython::Blueprint2::ETargetKind::Component,
            ComponentId);
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
