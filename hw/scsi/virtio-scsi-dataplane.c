/*
 * Virtio SCSI dataplane
 *
 * Copyright Red Hat, Inc. 2014
 *
 * Authors:
 *   Fam Zheng <famz@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/clone-visitor.h"
#include "qapi/qapi-visit-virtio.h"
#include "hw/virtio/virtio-scsi.h"
#include "qemu/error-report.h"
#include "system/block-backend.h"
#include "hw/scsi/scsi.h"
#include "scsi/constants.h"
#include "hw/virtio/iothread-vq-mapping.h"
#include "hw/virtio/virtio-bus.h"

/* Context: BQL held */
void virtio_scsi_dataplane_setup(VirtIOSCSI *s, Error **errp)
{
    VirtIOSCSICommon *vs = VIRTIO_SCSI_COMMON(s);
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    BusState *qbus = qdev_get_parent_bus(DEVICE(vdev));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);

    if (vs->conf.iothread && vs->conf.iothread_vq_mapping_list) {
        error_setg(errp,
                   "iothread and iothread-vq-mapping properties cannot be set "
                   "at the same time");
        return;
    }

    if (vs->conf.iothread || vs->conf.iothread_vq_mapping_list) {
        if (!k->set_guest_notifiers || !k->ioeventfd_assign) {
            error_setg(errp,
                       "device is incompatible with iothread "
                       "(transport does not support notifiers)");
            return;
        }
        if (!virtio_device_ioeventfd_enabled(vdev)) {
            error_setg(errp, "ioeventfd is required for iothread");
            return;
        }
    }

    s->vq_aio_context = g_new(AioContext *, vs->conf.num_queues +
                                            VIRTIO_SCSI_VQ_NUM_FIXED);

    /*
     * Handle the ctrl virtqueue in the main loop thread where device resets
     * can be performed.
     */
    s->vq_aio_context[0] = qemu_get_aio_context();

    /*
     * Handle the event virtqueue in the main loop thread where its no_poll
     * behavior won't stop IOThread polling.
     */
    s->vq_aio_context[1] = qemu_get_aio_context();

    if (vs->conf.iothread_vq_mapping_list) {
        if (!iothread_vq_mapping_apply(vs->conf.iothread_vq_mapping_list,
                    &s->vq_aio_context[VIRTIO_SCSI_VQ_NUM_FIXED],
                    vs->conf.num_queues, errp)) {
            g_free(s->vq_aio_context);
            s->vq_aio_context = NULL;
            return;
        }
    } else if (vs->conf.iothread) {
        AioContext *ctx = iothread_get_aio_context(vs->conf.iothread);
        for (uint16_t i = 0; i < vs->conf.num_queues; i++) {
            s->vq_aio_context[VIRTIO_SCSI_VQ_NUM_FIXED + i] = ctx;
        }

        /* Released in virtio_scsi_dataplane_cleanup() */
        object_ref(OBJECT(vs->conf.iothread));
    } else {
        AioContext *ctx = qemu_get_aio_context();
        for (unsigned i = 0; i < vs->conf.num_queues; i++) {
            s->vq_aio_context[VIRTIO_SCSI_VQ_NUM_FIXED + i] = ctx;
        }
    }
}

/* Context: BQL held */
void virtio_scsi_dataplane_cleanup(VirtIOSCSI *s)
{
    VirtIOSCSICommon *vs = VIRTIO_SCSI_COMMON(s);

    if (vs->conf.iothread_vq_mapping_list) {
        iothread_vq_mapping_cleanup(vs->conf.iothread_vq_mapping_list);
    }

    if (vs->conf.iothread) {
        object_unref(OBJECT(vs->conf.iothread));
    }

    g_free(s->vq_aio_context);
    s->vq_aio_context = NULL;
}

static int virtio_scsi_set_host_notifier(VirtIOSCSI *s, VirtQueue *vq, int n)
{
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(s)));
    int rc;

    /* Set up virtqueue notify */
    rc = virtio_bus_set_host_notifier(VIRTIO_BUS(qbus), n, true);
    if (rc != 0) {
        fprintf(stderr, "virtio-scsi: Failed to set host notifier (%d)\n",
                rc);
        s->dataplane_fenced = true;
        return rc;
    }

    return 0;
}

/* Context: BH in IOThread */
static void virtio_scsi_dataplane_stop_vq_bh(void *opaque)
{
    AioContext *ctx = qemu_get_current_aio_context();
    VirtQueue *vq = opaque;
    EventNotifier *host_notifier;

    virtio_queue_aio_detach_host_notifier(vq, ctx);
    host_notifier = virtio_queue_get_host_notifier(vq);

    /*
     * Test and clear notifier after disabling event, in case poll callback
     * didn't have time to run.
     */
    virtio_queue_host_notifier_read(host_notifier);
}

