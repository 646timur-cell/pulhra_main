# SWAMP MACHINE — этап 2: план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** По замечаниям Тима с железа (этап 1, сборка 59c8ccb): пиксельный интерфейс Elektron mk1 с кольцами-параметрами и заливкой локов, светодиоды, индикатор REC, удержания кнопок (слои), стабильные локи, страница STEP и длина трека, HOME, MIDI IN.

**Architecture:** Прежнее приложение `firmware/src/swamp/` на звуковом ядре MONOFM. Правило владения неизменно: главный цикл — единственный писатель `smp`, ISR только читает `smp` и владеет `trk[]`, транспортом и состоянием секвенсора. Локи — пул в паттерне; ISR при срабатывании трига пишет в `trk[t].p` значение лока или базовое из модели.

**Tech Stack:** C (unity build `swamp.c`), хост-тесты `tests/swamp_test.c`, CI `.github/workflows/firmware.yml` → `fm1-swamp-installer.html` в `firmware-builds`.

**Spec:** `docs/superpowers/specs/2026-10-16-monofm2-fork-design.md` (раздел 5 «Вид: пиксельный минимализм Elektron mk1», раздел 4 «Секвенсор и локи»).

## Global Constraints

- Ветка `swamp`; коммиты с трейлером из `.superpowers/sdd/2026-10-16-swamp-stage1-skeleton/context.md`; без force-push.
- Нет `#if FELUCCA_*` в `firmware/src/swamp/`; `ui*.c`, `seq.c`, `project.c`, `mf_pat.c`, `song_chain.c` не компилируются.
- Главный цикл — единственный писатель `smp`; ISR пишет только `trk[]`, транспорт, `sm_seq_*`. Публикация в модели, которую читает ISR: сначала поля, затем `SM_BARRIER()`, затем счётчик/флаг.
- RAM headroom ≥ 4096 B, pool headroom ≥ 8192 B (`build.py`).
- Вид mk1: один цвет чернил `0xFFFF` на `0x0000`, без сглаживания, зерна, потёков, градиентов; выделение — инверсией. Тема PIZDAKS (`sm_theme.c`, `cv_grime`, `gm_drip`) удаляется.
- Сообщения на экране только: «CLEARED», «UNDO», «SAVED», «LOCKS FULL», ошибка флеша.
- Удержание: кнопка нажата дольше `HOLD_MS[settings_hold]` (panel.c, по умолчанию 400 мс) **или** во время нажатия использована клавиша/ручка — это слой; короче и без действий — тап (страница). Так в Felucca/MONOFM.

## Review Focus

1. Удержание трига и поворот ручки во время игры на 300 BPM: лок пишется в шаг, который держишь, а не в играющий; триг при отпускании не переключается. Тест в Task 4.
2. Лок удаляется (снятие трига), пока ISR читает пул: ISR никогда не читает полузаписанную запись и не применяет лок удалённого шага. Тест в Task 4.
3. Пул полон (192 лока): новый лок не ставится, «LOCKS FULL» на 1 с, старые целы. Тест в Task 4.
4. Тап и удержание одной кнопки не путаются: быстрый тап EDIT листает страницу, удержание открывает выбор машины, нажатие клавиши при зажатом EDIT — тоже слой (даже до 400 мс). Тест в Task 3.
5. MIDI-нота во время игры секвенсора на том же треке не вешает ноты: note-off из MIDI снимает только свою ноту. Тест в Task 7.

---

### Task 1: Пиксельный интерфейс mk1, кольца, заливка локов, REC, пресет в шапке

**Files:**
- Create: `firmware/src/swamp/sm_font.c` — пиксельный шрифт 5×7 (ASCII 32–95 + `▶■` как коды 0x10/0x11), таблица `static const uint8_t SM_FONT[66][5]` (столбцы, бит 0 — верх), свой рисунок (без чужих шрифтов)
- Create: `firmware/src/swamp/sm_px.c` — примитивы поверх canvas: `sm_px_text(x, y, scale, s, ink, inv)`, `sm_px_ring(cx, cy, r, v01_256, filled, lockdot)`
- Modify: `firmware/src/swamp/sm_draw.c` — переписать раскладку на mk1; удалить `sm_theme.c` и всё зерно/потёки
- Modify: `firmware/src/swamp/sm_app.c` (включения), `tests/swamp_test.c`

