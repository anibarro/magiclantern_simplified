/* Included by flexinfo.c. All coordinates are720x480 LCD pixels; no heap. */
static int xpan_hud_background = COLOR_BLACK;
/* The workflow log reads these counters outside the display task. */
static volatile unsigned xpan_hud_render_calls, xpan_hud_last_clipped;
static volatile int xpan_hud_last_y = -1;

void xpan_photo_hud_debug_snapshot(unsigned *draw_calls, int *last_y, unsigned *clipped)
{
    if (draw_calls) *draw_calls = xpan_hud_render_calls;
    if (last_y) *last_y = xpan_hud_last_y;
    if (clipped) *clipped = xpan_hud_last_clipped;
}

static int xpan_hud_text(int x, int y, int width, int height,
    int preferred, int color, const char *text)
{
    /* Fit actual proportional glyph widths, then ellipsize inside this cell.
     * Large exposure values may step down to20px; labels stay12px. */
    static const int fonts[]={FONT_LARGE,FONT_MED_LARGE,FONT_MED,FONT_MONO_20};
    int bold=preferred!=FONT_SMALL;
    width-=bold; /* Reserve the extra stroke pixel inside the same cell. */
    int font=preferred;
    if (preferred!=FONT_SMALL)
        for (unsigned i=0;i<COUNT(fonts);i++)
            if (fontspec_height(fonts[i])<=fontspec_height(preferred)) {
                font=fonts[i];
                if (bmp_string_width(font,text)<=width) break;
            }
    char clipped[96];
    snprintf(clipped,sizeof(clipped),"%s",text);
    if (bmp_string_width(font,clipped)>width) {
        int n=bmp_strlen_clipped(font,clipped,MAX(0,width-bmp_string_width(font,"...")));
        n=COERCE(n,0,(int)sizeof(clipped)-4);
        strcpy(clipped+n,"...");
    }
    int baseline=y+MAX(0,(height-fontspec_height(font))/2);
    /* Transparent bitmap pixels expose Live View; clear the cell to erase old text. */
    if (xpan_hud_background==COLOR_TRANSPARENT_BLACK)
        bmp_fill(COLOR_TRANSPARENT_BLACK,x,y,width,height);
    bmp_printf(FONT(font,color,xpan_hud_background),x,baseline,"%s",clipped);
    /* Local one-pixel weight gives the draft's bolder hierarchy without
     * replacing fonts used by Canon or the rest of the ML menu. */
    if (bold) bmp_printf(FONT(font,color,NO_BG_ERASE),x+1,baseline,"%s",clipped);
    return bmp_string_width(font,clipped)+bold;
}

static void __attribute__((unused)) xpan_hud_dotted_line(int x, int y, int width, int color)
{
    /* Width follows the fitted value, including its bold stroke. */
    for (int dot=0;dot<width;dot+=5) bmp_fill(color,x+dot,y,MIN(2,width-dot),1);
}

/* Field edited by the rear wheel (INFO/REC cycles it): solid 2px underline. */
#ifdef CONFIG_XPAN
#define XPAN_HUD_SELECTED(field) (xpan_hud_controls_field()==(field))
/* Disabled (no effect in this setup): drawn in grey and not selectable. */
#define XPAN_HUD_ENABLED(field) xpan_hud_controls_field_enabled(field)
int xpan_native_look_loading(void);
#else
#define XPAN_HUD_SELECTED(field) 0
#define XPAN_HUD_ENABLED(field) 1
#endif
/* Set after ML's layer was cleared: the next HUD frame is drawn in full. */
static volatile int xpan_hud_force;
void xpan_photo_hud_force_redraw(void) { xpan_hud_force=1; }

static void xpan_hud_select_line(int x, int y, int width, int color)
{
    bmp_fill(color,x,y,MAX(width,12),2);
}

/* Touch targets, recorded by the last HUD draw (LCD pixels). */
#ifdef CONFIG_XPAN
static struct { short x,y,w,h; } xpan_hud_hit[XPAN_HUD_FIELDS];
static volatile int xpan_hud_hit_valid;
static void xpan_hud_hit_set(int field, int x, int y, int w, int h)
{
    xpan_hud_hit[field].x=x; xpan_hud_hit[field].y=y; xpan_hud_hit[field].w=w; xpan_hud_hit[field].h=h;
}
/* Field under an LCD point, -2 inside the HUD band but on no field, -1 outside. */
int xpan_photo_hud_hit_test(int x, int y)
{
    if (!xpan_hud_hit_valid || xpan_hud_last_y<0) return -1;
    if (y<xpan_hud_last_y || y>=xpan_hud_last_y+XPAN_PHOTO_HUD_HEIGHT) return -1;
    for (int f=0;f<XPAN_HUD_FIELDS;f++)
        if (xpan_hud_hit[f].w && x>=xpan_hud_hit[f].x && x<xpan_hud_hit[f].x+xpan_hud_hit[f].w &&
            y>=xpan_hud_hit[f].y && y<xpan_hud_hit[f].y+xpan_hud_hit[f].h) return f;
    return -2;
}
int xpan_photo_hud_hit_center(int field)
{
    if (field<0 || field>=XPAN_HUD_FIELDS) return 360;
    return xpan_hud_hit[field].x+xpan_hud_hit[field].w/2;
}
#define XPAN_HUD_HIT(f,x,y,w,h) xpan_hud_hit_set(f,x,y,w,h)
#else
#define XPAN_HUD_HIT(f,x,y,w,h) do {} while (0)
#endif

