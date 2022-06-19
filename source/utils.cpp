#include "utils.h"
#include "logger.h"
#include "mocha/commands.h"
#include "mocha/mocha.h"
#include <coreinit/ios.h>
#include <cstring>
#include <malloc.h>
#include <stdint.h>

int iosuhaxHandle        = -1;
int mochaInitDone        = 0;
uint32_t mochaApiVersion = 0;

#define IOCTL_MEM_WRITE        0x00
#define IOCTL_MEM_READ         0x01
#define IOCTL_SVC              0x02
#define IOCTL_MEMCPY           0x04
#define IOCTL_REPEATED_WRITE   0x05
#define IOCTL_KERN_READ32      0x06
#define IOCTL_KERN_WRITE32     0x07
#define IOCTL_READ_OTP         0x08
#define IOCTL_CHECK_IF_IOSUHAX 0x5B
#define IOSUHAX_MAGIC_WORD     0x4E696365

MochaUtilsStatus Mocha_SimpleCommand(uint32_t command, uint32_t apiVersion);

MochaUtilsStatus Mocha_InitLibrary() {
    if (mochaInitDone) {
        return MOCHA_RESULT_SUCCESS;
    }

    if (iosuhaxHandle < 0) {
        int haxHandle = IOS_Open((char *) ("/dev/iosuhax"), static_cast<IOSOpenMode>(0));
        if (haxHandle < 0) {
            DEBUG_FUNCTION_LINE_ERR("Failed to open /dev/iosuhax");
            return MOCHA_RESULT_UNSUPPORTED_COMMAND;
        }
        iosuhaxHandle = haxHandle;
    }

    mochaInitDone    = 1;
    mochaApiVersion  = 0;
    uint32_t version = 0;
    if (Mocha_CheckAPIVersion(&version) != MOCHA_RESULT_SUCCESS) {
        IOS_Close(iosuhaxHandle);
        iosuhaxHandle = -1;
        return MOCHA_RESULT_UNSUPPORTED_COMMAND;
    }

    mochaApiVersion = version;

    return MOCHA_RESULT_SUCCESS;
}

MochaUtilsStatus Mocha_DeinitLibrary() {
    mochaInitDone   = 0;
    mochaApiVersion = 0;

    if (iosuhaxHandle >= 0) {
        IOS_Close(iosuhaxHandle);
    }

    return MOCHA_RESULT_SUCCESS;
}

MochaUtilsStatus Mocha_CheckAPIVersion(uint32_t *version) {
    if (!version) {
        return MOCHA_RESULT_INVALID_ARGUMENT;
    }
    MochaUtilsStatus res = MOCHA_RESULT_UNKNOWN_ERROR;
    int mcpFd            = IOS_Open("/dev/mcp", IOS_OPEN_READ);
    if (mcpFd >= 0) {
        ALIGN_0x40 uint32_t io_buffer[0x100 / 4];
        io_buffer[0] = IPC_CUSTOM_GET_MOCHA_API_VERSION;

        if (IOS_Ioctl(mcpFd, 100, io_buffer, 4, io_buffer, 4) == IOS_ERROR_OK) {
            *version = io_buffer[0];
            res      = MOCHA_RESULT_SUCCESS;
        } else {
            res = MOCHA_RESULT_UNSUPPORTED_API_VERSION;
        }

        IOS_Close(mcpFd);
    } else {
        return res;
    }

    return res;
}

MochaUtilsStatus Mocha_IOSUKernelMemcpy(uint32_t dst, uint32_t src, uint32_t size) {
    if (size == 0) {
        return MOCHA_RESULT_SUCCESS;
    }
    if (dst == 0 || src == 0) {
        return MOCHA_RESULT_INVALID_ARGUMENT;
    }
    if (!mochaInitDone || iosuhaxHandle < 0) {
        return MOCHA_RESULT_LIB_UNINITIALIZED;
    }

    ALIGN_0x40 uint32_t io_buf[0x40 >> 2];
    io_buf[0] = dst;
    io_buf[1] = src;
    io_buf[2] = size;

    int res = IOS_Ioctl(iosuhaxHandle, IOCTL_MEMCPY, io_buf, 3 * sizeof(uint32_t), nullptr, 0);
    return res >= 0 ? MOCHA_RESULT_SUCCESS : MOCHA_RESULT_UNKNOWN_ERROR;
}

