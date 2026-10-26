// included in zebra.c
// always use cropmarkS in plural

#include "cropmarks.h"
#include "xpan-mode.h"

static void bvram_mirror_clear();
static void clrscr_mirror();

static void cropmark_cache_update_signature();
static int cropmark_cache_is_valid();
static void default_movie_cropmarks();
static void black_bars();

void cropmark_clear_cache();

static struct bmp_file_t * cropmarks = 0;

static CONFIG_INT( "crop.enable", crop_enabled, 0);  // index of crop file
static CONFIG_INT( "crop.index", crop_index, 0); // index of crop file
static CONFIG_INT( "crop.movieonly", cropmark_movieonly, 0);
static CONFIG_INT("crop.playback", cropmarks_play, 0);
#ifdef CONFIG_EOSM
#if !defined(CONFIG_QEMU)
/* Keep diagnostic camera menus honest while the hardware capture hook is disabled. */
#define XPAN_CAMERA_RAW_PROCESSING_DISABLED
#endif

/* One persistent mode switch replaces the optional guide/crop switches. */
static CONFIG_INT("xpan.enabled", xpan_enabled, 1);
/* The green Auto position chooses exposure itself: XPan is off there and
 * cannot be enabled. The stored setting is kept for the photo position. */
static int xpan_green_auto(void) { return shooting_mode==SHOOTMODE_AUTO || shooting_mode==SHOOTMODE_AP; }
#define XPAN_ACTIVE (xpan_enabled && !xpan_green_auto())
static CONFIG_INT("xpan.align", xpan_frame_align, 0);
/* Movie mode has its own XPan ON/OFF (the Movie LUT); photo uses xpan_enabled. */
static CONFIG_INT("xpan.movie", xpan_movie_on, 1);
int xpan_movie_enabled(void) { return xpan_movie_on; }
/* Keep the accepted panoramic behavior as the default for existing config files. */
static CONFIG_INT("xpan.aspect", xpan_aspect, XPAN_ASPECT_65_24);
/* Enabled XPan always uses the centered pixel crop and its integrated HUD. */
static CONFIG_INT("xpan.theme", xpan_photo_theme, 0);
/* XPan > Grid lines: 0 OFF, 1 rule of thirds (drawn by the HUD renderer). */
static CONFIG_INT("xpan.grid", xpan_grid_mode, 0);
int xpan_photo_grid_mode(void) { return xpan_grid_mode == 1 ? 1 : 0; }
static CONFIG_INT("xpan.status.time", xpan_photo_show_time, 1);
static CONFIG_INT("xpan.status.space", xpan_photo_show_space, 1);
/* The user can hide the XPan bitmap HUD without changing the saved framing. */
static CONFIG_INT("xpan.hud", xpan_photo_hud_enabled, 1);
/* XPan > Debug info: on-screen key/touch diagnostics only (no file logging). */
static CONFIG_INT("xpan.debug", xpan_photo_debug, 0);
int xpan_photo_debug_enabled(void) { return xpan_photo_debug; }
/* DNG sidecars are disabled: on-camera conversion stalls capture for minutes. */
/* XPan metadata cropping follows the mode switch unconditionally. */
/* Manual-lens values feed the XPan equivalent readout only; they never change exposure. */
static CONFIG_INT("xpan.manual.focal", xpan_manual_focal_mm, 0);
static CONFIG_INT("xpan.manual.aperture", xpan_manual_aperture_x10, 0);
/* Keep the last capture worker result available to the menu, not just the console. */
static volatile enum xpan_dng_copy_status xpan_dng_copy_status = XPAN_DNG_COPY_READY;

/* Declared here because ratio updates must restore an entry hidden by 3:2. */
static struct menu_entry xpan_photo_menu[];
static void xpan_alignment_hide(int hide);

void xpan_photo_set_dng_copy_status(enum xpan_dng_copy_status status)
{
    xpan_dng_copy_status = status;
}

int xpan_photo_enabled(void) { return XPAN_ACTIVE; }
int xpan_photo_aspect_ratio(void)
{
    return xpan_aspect == XPAN_ASPECT_3_2 ? XPAN_ASPECT_3_2 : XPAN_ASPECT_65_24;
}
int xpan_photo_is_panoramic(void) { return xpan_photo_aspect_ratio() == XPAN_ASPECT_65_24; }

int xpan_photo_ui_active(void)
{
    return XPAN_ACTIVE && !is_movie_mode() && lv && !PLAY_MODE;
}

unsigned xpan_photo_wheel_state(void)
{
    /* EOS M2.0.2's live OLC display-state cache. Confirm the ROM getters before
     * reading fixed RAM; these addresses are never written by XPan. Canon's
     * FF6AB6E4 focus-frame renderer and FF53A158 dial handler use state3. */
    static int recognized=-1;
    if (recognized<0) recognized=
        MEM(0xff388988)==0x517a0 && MEM(0xff38963c)==0xe5d00042 &&
        MEM(0xff389684)==0xe5d00048 && MEM(0xff3896c0)==0xe5d0004d &&
        MEM(0xff389768)==0xe5d0005b && MEM(0xff6ab718)==0x03560003;
    if (!recognized || !xpan_photo_ui_active() || PLAY_OR_QR_MODE || gui_menu_shown()) return 0;
    const volatile uint8_t *state=(const volatile uint8_t *)0x517a0;
    return state[0x42] | (state[0x48]<<8) | (state[0x4d]<<16) | ((unsigned)state[0x5b]<<24);
}

static void xpan_photo_capture_format_select(void *priv, int sign);
/* HUD wheel control: same rules as the menu item (XPan ON: JPG <-> RAW+JPG). */
void xpan_photo_capture_format_step(int sign) { xpan_photo_capture_format_select(NULL, sign); }

/* INFO in XPan Live View: 3:2 = layout on/off, 65:24 = layout top/bottom. */
void xpan_photo_info_toggle(void)
{
    if (xpan_photo_is_panoramic()) xpan_frame_align = !xpan_frame_align;
    else xpan_photo_hud_enabled = !xpan_photo_hud_enabled;
    redraw();
}

unsigned xpan_photo_wheel_targets(void)
{
    unsigned state=xpan_photo_wheel_state(),targets=0;
    if ((state&255)==3) targets|=XPAN_WHEEL_SHUTTER;
    if (((state>>8)&255)==3 && lens_info.lens_exists) targets|=XPAN_WHEEL_APERTURE;
    if (((state>>16)&255)==3) targets|=XPAN_WHEEL_ISO;
    if ((state>>24)==3) targets|=XPAN_WHEEL_COMPENSATION;
    /* Unknown/ambiguous native state must not invent an aperture selection. */
    return targets && !(targets&(targets-1)) ? targets:0;
}

/* Keep Canon's source aspect at full-frame 3:2 while the XPan photo UI is active. */
void xpan_photo_keep_3_2_aspect(void)
{
    static uint32_t last_request_ms;
    if (!xpan_photo_ui_active()) return;
    if (xpan_image_aspect_ratio_valid == 1 && xpan_image_aspect_ratio == 0) return;

    uint32_t now = get_ms_clock();
    if (now - last_request_ms < 1000) return;
    last_request_ms = now;

    uint32_t aspect_3_2 = 0; /* EOS M's full-frame code, verified from the supplied CR2 MakerNotes. */
    prop_request_change(PROP_IMAGE_ASPECT_RATIO, &aspect_3_2, sizeof(aspect_3_2));
}

