#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdbool.h>

#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <X11/keysym.h>

struct context
{
	xcb_connection_t*  xcb;
	int                scr_id;
	const xcb_setup_t* setup;
	xcb_window_t       win;
	xcb_gcontext_t     gc;
	int                shm_id;
	xcb_shm_seg_t      shm_seg;
	void*              shm_ptr;
	int                shm_complete_event;
	xcb_keysym_t*      keymap;
	uint8_t            keysyms_per_code;
	void*              backbuffer;
	xcb_atom_t         delete_win_atom;
};

#define WIN_W 256
#define WIN_H 256

#define TARGET_FRAME_MS  50

#define STATE_READY      0
#define STATE_REFRACTORY 0xff0000ff
#define STATE_FIRING     0xffffffff

void init_window(struct context* ctx)
{
	xcb_screen_iterator_t iter  = xcb_setup_roots_iterator(ctx->setup);

	for(size_t i = 0; i < ctx->scr_id; ++i){
		xcb_screen_next(&iter);
	}

	xcb_screen_t* screen = iter.data;
	ctx->win = xcb_generate_id(ctx->xcb);

	uint32_t attr_mask  = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	uint32_t win_attr[] = {
		0,
		XCB_EVENT_MASK_KEY_PRESS |
		XCB_EVENT_MASK_BUTTON_PRESS |
		XCB_EVENT_MASK_BUTTON_RELEASE |
		XCB_EVENT_MASK_BUTTON_1_MOTION,
	};

	xcb_create_window(ctx->xcb, 24, ctx->win, screen->root,
					  0, 0, WIN_W, WIN_H, 0,
					  XCB_WINDOW_CLASS_INPUT_OUTPUT,
					  screen->root_visual,
					  attr_mask, win_attr);

	xcb_map_window(ctx->xcb, ctx->win);

	xcb_intern_atom_cookie_t cookies[] = {
		xcb_intern_atom(ctx->xcb, 0, 12, "WM_PROTOCOLS"),
		xcb_intern_atom(ctx->xcb, 0, 16, "WM_DELETE_WINDOW"),
	};

	xcb_intern_atom_reply_t* atoms[] = {
		xcb_intern_atom_reply(ctx->xcb, cookies[0], 0),
		xcb_intern_atom_reply(ctx->xcb, cookies[1], 0),
	};

	xcb_change_property(ctx->xcb, XCB_PROP_MODE_REPLACE,
						ctx->win, atoms[0]->atom, 4, 32,
						1, &atoms[1]->atom);

	static const char win_name[] = "Brian's Brain";

	xcb_change_property(ctx->xcb, XCB_PROP_MODE_REPLACE,
						ctx->win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
						sizeof(win_name)-1,	win_name);

	ctx->delete_win_atom = atoms[1]->atom;

	free(atoms[0]);
	free(atoms[1]);

	ctx->backbuffer = calloc(WIN_W * WIN_H, sizeof(int));
}

void init_shm(struct context* ctx)
{
	const xcb_query_extension_reply_t* xcb_shm = xcb_get_extension_data(ctx->xcb, &xcb_shm_id);
	if (!xcb_shm->present) {
		fprintf(stderr, "XSHM not supported :(\n");
		exit(1);
	}
	ctx->shm_complete_event = xcb_shm->first_event + XCB_SHM_COMPLETION;

	ctx->shm_id  = shmget(IPC_PRIVATE, WIN_W * WIN_H * 4, IPC_CREAT | 0777);
	ctx->shm_ptr = shmat(ctx->shm_id, 0, 0);
	ctx->shm_seg = xcb_generate_id(ctx->xcb);

	xcb_shm_attach(ctx->xcb, ctx->shm_seg, ctx->shm_id, 1);
	shmctl(ctx->shm_id, IPC_RMID, 0);

	ctx->gc = xcb_generate_id(ctx->xcb);
	xcb_create_gc(ctx->xcb, ctx->gc, ctx->win, 0, 0);
}

void init_keymap(struct context* ctx)
{
	xcb_get_keyboard_mapping_cookie_t key_cookie;
	key_cookie = xcb_get_keyboard_mapping_unchecked(ctx->xcb,
													ctx->setup->min_keycode,
													(ctx->setup->max_keycode - ctx->setup->min_keycode) + 1);

	xcb_get_keyboard_mapping_reply_t* keymap;
	keymap = xcb_get_keyboard_mapping_reply(ctx->xcb, key_cookie, NULL);

	ctx->keymap = xcb_get_keyboard_mapping_keysyms(keymap);
	ctx->keysyms_per_code = keymap->keysyms_per_keycode;
}

void draw_window(struct context* ctx)
{
	xcb_shm_put_image(ctx->xcb, ctx->win, ctx->gc,
					  WIN_W, WIN_H, 0, 0,
					  WIN_W, WIN_H, 0, 0,
					  24, XCB_IMAGE_FORMAT_Z_PIXMAP,
					  true, ctx->shm_seg, 0);
	xcb_flush(ctx->xcb);
}

xcb_keysym_t event_to_keysym(struct context* ctx, xcb_generic_event_t* ev)
{
	xcb_key_press_event_t* k = (xcb_key_press_event_t*)ev;
	xcb_keysym_t sym = ctx->keymap[(k->detail - ctx->setup->min_keycode) * ctx->keysyms_per_code];
}

