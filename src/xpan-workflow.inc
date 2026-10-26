/* Included once by xpan-native-look.c: one idle worker owns Cube files,
 * profile requests and installation. Menu callbacks never close ML's menu,
 * perform SD I/O or allocate a second background task. */
static volatile unsigned workflow_request=1;
static volatile int cube_file_delta,cube_refresh_requested;
/* Startup/phase feedback is RAM-only: it works even if SD logging blocks.
 * State1 reserves the single worker before task_create can schedule it. */
static volatile unsigned workflow_worker_state,workflow_task_result;
static volatile int workflow_worker_retry;
static const char *volatile workflow_phase="Worker not started";
static volatile uint32_t workflow_phase_since;
static void xpan_workflow_task(void *unused);

static void workflow_set_phase(const char *phase)
{
    if (workflow_phase!=phase) {
        workflow_phase_since=get_ms_clock(); workflow_phase=phase;
    }
}
void xpan_native_look_start_worker(void)
{
    if (ml_shutdown_requested || (workflow_worker_state &&
        !(workflow_worker_state==3 && workflow_worker_retry))) return;
    uint32_t old=cli();
    workflow_worker_state=1; workflow_worker_retry=0;
    sei(old);
    workflow_set_phase("Starting worker");
    unsigned result=task_create("xpan_workflow",0x1a,0x8000,xpan_workflow_task,0);
    workflow_task_result=result;
    /* EOS M ROM7048 returns an odd error (3 or5), or an even task handle.
     * Never retry a successfully created but blocked task: that would race. */
    if (!result || (result&1)) {
        workflow_worker_state=3; workflow_set_phase("Worker start failed");
    }
}
static int cube_reload_requested;
static int jpeg_installed_automatic,table_storage_ready;
static uint8_t alo_payload[3];
static volatile int alo_payload_valid;
static volatile unsigned canon_colour_space;
static uint32_t neutral_payload[6];
static volatile int neutral_payload_valid;

PROP_HANDLER(PROP_ALO)
{
    /* ROM FF1B2104 writes exactly3 bytes: active mode, selected mode,
     * disable-in-M flag. Preserve that final byte when forcing Off. */
    if (len==3) { memcpy(alo_payload,buf,3); alo_payload_valid=1; }
}
PROP_HANDLER(PROP_XPAN_COLOR_SPACE)
{
    if (len==4) canon_colour_space=buf[0];
}
PROP_HANDLER(PROP_PICSTYLE_SETTINGS_NEUTRAL)
{
    /* EOS M uses the24-byte four-slider layout. Preserve its unused tail. */
    if (len==sizeof(neutral_payload))
    { memcpy(neutral_payload,buf,sizeof(neutral_payload)); neutral_payload_valid=1; }
}

static struct {
    int saved,neutral_saved,neutral_restored;
    uint32_t style,colour;
    uint8_t alo[3];
    int htp,sharpness,contrast,saturation,tone;
} previous_base;

static int neutral_parameters(int sharpness,int contrast,int saturation,int tone)
{
    if (!neutral_payload_valid) return 0;
    if ((int)neutral_payload[0]==contrast && (int)neutral_payload[1]==sharpness &&
        (int)neutral_payload[2]==saturation && (int)neutral_payload[3]==tone) return 1;
    uint32_t requested[6]; memcpy(requested,neutral_payload,sizeof(requested));
    requested[0]=contrast; requested[1]=sharpness; requested[2]=saturation; requested[3]=tone;
    prop_request_change(PROP_PICSTYLE_SETTINGS_NEUTRAL,requested,sizeof(requested));
    return 0; /* Only Canon's subsequent property event confirms the change. */
}
/* Sharpness (slot 1) does not change colour: XPan never forces or restores it,
 * so the user's Neutral sharpness stays in effect. Only contrast, saturation and
 * colour tone are zeroed for the XPan base. */
static int workflow_base_matches(void)
{
    return neutral_payload_valid && alo_payload_valid && lens_info.raw_picstyle==0x84 &&
        !(neutral_payload[0]|neutral_payload[2]|neutral_payload[3]) &&
        alo_payload[0]==ALO_OFF && alo_payload[1]==ALO_OFF && !get_htp() && canon_colour_space==1;
}

