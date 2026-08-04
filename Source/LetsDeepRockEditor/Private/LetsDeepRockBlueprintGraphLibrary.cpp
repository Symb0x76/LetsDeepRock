#include "LetsDeepRockBlueprintGraphLibrary.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/World.h"
#include "K2Node_CallFunction.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputKey.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace LetsDeepRockBlueprintGraph
{
	using FJsonObjectPtr = TSharedPtr<FJsonObject>;

	static FString ToJson(const FJsonObjectPtr& Root)
	{
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return Json;
	}

	static FString ErrorJson(const FString& AssetPath, const FString& Error)
	{
		const FJsonObjectPtr Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("asset_path"), AssetPath);
		Root->SetStringField(TEXT("error"), Error);
		return ToJson(Root);
	}

	static FString GuidString(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::DigitsWithHyphens);
	}

	static FString PinDirection(const UEdGraphPin* Pin)
	{
		return Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output");
	}

	static FString PinType(const UEdGraphPin* Pin)
	{
		FString Result = Pin->PinType.PinCategory.ToString();
		if (!Pin->PinType.PinSubCategory.IsNone())
		{
			Result += TEXT(":") + Pin->PinType.PinSubCategory.ToString();
		}
		if (const UObject* TypeObject = Pin->PinType.PinSubCategoryObject.Get())
		{
			Result += TEXT(":") + TypeObject->GetPathName();
		}
		return Result;
	}

	static FString NodeTitle(const UEdGraphNode* Node)
	{
		return Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	}

	static void AddUniqueString(TArray<FString>& Values, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Values.AddUnique(Value);
		}
	}

	static FJsonObjectPtr CalledFunctionJson(const UEdGraphNode* Node)
	{
		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		if (!Call)
		{
			return nullptr;
		}

		const UFunction* Function = Call->GetTargetFunction();
		const FName FunctionName = Function ? Function->GetFName() : Call->GetFunctionName();
		if (FunctionName.IsNone())
		{
			return nullptr;
		}

		const FJsonObjectPtr Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), FunctionName.ToString());
		Json->SetStringField(TEXT("owner"), Function && Function->GetOwnerClass() ? Function->GetOwnerClass()->GetName() : TEXT(""));
		Json->SetStringField(TEXT("path"), Function ? Function->GetPathName() : FunctionName.ToString());
		return Json;
	}

	static void GetInputSummary(const UEdGraphNode* Node, FString& InputAction, FString& InputKey, FString& InputKeyDisplay)
	{
		if (const UK2Node_InputAction* ActionNode = Cast<UK2Node_InputAction>(Node))
		{
			InputAction = ActionNode->InputActionName.ToString();
		}
		if (const UK2Node_InputKey* KeyNode = Cast<UK2Node_InputKey>(Node))
		{
			InputKey = KeyNode->InputKey.GetFName().ToString();
			InputKeyDisplay = KeyNode->InputKey.GetDisplayName().ToString();
		}
	}

	static void GetReferences(const UEdGraphNode* Node, TArray<FString>& AssetReferences, TArray<FString>& ClassReferences)
	{
		if (const FJsonObjectPtr CalledFunction = CalledFunctionJson(Node))
		{
			AddUniqueString(ClassReferences, CalledFunction->GetStringField(TEXT("owner")));
		}

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			if (const UObject* DefaultObject = Pin->DefaultObject)
			{
				AddUniqueString(AssetReferences, DefaultObject->GetPathName());
				if (const UClass* DefaultClass = Cast<UClass>(DefaultObject))
				{
					AddUniqueString(ClassReferences, DefaultClass->GetPathName());
				}
			}
			if (const UObject* TypeObject = Pin->PinType.PinSubCategoryObject.Get())
			{
				if (const UClass* TypeClass = Cast<UClass>(TypeObject))
				{
					AddUniqueString(ClassReferences, TypeClass->GetPathName());
				}
				else
				{
					AddUniqueString(AssetReferences, TypeObject->GetPathName());
				}
			}
		}
	}

	static FJsonObjectPtr NodeJson(const UEdGraphNode* Node, const bool bIncludePins)
	{
		const FJsonObjectPtr Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Json->SetStringField(TEXT("title"), NodeTitle(Node));
		Json->SetStringField(TEXT("guid"), GuidString(Node->NodeGuid));

		FString InputAction;
		FString InputKey;
		FString InputKeyDisplay;
		GetInputSummary(Node, InputAction, InputKey, InputKeyDisplay);
		if (!InputKey.IsEmpty())
		{
			Json->SetStringField(TEXT("input_key"), InputKey);
		}
		if (!InputKeyDisplay.IsEmpty())
		{
			Json->SetStringField(TEXT("input_key_display"), InputKeyDisplay);
		}
		if (!InputAction.IsEmpty())
		{
			Json->SetStringField(TEXT("input_action"), InputAction);
		}

		if (const FJsonObjectPtr CalledFunction = CalledFunctionJson(Node))
		{
			Json->SetObjectField(TEXT("called_function"), CalledFunction);
		}

		TArray<FString> AssetReferences;
		TArray<FString> ClassReferences;
		GetReferences(Node, AssetReferences, ClassReferences);

		TArray<TSharedPtr<FJsonValue>> Pins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			if (!bIncludePins)
			{
				continue;
			}

			const FJsonObjectPtr PinJson = MakeShared<FJsonObject>();
			PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinJson->SetStringField(TEXT("direction"), PinDirection(Pin));
			PinJson->SetStringField(TEXT("default_value"), Pin->DefaultValue);
			PinJson->SetStringField(TEXT("default_object"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : TEXT(""));
			PinJson->SetStringField(TEXT("type"), PinType(Pin));
			PinJson->SetStringField(TEXT("guid"), GuidString(Pin->PinId));
			if (Pin->PersistentGuid.IsValid())
			{
				PinJson->SetStringField(TEXT("persistent_guid"), GuidString(Pin->PersistentGuid));
			}
			Pins.Add(MakeShared<FJsonValueObject>(PinJson));
		}

		const FJsonObjectPtr References = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Assets;
		for (const FString& Value : AssetReferences)
		{
			Assets.Add(MakeShared<FJsonValueString>(Value));
		}
		TArray<TSharedPtr<FJsonValue>> Classes;
		for (const FString& Value : ClassReferences)
		{
			Classes.Add(MakeShared<FJsonValueString>(Value));
		}
		References->SetArrayField(TEXT("asset"), Assets);
		References->SetArrayField(TEXT("class"), Classes);
		Json->SetObjectField(TEXT("references"), References);
		if (bIncludePins)
		{
			Json->SetArrayField(TEXT("pins"), Pins);
		}
		return Json;
	}

	static FJsonObjectPtr ConnectionJson(const UEdGraphPin* Source, const UEdGraphPin* Target)
	{
		const UEdGraphNode* SourceNode = Source->GetOwningNode();
		const UEdGraphNode* TargetNode = Target->GetOwningNode();
		const FJsonObjectPtr Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("kind"), Source->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ? TEXT("execution") : TEXT("data"));
		Json->SetStringField(TEXT("source_node_guid"), SourceNode ? GuidString(SourceNode->NodeGuid) : TEXT(""));
		Json->SetStringField(TEXT("source_pin"), Source->PinName.ToString());
		Json->SetStringField(TEXT("source_pin_guid"), GuidString(Source->PinId));
		Json->SetStringField(TEXT("target_node_guid"), TargetNode ? GuidString(TargetNode->NodeGuid) : TEXT(""));
		Json->SetStringField(TEXT("target_pin"), Target->PinName.ToString());
		Json->SetStringField(TEXT("target_pin_guid"), GuidString(Target->PinId));
		return Json;
	}

	static FJsonObjectPtr GraphJson(const UEdGraph* Graph, const bool bIncludePins)
	{
		const FJsonObjectPtr Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Graph->GetName());
		TArray<TSharedPtr<FJsonValue>> Nodes;
		TArray<TSharedPtr<FJsonValue>> Connections;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			Nodes.Add(MakeShared<FJsonValueObject>(NodeJson(Node, bIncludePins)));
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (LinkedPin)
					{
						Connections.Add(MakeShared<FJsonValueObject>(ConnectionJson(Pin, LinkedPin)));
					}
				}
			}
		}
		Json->SetArrayField(TEXT("nodes"), Nodes);
		Json->SetArrayField(TEXT("connections"), Connections);
		return Json;
	}

	static UBlueprint* ResolveBlueprint(UObject* Asset, FString& AssetType, FString& Error)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
		{
			AssetType = Asset->GetClass()->GetName();
			return Blueprint;
		}
		if (UWorld* World = Cast<UWorld>(Asset))
		{
			AssetType = TEXT("World");
			ULevel* Level = World->PersistentLevel;
			ULevelScriptBlueprint* LevelBlueprint = Level ? Level->GetLevelScriptBlueprint(true) : nullptr;
			UBlueprint* Blueprint = LevelBlueprint;
			if (!Blueprint)
			{
				Error = TEXT("World 没有已存在的 PersistentLevel Level Script Blueprint；未创建新蓝图");
			}
			return Blueprint;
		}
		Error = FString::Printf(TEXT("不支持的资产类型：%s；仅支持 Blueprint（含 WidgetBlueprint）和 World"), Asset ? *Asset->GetClass()->GetName() : TEXT("null"));
		return nullptr;
	}

	static UObject* LoadAsset(const FString& AssetPath)
	{
		if (UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath))
		{
			return Asset;
		}

		FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> Assets;
		Registry.Get().GetAssetsByPackageName(FName(*AssetPath), Assets);
		return Assets.Num() == 1 ? Assets[0].GetAsset() : nullptr;
	}

	static void GetGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& Graphs)
	{
		auto AddGraphs = [&Graphs](const auto& SourceGraphs)
		{
			for (UEdGraph* Graph : SourceGraphs)
			{
				if (Graph)
				{
					Graphs.AddUnique(Graph);
				}
			}
		};

		AddGraphs(Blueprint->UbergraphPages);
		AddGraphs(Blueprint->FunctionGraphs);
		AddGraphs(Blueprint->MacroGraphs);
		AddGraphs(Blueprint->DelegateSignatureGraphs);
	}

	static bool EqualsClassFilter(const UEdGraphNode* Node, const TArray<FString>& Filters)
	{
		if (Filters.IsEmpty())
		{
			return true;
		}
		const FString ClassName = Node->GetClass()->GetName();
		for (const FString& Filter : Filters)
		{
			if (ClassName.Equals(Filter.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static void AddMatch(const FString& Field, const FString& Value, const TArray<FString>& Keywords, TArray<FString>& MatchedFields)
	{
		for (const FString& Keyword : Keywords)
		{
			if (!Keyword.IsEmpty() && Value.Contains(Keyword, ESearchCase::IgnoreCase))
			{
				MatchedFields.AddUnique(Field);
			}
		}
	}

	static TArray<FAssetData> ResolveSearchAssets(const TArray<FString>& Paths)
	{
		FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> Result;
		for (FString Path : Paths)
		{
			Path.TrimStartAndEndInline();
			if (Path.IsEmpty())
			{
				continue;
			}

			if (UObject* LoadedAsset = LoadAsset(Path))
			{
				Result.AddUnique(FAssetData(LoadedAsset));
				continue;
			}

			TArray<FAssetData> Found;
			if (Registry.Get().HasAssets(FName(*Path), true))
			{
				Registry.Get().GetAssetsByPath(FName(*Path), Found, true);
			}
			for (const FAssetData& Asset : Found)
			{
				if (Asset.IsValid())
				{
					Result.AddUnique(Asset);
				}
			}
		}
		return Result;
	}
}

FString ULetsDeepRockBlueprintGraphLibrary::InspectBlueprintGraphJson(const FString& AssetPath, const FString& GraphName, const bool bIncludePins)
{
	using namespace LetsDeepRockBlueprintGraph;
	const FString CleanPath = AssetPath.TrimStartAndEnd();
	if (CleanPath.IsEmpty())
	{
		return ErrorJson(AssetPath, TEXT("asset_path 不能为空"));
	}

	UObject* Asset = LoadAsset(CleanPath);
	if (!Asset)
	{
		return ErrorJson(CleanPath, TEXT("找不到或无法加载资产；请传入 /Game/... 包路径或完整对象路径"));
	}

	FString AssetType;
	FString Error;
	UBlueprint* Blueprint = ResolveBlueprint(Asset, AssetType, Error);
	if (!Blueprint)
	{
		return ErrorJson(CleanPath, Error);
	}

	TArray<UEdGraph*> Graphs;
	GetGraphs(Blueprint, Graphs);
	TArray<TSharedPtr<FJsonValue>> GraphValues;
	for (const UEdGraph* Graph : Graphs)
	{
		if (Graph && (GraphName.IsEmpty() || Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase)))
		{
			GraphValues.Add(MakeShared<FJsonValueObject>(GraphJson(Graph, bIncludePins)));
		}
	}
	if (!GraphName.IsEmpty() && GraphValues.IsEmpty())
	{
		return ErrorJson(CleanPath, FString::Printf(TEXT("找不到图：%s"), *GraphName));
	}

	const FJsonObjectPtr Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);
	Root->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Root->SetStringField(TEXT("asset_type"), AssetType);
	Root->SetArrayField(TEXT("graphs"), GraphValues);
	return ToJson(Root);
}

FString ULetsDeepRockBlueprintGraphLibrary::SearchBlueprintNodesJson(const TArray<FString>& Paths, const TArray<FString>& Keywords, const TArray<FString>& NodeClasses)
{
	using namespace LetsDeepRockBlueprintGraph;
	if (Paths.IsEmpty())
	{
		return ErrorJson(TEXT(""), TEXT("paths 不能为空"));
	}
	if (Keywords.IsEmpty() && NodeClasses.IsEmpty())
	{
		return ErrorJson(TEXT(""), TEXT("keywords 与 node_classes 至少提供一项"));
	}

	const TArray<FAssetData> SearchAssets = ResolveSearchAssets(Paths);
	if (SearchAssets.IsEmpty())
	{
		return ErrorJson(TEXT(""), TEXT("paths 未解析到任何资产；目录需使用 /Game/...，精确路径可使用包路径或完整对象路径"));
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	TArray<TSharedPtr<FJsonValue>> AssetErrors;
	for (const FAssetData& AssetData : SearchAssets)
	{
		UObject* Asset = AssetData.GetAsset();
		FString AssetType;
		FString ResolveError;
		UBlueprint* Blueprint = ResolveBlueprint(Asset, AssetType, ResolveError);
		if (!Blueprint)
		{
			const FJsonObjectPtr AssetError = MakeShared<FJsonObject>();
			AssetError->SetStringField(TEXT("asset_path"), Asset ? Asset->GetPathName() : AssetData.GetObjectPathString());
			AssetError->SetStringField(TEXT("error"), ResolveError);
			AssetErrors.Add(MakeShared<FJsonValueObject>(AssetError));
			continue;
		}

		TArray<UEdGraph*> Graphs;
		GetGraphs(Blueprint, Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || !EqualsClassFilter(Node, NodeClasses))
				{
					continue;
				}

				TArray<FString> MatchedFields;
				AddMatch(TEXT("title"), NodeTitle(Node), Keywords, MatchedFields);
				AddMatch(TEXT("class"), Node->GetClass()->GetName(), Keywords, MatchedFields);
				FString InputAction;
				FString InputKey;
				FString InputKeyDisplay;
				GetInputSummary(Node, InputAction, InputKey, InputKeyDisplay);
				AddMatch(TEXT("input_action"), InputAction, Keywords, MatchedFields);
				AddMatch(TEXT("input_key"), InputKey, Keywords, MatchedFields);
				AddMatch(TEXT("input_key_display"), InputKeyDisplay, Keywords, MatchedFields);
				const FJsonObjectPtr CalledFunction = CalledFunctionJson(Node);
				if (CalledFunction)
				{
					AddMatch(TEXT("called_function.name"), CalledFunction->GetStringField(TEXT("name")), Keywords, MatchedFields);
					AddMatch(TEXT("called_function.owner"), CalledFunction->GetStringField(TEXT("owner")), Keywords, MatchedFields);
					AddMatch(TEXT("called_function.path"), CalledFunction->GetStringField(TEXT("path")), Keywords, MatchedFields);
				}
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin)
					{
						continue;
					}
					AddMatch(TEXT("pin.name"), Pin->PinName.ToString(), Keywords, MatchedFields);
					AddMatch(TEXT("pin.default_value"), Pin->DefaultValue, Keywords, MatchedFields);
					AddMatch(TEXT("pin.default_object"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : TEXT(""), Keywords, MatchedFields);
				}
				TArray<FString> AssetReferences;
				TArray<FString> ClassReferences;
				GetReferences(Node, AssetReferences, ClassReferences);
				for (const FString& Reference : AssetReferences)
				{
					AddMatch(TEXT("references.asset"), Reference, Keywords, MatchedFields);
				}
				for (const FString& Reference : ClassReferences)
				{
					AddMatch(TEXT("references.class"), Reference, Keywords, MatchedFields);
				}

				if (!Keywords.IsEmpty() && MatchedFields.IsEmpty())
				{
					continue;
				}
				const FJsonObjectPtr Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
				Result->SetStringField(TEXT("graph"), Graph->GetName());
				Result->SetObjectField(TEXT("node"), NodeJson(Node, false));
				TArray<TSharedPtr<FJsonValue>> Fields;
				for (const FString& Field : MatchedFields)
				{
					Fields.Add(MakeShared<FJsonValueString>(Field));
				}
				Result->SetArrayField(TEXT("matched_fields"), Fields);
				Results.Add(MakeShared<FJsonValueObject>(Result));
			}
		}
	}

	const FJsonObjectPtr Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);
	Root->SetArrayField(TEXT("results"), Results);
	Root->SetArrayField(TEXT("asset_errors"), AssetErrors);
	return ToJson(Root);
}
