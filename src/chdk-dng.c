/**
 * DNG saving routines ported from CHDK
 * Code stripped down a bit, since we don't care about GPS and advanced exif stuff (at least for now)
 * 
 * TODO: make it platform-independent and move it to modules.
 */

/*
 * Original code copyright (C) CHDK (GPLv2); ported to Magic Lantern (2013)
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */


#undef RAW_DEBUG_BLACK

#ifdef CONFIG_MAGICLANTERN
#include "dryos.h"
#include "property.h"
#include "math.h"
#include "string.h"
#define umalloc fio_malloc
#define ufree fio_free
#define pow powf

static int32_t get_tick_count() { return get_ms_clock(); }

#ifdef CONFIG_EOSM
static struct semaphore *dng_writer_sem;

static void dng_writer_init(void *unused)
{
    (void)unused;
    /* DNG crop tags use shared storage, so serialize EOS M sidecars with other DNG saves. */
    dng_writer_sem = create_named_semaphore("dng_writer", SEM_CREATE_UNLOCKED);
}

INIT_FUNC("dng_writer", dng_writer_init);
#endif

#else // if we compile it for desktop
#include "stdint.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "string.h"
#include "math.h"
#include <sys/types.h>
#define FAST
#define UNCACHEABLE(x) (x)
#define umalloc malloc
#define ufree free

#define FIO_CreateFile(name) fopen(name, "wb")
#define FIO_WriteFile(f, ptr, count) fwrite(ptr, 1, count, f)
#define FIO_CloseFile(f) fclose(f)
#define FIO_RemoveFile(f) unlink(f)

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define COERCE(x,lo,hi) MAX(MIN((x),(hi)),(lo))

#define INVALID_PTR             ((void *)0xFFFFFFFF)

#endif

#include "raw.h"
#include "chdk-dng.h"
#include "xpan-mode.h"
#ifdef CONFIG_EOSM
#include "fio-ml.h"
#endif

/* adaptations from CHDK to ML */
#define camera_sensor (*raw_info)
#define raw_rowpix width
#define raw_rows height
#define raw_size frame_size
#define write FIO_WriteFile

void FAST reverse_bytes_order(char* buf, int32_t count)
{
#ifdef __ARM__
    /* optimized swap from g3gg0 */
    asm volatile ("\
        /* r2 = A B C D */\r\n\
        /* r5 = 0 B 0 D */\r\n\
        /* r2 = 0 A 0 C */\r\n\
        /* r2 = B 0 D 0 | 0 A 0 C */\r\n\
        \
        /* init swap mask */\r\n\
        mov r4, #0xff\r\n\
        orr r4, r4, #0xff0000\r\n\
        \
        _wswap128:\r\n\
        cmp %1, #0x10\r\n\
        blt _wswap64\r\n\
        \
        ldmia %0, {r2, r3, r6, r7}\r\n\
        and r5, r4, r2\r\n\
        and r2, r4, r2, ror #8\r\n\
        orr r2, r2, r5, lsl #8\r\n\
       and r5, r4, r3\r\n\
        and r3, r4, r3, ror #8\r\n\
        orr r3, r3, r5, lsl #8\r\n\
        and r5, r4, r6\r\n\
        and r6, r4, r6, ror #8\r\n\
        orr r6, r6, r5, lsl #8\r\n\
        and r5, r4, r7\r\n\
        and r7, r4, r7, ror #8\r\n\
        orr r7, r7, r5, lsl #8\r\n\
        stmia %0!, {r2, r3, r6, r7}\r\n\
        sub %1, #0x10\r\n\
        b _wswap128\r\n\
        \
        _wswap64:\r\n\
        cmp %1, #0x08\r\n\
        blt _wswap32\r\n\
        \
        ldmia %0, {r2, r3}\r\n\
        and r5, r4, r2\r\n\
        and r2, r4, r2, ror #8\r\n\
        orr r2, r2, r5, lsl #8\r\n\
        and r5, r4, r3\r\n\
        and r3, r4, r3, ror #8\r\n\
        orr r3, r3, r5, lsl #8\r\n\
        stmia %0!, {r2, r3}\r\n\
        sub %1, #0x08\r\n\
        b _wswap64\r\n\
        \
        _wswap32:\r\n\
        cmp %1, #0x04\r\n\
        blt _wswap16\r\n\
        \
        ldmia %0, {r2}\r\n\
        and r5, r4, r2\r\n\
        and r2, r4, r2, ror #8\r\n\
        orr r2, r2, r5, lsl #8\r\n\
        stmia %0!, {r2}\r\n\
        sub %1, #0x04\r\n\
        b _wswap32\r\n\
        \
        _wswap16:\r\n\
        cmp %1, #0x00\r\n\
        beq _wswap_end\r\n\
        ldrh r2, [%0]\r\n\
        mov r3, r2, lsr #8\r\n\
        orr r2, r3, r2, lsl #8\r\n\
        strh r2, [%0]\r\n\
        \
        _wswap_end:\
        " : : "r"(buf), "r"(count) : "r2", "r3", "r4", "r5", "r6", "r7");
#else
    int16_t* buf16 = (int16_t*) buf;
    int32_t i;
    for (i = 0; i < count/2; i++)
    {
        int16_t x = buf16[i];
        buf[2*i+1] = x;
        buf[2*i] = x >> 8;
    }
#endif
}

//thumbnail
static int32_t dng_th_width = 128;
static int32_t dng_th_height = 84;
// higly recommended that dng_th_width*dng_th_height would be divisible by 512

/* warning: not thread safe */
void dng_set_thumbnail_size(int32_t width, int32_t height)
{
    dng_th_width = width;
    dng_th_height = height;
}

struct dir_entry{uint16_t tag; uint16_t type; uint32_t count; unsigned int offset;};

#define T_BYTE      1
#define T_ASCII     2
#define T_SHORT     3
#define T_LONG      4
#define T_RATIONAL  5
#define T_SBYTE     6
#define T_UNDEFINED 7
#define T_SSHORT    8
#define T_SLONG     9
#define T_SRATIONAL 10
#define T_FLOAT     11
#define T_DOUBLE    12
#define T_PTR       0x100   // Stored as long/int16_t etc in DNG header, referenced by pointer in IFD (must be pointer to int32_t variable)
#define T_SKIP      0x200   // Tag value to be skipped (for marking GPS entries if camera does not have GPS)

static const int32_t cam_BaselineNoise[]           = {1,1};
static const int32_t cam_BaselineSharpness[]       = {4,3};
static const int32_t cam_LinearResponseLimit[]     = {1,1};
static const int32_t cam_AnalogBalance[]           = {1,1,1,1,1,1};
static char dng_lens_model[64]              = "";
static char dng_image_desc[64]              = "";
static char cam_name[32]                    = "Canikon";
static char cam_serial[64]                  = "";
static char dng_artist_name[64]             = "";
static char dng_copyright[64]               = "";
static const int16_t cam_PreviewBitsPerSample[]  = {8,8,8};
static const int32_t cam_Resolution[]              = {180,1};
static int32_t cam_AsShotNeutral[6]         = {473635,1000000,1000000,1000000,624000,1000000}; // wbgain default: daylight
static char cam_datetime[20]            = "";                   // DateTimeOriginal
static char cam_subsectime[4]           = "";                   // DateTimeOriginal (milliseconds component)
static int32_t cam_shutter[2]               = { 0, 1000000 };       // Shutter speed
static int32_t cam_aperture[2]              = { 0, 10 };            // Aperture
//static int32_t cam_apex_shutter[2]          = { 0, 96 };            // Shutter speed in APEX units
//static int32_t cam_apex_aperture[2]         = { 0, 96 };            // Aperture in APEX units
static int32_t cam_exp_bias[2]              = { 0, 96 };
static int32_t cam_max_av[2]                = { 0, 96 };
static int32_t cam_focal_length[2]          = { 0, 1000 };
static char* software_ver = "Magic Lantern";
static int32_t cam_FrameRate[]          = {25000,1000};

struct t_data_for_exif{
    int16_t iso;
    int32_t exp_program;
    int32_t effective_focal_length;
    int16_t orientation;
    int16_t flash_mode;
    int16_t flash_fired;
    int16_t metering_mode;
};


#define BE(v)   ((v&0x000000FF)<<24)|((v&0x0000FF00)<<8)|((v&0x00FF0000)>>8)|((v&0xFF000000)>>24)   // Convert to big_endian

#define BADPIX_CFA_INDEX    6   // Index of CFAPattern value in badpixel_opcodes array

static uint32_t badpixel_opcode[] =
{
    // *** all values must be in big endian order

    BE(1),              // Count = 1

    BE(4),              // FixBadPixelsConstant = 4
    BE(0x01030000),     // DNG version = 1.3.0.0
    BE(1),              // Flags = 1
    BE(8),              // Opcode length = 8 bytes
    BE(0),              // Constant = 0
    BE(0),              // CFAPattern (set in code below)
};


static struct t_data_for_exif exif_data;