#ifdef CONFIG_XPAN
int xpan_film_grain_enabled(void);
/* Film grain ON: a tiny fixed speckle, no text. */
static void xpan_hud_grain_icon(int x, int y)
{
    static const unsigned char dots[][2]={{1,1},{6,0},{11,2},{3,5},{9,6},{0,9},{6,10},{12,9},{3,13},{10,13}};
    for (unsigned i=0;i<COUNT(dots);i++) bmp_fill(COLOR_WHITE,x+dots[i][0],y+dots[i][1],2,2);
}
#endif

static const char *xpan_hud_drive(void)
{
    switch (drive_mode) {
        case DRIVE_SINGLE:return "SINGLE";
        case DRIVE_CONTINUOUS:return "BURST";
        case DRIVE_SELFTIMER_REMOTE:return "10s";
        case DRIVE_SELFTIMER_2SEC:return "2s";
        case DRIVE_SELFTIMER_CONTINUOUS:return "TIMER+";
        default:return "OTHER";
    }
}

static const char *xpan_hud_metering(void)
{
    /* Canon's mode values (also documented in libgphoto2's Canon table).
     * Unknown modes are explicit rather than falsely labeled evaluative. */
    switch (metering_mode) {
        case 0: case 5:return "CENTER";
        case 1:return "SPOT";
        case 2:return "AVERAGE";
        case 3:return "EVAL";
        case 4:return "PARTIAL";
        default:return "--";
    }
}

/* XPan logo, 125x46 px: traced from extras/UI ideas/xpanlogo.jpg (orange
 * coverage above 30% after an area downscale), so the X keeps its hollow
 * diagonal, separate arms and the underline running under PAN. One bit per
 * pixel, bit x%32 of word x/32; drawn as horizontal runs. */
#define XPAN_LOGO_W 125
#define XPAN_LOGO_H 46
static const uint32_t xpan_logo_bits[XPAN_LOGO_H][4]={
        {0x0003fffe,0x00000000,0x00000000,0x00000000},
        {0x0003fffe,0x00000000,0x00000000,0x00000000},
        {0x0007fffe,0x00000000,0x00000000,0x00000000},
        {0x0007001c,0x00000000,0x00000000,0x00000000},
        {0x000f0038,0x00000000,0x00000000,0x00000000},
        {0x000e0078,0x00000000,0x00000000,0x00000000},
        {0x001e0070,0x00000000,0x00000000,0x00000000},
        {0x001c00f0,0x00000000,0x00000000,0x00000000},
        {0x003800e0,0x00000000,0x00000000,0x00000000},
        {0x007801e0,0x00000000,0x00000000,0x00000000},
        {0x007001c0,0x00000000,0x00000000,0x00000000},
        {0xf0f003c0,0xffffff1f,0x0000f000,0x1c00000f},
        {0xf8e00780,0xffffff0f,0x0001f003,0x1c00001f},
        {0x3de00700,0xe0000707,0x0003f807,0x1c00003f},
        {0x9fc00f00,0x00000707,0x0003b80f,0x1c00007f},
        {0xdfc00e00,0x00000703,0x0007bc0e,0x1c0000f7},
        {0xef801e00,0x00000701,0x00071c1e,0x1c0001e7},
        {0xe7801c00,0x00000701,0x000f1e1e,0x1c0003c7},
        {0xf3003c00,0x00000700,0x000e0e1e,0x1c000787},
        {0x7a003800,0x00000700,0x001e0f1e,0x1c000f07},
        {0x3c007800,0x00000700,0x001c070e,0x1c001e07},
        {0x3c007000,0x00000700,0x003c078f,0x1c003c07},
        {0x3c00f000,0x80000700,0x00380387,0x1c007807},
        {0x3800e000,0xffffff00,0x007801c7,0x1c00f007},
        {0x7801c000,0xffffff00,0x007001c1,0x1c01e007},
        {0x7001c000,0x7fffff00,0x00f001e0,0x1c03c007},
        {0xf003c000,0x00000700,0x00e000e0,0x1c078007},
        {0xe001e000,0x00000700,0x01e000f0,0x1c0f0007},
        {0xc004e000,0x00000701,0x01fffff0,0x1c1e0007},
        {0xc00ef000,0x00000701,0x03fffff8,0x1c3c0007},
        {0x800e7800,0x00000703,0x03fffff8,0x1c780007},
        {0x801f3c00,0x00000707,0x0700001c,0x1cf00007},
        {0x001fbc00,0x00000707,0x0700001c,0x1de00007},
        {0x003f9e00,0x0000070f,0x0e00000e,0x1fc00007},
        {0x0039cf00,0x0000070e,0x1e00000e,0x1f800007},
        {0x0071e780,0x0000071e,0x1c000007,0x1f000007},
        {0x0070f380,0x8000071c,0x3c000007,0x1e000007},
        {0x00e073c0,0x80000738,0x38000003,0x1c000007},
        {0x01e079e0,0x00000000,0x00000000,0x00000000},
        {0x01c03cf0,0x00000000,0x00000000,0x00000000},
        {0x03c01e70,0x00000000,0x00000000,0x00000000},
        {0x03800e78,0x00000000,0x00000000,0x00000000},
        {0x07800f3c,0x00000000,0x00000000,0x00000000},
        {0xff0007fe,0xffffffff,0xffffffff,0x1fffffff},
        {0xff0003fe,0xffffffff,0xffffffff,0x1fffffff},
        {0xfe0001ff,0xffffffff,0xffffffff,0x1fffffff},
};

