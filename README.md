# KOMPAS-3D MCP Server for Linux

A Model Context Protocol (MCP) server that provides programmatic access to KOMPAS-3D v24 on Linux via the KsAPI C++ SDK.

## Architecture

1. **C++ bridge plugin** (`libkompas_mcp_bridge.rtw`) - KsAPI plugin that reads JSON commands, executes KOMPAS-3D operations, and writes JSON results.
2. **Python MCP server** (`server.py`) - Implements MCP protocol (JSON-RPC 2.0 over stdio), orchestrates plugin execution via `ksinvisible`.

## Requirements

- KOMPAS-3D v24 installed at `/opt/ascon/kompas3d-v24/`
- clang-18 / clang++-18
- CMake 3.20+
- Ninja build system
- Python 3.10+

## Building the C++ bridge

```bash
cd src
export KOMPAS_SDK=/opt/ascon/kompas3d-v24/SDK
cmake --preset Release-x64-Linux
cmake --build --preset Release-x64-Linux
```

The output plugin will be at `src/Build/Exe/Release-x64-Linux/kompas_mcp_bridge.rtw`.

## Running the MCP server

```bash
python3 src/server.py
```

The server communicates over stdin/stdout using the MCP protocol (JSON-RPC 2.0). Configure your MCP client to launch it as a stdio transport.

## Available Tools

| Tool | Description |
|------|-------------|
| `create_document` | Create a new 2D drawing or fragment |
| `create_line` | Draw a line segment |
| `create_circle` | Draw a circle |
| `create_rectangle` | Draw a rectangle |
| `create_text` | Add text to the drawing |
| `create_polyline` | Draw a polyline (open or closed) |
| `fill_drawing_stamp` | Fill the title block (stamp) fields |
| `save_document` | Save the document to .cdw file |
| `export_to_dxf` | Export document to DXF format |
| `screenshot_document` | Save a raster screenshot of the document |
| `get_active_document` | Get info about the active document |
| `list_objects` | List objects in the active view |

## License

This project provides a bridge to KOMPAS-3D which is proprietary software by ASCON.
