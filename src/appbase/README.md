# AppBase - Database Client (30KB)

Lightweight SQLite database client for LeviathanOS Pv3.2 built with GTK3 and WebKitGTK.

## Features

- **Database Browsing**: List all tables in SQLite database
- **Query Execution**: Run SELECT queries with result display
- **Table Sidebar**: Quick access to tables on left panel
- **Result Display**: HTML table rendering of query results
- **File Operations**: Open SQLite .db, .sqlite, .sqlite3 files

## Build

```bash
cd appbase
make
```

Binary: `appbase` (30KB stripped, optimized)

## Usage

```bash
./appbase                    # Open empty in-memory database
./appbase /path/to/data.db   # Open SQLite database file
```

## Technical Details

- **Language**: C
- **Toolkit**: GTK3 + WebKitGTK 4.1
- **Database**: SQLite3 (libsqlite3-dev)
- **Layout**: Two-panel UI (sidebar + main editor)
- **Optimization**: `-O3 -s` flags with linker GC

## Architecture

```
┌─────────────────────────────┐
│  Toolbar (Open/Refresh)     │
├──────────────┬──────────────┤
│              │              │
│ Tables List  │ Query Editor │
│              │              │
│              │ Query Result │
│              │   (Table)    │
└──────────────┴──────────────┘
```

## Planned Features

- INSERT/UPDATE/DELETE support
- Schema browsing and modification
- Export query results to CSV
- Query history
- Connection to remote databases

## Size Breakdown

- AppBase binary: 30KB (stripped)
- With symbols: ~80KB
- Compression potential: gzip -9 → ~7KB
