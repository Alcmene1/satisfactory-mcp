#include "McpRpc.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "McpTools.h"

namespace
{
	FString JsonToString(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	TSharedRef<FJsonObject> RpcMakeError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
		Err->SetNumberField(TEXT("code"), Code);
		Err->SetStringField(TEXT("message"), Message);
		Resp->SetObjectField(TEXT("error"), Err);
		Resp->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
		return Resp;
	}

	TSharedRef<FJsonObject> RpcMakeResult(const TSharedPtr<FJsonValue>& Id, TSharedPtr<FJsonValue> Result)
	{
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Resp->SetField(TEXT("result"), Result.IsValid() ? Result : MakeShared<FJsonValueNull>());
		Resp->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
		return Resp;
	}
}

FString FMcpRpc::HandleRequest(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Req;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody);
	if (!FJsonSerializer::Deserialize(Reader, Req) || !Req.IsValid())
	{
		return JsonToString(RpcMakeError(nullptr, -32700, TEXT("parse error")));
	}

	const FString Method = Req->GetStringField(TEXT("method"));
	TSharedPtr<FJsonValue> Id = Req->TryGetField(TEXT("id"));

	if (Method == TEXT("initialize"))
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("protocolVersion"), TEXT("2025-03-26"));
		TSharedRef<FJsonObject> Caps = MakeShared<FJsonObject>();
		Caps->SetObjectField(TEXT("tools"), MakeShared<FJsonObject>());
		Result->SetObjectField(TEXT("capabilities"), Caps);
		TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("name"), TEXT("satisfactory-mcp"));
		Info->SetStringField(TEXT("version"), TEXT("0.1.0"));
		Result->SetObjectField(TEXT("serverInfo"), Info);
		return JsonToString(RpcMakeResult(Id, MakeShared<FJsonValueObject>(Result)));
	}

	if (Method == TEXT("notifications/initialized") || Method == TEXT("initialized"))
	{
		return FString(); // notification: empty body
	}

	if (Method == TEXT("ping"))
	{
		return JsonToString(RpcMakeResult(Id, MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())));
	}

	if (Method == TEXT("tools/list"))
	{
		TSharedPtr<FJsonValue> Tools;
		const TSharedRef<TJsonReader<>> TR = TJsonReaderFactory<>::Create(FMcpTools::GetToolsListJson());
		FJsonSerializer::Deserialize(TR, Tools);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetField(TEXT("tools"), Tools.IsValid() ? Tools : MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>()));
		return JsonToString(RpcMakeResult(Id, MakeShared<FJsonValueObject>(Result)));
	}

	if (Method == TEXT("tools/call"))
	{
		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		if (!Req->TryGetObjectField(TEXT("params"), ParamsObj) || !ParamsObj)
		{
			return JsonToString(RpcMakeError(Id, -32602, TEXT("invalid params")));
		}
		const FString ToolName = (*ParamsObj)->GetStringField(TEXT("name"));
		FString ArgsJson;
		const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
		if ((*ParamsObj)->TryGetObjectField(TEXT("arguments"), ArgsObj) && ArgsObj)
		{
			ArgsJson = JsonToString(ArgsObj->ToSharedRef());
		}

		const FMcpToolResult ToolResult = FMcpTools::Call(ToolName, ArgsJson);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Content;
		TSharedRef<FJsonObject> TextItem = MakeShared<FJsonObject>();
		TextItem->SetStringField(TEXT("type"), TEXT("text"));
		TextItem->SetStringField(TEXT("text"), ToolResult.Text);
		Content.Add(MakeShared<FJsonValueObject>(TextItem));
		Result->SetArrayField(TEXT("content"), Content);
		Result->SetBoolField(TEXT("isError"), ToolResult.bIsError);
		return JsonToString(RpcMakeResult(Id, MakeShared<FJsonValueObject>(Result)));
	}

	return JsonToString(RpcMakeError(Id, -32601, FString::Printf(TEXT("method not found: %s"), *Method)));
}
