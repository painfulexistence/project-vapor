# Project Vapor — Docs Design System

All pages under `docs/` follow this system. When updating or adding a page, match these specs exactly.

---

## Color Palette

| Token | Hex | Usage |
|-------|-----|-------|
| `--bg` | `#0d0d0f` | Page background |
| `--bg-surface` | `#141418` | Card / panel background |
| `--bg-raised` | `#1c1c22` | Code blocks, table rows (alt) |
| `--border` | `#2a2a33` | All borders |
| `--text` | `#e2e2e8` | Body text |
| `--text-muted` | `#7a7a8c` | Secondary text, captions, timestamps |
| `--accent` | `#7c6af7` | Links, active states, accent marks |
| `--accent-dim` | `#3d3570` | Accent hover backgrounds |
| `--warn` | `#c97a2f` | Warnings, deprecated notices |
| `--ok` | `#4a9e6a` | Status: adopted, passing |
| `--danger` | `#9e4a4a` | Status: deprecated, failing |

---

## Typography

- **Font stack**: `'Berkeley Mono', 'JetBrains Mono', 'Fira Code', 'Cascadia Code', monospace`
- All text is monospace — code and prose share the same font, no serif/sans-serif
- Body: `15px / 1.75`
- Small / muted: `13px`
- No font loading from CDN — fall through the stack to system monospace

| Element | Size | Weight | Color |
|---------|------|--------|-------|
| `h1` | `22px` | `600` | `--text` |
| `h2` | `17px` | `600` | `--text` |
| `h3` | `15px` | `600` | `--text` |
| Body | `15px` | `400` | `--text` |
| Code inline | `13px` | `400` | `--text` on `--bg-raised` |
| Muted | `13px` | `400` | `--text-muted` |

---

## Layout

- Max content width: `860px`, centered
- Page padding: `48px 24px` (desktop), `24px 16px` (mobile, `< 640px`)
- No sidebar. Single-column.
- Nav sits at top, always visible, `--bg-surface` background, `1px` bottom border `--border`

---

## Nav

Every page includes the same top nav:

```
Project Vapor  ·  Architecture  Features  Tech Stack  ADRs
```

- Left: site title linking to `index.html`, dimmed (`--text-muted`)
- Right: page links, active page gets `--accent` color, no underline on hover — just color shift
- `12px` height total with `16px` vertical padding

---

## Components

### Cards / Sections
- `background: --bg-surface`
- `border: 1px solid --border`
- `border-radius: 4px`
- `padding: 24px`
- `margin-bottom: 24px`
- No box-shadow

### Tables
- Full-width
- Header row: `--bg-raised`, `--text-muted` label text, `11px`, uppercase, `0.08em` letter-spacing
- Body rows: alternate between `transparent` and `--bg-raised` (even rows)
- Cell padding: `10px 14px`
- Border: `1px solid --border` around table, `1px solid --border` between rows

### Code blocks
- `background: --bg-raised`
- `border: 1px solid --border`
- `border-radius: 3px`
- `padding: 16px`
- `font-size: 13px`
- `overflow-x: auto`
- No syntax highlighting — plain text, `--text` color

### Inline code
- `background: --bg-raised`
- `border-radius: 3px`
- `padding: 2px 6px`
- `font-size: 13px`

### Status badges
Used in ADR pages for decision status.

| Status | Background | Text |
|--------|-----------|------|
| Adopted | `--ok` (20% opacity bg) | `--ok` |
| Deprecated | `--danger` (20% opacity bg) | `--danger` |
| Proposed | `--warn` (20% opacity bg) | `--warn` |

- `border-radius: 3px`
- `padding: 3px 10px`
- `font-size: 12px`

### Horizontal rule / section dividers
- `border: none`
- `border-top: 1px solid --border`
- `margin: 32px 0`

---

## Page Structure (HTML skeleton)

Every page follows this structure exactly:

```html
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>[Page Title] — Project Vapor</title>
  <style>/* inline CSS — no external stylesheets */</style>
</head>
<body>
  <nav>...</nav>
  <main>
    <header class="page-header">
      <h1>[Page Title]</h1>
      <p class="muted">[subtitle or last-updated note]</p>
    </header>
    <!-- content -->
  </main>
</body>
</html>
```

- All CSS is inline in `<style>` — no external files, no CDN
- No JavaScript
- No images or icons
- Self-contained: each HTML file works when opened directly from the filesystem

---

## Voice and Tone

- English only
- Present tense ("the renderer uses", not "the renderer used")
- No marketing language
- Technical precision over completeness — say less, say it exactly
- Warnings and known gaps are stated plainly, not hedged