int xpan_photo_preview_crop_enabled(void)
{
    /* Do not let a stale saved setting silently turn the photo frame into a guide-only overlay. */
    /* Stays on while magnified so the LCD frame buffers are never freed and
     * re-allocated (that left 1x unshifted after a zoom). The display filter
     * itself passes Canon's zoom frame through untouched (tweaks.c). */
    return xpan_photo_ui_active() && xpan_photo_is_panoramic();
}

int xpan_photo_status_time_enabled(void)
{
    return xpan_photo_show_time;
}

int xpan_photo_status_space_enabled(void)
{
    return xpan_photo_show_space;
}

int xpan_photo_hud_is_enabled(void)
{
    return xpan_photo_hud_enabled;
}

int xpan_photo_dng_copy_enabled(void)
{
    /* Ignore legacy xpan.dng.copy=1 settings as well as new capture requests. */
    return 0;
}

int xpan_photo_cr2_crop_enabled(void)
{
    return XPAN_ACTIVE && xpan_photo_is_panoramic();
}

/* Use configured values only as fallbacks when the lens provides no electronic data. */
unsigned xpan_photo_manual_focal_mm(void)
{
    return xpan_manual_focal_mm;
}

unsigned xpan_photo_manual_aperture_x10(void)
{
    return xpan_manual_aperture_x10;
}

static int xpan_picq_has_raw(void)
{
    return (pic_quality & 0x60000) == 0x60000;
}

static int xpan_picq_has_jpeg(void)
{
    return (pic_quality >> 24) != 4;
}

/* Canon stores S1/S2/S3 JPEG sizes as codes 0x0e/0x0f/0x10 in PICQ's size byte. */
#define XPAN_PICQ_JPEG_FINE(size)   (0x03010000 | ((size) << 8))
#define XPAN_PICQ_JPEG_NORMAL(size) (0x02010000 | ((size) << 8))
static const int xpan_jpeg_quality_values[] = {
    PICQ_LARGE_FINE, PICQ_MED_FINE, PICQ_SMALL_FINE,
    XPAN_PICQ_JPEG_FINE(0x0e), XPAN_PICQ_JPEG_FINE(0x0f), XPAN_PICQ_JPEG_FINE(0x10),
    PICQ_LARGE_COARSE, PICQ_MED_COARSE, PICQ_SMALL_COARSE,
    XPAN_PICQ_JPEG_NORMAL(0x0e), XPAN_PICQ_JPEG_NORMAL(0x0f), XPAN_PICQ_JPEG_NORMAL(0x10)
};
static const char *xpan_jpeg_quality_names[] = {
    "Large Fine", "Medium Fine", "Small Fine", "S1 Fine", "S2 Fine", "S3 Fine",
    "Large Normal", "Medium Normal", "Small Normal", "S1 Normal", "S2 Normal", "S3 Normal"
};

/* Ignore unknown Canon codes instead of interpreting any nonzero value as JPEG. */
static int xpan_picq_is_known(void)
{
    if (pic_quality == PICQ_RAW || pic_quality == PICQ_RAW_JPG_LARGE_FINE) return 1;
    for (unsigned i = 0; i < COUNT(xpan_jpeg_quality_values); i++)
        if ((uint32_t)pic_quality == (uint32_t)xpan_jpeg_quality_values[i]) return 1;
    return 0;
}

/* Return Canon's expected capture types, or -1 until a supported property value arrives. */
int xpan_photo_capture_has_raw(void)
{
    if (xpan_pic_quality_valid != 1 || !xpan_picq_is_known()) return -1;
    return xpan_picq_has_raw();
}

int xpan_photo_capture_has_jpeg(void)
{
    if (xpan_pic_quality_valid != 1 || !xpan_picq_is_known()) return -1;
    return xpan_picq_has_jpeg();
}

static int xpan_pic_quality_change_failed;
static int xpan_pic_quality_request_pending;
static int xpan_pic_quality_requested;
static uint32_t xpan_pic_quality_request_ms;
/* Send all EOS M PICQ aliases, matching the established Magic Lantern setter. */
static void xpan_photo_set_pic_quality(int quality)
{
    static const unsigned properties[] = { PROP_PIC_QUALITY, PROP_PIC_QUALITY2, PROP_PIC_QUALITY3 };
    for (unsigned i = 0; i < COUNT(properties); i++)
        prop_request_change(properties[i], &quality, sizeof(quality));
    xpan_pic_quality_requested = quality;
    xpan_pic_quality_request_ms = get_ms_clock();
    xpan_pic_quality_request_pending = 1;
    xpan_pic_quality_change_failed = 0;
}

/* Give Canon time to publish a property reply before showing a failed-change warning. */
static void xpan_photo_check_pic_quality_request(void)
{
    if (!xpan_pic_quality_request_pending) return;
    if ((int)pic_quality == xpan_pic_quality_requested)
    {
        xpan_pic_quality_request_pending = 0;
        xpan_pic_quality_change_failed = 0;
    }
    else if (get_ms_clock() - xpan_pic_quality_request_ms > 1200)
    {
        xpan_pic_quality_request_pending = 0;
        xpan_pic_quality_change_failed = 1;
    }
}

int xpan_photo_ensure_capture_quality(void)
{
    if (!XPAN_ACTIVE) return 1;
    if (is_movie_mode() || lens_info.job_state ||
        get_halfshutter_pressed() || xpan_pic_quality_valid!=1 || !xpan_picq_is_known()) return 0;
    xpan_photo_check_pic_quality_request();
    if (xpan_pic_quality_request_pending) return 0;
    /* RAW-only stores a full3:2, ungraded embedded preview through a separate
     * Canon path. XPan RAW capture therefore uses RAW+JPEG so both file and
     * camera preview follow the verified crop/look path. Also normalize legacy
     * RAW-only settings or a RAW-only choice made in Canon's menu. */
    int target=xpan_picq_has_raw() ? PICQ_RAW_JPG_LARGE_FINE :
        ((pic_quality>>24)==2 ? PICQ_LARGE_COARSE:PICQ_LARGE_FINE);
    if ((int)pic_quality==target) return 1;
    xpan_photo_set_pic_quality(target);
    return 0; /* Only Canon's subsequent readback confirms the new format. */
}

/* Use Canon's native EOS M quality codes; RAW+JPEG is Large Fine on this model. */
static void xpan_photo_capture_format_select(void *priv, int sign)
{
    (void)priv;
    if (is_movie_mode()) return;
#ifdef CONFIG_EOSM
    /* Wait for Canon's first property event; zero-initialized state is not a format. */
    if (xpan_pic_quality_valid != 1 || !xpan_picq_is_known()) return;
#endif
    if (XPAN_ACTIVE)
    {
        /* Two supported XPan formats: either direction toggles the pair. */
        xpan_photo_set_pic_quality(xpan_picq_has_raw() ? PICQ_LARGE_FINE:PICQ_RAW_JPG_LARGE_FINE);
        return;
    }
    int format = xpan_picq_has_raw() ? (xpan_picq_has_jpeg() ? 2 : 0) : 1;
    int next_format = (format + sign + 3) % 3;
    int quality = next_format == 0 ? PICQ_RAW :
        next_format == 1 ? PICQ_LARGE_FINE : PICQ_RAW_JPG_LARGE_FINE;
    xpan_photo_set_pic_quality(quality);
}

