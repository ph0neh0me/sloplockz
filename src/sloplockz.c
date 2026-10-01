#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define TOOL_VERSION L"1.0.0"
#define BUFFER_SIZE (64 * 1024)

static void print_win32_error(const wchar_t *context) {
    wchar_t message[512] = {0};
    DWORD error = GetLastError();
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error, 0, message, (DWORD)_countof(message), NULL);
    fwprintf(stderr, L"sloplockz: %ls: %ls (error %lu)\n", context, message, error);
}

static unsigned long path_key(const wchar_t *path, wchar_t *full_path, DWORD capacity) {
    unsigned long hash = 2166136261u;
    DWORD length = GetFullPathNameW(path, capacity, full_path, NULL);
    if (!length || length >= capacity) return 0;
    CharUpperBuffW(full_path, length);
    for (DWORD i = 0; i < length; ++i) {
        hash ^= (unsigned long)full_path[i];
        hash *= 16777619u;
    }
    return hash;
}

static int control_paths(const wchar_t *target, wchar_t *root, wchar_t *state,
    wchar_t *write_request, wchar_t *write_done, wchar_t *release_request) {
    wchar_t temp[MAX_PATH], base[MAX_PATH * 4], full[MAX_PATH * 4];
    unsigned long key;

    if (!GetTempPathW((DWORD)_countof(temp), temp)) return 0;
    key = path_key(target, full, (DWORD)_countof(full));
    if (!key) return 0;

    _snwprintf_s(base, _countof(base), _TRUNCATE, L"%lssloplockz", temp);
    CreateDirectoryW(base, NULL);
    _snwprintf_s(root, MAX_PATH * 4, _TRUNCATE, L"%ls\\%08lx", base, key);
    _snwprintf_s(state, MAX_PATH * 4, _TRUNCATE, L"%ls\\state", root);
    _snwprintf_s(write_request, MAX_PATH * 4, _TRUNCATE, L"%ls\\write.req", root);
    _snwprintf_s(write_done, MAX_PATH * 4, _TRUNCATE, L"%ls\\write.done", root);
    _snwprintf_s(release_request, MAX_PATH * 4, _TRUNCATE, L"%ls\\release.req", root);
    return 1;
}

static int write_text(const wchar_t *path, const wchar_t *text) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size, written = 0;
    int ok;
    if (file == INVALID_HANDLE_VALUE) return 0;
    size = (DWORD)(wcslen(text) * sizeof(wchar_t));
    ok = WriteFile(file, text, size, &written, NULL) && written == size;
    FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

static int read_text(const wchar_t *path, wchar_t *text, DWORD capacity) {
    HANDLE file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD received = 0;
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(file, text, (capacity - 1) * sizeof(wchar_t), &received, NULL)) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    text[received / sizeof(wchar_t)] = L'\0';
    return 1;
}

static int copy_into_held_file(HANDLE destination, const wchar_t *input_path) {
    HANDLE source = INVALID_HANDLE_VALUE;
    BYTE *buffer = NULL;
    FILE_END_OF_FILE_INFO end = {0};
    ULONGLONG offset = 0;
    int result = 0;

    source = CreateFileW(input_path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (source == INVALID_HANDLE_VALUE) {
        print_win32_error(L"open replacement");
        goto cleanup;
    }

    buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, BUFFER_SIZE);
    if (!buffer) goto cleanup;

    if (!SetFileInformationByHandle(destination, FileEndOfFileInfo, &end, sizeof(end))) {
        print_win32_error(L"truncate target");
        goto cleanup;
    }

    for (;;) {
        DWORD received = 0, written = 0;
        OVERLAPPED write_operation;
        HANDLE event;

        if (!ReadFile(source, buffer, BUFFER_SIZE, &received, NULL)) {
            print_win32_error(L"read replacement");
            goto cleanup;
        }
        if (!received) break;

        ZeroMemory(&write_operation, sizeof(write_operation));
        event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!event) goto cleanup;
        write_operation.hEvent = event;
        write_operation.Offset = (DWORD)(offset & 0xffffffffu);
        write_operation.OffsetHigh = (DWORD)(offset >> 32);

        if (!WriteFile(destination, buffer, received, &written, &write_operation)) {
            if (GetLastError() != ERROR_IO_PENDING ||
                !GetOverlappedResult(destination, &write_operation, &written, TRUE)) {
                CloseHandle(event);
                print_win32_error(L"write target");
                goto cleanup;
            }
        }
        CloseHandle(event);
        if (written != received) goto cleanup;
        offset += written;
    }

    end.EndOfFile.QuadPart = offset;
    if (!SetFileInformationByHandle(destination, FileEndOfFileInfo, &end, sizeof(end)) ||
        !FlushFileBuffers(destination)) {
        print_win32_error(L"finalize target");
        goto cleanup;
    }
    result = 1;

cleanup:
    if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
    if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
    return result;
}

