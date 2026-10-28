/* Native grain between completed RAW development and final JPEG encoding.
 * No Canon Creative Filter setting or live-view job is modified. The CPU
 * prepares a tiny reusable seed; native engines filter overlapping32-row
 * strips and copy only their24 interior rows. */
#include "dryos.h"
#include "lens.h"
#include "menu.h"
#include "config.h"
#include "patch.h"
#include "propvalues.h"
#include "edmac-memcpy.h"
#include "xpan-mode.h"
#include "xpan-film-grain.h"
#if !defined(CONFIG_EOSM) || defined(CONFIG_QEMU) || !defined(CONFIG_XPAN)
#error Native saved-JPEG grain requires the physical EOS M2.0.2 build.
#endif
#define CAPTURE_SITE 0xff1703e0u
#define INPUT_SITE 0xff71aef0u
#define CONFIG_SITE 0xff731e88u
#define WIDTH 5184u
#define FULL_ROWS 3456u
#define STRIP_ROWS 32u
#define GUARD_ROWS 4u /* Discard the resizer's clamped top/bottom noise edges. */
#define CORE_ROWS (STRIP_ROWS-2*GUARD_ROWS)
#define COPY_BYTES (WIDTH*CORE_ROWS*2)
#define PANORAMIC_CROP_TOP 768u /* Mode-2 native JPEG crop: (3456-1920)/2. */
#define PANORAMIC_CROP_ROWS 1920u /* Every row of the 5184x1920 XPan JPEG. */
#define OUT_BYTES (WIDTH*STRIP_ROWS*2)
#define SHADOW_INPUT_BYTES 0u
#define NOISE_WIDTH (WIDTH/2)
#define NOISE_ROWS (STRIP_ROWS/2)
#define STREAM_BYTES (NOISE_WIDTH*NOISE_ROWS)
#define OFFSET_COUNT 1u /* One complete, hardware-assembled input per job. */
#define NOISE_BYTES (((STREAM_BYTES+4095)/4096)*4096*2)
#define SEED_BYTES 16384u
#define ASSET_OFFSETS 1094u
#define ASSET_BYTES (ASSET_OFFSETS*4+SEED_BYTES+20)
#define WORK_BYTES (OUT_BYTES+SHADOW_INPUT_BYTES+NOISE_BYTES+SEED_BYTES+STREAM_BYTES+4096+192)
#define LINEAR_ROM 0xff859910u
#define LINEAR_HASH 0xe427c32bu
#if PANORAMIC_CROP_ROWS%CORE_ROWS || GUARD_ROWS%2 || PANORAMIC_CROP_TOP<GUARD_ROWS || PANORAMIC_CROP_TOP+PANORAMIC_CROP_ROWS+GUARD_ROWS>FULL_ROWS
#error Grain interiors must cover the crop exactly with even, in-bounds guards.
#endif
#if STREAM_BYTES>65534 || STREAM_BYTES%128 || SEED_BYTES%16 || NOISE_WIDTH%16
#error Noise input must fit one native read and preserve16-byte DMA alignment.
#endif
static CONFIG_INT("xpan.film.grain",film_grain,0);
static CONFIG_INT("xpan.film.grain.strength",grain_strength,25);
/* Grain engine: 0 = Film (CPU, realistic), 1 = Canon (native Grainy filter). */
static CONFIG_INT("xpan.film.grain.engine",grain_engine,0);
/* Film grain size: 0 fine, 1 medium, 2 coarse (texture blur passes). */
static CONFIG_INT("xpan.film.grain.size",grain_size,1);
/* HUD indicator: grain will be added to the next JPEG. */
int xpan_film_grain_enabled(void) { return film_grain && grain_strength>0; }
#define grain_tone_probe 0
#define shadow_requested 0
#define tone_ramp_requested 0
static int prepared_tone_probe=0;
#define grain_stock_tone_probe 0
static int prepared_stock_tone_probe=0;
static int grain_table_ready;
static uint32_t asset[ASSET_BYTES/4];
static struct card_info *asset_card,*attempted_card,*last_card;
static void *allocation;
static uint8_t *output,*noise,*seed,*stream;
static uint32_t *linear,*offsets,*parameters,*colour;
static uint32_t job[16],native_driver[12],native_unpack[13];
static volatile unsigned busy,epoch=1;
static unsigned ready,prepared_strength,installed,install_attempted,patch_error;
static unsigned calls,applied,skipped,reason,chunks,colour_ok,config_ok;
static unsigned init_result,process_result,cleanup_result,copy_ok,elapsed_us;
static unsigned source_address,seed_hash,asset_hash,observed_input,observed_noise;
static unsigned asset_read_calls,asset_read_bytes,asset_read_max;
static unsigned observed_table,offset_phase,last_file=0xffffffffu,last_folder=0xffffffffu;
static unsigned stream_copies,stream_bytes,stream_fill_us,stream_position;
static unsigned stream_first_origin,stream_last_origin;
static uint32_t copy_status[7];
/* Retain earlier shot diagnostics: the idle snapshot used to describe only the
 * most recent JPEG, losing the phase/timing for an intermittently damaged one. */
struct grain_capture_record {
    unsigned file,folder,strength,phase,strips,reason,us,noise_us,copies;
    unsigned table_hash,seed_hash,tone_probe,stock_table,panoramic,native_return,published;
    uint32_t copy_status[7];
};
static struct grain_capture_record history[4];
static unsigned history_count;
static const char *status="OFF";
static uint32_t hash_bytes(const volatile uint8_t *data,unsigned length)
{
    uint32_t h=2166136261u;
    for (unsigned i=0;i<length;i++) h=(h^data[i])*16777619u;
    return h;
}
static uint32_t grain_prepared_table_hash;
static uint32_t selected_table_hash(void)
{
    return grain_prepared_table_hash;
}
static int ram_span(uint32_t address,unsigned length)
{
    uint32_t physical=address&~0x40000000u;
    return length && !(address&3) && physical>=0x10000 && physical<=0x10000000u-length;
}
static int idle(void)
{
    return !busy && !lens_info.job_state && !get_halfshutter_pressed() && !ml_shutdown_requested;
}
static int map_valid(void)
{
    /* This call site follows Canon's YUV-ready semaphore, but precedes the
     * JPEG resource lock. Grain must never run inside JPEG setup's lock. */
    return *(volatile uint32_t *)0xff170388==0xe59f0154 &&
        *(volatile uint32_t *)0xff170390==0xeb3a59c8 &&
        *(volatile uint32_t *)0xff1703d4==0xeb0adfbe &&
        *(volatile uint32_t *)0xff1703d8==0xe3e00000 &&
        *(volatile uint32_t *)0xff1703dc==0xe1c703b8 &&
        *(volatile uint32_t *)CAPTURE_SITE==0xe51f11b4 &&
        *(volatile uint32_t *)0xff170234==0x00070298 &&
        *(volatile uint32_t *)0xff1703e8==0xeb0ae388 &&
        *(volatile uint32_t *)0xff1704e4==0x0003fd54 &&
        *(volatile uint32_t *)0xff16d130==0xe51f011c &&
        *(volatile uint32_t *)0xff16d134==0xeb0aeba4 &&
        *(volatile uint32_t *)0xff16d140==0xeb0aebaf &&
        *(volatile uint32_t *)0xff16d174==0xeb3a668a &&
        *(volatile uint32_t *)0xff163238==0xe1cd01f0 &&
        *(volatile uint32_t *)0xff163244==0xe3a0200d &&
        *(volatile uint32_t *)0xff163254==0xe3a03011 &&
        *(volatile uint32_t *)0xff163258==0xe3a0102b &&
        *(volatile uint32_t *)0xff163260==0xeb0addb0 &&
        *(volatile uint32_t *)0xff41a928==0xe92d4fff &&
        *(volatile uint32_t *)0xff41aa24==0xe92d4070 &&
        *(volatile uint32_t *)0xff41aaa4==0xeb0639fb &&
        *(volatile uint32_t *)0xff41aac4==0xeb063a3f &&
        *(volatile uint32_t *)0xff41ab74==0xe92d4010 &&
        *(volatile uint32_t *)0xff41aba4==0x00054cb0 &&
        *(volatile uint32_t *)INPUT_SITE==0xe92d4010 &&
        *(volatile uint32_t *)0xff731e30==0xebffa42e &&
        *(volatile uint32_t *)0xff71ae9c==0xe1800601 &&
        *(volatile uint32_t *)CONFIG_SITE==0xe28dd098 &&
        *(volatile uint32_t *)0xff7324ac==0xe594202c &&
        *(volatile uint32_t *)0xff7324b8==0xebff9fa4 &&
        *(volatile uint32_t *)0xff5a946c==0xe5cd0028 &&
        /* Input byte count is passed from job[8] to RestartUnpack; a complete
         *41472-byte stream needs one read, below its65534-byte DMA limit. */
        *(volatile uint32_t *)0xff5a92d4==0xe5941004 &&
        *(volatile uint32_t *)0xff5a92f8==0xe5941020 &&
        *(volatile uint32_t *)0xff7319e4==0xe5941014 &&
        *(volatile uint32_t *)0xff7319ec==0xe594100c &&
        *(volatile uint32_t *)0xff7319f4==0xeb24160b &&
        *(volatile uint32_t *)0xff731794==0xe7905101 &&
        *(volatile uint32_t *)0xff7317f4==0x1594000c &&
        *(volatile uint32_t *)0xff731978==0x0000fffe &&
        /* Startup RAM SetEDmac copies these width masks from ROM. Odd xb is
         * truncated, rather than rejected; grain's copier must preserve it. */
        *(volatile uint32_t *)0xffa85aa4==0xe3c00106 &&
        *(volatile uint32_t *)0xffa85ab8==0xe3c00106 &&
        *(volatile uint32_t *)0xffa85b04==0xe3c00001 &&
        *(volatile uint32_t *)0xff731928==0x0009c37c &&
        *(volatile uint32_t *)0xff854880==0x00010005 &&
        *(volatile uint32_t *)0xff854928==0x0013001c &&
        *(volatile uint32_t *)0xff85492c==0xc0f0300c &&
        *(volatile uint32_t *)0xff8549b4==0xc0f03000;
}
/* Highlight-safe production grain (G5 camera calibration, 2026-10-22).
 * Native op: out = Table(clamp(Y*coefY + bias, 0, 255) - noise), coefficient
 * = mantissa(8 bits) * 2^exponent(bits 8-9) / 256 (G5: 0x240=1, 0x220=0.5,
 * 0x200=0, 0x180=1, 0x1C0=1.5, 0x120=0.25). Canon's bias 54 with noise
 * centred on 54 clipped every Y>201. Here the noise is centred on C (0..2C),
 * bias 2C lifts the image off black and coefficient k=(255-2C)/255 keeps
 * Y*k+2C <= 255, so nothing clips before the noise; the output table expands
 * (y - (2C - mean noise))/k back. Chroma coefficients stay Canon's 1.0. */
