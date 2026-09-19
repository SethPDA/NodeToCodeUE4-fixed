# NodeToCode UE4.27 Port

UE4.27.2 port of the NodeToCode plugin (originally UE5.3+). Translates Blueprint graphs to code via LLM providers.

## Project Context

- **Engine:** Unreal Engine 4.27 (installed at `C:\UE\UE_4.27`)
- **Test project:** `D:\DEV\Unreal\VoidLine\VoidLine427\VoidLine.uproject` (C++ module `VoidLine`, editor target `VoidLineEditor`)
- **Toolchain:** Visual Studio 2019 Community
- **Platform:** Windows 11

## Key Paths

| Path | Purpose |
|------|---------|
| `D:\DEV\Unreal\VoidLine\VoidLine427\Plugins\NodeToCodeUE4-main` | Git repo root — this folder is both the working copy compiled by VoidLine and the plugin's own git checkout (remote `github.com/SethPDA/NodeToCodeUE4-fixed`, branch `main`) |

No junction or worktree is needed: the plugin lives directly under the project's `Plugins/` folder and compiles together with the `VoidLine` module.

## Build Commands

Always close the UE4 editor before building. Live Coding blocks CLI builds.

```bash
# Build
"C:/UE/UE_4.27/Engine/Build/BatchFiles/Build.bat" VoidLineEditor Win64 Development -Project="D:/DEV/Unreal/VoidLine/VoidLine427/VoidLine.uproject" -WaitMutex

# Generate project files (after adding/removing source files or changing Build.cs)
"C:/UE/UE_4.27/Engine/Binaries/DotNET/UnrealBuildTool.exe" -projectfiles -project="D:/DEV/Unreal/VoidLine/VoidLine427/VoidLine.uproject" -game -engine -progress
```

## Automated tests

```bash
"C:/UE/UE_4.27/Engine/Binaries/Win64/UE4Editor-Cmd.exe" "D:/DEV/Unreal/VoidLine/VoidLine427/VoidLine.uproject" -ExecCmds="Automation RunTests NodeToCode;Quit" -TestExit="Automation Test Queue Empty" -unattended -nopause -nullrhi -log
```

Always commit (and push, when asked) before ending a session.

## Architecture

Pipeline: **Collect -> Translate -> Serialize -> LLM -> Parse -> Display**

- `FN2CNodeCollector` - Extracts UK2Nodes from Blueprint graphs
- `FN2CNodeTranslator` - Converts to FN2CBlueprint via 12 specialized processors
- `FN2CSerializer` - JSON serialization (compact, token-efficient)
- `UN2CLLMModule` - Orchestrates LLM request/response (6 providers)
- `SN2CEditorWindow` - Results display (full Slate UI with code editors, loading states, graph switching)

See `docs/PROJECT_OVERVIEW.md` for full architecture details.

## Blueprint <-> C++ Bridge (N2C Graph v2)

A second, newer pipeline (separate from the v1 architecture above) lets an AI chat (e.g. claude.ai, no API key) read a Blueprint graph as JSON and write real, pasteable Blueprint nodes back. Spec: `docs/BLUEPRINT_CPP_BRIDGE.md` (~83KB; §15 has the phase table, which is the source of truth for scope/status — re-read it and diff against `git log` before resuming work, since it goes stale fast).

**All new code lives under `Source/{Public,Private}/Bridge/`** and `Source/Private/Tests/N2CBridgeTests.cpp` — the v1 files above are untouched by this work.

- **Status:** P0-P3 done (test scaffolding, v2 exporter + Blueprint summary + node catalog, v2 importer core, Import panel + Copy-as-nodes + authoring prompt). 12/12 automation tests passing (`NodeToCode.Bridge.*`, see Automated tests command above). Commits `d84a4c5`..`f4e8895`. P4 (manual LLM provider), P5 (folder bridge), P6+ (offline C++ generator, MCP, local model) are next/further out.
- Key classes: `FN2CGraphExporterV2`, `FN2CBlueprintSummaryExporter`, `FN2CNodeCatalogExporter`, `FN2CTypeStringConverter`, `FN2CGraphDocument`/`FN2CGraphDocumentParser`, `FN2CImportReport`, `FN2CGraphImporter`.
- **Lesson learned the hard way:** `FScopedTransaction::Cancel()` does NOT revert already-applied mutations — it only drops the transaction from the undo history (`UTransBuffer::Cancel()` in engine source just pops the undo buffer, no `Apply(Undo)`). To genuinely revert, let the transaction commit (destroy/reset it) and then call `GEditor->UndoTransaction()`. This is why `EN2CImportMode::ValidateOnly` uses that pattern instead of `Cancel()`.

## Content Folder (prompting assets + schema, packaged with the plugin)

These ship inside `Content/` so they're available at runtime/in-editor, not just as dev docs:

