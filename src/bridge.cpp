////////////////////////////////////////////////////////////////////////////////
//
// bridge.cpp - KOMPAS-3D MCP Bridge Plugin
//
// Reads a JSON command file, executes KsAPI operations, writes result JSON.
// Environment variables:
//   KOMPAS_MCP_CMD_FILE    - path to input command JSON
//   KOMPAS_MCP_RESULT_FILE - path to output result JSON
//
////////////////////////////////////////////////////////////////////////////////

#include <KsAPI.h>
#include <KompasLibraryActions.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <locale>
#include <codecvt>
#include <dlfcn.h>
#include <thread>
#include <chrono>


static ksapi::IApplication * kompasApp = nullptr;

// ---------------------------------------------------------------------------
// Minimal JSON helpers (no external library needed)
// ---------------------------------------------------------------------------

// Trim whitespace
static std::string Trim(const std::string & s)
{
  size_t start = s.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\n\r");
  return s.substr(start, end - start + 1);
}

// Extract a string value for a given key from a flat JSON object
static std::string JsonGetString(const std::string & json, const std::string & key)
{
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) return "";
  pos = json.find(':', pos + search.size());
  if (pos == std::string::npos) return "";
  pos = json.find('"', pos + 1);
  if (pos == std::string::npos) return "";
  size_t end = json.find('"', pos + 1);
  if (end == std::string::npos) return "";
  return json.substr(pos + 1, end - pos - 1);
}

// Extract a numeric value for a given key from a flat JSON object
static double JsonGetNumber(const std::string & json, const std::string & key, double defaultVal = 0.0)
{
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) return defaultVal;
  pos = json.find(':', pos + search.size());
  if (pos == std::string::npos) return defaultVal;
  pos++;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
  std::string numStr;
  while (pos < json.size() && (std::isdigit(json[pos]) || json[pos] == '.' || json[pos] == '-' || json[pos] == '+'))
  {
    numStr += json[pos];
    pos++;
  }
  if (numStr.empty()) return defaultVal;
  return std::stod(numStr);
}

// Extract an integer value
static int JsonGetInt(const std::string & json, const std::string & key, int defaultVal = 0)
{
  return static_cast<int>(JsonGetNumber(json, key, static_cast<double>(defaultVal)));
}

// Extract a boolean value
static bool JsonGetBool(const std::string & json, const std::string & key, bool defaultVal = false)
{
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) return defaultVal;
  pos = json.find(':', pos + search.size());
  if (pos == std::string::npos) return defaultVal;
  std::string rest = json.substr(pos + 1);
  rest = Trim(rest);
  if (rest.substr(0, 4) == "true") return true;
  if (rest.substr(0, 5) == "false") return false;
  return defaultVal;
}

// Extract a JSON array of arrays of doubles: [[x,y], [x,y], ...]
static std::vector<std::pair<double, double>> JsonGetPointsArray(const std::string & json, const std::string & key)
{
  std::vector<std::pair<double, double>> result;
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) return result;
  pos = json.find('[', pos + search.size());
  if (pos == std::string::npos) return result;
  // Find the outer array
  pos++; // skip outer '['
  while (pos < json.size())
  {
    pos = json.find('[', pos);
    if (pos == std::string::npos) break;
    size_t end = json.find(']', pos);
    if (end == std::string::npos) break;
    std::string inner = json.substr(pos + 1, end - pos - 1);
    // Parse two numbers separated by comma
    size_t comma = inner.find(',');
    if (comma != std::string::npos)
    {
      double x = std::stod(Trim(inner.substr(0, comma)));
      double y = std::stod(Trim(inner.substr(comma + 1)));
      result.push_back({x, y});
    }
    pos = end + 1;
    // Check if we hit the outer closing bracket
    size_t next = json.find_first_not_of(" ,\t\n\r", pos);
    if (next != std::string::npos && json[next] == ']') break;
  }
  return result;
}