static int workflow_force_base(void)
{
    if (!alo_payload_valid || !canon_colour_space || !lens_info.raw_picstyle) return 0;
    if (!previous_base.saved)
    {
        previous_base.style=lens_info.raw_picstyle;
        previous_base.colour=canon_colour_space; previous_base.htp=get_htp();
        memcpy(previous_base.alo,alo_payload,3); previous_base.saved=1;
    }
    previous_base.neutral_restored=0;
    if (lens_info.raw_picstyle!=0x84)
    {
        uint32_t neutral=0x84;
        prop_request_change(PROP_PICTURE_STYLE,&neutral,4);
        return 0; /* Wait for Canon's style readback before editing its sliders. */
    }
    if (!previous_base.neutral_saved)
    {
        if (!neutral_payload_valid) return 0;
        previous_base.sharpness=neutral_payload[1]; previous_base.contrast=neutral_payload[0];
        previous_base.saturation=neutral_payload[2]; previous_base.tone=neutral_payload[3];
        previous_base.neutral_saved=1;
    }
    int ready=neutral_parameters(neutral_payload[1],0,0,0); /* keep user sharpness */
    if (alo_payload[0]!=ALO_OFF || alo_payload[1]!=ALO_OFF)
    {
        uint8_t off[3]={ALO_OFF,ALO_OFF,alo_payload[2]};
        prop_request_change(PROP_ALO,off,3); ready=0;
    }
    if (get_htp()) { set_htp(0); ready=0; }
    if (canon_colour_space!=1)
    {
        uint32_t srgb=1;
        prop_request_change(PROP_XPAN_COLOR_SPACE,&srgb,4); ready=0;
    }
    return ready;
}

static void workflow_restore_base(void)
{
    if (!previous_base.saved) return;
    /* Restore Neutral's own sliders before returning to the previous style.
     * This also preserves a user's customized Neutral definition across OFF. */
    if (previous_base.neutral_saved && !previous_base.neutral_restored)
    {
        if (lens_info.raw_picstyle!=0x84)
        {
            uint32_t neutral=0x84; prop_request_change(PROP_PICTURE_STYLE,&neutral,4); return;
        }
        if (!neutral_parameters(neutral_payload[1],previous_base.contrast, /* keep current sharpness */
                previous_base.saturation,previous_base.tone)) return;
        previous_base.neutral_restored=1;
    }
    int ready=1;
    if (memcmp(alo_payload,previous_base.alo,3))
    { prop_request_change(PROP_ALO,previous_base.alo,3); ready=0; }
    if (get_htp()!=previous_base.htp) { set_htp(previous_base.htp); ready=0; }
    if (canon_colour_space!=previous_base.colour)
    { prop_request_change(PROP_XPAN_COLOR_SPACE,&previous_base.colour,4); ready=0; }
    if (lens_info.raw_picstyle!=previous_base.style)
    { prop_request_change(PROP_PICTURE_STYLE,&previous_base.style,4); ready=0; }
    if (ready) memset(&previous_base,0,sizeof(previous_base));
}

const char *xpan_native_look_status(void)
{
    return workflow_worker_state!=2 ? workflow_phase:look_status;
}
int xpan_native_look_adjusting(void) { return look_busy; }

void xpan_native_look_hud_label(char *name, unsigned name_size, char *state, unsigned state_size)
{
    /* Copy RAM-only state; the LCD drawing task must never touch SD files. */
    const char *label=selected_look==3 ? xpan_native_cube_filename():"LUT OFF";
    snprintf(name,name_size,"%s",label);
    unsigned n=strlen(name);
    if (selected_look==3 && n>=5 && name[n-5]=='.') name[n-5]=0;
    /* Canon's formatter consumes an argument even for literal%%. Copy the
     * percent as a%s argument instead. The HUD sub-label is strength only;
     * loading/LCD status remains in the XPan menu. */
    if (selected_look==3) snprintf(state,state_size,"%d%s",COERCE(cube_strength,0,100),"%");
    else snprintf(state,state_size,"--");
}

static int workflow_menu_can_change(void)
{
    return !lens_info.job_state && !get_halfshutter_pressed() && look_state!=2;
}
static void workflow_pause_preview(void)
{
    xpan_live_native_pause();
}
static const char *workflow_preview_status(void)
{
    return xpan_live_native_status();
}
static void workflow_changed(void)
{
    /* Abort a stale table build and publish only the final queued selection. */
    automatic_capture_ready=0; look_cancel=1; workflow_request++;
    workflow_pause_preview();
    look_status="Preparing selected look";
}
void xpan_native_look_mode_changed(void) { workflow_changed(); }
static void workflow_select_pending_cube(struct card_info *card)
{
    uint32_t old=cli();
    int step=cube_file_delta,refresh=cube_refresh_requested;
    cube_file_delta=0; cube_refresh_requested=0;
    sei(old);
    if (refresh) cube_reload_requested=1; /* Defer RGB invalidation until idle. */
    if (refresh) xpan_native_cube_select(card,0,1);
    while (step)
    {
        int direction=step<0 ? -1:1;
        xpan_native_cube_select(card,direction,0); step-=direction;
    }
}
static void workflow_queue_cube_change(void)
{
    /* A list request is safe to queue even during capture. Keep the current
     * capture configuration intact until the worker reaches its idle point. */
    look_cancel=1; workflow_request++;
    if (workflow_menu_can_change()) {
        automatic_capture_ready=0; workflow_pause_preview();
    }
    look_status="Cube request queued";
}
/* Why the creative looks cannot apply with the current Canon settings.
 * The FOD hook only grades the Neutral style; HTP and ALO change Canon's
 * tone base (Test64) so the Cube and the headroom base would not match. */