**Interfaces:**
- Consumes: canvas `cv_begin/cv_rect`, `sm_ui`, `sm_cur_page`, `sm_desc_safe`, снимок `smd_pos/smd_play`, новые из Task 4 — `sm_lock_find(pat, t, s, param)` (до Task 4 — заглушка, возвращает -1), `sm_param_locked_any(t, param)`.
- Produces:
  - `static int32_t sm_px_text(int32_t x, int32_t y, uint32_t scale, const char *s, uint16_t ink, int inv)` → ширина в px; глиф 5×7, шаг 6×scale
  - `static void sm_px_ring(int32_t cx, int32_t cy, int32_t r, uint32_t pos256, int filled, int lockdot)` — 16 точек по дуге 270° (от 225° до −45°), точки от начала до `pos256` горят, остальные тусклые (`0x4208`); `filled` — сплошной диск; `lockdot` — точка 3×3 в центре
  - `static volatile uint8_t sm_rec` (взводится в Task 4 кнопкой REC)
  - Раскладка 240×240: шапка y 0–17 (scale 2): `T1 SAW 03` (трек, машина, номер пресета 2 цифры) слева, `A01 120.0 ▶` справа; при `sm_rec` — `REC` инверсией в правом углу; строка имени пресета и страницы y 20–37 (`EDIT 1 · SYNC LEAD`, scale 2); 4 кольца в ряд y 48–150 (r 22, подпись scale 2 над, значение scale 2 под); SELECT y 160–180 (подпись + значение scale 2); полоса шагов y 200–239 (16 клеток 14×30 с зазором 1: триг — заливка, пусто — рамка 1 px, играющий — инверсия клетки, зажатый — рамка 3 px)

- [ ] **Step 1: Write failing tests** `test_px_font_widths` (`sm_px_text` возвращает `6*scale*len`), `test_ring_dots` (на canvas ровно 16 точек кольца; при `pos256=128` горят 8), `test_ring_lock_fill` (с зажатым тригом и локом `P_FCUT` на этом шаге — кольцо CUT залито, остальные нет), `test_header_rec` (`sm_rec=1` → пиксели «REC» инвертированы), `test_draw_lint` и `test_draw_budget` — переписать под новую раскладку (текст в своих ячейках, смена шага ≤ 240×40 px на LCD), `test_no_grime` (на полном кадре только цвета `0x0000`, `0xFFFF`, `0x4208`).
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** `sm_font.c`, `sm_px.c`, новый `sm_draw.c`; удалить `sm_theme.c`; снимки `SHOTS` для всех страниц.
- [ ] **Step 4: Run** → PASS (и под `-fsanitize=address,undefined -fno-sanitize=shift`).
- [ ] **Step 5: Commit + push**, CI зелёный. `"SWAMP: Elektron mk1 pixel UI, knob rings, lock fill, REC, preset in the header"`

---

### Task 2: Светодиоды

**Files:**
- Create: `firmware/src/swamp/sm_leds.c` — `sm_leds()`
- Modify: `firmware/src/swamp/sm_main.c` (вызов в кадре; `boot_leds`-аналог: `fm1_led_dim_level(settings_leds == LEDS_DIM_LO)`), `sm_app.c`, `tests/swamp_test.c`

**Interfaces:**
- Consumes: `fm1_led[]`, `fm1_led_dim[]`, `fm1_led_breath[]`, `fm1_led_mid[]`, `FM1_NCOL`, `panel.btn[]`, `settings_leds` (режимы OFF / DIM LO / DIM HI / INV как в `ui_input.c ui_leds`), раскладку клавиш `14u + k` и `LED_PLAY_GREEN` — образец `firmware/src/ui_input.c:410-492` (механизм, не логику Felucca)
- Produces: `static void sm_leds(void)` — каждый кадр строит картинку оффлайн и копирует по столбцам (сначала dim, потом lit, как Felucca):
  - белые клавиши — три состояния шага, различимые с первого взгляда (решение Тима 2026-10-17): **триг** (`n>0`, без `SMF_REST`) — горит ярко; **REST** (флаг `SMF_REST`: только локи, без ноты) — breath; **пусто** — тёмный, без dim-подсветки в любом режиме LEDS; играющий шаг — инверсия (горит пустой/REST, гаснет триг); зажатый — mid
  - **полоса шагов на экране — аскетичнее** (переделать `smd_strip` из Task 1): одна строка 16 знаков высотой ≤ 12 px вместо клеток 14×30: триг — залитый квадрат 8×8, REST — контур 8×8, пусто — точка 2×2, играющий шаг — черта 8×2 под знаком, зажатый — инверсия знакоместа; освободившееся место отдать кольцам/SELECT
  - чёрные клавиши: D#5 / F#5 — горит та страница шагов, что видна (только если длина трека > 16)
  - кнопки: семейство текущей страницы горит; кнопка открытого слоя (Task 3) — breath; REC горит при `sm_rec`; PLAY — зелёный при игре
  - остальное — dim по режиму `settings_leds`

