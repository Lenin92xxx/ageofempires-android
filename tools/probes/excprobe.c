// excprobe: record what a vectored exception handler sees for the guest faults FEX raises from JIT code (ud2,
// int3, a trap-flag single step, hlt, a call into a non-executable page), and time ud2 and int3 round trips.
// Run it under two FEX builds and diff the output: the records must match exactly; only the timings may differ.
// usage: excprobe.exe [count] [outfile]    defaults: 20000 D:\aoe\excprobe.txt
// Build: x86_64-w64-mingw32-gcc -O1 -static -o excprobe.exe excprobe.c
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static volatile LONG handled;
static volatile int record_next;
static char rec_buf[8192];
static int rec_len;
static ULONG_PTR image_base;
static ULONG_PTR nx_page;
static FILE *g_out;

#define PUT(...) (rec_len += snprintf(rec_buf + rec_len, sizeof(rec_buf) - rec_len, __VA_ARGS__))

// Addresses differ between runs; print them relative to the image or to the non-executable page.
static void addr(const char *name, ULONG_PTR a)
{
    if (a - image_base < 0x10000000) PUT(" %s=img+%llx", name, (unsigned long long)(a - image_base));
    else if (a - nx_page < 0x1000) PUT(" %s=nx+%llx", name, (unsigned long long)(a - nx_page));
    else PUT(" %s=%llx", name, (unsigned long long)a);
}

static void record(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    CONTEXT *c = e->ContextRecord;
    PUT("code=%08lx flags=%lx nparams=%lu", r->ExceptionCode, r->ExceptionFlags, r->NumberParameters);
    for (DWORD i = 0; i < r->NumberParameters && i < 2; i++) {
        char n[8];
        snprintf(n, sizeof(n), "info%lu", i);
        addr(n, r->ExceptionInformation[i]);
    }
    addr("addr", (ULONG_PTR)r->ExceptionAddress);
    addr("rip", c->Rip);
    PUT(" rax=%llx rcx=%llx rdx=%llx r8=%llx r15=%llx tf=%lu cf=%lu rsp_mod16=%llu\n", c->Rax, c->Rcx, c->Rdx, c->R8,
        c->R15, (c->EFlags >> 8) & 1, c->EFlags & 1, (unsigned long long)(c->Rsp & 0xf));
}

static LONG CALLBACK handler(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    CONTEXT *c = e->ContextRecord;
    if (record_next) { record(e); record_next = 0; fputs(rec_buf, g_out); fflush(g_out); rec_len = 0; rec_buf[0] = 0; }
    switch (r->ExceptionCode) {
    case STATUS_ILLEGAL_INSTRUCTION:    c->Rip += 2; break;        // ud2
    case STATUS_BREAKPOINT:             c->Rip += 1; break;        // int3 (Rip points at it)
    case STATUS_SINGLE_STEP:            c->EFlags &= ~0x100; break; // stop stepping
    case STATUS_PRIVILEGED_INSTRUCTION: c->Rip += 1; break;        // hlt
    case STATUS_ACCESS_VIOLATION:                                  // call into the non-executable page: return
        if (c->Rip - nx_page >= 0x1000) return EXCEPTION_CONTINUE_SEARCH;
        c->Rip = *(ULONG64 *)c->Rsp;
        c->Rsp += 8;
        break;
    default: return EXCEPTION_CONTINUE_SEARCH;
    }
    handled++;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Each fault site loads known values into rax/rcx/rdx/r8/r15 and sets CF, so the recorded context can be compared.
#define SETREGS "mov $0x1111, %%rax\n mov $0x2222, %%rcx\n mov $0x3333, %%rdx\n mov $0x4444, %%r8\n mov $0x5555, %%r15\n stc\n"
#define CLOBBERS "rax", "rcx", "rdx", "r8", "r15", "cc", "memory"

__attribute__((noinline)) void site_ud2(void)  { __asm__ volatile(SETREGS "ud2\n" ::: CLOBBERS); }
__attribute__((noinline)) void site_int3(void) { __asm__ volatile(SETREGS "int3\n" ::: CLOBBERS); }
__attribute__((noinline)) void site_hlt(void)  { __asm__ volatile(SETREGS "hlt\n" ::: CLOBBERS); }
__attribute__((noinline)) void site_tf(void)
{
    __asm__ volatile("pushfq\n orq $0x100, (%%rsp)\n popfq\n" SETREGS "nop\n nop\n" ::: CLOBBERS);
}
__attribute__((noinline)) void site_nx(void)
{
    __asm__ volatile(SETREGS "call *%0\n" :: "m"(nx_page) : CLOBBERS);
}

static double now_us(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 20000;
    FILE *f = fopen(argc > 2 ? argv[2] : "D:\\aoe\\excprobe.txt", "w");
    if (!f) return 1;
    g_out = f;
    image_base = (ULONG_PTR)GetModuleHandleA(NULL);
    nx_page = (ULONG_PTR)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    memset((void *)nx_page, 0xc3, 0x1000); // ret, never executed
    AddVectoredExceptionHandler(1, handler);

    for (int i = 0; i < 50; i++) { site_ud2(); site_int3(); } // warm up (JIT, first-chance paths)

    struct { const char *name; void (*fn)(void); } sites[] = {
        {"ud2", site_ud2}, {"int3", site_int3}, {"tf_step", site_tf}, {"hlt", site_hlt}, {"nx_call", site_nx}};
    for (int s = 0; s < 5; s++) {
        PUT("%s (site img+%llx): ", sites[s].name, (unsigned long long)((ULONG_PTR)sites[s].fn - image_base));
        LONG before = handled;
        fputs(rec_buf, f);
        fflush(f);
        rec_len = 0;
        rec_buf[0] = 0;
        record_next = 1;
        sites[s].fn();
        if (record_next) { PUT("NO EXCEPTION\n"); record_next = 0; }
        PUT("  handled %ld\n", handled - before);
        fputs(rec_buf, f);
        fflush(f);
        rec_len = 0;
        rec_buf[0] = 0;
    }

    LONG h0 = handled;
    double t0 = now_us();
    for (int i = 0; i < n; i++) site_ud2();
    double t1 = now_us();
    for (int i = 0; i < n; i++) site_int3();
    double t2 = now_us();
    fprintf(f, "timing: ud2 %.2f us, int3 %.2f us per exception (n=%d each, handled %ld)\n",
            (t1 - t0) / n, (t2 - t1) / n, n, handled - h0);
    fprintf(f, "END\n");
    fclose(f);
    return 0;
}