// Convert std::string (UTF-8) to std::wstring
static std::wstring ToWString(const std::string & s)
{
  std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
  return converter.from_bytes(s);
}

// Write result JSON file
static void WriteResult(const std::string & resultFile, bool success, const std::string & message,
                        const std::string & extraJson = "")
{
  std::ofstream out(resultFile);
  out << "{\n";
  out << "  \"success\": " << (success ? "true" : "false") << ",\n";
  out << "  \"message\": \"" << message << "\"";
  if (!extraJson.empty())
  {
    out << ",\n  " << extraJson;
  }
  out << "\n}\n";
  out.close();
}

// Read the entire file into a string
static std::string ReadFile(const std::string & path)
{
  std::ifstream in(path);
  if (!in.is_open()) return "";
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}


// ---------------------------------------------------------------------------
// Helper: get the drawing container from active view
// ---------------------------------------------------------------------------
static ksapi::IDrawingContainerPtr GetDrawingContainer()
{
  if (!kompasApp) return nullptr;
  ksapi::IKompasDocument2DPtr doc2D = kompasApp->GetActiveDocument();
  if (!doc2D) return nullptr;
  ksapi::IViewsAndLayersManagerPtr viewsMgr = doc2D->GetViewsAndLayersManager();
  if (!viewsMgr) return nullptr;
  ksapi::IViewsPtr views = viewsMgr->GetViews();
  if (!views) return nullptr;
  return views->GetActiveView();
}


// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

static void CmdCreateDocument(const std::string & params, const std::string & resultFile)
{
  if (!kompasApp)
  {
    WriteResult(resultFile, false, "Application not initialized");
    return;
  }

  int type = JsonGetInt(params, "type", 1); // 1=drawing, 2=fragment
  // std::string format = JsonGetString(params, "format"); // A1, A3, A4 - for future use

  ksapi::IDocumentsPtr docs = kompasApp->GetDocuments();
  if (!docs)
  {
    WriteResult(resultFile, false, "Failed to get documents collection");
    return;
  }

  DocumentTypeEnum docType = (type == 2) ? ksDocumentFragment : ksDocumentDrawing;
  ksapi::IKompasDocumentPtr doc(docs->AddWithDefaultSettings(docType, true));
  if (!doc)
  {
    WriteResult(resultFile, false, "Failed to create document");
    return;
  }

  WriteResult(resultFile, true, "Document created", "\"type\": " + std::to_string(type));
}


static void CmdCreateLine(const std::string & params, const std::string & resultFile)
{
  ksapi::IDrawingContainerPtr container = GetDrawingContainer();
  if (!container)
  {
    WriteResult(resultFile, false, "No active drawing container");
    return;
  }

  ksapi::ILineSegmentsPtr segments = container->GetLineSegments();
  if (!segments)
  {
    WriteResult(resultFile, false, "Failed to get line segments collection");
    return;
  }

  ksapi::ILineSegmentPtr seg = segments->Add();
  if (!seg)
  {
    WriteResult(resultFile, false, "Failed to create line segment");
    return;
  }

  seg->SetX1(JsonGetNumber(params, "x1"));
  seg->SetY1(JsonGetNumber(params, "y1"));
  seg->SetX2(JsonGetNumber(params, "x2"));
  seg->SetY2(JsonGetNumber(params, "y2"));

  int style = JsonGetInt(params, "style", 1);
  // Map simplified style: 1=thin (ksCSNormal), 2=thick (ksCSThickened)
  if (style == 2)
    seg->SetStyle(static_cast<int32_t>(ksCurveStyleEnum::ksCSThick));
  else
    seg->SetStyle(static_cast<int32_t>(ksCurveStyleEnum::ksCSNormal));

  bool ok = seg->Update();
  WriteResult(resultFile, ok, ok ? "Line created" : "Failed to update line segment");
}