/* Context: BQL held */
int virtio_scsi_dataplane_start(VirtIODevice *vdev)
{
    int i;
    int rc;
    int vq_init_count = 0;
    BusState *qbus = qdev_get_parent_bus(DEVICE(vdev));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    VirtIOSCSICommon *vs = VIRTIO_SCSI_COMMON(vdev);
    VirtIOSCSI *s = VIRTIO_SCSI(vdev);

    if (s->dataplane_started ||
        s->dataplane_starting ||
        s->dataplane_fenced) {
        return 0;
    }

    s->dataplane_starting = true;

    /* Set up guest notifier (irq) */
    rc = k->set_guest_notifiers(qbus->parent, vs->conf.num_queues + 2, true);
    if (rc != 0) {
        error_report("virtio-scsi: Failed to set guest notifiers (%d), "
                     "ensure -accel kvm is set.", rc);
        goto fail_guest_notifiers;
    }

    /*
     * Batch all the host notifiers in a single transaction to avoid
     * quadratic time complexity in address_space_update_ioeventfds().
     */
    memory_region_transaction_begin();

    rc = virtio_scsi_set_host_notifier(s, vs->ctrl_vq, 0);
    if (rc != 0) {
        goto fail_host_notifiers;
    }

    vq_init_count++;
    rc = virtio_scsi_set_host_notifier(s, vs->event_vq, 1);
    if (rc != 0) {
        goto fail_host_notifiers;
    }

    vq_init_count++;

    for (i = 0; i < vs->conf.num_queues; i++) {
        rc = virtio_scsi_set_host_notifier(s, vs->cmd_vqs[i], i + 2);
        if (rc) {
            goto fail_host_notifiers;
        }
        vq_init_count++;
    }

    memory_region_transaction_commit();

    s->dataplane_starting = false;
    s->dataplane_started = true;
    smp_wmb(); /* paired with aio_notify_accept() */

    if (s->bus.drain_count == 0) {
        virtio_queue_aio_attach_host_notifier(vs->ctrl_vq,
                                              s->vq_aio_context[0]);
        virtio_queue_aio_attach_host_notifier_no_poll(vs->event_vq,
                                                      s->vq_aio_context[1]);

        for (i = 0; i < vs->conf.num_queues; i++) {
            AioContext *ctx = s->vq_aio_context[VIRTIO_SCSI_VQ_NUM_FIXED + i];
            virtio_queue_aio_attach_host_notifier(vs->cmd_vqs[i], ctx);
        }
    }
    return 0;

fail_host_notifiers:
    for (i = 0; i < vq_init_count; i++) {
        virtio_bus_set_host_notifier(VIRTIO_BUS(qbus), i, false);
    }

    /*
     * The transaction expects the ioeventfds to be open when it
     * commits. Do it now, before the cleanup loop.
     */
    memory_region_transaction_commit();

    for (i = 0; i < vq_init_count; i++) {
        virtio_bus_cleanup_host_notifier(VIRTIO_BUS(qbus), i);
    }
    k->set_guest_notifiers(qbus->parent, vs->conf.num_queues + 2, false);
fail_guest_notifiers:
    s->dataplane_fenced = true;
    s->dataplane_starting = false;
    s->dataplane_started = true;
    return -ENOSYS;
}

/* Context: BQL held */
void virtio_scsi_dataplane_stop(VirtIODevice *vdev)
{
    BusState *qbus = qdev_get_parent_bus(DEVICE(vdev));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    VirtIOSCSICommon *vs = VIRTIO_SCSI_COMMON(vdev);
    VirtIOSCSI *s = VIRTIO_SCSI(vdev);
    int i;

    if (!s->dataplane_started || s->dataplane_stopping) {
        return;
    }

    /* Better luck next time. */
    if (s->dataplane_fenced) {
        s->dataplane_fenced = false;
        s->dataplane_started = false;
        return;
    }
    s->dataplane_stopping = true;

    if (s->bus.drain_count == 0) {
        for (i = 0; i < vs->conf.num_queues + VIRTIO_SCSI_VQ_NUM_FIXED; i++) {
            VirtQueue *vq = virtio_get_queue(&vs->parent_obj, i);
            AioContext *ctx = s->vq_aio_context[i];
            aio_wait_bh_oneshot(ctx, virtio_scsi_dataplane_stop_vq_bh, vq);
        }
    }

    blk_drain_all(); /* ensure there are no in-flight requests */

    /*
     * Batch all the host notifiers in a single transaction to avoid
     * quadratic time complexity in address_space_update_ioeventfds().
     */
    memory_region_transaction_begin();

    for (i = 0; i < vs->conf.num_queues + 2; i++) {
        virtio_bus_set_host_notifier(VIRTIO_BUS(qbus), i, false);
    }

    /*
     * The transaction expects the ioeventfds to be open when it
     * commits. Do it now, before the cleanup loop.
     */
    memory_region_transaction_commit();

    for (i = 0; i < vs->conf.num_queues + 2; i++) {
        virtio_bus_cleanup_host_notifier(VIRTIO_BUS(qbus), i);
    }

    /* Clean up guest notifier (irq) */
    k->set_guest_notifiers(qbus->parent, vs->conf.num_queues + 2, false);
    s->dataplane_stopping = false;
    s->dataplane_started = false;
}

