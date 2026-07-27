// Copyright (c) 2025 GenOrca. All Rights Reserved.

#include "MCPythonBlueprint2Internal.h"

#include "MCPythonHelperInternal.h"

#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::MCPython::Blueprint2
{
namespace
{
const TCHAR* PrefixForKind(ETargetKind Kind)
{
    switch (Kind)
    {
    case ETargetKind::Graph: return TEXT("graph");
    case ETargetKind::Node: return TEXT("node");
    case ETargetKind::Pin: return TEXT("pin");
    case ETargetKind::Variable: return TEXT("variable");
    case ETargetKind::Component: return TEXT("component");
    case ETargetKind::Interface: return TEXT("interface");
    }
    return TEXT("unknown");
}

FString Sha1(const FString& Value)
{
    FTCHARToUTF8 Utf8(*Value);
    uint8 Digest[FSHA1::DigestSize];
    FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
    return BytesToHex(Digest, FSHA1::DigestSize).ToLower();
}

FString EscapeCanonicalJsonString(const FString& Value)
{
    FString Result(TEXT("\""));
    for (const TCHAR Character : Value)
    {
        switch (Character)
        {
        case TEXT('"'): Result += TEXT("\\\""); break;
        case TEXT('\\'): Result += TEXT("\\\\"); break;
        case TEXT('\b'): Result += TEXT("\\b"); break;
        case TEXT('\f'): Result += TEXT("\\f"); break;
        case TEXT('\n'): Result += TEXT("\\n"); break;
        case TEXT('\r'): Result += TEXT("\\r"); break;
        case TEXT('\t'): Result += TEXT("\\t"); break;
        default:
            if (Character < 0x20)
            {
                Result += FString::Printf(TEXT("\\u%04x"), Character);
            }
            else
            {
                Result.AppendChar(Character);
            }
            break;
        }
    }
    Result.AppendChar(TEXT('"'));
    return Result;
}

FString CanonicalJsonValue(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid() || Value->IsNull())
    {
        return TEXT("null");
    }
    switch (Value->Type)
    {
    case EJson::String:
        return EscapeCanonicalJsonString(Value->AsString());
    case EJson::Number:
        return FString::SanitizeFloat(Value->AsNumber(), 0);
    case EJson::Boolean:
        return Value->AsBool() ? TEXT("true") : TEXT("false");
    case EJson::Array:
    {
        TArray<FString> Items;
        for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
        {
            Items.Add(CanonicalJsonValue(Item));
        }
        return FString::Printf(TEXT("[%s]"), *FString::Join(Items, TEXT(",")));
    }
    case EJson::Object:
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        TArray<FString> Keys;
        Object->Values.GetKeys(Keys);
        Keys.Sort();
        TArray<FString> Fields;
        for (const FString& Key : Keys)
        {
            Fields.Add(EscapeCanonicalJsonString(Key) + TEXT(":") +
                CanonicalJsonValue(Object->Values[Key]));
        }
        return FString::Printf(TEXT("{%s}"), *FString::Join(Fields, TEXT(",")));
    }
    default:
        return TEXT("null");
    }
}

const FTransaction* TransactionAtIndex(int32 TransactionIndex)
{
    if (!GEditor || !GEditor->Trans || TransactionIndex < 0 ||
        TransactionIndex >= GEditor->Trans->GetQueueLength())
    {
        return nullptr;
    }
    return GEditor->Trans->GetTransaction(TransactionIndex);
}

FGuid TransactionGuidAtIndex(int32 TransactionIndex)
{
    const FTransaction* Transaction = TransactionAtIndex(TransactionIndex);
    return Transaction ? Transaction->GetContext().TransactionId : FGuid();
}

bool IsCurrentTransaction(int32 TransactionIndex, const FGuid& TransactionGuid)
{
    if (!GEditor || !GEditor->Trans || !TransactionGuid.IsValid())
    {
        return false;
    }
    const int32 CurrentUndoIndex =
        GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount() - 1;
    const FTransaction* Transaction = TransactionAtIndex(TransactionIndex);
    return CurrentUndoIndex == TransactionIndex && Transaction &&
        !Transaction->HasExpired() &&
        Transaction->GetContext().TransactionId == TransactionGuid;
}