static void CmdCreateCircle(const std::string & params, const std::string & resultFile)
{
  ksapi::IDrawingContainerPtr container = GetDrawingContainer();
  if (!container)
  {
    WriteResult(resultFile, false, "No active drawing container");
    return;
  }

  ksapi::ICirclesPtr circles = container->GetCircles();
  if (!circles)
  {
    WriteResult(resultFile, false, "Failed to get circles collection");
    return;
  }

  ksapi::ICirclePtr circle = circles->Add();
  if (!circle)
  {
    WriteResult(resultFile, false, "Failed to create circle");
    return;
  }

  circle->SetXc(JsonGetNumber(params, "cx"));
  circle->SetYc(JsonGetNumber(params, "cy"));
  circle->SetRadius(JsonGetNumber(params, "radius"));

  bool ok = circle->Update();
  WriteResult(resultFile, ok, ok ? "Circle created" : "Failed to update circle");
}


static void CmdCreateRectangle(const std::string & params, const std::string & resultFile)
{
  ksapi::IDrawingContainerPtr container = GetDrawingContainer();
  if (!container)
  {
    WriteResult(resultFile, false, "No active drawing container");
    return;
  }

  ksapi::IRectanglesPtr rects = container->GetRectangles();
  if (!rects)
  {
    WriteResult(resultFile, false, "Failed to get rectangles collection");
    return;
  }

  ksapi::IRectanglePtr rect = rects->Add();
  if (!rect)
  {
    WriteResult(resultFile, false, "Failed to create rectangle");
    return;
  }

  rect->SetX(JsonGetNumber(params, "x"));
  rect->SetY(JsonGetNumber(params, "y"));
  rect->SetWidth(JsonGetNumber(params, "width"));
  rect->SetHeight(JsonGetNumber(params, "height"));
  rect->SetAngle(JsonGetNumber(params, "angle", 0.0));

  bool ok = rect->Update();
  WriteResult(resultFile, ok, ok ? "Rectangle created" : "Failed to update rectangle");
}


static void CmdCreateText(const std::string & params, const std::string & resultFile)
{
  ksapi::IDrawingContainerPtr container = GetDrawingContainer();
  if (!container)
  {
    WriteResult(resultFile, false, "No active drawing container");
    return;
  }

  ksapi::IDrawingTextsPtr texts = container->GetDrawingTexts();
  if (!texts)
  {
    WriteResult(resultFile, false, "Failed to get drawing texts collection");
    return;
  }

  ksapi::IDrawingTextPtr text = texts->Add();
  if (!text)
  {
    WriteResult(resultFile, false, "Failed to create drawing text");
    return;
  }

  text->SetX(JsonGetNumber(params, "x"));
  text->SetY(JsonGetNumber(params, "y"));

  double height = JsonGetNumber(params, "height", 5.0);
  text->SetHeight(height);

  // Set the text content via IText interface
  ksapi::ITextPtr textContent(text);
  if (textContent)
  {
    std::string textStr = JsonGetString(params, "text");
    textContent->SetStr(ToWString(textStr));
  }

  bool ok = text->Update();
  WriteResult(resultFile, ok, ok ? "Text created" : "Failed to update text");
}


static void CmdCreatePolyline(const std::string & params, const std::string & resultFile)
{
  ksapi::IDrawingContainerPtr container = GetDrawingContainer();
  if (!container)
  {
    WriteResult(resultFile, false, "No active drawing container");
    return;
  }

  ksapi::IPolyLines2DPtr polylines = container->GetPolyLines2D();
  if (!polylines)
  {
    WriteResult(resultFile, false, "Failed to get polylines collection");
    return;
  }

  ksapi::IPolyLine2DPtr polyline = polylines->Add();
  if (!polyline)
  {
    WriteResult(resultFile, false, "Failed to create polyline");
    return;
  }

  bool closed = JsonGetBool(params, "closed", false);
  polyline->SetClosed(closed);

  auto points = JsonGetPointsArray(params, "points");
  // SetPoints expects a flat array: [x0, y0, x1, y1, ...]
  std::vector<double> flatPoints;
  flatPoints.reserve(points.size() * 2);
  for (auto & [x, y] : points)
  {
    flatPoints.push_back(x);
    flatPoints.push_back(y);
  }
  polyline->SetPoints(flatPoints);

  bool ok = polyline->Update();
  WriteResult(resultFile, ok, ok ? "Polyline created" : "Failed to update polyline");
}