static MENU_UPDATE_FUNC(xpan_photo_capture_format_display)
{
    xpan_photo_check_pic_quality_request();
#ifdef CONFIG_EOSM
    if (xpan_pic_quality_valid != 1)
    {
        MENU_SET_VALUE("Waiting for Canon");
        MENU_SET_ENABLED(0);
        return;
    }
    if (!xpan_picq_is_known())
    {
        MENU_SET_VALUE("Unsupported Canon code");
        MENU_SET_ENABLED(0);
        return;
    }
#endif
    if (is_movie_mode())
    {
        MENU_SET_VALUE("Photo mode only");
        MENU_SET_ENABLED(0);
        return;
    }
    MENU_SET_ENABLED(1);
    if (xpan_pic_quality_change_failed)
    {
        /* Show the rejected value and Canon's readback so alias/property mismatches are visible. */
        MENU_SET_WARNING(MENU_WARN_ADVICE,
            "PICQ %08x -> P1 %d:%08x P2 %d:%08x P3 %d:%08x",
            (unsigned)xpan_pic_quality_requested,
            xpan_pic_quality1_valid == 1, (unsigned)xpan_pic_quality1,
            xpan_pic_quality2_valid == 1, (unsigned)xpan_pic_quality2,
            xpan_pic_quality3_valid == 1, (unsigned)xpan_pic_quality3);
    }
    if (xpan_picq_has_raw() && xpan_picq_has_jpeg())
        MENU_SET_VALUE("RAW + JPEG");
    else if (xpan_picq_has_raw())
    {
        MENU_SET_VALUE("%s",XPAN_ACTIVE ? "Switching to RAW+JPEG":"RAW");
        if (XPAN_ACTIVE)
            MENU_SET_WARNING(MENU_WARN_INFO,"XPan uses RAW+JPEG for the cropped camera preview.");
    }
    else if (xpan_picq_has_jpeg())
        MENU_SET_VALUE("JPEG");
    else
        MENU_SET_VALUE("Canon setting");
}

static void xpan_photo_jpeg_quality_select(void *priv, int sign)
{
    (void)priv;
#ifdef CONFIG_EOSM
    if (xpan_pic_quality_valid != 1 || !xpan_picq_is_known()) return;
#endif
    if (is_movie_mode() || xpan_picq_has_raw() || !xpan_picq_has_jpeg()) return;

    /* XPan keeps all5184 columns. Smaller native JPEG sizes are unsupported. */
    int fine = pic_quality != PICQ_LARGE_COARSE;
    xpan_photo_set_pic_quality(fine ? PICQ_LARGE_COARSE : PICQ_LARGE_FINE);
}

static MENU_UPDATE_FUNC(xpan_photo_jpeg_quality_display)
{
    xpan_photo_check_pic_quality_request();
#ifdef CONFIG_EOSM
    if (xpan_pic_quality_valid != 1)
    {
        MENU_SET_VALUE("Waiting for Canon");
        MENU_SET_ENABLED(0);
        return;
    }
    if (!xpan_picq_is_known())
    {
        MENU_SET_VALUE("Unsupported Canon code");
        MENU_SET_ENABLED(0);
        return;
    }
#endif
    if (is_movie_mode())
    {
        MENU_SET_VALUE("Photo mode only");
        MENU_SET_ENABLED(0);
        return;
    }
    if (xpan_picq_has_raw() && xpan_picq_has_jpeg())
    {
        MENU_SET_VALUE("Large Fine (RAW+JPEG)");
        MENU_SET_ENABLED(0);
        return;
    }
    if (xpan_picq_has_raw() || !xpan_picq_has_jpeg())
    {
        MENU_SET_VALUE("Select JPEG first");
        MENU_SET_ENABLED(0);
        return;
    }

    MENU_SET_ENABLED(1);
    if (xpan_pic_quality_change_failed)
        MENU_SET_WARNING(MENU_WARN_ADVICE, "Canon did not confirm the JPEG quality change.");
    for (int i = 0; i < COUNT(xpan_jpeg_quality_values); i++)
        if (pic_quality == xpan_jpeg_quality_values[i])
        {
            MENU_SET_VALUE("%s", xpan_jpeg_quality_names[i]);
            return;
        }
    MENU_SET_VALUE("Other Canon JPEG");
}

int xpan_photo_theme_color(void)
{
    switch (xpan_photo_theme)
    {
        case 1: return COLOR_GRAY(78); /* silver body */
        case 2: return COLOR_RED;      /* red body */
        case 3: return COLOR_CYAN;     /* blue: palette colour closest to #387dbc */
        default: return COLOR_ORANGE;  /* Orange theme */
    }
}

/* Compute shared 65:24 geometry; each caller decides whether it is for display or saved metadata. */
int xpan_photo_get_65_24_rect(int width, int height, int *x, int *y, int *crop_width, int *crop_height)
{
    if (is_movie_mode() || width < 2 || height < 2 || !x || !y || !crop_width || !crop_height)
        return 0;

    /* Canon's CR2/JPEG dimensions already exclude sensor margins. Preserve every
     * column and use the nearest even 27:10 height: 5184x1920 at top768 for the
     * full EOS M image. 1920 matches the JPEG engine's processing grid exactly;
     * the older 1914-row (65:24) request repeated three rows at the half seam. */
    int h = ((width * 10 + 27) / 54) * 2;
    if (h < 2 || h > height) return 0;
    *x = 0;
    *y = (height - h) / 2;
    *crop_width = width;
    *crop_height = h;
    return 1;
}

/* Preserve the entire preview width; round 65:24 height to the nearest even row count.
 * LCD YUV has no Bayer phase constraint: choose the exact middle source rows. */
int xpan_photo_get_preview_rect(int width, int height, int *x, int *y, int *crop_width, int *crop_height)
{
    if (is_movie_mode() || width < 2 || height < 2 || !x || !y || !crop_width || !crop_height)
        return 0;
    if (!xpan_photo_is_panoramic())
    {
        /* Native 3:2 keeps every LCD row and leaves Canon's image buffer in place. */
        *x = 0; *y = 0; *crop_width = width; *crop_height = height;
        return 1;
    }
    int h = ((width * 24 + 65) / 130) * 2;
    if (h < 2 || h > height) return 0;
    *x = 0;
    *y = (height - h) / 2;
    *crop_width = width;
    *crop_height = h;
    return 1;
}

/* Return the on-screen composition rectangle for HUD layout; this never crops Live View pixels. */
int xpan_photo_get_liveview_frame_rect(int *x, int *y, int *crop_width, int *crop_height)
{
    /* Still-photo Live View is 3:2; the movie-only 16:9 bars are not part of this frame. */
    int visible_height = os.y_ex;
    /* Guide visibility is handled only by the overlay renderer; saved-image cropping has its own path. */
    if (!xpan_photo_get_preview_rect(os.x_ex, visible_height, x, y, crop_width, crop_height))
        return 0;

    /* Alignment applies only to the short panoramic frame, never native 3:2. */
    if (!xpan_photo_is_panoramic()) { *y = 0; return 1; }
    *y = xpan_frame_align == 0 ? 0 : ((visible_height - *crop_height) & ~1);
    return 1;
}

