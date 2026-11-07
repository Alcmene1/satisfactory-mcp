# Satisfactory MCP

MCP-сервер внутри Satisfactory (мод на SML): управляй строительством и производством из любого MCP-клиента (Claude, Hermes, свой скрипт) через HTTP JSON-RPC. Постройки идут **через нативный конвейер голограмм** — игра сама валидирует размещение (clearance, фундамент, стоимость) и строит так же, как игрок.

> Только для одиночной Creative/песочницы. Это инструмент автоматизации, а не обход мультиплеера: сервер слушает исключительно `127.0.0.1`.

## Возможности (26 инструментов)

| Группа | Инструменты |
|---|---|
| Разведка | `get_game_info`, `get_player_state`, `get_buildables_summary`, `find_buildables`, `get_machine_status` (+ координаты коннекторов и силовых розеток), `get_ground_z`, `power_check` (+ сокеты), `get_resource_nodes`, `list_recipes`, `list_storages` |
| Постройка | `build` (`grid` — привязка к мировой сетке, `flat_z` — ровные площадки, `dry_run` — валидация без постройки), `build_many` (батч одним запросом), `build_belt` (режимы `straight`/`curve` как в игре, waypoints `via`, `dry_run`), `wire`, `spawn_buildable`, `move_buildable` |
| Lightweight-постройки | `lightweight_list`, `lightweight_remove`, `lightweight_convert` — фундаменты и стены хранятся лёгкими инстансами, обычные актёрные API их не видят |
| Игра | `dismantle`, `set_recipe`, `give_item`, `teleport_player`, `set_phase`, `power_check` |

## Безопасность

- Токен: `Authorization: Bearer <token>` на всех `POST /mcp`. Берётся из `Mods/SatisfactoryMCP/mcp.ini` (`token=...`, опционально `port=...`) или генерируется при первом запуске в `Mods/SatisfactoryMCP/mcp_token.txt`.
- Браузерный drive-by закрыт: нет CORS-заголовков, чужие `Origin`/`Host` → 403 (анти-DNS-rebinding), не-JSON content-type → 415.
- Лимиты: тело ≤ 512 КБ, шапка ≤ 16 КБ, ≤ 8 одновременных соединений.
- `/health` — без токена.

## Подключение клиента

```json
{
  "mcpServers": {
    "satisfactory": {
      "type": "http",
      "url": "http://127.0.0.1:8898/mcp",
      "headers": { "Authorization": "Bearer <из mcp_token.txt>" }
    }
  }
}
```

curl:
```bash
curl -s -X POST http://127.0.0.1:8898/mcp \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer <token>' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

## Сборка

Требуются [Satisfactory Modding Starter Project](https://docs.ficsit.app/) (v3.12+) и Unreal Engine 5 с CSS-профилем.

```bash
# из корня StarterProject
RunUAT.bat -ScriptsForProject=FactoryGame.uproject PackagePlugin \
  -project=FactoryGame.uproject -clientconfig=Shipping -DLCName=SatisfactoryMCP \
  -build -platform=Win64 -nocompileeditor
```

Скопировать `Mods/SatisfactoryMCP/Binaries/Win64/*.dll` в `<игра>/FactoryGame/Mods/SatisfactoryMCP/Binaries/Win64/`.

## Структура

```
Source/SatisfactoryMCP/
  Private/McpHttpServer.cpp   — HTTP-сервер (сокет, bearer-auth, лимиты)
  Private/McpRpc.cpp          — JSON-RPC 2.0 слой (initialize, tools/list, tools/call)
  Private/McpTools.cpp        — все инструменты: голограмма-пайплайн, lightweight-подсистема, энергия
  Private/SatisfactoryMCPModule.cpp — запуск сервера при старте игры
```

## Лицензия

Пока не выбрана — хотите использовать: напишите автору.
