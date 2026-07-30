// Copyright (c) 2025 GenOrca. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "MCPythonBlueprint2Internal.h"
#include "MCPythonHelper.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeExit.h"
#include "Misc/Timespan.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace
{
using namespace UE::MCPython::Blueprint2;

FPaletteContext MakeContext()
{
    FPaletteContext Context;
    Context.AssetPath = TEXT("/Game/__MCPTests/BP_Palette.BP_Palette");
    Context.GraphId =
        TEXT("graph:11111111-1111-4111-8111-111111111111");
    Context.GraphSchemaPath = TEXT("/Script/BlueprintGraph.EdGraphSchema_K2");
    Context.RequestDigest = TEXT("sha1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    Context.ResultDigest = TEXT("sha1:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    Context.Limit = 50;
    return Context;
}

FPaletteContext MakeConnectionContext(const bool bAllowConversion)
{
    FPaletteContext Context = MakeContext();
    Context.Kind = EPaletteContextKind::Connection;
    Context.SourcePinId =
        TEXT("pin:22222222-2222-4222-8222-222222222222");
    Context.TargetPinId =
        TEXT("pin:33333333-3333-4333-8333-333333333333");
    Context.bAllowConversion = bAllowConversion;
    return Context;
}

FPaletteContextExpectation ExpectContext(const FPaletteContext& Context)
{
    FPaletteContextExpectation Expected;
    Expected.AssetPath = Context.AssetPath;
    Expected.GraphId = Context.GraphId;
    Expected.GraphSchemaPath = Context.GraphSchemaPath;
    Expected.Kind = Context.Kind;
    Expected.SourcePinId = Context.SourcePinId;
    Expected.TargetPinId = Context.TargetPinId;
    Expected.AllowConversion = Context.bAllowConversion;
    Expected.RequestDigest = Context.RequestDigest;
    Expected.ResultDigest = Context.ResultDigest;
    Expected.Limit = Context.Limit;
    return Expected;
}

FReplacementPlanRecord MakeReplacementPlan(const FString& Salt)
{
    FReplacementPlanRecord Record;
    Record.AssetPath = TEXT("/Game/__MCPTests/BP_Palette.BP_Palette");
    Record.GraphId =
        TEXT("graph:11111111-1111-4111-8111-111111111111");
    Record.GraphSchemaPath =
        TEXT("/Script/BlueprintGraph.EdGraphSchema_K2");
    Record.NodeId =
        TEXT("node:44444444-4444-4444-8444-444444444444");
    Record.NodeSnapshotDigest = TEXT("sha1:") + Sha1Hex(Salt);
    Record.ActionId =
        TEXT("action:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    Record.ActionResultDigest =
        TEXT("sha1:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    return Record;
}

FPaletteActionRecord MakeActionRecord()
{
    FPaletteActionRecord Record;
    Record.Context = MakeContext();
    Record.CandidateKey =
        TEXT("/Script/Engine.Actor\nSpawnerSignature\n");
    Record.SpawnerSignature = TEXT("SpawnerSignature");
    Record.OwnerPath = TEXT("/Script/Engine.Actor");
    Record.SortKey = TEXT("0600\ntransformation\nget actor location");
    return Record;
}

FString TamperToken(const FString& Token)
{
    const TCHAR Replacement = Token.EndsWith(TEXT("0")) ? TEXT('1') : TEXT('0');
    return Token.LeftChop(1) + FString::Chr(Replacement);
}

TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json)
{
    TSharedPtr<FJsonObject> Result;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    return FJsonSerializer::Deserialize(Reader, Result) ? Result : nullptr;
}

FString SerializeJsonObject(const TSharedRef<FJsonObject>& Object)
{
    FString Result;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
    FJsonSerializer::Serialize(Object, Writer);
    return Result;
}

FString SerializeJsonArray(const TArray<TSharedPtr<FJsonValue>>& Values)
{
    FString Result;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
    FJsonSerializer::Serialize(Values, Writer);
    return Result;
}

TSharedRef<FJsonObject> MakeSearchRequest(
    const FString& GraphId,
    const FString& Query,
    const FString& Cursor,
    const int32 Limit)
{
    const TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("graph_id"), GraphId);
    Request->SetStringField(TEXT("query"), Query);
    Request->SetObjectField(TEXT("filters"), MakeShared<FJsonObject>());
    Request->SetStringField(TEXT("cursor"), Cursor);
    Request->SetNumberField(TEXT("limit"), Limit);
    return Request;
}

TSharedRef<FJsonObject> MakeSpawnRequest(
    const FString& GraphId,
    const FString& ActionId,
    const double X,
    const double Y,
    const TArray<FString>& BindingIds = {})
{
    const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
    Position->SetNumberField(TEXT("x"), X);
    Position->SetNumberField(TEXT("y"), Y);
    TArray<TSharedPtr<FJsonValue>> Bindings;
    for (const FString& BindingId : BindingIds)
    {
        Bindings.Add(MakeShared<FJsonValueString>(BindingId));
    }
    const TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("graph_id"), GraphId);
    Request->SetStringField(TEXT("action_id"), ActionId);
    Request->SetObjectField(TEXT("position"), Position);
    Request->SetArrayField(TEXT("bindings"), MoveTemp(Bindings));
    return Request;
}

TSharedRef<FJsonObject> MakeSuggestRequest(
    const FString& GraphId,
    const FString& PinId,
    const FString& Query,
    const FString& Cursor,
    const int32 Limit)
{
    const TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("graph_id"), GraphId);
    Request->SetStringField(TEXT("pin_id"), PinId);
    Request->SetStringField(TEXT("query"), Query);
    Request->SetStringField(TEXT("cursor"), Cursor);
    Request->SetNumberField(TEXT("limit"), Limit);
    return Request;
}

FString FirstErrorCode(const TSharedPtr<FJsonObject>& Result)
{
    if (!Result || !Result->HasTypedField<EJson::Array>(TEXT("errors")))
    {
        return FString();
    }
    const TArray<TSharedPtr<FJsonValue>>& Errors =
        Result->GetArrayField(TEXT("errors"));
    return Errors.IsEmpty() || !Errors[0].IsValid()
        ? FString()
        : Errors[0]->AsObject()->GetStringField(TEXT("code"));
}

void CleanupFixturePackage(UPackage* Package)
{
    if (!Package)
    {
        return;
    }
    TArray<UObject*> Objects;
    GetObjectsWithOuter(Package, Objects, true);
    for (int32 Index = Objects.Num() - 1; Index >= 0; --Index)
    {
        Objects[Index]->ClearFlags(RF_Public | RF_Standalone);
        Objects[Index]->MarkAsGarbage();
    }
    Package->SetDirtyFlag(false);
    Package->ClearFlags(RF_Public | RF_Standalone);
    Package->MarkAsGarbage();
    CollectGarbage(RF_NoFlags);
}

struct FPaletteBlueprintFixture
{
    UPackage* Package = nullptr;
    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    FString GraphId;
};