/* Align the pixel transform against the LCD buffer itself, not the bitmap overlay's active area. */
int xpan_photo_get_preview_frame_y(int width, int height)
{
    int crop_x, crop_y, crop_width, crop_height;
    if (!xpan_photo_get_preview_rect(width, height,
            &crop_x, &crop_y, &crop_width, &crop_height))
        return -1;
    if (!xpan_photo_is_panoramic()) return 0;
    return xpan_frame_align == 0 ? 0 : ((height - crop_height) & ~1);
}

/* Anchor the complete panel beside the aligned photograph. */
int xpan_photo_get_hud_y(void)
{
    /* Keep one HUD anchor in both ratios; in 3:2 the panel overlays the image. */
    int panoramic_height = ((720 * 24 + 65) / 130) * 2;
    return xpan_frame_align ? 0 : panoramic_height;
}

int xpan_photo_get_monitor_rect(int *x, int *y, int *width, int *height)
{
    if (!x || !y || !width || !height || !xpan_photo_ui_active()) return 0;
    int frame_x, frame_y, frame_w, frame_h;
    if (!xpan_photo_get_preview_rect(720, 480, &frame_x, &frame_y, &frame_w, &frame_h)) return 0;
    if (!xpan_photo_is_panoramic())
    {
        /* Keep monitoring tools off the compact 3:2 header and footer. */
        *x = 8; *y = 64; *width = 704; *height = 320;
        return 1;
    }
    frame_y = xpan_photo_get_preview_frame_y(720, 480);
    if (frame_y < 0) return 0;
    /* Monitoring is intentionally over the image, with a12-pixel edge inset. */
    *x = 0;
    *y = frame_y + 12;
    *width = 720;
    *height = frame_h - 24;
    return *height > 0;
}

/* SET toggles the mode; Q (PLAY on EOS M) opens the same submenu. */
/* Movie mode keeps only the LUT rows of the XPan submenu visible. */
#ifdef CONFIG_XPAN
void xpan_native_look_menu_sanitize(void);   /* xpan-workflow.inc */
#endif
void xpan_menu_mode_visibility(void)
{
    static const char *const movie_rows[]={"Custom LUT","Cube file","Cube strength","Tone base"};
    /* Stored values from older builds must be valid before any row is drawn. */
    if (xpan_photo_theme<0 || xpan_photo_theme>3) xpan_photo_theme=0;
    if (xpan_grid_mode<0 || xpan_grid_mode>1) xpan_grid_mode=1;   /* old Square/3rds -> 3rds */
#ifdef CONFIG_XPAN
    xpan_native_look_menu_sanitize();
#endif
    static unsigned long long hidden_by_us;
    int movie=is_movie_mode(),i=0;
    for (struct menu_entry *e=xpan_photo_menu[0].children; e && i<64; e=e->next, i++) {
        int keep=0;
        for (unsigned k=0;k<COUNT(movie_rows);k++) if (e->name && streq(e->name,movie_rows[k])) keep=1;
        if (movie && !keep) { if (!e->shidden) { e->shidden=1; hidden_by_us|=1ULL<<i; } }
        else if (hidden_by_us&(1ULL<<i)) { e->shidden=0; hidden_by_us&=~(1ULL<<i); }
    }
    /* Frame alignment: 65:24 photo only. */
    if (!movie) xpan_alignment_hide(!xpan_photo_is_panoramic());
}

static MENU_SELECT_FUNC(xpan_mode_toggle)
{
    if (lens_info.job_state || get_halfshutter_pressed()) return;
    if (is_movie_mode()) {
        /* Movie mode: XPan = the Movie LUT on the Live View / H.264 image. */
        xpan_movie_on=!xpan_movie_on;
#ifdef CONFIG_XPAN
        xpan_native_look_mode_changed();
#endif
        redraw();
        return;
    }
    if (xpan_green_auto()) return;   /* not available there */
    xpan_enabled=!xpan_enabled;
#ifdef CONFIG_XPAN
    xpan_native_look_mode_changed(); /* Cancel any obsolete background LUT build. */
#endif
    crop_set_dirty(10);
    redraw();
}
static MENU_UPDATE_FUNC(xpan_mode_display)
{
    xpan_menu_mode_visibility();
    if (is_movie_mode()) {
        /* Movie mode: only the LUT rows apply; the XPan photo UI is off. */
        MENU_SET_VALUE("%s",xpan_movie_on ? "ON":"OFF");
        MENU_SET_ENABLED(xpan_movie_on);
        MENU_SET_HELP("Movie mode: SET toggles the XPan Movie LUT. PLAY opens Custom LUT / Cube / Tone base.");
        return;
    }
    MENU_SET_VALUE("%s",XPAN_ACTIVE ? "ON":"OFF");
    MENU_SET_ENABLED(XPAN_ACTIVE);
    if (xpan_green_auto())
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Not available in the green Auto position.");
#ifdef CONFIG_XPAN
    const char *status=xpan_native_look_status();
    MENU_SET_HELP("SET toggles XPan. PLAY opens options. %s",status);
#endif
}

/* Ratio changes are accepted only at an idle photo boundary. */
static MENU_SELECT_FUNC(xpan_photo_aspect_select)
{
    (void)priv; (void)delta;
    if (lens_info.job_state || get_halfshutter_pressed() || PLAY_OR_QR_MODE || is_movie_mode()) return;
    xpan_aspect = xpan_photo_aspect_ratio() == XPAN_ASPECT_65_24 ? XPAN_ASPECT_3_2 : XPAN_ASPECT_65_24;
    /* Hidden menu rows are not updated while absent; restore this row at the switch. */
    xpan_alignment_hide(!xpan_photo_is_panoramic());
    crop_set_dirty(10);
    redraw();
}
static MENU_UPDATE_FUNC(xpan_photo_aspect_display)
{
    /* Keep Frame alignment visibility synchronized even when it was hidden. */
    xpan_alignment_hide(!xpan_photo_is_panoramic() || is_movie_mode());
    MENU_SET_VALUE("%s", xpan_photo_is_panoramic() ? "65:24" : "3:2");
    MENU_SET_ENABLED(!lens_info.job_state && !get_halfshutter_pressed() && !is_movie_mode() && !PLAY_OR_QR_MODE);
    MENU_SET_HELP("Switch XPan framing between the centered 65:24 crop and full native 3:2.");
}
/* Manual lens values are only used for an unchipped lens. */
/* Frame alignment row, found by name (the submenu order is not fixed). */
static struct menu_entry *xpan_alignment_entry(void)
{
    for (struct menu_entry *e=xpan_photo_menu[0].children; e; e=e->next)
        if (e->name && streq(e->name,"Frame alignment") && !MENU_IS_PLACEHOLDER(e)) return e;
    return NULL;
}
static void xpan_alignment_hide(int hide)
{
    struct menu_entry *e=xpan_alignment_entry();
    if (e) e->shidden=hide;
}
/* Color theme: Orange / Silver / Red / Blue. */
static MENU_UPDATE_FUNC(xpan_photo_theme_display)
{
    if (xpan_photo_theme<0 || xpan_photo_theme>3) xpan_photo_theme=0;
}
static MENU_UPDATE_FUNC(xpan_photo_manual_lens_display)
{
    if (lens_info.lens_exists)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Chipped lens attached: its own values are used.");
}
static MENU_UPDATE_FUNC(xpan_photo_alignment_display)
{
    MENU_SET_SHIDDEN(!xpan_photo_is_panoramic() || is_movie_mode());
    MENU_SET_VALUE("%s", xpan_frame_align ? "Bottom" : "Top");
}

