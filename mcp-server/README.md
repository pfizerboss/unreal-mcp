# unreal-mcp-server

unreal-mcp-server is a Python-based server that implements the Model Context Protocol (MCP) for Unreal Engine. 
It enables smooth communication between MCP clients (e.g., Claude, Cursor, Windsurf) and the Unreal Editor, and is intended to be used together with the Unreal-MCPython Plugin.

- Demo : [Build 3D Scenes in Unreal Engine with Claude AI | Unreal-MCPython Demo](https://youtu.be/V7KyjzFlBLk?si=sOY1dEVGV2hqi4JC)
- Fab Link : [Unreal-MCPython: AI Assistant Plugin for Unreal Editor using Python & MCP](https://fab.com/s/aed5f75d50b2)
- Github Link : [GenOrca/unreal-mcpython](https://github.com/GenOrca/unreal-mcpython)


## 🎯 Why Choose Unreal-MCPython?

<p align="center">
<img src="https://raw.githubusercontent.com/GenOrca/Screenshot/refs/heads/main/unreal-mcp/Screenshot%202025-06-02%20025106.png" width="400">
<img src="https://raw.githubusercontent.com/GenOrca/Screenshot/refs/heads/main/unreal-mcp/Screenshot%202025-06-02%20025111.png" width="400">
<img src="https://raw.githubusercontent.com/GenOrca/Screenshot/refs/heads/main/unreal-mcp/Screenshot%202025-06-02%20025115.png" width="400">
<img src="https://raw.githubusercontent.com/GenOrca/Screenshot/refs/heads/main/unreal-mcp/Screenshot%202025-06-02%20025120.png" width="400">
</p>

- 🧠 **Unreal AI integration** - Direct Claude AI assistance in Unreal Engine
- 🔗 **Native MCP protocol support** - Seamless communication between AI and UE
- 🎮 **Intelligent game development** - AI-powered asset management and scene manipulation  
- ⚡ **Smart automation** - Context-aware blueprint scripting with AI guidance
- 🎨 **Technical artist focused** - AI assistance for complex production pipelines

## Key Features

- MCP server for communication with Unreal Engine
- 22 namespace dispatcher tools exposing 294 actions, each callable as `{action, params}`
- Supports Python 3.11 and later

## Universal Blueprint 2.0

Recommended sequence:

```text
get_blueprint_brief -> inspect_blueprint
-> search_blueprint_node_actions / suggest_blueprint_nodes_for_pin
-> describe_blueprint_node_action -> add_blueprint_action_node
-> connect_blueprint_pins -> compile_blueprint -> get_blueprint_health
-> asset.save_asset (only when persistence is wanted)
```

Inspection is paginated per query with `limit` 1-500 (default 100). Its opaque
cursor becomes invalid when the query, asset, or editor session changes.
Stable IDs use `graph:`, `node:`, `pin:`, `variable:`, `component:`, and
`interface:` prefixes; legacy records use `fallback:<kind>:<sha1>` and report
`stable=false`. Canonical types include `{"kind":"int"}`,
`{"kind":"struct","type_path":"/Script/CoreUObject.Vector"}`, and
`{"kind":"class","class_path":"/Script/Engine.Actor"}`; generated asset
classes use full paths such as `/Game/Characters/BP_Hero.BP_Hero_C`.
Reflected members also require exact paths, for example
`/Script/Engine.Actor:K2_GetActorLocation`.

Palette `action:` IDs and `palette-cursor:` values are bounded to the current
editor session, asset, graph, query, and optional source pin. Repeat search or
pin suggestion when an ID becomes stale; do not replay it in another graph.
Native palette pages return at most 200 actions. Pin suggestions reflect
Unreal's current native action filter, and spawning never auto-connects pins.

Blueprint mutations do not compile or save implicitly. Compile explicitly,
run the compile-backed health check as a separate diagnostic call, and save
with `asset.save_asset` only when wanted. Runtime capabilities target UE
5.6-5.8; this workspace has locally verified only UE 5.7 because UE 5.6 is not
installed and UE 5.8 source headers are unavailable. Check capability flags on
those versions. A base-game generator is not included.

# Installation

Clone the repository:

```bash
git clone https://github.com/your-org/unreal-mcp-server.git
cd unreal-mcp-server
```

# Running the Server

You can start the MCP server with the following command:
```bash
uv --directory absolute/path/to/unreal-mcp-server run src/unreal_mcp/main.py
```

# Example Configuration (Using Claude, VSCode, Cursor)

The following is an example configuration for launching the MCP server from Claude, VSCode, or Cursor:

```json
{
    "mcpServers": {
        "unreal-mcpython": {
            "command": "uv",
            "args": [
                "--directory",
                "/absolute/path/to/unreal-mcp-server",  // e.g., D:/GitHub/unreal-mcp-server
                "run",
                "src/unreal_mcp/main.py"
            ]
        }
    }
}
```

This configuration approach works similarly across editors like VSCode and Cursor.

# License

This project is licensed under the Apache-2.0 License. See the LICENSE file for details.