static unsigned grain_centre,grain_coef=0x240u,grain_bias=54u;
static uint32_t grain_noise_sum; /* sum of the 16384 seed values (mean = sum/16384) */
static unsigned grain_node(unsigned r,unsigned g,unsigned b)
{
    return 5u*(g%5u+5u*(b%5u))+(b>4u ? 513u:0u)+(g>4u ? 257u:0u)+r+(r>4u ? 124u:0u);
}
static uint32_t grain_arith_seen[4];
static int grain_set_arithmetic(unsigned bias,unsigned coefficient)
{
    uint32_t base=*(volatile uint32_t *)0x4c338;
    if (!ram_span(base,4) || !ram_span(base|0x15000,256)) return 0;
    ((void (*)(unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned))
        0xff71ad08)(2,4,bias,coefficient,0,0x240,0,0x240);
    __asm__ __volatile__("" ::: "memory");
    for (unsigned i=0;i<4;i++) grain_arith_seen[i]=*(volatile uint32_t *)((base|0x150d4)+i*4u);
    return grain_arith_seen[0]==4u && grain_arith_seen[1]==((bias<<16)|coefficient) &&
        grain_arith_seen[2]==0x240u && grain_arith_seen[3]==0x240u;
}
static int load_asset(struct card_info *card)
{
    if (asset_card==card) return 1;
    if (attempted_card==card) return 0;
    attempted_card=card;
    char path[FIO_MAX_PATH_LENGTH];
    snprintf(path,sizeof(path),"%s:/ML/XPAN/GRAIN.BIN",card->drive_letter);
    uint32_t bytes=0,header[8];
    if (FIO_GetFileSize(path,&bytes) || bytes!=32+ASSET_BYTES)
    { status="Grain asset missing"; return 0; }
    FILE *file=FIO_OpenFile(path,O_RDONLY|O_SYNC);
    if (!file) { status="Cannot open grain asset"; return 0; }
    /* FIO_ReadFile permits cached destinations only up to8192bytes and uses
     * a temporary DMA bounce buffer for those. Use one explicit small DMA
     * buffer for header/body reads, then copy only asset bytes into cached RAM. */
    uint8_t *io=__mem_malloc(4096,MEM_DMA,__FILE__,__LINE__);
    if (!io) { FIO_CloseFile(file); status="No grain read buffer"; return 0; }
    asset_read_calls=asset_read_bytes=asset_read_max=0;
    int received=FIO_ReadFile(file,io,sizeof(header));
    asset_read_calls++; asset_read_max=sizeof(header);
    if (received>0) asset_read_bytes=received;
    int ok=received==(int)sizeof(header);
    if (ok) memcpy(header,io,sizeof(header));
    for (unsigned offset=0;ok && offset<sizeof(asset);) {
        unsigned count=MIN(4096u,sizeof(asset)-offset);
        received=FIO_ReadFile(file,io,count);
        asset_read_calls++; asset_read_max=MAX(asset_read_max,count);
        if (received>0) asset_read_bytes+=received;
        if (received!=(int)count) { ok=0; break; }
        memcpy((uint8_t *)asset+offset,io,count); offset+=count;
    }
    __mem_free(io); FIO_CloseFile(file);
    if (!ok) { status="Short grain asset read"; return 0; }
    asset_hash=hash_bytes((const uint8_t *)asset,sizeof(asset));
    if (!ok || header[0]!=0x474e5058 || header[1]!=0x00314e52 || header[2]!=1 ||
        header[3]!=ASSET_BYTES || header[4]!=ASSET_OFFSETS || header[5]!=SEED_BYTES ||
        header[6]!=asset_hash || header[7]!=54 ||
        hash_bytes((const uint8_t *)(asset+ASSET_OFFSETS),SEED_BYTES)!=0x85cb4ae5)
    { status="Invalid grain asset"; return 0; }
    for (unsigned i=0;i<ASSET_OFFSETS;i++)
        if (asset[i]>=SEED_BYTES) { status="Invalid grain offsets"; return 0; }
    const uint32_t *settings=asset+ASSET_OFFSETS+SEED_BYTES/4;
    if (settings[0]!=0 || settings[1]!=2 || settings[2]!=1 || settings[3]!=32 || settings[4]!=32)
    { status="Invalid grain settings"; return 0; }
    asset_card=card; return 1;
}
static int prepare(struct card_info *card,int shadow)
{
    if (!load_asset(card)) return 0;
    if (!allocation) {
        if (hash_bytes((const volatile uint8_t *)LINEAR_ROM,4096)!=LINEAR_HASH)
        { status="Grain tone map mismatch"; return 0; }
        allocation=__mem_malloc(WORK_BYTES+127,MEM_DMA,__FILE__,__LINE__);
        if (!allocation) { status="No grain workspace"; return 0; }
        output=(uint8_t *)UNCACHEABLE(((uintptr_t)allocation+127)&~(uintptr_t)127);
        noise=output+OUT_BYTES+SHADOW_INPUT_BYTES; seed=noise+NOISE_BYTES; stream=seed+SEED_BYTES;
        linear=(uint32_t *)(stream+STREAM_BYTES); offsets=linear+1024;
        parameters=offsets+16; colour=parameters+16;
        for (unsigned i=0;i<5;i++) parameters[i]=asset[ASSET_OFFSETS+SEED_BYTES/4+i];
    }
    unsigned table_selection=shadow ? 0u:!!grain_stock_tone_probe;
    unsigned tone_probe=shadow ? 1u:!!grain_tone_probe;
    unsigned strength=COERCE(grain_strength,1,100);
    if (prepared_strength!=strength || (unsigned)prepared_tone_probe!=tone_probe) {
        const uint8_t *original=(const uint8_t *)(asset+ASSET_OFFSETS);
        uint32_t expected=2166136261u;
        /* Centre C = 2.66 x the scaled deviations' RMS (15 at strength 25,
         * the G5-measured case); rarer extremes are clipped to 0..2C. */
        uint64_t square=0;
        for (unsigned i=0;i<SEED_BYTES;i++) {
            int delta=tone_probe ? 0:(int)original[i]-54;
            int scaled=(ABS(delta)*(int)strength+50)/100;
            square+=(uint64_t)(scaled*scaled);
        }
        unsigned rms_x100=0; /* integer sqrt of mean square, x100 */
        {
            /* SEED_BYTES = 2^14: mean square x10000 by shift, no 64-bit divide. */
            uint32_t ms=(uint32_t)((square*10000u+(SEED_BYTES/2u))>>14), r=0;
            while ((r+1u)*(r+1u)<=ms) r++;
            rms_x100=r;
        }
        unsigned centre=(rms_x100*266u+5000u)/10000u;
        if (centre>60u) centre=60u;
        /* The output table expands grain by 1/k too: pre-shrink deviations by
         * the provisional k so each strength keeps its previous visible size. */
        unsigned shrink=((255u-(32u+centre))*128u+127u)/255u; /* k*128 */
        uint32_t sum=0;
        for (unsigned i=0;i<SEED_BYTES;i++) {
            int delta=tone_probe ? 0:(int)original[i]-54;
            int magnitude=(ABS(delta)*(int)strength*(int)shrink+6400)/12800;
            int value=(int)centre+(delta<0 ? -magnitude:magnitude);
            if (value<0) value=0;
            if (value>(int)(2u*centre)) value=(int)(2u*centre);
            seed[i]=(uint8_t)value; sum+=(uint32_t)value; expected=(expected^seed[i])*16777619u;
        }
        seed_hash=hash_bytes(seed,SEED_BYTES);
        if (seed_hash!=expected) { status="Grain seed copy failed"; return 0; }
        /* Lift black by exactly one 32-code table step (bias = 32 + mean
         * noise), so the expansion stays exactly affine through the 10-node
         * table and black lands on a node; k = mantissa/128 (exponent 1,
         * G5-verified) = (255-bias)/255 keeps Y*k + bias <= 255. */
        unsigned bias=32u+(sum+SEED_BYTES/2u)/SEED_BYTES;
        if (bias<2u*centre) bias=2u*centre; /* noise never drives below 0 */
        unsigned mantissa=((255u-bias)*128u+127u)/255u;
        grain_centre=centre; grain_noise_sum=sum;
        grain_bias=bias; grain_coef=0x100u|mantissa;
        grain_table_ready=0;
        prepared_strength=strength;
    }
    if (!grain_table_ready || (unsigned)prepared_stock_tone_probe!=table_selection) {
        {
            /* Canon Linear table (grey nodes Y=64*i, 2 field units per code),
             * Y expanded: yf' = (yf - 2*(bias - mean))*128/mantissa. */
            const volatile uint32_t *table=(const volatile uint32_t *)LINEAR_ROM;
            for (unsigned i=0;i<1024;i++) linear[i]=table[i];
            unsigned mantissa=grain_coef&0xffu;
            /* lift in field units x16384: 2*(bias*16384 - sum) */
            int lift=(int)(2u*(grain_bias*SEED_BYTES))-(int)(2u*grain_noise_sum);
            for (unsigned r=0;r<10;r++) for (unsigned g=0;g<10;g++) for (unsigned b=0;b<10;b++) {
                unsigned i=grain_node(r,g,b),word=linear[i];
                int y=(int)((word>>20)&1023u)*(int)SEED_BYTES-lift;
                if (y<0) y=0;
                /* y <= 1023*16384, so y*128 < 2^32. */
                uint32_t out=(((uint32_t)y*128u/mantissa)+SEED_BYTES/2u)/SEED_BYTES;
                if (out>1023u) out=1023u;
                linear[i]=(word&0xfffffu)|(out<<20);
            }
        }
        prepared_stock_tone_probe=table_selection;
        grain_prepared_table_hash=hash_bytes((const uint8_t *)linear,4096);
    }
    grain_table_ready=1;
    return 1;
}
static int fill_noise_stream(unsigned y)
{
    /* Build a window of one global randomized field, including its guards.
     * Canon chooses a new circular seed origin after every16384 samples. Keep
     * those boundaries global, rather than restarting the random sequence at
     * each strip. Overlapping windows then contain identical noise samples.
     * Only a few source/destination ranges are prepared by the CPU; every
     * texture byte is copied by DMA, with no image/noise-pixel CPU pass. */
    unsigned started=(uint32_t)get_us_clock(),written=0;
    unsigned position=SEED_BYTES+(y/2)*NOISE_WIDTH-(GUARD_ROWS/2)*NOISE_WIDTH;
    stream_position=position;
    if (!edmac_memcpy_res_lock_checked()) { reason=8192; return 0; }
    while (written<STREAM_BYTES) {
        unsigned block=position/SEED_BYTES,within=position%SEED_BYTES;
        /* Restrict the native even offsets to16-byte alignment for the copy
         * engine. This changes the phase only, not the seed or its strength. */
        unsigned origin=asset[(offset_phase+block)%ASSET_OFFSETS]&~15u;
        unsigned source=(origin+within)%SEED_BYTES;
        unsigned count=MIN(STREAM_BYTES-written,MIN(SEED_BYTES-within,SEED_BYTES-source));
        if (!count || ((source|written|count)&15) || !edmac_find_divider(count,0))
        { reason=32768; break; } /* Never allow the copier's CPU fallback. */
        if (!written) stream_first_origin=origin;
        stream_last_origin=origin;
        if (!edmac_memcpy_write_checked(stream+written,seed+source,count,1000,copy_status))
        { reason=16384; break; }
        /* Inspect only the tiny noise asset fragment's first/last16 bytes.
         * This checks DMA publication without scanning any photo pixels. */
        unsigned matched=1;
        for (unsigned i=0;i<4;i++) {
            if (((volatile uint32_t *)(stream+written))[i]!=((const volatile uint32_t *)(seed+source))[i] ||
                ((volatile uint32_t *)(stream+written+count-16))[i]!=
                ((const volatile uint32_t *)(seed+source+count-16))[i]) matched=0;
        }
        if (!matched) { reason=65536; break; }
        stream_copies++; stream_bytes+=count;
        written+=count; position+=count;
    }
    edmac_memcpy_res_unlock();
    stream_fill_us+=(uint32_t)get_us_clock()-started;
    return written==STREAM_BYTES;
}
/* Run the native tile and wait for its completion without publishing pixels. */
static int run_strip_native(unsigned source_address,unsigned source_y,unsigned noise_y)
{
    colour_ok=config_ok=0;
    /* The noise coordinate is crop-relative in 65:24 and frame-relative in 3:2. */
    if (!fill_noise_stream(noise_y)) return 0;
    offsets[0]=0; /* The prepared stream is consumed once, without wrap. */
    job[0]=source_address+source_y*WIDTH*2;
    job[1]=(uint32_t)stream; job[2]=(uint32_t)noise; job[3]=(uint32_t)output;
    job[4]=job[5]=job[6]=WIDTH; job[7]=STRIP_ROWS;
    job[8]=STREAM_BYTES; job[9]=NOISE_WIDTH; job[10]=NOISE_ROWS; job[11]=54;
    job[12]=(uint32_t)offsets; job[13]=OFFSET_COUNT;
    job[14]=(uint32_t)parameters; job[15]=(uint32_t)linear;
    __asm__ __volatile__("" ::: "memory");
    process_result=((unsigned (*)(uint32_t *))0xff41aa24)(job);
    if (process_result || !colour_ok || !config_ok) { reason=128; return 0; }
    return 1;
}
/* Publish only the valid guarded interior after native processing succeeds. */
static int publish_strip(unsigned source_address,unsigned destination_y,
    unsigned output_y,unsigned copy_rows)
{
    unsigned copy_bytes=WIDTH*copy_rows*2;
    /* Keep only valid interior rows; the four clamped rows at each edge are
     * discarded, including the partial 3:2 tail where only 16 rows remain. */
    if (!edmac_find_divider(copy_bytes,0)) { reason=32; return 0; }
    if (!edmac_memcpy_res_lock_checked()) { reason=256; return 0; }
    copy_ok=edmac_memcpy_write_checked(
        (void *)(source_address+destination_y*WIDTH*2),
        output+output_y*WIDTH*2,copy_bytes,1000,copy_status);
    edmac_memcpy_res_unlock();
    if (!copy_ok) { reason=512; return 0; }
    return 1;
}
static int process_strip(unsigned source_address,unsigned source_y,
    unsigned destination_y,unsigned output_y,unsigned copy_rows,unsigned noise_y)
{
    if (!run_strip_native(source_address,source_y,noise_y) ||
        !publish_strip(source_address,destination_y,output_y,copy_rows)) return 0;
    chunks++;
    return 1;
}
static int driver_matches(const volatile uint32_t *driver)
{
    uint32_t expected[12]={job[0],job[2],job[3],WIDTH,NOISE_WIDTH,
        WIDTH,WIDTH,STRIP_ROWS,NOISE_WIDTH,NOISE_ROWS,54,(uint32_t)linear};
    /* Canon stores the bias with STRB; the upper three stack bytes are not
     * initialized and are not part of that native field. */
    for (unsigned i=0;i<12;i++)
        if ((i==10 ? driver[i]&255u:driver[i])!=expected[i]) return 0;
    return 1;
}
static void input_hook(uint32_t *regs,uint32_t *stack,uint32_t pc)
{
    if (!busy || pc!=INPUT_SITE || regs[13]!=0xff731e34 || colour_ok) return;
    uint32_t sp=(uint32_t)stack;
    if (regs[0]!=0 || regs[1]!=sp+0x80 || regs[2]!=sp+0x60 ||
        !ram_span(sp,0x98) || !ram_span(regs[4],48) ||
        !driver_matches((const volatile uint32_t *)regs[4])) return;
    const volatile uint32_t *geometry=(const volatile uint32_t *)regs[1];
    const volatile uint32_t *original=(const volatile uint32_t *)regs[2];
    for (unsigned i=0;i<6;i++)
        if (geometry[i]!=(i==1 ? WIDTH:i==4 ? STRIP_ROWS:0)) return;
    for (unsigned i=0;i<8;i++) {
        if (original[i]!=(i==0 ? 1u:i==6 ? 6u:0u)) return;
        colour[i]=original[i];
    }
    /* Same image-only converter edit proven in test47..50. Noise input and
     * arithmetic stay stock. A persistent private block preserves Canon's stack. */
    /* Normal grain keeps the proven color-preserving conversion mode. */
    colour[6]=0;
    __asm__ __volatile__("" ::: "memory");
    regs[2]=(uint32_t)colour; colour_ok=1;
}
static void config_hook(uint32_t *regs,uint32_t *stack,uint32_t pc)
{
    (void)stack;
    if (!busy || pc!=CONFIG_SITE || !ram_span(regs[4],48)) return;
    const volatile uint32_t *driver=(const volatile uint32_t *)regs[4];
    if (!driver_matches(driver)) return;
    for (unsigned i=0;i<12;i++) native_driver[i]=driver[i];
    const volatile uint32_t *unpack=(const volatile uint32_t *)0x9c37c;
    uint32_t expected[13]={(uint32_t)stream,(uint32_t)noise,NOISE_WIDTH,STREAM_BYTES,
        NOISE_WIDTH,NOISE_ROWS,(uint32_t)offsets,OFFSET_COUNT,0,2,1,32,32};
    unsigned matched=1;
    for (unsigned i=0;i<13;i++) {
        native_unpack[i]=unpack[i]; if (unpack[i]!=expected[i]) matched=0;
    }
    uint32_t base=*(volatile uint32_t *)0x4c338;
    if (!ram_span(base,4) || !ram_span(base|0x15000,256)) return;
    /* At this established pre-start callback the native job still owns its
     * resources (G2/G4/G5). Production: highlight-safe bias/coefficient. */
    if (!grain_set_arithmetic(grain_bias,grain_coef)) return;
    observed_input=*(volatile uint32_t *)(base|0x15044);
    observed_noise=*(volatile uint32_t *)(base|0x15064);
    observed_table=driver[11];
    uint32_t expected_input=0x10000u;
    config_ok=colour_ok && matched && observed_input==expected_input && observed_noise==0x16000 &&
        *(volatile uint32_t *)(base|0x15014)==2 &&
        *(volatile uint32_t *)(base|0x150d4)==4 &&
        *(volatile uint32_t *)(base|0x1500c)==2 &&
        *(volatile uint32_t *)(base|0x150a4)==0x01001000;
}