FString TargetTypePath(const UEdGraph* Graph)
{
    return Graph && Graph->GetSchema()
        ? Graph->GetSchema()->GetClass()->GetPathName()
        : FString();
}

FString TargetTypePath(const UEdGraphNode* Node)
{
    return Node ? Node->GetClass()->GetPathName() : FString();
}

FString PinContainerName(const EPinContainerType ContainerType)
{
    switch (ContainerType)
    {
    case EPinContainerType::None: return TEXT("none");
    case EPinContainerType::Array: return TEXT("array");
    case EPinContainerType::Set: return TEXT("set");
    case EPinContainerType::Map: return TEXT("map");
    }
    return TEXT("unknown");
}

FString CanonicalPinType(const FEdGraphPinType& Type)
{
    const FSimpleMemberReference& Member = Type.PinSubCategoryMemberReference;
    const FEdGraphTerminalType& Terminal = Type.PinValueType;
    const FString MemberGuid = Member.MemberGuid.IsValid()
        ? Member.MemberGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
        : FString();
    return FString::Printf(
        TEXT("category=%s\nsubcategory=%s\nsubcategory_object=%s\n")
        TEXT("member_parent=%s\nmember_name=%s\nmember_guid=%s\n")
        TEXT("container=%s\nvalue_category=%s\nvalue_subcategory=%s\n")
        TEXT("value_subcategory_object=%s\nvalue_const=%d\nvalue_weak=%d\n")
        TEXT("value_wrapper=%d\nreference=%d\nconst=%d\nweak=%d\n")
        TEXT("wrapper=%d\nsingle_precision=%d"),
        *Type.PinCategory.ToString(),
        *Type.PinSubCategory.ToString(),
        *GetPathNameSafe(Type.PinSubCategoryObject.Get()),
        *GetPathNameSafe(Member.MemberParent.Get()),
        *Member.MemberName.ToString(),
        *MemberGuid,
        *PinContainerName(Type.ContainerType),
        *Terminal.TerminalCategory.ToString(),
        *Terminal.TerminalSubCategory.ToString(),
        *GetPathNameSafe(Terminal.TerminalSubCategoryObject.Get()),
        Terminal.bTerminalIsConst ? 1 : 0,
        Terminal.bTerminalIsWeakPointer ? 1 : 0,
        Terminal.bTerminalIsUObjectWrapper ? 1 : 0,
        Type.bIsReference ? 1 : 0,
        Type.bIsConst ? 1 : 0,
        Type.bIsWeakPointer ? 1 : 0,
        Type.bIsUObjectWrapper ? 1 : 0,
        Type.bSerializeAsSinglePrecisionFloat ? 1 : 0);
}

FString PinQualificationType(const UEdGraphPin* Pin)
{
    if (!Pin)
    {
        return {};
    }
    int32 MatchingPinOrdinal = 0;
    const UEdGraphNode* OwningNode = Pin->GetOwningNode();
    if (OwningNode)
    {
        for (const UEdGraphPin* Candidate : OwningNode->Pins)
        {
            if (Candidate == Pin)
            {
                break;
            }
            if (Candidate &&
                Candidate->PinName == Pin->PinName &&
                Candidate->Direction == Pin->Direction &&
                CanonicalPinType(Candidate->PinType) ==
                    CanonicalPinType(Pin->PinType))
            {
                ++MatchingPinOrdinal;
            }
        }
    }
    return FString::Printf(
        TEXT("%s\ndirection=%d\nordinal=%d"),
        *CanonicalPinType(Pin->PinType),
        static_cast<int32>(Pin->Direction),
        MatchingPinOrdinal);
}

FResolvedTarget StableGraph(UBlueprint* Blueprint, const FGuid& Guid)
{
    FResolvedTarget Result;
    if (!Blueprint || !Guid.IsValid())
    {
        return Result;
    }
    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        if (Graph && Graph->GraphGuid == Guid)
        {
            Result.Object = Graph;
            Result.Graph = Graph;
            Result.Id = MakeTargetId(ETargetKind::Graph, Guid);
            Result.IdKind = TEXT("graph_guid");
            Result.bStable = true;
            return Result;
        }
    }
    return Result;
}