static struct menu_entry xpan_photo_menu[] = {
    {
        .name = "XPan",
        .priv = &xpan_enabled, .max = 1, .update = xpan_mode_display,
        .help = "Still-photo framing for the Hasselblad XPan 65:24 image area.",
        /* SET is the switch; Q/PLAY is the dedicated submenu action. */
        .select = xpan_mode_toggle,
        .select_Q = menu_open_submenu,
        .submenu_width = 710,
        .submenu_height = 450,
        .children = (struct menu_entry[]) {
            /* Order: capture, look (Cube), film grain, display, lens, debug.
             * Look and grain rows are registered later (xpan-workflow.inc,
             * xpan-film-grain.c); the placeholders fix their position here. */
            /* --- Capture --- */
            {
                .name = "Aspect ratio",
                .priv = &xpan_aspect,
                .max = 1,
                .choices = (const char *[]) {"65:24", "3:2"},
                .select = xpan_photo_aspect_select,
                .update = xpan_photo_aspect_display,
                .help = "Choose 65:24 panoramic framing or full native 3:2 framing.",
            },
            {
                .name = "Frame alignment",
                .priv = &xpan_frame_align,
                .max = 1,
                .choices = (const char *[]) {"Top", "Bottom"},
                .update = xpan_photo_alignment_display,
                .help = "Place the frame against the top or bottom edge, leaving the opposite area free for monitoring tools.",
            },
            {
                .name = "Capture format",
                .priv = &pic_quality,
                .select = xpan_photo_capture_format_select,
                .update = xpan_photo_capture_format_display,
                .help = "XPan: JPEG or RAW+JPEG (Large Fine). RAW+JPEG supplies the cropped camera preview.",
            },
            {
                .name = "JPEG size/quality",
                .priv = &pic_quality,
                .select = xpan_photo_jpeg_quality_select,
                .update = xpan_photo_jpeg_quality_display,
                .help = "Full-width XPan JPEG: Large Fine or Large Normal. RAW+JPEG uses Large Fine.",
            },
            /* --- Look (Cube LUT) --- */
            MENU_PLACEHOLDER("Custom LUT"),
            MENU_PLACEHOLDER("Cube file"),
            MENU_PLACEHOLDER("Refresh Cube list"),
            MENU_PLACEHOLDER("Cube strength"),
            MENU_PLACEHOLDER("Tone base"),
            MENU_PLACEHOLDER("Highlight headroom"),
            MENU_PLACEHOLDER("Live LUT preview"),

            /* --- Film grain --- */
            MENU_PLACEHOLDER("Film grain"),
            MENU_PLACEHOLDER("Grain type"),
            MENU_PLACEHOLDER("Grain strength"),
            MENU_PLACEHOLDER("Grain size"),
            MENU_PLACEHOLDER("Watermark"),
            MENU_PLACEHOLDER("Watermark size"),
            /* --- Display --- */
            {
                .name = "XPan overlay",
                .priv = &xpan_photo_hud_enabled,
                .max = 1,
                .choices = (const char *[]) {"OFF", "ON"},
                .help = "Hide or show the XPan bitmap overlay; framing and capture stay active.",
            },
            {
                .name = "Grid lines",
                .priv = &xpan_grid_mode,
                .max = 1,
                .choices = (const char *[]) {"OFF", "3rds Grid"},
                .help = "Rule-of-thirds lines inside the frame, in 65:24 and 3:2. Hidden while magnified.",
            },
            {
                .name = "Color theme",
                .priv = &xpan_photo_theme,
                .max = 3,
                .choices = (const char *[]) {"Orange", "Silver", "Red", "Blue"},
                .update = xpan_photo_theme_display,
                .help = "Accent color of the XPan photo HUD.",
            },
            {
                .name = "Show time",
                .priv = &xpan_photo_show_time,
                .max = 1,
                .choices = (const char *[]) {"OFF", "ON"},
                .help = "Show the camera clock in the XPan photo HUD.",
            },
            {
                .name = "Show card space",
                .priv = &xpan_photo_show_space,
                .max = 1,
                .choices = (const char *[]) {"OFF", "ON"},
                .help = "Show the remaining card space in the XPan photo HUD.",
            },
            /* --- Unchipped lens --- */
            {
                .name = "Manual focal (mm)",
                .priv = &xpan_manual_focal_mm,
                .min = 0,
                .max = 1000,
                .update = xpan_photo_manual_lens_display,
                .help = "Fallback focal length for an unchipped lens; 0 disables it. Readout only; does not affect exposure.",
            },
            {
                .name = "Manual aperture",
                .priv = &xpan_manual_aperture_x10,
                .min = 0,
                .max = 320,
                .unit = UNIT_x10,
                .update = xpan_photo_manual_lens_display,
                .help = "Fallback f-number for an unchipped lens; enter the value set on its aperture ring. Readout only.",
            },
            /* --- Development --- */
            {
                .name = "Debug info",
                .priv = &xpan_photo_debug,
                .max = 1,
                .choices = (const char *[]) {"OFF", "ON"},
                .help = "Development only: key and touch diagnostics on the XPan HUD. Keep OFF.",
            },
            MENU_EOL
        },
    },
};
#endif

static int cropmark_cache_dirty = 1;
static int crop_dirty = 0; // redraw cropmarks after some time (unit: 0.1s)

static int cropmarks_x = -1;
static int cropmarks_y = -1;

void crop_set_dirty(int value)
{
    crop_dirty = MAX(crop_dirty, value);
}

static int is_valid_cropmark_filename(char* filename)
{
    int n = strlen(filename);
    if ((n > 4) && (streq(filename + n - 4, ".BMP") || streq(filename + n - 4, ".bmp")) && (filename[0] != '.') && (filename[0] != '_'))
        return 1;
    return 0;
}

#define MAX_CROP_NAME_LEN 15
#define MAX_CROPMARKS 9
static int num_cropmarks = 0;
static int cropmarks_initialized = 0;
static char cropmark_names[MAX_CROPMARKS][MAX_CROP_NAME_LEN];

// Cropmark sorting code contributed by Nathan Rosenquist
static void sort_cropmarks()
{
    int i = 0;
    int j = 0;
    
    char aux[MAX_CROP_NAME_LEN];
    
    for (i=0; i<num_cropmarks; i++)
    {
        for (j=i+1; j<num_cropmarks; j++)
        {
            if (strcmp(cropmark_names[i], cropmark_names[j]) > 0)
            {
                strcpy(aux, cropmark_names[i]);
                strcpy(cropmark_names[i], cropmark_names[j]);
                strcpy(cropmark_names[j], aux);
            }
        }
    }
}

