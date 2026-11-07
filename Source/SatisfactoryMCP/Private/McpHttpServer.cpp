#include "McpHttpServer.h"

#include "Networking.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformProcess.h"

#include <atomic>
#include <thread>

#include "McpRpc.h"

namespace
{
	std::atomic<bool> GRunning{false};
	FSocket* GListenerSocket = nullptr;
	std::thread GListenerThread;

	// Bearer token protecting POST /mcp. Loaded from Mods/SatisfactoryMCP/mcp.ini
	// ("token=...", optional "port=..."), otherwise generated at startup and
	// written to Mods/SatisfactoryMCP/mcp_token.txt (reused across restarts).
	FString GToken;
	std::atomic<int32> GActiveConns{0};

	constexpr int32 MaxHeaderBytes = 16 * 1024;
	constexpr int32 MaxBodyBytes   = 512 * 1024;
	constexpr int32 MaxActiveConns = 8;

	FString ModConfigDir()
	{
		// Packaged game: ProjectContentDir = <root>/FactoryGame/Content -> Mods sits beside it.
		FString Dir = FPaths::ProjectContentDir() / TEXT("../Mods/SatisfactoryMCP");
		FPaths::NormalizeFilename(Dir);
		FPaths::CollapseRelativeDirectories(Dir);
		return Dir;
	}

	void LoadOrGenerateToken(int32 DefaultPort, int32& OutPort)
	{
		OutPort = DefaultPort;
		GToken.Empty();

		const FString IniPath = ModConfigDir() / TEXT("mcp.ini");
		FString IniText;
		if (FFileHelper::LoadFileToString(IniText, *IniPath))
		{
			TArray<FString> Lines;
			IniText.ParseIntoArrayLines(Lines, false);
			for (const FString& Line : Lines)
			{
				const FString T = Line.TrimStart().TrimEnd();
				if (T.IsEmpty() || T.StartsWith(TEXT("#")) || T.StartsWith(TEXT(";")) || T.StartsWith(TEXT("[")))
				{
					continue;
				}
				int32 Eq = INDEX_NONE;
				if (!T.FindChar(TEXT('='), Eq)) continue;
				const FString Key = T.Left(Eq).TrimStart().TrimEnd().ToLower();
				const FString Val = T.Mid(Eq + 1).TrimStart().TrimEnd();
				if (Key == TEXT("token") && !Val.IsEmpty()) GToken = Val;
				else if (Key == TEXT("port") && !Val.IsEmpty()) OutPort = FMath::Clamp(FCString::Atoi(*Val), 1024, 65535);
			}
		}

		const FString TokenPath = ModConfigDir() / TEXT("mcp_token.txt");
		if (GToken.IsEmpty())
		{
			// Reuse a previously generated token so restarts keep working clients.
			FString Existing;
			if (FFileHelper::LoadFileToString(Existing, *TokenPath))
			{
				GToken = Existing.TrimStart().TrimEnd();
			}
		}
		if (GToken.IsEmpty())
		{
			GToken = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
			if (!FFileHelper::SaveStringToFile(GToken + LINE_TERMINATOR, *TokenPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				UE_LOG(LogTemp, Warning, TEXT("[MCP] Could not write %s; token only in memory: %s"), *TokenPath, *GToken);
			}
		}
		UE_LOG(LogTemp, Display, TEXT("[MCP] Auth enabled (Authorization: Bearer ...). Token file: %s"), *TokenPath);
	}

	FString HeaderValue(const FString& Line)
	{
		int32 Colon = INDEX_NONE;
		if (!Line.FindChar(TEXT(':'), Colon)) return FString();
		return Line.Mid(Colon + 1).TrimStart().TrimEnd();
	}

	bool IsLocalOriginOrHost(const FString& V)
	{
		// Accept http(s)://127.0.0.1:*, http(s)://localhost:* and bare host forms.
		if (V.IsEmpty()) return false;
		return V.Contains(TEXT("127.0.0.1"), ESearchCase::IgnoreCase)
			|| V.Contains(TEXT("localhost"), ESearchCase::IgnoreCase)
			|| V.Equals(TEXT("[::1]"), ESearchCase::IgnoreCase)
			|| V.StartsWith(TEXT("::1"));
	}

	bool RecvUntil(FSocket* S, TArray<uint8>& Buf, const uint8* Seq, SIZE_T SeqLen, int32& OutSeqStart, int32 MaxBytes)
	{
		uint8 Tmp[4096];
		while (true)
		{
			int32 Read = 0;
			if (!S->Recv(Tmp, UE_ARRAY_COUNT(Tmp), Read) || Read <= 0)
			{
				return false;
			}
			Buf.Append(Tmp, Read);
			if (Buf.Num() > MaxBytes)
			{
				return false; // header flood / oversized request
			}
			for (int32 i = 0; i + (int32)SeqLen <= Buf.Num(); ++i)
			{
				if (FMemory::Memcmp(Buf.GetData() + i, Seq, SeqLen) == 0)
				{
					OutSeqStart = i;
					return true;
				}
			}
		}
	}

	void HandleConnection(FSocket* Client)
	{
		if (++GActiveConns > MaxActiveConns)
		{
			static const ANSICHAR Busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			int32 Sent = 0;
			Client->Send((const uint8*)Busy, UE_ARRAY_COUNT(Busy) - 1, Sent);
			Client->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client);
			--GActiveConns;
			return;
		}
		ON_SCOPE_EXIT { --GActiveConns; Client->Close(); if (ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)) SS->DestroySocket(Client); };

