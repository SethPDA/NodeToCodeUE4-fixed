# Blueprint ⇄ Claude Bridge for NodeToCode (UE 4.27)

This is a handoff document. It covers extending the **NodeToCode (N2C)** UE 4.27 editor plugin so it can pass work back and forth between a developer who builds in **Blueprints** and an AI (Claude or any other model) that writes **Blueprint graphs and C++**.

**Read the whole document before writing code.** Sections 0, 5, 7, 8 and 15 are required.

> Analysis date: 2026-09-17. Everything below was checked against the code on disk at that date: plugin HEAD `42ead07`, engine `C:/UE/UE_4.27`.
> References like `File.cpp:123` point to the plugin source unless marked **engine**. Engine paths are relative to `C:/UE/UE_4.27/Engine/Source/`.

---

## 0. Rules for implementing models

1. **Do not commit or push** to either repository unless the user explicitly asks.
   - Game repo: `D:/DEV/Unreal/VoidLine/VoidLine427` (branch `master`, remote `gitlab.com/SethPDA/voidline.git`, Git LFS for `.uasset`/`.umap`).
   - Plugin repo: `D:/DEV/Unreal/_Plugin/NodeToCodeUE4` (branch `main`, remote `github.com/SethPDA/NodeToCodeUE4-fixed`).
2. **Edit the in-project copy**, `D:/DEV/Unreal/VoidLine/VoidLine427/Plugins/NodeToCodeUE4-main`. It compiles together with the VoidLine game module, so you can test quickly. At the end of each phase, **sync it back** to the plugin repo (§3.4).
   - This document exists **only in the plugin repo** (`docs/`). Never mirror the plugin root folder, or this file will be deleted.
3. **UE 4.27 only.** Follow the port rules in the plugin's `CLAUDE.md` ("UE4.27 Port Rules") and in `docs/UE4_PORT_GUIDE.md`. §3.5 summarizes them.
4. **Verify engine APIs in the local engine source** (`C:/UE/UE_4.27/Engine/Source`, which includes the editor `.cpp` files) before using them. Don't rely on what you remember from UE5.
5. **Keep v1 behaviour intact.** The existing "Node to Code" translation and "Copy Blueprint JSON" (v1.0.0 format) must keep working unchanged. All new work is additive.
6. **Work in phases** (§15). A phase must build cleanly and pass its acceptance checks before the next one starts. Report results honestly, including failing tests.
7. The user doesn't know C++ well. Any C++ written *for the game* (not the plugin) must follow §12.1 and come with Blueprint usage instructions or a usage graph.

---

## 1. TL;DR

- **Goal.** The user exports a Blueprint graph as JSON and gives it to Claude. Claude replies with one or both of:
  - a Blueprint graph as JSON, which the plugin turns into **real nodes** (inserted into the open graph, or copied for the user to paste);
  - **C++ `.h/.cpp`** files.
- **Problem.** The current N2C JSON (v1.0.0) was designed for LLMs to read, and **it can't be turned back into a graph** (evidence in §5). It loses:
  - which pin each exec wire starts from;
  - internal pin names (only display names are kept);
  - full class names;
  - variable owner and scope;
  - the node class;
  - Blueprint-level declarations.
- **Solution.** Add a lossless **"N2C Graph v2"** document format (§7), used both for export and for AI authoring. Add an **importer inside the plugin** (C++, editor module, §8). It:
  - resolves every node against live engine reflection;
  - creates nodes with the engine's own spawners;
  - wires them through the K2 schema (the same checks as a human drag);
  - sets defaults and lays the nodes out;
  - reports every problem in a copyable **validation report** that goes back to the AI.

  The import panel offers three actions: **Validate**, **Insert into focused graph** (one undo step), and **Copy as nodes** (native clipboard text you can Ctrl+V anywhere).
- **C++.**
  - **Primary path:** Claude writes idiomatic C++ directly, following the project's conventions (§12.1). With it comes a v2 *usage graph* that calls the new nodes, so the user can import ready-wired Blueprint nodes.
  - **Secondary path:** a **deterministic generator** (Python, standard library only) for a documented subset. It turns pure/stateless function graphs into a `UBlueprintFunctionLibrary` (§12.2).
  - A **"Manual (copy/paste)" LLM provider** makes N2C's existing C++ translation work with claude.ai chat, without an API key (§11).
- **Other conveniences** (§13–14):
  - A folder bridge, `VoidLine427/Bridge/`, plus a project `CLAUDE.md`. Claude Code reads exports and drops graphs/C++ there, with no copy/paste.
  - Blueprint summary and node catalog exports, so the AI never has to guess node names.
  - Later, an MCP server, by backporting the user's own **AgentBridge** plugin to 4.27.
- **Local model training: not recommended as the core** (Appendix A).
  - The conversions are deterministic code, so no model is needed for them. The only role left for a model is "author".
  - A 7–14B QLoRA model on an 11 GB Turing GPU won't match Claude at that. The user already tried a Qwen3-14B "Claude distill" in LM Studio and found it not good enough.
  - Appendix A still gives a complete, honest recipe in case they want to try.

---

## 2. Requirements and decisions (from the user)

| # | Requirement / decision |
|---|---|
| R1 | Main game logic stays in Blueprints. Claude implements selected features in C++. |
| R2 | The user sends a Blueprint node tree to Claude as JSON, which is easier to read than screenshots. |
| R3 | Claude replies with JSON. The plugin gets a **new field/button** to paste that JSON and turn it into a **proper, pasteable Blueprint node tree**. |
| R4 | A way to turn JSON into **C++ `.h` + `.cpp`**, either in the plugin or as a Python script. |
| R5 | If the JSON approach can't work well, consider training a local 7–14B model. That needs instructions and training data sources. |
| R6 | Any other ideas that make the BP-user ↔ Claude-C++ workflow smoother. |
| D1 | The user talks to Claude through **both** claude.ai chat (copy/paste) and **Claude Code** (desktop, with file access), with no Anthropic API key. **Build copy/paste first**, then the file-based bridge. |
| D2 | Edit the **in-project** plugin copy, then sync to the plugin repo. |
| D3 | This document lives in the plugin repo `docs/`. No commits. |

---

## 3. Environment

### 3.1 Paths

| What | Path |
|---|---|
| Game project | `D:/DEV/Unreal/VoidLine/VoidLine427/VoidLine.uproject` (C++ module `VoidLine`, editor target `VoidLineEditor`) |
| Plugin working copy | `D:/DEV/Unreal/VoidLine/VoidLine427/Plugins/NodeToCodeUE4-main` (tracked by the game repo) |
| Plugin repo | `D:/DEV/Unreal/_Plugin/NodeToCodeUE4` |
| Stale plugin copy (ignore) | `D:/DEV/Unreal/PluginCompile427/Plugins/NodeToCodeUE4-main` (older, 5 files differ) |
| Engine | `C:/UE/UE_4.27` (launcher build; `Engine/Source` includes editor `.cpp` files) |
| AgentBridge (MCP, UE 5.5+) | `D:/DEV/Unreal/_Plugin/Plugins/AgentBridge` |
| Real v1 exports (test data) | `D:/DEV/Unreal/VoidLine/VoidLine427/Saved/NodeToCode/Translations/BP_Grid_Revealed_*` |
| bpcodec skill (Claude desktop) | `C:/Users/SethPDA/AppData/Roaming/Claude/local-agent-mode-sessions/skills-plugin/.../skills/workflow-bp/scripts/bpcodec.py` |

- **Machine:** RTX 2080 Ti 11 GB, 64 GB RAM, Ryzen 7 5700X3D, Windows 11, Python 3.13 on PATH, WSL2 Ubuntu, Ollama 0.32 client.
- **Line endings:** the two plugin copies differ only in line endings (repo CRLF, in-project LF) and in `README.md`. Both repos use `core.autocrlf=true`. Compare them with `diff -rq --strip-trailing-cr`.

### 3.2 Build
Close the editor first; a normal build is blocked while Live Coding is active.

```bash
"C:/UE/UE_4.27/Engine/Build/BatchFiles/Build.bat" VoidLineEditor Win64 Development -Project="D:/DEV/Unreal/VoidLine/VoidLine427/VoidLine.uproject" -WaitMutex
```

After adding or removing source files or changing `Build.cs`, regenerate the project files:

```bash
"C:/UE/UE_4.27/Engine/Binaries/DotNET/UnrealBuildTool.exe" -projectfiles -project="D:/DEV/Unreal/VoidLine/VoidLine427/VoidLine.uproject" -game -engine -progress
```

**Live Coding** (Ctrl+Alt+F11 in the editor) only handles changes inside function bodies. New `UCLASS`/`UPROPERTY`/`UFUNCTION` declarations or header layout changes need a full build with the editor closed.

### 3.3 Automated tests from the command line
Verified in the engine source: `testexit=` is parsed at `Runtime/Launch/Private/LaunchEngineLoop.cpp:1588`, and `Automation RunTests` / `Quit` at `Developer/AutomationController/Private/AutomationCommandline.cpp:555,643`.

```bash
"C:/UE/UE_4.27/Engine/Binaries/Win64/UE4Editor-Cmd.exe" "D:/DEV/Unreal/VoidLine/VoidLine427/VoidLine.uproject" -ExecCmds="Automation RunTests NodeToCode;Quit" -TestExit="Automation Test Queue Empty" -unattended -nopause -nullrhi -log
```

### 3.4 Syncing the working copy back to the plugin repo (run in PowerShell or cmd)

```bash
robocopy "D:\DEV\Unreal\VoidLine\VoidLine427\Plugins\NodeToCodeUE4-main\Source" "D:\DEV\Unreal\_Plugin\NodeToCodeUE4\Source" /MIR
```
```bash
robocopy "D:\DEV\Unreal\VoidLine\VoidLine427\Plugins\NodeToCodeUE4-main\Content" "D:\DEV\Unreal\_Plugin\NodeToCodeUE4\Content" /MIR
```
```bash
robocopy "D:\DEV\Unreal\VoidLine\VoidLine427\Plugins\NodeToCodeUE4-main\Resources" "D:\DEV\Unreal\_Plugin\NodeToCodeUE4\Resources" /MIR
```
```bash
copy /Y "D:\DEV\Unreal\VoidLine\VoidLine427\Plugins\NodeToCodeUE4-main\NodeToCode.uplugin" "D:\DEV\Unreal\_Plugin\NodeToCodeUE4\NodeToCode.uplugin"
```

**Never** `/MIR` the plugin root, `docs/`, `.git`, `Binaries` or `Intermediate`. After syncing, review `git -C D:/DEV/Unreal/_Plugin/NodeToCodeUE4 status`. Don't commit.

### 3.5 UE 4.27 rules for new plugin code (from `CLAUDE.md` / `UE4_PORT_GUIDE.md`)