static void find_cropmarks()
{
    struct fio_file file;
    struct fio_dirent * dirent = FIO_FindFirstEx( "ML/CROPMKS/", &file );
    if( IS_ERROR(dirent) )
    {
        NotifyBox(2000, "ML/CROPMKS dir missing\n"
                        "Please copy all ML files!" );
        return;
    }
    int k = 0;
    do {
        if (file.mode & ATTR_DIRECTORY) continue; // is a directory
        if (is_valid_cropmark_filename(file.name))
        {
            if (k >= MAX_CROPMARKS)
            {
                NotifyBox(2000, "TOO MANY CROPMARKS (max=%d)", MAX_CROPMARKS);
                break;
            }
            snprintf(cropmark_names[k], MAX_CROP_NAME_LEN, "%s", file.name);
            k++;
        }
    } while( FIO_FindNextEx( dirent, &file ) == 0);
    FIO_FindClose(dirent);
    num_cropmarks = k;
    sort_cropmarks();
    cropmarks_initialized = 1;
}

static void reload_cropmark()
{
    int i = crop_index;
    static int old_i = -1;
    if (i == old_i) return; 
    old_i = i;
    //~ bmp_printf(FONT_LARGE, 0, 100, "reload crop: %d", i);

    if (cropmarks)
    {
        void* old_crop = cropmarks;
        cropmarks = 0;
        free(old_crop);
    }

    cropmark_clear_cache();
    
    if (!num_cropmarks) return;
    i = COERCE(i, 0, num_cropmarks-1);
    char bmpname[100];
    snprintf(bmpname, sizeof(bmpname), "ML/CROPMKS/%s", cropmark_names[i]);
    cropmarks = bmp_load(bmpname,1);
    if (!cropmarks) bmp_printf(FONT_LARGE, 0, 50, "LOAD ERROR %d:%s   ", i, bmpname);
}

static void
crop_toggle( void* priv, int sign )
{
    crop_index = MOD(crop_index + sign, num_cropmarks);
    //~ reload_cropmark(crop_index);
    crop_set_dirty(10);
}

static MENU_UPDATE_FUNC(crop_display)
{
    int index = crop_index;
    index = COERCE(index, 0, num_cropmarks-1);
    if (crop_enabled) MENU_SET_VALUE(
         num_cropmarks ? cropmark_names[index] : "N/A"
    );
    if (cropmark_movieonly && !is_movie_mode())
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Cropmarks are configured only for movie mode");
}

static MENU_UPDATE_FUNC(crop_display_submenu)
{
    int index = crop_index;
    index = COERCE(index, 0, num_cropmarks-1);
    MENU_SET_NAME(
        "Bitmap (%d/%d)",
         index+1, num_cropmarks
    );
    MENU_SET_VALUE(
        "%s",
         num_cropmarks ? cropmark_names[index] : "N/A"
    );

    if (info->can_custom_draw)
    {
        int h = 170;
        int w = h * 720 / 480;
        //~ int xc = 360 - w/2;
        int xc = 400;
        int yc = info->y + font_large.height * 3 + 10;
        BMP_LOCK( reload_cropmark(); )
        bmp_fill(0, xc, yc, w, h);
        BMP_LOCK( bmp_draw_scaled_ex(cropmarks, xc, yc, w, h, 0); )
        bmp_draw_rect(COLOR_WHITE, xc, yc, w, h);
    }
    
    MENU_SET_ICON(MNI_DICE, (num_cropmarks<<16) + index);
}

#ifdef FEATURE_CROPMARKS
static struct menu_entry cropmarks_menu[] = {
    {
        .name = "Cropmarks",
        .priv = &crop_enabled,
        .update    = crop_display,
        .max = 1,
        .help = "Cropmarks or custom grids for framing.",
        .depends_on = DEP_GLOBAL_DRAW,
        .submenu_width = 710,
        .submenu_height = 250,
        .children =  (struct menu_entry[]) {
            {
                .name = "Bitmap",
                .priv = &crop_index, 
                .select = crop_toggle,
                .update    = crop_display_submenu,
                .icon_type = IT_DICE,
                .help = "You can draw your own cropmarks in Paint.",
            },
            {
                .name = "Show in photo mode",
                .priv = &cropmark_movieonly, 
                .max = 1,
                .choices = (const char *[]) {"ON", "OFF"},
                .help = "Cropmarks are mostly used in movie mode.",
            },
            {
                .name = "Show in PLAY mode",
                .priv = &cropmarks_play, 
                .max = 1,
                .help = "You may also have cropmarks in Playback mode.",
            },
            MENU_EOL
        },
    },
};
#endif

static void cropmark_draw_from_cache()
{
    uint8_t* B = bmp_vram();
    uint8_t* M = get_bvram_mirror();
    get_yuv422_vram();
    ASSERT(B);
    ASSERT(M);
    
    for (int i = os.y0; i < os.y_max; i++)
    {
        for (int j = os.x0; j < os.x_max; j++)
        {
            uint8_t p = B[BM(j,i)];
            uint8_t m = M[BM(j,i)];
            if (!(m & 0x80)) continue;
            if (p != 0 && p != 0x14 && p != 0x3 && p != m) continue;
            B[BM(j,i)] = m & ~0x80;
        }
    }
}

void cropmark_clear_cache()
{
#ifdef FEATURE_CROPMARKS
    BMP_LOCK(
        /* clear focus peaking, zebras, but leave the old cropmark */
        clrscr_mirror();
        
        /* default_movie_cropmarks needs the old cropmark info, for incremental redrawing */
        /* e.g. if the cropmark shifts a little, it won't redraw the entire thing */
        /* so don't delete it */
        //~ bvram_mirror_clear();
        
        /* Cache the display overlay in bvram_mirror; this is separate from captured-image buffers. */
        /* Cropmark pixels are identified in this display-only mirror by 0x80. */
        /* This flag is internal bookkeeping and is never transferred to the real screen. */
        default_movie_cropmarks();
    )
#endif
}

static int cropmarks_last_drawn_x = -1;
static int cropmarks_last_drawn_y = -1;

static void cropmarks_clear_last_frame()
{
    if (cropmarks_last_drawn_x != -1 && cropmarks_last_drawn_y != -1)
    {
        int x1 = (cropmarks_last_drawn_x >> 16);
        int x2 = (cropmarks_last_drawn_x & 0xFFFF);
        int y1 = (cropmarks_last_drawn_y >> 16);
        int y2 = (cropmarks_last_drawn_y & 0xFFFF);
        bmp_draw_rect(0, x1, y1, x2-x1, y2-y1);
        bmp_draw_rect(0, x1-1, y1-1, x2-x1+2, y2-y1+2);
        //~ draw_line(x1,y1,x2,y2,0);
        //~ draw_line(x1,y2,x2,y1,0);
        cropmarks_last_drawn_x = -1;
        cropmarks_last_drawn_y = -1;
    }
}

/* only for bitmap cropmarks: if there are any overriden cropmarks, show where they are */
static void cropmarks_draw_frame()
{
    if (cropmarks_x != -1 && cropmarks_y != -1)
    {
        int x1 = (cropmarks_x >> 16);
        int x2 = (cropmarks_x & 0xFFFF);
        int y1 = (cropmarks_y >> 16);
        int y2 = (cropmarks_y & 0xFFFF);
        bmp_draw_rect(COLOR_GRAY(70), x1, y1, x2-x1, y2-y1);
        bmp_draw_rect(COLOR_BLACK, x1-1, y1-1, x2-x1+2, y2-y1+2);
        //~ draw_line(x1,y1,x2,y2,COLOR_RED);
        //~ draw_line(x1,y2,x2,y1,COLOR_RED);
        cropmarks_last_drawn_x = cropmarks_x;
        cropmarks_last_drawn_y = cropmarks_y;
    }
}

