# SWAMP MACHINE — этап 1 (каркас): план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Отдельная сборка SWAMP MACHINE: звуковое ядро MONOFM (машины, голоса, эффекты) под новым приложением — модель проекта с китом в паттерне, простой секвенсор через очередь команд, страницы EDIT / GLO / ENV, шапка и полоса шагов в теме PIZDAKS, установщик из CI.

**Architecture:** Новый unity-файл `firmware/src/swamp.c` включает HAL, `libc/lcd/gfx`, звуковое ядро (`core.h engines.c params.c mod.c voice.c slicer.c fx.c`) с `FELUCCA_MONOFM=1` и сервисы (`usb.c midi_uart.c storage*.c ota*.c master.c panel.c`), затем новое приложение из `firmware/src/swamp/sm_*.c`. Ядро требует от приложения ровно одну функцию — `events_block(uint32_t n)` (проверено: без `seq.c` неразрешённым остаётся только она). Правда о звуке живёт в модели (`sm_project_t`); `trk[]` ядра — её отражение, которое пишет только аудиопоток из очереди команд.

**Tech Stack:** C (unity build), JieLi pi32v2 clang через `tools/build.py`, хост-тесты `cc -O2` + `-fsanitize=address,undefined`, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-16-monofm2-fork-design.md` (SWAMP MACHINE). Этот план — этап 1 из 7 (раздел 11 спека); этапы 2–7 получают свои планы после проверки этапа 1 на железе.

## Global Constraints

- Ветка `swamp`, от `monofm` (fb92683 + спек). `monofm` не трогаем, кроме `tools/build.py` и CI (общие).
- Флаги сборки SWAMP: `FELUCCA_SWAMP=1 FELUCCA_MONOFM=1 FELUCCA_MODULE=0`, вход `swamp.c`, пакет `felucca-1.5.1-swamp.fwsc`, установщик `fm1-swamp-installer.html`, лог `build-swamp.log`.
- `build.py`: RAM headroom ≥ 4096 B, pool headroom ≥ 8192 B — не ослаблять.
- В `firmware/src/swamp/` нет `#if FELUCCA_*` и нет вызовов из `ui*.c`, `seq.c`, `project.c`, `mf_pat.c`, `song_chain.c` — эти файлы в SWAMP не компилируются.
- Аудиопоток (`events_block`) — единственный писатель `trk[]`; главный цикл пишет только модель и очередь `sm_cmd_push`.
- Сообщения на экране только: «CLEARED», «UNDO», «SAVED», «LOCKS FULL», ошибка флеша (в этапе 1 — ни одного).
- Шапка, значения ручек и полоса шагов всегда читаемы; тема PIZDAKS: чёрный фон, текст «кость», акцент кроваво-красный, второстепенное ржавое, толстые рамки без скруглений, зерно ксерокопии только на фоне.
- Имена на экране латиницей (шрифты `AF_S/AF_M/AF_L` без кириллицы).

## Review Focus

1. Команды быстрее, чем ISR их съедает (крутишь ручку быстро во время игры): очередь полна → последнее значение параметра не теряется (коалесценция SET_PARAM по `{trk,param}`), триг-тоглы не теряются. Тест в Task 4.
2. Смена машины во время игры: старые голоса гаснут через фейд ядра (`engine_block`), не щёлкают, без зависших нот. Тест в Task 3.
3. Темп на краях (30 и 300 BPM) и длина трека 1 и 32: шаги не дрейфуют и не пропускаются за 64 такта. Тест в Task 4.
4. STOP во время звучащих нот с LEN = INF: все ноты отпущены за один блок. Тест в Task 4.
5. Перерисовка экрана при игре не мешает вводу: кадр PIZDAKS укладывается в бюджет (≤ 8 мс на хосте-эквиваленте, считаем пиксели: ≤ 1 полный кадр 240×240 на изменение). Тест в Task 6.

---