		TArray<uint8> Buf;
		static const uint8 HeaderEnd[] = {'\r','\n','\r','\n'};
		int32 HeaderEndIdx = -1;
		if (!RecvUntil(Client, Buf, HeaderEnd, 4, HeaderEndIdx, MaxHeaderBytes))
		{
			return;
		}

		const int32 HeaderLen = HeaderEndIdx;
		FString Headers;
		Headers.AppendChars((const char*)Buf.GetData(), HeaderLen); // ascii-ish, fine for headers

		// Parse request line + selected headers
		TArray<FString> Lines;
		Headers.ParseIntoArrayLines(Lines, false);
		int32 ContentLength = 0;
		FString Method, Path, Auth, Origin, Host, ContentType;
		if (Lines.Num() > 0)
		{
			TArray<FString> Parts;
			Lines[0].ParseIntoArray(Parts, TEXT(" "), true);
			if (Parts.Num() >= 2) { Method = Parts[0]; Path = Parts[1]; }
		}
		for (int32 i = 1; i < Lines.Num(); ++i)
		{
			const FString& L = Lines[i];
			if (L.StartsWith(TEXT("Content-Length:"), ESearchCase::IgnoreCase))
			{
				ContentLength = FCString::Atoi(*HeaderValue(L));
			}
			else if (L.StartsWith(TEXT("Authorization:"), ESearchCase::IgnoreCase))
			{
				Auth = HeaderValue(L);
			}
			else if (L.StartsWith(TEXT("Origin:"), ESearchCase::IgnoreCase))
			{
				Origin = HeaderValue(L);
			}
			else if (L.StartsWith(TEXT("Host:"), ESearchCase::IgnoreCase))
			{
				Host = HeaderValue(L);
			}
			else if (L.StartsWith(TEXT("Content-Type:"), ESearchCase::IgnoreCase))
			{
				ContentType = HeaderValue(L);
			}
		}

		const bool bMcpPost = (Method == TEXT("POST") && (Path == TEXT("/mcp") || Path.StartsWith(TEXT("/mcp?"))));

		// --- security gates for tool calls -------------------------------------
		// Cross-origin browsers cannot set Authorization / Content-Type: application/json
		// without a CORS preflight, which we never answer -> drive-by blocked.
		// Origin/Host checks additionally stop DNS rebinding (MCP spec requirement).
		FString DenyStatus, DenyBody;
		if (bMcpPost)
		{
			if (!Origin.IsEmpty() && !IsLocalOriginOrHost(Origin))
			{
				DenyStatus = TEXT("403 Forbidden");
				DenyBody = TEXT("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,\"message\":\"bad Origin\"},\"id\":null}");
			}
			else if (!Host.IsEmpty() && !IsLocalOriginOrHost(Host))
			{
				DenyStatus = TEXT("403 Forbidden");
				DenyBody = TEXT("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,\"message\":\"bad Host\"},\"id\":null}");
			}
			else if (!ContentType.Contains(TEXT("application/json"), ESearchCase::IgnoreCase))
			{
				DenyStatus = TEXT("415 Unsupported Media Type");
				DenyBody = TEXT("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,\"message\":\"Content-Type must be application/json\"},\"id\":null}");
			}
			else if (ContentLength > MaxBodyBytes)
			{
				DenyStatus = TEXT("413 Payload Too Large");
				DenyBody = TEXT("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,\"message\":\"body too large\"},\"id\":null}");
			}
			else
			{
				FString Bearer;
				if (Auth.StartsWith(TEXT("Bearer "), ESearchCase::IgnoreCase))
				{
					Bearer = Auth.Mid(7).TrimStart().TrimEnd();
				}
				if (GToken.IsEmpty() || Bearer != GToken)
				{
					DenyStatus = TEXT("401 Unauthorized");
					DenyBody = TEXT("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,\"message\":\"missing or invalid bearer token\"},\"id\":null}");
				}
			}
		}

