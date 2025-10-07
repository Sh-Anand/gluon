#ifndef RADIANCE_DRIVER_H
#define RADIANCE_DRIVER_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <type_traits>
#include <vector>

#include "rad_defs.h"


typedef struct {
    unsigned int x;
    unsigned int y;
    unsigned int z;
} radDim3;

enum radMemCpyDir {
    radMemCpyDir_H2D,
    radMemCpyDir_D2H,
};

enum radCmdType {
    radCmdType_KERNEL,
    radCmdType_MEM,
    radCmdType_CSR,
    radCmdType_FENCE,
    radCmdType_UNDEFINED,
};

enum radMemCmdType {
    radMemCmdType_COPY,
    radMemCmdType_SET,
};

enum radErrorCode {
    radError_NONE,
    radError_EXECUTION,
};

struct radError {
    radErrorCode err_code;
    uint8_t cmd_id;
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

    const std::uint8_t* data() const {
        return storage.empty() ? nullptr : storage.data();
    }

    std::size_t size() const {
        return offset;
    }
};

void radKernelLaunch(const char *kernel_name, radDim3 grid_dim, radDim3 block_dim, radParamBuf* params);

void radMemCpy(void *dst, void *src, size_t bytes, radMemCpyDir dir);

void radMalloc(void **ptr, size_t bytes);

void radGetError(radError *err);

#endif  // RADIANCE_DRIVER_H