### Task 1: Сборка SWAMP — вход, флаги, хост-тест «ядро звучит тишиной»

**Files:**
- Create: `firmware/src/swamp.c` — unity: блок опций как в `felucca.c` (+ `FELUCCA_SWAMP 1`, `FELUCCA_MONOFM 1`, `FELUCCA_MODULE 0`), HAL, `libc.c lcd.c gfx.c`, ядро, `usb.c midi_uart.c`, `audio.c`, затем `swamp/sm_app.c`, `master.c`, `swamp/sm_main.c`
- Create: `firmware/src/swamp/sm_app.c` — пока только `static void events_block(uint32_t n) { (void)n; }` и подключения следующих задач
- Create: `firmware/src/swamp/sm_main.c` — `fm1_main()` по образцу `main.c`: boot, `lcd_init`, `panel_init`, `audio_init`, `usb_start`, `uart_midi_init`, цикл 15 мс: `fm1_wdt_feed`, `usb_retry`, `master_poll`, `sm_input_poll()`, `sm_draw()` (заглушки этой задачи), UBOOT по OCT−+OCT+ 5 с — перенести как есть; `fm1_cstart` — копия
- Create: `tests/swamp_host.h` — хост-пролог: как `tests/hostsim.c` строки 1–36 (libc, `fm1_in`, ядро), без `song_chain.c`/`seq.c`; HAL-заглушки ввода/LED как `tests/ui_test.c` (секция «HAL stubs»)
- Create: `tests/swamp_test.c` — `#include "swamp_host.h"`, затем `swamp/sm_app.c`; `check(name, ok)` как в `ui_test.c`; `main` гоняет тесты, печатает `swamp: N failed`
- Modify: `tools/build.py:193-210` — флаг `FELUCCA_SWAMP` в список; вход: `FW/"src"/("swamp.c" if os.environ.get("FELUCCA_SWAMP")=="1" else "felucca.c")` в обеих ветках компиляции (и в `size_fns.py`-ветке)
- Modify: `.github/workflows/firmware.yml` — шаг «Host tests (SWAMP)»: `cc -O2 -w -DFELUCCA_SWAMP=1 -DFELUCCA_MONOFM=1 -DFELUCCA_MODULE=0 -Ibuild/gen -Ifirmware/src -o build/host/swamp_test tests/swamp_test.c -lm && build/host/swamp_test`; шаг «Build (SWAMP)»: `FELUCCA_SWAMP=1 FELUCCA_MONOFM=1 FELUCCA_MODULE=0 ./build.sh --release 1.5.1-swamp 2>&1 | tee build-swamp.log`; артефакты и ветка `firmware-builds` — как у MONOFM, плюс `fm1-swamp-installer.html`; триггер `refs/heads/swamp`

**Interfaces:**
- Produces: `swamp.c` (порядок включений), `tests/swamp_host.h`, `check()`; `events_block` — заменяется в Task 4.

- [ ] **Step 1: Create branch** `git checkout -b swamp monofm`
- [ ] **Step 2: Write the failing test** `test_core_silent`: `mix_block(out, CTL)` 100 раз подряд без нот → все сэмплы `|x| <= 1`. Сама сборка теста без `seq.c`/`ui*.c` — проверка шва ядра.
- [ ] **Step 3: Run** `cc ... tests/swamp_test.c` → FAIL (нет `swamp/sm_app.c`).
- [ ] **Step 4: Implement** файлы выше.
- [ ] **Step 5: Run** host test → `swamp: 0 failed`; `FELUCCA_SWAMP=1 FELUCCA_MONOFM=1 FELUCCA_MODULE=0 ./build.sh` (в CI) → строка `ok image … RAM … pool` без ошибок headroom.
- [ ] **Step 6: Commit** `git commit -m "SWAMP: own entry swamp.c on the MONOFM sound core, host test, CI flavor"`; push, дождаться CI, проверить `git show FETCH_HEAD:build-swamp.log` в `firmware-builds`.

