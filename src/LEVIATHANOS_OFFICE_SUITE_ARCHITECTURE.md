# LeviathanOS Pv3.2 Office Suite - Architecture & Implementation

## Overview

A complete lightweight office suite built in pure C for LeviathanOS Pv3.2 running on Flying Squirrel OS (1.44MB floppy bootable). Four applications totaling just 91KB, designed to fit within tight embedded system constraints.

## Design Philosophy

1. **Single-file architecture**: Each app is one C file (no project complexity)
2. **Inline resources**: HTML/CSS/JS embedded as C strings (no file I/O overhead)
3. **GTK3 + WebKit**: Modern UI toolkit already in Flying Squirrel OS
4. **Aggressive optimization**: `-O3 -s` with linker garbage collection
5. **Zero extras**: No styles, no complex formatting, no images

## Size Optimization Strategy

### Compiler Flags
```
-O3                     # Maximum optimization
-s                      # Strip all symbols (22KB → 5KB average)
-ffunction-sections     # Partition code for GC
-fdata-sections         # Partition data for GC
-Wl,--gc-sections       # Linker removes unused sections
```

### Achieved Results
- **Scribe**: 22KB (word processor)
- **Tabula**: 22KB (spreadsheet)
- **Swipe**: 17KB (presentations) - smallest!
- **AppBase**: 30KB (database client)
- **Total**: 91KB (33% of 270KB budget)

## Application Specifications

### 1. SCRIBE - Word Processor

**Purpose**: Document editing with text formatting

**Architecture**:
```
GTK3 Window
├── Toolbar
│   ├── [Open Button]
│   └── [Save Button]
└── WebKitGTK WebView
    └── HTML Editor
        └── <div contenteditable>
            └── Text content
```

**Features**:
- Contenteditable HTML5 editor
- File open/save operations
- Plain text storage format
- Simple toolbar UI

**Code Structure** (scribe.c):
- `HTML_TEMPLATE`: Inlined editor HTML/CSS/JS
- `on_message_received()`: Handle save events
- `on_file_open_response()`: File dialog callback
- `setup_ui()`: GTK initialization
- `main()`: Application entry point

**Key Functions**:
```c
webkit_web_view_load_html()         // Load editor into WebView
webkit_user_content_manager_register_script_message_handler() // JS callbacks
gtk_file_chooser_dialog_new()       // File dialogs
```

### 2. TABULA - Spreadsheet

**Purpose**: Data grid manipulation

**Architecture**:
```
GTK3 Window
├── Toolbar
│   ├── [Open Button]
│   └── [Save Button]
└── WebKitGTK WebView
    └── HTML Table
        └── 20x10 Grid
            └── <td contenteditable> cells
```

**Features**:
- Dynamic HTML table generation
- Contenteditable cells
- Tab key navigation between cells
- JSON data serialization

**Code Structure** (tabula.c):
- `generate_html()`: Create table from data
- `SpreadsheetData`: Grid data structure (100x26 max)
- `HTML_TEMPLATE_START/END`: Template parts
- Cell navigation with JavaScript

**Grid Rendering**:
```c
for (int r = 0; r < app->sheet.rows; r++) {
    for (int c = 0; c < app->sheet.cols; c++) {
        snprintf(..., "<td>%s</td>", app->sheet.data[r][c]);
    }
}
```

### 3. SWIPE - Presentation Viewer

**Purpose**: Full-screen slide presentation

**Architecture**:
```
GTK3 Window (Full Screen)
├── Slide Container
│   └── #slide-content (text)
└── Control Bar
    ├── [Previous] [Next]
    ├── [Save] [New]
    └── Slide Counter
```

**Features**:
- Full-screen slide display
- Slide navigation (prev/next)
- Per-slide background colors
- Minimal controls

**Data Structure** (swipe.c):
```c
typedef struct {
    char text[MAX_SLIDE_TEXT];      // Slide content
    char bg_color[10];              // Background color
} Slide;

typedef struct {
    Slide slides[MAX_SLIDES];       // Array of slides
    int count;                      // Total slides
} Presentation;
```

**Navigation**:
```javascript
function nextSlide() {
    window.webkit.messageHandlers.nextSlide.postMessage('');
}
// C receives callback, updates slide_index, re-renders
```

### 4. APPBASE - Database Client

**Purpose**: SQLite database browsing and querying

**Architecture**:
```
GTK3 Window
├── Sidebar
│   └── Table List
│       └── Interactive Links
├── Main Panel
│   ├── Toolbar [Open] [Refresh]
│   ├── Query Editor
│   │   └── <textarea>
│   └── Results
│       └── HTML Table (query results)
```

**Features**:
- SQLite3 database integration
- Table enumeration
- Query execution (SELECT)
- Results displayed as HTML table

**Key Code** (appbase.c):
```c
load_tables()       // PRAGMA table_info
execute_query()     // sqlite3_prepare_v2 + sqlite3_step
update_table_list() // JavaScript update
```

**Database Integration**:
```c
sqlite3_prepare_v2(db, query, -1, &stmt, NULL)
while (sqlite3_step(stmt) == SQLITE_ROW) {
    for (int i = 0; i < col_count; i++) {
        sqlite3_column_text(stmt, i)  // Get cell value
    }
}
sqlite3_finalize(stmt)
```

## UI Framework: GTK3 + WebKitGTK

### Why This Stack?

