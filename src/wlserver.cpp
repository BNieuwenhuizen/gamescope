#define _GNU_SOURCE 1

#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h> 
#include <string.h> 
#include <sys/epoll.h>

#include <linux/input-event-codes.h>

#include <X11/extensions/XTest.h>
#include <xkbcommon/xkbcommon.h>

#include <wayland-server-core.h>

extern "C" {
#define static
#define class class_
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/noop.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/xwayland.h>
#include <wlr/util/log.h>
#undef static
#undef class
}

#include "wlserver.hpp"
#include "drm.hpp"
#include "main.hpp"
#include "steamcompmgr.hpp"

#include "gpuvis_trace_utils.h"

struct wlserver_t wlserver;

Display *g_XWLDpy;

bool run = true;

// O = hover, 1/2/3 = left/right/middle, 4 = touch passthrough
int g_nTouchClickMode = 1;

void sig_handler(int signal)
{
	if ( signal == SIGUSR2 )
	{
		g_bTakeScreenshot = true;
		return;
	}

	wlr_log(WLR_DEBUG, "Received kill signal. Terminating!");
	run = false;
}

void nudge_steamcompmgr(void)
{
	static bool bHasNestedDisplay = false;
	static XEvent XWLExposeEvent = {};
	
	if ( bHasNestedDisplay == false )
	{
		g_XWLDpy = XOpenDisplay( wlserver.wlr.xwayland_server->display_name );
		
		XWLExposeEvent.xclient.type = ClientMessage;
		XWLExposeEvent.xclient.window = DefaultRootWindow( g_XWLDpy );
		XWLExposeEvent.xclient.format = 32;

		bHasNestedDisplay = true;
	}
	
	XSendEvent( g_XWLDpy , DefaultRootWindow( g_XWLDpy ), True, SubstructureRedirectMask, &XWLExposeEvent);
	XFlush( g_XWLDpy );
}

extern const struct wlr_surface_role xwayland_surface_role;

void xwayland_surface_role_commit(struct wlr_surface *wlr_surface) {
	assert(wlr_surface->role == &xwayland_surface_role);

	if ( wlr_surface->buffer == NULL )
	{
		return;
	}

	struct wlr_buffer *buf = wlr_buffer_lock( &wlr_surface->buffer->base );

	gpuvis_trace_printf( "xwayland_surface_role_commit wlr_surface %p\n", wlr_surface );

	wayland_commit( wlr_surface, buf );
}

static void xwayland_surface_role_precommit(struct wlr_surface *wlr_surface) {
	assert(wlr_surface->role == &xwayland_surface_role);
	struct wlr_xwayland_surface *surface = (struct wlr_xwayland_surface *) wlr_surface->role_data;
	if (surface == NULL) {
		return;
	}
}

const struct wlr_surface_role xwayland_surface_role = {
	.name = "wlr_xwayland_surface",
	.commit = xwayland_surface_role_commit,
	.precommit = xwayland_surface_role_precommit,
};

static void xwayland_ready(struct wl_listener *listener, void *data)
{
	setenv("DISPLAY", wlserver.wlr.xwayland_server->display_name, true);
	
	startSteamCompMgr();
}

struct wl_listener xwayland_ready_listener = { .notify = xwayland_ready };

static void wlserver_handle_modifiers(struct wl_listener *listener, void *data)
{
	struct wlserver_keyboard *keyboard = wl_container_of( listener, keyboard, modifiers );
	
	wlr_seat_set_keyboard( wlserver.wlr.seat, keyboard->device );
	wlr_seat_keyboard_notify_modifiers( wlserver.wlr.seat, &keyboard->device->keyboard->modifiers );
}

