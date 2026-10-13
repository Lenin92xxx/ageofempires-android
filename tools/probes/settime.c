// settime: show or set a file's times. Used to test how AoE IV decides to show its start-up warnings ("Don't show
// this message for [N] days"; the files are Documents\My Games\Age of Empires IV\*_check.log).
// usage: settime.exe FILE [UNIX_SECONDS] [LOG]
//   without UNIX_SECONDS it only reads, and prints the last-write time through GetFileTime, GetFileAttributesEx and
//   FindFirstFile, plus the current time; with it, it creates the file if missing and sets all three times.
//   LOG defaults to D:\aoe\settime.txt (appended).
// Build: x86_64-w64-mingw32-gcc -O1 -static -o settime.exe settime.c
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static long long to_unix(FILETIME ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *log = fopen(argc > 3 ? argv[3] : "D:\\aoe\\settime.txt", "a");
    if (!log) return 3;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    if (argc > 2) {
        HANDLE h = CreateFileA(argv[1], GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            fprintf(log, "open for write failed: %s (error %lu)\n", argv[1], GetLastError());
            fclose(log);
            return 1;
        }
        ULARGE_INTEGER u;
        u.QuadPart = strtoull(argv[2], NULL, 10) * 10000000ULL + 116444736000000000ULL;
        FILETIME t = {u.LowPart, u.HighPart};
        BOOL ok = SetFileTime(h, &t, &t, &t);
        fprintf(log, "%s: set %s -> %s\n", argv[1], argv[2], ok ? "ok" : "FAILED");
        CloseHandle(h);
    }
    fprintf(log, "now=%lld", to_unix(now));
    HANDLE h = CreateFileA(argv[1], FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME c, a, w;
        GetFileTime(h, &c, &a, &w);
        fprintf(log, " GetFileTime(created=%lld written=%lld)", to_unix(c), to_unix(w));
        CloseHandle(h);
    } else {
        fprintf(log, " CreateFile failed (error %lu)", GetLastError());
    }
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExA(argv[1], GetFileExInfoStandard, &d))
        fprintf(log, " GetFileAttributesEx(written=%lld attrs=%#lx)", to_unix(d.ftLastWriteTime), d.dwFileAttributes);
    else
        fprintf(log, " GetFileAttributesEx failed (error %lu)", GetLastError());
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(argv[1], &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        fprintf(log, " FindFirstFile(written=%lld attrs=%#lx)", to_unix(fd.ftLastWriteTime), fd.dwFileAttributes);
        FindClose(fh);
    } else {
        fprintf(log, " FindFirstFile failed (error %lu)", GetLastError());
    }
    fprintf(log, "  %s\n", argv[1]);
    fclose(log);
    return 0;
}
