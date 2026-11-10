/**
 * MagicLantern GuiMainTask override
 * This was previously camera-specific
 **/

/*
 * Copyright (C) 2009 Trammell Hudson <hudson+ml@osresearch.net>
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

#include <gui.h>

#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <config.h>
#include <consts.h>
#include <lens.h>
#include <config-defines.h>
#include <boot-hack.h>
#include <menu.h>

/**
 * Supported cameras [E] Means it's enabled
 * [E] 1100D: counter_0x0c <-> msg_queue_0x30
 * [E] 600D : counter_0x0c <-> msg_queue_0x30
 * [E] 60D  : counter_0x0c <-> msg_queue_0x30
 * [E] 650D : counter_0x0c <-> msg_queue_0x30
 * [E] EOSM : counter_0x0c <-> msg_queue_0x30
 * [E] 700D : counter_0x0c <-> msg_queue_0x30
 * [E] 100D : counter_0x0c <-> msg_queue_0x30
 * [E] 5D3  : counter_0x0c <-> msg_queue_0x30
 * [E] 6D   : counter_0x0c <-> msg_queue_0x30
 */

/**
 * Easy to support cameras
 * [E] 550D : counter_0x04 <-> msg_queue_0x38
 * [D] 7D   : counter_0x04 <-> msg_queue_0x38
 */

/**
 * Unsupported cameras for now
 * 5D2  : counter_0x04 <-> msg_queue_0x34
 * 50D  : counter_0x04 <-> msg_queue_0x34
 * 500D : counter_0x04 <-> msg_queue_0x34
 */

struct semaphore * gui_sem;

// return 0 if you want to block this event
static int handle_buttons(struct event * event)
{
    ASSERT(event->type == 0)

    if (event->type != 0) return 1; // only handle events with type=0 (buttons)
    if (handle_common_events_startup(event) == 0) return 0;
    extern int ml_started;
    if (!ml_started) return 1;

// SJE Want to log button presses?  Uncomment the following line
//    DryosDebugMsg(0,15,"event: %08x", event->param);
    if (handle_common_events_by_feature(event) == 0) return 0;

    return 1;
}

/** TODO: rename stuff in this structure, don't use camera names maybe?
 * Other possibility, move in-structure offset to platform dir and define
 * per camera. */
struct gui_main_struct {
  void *          obj;                     // off_0x00;
  uint32_t        counter_550d;
  uint32_t        off_0x08;
  uint32_t        counter;                 // off_0x0c;
  uint32_t        off_0x10;
  uint32_t        off_0x14;
  struct msg_queue *    msg_queue_m50;     // off_0x18;
  struct msg_queue *    msg_queue_eosr;    // off_0x1C;
  uint32_t        off_0x20;
  uint32_t        off_0x24;
  uint32_t        off_0x28;
  uint32_t        off_0x2c;
  struct msg_queue *    msg_queue;         // off_0x30;
  struct msg_queue *    off_0x34;          // off_0x34;
  struct msg_queue *    msg_queue_550d;    // off_0x38;
  uint32_t        off_0x3c;
};

extern struct gui_main_struct gui_main_struct;

void ml_gui_main_task()
{
    struct event * event = NULL;
    int index = 0;
    void* funcs[GMT_NFUNCS];
    memcpy(funcs, (void*)GMT_FUNCTABLE, 4*GMT_NFUNCS);

    gui_init_end(); // no params?

    while(1)
    {
        #if defined(CONFIG_550D) || defined(CONFIG_7D)
        msg_queue_receive(gui_main_struct.msg_queue_550d, &event, 0);
        gui_main_struct.counter_550d--;
        #elif defined(CONFIG_R) || defined(CONFIG_RP) || defined(CONFIG_850D) || defined (CONFIG_R5) || defined(CONFIG_SX70) || defined(CONFIG_M6II)
        msg_queue_receive(gui_main_struct.msg_queue_eosr, &event, 0);
        gui_main_struct.counter_550d--;
        #elif defined(CONFIG_M50) || defined(CONFIG_SX740)
        msg_queue_receive(gui_main_struct.msg_queue_m50, &event, 0);
        gui_main_struct.counter_550d--;
        #else
        msg_queue_receive(gui_main_struct.msg_queue, &event, 0);
        gui_main_struct.counter--;
        #endif

        if (event == NULL) {
            continue;
        }

#if defined(CONFIG_M50) && defined(CONFIG_XPAN)
        /* Boot log (XBOOT.LOG): every GUI event that arrives before ML has
         * started, as type<<16 | code (README-XPAN-M50.md, boot log 13). */
        {
            extern int ml_started;
            if (!ml_started)
            {
                extern void m50_boot_mark(int id, int value);
                m50_boot_mark(30, (event->type << 16) | (event->param & 0xFFFF));
                /* Canon's short start after the battery/card door closes:
                 * ML must not start (xpan-m50.c m50_wait_for_real_start). */
                int cmd = event->param & 0xFFFF;
                if (cmd == GMT_GUICMD_START_AS_CHECK ||
                    cmd == GMT_GUICMD_OPEN_SLOT_COVER ||
                    cmd == GMT_GUICMD_LOCK_OFF)
                {
                    extern volatile int m50_check_start;
                    m50_check_start = cmd;
                }
            }
        }
#endif

        index = event->type;

#if defined(CONFIG_M50) && defined(CONFIG_XPAN)
        /* M50: no ML or XPan handler sees a GUI event before ML has started;
         * every event goes straight to Canon (README-XPAN-M50.md, "Card swap
         * after power-off", fifth attempt). 2026-11-08: the switch-on after
         * a battery-door close hung inside Canon's init_task (LED solid),
         * which waits on this task; the handlers (XPan's M50 controls among
         * them) are not initialised yet. */
        extern int ml_started;
        int ml_handlers_on = ml_started;
#else
        int ml_handlers_on = 1;
#endif
        if (ml_handlers_on && !magic_is_off())
        {

            if (event->type == 0)
            {
                if (handle_buttons(event) == 0) { // ML button/event handler
                    continue;
                }
            }
            else
            {
                if (handle_other_events(event) == 0) {
                    continue;
                }
            }
        }

        if (IS_FAKE(event)) {
           event->arg = 0;      /* do not pass the "fake" flag to Canon code */
        }

        if (event->type == 0 && event->param < 0) {
            continue;           /* do not pass internal ML events to Canon code */
        }

        if ((index >= GMT_NFUNCS) || (index < 0)) {
            continue;
        }

        void(*f)(struct event *) = funcs[index];
        if (f != NULL)
            f(event);
    }
}

TASK_OVERRIDE( gui_main_task, ml_gui_main_task);