- [ ] **Step 1: Write failing tests** `test_leds_three_states` (триг → lit, REST → breath и не lit, пусто → ни lit, ни dim, ни breath во всех режимах LEDS кроме INV), `test_strip_minimal` (знаки полосы по трём состояниям, высота полосы ≤ 12 px, бюджет смены шага ≤ 240×16), `test_leds_trigs` (триги на шагах 0 и 5 → горят LED белых клавиш 1 и 6; остальные белые не горят), `test_leds_playhead` (во время игры LED играющего шага инвертирован), `test_leds_rec_play` (`sm_rec` → LED REC; игра → бит `LED_PLAY_GREEN`), `test_leds_family` (страница FILTER → горит ENV).
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit + push**, CI зелёный. `"SWAMP: LEDs — trigs, playhead, page family, REC, PLAY"`

---

### Task 3: Удержания кнопок — слои EDIT, GLO, SEQ

**Files:**
- Create: `firmware/src/swamp/sm_layer.c` — распознавание тап/удержание и три слоя
- Modify: `firmware/src/swamp/sm_input.c` (кнопки семейств идут через слой), `sm_draw.c` (карта слоя поверх страницы: подписи клавиш scale 2), `sm_leds.c` (breath кнопки слоя), `tests/swamp_test.c`

