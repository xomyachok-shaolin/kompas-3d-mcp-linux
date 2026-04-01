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
  // The "params" section is the whole JSON for simplicity
  // (since our helpers search for keys in the full string)

  if (command == "create_document")       CmdCreateDocument(json, resultFile);
  else if (command == "create_line")      CmdCreateLine(json, resultFile);
  else if (command == "create_circle")    CmdCreateCircle(json, resultFile);
  else if (command == "create_rectangle") CmdCreateRectangle(json, resultFile);
  else if (command == "create_text")      CmdCreateText(json, resultFile);
  else if (command == "create_polyline")  CmdCreatePolyline(json, resultFile);
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