static void wlserver_handle_key(struct wl_listener *listener, void *data)
{
	struct wlserver_keyboard *keyboard = wl_container_of( listener, keyboard, key );
	struct wlr_event_keyboard_key *event = (struct wlr_event_keyboard_key *) data;

	xkb_keycode_t keycode = event->keycode + 8;
	xkb_keysym_t keysym = xkb_state_key_get_one_sym(keyboard->device->keyboard->xkb_state, keycode);

	if (wlserver.wlr.session && event->state == WL_KEYBOARD_KEY_STATE_PRESSED && keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
		unsigned vt = keysym - XKB_KEY_XF86Switch_VT_1 + 1;
		wlr_session_change_vt(wlserver.wlr.session, vt);
		return;
	}

	wlr_seat_set_keyboard( wlserver.wlr.seat, keyboard->device);
	wlr_seat_keyboard_notify_key( wlserver.wlr.seat, event->time_msec, event->keycode, event->state );
}

static void wlserver_movecursor( int x, int y )
{
	wlserver.mouse_surface_cursorx += x;
	
	if ( wlserver.mouse_surface_cursorx > wlserver.mouse_focus_surface->current.width - 1 )
	{
		wlserver.mouse_surface_cursorx = wlserver.mouse_focus_surface->current.width - 1;
	}
	
	if ( wlserver.mouse_surface_cursorx < 0 )
	{
		wlserver.mouse_surface_cursorx = 0;
	}
	
	wlserver.mouse_surface_cursory += y;
	
	if ( wlserver.mouse_surface_cursory > wlserver.mouse_focus_surface->current.height - 1 )
	{
		wlserver.mouse_surface_cursory = wlserver.mouse_focus_surface->current.height - 1;
	}
	
	if ( wlserver.mouse_surface_cursory < 0 )
	{
		wlserver.mouse_surface_cursory = 0;
	}
}