static void CmdFillDrawingStamp(const std::string & params, const std::string & resultFile)
{
  if (!kompasApp)
  {
    WriteResult(resultFile, false, "Application not initialized");
    return;
  }

  ksapi::IKompasDocumentPtr doc(kompasApp->GetActiveDocument());
  if (!doc)
  {
    WriteResult(resultFile, false, "No active document");
    return;
  }

  ksapi::ILayoutSheetsPtr sheets = doc->GetLayoutSheets();
  if (!sheets)
  {
    WriteResult(resultFile, false, "Failed to get layout sheets");
    return;
  }

  // Get the first layout sheet
  ksapi::ILayoutSheetPtr sheet = sheets->GetItem(0);
  if (!sheet)
  {
    WriteResult(resultFile, false, "Failed to get layout sheet");
    return;
  }

  ksapi::IStampPtr stamp = sheet->GetStamp();
  if (!stamp)
  {
    WriteResult(resultFile, false, "Failed to get stamp");
    return;
  }

  // Standard stamp cell IDs (GOST 2.104):
  // 1 - Designation (Обозначение)
  // 2 - Name (Наименование)
  // 3 - Material (Материал)
  // 110 - Organization (Организация)
  // 111 - Developer name (Разработал)
  // 112 - Checker name (Проверил)

  struct StampField
  {
    const char * jsonKey;
    int cellId;
  };

  StampField fields[] = {
    {"designation", 1},
    {"name",        2},
    {"material",    3},
    {"organization", 110},
    {"developer",   111},
    {"checker",     112},
    {"approver",    113},
    {"scale",       6},
    {"sheet",       7},
    {"sheets",      8},
    {"mass",        5},
  };

  for (auto & field : fields)
  {
    std::string val = JsonGetString(params, field.jsonKey);
    if (!val.empty())
    {
      ksapi::ITextPtr cellText = stamp->GetText(field.cellId);
      if (cellText)
        cellText->SetStr(ToWString(val));
    }
  }

  bool ok = stamp->Update();
  WriteResult(resultFile, ok, ok ? "Stamp filled" : "Failed to update stamp");
}


static void CmdSaveDocument(const std::string & params, const std::string & resultFile)
{
  if (!kompasApp)
  {
    WriteResult(resultFile, false, "Application not initialized");
    return;
  }

  ksapi::IKompasDocumentPtr doc(kompasApp->GetActiveDocument());
  if (!doc)
  {
    WriteResult(resultFile, false, "No active document");
    return;
  }

  std::string path = JsonGetString(params, "path");
  bool ok;
  if (path.empty())
    ok = doc->Save();
  else
    ok = doc->SaveAs(ToWString(path));

  WriteResult(resultFile, ok, ok ? "Document saved" : "Failed to save document");
}


static void CmdExportToDxf(const std::string & params, const std::string & resultFile)
{
  if (!kompasApp)
  {
    WriteResult(resultFile, false, "Application not initialized");
    return;
  }

  ksapi::IKompasDocumentPtr doc(kompasApp->GetActiveDocument());
  if (!doc)
  {
    WriteResult(resultFile, false, "No active document");
    return;
  }

  std::string path = JsonGetString(params, "path");
  if (path.empty())
  {
    WriteResult(resultFile, false, "Export path not specified");
    return;
  }

  // SaveAs with .dxf extension triggers DXF export in KOMPAS
  bool ok = doc->SaveAs(ToWString(path));
  WriteResult(resultFile, ok, ok ? "Exported to DXF" : "Failed to export to DXF");
}