**Interfaces:**
- Consumes: `HOLD_MS[settings_hold]`, `fm1_ticks`/`fm1_ms`, `sm_sound_from_preset`, `SMC_LOAD`, `smp.mute`, `sm_engine_of`, `SM_NMACHINE`
- Produces:
  - `enum { SML_NONE, SML_EDIT, SML_GLO, SML_SEQ };` `static uint8_t sm_layer;`
  - правило: нажатие кнопки семейства запоминает время; отпускание раньше `HOLD_MS` без действий → тап (как сейчас — следующая страница); `HOLD_MS` прошло или нажата клавиша / повёрнута ручка → слой открыт до отпускания кнопки
  - **EDIT держать:** белые клавиши с F3 — машины страницы по порядку (страницы как в MONOFM: MONO 13, SOMA 9, SLICE 5 — `engines.c` MF_PAGE / `ui_layer.c` MONOFM EDIT), OCT+ — следующая страница машин, G5 — INIT (пресет 0 текущей машины); загрузка: модель (`sm_sound_from_preset`, IRQ-окно как у пресета) + `SMC_LOAD`
  - **GLO держать:** чёрные 1–4 (F#3 G#3 A#3 C#4) — мьют T1–T4 (тогл `smp.mute`, бит на трек; ISR не запускает ноты замьюченного трека); белые F3–B3 — соло T1–T4 пока держишь
  - **SEQ держать:** K1 — длина выбранного трека 1..32 (`smp.pat[cur].kit.snd[t].len`), белая F3 тап — CLEAR трека (все `n=0`, локи трека удалены — после Task 4); остальное (паттерны, копирование) — этап 3
  - `sm_seq` уважает `smp.mute` (чтение маски один раз за блок)

- [ ] **Step 1: Write failing tests** `test_tap_vs_hold` (тап EDIT 100 мс → страница сменилась, слой нет; держать 500 мс → слой EDIT, страница прежняя; нажать клавишу через 50 мс удержания → слой EDIT, страница прежняя), `test_edit_layer_loads_machine` (EDIT + 4-я белая → машина 3 в модели и в ядре после блока), `test_glo_mute` (GLO + F#3 → T1 замьючен, его триги не звучат, другие звучат; ещё раз — размьючен), `test_seq_len` (SEQ + K1 +16 → длина 32, D#5/F#5 теперь работают и шаги 17–32 играют).
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit + push**, CI зелёный. `"SWAMP: hold layers EDIT (machines) GLO (mutes) SEQ (length, clear)"`

---

### Task 4: Локи

**Files:**
- Create: `firmware/src/swamp/sm_lock.c` — пул локов паттерна
- Modify: `sm_input.c` (жест удержания трига), `sm_seq.c` (применение), `sm_draw.c` (заливка), `sm_cmd.c` (если нужно), `tests/swamp_test.c`

**Interfaces:**
- Consumes: `sm_pattern_t.lock[SM_NLOCK]`, `nlock`, `sm_lock_t {ts = trk<<5|step, param, val}`
- Produces:
  - `static int32_t sm_lock_find(const sm_pattern_t *p, uint32_t t, uint32_t s, uint32_t param)` → индекс или -1
  - `static int sm_lock_set(uint32_t t, uint32_t s, uint32_t param, int32_t v)` — главный цикл: найти и обновить `val` (одна 16-битная запись); иначе добавить в конец: записать `ts/param/val`, `SM_BARRIER()`, `nlock++`; пул полон → 0 и «LOCKS FULL» 1 с
  - `static void sm_lock_clear_step(uint32_t t, uint32_t s)` — главный цикл: помечает записи шага `param = 0xFF` (надгробие; ISR пропускает); уплотнение пула только когда `!sm_playing`
  - `static int sm_param_locked_any(uint32_t t, uint32_t param)`
  - **ISR (`sm_seq`):** на срабатывании трига трека `t`: для каждого параметра с локом на этом шаге — `trk[t].p[param] = val` (через `sm_desc_safe` клип); параметры, залоченные на предыдущем сработавшем триге и не залоченные на этом, — вернуть базу из модели. Шаг с триггером без локов возвращает все ранее залоченные параметры трека к базе. Хранить маску «что сейчас залочено» на трек (`uint8_t locked[SM_NTRK][P_COUNT/8+1]` в состоянии секвенсора)
  - **Жест:** нажатие белой клавиши — шаг «зажат» (без тогла); поворот ручки/SELECT при зажатом шаге → `sm_lock_set` для параметра этой страницы (новое значение = текущий лок или база + дельта, клип), и **триг при отпускании не переключается**; отпускание без поворота → тогл (как раньше); снятие трига (тогл в выкл) → `sm_lock_clear_step`
  - **Отображение:** пока шаг зажат, кольца показывают значения лока (или базы, если лока нет) и залиты, если лок есть
  - **REC:** кнопка REC тогл `sm_rec`; при `sm_rec && sm_playing` поворот ручки без зажатого шага пишет лок на текущий играющий шаг трека, только если на нём есть триг (`n > 0`); база не меняется

- [ ] **Step 1: Write failing tests**
  - `test_lock_hold_turn`: держать шаг 4, K2 FILTER +10 → лок CUT на шаге 4 есть, база не изменилась, при отпускании триг остался включён.
  - `test_lock_applies`: триги на 0 и 4, лок CUT=20 на 4, база 100; играть → после шага 4 `trk.p[P_FCUT]==20`, после следующего шага 0 → 100.
  - `test_lock_during_play_300bpm`: 300 BPM, держать шаг 7 и крутить 50 детентов, пока секвенсор играет 4 такта → лок только на шаге 7, соседние шаги без локов.
  - `test_lock_stress`: 10 000 случайных `sm_lock_set`/`sm_lock_clear_step` на 4 треках во время игры (между блоками ISR) → каждый применённый в ISR лок соответствует модели на момент срабатывания, ни одного лока удалённого шага не применено, `nlock ≤ 192`.
  - `test_lock_full`: 192 лока → 193-й отклонён, `LOCKS FULL`, старые целы.
  - `test_retrig_clears_locks`: снять триг и поставить заново → у шага 0 локов.
  - `test_rec_records_on_trigs`: `sm_rec`, игра, крутить CUT → локи появились только на шагах с тригами.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS (и под санитайзерами).
- [ ] **Step 5: Commit + push**, CI зелёный. `"SWAMP: parameter locks — hold a trig and turn, REC onto trigs, stable during play"`

---

### Task 5: Страница STEP

**Files:**
- Modify: `sm_pages.c` (семейство SEQ: STEP), `sm_input.c`, `sm_draw.c`, `sm_seq.c` (VEL, LEN, COND, флаги, NUDGE), `tests/swamp_test.c`

**Interfaces:**
- Produces:
  - кнопка SEQ тап → страница STEP; выбранный шаг = последний зажатый (или удержание > 0,35 с)
  - K1 NOTE (C-1..G9 по `note[0]`), K2 VEL 1..127, K3 LEN (список MONOFM: TRK, 1/8, 1/4, 1/2, 3/4, 1, 1.5, 2, 3, 4, 6, 8, 16, INF — хранить индекс в `len`, в `sm_seq` перевести в 1/8 шага), K4 COND (1–100 %, затем 1ST, !1ST, PRE, !PRE, FILL, !FILL, A:B 1:2…8:8), SELECT NUDGE −8…+7 (1/16 шага), ручка PRESETS — FLAG по кругу: —, ACC, SLD, A+S, X2, X3, X4, AX2, AX3, AX4, REST, PSLD
  - секвенсор: VEL, LEN, COND (вероятность — свой LFSR в ISR; PRE — результат последнего условия трека; A:B — счётчик проходов трека; FILL — `sm_fill`, пока всегда 0), ACCENT (+ VEL до 127), SLIDE (`trk_note_on` с глайдом как в MONOFM seq), ратчет ×2..×4 внутри шага, REST (локи без ноты), PSLIDE — линейно к локам следующего трига за длину шага (шаг 1/32 шага, через `trk[t].p`), NUDGE — сдвиг старта
- Правки шага — только главный цикл (поля, барьер не нужен: ISR читает поля шага при `n>0` целиком в начале шага; правка между шагами атомарна по байтам; допустимо).

- [ ] **Step 1: Write failing tests** по одному на VEL, LEN (1/4 шага и INF), COND 50 % (≈ половина из 1000 проходов), A:B 2:4, PRE/!PRE, ACCENT, ратчет ×3 (3 note-on в шаге), REST (лок применён, нот нет), PSLIDE (значение на середине шага между двумя локами), NUDGE (+4 → старт через 1/4 шага).
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit + push**, CI зелёный. `"SWAMP: STEP page — note, vel, len, cond, flags, nudge"`

---

### Task 6: HOME

**Files:**
- Modify: `sm_pages.c`, `sm_input.c`, `sm_draw.c`, `tests/swamp_test.c`

**Interfaces:**
- Produces: семейство HOME (тап листает): **MIXER** — K1–K4 `P_LEVEL` треков T1–T4 (кольца с подписями T1..T4), **CLOCK** — K1 BPM (30.0–300.0, шаг 0,1, быстрый поворот ×10), K2 SWING (50–75 %); на HOME SELECT — темп (как в спеке); `smp.bpm10` пишет главный цикл, ISR читает раз за блок; SWING — сдвиг чётных шестнадцатых в `sm_seq`

- [ ] **Step 1: Write failing tests** `test_home_mixer` (K3 → `P_LEVEL` T3 в модели и ядре), `test_home_bpm_select` (SELECT +10 → 121.0), `test_swing` (SWING 66 → чётные шаги позже на 1/3 шага ±CTL).
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit + push**, CI зелёный. `"SWAMP: HOME — mixer, clock, tempo on SELECT, swing"`

---

### Task 7: MIDI IN, сборка, установщик

**Files:**
- Create: `firmware/src/swamp/sm_midi.c` — разбор `midi_in_q` (USB и TRS кладут туда, `usb.c:170`, `midi_uart.c`) в ISR в начале `events_block`
- Modify: `sm_seq.c`, `SWAMP.md`, `tests/swamp_test.c`

**Interfaces:**
- Produces: каналы 1–4 → T1–T4: note on/off (`trk_note_on/off` со своим учётом нот MIDI на трек, чтобы note-off снимал только MIDI-ноту), CC — нет (этап 5), clock — нет (этап 5); переполнение очереди сбрасывает флаг `midi_in_overflow`
- `SWAMP.md` — обновить: что работает в этапе 2, что нет (сохранение, паттерны, FX, ARP — этапы 3–5)

- [ ] **Step 1: Write failing tests** `test_midi_note_ch2` (note on ch2 → звучит T2; off → тишина), `test_midi_vs_seq_same_note` (секвенсор и MIDI одну ноту на T1: MIDI note-off не обрывает ноту секвенсора и наоборот — без зависших голосов через 400 блоков).
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit + push**, CI зелёный, `SOURCE_COMMIT` установщика = HEAD. `"SWAMP: MIDI IN notes on channels 1-4; stage 2 notes"`
