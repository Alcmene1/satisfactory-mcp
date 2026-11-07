#pragma once

#include "CoreMinimal.h"

struct FMcpToolResult
{
	bool bIsError = false;
	FString Text;
};

/** MCP tool registry. All game-state access is marshalled to the game thread. */
class FMcpTools
{
public:
	/** JSON string for tools/list result. */
	static FString GetToolsListJson();

	/** Dispatch a tools/call. ToolName + ParamsJson (may be empty). */
	static FMcpToolResult Call(const FString& ToolName, const FString& ParamsJson);
};
