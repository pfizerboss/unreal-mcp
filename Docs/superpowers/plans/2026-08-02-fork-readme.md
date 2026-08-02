# Fork README Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the upstream-oriented root README with a concise bilingual landing page that explains this fork and its Blueprint-focused additions.

**Architecture:** Keep all fork positioning, installation, workflow guidance, compatibility facts, attribution, and license information in the root `README.md`. Preserve deeper server documentation in `mcp-server/README.md`; do not duplicate the full action catalog or upstream promotional content in the root page.

**Tech Stack:** GitHub-flavored Markdown, PowerShell, Python 3.11+, generated MCP action catalog.

---

### Task 1: Lock the README acceptance contract

**Files:**
- Modify: `README.md`
- Reference: `Docs/superpowers/specs/2026-08-02-fork-readme-design.md`

- [ ] **Step 1: Run the pre-change content check**

Run:

```powershell
@'
from pathlib import Path

text = Path("README.md").read_text(encoding="utf-8")
required = (
    "pfizerboss/unreal-mcp",
    "GenOrca/unreal-mcp",
    "253",
    "299",
    "19",
    "57",
    "Connected spawn",
    "Подключённое создание",
    "English",
)
missing = [item for item in required if item not in text]
assert not missing, missing
assert len(text.splitlines()) <= 260, len(text.splitlines())
'@ | python -
```

Expected: FAIL because the current upstream README does not identify `pfizerboss/unreal-mcp`, has no mirrored English marker, and is longer than 260 lines.

- [ ] **Step 2: Confirm generated catalog totals before writing prose**

Run:

```powershell
python -c "import sys; sys.path.insert(0, 'mcp-server/src'); from unreal_mcp.dispatchers._catalog import CATALOG; total=sum(map(len,CATALOG.values())); print(total, len(CATALOG), len(CATALOG['blueprint']))"
```

Expected: `299 22 57`.

### Task 2: Replace the root README

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Replace the upstream landing page with the approved bilingual structure**

Write these sections in this exact order:

```markdown
# Unreal MCP — Blueprint & Workflow Fork

> Расширенный форк [GenOrca/unreal-mcp](https://github.com/GenOrca/unreal-mcp), ориентированный на безопасную и понятную для LLM работу с Blueprint.

[Русский](#русский) · [English](#english)

## Русский

### Что это
### Что добавлено относительно оригинала
### Главное в нашем форке
### Рекомендуемые Blueprint-процессы
### Быстрый запуск
### Совместимость и проверка

## English

### What this is
### Additions over the original project
### Main fork features
### Recommended Blueprint workflows
### Quick start
### Compatibility and validation

## Attribution and license
```

The Russian and English sections must both state:

- original baseline: 253 actions, 21 domains, 19 Blueprint actions;
- current fork: 299 actions, 22 domains, 57 Blueprint actions;
- 46 added public actions, including 38 Blueprint actions;
- stable Blueprint identifiers and filtered inspection;
- functions, macros, events, dispatchers, interfaces, variables, and components;
- native palette search, pin suggestions, connected spawn, exact-edge insertion, and preview/apply replacement;
- explicit strict/lossy replacement policy;
- snapshots, diffs, compile diagnostics, health checks, workflow plan/apply/cancel/undo;
- no implicit compile or save;
- no base-game generator.

Include one compact comparison table in the Russian section and avoid repeating it in English.

- [ ] **Step 2: Add the three semantic Blueprint workflows**

Use this exact copyable block in both languages, with translated labels only:

```text
Connected spawn:
inspect_blueprint -> suggest_blueprint_nodes_for_pin
-> add_blueprint_connected_action_node
-> snapshot_blueprint_graph -> compile_blueprint -> get_blueprint_health

Insertion:
inspect_blueprint -> suggest_blueprint_nodes_for_connection
-> insert_blueprint_action_node
-> snapshot_blueprint_graph -> compile_blueprint -> get_blueprint_health

Replacement:
inspect_blueprint -> search_blueprint_node_actions
-> preview_blueprint_action_replacement
-> replace_blueprint_node_with_action
-> diff_blueprint_graphs -> compile_blueprint -> get_blueprint_health
```

- [ ] **Step 3: Add minimal source installation and MCP configuration**

Document only the source workflow for this fork:

```powershell
git clone https://github.com/pfizerboss/unreal-mcp.git
cd unreal-mcp
uv sync --project mcp-server
```

Tell the user to copy `Plugins/UnrealMCPython` into the Unreal project's `Plugins` directory, open/build the project, and configure the client with:

```json
{
  "mcpServers": {
    "unreal-mcp": {
      "command": "uv",
      "args": [
        "--directory",
        "C:/absolute/path/to/unreal-mcp/mcp-server",
        "run",
        "python",
        "-m",
        "unreal_mcp.main"
      ]
    }
  }
}
```

State that UE 5.7 is locally verified, UE 5.6 was not run because it is absent, and UE 5.8 was not run because its local installation is incomplete.

### Task 3: Validate, commit, and publish

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Run the post-change README contract**

Run:

```powershell
@'
from pathlib import Path

text = Path("README.md").read_text(encoding="utf-8")
required = (
    "pfizerboss/unreal-mcp",
    "GenOrca/unreal-mcp",
    "253",
    "299",
    "19",
    "57",
    "Connected spawn",
    "Подключённое создание",
    "English",
)
missing = [item for item in required if item not in text]
assert not missing, missing
assert len(text.splitlines()) <= 260, len(text.splitlines())
'@ | python -
```

Expected: PASS with no output.

- [ ] **Step 2: Check Markdown structure and stale upstream promotion**

Run:

```powershell
@'
from pathlib import Path

text = Path("README.md").read_text(encoding="utf-8")
assert text.count("```") % 2 == 0
assert "```json" in text
assert "```powershell" in text
assert text.count("```text") == 2
assert "github.com/GenOrca/unreal-mcp/releases" not in text
assert "fab.com" not in text
assert "youtu.be" not in text
assert "299 actions" in text
assert "57 Blueprint" in text
'@ | python -
git diff --check
```

Expected: both commands exit 0.

- [ ] **Step 3: Run focused repository contract tests**

Run:

```powershell
uv run --project mcp-server --extra dev pytest mcp-server/tests/test_blueprint2_contracts.py mcp-server/tests/test_coverage.py -q
```

Expected: all selected tests pass.

- [ ] **Step 4: Review and commit only the README**

Run:

```powershell
git diff -- README.md
git add -- README.md
git diff --cached --check
git commit -m "docs: present blueprint-focused fork"
```

Expected: one commit containing only `README.md`.

- [ ] **Step 5: Push only the user fork branch**

Run:

```powershell
git push origin codex/llm-friendly-expansion
```

Expected: the remote branch advances and `git status --short` is empty.
