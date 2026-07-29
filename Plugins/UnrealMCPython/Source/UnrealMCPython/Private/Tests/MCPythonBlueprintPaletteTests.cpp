// Copyright (c) 2025 GenOrca. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "MCPythonBlueprint2Internal.h"
#include "MCPythonHelper.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
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
            ActionId, MakeContext(), Resolved, Error));
    TestEqual(TEXT("resolved candidate key"), Resolved.CandidateKey, Record.CandidateKey);

    FPaletteContext OtherGraph = MakeContext();
    OtherGraph.GraphId =
        TEXT("graph:22222222-2222-4222-8222-222222222222");
    TestFalse(
        TEXT("cross-graph context rejected"),
        ResolvePaletteActionToken(ActionId, OtherGraph, Resolved, Error));
    TestEqual(
        TEXT("cross-graph is a precondition"),
        Error.Code,
        FString(TEXT("PRECONDITION_FAILED")));

    TestFalse(
        TEXT("tampered token rejected"),
        ResolvePaletteActionToken(
            TamperToken(ActionId), MakeContext(), Resolved, Error));
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

#endif // WITH_DEV_AUTOMATION_TESTS