static void xpan_hud_logo(int y, int color)
{
    if (xpan_hud_background==COLOR_TRANSPARENT_BLACK)
        bmp_fill(COLOR_TRANSPARENT_BLACK,8,y,542,50);
    for (int row=0;row<XPAN_LOGO_H;row++)
        for (int x=0;x<XPAN_LOGO_W;) {
            if (!(xpan_logo_bits[row][x>>5]>>(x&31)&1)) { x++; continue; }
            int start=x;
            while (x<XPAN_LOGO_W && (xpan_logo_bits[row][x>>5]>>(x&31)&1)) x++;
            bmp_fill(color,10+start,y+2+row,x-start,1);
        }
}

static void xpan_hud_battery(int x, int y)
{
    /* EOS M reports three coarse levels, not a reliable percentage. */
    int level=COERCE(battery_level_bars,0,2);
    int color=level==0 ? COLOR_RED:level==1 ? COLOR_YELLOW:COLOR_GREEN2;
    bmp_draw_rect(COLOR_WHITE,x,y+12,43,23);
    bmp_draw_rect(COLOR_WHITE,x+1,y+13,41,21);
    bmp_fill(COLOR_WHITE,x+44,y+19,4,10);
    bmp_fill(color,x+5,y+17,12*(level+1),14);
}

static const char *xpan_hud_photo_mode(void)
{
    /* A/S are the compact aperture/shutter-priority labels requested for
     * this UI (Canon calls these Av/Tv). Read the mode; never change it. */
    switch (shooting_mode) {
        case SHOOTMODE_M:return "M";
        case SHOOTMODE_AV:return "A";
        case SHOOTMODE_TV:return "S";
        case SHOOTMODE_P:return "P";
        default:return get_shootmode_name_short(shooting_mode_custom);
    }
}

static void xpan_hud_exposure_scale(int y, int color, unsigned selected)
{
    /* Show compensation, not an inferred spot-meter measurement. Keep its
     * number signed and move the marker to the edge for values beyond3EV. */
    int ec=lens_info.ae, tenths=(ABS(ec)*10+4)/8;
    char value[24];
    snprintf(value,sizeof(value),"%s%d.%d EV",ec<0 ? "-":ec>0 ? "+":"",tenths/10,tenths%10);
    /* Center the white value on the zero tick, including the bold stroke. */
    int value_width=bmp_string_width(FONT_MED,value)+1;
    xpan_hud_text(596-value_width/2,y+140,value_width,25,FONT_MED,
        XPAN_HUD_ENABLED(XPAN_HUD_EV) ? COLOR_WHITE:COLOR_GRAY(45),value);
    (void)selected;
    if (XPAN_HUD_SELECTED(XPAN_HUD_EV))
        xpan_hud_select_line(596-value_width/2,y+166,value_width,color);
    XPAN_HUD_HIT(XPAN_HUD_EV,480,y+136,230,XPAN_PHOTO_HUD_HEIGHT-136);
    for (int step=-9;step<=9;step++) {
        int x=596+step*12;
        bmp_fill(step==0 ? color:COLOR_GRAY(55),x,y+190,1,step%3 ? 3:5);
        if (!(step%3)) {
            char tick[5];snprintf(tick,sizeof(tick),"%s%d",step>0?"+":"",step/3);
            /* Center each actual glyph advance on its own major tick. A
             * fixed left margin shifts the one-character zero off-center. */
            int width=bmp_string_width(FONT_SMALL,tick);
            xpan_hud_text(x-width/2,y+173,width,12,FONT_SMALL,step==0?color:COLOR_GRAY(65),tick);
        }
    }
    int marker=596+COERCE(ec,-24,24)*108/24;
    /* A larger upward triangle sits below the ticks, with a clear gap. */
    for (int row=0;row<10;row++) bmp_fill(color,marker-row,y+198+row,row*2+1,1);
}

/* AF frame drawn by XPan: Canon's GUI layer is hidden while the HUD owns
 * Live View, and Canon placed it in 3:2 coordinates anyway. The frame comes
 * from PROP_LV_AFFRAME, is mapped through the same row remap as the 65:24
 * preview (source band -> aligned frame) and clipped to the visible picture.
 * Green while half-pressed with focus confirmed, white otherwise. */
