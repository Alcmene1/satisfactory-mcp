#include "Modules/ModuleManager.h"

#include "McpHttpServer.h"

class FSatisfactoryMCPModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FMcpHttpServer::Start(8898);
	}

	virtual void ShutdownModule() override
	{
		FMcpHttpServer::Stop();
	}
};

IMPLEMENT_GAME_MODULE(FSatisfactoryMCPModule, SatisfactoryMCP);
