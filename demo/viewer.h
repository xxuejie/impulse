// impulse: native-only viewer for the live playground demo (demo/playground.cpp).
//
// A minimal GLX/X11 window rendering the Jolt scene with fixed-function
// OpenGL (no external dependencies beyond libX11/libGL). Everything here
// is display code: it may use any math it likes (host trig, doubles,
// wall clock) because NONE of it touches the simulation or anything that
// gets recorded — commands leave this file as fully quantized integers
// (op, a, b) and are applied by the shared, deterministic sim code.
//
// Opcodes must match the session opcodes in demo/playground.cpp
// (static_assert'ed there).

#ifndef IMPULSE_VIEWER_H
#define IMPULSE_VIEWER_H

#ifndef OPENVM

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdint.h>

enum
{
	PG_OP_FIRE_BALL = 1,
	PG_OP_DROP_BOX = 2,
	PG_OP_SHOCKWAVE = 3,
	PG_OP_GRAVITY = 4
};

struct PGViewBody
{
	float px, py, pz;		// position
	float rx, ry, rz, rw;	// rotation quaternion
	int kind;				// 0 = box, 1 = sphere
	float ex, ey, ez;		// box half extents
	float r;				// sphere radius
	float cr, cg, cb;		// color
};

struct PGViewCmd
{
	uint8_t op;
	int32_t a, b;
};

class PGViewer
{
public:
	static PGViewer* create(int inWidth, int inHeight, uint32_t inSceneHeight)
	{
		Display* dpy = XOpenDisplay(nullptr);
		if (!dpy)
			return nullptr;

		int attribs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24,
			GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
		XVisualInfo* vi = glXChooseVisual(dpy, DefaultScreen(dpy), attribs);
		if (!vi)
		{
			XCloseDisplay(dpy);
			return nullptr;
		}
		GLXContext ctx = glXCreateContext(dpy, vi, nullptr, True);
		if (!ctx)
		{
			XCloseDisplay(dpy);
			return nullptr;
		}

		Window root = RootWindow(dpy, DefaultScreen(dpy));
		Colormap cmap = XCreateColormap(dpy, root, vi->visual, AllocNone);
		XSetWindowAttributes swa;
		swa.colormap = cmap;
		swa.border_pixel = 0;
		swa.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask
			| ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | ExposureMask
			| FocusChangeMask;
		Window win = XCreateWindow(dpy, root, 0, 0, inWidth, inHeight, 0, vi->depth,
			InputOutput, vi->visual, CWColormap | CWEventMask | CWBorderPixel, &swa);
		XStoreName(dpy, win, "impulse - live Jolt session (recorded for zkVM replay)");

		Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
		XSetWMProtocols(dpy, win, &wm_delete, 1);
		Cursor cursor = XCreateFontCursor(dpy, XC_crosshair);
		XDefineCursor(dpy, win, cursor);
		XMapRaised(dpy, win);
		if (!glXMakeCurrent(dpy, win, ctx))
		{
			XCloseDisplay(dpy);
			return nullptr;
		}

		PGViewer* v = new PGViewer();
		v->m_dpy = dpy;
		v->m_win = win;
		v->m_ctx = ctx;
		v->m_cmap = cmap;
		v->m_wm_delete = wm_delete;
		v->m_w = inWidth;
		v->m_h = inHeight;
		v->m_target_y = inSceneHeight * 1.4f;
		if (v->m_target_y > 26.0f)
			v->m_target_y = 26.0f;
		v->m_dist = inSceneHeight * 3.0f + 26.0f;
		v->init_gl();
		return v;
	}

	~PGViewer()
	{
		glXMakeCurrent(m_dpy, None, nullptr);
		glXDestroyContext(m_dpy, m_ctx);
		XCloseDisplay(m_dpy);
	}