static void wlserver_handle_pointer_motion(struct wl_listener *listener, void *data)
{
	struct wlserver_pointer *pointer = wl_container_of( listener, pointer, motion );
	struct wlr_event_pointer_motion *event = (struct wlr_event_pointer_motion *) data;
	
	if ( wlserver.mouse_focus_surface != NULL )
	{
		wlserver_movecursor( event->unaccel_dx, event->unaccel_dy );

		wlr_seat_pointer_notify_motion( wlserver.wlr.seat, event->time_msec, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
		wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
	}
}

static void wlserver_handle_pointer_button(struct wl_listener *listener, void *data)
{
	struct wlserver_pointer *pointer = wl_container_of( listener, pointer, button );
	struct wlr_event_pointer_button *event = (struct wlr_event_pointer_button *) data;
	
	wlr_seat_pointer_notify_button( wlserver.wlr.seat, event->time_msec, event->button, event->state );
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
}

static inline uint32_t steamcompmgr_button_to_wlserver_button( int button )
{
	switch ( button )
	{
		default:
		case 0:
			return 0;
		case 1:
			return BTN_LEFT;
		case 2:
			return BTN_RIGHT;
		case 3:
			return BTN_MIDDLE;
	}
}

static void wlserver_handle_touch_down(struct wl_listener *listener, void *data)
{
	struct wlserver_touch *touch = wl_container_of( listener, touch, down );
	struct wlr_event_touch_down *event = (struct wlr_event_touch_down *) data;
	
	if ( wlserver.mouse_focus_surface != NULL )
	{
		double x = g_bRotated ? event->y : event->x;
		double y = g_bRotated ? 1.0 - event->x : event->y;
		
		x *= g_nOutputWidth;
		y *= g_nOutputHeight;

		x += focusedWindowOffsetX;
		y += focusedWindowOffsetY;

		x *= focusedWindowScaleX;
		y *= focusedWindowScaleY;

		if ( x < 0.0f ) x = 0.0f;
		if ( y < 0.0f ) y = 0.0f;

		if ( x > wlserver.mouse_focus_surface->current.width ) x = wlserver.mouse_focus_surface->current.width;
		if ( y > wlserver.mouse_focus_surface->current.height ) y = wlserver.mouse_focus_surface->current.height;

		wlserver.mouse_surface_cursorx = x;
		wlserver.mouse_surface_cursory = y;

		if ( g_nTouchClickMode == 4 )
		{
			if ( event->touch_id >= 0 && event->touch_id < WLSERVER_TOUCH_COUNT )
			{
				wlr_seat_touch_notify_down( wlserver.wlr.seat, wlserver.mouse_focus_surface, event->time_msec, event->touch_id,
											wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );

				wlserver.touch_down[ event->touch_id ] = true;
			}
		}
		else
		{
			wlr_seat_pointer_notify_motion( wlserver.wlr.seat, event->time_msec, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
			wlr_seat_pointer_notify_frame( wlserver.wlr.seat );

			uint32_t button = steamcompmgr_button_to_wlserver_button( g_nTouchClickMode );

			if ( button != 0 && g_nTouchClickMode < WLSERVER_BUTTON_COUNT )
			{
				wlr_seat_pointer_notify_button( wlserver.wlr.seat, event->time_msec, button, WLR_BUTTON_PRESSED );
				wlr_seat_pointer_notify_frame( wlserver.wlr.seat );

				wlserver.button_held[ g_nTouchClickMode ] = true;
			}
		}
	}
}

static void wlserver_handle_touch_up(struct wl_listener *listener, void *data)
{
	struct wlserver_touch *touch = wl_container_of( listener, touch, up );
	struct wlr_event_touch_up *event = (struct wlr_event_touch_up *) data;

	if ( wlserver.mouse_focus_surface != NULL )
	{
		bool bReleasedAny = false;
		for ( int i = 0; i < WLSERVER_BUTTON_COUNT; i++ )
		{
			if ( wlserver.button_held[ i ] == true )
			{
				uint32_t button = steamcompmgr_button_to_wlserver_button( i );

				if ( button != 0 )
				{
					wlr_seat_pointer_notify_button( wlserver.wlr.seat, event->time_msec, button, WLR_BUTTON_RELEASED );
					bReleasedAny = true;
				}

				wlserver.button_held[ i ] = false;
			}
		}

		if ( bReleasedAny == true )
		{
			wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
		}

		if ( event->touch_id >= 0 && event->touch_id < WLSERVER_TOUCH_COUNT && wlserver.touch_down[ event->touch_id ] == true )
		{
			wlr_seat_touch_notify_up( wlserver.wlr.seat, event->time_msec, event->touch_id );
			wlserver.touch_down[ event->touch_id ] = false;
		}
	}
}

static void wlserver_handle_touch_motion(struct wl_listener *listener, void *data)
{
	struct wlserver_touch *touch = wl_container_of( listener, touch, motion );
	struct wlr_event_touch_motion *event = (struct wlr_event_touch_motion *) data;
	
	if ( wlserver.mouse_focus_surface != NULL )
	{
		double x = g_bRotated ? event->y : event->x;
		double y = g_bRotated ? 1.0 - event->x : event->y;

		x *= g_nOutputWidth;
		y *= g_nOutputHeight;

		x += focusedWindowOffsetX;
		y += focusedWindowOffsetY;

		x *= focusedWindowScaleX;
		y *= focusedWindowScaleY;

		if ( x < 0.0f ) x = 0.0f;
		if ( y < 0.0f ) y = 0.0f;

		if ( x > wlserver.mouse_focus_surface->current.width ) x = wlserver.mouse_focus_surface->current.width;
		if ( y > wlserver.mouse_focus_surface->current.height ) y = wlserver.mouse_focus_surface->current.height;

		wlserver.mouse_surface_cursorx = x;
		wlserver.mouse_surface_cursory = y;

		if ( g_nTouchClickMode == 4 )
		{
			wlr_seat_touch_notify_motion( wlserver.wlr.seat, event->time_msec, event->touch_id, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
		}
		else
		{
			wlr_seat_pointer_notify_motion( wlserver.wlr.seat, event->time_msec, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
			wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
		}
	}
}

static void wlserver_new_input(struct wl_listener *listener, void *data)
{
	struct wlr_input_device *device = (struct wlr_input_device *) data;

	switch ( device->type )
	{
		case WLR_INPUT_DEVICE_KEYBOARD:
		{
			struct wlserver_keyboard *pKB = (struct wlserver_keyboard *) calloc( 1, sizeof( struct wlserver_keyboard ) );
			
			pKB->device = device;
			
			struct xkb_rule_names rules = { 0 };
			struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
			struct xkb_keymap *keymap = xkb_map_new_from_names(context, &rules,
															   XKB_KEYMAP_COMPILE_NO_FLAGS);
			
			wlr_keyboard_set_keymap(device->keyboard, keymap);
			xkb_keymap_unref(keymap);
			xkb_context_unref(context);
			wlr_keyboard_set_repeat_info(device->keyboard, 25, 600);
			
			pKB->modifiers.notify = wlserver_handle_modifiers;
			wl_signal_add( &device->keyboard->events.modifiers, &pKB->modifiers );
			
			pKB->key.notify = wlserver_handle_key;
			wl_signal_add( &device->keyboard->events.key, &pKB->key );
			
			wlr_seat_set_keyboard( wlserver.wlr.seat, device );
		}
		break;
		case WLR_INPUT_DEVICE_POINTER:
		{
			struct wlserver_pointer *pointer = (struct wlserver_pointer *) calloc( 1, sizeof( struct wlserver_pointer ) );
			
			pointer->device = device;

			pointer->motion.notify = wlserver_handle_pointer_motion;
			wl_signal_add( &device->pointer->events.motion, &pointer->motion );
			pointer->button.notify = wlserver_handle_pointer_button;
			wl_signal_add( &device->pointer->events.button, &pointer->button );
		}
		break;
		case WLR_INPUT_DEVICE_TOUCH:
		{
			struct wlserver_touch *touch = (struct wlserver_touch *) calloc( 1, sizeof( struct wlserver_touch ) );
			
			touch->device = device;
			
			touch->down.notify = wlserver_handle_touch_down;
			wl_signal_add( &device->touch->events.down, &touch->down );
			touch->up.notify = wlserver_handle_touch_up;
			wl_signal_add( &device->touch->events.up, &touch->up );
			touch->motion.notify = wlserver_handle_touch_motion;
			wl_signal_add( &device->touch->events.motion, &touch->motion );
		}
		break;
		default:
			break;
	}
}

struct wl_listener new_input_listener = { .notify = wlserver_new_input };

static void wlserver_surface_destroy( struct wl_listener *listener, void *data )
{
	wlr_surface *surf = (wlr_surface*)data;
	wayland_surfaces_deleted.push_back( surf );

	wl_list_remove( &listener->link );
	delete listener;
	surf->data = nullptr;
}

int wlserver_init(int argc, char **argv, bool bIsNested) {
	bool bIsDRM = bIsNested == false;

	wlr_log_init(WLR_DEBUG, NULL);
	wlserver.wl_display = wl_display_create();

	signal(SIGTERM, sig_handler);
	signal(SIGINT, sig_handler);
	signal(SIGUSR2, sig_handler);

	wlserver.wlr.session = ( bIsDRM == True ) ? wlr_session_create(wlserver.wl_display) : NULL;

	wlserver.wl_event_loop = wl_display_get_event_loop(wlserver.wl_display);
	wlserver.wl_event_loop_fd = wl_event_loop_get_fd( wlserver.wl_event_loop );

	wlserver.wlr.multi_backend = wlr_multi_backend_create(wlserver.wl_display);

	assert( wlserver.wl_display && wlserver.wl_event_loop && wlserver.wlr.multi_backend );
	assert( !bIsDRM || wlserver.wlr.session );

	wl_signal_add( &wlserver.wlr.multi_backend->events.new_input, &new_input_listener );

	wlserver.wlr.headless_backend = wlr_headless_backend_create( wlserver.wl_display, NULL );
	if ( wlserver.wlr.headless_backend == NULL )
	{
		return 1;
	}
	wlr_multi_backend_add( wlserver.wlr.multi_backend, wlserver.wlr.headless_backend );

	wlserver.wlr.noop_backend = wlr_noop_backend_create( wlserver.wl_display );
	wlr_multi_backend_add( wlserver.wlr.multi_backend, wlserver.wlr.noop_backend );

	wlserver.wlr.output = wlr_noop_add_output( wlserver.wlr.noop_backend );

	if ( bIsDRM == True )
	{
		wlserver.wlr.libinput_backend = wlr_libinput_backend_create( wlserver.wl_display, wlserver.wlr.session );
		if ( wlserver.wlr.libinput_backend == NULL)
		{
			return 1;
		}
		wlr_multi_backend_add( wlserver.wlr.multi_backend, wlserver.wlr.libinput_backend );
	}
	else if ( wlr_backend_is_headless(wlserver.wlr.headless_backend) )
	{
		wlr_headless_add_input_device( wlserver.wlr.headless_backend, WLR_INPUT_DEVICE_KEYBOARD );
	}

	wlserver.wlr.renderer = wlr_backend_get_renderer( wlserver.wlr.multi_backend );

	assert(wlserver.wlr.renderer);

	wlr_renderer_init_wl_display(wlserver.wlr.renderer, wlserver.wl_display);

	wlserver.wlr.compositor = wlr_compositor_create(wlserver.wl_display, wlserver.wlr.renderer);
	
	struct wlr_xwayland_server_options xwayland_options = {
		.lazy = false,
		.enable_wm = false,
	};
	wlserver.wlr.xwayland_server = wlr_xwayland_server_create(wlserver.wl_display, &xwayland_options);
	wl_signal_add(&wlserver.wlr.xwayland_server->events.ready, &xwayland_ready_listener);
	
	char wayland_display_name[32];
	int result = -1;
	int display_slot = 0;

	while ( result != 0 && display_slot < 128 )
	{
		sprintf( wayland_display_name, "gamescope-%d", display_slot );
		result = wl_display_add_socket( wlserver.wl_display, wayland_display_name );
		display_slot++;
	}

	if ( result != 0 )
	{
		wlr_log_errno(WLR_ERROR, "Unable to open wayland socket");
		wlr_backend_destroy( wlserver.wlr.multi_backend );
		return 1;
	}

	wlserver.wlr.seat = wlr_seat_create(wlserver.wl_display, "seat0");
	wlr_seat_set_capabilities( wlserver.wlr.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH );

	wlr_log(WLR_INFO, "Running compositor on wayland display '%s'", wayland_display_name);

	if (!wlr_backend_start( wlserver.wlr.multi_backend ))
	{
		wlr_log(WLR_ERROR, "Failed to start backend");
		wlr_backend_destroy( wlserver.wlr.multi_backend );
		wl_display_destroy(wlserver.wl_display);
		return 1;
	}

	wlr_output_enable( wlserver.wlr.output, true );
	wlr_output_set_custom_mode( wlserver.wlr.output, g_nNestedWidth, g_nNestedHeight, g_nOutputRefresh * 1000 );
	wlr_output_commit( wlserver.wlr.output );

	wlr_output_create_global( wlserver.wlr.output );

	return 0;
}

pthread_mutex_t waylock = PTHREAD_MUTEX_INITIALIZER;

void wlserver_lock(void)
{
	pthread_mutex_lock(&waylock);
}

void wlserver_unlock(void)
{
	wl_display_flush_clients(wlserver.wl_display);
	pthread_mutex_unlock(&waylock);
}

int wlserver_run(void)
{
	int epoll_fd = epoll_create( 1 );
	struct epoll_event ev;
	struct epoll_event events[128];
	int n;
	
	ev.events = EPOLLIN;
	
	if ( epoll_fd == -1 ||
		epoll_ctl( epoll_fd, EPOLL_CTL_ADD, wlserver.wl_event_loop_fd, &ev ) == -1 )
	{
		return 1;
	}

	while ( run )
	{
		n = epoll_wait( epoll_fd, events, 128, -1 );
		if ( n == -1 )
		{
			if ( errno == EINTR )
			{
				continue;
			}
			else
			{
				break;
			}
		}

		// We have wayland stuff to do, do it while locked
		wlserver_lock();
		
		for ( int i = 0; i < n; i++ )
		{
			wl_display_flush_clients(wlserver.wl_display);
			int ret = wl_event_loop_dispatch(wlserver.wl_event_loop, 0);
			if (ret < 0) {
				break;
			}
		}

		wlserver_unlock();
	}

	// We need to shutdown Xwayland before disconnecting all clients, otherwise
	// wlroots will restart it automatically.
	wlr_xwayland_server_destroy(wlserver.wlr.xwayland_server);
	wl_display_destroy_clients(wlserver.wl_display);
	wl_display_destroy(wlserver.wl_display);
	return 0;
}

void wlserver_keyboardfocus( struct wlr_surface *surface )
{
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard( wlserver.wlr.seat );
	if ( keyboard != NULL )
	{
		wlr_seat_keyboard_notify_enter( wlserver.wlr.seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

void wlserver_key( uint32_t key, bool press, uint32_t time )
{
	wlr_seat_keyboard_notify_key( wlserver.wlr.seat, time, key, press );
}

void wlserver_mousefocus( struct wlr_surface *wlrsurface )
{
	wlserver.mouse_focus_surface = wlrsurface;
	wlserver.mouse_surface_cursorx = wlrsurface->current.width / 2.0;
	wlserver.mouse_surface_cursory = wlrsurface->current.height / 2.0;
	wlr_seat_pointer_notify_enter( wlserver.wlr.seat, wlrsurface, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
}

void wlserver_mousemotion( int x, int y, uint32_t time )
{
	if ( g_XWLDpy != NULL )
	{
		XTestFakeRelativeMotionEvent( g_XWLDpy, x, y, CurrentTime );
		XFlush( g_XWLDpy );
	}
}

void wlserver_mousebutton( int button, bool press, uint32_t time )
{
	wlr_seat_pointer_notify_button( wlserver.wlr.seat, time, button, press ? WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED );
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
}

void wlserver_mousewheel( int x, int y, uint32_t time )
{
	if ( x != 0 )
	{
		wlr_seat_pointer_notify_axis( wlserver.wlr.seat, time, WLR_AXIS_ORIENTATION_HORIZONTAL, x, x, WLR_AXIS_SOURCE_WHEEL );
	}
	if ( y != 0 )
	{
		wlr_seat_pointer_notify_axis( wlserver.wlr.seat, time, WLR_AXIS_ORIENTATION_VERTICAL, y, y, WLR_AXIS_SOURCE_WHEEL );
	}
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
}

void wlserver_send_frame_done( struct wlr_surface *surf, const struct timespec *when )
{
	wlr_surface_send_frame_done( surf, when );
}

struct wlr_surface *wlserver_get_surface( long surfaceID )
{
	struct wlr_surface *ret = NULL;

	struct wl_resource *resource = wl_client_get_object(wlserver.wlr.xwayland_server->client, surfaceID);
	if (resource) {
		ret = wlr_surface_from_resource(resource);
	}
	else
	{
		return NULL;
	}
	
	if ( !wlr_surface_set_role(ret, &xwayland_surface_role, NULL, NULL, 0 ) )
	{
		fprintf (stderr, "Failed to set xwayland surface role");
		return NULL;
	}

	struct wl_listener *pListener = new struct wl_listener;
	pListener->notify = wlserver_surface_destroy;
	pListener->link.next = nullptr;
	pListener->link.prev = nullptr;

	wl_signal_add( &ret->events.destroy, pListener );
	ret->data = pListener;
	
	return ret;
}

const char *wlserver_get_nested_display( void )
{
	return wlserver.wlr.xwayland_server->display_name;
}