void get_adjacent(int x, int y, uint32_t (*ptr)[WIN_W], uint32_t adj[static 8])
{
	int left  = (x + WIN_W - 1) % WIN_W;
	int right = (x + 1) % WIN_W;
	int up    = (y + WIN_H - 1) % WIN_H;
	int down  = (y + 1) % WIN_H;

	adj[0] = ptr[up][left];
	adj[1] = ptr[up][x];
	adj[2] = ptr[up][right];

	adj[3] = ptr[y][left];
	adj[4] = ptr[y][right];

	adj[5] = ptr[down][left];
	adj[6] = ptr[down][x];
	adj[7] = ptr[down][right];
}

void simulation_step(struct context* ctx)
{
	uint32_t (*ptr)[WIN_W] = ctx->shm_ptr;
	uint32_t (*tmp)[WIN_W] = ctx->backbuffer;

	uint32_t adjacent[8];

	for(int y = 0; y < WIN_H; ++y){
		for(int x = 0; x < WIN_W; ++x){
			get_adjacent(x, y, ptr, adjacent);

			int count = 0;
			for(int i = 0; i < 8; ++i){
				if(adjacent[i] == STATE_FIRING){
					++count;
				}
			}

			if(count == 2 && ptr[y][x] == STATE_READY){
				tmp[y][x] = STATE_FIRING;
			} else if(ptr[y][x] == STATE_FIRING){
				tmp[y][x] = STATE_REFRACTORY;
			}
		}
	}

	memcpy(ptr, tmp, WIN_W * WIN_H * 4);
	memset(tmp, 0,   WIN_W * WIN_H * 4);
}

double get_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_nsec / 1000000.0) + (ts.tv_sec * 1000.0);
}

int main(void)
{
	struct context ctx = {};
	ctx.xcb   = xcb_connect(NULL, &ctx.scr_id);
	ctx.setup = xcb_get_setup(ctx.xcb);

	init_window (&ctx);
	init_shm    (&ctx);
	init_keymap (&ctx);

	draw_window (&ctx);
	bool redraw = false;

	while(1){
		double start_ms = get_ms();

		xcb_generic_event_t* e;
		while((e = xcb_poll_for_event(ctx.xcb))){
			e->response_type &= ~0x80;

			if(e->response_type == XCB_KEY_PRESS){
				xcb_keysym_t sym = event_to_keysym(&ctx, e);
				if(sym == XK_Escape || sym == XK_q) return 0;

				// reset
				if(sym == XK_r){
					memset(ctx.shm_ptr, 0, WIN_W * WIN_H * 4);
				}

				// middle diamond thing
				if(sym == XK_s){
					uint32_t (*ptr)[WIN_W] = ctx.shm_ptr;
					ptr[WIN_H/2+0][WIN_W/2+0] = STATE_FIRING;
					ptr[WIN_H/2+0][WIN_W/2+1] = STATE_FIRING;
					ptr[WIN_H/2+1][WIN_W/2+0] = STATE_FIRING;
					ptr[WIN_H/2+1][WIN_W/2+1] = STATE_FIRING;
				}

				// horizontal generator thing
				if(sym == XK_a){
					uint32_t (*ptr)[WIN_W] = ctx.shm_ptr;

					ptr[WIN_H/2+0][WIN_W/2-1] = STATE_FIRING;
					ptr[WIN_H/2+0][WIN_W/2+2] = STATE_FIRING;
					ptr[WIN_H/2+1][WIN_W/2-1] = STATE_FIRING;
					ptr[WIN_H/2+1][WIN_W/2+2] = STATE_FIRING;
				}

				// vertical generator thing
				if(sym == XK_d){
					uint32_t (*ptr)[WIN_W] = ctx.shm_ptr;

					ptr[WIN_H/2-1][WIN_W/2+0] = STATE_FIRING;
					ptr[WIN_H/2-1][WIN_W/2+1] = STATE_FIRING;
					ptr[WIN_H/2+2][WIN_W/2+0] = STATE_FIRING;
					ptr[WIN_H/2+2][WIN_W/2+1] = STATE_FIRING;
				}
			}

			if(e->response_type == XCB_MOTION_NOTIFY){
				xcb_motion_notify_event_t* ev = (xcb_motion_notify_event_t*)e;
				uint32_t (*ptr)[WIN_W] = ctx.shm_ptr;

				if (ev->event_x >= 0 &&
					ev->event_x < WIN_W &&
					ev->event_y >= 0 &&
					ev->event_y < WIN_H){
					ptr[ev->event_y][ev->event_x] = STATE_FIRING;
				}
			}

			if(e->response_type == ctx.shm_complete_event){
				redraw = true;
			}

			if(e->response_type == XCB_CLIENT_MESSAGE){
				xcb_client_message_event_t* ev = (xcb_client_message_event_t*)e;
				if(ev->data.data32[0] == ctx.delete_win_atom){
					return 0;
				}
			}

			free(e);
		}

		if(redraw){
			simulation_step(&ctx);
			draw_window(&ctx);
			redraw = false;
		}

		double diff_ms;
		while((diff_ms = (get_ms() - start_ms)) < TARGET_FRAME_MS){
			usleep((TARGET_FRAME_MS - diff_ms) * 1000);
		}

#if 0
		double end_ms = get_ms();
		printf("Frame ms: %.4f\n", end_ms - start_ms);
#endif

	}

	return 0;
}

