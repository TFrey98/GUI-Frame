# Toolbox

A VS Code extension providing session-based terminal, database, reporting, and scripting tools.

## Status

Milestone 1: extension skeleton. No terminal or database functionality yet — commands are stubs.

## Layout

```
src/
  extension.ts   activation entry point
  core/          shared types, state, session identity
  terminal/      terminal session model and views
  database/      SQLite recording (future)
  reports/       report generation (future)
  scripts/       script execution (future)
  execution/     tool/command execution (future)
  tools/         tool integrations (future)
  views/         webviews / tree views
```

## Commands

- `Toolbox: Open`
- `Toolbox: New Session`
- `Toolbox: Show Sessions`
- `Toolbox: Show Diagnostics`

## Development

```bash
npm install
npm run compile
```

Press `F5` in VS Code to launch an Extension Development Host with the extension loaded.

## Testing

```bash
npm test
```