static void CmdScreenshotDocument(const std::string & params, const std::string & resultFile)
{
  if (!kompasApp)
  {
    WriteResult(resultFile, false, "Application not initialized");
    return;
  }

  ksapi::IKompasDocumentPtr doc(kompasApp->GetActiveDocument());
  if (!doc)
  {
    WriteResult(resultFile, false, "No active document");
    return;
  }

  std::string path = JsonGetString(params, "path");
  if (path.empty())
  {
    WriteResult(resultFile, false, "Screenshot path not specified");
    return;
  }

  // Use SaveAsToRasterFormat for screenshots
  bool ok = doc->SaveAsToRasterFormat(ToWString(path), nullptr);
  WriteResult(resultFile, ok, ok ? "Screenshot saved" : "Failed to save screenshot");
}


static void CmdGetActiveDocument(const std::string & params, const std::string & resultFile)
{
  if (!kompasApp)
  {
    WriteResult(resultFile, false, "Application not initialized");
    return;
  }

  ksapi::IKompasDocumentPtr doc(kompasApp->GetActiveDocument());
  if (!doc)
  {
    WriteResult(resultFile, true, "No active document", "\"document\": null");
    return;
  }

  std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
  std::string name = converter.to_bytes(doc->GetName());
  std::string fullPath = converter.to_bytes(doc->GetFullPath());
  bool isChanged = doc->IsChanged();
  bool isReadOnly = doc->IsReadOnly();

  std::string extra = "\"document\": {"
    "\"name\": \"" + name + "\", "
    "\"path\": \"" + fullPath + "\", "
    "\"changed\": " + (isChanged ? "true" : "false") + ", "
    "\"readOnly\": " + (isReadOnly ? "true" : "false") +
    "}";

  WriteResult(resultFile, true, "Active document info", extra);
}


static void CmdListObjects(const std::string & params, const std::string & resultFile)
{
  ksapi::IDrawingContainerPtr container = GetDrawingContainer();
  if (!container)
  {
    WriteResult(resultFile, false, "No active drawing container");
    return;
  }

  // Get all objects (empty filter = all types)
  std::vector<int32_t> objTypes; // empty = all
  std::vector<ksapi::IDrawingObjectPtr> objects = container->GetObjects(objTypes);

  std::string extra = "\"count\": " + std::to_string(objects.size());
  WriteResult(resultFile, true, "Objects listed", extra);
}


// ---------------------------------------------------------------------------
// create_arc: Draw an arc by center, radius, start angle, end angle
// ---------------------------------------------------------------------------

static void CmdCreateArc(const std::string & params, const std::string & resultFile)
{
  ksapi::IDrawingContainerPtr container = GetDrawingContainer();
  if (!container)
  {
    WriteResult(resultFile, false, "No active drawing container");
    return;
  }

  double cx = JsonGetNumber(params, "cx");
  double cy = JsonGetNumber(params, "cy");
  double x1 = JsonGetNumber(params, "x1");
  double y1 = JsonGetNumber(params, "y1");
  double x2 = JsonGetNumber(params, "x2");
  double y2 = JsonGetNumber(params, "y2");
  bool direction = JsonGetBool(params, "direction", false); // false = counter-clockwise
  int style = JsonGetInt(params, "style", 1);

  ksapi::IArcsPtr arcs = container->GetArcs();
  if (!arcs)
  {
    WriteResult(resultFile, false, "Failed to get arcs collection");
    return;
  }

  ksapi::IArcPtr arc = arcs->Add();
  if (!arc)
  {
    WriteResult(resultFile, false, "Failed to create arc");
    return;
  }

  arc->SetXc(cx);
  arc->SetYc(cy);
  arc->SetX1(x1);
  arc->SetY1(y1);
  arc->SetX2(x2);
  arc->SetY2(y2);
  arc->SetDirection(direction);
  arc->SetStyle(style);
  arc->Update();

  WriteResult(resultFile, true, "Arc created");
}