/*
 * Reject calls that would race with dataplane start/stop, calls on a
 * fenced device, and calls on devices realized without an
 * iothread-vq-mapping property (there's nothing to remap in that case).
 */
static bool virtio_scsi_set_iothread_vq_mapping_check(VirtIOSCSI *s,
                                                      Error **errp)
{
    VirtIOSCSICommon *vs = VIRTIO_SCSI_COMMON(s);

    if (s->dataplane_starting || s->dataplane_stopping) {
        error_setg(errp, "virtio-scsi dataplane is in transition");
        return false;
    }
    if (s->dataplane_fenced) {
        error_setg(errp, "virtio-scsi dataplane is fenced");
        return false;
    }
    if (vs->conf.iothread) {
        error_setg(errp,
                   "runtime remap requires iothread-vq-mapping, not the "
                   "single-iothread shorthand");
        return false;
    }
    if (!vs->conf.iothread_vq_mapping_list) {
        error_setg(errp,
                   "device was not configured with iothread-vq-mapping");
        return false;
    }
    return true;
}

/*
 * Quiesce the dataplane: mark ourselves the sole owner of notifier
 * (re)attachment for the duration of the rebind, detach host notifiers
 * from their current AioContexts, and drain all in-flight requests.
 *
 * Setting s->dataplane_stopping = true suppresses the SCSIBus
 * ->drained_begin/end auto-attach path for the whole rebind.
 * blk_drain_all() and blk_set_aio_context() each run the block layer's
 * drain machinery, which calls into scsi-disk BlockDevOps and winds up
 * at virtio_scsi_drained_begin/end. drained_end unconditionally
 * re-attaches the host notifier to whatever s->vq_aio_context[i] points
 * at right now. That fights with our manual detach/attach in two ways:
 *
 *   - The drain inside blk_drain_all() re-attaches the cmd-vq host
 *     notifier to the OLD ctx (vq_aio_context[] has not yet been
 *     swapped). The next time we ourselves try to "attach" we double-
 *     attach, and any kick that snuck in between is dispatched on the
 *     OLD ctx even though we are about to move the BB.
 *
 *   - The drain inside blk_set_aio_context() (per-LUN) runs after we
 *     have swapped vq_aio_context[] but BEFORE the BB has actually
 *     moved. drained_end attaches the notifier to the NEW ctx, the new
 *     ctx starts popping requests, and those requests are dispatched to
 *     a BlockBackend whose AioContext is still in flight (mid-move).
 *     The completion then runs on the OLD ctx and calls virtqueue_push()
 *     on a cmd vq whose owner is now NEW. Cmd-vq completions are
 *     unlocked (single-owner assumption), so two threads concurrently
 *     write the same used-ring slot; the guest then reads a descriptor
 *     index it never submitted and prints "request id N is not head".
 *
 * The same flag is used by the dataplane teardown path for the same
 * reason; clearing it in the caller before returning restores normal
 * drain semantics.
 */
static void virtio_scsi_set_iothread_vq_mapping_quiesce(VirtIOSCSI *s)
{
    VirtIOSCSICommon *vs = VIRTIO_SCSI_COMMON(s);
    uint32_t i;

    assert(!s->dataplane_stopping);
    s->dataplane_stopping = true;

    for (i = 0; i < vs->conf.num_queues; i++) {
        VirtQueue *vq = vs->cmd_vqs[i];
        AioContext *ctx = s->vq_aio_context[VIRTIO_SCSI_VQ_NUM_FIXED + i];
        aio_wait_bh_oneshot(ctx, virtio_scsi_dataplane_stop_vq_bh, vq);
    }

    blk_drain_all();
}