MochaUtilsStatus Mocha_IOSUKernelWrite(uint32_t address, const uint8_t *buffer, uint32_t size) {
    if (size == 0) {
        return MOCHA_RESULT_SUCCESS;
    }
    if (address == 0 || buffer == nullptr) {
        return MOCHA_RESULT_INVALID_ARGUMENT;
    }
    if (!mochaInitDone || iosuhaxHandle < 0) {
        return MOCHA_RESULT_LIB_UNINITIALIZED;
    }

    auto *io_buf = (uint32_t *) memalign(0x40, ROUNDUP(size + 4, 0x40));
    if (!io_buf) {
        return MOCHA_RESULT_OUT_OF_MEMORY;
    }

    io_buf[0] = address;
    memcpy(io_buf + 1, buffer, size);

    int res = IOS_Ioctl(iosuhaxHandle, IOCTL_MEM_WRITE, io_buf, size + 4, nullptr, 0);

    free(io_buf);
    return res >= 0 ? MOCHA_RESULT_SUCCESS : MOCHA_RESULT_UNKNOWN_ERROR;
}

MochaUtilsStatus Mocha_IOSUKernelRead(uint32_t address, uint8_t *out_buffer, uint32_t size) {
    if (size == 0) {
        return MOCHA_RESULT_SUCCESS;
    }
    if (address == 0 || out_buffer == nullptr) {
        return MOCHA_RESULT_INVALID_ARGUMENT;
    }
    if (!mochaInitDone || iosuhaxHandle < 0) {
        return MOCHA_RESULT_LIB_UNINITIALIZED;
    }

    ALIGN_0x40 uint32_t io_buf[0x40 >> 2];
    io_buf[0] = address;

    void *tmp_buf = nullptr;

    if (((uintptr_t) out_buffer & 0x3F) || (size & 0x3F)) {
        tmp_buf = (uint32_t *) memalign(0x40, ROUNDUP(size, 0x40));
        if (!tmp_buf) {
            return MOCHA_RESULT_OUT_OF_MEMORY;
        }
    }

    int res = IOS_Ioctl(iosuhaxHandle, IOCTL_MEM_READ, io_buf, sizeof(address), tmp_buf ? tmp_buf : out_buffer, size);

    if (res >= 0 && tmp_buf) {
        memcpy(out_buffer, tmp_buf, size);
    }

    free(tmp_buf);
    return res >= 0 ? MOCHA_RESULT_SUCCESS : MOCHA_RESULT_UNKNOWN_ERROR;
}

MochaUtilsStatus Mocha_IOSUKernelWrite32(uint32_t address, uint32_t value) {
    return Mocha_IOSUKernelWrite(address, reinterpret_cast<const uint8_t *>(&value), 4);
}

MochaUtilsStatus Mocha_IOSUKernelRead32(uint32_t address, uint32_t *out_buffer) {
    return Mocha_IOSUKernelRead(address, reinterpret_cast<uint8_t *>(out_buffer), 4);
}

MochaUtilsStatus Mocha_ReadOTP(WiiUConsoleOTP *out_buffer) {
    if (out_buffer == nullptr) {
        return MOCHA_RESULT_INVALID_ARGUMENT;
    }
    if (!mochaInitDone || iosuhaxHandle < 0) {
        return MOCHA_RESULT_LIB_UNINITIALIZED;
    }

    ALIGN_0x40 uint32_t io_buf[0x400 >> 2];

    int res = IOS_Ioctl(iosuhaxHandle, IOCTL_READ_OTP, nullptr, 0, io_buf, 0x400);

    if (res < 0) {
        return MOCHA_RESULT_UNKNOWN_ERROR;
    }
    memcpy(out_buffer, io_buf, 0x400);

    return MOCHA_RESULT_SUCCESS;
}

int Mocha_IOSUCallSVC(uint32_t svc_id, uint32_t *args, uint32_t arg_cnt, int32_t *outResult) {
    if (!mochaInitDone || iosuhaxHandle < 0) {
        return MOCHA_RESULT_LIB_UNINITIALIZED;
    }

    ALIGN_0x40 uint32_t arguments[0x40 >> 2];
    arguments[0] = svc_id;

    if (args && arg_cnt) {
        if (arg_cnt > 8) {
            arg_cnt = 8;
        }

        memcpy(arguments + 1, args, arg_cnt * 4);
    }

    ALIGN_0x40 int result[0x40 >> 2];
    int res = IOS_Ioctl(iosuhaxHandle, IOCTL_SVC, arguments, (1 + arg_cnt) * 4, result, 4);

    if (res >= 0) {
        *outResult = *result;
    }

    return res >= 0 ? MOCHA_RESULT_SUCCESS : MOCHA_RESULT_UNKNOWN_ERROR;
}

