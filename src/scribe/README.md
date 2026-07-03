# Scribe - Word Processor (22KB)

Lightweight word processor for LeviathanOS Pv3.2 built with GTK3 and WebKitGTK.

## Features

- **Text Editing**: contenteditable HTML5 editor with basic text formatting
- **File Operations**: Open and save plain text documents
- **Minimal UI**: Simple toolbar with Open/Save buttons
- **No External Dependencies**: Uses only libgtk-3 and webkit2gtk-4.1 (already in system)

## Build

```bash
cd scribe
make
```

Binary: `scribe` (22KB stripped, optimized)

## Usage

```bash
./scribe                    # Open empty document
./scribe /path/to/file.txt  # Open existing file
```

## Technical Details

- **Language**: C
- **Toolkit**: GTK3 + WebKitGTK 4.1
- **Architecture**: Single window with WebView displaying HTML editor
- **Optimization**: `-O3 -s` flags for size reduction, linker garbage collection

## Future Enhancements

- Support for .docx files (basic format)
- Rich text formatting (bold, italic, underline)
- Find/Replace functionality
- Syntax highlighting for code

## Size Breakdown

- Scribe binary: 22KB (stripped)
- With symbols: ~60KB
- Compression potential: gzip -9 → ~5KB
