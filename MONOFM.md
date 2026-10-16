# Felucca — сборка MONOFM

Третья прошивка для M-VAVE FM-1 из этого репозитория, рядом со звуковым модулем и STRANGE (darkmachine). Идея: **Monomachine от Elektron на FM-1** — четыре трека, у каждого своя машина, секвенсор с p-lock'ами, но машины свои: движки darkmachine и машины в духе Monomachine. Флаг сборки `FELUCCA_MONOFM=1` (вместе с `FELUCCA_MODULE=0`), пакет `felucca-1.5.1-monofm.fwsc`, установщик `fm1-monofm-installer.html`. Лицензия GPL-3.0, как у Felucca.

## Что это по устройству

Основа — грувбокс Felucca, а не звуковой модуль: возвращаются его секвенсор, REC, слой FX и страницы. Из движков Felucca не звучит ни один: в списке только движки darkmachine (позже к ним добавятся машины Monomachine). Каркас Felucca (экран, MIDI, установщик, хранение) остаётся.

**Треки.** Четыре, все звучат и слышат MIDI (каналы 1–4 по умолчанию, MENU > MIDI). По умолчанию T1 STRING, T2 MODAL, T3 LYRA, T4 ERROR (глитч-кит), паттерны пустые.

**Машины.** 27 штук, на трёх страницах слоя EDIT: зажать EDIT, белые клавиши с F3 грузят машину в выбранный трек, OCT+ — следующая страница, G5 — INIT.
- **Страница 1, MONO** — машины в духе Monomachine (13): SAW PULSE ENS SID · WAVE DDRAW BBOX · FM-ST FM-PAR FM-DYN · VO-6 SINE NOISE.
- **Страница 2, SOMA** (9): STRING MODAL BLOW RESO JUNK LYRA CHAOS DRIFT PLANT.
- **Страница 3, SLICE** (5): GLITCH ERROR RITUAL MECH BIO.

По умолчанию T1 SAW, T2 FM-ST, T3 STRING, T4 BBOX. Машины MONO в заводских звуках моно, как треки Monomachine; EDIT > VOICE POLY даёт до четырёх голосов (ENS — два, SID — три).

## Машины MONO

У каждой восемь параметров (EDIT 1 / EDIT 2), последний — TUNE (±100 центов), как у Monomachine. Имена и смысл параметров — по описанию машин в мануале Monomachine; код свой.

| Машина | Параметры | Что это |
|---|---|---|
| SAW (SWAVE SAW) | UNIL UNIW UNXL AGE · SUBX SUB1 SUB2 TUNE | пила, унисонная пара (уровень, ширина), расширенная пара вдвое шире, AGE — дрейф осцилляторов, саб-меандр −1 окт и синусы −1 и −2 окт |
| PULSE (SWAVE PULSE) | UNIL UNIW SUB1 SUB2 · PW PWAD PWRS TUNE | импульс и унисонная пара, ширина и её свип к «иголке» и обратно (PWRS — свип заново с каждой нотой), два саб-синуса |
| ENS (SWAVE ENSEMBLE) | PCH2 PCH3 PCH4 WAVE · PW CHRL CHRW TUNE | до четырёх высот (полутона или чистые 6/5 5/4 4/3 3/2): аккорд на моно-треке, p-lock'ами меняется по шагам; WAVE и PW — от пилы к меандру и иголкам; свой хорус |
| SID | WAVE PW PWAD MOD · MRAT GRIT NOIS TUNE | осциллятор в духе 6581: TRI SAW PULS NOIS и комбинированные (AND, как у чипа), RING / SYNC / R+S со вторым осциллятором (MRAT полутонов), GRIT — 12 бит вниз до 5 и удержание частоты, NOIS — подмешанный шум |
| WAVE (DIGIPRO WAVE) | WAVE MORF MSPD MRST · SYNC BITS DEC TUNE | 32 сырые 12-битные волны (синус…вокальные формы, FM, хаос), морф к соседней, свип по волнам, хард-синк до 4 октав, биты, сэмпл-рейт |
| DDRAW (DIGIPRO DOUBLEDRAW) | WAV1 WAV2 MIX MMOD · DTUN OCT2 BITS TUNE | две волны, вторая со сдвигом октавы и расстройкой, смесь и её LFO |
| BBOX (DIGIPRO BEAT BOX) | как у GLITCH | 12-битный драм-кит из 16 ударов: BD, BD LONG, SD, SD SNAP, CLAP, RIM, HH CL/OP, три тома, COWBELL, CLAVE, CYMBAL, SHAKER, ZAP; STUT/REP — ретриги |
| FM-ST (FM+ STATIC) | RAT2 DEP2 RAT3 DEP3 · FB ALGO MDEC TUNE | синус-носитель, два модулятора параллельно или стопкой, фидбек, MDEC — спад глубины (0 — держится) |
| FM-PAR (FM+ PARALLEL) | RAT2 LEV2 RAT3 LEV3 · RAT4 LEV4 MDEC TUNE | три модулятора параллельно |
| FM-DYN (FM+ DYNAMIC) | LIN LRAT EXP ERAT · FB PENV PDEC TUNE | линейная и экспоненциальная FM, фидбек носителя, огибающая высоты (кики, лазеры) |
| VO-6 | CONS CLEN CVOL VOW1 · VOW2 MORF BRTH TUNE | голос: согласная (19 штук и без: B D G P T K M N L R F S SH H V Z CH J TH) в гласную VOW1, переходящую в VOW2; три форманты |
| SINE (GND SINE) | FOLD SUB · TUNE | синус, вейвфолдер, синус октавой ниже |
| NOISE (GND NOISE) | RATE COLR · TUNE | шум, удержанный с частотой ноты (питчевый цифровой шум), цвет |

