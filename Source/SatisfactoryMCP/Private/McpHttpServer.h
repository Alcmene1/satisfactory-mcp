#pragma once

#include "CoreMinimal.h"

/** Minimal blocking HTTP/1.1 server on a raw socket, bound to localhost.
 *  Serves POST /mcp (JSON-RPC 2.0) and GET /health. */
class FMcpHttpServer
{
public:
	static void Start(int32 Port = 8898);
	static void Stop();

private:
	static void ListenerLoop(int32 Port);
};