	// Process pending events; false when the user asked to quit.
	bool pump()
	{
		held_tick(now());
		while (XPending(m_dpy))
		{
			XEvent ev;
			XNextEvent(m_dpy, &ev);
			switch (ev.type)
		{
			case KeyPress:
				// X autorepeat fires synthetic press/release pairs sharing the
				// same keycode+time; skip the synthetic press — repeats are
				// driven by our own held-key timer at a chosen rate
				if (ev.xkey.keycode == m_last_press_keycode && ev.xkey.time == m_last_press_time)
					break;
				m_last_press_keycode = ev.xkey.keycode;
				m_last_press_time = ev.xkey.time;
				{
					const KeySym ks = XLookupKeysym(&ev.xkey, 0);
					handle_key(ks);
					hold_begin(ev.xkey.keycode, ks);
				}
				break;
			case KeyRelease:
				// synthetic release: a matching press is queued right behind it
				if (XPending(m_dpy))
				{
					XEvent nxt;
					XPeekEvent(m_dpy, &nxt);
					if (nxt.type == KeyPress && nxt.xkey.keycode == ev.xkey.keycode
						&& nxt.xkey.time == ev.xkey.time)
						break; // keep held
				}
				hold_end(ev.xkey.keycode);
				break;
			case FocusOut:
				hold_clear(); // we may have missed the releases
				break;
			case ButtonPress:
				if (ev.xbutton.button == 1)
				{
					m_dragging = true;
					m_last_x = ev.xbutton.x;
					m_last_y = ev.xbutton.y;
				}
				else if (ev.xbutton.button == 4)
					zoom(0.88f);
				else if (ev.xbutton.button == 5)
					zoom(1.0f / 0.88f);
				break;
			case ButtonRelease:
				if (ev.xbutton.button == 1)
					m_dragging = false;
				break;
			case MotionNotify:
				if (m_dragging)
				{
					m_yaw += (ev.xmotion.x - m_last_x) * 0.005f;
					m_pitch += (ev.xmotion.y - m_last_y) * 0.005f;
					if (m_pitch < 0.05f) m_pitch = 0.05f;
					if (m_pitch > 1.5f) m_pitch = 1.5f;
					m_last_x = ev.xmotion.x;
					m_last_y = ev.xmotion.y;
				}
				break;
			case ConfigureNotify:
				m_w = ev.xconfigure.width;
				m_h = ev.xconfigure.height;
				glViewport(0, 0, m_w, m_h);
				break;
			case ClientMessage:
				if ((Atom)ev.xclient.data.l[0] == m_wm_delete)
					m_quit = true;
				break;
			default:
				break;
			}
		}
		return !m_quit;
	}

	bool paused() const { return m_paused; }

	// Drain commands issued since the last poll (fully quantized).
	int poll_cmds(PGViewCmd* out, int cap)
	{
		int n = 0;
		while (n < cap && m_pending_n > 0)
		{
			out[n++] = m_pending[--m_pending_n];
		}
		// preserve issue order: pending is a stack; reverse drained part
		for (int i = 0; i < n / 2; ++i)
		{
			const PGViewCmd t = out[i];
			out[i] = out[n - 1 - i];
			out[n - 1 - i] = t;
		}
		return n;
	}

	void render(const PGViewBody* inBodies, int inN, uint32_t inStep, uint32_t inStepTotal,
		uint32_t inCmds, uint32_t inTotalBodies)
	{
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		const float aspect = (float)m_w / (float)(m_h ? m_h : 1);
		perspective(50.0f, aspect, 0.5f, 600.0f);

		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		const float ex = m_dist * cosf(m_pitch) * sinf(m_yaw);
		const float ey = m_target_y + m_dist * sinf(m_pitch);
		const float ez = m_dist * cosf(m_pitch) * cosf(m_yaw);
		look_at(ex, ey, ez, 0.0f, m_target_y, 0.0f);

		glClearColor(0.07f, 0.08f, 0.11f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		const float light_pos[4] = { 0.45f, 0.8f, 0.5f, 0.0f };
		glLightfv(GL_LIGHT0, GL_POSITION, light_pos);

		draw_floor();
		draw_aim_guide();
		draw_bodies(inBodies, inN);
		draw_hud(inStep, inStepTotal, inCmds, inTotalBodies);

		glXSwapBuffers(m_dpy, m_win);
	}

	double now() const
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (double)ts.tv_sec + 1.0e-9 * (double)ts.tv_nsec;
	}

	// display-held interaction state (quantized; recorded only when a key
	// actually issues a command)
	int aim_slot = 0;		// 0..31 (32 compass slots, ~11 deg apart)
	int mass_class = 1;		// 0..2
	int elev = 34;			// 0..68 launch angle slots (-85..+85 deg, 2.5 deg/step)
	int shock_k = 1;		// 0..2
	int drop_i = 4;			// 0..8 lattice cycle
	int drop_j = 4;			// 0..8
	int gravity_mode = 0;	// 0..3: earth / moon / zero / up