/* ---------------------------------------------------------------------------
 * Film grain (CPU). A tileable 512x512 grain texture (approximately Gaussian
 * white noise, softened by separable [1 2 1] blur passes for the grain size)
 * is added to the luma bytes of Canon's finished YUV image, just before JPEG
 * encoding, at the same hook as the native grain. The amplitude follows the
 * pixel's own brightness like film: strongest in the midtones, weaker in deep
 * shadows and highlights, so blacks and whites are not lifted or clipped.
 * Every 64x128-pixel block reads the texture at its own pseudo-random offset
 * (seeded per photo), so the texture never repeats visibly; grain is only a
 * few pixels wide, so the block seams cannot be seen. Chroma is unchanged.
 * --------------------------------------------------------------------------- */
#define FILM_TEX 512u
static int8_t *film_tex;
static int16_t film_amp[256];
static unsigned film_ready,film_strength=0xffffffffu,film_size=0xffffffffu,film_us;
static uint32_t film_rng(uint32_t *state)
{
    uint32_t x=*state; x^=x<<13; x^=x>>17; x^=x<<5; return *state=x;
}
static unsigned film_isqrt(unsigned v)
{
    unsigned r=0,b=1u<<30;
    while (b>v) b>>=2;
    while (b) { if (v>=r+b) { v-=r+b; r=(r>>1)+b; } else r>>=1; b>>=2; }
    return r;
}
static int film_prepare(void)
{
    unsigned strength=(unsigned)COERCE(grain_strength,1,100),size=(unsigned)COERCE(grain_size,0,2);
    if (film_ready && film_tex && film_strength==strength && film_size==size) return 1;
    film_ready=0;
    if (!film_tex) film_tex=malloc(FILM_TEX*FILM_TEX);
    int16_t *g=malloc(FILM_TEX*FILM_TEX*sizeof(int16_t));
    int16_t *line=malloc(FILM_TEX*sizeof(int16_t));
    if (!film_tex || !g || !line) { if (g) free(g); if (line) free(line); status="No memory for film grain"; return 0; }
    uint32_t rng=0x9e3779b9u;
    for (unsigned i=0;i<FILM_TEX*FILM_TEX;i++) {
        uint32_t r=film_rng(&rng);
        /* Sum of four uniform bytes: mean 510, close to Gaussian. */
        g[i]=(int16_t)((int)(r&255)+(int)((r>>8)&255)+(int)((r>>16)&255)+(int)(r>>24)-510);
    }
    static const unsigned passes[3]={1,2,4};
    for (unsigned pass=0;pass<passes[size];pass++) {
        for (unsigned y=0;y<FILM_TEX;y++) {          /* horizontal, wrapping */
            int16_t *row=g+y*FILM_TEX;
            for (unsigned x=0;x<FILM_TEX;x++) line[x]=row[x];
            for (unsigned x=0;x<FILM_TEX;x++)
                row[x]=(int16_t)((line[(x-1)&(FILM_TEX-1)]+2*line[x]+line[(x+1)&(FILM_TEX-1)])/4);
        }
        for (unsigned x=0;x<FILM_TEX;x++) {          /* vertical, wrapping */
            for (unsigned y=0;y<FILM_TEX;y++) line[y]=g[y*FILM_TEX+x];
            for (unsigned y=0;y<FILM_TEX;y++)
                g[y*FILM_TEX+x]=(int16_t)((line[(y-1)&(FILM_TEX-1)]+2*line[y]+line[(y+1)&(FILM_TEX-1)])/4);
        }
    }
    /* Normalize to a standard deviation of 32 texture units (int8 = +-4 sigma). */
    long long sum=0,sq=0;
    for (unsigned i=0;i<FILM_TEX*FILM_TEX;i++) { sum+=g[i]; sq+=(long long)g[i]*g[i]; }
    int mean=(int)(sum/(int)(FILM_TEX*FILM_TEX));
    unsigned sd=film_isqrt((unsigned)(sq/(FILM_TEX*FILM_TEX)-(long long)mean*mean));
    if (!sd) sd=1;
    for (unsigned i=0;i<FILM_TEX*FILM_TEX;i++) {
        int v=(g[i]-mean)*32/(int)sd;
        film_tex[i]=(int8_t)COERCE(v,-127,127);
    }
    free(line); free(g);
    /* Peak grain (8-bit levels, 1 sigma) = strength*0.12: 25 -> 3, 100 -> 12.
     * Shape 0.15+0.85*sqrt(4t(1-t)): 1 at mid grey, 0.15 at black/white.
     * film_amp = sigma*256/32, so noise = tex*amp/256 has that sigma. */
    for (unsigned y=0;y<256;y++) {
        /* q = 1000*sqrt(4t(1-t)), t=y/255: 4y(255-y)*15378 ~ 1e9*4t(1-t) (fits 32 bits). */
        unsigned q=film_isqrt(4u*y*(255u-y)*15378u)*1000u/31623u;
        unsigned shape=150u+850u*MIN(q,1000u)/1000u;                                       /* x1000 */
        film_amp[y]=(int16_t)(strength*12u*shape*8u/100000u);
    }
    film_strength=strength; film_size=size; film_ready=1;
    return 1;
}
static void film_apply(unsigned address,unsigned first_row,unsigned rows,unsigned seed)
{
    for (unsigned y=0;y<rows;y++) {
        volatile uint32_t *row=(volatile uint32_t *)(address+(first_row+y)*WIDTH*2u);
        unsigned by=(first_row+y)>>7;
        for (unsigned bx=0;bx<WIDTH/64u;bx++) {
            uint32_t h=(bx*0x9e3779b1u)^(by*0x85ebca77u)^seed; h^=h>>15; h*=0x2c1b3c6du; h^=h>>12;
            unsigned ox=h&(FILM_TEX-1),oy=(h>>9)&(FILM_TEX-1);
            const int8_t *tex=film_tex+(((first_row+y+oy)&(FILM_TEX-1))*FILM_TEX);
            for (unsigned x=bx*64u;x<bx*64u+64u;x+=2) {
                uint32_t w=row[x>>1];                 /* U Y0 V Y1 */
                int y0=(w>>8)&255,y1=w>>24;
                y0+=(tex[(x+ox)&(FILM_TEX-1)]*film_amp[y0])>>8;
                y1+=(tex[(x+1+ox)&(FILM_TEX-1)]*film_amp[y1])>>8;
                y0=COERCE(y0,0,255); y1=COERCE(y1,0,255);
                row[x>>1]=(w&0x00ff00ffu)|((uint32_t)y0<<8)|((uint32_t)y1<<24);
            }
        }
    }
}
static void wm_capture_hook(uint32_t *regs,uint32_t pc);
static void grain_capture_hook(uint32_t *regs,uint32_t *stack,uint32_t pc)
{
    (void)stack;
    int shadow=shadow_requested;
    int tone_ramp_run=tone_ramp_requested;
    int private_run=shadow||tone_ramp_run;
    if ((!film_grain && !private_run) || !xpan_photo_enabled() ||
        is_movie_mode() || ml_shutdown_requested || pc!=CAPTURE_SITE) return;
    /* R7 is the completed-YUV JPEG descriptor at this exact semaphore-protected
     * call site. Quality8 is the final JPEG; quality0 is the RAW preview. */
    if (regs[7]!=0x70298) return;
    const volatile uint32_t *source=(const volatile uint32_t *)regs[7];
    if (source[0x44/4]!=8) return;
    /* Excluded RAW-preview jobs must not erase the last JPEG's diagnostics. */
    calls++; reason=0;
    source_address=source[0x58/4];
    if (!grain_engine && !private_run) {
        /* Film engine: same finished-YUV descriptor checks as the native path,
         * then one CPU pass over the luma of the visible frame. */
        reason=0;
        if (!film_ready || film_strength!=(unsigned)COERCE(grain_strength,1,100)) reason|=2;
        if (source[0]!=0xff90f600 || source[1]!=0x26 || source[2]!=0xff9125a8 ||
            source[0x38/4]!=0x1440ffff || source[0x3c/4]!=3456 || source[0x40/4]!=0) reason|=4;
        uint32_t physical=source_address&~0x40000000u;
        if ((source_address&0xf0000000u)!=0x40000000u || (physical&127) ||
            physical<0x08000000 || !ram_span(source_address,WIDTH*3456*2)) reason|=8;
        struct card_info *card=get_shooting_card();
        if (!card) reason|=64;
        if (busy) reason|=16;
        if (reason) { skipped++; status="Grain skipped"; epoch++; return; }
        if (last_card==card && last_file==(unsigned)card->file_number &&
            last_folder==(unsigned)card->folder_number) return;
        unsigned started=(uint32_t)get_us_clock(); busy=1;
        unsigned panoramic=!!xpan_photo_is_panoramic();
        film_apply(source_address,panoramic ? PANORAMIC_CROP_TOP:0,
            panoramic ? PANORAMIC_CROP_ROWS:FULL_ROWS,
            (unsigned)card->file_number*2654435761u^(unsigned)get_ms_clock());
        wm_capture_hook(regs,pc);              /* logo on top of the grain */
        film_us=elapsed_us=(uint32_t)get_us_clock()-started; busy=0;
        last_card=card; last_file=card->file_number; last_folder=card->folder_number;
        applied++; status="Ready; last JPEG film grain"; epoch++;
        return;
    }
    if (!ready || prepared_strength!=(unsigned)COERCE(grain_strength,1,100) ||
        (private_run && (!prepared_tone_probe || prepared_stock_tone_probe))) reason|=2;
    uint32_t expected_table_hash=private_run ? LINEAR_HASH:selected_table_hash();
    if (ready && (hash_bytes(seed,SEED_BYTES)!=seed_hash ||
        hash_bytes((const uint8_t *)linear,4096)!=expected_table_hash)) reason|=4096;
    if (source[0]!=0xff90f600 || source[1]!=0x26 || source[2]!=0xff9125a8 ||
        source[0x38/4]!=0x1440ffff || source[0x3c/4]!=3456 || source[0x40/4]!=0 ||
        source[0x48/4]!=0x1c || source[0x50/4]!=0x20000000 || source[0x5c/4]!=0 ||
        source[0x70/4]!=0 || source[0x78/4]!=0x16 || source[0x90/4]!=0x15) reason|=4;
    uint32_t physical=source_address&~0x40000000u;
    if ((source_address&0xf0000000u)!=0x40000000u || (physical&127) ||
        physical<0x08000000 || !ram_span(source_address,WIDTH*3456*2)) reason|=8;
    /* Canon must have no active ArtFilter capture registration to overwrite.
     * Normal XPan has no such owner. Never switch Canon's Creative Filter mode. */
    if (*(volatile uint32_t *)0x54cb0 || busy) reason|=16;
    if (!edmac_find_divider(COPY_BYTES,0) ||
        !edmac_find_divider(WIDTH*16u*2u,0)) reason|=32; /* Forbid CPU-copy fallback. */
    struct card_info *card=get_shooting_card();
    if (!card) reason|=64;
    if (reason) {
        skipped++;
            status="Grain skipped";
        epoch++; return;
    }
    if (last_card==card && last_file==(unsigned)card->file_number &&
        last_folder==(unsigned)card->folder_number) return;
    unsigned started=(uint32_t)get_us_clock(); busy=1; chunks=0;
    init_result=process_result=cleanup_result=copy_ok=0;
    stream_copies=stream_bytes=stream_fill_us=stream_position=0;
    stream_first_origin=stream_last_origin=0;
    for (unsigned i=0;i<7;i++) copy_status[i]=0;
    copy_status[0]=(uint32_t)-3; /* No copy attempted for this capture yet. */
    /* Exact twelve-argument resource initialization from FF163228..260.
     * It registers native channels and a semaphore; it changes no camera mode. */
    typedef unsigned (*init_fn)(unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,
        unsigned,unsigned,unsigned,unsigned,unsigned,unsigned);
    init_result=((init_fn)0xff41a928)(0xff854880,43,0xff85492c,17,
        0xff8549b4,4,13,4,26,4,20,0);
    if (!init_result) {
        /* Select a photo-specific start in Canon's randomized offset table.
         * V3's continuous single-origin ring removed seams but exposed strong
         * translated texture repeats in test53. Use the native origin changes
         * again, while preserving the same global field across strip guards. */
        offset_phase=(card->file_number+(unsigned)get_ms_clock())%ASSET_OFFSETS;
        if (xpan_photo_is_panoramic()) {
            /* Keep the accepted 65:24 crop processing geometry unchanged. */
            for (unsigned y=0;y<PANORAMIC_CROP_ROWS;y+=CORE_ROWS) {
                if (!process_strip(source_address,
                    PANORAMIC_CROP_TOP+y-GUARD_ROWS,
                    PANORAMIC_CROP_TOP+y,GUARD_ROWS,CORE_ROWS,y)) break;
            }
        } else {
            /* Native 3:2 has no extra rows around the frame. Leave four edge
             * rows untouched, process full guarded tiles, then copy a safe
             * 16-row tail from an in-bounds 32-row native read. */
            if (process_strip(source_address,0,GUARD_ROWS,GUARD_ROWS,CORE_ROWS,GUARD_ROWS)) {
                for (unsigned y=GUARD_ROWS+CORE_ROWS;
                     y+CORE_ROWS<=FULL_ROWS-GUARD_ROWS-16u;y+=CORE_ROWS) {
                    if (!process_strip(source_address,y-GUARD_ROWS,y,
                        GUARD_ROWS,CORE_ROWS,y)) break;
                }
                if (!reason) {
                    /* Read rows 3424..3455 and retain only rows 3436..3451. */
                    process_strip(source_address,FULL_ROWS-STRIP_ROWS,
                        FULL_ROWS-GUARD_ROWS-16u,12u,16u,
                        FULL_ROWS-STRIP_ROWS+GUARD_ROWS);
                }
            }
        }
    } else {
        reason=1024;
    }
    if (!private_run) wm_capture_hook(regs,pc);   /* still holding the resources */
    if (*(volatile uint32_t *)0x54cb0)
        cleanup_result=((unsigned (*)(void))0xff41ab74)();
    if (cleanup_result) reason|=2048;
    elapsed_us=(uint32_t)get_us_clock()-started; busy=0;
    last_card=card; last_file=card->file_number; last_folder=card->folder_number;
    unsigned expected_chunks=xpan_photo_is_panoramic() ?
        PANORAMIC_CROP_ROWS/CORE_ROWS : 144u;
    if (!reason && chunks==expected_chunks) { applied++; status="Ready; last JPEG grained"; }
    else { skipped++; status="Grain interrupted"; }
    struct grain_capture_record *record=&history[history_count%COUNT(history)];
    record->file=last_file; record->folder=last_folder; record->strength=prepared_strength;
    record->phase=offset_phase; record->strips=chunks; record->reason=reason;
    record->us=elapsed_us; record->noise_us=stream_fill_us; record->copies=stream_copies;
    /* Snapshot the exact settings and native result for this file, not later menu state. */
    record->table_hash=linear ? hash_bytes((const uint8_t *)linear,4096):0;
    record->seed_hash=seed_hash; record->tone_probe=private_run ? 1u:!!grain_tone_probe;
    record->stock_table=private_run ? 0u:!!grain_stock_tone_probe;
    record->panoramic=!!xpan_photo_is_panoramic();
    record->native_return=process_result;
    record->published=(!private_run && !reason && chunks==expected_chunks);
    for (unsigned i=0;i<7;i++) record->copy_status[i]=copy_status[i];
    history_count++;
    epoch++;
}