static const char *workflow_look_blocker(void)
{
    if (lens_info.raw_picstyle!=0x84) return "Set Canon Picture Style to Neutral.";
    if (get_htp()) return "Turn Highlight Tone Priority OFF.";
    if (get_alo()!=ALO_OFF) return "Turn Auto Lighting Optimizer OFF.";
    return NULL;
}
/* Cube-only settings are greyed out and locked outside Custom .cube. */
static int workflow_cube_only(struct menu_entry *entry,struct menu_display_info *info)
{
    (void)entry;
    if (selected_look!=3) { MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Only with Custom LUT ON."); return 0; }
    const char *blocker=workflow_look_blocker();
    if (blocker) { MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"%s",blocker); return 0; }
    return 1;
}
void xpan_menu_mode_visibility(void);   /* cropmarks.c */
/* Values removed from the menu (example looks 1/2, Calibration base 2) are
 * migrated before the rows that index their choices are drawn. */
void xpan_native_look_menu_sanitize(void)
{
    if (selected_look!=0 && selected_look!=3) selected_look=0;
    if (headroom_stops!=0 && headroom_stops!=1) headroom_stops=1;
    if (headroom_ev!=1 && headroom_ev!=2) headroom_ev=1;
}
/* Custom LUT ON/OFF. Internally 3 = Custom .cube, 0 = Canon passthrough;
 * the old example looks (1 Warm tint, 2 Matte mono) are migrated to OFF. */
static MENU_UPDATE_FUNC(custom_lut_display)
{
    xpan_menu_mode_visibility();
    if (selected_look!=0 && selected_look!=3) selected_look=0;
    MENU_SET_VALUE("%s",selected_look==3 ? "ON":"OFF");
    MENU_SET_ENABLED(selected_look==3);
    const char *blocker=selected_look==3 ? workflow_look_blocker():NULL;
    if (blocker) MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"LUT inactive: %s",blocker);
}
static MENU_SELECT_FUNC(request_custom_lut)
{
    (void)delta;
    if (!workflow_menu_can_change()) return;
    selected_look=selected_look==3 ? 0:3;
    workflow_changed();
}
static MENU_SELECT_FUNC(request_cube_file)
{
    /* Choosing a Cube is an explicit request to use that file. Selecting it
     * must not silently leave Custom LUT OFF. */
    selected_look=3;
    uint32_t old=cli(); cube_file_delta=COERCE(cube_file_delta+(delta<0 ? -1:1),-16,16); sei(old);
    workflow_queue_cube_change();
}
static volatile int hud_cube_pending;
static volatile int hud_cube_since;
/* Live View HUD: same request as the Cube file menu item. */
void xpan_native_look_step_cube(int delta)
{
    if (!workflow_menu_can_change()) return;
    selected_look=3;
    uint32_t old=cli(); cube_file_delta=COERCE(cube_file_delta+(delta<0 ? -1:1),-16,16); sei(old);
    hud_cube_since=get_ms_clock(); hud_cube_pending=1;
    workflow_queue_cube_change();
}
/* True while a queued Cube/look change has not finished preparing. The short
 * grace covers the gap before the worker picks the request up. */
