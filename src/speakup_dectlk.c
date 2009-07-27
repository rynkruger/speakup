/*
 * originally written by: Kirk Reiser <kirk@braille.uwo.ca>
 * this version considerably modified by David Borowski, david575@rogers.com
 *
 * Copyright (C) 1998-99  Kirk Reiser.
 * Copyright (C) 2003 David Borowski.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * specificly written as a driver for the speakup screenreview
 * s not a general device driver.
 */
#include <linux/unistd.h>
#include <linux/proc_fs.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/kthread.h>
#include "speakup.h"
#include "spk_priv.h"
#include "serialio.h"

#define DRV_VERSION "2.14"
#define SYNTH_CLEAR 0x03
#define PROCSPEECH 0x0b
static volatile int xoff;
#define synth_full() (xoff)

static void do_catch_up(struct spk_synth *synth);
static void synth_flush(struct spk_synth *synth);
static void read_buff_add(u_char c);
static unsigned char get_index(void);

static int in_escape;
static int is_flushing;

static spinlock_t flush_lock;
static DECLARE_WAIT_QUEUE_HEAD(flush);

static struct var_t vars[] = {
	{ CAPS_START, .u.s = {"[:dv ap 200]" }},
	{ CAPS_STOP, .u.s = {"[:dv ap 100]" }},
	{ RATE, .u.n = {"[:ra %d]", 9, 0, 18, 150, 25, NULL }},
	{ PITCH, .u.n = {"[:dv ap %d]", 80, 0, 200, 20, 0, NULL }},
	{ VOL, .u.n = {"[:dv gv %d]", 13, 0, 14, 0, 5, NULL }},
	{ PUNCT, .u.n = {"[:pu %c]", 0, 0, 2, 0, 0, "nsa" }},
	{ VOICE, .u.n = {"[:n%c]", 0, 0, 9, 0, 0, "phfdburwkv" }},
	V_LAST_VAR
};

/*
 * These attributes will appear in /sys/accessibility/speakup/dectlk.
 */
static struct kobj_attribute caps_start_attribute =
	__ATTR(caps_start, USER_RW, spk_var_show, spk_var_store);
static struct kobj_attribute caps_stop_attribute =
	__ATTR(caps_stop, USER_RW, spk_var_show, spk_var_store);
static struct kobj_attribute pitch_attribute =
	__ATTR(pitch, USER_RW, spk_var_show, spk_var_store);
static struct kobj_attribute punct_attribute =
	__ATTR(punct, USER_RW, spk_var_show, spk_var_store);
static struct kobj_attribute rate_attribute =
	__ATTR(rate, USER_RW, spk_var_show, spk_var_store);
static struct kobj_attribute voice_attribute =
	__ATTR(voice, USER_RW, spk_var_show, spk_var_store);
static struct kobj_attribute vol_attribute =
	__ATTR(vol, USER_RW, spk_var_show, spk_var_store);

static struct kobj_attribute delay_time_attribute =
	__ATTR(delay_time, ROOT_W, spk_var_show, spk_var_store);
static struct kobj_attribute full_time_attribute =
	__ATTR(full_time, ROOT_W, spk_var_show, spk_var_store);
static struct kobj_attribute jiffy_delta_attribute =
	__ATTR(jiffy_delta, ROOT_W, spk_var_show, spk_var_store);
static struct kobj_attribute trigger_time_attribute =
	__ATTR(trigger_time, ROOT_W, spk_var_show, spk_var_store);

/*
 * Create a group of attributes so that we can create and destroy them all
 * at once.
 */
static struct attribute *synth_attrs[] = {
	&caps_start_attribute.attr,
	&caps_stop_attribute.attr,
	&pitch_attribute.attr,
	&punct_attribute.attr,
	&rate_attribute.attr,
	&voice_attribute.attr,
	&vol_attribute.attr,
	&delay_time_attribute.attr,
	&full_time_attribute.attr,
	&jiffy_delta_attribute.attr,
	&trigger_time_attribute.attr,
	NULL,	/* need to NULL terminate the list of attributes */
};

static struct spk_synth synth_dectlk = {
	.name = "dectlk",
	.version = DRV_VERSION,
	.long_name = "Dectalk Express",
	.init = "[:dv ap 100][:error sp]",
	.procspeech = PROCSPEECH,
	.clear = SYNTH_CLEAR,
	.delay = 500,
	.trigger = 50,
	.jiffies = 50,
	.full = 40000,
	.flags = SF_DEC,
	.startup = SYNTH_START,
	.checkval = SYNTH_CHECK,
	.vars = vars,
	.probe = serial_synth_probe,
	.release = spk_serial_release,
	.synth_immediate = spk_synth_immediate,
	.catch_up = do_catch_up,
	.flush = synth_flush,
	.is_alive = spk_synth_is_alive_restart,
	.synth_adjust = NULL,
	.read_buff_add = read_buff_add,
	.get_index = get_index,
	.indexing = {
		.command = "[:in re %d] ",
		.lowindex = 1,
		.highindex = 8,
		.currindex = 1,
	},
	.attributes = {
		.attrs = synth_attrs,
		.name = "dectlk",
	},
};