---

### Task 2: Модель проекта

**Files:**
- Create: `firmware/src/swamp/sm_model.h` — типы и константы
- Create: `firmware/src/swamp/sm_model.c` — `sm_project_init`, `sm_sound_from_preset`
- Modify: `firmware/src/swamp/sm_app.c` — включить `sm_model.c`
- Test: `tests/swamp_test.c`

**Interfaces:**
- Produces:
  - `SM_NTRK 4u`, `SM_NPAT 16u`, `SM_NSTEP 32u`, `SM_NLOCK 192u`, `SM_NCHAIN 16u`
  - флаги шага: `SMF_ACCENT 1u, SMF_SLIDE 2u, SMF_REST 4u, SMF_PSLIDE 8u`, ратчет `SMF_RAT(f) ((f) >> 4 & 3u)` (0..3 = ×1..×4)
  - `typedef struct { uint8_t note[4], n, vel, len, cond, flags; int8_t nudge; } sm_step_t;` (10 B; `n == 0` — триг выключен)
  - `typedef struct { uint8_t ts; uint8_t param; int16_t val; } sm_lock_t;` (`ts = trk << 5 | step`)
  - `typedef struct { uint8_t machine, preset, len, rsv; int8_t p[P_COUNT]; } sm_sound_t;` (`p` как в `mfp_trk_t`: −64..127)
  - `typedef struct { sm_sound_t snd[SM_NTRK]; } sm_kit_t;` (мастер-инсерты и компрессор — этап 4)
  - `typedef struct { sm_kit_t kit; sm_step_t step[SM_NTRK][SM_NSTEP]; uint16_t nlock; uint8_t mlen, mode; sm_lock_t lock[SM_NLOCK]; } sm_pattern_t;`
  - `typedef struct { sm_pattern_t pat[SM_NPAT]; uint16_t bpm10; uint8_t swing, cur, sel, nchain; uint8_t chain[SM_NCHAIN]; uint8_t mute; } sm_project_t;`
  - `static sm_project_t smp;` — половина паттернов в `.noinit` по образцу `mf_pat.c` (`MFP_LO`), если не влезает в `.bss` — решает Task 7 по логу
  - `static void sm_project_init(void)` — BPM 1200 (=120.0), swing 50, все шаги пусты, `len = 16`, кит по умолчанию T1 SAW, T2 FM-ST, T3 STRING, T4 BBOX (их нулевые пресеты), одинаковый во всех 16 паттернах
  - `static void sm_sound_from_preset(sm_sound_t *s, uint32_t machine, uint32_t preset)` — порт `ui.c apply_preset_to` + `set_engine_of` без UI: `TP[i].def` для `i < P_E0`, `e->presets[pi].e[]`, `P_E8`, ADSR, `P_ED_FLT`, F.ENV, `P_VOICE`, посылы `FX_DEF {0,24,28,36}`
  - `static uint32_t sm_engine_of(uint32_t machine)` — машина → `ENGI_*`: в этапе 1 это `eng_vis(machine)` (27 машин MONOFM); `SM_NMACHINE = NENG_SHOWN`

- [ ] **Step 1: Write failing tests** `test_model_init`: `sm_project_init()`; для всех `pp<16, t<4, s<32`: `smp.pat[pp].step[t][s].n == 0`; `smp.pat[pp].nlock == 0`; `smp.bpm10 == 1200`; `sm_engine_of(smp.pat[0].kit.snd[0].machine) == ENGI_MSAW`, `[1] == ENGI_MFMST`, `[2] == ENGI_STRING`, `[3] == ENGI_MBBOX`; `memcmp(&smp.pat[0].kit, &smp.pat[15].kit, sizeof(sm_kit_t)) == 0`. `test_sound_from_preset`: для каждой машины и каждого её пресета `p[P_E0+i] == presets[pi].e[i]` и `p[P_ATK] == env[0]`; `_Static_assert(sizeof(sm_step_t) == 10u)`.
- [ ] **Step 2: Run** → FAIL (нет `sm_model.h`).
- [ ] **Step 3: Implement** `sm_model.h/.c`.
- [ ] **Step 4: Run** → PASS, плюс прогон с `-fsanitize=address,undefined` → PASS.
- [ ] **Step 5: Commit** `"SWAMP: project model, kit in the pattern, sounds from presets"`