/* ---------------------------------------------------------------------------
 * Watermark: a logo blended into the top-left or bottom-left corner of the
 * saved JPEG, at the same hook as the grain (finished YUV image, before JPEG
 * encoding), after grain. Optionally the name of the LUT applied to that
 * photo is written to the right of the logo, centred on it, in the logo's
 * colour.
 * The PNGs are converted once on a PC (extras/XPan_tools/xpan_watermark.py)
 * into ML/XPAN/WMARK_1.BIN (orange) and WMARK_2.BIN (white), each with two
 * sizes (small 386 px, big 776 px wide), per pixel pair Y0 A0 Y1 A1 Cb Cr
 * (full-range BT.601, signed chroma). The text font is ML/XPAN/WMFONT.BIN
 * (extras/XPan_tools/xpan_wmfont.py): anti-aliased ASCII glyphs at the two
 * sizes, alpha only.
 * Only the selected logo is kept in RAM; a colour change loads the other one
 * at the next idle point. Each file is read at most once per boot (copy it,
 * then restart the camera). RAW files are never touched.
 * --------------------------------------------------------------------------- */
/* Canon's finished YUV buffer stores signed chroma (0 = neutral). If logos
 * ever come out tinted green/magenta, set this to 0 (offset-128 chroma). */
#define WM_CHROMA_SIGNED 1
#define WM_MAGIC 0x4d575058u            /* "XPWM" */
#define WM_VERSION 2u
#define WM_SIZES 2u
#define WM_MARGIN 128u                   /* pixels from the frame edges */
#define WM_MAX_BYTES (2u*1024u*1024u)
#define WF_MAGIC 0x46575058u            /* "XPWF" */
#define WF_VERSION 1u
#define WF_MAX_BYTES (1024u*1024u)
#define WM_COLORS 2u
/* XPan > Watermark: ON/OFF, then the options in its submenu. */
static CONFIG_INT("xpan.wm.on",wm_enabled,0);
static CONFIG_INT("xpan.wm.color",wm_color,0);    /* 0 orange (WMARK_1), 1 white (WMARK_2) */
static CONFIG_INT("xpan.wm.place",wm_place,1);    /* 0 top left, 1 bottom left */
static CONFIG_INT("xpan.wm.size",wm_size,0);      /* 0 small, 1 big */
static CONFIG_INT("xpan.wm.lut",wm_lut_name,1);   /* LUT name next to the logo */
/* Text colour per logo, BT.601 full range like the logo files:
 * #ff3300 -> Y 106, Cb -60, Cr 106; white -> Y 255, Cb 0, Cr 0. */
