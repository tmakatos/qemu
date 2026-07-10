/*
 * IOThread Virtqueue Mapping
 *
 * Copyright Red Hat, Inc
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef HW_VIRTIO_IOTHREAD_VQ_MAPPING_H
#define HW_VIRTIO_IOTHREAD_VQ_MAPPING_H

#include "qapi/error.h"
#include "qapi/qapi-types-virtio.h"

/**
 * iothread_vq_mapping_apply:
 * @list: The mapping of virtqueues to IOThreads.
 * @vq_aio_context: The array of AioContext pointers to fill in.
 * @num_queues: The length of @vq_aio_context.
 * @errp: If an error occurs, a pointer to the area to store the error.
 *
 * Fill in the AioContext for each virtqueue in the @vq_aio_context array given
 * the iothread-vq-mapping parameter in @list.
 *
 * iothread_vq_mapping_cleanup() must be called to free IOThread object
 * references after this function returns success.
 *
 * Returns: %true on success, %false on failure.
 **/
bool iothread_vq_mapping_apply(
        IOThreadVirtQueueMappingList *list,
        AioContext **vq_aio_context,
        uint16_t num_queues,
        Error **errp);

/**
 * iothread_vq_mapping_validate:
 * @list: The mapping of virtqueues to IOThreads.
 * @num_queues: Total number of virtqueues to validate against.
 * @errp: If an error occurs, a pointer to the area to store the error.
 *
 * Check that @list is a valid iothread-vq-mapping: no duplicate IOThreads,
 * no out-of-range or duplicate vq indices, and every vq in [0, @num_queues)
 * is assigned exactly once (when explicit vqs are used).
 *
 * Returns: %true on success, %false on failure.
 */
bool iothread_vq_mapping_validate(
        IOThreadVirtQueueMappingList *list,
        uint16_t num_queues,
        Error **errp);

/**
 * iothread_vq_mapping_cleanup:
 * @list: The mapping of virtqueues to IOThreads.
 *
 * Release IOThread object references that were acquired by
 * iothread_vq_mapping_apply().
 */
void iothread_vq_mapping_cleanup(IOThreadVirtQueueMappingList *list);

/**
 * iothread_vq_mapping_apply_runtime:
 * @new_list: The new mapping of virtqueues to IOThreads.
 * @conf_list_ptr: Pointer to the device's conf iothread_vq_mapping_list
 *     field.  On success the old list is freed and replaced with @new_list.
 * @vq_aio_context: The live array of AioContext pointers to update in place.
 * @num_queues: The length of @vq_aio_context.
 * @errp: If an error occurs, a pointer to the area to store the error.
 *
 * Runtime counterpart of iothread_vq_mapping_apply().  Validates @new_list,
 * takes fresh IOThread object references for it, overwrites @vq_aio_context,
 * drops the old IOThread references held via @conf_list_ptr, and swaps
 * @conf_list_ptr to point to @new_list.
 *
 * The caller must ensure all virtqueues have been quiesced (host notifiers
 * detached, requests drained) before calling this function, and must
 * reattach host notifiers to the new AioContexts afterwards.
 *
 * Returns: %true on success, %false on failure (state unchanged).
 */
bool iothread_vq_mapping_apply_runtime(
        IOThreadVirtQueueMappingList *new_list,
        IOThreadVirtQueueMappingList **conf_list_ptr,
        AioContext **vq_aio_context,
        uint16_t num_queues,
        Error **errp);

#endif /* HW_VIRTIO_IOTHREAD_VQ_MAPPING_H */