static int cropmarks_last_x = -1;
static int cropmarks_last_y = -1;

static int cropmarks_frame_changed()    /* changed since last call to cropmarks_frame_save */
{
    return (cropmarks_last_x != cropmarks_x) || (cropmarks_last_y != cropmarks_y);
}

static void cropmarks_frame_save()
{
    cropmarks_last_x = cropmarks_x;
    cropmarks_last_y = cropmarks_y;
}

static int should_use_default_cropmarks()
{
    return 
        (!crop_enabled) ||
        (cropmark_movieonly && !is_movie_mode() && lv);
}


static void 
cropmark_draw()
{
    if (!get_global_draw()) return;

    get_yuv422_vram(); // just to refresh VRAM params
    clear_lv_afframe_if_dirty();

    #ifdef FEATURE_GHOST_IMAGE
    if (transparent_overlay && !transparent_overlay_hidden && !PLAY_MODE)
    {
        show_overlay();
        zoom_overlay_dirty = 1;
        cropmark_cache_dirty = 1;
    }
    #endif
    crop_dirty = 0;

    reload_cropmark(); // reloads only when changed

    if (cropmarks_frame_changed())
    {
        cropmarks_clear_last_frame();
    }

    // this is very fast
    if (cropmark_cache_is_valid())
    {
        clrscr_mirror();
        cropmark_draw_from_cache();
        //~ bmp_printf(FONT_MED, 50, 50, "crop cached");
        //~ info_led_blink(5, 10, 10);
        goto end;
    }

    if (should_use_default_cropmarks())
    {
        // Cropmarks disabled (or not shown in this mode)
        // Generate and draw default cropmarks
        cropmark_cache_update_signature();
        cropmark_clear_cache();
        cropmark_draw_from_cache();
        //~ info_led_blink(5,50,50);
        goto end;
    }
    
    if (cropmarks) 
    {
        // Cropmarks enabled, but cache is not valid
        if (!lv) msleep(500); // let the bitmap buffer settle, otherwise ML may see black image and not draw anything (or draw half of cropmark)
        clrscr_mirror(); // clean any remaining zebras / peaking
        cropmark_cache_update_signature();
        bvram_mirror_clear();

        if (hdmi_code >= 5 && is_pure_play_movie_mode())
        {   // exception: cropmarks will have some parts of them outside the screen
            bmp_draw_scaled_ex(cropmarks, BMP_W_MINUS+1, BMP_H_MINUS - 50, 960, 640, bvram_mirror);
        }
        else
            bmp_draw_scaled_ex(cropmarks, os.x0, os.y0, os.x_ex, os.y_ex, bvram_mirror);
#ifdef CONFIG_EOSM
        /* Keep the XPan matte visible over a user cropmark bitmap. */
        if (!is_movie_mode() && XPAN_ACTIVE && !PLAY_MODE)
            default_movie_cropmarks();
#endif
        //~ info_led_blink(5,50,50);
        //~ bmp_printf(FONT_MED, 50, 50, "crop regen");
        goto end;
    }

end:

    if (!should_use_default_cropmarks())
    {
        /* bitmap cropmarks and overriden default cropmarks? show a simple rectangle */
        cropmarks_draw_frame();
    }
    cropmarks_frame_save();
    cropmark_cache_dirty = 0;
    zoom_overlay_dirty = 1;
    crop_dirty = 0;
}

static int cropmark_cache_sig = 0;

static int cropmark_cache_get_signature()
{
    get_yuv422_vram(); // update VRAM params if needed
    int sig = 
        (
            should_use_default_cropmarks() 
                ? (cropmarks_x * 1601 + cropmarks_y * 481)          /* default guide geometry cached for display redraw */
                : (crop_index * 13579 + crop_enabled * 14567)       /* bitmap guide cached for display redraw */
        )
        + (hdmi_code + EXT_MONITOR_RCA) * 315 +                     /* force redraw when changing display type (LCD, HDMI, SD) */
        os.x0*811 + os.y0*467 + os.x_ex*571 + os.y_ex*487 +         /* force redraw when bitmap parameters changed */
#ifdef CONFIG_EOSM
        (XPAN_ACTIVE * 73 + xpan_frame_align * 151) +        /* XPan photo guide settings */
#endif
        (is_movie_mode() ? 113 : 0) + video_mode_resolution * 8765; /* force redraw when video resolution changed, or when switching between video and photo mode */
    return sig;
}

static void cropmark_cache_update_signature()
{
    cropmark_cache_sig = cropmark_cache_get_signature();
}

static int cropmark_cache_is_valid()
{
    if (cropmark_cache_dirty) return 0; // some other ML task asked for redraw
    if (hdmi_code >= 5 && PLAY_MODE) return 0; // unusual geometry - better force full redraw every time
    
    int sig = cropmark_cache_get_signature(); // video mode changed => needs redraw
    if (cropmark_cache_sig != sig) return 0;
    
    return 1; // everything looks alright
}

static void
cropmark_redraw()
{
    if (!cropmarks_initialized) return;
    if (gui_menu_shown()) return; 
    if (!zebra_should_run() && !PLAY_OR_QR_MODE) return;
    if (digic_zoom_overlay_enabled()) return;
    BMP_LOCK(
        cropmark_draw(); 
    )
}

#if 0
void draw_cropmark_area()
{
    get_yuv422_vram();
    bmp_draw_rect(COLOR_BLUE, os.x0, os.y0, os.x_ex, os.y_ex);
    draw_line(os.x0, os.y0, os.x_max, os.y_max, COLOR_BLUE);
    draw_line(os.x0, os.y_max, os.x_max, os.y0, COLOR_BLUE);
    
    bmp_draw_rect(COLOR_RED, HD2BM_X(0), HD2BM_Y(0), HD2BM_DX(vram_hd.width), HD2BM_DY(vram_hd.height));
    draw_line(HD2BM_X(0), HD2BM_Y(0), HD2BM_X(vram_hd.width), HD2BM_Y(vram_hd.height), COLOR_RED);
    draw_line(HD2BM_X(0), HD2BM_Y(vram_hd.height), HD2BM_X(vram_hd.width), HD2BM_Y(0), COLOR_RED);
}

void show_apsc_crop_factor()
{
    int x_ex_crop = os.x_ex * 10/16;
    int y_ex_crop = os.y_ex * 10/16;
    int x_off = (os.x_ex - x_ex_crop)/2;
    int y_off = (os.y_ex - y_ex_crop)/2;
    bmp_draw_rect(COLOR_WHITE, os.x0 + x_off, os.y0 + y_off, x_ex_crop, y_ex_crop);
    bmp_draw_rect(COLOR_BLACK, os.x0 + x_off + 1, os.y0 + y_off + 1, x_ex_crop - 2, y_ex_crop - 2);
}
#endif

