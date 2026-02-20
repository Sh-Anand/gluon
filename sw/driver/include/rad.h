#ifndef RADIANCE_DRIVER_H
#define RADIANCE_DRIVER_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <type_traits>
#include <vector>

#include "rad_defs.h"
#include "command.hpp"

typedef struct {
    unsigned int x;
    unsigned int y;
    unsigned int z;
} radDim3;

enum radErrorCode {
    radError_NONE,
    radError_EXECUTION,
};

enum radMemCpyDir {
    radMemCpyDir_H2D,
    radMemCpyDir_D2H,
};

struct radError {
    radErrorCode err_code;
    uint8_t cmd_id;
    uint32_t pc;
};

struct radParamBuf {
    std::vector<std::uint8_t> storage;
    std::size_t offset = 0;

    void reset() {
        storage.clear();
        offset = 0;
    }

    template <class T>
    void push(const T& value) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "radParamBuf::push requires trivially copyable types");
        constexpr std::size_t alignment = alignof(T);
        constexpr std::size_t size = sizeof(T);
        if (alignment > 1) {
            offset = (offset + (alignment - 1)) & ~(alignment - 1);
        }
        std::size_t end = offset + size;
        if (storage.size() < end) {
            storage.resize(end);
        }
        std::memcpy(storage.data() + offset, &value, size);
        offset = end;
    }

    void push(void *ptr) {
        push((uint32_t)((uintptr_t)ptr));
    }

    const std::uint8_t* data() const {
        return storage.empty() ? nullptr : storage.data();
    }

    std::size_t size() const {
        return offset;
    }
};

struct radStream {
    std::size_t id;
    uint8_t hw_sid;
    CommandStream command_stream;
};

void radKernelLaunch(const char *kernel_name, radDim3 grid_dim, radDim3 block_dim, radParamBuf* params, radStream* stream = nullptr);

void radMemCpy(void *dst, void *src, size_t bytes, radMemCpyDir dir, radStream* stream = nullptr);

void radMalloc(void **ptr, size_t bytes, radStream* stream = nullptr);

void radGetError(radError *err, radStream* stream = nullptr);

void radCreateStream(radStream* stream);

#endif  // RADIANCE_DRIVER_H
