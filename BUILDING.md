# Building this fork

Branch `claude/dismantle-refund-run-chat-command` adds a refunding `dismantle` and a
`run_chat_command` tool. It was written against the headers but **has never been compiled or run**.
Expect that the first build may need small fixes.

Commands are PowerShell. Paths assume this PC: game at `G:\Steam\steamapps\common\Satisfactory`,
game build **517554** (1.2.5, read from `Engine\Binaries\Win64\FactoryGameSteam-Win64-Shipping.version`).

Rough cost: 30+ GB of disk, a few hours, mostly downloads and the first editor build.

## 1. Install Visual Studio 2022

Use VS 2022, not 2026. Run <https://aka.ms/vs/17/release/vs_community.exe>, choose **Import
Configuration** and give it
<https://docs.ficsit.app/satisfactory-modding/latest/_attachments/BeginnersGuide/dependencies/SML.vsconfig>
(download the file first).

Without the import, select by hand:

- Workloads: **.NET Desktop Development**, **Desktop development with C++**, **Game development with C++**
- Individual components: **MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.38-17.8)**,
  **.NET 8.0 Runtime (Long Term Support)**, **.NET Framework 4.8.1 SDK**

## 2. Install the custom Unreal Engine (5.6.1-CSS)

The engine download is private until your GitHub account is linked to Epic. It currently returns
404 for `Alcmene1`, so this step is needed.

1. Link GitHub to an Epic account as described at <https://github.com/EpicGames/UnrealEngine/>, and
   accept the invitation. That URL should then open instead of showing 404.
2. Link it to the modding organisation: <https://linker.ficsit.app/link>.
3. From <https://github.com/satisfactorymodding/UnrealEngine/releases/latest> download these four
   files into one folder, with exactly these names:
   - `UnrealEngine-CSS-Editor-Win64-1.bin`
   - `UnrealEngine-CSS-Editor-Win64-2.bin`
   - `UnrealEngine-CSS-Editor-Win64-3.bin`
   - `UnrealEngine-CSS-Editor-Win64.exe`
