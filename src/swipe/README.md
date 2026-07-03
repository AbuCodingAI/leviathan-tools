# Swipe - Presentation Viewer (17KB)

Lightweight presentation viewer for LeviathanOS Pv3.2 built with GTK3 and WebKitGTK.

## Features

- **Slide Viewer**: Full-screen presentation display
- **Navigation**: Previous/Next buttons for slide navigation
- **Background Colors**: Per-slide background color customization
- **Minimalist UI**: Slide viewer with bottom control bar
- **No Animations**: Clean, simple slide transitions

## Build

```bash
cd swipe
make
```

Binary: `swipe` (17KB stripped, optimized)

## Usage

```bash
./swipe                      # Open with welcome slide
./swipe /path/to/pres.pptx   # Open presentation file
```

## Technical Details

- **Language**: C
- **Toolkit**: GTK3 + WebKitGTK 4.1
- **Display**: Full-screen slide container with controls
- **Optimization**: `-O3 -s` flags for aggressive size reduction

## Planned Features

- .pptx file format support
- Animations and transitions
- Speaker notes
- Custom fonts and layouts
- Image support

## Size Breakdown

- Swipe binary: 17KB (stripped) - smallest app!
- With symbols: ~45KB
- Compression potential: gzip -9 → ~4KB
