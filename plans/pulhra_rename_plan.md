# Plan: Rename `pulhra` → `pulhra` — Simplified (Files & Directories Only)

## Scope

- **Rename files and directories** only (no `sm_` → `ph_` in code identifiers)
- **Update file paths** in includes, build scripts, configs
- **Update project/brand names** in docs, titles, package names
- **Do NOT touch** `sm_`/`SM_` prefixes in C code, `FELUCCA_SWAMP` flag stays as-is

---

## Phase 0: Git Branch

1. Create branch `beta_main_test` from current HEAD

## Phase 1: File & Directory Renames

| Old | New |
|-----|-----|
| `firmware/src/pulhra/` (dir) | `firmware/src/pulhra/` (dir, files keep `sm_` names) |
| `firmware/src/pulhra.c` | `firmware/src/pulhra.c` |
| `tests/pulhra_test.c` | `tests/pulhra_test.c` |
| `tests/pulhra_host.h` | `tests/pulhra_host.h` |
| `web/emu/pulhra_web.c` | `web/emu/pulhra_web.c` |
| `web/emu/build_pulhra.sh` | `web/emu/build_pulhra.sh` |
| `PULHRA.md` | `PULHRA.md` |
| `ui_desk_PULHRA.md` | `ui_desk_PULHRA MACHINE.md` |

## Phase 2: Content Updates (paths, names, titles — no code identifiers)

### 2.1 `firmware/src/pulhra.c`
- `pulhra/sm_` → `pulhra/sm_` in include paths
- `"PULHRA"` → `"PULHRA"` in comments

### 2.2 `tests/pulhra_test.c`
- `pulhra_host.h` → `pulhra_host.h`
- `pulhra/sm_app.c` → `pulhra/sm_app.c`
- `"pulhra:"` → `"pulhra:"` in printf
- `"PULHRA"` → `"PULHRA"` in comments
- `pulhra_` → `pulhra_` in shot filenames
- `FELUCCA_SWAMP` → `FELUCCA_PULHRA` in build command comment
- `build/host/pulhra_test` → `build/host/pulhra_test`

### 2.3 `tests/pulhra_host.h`
- `"PULHRA"` → `"PULHRA"` in comments
- `pulhra_test.c` → `pulhra_test.c`
- `pulhra/sm_app.c` → `pulhra/sm_app.c`

### 2.4 `web/emu/pulhra_web.c`
- `"PULHRA"` → `"PULHRA"` in comments
- `pulhra/sm_` → `pulhra/sm_` in include paths
- `build_pulhra.sh` → `build_pulhra.sh`
- `build/emu_pulhra/` → `build/emu_pulhra/`
- `pulhra.wasm` → `pulhra.wasm`

### 2.5 `web/emu/build_pulhra.sh`
- All `pulhra` → `pulhra` in paths, names, titles
- `"PULHRA"` → `"PULHRA"` in strings
- `firmware/src/pulhra.c` → `firmware/src/pulhra.c`

### 2.6 `tools/build.py`
- `"pulhra"` → `"pulhra"` in glob path (line 324)
- `"pulhra.c"` → `"pulhra.c"` in entry point (line 208)

### 2.7 `.github/workflows/firmware.yml`
- All `pulhra` → `pulhra` in names, paths, commands
- `FELUCCA_SWAMP` → `FELUCCA_PULHRA`
- `1.5.1` → `1.5.1`
- `pulhra` → `pulhra`
- `fm1-pulhra-installer.html` → `fm1-pulhra-installer.html`
- `build-pulhra.log` → `build-pulhra.log`
- `pulhra-1.5.1.fwsc` → `pulhra-1.5.1.fwsc`
- `"PULHRA"` → `"PULHRA"`

### 2.8 `web/make_module_installer.py`
- `PULHRA_TITLE` → `PULHRA_TITLE`: `"FM-1 PULHRA"` → `"FM-1 PULHRA"`
- `PULHRA_CREDIT` → `PULHRA_CREDIT`
- `"pulhra" in version` → `"pulhra" in version`
- `firmware/src/pulhra.c` → `firmware/src/pulhra.c`

### 2.9 `web/emu/worklet.js`
- `"PULHRA"` → `"PULHRA"` in comments and strings
- `pulhra_web.c` → `pulhra_web.c`

### 2.10 `web/emu/index.html`
- `" (PULHRA)"` → `" (PULHRA)"` (line 480)

### 2.11 `README.md`
- `# PULHRA` → `# PULHRA`
- `PULHRA.md` → `PULHRA.md`
- `fm1-pulhra-installer.html` → `fm1-pulhra-installer.html`
- `pulhra_test` → `pulhra_test`
- `"PULHRA"` → `"PULHRA"` in description
- `branch `pulhra`` → `branch `main``

### 2.12 `PULHRA.md` (was PULHRA.md)
- All `PULHRA` → `PULHRA`, `pulhra` → `pulhra`
- Package/installer/test command references updated

### 2.13 `ui_desk_PULHRA MACHINE.md`
- All `PULHRA` → `PULHRA`, `pulhra` → `pulhra`
- `firmware/src/pulhra/` → `firmware/src/pulhra/`

### 2.14 `LICENSING.md`
- `PULHRA` → `PULHRA`
- `firmware/src/pulhra/sm_font.c` → `firmware/src/pulhra/sm_font.c`

### 2.15 `firmware/src/voice.c`
- `"PULHRA"` → `"PULHRA"` in comments (lines 489, 493)

### 2.16 `firmware/src/eng_glitch.c`
- `"PULHRA NIGHT"` → `"PULHRA NIGHT"` (preset name, line 410)

### 2.17 `plans/*.md` and `docs/superpowers/plans/*.md` and `docs/superpowers/specs/*.md`
- All `firmware/src/pulhra/` → `firmware/src/pulhra/`
- All `PULHRA` → `PULHRA` in titles/descriptions
- All `pulhra` → `pulhra` in general references
- Package/installer/test command references updated

### 2.18 `build/site/index.html`
- `"Felucca"` → `"Pulhra"` in title and link

### 2.19 `build/site/webapp/try/index.html`
- `<title>FM-1 PULHRA</title>` → `<title>FM-1 PULHRA</title>`

### 2.20 `build/site/webapp/installer/index.html`
- `<title>Felucca</title>` → `<title>Pulhra</title>`
- `<h1>Felucca</h1>` → `<h1>Pulhra</h1>`

## Phase 3: Regenerate Build Artifacts

1. Clean old build artifacts (`build/emu_pulhra/`, `build/release-1.5.1/`, etc.)
2. Regenerate headers
3. Run host tests
4. Build firmware package
5. Build web emulator
6. Build installer

## Phase 4: Verification

1. Check no remaining `pulhra` references (except upstream Felucca code)
2. Verify host tests pass
3. Verify firmware builds

## Phase 5: Package

1. Commit to `beta_main_test` branch
2. Ready for push to new repo `pulhra_fw_main`