FResolvedTarget StableNode(UBlueprint* Blueprint, const FGuid& Guid)
{
    FResolvedTarget Result;
    TArray<UEdGraph*> Graphs;
    if (Blueprint)
    {
        Blueprint->GetAllGraphs(Graphs);
    }
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid == Guid)
            {
                Result.Object = Node;
                Result.Graph = Graph;
                Result.Node = Node;
                Result.Id = MakeTargetId(ETargetKind::Node, Guid);
                Result.IdKind = TEXT("node_guid");
                Result.bStable = true;
                return Result;
            }
        }
    }
    return Result;
}

FResolvedTarget StablePin(UBlueprint* Blueprint, const FGuid& Guid)
{
    FResolvedTarget Result;
    TArray<UEdGraph*> Graphs;
    if (Blueprint)
    {
        Blueprint->GetAllGraphs(Graphs);
    }
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->PinId == Guid)
                {
                    Result.Graph = Graph;
                    Result.Node = Node;
                    Result.Pin = Pin;
                    Result.Id = MakeTargetId(ETargetKind::Pin, Guid);
                    Result.IdKind = TEXT("pin_guid");
                    Result.bStable = true;
                    return Result;
                }
            }
        }
    }
    return Result;
}

FResolvedTarget StableVariable(UBlueprint* Blueprint, const FGuid& Guid)
{
    FResolvedTarget Result;
    if (!Blueprint) return Result;
    for (FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        if (Variable.VarGuid == Guid)
        {
            Result.Variable = &Variable;
            Result.Id = MakeTargetId(ETargetKind::Variable, Guid);
            Result.IdKind = TEXT("variable_guid");
            Result.bStable = true;
            return Result;
        }
    }
    return Result;
}

FResolvedTarget StableComponent(UBlueprint* Blueprint, const FGuid& Guid)
{
    FResolvedTarget Result;
    if (!Blueprint || !Blueprint->SimpleConstructionScript) return Result;
    if (USCS_Node* Component =
        Blueprint->SimpleConstructionScript->FindSCSNodeByGuid(Guid);
        Component && Component->VariableGuid == Guid)
    {
        Result.Object = Component;
        Result.Component = Component;
        Result.Id = MakeTargetId(ETargetKind::Component, Guid);
        Result.IdKind = TEXT("scs_variable_guid");
        Result.bStable = true;
    }
    return Result;
}

FResolvedTarget StableInterface(UBlueprint* Blueprint, const FString& Id)
{
    FResolvedTarget Result;
    if (!Blueprint) return Result;
    for (FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
    {
        UClass* InterfaceClass = Description.Interface.Get();
        if (!InterfaceClass) continue;
        const FString Path = InterfaceClass->GetPathName();
        const FString Expected = FString::Printf(TEXT("interface:%s"), *Path);
        if (Id == Expected)
        {
            Result.Object = InterfaceClass;
            Result.Id = Expected;
            Result.IdKind = TEXT("interface_path");
            Result.bStable = true;
            return Result;
        }
    }
    return Result;
}

TSharedPtr<FJsonValue> EmptyArrayValue()
{
    return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
}
}

