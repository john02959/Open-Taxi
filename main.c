/*
 * PS2 Taxi + Audio CD radio  (UNTESTED STARTER - built for ps2sdk)
 *
 * Idea (Vib-Ribbon style): the game ELF and all its data live in RAM.
 * Once loaded, the game tray is opened, the player inserts ANY audio CD,
 * and the music is streamed live from the CD while you drive.
 * Nothing from the CD is stored; only a small ring of sectors is buffered.
 *
 * Screen colours (no font code, to keep this small):
 *   RED border   = waiting for an audio CD
 *   GREEN border = CD detected, music streaming
 * Controls: X = gas, O = brake, D-pad left/right = steer
 */
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <libcdvd.h>
#include <libpad.h>
#include <audsrv.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern unsigned char audsrv_irx[];
extern unsigned int size_audsrv_irx;

#define W 640
#define H 448
#define CD_SECTORS 8
#define CD_SECTOR_BYTES 2352

static char padBuf[256] __attribute__((aligned(64)));
static GSGLOBAL *gs;

#define COL(r, g, b) GS_SETREG_RGBAQ(r, g, b, 0x80, 0x00)

/* ---------- init ---------- */
static void init_iop(void)
{
    SifInitRpc(0);
    SifIopReset("", 0);
    while (!SifIopSync()) {}
    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    SifLoadModule("rom0:LIBSD", 0, NULL);
    SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, NULL);
}

static void init_gfx(void)
{
    gs = gsKit_init_global();
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
}

static void init_pad(void)
{
    padInit(0);
    padPortOpen(0, 0, padBuf);
    while (padGetState(0, 0) != PAD_STATE_STABLE) {}
}

static u32 read_pad(void)
{
    struct padButtonStatus b;
    if (padRead(0, 0, &b) == 0) return 0;
    return 0xffff ^ b.btns;
}

/* ---------- audio CD streaming ---------- */
static u8 cdbuf[CD_SECTORS * CD_SECTOR_BYTES] __attribute__((aligned(64)));
static u32 cd_lsn = 0;       /* TODO: use sceCdGetToc to pick a track */
static int cd_playing = 0;
static int loudness = 0;     /* 0..255, handy for visual effects */

static int audio_cd_present(void)
{
    return sceCdGetDiskType() == SCECdCDDA;
}

static void start_audio(void)
{
    struct audsrv_fmt_t fmt = { 44100, 16, 2 };
    audsrv_init();
    audsrv_set_format(&fmt);
    audsrv_set_volume(MAX_VOLUME);
    cd_lsn = 0;
    cd_playing = 1;
}

static void pump_audio(void)
{
    sceCdRMode mode = { 1, SCECdSpinNom, SCECdSecS2352, 0 };
    int len = sizeof(cdbuf);

    if (!cd_playing || audsrv_available() < len) return;

    if (sceCdReadCDDA(cd_lsn, CD_SECTORS, cdbuf, &mode)) {
        sceCdSync(0);
        audsrv_play_audio((char *)cdbuf, len);
        cd_lsn += CD_SECTORS;

        /* rough loudness for effects */
        s16 *s = (s16 *)cdbuf;
        int peak = 0;
        for (int i = 0; i < len / 2; i += 16) {
            int v = abs(s[i]);
            if (v > peak) peak = v;
        }
        loudness = peak >> 7;
    }
}

/* ---------- taxi game ---------- */
typedef struct { float x, y, a, v; } Car;
static Car car = { W / 2, H / 2, 0, 0 };
static float px, py, dx, dy;      /* passenger and drop-off */
static int has_fare = 0, money = 0;

static void new_job(void)
{
    px = 40 + rand() % (W - 80);
    py = 40 + rand() % (H - 80);
    dx = 40 + rand() % (W - 80);
    dy = 40 + rand() % (H - 80);
    has_fare = 0;
}

static void update_game(u32 pad)
{
    if (pad & PAD_CROSS)  car.v += 0.15f;
    if (pad & PAD_CIRCLE) car.v -= 0.25f;
    if (pad & PAD_LEFT)   car.a -= 0.05f * (car.v > 0 ? 1 : 0.3f);
    if (pad & PAD_RIGHT)  car.a += 0.05f * (car.v > 0 ? 1 : 0.3f);
    car.v *= 0.98f;
    if (car.v > 6) car.v = 6;
    if (car.v < -2) car.v = -2;

    car.x += cosf(car.a) * car.v;
    car.y += sinf(car.a) * car.v;
    if (car.x < 10) car.x = 10;
    if (car.x > W - 10) car.x = W - 10;
    if (car.y < 10) car.y = 10;
    if (car.y > H - 10) car.y = H - 10;

    float tx = has_fare ? dx : px, ty = has_fare ? dy : py;
    if (fabsf(car.x - tx) < 16 && fabsf(car.y - ty) < 16) {
        if (has_fare) { money += 10; new_job(); }
        else has_fare = 1;
    }
}

static void box(float cx, float cy, float r, u64 col)
{
    gsKit_prim_sprite(gs, cx - r, cy - r, cx + r, cy + r, 1, col);
}

static void draw_game(void)
{
    int pulse = loudness / 32;                 /* beat-reactive background */
    gsKit_clear(gs, COL(20 + pulse, 20, 30 + pulse * 2));

    u64 border = cd_playing ? COL(0, 200, 0) : COL(220, 0, 0);
    gsKit_prim_sprite(gs, 0, 0, W, 6, 2, border);
    gsKit_prim_sprite(gs, 0, H - 6, W, H, 2, border);

    if (!has_fare) box(px, py, 10, COL(255, 220, 0));   /* passenger */
    else           box(dx, dy, 12, COL(0, 255, 120));   /* destination */

    box(car.x, car.y, 9, COL(255, 200, 0));             /* taxi */
    box(car.x + cosf(car.a) * 9, car.y + sinf(car.a) * 9, 3, COL(255, 255, 255));

    gsKit_prim_sprite(gs, 10, 12, 10 + money, 20, 3, COL(255, 255, 255)); /* fare bar */

    gsKit_queue_exec(gs);
    gsKit_sync_flip(gs);
}

/* ---------- main ---------- */
int main(void)
{
    int ret;

    init_iop();
    sceCdInit(SCECdINIT);
    sceCdMmode(SCECdCD);
    init_gfx();
    init_pad();

    /* All game data would be loaded into RAM here (malloc + fread from
       cdrom0:). The ELF itself is already in RAM. After this point the
       game never touches the game disc again. */
    new_job();

    /* Open the tray so the player can swap discs */
    sceCdTrayReq(SCECdTrayOpen, (u32 *)&ret);

    for (;;) {
        u32 pad = read_pad();

        if (!cd_playing) {
            sceCdDiskReady(0);
            if (audio_cd_present()) start_audio();
        } else {
            pump_audio();
        }

        update_game(pad);
        draw_game();
    }
    return 0;
}