#define BE(v)   ((v&0x000000FF)<<24)|((v&0x0000FF00)<<8)|((v&0x00FF0000)>>8)|((v&0xFF000000)>>24)   // Convert to big_endian

// warning: according to TIFF format specification, elements must be sorted by tag value in ascending order!

// Index of specific entries in ifd0 below.
static int32_t find_tag_index(struct dir_entry * ifd, int32_t num, uint16_t tag)
{
    int32_t i;
    for (i = 0; i < num; i++)
        if (ifd[i].tag == tag)
            return i;
    
    /* should be unreachable */
#ifdef CONFIG_MAGICLANTERN
    ASSERT(0);
#else
    exit(1);
#endif
    while(1);
    return -1;
}

// Index of specific entries in ifd0 below.
#define CAMERA_NAME_INDEX           find_tag_index(ifd0, DIR_SIZE(ifd0), 0x110)
#define THUMB_DATA_INDEX            find_tag_index(ifd0, DIR_SIZE(ifd0), 0x111)
#define ORIENTATION_INDEX           find_tag_index(ifd0, DIR_SIZE(ifd0), 0x112)
#define CHDK_VER_INDEX              find_tag_index(ifd0, DIR_SIZE(ifd0), 0x131)
#define ARTIST_NAME_INDEX           find_tag_index(ifd0, DIR_SIZE(ifd0), 0x13B)
#define SUBIFDS_INDEX               find_tag_index(ifd0, DIR_SIZE(ifd0), 0x14A)
#define COPYRIGHT_INDEX             find_tag_index(ifd0, DIR_SIZE(ifd0), 0x8298)
#define EXIF_IFD_INDEX              find_tag_index(ifd0, DIR_SIZE(ifd0), 0x8769)
#define DNG_VERSION_INDEX           find_tag_index(ifd0, DIR_SIZE(ifd0), 0xC612)
#define UNIQUE_CAMERA_MODEL_INDEX   find_tag_index(ifd0, DIR_SIZE(ifd0), 0xC614)

#define CAM_MAKE                    "Canon"


// Index of specific entries in ifd1 below.
#define RAW_DATA_INDEX              find_tag_index(ifd1, DIR_SIZE(ifd1), 0x111)
#define BADPIXEL_OPCODE_INDEX       find_tag_index(ifd1, DIR_SIZE(ifd1), 0xC740)

// Index of specific entries in exif_ifd below.
#define EXPOSURE_PROGRAM_INDEX      find_tag_index(exif_ifd, DIR_SIZE(exif_ifd), 0x8822)
#define METERING_MODE_INDEX         find_tag_index(exif_ifd, DIR_SIZE(exif_ifd), 0x9207)
#define FLASH_MODE_INDEX            find_tag_index(exif_ifd, DIR_SIZE(exif_ifd), 0x9209)
#define SSTIME_INDEX                find_tag_index(exif_ifd, DIR_SIZE(exif_ifd), 0x9290)
#define SSTIME_ORIG_INDEX           find_tag_index(exif_ifd, DIR_SIZE(exif_ifd), 0x9291)

static int32_t get_type_size(int32_t type)
{
    switch(type & 0xFF)
    {
    case T_BYTE:
    case T_SBYTE:
    case T_UNDEFINED:
    case T_ASCII:     return 1; 
    case T_SHORT:
    case T_SSHORT:    return 2;
    case T_LONG:
    case T_SLONG:
    case T_FLOAT:     return 4;
    case T_RATIONAL:
    case T_SRATIONAL:
    case T_DOUBLE:    return 8;
    default:          return 0;
    }
}

#define DIR_SIZE(ifd)   (sizeof(ifd)/sizeof(ifd[0]))

#define TIFF_HDR_SIZE (8)

static char* dng_header_buf;
static int32_t dng_header_buf_size;
static int32_t dng_header_buf_offset;
static char *thumbnail_buf;
/* Keep crop-tag values as rationals so fractional sensor-row crops stay exact. */
static int32_t dng_crop_origin[4];
static int32_t dng_crop_size[4];
#ifdef CONFIG_EOSM
/* DNG 1.4 stores the user-selected framing separately from the full active image area. */
static int32_t dng_user_crop[8];
#endif

static void add_to_buf(void* var, int32_t size)
{
    memcpy(dng_header_buf+dng_header_buf_offset,var,size);
    dng_header_buf_offset += size;
}

static void add_val_to_buf(int32_t val, int32_t size)
{
    add_to_buf(&val,size);
}


void dng_set_camname(char *str)
{
    strncpy(cam_name, str, sizeof(cam_name));
}

void dng_set_camserial(char *str)
{
    strncpy(cam_serial, str, sizeof(cam_serial));
}

void dng_set_description(char *str)
{
    strncpy(dng_image_desc, str, sizeof(dng_image_desc));
}

void dng_set_lensmodel(char *str)
{
    strncpy(dng_lens_model, str, sizeof(dng_lens_model));
}

void dng_set_focal(int32_t nom, int32_t denom)
{
    cam_focal_length[0] = nom;
    cam_focal_length[1] = denom;
}

void dng_set_aperture(int32_t nom, int32_t denom)
{
    cam_aperture[0] = nom;
    cam_aperture[1] = denom;
}

void dng_set_shutter(int32_t nom, int32_t denom)
{
    cam_shutter[0] = nom;
    cam_shutter[1] = denom;
}

void dng_set_framerate(int32_t fpsx1000)
{
    cam_FrameRate[0] = fpsx1000;
    cam_FrameRate[1] = 1000;
}

void dng_set_framerate_rational(int32_t nom, int32_t denom)
{
    cam_FrameRate[0] = nom;
    cam_FrameRate[1] = denom;
}

void dng_set_iso(int32_t value)
{
    exif_data.iso = value;
}

void dng_set_wbgain(int32_t gain_r_n, int32_t gain_r_d, int32_t gain_g_n, int32_t gain_g_d, int32_t gain_b_n, int32_t gain_b_d)
{
    cam_AsShotNeutral[0] = gain_r_n;
    cam_AsShotNeutral[1] = gain_r_d;
    cam_AsShotNeutral[2] = gain_g_n;
    cam_AsShotNeutral[3] = gain_g_d;
    cam_AsShotNeutral[4] = gain_b_n;
    cam_AsShotNeutral[5] = gain_b_d;
}

void dng_set_datetime(char *datetime, char *subsectime)
{
    strncpy(cam_datetime, datetime, sizeof(cam_datetime));
    strncpy(cam_subsectime, subsectime, sizeof(cam_subsectime));
}

static int32_t is_lossless_jpeg(struct raw_info * raw_info)
{
    return *(uint32_t*)(raw_info->buffer) == 0xC4FFD8FF;
}


