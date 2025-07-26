/*
 * Apple A7IOP Mailbox.
 *
 * Copyright (c) 2023-2025 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2025 Christian Inci (chris-pcguy).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "block/aio.h"
#include "hw/irq.h"
#include "hw/misc/apple-silicon/a7iop/base.h"
#include "hw/misc/apple-silicon/a7iop/mailbox/core.h"
#include "hw/misc/apple-silicon/a7iop/mailbox/private.h"
#include "hw/misc/apple-silicon/a7iop/mailbox/trace.h"
#include "hw/misc/apple-silicon/a7iop/private.h"
#include "hw/qdev-core.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/lockable.h"
#include "qemu/log.h"
#include "qemu/queue.h"

#define MAX_MESSAGE_COUNT (15)

#define CTRL_ENABLE BIT(0)
#define CTRL_FULL BIT(16)
#define CTRL_EMPTY BIT(17)
#define CTRL_OVERFLOW BIT(18)
#define CTRL_UNDERFLOW BIT(19)
#define CTRL_COUNT_SHIFT (20)
#define CTRL_COUNT_MASK (MAX_MESSAGE_COUNT << CTRL_COUNT_SHIFT)
#define CTRL_COUNT(v) (((v) << CTRL_COUNT_SHIFT) & CTRL_COUNT_MASK)

#define IOP_EMPTY BIT(0)
#define IOP_NONEMPTY BIT(4)
#define AP_EMPTY BIT(8)
#define AP_NONEMPTY BIT(12)

static bool is_interrupt_enabled(AppleA7IOPMailbox *s, uint32_t status)
{
    if ((status & 0xf0000) == 0x10000) {
        uint32_t interrupt = status & 0x7f;
        uint32_t interrupt_enabled =
            s->interrupts_enabled[interrupt / 32] & (1 << (interrupt % 32));
        if (interrupt_enabled) {
            return true;
        }
    } else {
        return true;
    }
    return false;
}


static bool apple_mbox_interrupt_status_empty(AppleA7IOPMailbox *s)
{
    AppleA7IOPInterruptStatusMessage *msg;

    QTAILQ_FOREACH (msg, &s->interrupt_status, entry) {
        if (is_interrupt_enabled(s, msg->status)) {
            return false;
        }
    }

    return true;
}

static inline bool iop_empty_is_unmasked(uint32_t int_mask)
{
    return (int_mask & IOP_EMPTY) == 0;
}

static inline bool iop_nonempty_is_unmasked(uint32_t int_mask)
{
    return (int_mask & IOP_NONEMPTY) == 0;
}

static inline bool ap_empty_is_unmasked(uint32_t int_mask)
{
    return (int_mask & AP_EMPTY) == 0;
}

static inline bool ap_nonempty_is_unmasked(uint32_t int_mask)
{
    return (int_mask & AP_NONEMPTY) == 0;
}

void apple_a7iop_mailbox_update_irq_status(AppleA7IOPMailbox *s)
{
    bool iop_empty;
    bool ap_empty;
    bool iop_underflow;
    bool ap_underflow;
    bool iop_nonempty_unmasked;
    bool iop_empty_unmasked;
    bool ap_nonempty_unmasked;
    bool ap_empty_unmasked;

    iop_empty = QTAILQ_EMPTY(&s->iop_mailbox->inbox);
    ap_empty = QTAILQ_EMPTY(&s->ap_mailbox->inbox);
    iop_underflow = s->iop_mailbox->underflow;
    ap_underflow = s->ap_mailbox->underflow;
    iop_nonempty_unmasked = iop_nonempty_is_unmasked(s->int_mask);
    iop_empty_unmasked = iop_empty_is_unmasked(s->int_mask);
    ap_nonempty_unmasked = ap_nonempty_is_unmasked(s->int_mask);
    ap_empty_unmasked = ap_empty_is_unmasked(s->int_mask);

    trace_apple_a7iop_mailbox_update_irq(
        s->role, iop_empty, ap_empty, !iop_nonempty_unmasked,
        !iop_empty_unmasked, !ap_nonempty_unmasked, !ap_empty_unmasked);

    qemu_set_irq(s->irqs[APPLE_A7IOP_IRQ_IOP_NONEMPTY],
                 (iop_nonempty_unmasked && !iop_empty) || iop_underflow);
    qemu_set_irq(s->irqs[APPLE_A7IOP_IRQ_IOP_EMPTY],
                 iop_empty_unmasked && iop_empty);

    qemu_set_irq(s->irqs[APPLE_A7IOP_IRQ_AP_NONEMPTY],
                 (ap_nonempty_unmasked && !ap_empty) || ap_underflow);
    qemu_set_irq(s->irqs[APPLE_A7IOP_IRQ_AP_EMPTY],
                 ap_empty_unmasked && ap_empty);

    s->iop_nonempty = (iop_nonempty_unmasked && !iop_empty) || iop_underflow;
    s->iop_empty = iop_empty_unmasked && iop_empty;
    s->ap_nonempty = (ap_nonempty_unmasked && !ap_empty) || ap_underflow;
    s->ap_empty = ap_empty_unmasked && ap_empty;
}

void apple_a7iop_mailbox_update_irq(AppleA7IOPMailbox *s)
{
    apple_a7iop_mailbox_update_irq_status(s);

    bool iop_irq_raised = s->iop_nonempty || s->iop_empty || s->ap_nonempty ||
                          s->ap_empty || !apple_mbox_interrupt_status_empty(s);
    if (!strcmp(s->role, "SEP-iop")) {
        qemu_set_irq(s->iop_irq, iop_irq_raised);
    }
    smp_mb();
    if (!strcmp(s->role, "SEP-ap")) {
        apple_a7iop_mailbox_update_irq(s->iop_mailbox);
    }
}

bool apple_a7iop_mailbox_is_empty(AppleA7IOPMailbox *s)
{
    QEMU_LOCK_GUARD(&s->lock);

    return s->underflow || QTAILQ_EMPTY(&s->inbox);
}

static void apple_a7iop_mailbox_send(AppleA7IOPMailbox *s,
                                     AppleA7IOPMessage *msg)
{
    g_assert_nonnull(msg);

    QEMU_LOCK_GUARD(&s->lock);

    trace_apple_a7iop_mailbox_send(s->role, ldq_le_p(msg->data),
                                   ldq_le_p(msg->data + sizeof(uint64_t)));

    QTAILQ_INSERT_TAIL(&s->inbox, msg, next);
    s->count++;
    apple_a7iop_mailbox_update_irq(s);

    if (s->bh != NULL) {
        qemu_bh_schedule(s->bh);
    }
}

void apple_a7iop_mailbox_send_ap(AppleA7IOPMailbox *s, AppleA7IOPMessage *msg)
{
    WITH_QEMU_LOCK_GUARD(&s->lock)
    {
        if (!s->ap_dir_en) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s direction not enabled.\n",
                          __FUNCTION__, s->role);
            return;
        }
    }

    apple_a7iop_mailbox_send(s->ap_mailbox, msg);
    WITH_QEMU_LOCK_GUARD(&s->lock)
    {
        apple_a7iop_mailbox_update_irq(s);
    }
}

void apple_a7iop_mailbox_send_iop(AppleA7IOPMailbox *s, AppleA7IOPMessage *msg)
{
    WITH_QEMU_LOCK_GUARD(&s->lock)
    {
        if (!s->iop_dir_en) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s direction not enabled.\n",
                          __FUNCTION__, s->role);
            return;
        }
    }

    apple_a7iop_mailbox_send(s->iop_mailbox, msg);
    WITH_QEMU_LOCK_GUARD(&s->lock)
    {
        apple_a7iop_mailbox_update_irq(s);
    }
}

AppleA7IOPMessage *apple_a7iop_inbox_peek(AppleA7IOPMailbox *s)
{
    return QTAILQ_FIRST(&s->inbox);
}

static AppleA7IOPMessage *apple_a7iop_mailbox_recv(AppleA7IOPMailbox *s)
{
    AppleA7IOPMessage *msg;

    QEMU_LOCK_GUARD(&s->lock);

    if (s->underflow) {
        return NULL;
    }

    msg = QTAILQ_FIRST(&s->inbox);
    if (msg == NULL) {
        s->underflow = true;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s underflowed.\n", __FUNCTION__,
                      s->role);
        apple_a7iop_mailbox_update_irq(s);
        return NULL;
    }

    QTAILQ_REMOVE(&s->inbox, msg, next);
    stl_le_p(msg->data + 0xC, ldl_le_p(msg->data + 0xC) | CTRL_COUNT(s->count));
    trace_apple_a7iop_mailbox_recv(s->role, ldq_le_p(msg->data),
                                   ldq_le_p(msg->data + sizeof(uint64_t)));
    s->count--;
    apple_a7iop_mailbox_update_irq(s);

    return msg;
}

AppleA7IOPMessage *apple_a7iop_mailbox_recv_iop(AppleA7IOPMailbox *s)
{
    AppleA7IOPMessage *msg;

    WITH_QEMU_LOCK_GUARD(&s->lock)
    {
        if (!s->iop_dir_en) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s direction not enabled.\n",
                          __FUNCTION__, s->role);
            return NULL;
        }
    }

    msg = apple_a7iop_mailbox_recv(s->iop_mailbox);

    WITH_QEMU_LOCK_GUARD(&s->lock)
    {
        apple_a7iop_mailbox_update_irq(s);
    }

    return msg;
}

AppleA7IOPMessage *apple_a7iop_mailbox_recv_ap(AppleA7IOPMailbox *s)
{
    AppleA7IOPMessage *msg;

    WITH_QEMU_LOCK_GUARD(&s->lock)
    {
        if (!s->ap_dir_en) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s direction not enabled.\n",
                          __FUNCTION__, s->role);
            return NULL;
        }
    }

    msg = apple_a7iop_mailbox_recv(s->ap_mailbox);

    WITH_QEMU_LOCK_GUARD(&s->lock)
    {
        apple_a7iop_mailbox_update_irq(s);
    }

    return msg;
}

uint32_t apple_a7iop_mailbox_get_int_mask(AppleA7IOPMailbox *s)
{
    QEMU_LOCK_GUARD(&s->lock);

    return s->int_mask;
}

void apple_a7iop_mailbox_set_int_mask(AppleA7IOPMailbox *s, uint32_t value)
{
    QEMU_LOCK_GUARD(&s->lock);

    s->int_mask |= value;
    apple_a7iop_mailbox_update_irq(s);
}

void apple_a7iop_mailbox_clear_int_mask(AppleA7IOPMailbox *s, uint32_t value)
{
    QEMU_LOCK_GUARD(&s->lock);

    s->int_mask &= ~value;
    apple_a7iop_mailbox_update_irq(s);
}

static inline uint32_t apple_a7iop_mailbox_ctrl(AppleA7IOPMailbox *s)
{
    if (s->underflow) {
        return CTRL_UNDERFLOW;
    }

    return (s->count >= MAX_MESSAGE_COUNT ? CTRL_FULL : 0) |
           (QTAILQ_EMPTY(&s->inbox) ? CTRL_EMPTY : 0) |
           CTRL_COUNT(MIN(s->count, MAX_MESSAGE_COUNT));
}

uint32_t apple_a7iop_mailbox_get_iop_ctrl(AppleA7IOPMailbox *s)
{
    QEMU_LOCK_GUARD(&s->lock);

    return (s->iop_dir_en ? CTRL_ENABLE : 0) |
           apple_a7iop_mailbox_ctrl(s->iop_mailbox);
}

void apple_a7iop_mailbox_set_iop_ctrl(AppleA7IOPMailbox *s, uint32_t value)
{
    QEMU_LOCK_GUARD(&s->lock);

    s->iop_dir_en = (value & CTRL_ENABLE) != 0;
}

uint32_t apple_a7iop_mailbox_get_ap_ctrl(AppleA7IOPMailbox *s)
{
    QEMU_LOCK_GUARD(&s->lock);

    return (s->ap_dir_en ? CTRL_ENABLE : 0) |
           apple_a7iop_mailbox_ctrl(s->ap_mailbox);
}

void apple_a7iop_mailbox_set_ap_ctrl(AppleA7IOPMailbox *s, uint32_t value)
{
    QEMU_LOCK_GUARD(&s->lock);

    s->ap_dir_en = (value & CTRL_ENABLE) != 0;
}

void apple_a7iop_interrupt_status_push(AppleA7IOPMailbox *s, uint32_t status)
{
    AppleA7IOPInterruptStatusMessage *msg;

    QEMU_LOCK_GUARD(&s->lock);

    msg = g_new0(struct AppleA7IOPInterruptStatusMessage, 1);
    msg->status = status;
    QTAILQ_INSERT_TAIL(&s->interrupt_status, msg, entry);
    apple_a7iop_mailbox_update_irq(s);
}

uint32_t apple_a7iop_interrupt_status_pop(AppleA7IOPMailbox *s)
{
    uint32_t ret = 0;
    AppleA7IOPInterruptStatusMessage *msg;
    AppleA7IOPInterruptStatusMessage *lowest_msg;

    QEMU_LOCK_GUARD(&s->lock);

    lowest_msg = NULL;
    QTAILQ_FOREACH (msg, &s->interrupt_status, entry) {
        if (is_interrupt_enabled(s, msg->status)) {
            if (lowest_msg == NULL || (msg->status < lowest_msg->status)) {
                lowest_msg = msg;
            }
        }
    }

    if (lowest_msg) {
        QTAILQ_REMOVE(&s->interrupt_status, lowest_msg, entry);
        ret = lowest_msg->status;
        g_free(lowest_msg);
    }

    apple_a7iop_mailbox_update_irq(s);

    return ret;
}

AppleA7IOPMailbox *apple_a7iop_mailbox_new(const char *role,
                                           AppleA7IOPVersion version,
                                           AppleA7IOPMailbox *iop_mailbox,
                                           AppleA7IOPMailbox *ap_mailbox,
                                           QEMUBH *bh)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    AppleA7IOPMailbox *s;
    int i;
    char name[128];

    dev = qdev_new(TYPE_APPLE_A7IOP_MAILBOX);
    sbd = SYS_BUS_DEVICE(dev);
    s = APPLE_A7IOP_MAILBOX(dev);

    s->role = g_strdup(role);
    s->iop_mailbox = iop_mailbox ? iop_mailbox : s;
    s->ap_mailbox = ap_mailbox ? ap_mailbox : s;
    s->bh = bh;
    QTAILQ_INIT(&s->inbox);
    QTAILQ_INIT(&s->interrupt_status);
    qemu_mutex_init(&s->lock);
    for (i = 0; i < APPLE_A7IOP_IRQ_MAX; i++) {
        sysbus_init_irq(sbd, s->irqs + i);
    }
    qdev_init_gpio_out_named(dev, &s->iop_irq, APPLE_A7IOP_IOP_IRQ, 1);
    snprintf(name, sizeof(name), TYPE_APPLE_A7IOP_MAILBOX ".%s.regs", s->role);
    switch (version) {
    case APPLE_A7IOP_V2:
        apple_a7iop_mailbox_init_mmio_v2(s, name);
        break;
    case APPLE_A7IOP_V4:
        apple_a7iop_mailbox_init_mmio_v4(s, name);
        break;
    }
    sysbus_init_mmio(sbd, &s->mmio);

    return s;
}

static void apple_a7iop_mailbox_reset(DeviceState *dev)
{
    AppleA7IOPMailbox *s;
    AppleA7IOPMessage *msg;

    s = APPLE_A7IOP_MAILBOX(dev);

    QEMU_LOCK_GUARD(&s->lock);

    g_assert_true(s->iop_mailbox != s->ap_mailbox);
    s->count = 0;
    s->iop_dir_en = true;
    s->ap_dir_en = true;
    s->underflow = false;
    memset(s->iop_recv_reg, 0, sizeof(s->iop_recv_reg));
    memset(s->ap_recv_reg, 0, sizeof(s->ap_recv_reg));
    memset(s->iop_send_reg, 0, sizeof(s->iop_send_reg));
    memset(s->ap_send_reg, 0, sizeof(s->ap_send_reg));

    while (!QTAILQ_EMPTY(&s->inbox)) {
        msg = QTAILQ_FIRST(&s->inbox);
        QTAILQ_REMOVE(&s->inbox, msg, next);
        g_free(msg);
    }
    while (!QTAILQ_EMPTY(&s->interrupt_status)) {
        AppleA7IOPInterruptStatusMessage *m =
            QTAILQ_FIRST(&s->interrupt_status);
        QTAILQ_REMOVE(&s->interrupt_status, m, entry);
        g_free(m);
    }
    for (int i = 0; i < 4; i++) {
        s->interrupts_enabled[i] = 0;
    }
    s->iop_nonempty = 0;
    s->iop_empty = 0;
    s->ap_nonempty = 0;
    s->ap_empty = 0;
    apple_a7iop_mailbox_update_irq(s);
}

const VMStateDescription vmstate_apple_a7iop_message = {
    .name = "Apple A7IOP Message State",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields =
        (const VMStateField[]){
            VMSTATE_UINT8_ARRAY(data, AppleA7IOPMessage, 16),
            VMSTATE_END_OF_LIST(),
        }
};

static const VMStateDescription vmstate_apple_a7iop_int_status_message = {
    .name = "Apple A7IOP Interrupt Status Message State",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields =
        (const VMStateField[]){
            VMSTATE_UINT32(status, AppleA7IOPInterruptStatusMessage),
            VMSTATE_END_OF_LIST(),
        }
};

static const VMStateDescription vmstate_apple_a7iop_mailbox = {
    .name = "Apple A7IOP Mailbox State",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields =
        (const VMStateField[]){
            VMSTATE_APPLE_A7IOP_MESSAGE(inbox, AppleA7IOPMailbox),
            VMSTATE_QTAILQ_V(interrupt_status, AppleA7IOPMailbox, 0,
                             vmstate_apple_a7iop_int_status_message,
                             AppleA7IOPInterruptStatusMessage, entry),
            VMSTATE_UINT32(count, AppleA7IOPMailbox),
            VMSTATE_BOOL(iop_dir_en, AppleA7IOPMailbox),
            VMSTATE_BOOL(ap_dir_en, AppleA7IOPMailbox),
            VMSTATE_BOOL(underflow, AppleA7IOPMailbox),
            VMSTATE_UINT32(int_mask, AppleA7IOPMailbox),
            VMSTATE_UINT8_ARRAY(iop_recv_reg, AppleA7IOPMailbox, 16),
            VMSTATE_UINT8_ARRAY(ap_recv_reg, AppleA7IOPMailbox, 16),
            VMSTATE_UINT8_ARRAY(iop_send_reg, AppleA7IOPMailbox, 16),
            VMSTATE_UINT8_ARRAY(ap_send_reg, AppleA7IOPMailbox, 16),
            VMSTATE_UINT32_ARRAY(interrupts_enabled, AppleA7IOPMailbox, 4),
            VMSTATE_BOOL(iop_nonempty, AppleA7IOPMailbox),
            VMSTATE_BOOL(iop_empty, AppleA7IOPMailbox),
            VMSTATE_BOOL(ap_nonempty, AppleA7IOPMailbox),
            VMSTATE_BOOL(ap_empty, AppleA7IOPMailbox),
            VMSTATE_END_OF_LIST(),
        }
};

static void apple_a7iop_mailbox_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc;

    dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_apple_a7iop_mailbox;
    device_class_set_legacy_reset(dc, apple_a7iop_mailbox_reset);
    dc->desc = "Apple A7IOP Mailbox";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo apple_a7iop_mailbox_info = {
    .name = TYPE_APPLE_A7IOP_MAILBOX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleA7IOPMailbox),
    .class_init = apple_a7iop_mailbox_class_init,
};

static void apple_a7iop_mailbox_register_types(void)
{
    type_register_static(&apple_a7iop_mailbox_info);
}

type_init(apple_a7iop_mailbox_register_types);