FString MakeTargetId(ETargetKind Kind, const FGuid& Guid)
{
    return FString::Printf(
        TEXT("%s:%s"),
        PrefixForKind(Kind),
        *Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
}

FString MakeQualifiedFallbackId(
    ETargetKind Kind,
    const FString& Owner,
    const FString& Name,
    const FString& TypePath)
{
    return FString::Printf(
        TEXT("fallback:%s:%s"),
        PrefixForKind(Kind),
        *Sha1(Owner + TEXT("\n") + Name + TEXT("\n") + TypePath));
}

FTargetRef DescribeGraphTarget(UBlueprint* Blueprint, const UEdGraph* Graph)
{
    FTargetRef Result;
    if (!Graph)
    {
        return Result;
    }
    Result.OwnerId = Graph->GetOuter()
        ? Graph->GetOuter()->GetPathName()
        : (Blueprint ? Blueprint->GetPathName() : FString());
    Result.Name = Graph->GetName();
    Result.TypePath = TargetTypePath(Graph);
    if (Graph->GraphGuid.IsValid())
    {
        Result.Id = MakeTargetId(ETargetKind::Graph, Graph->GraphGuid);
        return Result;
    }
    Result.Id = MakeQualifiedFallbackId(
        ETargetKind::Graph,
        Result.OwnerId,
        Result.Name,
        Result.TypePath);
    return Result;
}

FTargetRef DescribeNodeTarget(UBlueprint* Blueprint, const UEdGraphNode* Node)
{
    FTargetRef Result;
    if (!Node)
    {
        return Result;
    }
    Result.OwnerId = MakeGraphTargetId(Blueprint, Node->GetGraph());
    Result.Name = Node->GetName();
    Result.TypePath = TargetTypePath(Node);
    if (Node->NodeGuid.IsValid())
    {
        Result.Id = MakeTargetId(ETargetKind::Node, Node->NodeGuid);
        return Result;
    }
    Result.Id = MakeQualifiedFallbackId(
        ETargetKind::Node,
        Result.OwnerId,
        Result.Name,
        Result.TypePath);
    return Result;
}

FTargetRef DescribePinTarget(UBlueprint* Blueprint, const UEdGraphPin* Pin)
{
    FTargetRef Result;
    if (!Pin)
    {
        return Result;
    }
    const UEdGraphNode* OwningNode = Pin->GetOwningNode();
    Result.OwnerId = MakeNodeTargetId(Blueprint, OwningNode);
    Result.Name = Pin->GetName();
    Result.TypePath = PinQualificationType(Pin);
    if (Pin->PinId.IsValid())
    {
        Result.Id = MakeTargetId(ETargetKind::Pin, Pin->PinId);
        return Result;
    }
    Result.Id = MakeQualifiedFallbackId(
        ETargetKind::Pin,
        Result.OwnerId,
        Result.Name,
        Result.TypePath);
    return Result;
}

FTargetRef DescribeVariableTarget(
    UBlueprint* Blueprint,
    const FBPVariableDescription& Variable)
{
    FTargetRef Result;
    Result.OwnerId = Blueprint ? Blueprint->GetPathName() : FString();
    Result.Name = Variable.VarName.ToString();
    Result.TypePath = CanonicalPinType(Variable.VarType);
    Result.Id = Variable.VarGuid.IsValid()
        ? MakeTargetId(ETargetKind::Variable, Variable.VarGuid)
        : MakeQualifiedFallbackId(
            ETargetKind::Variable,
            Result.OwnerId,
            Result.Name,
            Result.TypePath);
    return Result;
}

FTargetRef DescribeComponentTarget(
    UBlueprint* Blueprint,
    const USCS_Node* Component)
{
    FTargetRef Result;
    if (!Component)
    {
        return Result;
    }
    Result.OwnerId = Blueprint ? Blueprint->GetPathName() : FString();
    Result.Name = Component->GetVariableName().ToString();
    const UClass* ComponentClass = Component->ComponentClass
        ? Component->ComponentClass.Get()
        : (Component->ComponentTemplate
            ? Component->ComponentTemplate->GetClass()
            : nullptr);
    Result.TypePath = ComponentClass ? ComponentClass->GetPathName() : FString();
    Result.Id = Component->VariableGuid.IsValid()
        ? MakeTargetId(ETargetKind::Component, Component->VariableGuid)
        : MakeQualifiedFallbackId(
            ETargetKind::Component,
            Result.OwnerId,
            Result.Name,
            Result.TypePath);
    return Result;
}

FString MakeGraphTargetId(UBlueprint* Blueprint, const UEdGraph* Graph)
{
    return DescribeGraphTarget(Blueprint, Graph).Id;
}

FString MakeNodeTargetId(UBlueprint* Blueprint, const UEdGraphNode* Node)
{
    return DescribeNodeTarget(Blueprint, Node).Id;
}

FString MakePinTargetId(UBlueprint* Blueprint, const UEdGraphPin* Pin)
{
    return DescribePinTarget(Blueprint, Pin).Id;
}

bool ParseTargetId(
    const FString& Id,
    ETargetKind ExpectedKind,
    FGuid& OutGuid)
{
    FString Prefix;
    FString Value;
    if (!Id.Split(TEXT(":"), &Prefix, &Value) ||
        Prefix != PrefixForKind(ExpectedKind))
    {
        return false;
    }
    return FGuid::Parse(Value, OutGuid) &&
        OutGuid.IsValid() &&
        Value.Equals(
            OutGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
            ESearchCase::CaseSensitive);
}

FResolvedTarget ResolveTarget(
    UBlueprint* Blueprint,
    ETargetKind Kind,
    const FTargetRef& Target,
    FString& OutError)
{
    OutError.Empty();
    if (!Blueprint)
    {
        OutError = TEXT("Blueprint is required.");
        return {};
    }

    if (Kind == ETargetKind::Interface)
    {
        FResolvedTarget Interface = StableInterface(Blueprint, Target.Id);
        if (Interface.bStable)
        {
            return Interface;
        }
        if (Target.Id.StartsWith(TEXT("interface:")))
        {
            OutError = TEXT("Stable target no longer exists.");
            return {};
        }
    }

    FGuid Guid;
    if (ParseTargetId(Target.Id, Kind, Guid))
    {
        FResolvedTarget Stable;
        switch (Kind)
        {
        case ETargetKind::Graph: Stable = StableGraph(Blueprint, Guid); break;
        case ETargetKind::Node: Stable = StableNode(Blueprint, Guid); break;
        case ETargetKind::Pin: Stable = StablePin(Blueprint, Guid); break;
        case ETargetKind::Variable: Stable = StableVariable(Blueprint, Guid); break;
        case ETargetKind::Component: Stable = StableComponent(Blueprint, Guid); break;
        case ETargetKind::Interface: Stable = StableInterface(Blueprint, Target.Id); break;
        }
        if (Stable.bStable)
        {
            if (Kind == ETargetKind::Node && !Target.OwnerId.IsEmpty() &&
                Stable.Graph && Target.OwnerId !=
                    DescribeNodeTarget(Blueprint, Stable.Node).OwnerId)
            {
                OutError = TEXT("Node does not belong to the requested graph.");
                return {};
            }
            if (Kind == ETargetKind::Pin && !Target.OwnerId.IsEmpty() &&
                Stable.Node && Target.OwnerId !=
                    DescribePinTarget(Blueprint, Stable.Pin).OwnerId)
            {
                OutError = TEXT("Pin does not belong to the requested node.");
                return {};
            }
            return Stable;
        }
        OutError = TEXT("Stable target no longer exists.");
        return {};
    }

    if (!Target.bAllowNameFallback || Target.OwnerId.IsEmpty() ||
        Target.Name.IsEmpty() ||
        Target.TypePath.IsEmpty())
    {
        OutError = TEXT("A stable target id is required.");
        return {};
    }

    auto IsExactFallback = [&Target](const FTargetRef& Candidate)
    {
        return Candidate.Id == Target.Id &&
            Candidate.OwnerId == Target.OwnerId &&
            Candidate.Name == Target.Name &&
            Candidate.TypePath == Target.TypePath &&
            Candidate.Id.StartsWith(TEXT("fallback:"));
    };

    TArray<FResolvedTarget> Matches;
    if (Kind == ETargetKind::Graph)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            const FTargetRef Candidate = DescribeGraphTarget(Blueprint, Graph);
            if (Graph && IsExactFallback(Candidate))
            {
                FResolvedTarget Match;
                Match.Object = Graph;
                Match.Graph = Graph;
                Match.Id = Candidate.Id;
                Match.IdKind = TEXT("qualified_name_fallback");
                Matches.Add(Match);
            }
        }
    }
    else if (Kind == ETargetKind::Node)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                const FTargetRef Candidate = DescribeNodeTarget(Blueprint, Node);
                if (Node && IsExactFallback(Candidate))
                {
                    FResolvedTarget Match;
                    Match.Object = Node;
                    Match.Graph = Graph;
                    Match.Node = Node;
                    Match.Id = Candidate.Id;
                    Match.IdKind = TEXT("qualified_name_fallback");
                    Matches.Add(Match);
                }
            }
        }
    }
    else if (Kind == ETargetKind::Pin)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node)
                {
                    continue;
                }
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    const FTargetRef Candidate = DescribePinTarget(Blueprint, Pin);
                    if (Pin && IsExactFallback(Candidate))
                    {
                        FResolvedTarget Match;
                        Match.Graph = Graph;
                        Match.Node = Node;
                        Match.Pin = Pin;
                        Match.Id = Candidate.Id;
                        Match.IdKind = TEXT("qualified_name_fallback");
                        Matches.Add(Match);
                    }
                }
            }
        }
    }
    else if (Kind == ETargetKind::Variable)
    {
        for (FBPVariableDescription& Variable : Blueprint->NewVariables)
        {
            const FTargetRef Candidate =
                DescribeVariableTarget(Blueprint, Variable);
            if (IsExactFallback(Candidate))
            {
                FResolvedTarget Match;
                Match.Variable = &Variable;
                Match.Id = Candidate.Id;
                Match.IdKind = TEXT("qualified_name_fallback");
                Matches.Add(Match);
            }
        }
    }
    else if (Kind == ETargetKind::Component && Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* Component : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            const FTargetRef Candidate =
                DescribeComponentTarget(Blueprint, Component);
            if (Component && IsExactFallback(Candidate))
            {
                FResolvedTarget Match;
                Match.Object = Component;
                Match.Component = Component;
                Match.Id = Candidate.Id;
                Match.IdKind = TEXT("qualified_name_fallback");
                Matches.Add(Match);
            }
        }
    }
    if (Matches.Num() != 1)
    {
        OutError = Matches.IsEmpty()
            ? TEXT("Fallback target was not found.")
            : TEXT("Fallback target is ambiguous.");
        return {};
    }
    Matches[0].bStable = Kind == ETargetKind::Interface;
    return Matches[0];
}