static void create_dng_header(struct raw_info * raw_info){
    int32_t i,j;
    int32_t extra_offset;
    int32_t raw_offset;

    int32_t dng_compression = 1;

    if (is_lossless_jpeg(raw_info))
    {
        dng_compression = 7; /* JPEG */
    }

    struct dir_entry ifd0[]={
        {0xFE,   T_LONG,       1,  1},                                 // NewSubFileType: Preview Image
        {0x100,  T_LONG,       1,  dng_th_width},                      // ImageWidth
        {0x101,  T_LONG,       1,  dng_th_height},                     // ImageLength
        {0x102,  T_SHORT,      3,  (intptr_t)cam_PreviewBitsPerSample},     // BitsPerSample: 8,8,8
        {0x103,  T_SHORT,      1,  1},                                 // Compression: Uncompressed
        {0x106,  T_SHORT,      1,  2},                                 // PhotometricInterpretation: RGB
        {0x10E,  T_ASCII|T_PTR,      sizeof(dng_image_desc), (intptr_t)dng_image_desc},               // ImageDescription
        {0x10F,  T_ASCII|T_PTR,      sizeof(CAM_MAKE), (intptr_t)CAM_MAKE},       // Make
        {0x110,  T_ASCII|T_PTR,      32, (intptr_t)cam_name},                     // Model: Filled at header generation.
        {0x111,  T_LONG,       1,  0},                                 // StripOffsets: Offset
        {0x112,  T_SHORT,      1,  1},                                 // Orientation: 1 - 0th row is top, 0th column is left
        {0x115,  T_SHORT,      1,  3},                                 // SamplesPerPixel: 3
        {0x116,  T_SHORT,      1,  dng_th_height},                     // RowsPerStrip
        {0x117,  T_LONG,       1,  dng_th_width*dng_th_height*3},      // StripByteCounts = preview size
        {0x11C,  T_SHORT,      1,  1},                                 // PlanarConfiguration: 1
        {0x131,  T_ASCII|T_PTR,32, (intptr_t)0},                                 // Software
        {0x132,  T_ASCII|T_PTR,      20, (intptr_t)cam_datetime},                 // DateTime
        {0x13B,  T_ASCII|T_PTR,64, (intptr_t)dng_artist_name},              // Artist: Filled at header generation.
        {0x14A,  T_LONG,       1,  0},                                 // SubIFDs offset
        {0x8298, T_ASCII|T_PTR,64, (intptr_t)dng_copyright},                // Copyright
        {0x8769, T_LONG,       1,  0},                                 // EXIF_IFD offset
        {0x9216, T_BYTE,       4,  0x00000001},                        // TIFF/EPStandardID: 1.0.0.0
#ifdef CONFIG_EOSM
        {0xC612, T_BYTE,       4,  0x00000401},                        // DNG 1.4 adds the user-crop tag used by EOS M.
#else
        {0xC612, T_BYTE,       4,  0x00000301},                        // DNGVersion: 1.3.0.0
#endif
        {0xC613, T_BYTE,       4,  0x00000301},                        // DNGBackwardVersion: 1.1.0.0
        {0xC614, T_ASCII|T_PTR,      32, (intptr_t)cam_name},                     // UniqueCameraModel. Filled at header generation.
        {0xC621, T_SRATIONAL,  9,  (intptr_t)&camera_sensor.color_matrix1},
        {0xC627, T_RATIONAL,   3,  (intptr_t)cam_AnalogBalance},
        {0xC628, T_RATIONAL,   3,  (intptr_t)cam_AsShotNeutral},
        {0xC62A, T_SRATIONAL,  1,  (intptr_t)&camera_sensor.exposure_bias},
        {0xC62B, T_RATIONAL,   1,  (intptr_t)cam_BaselineNoise},
        {0xC62C, T_RATIONAL,   1,  (intptr_t)cam_BaselineSharpness},
        {0xC62E, T_RATIONAL,   1,  (intptr_t)cam_LinearResponseLimit},
        {0xC65A, T_SHORT,      1, 21},                                 // CalibrationIlluminant1 D65
        // {0xC65B, T_SHORT,      1, 21},                                 // CalibrationIlluminant2 D65 (change this if ColorMatrix2 is added); see issue #2343
        {0xC764, T_SRATIONAL,  1,  (intptr_t)cam_FrameRate},
    };

    struct dir_entry ifd1[]={
        {0xFE,   T_LONG,       1,  0},                                 // NewSubFileType: Main Image
        {0x100,  T_LONG|T_PTR, 1,  (intptr_t)&camera_sensor.raw_rowpix},    // ImageWidth
        {0x101,  T_LONG|T_PTR, 1,  (intptr_t)&camera_sensor.raw_rows},      // ImageLength
        {0x102,  T_SHORT|T_PTR,1,  (intptr_t)&camera_sensor.bits_per_pixel},// BitsPerSample
        {0x103,  T_SHORT,      1,  dng_compression},                   // Compression: Uncompressed or JPEG
        {0x106,  T_SHORT,      1,  0x8023},                            // PhotometricInterpretation: CFA
        {0x111,  T_LONG,       1,  0},                                 // StripOffsets: Offset
        {0x115,  T_SHORT,      1,  1},                                 // SamplesPerPixel: 1
        {0x116,  T_SHORT|T_PTR,1,  (intptr_t)&camera_sensor.raw_rows},      // RowsPerStrip
        {0x117,  T_LONG|T_PTR, 1,  (intptr_t)&camera_sensor.raw_size},      // StripByteCounts = CHDK RAW size
        {0x11A,  T_RATIONAL,   1,  (intptr_t)cam_Resolution},               // XResolution
        {0x11B,  T_RATIONAL,   1,  (intptr_t)cam_Resolution},               // YResolution
        {0x11C,  T_SHORT,      1,  1},                                 // PlanarConfiguration: 1
        {0x128,  T_SHORT,      1,  2},                                 // ResolutionUnit: inch
        {0x828D, T_SHORT,      2,  0x00020002},                        // CFARepeatPatternDim: Rows = 2, Cols = 2
        {0x828E, T_BYTE|T_PTR, 4,  (intptr_t)&camera_sensor.cfa_pattern},
        {0xC61A, T_LONG|T_PTR, 1,  (intptr_t)&camera_sensor.black_level},   // BlackLevel
        {0xC61D, T_LONG|T_PTR, 1,  (intptr_t)&camera_sensor.white_level},   // WhiteLevel
        {0xC61F, T_RATIONAL,   2,  (intptr_t)dng_crop_origin},
        {0xC620, T_RATIONAL,   2,  (intptr_t)dng_crop_size},
        {0xC68D, T_LONG,       4,  (intptr_t)&camera_sensor.dng_active_area},
        {0xC740, T_UNDEFINED|T_PTR, sizeof(badpixel_opcode),  (intptr_t)&badpixel_opcode},
#ifdef CONFIG_EOSM
        {0xC7B5, T_RATIONAL,   4,  (intptr_t)dng_user_crop},
#endif
    };

    struct dir_entry exif_ifd[]={
        {0x829A, T_RATIONAL,   1,  (intptr_t)cam_shutter},          // Shutter speed
        {0x829D, T_RATIONAL,   1,  (intptr_t)cam_aperture},         // Aperture
        {0x8822, T_SHORT,      1,  0},                         // ExposureProgram
        {0x8827, T_SHORT|T_PTR,1,  (intptr_t)&exif_data.iso},       // ISOSpeedRatings
        {0x9000, T_UNDEFINED,  4,  0x31323230},                // ExifVersion: 2.21
        {0x9003, T_ASCII|T_PTR,      20, (intptr_t)cam_datetime},         // DateTimeOriginal
        //{0x9201, T_SRATIONAL,  1,  (intptr_t)cam_apex_shutter},     // ShutterSpeedValue (APEX units)
        //{0x9202, T_RATIONAL,   1,  (intptr_t)cam_apex_aperture},    // ApertureValue (APEX units)
        {0x9204, T_SRATIONAL,  1,  (intptr_t)cam_exp_bias},         // ExposureBias
        {0x9205, T_RATIONAL,   1,  (intptr_t)cam_max_av},           // MaxApertureValue
        {0x9207, T_SHORT,      1,  0},                         // Metering mode
        {0x9209, T_SHORT,      1,  0},                         // Flash mode
        {0x920A, T_RATIONAL,   1,  (intptr_t)cam_focal_length},     // FocalLength
        {0x9290, T_ASCII|T_PTR,4,  (intptr_t)cam_subsectime},       // DateTime milliseconds
        {0x9291, T_ASCII|T_PTR,4,  (intptr_t)cam_subsectime},       // DateTimeOriginal milliseconds
        {0xA405, T_SHORT|T_PTR,1,  (intptr_t)&exif_data.effective_focal_length},    // FocalLengthIn35mmFilm
        {0xA431, T_ASCII|T_PTR, sizeof(cam_serial), (intptr_t)cam_serial}, // BodySerialNumber belongs in ExifIFD.
        {0xA434, T_ASCII|T_PTR, sizeof(dng_lens_model), (intptr_t)dng_lens_model}, // LensModel belongs in ExifIFD.
    };

    struct
    {
        struct dir_entry* entry;
        int32_t count;                  // Number of entries to be saved
        int32_t entry_count;            // Total number of entries
    } ifd_list[] = 
    {
        {ifd0,      DIR_SIZE(ifd0),     DIR_SIZE(ifd0)}, 
        {ifd1,      DIR_SIZE(ifd1),     DIR_SIZE(ifd1)}, 
        {exif_ifd,  DIR_SIZE(exif_ifd), DIR_SIZE(exif_ifd)}, 
    };

#ifdef CONFIG_EOSM
    ifd0[DNG_VERSION_INDEX].offset = BE(0x01040000);
#else
    ifd0[DNG_VERSION_INDEX].offset = BE(0x01030000);
#endif
    
    ifd1[BADPIXEL_OPCODE_INDEX].type &= ~T_SKIP;
        // Set CFAPattern value
        switch (camera_sensor.cfa_pattern)
        {
        case 0x02010100:
            badpixel_opcode[BADPIX_CFA_INDEX] = BE(1);              // BayerPhase = 1 (top left pixel is green in a green/red row)
            break;
        case 0x01020001:
            badpixel_opcode[BADPIX_CFA_INDEX] = BE(0);              // BayerPhase = 0 (top left pixel is red)
            break;
        case 0x01000201:
            badpixel_opcode[BADPIX_CFA_INDEX] = BE(3);              // BayerPhase = 3 (top left pixel is blue)
            break;
        case 0x00010102:
            badpixel_opcode[BADPIX_CFA_INDEX] = BE(2);              // BayerPhase = 2 (top left pixel is green in a green/blue row)
            break;
        }

    // filling EXIF fields
    int32_t ifd_count = DIR_SIZE(ifd_list);

    // Fix the counts and offsets where needed
    ifd0[CAMERA_NAME_INDEX].count = ifd0[UNIQUE_CAMERA_MODEL_INDEX].count = strlen(cam_name) + 1;
    ifd0[CHDK_VER_INDEX].offset = (intptr_t)software_ver;
    ifd0[CHDK_VER_INDEX].count = strlen(software_ver) + 1;
    ifd0[ARTIST_NAME_INDEX].count = strlen(dng_artist_name) + 1;
    ifd0[COPYRIGHT_INDEX].count = strlen(dng_copyright) + 1;
    //~ ifd0[ORIENTATION_INDEX].offset = get_orientation_for_exif(exif_data.orientation);

    //~ exif_ifd[EXPOSURE_PROGRAM_INDEX].offset = get_exp_program_for_exif(exif_data.exp_program);
    //~ exif_ifd[METERING_MODE_INDEX].offset = get_metering_mode_for_exif(exif_data.metering_mode);
    //~ exif_ifd[FLASH_MODE_INDEX].offset = get_flash_mode_for_exif(exif_data.flash_mode, exif_data.flash_fired);
    //~ exif_ifd[SSTIME_INDEX].count = exif_ifd[SSTIME_ORIG_INDEX].count = strlen(cam_subsectime)+1;

    // calculating offset of RAW data and count of entries for each IFD
    raw_offset=TIFF_HDR_SIZE;

    for (j=0;j<ifd_count;j++)
    {
        raw_offset+=6; // IFD header+footer
        for(i=0; i<ifd_list[j].entry_count; i++)
        {
            if ((ifd_list[j].entry[i].type & T_SKIP) == 0)  // Exclude skipped entries (e.g. GPS info if camera doesn't have GPS)
            {
                raw_offset+=12; // IFD directory entry size
                int32_t size_ext=get_type_size(ifd_list[j].entry[i].type)*ifd_list[j].entry[i].count;
                if (size_ext>4) raw_offset+=size_ext+(size_ext&1);
            }
        }
    }

    // creating buffer for writing data
    raw_offset=(raw_offset/512+1)*512; // exlusively for CHDK fast file writing
    dng_header_buf_size=raw_offset;
    dng_header_buf=umalloc(raw_offset);
    dng_header_buf_offset=0;
    if (!dng_header_buf) return;

    // create buffer for thumbnail
    thumbnail_buf = malloc(dng_th_width*dng_th_height*3);
    if (!thumbnail_buf)
    {
        ufree(dng_header_buf);
        dng_header_buf = 0;
        return;
    }

    //  writing offsets for EXIF IFD and RAW data and calculating offset for extra data

    extra_offset=TIFF_HDR_SIZE;

    ifd0[SUBIFDS_INDEX].offset = TIFF_HDR_SIZE + ifd_list[0].count * 12 + 6;                            // SubIFDs offset
    ifd0[EXIF_IFD_INDEX].offset = TIFF_HDR_SIZE + (ifd_list[0].count + ifd_list[1].count) * 12 + 6 + 6; // EXIF IFD offset
    ifd0[THUMB_DATA_INDEX].offset = raw_offset;                                     //StripOffsets for thumbnail
    ifd1[RAW_DATA_INDEX].offset = raw_offset + dng_th_width * dng_th_height * 3;    //StripOffsets for main image

    for (j=0;j<ifd_count;j++)
    {
        extra_offset += 6 + ifd_list[j].count * 12; // IFD header+footer
    }

    // TIFF file header

    add_val_to_buf(0x4949, sizeof(int16_t));      // little endian
    add_val_to_buf(42, sizeof(int16_t));          // An arbitrary but carefully chosen number that further identifies the file as a TIFF file.
    add_val_to_buf(TIFF_HDR_SIZE, sizeof(int32_t)); // offset of first IFD

    // writing IFDs

    for (j=0;j<ifd_count;j++)
    {
        int32_t size_ext;
        add_val_to_buf(ifd_list[j].count, sizeof(int16_t));
        for(i=0; i<ifd_list[j].entry_count; i++)
        {
            if ((ifd_list[j].entry[i].type & T_SKIP) == 0)
            {
                add_val_to_buf(ifd_list[j].entry[i].tag, sizeof(int16_t));
                add_val_to_buf(ifd_list[j].entry[i].type & 0xFF, sizeof(int16_t));
                add_val_to_buf(ifd_list[j].entry[i].count, sizeof(int32_t));
                size_ext=get_type_size(ifd_list[j].entry[i].type)*ifd_list[j].entry[i].count;
                if (size_ext<=4) 
                {
                    if (ifd_list[j].entry[i].type & T_PTR)
                    {
                        add_to_buf((void*)ifd_list[j].entry[i].offset, sizeof(int32_t));
                    }
                    else
                    {
                        add_val_to_buf(ifd_list[j].entry[i].offset, sizeof(int32_t));
                    }
                }
                else
                {
                    add_val_to_buf(extra_offset, sizeof(int32_t));
                    extra_offset += size_ext+(size_ext&1);    
                }
            }
        }
        add_val_to_buf(0, sizeof(int32_t));
    }

    // writing extra data

    for (j=0;j<ifd_count;j++)
    {
        int32_t size_ext;
        for(i=0; i<ifd_list[j].entry_count; i++)
        {
            if ((ifd_list[j].entry[i].type & T_SKIP) == 0)
            {
                size_ext=get_type_size(ifd_list[j].entry[i].type)*ifd_list[j].entry[i].count;
                if (size_ext>4)
                {
                    add_to_buf((void*)ifd_list[j].entry[i].offset, size_ext);
                    if (size_ext&1) add_val_to_buf(0, 1);
                }
            }
        }
    }

    // writing zeros to tail of dng header (just for fun)
    for (i=dng_header_buf_offset; i<dng_header_buf_size; i++) dng_header_buf[i]=0;
}