// ---------------------------------------------------------------------------
// create_linear_dimension: Add a linear dimension
// ---------------------------------------------------------------------------

static void CmdCreateLinearDimension(const std::string & params, const std::string & resultFile)
{
  ksapi::IDrawingContainerPtr container = GetDrawingContainer();
  if (!container)
  {
    WriteResult(resultFile, false, "No active drawing container");
    return;
  }

  double x1 = JsonGetNumber(params, "x1");
  double y1 = JsonGetNumber(params, "y1");
  double x2 = JsonGetNumber(params, "x2");
  double y2 = JsonGetNumber(params, "y2");
  double textY = JsonGetNumber(params, "text_y", y1 + 10);
  int orientation = JsonGetInt(params, "orientation", 0); // 0=horizontal, 1=vertical

  // Get active view for dimensions
  if (!kompasApp) { WriteResult(resultFile, false, "No app"); return; }
  ksapi::IKompasDocument2DPtr doc2D = kompasApp->GetActiveDocument();
  if (!doc2D) { WriteResult(resultFile, false, "No active document"); return; }
  ksapi::IViewsAndLayersManagerPtr viewsMgr = doc2D->GetViewsAndLayersManager();
  if (!viewsMgr) { WriteResult(resultFile, false, "No views manager"); return; }
  ksapi::IViewsPtr views = viewsMgr->GetViews();
  if (!views) { WriteResult(resultFile, false, "No views"); return; }
  ksapi::IViewPtr view = views->GetActiveView();
  if (!view) { WriteResult(resultFile, false, "No active view"); return; }

  ksapi::ISymbols2DContainerPtr symContainer(view);
  if (!symContainer) { WriteResult(resultFile, false, "No symbols container"); return; }
  ksapi::ILineDimensionsPtr dims = symContainer->GetLineDimensions();
  if (!dims)
  {
    WriteResult(resultFile, false, "Failed to get dimensions collection");
    return;
  }

  ksapi::ILineDimensionPtr dim = dims->Add();
  if (!dim)
  {
    WriteResult(resultFile, false, "Failed to create dimension");
    return;
  }

  dim->SetX1(x1);
  dim->SetY1(y1);
  dim->SetX2(x2);
  dim->SetY2(y2);
  dim->SetX3((x1 + x2) / 2.0);
  dim->SetY3(textY);
  dim->SetOrientation(static_cast<ksLineDimensionOrientationEnum>(orientation));
  dim->Update();

  WriteResult(resultFile, true, "Linear dimension created");
}


// ---------------------------------------------------------------------------
// Command dispatcher
// ---------------------------------------------------------------------------

static void ExecuteCommand(const std::string & cmdFile, const std::string & resultFile)
{
  std::string json = ReadFile(cmdFile);
  if (json.empty())
  {
    WriteResult(resultFile, false, "Failed to read command file or file is empty");
    return;
  }

  std::string command = JsonGetString(json, "command");

  if (command == "create_document")       CmdCreateDocument(json, resultFile);
  else if (command == "create_line")      CmdCreateLine(json, resultFile);
  else if (command == "create_circle")    CmdCreateCircle(json, resultFile);
  else if (command == "create_arc")       CmdCreateArc(json, resultFile);
  else if (command == "create_rectangle") CmdCreateRectangle(json, resultFile);
  else if (command == "create_text")      CmdCreateText(json, resultFile);
  else if (command == "create_polyline")  CmdCreatePolyline(json, resultFile);
  else if (command == "create_linear_dimension") CmdCreateLinearDimension(json, resultFile);
  else if (command == "fill_drawing_stamp") CmdFillDrawingStamp(json, resultFile);
  else if (command == "save_document")    CmdSaveDocument(json, resultFile);
  else if (command == "export_to_dxf")    CmdExportToDxf(json, resultFile);
  else if (command == "screenshot_document") CmdScreenshotDocument(json, resultFile);
  else if (command == "get_active_document") CmdGetActiveDocument(json, resultFile);
  else if (command == "list_objects")     CmdListObjects(json, resultFile);
  else
    WriteResult(resultFile, false, "Unknown command: " + command);
}


