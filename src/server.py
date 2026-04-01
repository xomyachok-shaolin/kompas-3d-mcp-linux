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
KOMPAS_BIN = "/opt/ascon/kompas3d-v24/Bin/kKompas"
BRIDGE_PLUGIN = Path(__file__).parent / "Build" / "Exe" / "Release-x64-Linux" / "libkompas_mcp_bridge.rtw"
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


# Persistent KOMPAS process
_kompas_proc = None
_kompas_ready = False
CMD_FILE = "/tmp/kompas_mcp_cmd.json"
RESULT_FILE = "/tmp/kompas_mcp_result.json"


def ensure_kompas_running():
    """Start KOMPAS if not already running."""
    global _kompas_proc, _kompas_ready

    if _kompas_proc and _kompas_proc.poll() is None:
        return True  # Already running

    log("Starting KOMPAS-3D process...")

    template = "/tmp/kompas_mcp_template.cdw"
    Path(template).touch()

    env = os.environ.copy()
    env["KOMPAS_MCP_CMD_FILE"] = CMD_FILE
    env["KOMPAS_MCP_RESULT_FILE"] = RESULT_FILE
    env["KOMPAS_SDK"] = KOMPAS_SDK

    _kompas_proc = subprocess.Popen(
        ["xvfb-run", "-a", "--server-args=-screen 0 1920x1080x24",
         "dbus-run-session", KOMPAS_BIN, template],
        env=env,
        stdout=open("/tmp/kompas_stdout.log", "w"),
        stderr=open("/tmp/kompas_stderr.log", "w"),
    )

    # Wait for initial bridge result (from first command in env)
    time.sleep(12)
    _kompas_ready = _kompas_proc.poll() is None
    log(f"KOMPAS started, ready={_kompas_ready}, pid={_kompas_proc.pid}")
    return _kompas_ready


def execute_kompas_command(command: str, params: dict) -> dict:
    """
    Execute a KOMPAS-3D command via file-based IPC with persistent process.
    Bridge plugin watches for new command files via a polling loop.
    For first call: starts KOMPAS and uses env-var command.
    For subsequent calls: writes new command file (bridge needs polling support).
    """
    # Write command
    cmd_data = {"command": command}
    cmd_data.update(params)

    # Remove old result
    try:
        os.unlink(RESULT_FILE)
    except OSError:
        pass

    cmd_path = CMD_FILE
    result_path = RESULT_FILE

    with open(cmd_path, "w") as f:
        json.dump(cmd_data, f)

    try:
        # Ensure KOMPAS is running (starts on first call)
        if not ensure_kompas_running():
            return {"success": False, "message": "Failed to start KOMPAS-3D"}

        log(f"Sending command: {command}")

        # Wait for result file
        max_wait = 20
        poll_interval = 0.3
        waited = 0.0
        while waited < max_wait:
            time.sleep(poll_interval)
            waited += poll_interval
            if Path(result_path).exists():
                time.sleep(0.2)
                break

        if Path(result_path).exists():
            with open(result_path, "r") as f:
                return json.load(f)
        else:
            return {
                "success": False,
                "message": f"No result after {max_wait}s for command '{command}'",
            }

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