/* Context: BQL held (called from QMP dispatcher) */
void virtio_scsi_set_iothread_vq_mapping(VirtIOSCSI *s,
                                         IOThreadVirtQueueMappingList *map,
                                         Error **errp)
{
    VirtIOSCSICommon *vs = VIRTIO_SCSI_COMMON(s);
    IOThreadVirtQueueMappingList *new_map;
    AioContext *new_ctx;
    BusChild *kid;
    uint32_t i;

    if (!virtio_scsi_set_iothread_vq_mapping_check(s, errp)) {
        return;
    }

    /*
     * Deep-copy: the QAPI dispatcher frees the original after we return,
     * but iothread_vq_mapping_apply_runtime takes ownership.
     */
    new_map = QAPI_CLONE(IOThreadVirtQueueMappingList, map);

    if (!s->dataplane_started) {
        if (!iothread_vq_mapping_apply_runtime(
                new_map, &vs->conf.iothread_vq_mapping_list,
                &s->vq_aio_context[VIRTIO_SCSI_VQ_NUM_FIXED],
                vs->conf.num_queues, errp)) {
            qapi_free_IOThreadVirtQueueMappingList(new_map);
        }
        return;
    }

    virtio_scsi_set_iothread_vq_mapping_quiesce(s);

    /* Swap the mapping: validates, takes new refs, drops old refs */
    if (!iothread_vq_mapping_apply_runtime(
            new_map, &vs->conf.iothread_vq_mapping_list,
            &s->vq_aio_context[VIRTIO_SCSI_VQ_NUM_FIXED],
            vs->conf.num_queues, errp)) {
        /*
         * Mapping swap failed — reattach to old contexts so the device
         * doesn't stay wedged.
         */
        qapi_free_IOThreadVirtQueueMappingList(new_map);
        for (i = 0; i < vs->conf.num_queues; i++) {
            AioContext *ctx =
                s->vq_aio_context[VIRTIO_SCSI_VQ_NUM_FIXED + i];
            virtio_queue_aio_attach_host_notifier(vs->cmd_vqs[i], ctx);
        }
        s->dataplane_stopping = false;
        return;
    }

    /*
     * Re-home each LUN's BlockBackend to the new "primary" command-queue
     * AioContext BEFORE reattaching host notifiers.
     *
     * The order matters: virtio_queue_aio_attach_host_notifier() kicks the
     * VQ once the notifier is on the new ctx, so new guest requests will
     * be popped on the new ctx and dispatched to the BlockBackend. If the
     * BB still lives on the old iothread's ctx at that point, the blk_aio_*
     * dispatch crosses threads; the completion then runs on the OLD ctx and
     * calls virtqueue_push() on a VQ whose AioContext is now the NEW ctx.
     * The cmd-VQ completion path takes no lock (it assumes a single owning
     * iothread), so two threads end up writing to the same used-ring slot
     * and the guest observes spurious / duplicated descriptor indices
     * ("request id N is not head"). Moving the BBs first ensures the whole
     * post-rebind data path is single-threaded on the new ctx.
     *
     * Matches virtio_scsi_hotplug(), and virtio-blk's start path which also
     * calls blk_set_aio_context() before virtio_blk_ioeventfd_attach().
     *
     * BQL serializes against qbus child add/remove, so plain QTAILQ_FOREACH
     * is sufficient; no rcu_read_lock() needed (blk_set_aio_context() can
     * yield during drain and must not run inside an RCU read section).
     */
    new_ctx = s->vq_aio_context[VIRTIO_SCSI_VQ_NUM_FIXED];
    QTAILQ_FOREACH(kid, &s->bus.qbus.children, sibling) {
        SCSIDevice *sd = SCSI_DEVICE(kid->child);
        if (sd->conf.blk) {
            int ret = blk_set_aio_context(sd->conf.blk, new_ctx, NULL);
            if (ret < 0) {
                warn_report("virtio-scsi: failed to set AioContext "
                            "for LUN %d/%d/%d",
                            sd->channel, sd->id, sd->lun);
            }
        }
    }

    /* Reattach host notifiers to the new AioContexts. After this point
     * the new ctx will see kicks and start popping descriptors. */
    for (i = 0; i < vs->conf.num_queues; i++) {
        AioContext *ctx = s->vq_aio_context[VIRTIO_SCSI_VQ_NUM_FIXED + i];
        virtio_queue_aio_attach_host_notifier(vs->cmd_vqs[i], ctx);
    }

    s->dataplane_stopping = false;
}
