# План: MIDI/SETUP страницы (pulhra) — ALGORITHM, громкость, каналы с OFF

## Проблема (сводка от пользователя, подтверждённая)

На HOME-страницах **MIDI** и **SETUP** (OCT- + HOME открывает SETUP; следующее нажатие HOME даёт MIDI):

1. **ALGORITHM выбирает активный трек** — поведение **оставить как есть** (везде, включая SETUP/MIDI).
2. При повороте ALGORITHM на экране **меняется отображаемая громкость (LEVEL)** — причина найдена: зона SELECT рисует `LEVEL <громкость выбранного трека>` на всех HOME-страницах (`smd_select`), включая MIDI/SETUP; поворот ALGORITHM перерисовывает её (sm_dirty) → значение LEVEL прыгает.
3. **KNOB 1..4 на SETUP «не работают»** — обработчик SETUP меняет модель без `sm_dirty`, экран не перерисовывается при остановленном транспорте.
4. **Новое требование:** на странице MIDI у каждой ручки (K1..K4 = T1..T4) должна быть **опция OFF** (0) — трек не принимает внешний MIDI ни по какому каналу. Сейчас страница MIDI показывает тот же MODE/SRC, что и SETUP, а редактирования канала трека нет вовсе.

## Корневые причины (анализ кода)

### A. «ALGORITHM меняет громкость» — LEVEL в зоне SELECT на HOME-страницах
`firmware/src/pulhra/sm_draw.c`, `smd_select()` (стр. 618-653) и `smd_sel_sig()` (стр. 601-617):
```c
} else if (sm_page_home()) {                 /* all HOME pages: SELECT is the selected track's volume */
    ... "LEVEL" + s->p[P_LEVEL] выбранного трека ...
```
Предыдущий план убрал *редактирование* LEVEL через SELECT на SETUP (`sm_input.c`), но не убрал *отображение* в `smd_select()` — поэтому LEVEL виден и меняется при смене трека через ALGORITHM. На MIXER/CLOCK это корректно (SELECT там редактирует громкость). На MIDI/SETUP — это «призрак».

### B. «KNOB не работают» — нет `sm_dirty` в ветке MIDI/SETUP
`firmware/src/pulhra/sm_input.c` (стр. 1037-1039): `smp.mode` / `sm_midi_src` меняются без `sm_dirty`. `sm_draw()` (стр. 960) при `!sm_dirty` и стоящем транспорте не перерисовывается → кольца MODE/SRC не обновляются.

### C. Страница MIDI не редактирует каналы
`sm_midi.c`: `static uint8_t sm_midi_ch[SM_NTRK] = {1, 2, 3, 4};` (стр. 28), роутинг (стр. 121-125):
```c
for (i = 0; i < SM_NTRK; i++)
    if (sm_midi_ch[i] == chan) { ch = i; break; }
```
`chan` = 1..16, поэтому `sm_midi_ch[i] = 0` **уже означает «не принимать ни по одному каналу»** — роутинг менять не нужно, достаточно дать UI значение 0 (OFF). UI канала трека отсутствует: `smd_home_knob` и ветка ввода трактуют MIDI так же, как SETUP (MODE/SRC).

## Изменения

### 1. `firmware/src/pulhra/sm_input.c` — разделить MIDI и SETUP, ставить `sm_dirty`

Ветку стр. 1037-1039 заменить на две:

```c
} else if (sm_page_home() == SMH_MIDI) {
    /* K1..K4: MIDI-канал T1..T4, 0 = OFF (не принимать ни по какому каналу), 1..16 = канал */
    for (i = 0; i < SM_NTRK; i++)
        if (k[i]) {
            int32_t v = (int32_t)sm_midi_ch[i] + k[i];      /* 0 OFF, 1..16: 17 позиций, круг */
            v = v % 17;
            if (v < 0) v += 17;
            if ((uint8_t)v != sm_midi_ch[i]) {
                sm_midi_ch[i] = (uint8_t)v;
                sm_dirty = 1;
                if ((uint8_t)v == 0u) sm_midi_release_req(i);   /* (п.4: снять удержанные ноты) */
            }
        }
} else if (sm_page_home() == SMH_SETUP) {
    /* K1: pattern change mode (SEQ/DIR.S/DIR.J), K2: midi source (USB/INT/JACK) */
    uint8_t w = (uint8_t)((((int32_t)smp.mode + k[0]) % SM_PM_COUNT + SM_PM_COUNT) % SM_PM_COUNT);
    if (k[0] && w != smp.mode) { smp.mode = w; sm_dirty = 1; }
    w = (uint8_t)((((int32_t)sm_midi_src + k[1]) % 3 + 3) % 3);
    if (k[1] && w != sm_midi_src) { sm_midi_src = w; sm_dirty = 1; }
}
```

(`i` уже есть в `sm_input_poll` как `uint32_t i`; SM_NTRK = 4.)

### 2. `firmware/src/pulhra/sm_draw.c` — отрисовка страниц