| Path | Purpose |
|------|---------|
| `Content/Prompting/CodeGen_CPP.md` | v1 pipeline: system prompt for Blueprint -> C++ translation |
| `Content/Prompting/CodeGen_CSharp.md` | v1 pipeline: system prompt for Blueprint -> C# (Unity) translation |
| `Content/Prompting/CodeGen_JavaScript.md` | v1 pipeline: system prompt for Blueprint -> JavaScript translation |
| `Content/Prompting/CodeGen_Python.md` | v1 pipeline: system prompt for Blueprint -> Python translation |
| `Content/Prompting/CodeGen_Swift.md` | v1 pipeline: system prompt for Blueprint -> Swift translation |
| `Content/Prompting/CodeGen_Pseudocode.md` | v1 pipeline: system prompt for Blueprint -> CalPoly-style pseudocode |
| `Content/Prompting/BlueprintGraph_Authoring.md` | **v2 bridge**: the system prompt an AI chat uses to *author* N2C Graph v2 JSON — internal pin-name tables, semantic aliases, the full type grammar, and a worked example (docs §7.9). This is what you paste into claude.ai (or load via the future folder bridge) to have it write Blueprint graphs back. |
| `Content/Schemas/n2c.graph.v2.schema.json` | JSON Schema (draft-07, permissive `additionalProperties`) for the N2C Graph v2 document format that `FN2CGraphDocumentParser`/`FN2CGraphImporter` consume |

When the bridge work adds a new authoring/system prompt or schema, it belongs under `Content/Prompting/` or `Content/Schemas/` respectively, and should be listed here.

## UE4.27 Port Rules

When writing new code or modifying existing code, follow these UE4.27 constraints:

- Use `FEditorStyle` not `FAppStyle`
- Use `FSyntaxTokenizer` not `ISyntaxTokenizer`
- Use `ESPMode::ThreadSafe` explicitly on `TSharedRef<IHttpRequest>`
- Use `GetPathName()` not `GetStructPathName()` on UScriptStruct
- Use `operator->` on TScriptInterface, not `.GetInterface()->`
- `DECLARE_DYNAMIC_MULTICAST_DELEGATE` requires UObject+UFUNCTION for binding; use parallel native `DECLARE_MULTICAST_DELEGATE` for Slate widget binding via `AddRaw()`
- `TryGetNumberField` expects `double&` not `float&`
- No `PC_Double` or `PC_Real` pin types (only `PC_Float`)
- No `UK2Node_ExternalGraphInterface` or `UK2Node_PromotableOperator`
- `SMultiLineEditableText` lacks: `DeleteSelectedText`, `GetCursorLocation`, `SelectText`, `GetSelection`
- Include explicitly: `Misc/FileHelper.h`, `Internationalization/Regex.h`, `Policies/CondensedJsonPrintPolicy.h`, `Widgets/Layout/SGridPanel.h`
- Asset editor events: use `UAssetEditorSubsystem` via `GEditor->GetEditorSubsystem<>()`, not `FAssetEditorManager`

See `docs/UE4_PORT_GUIDE.md` for the complete API change reference.

## Remaining Work

- **Toolbar icon**: Registered but may not render on all configurations. Investigate if not visible.
- **Code editor stubs**: `SN2CCodeEditor` has stubbed cursor/selection methods for 4.27 compatibility.
- **Settings location**: Plugin settings are in **Project Settings > Plugins > Node to Code** (not Editor Preferences), because `UDeveloperSettings` with `Config=NodeToCode` defaults container to "Project".

## Completed Work

- **Slate UI rebuild**: `SN2CEditorWindow` fully rebuilt with SWidgetSwitcher (welcome/loading/results/error panels), dual SN2CCodeEditor widgets, SComboBox graph selector, copy/open-folder buttons, and native delegate subscriptions.
- **Native delegates**: Added `FOnTranslationRequestSentNative` and `FOnTranslationResponseReceivedNative` alongside existing dynamic delegates in `UN2CLLMModule` for Slate widget binding.
- **Anthropic model updates**: Updated to Claude Opus 4.6, Sonnet 4.6, Haiku 4.5 with correct API model IDs (no date suffix for 4.6 models). Updated pricing for Opus 4.6 ($5/$25 per MTok).
- **Max tokens**: Increased Anthropic max_tokens from 8192 to 16384 to prevent truncated responses on larger Blueprints.

## Documentation

- `docs/PROJECT_OVERVIEW.md` - Architecture, source layout, LLM providers, data flow (v1 pipeline)
- `docs/UE4_PORT_GUIDE.md` - Every UE5->UE4.27 API change with tables
- `docs/DEVELOPMENT_SETUP.md` - Junction setup, build commands, agent instructions (predates this being an in-project checkout; junction section is stale, see Key Paths above)
- `docs/BLUEPRINT_CPP_BRIDGE.md` - Spec for the v2 Blueprint<->C++ bridge (see section above); §15 phase table is the source of truth for status