FString CanonicalQueryDigest(const TSharedRef<FJsonObject>& Query)
{
    return Sha1(CanonicalJsonValue(MakeShared<FJsonValueObject>(Query)));
}

FString EncodeCursor(const FString& AssetPath, const FPageRequest& Page)
{
    const TSharedRef<FJsonObject> Cursor = MakeShared<FJsonObject>();
    Cursor->SetStringField(TEXT("asset_path"), AssetPath);
    Cursor->SetStringField(
        TEXT("editor_session"),
        UE::MCPython::GetEditorSessionId().ToString(
            EGuidFormats::DigitsWithHyphensLower));
    Cursor->SetStringField(TEXT("last_id"), Page.LastId);
    Cursor->SetStringField(TEXT("query_digest"), Page.QueryDigest);
    FString Serialized;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    FJsonSerializer::Serialize(Cursor, Writer);
    return FBase64::Encode(Serialized, EBase64Mode::UrlSafe);
}

bool DecodeCursor(
    const FString& Cursor,
    const FString& ExpectedAssetPath,
    const FString& ExpectedQueryDigest,
    FPageRequest& OutPage,
    FString& OutError)
{
    FString Decoded;
    if (!FBase64::Decode(Cursor, Decoded, EBase64Mode::UrlSafe))
    {
        OutError = TEXT("Cursor is not valid base64url.");
        return false;
    }
    TSharedPtr<FJsonObject> Object;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Decoded);
    if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
    {
        OutError = TEXT("Cursor does not contain a JSON object.");
        return false;
    }
    FString AssetPath;
    FString EditorSession;
    FString LastId;
    FString QueryDigest;
    if (!Object->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Object->TryGetStringField(TEXT("editor_session"), EditorSession) ||
        !Object->TryGetStringField(TEXT("last_id"), LastId) ||
        !Object->TryGetStringField(TEXT("query_digest"), QueryDigest))
    {
        OutError = TEXT("Cursor is missing required fields.");
        return false;
    }
    const FString CurrentSession = UE::MCPython::GetEditorSessionId().ToString(
        EGuidFormats::DigitsWithHyphensLower);
    if (AssetPath != ExpectedAssetPath || QueryDigest != ExpectedQueryDigest ||
        EditorSession != CurrentSession)
    {
        OutError = TEXT("Cursor does not match the asset, query, or editor session.");
        return false;
    }
    OutPage.LastId = LastId;
    OutPage.QueryDigest = QueryDigest;
    OutError.Empty();
    return true;
}

