# Satisfactory MCP

An in-game MCP server for Satisfactory (SML mod): control building and production from any MCP client (Claude, Hermes, your own script) over HTTP JSON-RPC. All construction goes **through the native hologram pipeline** — the game itself validates placement (clearance, foundations, cost) and builds exactly like a player would.

> Single-player Creative/sandbox only. This is an automation tool, not a multiplayer cheat: the server binds to `127.0.0.1` exclusively.

## Capabilities (28 tools)

| Group | Tools |
|---|---|
| Recon | `get_game_info`, `get_player_state`, `get_buildables_summary`, `find_buildables`, `find_pawns` (players, creatures, mod NPCs), `get_machine_status` (+ connector and power-socket coordinates), `get_ground_z`, `power_check` (+ sockets), `get_resource_nodes`, `list_recipes`, `list_storages` |
| Building | `build` (`grid` — world-grid snapping, `flat_z` — level floors, `dry_run` — validate without building), `build_many` (batch in one call), `build_belt` (native `straight`/`curve` build modes, `via` waypoints, `dry_run`), `wire`, `spawn_buildable`, `move_buildable` |
| Lightweight buildables | `lightweight_list`, `lightweight_remove`, `lightweight_convert` — foundations and walls are stored as lightweight instances that plain actor APIs do not see |
| Gameplay | `dismantle` (with refund), `run_chat_command`, `set_recipe`, `give_item`, `teleport_player`, `set_phase` |

## Security

- Every `POST /mcp` requires `Authorization: Bearer ***`. The token is read from `Mods/SatisfactoryMCP/mcp.ini` (`token=...`, optional `port=...`) or generated once at first launch into `Mods/SatisfactoryMCP/mcp_token.txt` (stable across restarts).
- Browser drive-by attacks are blocked: no CORS headers at all, foreign `Origin`/`Host` → 403 (DNS-rebinding guard per the MCP spec), non-JSON content type → 415.
- Limits: body ≤ 512 KB, headers ≤ 16 KB, ≤ 8 concurrent connections.
- `/health` stays token-free.

## Client configuration

```json
{
  "mcpServers": {
    "satisfactory": {
      "type": "http",
      "url": "http://127.0.0.1:8898/mcp",
      "headers": { "Authorization": "Bearer <token from mcp_token.txt>" }
    }
  }
}
```

curl:
```bash
curl -s -X POST http://127.0.0.1:8898/mcp \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer ***' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

## Building

Fork-specific setup, step by step: [BUILDING.md](BUILDING.md).

Requires the [Satisfactory Modding Starter Project](https://docs.ficsit.app/) (v3.12+) and Unreal Engine 5 with the CSS profile.

```bash
# from the StarterProject root
RunUAT.bat -ScriptsForProject=FactoryGame.uproject PackagePlugin \
  -project=FactoryGame.uproject -clientconfig=Shipping -DLCName=SatisfactoryMCP \
  -build -platform=Win64 -nocompileeditor
```

Copy `Mods/SatisfactoryMCP/Binaries/Win64/*.dll` into `<game>/FactoryGame/Mods/SatisfactoryMCP/Binaries/Win64/`.

## Layout

```
Source/SatisfactoryMCP/
  Private/McpHttpServer.cpp   — HTTP server (raw socket, bearer auth, limits)
  Private/McpRpc.cpp          — JSON-RPC 2.0 layer (initialize, tools/list, tools/call)
  Private/McpTools.cpp        — all tools: hologram pipeline, lightweight subsystem, power
  Private/SatisfactoryMCPModule.cpp — starts the server at game startup
```

## License

MIT — see [LICENSE](LICENSE).