static void black_bars()
{
    if (!get_global_draw()) return;
    if (!is_movie_mode()) return;
    if (video_mode_resolution > 1) return; // these are only for 16:9
    int i,j;
    uint8_t * const bvram = bmp_vram();
    get_yuv422_vram();
    ASSERT(bvram);
    for (i = os.y0; i < MIN(os.y_max+1, BMP_H_PLUS); i++)
    {
        if (i < os.y0 + os.off_169 || i > os.y_max - os.off_169)
        {
            int newcolor = (i < os.y0 + os.off_169 - 2 || i > os.y_max - os.off_169 + 2) ? COLOR_BLACK : COLOR_BG;
            for (j = os.x0; j < os.x_max; j++)
            {
                if (bvram[BM(j,i)] == COLOR_BG)
                    bvram[BM(j,i)] = newcolor;
            }
        }
    }
}

static void FAST default_movie_cropmarks()
{
    if (!get_global_draw()) return;
    if (!lv) return;
#ifdef CONFIG_EOSM
    int photo_xpan = !is_movie_mode() && XPAN_ACTIVE && !PLAY_MODE && lv_dispsize == 1;
#else
    if (!is_movie_mode())
    {
        /* no default cropmarks in photo mode */
        bvram_mirror_clear();
        return;
    }
#endif
    
    int x,y;
    /* Cropmarks are display annotations: they are written to bitmap VRAM, never the image buffers. */
    uint8_t * const bvram = bmp_vram();
    if (!bvram) return;
    uint8_t * const bvram_mirror = get_bvram_mirror();
    get_yuv422_vram();
    if (!bvram_mirror) return;

    int crop_x = cropmarks_x;
    int crop_y = cropmarks_y;
    
#ifdef CONFIG_EOSM
    if (photo_xpan)
    {
        /* Draw a preview-only guide using the same geometry; saved image data is handled elsewhere. */
        int frame_x, frame_y, frame_w, frame_h;
        if (xpan_photo_get_liveview_frame_rect(&frame_x, &frame_y, &frame_w, &frame_h))
        {
            int left = os.x0 + frame_x;
            int top = os.y0 + frame_y;
            /* The renderer includes both endpoints, so subtract one for exact pixel dimensions. */
            crop_x = (left << 16) | (left + frame_w - 1);
            crop_y = (top << 16) | (top + frame_h - 1);
        }
        else
        {
            crop_x = (os.x0 << 16) | os.x_max;
            crop_y = (os.y0 << 16) | os.y_max;
        }
    }
#endif
#ifdef CONFIG_EOSM
    if (!photo_xpan && !is_movie_mode())
    {
        /* no photo mask (or playback): use full image to clear old XPan bars */
        crop_x = (os.x0 << 16) | os.x_max;
        crop_y = (os.y0 << 16) | os.y_max;
    }
    else if (is_movie_mode() && (crop_x == -1 || crop_y == -1))
#else
    if (crop_x == -1 || crop_y == -1)
#endif
    {
        if(video_mode_resolution > 1) // 4:3
        {
            crop_x = ((os.x0 + os.off_43) << 16) | (os.x_max - os.off_43);
            crop_y = (os.y0 << 16) | os.y_max;
        }
        else
        {
            crop_x = (os.x0 << 16) | os.x_max;
            crop_y = ((os.y0 + os.off_169) << 16) | (os.y_max - os.off_169);
        }
    }
    
    /*
        doesn't work with HDMI, please fix that, I don't have it
    */
#ifdef CONFIG_EOSM
    /* Extend the XPan top matte to the screen edge so the Canon Live View seam is covered last. */
    int first_y = photo_xpan ? 0 : os.y0;
#else
    int first_y = os.y0;
#endif
    for (y = first_y; y < MIN(os.y_max+1, BMP_H_PLUS); y++)
    {
        int x1 = (crop_x >> 16);
        int x2 = (crop_x & 0xFFFF);
        int y1 = (crop_y >> 16);
        int y2 = (crop_y & 0xFFFF);
        
        bool draw = (y < y1 || y > y2);
        
        for (x = os.x0; x < os.x_max; x++)
        {
            int newcolor = 0;
            int newcolor_with_flag = 0;
            if(draw || (x < x1 || x > x2))
            {
                /* default cropmarks are black */
                /* border pixels should be brighter, to be visible in dark */
                int border = (x > x1-2 && x < x2+2 && y > y1-2 && y < y2+2);
#ifdef CONFIG_EOSM
                /* Keep the XPan frame edge flush; its old grey guide line is not part of the photo UI. */
                if (photo_xpan) border = 0;
#endif
                newcolor = border ? COLOR_GRAY(70) : COLOR_BLACK;
            }
            
            if (newcolor)
            {
                /* prevent zebras from drawing here */
                newcolor_with_flag = newcolor | 0x80;
            }

            int idx = BM(x,y);
            if (newcolor_with_flag != bvram_mirror[idx])
            {
                /* anything changed? */
                
                if (newcolor != bvram[idx] && (bvram_mirror[idx] & 0x80) && ((bvram_mirror[idx] & ~0x80) == bvram[idx]))
                {
                    /* pixel from the old cropmark? clear from the main screen too */
                    /* otherwise, cropmark_draw_from_cache will ignore those */
                    /* note: we could clear the entire screen, but redrawing everything looks ugly; incremental redraws look better */
                    bvram[idx] = 0;
                }
                
                bvram_mirror[idx] = newcolor_with_flag;
            }
        }
    }
}

void set_movie_cropmarks(int x, int y, int w, int h)
{
    x = COERCE(x, os.x0+1, os.x_max-1);
    y = COERCE(y, os.y0+2, os.y_max-2);
    w = COERCE(w, 0, os.x_max-2 - x);
    h = COERCE(h, 0, os.y_max-4 - y);
    cropmarks_x = (x << 16) | (x + w);
    cropmarks_y = (y << 16) | (y + h);
}

void reset_movie_cropmarks()
{
    cropmarks_x = cropmarks_y = -1;
}

static void cropmark_step()
{
#ifdef CONFIG_EOSM
    static int last_xpan_state = -1;
    int xpan_state = XPAN_ACTIVE * 4 + xpan_frame_align * 2 + xpan_photo_aspect_ratio() +
        (lv_dispsize == 1) * 64;
    if (xpan_state != last_xpan_state)
    {
        last_xpan_state = xpan_state;
        cropmark_cache_dirty = 1;
        crop_set_dirty(10);
    }
#endif

    /* cropmark frame changed and nobody asked for update? force an update now (once per second) */
    /* food for thought: could this replace the entire crop_set_dirty thingie? */
    static int aux = 0;
    if (!crop_dirty && should_run_polling_action(RECORDING ? 500 : 100, &aux) && cropmarks_frame_changed())
    {
        crop_dirty = 1;
    }

    if (crop_dirty && lv && zebra_should_run())
    {
        crop_dirty--;
        
        //~ bmp_printf(FONT_MED, 50, 100, "crop: cache=%d dirty=%d ", cropmark_cache_is_valid(), crop_dirty);

        // if cropmarks are disabled, we will still draw default cropmarks (fast)
        if (should_use_default_cropmarks()) 
            crop_dirty = MIN(crop_dirty, 2);
        
        // if cropmarks are cached, we can redraw them fast
        if (cropmark_cache_is_valid() && !should_draw_zoom_overlay() && !get_halfshutter_pressed())
            crop_dirty = MIN(crop_dirty, 2);
            
        if (crop_dirty == 0)
            cropmark_redraw();
    }
}
