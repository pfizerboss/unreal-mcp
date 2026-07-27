// Copyright (c) 2025 GenOrca. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "MCPythonBlueprint2Internal.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CustomEvent.h"
#include "Misc/AutomationTest.h"
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
    UK2Node_CustomEvent* Node = MakeNodeFixture<UK2Node_CustomEvent>(
        Graph,
        TEXT("NodeA"));
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
        TEXT("graph:c6afa87de837f324fd224c43d4f24e5fe74ce74d"));
    TestEqualSensitive(
        TEXT("Node fallback ID is independently precomputed"),
        *NodeFallback,
        TEXT("node:c83be440e1d769e125ee1f53bc085787330fcd18"));
    TestEqualSensitive(
        TEXT("Pin fallback ID is independently precomputed"),
        *PinFallback,
        TEXT("pin:dfbf200f14eb55f552440426594944a107ba6583"));
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

    Graph->GraphGuid.Invalidate();
    Node->NodeGuid.Invalidate();
    Pin->PinId.Invalidate();
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

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