static const int wm_text_yuv[WM_COLORS][3]={{106,-60,106},{255,0,0}};
static uint8_t *wm_file_data;
static const uint8_t *wm_data_size[WM_SIZES];
static unsigned wm_w_size[WM_SIZES],wm_h_size[WM_SIZES];
static unsigned wm_ready,wm_attempted,wm_applied,wm_skipped;
static int wm_loaded_color=-1;
static struct card_info *wm_last_card;
static unsigned wm_last_file=0xffffffffu,wm_last_folder=0xffffffffu,wm_us;
static const char *wm_status="OFF";
int xpan_watermark_enabled(void) { return wm_enabled==1; }
static unsigned wm_selected(void) { return wm_size==1 ? 1u:0u; }
static unsigned wm_selected_color(void) { return wm_color==1 ? 1u:0u; }
/* Whole file into one malloc'd buffer through the small DMA bounce buffer
 * used by the grain asset. NULL with *error set on failure. */
static uint8_t *wm_read_file(struct card_info *card,const char *name,unsigned min_bytes,
    unsigned max_bytes,unsigned *bytes,const char **error)
{
    char path[FIO_MAX_PATH_LENGTH];
    snprintf(path,sizeof(path),"%s:/ML/XPAN/%s",card->drive_letter,name);
    uint32_t file_bytes=0;
    if (FIO_GetFileSize(path,&file_bytes) || file_bytes<min_bytes || file_bytes>max_bytes)
    { *error="missing (restart after copying)"; return NULL; }
    FILE *file=FIO_OpenFile(path,O_RDONLY|O_SYNC);
    if (!file) { *error="cannot be opened"; return NULL; }
    uint8_t *io=__mem_malloc(4096,MEM_DMA,__FILE__,__LINE__);
    uint8_t *data=io ? malloc(file_bytes):NULL;
    int ok=data!=NULL;
    for (unsigned pos=0;ok && pos<file_bytes;) {
        unsigned n=MIN(4096u,file_bytes-pos);
        ok=FIO_ReadFile(file,io,n)==(int)n;
        if (ok) memcpy(data+pos,io,n);
        pos+=n;
    }
    if (io) __mem_free(io);
    FIO_CloseFile(file);
    if (!ok) { *error=data ? "short read":"no memory"; if (data) free(data); return NULL; }
    *bytes=file_bytes;
    return data;
}
/* The selected logo colour; switching colours reloads it from the card.
 * A file that failed is not retried until the next boot. Called only from
 * the idle poll, so the capture hook never sees a buffer being freed. */