**API differences from UE5**
- Use `FEditorStyle` / `EditorStyleSet.h`, never `FAppStyle`.
- No `PC_Double`/`PC_Real`; only `PC_Float`.
- No `UK2Node_PromotableOperator` or `UK2Node_ExternalGraphInterface`.
- `TryGetNumberField` takes `double&` (or `int32&`/`uint32&`/`int64&`).
- Write `ESPMode::ThreadSafe` explicitly on `TSharedRef<IHttpRequest, ...>`.
- For asset editors, use `UAssetEditorSubsystem` (`GEditor->GetEditorSubsystem<>()`), not `FAssetEditorManager`.
- `SMultiLineEditableText` has no `SelectText`/`GetSelection`/`GetCursorLocation`/`DeleteSelectedText`.
- `.uplugin` uses `WhitelistPlatforms`.

**Patterns**
- Dynamic multicast delegates need a UObject. Add a parallel native `DECLARE_MULTICAST_DELEGATE` for Slate `AddRaw`.
- No UMG, Blutility or Editor Utility Widgets in this plugin.
- Include explicitly: `Misc/FileHelper.h`, `Internationalization/Regex.h`, `Policies/CondensedJsonPrintPolicy.h`, `Widgets/Text/STextBlock.h`, `Widgets/Input/SMultiLineEditableTextBox.h`.
- Before using `FPlatformApplicationMisc`, include `Windows/WindowsPlatformApplicationMisc.h` under `#if PLATFORM_WINDOWS` (see the pattern at `N2CEditorWindow.cpp:25-31`).
- Use `LOCTEXT_NAMESPACE "NodeToCode"`, log through `FN2CLogger::Get()`, and use the style sets `"NodeToCodeStyle"` and `"N2CCodeEditor"`.

**Module**
- The module is `Editor`, loading phase `PostEngineInit`.
- `UnrealEd`, `BlueprintGraph`, `Kismet`, `GraphEditor`, `Json`, `ApplicationCore`, `ToolMenus` and `EditorStyle` are already dependencies.
- **Add `JsonUtilities`** if you use `FJsonObjectConverter`, and **`DirectoryWatcher`** for §13.

---

## 4. How NodeToCode works today

### 4.1 Pipeline

```
Blueprint editor toolbar ("Node to Code" button + dropdown: "Open Node to Code Editor", "Copy Blueprint JSON")
  └─ FN2CEditorIntegration::ExecuteCollectNodesForEditor / ExecuteCopyJsonForEditor   (N2CEditorIntegration.cpp:417 / :36)
       ├─ Editor->GetFocusedGraph()                                                    (:440 / :50)
       ├─ FN2CNodeCollector::CollectNodesFromGraph   → ALL UK2Nodes of the graph (selection ignored)   (N2CNodeCollector.cpp:34-40)
       ├─ FN2CNodeTranslator::GenerateN2CStruct → FN2CBlueprint                        (N2CNodeTranslator.cpp)
       ├─ FN2CBlueprint::IsValid() (FN2CBlueprintValidator)                            (blocks export on failure)
       └─ FN2CSerializer::ToJson (pretty for clipboard, condensed for LLM)
            └─ (translate) UN2CLLMModule::ProcessN2CJson → provider SendRequest → response parser
                 → SaveTranslationToDisk → OnTranslationResponseReceivedNative → SN2CEditorWindow shows code
```

### 4.2 Files that matter

| Area | Files |
|---|---|
| Models (v1) | `Source/Public/Models/N2CBlueprint.h`, `N2CNode.h`, `N2CPin.h`, `N2CTranslation.h` |
| Serialization | `Source/Private/Core/N2CSerializer.cpp` (`ToJson`; `FromJson` is never called) |
| Translation | `Source/Private/Core/N2CNodeTranslator.cpp`, `Utils/Processors/*`, `Utils/N2CNodeTypeRegistry.cpp` |
| Editor hooks | `Source/Private/Core/N2CEditorIntegration.cpp` (toolbar/menu, gets the editor and graph), `N2CToolbarCommand.cpp` (the 3 commands) |
| Window | `Source/Private/Core/N2CEditorWindow.cpp` (`SN2CEditorWindow`, tab id `"NodeToCodeEditor"`) |
| LLM | `Source/Private/LLM/N2CLLMModule.cpp`, `N2CBaseLLMService.cpp`, `N2CSystemPromptManager.cpp`, `N2CResponseParserBase.cpp`, `Providers/*` |
| Prompts | `Content/Prompting/CodeGen_CPP.md` (+ other languages) |
| Settings | `Source/Public/Core/N2CSettings.h` (`UN2CSettings`, saved to `Config/DefaultNodeToCode.ini`; VoidLine currently uses `Provider=LMStudio`) |

### 4.3 Editor window layout (`N2CEditorWindow.cpp`, `Construct` at 101-384)

**Current layout**
- **Toolbar** (`SHorizontalBox`): graph selector combo, status text, spacer, then the **Copy Header** (163-182), **Copy Code** (185-194) and **Open Folder** (197-206) buttons.
- **Body** (`SWidgetSwitcher PanelSwitcher`) has four panels:
  - [0] Welcome
  - [1] Loading
  - [2] Results (Declaration/Implementation `SN2CCodeEditor` + Notes)
  - [3] Error

**Where to add things**
- New toolbar buttons: after line 206.
- A new panel [4] "Import": after line 382.
- Handler declarations: next to `N2CEditorWindow.h:56-59`.

**Gotchas**
- The window has **no reference to the Blueprint editor**. `FN2CEditorIntegration::GetBlueprintEditorFromTab()` is a stub that returns nullptr (`N2CEditorIntegration.cpp:175-180`). You must add "last active Blueprint editor" tracking (§10.3).
- The Copy buttons copy `CachedResponse`, not the text as edited in the code panes.

### 4.4 Output on disk (`N2CLLMModule.cpp:216-428`)

Each translation goes to `Saved/NodeToCode/Translations/<BP>_<YYYY-MM-DD-HH.MM.SS>/`, which contains:
- `N2C_BP_<BP>_<ts>.json`
- `N2C_BP_Minified_<BP>_<ts>.json`
- `N2C_Translation_<BP>_<ts>.json`
- `<Graph>/<Graph>.h|.cpp|_Notes.txt`

The file stems drop the seconds: `FPaths::GetBaseFilename` treats `.SS` as an extension.

### 4.5 LLM response contract (unchanged; reused by §11 and §12)
```json
{"graphs":[{"graph_name":"X","graph_type":"Function","graph_class":"","code":{"graphDeclaration":"...","graphImplementation":"...","implementationNotes":"..."}}]}
```
It is parsed by `UN2CResponseParserBase::ParseLLMResponse` (`N2CResponseParserBase.cpp:12-151`).

---

## 5. Why v1 JSON can't be turned back into Blueprints

This was checked in the code and in the real export `Saved/NodeToCode/Translations/BP_Grid_Revealed_2026-07-06-12.59.56/N2C_BP_*.json` (77 nodes).

| Gap | Evidence | Consequence |
|---|---|---|
| Exec flows are node pairs `"N26->N27"` with **no pin names** | `N2CNodeTranslator.cpp:900-901` | You can't tell Branch True from False, Loop Body from Completed, or which Sequence output was used |
| Pin `name` is the **display name** (`"Return Value"`, `"Target Map"`, exec `""`) | `N2CNodeTranslator.cpp:760` (`GetDisplayName`) | Pins can't be found reliably; the internal names are `ReturnValue`, `TargetMap`, `execute`/`then` |
| `member_parent` is a short class name (`KismetMathLibrary`, `StandardMacros`) | `N2CFunctionCallProcessor.cpp:15` | Ambiguous, and there's no package path |
| On variable nodes, `member_parent` holds the variable **type** (`"int"`, `"struct/IntVector"`) | `N2CVariableProcessor.cpp:38-65` | The owner class, self-context and local/param scope are lost |
| Get vs Set is lost for local variables and function parameters (`LocalFunctionVariable` / `FunctionParameter`) | `N2CNodeTypeRegistry.cpp:958-1016` | The right node can't be recreated |
| No node UClass, GUIDs or positions | `N2CNode.h:181-236` | The graph can't be recreated exactly |
| The map **value** type is dropped; split struct pins are flattened; hidden pins, knots and comment boxes are dropped | `N2CNodeTranslator.cpp:735-738, 175` | Type information is lost |
| No Blueprint declarations (variables, function signatures, dispatchers, components, parent class, interfaces) | — | Missing members can't be created |
| `FromJson` exists but is never called. It drops every exec pin (it requires `type`) and never parses structs/enums | `N2CSerializer.cpp:55-68, 678-684` | There is no reverse path |

**Conclusion:** keep v1 as the format LLMs read, and add **v2** for round-tripping.

---

## 6. Target workflows

### 6.1 Chat (claude.ai) — built first
1. In the Blueprint editor, select nodes (or none, for the whole graph) and use **N2C ▸ Copy Graph JSON (v2)**. Optionally also use **Copy Blueprint Summary**.
2. Paste into claude.ai together with the feature request. Include the authoring prompt (`Content/Prompting/BlueprintGraph_Authoring.md`, §11.2), or keep it in a claude.ai Project's instructions.
3. Claude replies with a v2 JSON document and/or C++ files.
4. In the N2C window, open the **Import** tab and paste the JSON. Press **Validate**, then **Insert into graph** or **Copy as nodes** (then Ctrl+V in any graph).
5. If the report shows errors, press **Copy report**, paste it back to Claude, and repeat until the report is clean.
6. For C++, either use Claude's files directly or run N2C's normal translation through chat with the **Manual provider** (§11.1).

### 6.2 Claude Code (desktop app) — built second
1. The plugin writes every export to `VoidLine427/Bridge/outbox/` (graph v2, summary, catalog, reports).
2. The user tells Claude Code something like: "implement X, the graph is in the outbox".
3. Claude reads the files, writes C++ into `Source/VoidLine/`, builds it (§3.2), and writes graphs to `Bridge/inbox/*.n2cgraph.json`.
4. The plugin notices the new file (`DirectoryWatcher`) and shows a toast: **"Graph from Claude: X — Import / Copy as nodes / Open"**. Reports are written back to `outbox/` automatically, so Claude can fix problems without the user copying anything.
5. The project `CLAUDE.md` (§13.2) teaches every new Claude Code session this workflow.

---

## 7. N2C Graph v2 format (specification)

### 7.1 Design principles
- **Easy for an AI to author:** short, readable and tolerant. Short class names and display names are accepted; the resolver reports what it picked.
- **Lossless enough to round-trip** when the plugin exports it: internal names, full paths, positions.
- **Links are explicit pin-to-pin pairs**, so fan-out and fan-in are both possible.
- **Node vocabulary matches AgentBridge** (`call_function`, `variable_get`, …), so a future MCP tool can accept the same document (§14).
- **Plain JSON.** Real documents have no comments. Examples in this file that carry `//` notes are marked `jsonc`.
- **Versioned:** `"format": "n2c.graph", "version": 2`.

### 7.2 Top level

| Field | Type | Req | Meaning |
|---|---|---|---|
| `format` | `"n2c.graph"` | ✔ | Identifies the document |
| `version` | int `2` | ✔ | Schema version |
| `engine` | string | – | `"4.27"`, informational |
| `blueprint` | string | – | Target Blueprint asset path (`/Game/.../BP_X`) or name. The file bridge and the MCP tool use it. Import from the window uses the focused editor instead |
| `summary` | string | – | One-line description by the author, shown in the toast and the report |
| `declarations` | object | – | Members to create **if missing** (§7.6) |
| `graphs` | array | ✔ | One or more graphs (§7.3) |
| `symbols` | object | – | Written by the **exporter only**: C++ signature facts for the generator (§12.2.3). Authors never write it |