	// replay mode: the timeline is driven by a session recording, not the
	// keyboard — command keys are ignored (pause/quit/camera still work)
	bool replay_mode = false;
	char hud_note[96] = { 0 };	// set by the caller each frame (e.g. "REPLAY 123/456")

private:
	PGViewer() = default;

	// windowed start; F11 (or the WM) toggles EWMH fullscreen
	void toggle_fullscreen()
	{
		Atom wm_state = XInternAtom(m_dpy, "_NET_WM_STATE", False);
		Atom wm_fullscreen = XInternAtom(m_dpy, "_NET_WM_STATE_FULLSCREEN", False);
		XEvent e;
		memset(&e, 0, sizeof(e));
		e.xclient.type = ClientMessage;
		e.xclient.window = m_win;
		e.xclient.message_type = wm_state;
		e.xclient.format = 32;
		e.xclient.data.l[0] = m_fullscreen ? 0 : 1; // _NET_WM_STATE_REMOVE / _ADD
		e.xclient.data.l[1] = (long)wm_fullscreen;
		e.xclient.data.l[2] = 0;
		XSendEvent(m_dpy, DefaultRootWindow(m_dpy), False,
			SubstructureRedirectMask | SubstructureNotifyMask, &e);
		XFlush(m_dpy);
		m_fullscreen = !m_fullscreen;
	}

	// held-key repeat for the aim/elevation keys: our own timer (initial
	// delay, then fixed rate) — independent of X server autorepeat, which
	// is filtered out in pump() so it cannot double-fire
	static constexpr double HOLD_REPEAT_DELAY = 0.30;
	static constexpr double HOLD_REPEAT_RATE = 0.07;

	struct HeldKey
	{
		unsigned int keycode = 0;
		KeySym ks = 0;
		double next_time = 0.0;
		bool active = false;
	};

	static bool is_repeatable(KeySym ks)
	{
		return ks == XK_Left || ks == XK_Right || ks == XK_Up || ks == XK_Down
			|| ks == XK_comma || ks == XK_period;
	}

	void hold_begin(unsigned int keycode, KeySym ks)
	{
		if (!is_repeatable(ks))
			return;
		HeldKey* slot = nullptr;
		for (HeldKey& h : m_held)
		{
			if (h.active && h.keycode == keycode)
			{
				h.next_time = now() + HOLD_REPEAT_DELAY;
				return;
			}
			if (!h.active && !slot)
				slot = &h;
		}
		if (slot)
		{
			slot->active = true;
			slot->keycode = keycode;
			slot->ks = ks;
			slot->next_time = now() + HOLD_REPEAT_DELAY;
		}
	}

	void hold_end(unsigned int keycode)
	{
		for (HeldKey& h : m_held)
			if (h.active && h.keycode == keycode)
				h.active = false;
	}

	void hold_clear()
	{
		for (HeldKey& h : m_held)
			h.active = false;
	}

	void held_tick(double t)
	{
		for (HeldKey& h : m_held)
		{
			if (!h.active || t < h.next_time)
				continue;
			handle_key(h.ks);
			h.next_time = t + HOLD_REPEAT_RATE;
		}
	}

	void zoom(float f)
	{
		m_dist *= f;
		if (m_dist < 8.0f) m_dist = 8.0f;
		if (m_dist > 400.0f) m_dist = 400.0f;
	}

	void push_cmd(uint8_t op, int32_t a, int32_t b)
	{
		if (m_pending_n >= (int)(sizeof(m_pending) / sizeof(m_pending[0])))
			return; // drop oldest? just drop (16 buffered per frame is plenty)
		m_pending[m_pending_n].op = op;
		m_pending[m_pending_n].a = a;
		m_pending[m_pending_n].b = b;
		++m_pending_n;
	}

