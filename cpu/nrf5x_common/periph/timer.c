/*
 * Copyright (C) 2014-2016 Freie Universität Berlin
 *               2015 Jan Wagner <mail@jwagner.eu>
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     cpu_nrf5x_common
 * @ingroup     drivers_periph_timer
 * @{
 *
 * @file
 * @brief       Implementation of the peripheral timer interface
 *
 * @author      Christian Kühling <kuehling@zedat.fu-berlin.de>
 * @author      Timo Ziegler <timo.ziegler@fu-berlin.de>
 * @author      Hauke Petersen <hauke.petersen@fu-berlin.de>
 * @author      Jan Wagner <mail@jwagner.eu>
 *
 * @}
 */

#include "periph/timer.h"

#define F_TIMER             (16000000U)     /* the timer is clocked at 16MHz */

typedef struct {
    timer_cb_t cb;
    void *arg;
    uint8_t flags;
    uint8_t is_periodic;
} tim_ctx_t;

/**
 * @brief timer state memory
 */
static tim_ctx_t ctx[TIMER_NUMOF];

static inline NRF_TIMER_Type *dev(tim_t tim)
{
    return timer_config[tim].dev;
}

int timer_init(tim_t tim, uint32_t freq, timer_cb_t cb, void *arg)
{
    /* make sure the given timer is valid */
    if (tim >= TIMER_NUMOF) {
        return -1;
    }

    /* save interrupt context */
    ctx[tim].cb = cb;
    ctx[tim].arg = arg;

    /* power on timer */
#if CPU_FAM_NRF51
    dev(tim)->POWER = 1;
#endif

    /* reset and configure the timer */
    dev(tim)->TASKS_STOP = 1;
    dev(tim)->BITMODE = timer_config[tim].bitmode;
    dev(tim)->MODE = TIMER_MODE_MODE_Timer;
    dev(tim)->TASKS_CLEAR = 1;

    /* figure out if desired frequency is available */
    int i;
    unsigned long cando = F_TIMER;
    for (i = 0; i < 10; i++) {
        if (freq == cando) {
            dev(tim)->PRESCALER = i;
            break;
        }
        cando /= 2;
    }
    if (i == 10) {
        return -1;
    }

    /* reset compare state */
    dev(tim)->EVENTS_COMPARE[0] = 0;
    dev(tim)->EVENTS_COMPARE[1] = 0;
    dev(tim)->EVENTS_COMPARE[2] = 0;

    /* enable interrupts */
    NVIC_EnableIRQ(timer_config[tim].irqn);
    /* start the timer */
    dev(tim)->TASKS_START = 1;

    return 0;
}

int timer_set_absolute(tim_t tim, int chan, unsigned int value)
{
    /* see if channel is valid */
    if (chan >= timer_config[tim].channels) {
        return -1;
    }

    ctx[tim].flags |= (1 << chan);
    dev(tim)->CC[chan] = value;

    /* clear spurious IRQs */
    dev(tim)->EVENTS_COMPARE[chan] = 0;
    (void)dev(tim)->EVENTS_COMPARE[chan];

    /* enable IRQ */
    dev(tim)->INTENSET = (TIMER_INTENSET_COMPARE0_Msk << chan);

    return 0;
}

int timer_set_periodic(tim_t tim, int chan, unsigned int value, uint8_t flags)
{
    /* see if channel is valid */
    if (chan >= timer_config[tim].channels) {
        return -1;
    }

    /* stop timer to avoid race condition */
    dev(tim)->TASKS_STOP = 1;

    ctx[tim].flags |= (1 << chan);
    ctx[tim].is_periodic |= (1 << chan);
    dev(tim)->CC[chan] = value;
    if (flags & TIM_FLAG_RESET_ON_MATCH) {
        dev(tim)->SHORTS |= (1 << chan);
    }
    if (flags & TIM_FLAG_RESET_ON_SET) {
        dev(tim)->TASKS_CLEAR = 1;
    }

    /* clear spurious IRQs */
    dev(tim)->EVENTS_COMPARE[chan] = 0;
    (void)dev(tim)->EVENTS_COMPARE[chan];

    /* enable IRQ */
    dev(tim)->INTENSET = (TIMER_INTENSET_COMPARE0_Msk << chan);

    /* re-start timer */
    if (!(flags & TIM_FLAG_SET_STOPPED)) {
        dev(tim)->TASKS_START = 1;
    }

    return 0;
}

int timer_clear(tim_t tim, int chan)
{
    /* see if channel is valid */
    if (chan >= timer_config[tim].channels) {
        return -1;
    }

    dev(tim)->INTENCLR = (TIMER_INTENSET_COMPARE0_Msk << chan);
    /* Clear out the Compare->Clear flag of this channel */
    dev(tim)->SHORTS &= ~(1 << chan);
    ctx[tim].flags &= ~(1 << chan);
    ctx[tim].is_periodic &= ~(1 << chan);

    return 0;
}

unsigned int timer_read(tim_t tim)
{
    dev(tim)->TASKS_CAPTURE[timer_config[tim].channels] = 1;
    return dev(tim)->CC[timer_config[tim].channels];
}

void timer_start(tim_t tim)
{
    dev(tim)->TASKS_START = 1;
}

void timer_stop(tim_t tim)
{
    /* Errata: [78] TIMER: High current consumption when using
     *                     timer STOP task only
     *
     * # Symptoms
     *
     * Increased current consumption when the timer has been running and the
     * STOP task is used to stop it.
     *
     * # Conditions
     * The timer has been running (after triggering a START task) and then it is
     * stopped using a STOP task only.
     *
     * # Consequences
     *
     * Increased current consumption.
     *
     * # Workaround
     *
     * Use the SHUTDOWN task after the STOP task or instead of the STOP task
     *
     * cf. https://infocenter.nordicsemi.com/pdf/nRF52833_Engineering_A_Errata_v1.4.pdf
     */
    dev(tim)->TASKS_SHUTDOWN = 1;
}

static inline void irq_handler(int num)
{
    for (unsigned i = 0; i < timer_config[num].channels; i++) {
        if (dev(num)->EVENTS_COMPARE[i] == 1) {
            dev(num)->EVENTS_COMPARE[i] = 0;
            if (ctx[num].flags & (1 << i)) {
                if ((ctx[num].is_periodic & (1 << i)) == 0) {
                    ctx[num].flags &= ~(1 << i);
                    dev(num)->INTENCLR = (TIMER_INTENSET_COMPARE0_Msk << i);
                }
                ctx[num].cb(ctx[num].arg, i);
            }
        }
    }
    cortexm_isr_end();
}

#ifdef TIMER_0_ISR
void TIMER_0_ISR(void)
{
    irq_handler(0);
}
#endif

#ifdef TIMER_1_ISR
void TIMER_1_ISR(void)
{
    irq_handler(1);
}
#endif

#ifdef TIMER_2_ISR
void TIMER_2_ISR(void)
{
    irq_handler(2);
}
#endif

#ifdef TIMER_3_ISR
void TIMER_3_ISR(void)
{
    irq_handler(3);
}
#endif
