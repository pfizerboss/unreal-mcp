# Fork README Implementation Plan

**Goal:** Replace the upstream-oriented root README with a concise bilingual landing page that explains this fork and its Blueprint-focused additions.

**Constraints:** Edit only root `README.md` in Task 2. Keep the page at 260 lines or fewer. Keep deep server documentation in `mcp-server/README.md`.

---

### Task 1: Lock the README acceptance contract

**Files:**

- Reference: `README.md`

- [ ] **Step 1: Run the Windows PowerShell 5.1-safe pre-change content gate**

Run this ASCII-only Python source; `\u` escapes avoid PowerShell's UTF-8 stdin encoding ambiguity:

```powershell
python -c "from pathlib import Path; text=Path('README.md').read_text(encoding='utf-8'); required=('pfizerboss/unreal-mcp','GenOrca/unreal-mcp','# Unreal MCP \u2014 Blueprint & Workflow Fork','## \u0420\u0443\u0441\u0441\u043a\u0438\u0439','## English','### \u0427\u0442\u043e \u044d\u0442\u043e','### \u0427\u0442\u043e \u0434\u043e\u0431\u0430\u0432\u043b\u0435\u043d\u043e \u043e\u0442\u043d\u043e\u0441\u0438\u0442\u0435\u043b\u044c\u043d\u043e \u043e\u0440\u0438\u0433\u0438\u043d\u0430\u043b\u0430','### \u0413\u043b\u0430\u0432\u043d\u043e\u0435 \u0432 \u043d\u0430\u0448\u0435\u043c \u0444\u043e\u0440\u043a\u0435','### \u0420\u0435\u043a\u043e\u043c\u0435\u043d\u0434\u0443\u0435\u043c\u044b\u0435 Blueprint-\u043f\u0440\u043e\u0446\u0435\u0441\u0441\u044b','### \u0411\u044b\u0441\u0442\u0440\u044b\u0439 \u0437\u0430\u043f\u0443\u0441\u043a','### \u0421\u043e\u0432\u043c\u0435\u0441\u0442\u0438\u043c\u043e\u0441\u0442\u044c \u0438 \u043f\u0440\u043e\u0432\u0435\u0440\u043a\u0430','### What this is','### Additions over the original project','### Main fork features','### Recommended Blueprint workflows','### Quick start','### Compatibility and validation','253 actions, 21 domains, 19 Blueprint actions','299 actions, 22 domains, 57 Blueprint actions','46 added public actions, including 38 Blueprint actions','253 \u0434\u0435\u0439\u0441\u0442\u0432\u0438\u044f, 21 \u0434\u043e\u043c\u0435\u043d, 19 Blueprint-\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u0439','299 \u0434\u0435\u0439\u0441\u0442\u0432\u0438\u0439, 22 \u0434\u043e\u043c\u0435\u043d\u0430, 57 Blueprint-\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u0439','46 \u0434\u043e\u0431\u0430\u0432\u043b\u0435\u043d\u043d\u044b\u0445 \u043f\u0443\u0431\u043b\u0438\u0447\u043d\u044b\u0445 \u0434\u0435\u0439\u0441\u0442\u0432\u0438\u0439, \u0432 \u0442\u043e\u043c \u0447\u0438\u0441\u043b\u0435 38 Blueprint-\u0434\u0435\u0439\u0441\u0442\u0432\u0438\u0439','native palette search','pin suggestions','Connected spawn','exact-edge insertion','preview/apply replacement','\u043d\u0430\u0442\u0438\u0432\u043d\u044b\u0439 \u043f\u043e\u0438\u0441\u043a \u043f\u0430\u043b\u0438\u0442\u0440\u044b','\u043f\u043e\u0434\u0441\u043a\u0430\u0437\u043a\u0438 \u043f\u0438\u043d\u043e\u0432','\u041f\u043e\u0434\u043a\u043b\u044e\u0447\u0451\u043d\u043d\u043e\u0435 \u0441\u043e\u0437\u0434\u0430\u043d\u0438\u0435','\u0432\u0441\u0442\u0430\u0432\u043a\u0430 \u0432 \u0442\u043e\u0447\u043d\u043e\u0435 \u0440\u0435\u0431\u0440\u043e','\u043f\u0440\u0435\u0434\u043f\u0440\u043e\u0441\u043c\u043e\u0442\u0440/\u043f\u0440\u0438\u043c\u0435\u043d\u0435\u043d\u0438\u0435 \u0437\u0430\u043c\u0435\u043d\u044b','workflow plan/apply/cancel/undo','\u043f\u043b\u0430\u043d/\u043f\u0440\u0438\u043c\u0435\u043d\u0438\u0442\u044c/\u043e\u0442\u043c\u0435\u043d\u0438\u0442\u044c/\u043e\u0442\u043a\u0430\u0442\u0438\u0442\u044c \u043f\u0440\u043e\u0446\u0435\u0441\u0441\u0430','No implicit compile or save.','\u041d\u0435\u0442 \u043d\u0435\u044f\u0432\u043d\u043e\u0439 \u043a\u043e\u043c\u043f\u0438\u043b\u044f\u0446\u0438\u0438 \u0438\u043b\u0438 \u0441\u043e\u0445\u0440\u0430\u043d\u0435\u043d\u0438\u044f.','No base-game generator.','\u041d\u0435\u0442 \u0433\u0435\u043d\u0435\u0440\u0430\u0442\u043e\u0440\u0430 \u0431\u0430\u0437\u043e\u0432\u043e\u0439 \u0438\u0433\u0440\u044b.','Plugins/UnrealMCPython','uv sync --project mcp-server','mcpServers'); missing=[item for item in required if item not in text]; assert not missing, missing; assert len(text.splitlines()) <= 260, len(text.splitlines())"
```