	void handle_key(KeySym ks)
	{
		switch (ks)
		{
		case XK_F11:
			toggle_fullscreen();
			break;
		case XK_comma: case XK_Left:
			aim_slot = (aim_slot + 31) % 32;
			break;
		case XK_period: case XK_Right:
			aim_slot = (aim_slot + 1) % 32;
			break;
		case XK_Up:
			if (elev < 68) ++elev;
			break;
		case XK_Down:
			if (elev > 0) --elev;
			break;
		case XK_plus: case XK_equal: zoom(0.88f); break;
		case XK_minus: zoom(1.0f / 0.88f); break;
		case XK_p: case XK_P: case XK_space:
			m_paused = !m_paused;
			break;
		case XK_q: case XK_Q: case XK_Escape:
			m_quit = true;
			break;
		default:
			break;
		}
		if (replay_mode)
			return; // no live commands on a recorded timeline
		switch (ks)
		{
		case XK_F: case XK_f:
			push_cmd(PG_OP_FIRE_BALL, aim_slot, mass_class | (elev << 2));
			break;
		case XK_B: case XK_b:
			push_cmd(PG_OP_DROP_BOX, drop_i - 4, drop_j - 4);
			drop_i = (drop_i + 1) % 9;
			if (drop_i == 0)
				drop_j = (drop_j + 1) % 9;
			break;
		case XK_X: case XK_x:
			push_cmd(PG_OP_SHOCKWAVE, shock_k, 0);
			break;
		case XK_S: case XK_s:
			shock_k = (shock_k + 1) % 3;
			break;
		case XK_G: case XK_g:
			gravity_mode = (gravity_mode + 1) & 3;
			push_cmd(PG_OP_GRAVITY, gravity_mode, 0);
			break;
		case XK_1: mass_class = 0; break;
		case XK_2: mass_class = 1; break;
		case XK_3: mass_class = 2; break;
		default:
			break;
		}
	}

	void init_gl()
	{
		glEnable(GL_DEPTH_TEST);
		glEnable(GL_NORMALIZE);
		glEnable(GL_LIGHTING);
		glEnable(GL_LIGHT0);
		const float diffuse[4] = { 0.85f, 0.85f, 0.82f, 1.0f };
		const float ambient[4] = { 0.22f, 0.23f, 0.26f, 1.0f };
		glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
		glLightfv(GL_LIGHT0, GL_AMBIENT, ambient);
		glEnable(GL_COLOR_MATERIAL);
		glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
		glShadeModel(GL_FLAT);
		glViewport(0, 0, m_w, m_h);

		m_cube_list = glGenLists(1);
		glNewList(m_cube_list, GL_COMPILE);
		draw_unit_cube();
		glEndList();

		m_sphere_list = glGenLists(1);
		glNewList(m_sphere_list, GL_COMPILE);
		draw_unit_sphere(16, 10);
		glEndList();

		m_font_base = glGenLists(224);
		m_have_font = false;
		XFontStruct* font = XLoadQueryFont(m_dpy, "-misc-fixed-medium-semibold-*-*-*-13-*-*-*-*-*-*-*");
		if (!font)
			font = XLoadQueryFont(m_dpy, "fixed");
		if (font)
		{
			glXUseXFont(font->fid, 32, 224, m_font_base + 32);
			XFreeFont(m_dpy, font);
			m_have_font = true;
		}
	}

	void draw_unit_cube()
	{
		glBegin(GL_QUADS);
		glNormal3f(0, 0, 1);
		glVertex3f(-1, -1, 1); glVertex3f(1, -1, 1); glVertex3f(1, 1, 1); glVertex3f(-1, 1, 1);
		glNormal3f(0, 0, -1);
		glVertex3f(-1, -1, -1); glVertex3f(-1, 1, -1); glVertex3f(1, 1, -1); glVertex3f(1, -1, -1);
		glNormal3f(0, 1, 0);
		glVertex3f(-1, 1, -1); glVertex3f(-1, 1, 1); glVertex3f(1, 1, 1); glVertex3f(1, 1, -1);
		glNormal3f(0, -1, 0);
		glVertex3f(-1, -1, -1); glVertex3f(1, -1, -1); glVertex3f(1, -1, 1); glVertex3f(-1, -1, 1);
		glNormal3f(1, 0, 0);
		glVertex3f(1, -1, -1); glVertex3f(1, 1, -1); glVertex3f(1, 1, 1); glVertex3f(1, -1, 1);
		glNormal3f(-1, 0, 0);
		glVertex3f(-1, -1, -1); glVertex3f(-1, -1, 1); glVertex3f(-1, 1, 1); glVertex3f(-1, 1, -1);
		glEnd();
	}

