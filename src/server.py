#!/usr/bin/env python3
"""
KOMPAS-3D MCP Server for Linux

Implements Model Context Protocol (MCP) over stdio using JSON-RPC 2.0.
For each tool call:
  1. Writes a command JSON file
  2. Runs ksinvisible with the bridge plugin loaded
  3. Reads the result JSON file
  4. Returns the result to the MCP client
"""

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Configuration
KOMPAS_BIN = "/opt/ascon/kompas3d-v24/Bin/ksinvisible"
BRIDGE_PLUGIN = Path(__file__).parent / "Build" / "Exe" / "Release-x64-Linux" / "kompas_mcp_bridge.rtw"
KOMPAS_SDK = os.environ.get("KOMPAS_SDK", "/opt/ascon/kompas3d-v24/SDK")

# MCP server info
SERVER_NAME = "kompas-3d-mcp-linux"
SERVER_VERSION = "0.1.0"

# Tool definitions matching the Windows KOMPAS-3D MCP
TOOLS = [
    {
        "name": "create_document",
        "description": "Create a new KOMPAS-3D 2D document (drawing or fragment).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "type": {
                    "type": "integer",
                    "description": "Document type: 1=drawing (.cdw), 2=fragment (.frw)",
                    "enum": [1, 2],
                    "default": 1,
                },
                "format": {
                    "type": "string",
                    "description": "Paper format: A0, A1, A2, A3, A4",
                    "default": "A4",
                },
            },
        },
    },
    {
        "name": "create_line",
        "description": "Draw a line segment on the active 2D document.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x1": {"type": "number", "description": "Start point X coordinate (mm)"},
                "y1": {"type": "number", "description": "Start point Y coordinate (mm)"},
                "x2": {"type": "number", "description": "End point X coordinate (mm)"},
                "y2": {"type": "number", "description": "End point Y coordinate (mm)"},
                "style": {
                    "type": "integer",
                    "description": "Line style: 1=thin, 2=thick",
                    "default": 1,
                },
            },
            "required": ["x1", "y1", "x2", "y2"],
        },
    },
    {
        "name": "create_circle",
        "description": "Draw a circle on the active 2D document.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "cx": {"type": "number", "description": "Center X coordinate (mm)"},
                "cy": {"type": "number", "description": "Center Y coordinate (mm)"},
                "radius": {"type": "number", "description": "Radius (mm)"},
            },
            "required": ["cx", "cy", "radius"],
        },
    },
    {
        "name": "create_rectangle",
        "description": "Draw a rectangle on the active 2D document.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "number", "description": "Base point X coordinate (mm)"},
                "y": {"type": "number", "description": "Base point Y coordinate (mm)"},
                "width": {"type": "number", "description": "Width (mm)"},
                "height": {"type": "number", "description": "Height (mm)"},
            },
            "required": ["x", "y", "width", "height"],
        },
    },
    {
        "name": "create_text",
        "description": "Add text to the active 2D document.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "number", "description": "Anchor point X coordinate (mm)"},
                "y": {"type": "number", "description": "Anchor point Y coordinate (mm)"},
                "text": {"type": "string", "description": "Text content"},
                "height": {
                    "type": "number",
                    "description": "Text height (mm)",
                    "default": 5.0,
                },
            },
            "required": ["x", "y", "text"],
        },
    },
    {
        "name": "create_polyline",
        "description": "Draw a polyline (sequence of connected line segments) on the active 2D document.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "points": {
                    "type": "array",
                    "description": "Array of [x, y] coordinate pairs",
                    "items": {
                        "type": "array",
                        "items": {"type": "number"},
                        "minItems": 2,
                        "maxItems": 2,
                    },
                },
                "closed": {
                    "type": "boolean",
                    "description": "Whether to close the polyline",
                    "default": False,
                },
            },
            "required": ["points"],
        },
    },
    {
        "name": "fill_drawing_stamp",
        "description": "Fill the title block (stamp/osnovnaya nadpis) of the active drawing.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "designation": {"type": "string", "description": "Drawing designation (cell 1)"},
                "name": {"type": "string", "description": "Drawing name (cell 2)"},
                "material": {"type": "string", "description": "Material (cell 3)"},
                "organization": {"type": "string", "description": "Organization name (cell 110)"},
                "developer": {"type": "string", "description": "Developer name (cell 111)"},
                "checker": {"type": "string", "description": "Checker name (cell 112)"},
                "approver": {"type": "string", "description": "Approver name (cell 113)"},
                "scale": {"type": "string", "description": "Drawing scale (cell 6)"},
                "sheet": {"type": "string", "description": "Sheet number (cell 7)"},
                "sheets": {"type": "string", "description": "Total sheets (cell 8)"},
                "mass": {"type": "string", "description": "Mass (cell 5)"},
            },
        },
    },
    {
        "name": "save_document",
        "description": "Save the active document. If path is specified, performs Save As.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "File path for Save As (optional, .cdw or .frw)",
                },
            },
        },
    },
    {
        "name": "export_to_dxf",
        "description": "Export the active document to DXF format.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Output DXF file path"},
            },
            "required": ["path"],
        },
    },
    {
        "name": "screenshot_document",
        "description": "Save a raster screenshot (PNG/BMP) of the active document.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Output image file path (.png or .bmp)"},
            },
            "required": ["path"],
        },
    },
    {
        "name": "get_active_document",
        "description": "Get information about the currently active document.",
        "inputSchema": {
            "type": "object",
            "properties": {},
        },
    },
    {
        "name": "list_objects",
        "description": "List drawing objects in the active view of the current document.",
        "inputSchema": {
            "type": "object",
            "properties": {},
        },
    },
]


def log(msg: str) -> None:
    """Log to stderr (MCP servers communicate on stdout)."""
    print(f"[kompas-mcp] {msg}", file=sys.stderr, flush=True)


