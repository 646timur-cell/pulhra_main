# План исправления: ALGORITHM knob и SETUP страница

## Проблема

Пользователь сообщает: "кноб алгоритм сейчас управляет громкостью. это неприемлимо. он ТОЛЬКО МЕНЯЕТ АКТИВНЫЙ ТРЕК. mode и src ДОЛЖНЫ УПРАВЛЯТЬ кноб1 и кноб2 соответственно"

Уточнение: "алгоритм меняет громкость имеено в setup = этого быть не должно. он должен менять активный трек"

## Текущее поведение (анализ кода)

### `sm_input.c` — `sm_input_poll()`:

```
стр. 971:  alg = panel_enc(EN_ALGO)     // ALGORITHM энкодер
стр. 972:  k[i] = panel_enc(EN_K1 + i)  // KNOB 1-4
стр. 971:  sel = panel_enc(EN_SELECT)   // SELECT энкодер
```

**ALGORITHM (alg):**
- стр. 1007-1012: `if (alg) { ... sm_ui.trk += alg ... }` — меняет активный трек ✅

**SETUP страница (стр. 1037-1040):**
```c
} else if (sm_page_home() == SMH_MIDI || sm_page_home() == SMH_SETUP) {
    if (k[0]) smp.mode = ...;           // KNOB 1 → MODE ✅
    if (k[1]) sm_midi_src = ...;        // KNOB 2 → SRC ✅
    if (sel) sm_turn_base(trk, P_LEVEL, sel);  // SELECT → LEVEL ❌
}
```

**Проблема:** SELECT на странице SETUP управляет громкостью (LEVEL) выбранного трека. Это лишнее — на SETUP странице не должно быть управления громкостью.

### `sm_draw.c` — `smd_home_knob()`:

```
стр. 421-438: SETUP/MIDI отображение:
  KNOB 1 → "MODE" (smp.mode) ✅
  KNOB 2 → "SRC" (sm_midi_src) ✅
  KNOB 3,4 → off ✅
  SELECT → не отображается в этом блоке
```

Отображение корректное — SELECT не показывается на SETUP странице.

## Изменения

### 1. `firmware/src/pulhra/sm_input.c` — строка 1040

**Было:**
```c
} else if (sm_page_home() == SMH_MIDI || sm_page_home() == SMH_SETUP) {
    if (k[0]) smp.mode = (uint8_t)((((int32_t)smp.mode + k[0]) % SM_PM_COUNT + SM_PM_COUNT) % SM_PM_COUNT);
    if (k[1]) sm_midi_src = (uint8_t)((((int32_t)sm_midi_src + k[1]) % 3 + 3) % 3);
    if (sel) sm_turn_base(sm_ui.trk % SM_NTRK, P_LEVEL, sel);
}
```

**Стало:**
```c
} else if (sm_page_home() == SMH_MIDI || sm_page_home() == SMH_SETUP) {
    if (k[0]) smp.mode = (uint8_t)((((int32_t)smp.mode + k[0]) % SM_PM_COUNT + SM_PM_COUNT) % SM_PM_COUNT);
    if (k[1]) sm_midi_src = (uint8_t)((((int32_t)sm_midi_src + k[1]) % 3 + 3) % 3);
}
```

Удалена строка `if (sel) sm_turn_base(...)` — SELECT больше не управляет громкостью на SETUP/MIDI страницах.

### 2. `firmware/src/pulhra/sm_input.c` — строка 1007-1012 (проверка)

ALGORITHM уже меняет только активный трек:
```c
if (alg) {
    int32_t t = ((int32_t)sm_ui.trk + alg) % (int32_t)SM_NTRK;
    sm_ui.trk = (uint8_t)(t < 0 ? t + (int32_t)SM_NTRK : t);
    sm_ui.held_step = sm_ui.sel_step = 0xFFu;
    sm_dirty = 1;
}
```
Изменений не требуется. ✅

### 3. `firmware/src/pulhra/sm_draw.c` — строка 421-438 (проверка)

Отображение SETUP/MIDI страницы уже корректное:
- KNOB 1 → "MODE" ✅
- KNOB 2 → "SRC" ✅
- KNOB 3,4 → off ✅

Изменений не требуется. ✅

## Итоговая схема работы SETUP страницы

```
┌─────────────────────────────────────────────┐
│  SETUP страница                              │
│                                             │
│  KNOB 1 → smp.mode (SEQ/DIR.S/DIR.J)       │
│  KNOB 2 → sm_midi_src (USB/INT/JACK)       │
│  KNOB 3 → off                               │
│  KNOB 4 → off                               │
│  SELECT → ничего (убрано)                    │
│  ALGORITHM → меняет активный трек            │
│  PRESETS → ничего                            │
└─────────────────────────────────────────────┘
```

## Файлы для изменения

1. `firmware/src/pulhra/sm_input.c` — удалить строку 1040 (`if (sel) sm_turn_base(...)`)