**`smd_home_knob()`** (сейчас стр. 421-439: общий блок для MIDI/SETUP) — разделить:

```c
if (sm_page_home() == SMH_MIDI) {                 /* K1..K4: T1..T4 MIDI-канал, OFF(0) + 1..16 */
    o->on = 1;
    if (k >= SM_NTRK) { o->on = 0; return; }
    o->lab[0] = 'T'; o->lab[1] = (char)('1' + k); o->lab[2] = 0;
    if (sm_midi_ch[k]) smd_int(o->val, (int32_t)sm_midi_ch[k], 0);
    else { o->val[0] = 'O'; o->val[1] = 'F'; o->val[2] = 'F'; o->val[3] = 0; }
    o->v = (int32_t)sm_midi_ch[k];
    o->pos = (uint32_t)sm_midi_ch[k] * 256u / 16u;
    o->dim = 0; o->inv = 0;
    return;
}
if (sm_page_home() == SMH_SETUP) {                /* K1 MODE, K2 SRC, K3/4 off (как сейчас, без MIDI) */
    ...текущий блок MODE/SRC...
    return;
}
```

**`smd_select()`** (стр. 630) — убрать LEVEL для MIDI/SETUP:
```c
} else if (sm_page_home() == SMH_EUCLID || sm_page_home() == SMH_MIDI || sm_page_home() == SMH_SETUP) {
    /* (nothing on SELECT for EUCLID / MIDI / SETUP) */
} else if (sm_page_home()) {
```

**`smd_sel_sig()`** (стр. 614-615):
```c
if (sm_page_home() && sm_page_home() != SMH_MIDI && sm_page_home() != SMH_SETUP)
    return smd_mix(h, (uint32_t)clamp(s->p[P_LEVEL], TP[P_LEVEL].min, TP[P_LEVEL].max));
```

### 3. `firmware/src/pulhra/sm_midi.c` — OFF канала

- Комментарий у `sm_midi_ch` (стр. 25-28): диапазон `0 OFF, 1..16`; `0` не совпадает ни с одним `chan` (1..16) → трек не принимает внешний MIDI.
- Роутинг (стр. 121-125) менять **не нужно** — 0 уже «не принимает».

### 4. `firmware/src/pulhra/sm_midi.c` — снятие удержанных нот при выключении канала (защита от «зависших» нот)

Если канал трека выключен (OFF) или изменён, пока внешние ноты на нём звучат, ноте-off по старому каналу уже не дойдёт (трека нет в матчинге). Добавить рукопожатие главный цикл → драйн (по образцу `sm_unl_pend`):

```c
static volatile uint8_t sm_midi_ch_release;       /* bit per track: its channel went OFF; the drain releases its MIDI notes once */
/* main loop (sm_input.c): a track's channel turned OFF */
void sm_midi_release_req(uint32_t t) { sm_midi_ch_release |= (uint8_t)(1u << (t % SM_NTRK)); }
```
В `sm_midi_drain()` в начале, рядом с обработкой mute:
```c
if (sm_midi_ch_release) {
    for (t = 0; t < SM_NTRK; t++)
        if ((sm_midi_ch_release >> t) & 1u) sm_midi_release(t);
    RING_PUBLISH();
    sm_midi_ch_release = 0;
}
```

## Итоговая схема

```
MIDI (HOME → MIDI)                SETUP (OCT- + HOME)
K1..K4: канал T1..T4,             K1: mode (SEQ/DIR.S/DIR.J)
        OFF(0) + 1..16            K2: src (USB/INT/JACK)
ALGORITHM: трек (как везде)       K3/K4: off
SELECT: ничего (без LEVEL)        ALGORITHM: трек (как везде)
                                  SELECT: ничего (без LEVEL)
```

## Файлы

1. `firmware/src/pulhra/sm_input.c` — раздельные ветки MIDI/SETUP + `sm_dirty` + вызов `sm_midi_release_req`.
2. `firmware/src/pulhra/sm_draw.c` — `smd_home_knob` (MIDI каналы), `smd_select`, `smd_sel_sig`.
3. `firmware/src/pulhra/sm_midi.c` — комментарий OFF; `sm_midi_release_req` + сброс в драйне.

## Верификация

1. Пересобрать firmware (`.fwsc`) и web-эмулятор (`web/emu/build_pulhra.sh`).
2. **SETUP** (OCT-+HOME): K1 циклически меняет MODE (SEQ→DIR.S→DIR.J), K2 меняет SRC (USB→INT→JACK); кольца обновляются при остановленном транспорте; ALGORITHM меняет шапку T1→T2, LEVEL на экране не меняется; зона SELECT пустая.
3. **MIDI** (следующий HOME tap): K1..K4 = канал T1..T4: OFF → 1..16 → OFF по кругу; при OFF внешний MIDI на каналах 1..16 трек не принимает; при канале 1..16 играет только на своём канале.
4. MIXER и CLOCK не затронуты: LEVEL через SELECT по-прежнему показывается и редактируется.
5. Хост-тест: `tests/pulhra_test.c` — сборка и прогон без регрессий.