extern int lv_dispsize;   /* propvalues.h: 1, 5 or 10 (magnified Live View) */
int xpan_photo_get_preview_rect(int width, int height, int *x, int *y, int *crop_width, int *crop_height);
int xpan_photo_get_liveview_frame_rect(int *x, int *y, int *crop_width, int *crop_height);
static struct { short x,y,w,h; } xpan_af_drawn;
static int xpan_af_rect(int *x, int *y, int *w, int *h, int hud_y)
{
    if (!lens_info.lens_exists || is_manual_focus() || lv_dispsize != 1) return 0;
    int ax,ay,aw,ah,W,H;
    if (!get_afframe_rect(&ax,&ay,&aw,&ah,&W,&H)) return 0;
    int lx=os.x0+ax*os.x_ex/W, ly=os.y0+ay*os.y_ex/H;
    int lw=MAX(aw*os.x_ex/W,16), lh=MAX(ah*os.y_ex/H,16);
    int top=0,bottom=480;
    if (xpan_photo_is_panoramic()) {
        int sx,sy,sw,sh,fx,fy,fw,fh;
        if (!xpan_photo_get_preview_rect(os.x_ex,os.y_ex,&sx,&sy,&sw,&sh) ||
            !xpan_photo_get_liveview_frame_rect(&fx,&fy,&fw,&fh)) return 0;
        ly+=fy-sy; top=fy; bottom=fy+fh;          /* same shift as the pixels */
    } else if (xpan_photo_hud_is_enabled()) {
        /* 3:2: keep the frame off the HUD panel. */
        if (hud_y<=0) top=XPAN_PHOTO_HUD_HEIGHT; else bottom=hud_y;
    }
    int y0=MAX(ly,top), y1=MIN(ly+lh,bottom);
    if (y1-y0<6) return 0;                         /* outside the 65:24 picture */
    *x=lx; *y=y0; *w=lw; *h=y1-y0;
    return 1;
}
/* XPan touch AF: an LCD point in the visible picture -> AF frame centre.
 * In 65:24 the picture rows are Canon's middle rows shifted to the aligned
 * frame, so the touch row is shifted back before conversion. Returns 0 when
 * the point is outside the picture (or AF is not in use). */
int xpan_photo_touch_af(int x, int y)
{
    if (!lens_info.lens_exists || is_manual_focus() || lv_dispsize != 1) return 0;
    int ax,ay,aw,ah,W,H;
    if (!get_afframe_rect(&ax,&ay,&aw,&ah,&W,&H) || os.x_ex<=0 || os.y_ex<=0) return 0;
    if (xpan_photo_is_panoramic()) {
        int sx,sy,sw,sh,fx,fy,fw,fh;
        if (!xpan_photo_get_preview_rect(os.x_ex,os.y_ex,&sx,&sy,&sw,&sh) ||
            !xpan_photo_get_liveview_frame_rect(&fx,&fy,&fw,&fh)) return 0;
        if (y<fy || y>=fy+fh) return 0;
        y+=sy-fy;
    }
    set_lv_afframe_center((x-os.x0)*W/os.x_ex,(y-os.y0)*H/os.y_ex);
    return 1;
}

static void xpan_af_corners(int x, int y, int w, int h, int color)
{
    int l=COERCE(MIN(w,h)/4,6,24);
    bmp_fill(color,x,y,l,2);         bmp_fill(color,x,y,2,l);
    bmp_fill(color,x+w-l,y,l,2);     bmp_fill(color,x+w-2,y,2,l);
    bmp_fill(color,x,y+h-2,l,2);     bmp_fill(color,x,y+h-l,2,l);
    bmp_fill(color,x+w-l,y+h-2,l,2); bmp_fill(color,x+w-2,y+h-l,2,l);
}
static void xpan_af_draw(int hud_y)
{
    if (xpan_af_drawn.w) {           /* erase the previous frame */
        xpan_af_corners(xpan_af_drawn.x,xpan_af_drawn.y,xpan_af_drawn.w,xpan_af_drawn.h,COLOR_EMPTY);
        xpan_af_drawn.w=0;
    }
    int x,y,w,h;
    if (!xpan_af_rect(&x,&y,&w,&h,hud_y)) return;
    int focused=get_halfshutter_pressed() && get_focus_confirmation();
    xpan_af_corners(x,y,w,h,focused ? COLOR_GREEN1:COLOR_WHITE);
    xpan_af_drawn.x=x; xpan_af_drawn.y=y; xpan_af_drawn.w=w; xpan_af_drawn.h=h;
}

/* XPan > Grid lines (rule of thirds), drawn by this renderer so the HUD's own
 * full-screen clear (overlay OFF) and the AF frame erase cannot remove it.
 * In 3:2 with the overlay on, the HUD panel rows stay free of lines. */
int xpan_photo_grid_mode(void);   /* cropmarks.c: 0 OFF, 1 thirds */
static struct { short x,y,w,h,s0,s1; char on; } xpan_grid_drawn;
static void xpan_grid_lines(int x, int y, int w, int h, int s0, int s1, int color)
{
    for (int k=1;k<3;k++) {
        int gx=x+w*k/3, gy=y+h*k/3;
        int a1=MIN(y+h,s0);                         /* vertical, above the panel */
        if (a1>y) bmp_fill(color,gx,y,1,a1-y);
        int b0=MAX(y,s1);                           /* vertical, below the panel */
        if (y+h>b0) bmp_fill(color,gx,b0,1,y+h-b0);
        if (gy<s0 || gy>=s1) bmp_fill(color,x,gy,w,1);
    }
}
static void xpan_grid_draw(int hud_y)
{
    int fx,fy,fw,fh,want=xpan_photo_grid_mode() && lv_dispsize==1 &&
        xpan_photo_get_liveview_frame_rect(&fx,&fy,&fw,&fh);
    int x=0,y=0,s0=0,s1=0;
    if (want) {
        x=os.x0+fx; y=os.y0+fy;
        if (!xpan_photo_is_panoramic() && xpan_photo_hud_is_enabled() && hud_y>=0)
        { s0=hud_y; s1=hud_y+XPAN_PHOTO_HUD_HEIGHT; }
    }
    if (xpan_grid_drawn.on && (!want || xpan_grid_drawn.x!=x || xpan_grid_drawn.y!=y ||
        xpan_grid_drawn.w!=fw || xpan_grid_drawn.h!=fh || xpan_grid_drawn.s0!=s0 || xpan_grid_drawn.s1!=s1))
        xpan_grid_lines(xpan_grid_drawn.x,xpan_grid_drawn.y,xpan_grid_drawn.w,xpan_grid_drawn.h,
            xpan_grid_drawn.s0,xpan_grid_drawn.s1,COLOR_EMPTY);
    xpan_grid_drawn.on=0;
    if (!want) return;
    xpan_grid_lines(x,y,fw,fh,s0,s1,COLOR_GRAY(80));
    xpan_grid_drawn.x=x; xpan_grid_drawn.y=y; xpan_grid_drawn.w=fw; xpan_grid_drawn.h=fh;
    xpan_grid_drawn.s0=s0; xpan_grid_drawn.s1=s1; xpan_grid_drawn.on=1;
}