---

### Task 3: Кит → звуковое ядро

**Files:**
- Create: `firmware/src/swamp/sm_kit.c`
- Modify: `firmware/src/swamp/sm_app.c`
- Test: `tests/swamp_test.c`

**Interfaces:**
- Consumes: `sm_sound_t`, `sm_engine_of`, ядро: `trk[]`, `TP[]`, `trk_note_on/off`, `trk_all_off`, `eng_mem_claim`, `mix_block`
- Produces:
  - `static void sm_track_load(uint32_t t, const sm_sound_t *s)` — **только из ISR** (вызывается очередью): `trk[t].eng_req = sm_engine_of(s->machine)`, `trk[t].preset = s->preset`, `trk[t].p[i] = s->p[i]` для всех `i`; если машина сменилась — фейд ядра (`engine_block` сам гасит старые голоса) — это и есть кроссфейд спека (≈ 5 мс, проверить длину фейда `xf` и задокументировать в коде)
  - `static void sm_kit_boot(const sm_kit_t *k)` — до старта аудио: `sm_track_load` всех 4 + `trk[t].engine = eng_req; eng_mem_claim(&trk[t])`

- [ ] **Step 1: Write failing tests** `test_kit_sounds`: `sm_project_init(); sm_kit_boot(&smp.pat[0].kit);` для каждого трека: `trk_note_on(&trk[t], 60, 100)`, 40 блоков `mix_block` → пик > 200 и нет `|x| >= 32767`; `trk_all_off`, ещё 400 блоков → последние 10 блоков `|x| <= 2`. `test_machine_switch_no_click`: T1 играет ноту SAW, `sm_track_load(0, &snd_string)` посреди ноты → максимум разности соседних сэмплов на стыке ≤ 4× медианы разностей до смены; `voices_busy()` после 400 блоков без нот == 0.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** `sm_kit.c`.
- [ ] **Step 4: Run** → PASS (и под санитайзерами).
- [ ] **Step 5: Commit** `"SWAMP: kit to the sound core (ISR-side load, the core's fade on a machine change)"`

---

### Task 4: Очередь команд и простой секвенсор

**Files:**
- Create: `firmware/src/swamp/sm_cmd.c` — кольцо команд
- Create: `firmware/src/swamp/sm_seq.c` — `events_block`, транспорт, шаги
- Modify: `firmware/src/swamp/sm_app.c` — убрать заглушку `events_block`
- Test: `tests/swamp_test.c`

**Interfaces:**
- Consumes: `smp`, `sm_track_load`, `trk_note_on/off`, `FS`, `CTL`
- Produces:
  - `enum { SMC_PLAY, SMC_STOP, SMC_TRIG, SMC_PARAM, SMC_LOAD };`
  - `typedef struct { uint8_t kind, trk, step, param; int16_t val; } sm_cmd_t;`
  - `static int sm_cmd_push(sm_cmd_t c)` — главный цикл; кольцо 64, один писатель/один читатель, индексы `volatile uint8_t`; при заполнении `SMC_PARAM` с тем же `{trk,param}` перезаписывает ожидающий (коалесценция), иначе возвращает 0
  - `static void sm_cmd_drain(void)` — в начале `events_block`: применяет к модели и к `trk[]` (`SMC_TRIG`: тогл шага в `smp.pat[cur]`, нота `C3 + p[P_ROOT]`, vel 100, len 0 = GATE трека; `SMC_PARAM`: `smp...kit.snd[t].p[param] = val` и `trk[t].p[param] = val`; `SMC_LOAD`: `sm_track_load`)
  - `static void events_block(uint32_t n)` — drain, затем при игре: шаг 1/16 = `FS * 600u * 10u / (smp.bpm10 * 4u)` сэмплов в фиксированной точке (накопитель q8, без дрейфа), каждый трек со своей `len`; на начале шага с `n > 0` и не `SMF_REST` → `trk_note_on` нот, note-off по GATE; `SMC_STOP` → `trk_all_off` всех и позиция 0
  - `static uint32_t sm_seq_pos(uint32_t t)` — играющий шаг трека (для UI), `static volatile uint8_t sm_playing`