int xpan_native_look_loading(void)
{
    if (cube_file_delta || cube_refresh_requested || look_busy) return 1;
    if (hud_cube_pending && get_ms_clock()-hud_cube_since<1500) return 1;
    hud_cube_pending=0;
    return 0;
}
static MENU_SELECT_FUNC(request_cube_refresh)
{
    /* A failed allocation can be retried; an existing worker is never cloned. */
    workflow_worker_retry=1;
    cube_refresh_requested=1; workflow_queue_cube_change();
}
static MENU_SELECT_FUNC(request_cube_strength)
{
    if (!workflow_menu_can_change() || selected_look!=3) return;
    cube_strength=COERCE(cube_strength+(delta<0 ? -1:1),0,100);
    workflow_changed();
}
/* Photo base file follows XPan > Highlight headroom. */
static int workflow_base_load(struct card_info *card)
{
    xpan_native_base_set_photo_file(headroom_ev==2 ? XPAN_BASE_FILE_2EV:XPAN_BASE_FILE);
    return xpan_native_base_load(card,&look_cancel);
}
static MENU_SELECT_FUNC(request_headroom_ev)
{
    if (!workflow_menu_can_change() || selected_look!=3 || headroom_stops!=1) return;
    headroom_ev=headroom_ev==2 ? 1:2;
    cube_reload_requested=1; workflow_changed();
}
static MENU_UPDATE_FUNC(headroom_ev_display)
{
    MENU_SET_VALUE("%s",headroom_ev==2 ? "+2 EV":"+1 EV");
    if (workflow_cube_only(entry,info) && headroom_stops!=1)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Only for Tone base = Cineon log.");
}
static MENU_SELECT_FUNC(request_headroom)
{
    if (!workflow_menu_can_change() || selected_look!=3) return;
    (void)delta;
    headroom_stops=headroom_stops==1 ? 0:1;   /* Calibration (2) is not offered */
    cube_reload_requested=1; workflow_changed();
}
static MENU_UPDATE_FUNC(headroom_display)
{
    if (headroom_stops!=0 && headroom_stops!=1) headroom_stops=1;   /* old Calibration */
    MENU_SET_VALUE("%s",headroom_stops==1 ? "Cineon log":"Canon Neutral");
    workflow_cube_only(entry,info);
    static const char *const why[]={"applied","Neutral","HTP/ALO on","WB block check","table prepare","look not active","base file missing","tone row check"};
    int code=COERCE(headroom_code,0,7);
    MENU_SET_HELP("Last: %s (mode %d, wb %d, row %d/%d, flags %x, guard %x, look %d). %s",
        why[code],headroom_last,headroom_obwb_code,headroom_angel_code,xpan_native_angel_diff,format_flags,last_guard_mask,(int)look_reason,
        headroom_stops==1 ? xpan_native_base_status():"");
    if (headroom_stops==1 && !xpan_native_base_table() && selected_look==3)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"%s",xpan_native_base_status());
}
static MENU_SELECT_FUNC(request_live_preview)
{
    if (!workflow_menu_can_change() || selected_look!=3) return;
    live_preview_enabled=!live_preview_enabled; workflow_changed();
}
static MENU_UPDATE_FUNC(cube_file_display)
{
    MENU_SET_VALUE("%s",xpan_native_cube_filename());
    /* Status goes to the help line, never next to the value. */
    MENU_SET_HELP("%s%s",
        (cube_refresh_requested || cube_file_delta) ? "Queued. ":look_busy ? "Loading. ":"",
        xpan_native_cube_status());
    if (xpan_native_cube_list_state()==XPAN_CUBE_LIST_UNAVAILABLE ||
        xpan_native_cube_list_state()==XPAN_CUBE_LIST_EMPTY)
        MENU_SET_WARNING(MENU_WARN_ADVICE,"%s",xpan_native_cube_status());
}
static MENU_UPDATE_FUNC(cube_refresh_display)
{
    if (workflow_worker_state==3) MENU_SET_VALUE("Start failed (%x)",workflow_task_result);
    else if (workflow_worker_state!=2) MENU_SET_VALUE("%s",workflow_phase);
    else if (cube_refresh_requested && get_ms_clock()-workflow_phase_since>5000)
        MENU_SET_VALUE("Waiting: %s",workflow_phase);
    else if (cube_refresh_requested) MENU_SET_VALUE("Queued");
    else if (xpan_native_cube_list_state()==XPAN_CUBE_LIST_SCANNING) MENU_SET_VALUE("Scanning...");
    else if (xpan_native_cube_list_state()==XPAN_CUBE_LIST_READY)
        MENU_SET_VALUE("%d files",xpan_native_cube_file_count());
    else MENU_SET_VALUE("%s",xpan_native_cube_filename());
    MENU_SET_HELP("Worker: %s; task %x; job %d. %s",workflow_phase,
        workflow_task_result,lens_info.job_state,xpan_native_cube_status());
}
static MENU_UPDATE_FUNC(cube_strength_display)
{
    MENU_SET_VALUE("%d%%",cube_strength);
    workflow_cube_only(entry,info);
}
static MENU_UPDATE_FUNC(live_preview_display)
{
    MENU_SET_VALUE("%s",live_preview_enabled ? "ON":"OFF");
    MENU_SET_HELP("Display-only preview. %s",workflow_preview_status());
    if (workflow_cube_only(entry,info) && is_movie_mode())
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Photo mode only; use Movie LUT for video.");
}