### 7.3 Graph

| Field | Type | Req | Meaning |
|---|---|---|---|
| `name` | string | ✔ | `EventGraph`, a function name, or a macro name. It matters only when the target is chosen automatically (file bridge/MCP). Window import uses the focused graph, unless `name` matches a function graph and "Import into matching graph" is enabled |
| `kind` | `event_graph` \| `function` \| `macro` | – | Defaults to `function` if there's a `function_entry` node, otherwise `event_graph` |
| `nodes` | array | ✔ | §7.4 |
| `links` | array of `[from, to]` | ✔ (may be `[]`) | Each end is `"nodeId.pin"`. `from` must be an **output** pin and `to` an **input** pin. The importer swaps reversed pairs and warns |
| `comments` | array | – | `{"text": "...", "nodes": ["id", ...], "color": [r,g,b,a]}` puts a comment box around those nodes |

### 7.4 Node

Common fields:

| Field | Type | Req | Meaning |
|---|---|---|---|
| `id` | string | ✔ | Unique within the graph; letters, digits and `_` (e.g. `begin`, `print1`). Used in links |
| `kind` | string | ✔ | See the table below |
| `defaults` | object | – | `{ "PinName": "value" }`, applied to **unconnected input** pins (§7.8) |
| `pos` | `[x, y]` | – | Optional. The exporter always writes it; without it the importer lays nodes out automatically |
| `comment` | string | – | Node comment bubble |
| `pure` | bool | – | Only for `call_function` on a function that can be either pure or impure. Normally ignored |

Kind-specific fields:

| `kind` | Fields | Creates (4.27 class) |
|---|---|---|
| `event` | `event` (function name, e.g. `ReceiveBeginPlay`, `ReceiveTick`, `ReceiveActorBeginOverlap`), `class` (optional owner) | `UK2Node_Event` (override). **Reuses the existing node** if the Blueprint already has one (`FBlueprintEditorUtils::FindOverrideForFunction`) |
| `custom_event` | `name`, `inputs` (optional param list, §7.7) | `UK2Node_CustomEvent`. An existing event with the same name is **reused** |
| `call_function` | `function` (internal name, e.g. `PrintString`), `class` (optional: short name, `U`-prefixed name, or path such as `/Script/Engine.KismetSystemLibrary`) | Chosen by `UBlueprintFunctionNodeSpawner` (plain CallFunction, CommutativeAssociativeBinaryOperator, CallArrayFunction, …) |
| `call_parent` | `function` | `UK2Node_CallParentFunction` |
| `variable_get` / `variable_set` | `variable`; `scope`: `self` (default) \| `local` \| `param` \| `external`; `class` is required for `external` | `UK2Node_VariableGet/Set` with `SetSelfMember` / `SetLocalMember` / `SetExternalMember` |
| `self` | – | `UK2Node_Self` |
| `branch` | – | `UK2Node_IfThenElse` |
| `sequence` | `outputs` (int, default 2) | `UK2Node_ExecutionSequence` |
| `macro` | `macro` (e.g. `ForEachLoop`), `library` (default `/Engine/EditorBlueprintResources/StandardMacros.StandardMacros`) | `UK2Node_MacroInstance` + `SetMacroGraph` |
| `cast` | `class` (target class name or path; Blueprint classes by asset name, e.g. `BP_Enemy`), `pure` (bool) | `UK2Node_DynamicCast` (`TargetType`, `SetPurity`) |
| `make_struct` / `break_struct` | `struct` (e.g. `Vector`, `IntVector`, `/Script/CoreUObject.Vector`, or a user struct asset path) | `UK2Node_MakeStruct` / `UK2Node_BreakStruct` |
| `select` | `options` (int, default 2) | `UK2Node_Select` |
| `switch_int` | `cases` (int array, e.g. `[0,1,2]`), `default_pin` (bool, default true) | `UK2Node_SwitchInteger` |
| `switch_enum` | `enum` (name or path) | `UK2Node_SwitchEnum` |
| `switch_string` / `switch_name` | `cases` (string array) | `UK2Node_SwitchString` / `UK2Node_SwitchName` |
| `make_array` | `inputs` (int, default 1) | `UK2Node_MakeArray` |
| `spawn_actor` | `class` (optional; sets the Class pin default) | `UK2Node_SpawnActorFromClass` |
| `reroute` | – | `UK2Node_Knot` |
| `function_entry` / `function_result` | – | **Reuses** the graph's existing entry/result node (function graphs only). A second entry node is never created |
| `local_variables` | `variables` (list of `{name,type,default}`) | Adds local variables to the function (function graphs only; not a visual node) |
| `k2node` | `class` (e.g. `K2Node_GetSubsystem`), `properties` (`{ "PropName": "UE text value" }`) | **Generic fallback**: `NewObject` of the class, `FProperty::ImportText` for each property, then `AllocateDefaultPins`. The exporter uses it for any node without a friendly kind, so round trips stay lossless |

**Deliberately not supported:**
- Timelines (they need `UTimelineTemplate` assets).
- Component-bound events.
- Input action/axis events, because they depend on the project's input settings. These are allowed through `k2node`, with a warning.
- AnimGraph and Material graphs.

The importer reports these with an explanation.

### 7.5 Pin references and names

A link end is `"<nodeId>.<pin>"`. The importer tries these in order:
1. The **exact internal `PinName`** (case-sensitive, then case-insensitive).
2. **Semantic aliases** for the node kind (table below).
3. **Normalized match:** strip spaces and `_`, lowercase, and compare against both the internal and display names (`"Loop Body"` = `LoopBody`, `"Return Value"` = `ReturnValue`).
4. **`exec` / `then` shorthand**, but only when the node has **exactly one** exec pin in that direction. Otherwise it's an error that lists the valid choices.
5. **Split struct sub-pins:** `Location_X` is resolved by splitting the parent pin (`UEdGraphSchema_K2::SplitPin`) if it isn't split yet. In 4.27, sub-pin internal names are `<Parent>_<Member>`.

A pin that matches nothing is **an error listing all visible pins of the node**, with internal and display names. That list is what lets the AI fix its own mistake.

**Internal pin names (UE 4.27, verified in engine source):**

| Node | Inputs | Outputs | Source |
|---|---|---|---|
| any impure call | `execute`, `self` (Target), params by C++ name | `then`, `ReturnValue`, out-params by name | `EdGraphSchema_K2.cpp:646-665` |
| Branch | `execute`, `Condition` | `then` (True), `else` (False) | `K2Node_IfThenElse.cpp:96-103` |
| Sequence | `execute` | `then_0`, `then_1`, … | `K2Node_ExecutionSequence.cpp:261` |
| Variable Get | (`self` if external) | `<VarName>` | – |
| Variable Set | `execute`, `<VarName>` | `then`, `Output_Get` | `K2Node_VariableSet.cpp:84-85,333` |
| Cast | `execute`, `Object` | `then`, `CastFailed`, `As<TargetDisplayName>`; a pure cast adds `bSuccess` | `K2Node_DynamicCast.cpp:22,50-74` |
| Select | `Option 0`, `Option 1`, …, `Index` | `ReturnValue` | `K2Node_Select.cpp:26-27,217-220` |
| Switch on Int | `execute`, `Selection` | `0`, `1`, …, `Default` | `K2Node_SwitchInteger.cpp:50,61`, `K2Node_Switch.cpp:18` |
| Reroute | `InputPin` | `OutputPin` | `K2Node_Knot.cpp:22-23` |
| Event / Custom event | – | `then`, params by name (`OutputDelegate` hidden) | – |
| Function entry | – | `then`, params by name | – |
| Function result | `execute`, outputs by name | – | – |
| ForEachLoop (macro) | `Exec`, `Array` | `LoopBody`, `Array Element`, `Array Index`, `Completed` | strings in `StandardMacros.uasset` |
| ForEachLoopWithBreak | `Exec`, `Array`, `Break` | `LoopBody`, `Array Element`, `Array Index`, `Completed` | ″ |
| ForLoop | `Exec`, `FirstIndex`, `LastIndex` | `LoopBody`, `Index`, `Completed` | ″ |
| IsValid (macro) | `Exec`, `InputObject` | `Is Valid`, `Is Not Valid` | ″ |
| Gate / DoOnce / WhileLoop | see the node catalog (§9.3) | | Macro pins are user-defined; dump them at runtime |

**Semantic aliases.** These are resolved through node accessor functions, so they work whatever the display names are:

| Kind | Alias → pin |
|---|---|
| branch | `true` → `then`, `false` → `else`, `condition` → `Condition` |
| cast | `object` → `Object`, `result` → `GetCastResultPin()`, `failed` → `CastFailed`, `success` → `bSuccess` |
| variable_get | `value` → the variable pin |
| variable_set | `value` → the input variable pin, `out` → `Output_Get` |
| call_function | `target` → `self`, `return` → `ReturnValue` |
| sequence | `0`, `1`, … → `then_0`, `then_1`, … |
| select | `0`, `1`, … → `Option 0`, …; `index` → `Index` |
| macro ForEach* | `body` → `LoopBody`, `element` → `Array Element`, `index` → `Array Index`, `completed` → `Completed` |

### 7.6 Declarations (only created if missing; never modified)

```json
{
  "variables": [
    {"name": "bIsAlerted", "type": "bool", "default": "false", "category": "AI", "editable": false},
    {"name": "PatrolPoints", "type": "array<vector>"}
  ],
  "functions": [
    {"name": "GetDamageMultiplier", "pure": true, "const": false,
     "inputs": [{"name": "Distance", "type": "float"}],
     "outputs": [{"name": "Multiplier", "type": "float"}]}
  ],
  "custom_events": [
    {"name": "OnAlerted", "inputs": [{"name": "Source", "type": "object:Actor"}]}
  ],
  "dispatchers": [
    {"name": "OnHealthChanged", "inputs": [{"name": "NewHealth", "type": "float"}]}
  ]
}
```

- If a member already exists with the same name and type, nothing happens and the report says `exists`.
- If it exists with the same name but a different type, that's an **error**. The existing member is never changed.
- Functions: `FBlueprintEditorUtils::CreateNewGraph` + `AddFunctionGraph`, then user pins on the entry/result nodes (`UK2Node_EditablePinBase::CreateUserDefinedPin`).
- Variables: `FBlueprintEditorUtils::AddMemberVariable` (`BlueprintEditorUtils.h:841`).

### 7.7 Type grammar (declarations, `local_variables`, event inputs)

| Category | Syntax |
|---|---|
| Basic | `bool`, `byte`, `int`, `int64`, `float`, `string`, `name`, `text` |
| Built-in structs | `vector`, `vector2d`, `rotator`, `transform`, `linearcolor`, `intpoint`, `intvector` |
| Other structs / enums | `struct:<NameOrPath>`, `enum:<NameOrPath>` |
| Object references | `object:<Class>`, `class:<Class>`, `softobject:<Class>`, `softclass:<Class>`, `interface:<Class>` |
| Containers | `array<T>`, `set<T>`, `map<K,V>` (e.g. `map<int,object:StaticMeshComponent>`) |