static void free_dng_header(void)
{
    if (dng_header_buf)
    {
        ufree(dng_header_buf);
        dng_header_buf=NULL;
    }
    if (thumbnail_buf)
    {
        free(thumbnail_buf);
        thumbnail_buf = 0;
    }
}

//-------------------------------------------------------------------
// Functions for creating DNG thumbnail image

static inline int32_t raw_to_8bit(int32_t raw, int32_t wb, struct raw_info * raw_info)
{
    if (raw_info->bits_per_pixel == 16) /* big endian */
    {
        raw = ((raw & 0xFF00) >> 8) | ((raw & 0xFF) << 8);
    }
    int32_t black = raw_info->black_level;
    int32_t white = raw_info->white_level;
    float ev = log2f(MAX(1, raw - black)) + wb - 5;
    float max = log2f(white - black) - 5;
    int32_t out = ev * 255 / max;
    return COERCE(out, 0, 255);
}

static void create_thumbnail(struct raw_info * raw_info)
{
    register int32_t i, j, x, y, yadj, xadj;
    register char *buf = thumbnail_buf;
    
    if (is_lossless_jpeg(raw_info))
    {
        memset(thumbnail_buf, 0, dng_th_width*dng_th_height*3);
        return;
    }

    // The sensor bayer patterns are:
    //  0x02010100  0x01000201  0x01020001
    //      R G         G B         G R
    //      G B         R G         B G
    // for the second pattern yadj shifts the thumbnail row down one line
    // for the third pattern xadj shifts the thumbnail row accross one pixel
    // these make the patterns the same
    yadj = (camera_sensor.cfa_pattern == 0x01000201) ? 1 : 0;
    xadj = (camera_sensor.cfa_pattern == 0x01020001) ? 1 : 0;
    
    for (i=0; i<dng_th_height; i++)
        for (j=0; j<dng_th_width; j++)
        {
            x = camera_sensor.active_area.x1 + ((camera_sensor.jpeg.x + (camera_sensor.jpeg.width  * j) / dng_th_width)  & 0xFFFFFFFE) + xadj;
            y = camera_sensor.active_area.y1 + ((camera_sensor.jpeg.y + (camera_sensor.jpeg.height * i) / dng_th_height) & 0xFFFFFFFE) + yadj;

            *buf++ = raw_to_8bit(raw_get_pixel_from_buffer(raw_info->buffer, raw_info->pitch, x, y), 0, raw_info);        // red pixel
            *buf++ = raw_to_8bit(raw_get_pixel_from_buffer(raw_info->buffer, raw_info->pitch, x+1, y), -1, raw_info);      // green pixel
            *buf++ = raw_to_8bit(raw_get_pixel_from_buffer(raw_info->buffer, raw_info->pitch, x+1, y+1), 0, raw_info);    // blue pixel
        }
}

//-------------------------------------------------------------------
// Write DNG header, thumbnail and data to file

