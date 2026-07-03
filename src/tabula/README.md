# Tabula - Spreadsheet (22KB)

Lightweight spreadsheet application for LeviathanOS Pv3.2 built with GTK3 and WebKitGTK.

## Features

- **Grid Editor**: 20x10 cell grid with contenteditable cells
- **Navigation**: Arrow key support for cell navigation
- **Tab Navigation**: Tab key moves between cells
- **File Operations**: Save/Load spreadsheet data
- **No Formulas**: Data-only cells (formulas planned for Pv4)
- **Minimal UI**: Simple toolbar with Open/Save buttons

## Build

```bash
cd tabula
make
```

Binary: `tabula` (22KB stripped, optimized)

## Usage

```bash
./tabula                    # Open empty spreadsheet
./tabula /path/to/file.csv  # Open CSV data
```

## Technical Details

- **Language**: C
- **Toolkit**: GTK3 + WebKitGTK 4.1
- **Grid**: HTML `<table>` with contenteditable cells
- **Optimization**: `-O3 -s` flags for size reduction

## Planned Features

- Formula evaluation (Pv4)
- .xlsx file format support
- Sheet tabs/multiple sheets
- Column/row resizing
- Number formatting

## Size Breakdown

- Tabula binary: 22KB (stripped)
- With symbols: ~60KB
- Compression potential: gzip -9 → ~5KB