static int is_indnum(u_char *ch)
{
	if ((*ch >= '0') && (*ch <= '9')) {
		*ch = *ch - '0';
		return 1;
	}
	return 0;
}

static u_char lastind = 0;

static unsigned char get_index(void)
{
	u_char rv;
	rv = lastind;
	lastind = 0;
	return rv;
}

static void read_buff_add(u_char c)
{
	static int ind = -1;

	if (c == 0x01) {
		unsigned long flags;
		spin_lock_irqsave(&flush_lock, flags);
		is_flushing = 0;
		wake_up_interruptible(&flush);
		spin_unlock_irqrestore(&flush_lock, flags);
	} else if (c == 0x13) {
		xoff = 1;
	} else if (c == 0x11) {
		xoff = 0;
	} else if (is_indnum(&c)) {
		if (ind == -1)
			ind = c;
		else
			ind = ind * 10 + c;
	} else if ((c > 31) && (c < 127)) {
		if (ind != -1)
			lastind = (u_char)ind;
		ind = -1;
	}
}

static void do_catch_up(struct spk_synth *synth)
{
	static u_char ch = 0;
	static u_char last = '\0';
	unsigned long flags;
	unsigned long jiff_max;
	unsigned long timeout = msecs_to_jiffies(4000);
	DEFINE_WAIT(wait);
	struct var_t *jiffy_delta;
	struct var_t *delay_time;
	int jiffy_delta_val;
	int delay_time_val;

	jiffy_delta = get_var(JIFFY);
	delay_time = get_var(DELAY);
	spk_lock(flags);
	jiffy_delta_val = jiffy_delta->u.n.value;
	spk_unlock(flags);
	jiff_max = jiffies + jiffy_delta_val;

	while (!kthread_should_stop()) {
		/* if no ctl-a in 4, send data anyway */
		spin_lock_irqsave(&flush_lock, flags);
		while (is_flushing && timeout) {
			prepare_to_wait(&flush, &wait, TASK_INTERRUPTIBLE);
			spin_unlock_irqrestore(&flush_lock, flags);
			timeout = schedule_timeout(timeout);
			spin_lock_irqsave(&flush_lock, flags);
		}
		finish_wait(&flush, &wait);
		is_flushing = 0;
		spin_unlock_irqrestore(&flush_lock, flags);

		spk_lock(flags);
		if (speakup_info.flushing) {
			speakup_info.flushing = 0;
			spk_unlock(flags);
			synth->flush(synth);
			continue;
		}
		if (synth_buffer_empty()) {
			spk_unlock(flags);
			break;
		}
		ch = synth_buffer_peek();
		set_current_state(TASK_INTERRUPTIBLE);
		delay_time_val = delay_time->u.n.value;
		spk_unlock(flags);
		if (ch == '\n')
			ch = 0x0D;
		if (synth_full() || !spk_serial_out(ch)) {
			schedule_timeout(msecs_to_jiffies(delay_time_val));
			continue;
		}
		set_current_state(TASK_RUNNING);
		spk_lock(flags);
		synth_buffer_getc();
		spk_unlock(flags);
		if (ch == '[')
			in_escape = 1;
		else if (ch == ']')
			in_escape = 0;
		else if (ch <= SPACE) {
			if (!in_escape && strchr(",.!?;:", last))
				spk_serial_out(PROCSPEECH);
			if (jiffies >= jiff_max) {
				if ( ! in_escape )
					spk_serial_out(PROCSPEECH);
				spk_lock(flags);
				jiffy_delta_val = jiffy_delta->u.n.value;
				delay_time_val = delay_time->u.n.value;
				spk_unlock(flags);
				schedule_timeout(msecs_to_jiffies(delay_time_val));
				jiff_max = jiffies + jiffy_delta_val;
			}
		}
		last = ch;
	}
	if (!in_escape)
		spk_serial_out(PROCSPEECH);
}

static void synth_flush(struct spk_synth *synth)
{
	if (in_escape) {
		/* if in command output ']' so we don't get an error */
		spk_serial_out(']');
	}
	in_escape = 0;
	is_flushing = 1;
	spk_serial_out(SYNTH_CLEAR);
}

module_param_named(ser, synth_dectlk.ser, int, S_IRUGO);
module_param_named(start, synth_dectlk.startup, short, S_IRUGO);

MODULE_PARM_DESC(ser, "Set the serial port for the synthesizer (0-based).");
MODULE_PARM_DESC(start, "Start the synthesizer once it is loaded.");

static int __init dectlk_init(void)
{
	return synth_add(&synth_dectlk);
}

static void __exit dectlk_exit(void)
{
	synth_remove(&synth_dectlk);
}

module_init(dectlk_init);
module_exit(dectlk_exit);
MODULE_AUTHOR("Kirk Reiser <kirk@braille.uwo.ca>");
MODULE_AUTHOR("David Borowski");
MODULE_DESCRIPTION("Speakup support for DECtalk Express synthesizers");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);

