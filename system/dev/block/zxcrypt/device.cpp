// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <threads.h>

#include <ddk/debug.h>
#include <ddk/device.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/atomic.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <fbl/unique_ptr.h>
#include <lib/zx/port.h>
#include <lib/zx/vmar.h>
#include <lib/zx/vmo.h>
#include <zircon/compiler.h>
#include <zircon/device/block.h>
#include <zircon/errors.h>
#include <zircon/status.h>
#include <zircon/thread_annotations.h>
#include <zircon/types.h>
#include <zxcrypt/volume.h>

#include "debug.h"
#include "device.h"
#include "extra.h"
#include "worker.h"

namespace zxcrypt {
namespace {

// Cap largest transaction to a quarter of the VMO buffer.
const uint32_t kMaxTransferSize = Volume::kBufferSize / 4;

// Kick off |Init| thread when binding.
int InitThread(void* arg) {
    return static_cast<Device*>(arg)->Init();
}

} // namespace

// Public methods

Device::Device(zx_device_t* parent)
    : DeviceType(parent), active_(false), stalled_(false), num_ops_(0), info_(nullptr), hint_(0) {
    LOG_ENTRY();

    list_initialize(&queue_);
}

Device::~Device() {
    LOG_ENTRY();
}

// Public methods called from global context

zx_status_t Device::Bind() {
    LOG_ENTRY();
    ZX_DEBUG_ASSERT(!info_);
    zx_status_t rc;

    // Add the (invisible) device to devmgr
    if ((rc = DdkAdd("zxcrypt", DEVICE_ADD_INVISIBLE)) != ZX_OK) {
        zxlogf(ERROR, "DdkAdd('zxcrypt', DEVICE_ADD_INVISIBLE) failed: %s\n",
               zx_status_get_string(rc));
        return rc;
    }
    auto cleanup = fbl::MakeAutoCall([this] { DdkRemove(); });

    // Launch the init thread.
    if (thrd_create(&init_, InitThread, this) != thrd_success) {
        zxlogf(ERROR, "zxcrypt device %p initialization aborted: failed to start thread\n", this);
        return ZX_ERR_INTERNAL;
    }

    cleanup.cancel();
    return ZX_OK;
}

zx_status_t Device::Init() {
    LOG_ENTRY();
    ZX_DEBUG_ASSERT(!info_);
    zx_status_t rc;

    zxlogf(TRACE, "zxcrypt device %p initializing\n", this);
    auto cleanup = fbl::MakeAutoCall([this]() {
        zxlogf(ERROR, "zxcrypt device %p failed to initialize\n", this);
        DdkRemove();
    });

    fbl::AllocChecker ac;
    fbl::unique_ptr<DeviceInfo> info(new (&ac) DeviceInfo());
    if (!ac.check()) {
        zxlogf(ERROR, "failed to allocate %zu bytes\n", sizeof(DeviceInfo));
        return ZX_ERR_NO_MEMORY;
    }
    info->base = nullptr;
    info->num_workers = 0;
    info_ = info.get();

    // Open the zxcrypt volume.  The volume may adjust the block info, so get it again and determine
    // the multiplicative factor needed to transform this device's blocks into its parent's.
    // TODO(security): ZX-1130 workaround.  Use null key of a fixed length until fixed
    crypto::Secret root_key;
    uint8_t* buf;
    if ((rc = root_key.Allocate(kZx1130KeyLen, &buf)) != ZX_OK) {
        zxlogf(ERROR, "failed to key of %zu bytes: %s\n", kZx1130KeyLen, zx_status_get_string(rc));
        return rc;
    }
    memset(buf, 0, root_key.len());
    fbl::unique_ptr<Volume> volume;
    if ((rc = Volume::Unlock(parent(), root_key, 0, &volume)) != ZX_OK) {
        zxlogf(ERROR, "failed to unlock volume: %s\n", zx_status_get_string(rc));
        return rc;
    }

    // Get the parent device's block interface
    block_info_t blk;
    if ((rc = device_get_protocol(parent(), ZX_PROTOCOL_BLOCK, &info->proto)) != ZX_OK) {
        zxlogf(ERROR, "failed to get block protocol: %s\n", zx_status_get_string(rc));
        return rc;
    }
    info->proto.ops->query(info->proto.ctx, &blk, &info->op_size);

    // Save device sizes
    info->block_size = blk.block_size;
    info->op_size += sizeof(extra_op_t);
    info->reserved_blocks = volume->reserved_blocks();
    info->reserved_slices = volume->reserved_slices();

    // Reserve space for shadow I/O transactions
    if ((rc = zx::vmo::create(Volume::kBufferSize, 0, &info->vmo)) != ZX_OK) {
        zxlogf(ERROR, "zx::vmo::create failed: %s\n", zx_status_get_string(rc));
        return rc;
    }
    constexpr uint32_t flags = ZX_VM_PERM_READ | ZX_VM_PERM_WRITE;
    uintptr_t address;
    if ((rc = zx::vmar::root_self()->map(0, info->vmo, 0, Volume::kBufferSize, flags, &address)) !=
        ZX_OK) {
        zxlogf(ERROR, "zx::vmar::map failed: %s\n", zx_status_get_string(rc));
        return rc;
    }
    info->base = reinterpret_cast<uint8_t*>(address);

    // Set up allocation bitmap
    {
        fbl::AutoLock lock(&mtx_);
        rc = map_.Reset(Volume::kBufferSize / info->block_size);
    }
    if (rc != ZX_OK) {
        zxlogf(ERROR, "bitmap allocation failed: %s\n", zx_status_get_string(rc));
        return rc;
    }

    // Start workers
    // TODO(aarongreen): Investigate performance implications of adding more workers.
    if ((rc = zx::port::create(0, &port_)) != ZX_OK) {
        zxlogf(ERROR, "zx::port::create failed: %s\n", zx_status_get_string(rc));
        return rc;
    }
    for (size_t i = 0; i < kNumWorkers; ++i) {
        zx::port port;
        port_.duplicate(ZX_RIGHT_SAME_RIGHTS, &port);
        if ((rc = workers_[i].Start(this, *volume, fbl::move(port))) != ZX_OK) {
            zxlogf(ERROR, "failed to start worker %zu: %s\n", i, zx_status_get_string(rc));
            return rc;
        }
        ++info->num_workers;
    }

    // |info_| now holds the pointer; it is reclaimed in |DdkRelease|.
    DeviceInfo* released __attribute__((unused)) = info.release();

    // Enable the device
    active_.store(true);
    DdkMakeVisible();
    zxlogf(TRACE, "zxcrypt device %p initialized\n", this);

    cleanup.cancel();
    return ZX_OK;
}

////////////////////////////////////////////////////////////////
// ddk::Device methods

zx_status_t Device::DdkIoctl(uint32_t op, const void* in, size_t in_len, void* out, size_t out_len,
                             size_t* actual) {
    LOG_ENTRY_ARGS("op=%0x" PRIx32 ", in=%p, in_len=%zu, out=%p, out_len=%zu, actual=%p", op, in,
                   in_len, out, out_len, actual);
    ZX_DEBUG_ASSERT(info_);
    zx_status_t rc;

    // Modify inputs
    switch (op) {
    case IOCTL_BLOCK_FVM_EXTEND:
    case IOCTL_BLOCK_FVM_SHRINK: {
        extend_request_t mod;
        if (!in || in_len < sizeof(mod)) {
            zxlogf(ERROR, "bad parameter(s): in=%p, in_len=%zu\n", in, in_len);
            return ZX_ERR_INVALID_ARGS;
        }
        memcpy(&mod, in, sizeof(mod));
        mod.offset += info_->reserved_slices;
        rc = device_ioctl(parent(), op, &mod, sizeof(mod), out, out_len, actual);
        break;
    }
    case IOCTL_BLOCK_FVM_VSLICE_QUERY: {
        query_request_t mod;
        if (!in || in_len < sizeof(mod)) {
            zxlogf(ERROR, "bad parameter(s): in=%p, in_len=%zu\n", in, in_len);
            return ZX_ERR_INVALID_ARGS;
        }
        memcpy(&mod, in, sizeof(mod));
        for (size_t i = 0; i < mod.count; ++i) {
            mod.vslice_start[i] += info_->reserved_slices;
        }
        rc = device_ioctl(parent(), op, &mod, sizeof(mod), out, out_len, actual);
        break;
    }
    default:
        rc = device_ioctl(parent(), op, in, in_len, out, out_len, actual);
        break;
    }
    if (rc < 0) {
        zxlogf(ERROR, "parent device returned failure for ioctl 0x%" PRIx32 ": %s\n", op,
               zx_status_get_string(rc));
        return rc;
    }

    // Modify outputs
    switch (op) {
    case IOCTL_BLOCK_GET_INFO: {
        block_info_t* mod = static_cast<block_info_t*>(out);
        mod->block_count -= info_->reserved_blocks;
        if (mod->max_transfer_size > kMaxTransferSize) {
            mod->max_transfer_size = kMaxTransferSize;
        }
        break;
    }
    case IOCTL_BLOCK_FVM_QUERY: {
        fvm_info_t* mod = static_cast<fvm_info_t*>(out);
        mod->vslice_count -= info_->reserved_slices;
        break;
    }
    default:
        break;
    }
    return ZX_OK;
}

zx_off_t Device::DdkGetSize() {
    LOG_ENTRY();

    zx_off_t reserved, size;
    if (mul_overflow(info_->block_size, info_->reserved_blocks, &reserved) ||
        sub_overflow(device_get_size(parent()), reserved, &size)) {
        zxlogf(ERROR, "device_get_size returned less than what has been reserved\n");
        return 0;
    }

    return size;
}

// TODO(aarongreen): See ZX-1138.  Currently, there's no good way to trigger
// this on demand.
void Device::DdkUnbind() {
    LOG_ENTRY();
    active_.store(false);
    DdkRemove();
}

void Device::DdkRelease() {
    LOG_ENTRY();
    zx_status_t rc;

    // One way or another we need to release the memory
    auto cleanup = fbl::MakeAutoCall([this]() {
        zxlogf(TRACE, "zxcrypt device %p released\n", this);
        delete this;
    });

    // Make sure |Init()| is complete
    thrd_join(init_, &rc);
    if (rc != ZX_OK) {
        zxlogf(WARN, "init thread returned %s\n", zx_status_get_string(rc));
    }

    // If we died early enough (e.g. OOM), this doesn't exist
    if (!info_) {
        return;
    }

    // Stop workers; send a stop message to each, then join each (possibly in different order).
    StopWorkersIfDone();
    for (size_t i = 0; i < info_->num_workers; ++i) {
        workers_[i].Stop();
    }

    // Reclaim |info_| to ensure its memory is freed.
    fbl::unique_ptr<DeviceInfo> info(const_cast<DeviceInfo*>(info_));

    // Release write buffer
    const uintptr_t address = reinterpret_cast<uintptr_t>(info->base);
    if (address != 0 &&
        (rc = zx::vmar::root_self()->unmap(address, Volume::kBufferSize)) != ZX_OK) {
        zxlogf(WARN, "failed to unmap %" PRIu32 " bytes at %" PRIuPTR ": %s\n", Volume::kBufferSize,
               address, zx_status_get_string(rc));
    }
}

////////////////////////////////////////////////////////////////
// ddk::BlockProtocol methods

void Device::BlockImplQuery(block_info_t* out_info, size_t* out_op_size) {
    LOG_ENTRY_ARGS("out_info=%p, out_op_size=%p", out_info, out_op_size);
    ZX_DEBUG_ASSERT(info_);

    info_->proto.ops->query(info_->proto.ctx, out_info, out_op_size);
    out_info->block_count -= info_->reserved_blocks;
    *out_op_size = info_->op_size;
}

void Device::BlockImplQueue(block_op_t* block, block_impl_queue_callback completion_cb,
                            void* cookie) {
    LOG_ENTRY_ARGS("block=%p", block);
    ZX_DEBUG_ASSERT(info_);

    // Check if the device is active.
    if (!active_.load()) {
        zxlogf(ERROR, "rejecting I/O request: device is not active\n");
        completion_cb(cookie, ZX_ERR_BAD_STATE, block);
        return;
    }
    num_ops_.fetch_add(1);

    // Initialize our extra space and save original values
    extra_op_t* extra = BlockToExtra(block, info_->op_size);
    zx_status_t rc = extra->Init(block, completion_cb, cookie, info_->reserved_blocks);
    if (rc != ZX_OK) {
        zxlogf(ERROR, "failed to initialize extra info: %s\n", zx_status_get_string(rc));
        BlockComplete(block, rc);
        return;
    }

    switch (block->command & BLOCK_OP_MASK) {
    case BLOCK_OP_WRITE:
        EnqueueWrite(block);
        break;
    case BLOCK_OP_READ:
    default:
        BlockForward(block, ZX_OK);
        break;
    }
}

void Device::BlockForward(block_op_t* block, zx_status_t status) {
    LOG_ENTRY_ARGS("block=%p, status=%s", block, zx_status_get_string(status));
    ZX_DEBUG_ASSERT(info_);

    if (!block) {
        zxlogf(SPEW, "early return; no block provided\n");
        return;
    }
    if (status != ZX_OK) {
        zxlogf(ERROR, "aborting request due to failure: %s\n", zx_status_get_string(status));
        BlockComplete(block, status);
        return;
    }
    // Check if the device is active (i.e. |DdkUnbind| has not been called).
    if (!active_.load()) {
        zxlogf(ERROR, "aborting request; device is not active\n");
        BlockComplete(block, ZX_ERR_BAD_STATE);
        return;
    }

    // Send the request to the parent device
    block_impl_queue(&info_->proto, block, BlockCallback, this);
}

void Device::BlockComplete(block_op_t* block, zx_status_t status) {
    LOG_ENTRY_ARGS("block=%p, status=%s", block, zx_status_get_string(status));
    ZX_DEBUG_ASSERT(info_);
    zx_status_t rc;

    // If a portion of the write buffer was allocated, release it.
    extra_op_t* extra = BlockToExtra(block, info_->op_size);
    if (extra->data) {
        uint64_t off = (extra->data - info_->base) / info_->block_size;
        uint64_t len = block->rw.length;
        extra->data = nullptr;

        fbl::AutoLock lock(&mtx_);
        ZX_DEBUG_ASSERT(map_.Get(off, off + len));
        rc = map_.Clear(off, off + len);
        ZX_DEBUG_ASSERT(rc == ZX_OK);
    }

    // Complete the request.
    extra->completion_cb(extra->cookie, status, block);

    // If we previously stalled, try to re-queue the deferred requests; otherwise, avoid taking the
    // lock.
    if (stalled_.exchange(false)) {
        EnqueueWrite();
    }

    if (num_ops_.fetch_sub(1) == 1) {
        StopWorkersIfDone();
    }
}

////////////////////////////////////////////////////////////////
// Private methods

void Device::EnqueueWrite(block_op_t* block) {
    LOG_ENTRY_ARGS("block=%p", block);
    zx_status_t rc = ZX_OK;

    fbl::AutoLock lock(&mtx_);

    // Append the request to the write queue (if not null)
    extra_op_t* extra;
    if (block) {
        extra = BlockToExtra(block, info_->op_size);
        list_add_tail(&queue_, &extra->node);
    }
    if (stalled_.load()) {
        zxlogf(SPEW, "early return; no requests completed since last stall\n");
        return;
    }

    // Process as many pending write requests as we can right now.
    list_node_t pending;
    list_initialize(&pending);
    while (!list_is_empty(&queue_)) {
        extra = list_peek_head_type(&queue_, extra_op_t, node);
        block = ExtraToBlock(extra, info_->op_size);

        // Find an available offset in the write buffer
        uint64_t off;
        uint64_t len = block->rw.length;
        if ((rc = map_.Find(false, hint_, map_.size(), len, &off)) == ZX_ERR_NO_RESOURCES &&
            (rc = map_.Find(false, 0, map_.size(), len, &off)) == ZX_ERR_NO_RESOURCES) {
            zxlogf(TRACE, "zxcrypt device %p stalled pending request completion\n", this);
            stalled_.store(true);
            break;
        }

        // We don't expect any other errors
        ZX_DEBUG_ASSERT(rc == ZX_OK);
        rc = map_.Set(off, off + len);
        ZX_DEBUG_ASSERT(rc == ZX_OK);

        // Save a hint as to where to start looking next time
        hint_ = (off + len) % map_.size();

        // Modify request to use write buffer
        extra->data = info_->base + (off * info_->block_size);
        block->rw.vmo = info_->vmo.get();
        block->rw.offset_vmo = (extra->data - info_->base) / info_->block_size;

        list_add_tail(&pending, list_remove_head(&queue_));
    }

    // Release the lock and send blocks that are ready to the workers
    lock.release();
    extra_op_t* tmp;
    list_for_every_entry_safe (&pending, extra, tmp, extra_op_t, node) {
        list_delete(&extra->node);
        block = ExtraToBlock(extra, info_->op_size);
        SendToWorker(block);
    }
}

void Device::SendToWorker(block_op_t* block) {
    LOG_ENTRY_ARGS("block=%p", block);
    zx_status_t rc;

    zx_port_packet_t packet;
    Worker::MakeRequest(&packet, Worker::kBlockRequest, block);
    if ((rc = port_.queue(&packet)) != ZX_OK) {
        zxlogf(ERROR, "zx::port::queue failed: %s\n", zx_status_get_string(rc));
        BlockComplete(block, rc);
        return;
    }
}

void Device::BlockCallback(void* cookie, zx_status_t status, block_op_t* block) {
    LOG_ENTRY_ARGS("block=%p, status=%s", block, zx_status_get_string(status));

    // Restore data that may have changed
    Device* device = static_cast<Device*>(cookie);
    extra_op_t* extra = BlockToExtra(block, device->op_size());
    block->rw.vmo = extra->vmo;
    block->rw.length = extra->length;
    block->rw.offset_dev = extra->offset_dev;
    block->rw.offset_vmo = extra->offset_vmo;

    if (status != ZX_OK) {
        zxlogf(TRACE, "parent device returned %s\n", zx_status_get_string(status));
        device->BlockComplete(block, status);
        return;
    }
    switch (block->command & BLOCK_OP_MASK) {
    case BLOCK_OP_READ:
        device->SendToWorker(block);
        break;
    case BLOCK_OP_WRITE:
    default:
        device->BlockComplete(block, ZX_OK);
        break;
    }
}

void Device::StopWorkersIfDone() {
    // Multiple threads may pass this check, but that's harmless.
    if (!active_.load() && num_ops_.load() == 0) {
        zx_port_packet_t packet;
        Worker::MakeRequest(&packet, Worker::kStopRequest);
        for (size_t i = 0; i < info_->num_workers; ++i) {
            port_.queue(&packet);
        }
    }
}

} // namespace zxcrypt

extern "C" zx_status_t zxcrypt_device_bind(void* ctx, zx_device_t* parent) {
    LOG_ENTRY_ARGS("ctx=%p, parent=%p", ctx, parent);
    zx_status_t rc;

    fbl::AllocChecker ac;
    auto dev = fbl::make_unique_checked<zxcrypt::Device>(&ac, parent);
    if (!ac.check()) {
        zxlogf(ERROR, "failed to allocate %zu bytes\n", sizeof(zxcrypt::Device));
        return ZX_ERR_NO_MEMORY;
    }
    if ((rc = dev->Bind()) != ZX_OK) {
        zxlogf(ERROR, "failed to bind: %s\n", zx_status_get_string(rc));
        return rc;
    }
    // devmgr is now in charge of the memory for |dev|
    zxcrypt::Device* devmgr_owned __attribute__((unused));
    devmgr_owned = dev.release();

    return ZX_OK;
}