	void draw_unit_sphere(int slices, int stacks)
	{
		glBegin(GL_QUADS);
		for (int i = 0; i < slices; ++i)
		{
			const float a0 = 2.0f * 3.14159265f * (float)i / slices;
			const float a1 = 2.0f * 3.14159265f * (float)(i + 1) / slices;
			for (int j = 0; j < stacks; ++j)
			{
				const float b0 = 3.14159265f * (float)j / stacks - 1.57079632f;
				const float b1 = 3.14159265f * (float)(j + 1) / stacks - 1.57079632f;
				const float v[4][3] = {
					{ cosf(b0) * cosf(a0), sinf(b0), cosf(b0) * sinf(a0) },
					{ cosf(b1) * cosf(a0), sinf(b1), cosf(b1) * sinf(a0) },
					{ cosf(b1) * cosf(a1), sinf(b1), cosf(b1) * sinf(a1) },
					{ cosf(b0) * cosf(a1), sinf(b0), cosf(b0) * sinf(a1) }
				};
				for (int k = 0; k < 4; ++k)
				{
					glNormal3fv(v[k]);
					glVertex3fv(v[k]);
				}
			}
		}
		glEnd();
	}

	void draw_floor()
	{
		glDisable(GL_LIGHTING);
		glColor3f(0.23f, 0.26f, 0.31f);
		glBegin(GL_LINES);
		for (int i = -10; i <= 10; ++i)
		{
			const float d = (float)i * 5.0f;
			glVertex3f(-50.0f, 0.0f, d);
			glVertex3f(50.0f, 0.0f, d);
			glVertex3f(d, 0.0f, -50.0f);
			glVertex3f(d, 0.0f, 50.0f);
		}
		glEnd();
		glEnable(GL_LIGHTING);
	}

	void draw_aim_guide()
	{
		// same quantized compass table + elevation slope as the sim
		// (display only — the recorded command is just (aim_slot, mass|elev<<2))
		static const int dx[32] = {
			1, 4, 2, 4, 1, 3, 1, 1, 0, -1, -1, -3, -1, -4, -2, -4,
			-1, -4, -2, -4, -1, -3, -1, -1, 0, 1, 1, 3, 1, 4, 2, 4 };
		static const int dz[32] = {
			0, 1, 1, 3, 1, 4, 2, 4, 1, 4, 2, 4, 1, 3, 1, 1,
			0, -1, -1, -3, -1, -4, -2, -4, -1, -4, -2, -4, -1, -3, -1, -1 };
		static const float elev_slope[69] = {
	-11.430052757263184f, -7.595754146575928f, -5.671281814575195f, -4.510708332061768f, -3.732050895690918f, -3.1715948581695557f,
	-2.7474775314331055f, -2.4142136573791504f, -2.1445069313049316f, -1.9209821224212646f, -1.7320507764816284f, -1.5696855783462524f,
	-1.4281480312347412f, -1.3032253980636597f, -1.191753625869751f, -1.0913084745407104f, -1.0f, -0.9163311719894409f,
	-0.839099645614624f, -0.7673270106315613f, -0.700207531452179f, -0.6370702385902405f, -0.5773502588272095f, -0.5205670595169067f,
	-0.466307669878006f, -0.4142135679721832f, -0.36397022008895874f, -0.31529879570007324f, -0.2679491937160492f, -0.22169466316699982f,
	-0.17632697522640228f, -0.13165250420570374f, -0.08748866617679596f, -0.04366094246506691f, 0.0f, 0.04366094246506691f,
	0.08748866617679596f, 0.13165250420570374f, 0.17632697522640228f, 0.22169466316699982f, 0.2679491937160492f, 0.31529879570007324f,
	0.36397022008895874f, 0.4142135679721832f, 0.466307669878006f, 0.5205670595169067f, 0.5773502588272095f, 0.6370702385902405f,
	0.700207531452179f, 0.7673270106315613f, 0.839099645614624f, 0.9163311719894409f, 1.0f, 1.0913084745407104f,
	1.191753625869751f, 1.3032253980636597f, 1.4281480312347412f, 1.5696855783462524f, 1.7320507764816284f, 1.9209821224212646f,
	2.1445069313049316f, 2.4142136573791504f, 2.7474775314331055f, 3.1715948581695557f, 3.732050895690918f, 4.510708332061768f,
	5.671281814575195f, 7.595754146575928f, 11.430052757263184f
		};
		static const float ball_r[3] = { 0.6f, 0.95f, 1.3f };
		const float hl = sqrtf((float)(dx[aim_slot] * dx[aim_slot] + dz[aim_slot] * dz[aim_slot]));
		const float ux = (float)dx[aim_slot] / hl;
		const float uz = (float)dz[aim_slot] / hl;

		// launch velocity direction (horizontal aim + elevation slope),
		// normalized exactly like the sim does
		float vx = ux, vy = elev_slope[elev], vz = uz;
		const float vl = sqrtf(vx * vx + vy * vy + vz * vz);
		vx /= vl; vy /= vl; vz /= vl;

		const float sx = -ux * 30.0f;
		const float sy = 8.0f;
		const float sz = -uz * 30.0f;

		glDisable(GL_LIGHTING);

		// ghost ball at the spawn point (wireframe, mass-class color)
		static const float mass_col[3][3] = {
			{ 0.25f, 0.85f, 0.95f }, { 0.95f, 0.6f, 0.15f }, { 0.95f, 0.2f, 0.2f } };
		glColor3f(mass_col[mass_class][0], mass_col[mass_class][1], mass_col[mass_class][2]);
		glPushMatrix();
		glTranslatef(sx, sy, sz);
		const float r = ball_r[mass_class];
		glScalef(r, r, r);
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		glCallList(m_sphere_list);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		glPopMatrix();

		// guide line along the launch direction, through the scene
		glColor3f(0.95f, 0.85f, 0.2f);
		glLineWidth(2.0f);
		glBegin(GL_LINES);
		glVertex3f(sx, sy, sz);
		glVertex3f(sx + vx * 45.0f, sy + vy * 45.0f, sz + vz * 45.0f);
		glEnd();
		glLineWidth(1.0f);
		glEnable(GL_LIGHTING);
	}