static int hold_oplock(const wchar_t *target) {
    HANDLE file = INVALID_HANDLE_VALUE, event = NULL;
    OVERLAPPED operation;
    DWORD ignored = 0, error;
    BOOL completed;
    wchar_t root[MAX_PATH * 4], state[MAX_PATH * 4], write_request[MAX_PATH * 4];
    wchar_t write_done[MAX_PATH * 4], release_request[MAX_PATH * 4];
    wchar_t replacement[MAX_PATH * 4];
    int result = 1;

    ZeroMemory(&operation, sizeof(operation));
    if (!control_paths(target, root, state, write_request, write_done, release_request)) return 1;
    CreateDirectoryW(root, NULL);
    DeleteFileW(write_request);
    DeleteFileW(write_done);
    DeleteFileW(release_request);

    file = CreateFileW(target, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        print_win32_error(L"open target");
        goto cleanup;
    }

    event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!event) goto cleanup;
    operation.hEvent = event;

    completed = DeviceIoControl(file, FSCTL_REQUEST_OPLOCK_LEVEL_1,
        NULL, 0, NULL, 0, &ignored, &operation);
    error = GetLastError();
    if (!completed && error != ERROR_IO_PENDING) {
        SetLastError(error);
        print_win32_error(L"request oplock");
        goto cleanup;
    }

    write_text(state, L"held");
    wprintf(L"Oplock held. Use status, write, and release from another process.\n");

    for (;;) {
        if (WaitForSingleObject(event, 50) == WAIT_OBJECT_0)
            write_text(state, L"break-pending");

        if (GetFileAttributesW(write_request) != INVALID_FILE_ATTRIBUTES) {
            int copied = read_text(write_request, replacement, (DWORD)_countof(replacement)) &&
                copy_into_held_file(file, replacement);
            write_text(write_done, copied ? L"ok" : L"failed");
            DeleteFileW(write_request);
        }

        if (GetFileAttributesW(release_request) != INVALID_FILE_ATTRIBUTES) {
            result = 0;
            break;
        }
    }

cleanup:
    write_text(state, L"released");
    if (event) CloseHandle(event);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return result;
}

static int show_status(const wchar_t *target) {
    wchar_t root[MAX_PATH * 4], state[MAX_PATH * 4], write_request[MAX_PATH * 4];
    wchar_t write_done[MAX_PATH * 4], release_request[MAX_PATH * 4], value[64];
    if (!control_paths(target, root, state, write_request, write_done, release_request) ||
        !read_text(state, value, (DWORD)_countof(value))) {
        wprintf(L"inactive\n");
        return 3;
    }
    wprintf(L"%ls\n", value);
    return 0;
}

static int request_write(const wchar_t *target, const wchar_t *input) {
    wchar_t root[MAX_PATH * 4], state[MAX_PATH * 4], write_request[MAX_PATH * 4];
    wchar_t write_done[MAX_PATH * 4], release_request[MAX_PATH * 4];
    wchar_t full_input[MAX_PATH * 4], value[64];

    if (!control_paths(target, root, state, write_request, write_done, release_request)) return 1;
    if (!GetFullPathNameW(input, (DWORD)_countof(full_input), full_input, NULL)) return 1;
    DeleteFileW(write_done);
    if (!write_text(write_request, full_input)) {
        print_win32_error(L"queue write");
        return 1;
    }

    for (DWORD i = 0; i < 600; ++i) {
        if (read_text(write_done, value, (DWORD)_countof(value))) {
            DeleteFileW(write_done);
            wprintf(L"%ls\n", value);
            return _wcsicmp(value, L"ok") == 0 ? 0 : 1;
        }
        Sleep(50);
    }
    fwprintf(stderr, L"sloplockz: write request timed out\n");
    return 4;
}

static int request_release(const wchar_t *target) {
    wchar_t root[MAX_PATH * 4], state[MAX_PATH * 4], write_request[MAX_PATH * 4];
    wchar_t write_done[MAX_PATH * 4], release_request[MAX_PATH * 4], value[64];

    if (!control_paths(target, root, state, write_request, write_done, release_request) ||
        !write_text(release_request, L"release")) return 1;

    for (DWORD i = 0; i < 200; ++i) {
        if (read_text(state, value, (DWORD)_countof(value)) && !_wcsicmp(value, L"released")) {
            DeleteFileW(release_request);
            wprintf(L"released\n");
            return 0;
        }
        Sleep(50);
    }
    fwprintf(stderr, L"sloplockz: release request timed out\n");
    return 4;
}

static int usage(void) {
    fwprintf(stderr, L"sloplockz %ls\n", TOOL_VERSION);
    fwprintf(stderr, L"Usage:\n");
    fwprintf(stderr, L"  sloplockz hold <file>                 Hold a level-1 oplock in the foreground.\n");
    fwprintf(stderr, L"  sloplockz status <file>               Print held, break-pending, released, or inactive.\n");
    fwprintf(stderr, L"  sloplockz write <file> <replacement>  Replace data through the held handle.\n");
    fwprintf(stderr, L"  sloplockz release <file>              Release the held oplock.\n");
    fwprintf(stderr, L"\nThe control commands may be run from a second terminal or process.\n");
    return 2;
}

int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && !_wcsicmp(argv[1], L"hold")) return hold_oplock(argv[2]);
    if (argc == 3 && !_wcsicmp(argv[1], L"status")) return show_status(argv[2]);
    if (argc == 4 && !_wcsicmp(argv[1], L"write")) return request_write(argv[2], argv[3]);
    if (argc == 3 && !_wcsicmp(argv[1], L"release")) return request_release(argv[2]);
    return usage();
}