- Blueprint classes use the asset name without `_C` (`object:BP_Enemy`).
- Types map to `FEdGraphPinType`. `float` is `PC_Float`; 4.27 has no double.

### 7.8 Default value strings
These use the same format the K2 schema stores, and are validated with `TrySetDefaultValue`.

| Type | Example |
|---|---|
| bool | `"true"` / `"false"` |
| int/float | `"3"`, `"2.5"` |
| string/name | `"Hello"` |
| text | `"Hello"` (a localization key is generated) |
| vector | `"1.0,2.0,3.0"` |
| rotator | `"0.0,90.0,0.0"` (pitch, yaw, roll) |
| linearcolor | `"(R=1.0,G=0.0,B=0.0,A=1.0)"` |
| transform | `"0,0,0\|0,0,0\|1,1,1"` |
| enum | Enumerator name. User enums use the internal name, such as `NewEnumerator0`; the importer also accepts the display name |
| object/class | Asset path `"/Game/Path/Asset.Asset"` (uses `TrySetDefaultObject`) |

A rejected default is reported along with the format that type expects.

### 7.9 Example — event graph + function (valid JSON)

The event graph: BeginPlay → Branch on `bIsAlerted`. On True it prints "Alerted!". On False it loops over `PatrolPoints` with ForEachLoop and prints each point.

The function `GetDamageMultiplier` returns `Distance / 1000`.

```json
{
  "format": "n2c.graph",
  "version": 2,
  "engine": "4.27",
  "blueprint": "/Game/PuzzleMechanics/NPC/BP_EnemyController",
  "summary": "Alert check on BeginPlay and a damage multiplier helper",
  "declarations": {
    "variables": [
      {"name": "bIsAlerted", "type": "bool", "default": "false"},
      {"name": "PatrolPoints", "type": "array<vector>"}
    ],
    "functions": [
      {"name": "GetDamageMultiplier", "pure": true,
       "inputs": [{"name": "Distance", "type": "float"}],
       "outputs": [{"name": "Multiplier", "type": "float"}]}
    ]
  },
  "graphs": [
    {
      "name": "EventGraph",
      "kind": "event_graph",
      "nodes": [
        {"id": "begin", "kind": "event", "event": "ReceiveBeginPlay"},
        {"id": "alert", "kind": "variable_get", "variable": "bIsAlerted"},
        {"id": "br", "kind": "branch"},
        {"id": "print", "kind": "call_function", "function": "PrintString", "class": "KismetSystemLibrary",
         "defaults": {"InString": "Alerted!", "Duration": "5.0"}},
        {"id": "points", "kind": "variable_get", "variable": "PatrolPoints"},
        {"id": "loop", "kind": "macro", "macro": "ForEachLoop"},
        {"id": "tostr", "kind": "call_function", "function": "Conv_VectorToString", "class": "KismetStringLibrary"},
        {"id": "print2", "kind": "call_function", "function": "PrintString", "class": "KismetSystemLibrary"}
      ],
      "links": [
        ["begin.then", "br.execute"],
        ["alert.bIsAlerted", "br.Condition"],
        ["br.then", "print.execute"],
        ["br.else", "loop.Exec"],
        ["points.PatrolPoints", "loop.Array"],
        ["loop.LoopBody", "print2.execute"],
        ["loop.Array Element", "tostr.InVec"],
        ["tostr.ReturnValue", "print2.InString"]
      ],
      "comments": [{"text": "Alert check", "nodes": ["begin", "alert", "br", "print"]}]
    },
    {
      "name": "GetDamageMultiplier",
      "kind": "function",
      "nodes": [
        {"id": "entry", "kind": "function_entry"},
        {"id": "div", "kind": "call_function", "function": "Divide_FloatFloat", "class": "KismetMathLibrary",
         "defaults": {"B": "1000.0"}},
        {"id": "ret", "kind": "function_result"}
      ],
      "links": [
        ["entry.then", "ret.execute"],
        ["entry.Distance", "div.A"],
        ["div.ReturnValue", "ret.Multiplier"]
      ]
    }
  ]
}
```

### 7.10 How exporter output differs (annotated)

```jsonc
{
  "id": "n12",
  "kind": "call_function",
  "function": "Map_Keys",
  "class": "/Script/Engine.BlueprintMapLibrary",   // exporter always writes the full path
  "pos": [1184, 320],                              // always written
  "defaults": {},                                  // only NON-autogenerated defaults (DoesDefaultValueMatchAutogenerated == false)
  "display": "Keys"                                // informational menu title; ignored on import
}
```

With the verbose export setting enabled, the exporter also writes `"types"` next to `links`, e.g. `{"n3.TargetMap": "map<int,object:StaticMeshComponent>"}`. This helps the AI; the importer ignores it.

### 7.11 JSON Schema
Put a formal draft-07 schema at `Content/Schemas/n2c.graph.v2.schema.json` (new file). It is used for three things:
1. Documentation for AI authors.
2. Early structural validation in the importer, before any resolution.
3. Structured output for local models (Ollama `format`, LM Studio `json_schema`; see Appendix A).

Keep it permissive about extra fields (`additionalProperties: true` on nodes), so older importers don't break on newer exporter fields.

---

## 8. Importer design (plugin C++, editor module)

### 8.1 New files (suggested)
- `Source/Public/Bridge/N2CGraphDocument.h`: plain C++ structs for v2 (not USTRUCTs, to avoid reflection limits on nested maps) and `FN2CGraphDocumentParser` (JSON → structs, with line-precise errors where possible).
- `Source/Public/Bridge/N2CGraphImporter.h` / `Private/Bridge/N2CGraphImporter.cpp`: resolution, creation, wiring, layout, report.
- `Source/Public/Bridge/N2CImportReport.h`: report structs plus `ToText()` / `ToJson()`.
- `Source/Public/Bridge/N2CGraphExporterV2.h`: see §9.
- `Source/Private/Tests/N2CBridgeTests.cpp`: automation tests (§16).

### 8.2 API

```cpp
enum class EN2CImportMode : uint8 { ValidateOnly, InsertIntoGraph, CopyAsNodes };

struct FN2CImportOptions
{
    EN2CImportMode Mode = EN2CImportMode::ValidateOnly;
    FVector2D Origin = FVector2D::ZeroVector;     // paste location (SGraphEditor::GetPasteLocation)
    bool bCreateDeclarations = true;
    bool bCompileAfter = true;                    // Insert mode only
    bool bSelectImportedNodes = true;
};

struct FN2CImportResult
{
    bool bSuccess = false;                        // false if any Error entry
    FN2CImportReport Report;                      // per node / link / default / declaration entries
    FString ClipboardText;                        // CopyAsNodes mode
    TArray<UEdGraphNode*> CreatedNodes;           // Insert mode
};

class FN2CGraphImporter
{
public:
    static FN2CImportResult Import(const FString& JsonText, UBlueprint* Blueprint, UEdGraph* TargetGraph, const FN2CImportOptions& Options);
};
```

### 8.3 Algorithm

1. **Parse** into `FN2CGraphDocument`. Structural errors stop the import here; report the JSON path, e.g. `graphs[0].nodes[3].kind`.

2. **Choose the target graph.**
   - Window import uses the focused graph. Capture it **before** the N2C tab takes focus, as the existing code does at `N2CEditorIntegration.cpp:439-454`.
   - With several `graphs[]`, import each into the graph with the same name, creating function graphs from `declarations` if needed.
   - In window mode, a graph name with no match is an error.

3. **Open a transaction:** `FScopedTransaction Tx(LOCTEXT("N2CImport", "Import N2C Graph"))`, then `Blueprint->Modify(); Graph->Modify();`.

4. **Create declarations** (§7.6), then call `FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified`.
   - Compile the skeleton so the new members resolve: `FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection)`, or regenerate the skeleton class through `FBlueprintEditorUtils::...`.
   - Use the cheapest option that works, and document the choice in the code.

5. **Resolve every node** before creating anything.
   - **`call_function`**, tried in this order:
     1. The `class` hint. A path goes through `FindObject<UClass>`/`LoadObject`; a short name, with or without the `U`/`A` prefix, through `FindObject<UClass>(ANY_PACKAGE, ...)`.
     2. `Blueprint->SkeletonGeneratedClass`, so newly declared functions resolve.
     3. `Blueprint->ParentClass`.
     4. Every `UBlueprintFunctionLibrary` subclass (`TObjectIterator<UClass>`), then every other class that has the function — **but only if exactly one does**. If it's ambiguous, report an error listing the candidates.

     The function must be `FUNC_BlueprintCallable` or `FUNC_BlueprintPure`, or an event the Blueprint can call. For typos, suggest "Did you mean …" using Levenshtein distance ≤ 3 against the function names of the resolved class.

     AgentBridge uses the same resolution order (`D:/DEV/Unreal/_Plugin/Plugins/AgentBridge/Source/AgentBridgeEditor/Private/Tools/AgentBlueprintTools.cpp:530-591`). Reuse that logic, but watch for its UE5-only APIs.
   - **Variables:** look in `Blueprint->NewVariables`, then properties on `SkeletonGeneratedClass`/`ParentClass` (`FindFProperty`), then local variables (`FBlueprintEditorUtils::FindLocalVariable`), then function parameters (user pins on the entry node).
   - **Macros:** `LoadObject<UBlueprint>` of `library`, then find its `MacroGraphs` by name. Also check the Blueprint's own macro graphs.
   - **Casts, structs and enums:** by path, by short name (`FindObject` with `ANY_PACKAGE`), and by Blueprint asset name via the AssetRegistry (`BP_Enemy` → `BP_Enemy_C` through `GeneratedClass`).

6. **Create the nodes**, in the target graph or, in Copy mode, in the temporary graph (§8.5).
   - **`call_function`:** `UBlueprintFunctionNodeSpawner::Create(Function)->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), Location)` (`BlueprintFunctionNodeSpawner.h:37,54`). This picks the right `K2Node_CallFunction` subclass, exactly like the right-click menu.
   - **`variable_get`/`variable_set`:**
     ```cpp
     FGraphNodeCreator<UK2Node_VariableGet> C(*Graph);
     auto* N = C.CreateNode();
     N->VariableReference.SetSelfMember(Name); // or SetLocalMember(Name, Scope, Guid) / SetExternalMember(Name, Class)
     C.Finalize();
     ```
     See `MemberReference.h:178-195`.
   - **`macro`:** call `N->SetMacroGraph(MacroGraph)` before `Finalize` (`K2Node_MacroInstance.h:85`).
   - **`cast`:** set `TargetType` before `Finalize` (`K2Node_DynamicCast.h:22`), and call `SetPurity(bPure)` if requested.
   - **`event`:** first call `FBlueprintEditorUtils::FindOverrideForFunction(BP, Class, FuncName)` (`BlueprintEditorUtils.h:492`). If it finds a node, **reuse** it rather than creating a duplicate. Otherwise create a `UK2Node_Event` with `EventReference.SetExternalMember(FuncName, OwnerClass)` and `bOverrideFunction = true`.
   - **`custom_event`:** `CustomFunctionName` must be unique; if the name already exists, reuse that node. Add parameters with `CreateUserDefinedPin`.
   - **`function_entry` / `function_result`:** find the existing nodes (`Graph->GetNodesOfClass`). Create a result node (with `FBlueprintEditorUtils` / `FGraphNodeCreator<UK2Node_FunctionResult>`) only if none exists.
   - **`sequence`, `select`, `switch*`, `make_array`:** after creation, add pins until the count matches, using each class's add-pin API (`AddInputPin`, `AddPinToExecutionNode` or similar; check per class).
   - **`k2node`:** `NewObject<UK2Node>(Graph, Class)`, `ImportText` for each property, then `Graph->AddNode(Node, true, false)`, `Node->CreateNewGuid()`, `PostPlacedNewNode()` and `AllocateDefaultPins()`.

