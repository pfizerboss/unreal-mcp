// Copyright (c) 2025 GenOrca. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "MCPythonBlueprint2Internal.h"

#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