4. Run the `.exe` and accept the default location (`C:\Program Files\Unreal Engine - CSS\`).

## 3. Install Wwise

1. Get the Audiokinetic Launcher from <https://www.audiokinetic.com/en/download/> (needs a free account).
2. On the launcher's Wwise tab change **Latest** to **All**, then Major **2025.1**, Version **2025.1.6.9117**.
3. Packages: **Authoring** and **SDK (C++)**.
4. Deployment platforms: **Linux**, and under Microsoft/Windows **Visual Studio 2019**,
   **Visual Studio 2022** and **Game Core**. Deselect all plugins.

## 4. Get the Starter Project at the commit matching the game

The headers must match the game build. Commit `a7e8550` ("Update headers to CL517554", SML 3.12.0)
matches build 517554. The `v3.12.0` tag is older, so do not use it. The folder is outside `D:\Dev`
on purpose (about 30 GB, third-party), and its path must contain only ASCII characters.

```powershell
New-Item -ItemType Directory -Force D:\Modding
git clone -b master https://github.com/satisfactorymodding/SatisfactoryModLoader.git D:\Modding\SatisfactoryModLoader
git -C D:\Modding\SatisfactoryModLoader checkout a7e8550
```

After a game update, read the new `BuildId` from the `.version` file above and check out the SML
commit named "Update headers to CL<BuildId>".

## 5. Integrate Wwise into the project

In the Wwise launcher, **Unreal Engine** tab:

1. **Open other** next to "RECENT UNREAL ENGINE PROJECTS", then select
   `D:\Modding\SatisfactoryModLoader\FactoryGame.uproject`.
2. **Integrate Wwise in Project...**, change Integration Version from **Latest** to **All**, select
   **2025.1.6.9117**.
3. Under Wwise Project choose **New project**, then **Integrate**.
4. Check `FactoryGame.uproject` still says `"EngineAssociation": "5.6.1-CSS"`.
5. Open the generated `SatisfactoryModLoader_WwiseProject.wproj`. In Project Explorer > SoundBanks,
   right-click the top folder and choose **Generate Soundbank(s) for all platforms**.

## 6. Add the mod source

```powershell
git clone -b claude/dismantle-refund-run-chat-command https://github.com/Alcmene1/satisfactory-mcp.git D:\Modding\SatisfactoryModLoader\Mods\SatisfactoryMCP
```

## 7. Build the editor

```powershell
& "C:\Program Files\Unreal Engine - CSS\Engine\Build\BatchFiles\Build.bat" -projectfiles -project="D:\Modding\SatisfactoryModLoader\FactoryGame.uproject" -game -rocket -progress
```

Open `D:\Modding\SatisfactoryModLoader\FactoryGame.sln`. Set the configuration to **Development
Editor** and the platform to **Win64**. Right-click the **FactoryGame** project under
Games/FactoryGame and choose **Build**. Do not use Build > Build Solution.

Errors mentioning AkAudio mean step 5 needs redoing. Errors in `McpTools.cpp` are the untested new
code; fix them in this clone and push the fix to the branch.

## 8. Back up, and save the token

Close the game first, then back up the saves. This prints the backup folder, or `None` when the
newest backup already holds the current saves.

```powershell
Set-Location D:\Dev\satisfactory-companion
& "C:\Users\ninas\AppData\Local\Programs\Python\Python313\python.exe" -c "import backup; print(backup.backup_saves({}))"
```

The next step deletes the mod's folder, which holds the bearer token both MCP registrations use.
Without this copy the mod generates a new token and both registrations stop working.

```powershell
Copy-Item "G:\Steam\steamapps\common\Satisfactory\FactoryGame\Mods\SatisfactoryMCP\mcp_token.txt" D:\Backups\SatisfactoryMCP-mcp_token.txt
```

## 9. Take the release build out of SMM

In Satisfactory Mod Manager, on the **Companion** profile, remove **Satisfactory MCP Server**. Keep
FRM, SML and SatisfactoBot. Let SMM apply the change, then close it.

This step is required. SMM writes a `.smm` hash file into each mod it manages, and on every sync it
deletes and re-extracts any mod in the profile whose hash file is missing or different, so it would
replace your build with release 1.0.1. A folder with no `.smm` file that is not in the profile is
left alone (`cli/installations.go` and `utils/io.go` in `satisfactorymodding/ficsit-cli`).

The local build then stays installed whichever SMM profile is active, and SMM does not list it.

## 10. Package into the game

1. Open `D:\Modding\SatisfactoryModLoader\FactoryGame.uproject`.
2. Open **File > Alpakit Dev** (or the alpaca icon on the toolbar).
3. Under Dev Packaging Settings > Windows, tick **Enabled** and **Copy to Game Path**, then use the
   three dots to pick `G:\Steam\steamapps\common\Satisfactory`.
4. Press **Alpakit!** on the SatisfactoryMCP row. Progress is in **File > Alpakit Log**.

Then restore the token:

```powershell
Copy-Item D:\Backups\SatisfactoryMCP-mcp_token.txt "G:\Steam\steamapps\common\Satisfactory\FactoryGame\Mods\SatisfactoryMCP\mcp_token.txt"
```

## 11. Check it

Start the game from SMM on the Companion profile and load a save, then from a bash shell in
`D:\Dev\satisfactory-companion`:

```bash
./mcpcall.sh run_chat_command '{"command":"/help"}'
```

Expect `"recognised":true`, with SML's help text in the game chat. `/follow` then shows whether
SatisfactoBot registers real SML commands: `"recognised":false` with the bot still reacting means it
uses its own chat hook.

For the refund, build one Conveyor Pole with `build`, then call `dismantle` on it. Expect
`"gone":true` and a `refunded` list, with the items back in the pocket inventory. Repeat once with a
full inventory and expect `toCrate` above 0 and a dismantle crate beside the site.

**If the game crashes on start or on a call:** delete
`G:\Steam\steamapps\common\Satisfactory\FactoryGame\Mods\SatisfactoryMCP`, add the mod back in SMM to
get release 1.0.1, and restore the token as above.

## After every game update

Repeat from step 4 with the new SML commit, then steps 7 and 10. Back up the token first, and restore
it if the folder was replaced.

## What actually worked (2026-11-08, no editor needed)

Steps 1 to 6 as above (the Wwise integration in the launcher took the defaults: version 2025.1.6.9117, Wwise Project left empty). Everything after that ran from PowerShell; the editor was never opened. The whole thing took about an hour, most of it downloads.

Soundbanks (replaces the Wwise UI part of step 5):

```powershell
& 'C:\Audiokinetic\Wwise_2025.1.6.9117\Authoring\x64\Release\bin\WwiseConsole.exe' generate-soundbank 'D:\Modding\SatisfactoryModLoader\SatisfactoryModLoader_WwiseProject\SatisfactoryModLoader_WwiseProject.wproj'
```

Project files, then the editor build (step 7). The editor target is `FactoryEditor`, not `FactoryGameEditor`. The build took 4 minutes because the CSS engine ships prebuilt; the mod compiled cleanly first time.

```powershell
& 'C:\Program Files\Unreal Engine - CSS\Engine\Build\BatchFiles\Build.bat' -projectfiles -project='D:\Modding\SatisfactoryModLoader\FactoryGame.uproject' -game -rocket -progress
& 'C:\Program Files\Unreal Engine - CSS\Engine\Build\BatchFiles\Build.bat' FactoryEditor Win64 Development -Project='D:\Modding\SatisfactoryModLoader\FactoryGame.uproject' -WaitMutex -FromMsBuild
```

Packaging and copying into the game (step 10), the same UAT command Alpakit Dev runs, Windows only. It also writes `Saved\ArchivedPlugins\SatisfactoryMCP\SatisfactoryMCP-Windows.zip`. It replaces the game's `Mods\SatisfactoryMCP` folder, so restore the token afterwards as in step 10.

```powershell
& 'C:\Program Files\Unreal Engine - CSS\Engine\Build\BatchFiles\RunUAT.bat' -ScriptsForProject='D:\Modding\SatisfactoryModLoader\FactoryGame.uproject' PackagePlugin -project='D:\Modding\SatisfactoryModLoader\FactoryGame.uproject' -clientconfig=Shipping -serverconfig=Shipping -utf8output -DLCName=SatisfactoryMCP -build -platform=Win64 -nocompileeditor -installed -CopyToGameDirectory_Windows='G:\Steam\steamapps\common\Satisfactory'
```

After a code change: edit under `D:\Modding\SatisfactoryModLoader\Mods\SatisfactoryMCP` (a clone of this branch), commit and push there, then rerun only the packaging command and restore the token.