7. **Create the links.**
   - First all **exec** links, then the **data** links, starting with those whose source pin has a concrete type.
   - For each link, get the message from `Schema->CanCreateConnection(A,B)`, then call `Schema->TryCreateConnection(A,B)`. When the response is `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE`, this inserts a conversion node automatically; record that in the report.
   - **Retry loop:** a link that failed because of a wildcard pin (ForEachLoop `Array`, Select, MakeArray, Knot) is retried after each pass, until a pass makes no progress. Wildcards resolve through `PinConnectionListChanged` during `TryCreateConnection`.
   - Report the schema's reason text for every failure (e.g. "Integer is not compatible with String").

8. **Set defaults**, on unconnected inputs only, with `Schema->TrySetDefaultValue(*Pin, Value)` or `TrySetDefaultObject` (`EdGraphSchema_K2.h:488-489`). Then read the value back. If it changed or is empty, the value was rejected; report it.

9. **Lay out the nodes** (when there's no `pos`).
   - Layer impure nodes by their longest exec path from the roots (x = layer × 400), in first-seen order within a layer (y += 200 per branch).
   - Place each pure node to the left of its first consumer (x − 250, stacked).
   - Offset everything to `Origin`, then `SnapToGrid(16)`.
   - When `pos` is given, keep the relative positions, offset by `Origin` minus the document's minimum x/y.

10. **Comment boxes:** a `UEdGraphNode_Comment` sized to the bounding box of its listed nodes plus padding (`FEdGraphUtilities::CalculateApproximateNodeBoundaries`).

11. **Finish:**
    - **ValidateOnly:** `Tx.Cancel()` and delete the temporary graph. Nothing changes.
    - **InsertIntoGraph:**
      1. Call `MarkBlueprintAsStructurallyModified`.
      2. If `bCompileAfter` is set, compile and add the `FCompilerResultsLog` messages to the report. A compile error does **not** roll back automatically; the user decides with Ctrl+Z.
      3. Select the new nodes (`SGraphEditor::SetNodeSelection`) and zoom to them.
    - **CopyAsNodes:** see §8.5.

### 8.4 Report format
The text version is what the user copies back to Claude:

```
N2C IMPORT REPORT — 2 errors, 1 warning — target BP_EnemyController / EventGraph — mode Validate
[OK]    node begin    -> Event BeginPlay (existing node reused)
[OK]    node print    -> KismetSystemLibrary::PrintString
[ERROR] node tostr    -> function 'Conv_VectorToStrng' not found. Did you mean: Conv_VectorToString (KismetStringLibrary)?
[ERROR] link loop.ArrayElem -> print2.InString : pin 'ArrayElem' not found on 'For Each Loop'. Pins: in[Exec, Array] out[LoopBody, Array Element, Array Index, Completed]
[WARN]  default print.Duration='5' -> stored as '5.000000'
[OK]    compile: 0 errors, 0 warnings
```

The report is also available as JSON for the folder bridge and MCP: `"entries":[{"level":"error","scope":"link","ref":"loop.ArrayElem->print2.InString","message":"...","choices":[...]}]`.

### 8.5 Copy as nodes (native clipboard text)
1. Create a **temporary graph outered to the Blueprint** with `FBlueprintEditorUtils::CreateNewGraph(Blueprint, MakeUniqueObjectName(...), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass())` (`BlueprintEditorUtils.h:332`).
   - **Don't** add it to `FunctionGraphs` or `UbergraphPages`.
   - Outering it to the Blueprint is what lets self-member variables and self functions resolve (`FBlueprintEditorUtils::FindBlueprintForGraph`).
2. Run the normal import into it. Copy mode does **not** create declarations. Missing members are reported as errors that tell the user to use Insert instead, or to declare the members first.
3. Export with `FEdGraphUtilities::ExportNodesToText(TSet<UObject*>(nodes), Text)` (engine `Editor/UnrealEd/Private/EdGraphUtilities.cpp:380`), then `FPlatformApplicationMisc::ClipboardCopy`.
4. Destroy the temporary graph (`Rename(nullptr, GetTransientPackage())` + `MarkPendingKill()`) and restore the package's dirty flag (`Package->SetDirtyFlag(bWasDirty)`).
5. On paste, the engine calls `PostPasteNode()` + `ReconstructNode()` on every node (`EdGraphUtilities.cpp:207-223`) and matches old pins to new ones by `PinName` (engine `Editor/BlueprintGraph/Private/K2Node.cpp:836`). The pasted nodes are rebuilt cleanly with new GUIDs.

Pasting into a different Blueprint turns self-member variables into broken references, unless that Blueprint has the same variables. The report warns about this in Copy mode.

### 8.6 Edge cases to handle explicitly
- **Pure nodes:** a link to `execute` on a pure node is an error ("node is pure; remove exec links").
- **Latent functions** (`Delay`, `AI MoveTo` as a function) are fine in event graphs but **an error in function graphs**, the same rule as the editor.
- **Interface calls/messages:** a `call_function` whose function belongs to a `UInterface` creates a `UK2Node_Message` when the target isn't self. Resolve it through the spawner, or report it as not supported in v2.0.
- **Overriding a parent function** (as opposed to an event) is not handled by `event`. The report should tell the author to use Blueprint → Override in the editor.
- **Imports only add.** Nothing existing is deleted or rewired. A later v2.1 "replace selection" mode may delete the selected nodes first, inside the same transaction.

---

## 9. Exporter v2, Blueprint summary, node catalog

### 9.1 Graph export (new command "Copy Graph JSON (v2)")

**Scope:** the **selected nodes** if there are any, otherwise the whole focused graph. Get the selection from `FBlueprintEditor::GetSelectedNodes()` (engine `Editor/Kismet/Public/BlueprintEditor.h:321`).

**Per node**, the export contains:
- `kind` and resolvable references (full class paths; `scope` for variables);
- `pos` and `comment`;
- `defaults`: only values that are not autogenerated (`!Pin->DoesDefaultValueMatchAutogenerated()`), for every visible input pin whose value differs from the autogenerated default.

**Links:**
- Every `LinkedTo` from an **output** pin becomes `["id.PinName","id.PinName"]`, using **internal** pin names.
- Links to nodes **outside** the selection go into a separate `"external_links"` list. It is informational: it shows what the fragment connects to.
- Knots are exported as `reroute`, which is lossless. The setting "Collapse reroutes" (off by default) removes them instead.

**Other content:**
- **Node ids** are readable and stable per export: `snake_case(kind or function) + counter` (e.g. `print_string_1`). Exporting twice gives the same output.
- **Comment boxes** that contain selected nodes are exported under `comments`.
- A **`symbols` table** (§12.2.3) is written, once per referenced function, variable or struct.

**Round-trip property (acceptance test):** export → import into an empty graph of a copy of the Blueprint → export again. The two exports must have **the same set of nodes (duplicates counted) and the same set of links**. Ids and positions may differ.

### 9.2 Blueprint summary (new command "Copy Blueprint Summary")
This gives Claude the Blueprint's state without screenshots:

```json
{
  "format": "n2c.bpsummary", "version": 1,
  "blueprint": "/Game/PuzzleMechanics/NPC/BP_EnemyController",
  "parent_class": "/Script/AIModule.AIController",
  "interfaces": ["/Script/VoidLine.VoidLineCombatBridge"],
  "variables": [{"name": "TargetCell", "type": "int", "default": "0", "category": "Default", "editable": false, "replicated": false}],
  "functions": [{"name": "OnTelegraphCells", "kind": "interface_event", "inputs": [{"name": "CellIndices", "type": "array<int>"}]}],
  "dispatchers": [],
  "components": [{"name": "DefaultSceneRoot", "class": "SceneComponent", "parent": null}],
  "graphs": ["EventGraph", "OnAttackLand"]
}
```

Sources: `Blueprint->NewVariables`, `FunctionGraphs`, `DelegateSignatureGraphs`, `ImplementedInterfaces`, `SimpleConstructionScript->GetAllNodes()`.

### 9.3 Node catalog (new command "Export Node Catalog…")
This writes `Bridge/outbox/node_catalog.json` (or asks where to save it): a compact list of what the AI may call.

**What it includes:**
- Always: project classes (the `VoidLine` module, plus all `/Game` Blueprints and function libraries).
- A curated engine list: KismetSystemLibrary, KismetMathLibrary, KismetStringLibrary, KismetArrayLibrary, BlueprintMapLibrary, BlueprintSetLibrary, GameplayStatics, AIBlueprintHelperLibrary, BTFunctionLibrary, BlackboardComponent.
- Anything the user adds in the settings.

**Entry format:** `{"class":"/Script/VoidLine.VoidLineGridAttackLibrary","function":"ResolveAttackCells","pure":false,"static":true,"latent":false,"inputs":[["Pattern","object:GridAttackPattern"],...],"outputs":[["OutCellIndices","array<int>"]],"category":"VoidLine|Combat","tooltip":"..."}`.

**StandardMacros pins:** the catalog also lists the real pins of every StandardMacros macro. To get them, spawn each macro in a transient graph and dump its pins. This settles the macro pin names in §7.5 for good.

---

## 10. UI and commands

### 10.1 Blueprint editor dropdown (`N2CEditorIntegration.cpp:361-364`; commands in `N2CToolbarCommand.h/.cpp`)
Add these commands:
- **Copy Graph JSON (v2)**: uses the selection (§9.1).
- **Copy Blueprint Summary** (§9.2).
- **Import Graph JSON…**: opens the N2C window on the Import panel and records this editor as the target.
- **Export Node Catalog…** (§9.3).

Keep the three existing commands unchanged. Fix the tooltip "Translate selected Blueprint nodes", since the collector uses the whole graph (or make v1 selection-aware too, as a separate task).

### 10.2 N2C window: new "Import" panel (switcher index 4)
- A toolbar button **Import** (new, after `N2CEditorWindow.cpp:206`) switches to panel 4.
- The panel contains:
  - **Top row:** a target label ("Target: BP_EnemyController ▸ EventGraph", kept current by §10.3) and a **Load file…** button.
  - **JSON box:** a large `SMultiLineEditableTextBox` with a monospace font. `SN2CCodeEditor` is fine if you add a JSON language entry.
  - **Buttons:** **Validate**, **Insert into graph**, **Copy as nodes**, **Copy report**, **Clear**.
  - **Options:** `[x] Create missing declarations`, `[x] Compile after insert`.
  - **Report view:** read-only (`SN2CCodeEditor`, or `SMultiLineEditableTextBox` with `IsReadOnly(true)`). Color lines by level if that's cheap.
- **Accepted input:**
  - JSON wrapped in ```` ```json ```` fences or preceded by prose: extract the first top-level `{...}` that contains `"format":"n2c.graph"`.
  - Text starting with `BPC1`: show a hint that this is a bpcodec string, to be unpacked with bpcodec.py and pasted straight into the graph with Ctrl+V. Native T3D text needs no importer.
  - Text starting with `Begin Object`: offer **Paste as nodes**. Put the text on the clipboard, then call `IBlueprintEditor::PasteNodesHere` on the target editor.

### 10.3 Target tracking
- Add `TWeakPtr<FBlueprintEditor> LastActiveBlueprintEditor` to `FN2CEditorIntegration`. Update it in `HandleAssetEditorOpened` and whenever an N2C command runs from an editor.
- The Import panel reads the focused graph from it **when a button is pressed**. If the editor is gone, show an error.
- `FBlueprintEditor::GetFocusedGraph()` is in engine `Editor/Kismet/Public/BlueprintEditor.h:618`.
- For the paste location, use the focused graph editor's `SGraphEditor::GetPasteLocation()` (`GraphEditor.h:172`), or the view centre as a fallback.

### 10.4 Settings (`UN2CSettings`, category "Bridge")
- `BridgeFolder` (default `<Project>/Bridge`)
- `bWatchInbox` (default true)
- `bWriteExportsToOutbox` (default true)
- `bCompileAfterInsert`
- `bCollapseReroutesOnExport`
- `CatalogExtraClasses` (array of class paths)
- `PythonExecutable` (default `python`, for §12.2)

---

## 11. Manual LLM provider and authoring prompt

### 11.1 "Manual (copy/paste)" provider
This lets the user run N2C's existing **Translate to C++** with claude.ai, without an API key.

**Wiring**
- Add `EN2CLLMProvider::Manual` (`N2CLLMTypes.h:23-32`).
- Add `UN2CManualService : UN2CBaseLLMService`, overriding `SendRequest` (`N2CBaseLLMService.h:27-28`).
- Register it in `N2CLLMModule.cpp:495-500`, and add cases to `GetActiveApiKey`/`GetActiveModel` (`N2CSettings.cpp:64-109`).
- Add a concrete `UN2CManualResponseParser`; the base parser is `Abstract`.

**`SendRequest`**
1. Build the merged prompt exactly as the other providers do (`PromptManager->MergePrompts`, `PrependSourceFilesToUserMessage` → `"##### NODE TO CODE JSON #####\n<json>\n\n##### YOUR TASK #####\n\n<system prompt>"`).
2. Copy it with `ClipboardCopy`.
3. Store the `OnComplete` delegate.
4. Show a notification: "Prompt copied — paste it into Claude, then paste the reply into the N2C window".

**Window changes**
- When the provider is Manual, the Loading panel [1] gets a **"Paste response"** text box plus **Submit** and **Cancel** buttons.
- **Submit** calls the stored `OnComplete(PastedText)`. The existing parse → save → display flow then runs unchanged (`N2CLLMModule.cpp:119-169`).
- **Cancel** needs a new public `UN2CLLMModule::CancelPendingRequest()` that resets `CurrentStatus` to Idle. The status is private today, and new translations are refused while it is Processing (`N2CEditorIntegration.cpp:420-425`).

**Parser robustness:** strip ```` ```json ```` fences anywhere and remove `<think>…</think>` blocks, then extract the outermost JSON object that contains `"graphs"`.

**Also add "Load response from file…"**, which feeds a saved `N2C_Translation_*.json` straight into the parser.

### 11.2 Authoring prompt (new file `Content/Prompting/BlueprintGraph_Authoring.md`)
This is a system prompt that teaches any model to write v2 documents. The user pastes it once into a claude.ai Project's instructions; Claude Code gets it through the project `CLAUDE.md`.

Draft:

```text
You author Unreal Engine 4.27 Blueprint graphs as "N2C Graph v2" JSON (format "n2c.graph", version 2).
Output exactly one JSON object in a ```json fence, then a short plain-language explanation for a designer.
Rules:
- Use internal names: functions by C++ name (PrintString, not "Print String"), pins by internal PinName
  (execute/then/else/ReturnValue/Condition; Sequence then_0..; Cast result via alias "result").
- Every link is ["nodeId.OutputPin", "nodeId.InputPin"]. Exec output pins may connect to one target; data outputs may fan out.
- Declare any new member variable, function, custom event or dispatcher under "declarations".
- Only use functions that exist in UE 4.27 or in the provided node catalog / C++ headers. If unsure, say so in the explanation.
- No Timelines; use a looping Timer (SetTimerByFunctionName / custom event) instead.
- Latent nodes (Delay) only in event graphs.
- Prefer small graphs; put reusable logic into functions.
If you receive an "N2C IMPORT REPORT", fix every [ERROR] and resend the whole document.
```

The prompt file should also include the pin-name table from §7.5 and the example from §7.9.

---

## 12. C++ path

### 12.1 Primary: Claude writes C++ directly (conventions from existing VoidLine code)
The existing code in `Source/VoidLine/*` sets the house style.

**Architecture**
- Stateless logic goes in static functions on a `UBlueprintFunctionLibrary` (`UVoidLineGridSlideLibrary`, `UVoidLineGridAttackLibrary`).
- Designer data goes in a `UDataAsset` (`UGridAttackPattern`).
- AI timing goes in Behavior Tree tasks (`UBTTask_TelegraphGridAttack`).
- **Callbacks into Blueprint** go through a `UINTERFACE` whose functions are `BlueprintNativeEvent, BlueprintCallable` and implemented in Blueprint. C++ checks `ImplementsInterface`, then calls `IVoidLineCombatBridge::Execute_OnTelegraphCells(Obj, ...)` (`VoidLineCombatBridge.h`).

**Declarations**
- `VOIDLINE_API` on every class.
- `Category = "VoidLine|<Domain>"`.
- `/** */` doc comments with `@param`; they become node tooltips.
- `meta = (AutoCreateRefTerm = "...")` for const-ref struct and array parameters.
- Out parameters are non-const references named `Out...`.

**File layout and style**
- Each header starts with a design comment (PURPOSE / CONVENTION / EXTENSIBILITY).
- Include order: `CoreMinimal.h` → engine headers → `.generated.h` last.
- Tabs; braces on their own lines.

**Logging:** `UE_LOG(LogTemp, ...)` with a `[Tag]` prefix, switched by a `bEnableLogging` parameter. For new systems, a proper `DEFINE_LOG_CATEGORY` is recommended.

**Grid convention:** column-major, `index = column * GridRes_X + row`.

Every C++ feature must come with:
1. The `.h/.cpp` files. In Claude Code, build them with §3.2; in chat, include build instructions.
2. Any `Build.cs` module changes.
3. **A v2 usage graph** that shows the Blueprint side, so the user can import ready-wired nodes. For example: the event that calls `ResolveAttackCells` and loops over the result.
4. **Plain-language steps** for anything the importer can't do: reparenting a Blueprint, adding an interface in Class Settings, creating a Data Asset, assigning it in a Behavior Tree.

**Blueprint-owned state that C++ needs.** Options, in order of preference:
- Pass it in as parameters.
- Move it into a C++ base class and reparent the Blueprint. This risks losing variable values, so warn the user and only do it with their agreement.
- Read and write it through an interface.

Never use reflection string lookups (`FindFProperty` by name) in game code.

### 12.2 Secondary: deterministic JSON → C++ generator

#### 12.2.1 Purpose and limits
The generator gives an **offline, repeatable** way to move a pure Blueprint function into C++ without any AI. It is **not** a general Blueprint-to-C++ transpiler. UE 4.27's built-in Blueprint Nativization produces unreadable machine code, so it isn't an option.

**Supported in v1 of the generator:**
- Function graphs only.
- Exec flow: the entry node and chains from it, `branch`, `sequence`, the macros ForLoop / ForEachLoop / ForEachLoopWithBreak / WhileLoop, and `function_result` (return).
- Local variables and parameters, and `variable_get`/`variable_set` on self members (which become function parameters).
- Static library function calls → `UClass::Func(...)`; member function calls on object pins → `Target->Func(...)`.
- Pure nodes become expressions. Each is evaluated into a `const auto` temporary **once per impure node that consumes it**, which matches Blueprint re-evaluation semantics. Say so in the generated comments.
- `cast` → `Cast<T>(Obj)` plus an `if`.
- `make_struct`/`break_struct` for native structs, and `select`.
- `switch_int`/`switch_enum` → `switch`.

**Rejected with a clear report entry:** latent nodes, timelines, delegates/dispatchers, event graphs, interface messages, `k2node` fallback nodes, and user macros other than the standard loops.

#### 12.2.2 Output shape
```cpp
// <Name>Library.h  — generated by n2c_cpp_gen.py from <BP>/<Graph>; review before use
UCLASS()
class VOIDLINE_API U<Name>Library : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /** <graph comment / summary> */
    UFUNCTION(BlueprintCallable /*or BlueprintPure*/, Category = "VoidLine|Generated")
    static <ret> <Graph>(<params incl. former self-variables as (Out)params>);
};
```

The generator also writes:
- `<Name>_GenerationReport.txt`: unsupported nodes, assumptions, and which self-variables became parameters.
- A **v2 usage graph** that replaces the original Blueprint call.

#### 12.2.3 `symbols` table (written by the exporter, read by the generator)
```json
{
  "functions": {
    "/Script/Engine.KismetMathLibrary:Divide_FloatFloat": {
      "cpp_owner": "UKismetMathLibrary", "include": "Kismet/KismetMathLibrary.h", "static": true, "pure": true,
      "params": [{"name": "A", "cpp_type": "float", "dir": "in"}, {"name": "B", "cpp_type": "float", "dir": "in"}],
      "return": "float", "world_context": null
    }
  },
  "variables": {"self:PatrolPoints": {"cpp_type": "TArray<FVector>"}},
  "structs": {"/Script/CoreUObject.IntVector": {"cpp_type": "FIntVector", "include": "Math/IntVector.h"}}
}
```

Where each value comes from:
- `cpp_type`: `FProperty::GetCPPType()`, plus `GetCPPTypeForwardDeclaration` and the template arguments for containers.
- `include`: the class metadata `IncludePath` (`Class->GetMetaData(TEXT("IncludePath"))`, editor only).
- `dir`: `CPF_OutParm` / `CPF_ReferenceParm`.
- `world_context`: the function's `WorldContext` meta.

The generator never guesses C++ types. A missing symbol is a report error.

#### 12.2.4 Implementation
- **Script:** new file `Content/Python/n2c_cpp_gen.py`.
  - Standard library only, and **Python 3.7-compatible syntax**, so it can also run in UE 4.27's embedded Python 3.7 if the PythonScriptPlugin is ever enabled.
  - Usage: `python n2c_cpp_gen.py graph.json --graph <name> --class <Name>Library --out <dir>`.
  - Tests go in `Content/Python/tests/` and must run with both pytest and `python -m unittest`.
- **Plugin button "Generate C++ (offline)"**, in both the Import panel and the Results panel:
  1. Save the current v2 export and its symbols to a temp file.
  2. Run `FPlatformProcess::ExecProcess(PythonExecutable, args, &RetCode, &StdOut, &StdErr)`.
  3. Load the generated files into the Declaration/Implementation panes by broadcasting a synthetic `FN2CTranslationResponse` through `OnTranslationResponseReceivedNative`.
  4. Save them via `SaveTranslationToDisk`.
  5. Show the generation report in the Notes section.

---

## 13. Folder bridge and project `CLAUDE.md` (for Claude Code)

### 13.1 Folder bridge
The folder is `VoidLine427/Bridge/`; add `Bridge/` to the game's `.gitignore`.

```
Bridge/
  outbox/   plugin → Claude : graph_<BP>_<Graph>_<ts>.n2cgraph.json, summary_<BP>.json, node_catalog.json, report_<ts>.txt/.json
  inbox/    Claude → plugin : *.n2cgraph.json   (optional sidecar *.md with instructions for the user)
  archive/  imported inbox files are moved here with a timestamp
