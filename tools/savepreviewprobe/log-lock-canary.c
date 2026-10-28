// Replicate enginefix's fopen_s writer and leave time for an attach/detach test.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
int main(int argc, char** argv) {
    FILE* log = NULL;
    if (argc != 2 || fopen_s(&log, argv[1], "w") || !log) return 2;
    fputs("settings object 0x02580E78 found: mesh=4.000 foliage=2.000\n", log);
    fflush(log);
    printf("CANARY READY pid=%lu\n", (unsigned long)GetCurrentProcessId());
    fflush(stdout);
    Sleep(20000);
    fputs("CANARY CONTINUED AFTER DEBUGGER DETACH\n", log);
    fclose(log);
    puts("CANARY FINISHED");
    return 0;
}
