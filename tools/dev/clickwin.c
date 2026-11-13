// clickwin: list a top-level window's children, or click a point in one (client coords) with posted mouse messages.
// usage: clickwin.exe CLASS|title=TITLE            -> list children (id, class, text, rect relative to the parent)
//        clickwin.exe CLASS|title=TITLE X Y        -> WM_LBUTTONDOWN/UP at X,Y of the child under that point (or the window)
//        clickwin.exe CLASS|title=TITLE id=N       -> BN_CLICKED-style WM_COMMAND for control id N plus a click on it
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static HWND top;
static BOOL CALLBACK child(HWND h, LPARAM lp) {
    char cls[64], txt[128]; RECT r; POINT p = {0, 0};
    GetClassNameA(h, cls, sizeof cls); GetWindowTextA(h, txt, sizeof txt); GetWindowRect(h, &r); ClientToScreen(top, &p);
    printf("  child %p id=%d class='%s' text='%s' rect %ld,%ld-%ld,%ld vis=%d style=%08lx\n", (void *)h, GetDlgCtrlID(h), cls, txt, r.left - p.x, r.top - p.y, r.right - p.x, r.bottom - p.y, IsWindowVisible(h), (unsigned long)GetWindowLongA(h, GWL_STYLE));
    return TRUE;
}
static void click(HWND h, int x, int y) {
    LPARAM lp = MAKELPARAM(x, y);
    PostMessageA(h, WM_MOUSEMOVE, 0, lp); Sleep(50); PostMessageA(h, WM_LBUTTONDOWN, MK_LBUTTON, lp); Sleep(80); PostMessageA(h, WM_LBUTTONUP, 0, lp);
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    top = !strncmp(argv[1], "title=", 6) ? FindWindowA(NULL, argv[1] + 6) : FindWindowA(argv[1], NULL);
    if (!top) { printf("window not found\n"); return 1; }
    RECT r; char t[128]; GetWindowRect(top, &r); GetWindowTextA(top, t, sizeof t);
    printf("window %p '%s' rect %ld,%ld-%ld,%ld\n", (void *)top, t, r.left, r.top, r.right, r.bottom);
    if (argc == 2) { EnumChildWindows(top, child, 0); fflush(stdout); return 0; }
    if (!strncmp(argv[2], "id=", 3)) {
        int id = atoi(argv[2] + 3); HWND c = GetDlgItem(top, id); RECT cr;
        if (!c) { printf("no control %d\n", id); return 1; }
        GetClientRect(c, &cr); click(c, cr.right / 2, cr.bottom / 2);
        PostMessageA(top, WM_COMMAND, MAKEWPARAM(id, 0 /* BN_CLICKED == STN_CLICKED == 0 */), (LPARAM)c);
        printf("clicked control %d\n", id); fflush(stdout); return 0;
    }
    if (argc >= 4) {
        POINT p = { atoi(argv[2]), atoi(argv[3]) }; HWND c = ChildWindowFromPointEx(top, p, CWP_SKIPINVISIBLE);
        if (argc >= 5) c = top; /* 5th arg: post to the window itself (statics without SS_NOTIFY are transparent) */ else if (c && c != top) { MapWindowPoints(top, c, &p, 1); }
        else c = top;
        click(c, p.x, p.y); printf("clicked %p at %ld,%ld\n", (void *)c, p.x, p.y); fflush(stdout);
    }
    return 0;
}