/* Write a centered 14-bit crop row without copying the complete sensor buffer. */
static int32_t write_cropped_dng_raw(FILE *fd, struct raw_info *source,
    int source_x, int source_y, int width, int height)
{
    if (source->bits_per_pixel != 14 || is_lossless_jpeg(source) || width <= 0 || height <= 0)
        return 0;

    int row_size = (width * 14 + 7) / 8;
    uint8_t *row = umalloc(row_size);
    if (!row) return 0;

    for (int y = 0; y < height; y++)
    {
        memset(row, 0, row_size);
        int x = 0;
        int byte = 0;
        /* Four 14-bit samples form seven bytes and keep each row independently aligned. */
        for (; x + 4 <= width; x += 4, byte += 7)
        {
            uint32_t a = raw_get_pixel_from_buffer(source->buffer, source->pitch, source_x + x, source_y + y);
            uint32_t b = raw_get_pixel_from_buffer(source->buffer, source->pitch, source_x + x + 1, source_y + y);
            uint32_t c = raw_get_pixel_from_buffer(source->buffer, source->pitch, source_x + x + 2, source_y + y);
            uint32_t d = raw_get_pixel_from_buffer(source->buffer, source->pitch, source_x + x + 3, source_y + y);
            row[byte + 0] = a >> 6;
            row[byte + 1] = (a << 2) | (b >> 12);
            row[byte + 2] = b >> 4;
            row[byte + 3] = (b << 4) | (c >> 10);
            row[byte + 4] = c >> 2;
            row[byte + 5] = (c << 6) | (d >> 8);
            row[byte + 6] = d;
        }

        /* TIFF aligns rows to bytes, so pad only this row's final partial sample group. */
        int remaining = width - x;
        if (remaining == 3)
        {
            uint32_t a = raw_get_pixel_from_buffer(source->buffer, source->pitch, source_x + x, source_y + y);
            uint32_t b = raw_get_pixel_from_buffer(source->buffer, source->pitch, source_x + x + 1, source_y + y);
            uint32_t c = raw_get_pixel_from_buffer(source->buffer, source->pitch, source_x + x + 2, source_y + y);
            row[byte + 0] = a >> 6;
            row[byte + 1] = (a << 2) | (b >> 12);
            row[byte + 2] = b >> 4;
            row[byte + 3] = (b << 4) | (c >> 10);
            row[byte + 4] = c >> 2;
            row[byte + 5] = c << 6;
        }
        else if (remaining == 2)
        {
            uint32_t a = raw_get_pixel_from_buffer(source->buffer, source->pitch, source_x + x, source_y + y);
            uint32_t b = raw_get_pixel_from_buffer(source->buffer, source->pitch, source_x + x + 1, source_y + y);
            row[byte + 0] = a >> 6;
            row[byte + 1] = (a << 2) | (b >> 12);
            row[byte + 2] = b >> 4;
            row[byte + 3] = b << 4;
        }
        else if (remaining == 1)
        {
            uint32_t a = raw_get_pixel_from_buffer(source->buffer, source->pitch, source_x + x, source_y + y);
            row[byte + 0] = a >> 6;
            row[byte + 1] = a << 2;
        }

        if (write(fd, row, row_size) != row_size)
        {
            ufree(row);
            return 0;
        }
    }

    ufree(row);
    return 1;
}

static int32_t write_dng(FILE* fd, struct raw_info * raw_info,
    struct raw_info *source_info, int source_x, int source_y)
{
    create_dng_header(raw_info);
    if (!dng_header_buf) return 0;
    char* rawadr = (void*)raw_info->buffer;

    struct raw_info thumbnail_info = *source_info;
    thumbnail_info.jpeg.x = source_x - source_info->active_area.x1;
    thumbnail_info.jpeg.y = source_y - source_info->active_area.y1;
    thumbnail_info.jpeg.width = raw_info->width;
    thumbnail_info.jpeg.height = raw_info->height;
    create_thumbnail(&thumbnail_info);
    if (write(fd, dng_header_buf, dng_header_buf_size) != dng_header_buf_size)
    {
        free_dng_header();
        return 0;
    }
    if (write(fd, thumbnail_buf, dng_th_width*dng_th_height*3) != dng_th_width*dng_th_height*3)
    {
        free_dng_header();
        return 0;
    }

    if (source_info != raw_info)
    {
        if (!write_cropped_dng_raw(fd, source_info, source_x, source_y,
                raw_info->width, raw_info->height))
        {
            free_dng_header();
            return 0;
        }
    }
    else if (!is_lossless_jpeg(raw_info))
    {
        /* Serialize swapped RAW data through a small buffer; never mutate the camera's live frame. */
        const int32_t chunk_capacity = 64 * 1024;
        char *chunk = umalloc(chunk_capacity);
        if (!chunk)
        {
            free_dng_header();
            return 0;
        }

        int32_t offset = 0;
        while (offset < camera_sensor.raw_size)
        {
            int32_t count = MIN(chunk_capacity, camera_sensor.raw_size - offset);
            memcpy(chunk, UNCACHEABLE(rawadr + offset), count);
            reverse_bytes_order(chunk, count & ~1);
            if (write(fd, chunk, count) != count)
            {
                ufree(chunk);
                free_dng_header();
                return 0;
            }
            offset += count;
        }
        ufree(chunk);
    }
    else if (write(fd, UNCACHEABLE(rawadr), camera_sensor.raw_size) != camera_sensor.raw_size)
    {
        free_dng_header();
        return 0;
    }

    free_dng_header();
    return 1;
}

#ifdef CONFIG_MAGICLANTERN
PROP_HANDLER(PROP_CAM_MODEL)
{
    snprintf(cam_name, sizeof(cam_name), (const char *)buf);
}
#endif

#ifdef CONFIG_EOSM
extern int xpan_photo_get_65_24_rect(int width, int height, int *x, int *y, int *crop_width, int *crop_height);

static uint16_t cr2_u16(const uint8_t *p, int little_endian)
{
    return little_endian ? (uint16_t)p[0] | ((uint16_t)p[1] << 8) :
        ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t cr2_u32(const uint8_t *p, int little_endian)
{
    return little_endian ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24) :
        ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | p[3];
}

static void cr2_put_u32(uint8_t *p, uint32_t value, int little_endian)
{
    if (little_endian)
    {
        p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
    }
    else
    {
        p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
    }
}

static int cr2_read_at(FILE *file, uint32_t file_size, uint32_t offset, void *buffer, uint32_t size)
{
    if (offset > file_size || size > file_size - offset) return 0;
    if (FIO_SeekSkipFile(file, offset, SEEK_SET) != offset) return 0;
    return FIO_ReadFile(file, buffer, size) == (int)size;
}

/* Read one TIFF IFD entry without trusting offsets or entry counts from the file. */
static int cr2_find_tag(FILE *file, uint32_t file_size, uint32_t ifd_offset,
    uint16_t wanted_tag, int little_endian, uint16_t *type, uint32_t *count, uint32_t *value)
{
    uint8_t bytes[2], entry[12];
    if (!cr2_read_at(file, file_size, ifd_offset, bytes, sizeof(bytes))) return 0;
    uint16_t entries = cr2_u16(bytes, little_endian);
    if (entries > 512 || ifd_offset > file_size - 2 ||
        (uint32_t)entries > (file_size - ifd_offset - 2) / 12) return 0;

    for (uint16_t i = 0; i < entries; i++)
    {
        uint32_t pos = ifd_offset + 2 + (uint32_t)i * 12;
        if (!cr2_read_at(file, file_size, pos, entry, sizeof(entry))) return 0;
        if (cr2_u16(entry, little_endian) == wanted_tag)
        {
            *type = cr2_u16(entry + 2, little_endian);
            *count = cr2_u32(entry + 4, little_endian);
            *value = cr2_u32(entry + 8, little_endian);
            return 1;
        }
    }
    return 0;
}