**Секвенсор — Felucca как есть:** 64 шага на трек (страницы по 16), длина у каждого трека своя; на шаге аккорд, accent, slide, tie, шанс, ратчет, nudge (микросдвиг), velocity; p-lock любого параметра (держать шаг на SEQ > STEP и крутить ручку); SEQ > AUTOMATION — список всех локов и событий; живая запись нот и ручек (REC + PLAY), пошаговая запись; swing общий на CLOCK (HOME трижды) и свой у трека в SEQ TOOLS; слой FX (зажать FX) — repeat, reverse, фильтр, tape stop, freeze и остальные punch-in эффекты Felucca.

**Эффекты.** На треке DIST, INSERT (драйвы, CRUSH, фейзер, флэнжер, хорус) и SLICER; общие посылы — хорус, дилей, ревер (HALL / ROOM / SPRING).

## Память и процессор

- Область движка на трек — 28 КиБ (у STRANGE 36 КиБ на две дорожки, у стока 12,6). Чтобы четыре таких влезли в пул, общий дилей укорочен до 1,11 с (было 1,49): 1/4 помещается с 54 BPM, 1/4D — с 81.
- В MONOFM у LYRA дилей до 0,56 с (в STRANGE 0,74), у DRIFT вторая петля 1,07 с (было 1,11). Остальные движки как в STRANGE.
- Машины MONO лёгкие: четыре трека по одной ноте — от ~900 (NOISE, SINE, WAVE) до ~1870 (ENS с аккордом и хорусом) инструкций на сэмпл.
- CPU (`tests/monofm_cpu.sh`, valgrind, инструкции на сэмпл на хосте, сравнимо с таблицей в STRANGE.md): четыре трека по паттерну — LYRA + DRIFT + RESO + STRING ≈ 2035, STRING + BLOW + LYRA + CHAOS ≈ 1220, четыре MODAL ≈ 870. Самый тяжёлый набор примерно равен PHYS на трёх голосах в стоке (1911). При перегрузке срабатывает штатная защита Felucca: гасит по голосу.

## Проверки

`tests/monofm_test.c`: показаны 27 машин, движки Felucca скрыты; три страницы EDIT; каждый заводской звук каждой машины MONO звучит, без клипа и DC, затихает после отпускания, простые питчевые — в строю; четыре трека слышат MIDI; p-lock шага срабатывает и возвращает базу; паттерн на четырёх треках (пять наборов движков, включая самый тяжёлый) — все треки звучат, без клипа и DC; заглушённый трек молчит. `WAVS=папка build/host/monofm_test` пишет миксы в WAV.

```
cc -O2 -w -DFELUCCA_MONOFM=1 -DFELUCCA_MODULE=0 -Ibuild/gen -Ifirmware/src -o build/host/monofm_test tests/monofm_test.c -lm
```

## План

1. **Каркас** — готово: 4 трека, секвенсор Felucca, только движки darkmachine, тёмная палитра.
2. **Машины Monomachine** — готово: 13 машин, страницы EDIT по семействам.
3. **Евклид на трек** (двойное нажатие SEQ, p-lock'и на его шагах), **XY на ARP глобально** (до 4 целей на ось, с любых треков), двойное нажатие HOME → XY, запись XY в REC.
4. **Страницы как у Elektron** (SEL / ENV / LFO / EDIT / GLO / FX), MICRO на трек, punch-in на выбранный трек, выбор машин на EDIT по страницам (не больше трёх), только chain без song.

## Ограничения

- **На железе пока не проверено.** Хост-тесты пройдены; сборщик CI проверяет RAM, пул и ROM.
- Стоковые проекты с движками Felucca откроются, их треки играют теми движками (номера сохранены), но выбрать такой движок заново нельзя.