FPaletteBlueprintFixture MakePaletteBlueprintFixture(const TCHAR* TestName)
{
    using namespace UE::MCPython::Blueprint2;

    FPaletteBlueprintFixture Fixture;
    const FString PackageName = FString::Printf(
        TEXT("/Game/__MCPTests/Palette_%s_%s"),
        TestName,
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Fixture.Package = CreatePackage(*PackageName);
    Fixture.Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Fixture.Package,
        TEXT("BP_Palette"),
        BPTYPE_Normal,
        TestName);
    if (Fixture.Blueprint && !Fixture.Blueprint->UbergraphPages.IsEmpty())
    {
        Fixture.Graph = Fixture.Blueprint->UbergraphPages[0];
        Fixture.GraphId = MakeGraphTargetId(Fixture.Blueprint, Fixture.Graph);
    }
    return Fixture;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintPaletteTokenTest,
    "UnrealMCP.Blueprint2.Palette.TokenIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprintPaletteTokenTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace UE::MCPython::Blueprint2;

    ResetPaletteTokenStateForTests();
    SetPaletteTokenClockForTests(FDateTime(2026, 7, 30, 12, 0));

    FPaletteActionRecord Record = MakeActionRecord();
    const FString ActionId = RegisterPaletteActionToken(Record);
    TestTrue(TEXT("opaque action prefix"), ActionId.StartsWith(TEXT("action:")));
    TestEqual(TEXT("opaque action length"), ActionId.Len(), 47);
    TestEqual(TEXT("record receives its ID"), Record.ActionId, ActionId);

    FPaletteActionRecord Repeated = MakeActionRecord();
    const FString RepeatedId = RegisterPaletteActionToken(Repeated);
    TestEqual(TEXT("same canonical record is deterministic"), RepeatedId, ActionId);

    FPaletteActionRecord Resolved;
    FError Error;
    TestTrue(
        TEXT("original context resolves"),
        ResolvePaletteActionToken(
            ActionId, ExpectContext(MakeContext()), Resolved, Error));
    TestEqual(TEXT("resolved candidate key"), Resolved.CandidateKey, Record.CandidateKey);

    FPaletteContext OtherGraph = MakeContext();
    OtherGraph.GraphId =
        TEXT("graph:22222222-2222-4222-8222-222222222222");
    TestFalse(
        TEXT("cross-graph context rejected"),
        ResolvePaletteActionToken(
            ActionId, ExpectContext(OtherGraph), Resolved, Error));
    TestEqual(
        TEXT("cross-graph is a precondition"),
        Error.Code,
        FString(TEXT("PRECONDITION_FAILED")));

    TestFalse(
        TEXT("tampered token rejected"),
        ResolvePaletteActionToken(
            TamperToken(ActionId),
            ExpectContext(MakeContext()),
            Resolved,
            Error));
    TestEqual(
        TEXT("tampering is invalid input"),
        Error.Code,
        FString(TEXT("INVALID_INPUT")));

    FPaletteBindingRecord Binding;
    Binding.ActionId = ActionId;
    Binding.ObjectPath = TEXT("/Script/Engine.Actor:CustomTimeDilation");
    Binding.ExpectedClassPath = TEXT("/Script/CoreUObject.FloatProperty");
    const FString BindingId = RegisterPaletteBinding(Binding);
    TestTrue(
        TEXT("opaque binding prefix"),
        BindingId.StartsWith(TEXT("binding:")));

    TArray<FPaletteBindingRecord> Bindings;
    TestTrue(
        TEXT("binding resolves for its action"),
        ResolvePaletteBindings(
            ActionId, {BindingId}, Bindings, Error));
    TestEqual(TEXT("one binding resolved"), Bindings.Num(), 1);
    TestEqual(
        TEXT("binding path round trips"),
        Bindings[0].ObjectPath,
        Binding.ObjectPath);

    TestFalse(
        TEXT("binding cannot cross actions"),
        ResolvePaletteBindings(
            TEXT("action:cccccccccccccccccccccccccccccccccccccccc"),
            {BindingId},
            Bindings,
            Error));
    TestEqual(
        TEXT("cross-action binding is invalid"),
        Error.Code,
        FString(TEXT("INVALID_INPUT")));

    SetPaletteTokenClockForTests(TOptional<FDateTime>());
    ResetPaletteTokenStateForTests();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintSemanticCapabilityTest,
    "UnrealMCP.Blueprint2.Palette.SemanticCapabilities",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprintSemanticCapabilityTest::RunTest(
    const FString& Parameters)
{
    (void)Parameters;
    using namespace UE::MCPython::Blueprint2;

    ResetPaletteTokenStateForTests();
    ResetSemanticTokenStateForTests();
    const FDateTime Start(2026, 7, 30, 12, 0);
    SetPaletteTokenClockForTests(Start);
    SetSemanticTokenClockForTests(Start);

    FPaletteActionRecord GraphRecord = MakeActionRecord();
    GraphRecord.Context.Kind = EPaletteContextKind::Graph;
    const FString GraphActionId = RegisterPaletteActionToken(GraphRecord);

    FPaletteActionRecord PinRecord = MakeActionRecord();
    PinRecord.Context.Kind = EPaletteContextKind::Pin;
    PinRecord.Context.SourcePinId =
        TEXT("pin:22222222-2222-4222-8222-222222222222");
    const FString PinActionId = RegisterPaletteActionToken(PinRecord);

    FPaletteActionRecord ConnectionRecord = MakeActionRecord();
    ConnectionRecord.Context = MakeConnectionContext(false);
    const FString ConnectionActionId =
        RegisterPaletteActionToken(ConnectionRecord);

    FPaletteActionRecord ConversionRecord = ConnectionRecord;
    ConversionRecord.Context = MakeConnectionContext(true);
    const FString ConversionActionId =
        RegisterPaletteActionToken(ConversionRecord);

    TestNotEqual(TEXT("graph and pin actions differ"), GraphActionId, PinActionId);
    TestNotEqual(
        TEXT("pin and connection actions differ"),
        PinActionId,
        ConnectionActionId);
    TestNotEqual(
        TEXT("conversion policy changes action identity"),
        ConnectionActionId,
        ConversionActionId);

    FPaletteActionRecord ResolvedAction;
    FError Error;
    TestFalse(
        TEXT("connection action rejects pin expectation"),
        ResolvePaletteActionToken(
            ConnectionActionId,
            ExpectContext(PinRecord.Context),
            ResolvedAction,
            Error));
    TestEqual(
        TEXT("wrong context kind is stale"),
        Error.Code,
        FString(TEXT("PRECONDITION_FAILED")));

    FPaletteContext ChangedTarget = ConnectionRecord.Context;
    ChangedTarget.TargetPinId =
        TEXT("pin:55555555-5555-4555-8555-555555555555");
    TestFalse(
        TEXT("connection action rejects another target pin"),
        ResolvePaletteActionToken(
            ConnectionActionId,
            ExpectContext(ChangedTarget),
            ResolvedAction,
            Error));

    FPaletteContext ChangedPolicy = ConnectionRecord.Context;
    ChangedPolicy.bAllowConversion = true;
    TestFalse(
        TEXT("connection action rejects widened conversion policy"),
        ResolvePaletteActionToken(
            ConnectionActionId,
            ExpectContext(ChangedPolicy),
            ResolvedAction,
            Error));

    FPaletteBindingRecord ObjectBinding;
    ObjectBinding.Kind = EPaletteBindingKind::Object;
    ObjectBinding.ActionId = GraphActionId;
    ObjectBinding.ObjectPath =
        TEXT("/Script/Engine.Actor:CustomTimeDilation");
    ObjectBinding.ExpectedClassPath =
        TEXT("/Script/CoreUObject.FloatProperty");
    const FString ObjectBindingId =
        RegisterPaletteBinding(ObjectBinding);

    FPaletteBindingRecord PinBinding;
    PinBinding.Kind = EPaletteBindingKind::TemplatePin;
    PinBinding.ActionId = GraphActionId;
    PinBinding.PinName = TEXT("Value");
    PinBinding.PinDirection = TEXT("input");
    PinBinding.PinTypeJson = TEXT("{\"kind\":\"int\"}");
    PinBinding.PinOccurrence = 0;
    const FString PinBindingId =
        RegisterPaletteTemplatePinBinding(PinBinding);

    TArray<FPaletteBindingRecord> ObjectBindings;
    TestFalse(
        TEXT("template pin cannot resolve as object binding"),
        ResolvePaletteBindings(
            GraphActionId,
            {PinBindingId},
            ObjectBindings,
            Error));

    FPaletteBindingRecord ResolvedPin;
    TestFalse(
        TEXT("object binding cannot resolve as template pin"),
        ResolvePaletteTemplatePinBinding(
            GraphActionId,
            ObjectBindingId,
            ResolvedPin,
            Error));
    TestTrue(
        TEXT("template pin resolves for its action"),
        ResolvePaletteTemplatePinBinding(
            GraphActionId,
            PinBindingId,
            ResolvedPin,
            Error));
    TestEqual(
        TEXT("template pin name round trips"),
        ResolvedPin.PinName,
        FString(TEXT("Value")));

    TestTrue(
        TEXT("resolving the object binding refreshes shared binding LRU"),
        ResolvePaletteBindings(
            GraphActionId,
            {ObjectBindingId},
            ObjectBindings,
            Error));
    for (int32 Index = 1; Index <= 1023; ++Index)
    {
        FPaletteBindingRecord PagePinBinding = PinBinding;
        PagePinBinding.BindingId.Reset();
        PagePinBinding.PinOccurrence = Index;
        TestFalse(
            TEXT("page template binding registers"),
            RegisterPaletteTemplatePinBinding(PagePinBinding).IsEmpty());
    }
    TestTrue(
        TEXT("shared binding LRU retains the recently resolved object binding"),
        ResolvePaletteBindings(
            GraphActionId,
            {ObjectBindingId},
            ObjectBindings,
            Error));
    TestFalse(
        TEXT("shared 1024-record binding LRU evicts the untouched oldest template binding"),
        ResolvePaletteTemplatePinBinding(
            GraphActionId,
            PinBindingId,
            ResolvedPin,
            Error));

    FString FirstPlanId;
    FString SecondPlanId;
    for (int32 Index = 0; Index < 1024; ++Index)
    {
        FReplacementPlanRecord Plan = MakeReplacementPlan(
            FString::Printf(TEXT("plan-%04d"), Index));
        const FString PlanId = RegisterReplacementPlan(Plan);
        if (Index == 0)
        {
            FirstPlanId = PlanId;
        }
        else if (Index == 1)
        {
            SecondPlanId = PlanId;
        }
    }

    FReplacementPlanRecord ResolvedPlan;
    TestFalse(
        TEXT("malformed replacement plan token is invalid"),
        ResolveReplacementPlan(
            TEXT("replacement-plan:not-a-sha1"),
            GraphRecord.Context.AssetPath,
            GraphRecord.Context.GraphId,
            ResolvedPlan,
            Error));
    TestEqual(
        TEXT("malformed replacement plan reports invalid input"),
        Error.Code,
        FString(TEXT("INVALID_INPUT")));
    TestTrue(
        TEXT("oldest plan resolves and becomes most recently used"),
        ResolveReplacementPlan(
            FirstPlanId,
            GraphRecord.Context.AssetPath,
            GraphRecord.Context.GraphId,
            ResolvedPlan,
            Error));
    FReplacementPlanRecord Overflow = MakeReplacementPlan(TEXT("overflow"));
    RegisterReplacementPlan(Overflow);
    TestFalse(
        TEXT("least recently used plan is evicted"),
        ResolveReplacementPlan(
            SecondPlanId,
            GraphRecord.Context.AssetPath,
            GraphRecord.Context.GraphId,
            ResolvedPlan,
            Error));
    TestTrue(
        TEXT("touched oldest plan survives eviction"),
        ResolveReplacementPlan(
            FirstPlanId,
            GraphRecord.Context.AssetPath,
            GraphRecord.Context.GraphId,
            ResolvedPlan,
            Error));

    ResetSemanticTokenStateForTests();
    FReplacementPlanRecord Expiring = MakeReplacementPlan(TEXT("expires"));
    const FString ExpiringId = RegisterReplacementPlan(Expiring);
    SetSemanticTokenClockForTests(Start + FTimespan::FromMinutes(31.0));
    TestFalse(
        TEXT("replacement plan expires after thirty idle minutes"),
        ResolveReplacementPlan(
            ExpiringId,
            GraphRecord.Context.AssetPath,
            GraphRecord.Context.GraphId,
            ResolvedPlan,
            Error));
    TestEqual(
        TEXT("expired plan is invalid input"),
        Error.Code,
        FString(TEXT("INVALID_INPUT")));

    SetSemanticTokenClockForTests(Start);
    FReplacementPlanRecord TamperedPlan = MakeReplacementPlan(TEXT("tamper"));
    const FString TamperedPlanId = RegisterReplacementPlan(TamperedPlan);
    TestFalse(
        TEXT("tampered replacement plan is invalid"),
        ResolveReplacementPlan(
            TamperToken(TamperedPlanId),
            GraphRecord.Context.AssetPath,
            GraphRecord.Context.GraphId,
            ResolvedPlan,
            Error));
    ResetSemanticTokenStateForTests();
    TestFalse(
        TEXT("reset clears replacement plans"),
        ResolveReplacementPlan(
            TamperedPlanId,
            GraphRecord.Context.AssetPath,
            GraphRecord.Context.GraphId,
            ResolvedPlan,
            Error));

    SetPaletteTokenClockForTests(TOptional<FDateTime>());
    SetSemanticTokenClockForTests(TOptional<FDateTime>());
    ResetPaletteTokenStateForTests();
    ResetSemanticTokenStateForTests();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintPaletteCursorTest,
    "UnrealMCP.Blueprint2.Palette.CursorIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprintPaletteCursorTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace UE::MCPython::Blueprint2;

    ResetPaletteTokenStateForTests();
    const FDateTime Start(2026, 7, 30, 12, 0);
    SetPaletteTokenClockForTests(Start);

    FPaletteCursorRecord Cursor;
    Cursor.Context = MakeContext();
    Cursor.LastSortKey = TEXT("0600\ntransformation\nget actor location");
    const FString CursorId = RegisterPaletteCursor(Cursor);
    TestTrue(
        TEXT("opaque cursor prefix"),
        CursorId.StartsWith(TEXT("palette-cursor:")));

    FPaletteCursorRecord Resolved;
    FError Error;
    TestTrue(
        TEXT("cursor resolves for exact request"),
        ResolvePaletteCursor(CursorId, MakeContext(), Resolved, Error));
    TestEqual(
        TEXT("cursor last key round trips"),
        Resolved.LastSortKey,
        Cursor.LastSortKey);

    FPaletteContext ChangedRequest = MakeContext();
    ChangedRequest.RequestDigest =
        TEXT("sha1:cccccccccccccccccccccccccccccccccccccccc");
    TestFalse(
        TEXT("cursor rejects another query"),
        ResolvePaletteCursor(CursorId, ChangedRequest, Resolved, Error));
    TestEqual(
        TEXT("query replay is a precondition"),
        Error.Code,
        FString(TEXT("PRECONDITION_FAILED")));

    FPaletteContext ChangedResult = MakeContext();
    ChangedResult.ResultDigest =
        TEXT("sha1:dddddddddddddddddddddddddddddddddddddddd");
    TestFalse(
        TEXT("cursor rejects a changed result set"),
        ResolvePaletteCursor(CursorId, ChangedResult, Resolved, Error));
    TestEqual(
        TEXT("result replay is a precondition"),
        Error.Code,
        FString(TEXT("PRECONDITION_FAILED")));

    FPaletteContext ChangedLimit = MakeContext();
    ChangedLimit.Limit = 25;
    TestFalse(
        TEXT("cursor rejects a changed limit"),
        ResolvePaletteCursor(CursorId, ChangedLimit, Resolved, Error));
    TestEqual(
        TEXT("limit replay is a precondition"),
        Error.Code,
        FString(TEXT("PRECONDITION_FAILED")));

    ResetPaletteTokenStateForTests();
    FString OldestCursor;
    for (int32 Index = 0; Index < 1025; ++Index)
    {
        FPaletteCursorRecord Item;
        Item.Context = MakeContext();
        Item.LastSortKey = FString::Printf(TEXT("sort-%04d"), Index);
        const FString ItemId = RegisterPaletteCursor(Item);
        if (Index == 0)
        {
            OldestCursor = ItemId;
        }
    }
    TestFalse(
        TEXT("oldest cursor is evicted at the bound"),
        ResolvePaletteCursor(
            OldestCursor, MakeContext(), Resolved, Error));
    TestEqual(
        TEXT("evicted cursor is invalid input"),
        Error.Code,
        FString(TEXT("INVALID_INPUT")));

    ResetPaletteTokenStateForTests();
    FPaletteCursorRecord Expiring;
    Expiring.Context = MakeContext();
    Expiring.LastSortKey = TEXT("expires");
    SetPaletteTokenClockForTests(Start);
    const FString ExpiringId = RegisterPaletteCursor(Expiring);
    SetPaletteTokenClockForTests(Start + FTimespan::FromMinutes(31.0));
    TestFalse(
        TEXT("cursor expires after thirty minutes"),
        ResolvePaletteCursor(
            ExpiringId, MakeContext(), Resolved, Error));
    TestEqual(
        TEXT("expired cursor is invalid input"),
        Error.Code,
        FString(TEXT("INVALID_INPUT")));

    SetPaletteTokenClockForTests(TOptional<FDateTime>());
    ResetPaletteTokenStateForTests();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintPaletteSearchPaginationTest,
    "UnrealMCP.Blueprint2.Palette.SearchPagination",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprintPaletteSearchPaginationTest::RunTest(
    const FString& Parameters)
{
    (void)Parameters;
    using namespace UE::MCPython::Blueprint2;

    ResetPaletteTokenStateForTests();
    FPaletteBlueprintFixture Fixture = MakePaletteBlueprintFixture(
        TEXT("MCPythonBlueprintPaletteSearchPaginationTest"));
    ON_SCOPE_EXIT
    {
        ResetPaletteTokenStateForTests();
        CleanupFixturePackage(Fixture.Package);
    };
    TestNotNull(TEXT("palette search fixture Blueprint is created"), Fixture.Blueprint);
    TestNotNull(TEXT("palette search fixture EventGraph exists"), Fixture.Graph);
    TestFalse(TEXT("palette search fixture graph ID is stable"), Fixture.GraphId.IsEmpty());
    if (!Fixture.Blueprint || !Fixture.Graph || Fixture.GraphId.IsEmpty())
    {
        return false;
    }

    const TSharedRef<FJsonObject> Capabilities = BuildCapabilities(Fixture.Blueprint);
    TestTrue(
        TEXT("UE 5.7 advertises native Blueprint palette support"),
        Capabilities->GetBoolField(TEXT("supports_blueprint_node_palette")));
    TestTrue(
        TEXT("Actor Blueprint advertises a palette-compatible graph"),
        Capabilities->GetBoolField(TEXT("has_palette_compatible_graphs")));

    const FString SearchRequest = SerializeJsonObject(MakeSearchRequest(
        Fixture.GraphId, TEXT("Get Actor Location"), TEXT(""), 50));
    const TSharedPtr<FJsonObject> First = ParseJsonObject(
        UMCPythonHelper::SearchBlueprintNodeActions(
            Fixture.Blueprint, SearchRequest));
    const TSharedPtr<FJsonObject> Repeated = ParseJsonObject(
        UMCPythonHelper::SearchBlueprintNodeActions(
            Fixture.Blueprint, SearchRequest));
    TestTrue(
        TEXT("first palette search succeeds"),
        First.IsValid() && First->GetBoolField(TEXT("success")));
    TestTrue(
        TEXT("repeated palette search succeeds"),
        Repeated.IsValid() && Repeated->GetBoolField(TEXT("success")));
    if (!First || !Repeated ||
        !First->GetBoolField(TEXT("success")) ||
        !Repeated->GetBoolField(TEXT("success")))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> FirstData = First->GetObjectField(TEXT("data"));
    const TSharedPtr<FJsonObject> RepeatedData = Repeated->GetObjectField(TEXT("data"));
    const TArray<TSharedPtr<FJsonValue>>& FirstItems =
        FirstData->GetArrayField(TEXT("items"));
    const TArray<TSharedPtr<FJsonValue>>& RepeatedItems =
        RepeatedData->GetArrayField(TEXT("items"));
    TestTrue(TEXT("native palette search returns an action"), !FirstItems.IsEmpty());
    TestEqual(
        TEXT("repeated palette items are byte-for-byte deterministic"),
        SerializeJsonArray(RepeatedItems),
        SerializeJsonArray(FirstItems));
    TestEqual(
        TEXT("repeated palette result digest is deterministic"),
        RepeatedData->GetStringField(TEXT("result_digest")),
        FirstData->GetStringField(TEXT("result_digest")));
    if (!FirstItems.IsEmpty())
    {
        TestTrue(
            TEXT("palette action has an opaque action ID"),
            FirstItems[0]->AsObject()->GetStringField(TEXT("action_id"))
                .StartsWith(TEXT("action:")));
    }

    const TSharedPtr<FJsonObject> PageOne = ParseJsonObject(
        UMCPythonHelper::SearchBlueprintNodeActions(
            Fixture.Blueprint,
            SerializeJsonObject(MakeSearchRequest(
                Fixture.GraphId, TEXT(""), TEXT(""), 1))));
    TestTrue(
        TEXT("first palette page succeeds"),
        PageOne.IsValid() && PageOne->GetBoolField(TEXT("success")));
    if (!PageOne || !PageOne->GetBoolField(TEXT("success")))
    {
        return false;
    }
    const TSharedPtr<FJsonObject> PageOneData = PageOne->GetObjectField(TEXT("data"));
    const FString Cursor = PageOneData->GetStringField(TEXT("next_cursor"));
    TestFalse(TEXT("first bounded page returns a cursor"), Cursor.IsEmpty());
    const TSharedPtr<FJsonObject> PageTwo = ParseJsonObject(
        UMCPythonHelper::SearchBlueprintNodeActions(
            Fixture.Blueprint,
            SerializeJsonObject(MakeSearchRequest(
                Fixture.GraphId, TEXT(""), Cursor, 1))));
    TestTrue(
        TEXT("second palette page succeeds"),
        PageTwo.IsValid() && PageTwo->GetBoolField(TEXT("success")));
    if (PageTwo && PageTwo->GetBoolField(TEXT("success")))
    {
        const TArray<TSharedPtr<FJsonValue>>& PageOneItems =
            PageOneData->GetArrayField(TEXT("items"));
        const TArray<TSharedPtr<FJsonValue>>& PageTwoItems =
            PageTwo->GetObjectField(TEXT("data"))->GetArrayField(TEXT("items"));
        TestEqual(TEXT("first palette page has one item"), PageOneItems.Num(), 1);
        TestEqual(TEXT("second palette page has one item"), PageTwoItems.Num(), 1);
        if (PageOneItems.Num() == 1 && PageTwoItems.Num() == 1)
        {
            TestNotEqual(
                TEXT("palette pages do not duplicate action IDs"),
                PageOneItems[0]->AsObject()->GetStringField(TEXT("action_id")),
                PageTwoItems[0]->AsObject()->GetStringField(TEXT("action_id")));
        }
    }

    const TSharedPtr<FJsonObject> ChangedQuery = ParseJsonObject(
        UMCPythonHelper::SearchBlueprintNodeActions(
            Fixture.Blueprint,
            SerializeJsonObject(MakeSearchRequest(
                Fixture.GraphId, TEXT("actor"), Cursor, 1))));
    TestTrue(
        TEXT("cursor replay with another query is rejected"),
        ChangedQuery.IsValid() && !ChangedQuery->GetBoolField(TEXT("success")));
    if (ChangedQuery && !ChangedQuery->GetBoolField(TEXT("success")))
    {
        TestEqual(
            TEXT("changed-query cursor replay is a precondition"),
            ChangedQuery->GetArrayField(TEXT("errors"))[0]
                ->AsObject()->GetStringField(TEXT("code")),
            FString(TEXT("PRECONDITION_FAILED")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintPaletteDescribeTest,
    "UnrealMCP.Blueprint2.Palette.Describe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprintPaletteDescribeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace UE::MCPython::Blueprint2;

    ResetPaletteTokenStateForTests();
    FPaletteBlueprintFixture Fixture = MakePaletteBlueprintFixture(
        TEXT("MCPythonBlueprintPaletteDescribeTest"));
    ON_SCOPE_EXIT
    {
        ResetPaletteTokenStateForTests();
        CleanupFixturePackage(Fixture.Package);
    };
    TestNotNull(TEXT("palette describe fixture Blueprint is created"), Fixture.Blueprint);
    TestNotNull(TEXT("palette describe fixture EventGraph exists"), Fixture.Graph);
    if (!Fixture.Blueprint || !Fixture.Graph || Fixture.GraphId.IsEmpty())
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Search = ParseJsonObject(
        UMCPythonHelper::SearchBlueprintNodeActions(
            Fixture.Blueprint,
            SerializeJsonObject(MakeSearchRequest(
                Fixture.GraphId, TEXT("Get Actor Location"), TEXT(""), 50))));
    TestTrue(
        TEXT("palette describe setup search succeeds"),
        Search.IsValid() && Search->GetBoolField(TEXT("success")));
    if (!Search || !Search->GetBoolField(TEXT("success")))
    {
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>& Items =
        Search->GetObjectField(TEXT("data"))->GetArrayField(TEXT("items"));
    TestTrue(TEXT("palette describe setup returns an action"), !Items.IsEmpty());
    if (Items.IsEmpty())
    {
        return false;
    }

    const FString ActionId =
        Items[0]->AsObject()->GetStringField(TEXT("action_id"));
    const int32 NodeCountBefore = Fixture.Graph->Nodes.Num();
    const TSharedRef<FJsonObject> DescribeRequest = MakeShared<FJsonObject>();
    DescribeRequest->SetStringField(TEXT("action_id"), ActionId);
    const TSharedPtr<FJsonObject> Describe = ParseJsonObject(
        UMCPythonHelper::DescribeBlueprintNodeAction(
            SerializeJsonObject(DescribeRequest)));
    TestTrue(
        TEXT("palette action description succeeds"),
        Describe.IsValid() && Describe->GetBoolField(TEXT("success")));
    if (!Describe || !Describe->GetBoolField(TEXT("success")))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Data = Describe->GetObjectField(TEXT("data"));
    TestEqual(
        TEXT("description preserves the action title"),
        Data->GetStringField(TEXT("title")),
        Items[0]->AsObject()->GetStringField(TEXT("title")));
    TestFalse(
        TEXT("description returns the node class path"),
        Data->GetStringField(TEXT("node_class_path")).IsEmpty());
    TestFalse(
        TEXT("description returns the action owner path"),
        Data->GetStringField(TEXT("owner_path")).IsEmpty());
    TestFalse(
        TEXT("description returns the associated member path"),
        Data->GetStringField(TEXT("member_path")).IsEmpty());
    TestTrue(
        TEXT("description returns bindings"),
        Data->HasTypedField<EJson::Array>(TEXT("bindings")));
    TestTrue(
        TEXT("description returns restrictions"),
        Data->HasTypedField<EJson::Array>(TEXT("restrictions")));
    TestTrue(
        TEXT("description reports pin preview availability"),
        Data->HasTypedField<EJson::Boolean>(TEXT("pin_preview_available")));
    TestTrue(
        TEXT("description returns template pins"),
        Data->HasTypedField<EJson::Array>(TEXT("template_pins")));
    TestEqual(
        TEXT("describing an action does not mutate the target graph"),
        Fixture.Graph->Nodes.Num(),
        NodeCountBefore);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintPaletteSpawnTest,
    "UnrealMCP.Blueprint2.Palette.Spawn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprintPaletteSpawnTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace UE::MCPython::Blueprint2;

    ResetPaletteTokenStateForTests();
    FPaletteBlueprintFixture Fixture = MakePaletteBlueprintFixture(
        TEXT("MCPythonBlueprintPaletteSpawnTest"));
    ON_SCOPE_EXIT
    {
        ResetPaletteTokenStateForTests();
        CleanupFixturePackage(Fixture.Package);
    };
    TestNotNull(TEXT("palette spawn fixture Blueprint is created"), Fixture.Blueprint);
    TestNotNull(TEXT("palette spawn fixture EventGraph exists"), Fixture.Graph);
    if (!Fixture.Blueprint || !Fixture.Graph || Fixture.GraphId.IsEmpty())
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Search = ParseJsonObject(
        UMCPythonHelper::SearchBlueprintNodeActions(
            Fixture.Blueprint,
            SerializeJsonObject(MakeSearchRequest(
                Fixture.GraphId, TEXT("Get Actor Location"), TEXT(""), 50))));
    TestTrue(
        TEXT("palette spawn setup search succeeds"),
        Search.IsValid() && Search->GetBoolField(TEXT("success")));
    if (!Search || !Search->GetBoolField(TEXT("success")))
    {
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>& SearchItems =
        Search->GetObjectField(TEXT("data"))->GetArrayField(TEXT("items"));
    TestTrue(TEXT("palette spawn setup returns an action"), !SearchItems.IsEmpty());
    if (SearchItems.IsEmpty())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Action = SearchItems[0]->AsObject();
    const FString ActionId = Action->GetStringField(TEXT("action_id"));

    Fixture.Blueprint->Status = BS_UpToDate;
    Fixture.Package->SetDirtyFlag(false);
    const EBlueprintStatus StatusBefore = Fixture.Blueprint->Status;
    const bool bPackageDirtyBefore = Fixture.Package->IsDirty();
    const int32 NodeCountBefore = Fixture.Graph->Nodes.Num();
    TSet<UEdGraphNode*> NodesBefore;
    for (UEdGraphNode* Node : Fixture.Graph->Nodes)
    {
        if (Node)
        {
            NodesBefore.Add(Node);
        }
    }

    const TSharedPtr<FJsonObject> Spawn = ParseJsonObject(
        UMCPythonHelper::AddBlueprintActionNode(
            Fixture.Blueprint,
            SerializeJsonObject(MakeSpawnRequest(
                Fixture.GraphId, ActionId, 320.0, 160.0))));
    TestTrue(
        TEXT("palette action spawn succeeds"),
        Spawn.IsValid() && Spawn->GetBoolField(TEXT("success")));
    if (!Spawn || !Spawn->GetBoolField(TEXT("success")))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Data = Spawn->GetObjectField(TEXT("data"));
    TestEqual(
        TEXT("palette spawn adds exactly one primary node"),
        Fixture.Graph->Nodes.Num(),
        NodeCountBefore + 1);
    TestEqual(
        TEXT("palette spawn preserves its action ID"),
        Data->GetStringField(TEXT("action_id")),
        ActionId);
    const FString NodeId = Data->GetStringField(TEXT("node_id"));
    TestTrue(TEXT("palette spawn returns a stable node ID"), NodeId.StartsWith(TEXT("node:")));
    UEdGraphNode* NewNode = nullptr;
    for (UEdGraphNode* Node : Fixture.Graph->Nodes)
    {
        if (Node && !NodesBefore.Contains(Node))
        {
            TestNull(TEXT("palette spawn has only one primary node"), NewNode);
            NewNode = Node;
        }
    }
    TestNotNull(TEXT("palette spawn primary node exists"), NewNode);
    if (!NewNode)
    {
        return false;
    }
    TestEqual(
        TEXT("returned stable node ID identifies the new node"),
        NodeId,
        DescribeNodeTarget(Fixture.Blueprint, NewNode).Id);
    TestEqual(
        TEXT("palette spawn returns the exact node class"),
        Data->GetStringField(TEXT("class_path")),
        NewNode->GetClass()->GetPathName());
    TestEqual(TEXT("palette spawn x position is exact"), NewNode->NodePosX, 320);
    TestEqual(TEXT("palette spawn y position is exact"), NewNode->NodePosY, 160);
    TestEqual(
        TEXT("serialized x position is exact"),
        Data->GetObjectField(TEXT("position"))->GetIntegerField(TEXT("x")),
        320);
    TestEqual(
        TEXT("serialized y position is exact"),
        Data->GetObjectField(TEXT("position"))->GetIntegerField(TEXT("y")),
        160);

    const TArray<TSharedPtr<FJsonValue>>& PinIds =
        Data->GetArrayField(TEXT("pin_ids"));
    const TArray<TSharedPtr<FJsonValue>>& Pins = Data->GetArrayField(TEXT("pins"));
    TestFalse(TEXT("palette spawn returns visible stable pins"), PinIds.IsEmpty());
    TestEqual(TEXT("pin details align with pin IDs"), Pins.Num(), PinIds.Num());
    for (int32 Index = 0; Index < PinIds.Num(); ++Index)
    {
        const FString PinId = PinIds[Index]->AsString();
        TestTrue(
            *FString::Printf(TEXT("pin %d has a stable ID"), Index),
            PinId.StartsWith(TEXT("pin:")));
        TestEqual(
            *FString::Printf(TEXT("pin %d detail uses the same ID"), Index),
            Pins[Index]->AsObject()->GetStringField(TEXT("id")),
            PinId);
    }
    TestTrue(
        TEXT("simple function spawn has no auxiliary nodes"),
        Data->GetArrayField(TEXT("auxiliary_node_ids")).IsEmpty());
    const TArray<TSharedPtr<FJsonValue>>& Changes =
        Spawn->GetArrayField(TEXT("changes"));
    TestEqual(TEXT("palette spawn emits one change"), Changes.Num(), 1);
    if (Changes.Num() == 1)
    {
        TestEqual(
            TEXT("palette spawn emits a create change"),
            Changes[0]->AsObject()->GetStringField(TEXT("kind")),
            FString(TEXT("create")));
        TestEqual(
            TEXT("palette spawn change targets the new node"),
            Changes[0]->AsObject()->GetStringField(TEXT("target_id")),
            NodeId);
    }
    const TArray<TSharedPtr<FJsonValue>>& NextActions =
        Spawn->GetArrayField(TEXT("next_actions"));
    TestEqual(TEXT("palette spawn emits one next action"), NextActions.Num(), 1);
    if (NextActions.Num() == 1)
    {
        TestEqual(
            TEXT("palette spawn requires explicit compilation"),
            NextActions[0]->AsObject()->GetStringField(TEXT("action")),
            FString(TEXT("compile_blueprint")));
    }
    TestEqual(TEXT("fixture begins up to date"), StatusBefore, BS_UpToDate);
    TestFalse(TEXT("fixture begins with a clean package"), bPackageDirtyBefore);
    TestEqual(
        TEXT("palette spawn marks Blueprint compile status dirty"),
        Fixture.Blueprint->Status,
        BS_Dirty);
    TestTrue(
        TEXT("palette spawn marks the package dirty without saving"),
        Fixture.Package->IsDirty());

    const int32 CountAfterSpawn = Fixture.Graph->Nodes.Num();
    const TSharedPtr<FJsonObject> Tampered = ParseJsonObject(
        UMCPythonHelper::AddBlueprintActionNode(
            Fixture.Blueprint,
            SerializeJsonObject(MakeSpawnRequest(
                Fixture.GraphId, TamperToken(ActionId), 640.0, 160.0))));
    TestTrue(
        TEXT("tampered palette action is rejected"),
        Tampered.IsValid() && !Tampered->GetBoolField(TEXT("success")));
    TestEqual(
        TEXT("tampered palette action is invalid input"),
        FirstErrorCode(Tampered),
        FString(TEXT("INVALID_INPUT")));
    TestEqual(
        TEXT("tampered palette action does not mutate the graph"),
        Fixture.Graph->Nodes.Num(),
        CountAfterSpawn);

    const FGuid OriginalGraphGuid = Fixture.Graph->GraphGuid;
    Fixture.Graph->GraphGuid = FGuid::NewGuid();
    const TSharedPtr<FJsonObject> Stale = ParseJsonObject(
        UMCPythonHelper::AddBlueprintActionNode(
            Fixture.Blueprint,
            SerializeJsonObject(MakeSpawnRequest(
                Fixture.GraphId, ActionId, 640.0, 160.0))));
    TestTrue(
        TEXT("stale graph palette action is rejected"),
        Stale.IsValid() && !Stale->GetBoolField(TEXT("success")));
    TestEqual(
        TEXT("stale graph palette action is a precondition"),
        FirstErrorCode(Stale),
        FString(TEXT("PRECONDITION_FAILED")));
    TestEqual(
        TEXT("stale graph palette action does not invoke the spawner"),
        Fixture.Graph->Nodes.Num(),
        CountAfterSpawn);
    Fixture.Graph->GraphGuid = OriginalGraphGuid;

    const TSharedPtr<FJsonObject> EventSearch = ParseJsonObject(
        UMCPythonHelper::SearchBlueprintNodeActions(
            Fixture.Blueprint,
            SerializeJsonObject(MakeSearchRequest(
                Fixture.GraphId, TEXT("Event BeginPlay"), TEXT(""), 200))));
    TestTrue(
        TEXT("singleton event search succeeds"),
        EventSearch.IsValid() && EventSearch->GetBoolField(TEXT("success")));
    TSharedPtr<FJsonObject> EventAction;
    if (EventSearch && EventSearch->GetBoolField(TEXT("success")))
    {
        for (const TSharedPtr<FJsonValue>& Item :
            EventSearch->GetObjectField(TEXT("data"))->GetArrayField(TEXT("items")))
        {
            const TSharedPtr<FJsonObject> Candidate = Item->AsObject();
            if (Candidate->GetStringField(TEXT("action_kind")) == TEXT("event") &&
                Candidate->GetStringField(TEXT("member_path")).Contains(
                    TEXT("ReceiveBeginPlay")))
            {
                EventAction = Candidate;
                break;
            }
        }
    }
    TestTrue(
        TEXT("singleton BeginPlay event action is available"),
        EventAction.IsValid());
    if (EventAction)
    {
        const FString EventActionId =
            EventAction->GetStringField(TEXT("action_id"));
        const TSharedPtr<FJsonObject> ActivatedEvent = ParseJsonObject(
            UMCPythonHelper::AddBlueprintActionNode(
                Fixture.Blueprint,
                SerializeJsonObject(MakeSpawnRequest(
                    Fixture.GraphId,
                    EventActionId,
                    960.0,
                    160.0))));
        TestTrue(
            TEXT("native spawner activates the default BeginPlay ghost"),
            ActivatedEvent.IsValid() &&
                ActivatedEvent->GetBoolField(TEXT("success")));
        const int32 CountBeforeSingleton = Fixture.Graph->Nodes.Num();
        const TSharedPtr<FJsonObject> Singleton = ParseJsonObject(
            UMCPythonHelper::AddBlueprintActionNode(
                Fixture.Blueprint,
                SerializeJsonObject(MakeSpawnRequest(
                    Fixture.GraphId,
                    EventActionId,
                    1280.0,
                    160.0))));
        TestTrue(
            TEXT("existing singleton event is rejected"),
            Singleton.IsValid() && !Singleton->GetBoolField(TEXT("success")));
        TestEqual(
            TEXT("existing singleton event reports a precondition"),
            FirstErrorCode(Singleton),
            FString(TEXT("PRECONDITION_FAILED")));
        TestEqual(
            TEXT("singleton rejection rolls back without a second event"),
            Fixture.Graph->Nodes.Num(),
            CountBeforeSingleton);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintPalettePinSuggestionsTest,
    "UnrealMCP.Blueprint2.Palette.PinSuggestions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprintPalettePinSuggestionsTest::RunTest(
    const FString& Parameters)
{
    (void)Parameters;
    using namespace UE::MCPython::Blueprint2;

    ResetPaletteTokenStateForTests();
    FPaletteBlueprintFixture Fixture = MakePaletteBlueprintFixture(
        TEXT("MCPythonBlueprintPalettePinSuggestionsTest"));
    ON_SCOPE_EXIT
    {
        ResetPaletteTokenStateForTests();
        CleanupFixturePackage(Fixture.Package);
    };
    TestNotNull(TEXT("pin suggestion fixture Blueprint is created"), Fixture.Blueprint);
    TestNotNull(TEXT("pin suggestion fixture EventGraph exists"), Fixture.Graph);
    if (!Fixture.Blueprint || !Fixture.Graph || Fixture.GraphId.IsEmpty())
    {
        return false;
    }

    FGraphNodeCreator<UK2Node_CustomEvent> SourceCreator(*Fixture.Graph);
    UK2Node_CustomEvent* SourceNode = SourceCreator.CreateNode(false);
    SourceNode->CustomFunctionName = TEXT("PalettePinSource");
    SourceCreator.Finalize();
    UEdGraphPin* IntegerPin = SourceNode->CreatePin(
        EGPD_Output, UEdGraphSchema_K2::PC_Int, TEXT("IntegerValue"));
    UEdGraphPin* BooleanPin = SourceNode->CreatePin(
        EGPD_Output, UEdGraphSchema_K2::PC_Boolean, TEXT("BooleanValue"));
    UEdGraphPin* ExecPin = SourceNode->FindPin(
        UEdGraphSchema_K2::PN_Then, EGPD_Output);
    TestNotNull(TEXT("integer source pin exists"), IntegerPin);
    TestNotNull(TEXT("boolean source pin exists"), BooleanPin);
    TestNotNull(TEXT("execution source pin exists"), ExecPin);
    if (!IntegerPin || !BooleanPin || !ExecPin)
    {
        return false;
    }
    const FString IntegerPinId = DescribePinTarget(Fixture.Blueprint, IntegerPin).Id;
    const FString BooleanPinId = DescribePinTarget(Fixture.Blueprint, BooleanPin).Id;
    const FString ExecPinId = DescribePinTarget(Fixture.Blueprint, ExecPin).Id;
    TestTrue(TEXT("integer pin has a stable ID"), IntegerPinId.StartsWith(TEXT("pin:")));
    TestTrue(TEXT("boolean pin has a stable ID"), BooleanPinId.StartsWith(TEXT("pin:")));
    TestTrue(TEXT("exec pin has a stable ID"), ExecPinId.StartsWith(TEXT("pin:")));

    auto Suggest = [&Fixture](
        const FString& PinId,
        const FString& Query = FString(),
        const FString& Cursor = FString(),
        const int32 Limit = 50)
    {
        return ParseJsonObject(UMCPythonHelper::SuggestBlueprintNodesForPin(
            Fixture.Blueprint,
            SerializeJsonObject(MakeSuggestRequest(
                Fixture.GraphId, PinId, Query, Cursor, Limit))));
    };
    auto ActionIds = [](const TSharedPtr<FJsonObject>& Result)
    {
        TArray<FString> Ids;
        if (!Result || !Result->GetBoolField(TEXT("success")))
        {
            return Ids;
        }
        for (const TSharedPtr<FJsonValue>& Item :
            Result->GetObjectField(TEXT("data"))->GetArrayField(TEXT("items")))
        {
            Ids.Add(Item->AsObject()->GetStringField(TEXT("action_id")));
        }
        return Ids;
    };

    const TSharedPtr<FJsonObject> IntegerSuggestions = Suggest(IntegerPinId);
    const TSharedPtr<FJsonObject> BooleanSuggestions = Suggest(BooleanPinId);
    const TSharedPtr<FJsonObject> ExecSuggestions = Suggest(ExecPinId);
    TestTrue(
        TEXT("integer pin suggestions succeed"),
        IntegerSuggestions.IsValid() &&
            IntegerSuggestions->GetBoolField(TEXT("success")));
    TestTrue(
        TEXT("boolean pin suggestions succeed"),
        BooleanSuggestions.IsValid() &&
            BooleanSuggestions->GetBoolField(TEXT("success")));
    TestTrue(
        TEXT("exec pin suggestions succeed"),
        ExecSuggestions.IsValid() &&
            ExecSuggestions->GetBoolField(TEXT("success")));
    if (IntegerSuggestions && IntegerSuggestions->GetBoolField(TEXT("success")))
    {
        TestTrue(
            TEXT("suggestion summary identifies the native filter authority"),
            IntegerSuggestions->GetStringField(TEXT("summary")).Contains(
                TEXT("compatible according to the current native action filter")));
    }
    const TArray<FString> IntegerIds = ActionIds(IntegerSuggestions);
    const TArray<FString> BooleanIds = ActionIds(BooleanSuggestions);
    const TArray<FString> ExecIds = ActionIds(ExecSuggestions);
    TestTrue(TEXT("integer suggestions exist"), IntegerIds.Num() > 0);
    TestTrue(TEXT("boolean suggestions exist"), BooleanIds.Num() > 0);
    TestTrue(TEXT("exec suggestions exist"), ExecIds.Num() > 0);
    TestTrue(TEXT("typed contexts differ"), IntegerIds != BooleanIds);
    TestTrue(TEXT("data and exec contexts differ"), IntegerIds != ExecIds);

    const TArray<TPair<TSharedPtr<FJsonObject>, FString>> SuggestionContexts = {
        {IntegerSuggestions, IntegerPinId},
        {BooleanSuggestions, BooleanPinId},
        {ExecSuggestions, ExecPinId}};
    for (const TPair<TSharedPtr<FJsonObject>, FString>& Context : SuggestionContexts)
    {
        if (!Context.Key || !Context.Key->GetBoolField(TEXT("success")))
        {
            continue;
        }
        const TSharedPtr<FJsonObject> Data = Context.Key->GetObjectField(TEXT("data"));
        TestEqual(
            TEXT("suggestion page preserves the exact source pin"),
            Data->GetStringField(TEXT("source_pin_id")),
            Context.Value);
        for (const TSharedPtr<FJsonValue>& Item : Data->GetArrayField(TEXT("items")))
        {
            const TSharedPtr<FJsonObject> Card = Item->AsObject();
            TestTrue(
                TEXT("suggestion card exposes explicit connection bindings"),
                Card->HasTypedField<EJson::Array>(TEXT("connection_bindings")));
            const TArray<TSharedPtr<FJsonValue>>& ConnectionBindings =
                Card->GetArrayField(TEXT("connection_bindings"));
            TestFalse(
                TEXT("every returned suggestion has a compatible template pin"),
                ConnectionBindings.IsEmpty());

            FPaletteContextExpectation Expected;
            Expected.Kind = EPaletteContextKind::Pin;
            Expected.SourcePinId = Context.Value;
            Expected.Limit = 0;
            FPaletteActionRecord Record;
            FError Error;
            TestTrue(
                TEXT("suggestion action token preserves source pin ownership"),
                ResolvePaletteActionToken(
                    Card->GetStringField(TEXT("action_id")),
                    Expected,
                    Record,
                    Error));
            TestEqual(
                TEXT("suggestion record contains the exact source pin"),
                Record.Context.SourcePinId,
                Context.Value);

            bool bSeenConversion = false;
            for (int32 BindingIndex = 0;
                 BindingIndex < ConnectionBindings.Num();
                 ++BindingIndex)
            {
                const TSharedPtr<FJsonObject> Binding =
                    ConnectionBindings[BindingIndex]->AsObject();
                TestEqual(
                    TEXT("connection binding rank is contiguous"),
                    Binding->GetIntegerField(TEXT("rank")),
                    BindingIndex);
                TestEqual(
                    TEXT("output source suggests input template pins"),
                    Binding->GetStringField(TEXT("direction")),
                    FString(TEXT("input")));

                FPaletteBindingRecord ResolvedBinding;
                TestTrue(
                    TEXT("connection binding resolves for its action"),
                    ResolvePaletteTemplatePinBinding(
                        Card->GetStringField(TEXT("action_id")),
                        Binding->GetStringField(TEXT("binding_id")),
                        ResolvedBinding,
                        Error));
                TestEqual(
                    TEXT("resolved binding keeps the pin direction"),
                    ResolvedBinding.PinDirection,
                    Binding->GetStringField(TEXT("direction")));

                const bool bRequiresConversion =
                    Binding->GetObjectField(TEXT("response"))
                        ->GetBoolField(TEXT("requires_conversion"));
                TestFalse(
                    TEXT("direct bindings precede conversion bindings"),
                    bSeenConversion && !bRequiresConversion);
                bSeenConversion |= bRequiresConversion;
            }
        }
    }

    const TSharedPtr<FJsonObject> IntegerPage = Suggest(
        IntegerPinId, TEXT(""), TEXT(""), 1);
    TestTrue(
        TEXT("bounded integer suggestion page succeeds"),
        IntegerPage.IsValid() && IntegerPage->GetBoolField(TEXT("success")));
    const FString IntegerCursor = IntegerPage &&
        IntegerPage->GetBoolField(TEXT("success"))
        ? IntegerPage->GetObjectField(TEXT("data"))->GetStringField(TEXT("next_cursor"))
        : FString();
    TestFalse(TEXT("bounded pin suggestions return a cursor"), IntegerCursor.IsEmpty());
    const TSharedPtr<FJsonObject> CrossPinCursor = Suggest(
        BooleanPinId, TEXT(""), IntegerCursor, 1);
    TestTrue(
        TEXT("pin cursor cannot be replayed for another pin"),
        CrossPinCursor.IsValid() &&
            !CrossPinCursor->GetBoolField(TEXT("success")));
    TestEqual(
        TEXT("cross-pin cursor replay is a precondition"),
        FirstErrorCode(CrossPinCursor),
        FString(TEXT("PRECONDITION_FAILED")));

    UEdGraph* OtherGraph = FBlueprintEditorUtils::CreateNewGraph(
        Fixture.Blueprint,
        TEXT("PaletteOtherGraph"),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    Fixture.Blueprint->FunctionGraphs.Add(OtherGraph);
    FGraphNodeCreator<UK2Node_CustomEvent> OtherCreator(*OtherGraph);
    UK2Node_CustomEvent* OtherNode = OtherCreator.CreateNode(false);
    OtherNode->CustomFunctionName = TEXT("PaletteOtherSource");
    OtherCreator.Finalize();
    UEdGraphPin* OtherPin = OtherNode->CreatePin(
        EGPD_Output, UEdGraphSchema_K2::PC_Int, TEXT("OtherValue"));
    const FString OtherPinId = DescribePinTarget(Fixture.Blueprint, OtherPin).Id;
    const int32 MainCountBeforeCrossGraph = Fixture.Graph->Nodes.Num();
    const int32 OtherCountBeforeCrossGraph = OtherGraph->Nodes.Num();
    const TSharedPtr<FJsonObject> CrossGraph = Suggest(OtherPinId);
    TestTrue(
        TEXT("pin from another graph is rejected"),
        CrossGraph.IsValid() && !CrossGraph->GetBoolField(TEXT("success")));
    TestEqual(
        TEXT("cross-graph pin rejection is a precondition"),
        FirstErrorCode(CrossGraph),
        FString(TEXT("PRECONDITION_FAILED")));
    TestEqual(
        TEXT("cross-graph pin rejection leaves requested graph unchanged"),
        Fixture.Graph->Nodes.Num(),
        MainCountBeforeCrossGraph);
    TestEqual(
        TEXT("cross-graph pin rejection leaves source graph unchanged"),
        OtherGraph->Nodes.Num(),
        OtherCountBeforeCrossGraph);

    const TSharedPtr<FJsonObject> FlowSuggestions = Suggest(
        ExecPinId, TEXT("Sequence"), TEXT(""), 200);
    TestTrue(
        TEXT("execution Sequence suggestions succeed"),
        FlowSuggestions.IsValid() &&
            FlowSuggestions->GetBoolField(TEXT("success")));
    TSharedPtr<FJsonObject> FlowAction;
    if (FlowSuggestions && FlowSuggestions->GetBoolField(TEXT("success")))
    {
        const TSharedPtr<FJsonObject> FlowData =
            FlowSuggestions->GetObjectField(TEXT("data"));
        for (const TSharedPtr<FJsonValue>& Item : FlowData->GetArrayField(TEXT("items")))
        {
            const TSharedPtr<FJsonObject> Candidate = Item->AsObject();
            if (Candidate->GetStringField(TEXT("title")).Contains(
                    TEXT("Sequence")) &&
                Candidate->GetStringField(TEXT("action_kind")) ==
                    TEXT("flow_control") &&
                !Candidate->GetBoolField(TEXT("requires_binding")))
            {
                FlowAction = Candidate;
                break;
            }
        }
    }
    TestTrue(
        TEXT("Sequence is suggested with an explicit exec input binding"),
        FlowAction.IsValid());
    if (FlowAction)
    {
        const FString ActionId = FlowAction->GetStringField(TEXT("action_id"));
        const TSharedRef<FJsonObject> DescribeRequest = MakeShared<FJsonObject>();
        DescribeRequest->SetStringField(TEXT("action_id"), ActionId);
        const TSharedPtr<FJsonObject> Description = ParseJsonObject(
            UMCPythonHelper::DescribeBlueprintNodeAction(
                SerializeJsonObject(DescribeRequest)));
        TestTrue(
            TEXT("pin suggestion action can be described"),
            Description.IsValid() && Description->GetBoolField(TEXT("success")));
        if (Description && Description->GetBoolField(TEXT("success")))
        {
            TestEqual(
                TEXT("description preserves suggestion source pin"),
                Description->GetObjectField(TEXT("data"))->GetStringField(
                    TEXT("source_pin_id")),
                ExecPinId);
        }

        const int32 CountBeforeSpawn = Fixture.Graph->Nodes.Num();
        const TSharedPtr<FJsonObject> Spawn = ParseJsonObject(
            UMCPythonHelper::AddBlueprintActionNode(
                Fixture.Blueprint,
                SerializeJsonObject(MakeSpawnRequest(
                    Fixture.GraphId, ActionId, 1600.0, 400.0))));
        TestTrue(
            TEXT("pin suggestion action can be spawned while source pin is valid"),
            Spawn.IsValid() && Spawn->GetBoolField(TEXT("success")));
        TestEqual(
            TEXT("pin suggestion spawn adds one node"),
            Fixture.Graph->Nodes.Num(),
            CountBeforeSpawn + 1);
        FTargetRef PinTarget;
        PinTarget.Id = ExecPinId;
        FString ResolveError;
        const FResolvedTarget ResolvedPin = ResolveTarget(
            Fixture.Blueprint, ETargetKind::Pin, PinTarget, ResolveError);
        TestTrue(
            TEXT("source pin remains valid after suggestion spawn"),
            ResolvedPin.bStable && ResolvedPin.Pin == ExecPin);
    }
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