- [ ] **Step 1: Write failing tests**
  - `test_seq_timing`: 120 BPM, трек 0 триги на шагах 0 и 4, PLAY; гонять `mix_block` и логировать note-on (обёртка-счётчик вокруг `trk_note_on` через `#define` в тесте до включения `sm_seq.c`) → note-on на сэмплах 0 и `4 * FS/8` с допуском `CTL`.
  - `test_seq_tempo_edges`: 30 и 300 BPM, `len` 1 и 32, 64 такта → число note-on ровно `64 * 16 / len * hits`, последний — на ожидаемом сэмпле ±`CTL`.
  - `test_cmd_coalesce`: заполнить очередь 64 `SMC_TRIG` (без drain), затем 200 `SMC_PARAM(0, P_FCUT, v=0..199)` → после drain `trk[0].p[P_FCUT] == 199`, все 64 трига применены.
  - `test_trig_while_playing`: во время игры `SMC_TRIG` шага 7 → меняется только шаг 7 (`memcmp` остальных шагов до/после).
  - `test_stop_releases`: ноты с LEN=INF (len 255) звучат, `SMC_STOP` → после одного блока `voices_busy()` гаснущие только (`trk[t].v[i].stage == 4` или выкл), через 400 блоков — тишина.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** `sm_cmd.c`, `sm_seq.c`.
- [ ] **Step 4: Run** → PASS (и под санитайзерами).
- [ ] **Step 5: Commit** `"SWAMP: command queue into the audio side, a plain step sequencer"`

---

### Task 5: Ввод — страницы, трек, триги

**Files:**
- Create: `firmware/src/swamp/sm_pages.c` — таблица страниц этапа 1
- Create: `firmware/src/swamp/sm_input.c` — `sm_input_poll()`
- Modify: `firmware/src/swamp/sm_app.c`, `sm_main.c`
- Test: `tests/swamp_test.c` (HAL-заглушки `host_pressed`, `host_notes`, `host_enc[]` из `tests/swamp_host.h`)

**Interfaces:**
- Consumes: `panel.c` (`panel.btn[]`, `panel_enc(EN_*)`, `B_*`), `TP[]`/`param_desc_of`, `sm_cmd_push`
- Produces:
  - `typedef struct { const char *name; uint8_t fam; uint8_t id[4]; uint8_t sel; } sm_page_t;` (`sel` — id параметра на SELECT, `0xFF` нет)
  - `static const sm_page_t SM_PAGES[]` этапа 1: `EDIT 1 {P_E0..P_E3} sel P_E8`, `EDIT 2 {P_E4..P_E7} sel P_E8`, `VOICE {P_VOICE,P_GLIDE,P_DETUNE,P_PRIO}`, `AMP {P_ATK,P_DEC,P_SUS,P_REL} sel P_ESYNC`, `MIX {P_LEVEL,P_PAN,P_DIST,P_REV}`, `FILTER {P_FLT,P_FCUT,P_FRES,P_ED_FLT} sel P_HWIDTH`, `F.ENV {P_FATK,P_FDEC,P_ED_FLT,0xFF}`; семейства по кнопкам EDIT / GLO / ENV, повторное нажатие — следующая страница семейства
  - `static struct { uint8_t page, trk, held_step, bank; } sm_ui;`
  - ALGORITHM — трек 0..3 (по кругу); PRESETS — пресет трека: строит `sm_sound_t` через `sm_sound_from_preset` и шлёт `SMC_LOAD` (в модель — сразу, в ядро — через очередь)
  - ручки K1–K4 и SELECT: значение в модели ± шаг по `TP`, клип по диапазону, `SMC_PARAM`
  - белые клавиши F3…G5 (по `FM1_KEYMAP`, без чёрных) на страницах звука: 16 шагов страницы `bank` → `SMC_TRIG`; D#5 / F#5 — `bank` 0 / 1 (как MONOFM.md); удержание шага пока только запоминается в `held_step` (локи — этап 2)
  - `static uint32_t sm_page_count(void)`, `static const sm_page_t *sm_cur_page(void)`

