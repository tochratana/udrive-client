# UDrive file-type icons

The four transparent PNGs are derived from the JPG icons supplied for UDrive.
They are embedded by `src/resources/client.qrc`; `UDriveIcons::fileIcon()` maps
file extensions to these assets in both table and tile views. Folders, PDFs,
videos, photo thumbnails and unknown-file fallbacks keep their existing icons.

| Asset | File types |
| --- | --- |
| `document.png` | DOC, DOCX, DOCM, DOT, DOTX, ODT, RTF |
| `spreadsheet.png` | XLS, XLSX, XLSM, XLSB, CSV, TSV, ODS |
| `presentation.png` | PPT, PPTX, PPTM, PPS, PPSX, ODP |
| `drawing.png` | SVG, ODG, OTG, DRAWIO, VSD, VSDX, DWG, DXF |

These indicate file type; they do not add an editor or previewer for those formats.
Replace an asset with a transparent PNG of the same name and rebuild to change it.

## Asset preparation

Prepared with the built-in imagegen tool using `transparent_background=true`,
one edit per supplied image (`doc.jpg`, `draw.jpg`, `excel.jpg`, `powerpoint.jpg`).
The original files remain unchanged. The prompt for each edit was the following,
with `<kind>` replaced by document, drawing, spreadsheet or presentation:

> Use case: background-extraction. Asset type: UDrive desktop <kind> file-type icon. Edit target: the supplied JPG. Remove only the external white background and its surrounding drop shadow; output one isolated icon on a genuinely transparent alpha background. Preserve the entire colored icon, its exact geometry, colors, layered gradients and internal white document/pencil glyph; do not make internal white regions transparent. Keep the icon upright, centered, with a small even transparent margin, square canvas. No text, no extra elements, no redesign.