MochaUtilsStatus Mocha_GetEnvironmentPath(char *environmentPathBuffer, uint32_t bufferLen) {
    if (!mochaInitDone) {
        return MOCHA_RESULT_LIB_UNINITIALIZED;
    }
    if (mochaApiVersion < 1) {
        return MOCHA_RESULT_UNSUPPORTED_COMMAND;
    }
    if (!environmentPathBuffer || bufferLen < 0x100) {
        return MOCHA_RESULT_INVALID_ARGUMENT;
    }
    MochaUtilsStatus res = MOCHA_RESULT_UNKNOWN_ERROR;
    int mcpFd            = IOS_Open("/dev/mcp", (IOSOpenMode) 0);
    if (mcpFd >= 0) {
        ALIGN_0x40 uint32_t io_buffer[0x100 / 4];
        io_buffer[0] = IPC_CUSTOM_COPY_ENVIRONMENT_PATH;

        if (IOS_Ioctl(mcpFd, 100, io_buffer, 4, io_buffer, 0x100) == IOS_ERROR_OK) {
            memcpy(environmentPathBuffer, reinterpret_cast<const char *>(io_buffer), 0xFF);
            res = MOCHA_RESULT_SUCCESS;
        }

        IOS_Close(mcpFd);
    }

    return res;
}

MochaUtilsStatus Mocha_SimpleCommand(uint32_t command, uint32_t apiVersion) {
    if (!mochaInitDone) {
        return MOCHA_RESULT_LIB_UNINITIALIZED;
    }
    if (mochaApiVersion < apiVersion) {
        return MOCHA_RESULT_UNSUPPORTED_COMMAND;
    }
    MochaUtilsStatus res = MOCHA_RESULT_UNKNOWN_ERROR;
    int mcpFd            = IOS_Open("/dev/mcp", (IOSOpenMode) 0);
    if (mcpFd >= 0) {
        ALIGN_0x40 uint32_t io_buffer[0x40 / 4];
        io_buffer[0] = command;

        if (IOS_Ioctl(mcpFd, 100, io_buffer, 4, io_buffer, 0x4) == IOS_ERROR_OK) {
            res = MOCHA_RESULT_SUCCESS;
        }

        IOS_Close(mcpFd);
    }

    return res;
}

MochaUtilsStatus Mocha_RPXHookCompleted() {
    return Mocha_SimpleCommand(IPC_CUSTOM_MEN_RPX_HOOK_COMPLETED, 1);
}

MochaUtilsStatus Mocha_StartUSBLogging(bool avoidLogCatchup) {
    if (!mochaInitDone) {
        return MOCHA_RESULT_LIB_UNINITIALIZED;
    }
    if (mochaApiVersion < 1) {
        return MOCHA_RESULT_UNSUPPORTED_COMMAND;
    }
    MochaUtilsStatus res = MOCHA_RESULT_UNKNOWN_ERROR;
    int mcpFd            = IOS_Open("/dev/mcp", (IOSOpenMode) 0);
    if (mcpFd >= 0) {
        ALIGN_0x40 uint32_t io_buffer[0x40 / 4];
        io_buffer[0] = IPC_CUSTOM_START_USB_LOGGING;
        io_buffer[1] = avoidLogCatchup;

        if (IOS_Ioctl(mcpFd, 100, io_buffer, 8, io_buffer, 0x4) == IOS_ERROR_OK) {
            res = MOCHA_RESULT_SUCCESS;
        }

        IOS_Close(mcpFd);
    }

    return res;
}

MochaUtilsStatus Mocha_UnlockFSClient(FSClient *client) {
    if (!client) {
        return MOCHA_RESULT_INVALID_ARGUMENT;
    }

    return Mocha_UnlockFSClientEx(FSGetClientBody(client)->clientHandle);
}