static int wm_load(struct card_info *card)
{
    unsigned color=wm_selected_color();
    if (wm_ready && wm_loaded_color==(int)color) return 1;
    if (wm_attempted&(1u<<color)) return 0;   /* this colour failed before */
    wm_attempted|=1u<<color;
    wm_ready=0;
    if (wm_file_data) { free(wm_file_data); wm_file_data=NULL; }
    wm_loaded_color=-1;
    const char *error="";
    unsigned bytes=0;
    uint8_t *data=wm_read_file(card,color ? "WMARK_2.BIN":"WMARK_1.BIN",65,64+WM_MAX_BYTES,&bytes,&error);
    if (!data) { wm_status=color ? "WMARK_2.BIN (white) not loaded":"WMARK_1.BIN (orange) not loaded"; return 0; }
    const uint32_t *header=(const uint32_t *)data;
    int ok=header[0]==WM_MAGIC && header[1]==WM_VERSION && header[2]==WM_SIZES;
    for (unsigned i=0;ok && i<WM_SIZES;i++) {
        unsigned w=header[3+i*4],h=header[4+i*4],offset=header[5+i*4],size=header[6+i*4];
        ok=w>=2 && !(w&1) && w<=WIDTH-2*WM_MARGIN && h>=1 && h<=FULL_ROWS &&
            size==w/2*h*6 && offset>=64 && offset<=bytes && size<=bytes-offset;
    }
    if (!ok) { free(data); wm_status="Invalid watermark file"; return 0; }
    for (unsigned i=0;i<WM_SIZES;i++) {
        wm_w_size[i]=header[3+i*4]; wm_h_size[i]=header[4+i*4];
        wm_data_size[i]=data+header[5+i*4];
    }
    wm_file_data=data; wm_loaded_color=color;
    wm_attempted&=~(1u<<color);               /* loaded: may be reloaded later */
    wm_ready=1; wm_status="Ready; watermark on saved JPEG";
    return 1;
}
/* LUT name font: glyphs 'first'..'first+count-1' per size. */
struct wf_glyph { int16_t x,y; uint16_t w,h; uint32_t offset; };
static uint8_t *wf_data;
static const struct wf_glyph *wf_glyphs[WM_SIZES];
static unsigned wf_advance_q6[WM_SIZES],wf_cap[WM_SIZES],wf_gap[WM_SIZES];
static unsigned wf_first,wf_count,wf_ready,wf_attempted;
static const char *wf_status="";
static int wf_load(struct card_info *card)
{
    if (wf_ready) return 1;
    if (wf_attempted) return 0;
    wf_attempted=1;
    const char *error="";
    unsigned bytes=0;
    uint8_t *data=wm_read_file(card,"WMFONT.BIN",64,WF_MAX_BYTES,&bytes,&error);
    if (!data) { wf_status="WMFONT.BIN not loaded"; return 0; }
    const uint32_t *header=(const uint32_t *)data;
    unsigned count=header[4];
    int ok=header[0]==WF_MAGIC && header[1]==WF_VERSION && header[2]==WM_SIZES &&
        header[3]>=32 && count>=1 && header[3]+count<=127;
    for (unsigned i=0;ok && i<WM_SIZES;i++) {
        unsigned advance=header[6+i*5],cap=header[7+i*5],table=header[9+i*5];
        ok=advance>=64 && cap>=4 && cap<FULL_ROWS && table>=64 && !(table&3) &&
            table<=bytes && count*sizeof(struct wf_glyph)<=bytes-table;
        for (unsigned g=0;ok && g<count;g++) {
            const struct wf_glyph *glyph=(const struct wf_glyph *)(data+table)+g;
            ok=glyph->offset<=bytes && (unsigned)glyph->w*glyph->h<=bytes-glyph->offset;
        }
    }
    if (!ok) { free(data); wf_status="Invalid WMFONT.BIN"; return 0; }
    wf_first=header[3]; wf_count=count;
    for (unsigned i=0;i<WM_SIZES;i++) {
        wf_advance_q6[i]=header[6+i*5]; wf_cap[i]=header[7+i*5]; wf_gap[i]=header[8+i*5];
        wf_glyphs[i]=(const struct wf_glyph *)(data+header[9+i*5]);
    }
    wf_data=data; wf_ready=1; wf_status="";
    return 1;
}
static inline int wm_mix(int base,int logo,unsigned a)
{
    return base+(((logo-base)*(int)a*257+32768)>>16);   /* a/255 */
}
/* Blend one pixel pair (U Y0 V Y1) towards Y0/Y1 and a shared chroma. */
static inline void wm_blend_pair(volatile uint32_t *p,int ly0,unsigned a0,int ly1,unsigned a1,int lu,int lv)
{
    uint32_t w=*p;
    int y0v=(w>>8)&255,y1v=w>>24;
    y0v=wm_mix(y0v,ly0,a0); y1v=wm_mix(y1v,ly1,a1);
    unsigned ac=(a0+a1+1)/2;
#if WM_CHROMA_SIGNED
    int u=(int8_t)(w&255),v=(int8_t)((w>>16)&255);
    u=wm_mix(u,lu,ac); v=wm_mix(v,lv,ac);
    u=COERCE(u,-128,127); v=COERCE(v,-128,127);
#else
    int u=w&255,v=(w>>16)&255;
    u=wm_mix(u,lu+128,ac); v=wm_mix(v,lv+128,ac);
    u=COERCE(u,0,255); v=COERCE(v,0,255);
#endif
    y0v=COERCE(y0v,0,255); y1v=COERCE(y1v,0,255);
    *p=((uint32_t)(u&255))|((uint32_t)y0v<<8)|((uint32_t)(v&255)<<16)|((uint32_t)y1v<<24);
}
/* Blend one logo row (pairs of pixels) into the image row. */
static void wm_blend_row(volatile uint32_t *row,const uint8_t *src,unsigned pairs)
{
    for (unsigned i=0;i<pairs;i++,src+=6) {
        unsigned a0=src[1],a1=src[3];
        if (!(a0|a1)) continue;
        wm_blend_pair(row+i,src[0],a0,src[2],a1,(int8_t)src[4],(int8_t)src[5]);
    }
}
/* LUT name of the current photo: characters, pen origin (even x, baseline
 * row relative to the logo top) and colour. Set by wm_apply per photo. */