Expected: **FAIL** on the old upstream README because it does not meet the fork-specific bilingual contract. The assertion reports the missing phrases; rerun after each correction to reveal further missing evidence.

- [ ] **Step 2: Confirm generated catalog totals before writing prose**

```powershell
python -c "import sys; sys.path.insert(0, 'mcp-server/src'); from unreal_mcp.dispatchers._catalog import CATALOG; actual=(sum(map(len,CATALOG.values())), len(CATALOG), len(CATALOG['blueprint'])); assert actual == (299, 22, 57), actual; print(*actual)"
```

Expected exact output: `299 22 57`.

### Task 2: Replace the root README

**Files:**

- Modify: `README.md`

- [ ] **Step 1: Write the exact bilingual structure**

Use this exact heading order:

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
```

In both languages include these exact count phrases: `253 actions, 21 domains, 19 Blueprint actions`; `299 actions, 22 domains, 57 Blueprint actions`; `46 added public actions, including 38 Blueprint actions`; and their Russian counterparts required by the gate. State stable Blueprint identifiers and filtered inspection; functions, macros, events, dispatchers, interfaces, variables, and components; and strict/lossy replacement policy. Include snapshots, diffs, compile diagnostics, and health checks. Use no implicit compile/save or base-game-generator claims except the explicit required denials.

- [ ] **Step 2: Add the workflows and action names**

Include the five semantic action names in prose: native palette search, pin suggestions, connected spawn, exact-edge insertion, preview/apply replacement (and their Russian phrases required by the gate). Include these copyable action workflows in both language sections:

```text
Connected spawn:
inspect_blueprint -> suggest_blueprint_nodes_for_pin -> add_blueprint_connected_action_node
-> snapshot_blueprint_graph -> compile_blueprint -> get_blueprint_health

Insertion:
inspect_blueprint -> suggest_blueprint_nodes_for_connection -> insert_blueprint_action_node
-> snapshot_blueprint_graph -> compile_blueprint -> get_blueprint_health

Replacement:
inspect_blueprint -> search_blueprint_node_actions -> preview_blueprint_action_replacement
-> replace_blueprint_node_with_action -> diff_blueprint_graphs -> compile_blueprint -> get_blueprint_health
```

Name the workflow lifecycle as `workflow plan/apply/cancel/undo` and `план/применить/отменить/откатить процесса`.

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

Run exactly the same ASCII-only Python command from Task 1, Step 1.

Expected: PASS with no output.

- [ ] **Step 2: Check Markdown structure and stale upstream promotion**

```powershell
python -c "from pathlib import Path; text=Path('README.md').read_text(encoding='utf-8'); assert text.count('```') % 2 == 0; assert '```json' in text; assert '```powershell' in text; assert text.count('```text') == 2; assert 'github.com/GenOrca/unreal-mcp/releases' not in text; assert 'fab.com' not in text; assert 'youtu.be' not in text"
git diff --check
```

Expected: both commands exit 0.

- [ ] **Step 3: Confirm catalog counts and run focused repository tests**

```powershell
python -c "import sys; sys.path.insert(0, 'mcp-server/src'); from unreal_mcp.dispatchers._catalog import CATALOG; actual=(sum(map(len,CATALOG.values())), len(CATALOG), len(CATALOG['blueprint'])); assert actual == (299, 22, 57), actual; print(*actual)"
uv run --project mcp-server --extra dev pytest mcp-server/tests/test_blueprint2_contracts.py mcp-server/tests/test_coverage.py -q
```

Expected: catalog output exactly `299 22 57`; all selected tests pass.

- [ ] **Step 4: Review and commit only the README**

```powershell
git diff -- README.md
git add -- README.md
git diff --cached --check
git commit -m "docs: present blueprint-focused fork"
```

Expected: one commit containing only `README.md`.

- [ ] **Step 5: Push only the user fork branch**

```powershell
git push origin codex/llm-friendly-expansion
```

Expected: the remote branch advances and `git status --short` is empty.
