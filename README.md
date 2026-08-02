# Unreal MCP — Blueprint & Workflow Fork

> Расширенный форк [GenOrca/unreal-mcp](https://github.com/GenOrca/unreal-mcp), ориентированный на безопасную и понятную для LLM работу с Blueprint.

[Русский](#русский) · [English](#english)

## Русский

### Что это

Unreal MCP подключает AI-ассистента к Unreal Editor через Model Context Protocol. Форк сохраняет широкую работу оригинала с акторами, ассетами, материалами, анимацией, UMG, GAS и другими системами, но значительно расширяет Blueprint и добавляет управляемые транзакционные процессы.

### Что добавлено относительно оригинала

| Version | Actions | Domains | Blueprint |
|---|---:|---:|---:|
| Оригинальная база | 253 | 21 | 19 |
| Этот форк | 299 | 22 | 57 |

Оригинальная база: **253 действия, 21 домен, 19 Blueprint-действий**. Этот форк: **299 действий, 22 домена, 57 Blueprint-действий**. Всего получено **46 добавленных публичных действий, в том числе 38 Blueprint-действий**.

### Главное в нашем форке

- Blueprint 2.0: стабильные ID графов, узлов и пинов, краткий обзор и фильтрованная инспекция.
- Полное авторство функций, макросов, событий, dispatchers, interfaces, переменных и компонентов.
- Нативный поиск палитры (`native palette search`), подсказки пинов (`pin suggestions`) и безопасное создание узлов из реальной палитры Unreal.
- Подключённое создание, вставка в точное ребро и предпросмотр/применение замены с явным выбором совместимых bindings.
- Строгая замена по умолчанию; потеря связей или defaults возможна только через явно согласованный lossy-план.
- Детерминированные snapshots и diffs, диагностика компиляции и Blueprint health checks.
- Транзакционный план/применить/отменить/откатить процесса (`workflow plan/apply/cancel/undo`) с confirmation tokens и undo.
- Нет неявной компиляции или сохранения. Сохранение выполняется отдельным `asset.save_asset`.
- Нет генератора базовой игры. MCP предоставляет универсальные Blueprint-примитивы.

### Рекомендуемые Blueprint-процессы

```text
Подключённое создание (Connected spawn):
inspect_blueprint -> suggest_blueprint_nodes_for_pin -> add_blueprint_connected_action_node
-> snapshot_blueprint_graph -> compile_blueprint -> get_blueprint_health

Вставка (Insertion):
inspect_blueprint -> suggest_blueprint_nodes_for_connection -> insert_blueprint_action_node
-> snapshot_blueprint_graph -> compile_blueprint -> get_blueprint_health

Замена (Replacement):
inspect_blueprint -> search_blueprint_node_actions -> preview_blueprint_action_replacement
-> replace_blueprint_node_with_action -> diff_blueprint_graphs -> compile_blueprint -> get_blueprint_health
```

Capability-токены `action:`, `binding:`, `palette-cursor:` и `replacement-plan:` относятся к текущей сессии и контексту. При stale-ошибке повторите inspect, search/suggestion или replacement preview.

### Быстрый запуск

Требуются Unreal Engine, Python 3.11+, [uv](https://docs.astral.sh/uv/) и MCP-клиент.

```powershell
git clone --branch codex/llm-friendly-expansion https://github.com/pfizerboss/unreal-mcp.git
cd unreal-mcp
uv sync --project mcp-server
```

Скопируйте `Plugins/UnrealMCPython` в каталог `Plugins` своего Unreal-проекта и откройте или соберите проект. Затем добавьте сервер в конфигурацию MCP-клиента:

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

Откройте Unreal Editor: плагин запускает локальный TCP-сервер на `127.0.0.1:12029`. После этого MCP-клиент сможет вызывать namespace tools.

### Совместимость и проверка

- Локально полностью проверен **Unreal Engine 5.7**.
- UE 5.6 не запускался: движок не установлен в тестовой среде.
- UE 5.8 не запускался: локальная установка не содержит build tools, editor binary и source headers.
- Последний release gate: 930 MCP/Python tests, 16/16 native Blueprint2 tests и 420 in-editor tests без failures/errors; semantic workflow stress — 12/12.

## English

### What this is

Unreal MCP connects an AI assistant to Unreal Editor through the Model Context Protocol. This fork keeps the original project's broad actor, asset, material, animation, UMG, GAS, and editor tooling while expanding Blueprint authoring and guarded transactional workflows.

### Additions over the original project

The original baseline had **253 actions, 21 domains, 19 Blueprint actions**. This fork has **299 actions, 22 domains, 57 Blueprint actions**: **46 added public actions, including 38 Blueprint actions**.

### Main fork features

- Blueprint 2.0 with stable graph/node/pin identifiers, compact orientation, and filtered inspection.
- Authoring for functions, macros, events, dispatchers, interfaces, variables, and components.
- Native palette search, pin suggestions, and safe spawning from Unreal's real node palette.
- Connected spawn, exact-edge insertion, and preview/apply replacement with explicit binding selection.
- Strict replacement by default; a lossy replacement requires an explicitly matching preview/apply policy.
- Deterministic snapshots and diffs, compile diagnostics, and Blueprint health checks.
- Guarded workflow plan/apply/cancel/undo with confirmation tokens and transaction rollback.
- No implicit compile or save. Persist explicitly with `asset.save_asset`.
- No base-game generator. The project exposes universal Blueprint primitives instead.

### Recommended Blueprint workflows

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

`action:`, `binding:`, `palette-cursor:`, and `replacement-plan:` capabilities are scoped to the current editor session and exact context. Repeat inspect, search/suggestion, or replacement preview after a stale-capability error.

### Quick start

Requirements: Unreal Engine, Python 3.11+, [uv](https://docs.astral.sh/uv/), and an MCP client.

```powershell
git clone --branch codex/llm-friendly-expansion https://github.com/pfizerboss/unreal-mcp.git
cd unreal-mcp
uv sync --project mcp-server
```

Copy `Plugins/UnrealMCPython` into your Unreal project's `Plugins` directory, then open or build the project. Configure the MCP client using the JSON example in the Russian section. The plugin listens locally on `127.0.0.1:12029`.

### Compatibility and validation

- **Unreal Engine 5.7** is the locally verified release target.
- UE 5.6 was not run because it is not installed in the test environment.
- UE 5.8 was not run because the local installation lacks build tools, the editor binary, and source headers.
- Latest release gate: 930 MCP/Python tests, 16/16 native Blueprint2 tests, and 420 in-editor tests with no failures/errors; semantic workflow stress passed 12/12.

## Attribution and license

This repository is an independent fork of [GenOrca/unreal-mcp](https://github.com/GenOrca/unreal-mcp). Original work remains credited to its authors and contributors. Fork additions are distributed under the repository's [Apache License 2.0](LICENSE.txt).
