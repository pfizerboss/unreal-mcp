// Copyright (c) 2025 GenOrca. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "MCPythonBlueprint2Internal.h"
#include "MCPythonHelper.h"

#include "Dom/JsonObject.h"
#include "Components/SceneComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"

namespace
{
template <typename TObjectType>
TObjectType* FindOrCreateFixture(UObject* Outer, const TCHAR* Name)
{
    if (TObjectType* Existing = FindObject<TObjectType>(Outer, Name))
    {
        return Existing;
    }
    return NewObject<TObjectType>(Outer, FName(Name), RF_Transient);
}

UBlueprint* MakeBlueprintFixture(const TCHAR* PackageName)
{
    UPackage* Package = CreatePackage(PackageName);
    return FindOrCreateFixture<UBlueprint>(Package, TEXT("BP_StableIds"));
}

UEdGraph* MakeGraphFixture(
    UBlueprint* Blueprint,
    const TCHAR* Name,
    UClass* SchemaClass)
{
    UEdGraph* Graph = FindOrCreateFixture<UEdGraph>(Blueprint, Name);
    Graph->Schema = SchemaClass;
    Graph->GraphGuid.Invalidate();
    return Graph;
}

template <typename TNodeType>
TNodeType* MakeNodeFixture(UEdGraph* Graph, const TCHAR* Name)
{
    TNodeType* Node = FindOrCreateFixture<TNodeType>(Graph, Name);
    Node->NodeGuid.Invalidate();
    return Node;
}

UEdGraphPin* MakePinFixture(
    UEdGraphNode* Node,
    const TCHAR* Name,
    const FName Category)
{
    UEdGraphPin* Pin = Node->CreatePin(EGPD_Input, Category, Name);
    Pin->PinId.Invalidate();
    return Pin;
}

TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json)
{
    TSharedPtr<FJsonObject> Result;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    return FJsonSerializer::Deserialize(Reader, Result) ? Result : nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprint2TargetIdsTest,
    "UnrealMCPython.Blueprint2.TargetIds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprint2TargetIdsTest::RunTest(const FString& Parameters)
{
    using namespace UE::MCPython::Blueprint2;

    (void)Parameters;

    UBlueprint* Blueprint = MakeBlueprintFixture(TEXT("/MCPythonTests/StableIds"));
    UEdGraph* Graph = MakeGraphFixture(
        Blueprint,
        TEXT("GraphA"),
        UEdGraphSchema_K2::StaticClass());
    Blueprint->UbergraphPages.AddUnique(Graph);
    UK2Node_CustomEvent* Node = MakeNodeFixture<UK2Node_CustomEvent>(
        Graph,
        TEXT("NodeA"));
    Graph->AddNode(Node, false, false);
    UEdGraphPin* Pin = MakePinFixture(
        Node,
        TEXT("InputA"),
        UEdGraphSchema_K2::PC_Boolean);

    TestEqualSensitive(
        TEXT("Blueprint path matches the fallback qualification assumption"),
        *Blueprint->GetPathName(),
        TEXT("/MCPythonTests/StableIds.BP_StableIds"));
    TestEqualSensitive(
        TEXT("Graph schema class matches the fallback qualification assumption"),
        *Graph->GetSchema()->GetClass()->GetPathName(),
        TEXT("/Script/BlueprintGraph.EdGraphSchema_K2"));
    TestEqualSensitive(
        TEXT("Node class matches the fallback qualification assumption"),
        *Node->GetClass()->GetPathName(),
        TEXT("/Script/BlueprintGraph.K2Node_CustomEvent"));
    TestEqualSensitive(
        TEXT("Pin category matches the fallback qualification assumption"),
        *Pin->PinType.PinCategory.ToString(),
        TEXT("bool"));

    TestFalse(TEXT("Fallback graph fixture has no GUID"), Graph->GraphGuid.IsValid());
    TestFalse(TEXT("Fallback node fixture has no GUID"), Node->NodeGuid.IsValid());
    TestFalse(TEXT("Fallback pin fixture has no GUID"), Pin->PinId.IsValid());

    const FString GraphFallback = MakeGraphTargetId(Blueprint, Graph);
    const FString NodeFallback = MakeNodeTargetId(Blueprint, Node);
    const FString PinFallback = MakePinTargetId(Blueprint, Pin);

    TestFalse(TEXT("Graph fallback ID is non-empty"), GraphFallback.IsEmpty());
    TestFalse(TEXT("Node fallback ID is non-empty"), NodeFallback.IsEmpty());
    TestFalse(TEXT("Pin fallback ID is non-empty"), PinFallback.IsEmpty());
    TestEqualSensitive(
        TEXT("Graph fallback ID is independently precomputed"),
        *GraphFallback,
        TEXT("fallback:graph:c6afa87de837f324fd224c43d4f24e5fe74ce74d"));
    TestEqualSensitive(
        TEXT("Node fallback ID is independently precomputed"),
        *NodeFallback,
        TEXT("fallback:node:79bfb622f4b03440dc12fba6a89ae53d44b0e46b"));
    TestEqualSensitive(
        TEXT("Pin fallback ID is independently precomputed"),
        *PinFallback,
        TEXT("fallback:pin:19239ed3e36212ec138f5c58de68d7e834fff3ca"));
    TestEqualSensitive(
        TEXT("Graph fallback ID is deterministic"),
        MakeGraphTargetId(Blueprint, Graph),
        GraphFallback);
    TestEqualSensitive(
        TEXT("Node fallback ID is deterministic"),
        MakeNodeTargetId(Blueprint, Node),
        NodeFallback);
    TestEqualSensitive(
        TEXT("Pin fallback ID is deterministic"),
        MakePinTargetId(Blueprint, Pin),
        PinFallback);

    const FGuid FixedGuid(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff);
    Graph->GraphGuid = FixedGuid;
    Node->NodeGuid = FixedGuid;
    Pin->PinId = FixedGuid;

    TestEqualSensitive(
        TEXT("Graph GUID target ID uses lowercase hyphen formatting"),
        *MakeGraphTargetId(Blueprint, Graph),
        TEXT("graph:00112233-4455-6677-8899-aabbccddeeff"));
    TestEqualSensitive(
        TEXT("Node GUID target ID uses lowercase hyphen formatting"),
        *MakeNodeTargetId(Blueprint, Node),
        TEXT("node:00112233-4455-6677-8899-aabbccddeeff"));
    TestEqualSensitive(
        TEXT("Pin GUID target ID uses lowercase hyphen formatting"),
        *MakePinTargetId(Blueprint, Pin),
        TEXT("pin:00112233-4455-6677-8899-aabbccddeeff"));
    FGuid ParsedGuid;
    TestTrue(
        TEXT("Strict target parser accepts lowercase hyphenated GUIDs"),
        ParseTargetId(
            TEXT("graph:00112233-4455-6677-8899-aabbccddeeff"),
            ETargetKind::Graph,
            ParsedGuid));
    TestFalse(
        TEXT("Strict target parser rejects uppercase GUIDs"),
        ParseTargetId(
            TEXT("graph:00112233-4455-6677-8899-AABBCCDDEEFF"),
            ETargetKind::Graph,
            ParsedGuid));
    TestFalse(
        TEXT("Strict target parser rejects compact GUIDs"),
        ParseTargetId(
            TEXT("graph:00112233445566778899aabbccddeeff"),
            ETargetKind::Graph,
            ParsedGuid));

    FString ResolveError;
    FTargetRef StableGraphTarget;
    StableGraphTarget.Id = MakeGraphTargetId(Blueprint, Graph);
    const FResolvedTarget ResolvedGraph = ResolveTarget(
        Blueprint,
        ETargetKind::Graph,
        StableGraphTarget,
        ResolveError);
    TestEqualSensitive(
        TEXT("Resolved graph uses graph_guid id kind"),
        ResolvedGraph.IdKind,
        TEXT("graph_guid"));
    FTargetRef StableNodeTarget;
    StableNodeTarget.Id = MakeNodeTargetId(Blueprint, Node);
    const FResolvedTarget ResolvedNode = ResolveTarget(
        Blueprint,
        ETargetKind::Node,
        StableNodeTarget,
        ResolveError);
    TestEqualSensitive(
        TEXT("Resolved node uses node_guid id kind"),
        ResolvedNode.IdKind,
        TEXT("node_guid"));
    FTargetRef StablePinTarget;
    StablePinTarget.Id = MakePinTargetId(Blueprint, Pin);
    const FResolvedTarget ResolvedPin = ResolveTarget(
        Blueprint,
        ETargetKind::Pin,
        StablePinTarget,
        ResolveError);
    TestEqualSensitive(
        TEXT("Resolved pin uses pin_guid id kind"),
        ResolvedPin.IdKind,
        TEXT("pin_guid"));

    Graph->GraphGuid.Invalidate();
    Node->NodeGuid.Invalidate();
    Pin->PinId.Invalidate();
    FTargetRef FallbackGraphTarget;
    FallbackGraphTarget.Id = GraphFallback;
    FallbackGraphTarget.Name = Graph->GetName();
    FallbackGraphTarget.TypePath = Graph->GetSchema()->GetClass()->GetPathName();
    FallbackGraphTarget.bAllowNameFallback = true;
    const FResolvedTarget ResolvedFallbackGraph = ResolveTarget(
        Blueprint,
        ETargetKind::Graph,
        FallbackGraphTarget,
        ResolveError);
    TestEqualSensitive(
        TEXT("Resolved graph fallback uses qualified_name_fallback id kind"),
        ResolvedFallbackGraph.IdKind,
        TEXT("qualified_name_fallback"));
    TestFalse(
        TEXT("Resolved graph fallback remains unstable"),
        ResolvedFallbackGraph.bStable);
    TestEqualSensitive(
        TEXT("Graph fallback is restored after clearing its GUID"),
        MakeGraphTargetId(Blueprint, Graph),
        GraphFallback);
    TestEqualSensitive(
        TEXT("Node fallback is restored after clearing its GUID"),
        MakeNodeTargetId(Blueprint, Node),
        NodeFallback);
    TestEqualSensitive(
        TEXT("Pin fallback is restored after clearing its GUID"),
        MakePinTargetId(Blueprint, Pin),
        PinFallback);

    const TSharedPtr<FJsonObject> LegacyGraphInfo = ParseJsonObject(
        UMCPythonHelper::GetBlueprintGraphInfo(Blueprint, TEXT("GraphA")));
    TestTrue(TEXT("Legacy graph info is valid JSON"), LegacyGraphInfo.IsValid());
    if (LegacyGraphInfo)
    {
        TestEqualSensitive(
            TEXT("Legacy graph info classifies fallback graph ID"),
            LegacyGraphInfo->GetStringField(TEXT("id_kind")),
            TEXT("qualified_name_fallback"));
        TestFalse(
            TEXT("Legacy graph info marks fallback graph unstable"),
            LegacyGraphInfo->GetBoolField(TEXT("stable")));
        const TArray<TSharedPtr<FJsonValue>>& LegacyNodes =
            LegacyGraphInfo->GetArrayField(TEXT("nodes"));
        TestTrue(
            TEXT("Legacy graph info contains the fixture node"),
            !LegacyNodes.IsEmpty());
        if (!LegacyNodes.IsEmpty())
        {
            const TSharedPtr<FJsonObject> LegacyNode = LegacyNodes[0]->AsObject();
            TestEqualSensitive(
                TEXT("Legacy node classifies fallback ID"),
                LegacyNode->GetStringField(TEXT("id_kind")),
                TEXT("qualified_name_fallback"));
            TestFalse(
                TEXT("Legacy node marks fallback ID unstable"),
                LegacyNode->GetBoolField(TEXT("stable")));
            const TArray<TSharedPtr<FJsonValue>>& LegacyPins =
                LegacyNode->GetArrayField(TEXT("pins"));
            TestTrue(
                TEXT("Legacy node contains the fixture pin"),
                !LegacyPins.IsEmpty());
            if (!LegacyPins.IsEmpty())
            {
                const TSharedPtr<FJsonObject> LegacyPin =
                    LegacyPins[0]->AsObject();
                TestEqualSensitive(
                    TEXT("Legacy pin classifies fallback ID"),
                    LegacyPin->GetStringField(TEXT("id_kind")),
                    TEXT("qualified_name_fallback"));
                TestFalse(
                    TEXT("Legacy pin marks fallback ID unstable"),
                    LegacyPin->GetBoolField(TEXT("stable")));
            }
        }
    }

    UBlueprint* GraphOwnerBlueprint = MakeBlueprintFixture(
        TEXT("/MCPythonTests/TargetIdsGraphOwner"));
    UEdGraph* GraphOwnerVariant = MakeGraphFixture(
        GraphOwnerBlueprint,
        TEXT("GraphA"),
        UEdGraphSchema_K2::StaticClass());
    UEdGraph* GraphNameVariant = MakeGraphFixture(
        Blueprint,
        TEXT("GraphB"),
        UEdGraphSchema_K2::StaticClass());
    UBlueprint* GraphSchemaBlueprint = MakeBlueprintFixture(
        TEXT("/MCPythonTests/TargetIdsGraphSchema"));
    UEdGraph* GraphSchemaVariant = MakeGraphFixture(
        GraphSchemaBlueprint,
        TEXT("GraphA"),
        UEdGraphSchema::StaticClass());

    TestNotEqualSensitive(
        TEXT("Graph fallback changes with its Blueprint owner path"),
        MakeGraphTargetId(GraphOwnerBlueprint, GraphOwnerVariant),
        GraphFallback);
    TestNotEqualSensitive(
        TEXT("Graph fallback changes with its graph name"),
        MakeGraphTargetId(Blueprint, GraphNameVariant),
        GraphFallback);
    TestNotEqualSensitive(
        TEXT("Graph fallback changes with its schema class"),
        MakeGraphTargetId(Blueprint, GraphSchemaVariant),
        GraphFallback);

    UEdGraph* NestedOwnerA = MakeGraphFixture(
        Blueprint,
        TEXT("NestedOwnerA"),
        UEdGraphSchema_K2::StaticClass());
    UEdGraph* NestedOwnerB = MakeGraphFixture(
        Blueprint,
        TEXT("NestedOwnerB"),
        UEdGraphSchema_K2::StaticClass());
    UEdGraph* NestedGraphA = FindOrCreateFixture<UEdGraph>(
        NestedOwnerA,
        TEXT("NestedGraph"));
    UEdGraph* NestedGraphB = FindOrCreateFixture<UEdGraph>(
        NestedOwnerB,
        TEXT("NestedGraph"));
    NestedGraphA->Schema = UEdGraphSchema_K2::StaticClass();
    NestedGraphB->Schema = UEdGraphSchema_K2::StaticClass();
    NestedGraphA->GraphGuid.Invalidate();
    NestedGraphB->GraphGuid.Invalidate();
    TestNotEqualSensitive(
        TEXT("Graph fallback distinguishes identical nested graph names"),
        MakeGraphTargetId(Blueprint, NestedGraphA),
        MakeGraphTargetId(Blueprint, NestedGraphB));

    UK2Node_CustomEvent* NodeGraphOwnerVariant =
        MakeNodeFixture<UK2Node_CustomEvent>(GraphNameVariant, TEXT("NodeA"));
    UK2Node_CustomEvent* NodeNameVariant =
        MakeNodeFixture<UK2Node_CustomEvent>(Graph, TEXT("NodeB"));
    UBlueprint* NodeClassBlueprint = MakeBlueprintFixture(
        TEXT("/MCPythonTests/TargetIdsNodeClass"));
    UEdGraph* NodeClassGraph = MakeGraphFixture(
        NodeClassBlueprint,
        TEXT("GraphA"),
        UEdGraphSchema_K2::StaticClass());
    UEdGraphNode* NodeClassVariant = MakeNodeFixture<UEdGraphNode>(
        NodeClassGraph,
        TEXT("NodeA"));

    TestNotEqualSensitive(
        TEXT("Node fallback changes with its graph owner"),
        MakeNodeTargetId(Blueprint, NodeGraphOwnerVariant),
        NodeFallback);
    TestNotEqualSensitive(
        TEXT("Node fallback changes with its node name"),
        MakeNodeTargetId(Blueprint, NodeNameVariant),
        NodeFallback);
    TestNotEqualSensitive(
        TEXT("Node fallback changes with its node class"),
        MakeNodeTargetId(Blueprint, NodeClassVariant),
        NodeFallback);

    UEdGraphPin* PinOwnerVariant = MakePinFixture(
        NodeNameVariant,
        TEXT("InputA"),
        UEdGraphSchema_K2::PC_Boolean);
    UEdGraphPin* PinNameVariant = MakePinFixture(
        Node,
        TEXT("InputB"),
        UEdGraphSchema_K2::PC_Boolean);
    UEdGraphPin* PinTypeVariant = MakePinFixture(
        Node,
        TEXT("InputA"),
        UEdGraphSchema_K2::PC_Int);

    TestNotEqualSensitive(
        TEXT("Pin fallback changes with its owning node"),
        MakePinTargetId(Blueprint, PinOwnerVariant),
        PinFallback);
    TestNotEqualSensitive(
        TEXT("Pin fallback changes with its pin name"),
        MakePinTargetId(Blueprint, PinNameVariant),
        PinFallback);
    TestNotEqualSensitive(
        TEXT("Pin fallback changes with its pin category/type"),
        MakePinTargetId(Blueprint, PinTypeVariant),
        PinFallback);

    UEdGraphPin* PinDirectionVariant = Node->CreatePin(
        EGPD_Output,
        UEdGraphSchema_K2::PC_Boolean,
        TEXT("InputA"));
    PinDirectionVariant->PinId.Invalidate();
    TestNotEqualSensitive(
        TEXT("Pin fallback changes with its direction"),
        MakePinTargetId(Blueprint, PinDirectionVariant),
        PinFallback);

    UEdGraphPin* DuplicatePinA = MakePinFixture(
        Node,
        TEXT("Repeated"),
        UEdGraphSchema_K2::PC_Boolean);
    UEdGraphPin* DuplicatePinB = MakePinFixture(
        Node,
        TEXT("Repeated"),
        UEdGraphSchema_K2::PC_Boolean);
    TestNotEqualSensitive(
        TEXT("Pin fallback distinguishes repeated same-signature pins"),
        MakePinTargetId(Blueprint, DuplicatePinA),
        MakePinTargetId(Blueprint, DuplicatePinB));

    Node->NodeGuid = FixedGuid;
    FTargetRef PinFallbackTarget;
    PinFallbackTarget.Id = MakePinTargetId(Blueprint, Pin);
    PinFallbackTarget.OwnerId = MakeNodeTargetId(Blueprint, Node);
    PinFallbackTarget.Name = Pin->GetName();
    PinFallbackTarget.TypePath = Pin->PinType.PinCategory.ToString();
    PinFallbackTarget.bAllowNameFallback = true;
    const FResolvedTarget ResolvedFallbackPin = ResolveTarget(
        Blueprint,
        ETargetKind::Pin,
        PinFallbackTarget,
        ResolveError);
    TestTrue(
        TEXT("Pin fallback resolution honors its full qualification"),
        ResolvedFallbackPin.Pin == Pin);
    TestEqualSensitive(
        TEXT("Resolved pin fallback uses qualified_name_fallback id kind"),
        ResolvedFallbackPin.IdKind,
        TEXT("qualified_name_fallback"));

    const TSharedRef<FJsonObject> GlobalCapabilities = BuildCapabilities(nullptr);
    bool bSupportsK2Schema = false;
    bool bSupportsSCSOperations = false;
    bool bSupportsCompilerTokens = false;
    bool bHasK2Graphs = true;
    TestTrue(
        TEXT("Global capabilities declare K2 schema runtime support"),
        GlobalCapabilities->TryGetBoolField(
            TEXT("supports_k2_schema"),
            bSupportsK2Schema) && bSupportsK2Schema);
    TestTrue(
        TEXT("Global capabilities declare SCS runtime support"),
        GlobalCapabilities->TryGetBoolField(
            TEXT("supports_scs_operations"),
            bSupportsSCSOperations) && bSupportsSCSOperations);
    TestTrue(
        TEXT("UE 5.7 capabilities declare compiler-token runtime support"),
        GlobalCapabilities->TryGetBoolField(
            TEXT("supports_compiler_tokens"),
            bSupportsCompilerTokens) && bSupportsCompilerTokens);
    TestTrue(
        TEXT("Global capabilities distinguish absent asset K2 graphs"),
        GlobalCapabilities->TryGetBoolField(
            TEXT("has_k2_graphs"),
            bHasK2Graphs) && !bHasK2Graphs);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprint2BriefCountsTest,
    "UnrealMCPython.Blueprint2.BriefCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprint2BriefCountsTest::RunTest(const FString& Parameters)
{
    using namespace UE::MCPython::Blueprint2;

    (void)Parameters;

    UPackage* BriefPackage = CreatePackage(
        TEXT("/Game/Tests/MCP/Blueprint2NativeBriefCounts"));
    UBlueprint* BriefBlueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        BriefPackage,
        TEXT("BP_BriefCounts"),
        BPTYPE_Normal,
        TEXT("MCPythonBlueprint2BriefCountsTest"));
    TestNotNull(TEXT("Brief count fixture Blueprint is created"), BriefBlueprint);
    if (!BriefBlueprint)
    {
        return false;
    }

    FEdGraphPinType DispatcherType;
    DispatcherType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    TestTrue(
        TEXT("Brief fixture dispatcher variable is added"),
        FBlueprintEditorUtils::AddMemberVariable(
            BriefBlueprint,
            TEXT("OnBriefEvent"),
            DispatcherType));

    UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
        BriefBlueprint,
        TEXT("BriefMacro"),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddMacroGraph(
        BriefBlueprint,
        MacroGraph,
        true,
        nullptr);
    UEdGraph* NestedBriefGraph = NewObject<UEdGraph>(
        MacroGraph,
        TEXT("NestedBriefGraph"),
        RF_Transient);
    NestedBriefGraph->Schema = UEdGraphSchema_K2::StaticClass();
    NestedBriefGraph->GraphGuid = FGuid::NewGuid();
    MacroGraph->SubGraphs.Add(NestedBriefGraph);
    TestTrue(
        TEXT("Brief fixture interface is implemented"),
        FBlueprintEditorUtils::ImplementNewInterface(
            BriefBlueprint,
            UInterface::StaticClass()->GetClassPathName()));
    BriefBlueprint->ImplementedInterfaces.AddDefaulted();

    FString ResolveError;
    FTargetRef InterfaceTarget;
    InterfaceTarget.Id = FString::Printf(
        TEXT("interface:%s"),
        *UInterface::StaticClass()->GetPathName());
    const FResolvedTarget ResolvedInterface = ResolveTarget(
        BriefBlueprint,
        ETargetKind::Interface,
        InterfaceTarget,
        ResolveError);
    TestTrue(
        TEXT("Interface path target resolves as stable"),
        ResolvedInterface.bStable);
    TestEqualSensitive(
        TEXT("Resolved interface uses interface_path id kind"),
        ResolvedInterface.IdKind,
        TEXT("interface_path"));

    TestTrue(
        TEXT("Dispatcher fixture has a persisted variable GUID"),
        BriefBlueprint->NewVariables[0].VarGuid.IsValid());
    FTargetRef VariableTarget;
    VariableTarget.Id = MakeTargetId(
        ETargetKind::Variable,
        BriefBlueprint->NewVariables[0].VarGuid);
    const FResolvedTarget ResolvedVariable = ResolveTarget(
        BriefBlueprint,
        ETargetKind::Variable,
        VariableTarget,
        ResolveError);
    TestEqualSensitive(
        TEXT("Resolved variable uses variable_guid id kind"),
        ResolvedVariable.IdKind,
        TEXT("variable_guid"));

    const TSharedPtr<FJsonObject> LegacyVariables = ParseJsonObject(
        UMCPythonHelper::ListBlueprintVariables(BriefBlueprint));
    const TSharedPtr<FJsonObject> LegacyVariable =
        LegacyVariables->GetArrayField(TEXT("variables"))[0]->AsObject();
    TestEqualSensitive(
        TEXT("Legacy variable emits variable_guid id kind"),
        LegacyVariable->GetStringField(TEXT("id_kind")),
        TEXT("variable_guid"));
    TestTrue(
        TEXT("Legacy variable emits stable persisted target"),
        LegacyVariable->GetBoolField(TEXT("stable")));

    USCS_Node* BriefComponent =
        BriefBlueprint->SimpleConstructionScript->CreateNode(
            USceneComponent::StaticClass(),
            TEXT("BriefComponent"));
    BriefBlueprint->SimpleConstructionScript->AddNode(BriefComponent);
    TestTrue(
        TEXT("Brief component fixture has a persisted SCS variable GUID"),
        BriefComponent->VariableGuid.IsValid());
    FTargetRef ComponentTarget;
    ComponentTarget.Id = MakeTargetId(
        ETargetKind::Component,
        BriefComponent->VariableGuid);
    const FResolvedTarget ResolvedComponent = ResolveTarget(
        BriefBlueprint,
        ETargetKind::Component,
        ComponentTarget,
        ResolveError);
    TestEqualSensitive(
        TEXT("Resolved component uses scs_variable_guid id kind"),
        ResolvedComponent.IdKind,
        TEXT("scs_variable_guid"));
    const TSharedPtr<FJsonObject> LegacyComponents = ParseJsonObject(
        UMCPythonHelper::ListBlueprintComponents(BriefBlueprint));
    const TSharedPtr<FJsonObject> LegacyComponent =
        LegacyComponents->GetArrayField(TEXT("components"))[0]->AsObject();
    TestEqualSensitive(
        TEXT("Legacy component emits scs_variable_guid id kind"),
        LegacyComponent->GetStringField(TEXT("id_kind")),
        TEXT("scs_variable_guid"));
    TestTrue(
        TEXT("Legacy component emits stable persisted target"),
        LegacyComponent->GetBoolField(TEXT("stable")));

    const TSharedPtr<FJsonObject> Brief = ParseJsonObject(
        UMCPythonHelper::GetBlueprintBrief(BriefBlueprint));
    TestTrue(TEXT("Blueprint brief is valid JSON"), Brief.IsValid());
    if (!Brief)
    {
        return false;
    }
    TestTrue(TEXT("Blueprint brief succeeds"), Brief->GetBoolField(TEXT("success")));
    const TSharedPtr<FJsonObject> Data = Brief->GetObjectField(TEXT("data"));
    const TSharedPtr<FJsonObject> Counts = Data->GetObjectField(TEXT("counts"));
    const TSharedPtr<FJsonObject> Capabilities =
        Data->GetObjectField(TEXT("capabilities"));
    TestTrue(
        TEXT("Brief capabilities report K2 graph presence"),
        Capabilities->GetBoolField(TEXT("has_k2_graphs")));
    TestTrue(
        TEXT("Brief capabilities report all fixture graphs use K2 schema"),
        Capabilities->GetBoolField(TEXT("all_graphs_k2_schema")));
    TestEqual(
        TEXT("Blueprint brief counts one variable"),
        Counts->GetIntegerField(TEXT("variables")),
        int32(1));
    TestEqual(
        TEXT("Blueprint brief counts one dispatcher"),
        Counts->GetIntegerField(TEXT("dispatchers")),
        int32(1));
    TestEqual(
        TEXT("Blueprint brief counts one macro"),
        Counts->GetIntegerField(TEXT("macros")),
        int32(1));
    TestEqual(
        TEXT("Blueprint brief counts one implemented interface"),
        Counts->GetIntegerField(TEXT("interfaces")),
        int32(1));
    const TArray<TSharedPtr<FJsonValue>>& Interfaces =
        Data->GetArrayField(TEXT("interfaces"));
    TestEqual(TEXT("Blueprint brief emits one interface path"), Interfaces.Num(), 1);
    if (Interfaces.Num() == 1)
    {
        TestEqualSensitive(
            TEXT("Blueprint brief emits the implemented interface path"),
            Interfaces[0]->AsString(),
            UInterface::StaticClass()->GetPathName());
    }
    const TArray<TSharedPtr<FJsonValue>>& Graphs =
        Data->GetArrayField(TEXT("graphs"));
    const bool bContainsNestedGraph = Graphs.ContainsByPredicate(
        [](const TSharedPtr<FJsonValue>& Value)
        {
            return Value && Value->AsString() == TEXT("NestedBriefGraph");
        });
    TestFalse(
        TEXT("Blueprint brief excludes nested graph names"),
        bContainsNestedGraph);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