int xpan_photo_patch_cr2_file(const char *filename)
{
    uint32_t file_size = 0, ifd0, exif_ifd, maker_ifd, maker_size, aspect_offset;
    uint16_t type;
    uint32_t count, value;
    uint8_t header[16], aspect[20], updated[20], verify[20];
    int little_endian;
    FILE *file;
    int ok = 0;

    /* Native 3:2 preserves Canon's full-frame CR2 crop metadata unchanged. */
    if (!xpan_photo_is_panoramic()) return 1;
    if (!filename || FIO_GetFileSize(filename, &file_size) != 0 || file_size < sizeof(header)) return 0;
    file = FIO_OpenFile(filename, O_RDWR | O_SYNC);
    if (!file) return 0;

    /* Canon CR2 starts with a TIFF header followed by its CR2 marker. */
    if (!cr2_read_at(file, file_size, 0, header, sizeof(header))) goto done;
    little_endian = header[0] == 'I' && header[1] == 'I';
    if ((!little_endian && !(header[0] == 'M' && header[1] == 'M')) ||
        cr2_u16(header + 2, little_endian) != 42 || header[8] != 'C' ||
        header[9] != 'R' || header[10] != 2) goto done;

    ifd0 = cr2_u32(header + 4, little_endian);
    if (!cr2_find_tag(file, file_size, ifd0, 0x8769, little_endian, &type, &count, &exif_ifd) ||
        type != 4 || count != 1) goto done;
    if (!cr2_find_tag(file, file_size, exif_ifd, 0x927c, little_endian, &type, &count, &maker_ifd) ||
        type != 7 || count < 16) goto done;
    maker_size = count;
    if (!cr2_find_tag(file, file_size, maker_ifd, 0x009a, little_endian, &type, &count, &aspect_offset) ||
        type != 4 || count != 5) goto done;
    /* Keep the crop write inside Canon's MakerNote payload, away from adjacent CR2 data. */
    if (aspect_offset < maker_ifd || aspect_offset - maker_ifd > maker_size ||
        sizeof(aspect) > maker_size - (aspect_offset - maker_ifd)) goto done;
    if (!cr2_read_at(file, file_size, aspect_offset, aspect, sizeof(aspect))) goto done;

    /* XPan mode forces Canon's full-frame 3:2 source aspect. */
    uint32_t old_ratio = cr2_u32(aspect, little_endian);
    uint32_t old_width = cr2_u32(aspect + 4, little_endian);
    uint32_t old_height = cr2_u32(aspect + 8, little_endian);
    uint32_t old_left = cr2_u32(aspect + 12, little_endian);
    uint32_t old_top = cr2_u32(aspect + 16, little_endian);
    /* Accept only the exact full-resolution Canon crops seen in untouched EOS M samples. */
    int known_3_2 = old_ratio == 0 && old_width == 5184 && old_height == 3456 && old_left == 0 && old_top == 0;
    int known_16_9 = old_ratio == 7 && old_width == 5184 && old_height == 2916 && old_left == 0 && old_top == 270;
    int known_4_3 = old_ratio == 2 && old_width == 4608 && old_height == 3456 && old_left == 288 && old_top == 0;
    int known_1_1 = old_ratio == 1 && old_width == 3456 && old_height == 3456 && old_left == 864 && old_top == 0;
    /* Accept earlier XPan records so existing test files can be moved to the active-area crop. */
    int known_xpan = old_ratio == 7 &&
        ((old_width == 5135 && old_height == 1896 && old_left == 24 &&
            (old_top == 0 || old_top == 780 || old_top == 1560)) ||
         (old_width == 5070 && old_height == 1872 && old_left == 92 && old_top == 818) ||
         (old_width == 5184 && old_height == 1914 && old_left == 0 && old_top == 771) ||
         (old_width == 5184 && old_height == 1920 && old_left == 0 && old_top == 768));
#ifdef CONFIG_XPAN
    /* Also recognize a raster-relative exact1914 proof record at top0. RAW
     * pixels still use IFD0's full 5184x3456 canvas, checked below. This refers
     * to AspectInfo, not Canon's separate CropInfo.CropTopMargin field; Test20
     * and Test21 actually store the correct AspectInfo top771 already. */
    known_xpan |= old_ratio==7 && old_width==5184 &&
        (old_height==1914 || old_height==1920) && old_left==0 && old_top==0;
#endif
    /* Preserve an unknown Canon crop instead of replacing a merely plausible record. */
    if (!known_3_2 && !known_16_9 && !known_4_3 && !known_1_1 && !known_xpan) goto done;

    /* TIFF ImageWidth/ImageLength must be SHORT or LONG before their inline value is decoded. */
    if (!cr2_find_tag(file, file_size, ifd0, 0x0100, little_endian, &type, &count, &value) ||
        (type != 3 && type != 4) || count != 1) goto done;
    uint32_t width = type == 3 ? (little_endian ? value & 0xFFFF : value >> 16) : value;
    if (!cr2_find_tag(file, file_size, ifd0, 0x0101, little_endian, &type, &count, &value) ||
        (type != 3 && type != 4) || count != 1) goto done;
    uint32_t height = type == 3 ? (little_endian ? value & 0xFFFF : value >> 16) : value;
    /* The recognized MakerNote rectangles above are specific to EOS M full-resolution CR2 files. */
    if (width != 5184 || height != 3456) goto done;

    /* CR2 IFD0 is already the trimmed 5184x3456 image, not the larger RAW DMA
     * buffer. Subtracting RAW margins again incorrectly discarded side pixels. */
    int crop_x, crop_y, crop_width, crop_height;
    if (!xpan_photo_get_65_24_rect(width, height,
            &crop_x, &crop_y, &crop_width, &crop_height)) goto done;
    uint32_t left = crop_x;
    uint32_t top = crop_y;

    cr2_put_u32(updated, 7, little_endian); /* Canon has no 65:24 code; dimensions carry the pixel-rounded crop. */
    cr2_put_u32(updated + 4, crop_width, little_endian);
    cr2_put_u32(updated + 8, crop_height, little_endian);
    cr2_put_u32(updated + 12, left, little_endian);
    cr2_put_u32(updated + 16, top, little_endian);
    if (memcmp(aspect, updated, sizeof(aspect)) == 0) { ok = 1; goto done; }

    /* Replace only the existing 20-byte crop record; CR2 pixel data stays untouched. */
    if (FIO_SeekSkipFile(file, aspect_offset, SEEK_SET) != aspect_offset) goto done;
    int write_ok = FIO_WriteFile(file, updated, sizeof(updated)) == (int)sizeof(updated);
    int verified = write_ok && cr2_read_at(file, file_size, aspect_offset, verify, sizeof(verify)) &&
        memcmp(verify, updated, sizeof(updated)) == 0;
    if (!verified)
    {
        /* Restore the original crop record if the short write or read-back check failed. */
        int restored = FIO_SeekSkipFile(file, aspect_offset, SEEK_SET) == aspect_offset &&
            FIO_WriteFile(file, aspect, sizeof(aspect)) == (int)sizeof(aspect);
        restored = restored && cr2_read_at(file, file_size, aspect_offset, verify, sizeof(verify)) &&
            memcmp(verify, aspect, sizeof(aspect)) == 0;
        if (!restored) printf("XPan warning: CR2 crop-record restore could not be verified\n");
        goto done;
    }
    ok = 1;

done:
    FIO_CloseFile(file);
    return ok;
}

#ifdef CONFIG_XPAN
/* Read the actual baseline JPEG dimensions, independently of Canon's stale Exif
 * dimensions after private-descriptor cropping. Seek past segments; do not load
 * the image or entropy stream. Unknown formats remain on the existing path. */
static int xpan_jpeg_raster_size(FILE *file, uint32_t file_size, uint32_t *width, uint32_t *height)
{
    uint8_t bytes[5];
    uint32_t pos=2;
    for (unsigned i=0; i<128 && pos<=file_size-4; i++)
    {
        if (!cr2_read_at(file,file_size,pos,bytes,4) || bytes[0]!=0xff) return 0;
        if (bytes[1]==0xda || bytes[1]==0xd9) return 0;
        if (bytes[1]==1 || (bytes[1]>=0xd0 && bytes[1]<=0xd7)) { pos+=2; continue; }
        uint32_t length=((uint32_t)bytes[2]<<8)|bytes[3];
        if (length<2 || length>file_size-pos-2) return 0;
        if (bytes[1]==0xc0)
        {
            if (length<8 || !cr2_read_at(file,file_size,pos+4,bytes,5) || bytes[0]!=8) return 0;
            *height=((uint32_t)bytes[1]<<8)|bytes[2];
            *width=((uint32_t)bytes[3]<<8)|bytes[4];
            return 1;
        }
        pos+=length+2;
    }
    return 0;
}

/* Native crop proofs leave Canon's original photo descriptor unchanged. Match
 * Exif height and crop coordinates to the JPEG's real raster, so readers cannot
 * apply the full-sensor top=771 a second time to already cropped pixels. */