TSharedRef<FJsonObject> MakeSuccess(
    const FString& Summary,
    const TSharedPtr<FJsonObject>& Data)
{
    const FString TraceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("status"), TEXT("succeeded"));
    Result->SetStringField(TEXT("summary"), Summary);
    Result->SetObjectField(TEXT("data"), Data.IsValid() ? Data : MakeShared<FJsonObject>());
    Result->SetField(TEXT("changes"), EmptyArrayValue());
    Result->SetField(TEXT("warnings"), EmptyArrayValue());
    Result->SetField(TEXT("errors"), EmptyArrayValue());
    Result->SetField(TEXT("next_actions"), EmptyArrayValue());
    Result->SetStringField(TEXT("trace_id"), TraceId);
    return Result;
}

TSharedRef<FJsonObject> MakeFailure(
    const FString& Code,
    const FString& Path,
    const FString& Message,
    bool bRetryable,
    const FString& Hint,
    const TSharedPtr<FJsonObject>& Details)
{
    static const TSet<FString> ApprovedCodes = {
        TEXT("INVALID_INPUT"), TEXT("UNKNOWN_ACTION"), TEXT("CONFLICT"),
        TEXT("PRECONDITION_FAILED"), TEXT("CONFIRMATION_REQUIRED"),
        TEXT("CONFIRMATION_EXPIRED"), TEXT("PLUGIN_REQUIRED"),
        TEXT("UE_VERSION_UNSUPPORTED"), TEXT("UE_UNAVAILABLE"),
        TEXT("TIMEOUT"), TEXT("COMPILE_FAILED"), TEXT("VERIFICATION_FAILED"),
        TEXT("TRANSACTION_FAILED"), TEXT("ROLLBACK_FAILED"),
        TEXT("INTERNAL_ERROR")};
    const FString SafeCode = ApprovedCodes.Contains(Code)
        ? Code
        : FString(TEXT("INTERNAL_ERROR"));
    const FString TraceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
    Error->SetStringField(TEXT("code"), SafeCode);
    if (Path.IsEmpty())
    {
        Error->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
    }
    else
    {
        Error->SetStringField(TEXT("path"), Path);
    }
    Error->SetStringField(TEXT("message"), Message);
    Error->SetBoolField(TEXT("retryable"), bRetryable);
    Error->SetStringField(TEXT("hint"), Hint);
    Error->SetObjectField(
        TEXT("details"), Details.IsValid() ? Details : MakeShared<FJsonObject>());
    Error->SetStringField(TEXT("trace_id"), TraceId);

    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(TEXT("status"), TEXT("failed"));
    Result->SetStringField(TEXT("summary"), Message);
    Result->SetObjectField(TEXT("data"), MakeShared<FJsonObject>());
    Result->SetField(TEXT("changes"), EmptyArrayValue());
    Result->SetField(TEXT("warnings"), EmptyArrayValue());
    Result->SetArrayField(
        TEXT("errors"), {MakeShared<FJsonValueObject>(Error)});
    Result->SetField(TEXT("next_actions"), EmptyArrayValue());
    Result->SetStringField(TEXT("trace_id"), TraceId);
    return Result;
}