MochaUtilsStatus Mocha_UnlockFSClientEx(int clientHandle) {
    if (!mochaInitDone) {
        return MOCHA_RESULT_LIB_UNINITIALIZED;
    }
    if (mochaApiVersion < 1) {
        return MOCHA_RESULT_UNSUPPORTED_COMMAND;
    }
    ALIGN_0x40 int dummy[0x40 >> 2];

    auto res = IOS_Ioctl(clientHandle, 0x28, dummy, sizeof(dummy), dummy, sizeof(dummy));
    if (res == 0) {
        return MOCHA_RESULT_SUCCESS;
    }
    if (res == -5) {
        return MOCHA_RESULT_MAX_CLIENT;
    }
    return MOCHA_RESULT_UNKNOWN_ERROR;
}

MochaUtilsStatus Mocha_LoadRPXOnNextLaunch(MochaRPXLoadInfo *loadInfo) {
    if (!mochaInitDone) {
        return MOCHA_RESULT_LIB_UNINITIALIZED;
    }
    if (mochaApiVersion < 1) {
        return MOCHA_RESULT_UNSUPPORTED_COMMAND;
    }
    if (!loadInfo) {
        return MOCHA_RESULT_INVALID_ARGUMENT;
    }
    MochaUtilsStatus res = MOCHA_RESULT_UNKNOWN_ERROR;
    int mcpFd            = IOS_Open("/dev/mcp", (IOSOpenMode) 0);
    if (mcpFd >= 0) {
        ALIGN_0x40 uint32_t io_buffer[ROUNDUP(sizeof(MochaRPXLoadInfo) + 4, 0x40)];
        io_buffer[0] = IPC_CUSTOM_LOAD_CUSTOM_RPX;
        memcpy(&io_buffer[1], loadInfo, sizeof(MochaRPXLoadInfo));

        if (IOS_Ioctl(mcpFd, 100, io_buffer, sizeof(MochaRPXLoadInfo) + 4, io_buffer, 0x4) == IOS_ERROR_OK) {
            res = MOCHA_RESULT_SUCCESS;
        }

        IOS_Close(mcpFd);
    }

    return res;
}

MochaUtilsStatus Mocha_ODMGetDiscKey(WUDDiscKey *discKey) {
    if (!mochaInitDone) {
        return MOCHA_RESULT_LIB_UNINITIALIZED;
    }
    if (mochaApiVersion < 1) {
        return MOCHA_RESULT_UNSUPPORTED_COMMAND;
    }
    if (!discKey) {
        return MOCHA_RESULT_INVALID_ARGUMENT;
    }
    int odm_handle       = IOS_Open("/dev/odm", IOS_OPEN_READ);
    MochaUtilsStatus res = MOCHA_RESULT_UNKNOWN_ERROR;
    if (odm_handle >= 0) {
        ALIGN_0x40 uint32_t io_buffer[0x40 / 4];
        // disc encryption key, only works with patched IOSU
        io_buffer[0] = 3;

        if (IOS_Ioctl(odm_handle, 0x06, io_buffer, 0x14, io_buffer, 0x20) == IOS_ERROR_OK) {
            memcpy(discKey, io_buffer, 16);
            res = MOCHA_RESULT_SUCCESS;
        }
        IOS_Close(odm_handle);
    }
    return res;
}

extern "C" int bspRead(const char *, uint32_t, const char *, uint32_t, uint16_t *);
MochaUtilsStatus Mocha_SEEPROMRead(uint8_t *out_buffer, uint32_t offset, uint32_t size) {
    if (!mochaInitDone) {
        return MOCHA_RESULT_LIB_UNINITIALIZED;
    }
    if (mochaApiVersion < 1) {
        return MOCHA_RESULT_UNSUPPORTED_COMMAND;
    }
    if (out_buffer == nullptr || offset > 0x200 || offset & 0x01) {
        return MOCHA_RESULT_INVALID_ARGUMENT;
    }

    uint32_t sizeInShorts   = size >> 1;
    uint32_t offsetInShorts = offset >> 1;
    int32_t maxReadCount    = 0x100 - offsetInShorts;

    if (maxReadCount <= 0) {
        return MOCHA_RESULT_SUCCESS;
    }

    uint32_t count = sizeInShorts > (uint32_t) maxReadCount ? (uint32_t) maxReadCount : sizeInShorts;
    auto *ptr      = (uint16_t *) out_buffer;

    int res = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (bspRead("EE", offsetInShorts + i, "access", 2, ptr) != 0) {
            return MOCHA_RESULT_UNKNOWN_ERROR;
        }
        res += 2;
        ptr++;
    }

    return static_cast<MochaUtilsStatus>(res);
}