static int xpan_patch_native_jpeg_metadata(FILE *file, uint32_t file_size,
    uint32_t exif_start, uint32_t aspect_absolute, const uint8_t *aspect,
    uint32_t ifd_width, uint32_t ifd_height, uint32_t raster_height, int little_endian)
{
    uint32_t ratio=cr2_u32(aspect,little_endian),width=cr2_u32(aspect+4,little_endian);
    uint32_t height=cr2_u32(aspect+8,little_endian),left=cr2_u32(aspect+12,little_endian);
    uint32_t top=cr2_u32(aspect+16,little_endian);
    /* The visible XPan frame is the shared photo rectangle (5184x1920). */
    int rect_x,rect_y,rect_width,rect_height;
    if (!xpan_photo_get_65_24_rect(5184,3456,&rect_x,&rect_y,&rect_width,&rect_height) ||
        (uint32_t)rect_height>raster_height) return 0;
    uint32_t visible=rect_height,crop_top=(raster_height-visible)/2;
    int full_record=ifd_width==5184 && ifd_height==3456 && left==0 &&
        ((ratio==0 && width==5184 && height==3456 && top==0) ||
         (ratio==7 && width==5184 && height==1914 && (top==771 || top==0)) ||
         (ratio==7 && width==5184 && height==visible && (top==(uint32_t)rect_y || top==0)));
    int raster_record=ifd_width==5184 && ifd_height==raster_height && ratio==7 &&
        width==5184 && left==0 && ((height==visible && top==crop_top) ||
                                 (height==raster_height && top==0));
    if (!full_record && !raster_record) return 0;

    /* Locate the inline ExifImageHeight value; preserve its SHORT/LONG type and
     * any padding. No IFD is rebuilt and no MakerNote offsets are relocated. */
    uint8_t bytes[2],entry[12],old_height[4],new_height[4],updated[20],verify[20];
    if (!cr2_read_at(file,file_size,exif_start,bytes,2)) return 0;
    unsigned entries=cr2_u16(bytes,little_endian);
    if (exif_start>file_size-2 || entries>512 || entries>(file_size-exif_start-2)/12) return 0;
    uint32_t height_offset=0;
    uint16_t height_type=0;
    for (unsigned i=0; i<entries; i++)
    {
        uint32_t pos=exif_start+2+i*12;
        if (!cr2_read_at(file,file_size,pos,entry,12)) return 0;
        if (cr2_u16(entry,little_endian)!=0xa003) continue;
        height_type=cr2_u16(entry+2,little_endian);
        if ((height_type!=3 && height_type!=4) || cr2_u32(entry+4,little_endian)!=1) return 0;
        height_offset=pos+8; memcpy(old_height,entry+8,4); break;
    }
    if (!height_offset) return 0;
    memcpy(new_height,old_height,4);
    if (height_type==4) cr2_put_u32(new_height,raster_height,little_endian);
    else if (little_endian) { new_height[0]=raster_height; new_height[1]=raster_height>>8; }
    else { new_height[0]=raster_height>>8; new_height[1]=raster_height; }
    cr2_put_u32(updated,7,little_endian);
    cr2_put_u32(updated+4,5184,little_endian);
    cr2_put_u32(updated+8,visible,little_endian);
    cr2_put_u32(updated+12,0,little_endian);
    cr2_put_u32(updated+16,crop_top,little_endian);
    if (!memcmp(new_height,old_height,4) && !memcmp(aspect,updated,20)) return 1;

    /* Treat the two tiny metadata writes as one checked update. Restore both
     * original fields on any short write or read-back mismatch. JPEG compressed
     * pixels/SOF and all CR2 metadata are untouched by this helper. */
    int ok=FIO_SeekSkipFile(file,height_offset,SEEK_SET)==height_offset &&
        FIO_WriteFile(file,new_height,4)==4 &&
        FIO_SeekSkipFile(file,aspect_absolute,SEEK_SET)==aspect_absolute &&
        FIO_WriteFile(file,updated,20)==20 &&
        cr2_read_at(file,file_size,height_offset,verify,4) && !memcmp(verify,new_height,4) &&
        cr2_read_at(file,file_size,aspect_absolute,verify,20) && !memcmp(verify,updated,20);
    if (!ok)
    {
        int restored_height=FIO_SeekSkipFile(file,height_offset,SEEK_SET)==height_offset &&
            FIO_WriteFile(file,old_height,4)==4 &&
            cr2_read_at(file,file_size,height_offset,verify,4) && !memcmp(verify,old_height,4);
        int restored_aspect=FIO_SeekSkipFile(file,aspect_absolute,SEEK_SET)==aspect_absolute &&
            FIO_WriteFile(file,aspect,20)==20 &&
            cr2_read_at(file,file_size,aspect_absolute,verify,20) && !memcmp(verify,aspect,20);
        if (!restored_height || !restored_aspect)
            printf("XPan warning: native JPEG metadata restore could not be verified\n");
    }
    return ok;
}
#endif

/* Set the same Canon crop record in an EOS M JPEG without recompressing its pixels. */
int xpan_photo_patch_jpeg_file(const char *filename)
{
    uint32_t file_size = 0, tiff_base = 0, ifd0, exif_ifd, maker_ifd, maker_start;
    uint32_t maker_size, aspect_offset, aspect_absolute, count, value;
    uint16_t type;
    uint8_t marker[4], prefix[6], header[16], aspect[20], updated[20], verify[20];
    int little_endian = 0;
    FILE *file;
    int ok = 0;

    /* Native 3:2 preserves Canon's full-resolution JPEG and original metadata. */
    if (!xpan_photo_is_panoramic()) return 1;
    if (!filename || FIO_GetFileSize(filename, &file_size) != 0 || file_size < 4) return 0;
    file = FIO_OpenFile(filename, O_RDWR | O_SYNC);
    if (!file) return 0;

    /* Find the existing Exif APP1 segment; leave XMP APP1 segments and image data alone. */
    if (!cr2_read_at(file, file_size, 0, marker, 2) || marker[0] != 0xFF || marker[1] != 0xD8)
        goto done;
    uint32_t pos = 2;
    while (pos <= file_size - 4)
    {
        if (!cr2_read_at(file, file_size, pos, marker, sizeof(marker)) || marker[0] != 0xFF) goto done;
        if (marker[1] == 0xD9 || marker[1] == 0xDA) break;

        /* Restart markers and TEM carry no segment length. */
        if (marker[1] == 0x01 || (marker[1] >= 0xD0 && marker[1] <= 0xD7))
        {
            pos += 2;
            continue;
        }

        uint32_t segment_length = ((uint32_t)marker[2] << 8) | marker[3];
        if (segment_length < 2 || segment_length > file_size - pos - 2) goto done;
        uint32_t payload = pos + 4;
        if (marker[1] == 0xE1 && segment_length >= 8 &&
            cr2_read_at(file, file_size, payload, prefix, sizeof(prefix)) &&
            memcmp(prefix, "Exif\0\0", sizeof(prefix)) == 0)
        {
            tiff_base = payload + sizeof(prefix);
            break;
        }
        pos += 2 + segment_length;
    }
    if (!tiff_base || tiff_base > file_size - sizeof(header) ||
        !cr2_read_at(file, file_size, tiff_base, header, sizeof(header))) goto done;

    little_endian = header[0] == 'I' && header[1] == 'I';
    if ((!little_endian && !(header[0] == 'M' && header[1] == 'M')) ||
        cr2_u16(header + 2, little_endian) != 42) goto done;
    ifd0 = cr2_u32(header + 4, little_endian);
    if (ifd0 > file_size - tiff_base - 2 ||
        !cr2_find_tag(file, file_size, tiff_base + ifd0, 0x8769, little_endian,
            &type, &count, &exif_ifd) || type != 4 || count != 1 ||
        exif_ifd > file_size - tiff_base - 2) goto done;
    uint32_t exif_start = tiff_base + exif_ifd;
    if (!cr2_find_tag(file, file_size, exif_start, 0x927c, little_endian,
            &type, &count, &maker_start) || type != 7 || count < 16 ||
        maker_start > file_size - tiff_base) goto done;
    maker_size = count;
    maker_ifd = tiff_base + maker_start;
    if (maker_ifd > file_size - 2 ||
        !cr2_find_tag(file, file_size, maker_ifd, 0x009a, little_endian,
            &type, &count, &aspect_offset) || type != 4 || count != 5 ||
        aspect_offset < maker_start || aspect_offset - maker_start > maker_size ||
        sizeof(aspect) > maker_size - (aspect_offset - maker_start) ||
        aspect_offset > file_size - tiff_base - sizeof(aspect)) goto done;
    aspect_absolute = tiff_base + aspect_offset;
    if (!cr2_read_at(file, file_size, aspect_absolute, aspect, sizeof(aspect))) goto done;

    /* Read the existing Canon crop record before validating it against the JPEG raster. */
    uint32_t old_ratio = cr2_u32(aspect, little_endian);
    uint32_t old_width = cr2_u32(aspect + 4, little_endian);
    uint32_t old_height = cr2_u32(aspect + 8, little_endian);
    uint32_t old_left = cr2_u32(aspect + 12, little_endian);
    uint32_t old_top = cr2_u32(aspect + 16, little_endian);

    /* JPEG pixels may already be inside one of Canon's native aspect crops. */
    uint32_t ifd_width, ifd_height;
    if (!cr2_find_tag(file, file_size, exif_start, 0xa002, little_endian,
            &type, &count, &value) || (type != 3 && type != 4) || count != 1) goto done;
    ifd_width = type == 3 ? (little_endian ? value & 0xFFFF : value >> 16) : value;
    if (!cr2_find_tag(file, file_size, exif_start, 0xa003, little_endian,
            &type, &count, &value) || (type != 3 && type != 4) || count != 1) goto done;
    ifd_height = type == 3 ? (little_endian ? value & 0xFFFF : value >> 16) : value;

#ifdef CONFIG_XPAN
    uint32_t raster_width=0,raster_height=0;
    if (xpan_jpeg_raster_size(file,file_size,&raster_width,&raster_height) && raster_width==5184 &&
        (raster_height==1914 || raster_height==1920 || raster_height==2912))
    {
        /* These heights are the explicit native proof modes. The normal
         * full-size JPEG metadata path and RAW crop code stay unchanged. */
        ok=xpan_patch_native_jpeg_metadata(file,file_size,exif_start,aspect_absolute,aspect,
            ifd_width,ifd_height,raster_height,little_endian);
        goto done;
    }
#endif

    int crop_x, crop_y, crop_width, crop_height;
    /* Accept the four full-resolution native JPEG rasters verified from EOS M samples. */
    int known_native =
        (ifd_width == 5184 && ifd_height == 3456 && old_ratio == 0 &&
            old_width == 5184 && old_height == 3456 && old_left == 0 && old_top == 0) ||
        (ifd_width == 5184 && ifd_height == 2912 && old_ratio == 7 &&
            old_width == 5184 && old_height == 2912 && old_left == 0 && old_top == 0) ||
        (ifd_width == 4608 && ifd_height == 3456 && old_ratio == 2 &&
            old_width == 4608 && old_height == 3456 && old_left == 0 && old_top == 0) ||
        (ifd_width == 3456 && ifd_height == 3456 && old_ratio == 1 &&
            old_width == 3456 && old_height == 3456 && old_left == 0 && old_top == 0);
    if (!known_native && old_ratio != 7) goto done;
    if (!xpan_photo_get_65_24_rect(ifd_width, ifd_height,
            &crop_x, &crop_y, &crop_width, &crop_height)) goto done;
    /* Also migrate the narrower metadata crops produced by earlier XPan builds. */
    uint32_t legacy_units = MIN(ifd_width / 65, ifd_height / 24);
    uint32_t legacy_width = legacy_units * 65, legacy_height = legacy_units * 24;
    int known_xpan = old_ratio == 7 &&
        ((old_width == (uint32_t)crop_width && old_height == (uint32_t)crop_height &&
          old_left == (uint32_t)crop_x && old_top == (uint32_t)crop_y) ||
         (old_width == legacy_width && old_height == legacy_height &&
          old_left == ((ifd_width - legacy_width) / 2 & ~1u) &&
          old_top == ((ifd_height - legacy_height) / 2 & ~1u)));
    if (!known_native && !known_xpan) goto done;

    cr2_put_u32(updated, 7, little_endian); /* Closest Canon aspect code; dimensions carry the pixel-rounded XPan crop. */
    cr2_put_u32(updated + 4, crop_width, little_endian);
    cr2_put_u32(updated + 8, crop_height, little_endian);
    cr2_put_u32(updated + 12, crop_x, little_endian);
    cr2_put_u32(updated + 16, crop_y, little_endian);
    if (memcmp(aspect, updated, sizeof(aspect)) == 0) { ok = 1; goto done; }

    /* Update only Canon's existing 20-byte crop record; the JPEG bitstream remains untouched. */
    if (FIO_SeekSkipFile(file, aspect_absolute, SEEK_SET) != aspect_absolute) goto done;
    int write_ok = FIO_WriteFile(file, updated, sizeof(updated)) == (int)sizeof(updated);
    int verified = write_ok && cr2_read_at(file, file_size, aspect_absolute, verify, sizeof(verify)) &&
        memcmp(verify, updated, sizeof(updated)) == 0;
    if (!verified)
    {
        /* Restore Canon's original record if the short write or read-back check fails. */
        int restored = FIO_SeekSkipFile(file, aspect_absolute, SEEK_SET) == aspect_absolute &&
            FIO_WriteFile(file, aspect, sizeof(aspect)) == (int)sizeof(aspect);
        restored = restored && cr2_read_at(file, file_size, aspect_absolute, verify, sizeof(verify)) &&
            memcmp(verify, aspect, sizeof(aspect)) == 0;
        if (!restored) printf("XPan warning: JPEG crop-record restore could not be verified\n");
        goto done;
    }
    ok = 1;

done:
    FIO_CloseFile(file);
    return ok;
}
#endif