	void draw_bodies(const PGViewBody* inBodies, int inN)
	{
		for (int i = 0; i < inN; ++i)
		{
			const PGViewBody& b = inBodies[i];
			glColor3f(b.cr, b.cg, b.cb);
			glPushMatrix();
			glTranslatef(b.px, b.py, b.pz);
			float m[16];
			quat_matrix(b.rx, b.ry, b.rz, b.rw, m);
			glMultMatrixf(m);
			if (b.kind == 1)
			{
				glScalef(b.r, b.r, b.r);
				glCallList(m_sphere_list);
			}
			else
			{
				glScalef(b.ex, b.ey, b.ez);
				glCallList(m_cube_list);
			}
			glPopMatrix();
		}
	}

	void hud_line(float x, float y, const char* s)
	{
		if (!m_have_font)
			return;
		glRasterPos2f(x, y);
		const int len = (int)strlen(s);
		glListBase(m_font_base);
		glCallLists(len, GL_UNSIGNED_BYTE, s);
	}

	void draw_hud(uint32_t inStep, uint32_t inStepTotal, uint32_t inCmds, uint32_t inTotalBodies)
	{
		glDisable(GL_LIGHTING);
		glMatrixMode(GL_PROJECTION);
		glPushMatrix();
		glLoadIdentity();
		glOrtho(0, m_w, m_h, 0, -1, 1);
		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		glLoadIdentity();

		glColor3f(1.0f, 1.0f, 1.0f);
		char line[256];
		snprintf(line, sizeof(line), "impulse %s   step %u/%u   %.1fs   bodies %u   cmds %u%s",
			replay_mode ? "REPLAY" : "live", (unsigned)inStep, (unsigned)inStepTotal,
			(double)inStep / 60.0, (unsigned)inTotalBodies, (unsigned)inCmds,
			m_paused ? "   [PAUSED]" : "");
		hud_line(10, 20, line);
		if (replay_mode)
		{
			if (hud_note[0])
				hud_line(10, 36, hud_note);
		}
		else
		{
			static const char* grav_name[4] = { "earth", "moon", "zero", "up" };
			snprintf(line, sizeof(line), "aim %d/32   elev %d/68 (%+.1f deg)   ball %d   shock %d/3   gravity %s",
				aim_slot, elev, (float)(elev - 34) * 2.5f,
				mass_class + 1, shock_k + 1, grav_name[gravity_mode]);
			hud_line(10, 36, line);
		}
		snprintf(line, sizeof(line), replay_mode
			? "replay: [p]ause [q]uit   mouse: orbit / zoom   [F11]fullscreen"
			: "[f]ire [b]ox   shock: [s] strength, [x] blast   [g]ravity cycles earth/moon/zero/up   aim: hold arrows   elev: hold Up/Down   [1-3]ball [p]ause [F11] [q]end");
		hud_line(10, m_h - 12, line);

		glPopMatrix();
		glMatrixMode(GL_PROJECTION);
		glPopMatrix();
		glMatrixMode(GL_MODELVIEW);
		glEnable(GL_LIGHTING);
	}