		FString Status = TEXT("200 OK");
		FString Body;
		if (!DenyStatus.IsEmpty())
		{
			Status = DenyStatus;
			Body = DenyBody;
		}
		else if (Path == TEXT("/health"))
		{
			Body = TEXT("ok");
		}
		else if (bMcpPost)
		{
			FString RequestBody;
			const int32 BodyOffset = HeaderLen + 4; // raw bytes up to and including \r\n\r\n
			// Read remaining body bytes (bounded).
			while (Buf.Num() - BodyOffset < ContentLength)
			{
				uint8 Tmp[4096];
				int32 Read = 0;
				if (!Client->Recv(Tmp, UE_ARRAY_COUNT(Tmp), Read) || Read <= 0) break;
				Buf.Append(Tmp, Read);
				if (Buf.Num() > MaxHeaderBytes + MaxBodyBytes) break;
			}
			const int32 Available = FMath::Min(ContentLength, Buf.Num() - BodyOffset);
			if (Available > 0)
			{
				TArray<ANSICHAR> NullTerm;
				NullTerm.Append((const ANSICHAR*)(Buf.GetData() + BodyOffset), Available);
				NullTerm.Add('\0');
				RequestBody = FString(UTF8_TO_TCHAR(NullTerm.GetData()));
			}
			Body = FMcpRpc::HandleRequest(RequestBody);
			Status = TEXT("200 OK");
		}
		else
		{
			Status = TEXT("404 Not Found");
			Body = TEXT("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32601,\"message\":\"not found\"},\"id\":null}");
		}

		FTCHARToUTF8 BodyUtf8(*Body);
		// No Access-Control-Allow-Origin: browsers from other origins must not be
		// able to read responses either. Local curl/SDK clients ignore CORS.
		FString Response = FString::Printf(
			TEXT("HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"),
			*Status, BodyUtf8.Length());

		FTCHARToUTF8 RespUtf8(*Response);
		int32 Sent = 0;
		Client->Send((uint8*)RespUtf8.Get(), RespUtf8.Length(), Sent);
		if (BodyUtf8.Length() > 0)
		{
			Client->Send((uint8*)BodyUtf8.Get(), BodyUtf8.Length(), Sent);
		}
	}
}

void FMcpHttpServer::Start(int32 Port)
{
	if (GRunning.exchange(true))
	{
		return;
	}
	int32 EffectivePort = Port;
	LoadOrGenerateToken(Port, EffectivePort);
	GListenerThread = std::thread([EffectivePort]() { ListenerLoop(EffectivePort); });
}

void FMcpHttpServer::Stop()
{
	GRunning = false;
	if (ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
	{
		if (GListenerSocket)
		{
			GListenerSocket->Close();
			SS->DestroySocket(GListenerSocket);
			GListenerSocket = nullptr;
		}
	}
	if (GListenerThread.joinable())
	{
		GListenerThread.join();
	}
}

void FMcpHttpServer::ListenerLoop(int32 Port)
{
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SS)
	{
		UE_LOG(LogTemp, Error, TEXT("[MCP] No socket subsystem"));
		GRunning = false;
		return;
	}

	GListenerSocket = SS->CreateSocket(NAME_Stream, TEXT("SatisfactoryMCP"), false);
	if (!GListenerSocket)
	{
		UE_LOG(LogTemp, Error, TEXT("[MCP] Failed to create listener socket"));
		GRunning = false;
		return;
	}
	GListenerSocket->SetReuseAddr(true);

	TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
	bool bValid = false;
	Addr->SetIp(TEXT("127.0.0.1"), bValid);
	Addr->SetPort(Port);
	if (!bValid || !GListenerSocket->Bind(*Addr))
	{
		UE_LOG(LogTemp, Error, TEXT("[MCP] Failed to bind 127.0.0.1:%d"), Port);
		SS->DestroySocket(GListenerSocket);
		GListenerSocket = nullptr;
		GRunning = false;
		return;
	}
	if (!GListenerSocket->Listen(8))
	{
		UE_LOG(LogTemp, Error, TEXT("[MCP] Failed to listen on %d"), Port);
		SS->DestroySocket(GListenerSocket);
		GListenerSocket = nullptr;
		GRunning = false;
		return;
	}

	UE_LOG(LogTemp, Display, TEXT("[MCP] MCP HTTP server listening on http://127.0.0.1:%d/mcp (auth: bearer)"), Port);

	while (GRunning)
	{
		FSocket* Client = GListenerSocket->Accept(TEXT("mcp client"));
		if (!Client)
		{
			continue;
		}
		std::thread(HandleConnection, Client).detach();
	}

	UE_LOG(LogTemp, Display, TEXT("[MCP] MCP HTTP server stopped"));
}