static struct {
    char text[48];
    unsigned length,x,size,color;
    int baseline;
} wm_text;
/* Blend the text pixels that fall on logo row 'y' (row = start of image row). */
static void wm_text_row(volatile uint32_t *row,int y)
{
    const struct wf_glyph *glyphs=wf_glyphs[wm_text.size];
    const int *yuv=wm_text_yuv[wm_text.color];
    for (unsigned i=0;i<wm_text.length;i++) {
        unsigned c=(unsigned char)wm_text.text[i]-wf_first;
        if (c>=wf_count) continue;
        const struct wf_glyph *g=&glyphs[c];
        int top=wm_text.baseline+g->y;
        if (y<top || y>=top+(int)g->h || !g->w) continue;
        const uint8_t *alpha=wf_data+g->offset+(unsigned)(y-top)*g->w;
        int gx=(int)wm_text.x+(int)((i*wf_advance_q6[wm_text.size]+32)>>6)+g->x;
        for (int x=gx&~1;x<gx+(int)g->w;x+=2) {
            if (x<0 || x+1>=(int)WIDTH) continue;
            unsigned a0=x>=gx ? alpha[x-gx]:0, a1=x+1<gx+(int)g->w ? alpha[x+1-gx]:0;
            if (a0|a1) wm_blend_pair(row+x/2,yuv[0],a0,yuv[0],a1,yuv[1],yuv[2]);
        }
    }
}
/* Fill wm_text for this photo; 0 when there is no name to write. */
static int wm_text_prepare(unsigned sel,unsigned x0,unsigned w,unsigned h)
{
    wm_text.length=0;
    if (wm_lut_name!=1 || !wf_ready) return 0;
    char name[48];
    if (!xpan_native_look_photo_lut(name,sizeof(name))) return 0;
    unsigned advance=wf_advance_q6[sel];
    wm_text.x=(x0+w+wf_gap[sel]+1)&~1u;
    /* Fit inside the right-hand margin; cut long names with "...". */
    unsigned room=WIDTH-WM_MARGIN>wm_text.x ? ((WIDTH-WM_MARGIN-wm_text.x)<<6)/advance:0;
    unsigned n=0;
    for (const char *s=name;*s && n<sizeof(wm_text.text)-1;s++) {
        unsigned char c=*s;
        wm_text.text[n++]=c>=wf_first && c<wf_first+wf_count ? c:'?';
    }
    if (n>room) { if (room<4) return 0; n=room-3; memcpy(wm_text.text+n,"...",3); n+=3; }
    wm_text.text[n]=0; wm_text.length=n;
    wm_text.size=sel; wm_text.color=(unsigned)wm_loaded_color;   /* same as the logo in RAM */
    /* Centre the capital height on the logo. */
    wm_text.baseline=(int)(h/2+wf_cap[sel]/2);
    return n>0;
}
/* Change detector: 16 words (pixel pairs) spread across the logo row.
 * Canon rewrites whole rows, and sensor noise changes every word, so a few
 * samples catch it; reading the full row from uncached RAM cost ~10 ms per
 * check for the big logo, this costs well under 1 ms. */
#define WM_SAMPLES 16u
static uint32_t wm_hash_words(const volatile uint32_t *p,unsigned words)
{
    uint32_t h=2166136261u;
    unsigned step=MAX(1u,words/WM_SAMPLES);
    for (unsigned i=step/2;i<words;i+=step) h=(h^p[i])*16777619u;
    return h;
}
/* At this hook Canon is still writing the lower part of the frame (top-down):
 * without grain (which takes long) a bottom-corner logo was overwritten,
 * fully or in its upper rows (IMG_9631/9636); top corners always survived.
 * So blend, then keep checking the logo rows: any row that changes was
 * rewritten by Canon and is blended again on the new data. Stop once the
 * band just below the logo has changed (Canon's writer has passed it), or
 * after WM_WAIT_MS if nothing changes (the frame was already complete).
 * Canon writes top-down, so rows above the lowest changed row are final and
 * are not checked again. The LUT name sits on the logo rows, so the same
 * rows (hashed across logo and text) cover it. */