	static void quat_matrix(float rx, float ry, float rz, float rw, float out[16])
	{
		out[0] = 1.0f - 2.0f * (ry * ry + rz * rz);
		out[1] = 2.0f * (rx * ry + rw * rz);
		out[2] = 2.0f * (rx * rz - rw * ry);
		out[3] = 0.0f;
		out[4] = 2.0f * (rx * ry - rw * rz);
		out[5] = 1.0f - 2.0f * (rx * rx + rz * rz);
		out[6] = 2.0f * (ry * rz + rw * rx);
		out[7] = 0.0f;
		out[8] = 2.0f * (rx * rz + rw * ry);
		out[9] = 2.0f * (ry * rz - rw * rx);
		out[10] = 1.0f - 2.0f * (rx * rx + ry * ry);
		out[11] = 0.0f;
		out[12] = 0.0f;
		out[13] = 0.0f;
		out[14] = 0.0f;
		out[15] = 1.0f;
	}

	static void perspective(float fovy_deg, float aspect, float z_near, float z_far)
	{
		const float fh = tanf(fovy_deg * 3.14159265f / 360.0f) * z_near;
		const float fw = fh * aspect;
		glFrustum(-fw, fw, -fh, fh, z_near, z_far);
	}

	static void look_at(float ex, float ey, float ez, float tx, float ty, float tz)
	{
		float fx = tx - ex, fy = ty - ey, fz = tz - ez;
		float fl = sqrtf(fx * fx + fy * fy + fz * fz);
		fx /= fl; fy /= fl; fz /= fl;
		// s = f x up(0,1,0) = (-fz, 0, fx)  [sign matters: up x f mirrors
		// the view — two flipped axes = 180-deg roll, world upside down]
		float sx = -fz, sy = 0.0f, sz = fx;
		const float sl = sqrtf(sx * sx + sy * sy + sz * sz);
		sx /= sl; sy /= sl; sz /= sl;
		// u = s x f
		const float ux = sy * fz - sz * fy;
		const float uy = sz * fx - sx * fz;
		const float uz = sx * fy - sy * fx;
		const float m[16] = {
			sx, ux, -fx, 0.0f,
			sy, uy, -fy, 0.0f,
			sz, uz, -fz, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f
		};
		glMultMatrixf(m);
		glTranslatef(-ex, -ey, -ez);
	}

	Display* m_dpy = nullptr;
	Window m_win = 0;
	GLXContext m_ctx = nullptr;
	Colormap m_cmap = 0;
	Atom m_wm_delete = 0;
	int m_w = 0, m_h = 0;
	bool m_quit = false;
	bool m_paused = false;
	bool m_fullscreen = false;
	bool m_dragging = false;
	int m_last_x = 0, m_last_y = 0;
	unsigned int m_last_press_keycode = 0;
	unsigned long m_last_press_time = 0;
	HeldKey m_held[6];
	float m_yaw = 0.7f;
	float m_pitch = 0.45f;
	float m_dist = 50.0f;
	float m_target_y = 10.0f;
	GLuint m_cube_list = 0;
	GLuint m_sphere_list = 0;
	GLuint m_font_base = 0;
	bool m_have_font = false;
	PGViewCmd m_pending[32];
	int m_pending_n = 0;
};

#endif // !OPENVM
#endif // IMPULSE_VIEWER_H
