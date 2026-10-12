# Felucca — сборка «звуковой модуль»

Форк [Felucca 1.5.1](https://github.com/hugelton/Felucca) для M-VAVE FM-1. Это 4-тембральный звуковой модуль, которым управляют снаружи по MIDI. Лицензия GPL-3.0, как у оригинала.

Из тех же исходников собирается и вторая прошивка — **STRANGE** (`FELUCCA_STRANGE=1`): две дорожки и пять своих движков (LYRA, CHAOS, STRING, MODAL, BLOW). Подробно — в [STRANGE.md](STRANGE.md).

## Что изменено

**MIDI.** Режим MENU > MIDI > MIDI IN = **TRACK CH** включён по умолчанию: у каждой дорожки свой канал. Каналы задаются там же, в строках **T1 CH … T4 CH**, можно выбрать любой из 16. По умолчанию стоят 1–4. Настройка хранится в настройках прибора, а не в проекте. Если два трека стоят на одном канале, ноты получает первый из них. Режимы SEL и CH5-8 … CH13-16 работают как раньше.

**Транспонирование.** Параметр TRN дорожки (±24 полутона) сдвигает и клавиши, и входящие MIDI-ноты. Барабанные киты не транспонируются. Note-off всегда гасит именно ту ноту, которую включил note-on, даже если TRN успели поменять, пока нота звучала. При смене пресета TRN сохраняется.

**HOME.**
- KNOB 1–4 — громкость (LEVEL) T1–T4.
- Второе нажатие HOME открывает **TRANSPOSE**: KNOB 1–4 задают TRN для T1–T4.
- Третье нажатие — MIXER, четвёртое — CLOCK (BPM нужен для синхронизированных эффектов и LFO), пятое — снова HOME.
- Удержание HOME открывает меню.
- В MENU > DISPLAY > HOME можно выбрать, что показывать под ручками: строки дорожек (по умолчанию) или осциллограф.

**Убрано:** секвенсор (SEQ, DETAIL, AUTOMATION, SEQ TOOLS), ARP, SONG, PHRASES, запись (REC). Транспорт (PLAY или внешний клок) по-прежнему идёт, чтобы эффекты и LFO держали темп, но шаги не играются.

**Оставлено:** все движки и всё редактирование звука (EDIT, ENV, LFO, MOD, SCL/CHORD), все FX-страницы (INSERT, SLICER, DIST/CHO/DLY/REV) и FX-слой (repeat, freeze, tape stop, throw…). Слой GLO тоже на месте: mute, solo, TAP, MIDI LEARN. Пресеты по категориям: PRESETS > LIST (KNOB 4).

**GURDY** — колёсная лира из [GHOULBOX](https://github.com/jasonpersinger/ghoulbox-fm1-dungeon-synth) (Jason Persinger, GPL-3.0), движок №14. Шесть пресетов: HURDY GURDY, DRONE WHEEL, DANCE GURDY, TROMPETTE, VIELLE, BAGPIPE. В режиме COUP = CRANK ручка SELECT работает как рукоятка колеса. Дроны настроены в равной темперации: старинные строи GHOULBOX (TEMP) сюда не переносились.

Флаг сборки `FELUCCA_MODULE=0` возвращает обычную панель Felucca. С этим флагом гоняются оригинальные UI-тесты.

## Сборка

Проще всего через GitHub Actions: запушить репозиторий на GitHub, и workflow `.github/workflows/firmware.yml` сам скачает тулчейн JieLi и SDK, прогонит тесты и соберёт `felucca-1.5.1-mod.fwsc` (лежит в артефактах прогона). Если запушить тег `v…`, файл ещё и прикрепится к релизу.

Локально сборка описана в `BUILDING.md` (`./build.sh`).

Тесты на компьютере:

```
python3 -c "import sys; sys.path.insert(0,'tools'); import build; build.generate()"
cc -O1 -w -Ibuild/gen -Ifirmware/src -o build/host/module_test tests/module_test.c -lm && build/host/module_test
```

## Установка

Проще всего: открыть `fm1-sound-module-installer.html` (ветка `firmware-builds`, или `python3 web/make_module_installer.py build/felucca-1.5.1-mod.fwsc 1.5.1-mod out.html`) в Chrome или Edge, подключить FM-1 по USB и нажать Install. Прошивка уже внутри страницы, сервер не нужен.

Или из терминала:

```
pip3 install mido python-rtmidi
python3 tools/fm1_install.py felucca-1.5.1-mod.fwsc
```

Вернуться можно официальным апдейтером M-VAVE (V15) или установщиком Felucca («Return to official V15»).