#ifdef CONFIG_XPAN
/* "Loading..." box in the middle of the picture while a LUT is prepared
 * (photo 65:24 and 3:2, movie). The LUT name below it is the one selected,
 * which may already be ahead of the table being built. */
static struct { short x,y,w,h; } xpan_loading_drawn;
#define XPAN_LOADING_W 320
#define XPAN_LOADING_H 76
static int xpan_loading_erase(void)
{
    if (!xpan_loading_drawn.w) return 0;
    bmp_fill(COLOR_EMPTY,xpan_loading_drawn.x,xpan_loading_drawn.y,xpan_loading_drawn.w,xpan_loading_drawn.h);
    xpan_loading_drawn.w=0;
    return 1;
}
/* Picture rows top..bottom (LCD); the box is centred in them. */
static void xpan_loading_box(int top, int bottom, int color)
{
    int x=360-XPAN_LOADING_W/2, y=(top+bottom)/2-XPAN_LOADING_H/2;
    char name[64],state[24];
    xpan_native_look_hud_label(name,sizeof(name),state,sizeof(state));
    int background=xpan_hud_background;
    xpan_hud_background=COLOR_TRANSPARENT_BLACK;   /* dims Live View, keeps it visible */
    bmp_fill(COLOR_TRANSPARENT_BLACK,x,y,XPAN_LOADING_W,XPAN_LOADING_H);
    bmp_draw_rect(color,x,y,XPAN_LOADING_W-1,XPAN_LOADING_H-1);
    int w=bmp_string_width(FONT_LARGE,"Loading...")+1;
    xpan_hud_text(360-w/2,y+6,w,36,FONT_LARGE,COLOR_WHITE,"Loading...");
    w=MIN(XPAN_LOADING_W-24,bmp_string_width(FONT_MED,name)+1);
    xpan_hud_text(360-w/2,y+44,w,26,FONT_MED,color,name);
    xpan_hud_background=background;
    xpan_loading_drawn.x=x; xpan_loading_drawn.y=y;
    xpan_loading_drawn.w=XPAN_LOADING_W; xpan_loading_drawn.h=XPAN_LOADING_H;
}
/* Photo: the visible picture is the Live View frame, minus the HUD panel
 * when it overlays the 3:2 view. Returns 0 when nothing is loading. */
static int xpan_loading_photo_area(int hud_y, int *top, int *bottom)
{
    if (!xpan_native_look_loading()) return 0;
    int fx,fy,fw,fh;
    *top=0; *bottom=480;
    if (xpan_photo_get_liveview_frame_rect(&fx,&fy,&fw,&fh)) { *top=os.y0+fy; *bottom=*top+fh; }
    if (!xpan_photo_is_panoramic() && xpan_photo_hud_is_enabled() && hud_y>=0) {
        if (hud_y<=*top) *top=MAX(*top,hud_y+XPAN_PHOTO_HUD_HEIGHT);
        else *bottom=MIN(*bottom,hud_y);
    }
    if (*bottom-*top<XPAN_LOADING_H) { *top=0; *bottom=480; }
    return 1;
}
int xpan_movie_enabled(void);   /* cropmarks.c: XPan ON/OFF in movie mode */
/* Movie mode has no XPan HUD: called from ML's Live View info loop. Redrawn
 * every half second, because Canon's movie screen can paint over it. */
void xpan_movie_loading_draw(void)
{
    static int last_ms;
    int want=is_movie_mode() && xpan_movie_enabled() && lv && !gui_menu_shown() &&
        xpan_native_look_loading();
    if (!want) { xpan_loading_erase(); return; }
    if (xpan_loading_drawn.w && get_ms_clock()-last_ms<500) return;
    last_ms=get_ms_clock();
    xpan_loading_box(os.y0,os.y0+os.y_ex,xpan_photo_theme_color());
}
#endif

/* Copy only the HUD rows between the live bitmap and the idle buffer, so
 * whatever else is on screen (Canon remnants, ML overlays) is never captured
 * into the idle buffer and written back. */
static void xpan_hud_copy_band(int to_front, int y0, int h)
{
    uint8_t *real=bmp_vram_real(),*idle=bmp_vram_idle();
    if (!real || !idle) return;
    for (int y=MAX(y0,0);y<MIN(y0+h,480);y++)
        memcpy((to_front ? real:idle)+y*BMPPITCH,(to_front ? idle:real)+y*BMPPITCH,720);
}