- [ ] **Step 1: Write failing tests**
  - `test_alg_selects_track`: 5 детентов ALGORITHM → `sm_ui.trk == 1`.
  - `test_knob_writes_model_and_core`: страница FILTER, K2 +10 детентов → `smp.pat[cur].kit.snd[sel].p[P_FCUT]` выросло и после одного `mix_block` равно `trk[sel].p[P_FCUT]`; клип на `TP[P_FCUT].max`.
  - `test_white_key_toggles_trig`: клавиша 3-й белой → `step[sel][2].n == 1`; ещё раз → 0; на `bank` 1 — шаг 18.
  - `test_presets_knob_loads`: PRESETS +1 → `kit.snd[sel].preset == 1`, после блока `trk[sel].preset == 1`.
  - `test_pages_cycle`: EDIT ×3 → EDIT 1 → EDIT 2 → VOICE → EDIT 1.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** `sm_pages.c`, `sm_input.c`; в `sm_main.c` — вызов в цикле.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `"SWAMP: pages EDIT GLO ENV, track on ALGORITHM, presets, trigs on the white keys"`

---

### Task 6: Экран — шапка, параметры, полоса шагов, тема PIZDAKS

**Files:**
- Create: `firmware/src/swamp/sm_theme.c` — `sm_theme_pizdaks()`: заполняет `ux` (`gfx.c`), зерно фона через `ux.grime` (механизм `cv_grime` от STRANGE DARK, статичный)
- Create: `firmware/src/swamp/sm_draw.c` — `sm_draw()`
- Modify: `firmware/src/swamp/sm_app.c`, `sm_main.c`
- Test: `tests/swamp_test.c` (PPM-снимки как `SHOTS=` в `monofm_test.c`)

**Interfaces:**
- Consumes: `cv_begin/cv_rect/cv_text_in/cv_text_on`, `AF_S/AF_M/AF_L`, `sm_cur_page`, `param_desc_of`, `sm_seq_pos`, `sm_playing`
- Produces:
  - Цвета PIZDAKS (RGB565): `bg 0x0000`, `surf 0x0841`, `text 0xEF5B` (кость), `theme 0xEF5B`, `accent 0xB800` (кровь), `mid 0x8A22` (ржавчина), `dim 0x4100`, `rec 0xF800`
  - Раскладка 240×240: шапка y 0–21 (`T1 SAW · A01 · 120.0 · ▶/■ · REC`, AF_S, рамка 2 px снизу), имя страницы y 24 (AF_M капсом, тень +1/+1 цветом `dim`), 4 ячейки параметров 2×2 y 48–175 (подпись AF_S `mid`, значение AF_L `text`), SELECT y 178–197 (подпись + значение AF_S), полоса шагов y 202–239: 16 клеток 15×30 (триг — заливка `text`, пусто — рамка `dim`, играющий шаг — рамка 2 px `accent`, страница шагов — `1/2` справа в шапке)
  - `static void sm_draw(void)` — перерисовка по флагу `sm_dirty` и при смене играющего шага; рисует только изменённые зоны (шапка, параметры, полоса) отдельными `cv_begin` полосами
  - `static uint32_t sm_draw_px` — счётчик залитых пикселей за кадр (для теста бюджета)

