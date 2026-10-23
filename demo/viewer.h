// impulse: native-only viewer for the live playground demo (demo/playground.cpp).
//
// GLFW + fixed-function OpenGL: one code path for Linux, macOS and
// Windows (macOS gets the legacy 2.1 context, which still supports the
// whole fixed-function pipeline used here). Everything in this file is
// display code: it may use any math it likes (host trig, doubles, wall
// clock) because NONE of it touches the simulation or anything that
// gets recorded — commands leave this file as fully quantized integers
// (op, a, b) and are applied by the shared, deterministic sim code.
//
// The HUD font is embedded (demo/hud_font.h) — no system font calls.
//
// Opcodes must match the session opcodes in demo/playground.cpp.

#ifndef IMPULSE_VIEWER_H
#define IMPULSE_VIEWER_H

#ifndef OPENVM

#include <GLFW/glfw3.h>
#ifdef __APPLE__
#include <OpenGL/gl.h> // legacy 2.1 fixed-function context
#else
#include <GL/gl.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "hud_font.h"

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
		if (!glfwInit())
			return nullptr;

		// No version hints: macOS yields a legacy 2.1 context (full
		// fixed-function), Linux/Windows a compatibility context.
		GLFWwindow* win = glfwCreateWindow(inWidth, inHeight,
			"impulse - live Jolt session (recorded for zkVM replay)", nullptr, nullptr);
		if (!win)
		{
			glfwTerminate();
			return nullptr;
		}
		glfwMakeContextCurrent(win);
		glfwSwapInterval(1);

		PGViewer* v = new PGViewer();
		v->m_win = win;
		glfwGetFramebufferSize(win, &v->m_w, &v->m_h);
		v->m_target_y = inSceneHeight * 1.4f;
		if (v->m_target_y > 26.0f)
			v->m_target_y = 26.0f;
		v->m_dist = inSceneHeight * 3.0f + 26.0f;
		v->init_gl();
		glfwSetWindowUserPointer(win, v);
		glfwSetKeyCallback(win, key_cb);
		glfwSetFramebufferSizeCallback(win, fb_size_cb);
		glfwSetMouseButtonCallback(win, mouse_cb);
		glfwSetCursorPosCallback(win, cursor_cb);
		glfwSetScrollCallback(win, scroll_cb);
		glfwSetWindowFocusCallback(win, focus_cb);
		glfwSetWindowCloseCallback(win, close_cb);
		return v;
	}

	~PGViewer()
	{
		glfwDestroyWindow(m_win);
		glfwTerminate();
	}

	// Process pending events; false when the user asked to quit.
	bool pump()
	{
		held_tick(now());
		glfwPollEvents();
		return !m_quit;
	}

	bool paused() const { return m_paused; }

	// Drain commands issued since the last poll (fully quantized).
	int poll_cmds(PGViewCmd* out, int cap)
	{
		int n = 0;
		while (n < cap && m_pending_n > 0)
			out[n++] = m_pending[--m_pending_n];
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

		glfwSwapBuffers(m_win);
	}

	double now() const { return glfwGetTime(); }

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

	// --- GLFW callbacks (route into the instance) ---
	static PGViewer* self(GLFWwindow* w) { return (PGViewer*)glfwGetWindowUserPointer(w); }

	static void close_cb(GLFWwindow* w)
	{
		self(w)->m_quit = true;
	}

	static void fb_size_cb(GLFWwindow* w, int width, int height)
	{
		PGViewer* v = self(w);
		v->m_w = width;
		v->m_h = height;
		glViewport(0, 0, width, height);
	}

	static void focus_cb(GLFWwindow* w, int focused)
	{
		if (!focused)
			self(w)->hold_clear(); // we may have missed the releases
	}

	static void mouse_cb(GLFWwindow* w, int button, int action, int)
	{
		PGViewer* v = self(w);
		if (button == GLFW_MOUSE_BUTTON_LEFT)
		{
			if (action == GLFW_PRESS)
			{
				v->m_dragging = true;
				glfwGetCursorPos(w, (double*)&v->m_last_x, (double*)&v->m_last_y);
			}
			else
			{
				v->m_dragging = false;
			}
		}
	}

	static void cursor_cb(GLFWwindow* w, double x, double y)
	{
		PGViewer* v = self(w);
		if (!v->m_dragging)
			return;
		v->m_yaw += (float)(x - v->m_last_x) * 0.005f;
		v->m_pitch += (float)(y - v->m_last_y) * 0.005f;
		if (v->m_pitch < 0.05f) v->m_pitch = 0.05f;
		if (v->m_pitch > 1.5f) v->m_pitch = 1.5f;
		v->m_last_x = (float)x;
		v->m_last_y = (float)y;
	}

	static void scroll_cb(GLFWwindow* w, double, double dy)
	{
		self(w)->zoom(dy > 0 ? 0.88f : 1.0f / 0.88f);
	}

	static void key_cb(GLFWwindow* w, int key, int, int action, int)
	{
		// GLFW_REPEAT is ignored: repeats are driven by our own held-key
		// timer (uniform delay/rate on every platform)
		PGViewer* v = self(w);
		if (action == GLFW_PRESS || action == GLFW_RELEASE)
		{
			if (action == GLFW_PRESS)
			{
				v->handle_key(key);
				v->hold_begin(key);
			}
			else
			{
				v->hold_end(key);
			}
		}
	}

	// --- interaction ---
	void zoom(float f)
	{
		m_dist *= f;
		if (m_dist < 8.0f) m_dist = 8.0f;
		if (m_dist > 400.0f) m_dist = 400.0f;
	}

	void toggle_fullscreen()
	{
		if (!m_fullscreen)
		{
			glfwGetWindowPos(m_win, &m_win_x, &m_win_y);
			glfwGetWindowSize(m_win, &m_win_w, &m_win_h);
			GLFWmonitor* mon = glfwGetPrimaryMonitor();
			const GLFWvidmode* mode = glfwGetVideoMode(mon);
			glfwSetWindowMonitor(m_win, mon, 0, 0, mode->width, mode->height, GLFW_DONT_CARE);
			m_fullscreen = true;
		}
		else
		{
			glfwSetWindowMonitor(m_win, nullptr, m_win_x, m_win_y, m_win_w, m_win_h, GLFW_DONT_CARE);
			m_fullscreen = false;
		}
	}

	void push_cmd(uint8_t op, int32_t a, int32_t b)
	{
		if (m_pending_n >= (int)(sizeof(m_pending) / sizeof(m_pending[0])))
			return;
		m_pending[m_pending_n].op = op;
		m_pending[m_pending_n].a = a;
		m_pending[m_pending_n].b = b;
		++m_pending_n;
	}

	void handle_key(int key)
	{
		switch (key)
		{
		case GLFW_KEY_COMMA: case GLFW_KEY_LEFT:
			aim_slot = (aim_slot + 31) % 32;
			break;
		case GLFW_KEY_PERIOD: case GLFW_KEY_RIGHT:
			aim_slot = (aim_slot + 1) % 32;
			break;
		case GLFW_KEY_UP:
			if (elev < 68) ++elev;
			break;
		case GLFW_KEY_DOWN:
			if (elev > 0) --elev;
			break;
		case GLFW_KEY_EQUAL: case GLFW_KEY_KP_ADD:
			zoom(0.88f);
			break;
		case GLFW_KEY_MINUS: case GLFW_KEY_KP_SUBTRACT:
			zoom(1.0f / 0.88f);
			break;
		case GLFW_KEY_P: case GLFW_KEY_SPACE:
			m_paused = !m_paused;
			break;
		case GLFW_KEY_F11:
			toggle_fullscreen();
			break;
		case GLFW_KEY_Q: case GLFW_KEY_ESCAPE:
			m_quit = true;
			break;
		default:
			break;
		}
		if (replay_mode)
			return; // no live commands on a recorded timeline
		switch (key)
		{
		case GLFW_KEY_F:
			push_cmd(PG_OP_FIRE_BALL, aim_slot, mass_class | (elev << 2));
			break;
		case GLFW_KEY_B:
			push_cmd(PG_OP_DROP_BOX, drop_i - 4, drop_j - 4);
			drop_i = (drop_i + 1) % 9;
			if (drop_i == 0)
				drop_j = (drop_j + 1) % 9;
			break;
		case GLFW_KEY_X:
			push_cmd(PG_OP_SHOCKWAVE, shock_k, 0);
			break;
		case GLFW_KEY_S:
			shock_k = (shock_k + 1) % 3;
			break;
		case GLFW_KEY_G:
			gravity_mode = (gravity_mode + 1) & 3;
			push_cmd(PG_OP_GRAVITY, gravity_mode, 0);
			break;
		case GLFW_KEY_1: mass_class = 0; break;
		case GLFW_KEY_2: mass_class = 1; break;
		case GLFW_KEY_3: mass_class = 2; break;
		default:
			break;
		}
	}

	// held-key repeat for the aim/elevation keys: our own timer (initial
	// delay, then fixed rate) — platform-independent (GLFW repeats ignored)
	static constexpr double HOLD_REPEAT_DELAY = 0.30;
	static constexpr double HOLD_REPEAT_RATE = 0.07;

	struct HeldKey
	{
		int key = 0;
		double next_time = 0.0;
		bool active = false;
	};

	static bool is_repeatable(int key)
	{
		return key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT || key == GLFW_KEY_UP
			|| key == GLFW_KEY_DOWN || key == GLFW_KEY_COMMA || key == GLFW_KEY_PERIOD;
	}

	void hold_begin(int key)
	{
		if (!is_repeatable(key))
			return;
		for (HeldKey& h : m_held)
		{
			if (h.active && h.key == key)
			{
				h.next_time = now() + HOLD_REPEAT_DELAY;
				return;
			}
		}
		for (HeldKey& h : m_held)
		{
			if (!h.active)
			{
				h.active = true;
				h.key = key;
				h.next_time = now() + HOLD_REPEAT_DELAY;
				return;
			}
		}
	}

	void hold_end(int key)
	{
		for (HeldKey& h : m_held)
			if (h.active && h.key == key)
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
			handle_key(h.key);
			h.next_time = t + HOLD_REPEAT_RATE;
		}
	}

	// --- GL setup and drawing (unchanged fixed-function code) ---
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
			-11.430052757263184f, -7.595754146575928f, -5.671281814575195f, -4.510708332061768f, -3.732050895690918f, -3.1715948581695557f, -2.7474775314331055f, -2.4142136573791504f,
			-2.1445069313049316f, -1.9209821224212646f, -1.7320507764816284f, -1.5696855783462524f, -1.4281480312347412f, -1.3032253980636597f, -1.191753625869751f, -1.0913084745407104f,
			-1.0f, -0.9163311719894409f, -0.839099645614624f, -0.7673270106315613f, -0.700207531452179f, -0.6370702385902405f, -0.5773502588272095f, -0.5205670595169067f,
			-0.466307669878006f, -0.4142135679721832f, -0.36397022008895874f, -0.31529879570007324f, -0.2679491937160492f, -0.22169466316699982f, -0.17632697522640228f, -0.13165250420570374f,
			-0.08748866617679596f, -0.04366094246506691f, 0.0f, 0.04366094246506691f, 0.08748866617679596f, 0.13165250420570374f, 0.17632697522640228f, 0.22169466316699982f,
			0.2679491937160492f, 0.31529879570007324f, 0.36397022008895874f, 0.4142135679721832f, 0.466307669878006f, 0.5205670595169067f, 0.5773502588272095f, 0.6370702385902405f,
			0.700207531452179f, 0.7673270106315613f, 0.839099645614624f, 0.9163311719894409f, 1.0f, 1.0913084745407104f, 1.191753625869751f, 1.3032253980636597f,
			1.4281480312347412f, 1.5696855783462524f, 1.7320507764816284f, 1.9209821224212646f, 2.1445069313049316f, 2.4142136573791504f, 2.7474775314331055f, 3.1715948581695557f,
			3.732050895690918f, 4.510708332061768f, 5.671281814575195f, 7.595754146575928f, 11.430052757263184f };
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

	// 8x8 embedded font, drawn as quads (fixed-function only, no system
	// font dependency)
	void hud_text(float x, float y, const char* s)
	{
		glBegin(GL_QUADS);
		float cx = x;
		for (const char* p = s; *p; ++p)
		{
			const unsigned char c = (unsigned char)*p;
			if (c >= 32 && c < 127)
			{
				const unsigned char* g = HUD_FONT[c - 32];
				for (int row = 0; row < 8; ++row)
				{
					const unsigned char bits = g[row];
					if (!bits)
						continue;
					for (int col = 0; col < 8; ++col)
					{
						if (bits & (0x80 >> col))
						{
							const float px = cx + (float)col;
							const float py = y + (float)row;
							glVertex2f(px, py);
							glVertex2f(px + 1.0f, py);
							glVertex2f(px + 1.0f, py + 1.0f);
							glVertex2f(px, py + 1.0f);
						}
					}
				}
			}
			cx += 8.0f;
		}
		glEnd();
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
		hud_text(10, 14, line);
		if (replay_mode)
		{
			if (hud_note[0])
				hud_text(10, 30, hud_note);
		}
		else
		{
			static const char* grav_name[4] = { "earth", "moon", "zero", "up" };
			snprintf(line, sizeof(line), "aim %d/32   elev %d/68 (%+.1f deg)   ball %d   shock %d/3   gravity %s",
				aim_slot, elev, (float)(elev - 34) * 2.5f,
				mass_class + 1, shock_k + 1, grav_name[gravity_mode]);
			hud_text(10, 30, line);
		}
		snprintf(line, sizeof(line), replay_mode
			? "replay: [p]ause [q]uit   mouse: orbit / zoom   [F11]fullscreen"
			: "[f]ire [b]ox   shock: [s] strength, [x] blast   [g]ravity cycles   aim: hold arrows   elev: hold Up/Down   [1-3]ball [p]ause [F11] [q]end");
		hud_text(10, (float)m_h - 14, line);

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

	GLFWwindow* m_win = nullptr;
	int m_w = 0, m_h = 0;
	bool m_quit = false;
	bool m_paused = false;
	bool m_fullscreen = false;
	bool m_dragging = false;
	float m_last_x = 0.0f, m_last_y = 0.0f;
	int m_win_x = 0, m_win_y = 0, m_win_w = 0, m_win_h = 0;
	float m_yaw = 0.7f;
	float m_pitch = 0.45f;
	float m_dist = 50.0f;
	float m_target_y = 10.0f;
	GLuint m_cube_list = 0;
	GLuint m_sphere_list = 0;
	HeldKey m_held[6];
	PGViewCmd m_pending[32];
	int m_pending_n = 0;
};

#endif // !OPENVM
#endif // IMPULSE_VIEWER_H