static void xpan_photo_render_hud(info_elem_t *config)
{
    (void)config;
    static int last_hud_y=-1;
    int panoramic=xpan_photo_is_panoramic();
    int y=xpan_photo_get_hud_y(),theme=xpan_photo_theme_color();
    xpan_hud_render_calls++;
    xpan_hud_last_y=y;
    xpan_hud_last_clipped=(y<0 || y+XPAN_PHOTO_HUD_HEIGHT>480);
    if (xpan_hud_last_clipped) return;
    /* Skip identical frames: each draw copies the whole 720x480 bitmap twice.
     * Anything the controls change is in the signature; a full refresh every
     * 600 ms still covers the clock, battery, card space and ML redraws. */
    {
        unsigned sig=2166136261u;
        #define XPAN_HUD_SIG(v) (sig=(sig^(unsigned)(v))*16777619u)
        XPAN_HUD_SIG(y); XPAN_HUD_SIG(theme); XPAN_HUD_SIG(panoramic); XPAN_HUD_SIG(xpan_photo_hud_is_enabled());
        XPAN_HUD_SIG(xpan_photo_grid_mode()); XPAN_HUD_SIG(lv_dispsize);
        XPAN_HUD_SIG(shooting_mode); XPAN_HUD_SIG(lens_info.raw_iso); XPAN_HUD_SIG(lens_info.iso_auto);
        XPAN_HUD_SIG(lens_info.raw_shutter); XPAN_HUD_SIG(lens_info.raw_aperture); XPAN_HUD_SIG(lens_info.ae);
        XPAN_HUD_SIG(lens_info.wb_mode); XPAN_HUD_SIG(lens_info.kelvin); XPAN_HUD_SIG(lens_info.lens_exists);
        XPAN_HUD_SIG(lens_info.focal_len); XPAN_HUD_SIG(af_mode); XPAN_HUD_SIG(drive_mode); XPAN_HUD_SIG(metering_mode);
        XPAN_HUD_SIG(pic_quality); XPAN_HUD_SIG(xpan_pic_quality_valid);
        {   /* AF frame position and focus confirmation */
            int ax=0,ay=0,aw=0,ah=0,W=0,H=0;
            get_afframe_rect(&ax,&ay,&aw,&ah,&W,&H);
            XPAN_HUD_SIG(ax); XPAN_HUD_SIG(ay); XPAN_HUD_SIG(aw); XPAN_HUD_SIG(ah); XPAN_HUD_SIG(af_mode);
            XPAN_HUD_SIG(get_halfshutter_pressed() && get_focus_confirmation());
        }
#ifdef CONFIG_XPAN
        XPAN_HUD_SIG(xpan_hud_controls_field()); XPAN_HUD_SIG(xpan_native_look_loading());
        XPAN_HUD_SIG(xpan_photo_debug_enabled());
        {
            char name[64],state[24];
            xpan_native_look_hud_label(name,sizeof(name),state,sizeof(state));
            for (char *c=name;*c;c++) XPAN_HUD_SIG(*c);
            for (char *c=state;*c;c++) XPAN_HUD_SIG(*c);
        }
#endif
#ifdef CONFIG_XPAN
        XPAN_HUD_SIG(xpan_film_grain_enabled());
#endif
        #undef XPAN_HUD_SIG
        static unsigned last_sig; static int last_ms;
        int now=get_ms_clock();
        if (xpan_hud_force) { xpan_hud_force=0; last_sig=~sig; }
        if (sig==last_sig && now-last_ms<600 && !xpan_photo_debug_enabled()) return;
        last_sig=sig; last_ms=now;
    }
    if (!xpan_photo_hud_is_enabled()) bmp_idle_copy(0,0);
    else {
        xpan_hud_copy_band(0,y,XPAN_PHOTO_HUD_HEIGHT);
        if (last_hud_y>=0 && last_hud_y!=y) xpan_hud_copy_band(0,last_hud_y,XPAN_PHOTO_HUD_HEIGHT);
    }
    bmp_draw_to_idle(1);
    if (!xpan_photo_hud_is_enabled()) {
        /* Clear with true empty pixels; transparent black dims the EOS M image. */
        bmp_fill(COLOR_EMPTY,0,0,720,480);
        xpan_af_drawn.w=0;
        xpan_af_draw(y);
        xpan_grid_drawn.on=0;       /* the whole screen was just cleared */
        xpan_grid_draw(y);
#ifdef CONFIG_XPAN
        int top,bottom;
        xpan_loading_drawn.w=0;
        if (xpan_loading_photo_area(y,&top,&bottom)) xpan_loading_box(top,bottom,theme);
#endif
        bmp_draw_to_idle(0);
        bmp_idle_copy(1,0);
        last_hud_y=-1;
        return;
    }
    if (last_hud_y>=0 && last_hud_y!=y) {
        /* Erase the old anchor before drawing at the selected top/bottom edge. */
        bmp_fill(COLOR_EMPTY,0,last_hud_y,720,XPAN_PHOTO_HUD_HEIGHT);
    }
    int moved_from=(last_hud_y>=0 && last_hud_y!=y) ? last_hud_y:-1;
    last_hud_y=y;
    /* Share the panoramic layout; the EOS M transparent-black color softens
     * the panel when it overlays the full-height 3:2 Live View. */
    xpan_hud_background=panoramic ? COLOR_BLACK:COLOR_TRANSPARENT_BLACK;
    bmp_fill(xpan_hud_background,0,y,720,XPAN_PHOTO_HUD_HEIGHT);
    xpan_hud_logo(y,theme);

    char text[96];int focal=0,aperture=0;
    int eq=xpan_photo_get_lens_equivalents(lens_info.lens_exists,lens_info.focal_len,
        lens_info.aperture,xpan_photo_manual_focal_mm(),xpan_photo_manual_aperture_x10(),&focal,&aperture);
    if ((eq&3)==3) snprintf(text,sizeof(text),"%dmm  f/%d.%d",focal,aperture/10,aperture%10);
    else if (eq&1) snprintf(text,sizeof(text),"%dmm  f/--",focal);
    else if (eq&2) snprintf(text,sizeof(text),"--mm  f/%d.%d",aperture/10,aperture%10);
    else snprintf(text,sizeof(text),"--mm  f/--");
    /* Bound the equivalents before the mode, then compact the status group.
     * Optional clock/space cells retain their positions when hidden. */
    int equivalents_width=xpan_hud_text(152,y+13,240,30,FONT_MED_LARGE,theme,text);
    const char *mode=xpan_hud_photo_mode();
    int mode_font=bmp_string_width(FONT_MED_LARGE,mode)+1<=48 ? FONT_MED_LARGE:FONT_MONO_20;
    int mode_width=MIN(56,bmp_string_width(mode_font,mode)+1);
    /* Center in the visible gap after the fitted equivalents, not their full
     * reserved cell. Keep the clock edge fixed even when it is hidden. */
    int mode_center=(152+equivalents_width+478)/2;
    /* The font's capital ink sits3px above its cell center; align the frame
     * to that ink while retaining the mode's existing text baseline. */
    bmp_draw_rect(theme,mode_center-33,y+8,66,34);
    xpan_hud_text(mode_center-mode_width/2,y+13,mode_width,30,mode_font,COLOR_WHITE,mode);
    if (XPAN_HUD_SELECTED(XPAN_HUD_MODE)) xpan_hud_select_line(mode_center-33,y+44,66,theme);
    XPAN_HUD_HIT(XPAN_HUD_MODE,mode_center-40,y,80,60);
    if (xpan_photo_status_time_enabled()) {
        struct tm now;LoadCalendarFromRTC(&now);
        snprintf(text,sizeof(text),"%02d:%02d",now.tm_hour,now.tm_min);
        xpan_hud_text(478,y+15,70,28,FONT_MED,COLOR_WHITE,text);
    }
    if (xpan_photo_status_space_enabled()) {
        int space=get_free_space_32k(get_shooting_card());
        snprintf(text,sizeof(text),"%d.%dGB",space>>15,((space&32767)*10)>>15);
        xpan_hud_text(560,y+15,95,28,FONT_MED,COLOR_WHITE,text);
    }
    xpan_hud_battery(666,y+3);
#ifdef CONFIG_XPAN
    /* XPan > Debug info: key/touch diagnostics (off by default). */
    if (xpan_photo_debug_enabled()) {
        xpan_hud_controls_debug(text,sizeof(text));
        xpan_hud_text(478,y+47,232,14,FONT_SMALL,COLOR_GRAY(70),text);
        xpan_hud_controls_events(text,sizeof(text));
        xpan_hud_text(152,y+47,320,14,FONT_SMALL,COLOR_GRAY(70),text);
    }
#endif

    /* Equal-width bounded cells keep25600ISO,1/4000 and10000K readable. */
    static const char *labels[]={"ISO","APERTURE","SHUTTER","WB","FOCUS"};
    static const unsigned types[]={INFO_STRING_ISO,INFO_STRING_APERTURE,
        INFO_STRING_SHUTTER,INFO_STRING_WBMODE,INFO_STRING_FOCUSMODE};
    /* Cell i is HUD field i (ISO, APERTURE, SHUTTER, WB, FOCUS). */
    unsigned selected=0;
    for (unsigned i=0;i<COUNT(types);i++) {
        int x=10+i*144;
        int enabled=XPAN_HUD_ENABLED((int)i);
        xpan_hud_text(x,y+68,125,20,FONT_MONO_20,enabled ? theme:COLOR_GRAY(45),labels[i]);
        info_get_string(text,sizeof(text),types[i]);
        if (i==0) {
            unsigned iso=lens_info.iso ? lens_info.iso:lens_info.iso_auto;
            if (iso) snprintf(text,sizeof(text),"%s%d",lens_info.raw_iso ? "":"A",iso);
            else snprintf(text,sizeof(text),"AUTO");
        }
        /* Automatic value (aperture in Tv, shutter in Av): Canon publishes it
         * only while metering (half-press), so show a greyed "-". */
        if ((i==1 && shooting_mode==SHOOTMODE_TV) || (i==2 && shooting_mode==SHOOTMODE_AV))
            snprintf(text,sizeof(text),"-");
        /* Canon's nominal shutter label (1/50), as in the ML menu, not the
         * exact APEX reciprocal (1/48). */
        if (i==2 && lens_info.raw_shutter && strcmp(text,"-") && !is_bulb_mode())
            snprintf(text,sizeof(text),"%s",lens_format_shutter(lens_info.raw_shutter));
        if (!text[0]) snprintf(text,sizeof(text),"--");
        int value_width=xpan_hud_text(x,y+90,125,44,FONT_LARGE,enabled ? COLOR_WHITE:COLOR_GRAY(45),text);
        if (XPAN_HUD_SELECTED((int)i)) xpan_hud_select_line(x,y+130,value_width,theme);
        XPAN_HUD_HIT((int)i,x-8,y+62,144,74);
        /* Trim10px from the separator's top; retain its previous bottom. */
        if (i) bmp_fill(COLOR_GRAY(45),x-10,y+70,1,58);
    }
    bmp_fill(COLOR_GRAY(40),10,y+135,700,1);
    xpan_hud_text(10,y+144,40,17,FONT_SMALL,theme,"DRIVE");
    int field_width=xpan_hud_text(56,y+141,98,23,FONT_MONO_20,COLOR_WHITE,xpan_hud_drive());
    if (XPAN_HUD_SELECTED(XPAN_HUD_DRIVE)) xpan_hud_select_line(56,y+164,field_width,theme);
    XPAN_HUD_HIT(XPAN_HUD_DRIVE,0,y+136,156,36);
    xpan_hud_text(162,y+144,40,17,FONT_SMALL,theme,"METER");
    field_width=xpan_hud_text(208,y+141,115,23,FONT_MONO_20,COLOR_WHITE,xpan_hud_metering());
    if (XPAN_HUD_SELECTED(XPAN_HUD_METER)) xpan_hud_select_line(208,y+164,field_width,theme);
    XPAN_HUD_HIT(XPAN_HUD_METER,156,y+136,172,36);
    xpan_hud_text(334,y+144,24,17,FONT_SMALL,theme,"FMT");
    if (xpan_pic_quality_valid!=1) snprintf(text,sizeof(text),"WAIT");
    else if ((pic_quality&0x60000)==0x60000)
        snprintf(text,sizeof(text),"%s",(pic_quality>>24)==4 ? "RAW":"RAW+JPG F");
    else snprintf(text,sizeof(text),"JPG L %s",(pic_quality>>24)==2 ? "N":"F");
    field_width=xpan_hud_text(364,y+141,180,23,FONT_MONO_20,COLOR_WHITE,text);
    if (XPAN_HUD_SELECTED(XPAN_HUD_FORMAT)) xpan_hud_select_line(364,y+164,field_width,theme);
    XPAN_HUD_HIT(XPAN_HUD_FORMAT,328,y+136,152,36);
#ifdef CONFIG_XPAN
    if (xpan_film_grain_enabled()) xpan_hud_grain_icon(364+field_width+10,y+145);
#endif

    char state[24];
#ifdef CONFIG_XPAN
    /* The name follows the selection at once, so the next LUT can be picked
     * without waiting; "Loading..." goes in the middle of the picture. */
    xpan_native_look_hud_label(text,sizeof(text),state,sizeof(state));
#else
    snprintf(text,sizeof(text),"CANON");state[0]=0;
#endif
    /* A two-line40px column centers LUT/strength beside the28px name.
     * Status text no longer occupies the space next to the -3EV tick. */
    int label_width=bmp_string_width(FONT_SMALL,"LUT");
    int strength_width=MIN(40,bmp_string_width(FONT_SMALL,state));
    xpan_hud_text(30-label_width/2,y+178,label_width,12,FONT_SMALL,theme,"LUT");
    if (strength_width) xpan_hud_text(30-strength_width/2,y+194,strength_width,12,FONT_SMALL,theme,state);
    field_width=xpan_hud_text(62,y+178,415,28,FONT_MED,COLOR_WHITE,text);
    if (XPAN_HUD_SELECTED(XPAN_HUD_LUT)) xpan_hud_select_line(62,y+206,field_width,theme);
    XPAN_HUD_HIT(XPAN_HUD_LUT,0,y+172,480,XPAN_PHOTO_HUD_HEIGHT-172);
#ifdef CONFIG_XPAN
    xpan_hud_hit_valid=1;
#endif
    xpan_hud_exposure_scale(y,theme,selected);
    bmp_draw_to_idle(0);
    xpan_hud_copy_band(1,y,XPAN_PHOTO_HUD_HEIGHT);
    if (moved_from>=0) xpan_hud_copy_band(1,moved_from,XPAN_PHOTO_HUD_HEIGHT);
#ifdef CONFIG_XPAN
    /* Erased only when it ends or moves (no flicker), and before the AF
     * frame and grid so they repair what it covered. */
    int loading_top=0,loading_bottom=0;
    int loading=xpan_loading_photo_area(y,&loading_top,&loading_bottom);
    if (xpan_loading_drawn.w && (!loading ||
        xpan_loading_drawn.y!=(loading_top+loading_bottom)/2-XPAN_LOADING_H/2))
    { xpan_loading_erase(); xpan_grid_drawn.on=0; }
#endif
    xpan_af_draw(y);            /* image area: drawn straight to the screen */
    xpan_grid_draw(y);          /* after the AF erase, which can cut the lines */
#ifdef CONFIG_XPAN
    if (loading) xpan_loading_box(loading_top,loading_bottom,theme);   /* on top */
#endif
}