- [ ] **Step 1: Write failing tests**
  - `test_draw_lint`: для каждой страницы этапа 1 и для значений `min`/`max`/`def` каждого параметра — текст не выходит за свою ячейку (используй `GFX_HOOK_*` линт из `tests/ui_render.c`, режим без палитр Felucca).
  - `test_draw_budget`: играющий шаг сменился, больше ничего → `sm_draw_px <= 240 * 40`; смена страницы → `<= 240 * 240`.
  - `test_draw_shots`: `SHOTS=dir` пишет `swamp_<page>.ppm` для всех страниц (для ручной проверки, не assert).
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** `sm_theme.c`, `sm_draw.c`.
- [ ] **Step 4: Run** → PASS; посмотреть снимки, приложить 2–3 к сообщению Тиму.
- [ ] **Step 5: Commit** `"SWAMP: header, page, step strip in PIZDAKS"`

---

### Task 7: Транспорт, проверка на железе, установщик

**Files:**
- Modify: `firmware/src/swamp/sm_input.c` — кнопка PLAY: `SMC_PLAY` / `SMC_STOP` по очереди
- Modify: `firmware/src/swamp/sm_model.c` — размещение `smp` (`.noinit`/`.bss`) по логу сборки
- Create: `SWAMP.md` — что есть в этапе 1, как ставить, чего пока нет (по образцу `MONOFM.md`, коротко)
- Test: `tests/swamp_test.c`

**Interfaces:**
- Consumes: всё выше.
- Produces: установщик `fm1-swamp-installer.html` в ветке `firmware-builds`.

- [ ] **Step 1: Write failing test** `test_play_key`: кнопка PLAY → `sm_playing == 1` после блока; ещё раз → 0 и тишина через 400 блоков.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Build** в CI; в `build-swamp.log` строка `ok image … RAM … pool` без нарушений порогов. Если RAM не хватает — первые 7 паттернов в `.noinit`, остальные в `.bss` (как `MFP_LO` в `mf_pat.c`), с `_Static_assert` на размер `.noinit` из `app.ld`.
- [ ] **Step 6: Commit + push**, дождаться CI, отдать Тиму `fm1-swamp-installer.html` (`git show FETCH_HEAD:fm1-swamp-installer.html > …`, `SendUserFile`).
- [ ] **Step 7: Hardware check (Тим):** звук на 4 треках, ALGORITHM — трек, PRESETS — пресет, ручки, триги белыми клавишами во время игры, PLAY/STOP, экран читаем. Записать замечания в план этапа 2.

---

## Этапы 2–7 (свои планы после этапа 1)

2. Локи через очередь (`SMC_LOCK`), удержание трига на любой странице, поимка ручки, STEP (NOTE/VEL/LEN/COND, флаги на PRESETS: ACCENT SLIDE RATCHET REST PSLIDE), NUDGE, REC на триги, SEQ-слой, FILL (GLO + G5, и при зажатом FX), стресс-тест 10 000 локов.
3. 16 паттернов со сменой китов, цепочки, SAVE / PROJECT (ALGORITHM листает паттерны), хранение (2 копии + автосейв), CLEAR / UNDO.
4. FX, DIST, INSERT трека, два мастер-инсерта, SLICER с волнами WAVE, слой FX, компрессор и дакер, страницы HOME (MIXER, CLOCK, SCOPE, COMP).
5. ARP + RETRIG, EUCLID, SCL / CHORD / MICRO, мьюты на GLO, MIDI.
6. Машины: FM (слияние), FOREST, KICK, SNARE, DRUM; ERROR убрать.
7. Тема POLNYI PIZDAKC.