static unsigned movie_prepared_revision,movie_prepared_strength;
static const struct xpan_lut *movie_prepared_base;
static const char *movie_lut_status="Movie LUT OFF";
/* Movie LUT: force the same Neutral base as photos (the Cube expects it),
 * install the two Live View table hooks, and prepare the table once per Cube,
 * strength or Tone base change. Runs only in the idle worker. */
static void workflow_movie_lut(struct card_info *card)
{
    if (!card || !lv || PLAY_OR_QR_MODE) { movie_lut_status="Movie LUT waiting for Live View"; return; }
    workflow_force_base();
    xpan_live_color_probe_poll(card);
    if (selected_look!=3) { movie_lut_ready=0; movie_lut_status="Movie LUT: turn Custom LUT ON"; return; }
    if (cube_reload_requested) { xpan_native_cube_invalidate(); cube_reload_requested=0; movie_lut_ready=0; }
    look_cancel=0;
    if (!xpan_native_cube_load(card,&look_cancel)) { movie_lut_ready=0; movie_lut_status=xpan_native_cube_status(); return; }
    if (headroom_stops==1) workflow_base_load(card);
    unsigned strength=(unsigned)COERCE(cube_strength,0,100)*65535u/100;
    const struct xpan_lut *base=live_base();
    if (!movie_lut_ready || movie_prepared_revision!=xpan_native_cube_revision() ||
        movie_prepared_strength!=strength || movie_prepared_base!=base)
    {
        movie_lut_ready=0;
        if (!xpan_live_native_prepare(xpan_native_cube_table(),xpan_native_cube_revision(),
                strength,xpan_native_cube_identity(),base,&look_cancel))
        { movie_lut_status="Movie LUT table failed"; return; }
        movie_prepared_revision=xpan_native_cube_revision();
        movie_prepared_strength=strength;movie_prepared_base=base;movie_lut_ready=1;
    }
    movie_lut_status=lens_info.raw_picstyle==0x84 ? "Movie LUT ready":"Movie LUT: setting Neutral";
}
static MENU_SELECT_FUNC(request_movie_lut)
{
    movie_lut_enabled=!movie_lut_enabled;
    if (!movie_lut_enabled) { movie_lut_ready=0; xpan_live_native_pause(); }
}
static MENU_UPDATE_FUNC(movie_lut_display)
{
    char diag[128];xpan_live_native_movie_diag(diag,sizeof(diag));
    MENU_SET_VALUE("%s",movie_lut_enabled ? "ON":"OFF");
    MENU_SET_HELP("%s. %s",movie_lut_enabled ? movie_lut_status:"Movie mode only",diag);
    if (selected_look!=3) MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Only with Custom LUT ON.");
    else if (!is_movie_mode()) MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Applies in movie mode only.");
}

/* These entries are appended to the existing XPan submenu, never a second
 * parent. EM_SHOW_LIVEVIEW uses ML's ISO-style transparent adjustment view. */
static struct menu_entry workflow_menu[]={
    { .name="Custom LUT", .icon_type=IT_BOOL,
      .select=request_custom_lut, .update=custom_lut_display,
      .help="ON: apply the selected Cube file on Canon Neutral + Tone base. OFF: Canon passthrough." },
    { .name="Cube file", .select=request_cube_file, .update=cube_file_display,
      .icon_type=IT_DICE, .edit_mode=EM_SHOW_LIVEVIEW,
      .help="Cycle files and select Custom .cube. Live LUT preview enables the LCD effect." },
    { .name="Cube strength", .priv=&cube_strength, .max=100,
      .select=request_cube_strength, .update=cube_strength_display,
      .edit_mode=EM_SHOW_LIVEVIEW, .unit=UNIT_PERCENT,
      .help="Blend the custom Cube0..100 percent. Zero preserves Canon colour." },
    { .name="Tone base", .priv=&headroom_stops, .max=1,
      .select=request_headroom, .update=headroom_display,
      .choices=CHOICES("Canon Neutral","Cineon log"),
      .help="Canon Neutral: Cube on Canon's render. Cineon log: extra highlight range, Cube gets Cineon log." },
    { .name="Highlight headroom", .priv=&headroom_ev, .min=1, .max=2,
      .select=request_headroom_ev, .update=headroom_ev_display,
      .choices=CHOICES("+1 EV","+2 EV"),
      .help="+1 EV: smoother shadows. +2 EV: one more stop of highlight colour, coarser deep shadows." },
    { .name="Live LUT preview", .priv=&live_preview_enabled, .max=1,
      .select=request_live_preview, .update=live_preview_display,
      .help="Show the selected custom Cube on the LCD. Saved photos keep the look either way." },
    /* Movie LUT has no switch any more: in movie mode it follows Custom LUT. */
    { .name="Refresh Cube list", .select=request_cube_refresh, .update=cube_refresh_display,
      .help="Rescan Cube files and reload the selected file. The menu stays open." },
};
void xpan_native_look_add_menu(void)
{
    menu_add("XPan",workflow_menu,COUNT(workflow_menu));
#ifdef CONFIG_XPAN
    xpan_film_grain_add_menu();
#endif
}