// ---------------------------------------------------------------------------
// Plugin entry points
// ---------------------------------------------------------------------------

void RunCommand(unsigned int commandId, ksapi::ksRunCommandModeEnum mode)
{
  // Read command and result file paths from environment
  const char * cmdFileEnv = std::getenv("KOMPAS_MCP_CMD_FILE");
  const char * resultFileEnv = std::getenv("KOMPAS_MCP_RESULT_FILE");

  if (!cmdFileEnv || !resultFileEnv)
  {
    // No command file specified - nothing to do
    if (kompasApp)
      kompasApp->ShowMessageBox(L"MCP Bridge: no command file specified", L"MCP Bridge",
                                ksMessageWarning, ksButtonSetOk, true);
    return;
  }

  ExecuteCommand(std::string(cmdFileEnv), std::string(resultFileEnv));
}


APP_EXP_FUNC(bool) LoadKompasLibrary(ksapi::IApplication & app, ksapi::IKompasLibraryActions & libraryActions)
{
  // Debug: log that we were loaded
  {
    std::ofstream dbg("/tmp/mcp_bridge_debug.log", std::ios::app);
    dbg << "LoadKompasLibrary called!" << std::endl;
  }

  libraryActions.AddRunCommandHandler(RunCommand);
  kompasApp = &app;

  // Auto-execute command on load if env vars are set
  const char * cmdFileEnv = std::getenv("KOMPAS_MCP_CMD_FILE");
  const char * resultFileEnv = std::getenv("KOMPAS_MCP_RESULT_FILE");
  if (cmdFileEnv && resultFileEnv)
  {
    ExecuteCommand(std::string(cmdFileEnv), std::string(resultFileEnv));
  }

  return true;
}


APP_EXP_FUNC(void) UnloadKompasLibrary()
{
}

// ---------------------------------------------------------------------------
// Background thread: waits for kompasApp then executes command
// ---------------------------------------------------------------------------
#include <thread>
#include <chrono>

static bool TryGetAppFromSymbol(); // forward declaration

// Flag: command needs execution from main thread
static bool g_needsExecution = false;
static std::string g_cmdFile;
static std::string g_resultFile;


static void BackgroundWaitForApp()
{
  std::ofstream dbg("/tmp/mcp_bridge_debug.log", std::ios::app);

  const char * cmdFileEnv = std::getenv("KOMPAS_MCP_CMD_FILE");
  const char * resultFileEnv = std::getenv("KOMPAS_MCP_RESULT_FILE");
  if (!cmdFileEnv || !resultFileEnv) return;

  g_cmdFile = cmdFileEnv;
  g_resultFile = resultFileEnv;

  dbg << "BackgroundWait: waiting for app..." << std::endl;

  for (int i = 0; i < 30; ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!kompasApp && i >= 8)
      TryGetAppFromSymbol();
    // Try calling ProcessLibRun to trigger LoadKompasLibrary
    if (!kompasApp && i == 15) // after 7.5s
    {
      typedef void (*ProcessLibRunFn)(const wchar_t*);
      void * plrSym = dlsym(RTLD_DEFAULT, "_ZN15MainApplication13ProcessLibRunEPKw");
      if (plrSym)
      {
        dbg << "BackgroundWait: calling ProcessLibRun('MCP Bridge')" << std::endl;
        auto processLibRun = reinterpret_cast<ProcessLibRunFn>(plrSym);
        processLibRun(L"MCP Bridge");
        dbg << "BackgroundWait: ProcessLibRun returned" << std::endl;
      }
    }

    if (kompasApp)
    {
      dbg << "BackgroundWait: kompasApp ready after " << (i+1)*500 << "ms!" << std::endl;
      std::this_thread::sleep_for(std::chrono::milliseconds(2000));
      ExecuteCommand(g_cmdFile, g_resultFile);
      dbg << "BackgroundWait: command executed!" << std::endl;
      return;
    }
  }
  dbg << "BackgroundWait: timeout" << std::endl;
  WriteResult(std::string(resultFileEnv), false, "Timeout waiting for KOMPAS");
}


