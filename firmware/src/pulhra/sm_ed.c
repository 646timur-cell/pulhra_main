/* SPDX-License-Identifier: GPL-3.0-only */
/* PULHRA's editor link (Tim, 31.10: the web editor's: the user sounds' library, the user sample slot, the banks' backup):
 * SysEx over USB-MIDI, a few plain operations on the flash regions PULHRA keeps its data in; the page does the formats
 * (web/pulhra_editor.html: the banks, the records, the slot's header, the ADPCM).
 *
 *   F0 7D 50 48 cmd args .. F7          (7D: non-commercial; "PH"); the reply: F0 7D 50 48 cmd rc .. F7
 *   numbers: u21 = three 7-bit bytes, low first; data: pack7 (groups of up to 7 bytes, their top bits first)
 *
 *   1 INFO                    -> rc, version, the regions: count, then each id, base u21, size u21; the firmware's name
 *   2 READ   reg off len      -> rc, reg, off, len (u14), pack7 data (len <= 256)
 *   3 ERASE  reg off          -> rc, reg, off         (a 4 KiB sector, off on its start)
 *   4 WRITE  reg off pack7    -> rc, reg, off         (<= 256 bytes into erased flash; read back and compared)
 *   5 RELOAD what             -> rc                   (1: the banks and the project: the device restarts; 2: the user
 *                                                      sounds' index; 4: the sample slot rescanned)
 *   6 STATE                   -> rc, playing
 * rc: 0 done, 1 bad arguments, 2 busy (playing: ERASE / WRITE / RELOAD only while stopped: an erase stops the audio),
 * 3 the flash failed, 4 no flash. A READ works while playing (256-byte windows, the audio keeps up).
 * The regions: 0 PROJECT (both copies), 1 BANKS (the places), 2 PRESETS (the user sounds' two sectors), 3 SAMPLE (USR1's
 * 80 KiB: the three slots, packed: eng_sample.c smp_user_map) */

#define SM_ED_H0 0x7Du
#define SM_ED_H1 0x50u
#define SM_ED_H2 0x48u
#define SM_ED_VER 1u
enum { SM_ED_INFO = 1, SM_ED_READ, SM_ED_ERASE, SM_ED_WRITE, SM_ED_RELOAD, SM_ED_STATE };
enum { SM_ED_OK, SM_ED_BADARG, SM_ED_BUSY, SM_ED_FLASH, SM_ED_NOFLASH };
static const struct { uint32_t base, size; } SM_ED_REG[4] = {
    {SM_FL_PROJ, 2u * SM_FL_SEC},
    {SM_FL_SLOT0, SM_FL_NSLOT * SM_FL_SPAN},
    {SM_UP_SEC0, 2u * SM_FL_SEC},
    {SMP_USER_BASE, SMP_USER_SIZE},
};
_Static_assert(SM_FL_PROJ + 2u * SM_FL_SEC <= SM_FL_SLOT0 && SMP_USER_BASE + SMP_USER_SIZE <= SM_FL_LO,
               "the editor's regions apart");
#define SM_ED_OUT 320u                                    /* (a READ's reply: 256 bytes packed, 293, and its head) */
static uint8_t sm_ed_out[SM_ED_OUT] __attribute__((section(".pool")));
static uint8_t sm_ed_buf[256] __attribute__((section(".pool")));
static uint8_t sm_ed_reboot;                              /* RELOAD 1 answered: the main loop restarts the device */
static uint32_t sm_ed_n;

static void sm_ed_b(uint32_t v)
{
    if (sm_ed_n < SM_ED_OUT - 1u)
        sm_ed_out[sm_ed_n++] = (uint8_t)(v & 0x7Fu);
}
static void sm_ed_u21(uint32_t v) { sm_ed_b(v); sm_ed_b(v >> 7); sm_ed_b(v >> 14); }
static uint32_t sm_ed_ru21(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 7 | (uint32_t)p[2] << 14; }
static void sm_ed_pack7(const uint8_t *d, uint32_t n)
{
    uint32_t i, j;
    for (i = 0; i < n; i += 7u) {
        uint32_t top = 0, k = n - i < 7u ? n - i : 7u;
        for (j = 0; j < k; j++)
            top |= (uint32_t)(d[i + j] >> 7) << j;
        sm_ed_b(top);
        for (j = 0; j < k; j++)
            sm_ed_b(d[i + j]);
    }
}
static uint32_t sm_ed_unpack7(const uint8_t *a, uint32_t n, uint8_t *d, uint32_t max)
{
    uint32_t i, j, o = 0;
    for (i = 0; i < n; i += 8u)
        for (j = 0; j < 7u && i + 1u + j < n; j++) {
            if (o >= max)
                return o;
            d[o++] = (uint8_t)(a[i + 1u + j] | (((a[i] >> j) & 1u) << 7));
        }
    return o;
}
/* (reg, off, len) inside a region: its flash address, else ~0 */
static uint32_t sm_ed_addr(uint32_t reg, uint32_t off, uint32_t len)
{
    if (reg >= NELEM(SM_ED_REG) || off > SM_ED_REG[reg].size || len > SM_ED_REG[reg].size - off)
        return ~0u;
    return SM_ED_REG[reg].base + off;
}

