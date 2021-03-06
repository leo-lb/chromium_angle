//
// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// BufferVk.cpp:
//    Implements the class methods for BufferVk.
//

#include "libANGLE/renderer/vulkan/BufferVk.h"

#include "common/debug.h"
#include "common/utilities.h"
#include "libANGLE/Context.h"
#include "libANGLE/renderer/vulkan/ContextVk.h"
#include "libANGLE/renderer/vulkan/RendererVk.h"
#include "third_party/trace_event/trace_event.h"

namespace rx
{

namespace
{
// Vertex attribute buffers are used as storage buffers for conversion in compute, where access to
// the buffer is made in 4-byte chunks.  Assume the size of the buffer is 4k+n where n is in [0, 3).
// On some hardware, reading 4 bytes from address 4k returns 0, making it impossible to read the
// last n bytes.  By rounding up the buffer sizes to a multiple of 4, the problem is alleviated.
constexpr size_t kBufferSizeGranularity = 4;

// Start with a fairly small buffer size. We can increase this dynamically as we convert more data.
constexpr size_t kConvertedArrayBufferInitialSize = 1024 * 8;
}  // namespace

// ConversionBuffer implementation.
ConversionBuffer::ConversionBuffer(RendererVk *renderer,
                                   VkBufferUsageFlags usageFlags,
                                   size_t initialSize,
                                   size_t alignment)
    : dirty(true), lastAllocationOffset(0), data(usageFlags, initialSize, true)
{
    data.init(alignment, renderer);
}

ConversionBuffer::~ConversionBuffer() = default;

ConversionBuffer::ConversionBuffer(ConversionBuffer &&other) = default;

// BufferVk::VertexConversionBuffer implementation.
BufferVk::VertexConversionBuffer::VertexConversionBuffer(RendererVk *renderer,
                                                         angle::FormatID formatIDIn,
                                                         GLuint strideIn,
                                                         size_t offsetIn)
    : ConversionBuffer(renderer,
                       vk::kVertexBufferUsageFlags,
                       kConvertedArrayBufferInitialSize,
                       vk::kVertexBufferAlignment),
      formatID(formatIDIn),
      stride(strideIn),
      offset(offsetIn)
{}

BufferVk::VertexConversionBuffer::VertexConversionBuffer(VertexConversionBuffer &&other) = default;

BufferVk::VertexConversionBuffer::~VertexConversionBuffer() = default;

// BufferVk implementation.
BufferVk::BufferVk(const gl::BufferState &state) : BufferImpl(state), mDataWriteAccessFlags(0) {}

BufferVk::~BufferVk() {}

void BufferVk::destroy(const gl::Context *context)
{
    ContextVk *contextVk = vk::GetImpl(context);

    release(contextVk);
}

void BufferVk::release(ContextVk *contextVk)
{
    mBuffer.release(contextVk);

    for (ConversionBuffer &buffer : mVertexConversionBuffers)
    {
        buffer.data.release(contextVk);
    }
}

angle::Result BufferVk::setData(const gl::Context *context,
                                gl::BufferBinding target,
                                const void *data,
                                size_t size,
                                gl::BufferUsage usage)
{
    ContextVk *contextVk = vk::GetImpl(context);

    if (size > static_cast<size_t>(mState.getSize()))
    {
        // Release and re-create the memory and buffer.
        release(contextVk);

        // We could potentially use multiple backing buffers for different usages.
        // For now keep a single buffer with all relevant usage flags.
        const VkImageUsageFlags usageFlags =
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;

        VkBufferCreateInfo createInfo    = {};
        createInfo.sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        createInfo.flags                 = 0;
        createInfo.size                  = roundUp(size, kBufferSizeGranularity);
        createInfo.usage                 = usageFlags;
        createInfo.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.queueFamilyIndexCount = 0;
        createInfo.pQueueFamilyIndices   = nullptr;

        // Assume host visible/coherent memory available.
        const VkMemoryPropertyFlags memoryPropertyFlags =
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        ANGLE_TRY(mBuffer.init(contextVk, createInfo, memoryPropertyFlags));
    }

    if (data && size > 0)
    {
        ANGLE_TRY(setDataImpl(contextVk, static_cast<const uint8_t *>(data), size, 0));
    }

    return angle::Result::Continue;
}

angle::Result BufferVk::setSubData(const gl::Context *context,
                                   gl::BufferBinding target,
                                   const void *data,
                                   size_t size,
                                   size_t offset)
{
    ASSERT(mBuffer.valid());

    ContextVk *contextVk = vk::GetImpl(context);
    ANGLE_TRY(setDataImpl(contextVk, static_cast<const uint8_t *>(data), size, offset));

    return angle::Result::Continue;
}

angle::Result BufferVk::copySubData(const gl::Context *context,
                                    BufferImpl *source,
                                    GLintptr sourceOffset,
                                    GLintptr destOffset,
                                    GLsizeiptr size)
{
    ANGLE_VK_UNREACHABLE(vk::GetImpl(context));
    return angle::Result::Stop;
}

angle::Result BufferVk::map(const gl::Context *context, GLenum access, void **mapPtr)
{
    ASSERT(mBuffer.valid());

    ContextVk *contextVk = vk::GetImpl(context);
    return mapImpl(contextVk, mapPtr);
}

angle::Result BufferVk::mapImpl(ContextVk *contextVk, void **mapPtr)
{
    ANGLE_VK_TRY(contextVk,
                 mBuffer.getDeviceMemory().map(contextVk->getDevice(), 0, mState.getSize(), 0,
                                               reinterpret_cast<uint8_t **>(mapPtr)));
    return angle::Result::Continue;
}

angle::Result BufferVk::mapRange(const gl::Context *context,
                                 size_t offset,
                                 size_t length,
                                 GLbitfield access,
                                 void **mapPtr)
{
    ASSERT(mBuffer.valid());

    ContextVk *contextVk = vk::GetImpl(context);

    ANGLE_VK_TRY(contextVk, mBuffer.getDeviceMemory().map(contextVk->getDevice(), offset, length, 0,
                                                          reinterpret_cast<uint8_t **>(mapPtr)));
    return angle::Result::Continue;
}

angle::Result BufferVk::unmap(const gl::Context *context, GLboolean *result)
{
    return unmapImpl(vk::GetImpl(context));
}

angle::Result BufferVk::unmapImpl(ContextVk *contextVk)
{
    ASSERT(mBuffer.valid());

    mBuffer.getDeviceMemory().unmap(contextVk->getDevice());
    mDataWriteAccessFlags = VK_ACCESS_HOST_WRITE_BIT;

    markConversionBuffersDirty();

    return angle::Result::Continue;
}

angle::Result BufferVk::onRead(ContextVk *contextVk,
                               vk::CommandGraphResource *reader,
                               VkAccessFlagBits readAccessType)
{
    // Now that the buffer helper is being used (and will be part of the command graph), make sure
    // its data write barrier is executed.
    if (mDataWriteAccessFlags != 0)
    {
        vk::CommandBuffer *commandBuffer;
        ANGLE_TRY(mBuffer.recordCommands(contextVk, &commandBuffer));

        mBuffer.onWrite(mDataWriteAccessFlags);
        mDataWriteAccessFlags = 0;
    }

    mBuffer.onRead(reader, readAccessType);

    return angle::Result::Continue;
}

angle::Result BufferVk::getIndexRange(const gl::Context *context,
                                      gl::DrawElementsType type,
                                      size_t offset,
                                      size_t count,
                                      bool primitiveRestartEnabled,
                                      gl::IndexRange *outRange)
{
    ContextVk *contextVk = vk::GetImpl(context);
    RendererVk *renderer = contextVk->getRenderer();

    // This is a workaround for the mock ICD not implementing buffer memory state.
    // Could be removed if https://github.com/KhronosGroup/Vulkan-Tools/issues/84 is fixed.
    if (renderer->isMockICDEnabled())
    {
        outRange->start = 0;
        outRange->end   = 0;
        return angle::Result::Continue;
    }

    TRACE_EVENT0("gpu.angle", "BufferVk::getIndexRange");
    // Needed before reading buffer or we could get stale data.
    ANGLE_TRY(contextVk->finishImpl());

    // TODO(jmadill): Consider keeping a shadow system memory copy in some cases.
    ASSERT(mBuffer.valid());

    const GLuint &typeBytes = gl::GetDrawElementsTypeSize(type);

    uint8_t *mapPointer = nullptr;
    ANGLE_VK_TRY(contextVk, mBuffer.getDeviceMemory().map(contextVk->getDevice(), offset,
                                                          typeBytes * count, 0, &mapPointer));

    *outRange = gl::ComputeIndexRange(type, mapPointer, count, primitiveRestartEnabled);

    mBuffer.getDeviceMemory().unmap(contextVk->getDevice());
    return angle::Result::Continue;
}

angle::Result BufferVk::setDataImpl(ContextVk *contextVk,
                                    const uint8_t *data,
                                    size_t size,
                                    size_t offset)
{
    VkDevice device = contextVk->getDevice();

    // Use map when available.
    if (mBuffer.isResourceInUse(contextVk))
    {
        vk::StagingBuffer stagingBuffer;
        ANGLE_TRY(stagingBuffer.init(contextVk, static_cast<VkDeviceSize>(size),
                                     vk::StagingUsage::Write));

        uint8_t *mapPointer = nullptr;
        ANGLE_VK_TRY(contextVk,
                     stagingBuffer.getDeviceMemory().map(device, 0, size, 0, &mapPointer));
        ASSERT(mapPointer);

        memcpy(mapPointer, data, size);
        stagingBuffer.getDeviceMemory().unmap(device);

        // Enqueue a copy command on the GPU.
        VkBufferCopy copyRegion = {0, offset, size};
        ANGLE_TRY(mBuffer.copyFromBuffer(contextVk, stagingBuffer.getBuffer(),
                                         VK_ACCESS_HOST_WRITE_BIT, copyRegion));

        // Immediately release staging buffer. We should probably be using a DynamicBuffer here.
        contextVk->releaseObject(contextVk->getCurrentQueueSerial(), &stagingBuffer);
    }
    else
    {
        uint8_t *mapPointer = nullptr;
        ANGLE_VK_TRY(contextVk,
                     mBuffer.getDeviceMemory().map(device, offset, size, 0, &mapPointer));
        ASSERT(mapPointer);

        memcpy(mapPointer, data, size);

        mBuffer.getDeviceMemory().unmap(device);
        mDataWriteAccessFlags = VK_ACCESS_HOST_WRITE_BIT;
    }

    // Update conversions
    markConversionBuffersDirty();

    return angle::Result::Continue;
}

angle::Result BufferVk::copyToBuffer(ContextVk *contextVk,
                                     vk::BufferHelper *destBuffer,
                                     uint32_t copyCount,
                                     const VkBufferCopy *copies)
{
    vk::CommandBuffer *commandBuffer;
    ANGLE_TRY(destBuffer->recordCommands(contextVk, &commandBuffer));
    commandBuffer->copyBuffer(mBuffer.getBuffer(), destBuffer->getBuffer(), copyCount, copies);

    mBuffer.onRead(destBuffer, VK_ACCESS_TRANSFER_READ_BIT);
    destBuffer->onWrite(VK_ACCESS_TRANSFER_WRITE_BIT);

    return angle::Result::Continue;
}

ConversionBuffer *BufferVk::getVertexConversionBuffer(RendererVk *renderer,
                                                      angle::FormatID formatID,
                                                      GLuint stride,
                                                      size_t offset)
{
    for (VertexConversionBuffer &buffer : mVertexConversionBuffers)
    {
        if (buffer.formatID == formatID && buffer.stride == stride && buffer.offset == offset)
        {
            return &buffer;
        }
    }

    mVertexConversionBuffers.emplace_back(renderer, formatID, stride, offset);
    return &mVertexConversionBuffers.back();
}

void BufferVk::markConversionBuffersDirty()
{
    for (VertexConversionBuffer &buffer : mVertexConversionBuffers)
    {
        buffer.dirty = true;
    }
}

}  // namespace rx