static int workflow_install_hooks(struct card_info *card)
{
    /* Install at idle once. Callbacks gate subsequent photos by the saved
     * switch; ordinary menu adjustments never unpatch running Canon code. */
    if (!xpan_native_jpeg_playback_enable()) { look_status=xpan_native_jpeg_playback_status(); return 0; }
    if (!jpeg_installed_automatic)
    {
        xpan_native_jpeg_set_automatic();
        if (!xpan_native_jpeg_start(2)) { look_status=xpan_native_jpeg_status(); return 0; }
        jpeg_installed_automatic=1;
    }
    if (!table_storage_ready)
    {
        /* Automatic capture uses the actual Canon table. The old NLT fixture
         * was only for one-shot comparisons and is not a runtime dependency. */
        if (!look_allocation)
        {
            look_allocation=__mem_malloc(TABLE_BYTES*2+FRONT_PREFIX_BYTES+XPAN_OBWB_BYTES+XPAN_ANGEL_BYTES+63,MEM_DMA,__FILE__,__LINE__);
            if (look_allocation)
            {
                look_output=(uint32_t *)(((uintptr_t)look_allocation+63)&~(uintptr_t)63);
                look_actual=look_output+TABLE_WORDS;
                look_front=look_actual+TABLE_WORDS;
                look_obwb=look_front+FRONT_PREFIX_BYTES/4;
                look_angel=look_obwb+XPAN_OBWB_BYTES/4;
            }
        }
        if (!preview_allocation)
        {
            preview_allocation=__mem_malloc(TABLE_BYTES*2+FRONT_PREFIX_BYTES+XPAN_OBWB_BYTES+XPAN_ANGEL_BYTES+63,MEM_DMA,__FILE__,__LINE__);
            if (preview_allocation)
            {
                preview_output=(uint32_t *)(((uintptr_t)preview_allocation+63)&~(uintptr_t)63);
                preview_actual=preview_output+TABLE_WORDS;
                preview_front=preview_actual+TABLE_WORDS;
                preview_obwb=preview_front+FRONT_PREFIX_BYTES/4;
                preview_angel=preview_obwb+XPAN_OBWB_BYTES/4;
            }
        }
        if (!look_allocation || !preview_allocation)
        { look_status="Look resources unavailable"; return 0; }
        table_storage_ready=1;
    }
    if (!hook_installed)
    {
        if (*(volatile uint32_t *)0xff42d174!=0xe1d433b4 ||
            *(volatile uint32_t *)0xff42d178!=LOOK_INSTRUCTION ||
            *(volatile uint32_t *)0xff42d17c!=0xe25000ff ||
            *(volatile uint32_t *)0xff42d1a0!=0xe5941030 ||
            *(volatile uint32_t *)0xff42d204!=0xe8bd87ff ||
            *(volatile uint32_t *)0xff42d1b0!=0x1b2f76a2 ||
            *(volatile uint32_t *)0xff42d264!=0xc0f20000)
        { look_status="Unexpected Canon transfer map"; return 0; }
        patch_result=patch_hook_function(LOOK_HOOK,LOOK_INSTRUCTION,native_table_hook,"XPan automatic look");
        if (patch_result) { look_status="Look hook unavailable"; return 0; }
        hook_installed=1;
    }
    return 1;
}

/* Keep existing error paths intact while omitting automatic experiment logs. */
static int workflow_save_status(struct card_info *card) { (void)card; return 1; }