FString SerializeResult(const TSharedRef<FJsonObject>& Result)
{
    FString Serialized;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    FJsonSerializer::Serialize(Result, Writer);
    return Serialized;
}

TSharedRef<FJsonObject> BuildCapabilities(UBlueprint* Blueprint)
{
    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("api_version"), 2);
    Result->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    auto StringArray = [](std::initializer_list<const TCHAR*> Values)
    {
        TArray<TSharedPtr<FJsonValue>> ResultValues;
        for (const TCHAR* Value : Values)
        {
            ResultValues.Add(MakeShared<FJsonValueString>(Value));
        }
        return ResultValues;
    };
    Result->SetArrayField(TEXT("scalar_kinds"), StringArray({
        TEXT("bool"), TEXT("byte"), TEXT("int"), TEXT("int64"),
        TEXT("real"), TEXT("string"), TEXT("name"), TEXT("text"),
        TEXT("enum"), TEXT("struct"), TEXT("object"), TEXT("class"),
        TEXT("interface"), TEXT("soft_object"), TEXT("soft_class")}));
    Result->SetArrayField(
        TEXT("container_kinds"),
        StringArray({TEXT("array"), TEXT("set"), TEXT("map")}));
    Result->SetArrayField(TEXT("node_families"), StringArray({
        TEXT("event"), TEXT("call_function"), TEXT("variable"),
        TEXT("branch"), TEXT("cast"), TEXT("construct_object"),
        TEXT("dynamic_cast"), TEXT("macro"), TEXT("interface_message")}));

    bool bHasGraph = false;
    bool bHasK2Graph = false;
    bool bAllGraphsUseK2Schema = true;
    if (Blueprint)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (const UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            bHasGraph = true;
            const bool bUsesK2Schema = Graph->GetSchema() &&
                Graph->GetSchema()->IsA<UEdGraphSchema_K2>();
            bHasK2Graph |= bUsesK2Schema;
            bAllGraphsUseK2Schema &= bUsesK2Schema;
        }
    }
    bAllGraphsUseK2Schema &= bHasGraph;
    const bool bHasSCS = Blueprint && Blueprint->SimpleConstructionScript;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 7
    constexpr bool bSupportsCompilerTokens = true;