// ---------------------------------------------------------------------------
// Try to get IApplication via KOMPAS internal symbol
// ---------------------------------------------------------------------------
#include <dlfcn.h>

static bool TryGetAppFromSymbol()
{
  std::ofstream dbg("/tmp/mcp_bridge_debug.log", std::ios::app);

  // GetItApplicationObject returns the internal app object
  // We need to cast it to ksapi::IApplication
  typedef void* (*GetItAppFn)();

  // Try to find the symbol in already-loaded libraries
  void * sym = dlsym(RTLD_DEFAULT, "_Z22GetItApplicationObjectv");
  if (!sym)
  {
    dbg << "TryGetApp: GetItApplicationObject not found" << std::endl;
    return false;
  }

  dbg << "TryGetApp: GetItApplicationObject found at " << sym << std::endl;

  auto getApp = reinterpret_cast<GetItAppFn>(sym);
  void * appObj = getApp();
  if (!appObj)
  {
    dbg << "TryGetApp: GetItApplicationObject returned null" << std::endl;
    return false;
  }

  dbg << "TryGetApp: got app object at " << appObj << std::endl;

  // GetKsAPIC returns a C-API wrapper, not IApplication directly
  // Use GetApplicationForIID to get the proper ksapi interface
  // Signature: void* MainApplication::GetApplicationForIID(unsigned int iid, int* result)
  // But IApplication has no IID, so we need another approach

  // Try AC::GetApplication (static member, simpler signature)
  typedef void* (*ACGetAppFn)(int);
  void * acGetAppSym = dlsym(RTLD_DEFAULT, "_ZN2AC14GetApplicationE11ApeApplType");
  if (acGetAppSym)
  {
    dbg << "TryGetApp: AC::GetApplication found" << std::endl;
    auto acGetApp = reinterpret_cast<ACGetAppFn>(acGetAppSym);
    for (int i = 0; i < 8; ++i)
    {
      void * app = acGetApp(i);
      if (app)
      {
        dbg << "TryGetApp: AC::GetApplication(" << i << ") returned " << app << std::endl;
        // Try to use as IApplication - test with a safe virtual call
        // IApplication first virtual method after IAPIObject should be safe to probe
        kompasApp = reinterpret_cast<ksapi::IApplication*>(app);
        return true;
      }
    }
  }

  // Try entry_points::GetKsAPIC
  typedef void* (*GetKsAPICFn)();
  void * ksApiSym = dlsym(RTLD_DEFAULT, "_ZN12entry_points9GetKsAPICEv");
  if (ksApiSym)
  {
    auto getKsApi = reinterpret_cast<GetKsAPICFn>(ksApiSym);
    void * ksApiObj = getKsApi();
    if (ksApiObj)
    {
      dbg << "TryGetApp: GetKsAPIC returned " << ksApiObj << " (NOT using as IApplication - different type)" << std::endl;
    }
  }

  dbg << "TryGetApp: could not obtain IApplication" << std::endl;
  return false;
}


__attribute__((constructor))
static void OnDlOpen()
{
  std::ofstream dbg("/tmp/mcp_bridge_debug.log", std::ios::app);
  dbg << "OnDlOpen: .rtw loaded" << std::endl;

  const char * cmdFileEnv = std::getenv("KOMPAS_MCP_CMD_FILE");
  if (cmdFileEnv)
  {
    dbg << "OnDlOpen: starting background executor thread" << std::endl;
    std::thread(BackgroundWaitForApp).detach();
  }
}
