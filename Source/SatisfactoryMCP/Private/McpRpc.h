#pragma once

#include "CoreMinimal.h"

/** Handles one JSON-RPC 2.0 (MCP) request body and returns the response string. */
class FMcpRpc
{
public:
	static FString HandleRequest(const FString& RequestBody);
};