```

- **Watching the inbox:** `FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>("DirectoryWatcher").Get()->RegisterDirectoryChangedCallback_Handle(...)` (engine `Developer/DirectoryWatcher/Public/IDirectoryWatcher.h`).
  - Debounce changes by 500 ms.
  - Ignore files that are still being written: retry reading until the JSON parses, for at most 5 s.
- **Toast:** an `FNotificationInfo` with the buttons **Import into focused graph**, **Copy as nodes**, **Open in N2C** and **Dismiss**.
- **After an import:** write the report to `outbox/report_<ts>.*` and move the inbox file to `archive/`.
- **Exports:** every "Copy …" command also writes its file to `outbox/` when `bWriteExportsToOutbox` is set.

### 13.2 Draft `D:/DEV/Unreal/VoidLine/VoidLine427/CLAUDE.md` (create in P5)
The file should cover:
- **The project:** UE 4.27, Blueprint-first, C++ module `VoidLine`. The user is not a C++ programmer, so always explain the Blueprint-side steps.
- **The bridge:**
  - where the exports are;
  - how to write graphs (the `BlueprintGraph_Authoring.md` rules and the pin-name table in §7.5);
  - read `outbox/report_*` after the user imports.
- **C++ conventions** from §12.1.
- **Building:** the commands in §3.2, when to ask the user to close the editor, and the limits of Live Coding.
- **Don't** edit `.uasset` files, commit, or touch `Plugins/` unless asked.

---

## 14. Future options

### 14.1 MCP (the smoothest option; a later phase)
- **Current state:**
  - `C:/Users/SethPDA/.claude.json` already defines `"unreal": {"type":"http","url":"http://127.0.0.1:8791/mcp"}`. Connections are refused because nothing listens on that port in 4.27.
  - The user's **AgentBridge** plugin (`D:/DEV/Unreal/_Plugin/Plugins/AgentBridge`) implements this server, but only for UE 5.5+. It uses `EAllowShrinking`, `PC_Real`/`PC_Double` and `FHttpServerRequest::PeerAddress`, none of which exist in 4.27.
- **Option A (recommended): backport AgentBridge to 4.27.**
  - 4.27 has the `HTTPServer` module (`IHttpRouter::BindRoute`) but no `PeerAddress`. Keep localhost-only binding and token auth, and drop the peer check.
  - Add **batch** tools that reuse the N2C importer and exporter: `blueprint.export_graph` (v2), `blueprint.import_graph` (a v2 document plus a mode), `blueprint.summary` and `blueprint.node_catalog`.
  - Claude Code could then read and write graphs directly, without the clipboard.
- **Option B: a slim MCP endpoint inside NodeToCode itself** (HTTP JSON-RPC on 127.0.0.1:8791), exposing only those four tools plus `blueprint.compile`.
- Either way, the v2 document **is** the tool payload, so there's no second format to maintain.

### 14.2 bpcodec (T3D) path — works today without plugin changes
The Claude desktop skill `workflow-bp` packs native clipboard text (Ctrl+C in a graph) into `BPC1…` strings.
- **Best for small edits to existing graphs in chat.** The user copies nodes and packs them; Claude edits the T3D and repacks it; the user unpacks it and presses Ctrl+V.
- **Weaker for new graphs.** The AI must invent GUIDs and exact pin data, and mistakes only show up silently at paste time. The v2 importer is the reliable path for new logic.
- **Optional later:** N2C commands "Copy selected as BPC1" and "Paste BPC1" that shell out to `bpcodec.py` rather than reimplementing the codec.

### 14.3 Data assets
The user's `D:/DEV/Unreal/VoidLine/Other/voidline_pattern_designer.py` exports `UGridAttackPattern` data as JSON, but nothing imports it yet. A small, generic editor command, "Import Data Asset JSON", would close that gap: a class path plus property values, applied with `FJsonObjectConverter::JsonObjectToUStruct` on the asset. This is a separate small task.

---

## 15. Phased roadmap
Each phase must build and be testable on its own, and ends with a sync (not a commit).

| Phase | Scope | Acceptance criteria |
|---|---|---|
| **P0 Setup** | Test module skeleton (`N2CBridgeTests.cpp`, filter `NodeToCode.Bridge`); add `JsonUtilities` to `Build.cs`; new fixtures folder `Source/Private/Tests/Fixtures/` containing the §7.9 example | The project builds (§3.2), and the §3.3 command runs one passing smoke test |
| **P1 Exporter v2 + summary + catalog** | §9 (selection-aware export, symbols, reroutes, comments, `external_links`) and the §10.1 commands except Import | Exporting `BP_Grid_Revealed` ▸ `GenerateBoxLocation_FOR_BREAK` meets all of:<br>• every link is pin-level, with internal names<br>• the link count equals the number of `LinkedTo` pairs in the graph (counted by a test)<br>• Branch and Loop outputs can be told apart<br>• map value types are present<br>• the catalog lists the real StandardMacros pins |
| **P2 Importer core** | §8.1–8.4 and §8.6; declarations; ValidateOnly and InsertIntoGraph modes; report.<br>Kinds: event, custom_event, call_function, variable_get/set (self/local/param), self, branch, sequence, macro, cast, make/break_struct, select, function_entry/result, reroute, k2node | (a) The §7.9 example imports into a test Blueprint with 0 errors and **compiles**.<br>(b) Round trip: for each function graph of `BP_Grid_Revealed` (on a **duplicated** test asset, never the original), export → clear the graph → import → export gives the same nodes and links.<br>(c) Typos produce the "did you mean" and pin-list errors shown in §8.4.<br>(d) One Ctrl+Z removes the whole import |
| **P3 Import panel, Copy as nodes, authoring prompt** | §10.2–10.3, §8.5, §11.2, and the JSON Schema (§7.11) | • A document pasted into the panel → **Copy as nodes** → Ctrl+V in another graph of the same Blueprint produces identical nodes<br>• **Validate** changes nothing (the package is not dirtied)<br>• JSON wrapped in fences or prose is accepted |
| **P4 Manual provider** | §11.1, including Cancel and "Load response from file" | • With `Provider=Manual`, a full Translate round trip through claude.ai produces the usual folder in `Saved/NodeToCode/Translations/` and shows the code<br>• Cancel returns to Idle |
| **P5 Folder bridge + CLAUDE.md** | §13 and the §10.4 settings | • Claude Code writes `Bridge/inbox/test.n2cgraph.json` and a toast appears within 2 s<br>• Import works<br>• the report lands in `outbox/` and the file moves to `archive/`<br>• `Bridge/` is git-ignored |
| **P6 Offline C++ generator** | §12.2 | • Unit tests pass (`python -m unittest discover Content/Python/tests`)<br>• A pure-math function exported from a test Blueprint generates a library that **compiles** in VoidLine and returns the same results as the Blueprint (an automation test calls both)<br>• Unsupported nodes are listed in the report |
| **P7 (optional) MCP** | §14.1 | • With the 4.27 editor open, `claude mcp list` shows `unreal` connected<br>• `blueprint.import_graph` works in both dry-run and real modes |
| **P8 (optional) Local model** | Appendix A | Only with the user's explicit go-ahead |

Fix the bugs in §18 along the way, during P1/P2, when you're touching the same code anyway.

---

## 16. Testing and verification

**Automation tests.** Declare them as `IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CBridge..., "NodeToCode.Bridge.<Name>", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)` and run them with the §3.3 command. Cover:
- **Parser:** valid and invalid documents, and the error paths they report.
- **Resolver:** function resolution order, ambiguous names, "did you mean" suggestions.
- **Importer:** against a **transient test Blueprint** created inside the test (`FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), GetTransientPackage(), ...)`), so no assets are touched.
- **Round trips:** exporter ⇄ importer, both on the transient Blueprint and on a **duplicate** of `BP_Grid_Revealed` loaded from `/Game/PuzzleMechanics/Grid/`.
- **Copy as nodes:** `ExportNodesToText` → `ImportNodesFromText` into another transient graph gives the same structure.
- **Compile check:** `FKismetEditorUtilities::CompileBlueprint` with an `FCompilerResultsLog` reports 0 errors.

**Python:** unittest for the generator, with golden files in `Content/Python/tests/golden/`.

**Manual checklist (every phase):**
1. Open VoidLine.
2. Run Copy Graph JSON (v2) on a selection.
3. Paste the §7.9 example into the Import panel.
4. Validate.
5. Insert.
6. Ctrl+Z.
7. Copy as nodes, then paste into another graph.
8. Compile.

**Never modify real project assets in tests.** Use transient packages or duplicates under `/Game/__N2CTest/`, and have the test delete them.

---

## 17. Risks and mitigations

| Risk | Mitigation |
|---|---|
| The AI invents functions or pins | Resolver with "did you mean" and pin lists in the report; the node catalog (§9.3) as context; the report is fed back to the AI |
| Wildcard pins resolve in the wrong order | Multi-pass link retry (§8.3 step 7); tests with ForEachLoop, Select and MakeArray |
| Duplicate override events or custom event names | Reuse the existing nodes (`FindOverrideForFunction`) and report them as "reused" |
| An import corrupts a Blueprint | One transaction (one Ctrl+Z); ValidateOnly is the default; declarations only add, never change; compile report; tests use transient Blueprints |
| Copy as nodes leaves junk in the asset | The temporary graph is never registered in the Blueprint and is destroyed afterwards; the dirty flag is restored; a test checks the package isn't dirty |
| Default value formats differ by type | `TrySetDefaultValue` plus a read-back check; the §7.8 table goes in the authoring prompt |
| UE5 knowledge leaks into 4.27 code | Follow §3.5; verify in `C:/UE/UE_4.27/Engine/Source`; build after every change |
| The two plugin copies drift apart | One working copy (in-project), synced with §3.4 at the end of each phase; check with `diff -rq --strip-trailing-cr` |
| The deterministic generator produces Blueprint-shaped C++ | Treat it as a starting point; the primary path is Claude-written C++ (§12.1) |

---

## 18. Existing N2C bugs found during analysis
Fix these when you're touching the same code. Each one is a small, separate change.

1. **`TranslationDepth` ≥ 1 never stops recursing.** Graphs are queued with `ParentDepth = CurrentDepth`, and processing sets `CurrentDepth = GraphInfo.ParentDepth` (`N2CNodeTranslator.cpp:143,350`), so the depth never goes up.
2. **MakeMap key and value types are swapped** (`N2CArrayProcessor.cpp:36-72`).
3. **Unreachable processor branches**, caused by the order of the `IsA` checks:
   - CustomEvent/ActorBound/ComponentBound events (`N2CEventProcessor.cpp:24-73`) are never reached, so custom events export `member_name: "None"`.
   - The CallDelegate branch is never reached (`N2CDelegateProcessor.cpp:54-72`).
   - MultiGate is named "Sequence" (`N2CFlowControlProcessor.cpp:67-87`).
   - SetFieldsInStruct is named "Make X" (`N2CStructProcessor.cpp:82-108`).
   - BreakStruct and StructMember* are exported as `Variable`.
4. **The "No active LLM service" path leaves the status at Processing** (`N2CLLMModule.cpp:84-89`).
5. **Copy Header / Copy Code copy the cached response**, not the text as edited in the panes (`N2CEditorWindow.cpp:554,572`).
6. **User structs with Object or Class members are dropped.** `TypeName` is never set for those members, but the validator requires it (`N2CBlueprintValidator.cpp:278-288`).
7. **Enum values containing "max" (case-insensitive) are dropped** (`N2CNodeTranslator.cpp:1043-1056`).
8. **The Anthropic model id `claude-haiku-4-5-20241022` is wrong** (`N2CLLMModels.cpp:71-90`). Check the current model ids.
9. **`CodeGen_CPP.md` describes a different v1 format than the serializer writes** (metadata keys, `data` arrays, chains). It also contradicts itself: "object" at line 248 vs "array" at line 312.
10. **The Ollama payload carries leftover top-level `temperature` and `max_tokens`**, because `Initialize()` runs before `ConfigureForOllama` (`N2CLLMPayloadBuilder.cpp:18-19`).
11. **The Blueprint name lookup via `FocusedGraph->GetOuter()` fails for collapsed graphs.** Use `FBlueprintEditorUtils::FindBlueprintForGraph` instead.
12. **Copy Blueprint JSON overwrites the translator's stored Blueprint while a translation is pending**, so that translation is saved under the wrong Blueprint (`N2CLLMModule.cpp:136`).
13. **`FN2CSerializer::FromJson` drops exec pins** (it requires `type`) and never parses structs or enums. Fix it or delete it; v2 uses its own parser.

---

## Appendix A — Local model: evaluation and training recipe

### A.1 Recommendation
Don't make a fine-tuned local model part of the core workflow:
- **The conversions don't need a model.** JSON ⇄ Blueprint and the C++ subset are deterministic code (§8, §12.2).
- **The remaining job is hard.** Turning a feature request into a correct graph or idiomatic UE C++ needs broad engine knowledge and reasoning. A 7–14B model fine-tuned on a few thousand examples will stay well below Claude. The user already found a Qwen3-14B "Claude distill" in LM Studio not good enough, and that was for the easier v1 translation task.
- **The hardware is limiting.** The RTX 2080 Ti has **11 GB and is a Turing card**: no bf16 and no FlashAttention 2. QLoRA is practical up to about **7–8B** parameters with a 2–4k context. 14B QLoRA needs roughly 16–24 GB, so a rented GPU or Colab.
- **The real cost is data.** It takes thousands of high-quality pairs of (request → v2 graph / C++).

### A.2 If they want to try anyway, in this order

**Step 1 — try without training.**
- Use a current open-weight instruct coder model in the 7–14B range (for example the Qwen-Coder family; check what's current) at Q4_K_M/Q5_K_M in Ollama or LM Studio.
- Give it the §11.2 authoring prompt, the §7.9 example and the relevant slice of the node catalog as context.
- Use **schema-constrained decoding**: Ollama `format: <n2c.graph.v2.schema.json>` or LM Studio `response_format: json_schema`. The JSON is then always syntactically valid.

**Step 2 — build the evaluation harness before any training.**
- `N2C Import` in ValidateOnly mode works as an **automatic grader**. Measure: % valid documents, % nodes resolved, % links created, % compile success.
- Build 50–100 held-out tasks written by the user (e.g. "on BeginPlay, if X then Y").
- Compare models or prompts on the same set. Only train if the measured gap justifies it.

**Step 3 — build the dataset.**
- **Inputs:** Blueprints exported with the v2 exporter through a new **batch export commandlet** (`UN2CExportCommandlet`: loads every Blueprint under a path and writes v2 per graph). Sources:
  - the user's own projects under `D:/DEV/Unreal/*` (VoidLine, StealthMegamanGame, AdvancedTurnBasedTileTool, GridIndexing, …; 4.27 or compatible);
  - the UE 4.27 template content in `C:/UE/UE_4.27/Templates`;
  - the 4.27 Content Examples project (Epic launcher → Learn tab);
  - marketplace packs they own — check each license for machine-learning use.
- **Targets:** the exported graph **is** the target.
- **Instructions** (what the graph does) must be written by hand, or generated by a model whose license allows its outputs to be used to train other models.
  - **Anthropic's terms restrict using Claude outputs to develop or train AI models.** Don't generate training data with Claude without checking those terms first.
  - Check the terms of any other model you use for this too.
- **Augmentation:** rename variables, reorder independent nodes, and split or merge functions. Re-check every sample with the importer; it must compile.
- **Format:** chat JSONL, `{"messages":[{"role":"system",...},{"role":"user","content":"<request + context>"},{"role":"assistant","content":"<v2 json>"}]}`. Hold out 10% for testing.

**Step 4 — train** (WSL2 Ubuntu, already installed).
- **Environment:**
  - the NVIDIA CUDA driver for WSL;
  - Python 3.11 in a venv or conda;
  - PyTorch with CUDA;
  - **Unsloth**, which supports Turing through fp16 plus xformers/SDPA.
- **Base model:** a 7–8B instruct coder model.
- **QLoRA settings:**
  - 4-bit, r=16, alpha=16, dropout 0;
  - learning rate 2e-4 with a cosine schedule, 2–3 epochs;
  - `max_seq_length` 4096; batch size 1–2 with gradient accumulation 8;
  - `fp16=True`, `bf16=False`;
  - train on assistant tokens only.
- **Export:** merge the adapter, convert to **GGUF** (Unsloth `save_pretrained_gguf` or llama.cpp `convert_hf_to_gguf.py`), and quantize to Q4_K_M/Q5_K_M.
- **Serve with Ollama:** write a `Modelfile` (`FROM ./model.gguf`, the §11.2 system prompt, `PARAMETER num_ctx 8192`), then run `ollama create n2c-author -f Modelfile`.
- **Connect to N2C:** set Provider=Ollama and `OllamaModel=n2c-author`. For graph authoring, call Ollama directly with schema-constrained output, or add a small "Author graph" action later.

**Step 5 — evaluate** with the Step 2 harness. Keep the model only if it clearly beats Step 1.

---

## Appendix B — Side findings in VoidLine C++
These are out of scope. Tell the user about them; don't fix them without asking.

- **`UBTTask_TelegraphGridAttack`:**
  - It keeps per-run state (`TelegraphTimer`, `ResolvedCells`) on the node without `bCreateNodeInstance = true`, so enemies sharing `BT_Enemy` overwrite each other's state.
  - It has no `AbortTask` override to clear the timer.
  - The header calls `GridResolutionKey` an IntVector, but it's a Vector key.
  - It only calls the interface on the **AIController**, although the header suggests the pawn.
  - `bSnapEnemyToCell` is never used.
- **`VoidLineGridSlideLibrary`:** the header says `Up = -1 / Down = +1`, but the `.cpp` uses `Up: +1`.
- **No `UGridAttackPattern` assets exist yet**, so the BT task's `Pattern` is probably unset (inferred from an asset search).
- **`MyClass.h/.cpp`** is an unused leftover from the class wizard.

---

## Appendix C — Key file index

**Plugin** (`Plugins/NodeToCodeUE4-main/`):
- `Source/Private/Core/N2CEditorIntegration.cpp`:
  - toolbar/menu registration: `RegisterToolbarForEditor` 232-383
  - `ExecuteCopyJsonForEditor` 36-129
  - `ExecuteCollectNodesForEditor` 417-542
  - target-editor stub 175-180
- `Source/Private/Core/N2CToolbarCommand.cpp`: `UI_COMMAND`s 35-57
- `Source/Private/Core/N2CEditorWindow.cpp`: `Construct` 101-384, response handling 425-536, copy buttons 550-584
- `Source/Private/Core/N2CSerializer.cpp`: `ToJson` 11-53, `FromJson` 55-68, flows 278-308
- `Source/Private/Core/N2CNodeTranslator.cpp`: IDs 156-164, pins 727-808, flows 900-979, depth bug 143/350
- `Source/Private/LLM/N2CLLMModule.cpp`: `ProcessN2CJson` 66-170, `SaveTranslationToDisk` 216-428, provider registry 489-501
- `Source/Public/LLM/N2CBaseLLMService.h`: `SendRequest` 27-28
- `Source/Private/LLM/N2CResponseParserBase.cpp`: response contract 12-151
- `Source/Public/Core/N2CSettings.h`: settings 368-654

**Engine** (`C:/UE/UE_4.27/Engine/Source/`):
- `Editor/UnrealEd/Private/EdGraphUtilities.cpp`: `PostProcessPastedNodes` 207, `ExportNodesToText` 380, `ImportNodesFromText` 406
- `Editor/BlueprintGraph/Private/K2Node.cpp:836`: `DoPinsMatchForReconstruction`
- `Editor/BlueprintGraph/Private/EdGraphSchema_K2.cpp:646-665`: pin name constants
- `Editor/BlueprintGraph/Classes/BlueprintFunctionNodeSpawner.h:37,54`
- `Runtime/Engine/Classes/Engine/MemberReference.h:178-195`
- `Editor/UnrealEd/Public/Kismet2/BlueprintEditorUtils.h`: `MarkBlueprintAsStructurallyModified` 308, `CreateNewGraph` 332, `AddFunctionGraph` 393, `FindOverrideForFunction` 492, `AddMemberVariable` 841
- `Editor/BlueprintGraph/Classes/EdGraphSchema_K2.h`: `TrySetDefaultValue`/`TrySetDefaultObject` 488-489, `SplitPin` 522
- `Editor/Kismet/Public/BlueprintEditor.h`: `GetSelectedNodes` 321, `GetFocusedGraph` 618
- `Runtime/Launch/Private/LaunchEngineLoop.cpp:1588`: `-TestExit`
- `Developer/AutomationController/Private/AutomationCommandline.cpp:555`: `RunTests`

**AgentBridge** (`D:/DEV/Unreal/_Plugin/Plugins/AgentBridge/`):
- `Source/AgentBridgeEditor/Private/Tools/AgentBlueprintTools.cpp`: pin lookup 121-162, function resolution 530-591, node creation 645-748, connect 855-877
- `Docs/ToolReference.md`