/* Shared writer; XPan sidecars request physical pixel trimming explicitly. */
static int save_dng_internal(char* filename, struct raw_info * raw_info, int physical_xpan_crop)
{
#ifdef CONFIG_EOSM
    if (!dng_writer_sem || take_semaphore(dng_writer_sem, 0)) return 0;
#endif
    int32_t previous_thumbnail_width = dng_th_width;
    int32_t previous_thumbnail_height = dng_th_height;

    #ifdef RAW_DEBUG_BLACK
    raw_info->active_area.x1 = 0;
    raw_info->active_area.x2 = raw_info->width;
    raw_info->active_area.y1 = 0;
    raw_info->active_area.y2 = raw_info->height;
    raw_info->jpeg.x = 0;
    raw_info->jpeg.y = 0;
    raw_info->jpeg.width = raw_info->width;
    raw_info->jpeg.height = raw_info->height;
    #endif

    struct raw_info output = *raw_info;
    /* Default to the integer crop supplied by the capture path for all cameras. */
    dng_crop_origin[0] = output.jpeg.x;
    dng_crop_origin[1] = 1;
    dng_crop_origin[2] = output.jpeg.y;
    dng_crop_origin[3] = 1;
    dng_crop_size[0] = output.jpeg.width;
    dng_crop_size[1] = 1;
    dng_crop_size[2] = output.jpeg.height;
    dng_crop_size[3] = 1;
    /* Physical trimming (EOS M XPan sidecars only) writes a sub-rectangle of source. */
    struct raw_info source = output;
    int source_x = 0;
    int source_y = 0;
    int physical_crop = 0;
#ifdef CONFIG_EOSM
    /* A full-range user crop preserves the rectangle specified by the default crop tags. */
    dng_user_crop[0] = 0; dng_user_crop[1] = 1;
    dng_user_crop[2] = 0; dng_user_crop[3] = 1;
    dng_user_crop[4] = 1; dng_user_crop[5] = 1;
    dng_user_crop[6] = 1; dng_user_crop[7] = 1;

    int crop_x, crop_y, crop_width, crop_height;
    /* Center the XPan crop within the full RAW active area, excluding optical-black margins. */
    if (xpan_photo_get_65_24_rect(output.jpeg.width, output.jpeg.height,
            &crop_x, &crop_y, &crop_width, &crop_height))
    {
        if (physical_xpan_crop)
        {
            /* The XPan sidecar stores only its Bayer-aligned sensor rectangle. */
            source_x = source.active_area.x1 + crop_x;
            source_y = source.active_area.y1 + crop_y;
            output.width = crop_width;
            output.height = crop_height;
            output.pitch = (crop_width * output.bits_per_pixel + 7) / 8;
            output.frame_size = output.pitch * crop_height;
            output.active_area.x1 = 0;
            output.active_area.y1 = 0;
            output.active_area.x2 = crop_width;
            output.active_area.y2 = crop_height;
            output.jpeg.x = 0;
            output.jpeg.y = 0;
            output.jpeg.width = crop_width;
            output.jpeg.height = crop_height;
            physical_crop = 1;

            /* The physical DNG raster is already framed, so its default crop is full size. */
            dng_crop_origin[0] = 0;
            dng_crop_origin[1] = 1;
            dng_crop_origin[2] = 0;
            dng_crop_origin[3] = 1;
            dng_crop_size[0] = crop_width;
            dng_crop_size[1] = 1;
            dng_crop_size[2] = crop_height;
            dng_crop_size[3] = 1;
            output.dng_active_area[0] = 0;
            output.dng_active_area[1] = 0;
            output.dng_active_area[2] = crop_height;
            output.dng_active_area[3] = crop_width;
        }
        else
        {
            /* Keep legacy DNG calls non-destructive and represent the crop with metadata. */
            dng_crop_origin[0] += crop_x;
            dng_crop_origin[2] = output.jpeg.y + crop_y;
            dng_crop_size[0] = crop_width;
            dng_crop_size[2] = crop_width * 24;
            dng_crop_size[3] = 65;
            output.jpeg.x += crop_x;
            output.jpeg.y += crop_y;
            output.jpeg.width = crop_width;
            output.jpeg.height = crop_height;
        }

        /* Give file browsers an embedded preview with the same XPan ratio as the DNG crop. */
        dng_th_width = 195;
        dng_th_height = 72;
    }
#endif
    
    FILE* f = FIO_CreateFile(filename);
    if (!f)
    {
        dng_th_width = previous_thumbnail_width;
        dng_th_height = previous_thumbnail_height;
#ifdef CONFIG_EOSM
        give_semaphore(dng_writer_sem);
#endif
        return 0;
    }
    int32_t ok = write_dng(f, &output, physical_crop ? &source : &output,
        physical_crop ? source_x : 0, physical_crop ? source_y : 0);
    FIO_CloseFile(f);
    dng_th_width = previous_thumbnail_width;
    dng_th_height = previous_thumbnail_height;
    if (!ok)
    {
        FIO_RemoveFile(filename);
#ifdef CONFIG_EOSM
        give_semaphore(dng_writer_sem);
#endif
        return 0;
    }
#ifdef CONFIG_EOSM
    give_semaphore(dng_writer_sem);
#endif
    return 1;
}

/* returns 1 on success, 0 on error */
int save_dng(char* filename, struct raw_info *raw_info)
{
    return save_dng_internal(filename, raw_info, 0);
}

#ifdef CONFIG_EOSM
/* The capture worker uses this only for its optional XPan RAW sidecar. */
int save_dng_xpan(char *filename, struct raw_info *raw_info)
{
    return save_dng_internal(filename, raw_info, 1);
}
#endif