#define WM_WAIT_MS 400
#define WM_MAX_ROWS 1700u
static uint32_t *wm_row_hash;
static unsigned wm_repairs;
static void wm_apply(unsigned address,unsigned top,unsigned rows)
{
    unsigned sel=wm_selected();
    unsigned w=wm_w_size[sel],h=wm_h_size[sel],pairs=w/2u;
    if (h+2*WM_MARGIN>rows || h>WM_MAX_ROWS) { wm_skipped++; wm_status="Watermark taller than the frame"; return; }
    if (!wm_row_hash) wm_row_hash=malloc(WM_MAX_ROWS*sizeof(uint32_t));
    unsigned x0=WM_MARGIN;
    unsigned y0=wm_place==0 ? top+WM_MARGIN : top+rows-WM_MARGIN-h;
    const uint8_t *src=wm_data_size[sel];
    int text=wm_text_prepare(sel,x0,w,h);
    /* Hashed span: the logo, plus the text to its right. */
    unsigned span=text ? (WIDTH-WM_MARGIN-x0)/2u:pairs;
    #define WM_LINE(y) ((volatile uint32_t *)(address+(y0+(y))*WIDTH*2u))
    #define WM_ROW(y) (WM_LINE(y)+x0/2u)
    #define WM_BLEND(y) do { wm_blend_row(WM_ROW(y),src+(y)*pairs*6u,pairs); \
        if (text) wm_text_row(WM_LINE(y),(int)(y)); } while (0)
    /* Sentinel band: 8 rows just below the logo (inside the frame margin). */
    unsigned band_y=y0+h,band_rows=MIN(8u,top+rows-band_y);
    uint32_t band0=0;
    for (unsigned r=0;r<band_rows;r++) band0^=wm_hash_words(WM_ROW(h+r),span)+r;
    for (unsigned y=0;y<h;y++) {
        WM_BLEND(y);
        if (wm_row_hash) wm_row_hash[y]=wm_hash_words(WM_ROW(y),span);
    }
    wm_repairs=0;
    if (wm_row_hash) {
        int start=get_ms_clock(),passed=0;
        unsigned first=0;                      /* rows above this are final */
        while (get_ms_clock()-start<WM_WAIT_MS) {
            msleep(10);
            unsigned lowest=first;
            for (unsigned y=first;y<h;y++) {
                if (wm_hash_words(WM_ROW(y),span)==wm_row_hash[y]) continue;
                WM_BLEND(y);
                wm_row_hash[y]=wm_hash_words(WM_ROW(y),span); wm_repairs++;
                lowest=y;
            }
            first=lowest;                      /* re-check that row: it may be mid-write */
            if (passed) break;                 /* one last pass after the writer */
            uint32_t band=0;
            for (unsigned r=0;r<band_rows;r++) band^=wm_hash_words(WM_ROW(h+r),span)+r;
            if (band!=band0) passed=1;
        }
    }
    #undef WM_BLEND
    #undef WM_ROW
    #undef WM_LINE
    wm_applied++; wm_status=wm_repairs ? "Ready; last logo re-applied after Canon" : "Ready; last JPEG watermarked";
}
static void wm_capture_hook(uint32_t *regs,uint32_t pc)
{
    if (!xpan_watermark_enabled() || !wm_ready || !xpan_photo_enabled() ||
        is_movie_mode() || ml_shutdown_requested || pc!=CAPTURE_SITE) return;
    /* Same finished-YUV JPEG descriptor checks as the grain (quality 8 only). */
    if (regs[7]!=0x70298) return;
    const volatile uint32_t *source=(const volatile uint32_t *)regs[7];
    if (source[0x44/4]!=8) return;
    if (source[0]!=0xff90f600 || source[1]!=0x26 || source[2]!=0xff9125a8 ||
        source[0x38/4]!=0x1440ffff || source[0x3c/4]!=3456 || source[0x40/4]!=0)
    { wm_skipped++; wm_status="Watermark skipped (descriptor)"; return; }
    uint32_t address=source[0x58/4],physical=address&~0x40000000u;
    if ((address&0xf0000000u)!=0x40000000u || (physical&127) ||
        physical<0x08000000 || !ram_span(address,WIDTH*3456*2))
    { wm_skipped++; wm_status="Watermark skipped (buffer)"; return; }
    struct card_info *card=get_shooting_card();
    if (!card) return;
    /* Never blend twice into the same file (alpha would accumulate). */
    if (wm_last_card==card && wm_last_file==(unsigned)card->file_number &&
        wm_last_folder==(unsigned)card->folder_number) return;
    unsigned started=(uint32_t)get_us_clock();
    unsigned panoramic=!!xpan_photo_is_panoramic();
    wm_apply(address,panoramic ? PANORAMIC_CROP_TOP:0,panoramic ? PANORAMIC_CROP_ROWS:FULL_ROWS);
    wm_us=(uint32_t)get_us_clock()-started;
    wm_last_card=card; wm_last_file=card->file_number; wm_last_folder=card->folder_number;
}
/* One Canon hook: grain first (it may be OFF), then the watermark on top. */
static void capture_hook(uint32_t *regs,uint32_t *stack,uint32_t pc)
{
    grain_capture_hook(regs,stack,pc);      /* may already have placed the logo */
    if (!xpan_watermark_enabled() || !wm_ready || pc!=CAPTURE_SITE || regs[7]!=0x70298) return;
    struct card_info *card=get_shooting_card();
    if (!card || (wm_last_card==card && wm_last_file==(unsigned)card->file_number &&
        wm_last_folder==(unsigned)card->folder_number)) return;
    wm_capture_hook(regs,pc);
}
static int install(void)
{
    if (installed==7) return 1;
    if (install_attempted) return 0;
    install_attempted=1;
    if (!map_valid()) { status="Grain ROM map mismatch"; return 0; }
    patch_error=patch_hook_function(INPUT_SITE,0xe92d4010,input_hook,"XPan JPEG grain colour");
    if (!patch_error) installed|=1;
    if (!patch_error) {
        patch_error=patch_hook_function(CONFIG_SITE,0xe28dd098,config_hook,"XPan JPEG grain verify");
        if (!patch_error) installed|=2;
    }
    if (!patch_error) {
        patch_error=patch_hook_function(CAPTURE_SITE,0xe51f11b4,capture_hook,"XPan saved JPEG grain");
        if (!patch_error) installed|=4;
    }
    if (patch_error) {
        if (installed&4) unpatch_memory(CAPTURE_SITE);
        if (installed&2) unpatch_memory(CONFIG_SITE);
        if (installed&1) unpatch_memory(INPUT_SITE);
        installed=0; status="Cannot install JPEG grain";
    }
    return installed==7;
}
static MENU_SELECT_FUNC(select_grain)
{
    if (!idle()) return;
    film_grain=!film_grain; ready=0; epoch++;
    if (film_grain) { attempted_card=NULL; status="Preparing grain"; }
    else status="OFF";
}
static MENU_SELECT_FUNC(select_strength)
{
    if (!idle()) return;
    grain_strength=COERCE(grain_strength+(delta<0 ? -1:1),1,100);
    ready=0; epoch++; if (film_grain) status="Preparing grain";
}
static MENU_SELECT_FUNC(select_engine)
{
    if (!idle()) return;
    grain_engine=!grain_engine; ready=0; epoch++; if (film_grain) status="Preparing grain";
}
static MENU_UPDATE_FUNC(display_engine)
{
    MENU_SET_VALUE("%s",grain_engine ? "Canon":"Film");
    if (!film_grain) MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Film grain is OFF.");
    MENU_SET_HELP("Film: soft, brightness-dependent grain added by the CPU (%d ms last). Canon: native Grainy filter.",(int)(film_us/1000));
}
static MENU_SELECT_FUNC(select_size)
{
    if (!idle()) return;
    grain_size=(grain_size+(delta<0 ? 2:1))%3; ready=0; epoch++; if (film_grain) status="Preparing grain";
}
static MENU_UPDATE_FUNC(display_size)
{
    static const char *const names[3]={"Fine","Medium","Coarse"};
    MENU_SET_VALUE("%s",names[COERCE(grain_size,0,2)]);
    MENU_SET_HELP("Film grain size (texture softness).");
    if (!film_grain) MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Film grain is OFF.");
    else if (grain_engine) MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Only for Grain type = Film.");
}
static MENU_UPDATE_FUNC(display_grain)
{
    /* The menu's green status dot follows the real capture switch, not availability. */
    MENU_SET_ENABLED(film_grain);
    MENU_SET_VALUE("%s",film_grain ? "ON":"OFF");
    MENU_SET_HELP("SET: ON/OFF, PLAY: options. Saved JPEG only; Canon Creative Filters must be OFF. %s",status);
    if (film_grain && !ready) MENU_SET_WARNING(MENU_WARN_ADVICE,"%s",status);
}
static MENU_UPDATE_FUNC(display_strength)
{
    MENU_SET_VALUE("%d%s",COERCE(grain_strength,1,100),"%");
    MENU_SET_ENABLED(1);
    MENU_SET_HELP("25 is the accepted Subtle recipe;100 is the original strong grain. Live view has no grain.");
    if (!film_grain) MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Film grain is OFF.");
}
static MENU_SELECT_FUNC(select_watermark)
{
    wm_enabled=!wm_enabled;
    if (wm_enabled) wm_status="Loading watermark";
}
static MENU_UPDATE_FUNC(display_watermark)
{
    MENU_SET_ENABLED(xpan_watermark_enabled());
    MENU_SET_VALUE("%s",xpan_watermark_enabled() ? "ON":"OFF");
    MENU_SET_HELP("SET: ON/OFF, PLAY: options. %s (%d ms, %d rows redone).",
        wm_status,(int)(wm_us/1000),(int)wm_repairs);
    if (xpan_watermark_enabled() && !wm_ready) MENU_SET_WARNING(MENU_WARN_ADVICE,"%s",wm_status);
}
/* Values from older builds (four corners, Medium) are folded into range. */
static MENU_UPDATE_FUNC(display_watermark_option)
{
    if (wm_color<0 || wm_color>1) wm_color=0;
    if (wm_place<0 || wm_place>1) wm_place=1;
    if (wm_size<0 || wm_size>1) wm_size=1;
    if (wm_lut_name<0 || wm_lut_name>1) wm_lut_name=1;
    if (!xpan_watermark_enabled()) MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Watermark is OFF.");
}
static MENU_UPDATE_FUNC(display_watermark_color)
{
    display_watermark_option(entry,info);
    if (xpan_watermark_enabled() && wm_ready && wm_loaded_color!=(int)wm_selected_color())
        MENU_SET_WARNING(MENU_WARN_ADVICE,"Loading %s",wm_color ? "WMARK_2.BIN":"WMARK_1.BIN");
    else if (xpan_watermark_enabled() && !wm_ready) MENU_SET_WARNING(MENU_WARN_ADVICE,"%s",wm_status);
}
static MENU_UPDATE_FUNC(display_watermark_lut)
{
    display_watermark_option(entry,info);
    if (xpan_watermark_enabled() && wm_lut_name==1 && wf_attempted && !wf_ready)
        MENU_SET_WARNING(MENU_WARN_ADVICE,"%s",wf_status);
}
static struct menu_entry entries[]={
    { .name="Film grain",.priv=&film_grain,.max=1,.select=select_grain,.update=display_grain,
      .select_Q=menu_open_submenu,
      .help="Optional native grain on the saved JPEG, independent of Canon Creative Filters.",
      .children=(struct menu_entry[]) {
        { .name="Grain strength",.priv=&grain_strength,.min=1,.max=100,.unit=UNIT_PERCENT,
          .select=select_strength,.update=display_strength,.help="Strength1..100; default25 Subtle." },
        { .name="Grain type",.priv=&grain_engine,.max=1,.select=select_engine,.update=display_engine,
          .choices=CHOICES("Film","Canon"),
          .help="Film: soft, brightness-dependent grain (CPU). Canon: native Grainy filter." },
        { .name="Grain size",.priv=&grain_size,.max=2,.select=select_size,.update=display_size,
          .choices=CHOICES("Fine","Medium","Coarse"),
          .help="Film grain size." },
        MENU_EOL
      } },
    { .name="Watermark",.priv=&wm_enabled,.max=1,.select=select_watermark,.update=display_watermark,
      .select_Q=menu_open_submenu,
      .help="XPan logo in a corner of the saved JPEG, optionally with the LUT name.",
      .children=(struct menu_entry[]) {
        { .name="Logo colour",.priv=&wm_color,.max=1,.update=display_watermark_color,
          .choices=CHOICES("Orange","White"),
          .help="Orange: WMARK_1.BIN, LUT name in #ff3300. White: WMARK_2.BIN, white LUT name." },
        { .name="Position",.priv=&wm_place,.max=1,.update=display_watermark_option,
          .choices=CHOICES("Top left","Bottom left"),
          .help="Logo in the top-left or bottom-left corner; the LUT name goes to its right." },
        { .name="Size",.priv=&wm_size,.max=1,.update=display_watermark_option,
          .choices=CHOICES("Small","Big"),
          .help="Logo width: small 386 px, big 776 px (of the 5184 px image)." },
        { .name="LUT name",.priv=&wm_lut_name,.max=1,.update=display_watermark_lut,
          .choices=CHOICES("OFF","ON"),
          .help="Name of the LUT applied to the photo, next to the logo (font: ML/XPAN/WMFONT.BIN)." },
        MENU_EOL
      } },
};
void xpan_film_grain_add_menu(void) { menu_add("XPan",entries,COUNT(entries)); }
void xpan_film_grain_poll(struct card_info *card)
{
    if (!card || !idle()) return;
    unsigned expected_tone=!!grain_tone_probe,expected_stock=!!grain_stock_tone_probe;
    if ((!film_grain && !shadow_requested && !tone_ramp_requested) ||
        !xpan_photo_enabled() || is_movie_mode()) {
        ready=0;
        if (!film_grain) status="OFF";
    } else if (!grain_engine && !shadow_requested && !tone_ramp_requested) {
        /* Film engine: CPU texture only; the capture hook is still needed. */
        if (!film_ready || film_strength!=(unsigned)COERCE(grain_strength,1,100) ||
            film_size!=(unsigned)COERCE(grain_size,0,2) || !ready) {
            ready=0;
            if (film_prepare() && install()) { ready=1; status="Ready; film grain on saved JPEG"; epoch++; }
        }
    } else if (!ready || asset_card!=card || prepared_strength!=(unsigned)COERCE(grain_strength,1,100) ||
        (unsigned)prepared_tone_probe!=expected_tone ||
        (unsigned)prepared_stock_tone_probe!=expected_stock) {
        ready=0;
        if (prepare(card,shadow_requested || tone_ramp_requested) && install()) {
            ready=1;
            status=(shadow_requested || tone_ramp_requested) ?
                "Diagnostic ready; take one JPEG" : "Ready; saved JPEG only";
            epoch++;
        }
    }
    /* Watermark: needs the same capture hook even with grain OFF. */
    if (xpan_watermark_enabled() && xpan_photo_enabled() && !is_movie_mode()) {
        if (wm_load(card) && !install()) wm_status="Cannot install JPEG hook";
        if (wm_lut_name==1) wf_load(card);
    }
}