def execute_kompas_command(command: str, params: dict) -> dict:
    """
    Execute a KOMPAS-3D command by:
    1. Writing a command JSON file
    2. Running ksinvisible with the bridge plugin
    3. Reading the result JSON file
    """
    # Create temp files for command/result exchange
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".json", prefix="kompas_cmd_", delete=False
    ) as cmd_file:
        cmd_data = {"command": command}
        cmd_data.update(params)
        json.dump(cmd_data, cmd_file)
        cmd_path = cmd_file.name

    result_path = cmd_path.replace("kompas_cmd_", "kompas_result_")

    try:
        plugin_path = str(BRIDGE_PLUGIN)
        if not Path(plugin_path).exists():
            # Try debug build
            debug_plugin = Path(__file__).parent / "Build" / "Exe" / "Debug-x64-Linux" / "kompas_mcp_bridge.rtw"
            if debug_plugin.exists():
                plugin_path = str(debug_plugin)
            else:
                return {
                    "success": False,
                    "message": f"Bridge plugin not found. Build it first. Looked at: {plugin_path}",
                }

        env = os.environ.copy()
        env["KOMPAS_MCP_CMD_FILE"] = cmd_path
        env["KOMPAS_MCP_RESULT_FILE"] = result_path
        env["KOMPAS_SDK"] = KOMPAS_SDK

        # Run ksinvisible with the plugin
        # ksinvisible accepts -l <library_path> to load a plugin
        ksinvisible_cmd = [KOMPAS_BIN, "-l", plugin_path]

        log(f"Executing: {' '.join(ksinvisible_cmd)}")
        log(f"  CMD_FILE={cmd_path}")
        log(f"  RESULT_FILE={result_path}")

        result = subprocess.run(
            ksinvisible_cmd,
            env=env,
            timeout=60,
            capture_output=True,
            text=True,
        )

        if result.returncode != 0:
            log(f"ksinvisible stderr: {result.stderr}")
            # Still try to read the result file - the plugin might have written it
            # before the process failed

        # Read result
        if Path(result_path).exists():
            with open(result_path, "r") as f:
                return json.load(f)
        else:
            return {
                "success": False,
                "message": f"Result file not created. ksinvisible exit code: {result.returncode}. "
                           f"stderr: {result.stderr[:500] if result.stderr else 'none'}",
            }

    except subprocess.TimeoutExpired:
        return {"success": False, "message": "KOMPAS-3D execution timed out (60s)"}
    except Exception as e:
        return {"success": False, "message": f"Execution error: {str(e)}"}
    finally:
        # Clean up temp files
        try:
            os.unlink(cmd_path)
        except OSError:
            pass
        try:
            os.unlink(result_path)
        except OSError:
            pass


def handle_initialize(params: dict) -> dict:
    """Handle MCP initialize request."""
    return {
        "protocolVersion": "2024-11-05",
        "capabilities": {
            "tools": {},
        },
        "serverInfo": {
            "name": SERVER_NAME,
            "version": SERVER_VERSION,
        },
    }


def handle_tools_list(params: dict) -> dict:
    """Handle MCP tools/list request."""
    return {"tools": TOOLS}


def handle_tools_call(params: dict) -> dict:
    """Handle MCP tools/call request."""
    tool_name = params.get("name", "")
    arguments = params.get("arguments", {})

    # Validate tool exists
    tool_names = {t["name"] for t in TOOLS}
    if tool_name not in tool_names:
        return {
            "content": [
                {
                    "type": "text",
                    "text": json.dumps({"error": f"Unknown tool: {tool_name}"}),
                }
            ],
            "isError": True,
        }

    # Execute the command
    result = execute_kompas_command(tool_name, arguments)

    is_error = not result.get("success", False)
    return {
        "content": [
            {
                "type": "text",
                "text": json.dumps(result, ensure_ascii=False, indent=2),
            }
        ],
        "isError": is_error,
    }


def handle_request(method: str, params: dict) -> dict | None:
    """Route a JSON-RPC request to the appropriate handler."""
    handlers = {
        "initialize": handle_initialize,
        "tools/list": handle_tools_list,
        "tools/call": handle_tools_call,
    }
    handler = handlers.get(method)
    if handler:
        return handler(params)
    return None


def main():
    """Main MCP server loop - reads JSON-RPC from stdin, writes to stdout."""
    log("KOMPAS-3D MCP Server starting...")
    log(f"Bridge plugin path: {BRIDGE_PLUGIN}")
    log(f"KOMPAS binary: {KOMPAS_BIN}")

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            request = json.loads(line)
        except json.JSONDecodeError as e:
            log(f"JSON parse error: {e}")
            continue

        method = request.get("method", "")
        params = request.get("params", {})
        req_id = request.get("id")

        # Notifications (no id) - just acknowledge
        if method == "notifications/initialized":
            log("Client initialized notification received")
            continue
        if method == "notifications/cancelled":
            log("Cancellation notification received")
            continue

        # Handle the request
        result = handle_request(method, params)

        if result is not None and req_id is not None:
            response = {
                "jsonrpc": "2.0",
                "id": req_id,
                "result": result,
            }
            response_str = json.dumps(response, ensure_ascii=False)
            sys.stdout.write(response_str + "\n")
            sys.stdout.flush()
            log(f"Responded to {method} (id={req_id})")
        elif req_id is not None:
            # Method not found
            error_response = {
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {
                    "code": -32601,
                    "message": f"Method not found: {method}",
                },
            }
            sys.stdout.write(json.dumps(error_response) + "\n")
            sys.stdout.flush()
            log(f"Method not found: {method}")

    log("KOMPAS-3D MCP Server shutting down")


if __name__ == "__main__":
    main()
