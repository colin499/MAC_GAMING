#include <windows.h>
#include <stdio.h>
static BOOL CALLBACK child(HWND h, LPARAM lp) {
    char cls[64], txt[512]; GetClassNameA(h, cls, sizeof cls); GetWindowTextA(h, txt, sizeof txt);
    printf("    child %p class='%s' text='%s'\n", (void *)h, cls, txt); return TRUE;
}
static BOOL CALLBACK top(HWND h, LPARAM lp) {
    char cls[64], txt[256]; DWORD pid = 0; RECT r;
    if (!IsWindowVisible(h)) return TRUE;
    GetClassNameA(h, cls, sizeof cls); GetWindowTextA(h, txt, sizeof txt); GetWindowThreadProcessId(h, &pid); GetWindowRect(h, &r);
    printf("hwnd %p pid %lu class='%s' title='%s' rect %ld,%ld-%ld,%ld%s\n", (void *)h, pid, cls, txt, r.left, r.top, r.right, r.bottom, h == GetForegroundWindow() ? " [FOREGROUND]" : "");
    EnumChildWindows(h, child, 0); return TRUE;
}
int main(void) { EnumWindows(top, 0); fflush(stdout); return 0; }
