// sendkey: inject keyboard input inside the Wine prefix via SendInput (no macOS permission needed).
// usage: sendkey.exe [wait:MS] KEY[:HOLDMS] ... ; KEY = ENTER SPACE ESC UP DOWN LEFT RIGHT TAB A..Z 0..9 F1..F12 or hex VK (0x..)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static WORD vk_of(const char *s) {
    if (!strcmp(s, "ENTER")) return VK_RETURN; if (!strcmp(s, "SPACE")) return VK_SPACE; if (!strcmp(s, "ESC")) return VK_ESCAPE;
    if (!strcmp(s, "UP")) return VK_UP; if (!strcmp(s, "DOWN")) return VK_DOWN; if (!strcmp(s, "LEFT")) return VK_LEFT; if (!strcmp(s, "RIGHT")) return VK_RIGHT;
    if (!strcmp(s, "TAB")) return VK_TAB; if (!strcmp(s, "SHIFT")) return VK_SHIFT; if (!strcmp(s, "CTRL")) return VK_CONTROL; if (!strcmp(s, "ALT")) return VK_MENU;
    if (s[0] == 'F' && s[1]) return VK_F1 + atoi(s + 1) - 1;
    if (!strncmp(s, "0x", 2)) return (WORD)strtoul(s, NULL, 16);
    return (WORD)s[0];
}
static void key(WORD vk, DWORD hold, int extended) {
    INPUT in[1]; memset(in, 0, sizeof in);
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk; in[0].ki.wScan = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    in[0].ki.dwFlags = extended ? KEYEVENTF_EXTENDEDKEY : 0;
    SendInput(1, in, sizeof(INPUT));
    Sleep(hold);
    in[0].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, in, sizeof(INPUT));
}
int main(int argc, char **argv) {
    HWND fg = GetForegroundWindow(); char title[256] = ""; if (fg) GetWindowTextA(fg, title, sizeof title);
    printf("foreground window %p '%s'\n", (void *)fg, title); fflush(stdout);
    int post = 0; HWND target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--focus") && i + 1 < argc) {   // bring the window of this class to the front first
            target = FindWindowA(argv[++i], NULL);
            if (target) { ShowWindow(target, SW_SHOW); SetForegroundWindow(target); SetActiveWindow(target); SetFocus(target); Sleep(400); }
            fg = GetForegroundWindow(); printf("focus %s -> hwnd %p, foreground now %p\n", argv[i], (void *)target, (void *)fg); fflush(stdout); continue;
        }
        if (!strcmp(argv[i], "--post")) { post = 1; continue; }   // PostMessage WM_KEYDOWN/UP to the target instead of SendInput
        char *a = strdup(argv[i]); char *colon = strchr(a, ':'); DWORD n = 0;
        if (colon) { *colon = 0; n = atoi(colon + 1); }
        if (!strcmp(a, "wait")) { Sleep(n); continue; }
        if (!strcmp(a, "click")) { INPUT in[2]; memset(in, 0, sizeof in); in[0].type = in[1].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1, in, sizeof(INPUT)); Sleep(60); SendInput(1, in + 1, sizeof(INPUT)); printf("click\n"); continue; }
        if (!strcmp(a, "move")) { INPUT in; memset(&in, 0, sizeof in); in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MOVE; in.mi.dx = (LONG)n; in.mi.dy = 0; SendInput(1, &in, sizeof in); continue; }
        WORD vk = vk_of(a); int ext = vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT;
        if (post && target) { LPARAM lp = 1 | ((LPARAM)MapVirtualKey(vk, MAPVK_VK_TO_VSC) << 16); PostMessageA(target, WM_KEYDOWN, vk, lp); Sleep(n ? n : 80); PostMessageA(target, WM_KEYUP, vk, lp | (1u << 30) | (1u << 31)); printf("posted %s\n", a); fflush(stdout); Sleep(150); continue; }
        key(vk, n ? n : 80, ext); printf("key %s (vk 0x%x)\n", a, vk); fflush(stdout);
        Sleep(150);
    }
    return 0;
}