/* one frame (p: the bytes after F0, F7 not included, n of them) -> the reply in sm_ed_out (F0 .. F7), its length; 0 not
 * ours. The main loop's (the flash) */
static uint32_t sm_ed_handle(const uint8_t *p, uint32_t n)
{
    uint32_t cmd, rc = SM_ED_OK, i, a;
    if (n < 4u || p[0] != SM_ED_H0 || p[1] != SM_ED_H1 || p[2] != SM_ED_H2)
        return 0;
    cmd = p[3];
    p += 4;
    n -= 4u;
    sm_ed_n = 0;
    sm_ed_out[sm_ed_n++] = 0xF0;
    sm_ed_b(SM_ED_H0);
    sm_ed_b(SM_ED_H1);
    sm_ed_b(SM_ED_H2);
    sm_ed_b(cmd);
    if (cmd != SM_ED_INFO && cmd != SM_ED_STATE && !flash_ok) {
        sm_ed_b(SM_ED_NOFLASH);
    } else if (cmd == SM_ED_INFO) {
#ifdef FELUCCA_VERSION
        static const char NAME[] = "PULHRA " FELUCCA_VERSION;
#else
        static const char NAME[] = "PULHRA";
#endif
        sm_ed_b(SM_ED_OK);
        sm_ed_b(SM_ED_VER);
        sm_ed_b(NELEM(SM_ED_REG));
        for (i = 0; i < NELEM(SM_ED_REG); i++) {
            sm_ed_b(i);
            sm_ed_u21(SM_ED_REG[i].base);
            sm_ed_u21(SM_ED_REG[i].size);
        }
        for (i = 0; NAME[i]; i++)
            sm_ed_b((uint8_t)NAME[i]);
        sm_ed_b(0);
    } else if (cmd == SM_ED_STATE) {
        sm_ed_b(SM_ED_OK);
        sm_ed_b(*(volatile uint8_t *)&sm_playing != 0);
    } else if (cmd == SM_ED_READ) {
        uint32_t len = n >= 6u ? (uint32_t)p[4] | (uint32_t)p[5] << 7 : 0u;
        a = n >= 6u && len && len <= 256u ? sm_ed_addr(p[0], sm_ed_ru21(p + 1), len) : ~0u;
        if (a == ~0u) {
            sm_ed_b(SM_ED_BADARG);
        } else if (st_read(a, sm_ed_buf, len)) {
            sm_ed_b(SM_ED_FLASH);
        } else {
            sm_ed_b(SM_ED_OK);
            sm_ed_b(p[0]);
            sm_ed_u21(sm_ed_ru21(p + 1));
            sm_ed_b(len);
            sm_ed_b(len >> 7);
            sm_ed_pack7(sm_ed_buf, len);
        }
    } else if (cmd == SM_ED_ERASE || cmd == SM_ED_WRITE) {
        uint32_t off = n >= 4u ? sm_ed_ru21(p + 1) : 0u, len = 0;
        if (cmd == SM_ED_WRITE && n > 4u)
            len = sm_ed_unpack7(p + 4, n - 4u, sm_ed_buf, sizeof sm_ed_buf);
        a = n < 4u ? ~0u : cmd == SM_ED_ERASE ? ((off & (SM_FL_SEC - 1u)) ? ~0u : sm_ed_addr(p[0], off, SM_FL_SEC))
                                               : (len ? sm_ed_addr(p[0], off, len) : ~0u);
        if (a == ~0u)
            rc = SM_ED_BADARG;
        else if (*(volatile uint8_t *)&sm_playing)
            rc = SM_ED_BUSY;
        else if (cmd == SM_ED_ERASE)
            rc = st_erase(a) ? SM_ED_FLASH : SM_ED_OK;
        else {
            uint8_t chk[64];
            rc = sm_flash_put(a, sm_ed_buf, len) ? SM_ED_FLASH : SM_ED_OK;
            for (i = 0; rc == SM_ED_OK && i < len; i += sizeof chk) {   /* (read back) */
                uint32_t k = len - i < sizeof chk ? len - i : (uint32_t)sizeof chk;
                if (st_read(a + i, chk, k) || memcmp(chk, sm_ed_buf + i, k))
                    rc = SM_ED_FLASH;
            }
        }
        sm_ed_b(rc);
        if (n >= 4u) {
            sm_ed_b(p[0]);
            sm_ed_u21(off);
        }
    } else if (cmd == SM_ED_RELOAD) {
        uint32_t what = n ? p[0] : 0u;
        if (*(volatile uint8_t *)&sm_playing) {
            rc = SM_ED_BUSY;
        } else {
            if (what & 2u)
                sm_up_boot();                             /* (the user sounds: their index anew) */
            if (what & 4u)
                for (i = 0; i < SMP_USER_SLOTS; i++)
                    smp_user_scan(i);                     /* (the slots: their places and zones anew; CLOUD, SAMP, the slices follow) */
            if (what & 1u)
                sm_ed_reboot = 1;                         /* (the banks, the project: as at power-on) */
            sm_dirty = 1;
        }
        sm_ed_b(rc);
    } else {
        sm_ed_b(SM_ED_BADARG);
    }
    sm_ed_out[sm_ed_n++] = 0xF7;
    return sm_ed_n;
}