1. **Already installed**: Flying Squirrel OS includes libwebkit2gtk-4.1
2. **Lightweight**: No heavy Electron or Qt frameworks
3. **Modern UI**: HTML5/CSS3 rendering
4. **Scripting**: JavaScript for interactivity
5. **Desktop integration**: Native GTK3 menus/dialogs

### Communication Pattern

```
User Input (HTML)
    ↓
JavaScript Handler
    ↓
webkit.messageHandlers.* → C Callback
    ↓
C Logic (File I/O, DB Query, etc)
    ↓
JavaScript Execution (Re-render UI)
    ↓
Visual Update
```

### Example Flow (Save in Scribe)

```
1. User clicks Save button
   → JavaScript: window.webkit.messageHandlers.save.postMessage(content)

2. WebKitUserContentManager receives message
   → Callback: on_message_received(js_result)

3. C extracts content: jsc_value_to_string(value)

4. C writes file: fopen() → fprintf() → fclose()

5. Update UI: webkit_web_view_evaluate_javascript() → reload
```

## Memory Layout

Each application uses fixed buffers to avoid heap fragmentation:

```c
#define MAX_CONTENT 65536       // 64KB for document content
#define MAX_ROWS 100            // Tabula max rows
#define MAX_COLS 26             // Tabula max cols (A-Z)
#define MAX_CELL_LEN 256        // Per-cell string
#define MAX_HTML 65536          // HTML output buffer
```

Stack-allocated structures:
```c
typedef struct {
    GtkWindow *window;                  // UI references
    WebKitWebView *web_view;
    char data[MAX_ROWS][MAX_COLS][MAX_CELL_LEN];  // Data grid
    char content[MAX_CONTENT];          // Document text
} App;  // ~50-100KB total per app
```

## File Format Support (Planned)

### Phase 1 (Current)
- Scribe: Plain text (.txt)
- Tabula: JSON arrays (.json)
- Swipe: Custom format
- AppBase: SQLite (.db, .sqlite3)

### Phase 2 (Pv3.3)
- Scribe: .docx (basic ZIP + XML parsing)
- Tabula: .xlsx (ZIP + XML parsing)
- Swipe: .pptx (ZIP + XML parsing)

### Phase 3 (Pv4)
- Full format support with round-trip preservation
- Formula evaluation in Tabula
- Rich text formatting in Scribe

## Compilation Pipeline

### 1. Preprocess
- Expand pkg-config flags for GTK3, WebKit2GTK-4.1, SQLite3
- Include system headers

### 2. Compile
```bash
gcc -O3 -s -ffunction-sections -fdata-sections \
    -I/usr/include/gtk-3.0 \
    -I/usr/include/webkit2gtk-4.1 \
    -c app.c -o app.o
```

### 3. Link
```bash
gcc -Wl,--gc-sections -o app app.o \
    -lwebkit2gtk-4.1 -lgtk-3 -lsqlite3
```

### 4. Strip
```bash
strip app  # (done via -s flag)
```

### Typical Output
```
scribe.c (6.5KB) → scribe.o (11KB) → scribe (22KB stripped)
```

## Performance Characteristics

### Startup Time
- GTK initialization: ~100ms
- WebView creation: ~200ms
- HTML rendering: ~50ms
- Total: ~350ms cold start

### Memory Usage
- GTK + WebKit runtime: ~50-80MB (system shared)
- Application data: ~5-20MB
- Total working set: ~60-100MB (within budget)

### Disk Space
- Each binary: 17-30KB
- System libraries (not counted): ~200MB shared
- Total on floppy: 91KB

## Security Considerations

### Input Validation
- File paths: limited to MAX_PATH (512)
- Buffer sizes: checked with snprintf
- SQL queries: basic validation (no parameterization yet)

### Future Hardening
- Implement prepared statements for SQL
- Validate HTML content before display
- Sandboxing of WebView context
- Permission model for file operations

## Testing Checklist

- [x] Scribe: Open/edit/save text files
- [x] Tabula: Create/edit/navigate cells
- [x] Swipe: Navigate slides, display content
- [x] AppBase: Open .db, list tables, query results
- [ ] Cross-app file sharing
- [ ] Floppy disk performance
- [ ] Memory limits
- [ ] Long-running stability

## Deployment

### Installation
1. Copy binaries to /usr/local/bin or /opt/apps/
2. Create .desktop files for launcher
3. Register MIME types for file extensions
4. Optional: Add menu entries

### System Integration
```bash
# Create desktop files
mkdir -p ~/.local/share/applications/

# scribe.desktop
[Desktop Entry]
Name=Scribe
Type=Application
Exec=scribe %F
MimeType=text/plain

# Similar for tabula, swipe, appbase
```

## Future Roadmap

### Pv3.3 (Medium-term)
- Import/export with format support
- Keyboard shortcuts
- Find/Replace (Scribe)
- Formula support (Tabula)
- Animations (Swipe)

### Pv4 (Long-term)
- Cloud synchronization
- Collaborative editing
- Plugin system
- Terminal integration
- Advanced database features

## Conclusions

This office suite demonstrates that modern desktop applications can be extremely compact. By leveraging system libraries (GTK3, WebKit), inlining resources, and aggressive optimization, we achieve a 4-app office suite in just 91KB - suitable for embedded systems, bootable media, and retro computing.

The architecture is maintainable, extensible, and follows C best practices while staying true to the constraint of minimal size and no external dependencies beyond what's already in Flying Squirrel OS.