#else
    constexpr bool bSupportsCompilerTokens = false;
#endif
    Result->SetBoolField(TEXT("supports_k2_schema"), true);
    Result->SetBoolField(TEXT("supports_scs_operations"), true);
    Result->SetBoolField(
        TEXT("supports_compiler_tokens"),
        bSupportsCompilerTokens);
    Result->SetBoolField(TEXT("has_k2_graphs"), bHasK2Graph);
    Result->SetBoolField(
        TEXT("all_graphs_k2_schema"),
        bAllGraphsUseK2Schema);
    Result->SetBoolField(TEXT("k2_schema"), bAllGraphsUseK2Schema);
    Result->SetBoolField(TEXT("compiler_tokens"), bSupportsCompilerTokens);
    Result->SetBoolField(TEXT("has_scs"), bHasSCS);
    Result->SetBoolField(TEXT("scs_operations"), bHasSCS);
    return Result;
}

bool IsSupportedBlueprintSelectionEditor(const FName& EditorName)
{
    return EditorName == TEXT("BlueprintEditor") ||
        EditorName == TEXT("WidgetBlueprintEditor") ||
        EditorName == TEXT("AnimationBlueprintEditor");
}

FMutationScope::FMutationScope(const FText& Description)
    : bWorkflowOwned(UE::MCPython::HasActiveWorkflowTransaction())
{
    if (bWorkflowOwned)
    {
        return;
    }
    if (!GEditor || !GEditor->Trans || GEditor->Trans->IsActive())
    {
        return;
    }
    LocalTransaction = MakeUnique<FScopedTransaction>(Description);
    if (!LocalTransaction->IsOutstanding())
    {
        LocalTransaction.Reset();
        return;
    }
    TransactionIndex = GEditor->Trans->GetQueueLength() - 1;
    TransactionGuid = TransactionGuidAtIndex(TransactionIndex);
    if (!TransactionGuid.IsValid())
    {
        LocalTransaction->Cancel();
        LocalTransaction.Reset();
        TransactionIndex = INDEX_NONE;
    }
}

FMutationScope::~FMutationScope() = default;

bool FMutationScope::IsValid() const
{
    return bWorkflowOwned ||
        (LocalTransaction.IsValid() && LocalTransaction->IsOutstanding() &&
         TransactionGuid.IsValid());
}

void FMutationScope::Modify(UObject* Object)
{
    if (IsValid() && Object)
    {
        Object->Modify();
    }
}

FRollbackResult FMutationScope::Rollback()
{
    FRollbackResult Result;
    if (bWorkflowOwned)
    {
        Result.bSucceeded = true;
        Result.bDeferredToWorkflow = true;
        return Result;
    }
    if (!LocalTransaction.IsValid())
    {
        return Result;
    }

    LocalTransaction.Reset();
    const bool bUndoSucceeded =
        IsCurrentTransaction(TransactionIndex, TransactionGuid) &&
        GEditor->UndoTransaction();
    Result.bSucceeded = bUndoSucceeded;
    if (!bUndoSucceeded)
    {
        const TSharedRef<FJsonObject> Residual = MakeShared<FJsonObject>();
        Residual->SetStringField(TEXT("code"), TEXT("ROLLBACK_FAILED"));
        Residual->SetNumberField(TEXT("transaction_index"), TransactionIndex);
        Residual->SetStringField(
            TEXT("transaction_guid"),
            TransactionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        Result.ResidualChanges.Add(MakeShared<FJsonValueObject>(Residual));
    }
    TransactionIndex = INDEX_NONE;
    TransactionGuid.Invalidate();
    return Result;
}
}
