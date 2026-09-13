/* scroll-tester — minimal X11 tester: prints Button4/5 (scroll) and motion.
 * Run it UNDER gamescope to check whether scroll reaches the X11 client at all:
 *   ./gamescope/build/src/gamescope -W 2880 -H 1800 -w 1440 -h 900 -f -S integer -F nearest -- ./scroll-tester
 * Click / scroll inside the tester window; Ctrl+C exits.
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) { fprintf(stderr, "No X11 display\n"); return 1; }

	int scr = DefaultScreen(dpy);
	Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr),
		0, 0, 800, 500, 1,
		BlackPixel(dpy, scr), WhitePixel(dpy, scr));

	XSelectInput(dpy, win, ButtonPressMask | ButtonReleaseMask |
		PointerMotionMask | EnterWindowMask | LeaveWindowMask);
	XMapWindow(dpy, win);
	XStoreName(dpy, win, "scroll-tester — scroll here");

	while (1) {
		XEvent ev;
		XNextEvent(dpy, &ev);
		switch (ev.type) {
		case ButtonPress:
		case ButtonRelease:
			printf("%s button=%d state=0x%x x=%d y=%d\n",
				ev.type == ButtonPress ? "PRESS" : "RELEASE",
				ev.xbutton.button, ev.xbutton.state,
				ev.xbutton.x, ev.xbutton.y);
			fflush(stdout);
			break;
		case MotionNotify:
			/* only every so often, so the log does not get flooded */
			if (ev.xmotion.x % 64 == 0) {
				printf("MOTION x=%d y=%d\n", ev.xmotion.x, ev.xmotion.y);
				fflush(stdout);
			}
			break;
		}
	}
	return 0;
}