static void xpan_workflow_task(void *unused)
{
    (void)unused;
    /* Publish entry before sleeping or touching SD. Menu feedback must not
     * depend on a log write by the very task whose startup is in question. */
    workflow_worker_state=2;
    workflow_set_phase("Startup delay");
    /* Never perform the old manual experiments from early boot callbacks. */
    msleep(3500);
    unsigned prepared=0,base_check=0,failed_request=0,hook_retry=0;
    unsigned logged_request=0,logged_file=0xffffffffu,logged_trace_count=0,idle_since=0;
    unsigned logged_wheel=0xffffffffu;
    /* Preserve exposure-related preview exclusions even without a photo. */
    unsigned logged_lcd_revision=0;
    struct card_info *prepared_card=NULL,*catalogue_card=NULL;
    int was_enabled=-1,aspect_saved=0,prepared_in_lv=0;
    uint32_t previous_aspect=0;
    while (!ml_shutdown_requested)
    {
        msleep(100);
        workflow_set_phase("Checking Cube requests");
        struct card_info *card=get_ml_card();
        /* Directory browsing only changes names/indexes, not RGB storage.
         * Service it before idle, hook, aspect and Neutral readiness checks. */
        if (card && (catalogue_card!=card || cube_refresh_requested || cube_file_delta))
        {
            if (catalogue_card!=card) { cube_refresh_requested=1; automatic_capture_ready=0; }
            /* No startup log precedes this first useful operation. */
            workflow_set_phase("Scanning Cube folder");
            workflow_select_pending_cube(card);
            catalogue_card=card;
            workflow_set_phase("Refreshing Cube menu");
            if (gui_menu_shown()) menu_redraw();
        }
        if (!card) catalogue_card=NULL;
#ifdef CONFIG_XPAN
        /* The saved-image stage prepares only small assets, independently of
         * live-view LUT requests and without waiting for the one-second log idle. */
        xpan_film_grain_poll(card);
#endif
        if (!workflow_menu_can_change()) {
            idle_since=0;
            look_status="Waiting for camera idle";
            workflow_set_phase(look_status);
            continue;
        }
        if (!idle_since) idle_since=get_ms_clock();
        int enabled=xpan_photo_enabled();
        workflow_set_phase("Checking photo hooks");
        /* Exact-size playback support also serves previously saved XPan files
         * while the shooting mode is OFF. Install only outside decoder/review. */
        if (lv && !PLAY_OR_QR_MODE && !is_movie_mode()) xpan_native_jpeg_playback_enable();
        if (enabled!=was_enabled)
        {
            was_enabled=enabled; workflow_changed();
            if (!enabled) look_status="XPan OFF";
        }
        if (is_movie_mode() && (!xpan_movie_enabled() || selected_look!=3) && movie_lut_ready)
        { movie_lut_ready=0; xpan_live_native_pause(); }   /* XPan OFF / LUT OFF in movie */
        if (is_movie_mode() && selected_look==3 && xpan_movie_enabled())
        {
            /* Movie LUT experiment: no photo capture; only the LV table. */
            automatic_capture_ready=0; look_busy=0;
            workflow_set_phase("Movie LUT");
            workflow_movie_lut(card);
            continue;
        }
        if (!enabled || is_movie_mode())
        {
            automatic_capture_ready=0; look_busy=0;
            look_status=enabled ? "XPan is photo mode only":"XPan OFF";
            workflow_set_phase("Restoring Canon base");
            workflow_restore_base();
            if (!enabled && aspect_saved && xpan_image_aspect_ratio_valid==1)
            {
                if (xpan_image_aspect_ratio!=previous_aspect)
                    prop_request_change(PROP_IMAGE_ASPECT_RATIO,&previous_aspect,4);
                else aspect_saved=0;
            }
            continue;
        }
        if (!card) { automatic_capture_ready=0; look_status="No ML card"; continue; }
        /* Small observations are saved by this existing idle worker only. */
        if (get_ms_clock()-idle_since>=1000) xpan_live_color_probe_poll(card);
        if (!aspect_saved && xpan_image_aspect_ratio_valid==1)
        { previous_aspect=xpan_image_aspect_ratio; aspect_saved=1; }
        xpan_photo_keep_3_2_aspect();
        workflow_set_phase("Applying capture quality");
        if (!xpan_photo_ensure_capture_quality())
        {
            /* Do not advertise readiness before Canon accepts JPEG/RAW+JPEG. */
            automatic_capture_ready=0; look_status="Applying supported capture format";
            workflow_save_status(card); continue;
        }
        if (!jpeg_installed_automatic || !hook_installed)
        {
            if (!xpan_photo_ui_active() || PLAY_OR_QR_MODE) continue;
            if (hook_retry && get_ms_clock()-hook_retry<5000) continue;
            hook_retry=get_ms_clock();
            workflow_set_phase("Installing photo hooks");
            if (!workflow_install_hooks(card))
            { automatic_capture_ready=0; workflow_save_status(card); continue; }
        }
        if (xpan_image_aspect_ratio_valid!=1 || xpan_image_aspect_ratio!=0)
        { automatic_capture_ready=0; look_status="Applying Canon 3:2 aspect";
          workflow_save_status(card); continue; }
        /* Reassert required settings at idle, including changes made in Canon's
         * menu. Crop hooks remain independent of colour-base convergence. */
        if (selected_look!=0 && selected_look!=3) selected_look=0;   /* old example looks */
        if (headroom_stops!=0 && headroom_stops!=1) headroom_stops=1; /* old Calibration */
        int needs_base=selected_look!=0;
        workflow_set_phase("Applying Canon base");
        if (get_ms_clock()-base_check>=500)
        {
            base_check=get_ms_clock();
            if (needs_base && !workflow_force_base())
            { automatic_capture_ready=0; look_status="Applying Canon Neutral base";
              workflow_save_status(card); continue; }
            if (!needs_base) workflow_restore_base();
        }
        if (needs_base && !workflow_base_matches())
        { automatic_capture_ready=0; look_status="Applying Canon Neutral base";
          workflow_save_status(card); continue; }
        if (selected_look==3 && live_preview_enabled && !prepared_in_lv && xpan_photo_ui_active())
        { workflow_request++; prepared_in_lv=1; }
        if (prepared!=workflow_request || prepared_card!=card)
        {
            /* A malformed/missing Cube is retried only on a new selection or
             * Refresh, rather than reopening the SD file every100ms. */
            if (failed_request==workflow_request && prepared_card==card) continue;
            automatic_capture_ready=0; look_busy=1;
            unsigned request=workflow_request;
            look_cancel=0;
            workflow_set_phase("Preparing selected Cube");
            workflow_pause_preview();
            if (cube_reload_requested) {
                /* Only this idle stage may replace the capture table. A list
                 * refresh is no longer able to invalidate it during a photo. */
                xpan_native_cube_invalidate(); cube_reload_requested=0;
            }
            int ok=selected_look!=3 || xpan_native_cube_load(card,&look_cancel);
            /* Missing base is reported in the menu; capture then stays Neutral. */
            if (ok && selected_look==3 && headroom_stops==1) workflow_base_load(card);
            if (ok && selected_look==3) {
                workflow_set_phase("Building LCD look"); prepare_live_preview(card);
            }
            prepared_in_lv=xpan_photo_ui_active();
            look_busy=0;
            if (!ok || look_cancel || request!=workflow_request || !xpan_photo_enabled())
            {
                look_status=ok ? "Preparing selected look":xpan_native_cube_status();
                if (!ok && !look_cancel && request==workflow_request)
                { failed_request=request; prepared_card=card; workflow_save_status(card); }
                continue;
            }
            if (!workflow_menu_can_change()) continue;
            armed_look=selected_look;
            armed_cube_strength=(unsigned)COERCE(cube_strength,0,100)*65535u/100;
            armed_headroom=headroom_stops==0 ? 0u:1u;   /* Calibration not offered */
            armed_headroom_ev=headroom_ev==2 ? 2u:1u;
            automatic_look_file=automatic_look_folder=0xffffffffu;
            armed_shooting_card=get_shooting_card(); look_state=1;
            prepared=request; prepared_card=card;
            look_status="Ready; automatic crop and look";
            if (gui_menu_shown()) menu_redraw();
        }
        automatic_capture_ready=prepared==workflow_request && prepared_card==card;
        if (automatic_capture_ready) look_status="Ready; automatic crop and look";
        workflow_set_phase(look_status);
        struct card_info *shooting=get_shooting_card();
        unsigned file=shooting ? shooting->file_number:0;
        unsigned wheel=xpan_photo_wheel_state();
        unsigned lcd_revision=xpan_live_native_log_revision();
        if (get_ms_clock()-idle_since>=1000 &&
            (logged_request!=prepared || logged_file!=file || logged_trace_count!=capture_trace_count ||
             logged_wheel!=wheel
             || logged_lcd_revision!=lcd_revision
             ))
        {
            /* A late encoder callback may share the current file counter. */
            unsigned trace_count=capture_trace_count;
            if (workflow_save_status(card)) {
                logged_request=prepared; logged_file=file; logged_trace_count=trace_count;
                /* One latest-state write per selection change, rate-limited
                 * by workflow_save_status. Wheel rotation itself adds no I/O. */
                logged_wheel=wheel;
                logged_lcd_revision=lcd_revision;
            }
        }
    }
    automatic_capture_ready=0;
    workflow_set_phase("Worker stopped at shutdown"); workflow_worker_state=4;
}
/* Created explicitly by the existing LCD task, with its result checked. */
