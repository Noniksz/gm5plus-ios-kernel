/*
 * libhakios.dylib: code of ours inside stock iOS programs.
 *
 * launchd starts the jobs named in INJECT (see build.sh / mkjobs.py) with
 * DYLD_INSERT_LIBRARIES pointing here; dyld honours that because the kernel's AMFI stand-in
 * says so. What is done depends on the program:
 *
 *   backboardd   the window server (CoreAnimation in QuartzCore). There is no GPU, so the
 *                render server must use CoreAnimation's own software rasterizer, and we want to
 *                see why a frame is or is not drawn. The C++ objects are reached by replacing
 *                entries of their virtual tables.
 *                It is also where input enters the system: backboardd is the HID event server,
 *                and without Apple's multitouch driver it has no touch screen. We give it one:
 *                a "virtual" HID service that turns the samples of /dev/hakios-touch into the
 *                events that driver would have sent.
 *
 *                And it does not wait five seconds for an ambient light sensor.
 *
 *   containermanagerd (both of them)
 *                They ask the user manager for the "personas" of the user, of which this
 *                system has none, twenty times a second apart before they give up -- and
 *                everything that needs a container (the window server, for one) waits with
 *                them. Told that personas are not supported, they do not ask.
 *
 * Addresses of things inside the dyld shared cache are those of iOS 16.5 (20F66) for iPhone10,3;
 * every one is checked against what is expected to be there before it is touched.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_port.h>
#include <mach/arm/thread_status.h>
#include <mach_debug/mach_debug_types.h>
#include <notify.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "hakios.gl.h"
#include "hakios.input.h"

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */
#define HAKIOS_GL "/redwood/lib/libhakios.gl.dylib"

static uintptr_t slide;
static int hakios_ca_prod;               /* fix77: boot-arg ca_prod=1 re-arms fix76's prod */

static void
say(const char *format, ...)
{
	char line[640];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	int cfd = open("/dev/console", O_WRONLY);
	if (cfd >= 0) {
		write(cfd, line, (size_t)n);
		close(cfd);
	}
	write(2, line, (size_t)n);
}

/* Replace one entry of a virtual table (unslid address `slot`) that must now hold `expected`. */
static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr;
	void *original;

	if (*entry != expected + slide) {
		say("hakios: %s: found %#lx, expected %#lx; left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	/* the tables are in read-only memory shared by every process: get a private writable copy of the page */
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("hakios: %s: cannot make the table writable (%d)\n", what, kr);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

/* ---- backboardd: CA::WindowServer ------------------------------------------------------------ */

#define VT_APPLE_CLCD_DISPLAY   0x1d5b44520UL   /* vtable for CA::WindowServer::AppleCLCDDisplay */
#define VT_EAGL_SERVER          0x1d5b45c58UL   /* vtable for CA::WindowServer::EAGLServer */
#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))

#define DISPLAY_FLAGS(display)  (*(uint64_t *)((char *)(display) + 0x9f2))
#define DISPLAY_B233(display)   (*(uint8_t *)((char *)(display) + 0x233))

static int (*orig_is_ready)(void *display);
static int (*orig_ignore_update_p)(void *display, void *update);
static uintptr_t (*orig_begin_update)(void *display, void *update);
static uintptr_t (*orig_finish_update)(void *display, void *update, unsigned flags);
static void *(*orig_renderer)(void *server);
static uintptr_t (*orig_render_update)(void *server, void *update, unsigned *flags);

static unsigned calls_ready, calls_ignore, calls_begin, calls_finish, calls_render;
static int last_ready = -1, last_ignore = -1;
static void input_start(void);

static int
my_is_ready(void *display)
{
	int ready = orig_is_ready(display);

	if (ready != last_ready || calls_ready < 3) {
		say("hakios: is_ready: orig=%d -> FORCED 1 (call %u, flags %#llx, b233 %#x)\n", ready,
		    calls_ready, (unsigned long long)DISPLAY_FLAGS(display), DISPLAY_B233(display));
		last_ready = ready;
	}
	calls_ready++;
	/*
	 * fix65: forced.  is_ready() reads the display's power state, and our display never
	 * completes a power change (no real hardware behind it), so orig stays 0 forever -- and
	 * CA::WindowServer then drops every update as "not ready".  The CPU compositor paints
	 * whatever arrives anyway, so ready is always true here.
	 */
	return 1;
}

static int
my_ignore_update_p(void *display, void *update)
{
	int orig = orig_ignore_update_p(display, update) & 1;

	if (orig != last_ignore || calls_ignore < 3) {
		uint64_t *q = (uint64_t *)((char *)display + 0x220);

		say("hakios: ignore_update_p: orig=%d -> FORCED 0 (call %u, flags %#llx, update flags %#x)\n", orig, calls_ignore,
		    (unsigned long long)DISPLAY_FLAGS(display), *(uint32_t *)((char *)update + 0xf0));
		say("hakios:   display +0x220: %#llx %#llx %#llx %#llx %#llx %#llx\n", (unsigned long long)q[0],
		    (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4],
		    (unsigned long long)q[5]);
		last_ignore = orig;
	}
	calls_ignore++;
	return 0; /* Bare-metal MSM8952: NEVER discard any frame update! */
}

static uintptr_t
my_begin_update(void *display, void *update)
{
	uintptr_t r = orig_begin_update(display, update);

	if (calls_begin++ < 6) {
		say("hakios: begin_update -> %#lx (flags %#llx)\n", r, (unsigned long long)DISPLAY_FLAGS(display));
	}
	return r;
}

static uintptr_t
my_finish_update(void *display, void *update, unsigned flags)
{
	uintptr_t r = orig_finish_update(display, update, flags);

	if (calls_finish++ < 6) {
		say("hakios: finish_update(%#x) -> %#lx\n", flags, r);
	}
	return r;
}

/*
 * Every frame the server asks for its renderer, and while there is none it tries again to make a
 * Metal and then an OpenGL ES context, which costs a lot and cannot succeed here. One try is enough:
 * without a renderer the server draws with CA::WindowServer::Server::sw_renderer().
 */
static void *
my_renderer(void *server)
{
	static int failed, said;
	void *renderer;

	if (failed) {
		return NULL;
	}
	renderer = orig_renderer(server);
	if (renderer == NULL) {
		failed = 1;
		say("hakios: no accelerated renderer; the software one will draw\n");
	} else if (!said) {
		said = 1;
		say("hakios: the server has an accelerated renderer (%p)\n", renderer);
	}
	return renderer;
}

static uintptr_t
my_render_update(void *server, void *update, unsigned *flags)
{
	uintptr_t r = orig_render_update(server, update, flags);

	if (calls_render++ < 6) {
		say("hakios: render_update -> %#lx (flags %#x)\n", r, flags ? *flags : 0);
	}
	return r;
}

/*
 * CoreAnimation draws a layer's pixels with one of ninety "functions" (a fragment shader each: an
 * image as it is, an image through a colour matrix, a blur of so many samples, ...), and asks
 * its renderer which of them it has. Its OpenGL ES renderer has not half of them (the Metal one
 * has all). Which ones are asked for and missing is worth knowing: said once each.
 * (The last of the ninety, "cst", is not asked about: the OpenGL ES renderer draws it with no
 * program at all. The lock screen's wallpaper goes that way; see plain_draw_begin() in eagl.c.)
 */
#define VT_GLES_CONTEXT         0x1d5b3b540UL   /* vtable for CA::OGL::GLESContext */

static int gl_loaded;                           /* CoreAnimation draws with OpenGL ES (opengl_es() below) */
static void (*gl_function_refused)(unsigned function);  /* our OpenGL ES library keeps count (eagl.c, the census) */
static int (*orig_function_supported)(void *context);

/*
 * Boot-arg glyes=N[,N...]: the renderer is made to answer that it has those functions after all.
 * It has no shader for them, so CoreAnimation draws them with no program -- which is the way the
 * functions it never asks about are drawn, and which our OpenGL ES library stands in for
 * (eagl.c, plain_draw_begin()); without this, what needs such a function is not drawn at all.
 * For finding out what a missing function is for: with it the draw is made, its numbers arrive
 * (function_uniform below), and the picture says whether the thing was that function's.
 */
/*
 * Saying yes to a function the renderer has no shader for is not enough and not safe. Told yes for
 * "sb3" (40) -- the four samples of a shadow's blur, which our OpenGL ES library has a stand-in for
 * (eagl.c, blur4_fragment) -- CoreAnimation takes the way of one it has a shader for, finds none,
 * and the window server dies; it came back every half a minute, with SpringBoard after it, and the
 * stand-in was never reached: a function it asks about and is told yes to is not one it draws with
 * no program. What is needed is to hand it a program where it looks for one, which is another
 * entry of the same table. Until then it is told the truth, and what needs that function is not
 * drawn -- the shadow under a card of the switcher of running applications.
 */
/*
 * Where the window server died. Its crash reports never arrive here -- the kernel says "Failed to
 * send exception EXC_CORPSE_NOTIFY" and that is the end of it -- so a SIGSEGV in CoreAnimation has
 * been a silent restart every half a minute with nothing to go on. This turns one into a line: the
 * address it touched and where it was, both as they would be in the shared cache as built, which
 * "ipsw dyld a2s" turns into a name. Then the signal is let through to kill the process as before.
 */
static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, lr = 0;

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
	}
	/*
	 * And who called, by walking the frame pointers: on arm64 each frame holds the previous one
	 * at [fp] and its return address at [fp + 8]. The crash itself is usually in a library
	 * function -- strdup of a null name, say -- and the frame that matters is two or three up.
	 */
	char frames[8 * 20 + 1] = "";
	size_t used = 0;
	uintptr_t fp = context != NULL && context->uc_mcontext != NULL ? (uintptr_t)context->uc_mcontext->__ss.__fp : 0;

	for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;      /* the chain only ever goes up the stack */
	}
	say("hakios: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void
watch_for_faults(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = say_the_fault;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	/* An Objective-C exception that nobody catches ends in abort(), not in a fault, and a program
	   that dies that way says nothing at all here -- a run that restarted a daemon 31 times was
	   silent until these three were watched as well. */
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
}

/*
 * CoreAnimation's OpenGL ES renderer keeps the source of one fragment shader per function in a
 * table of its own -- unslid 0x1d5b54790, indexed by the function's number -- and the functions it
 * has no shader for are null entries there. That is the whole of "the renderer has no function
 * 40": GLContext::create_shader takes the source from that table, and function_supported answers
 * from the same place. Made to answer yes without the entry, create_shader hands the null to
 * strdup and the window server dies in strlen, which is what glyes=40 did for a whole afternoon.
 *
 * So the way to give the renderer a function is to give it a source. "sb3" (40) is the single
 * pass of a blur -- GaussianBlurFilter::render -> Context::blur_surface ->
 * BlurState::single_pass_blur -- the one that lays the shadow under a card of the switcher of
 * running applications. It is given the same four places as "ds4" (26) and has no weights of its
 * own, so this is ds4's own shader with the weights taken out, in the dialect the others are
 * written in (sampler2DRect, varying, gl_FragData -- the preamble is added by the renderer).
 *
 * Where this stands. With the entry filled the window server no longer dies: thirty-odd crashes a
 * run become none, with glyes=40 as well, and the function really is drawn -- the renderer rewrites
 * our source into its own dialect (sampler2DRect becomes sampler2D) and compiles it, and the
 * picture changes by a level or two across the whole screen where before it did not change at all.
 *
 * What it does not do is bring back a shadow, and the reason is that none was missing. This
 * function was taken up because a note here said the shadow under a card of the switcher was not
 * drawn; that note was reasoning from "the function is refused", not an observation. Against the
 * same frame drawn by CoreAnimation's software renderer -- which draws everything correctly, only
 * slowly -- the edge of a card is the same to the level: 18, 17, 17, then the card. There is no
 * soft falloff there in either, so there is nothing of that kind to restore.
 *
 * Its effect is nil for a second reason worth writing down: the four places reach the shader as
 * varyings that a vertex shader computes from "uniform highp vec2 offset[4]", and the two groups
 * of four numbers function_uniform delivers for it are of the order of 5e-19 -- zero, for a
 * blur. CoreAnimation asks for the single-pass blur exactly when the radius is too small to need
 * the multi-pass one, so the four samples coincide and the pass copies what it was given.
 */
#define CA_SHADER_TABLE         0x1d5b54790UL

static const char shadow_blur_source[] =
    "uniform lowp sampler2DRect texture0;\n"
    "varying highp vec2 texcoord0, texcoord1, texcoord2, texcoord3;\n"
    "void main () {\n"
    "  lowp vec4 q0, q1, q2, q3;\n"
    "  q0 = texture2DRect(texture0, texcoord0);\n"
    "  q1 = texture2DRect(texture0, texcoord1);\n"
    "  q2 = texture2DRect(texture0, texcoord2);\n"
    "  q3 = texture2DRect(texture0, texcoord3);\n"
    "  gl_FragData[0] = (q0 + q1 + q2 + q3) * 0.25;\n"
    "}\n";

static void
give_the_renderer_a_shader(unsigned function, const char *source)
{
	uintptr_t *entry = (uintptr_t *)(CA_SHADER_TABLE + slide + 8 * (uintptr_t)function);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);

	if (*entry != 0) {
		say("hakios: CoreAnimation's function %u has a shader of its own; left alone\n", function);
		return;
	}
	/* the table is read-only memory shared by every process: a private writable copy of the page */
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("hakios: CoreAnimation's function %u: cannot make the table of shaders writable\n", function);
		return;
	}
	*entry = (uintptr_t)source;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	say("hakios: CoreAnimation's function %u (\"sb3\") is given a shader of ours\n", function);
}

static uint8_t say_yes[90];
static int compile_shaders_said;                /* boot-arg glcompile=1: write down every shader source */

static void
functions_said_yes(const char *arguments)
{
	static const char names[] = HAKIOS_CA_FUNCTIONS;
	const char *argument = strstr(arguments, "glno=");

	/* glno=N[,N...]: and the other way about, for seeing what a function of ours stands in for */
	for (argument = argument != NULL ? argument + 5 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 0;
			say("hakios: the renderer will not answer that it has CoreAnimation's function %u (\"%.3s\")\n",
			    function, names + 4 * function);
		}
		argument = *argument == ',' ? argument + 1 : NULL;
	}
	argument = strstr(arguments, "glyes=");
	for (argument = argument != NULL ? argument + 6 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 1;
			say("hakios: the renderer will answer that it has CoreAnimation's function %u (\"%.3s\")\n",
			    function, names + 4 * function);
		}
		argument = *argument == ',' ? argument + 1 : NULL;
	}
}

/*
 * CA::OGL::Context::compile_shader(const char *) -- slot 0x060 of the same table. The OpenGL ES
 * renderer does not carry its shaders ready-made: it assembles the text of each from a preamble
 * (GLESContext::shader_preamble, slot 0x400) and pieces chosen by what the draw needs, and
 * compiles it here. That makes this the place where it looks for a program, and therefore the
 * place where one can be handed to it for a function it has no pieces for ("sb3", 40: the four
 * samples of a shadow's blur, missing under the cards of the switcher). This writes down what it
 * is asked to compile, so that a run with glyes=40 says whether a source is assembled at all
 * before the window server dies, and what it looks like if so.
 */
static uintptr_t (*orig_compile_shader)(void *context, const char *source);

static uintptr_t
my_compile_shader(void *context, const char *source)
{
	static unsigned told, seen;

	/* that it is called at all is worth one line, because a renderer that compiles nothing here
	   would mean the text is assembled and compiled somewhere this table does not reach */
	if (seen++ == 0) {
		say("hakios: CoreAnimation compiles its shaders through the table (the first of them, %zu characters)\n",
		    source != NULL ? strlen(source) : 0);
	}
	if (compile_shaders_said && told < 24) {
		told++;
		say("hakios: CoreAnimation compiles a shader, %zu characters:\n", source != NULL ? strlen(source) : 0);
		for (size_t at = 0, length = source != NULL ? strlen(source) : 0; at < length; at += 700) {
			say("hakios:  %.700s\n", source + at);
		}
	}
	return orig_compile_shader(context, source);
}

static int
my_function_supported(void *context)
{
	static const char names[] = HAKIOS_CA_FUNCTIONS;
	static uint8_t said[256];
	int supported = orig_function_supported(context);
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	if (!supported && function < 90 && say_yes[function] && gl_loaded) {
		if (!said[function]) {
			said[function] = 1;
			say("hakios: CoreAnimation's function %u (\"%.3s\"): the renderer is made to say it has it\n",
			    function, names + 4 * function);
		}
		return 1;
	}
	if (!supported && gl_function_refused != NULL) {
		gl_function_refused(function);
	}
	if (!supported && !said[function]) {
		said[function] = 1;
		say("hakios: CoreAnimation's OpenGL ES renderer has no function %u (\"%.3s\"): what needs it is not drawn\n",
		    function, function < 90 ? names + 4 * function : "?");
	}
	return supported;
}

/*
 * A function that takes numbers besides its textures (a colour matrix, say) is given them through
 * GLContext::function_uniform(), which hands them to the function's shader. The functions the
 * OpenGL ES renderer has no shader for are drawn all the same, with no program, and their numbers
 * go nowhere. Our OpenGL ES library draws some of those with programs of its own (eagl.c,
 * plain_draw_begin()): it is told the numbers here, and with them the context, in which it
 * finds which function a draw is.
 */
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);
static void (*gl_function_values)(void *context, unsigned function, unsigned index, size_t components, size_t count,
    const float *values);

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	/*
	 * Who is calling. Told yes for a function it has no shader for (glyes=40), CoreAnimation gets
	 * this far -- it hands the function its numbers -- and then dies in whatever comes next, which
	 * is where it takes the program for the function out of a table of its own. These addresses,
	 * unslid, name that caller for "ipsw dyld a2s", and it is the only thread to pull: nothing is
	 * ever compiled for the function (no shader arrives at our library), and the compiler is not
	 * reached through the renderer's table either.
	 */
	if (say_yes[function < 90 ? function : 0]) {
		static uint8_t told[90];

		if (function < 90 && !told[function]) {
			told[function] = 1;
			say("hakios: function %u takes its numbers from %#lx %#lx %#lx\n", function,
			    (uintptr_t)__builtin_return_address(0) - slide,
			    (uintptr_t)__builtin_return_address(1) - slide,
			    (uintptr_t)__builtin_return_address(2) - slide);
		}
	}
	if (gl_function_values != NULL) {
		gl_function_values(context, function, index, components, count, values);
	}
	orig_function_uniform(context, index, components, count, values);
}

/*
 * fix78: the ObjC display's own update path, watched.  fix77's enumeration named the methods;
 * this round asks what they answer -- whether the server ever calls update (our kick does),
 * what canUpdate/canUpdateWithFlags/needsUpdate say, and whether the update request callback
 * (the object EAGLServer::update dispatches through, [display+0x28]) is ever set.  Every hook
 * calls the original; this round changes no answer.
 */
static int safe_copy(uintptr_t from, void *into, size_t length);   /* defined below: read without taking a fault */

static uintptr_t
ca_unslide(uintptr_t p)
{
	/* the shared cache lives above CACHE_BASE; heap objects below it */
	return (p > CACHE_BASE) ? p - slide : p;
}

/*
 * fix82: what the ObjC update path is really looking at.  The static read said
 * [display+8] -> EAGLServer at +0x40 -> AppleCLCDDisplay at +0x60, and that EAGLServer::update
 * reads the callback at C+0x28 while the ObjC setter stores it at C+0x680.  One of the two
 * offsets is wrong; print the whole chain and both callback slots, before and after the
 * original runs.  Reads go through vm_read_overwrite, so a stale pointer prints as NULL, not
 * as a fault.
 */
static void
ca_dump_chain(id self, const char *when)
{
	uintptr_t impl = 0, server = 0, disp = 0, vt = 0, iomfb = 0;
	uint64_t cb28 = 0, cb680 = 0, flags = 0, w250 = 0;

	if (!safe_copy((uintptr_t) self + 8, &impl, sizeof(impl)) || impl == 0) {
		say("hakios: CA chain %s: self=%p no _impl\n", when, (void *) self);
		return;
	}
	safe_copy(impl + 0x40, &server, sizeof(server));
	if (server == 0) {
		say("hakios: CA chain %s: self=%p impl=%p srv=NULL\n", when, (void *) self, (void *) impl);
		return;
	}
	safe_copy(server + 0x60, &disp, sizeof(disp));
	if (disp == 0) {
		say("hakios: CA chain %s: self=%p impl=%p srv=%p disp=NULL\n",
		    when, (void *) self, (void *) impl, (void *) server);
		return;
	}
	safe_copy(disp, &vt, sizeof(vt));
	safe_copy(disp + 0x28, &cb28, sizeof(cb28));
	safe_copy(disp + 0x680, &cb680, sizeof(cb680));
	safe_copy(disp + 0x9f2, &flags, sizeof(flags));
	safe_copy(disp + 0x250, &w250, sizeof(w250));
	safe_copy(disp + 0x318, &iomfb, sizeof(iomfb));
	say("hakios: CA chain %s: self=%p impl=%p srv=%p disp=%p vt=%#lx\n",
	    when, (void *) self, (void *) impl, (void *) server, (void *) disp, (unsigned long) ca_unslide(vt));
	say("hakios: CA cb %s: c28=%#lx c680=%#lx flags=%#llx w250=%#llx iomfb=%p\n",
	    when, (unsigned long) ca_unslide(cb28), (unsigned long) ca_unslide(cb680),
	    (unsigned long long) flags, (unsigned long long) w250, (void *) iomfb);
}

static void (*orig_ca_update)(id, SEL);
static BOOL (*orig_ca_canUpdate)(id, SEL);
static BOOL (*orig_ca_canUpdateWithFlags)(id, SEL, unsigned);
static BOOL (*orig_ca_needsUpdate)(id, SEL);
static void (*orig_ca_setUpdateRequestCallback)(id, SEL, id);
static id (*orig_ca_updateRequestCallback)(id, SEL);
static void (*orig_ca_powerStateDidChange)(id, SEL);
static BOOL (*orig_ca_disablesUpdates)(id, SEL);
static void (*orig_ca_setDisablesUpdates)(id, SEL, BOOL);

/* fix129: backboardd başlangıçta "disabling framebuffer changes on display (1)" setzt ve asla
 * temizlemiyor, bu yüzden CoreAnimation bir kere çizer ve asla yenilemez -- panel sabit mavi kalır.
 * setDisablesUpdates: çağrısındaki değeri her zaman NO yapar; getter her çağrıldığında durumu loglar. */
static BOOL
hooked_ca_disablesUpdates(id self, SEL _cmd)
{
	BOOL r = orig_ca_disablesUpdates ? orig_ca_disablesUpdates(self, _cmd) : NO;
	static unsigned n;

	if (n++ < 16) {
		say("hakios: -[CAWindowServerDisplay disablesUpdates] -> %d (call %u)\n", (int) r, n);
	}
	return r;
}

static void
hooked_ca_setDisablesUpdates(id self, SEL _cmd, BOOL value)
{
	static unsigned n;

	if (n++ < 16) {
		say("hakios: -[CAWindowServerDisplay setDisablesUpdates:%d] -> forced NO (call %u)\n",
		    (int) value, n);
	}
	if (orig_ca_setDisablesUpdates) {
		orig_ca_setDisablesUpdates(self, _cmd, NO);
	}
}

static void
hooked_ca_update(id self, SEL _cmd)
{
	static unsigned n;

	if (n++ < 8) {
		say("hakios: -[CAWindowServerDisplay update] call %u\n", n);
		ca_dump_chain(self, "pre");
	}
	if (orig_ca_update) {
		orig_ca_update(self, _cmd);
	}
	if (n <= 8) {
		ca_dump_chain(self, "post");
	}
}

static BOOL
hooked_ca_canUpdate(id self, SEL _cmd)
{
	BOOL r = orig_ca_canUpdate ? orig_ca_canUpdate(self, _cmd) : NO;
	static unsigned n;

	if (n++ < 8) {
		say("hakios: -[CAWindowServerDisplay canUpdate:] -> %d\n", (int) r);
	}
	return r;
}

static BOOL
hooked_ca_canUpdateWithFlags(id self, SEL _cmd, unsigned flags)
{
	BOOL r = orig_ca_canUpdateWithFlags ? orig_ca_canUpdateWithFlags(self, _cmd, flags) : NO;
	static unsigned n;

	if (n++ < 8) {
		say("hakios: -[CAWindowServerDisplay canUpdateWithFlags:%#x] -> %d\n", flags, (int) r);
	}
	return r;
}

static BOOL
hooked_ca_needsUpdate(id self, SEL _cmd)
{
	BOOL r = orig_ca_needsUpdate ? orig_ca_needsUpdate(self, _cmd) : NO;
	static unsigned n;

	if (n++ < 8) {
		say("hakios: -[CAWindowServerDisplay needsUpdate] -> %d\n", (int) r);
	}
	return r;
}

static void
hooked_ca_setUpdateRequestCallback(id self, SEL _cmd, id callback)
{
	static unsigned n;

	if (n++ < 8) {
		say("hakios: -[CAWindowServerDisplay setUpdateRequestCallback:%p] call %u\n", (void *) callback, n);
	}
	if (orig_ca_setUpdateRequestCallback) {
		orig_ca_setUpdateRequestCallback(self, _cmd, callback);
	}
}

static id
hooked_ca_updateRequestCallback(id self, SEL _cmd)
{
	id r = orig_ca_updateRequestCallback ? orig_ca_updateRequestCallback(self, _cmd) : nil;
	static unsigned n;

	if (n++ < 8) {
		say("hakios: -[CAWindowServerDisplay updateRequestCallback] -> %p\n", (void *) r);
	}
	return r;
}

static void
hooked_ca_powerStateDidChange(id self, SEL _cmd)
{
	static unsigned n;

	if (n++ < 8) {
		say("hakios: -[CAWindowServerDisplay powerStateDidChange:] call %u\n", n);
	}
	if (orig_ca_powerStateDidChange) {
		orig_ca_powerStateDidChange(self, _cmd);
	}
}

static void
ca_display_probe_fixes(void)
{
	Class cls = objc_getClass("CAWindowServerDisplay");
	Method m;

	if (cls == NULL) {
		say("hakios: CAWindowServerDisplay not found for the probe\n");
		return;
	}
	m = class_getInstanceMethod(cls, sel_registerName("update"));
	if (m != NULL) {
		orig_ca_update = (void (*)(id, SEL)) method_setImplementation(m, (IMP) hooked_ca_update);
	}
	m = class_getInstanceMethod(cls, sel_registerName("canUpdate:"));
	if (m != NULL) {
		orig_ca_canUpdate = (BOOL (*)(id, SEL)) method_setImplementation(m, (IMP) hooked_ca_canUpdate);
	}
	m = class_getInstanceMethod(cls, sel_registerName("canUpdateWithFlags:"));
	if (m != NULL) {
		orig_ca_canUpdateWithFlags = (BOOL (*)(id, SEL, unsigned)) method_setImplementation(m,
		    (IMP) hooked_ca_canUpdateWithFlags);
	}
	m = class_getInstanceMethod(cls, sel_registerName("needsUpdate"));
	if (m != NULL) {
		orig_ca_needsUpdate = (BOOL (*)(id, SEL)) method_setImplementation(m, (IMP) hooked_ca_needsUpdate);
	}
	m = class_getInstanceMethod(cls, sel_registerName("setUpdateRequestCallback:"));
	if (m != NULL) {
		orig_ca_setUpdateRequestCallback = (void (*)(id, SEL, id)) method_setImplementation(m,
		    (IMP) hooked_ca_setUpdateRequestCallback);
	}
	m = class_getInstanceMethod(cls, sel_registerName("updateRequestCallback"));
	if (m != NULL) {
		orig_ca_updateRequestCallback = (id (*)(id, SEL)) method_setImplementation(m,
		    (IMP) hooked_ca_updateRequestCallback);
	}
	m = class_getInstanceMethod(cls, sel_registerName("powerStateDidChange:"));
	if (m != NULL) {
		orig_ca_powerStateDidChange = (void (*)(id, SEL)) method_setImplementation(m,
		    (IMP) hooked_ca_powerStateDidChange);
	}
	m = class_getInstanceMethod(cls, sel_registerName("disablesUpdates"));
	if (m != NULL) {
		orig_ca_disablesUpdates = (BOOL (*)(id, SEL)) method_setImplementation(m,
		    (IMP) hooked_ca_disablesUpdates);
	}
	m = class_getInstanceMethod(cls, sel_registerName("setDisablesUpdates:"));
	if (m != NULL) {
		orig_ca_setDisablesUpdates = (void (*)(id, SEL, BOOL)) method_setImplementation(m,
		    (IMP) hooked_ca_setDisablesUpdates);
	}
	say("hakios: CAWindowServerDisplay probe: update=%d canUpdate=%d needsUpdate=%d callback=%d power=%d disablesUpdates=%d setDisablesUpdates=%d\n",
	    orig_ca_update != NULL, orig_ca_canUpdate != NULL, orig_ca_needsUpdate != NULL,
	    orig_ca_setUpdateRequestCallback != NULL, orig_ca_powerStateDidChange != NULL,
	    orig_ca_disablesUpdates != NULL, orig_ca_setDisablesUpdates != NULL);
	/*
	 * fix82: the ObjC method bodies, unslid, so the host can disassemble exactly what update
	 * calls and settle the C+0x28 / C+0x680 question from the real code, not from inference.
	 */
	say("hakios: CA IMPs: update=%#lx canUpd=%#lx canUpdF=%#lx needs=%#lx setCb=%#lx getCb=%#lx power=%#lx\n",
	    (unsigned long) ca_unslide((uintptr_t) orig_ca_update),
	    (unsigned long) ca_unslide((uintptr_t) orig_ca_canUpdate),
	    (unsigned long) ca_unslide((uintptr_t) orig_ca_canUpdateWithFlags),
	    (unsigned long) ca_unslide((uintptr_t) orig_ca_needsUpdate),
	    (unsigned long) ca_unslide((uintptr_t) orig_ca_setUpdateRequestCallback),
	    (unsigned long) ca_unslide((uintptr_t) orig_ca_updateRequestCallback),
	    (unsigned long) ca_unslide((uintptr_t) orig_ca_powerStateDidChange));
}

static void
window_server_fixes(void)
{
	orig_is_ready = replace_virtual("is_ready", VSLOT(VT_APPLE_CLCD_DISPLAY, 0xb0), 0x1882f0908UL, my_is_ready);
	orig_ignore_update_p = replace_virtual("ignore_update_p", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x138), 0x1882e0c90UL,
	    my_ignore_update_p);
	orig_begin_update = replace_virtual("begin_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x170), 0x1882ddda4UL,
	    my_begin_update);
	orig_finish_update = replace_virtual("finish_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x178), 0x1882c8568UL,
	    my_finish_update);
	orig_renderer = replace_virtual("renderer", VSLOT(VT_EAGL_SERVER, 0x1a0), 0x1882e2548UL, my_renderer);
	orig_render_update = replace_virtual("render_update", VSLOT(VT_EAGL_SERVER, 0x178), 0x18835770cUL,
	    my_render_update);
	if (gl_loaded) {
		orig_function_supported = replace_virtual("function_supported", VSLOT(VT_GLES_CONTEXT, 0x378), 0x1884a9308UL,
		    my_function_supported);
		if (gl_function_values != NULL) {
			orig_function_uniform = replace_virtual("function_uniform", VSLOT(VT_GLES_CONTEXT, 0x388), 0x1884a9350UL,
			    my_function_uniform);
		}
		/* always, because the boot-args are read in another place than this and the order of the
		   two is not ours to rely on; the hook writes nothing unless glcompile=1 */
		orig_compile_shader = (uintptr_t (*)(void *, const char *))replace_virtual("compile_shader",
		    VSLOT(VT_GLES_CONTEXT, 0x060), 0x1883fc808UL, my_compile_shader);
		say("hakios: the shader compiler of CoreAnimation is %s\n", orig_compile_shader ? "watched" : "NOT watched");
		watch_for_faults();
		{
			char arguments[1024] = "";
			size_t size = sizeof(arguments) - 1;

			if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
			    strstr(arguments, "glsb3=1") != NULL) {
				give_the_renderer_a_shader(40, shadow_blur_source);
			}
		}
	}
	say("hakios: window server hooks %s\n", orig_is_ready && orig_ignore_update_p && orig_begin_update &&
	    orig_finish_update && orig_renderer && orig_render_update ? "installed" : "INCOMPLETE");

	/*
	 * fix77: the map we are missing.  Nine rounds say the ObjC display object is open (blanked=0,
	 * disablesUpdates=0, curMode good) and the C++ display is now ready (is_ready=1 when asked);
	 * what never happens is the server's OWN update loop: its vtable slots see zero calls.  So
	 * the gate lives in the ObjC server layer -- the question is which method of it stands
	 * closed.  Instead of guessing names, enumerate the server's and the display's methods whose
	 * selectors speak of updating, powering, waking, timers and vsync, with their unslid IMPs:
	 * that is the map the next round hooks, and for "ipsw dyld a2s" the addresses are a sentence.
	 */
	{
		static const char *const classes[] = { "CAWindowServer", "CAWindowServerDisplay" };
		static const char *const needles[] = { "update", "Update", "wake", "Wake", "power", "Power",
		    "timer", "Timer", "vsync", "VSync", "displayDid", "awake" };

		for (unsigned c = 0; c < 2; c++) {
			Class cls = objc_getClass(classes[c]);
			unsigned nmethods = 0;
			Method *methods = cls != NULL ? class_copyMethodList(cls, &nmethods) : NULL;
			unsigned shown = 0;

			for (unsigned m = 0; methods != NULL && m < nmethods && shown < 40; m++) {
				const char *name = sel_getName(method_getName(methods[m]));
				bool interesting = false;

				for (unsigned k = 0; k < sizeof(needles) / sizeof(needles[0]) && !interesting; k++) {
					interesting = strstr(name, needles[k]) != NULL;
				}
				if (interesting) {
					shown++;
					say("hakios:   %c%s %u: %s imp=%#lx\n", c == 0 ? '+' : '+', classes[c], m, name,
					    (unsigned long)((uintptr_t)method_getImplementation(methods[m]) - slide));
				}
			}
			free(methods);
			say("hakios: %s: %u methods, %u of interest\n", classes[c], nmethods, shown);
		}
	}

	/* fix78: ask the display's own update path what it answers; see ca_display_probe_fixes(). */
	ca_display_probe_fixes();
}

/* ---- backboardd: no ambient light sensor ------------------------------------------------------------ */

/*
 * CoreBrightness (-[BLControl waitForALSArrival], called while the window server starts) waits
 * five seconds for the ambient light sensor to turn up among the HID services. There is none
 * and none will come: do not wait for that.
 *
 * Wait for something else instead, at this convenient point: our own touch screen service.
 * backboardd takes for its touch screen a service that is there when its touch processing
 * starts, a moment later; one that turns up afterwards it does not notice. (For as long as
 * the five seconds were waited here, ours was always in time by accident.)
 */
static dispatch_semaphore_t input_services_made;

static void
wait_for_als_arrival(__unused id self, __unused SEL selector)
{
	if (input_services_made != NULL &&
	    dispatch_semaphore_wait(input_services_made, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
		say("hakios: input: no touch screen service after five seconds; going on without\n");
	}
}

__attribute__((unused)) static void
brightness_fixes(void)
{
	Class cls = objc_getClass("BLControl");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName("waitForALSArrival")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != 0x1af043734UL + slide) {
		say("hakios: -[BLControl waitForALSArrival]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)wait_for_als_arrival);
}

/* ---- backboardd: the touch screen and the buttons ---------------------------------------------- */

/* IOKit's HID event system, the part that has no public header on iOS (prototypes checked against 20F66) */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

/* IOHIDVirtualServiceClientCallbacksV2 */
struct virtual_service_callbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateDigitizerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t transducer, uint32_t index,
    uint32_t identity, uint32_t event_mask, uint32_t button_mask, double x, double y, double z, double tip_pressure,
    double barrel_pressure, double twist, Boolean range, Boolean touch, uint32_t options);
extern IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t index, uint32_t identity,
    uint32_t event_mask, double x, double y, double z, double tip_pressure, double twist, Boolean range, Boolean touch,
    uint32_t options);
extern IOHIDEventRef IOHIDEventCreateKeyboardEvent(CFAllocatorRef allocator, uint64_t time, uint32_t usage_page, uint32_t usage,
    Boolean down, uint32_t options);
extern void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);
extern void IOHIDEventSetFloatValue(IOHIDEventRef event, uint32_t field, double value);

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_CLIENT_RATE_CONTROLLED      3       /* what IOHIDEventSystemClientCreate() makes */
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define HID_PAGE_CONSUMER               0x0c
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_USAGE_POWER                 0x30    /* the lock button */
#define HID_USAGE_MENU                  0x40    /* the home button */
#define HID_USAGE_VOLUME_UP             0xe9
#define HID_USAGE_VOLUME_DOWN           0xea
#define DIGITIZER_HAND                  3       /* kIOHIDDigitizerTransducerTypeHand */
#define DIGITIZER_RANGE                 0x1     /* event mask: what has changed */
#define DIGITIZER_TOUCH                 0x2
#define DIGITIZER_POSITION              0x4
#define DIGITIZER_FROM_EDGE_TIP         0x800   /* not a change: this finger came in over an edge of the screen */
#define FIELD_IS_BUILT_IN               0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019
#define FIELD_DIGITIZER_QUALITY         0xb0009
#define FIELD_DIGITIZER_DENSITY         0xb000a
#define FIELD_DIGITIZER_MAJOR_RADIUS    0xb0014
#define FIELD_DIGITIZER_MINOR_RADIUS    0xb0015

#define MAX_FINGERS     10

/*
 * A finger that lands this close to an edge of the screen (a fraction of the width or the height)
 * is said to have come in over that edge. Apple's touch controller tells that from the shape of
 * the contact and marks the finger's events; UIKit's screen edge recognizers (the swipe up that
 * leaves an application, the swipe back in a navigation stack, the pull down from the top) take
 * no touch that is not so marked, whatever its path. All we have is where the finger landed.
 */
#define EDGE_SIDE       (16.0 / 375)
#define EDGE_TOP        (16.0 / 812)
#define EDGE_BOTTOM     (34.0 / 812)    /* the strip below the applications' content, where the home bar is */

static struct {
	bool touching, edge;
	double x, y;
} fingers[MAX_FINGERS];

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* A service of ours (the callbacks' refcon): what it says of itself, and whether the event server has taken it. */
struct service {
	CFMutableDictionaryRef properties;
	volatile bool opened;
};

/* The properties of a service of ours; the event server asks for them one at a time. */
static CFTypeRef
service_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	static CFMutableSetRef asked;
	CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);

	if (asked == NULL) {
		asked = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (value == NULL && !CFSetContainsValue(asked, key) && CFSetGetCount(asked) < 40) {
		char name[96] = "?";

		CFSetAddValue(asked, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("hakios: input: the event server asks for property \"%s\", which we do not have\n", name);
	}
	return value ? CFRetain(value) : NULL;
}

/*
 * What the window server last asked of the touch screen ("TouchDetectionMode"), in the low
 * byte: 1 = report touches; TOUCH_MODE_WAKE while the display is off and a tap is to wake it
 * -- which Apple's multitouch controller then does on its own; TOUCH_MODE_OFF = nothing at
 * all. (Seen: 0xff and 0xfe during the boot, 0x1 with the display on, 0x1fe after the lock
 * button.)
 */
#define TOUCH_MODE_WAKE         0xfe
#define TOUCH_MODE_OFF          0xff
static volatile int touch_mode;

/* What the event server tells a service of ours: said once per name, to learn what there is. */
static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
	static CFMutableSetRef told;
	char name[96] = "?", text[160] = "?";
	CFStringRef description;

	/* the last thing the event server does when it opens a service; see make_service() */
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
	    CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
		((struct service *)refcon)->opened = true;
	}
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("TouchDetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		int mode = 0;

		CFNumberGetValue(value, kCFNumberIntType, &mode);
		if (mode != touch_mode) {
			say("hakios: input: touch detection mode %#x\n", mode);
		}
		touch_mode = mode;
	}
	if (told == NULL) {
		told = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (key != NULL && !CFSetContainsValue(told, key) && CFSetGetCount(told) < 24) {
		CFSetAddValue(told, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		description = value ? CFCopyDescription(value) : NULL;
		if (description != NULL) {
			CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(description);
		}
		say("hakios: input: the event server sets \"%s\" = %s\n", name, text);
	}
	return true;
}

static void
service_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    __unused uint32_t type, __unused CFDictionaryRef property)
{
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property,
};

/*
 * A HID service of ours inside the event server: built in, with one usage.
 *
 * The event server is this very program, and it is still starting. A client that reaches it at
 * the wrong moment of that (backboardd's "HID system open ... start": the connection is made
 * and then replaced, "IOHIDEventSystemServer died. Reestablishing connection.") is left with a
 * service the server cannot talk to and never opens -- and a touch screen that backboardd waits
 * for in vain ("still waiting for a main screen digitizer"). When that moment comes depends on
 * how fast the machine is and on how many processors it has, so no fixed delay avoids it:
 * the service is made again until the server has opened it.
 */
static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
	struct service *made = calloc(1, sizeof(*made));
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
	CFArrayRef pairs;

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_number);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPI"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("redwood"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	if (display) {
		CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	}
	/* the dictionary stays: it is what service_copy_property() answers from */
	made->properties = properties;
	for (int attempt = 1; attempt <= 20; attempt++) {
		made->opened = false;
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, &service_callbacks, NULL, made);
		if (service == NULL) {
			if (attempt % 5 == 1) {
				say("hakios: input: IOHIDVirtualServiceClientCreateWithCallbacks returned NULL for \"%s\" (attempt %d/20), retrying...\n", product, attempt);
			}
			usleep(500000);
			continue;
		}
		for (int tenths = 0; tenths < 20 && !made->opened; tenths++) {
			usleep(100000);
		}
		if (made->opened) {
			say("hakios: input: \"%s\" successfully opened by event server on attempt %d!\n", product, attempt);
			break;
		}
		if (attempt % 5 == 1) {
			say("hakios: input: the event server has not opened \"%s\" (attempt %d/20): recreating...\n", product, attempt);
		}
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

static void (*s_IOHIDEventSetSenderID)(IOHIDEventRef, uint64_t) = NULL;
static volatile bool g_hid_server_ready = false;
static pthread_once_t g_input_once = PTHREAD_ONCE_INIT;
static IOHIDServiceClientRef g_touch_service = NULL;
static IOHIDServiceClientRef g_button_service = NULL;

#define HAKIOS_TOUCH_SENDER_ID          0x10000027FULL
#define HAKIOS_BUTTON_SENDER_ID         0x100000280ULL

static void hakios_inject_hid_event_direct(IOHIDEventRef event) {
	if (!event) return;

	Class clsBKHID = objc_getClass("BKHIDSystemInterface");
	if (clsBKHID) {
		SEL selShared = sel_registerName("sharedInstance");
		Method mShared = class_getClassMethod(clsBKHID, selShared);
		if (mShared) {
			id shared = ((id (*)(id, SEL))method_getImplementation(mShared))((id)clsBKHID, selShared);
			if (shared) {
				SEL selInject = sel_registerName("injectHIDEvent:");
				Method mInject = class_getInstanceMethod(clsBKHID, selInject);
				if (mInject) {
					((void (*)(id, SEL, IOHIDEventRef))method_getImplementation(mInject))(shared, selInject, event);
				}
			}
		}
	}

	Class clsTimer = objc_getClass("BKUserEventTimer");
	if (clsTimer) {
		SEL selShared = sel_registerName("sharedInstance");
		Method mShared = class_getClassMethod(clsTimer, selShared);
		if (mShared) {
			id timer = ((id (*)(id, SEL))method_getImplementation(mShared))((id)clsTimer, selShared);
			if (timer) {
				SEL selUserEvent = sel_registerName("userEventOccurredOnDisplay:");
				Method mUserEvent = class_getInstanceMethod(clsTimer, selUserEvent);
				if (mUserEvent) {
					((void (*)(id, SEL, id))method_getImplementation(mUserEvent))(timer, selUserEvent, nil);
				}
			}
		}
	}
}

/* One frame of the touch screen: the hand, and under it every finger that is down or has just lifted. */
static void
send_touch(IOHIDServiceClientRef screen, const struct hakios_touch *t)
{
	uint32_t change = t->type == HAKIOS_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
	unsigned f = t->finger;

	if (f >= MAX_FINGERS || (t->type != HAKIOS_TOUCH_DOWN && !fingers[f].touching)) {
		return;
	}
	fingers[f].touching = t->type != HAKIOS_TOUCH_UP;
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (t->type == HAKIOS_TOUCH_DOWN) {
		fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
		    fingers[f].y > 1 - EDGE_BOTTOM;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fingers[i].touching;
	}

	hand = IOHIDEventCreateDigitizerEvent(NULL, t->time, DIGITIZER_HAND, 0, 0,
	    (change & ~DIGITIZER_RANGE) | (fingers[f].edge ? DIGITIZER_FROM_EDGE_TIP : 0), 0, fingers[f].x,
	    fingers[f].y, 0, 0, 0, 0, any, any, 0);
	IOHIDEventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	IOHIDEventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (!fingers[i].touching && i != f) {
			continue;
		}
		finger = IOHIDEventCreateDigitizerFingerEvent(NULL, t->time, i + 1, i + 2,
		    (i == f ? change : 0) | (fingers[i].edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
		    fingers[i].touching ? 1.0 : 0.0, 0, fingers[i].touching, fingers[i].touching, 0);
		IOHIDEventSetIntegerValue(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
		IOHIDEventSetIntegerValue(finger, FIELD_IS_BUILT_IN, 1);
		IOHIDEventSetFloatValue(finger, FIELD_DIGITIZER_MAJOR_RADIUS, 6.0);
		IOHIDEventSetFloatValue(finger, FIELD_DIGITIZER_MINOR_RADIUS, 6.0);
		IOHIDEventSetFloatValue(finger, FIELD_DIGITIZER_QUALITY, 1.0);
		IOHIDEventSetFloatValue(finger, FIELD_DIGITIZER_DENSITY, 1.0);
		IOHIDEventAppendEvent(hand, finger, 0);
		CFRelease(finger);
	}
	if (!s_IOHIDEventSetSenderID) {
		s_IOHIDEventSetSenderID = (void (*)(IOHIDEventRef, uint64_t))dlsym(RTLD_DEFAULT, "IOHIDEventSetSenderID");
	}
	if (s_IOHIDEventSetSenderID) {
		s_IOHIDEventSetSenderID(hand, HAKIOS_TOUCH_SENDER_ID);
	}
	if (screen) {
		IOHIDVirtualServiceClientDispatchEvent(screen, hand);
	}
	hakios_inject_hid_event_direct(hand);
	CFRelease(hand);
}

/*
 * Whether the side button is held right now, for our code in SpringBoard (inject_sb.c,
 * lock_long_press()): kept as the state of a Darwin notification, 2 = held, 1 = released
 * (0, what a name nobody has set says, = not known), and set before the event is sent.
 */
static void
publish_lock_button(bool down)
{
	static int token = NOTIFY_TOKEN_INVALID;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.hakios.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return;
	}
	notify_set_state(token, down ? 2 : 1);
}

static void
send_button(IOHIDServiceClientRef buttons, const struct hakios_touch *t)
{
	uint32_t usage = t->finger == HAKIOS_BUTTON_LOCK ? HID_USAGE_POWER : t->finger == HAKIOS_BUTTON_HOME ? HID_USAGE_MENU :
	    t->finger == HAKIOS_BUTTON_VOLUME_UP ? HID_USAGE_VOLUME_UP : t->finger == HAKIOS_BUTTON_VOLUME_DOWN ?
	    HID_USAGE_VOLUME_DOWN : 0;
	IOHIDEventRef event;

	if (usage == 0) {
		return;
	}
	if (t->finger == HAKIOS_BUTTON_LOCK) {
		publish_lock_button(t->type == HAKIOS_BUTTON_DOWN);
	}
	event = IOHIDEventCreateKeyboardEvent(NULL, t->time, HID_PAGE_CONSUMER, usage, t->type == HAKIOS_BUTTON_DOWN, 0);
	if (!event) return;

	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	if (!s_IOHIDEventSetSenderID) {
		s_IOHIDEventSetSenderID = (void (*)(IOHIDEventRef, uint64_t))dlsym(RTLD_DEFAULT, "IOHIDEventSetSenderID");
	}
	if (s_IOHIDEventSetSenderID) {
		s_IOHIDEventSetSenderID(event, HAKIOS_BUTTON_SENDER_ID);
	}
	if (buttons) {
		IOHIDVirtualServiceClientDispatchEvent(buttons, event);
	}
	hakios_inject_hid_event_direct(event);
	CFRelease(event);
}

/*
 * Boot-arg touch_lat=1: a line for every touch sample, "tl: i TYPE posted US read US sent US" -- when
 * the kernel stamped it, when this thread had it, when the event system had the event -- in
 * microseconds of mach_absolute_time, the kernel's own time (tools/latency.py).
 */
static bool touch_latency;
static mach_timebase_info_data_t timebase;

static uint64_t
micros(uint64_t absolute)
{
	return absolute * timebase.numer / timebase.denom / 1000;
}

static void *
input_thread(__unused void *arg)
{
	struct hakios_touch t;
	unsigned count = 0, waking = 0;         /* waking: the fingers whose touch went into waking the display */
	int device;

	pthread_setname_np("shamrock input");
	say("hakios: input_thread running (tid=%p) — Zero-Deadlock Direct In-Process HID Mode\n", (void *)pthread_self());

	{
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			touch_latency = strstr(arguments, "touch_lat=1") != NULL;
		}
		mach_timebase_info(&timebase);
	}

	device = -1;
	for (int attempt = 0; attempt < 30; attempt++) {
		device = open(HAKIOS_TOUCH_DEVICE, O_RDONLY);
		if (device >= 0) break;
		usleep(100000);
	}

	if (device < 0) {
		say("hakios: input: failed to open %s: %s\n", HAKIOS_TOUCH_DEVICE, strerror(errno));
		return NULL;
	}
	say("hakios: input: device %s opened (fd=%d), ready for touch & button events!\n", HAKIOS_TOUCH_DEVICE, device);

	while (read(device, &t, sizeof(t)) == (ssize_t)sizeof(t)) {
		uint64_t read_at = mach_absolute_time();

		if (count++ < 10) {
			say("hakios: input: sample type %u finger %u at %u %u\n", t.type, t.finger, t.x, t.y);
		}
		if (t.type > HAKIOS_TOUCH_UP) {
			send_button(NULL, &t);
		} else if (t.type == HAKIOS_TOUCH_DOWN && (touch_mode & 0xff) == TOUCH_MODE_WAKE) {
			/*
			 * The display is off and a tap is to wake it. The lock button is what wakes it here:
			 * press it for the finger, and keep the rest of this touch to ourselves.
			 */
			struct hakios_touch press = { .time = t.time, .type = HAKIOS_BUTTON_DOWN, .finger = HAKIOS_BUTTON_LOCK };

			send_button(NULL, &press);
			press.type = HAKIOS_BUTTON_UP;
			send_button(NULL, &press);
			waking |= 1u << (t.finger & 31);
		} else if (waking & (1u << (t.finger & 31))) {
			if (t.type == HAKIOS_TOUCH_UP) {
				waking &= ~(1u << (t.finger & 31));
			}
		} else {
			send_touch(NULL, &t);
			if (touch_latency) {
				say("tl: i %u posted %llu read %llu sent %llu\n", t.type, micros(t.time), micros(read_at),
				    micros(mach_absolute_time()));
			}
		}
	}
	say("hakios: input: read loop exited (%s)\n", strerror(errno));
	return NULL;
}

static void
start_input_thread_once(void)
{
	pthread_t thread;

	say("hakios: input_start() spawning input_thread...\n");
	input_services_made = dispatch_semaphore_create(0);
	if (pthread_create(&thread, NULL, input_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

static void
input_start(void)
{
	pthread_once(&g_input_once, start_input_thread_once);
}

/* ---- containermanagerd ---------------------------------------------------------------------------- */

static BOOL
personas_are_supported(__unused id self, __unused SEL selector)
{
	return NO;
}

static void
container_manager_fixes(void)
{
	Class cls = objc_getClass("MCMUserIdentityCache");
	Method method = cls ? class_getClassMethod(cls, sel_registerName("personasAreSupported")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != 0x1c3ea6298UL + slide) {
		say("hakios: +[MCMUserIdentityCache personasAreSupported]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)personas_are_supported);
	say("hakios: container manager: no personas\n");
}

static id (*orig_BKHIDSystemInterface_sharedInstance)(id self, SEL _cmd);
static int hid_bypass_start;
static id (*orig_BKHIDSystemInterface_init)(id self, SEL _cmd);
static void biometric_bypass_fixes(void);

static id
my_BKHIDSystemInterface_init(id self, SEL _cmd)
{
	say("hakios: -[BKHIDSystemInterface init] START (%p)\n", (void *)self);
	biometric_bypass_fixes();
	id res = nil;
	if (orig_BKHIDSystemInterface_init) {
		res = orig_BKHIDSystemInterface_init(self, _cmd);
	} else {
		struct objc_super sup = { self, objc_getClass("NSObject") };
		res = ((id (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
	}
	say("hakios: -[BKHIDSystemInterface init] DONE -> %p\n", (void *)res);
	return res;
}

static id
my_BKHIDSystemInterface_sharedInstance(id self, SEL _cmd)
{
	say("hakios: +[BKHIDSystemInterface sharedInstance] START\n");
	biometric_bypass_fixes();
	id res = orig_BKHIDSystemInterface_sharedInstance(self, _cmd);
	say("hakios: +[BKHIDSystemInterface sharedInstance] DONE -> %p\n", (void *)res);
	return res;
}

static int hid_bypass_start = 1;  /* (declared above) */
static void (*orig_hid_startHIDSystem)(id, SEL);
static void (*orig_hid_startEventRouting)(id, SEL);
static void (*orig_hid_startEventProcessing)(id, SEL);
static void (*orig_hid_startServer)(id, SEL);
static void (*orig_hid_registerWatchdog)(id, SEL);

/* what the interface holds at the time: which ivars are set (nil ones are what hangs later) */
static void
dump_ivars(id object, const char *label)
{
	unsigned int count = 0;
	Ivar *ivars;

	if (object == nil) {
		say("hakios: ivars %s: nil\n", label);
		return;
	}
	ivars = class_copyIvarList(object_getClass(object), &count);
	say("hakios: ivars %s (%s %p, %u):\n", label, class_getName(object_getClass(object)), (void *)object, count);
	for (unsigned int i = 0; i < count; i++) {
		const char *type = ivar_getTypeEncoding(ivars[i]);
		ptrdiff_t offset = ivar_getOffset(ivars[i]);

		say("hakios:   +%#tx %s %s = %p\n", offset, ivar_getName(ivars[i]), type ? type : "?",
		    *(void **)((uintptr_t)object + (uintptr_t)offset));
	}
	free(ivars);
}

static void
my_hid_startHIDSystem(id self, SEL cmd)
{
	say("hakios: -[BKHIDSystemInterface startHIDSystem] skipped (no IOHIDSystem on MSM8952)\n");
	return;
}

#define HID_STEP(name) \
static void my_hid_##name(id self, SEL cmd) \
{ \
	say("hakios: -[BKHIDSystemInterface " #name "] enter\n"); \
	orig_hid_##name(self, cmd); \
	say("hakios: -[BKHIDSystemInterface " #name "] returned\n"); \
}
HID_STEP(startEventRouting)
HID_STEP(startEventProcessing)

static void
my_hid_startServer(id self, SEL cmd)
{
	say("hakios: -[BKHIDSystemInterface startServer] enter (starting com.apple.iohideventsystem Mach server!)\n");
	if (orig_hid_startServer) {
		orig_hid_startServer(self, cmd);
	}
	g_hid_server_ready = true;
	Ivar ivarChannel = class_getInstanceVariable(object_getClass(self), "_HIDSystemChannel");
	id channel = ivarChannel ? object_getIvar(self, ivarChannel) : nil;
	say("hakios: -[BKHIDSystemInterface startServer] returned (_HIDSystemChannel = %p)\n", channel);
}

/* the watchdog over the event queue the opened event system would have: nothing to watch */
static void
my_hid_registerWatchdog(__unused id self, __unused SEL cmd)
{
	say("hakios: -[BKHIDSystemInterface registerWatchdog] skipped\n");
}

/* ============================================================================
 * 1. MINIIOMOBILEFRAMEBUFFER DIRECT FRAMEBUFFER MAP & BOOT FRAME
 * ============================================================================ */
static volatile uint8_t *g_fb_base = NULL;
static vm_size_t         g_fb_size = 0;
static io_connect_t      g_fb_conn = MACH_PORT_NULL;

#define PANEL_WIDTH   1080
#define PANEL_HEIGHT  1920
#define PANEL_STRIDE  3240
#define D22_WIDTH     1125.0
#define D22_HEIGHT    2436.0
#define D22_SCALE     3.0

#ifndef kIOMapAnywhere
#define kIOMapAnywhere 0x00000001
#endif

#ifndef kIOMasterPortDefault
#define kIOMasterPortDefault MACH_PORT_NULL
#endif

static void
hakios_fb_init_and_draw_boot_frame(void)
{
	if (g_fb_base != NULL) return;

	const char *service_names[] = {
		"MiniIOMobileFramebuffer",
		"IOMobileFramebuffer",
		"AppleCLCD",
		NULL
	};

	io_service_t service = MACH_PORT_NULL;
	for (int i = 0; service_names[i] != NULL; i++) {
		service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(service_names[i]));
		if (service != MACH_PORT_NULL) {
			say("hakios: MiniIOMFB matched service '%s' (port=0x%x)\n", service_names[i], service);
			break;
		}
	}

	if (service == MACH_PORT_NULL) {
		say("hakios: WARNING: MiniIOMobileFramebuffer service not found in IORegistry!\n");
		return;
	}

	kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &g_fb_conn);
	if (kr != KERN_SUCCESS || g_fb_conn == MACH_PORT_NULL) {
		say("hakios: WARNING: IOServiceOpen(MiniIOMFB) failed: 0x%x\n", kr);
		return;
	}

	vm_address_t addr = 0;
	vm_size_t size = 0;
	kr = IOConnectMapMemory(g_fb_conn, 0, mach_task_self(), &addr, &size, kIOMapAnywhere);
	if (kr != KERN_SUCCESS || addr == 0) {
		say("hakios: WARNING: IOConnectMapMemory(MiniIOMFB, type=0) failed: 0x%x\n", kr);
		return;
	}

	g_fb_base = (volatile uint8_t *)addr;
	g_fb_size = size;
	say("hakios: MiniIOMFB mapped in backboardd at %p (size=%lu bytes)\n", (void *)g_fb_base, (unsigned long)g_fb_size);

	if (g_fb_size >= (PANEL_STRIDE * PANEL_HEIGHT)) {
		uint8_t *fb = (uint8_t *)g_fb_base;
		for (int y = 0; y < PANEL_HEIGHT; y++) {
			uint8_t *row = fb + (y * PANEL_STRIDE);
			bool is_header = (y >= 60 && y < 68);
			for (int x = 0; x < PANEL_WIDTH; x++) {
				row[x * 3 + 0] = is_header ? 0xFF : 0x28; // B / R
				row[x * 3 + 1] = is_header ? 0xFF : 0x1A; // G
				row[x * 3 + 2] = is_header ? 0xFF : 0x12; // R / B
			}
		}
		__sync_synchronize();
		say("hakios: MiniIOMFB initial frame swapped to panel (1080x1920x24bpp)!\n");
	}
}

/* ============================================================================
 * 2. ARM64 ABI SAFE (GP + NEON ZEROING) DYNAMIC STUB DISPATCH
 * ============================================================================ */
typedef struct {
	double a, b, c, d;
} HakiosZeroQuad;

static HakiosZeroQuad hakios_zero_imp(id self, SEL _cmd) {
	HakiosZeroQuad z = {0.0, 0.0, 0.0, 0.0};
	__asm__ volatile("mov x0, #0\n\tmov x1, #0" ::: "x0", "x1");
	return z;
}

#if defined(__aarch64__)
__attribute__((naked)) static void hakios_universal_scale_3x(void) {
	__asm__ volatile(
		"mov x0, #3\n"
		"fmov d0, #3.0\n"
		"fmov d1, #3.0\n"
		"ret\n"
	);
}

__attribute__((naked)) static void hakios_universal_scale_float_3x(void) {
	__asm__ volatile(
		"mov x0, #3\n"
		"fmov s0, #3.0\n"
		"fmov s1, #3.0\n"
		"ret\n"
	);
}
#else
static double hakios_universal_scale_3x(id self, SEL _cmd) {
	return 3.0;
}
#endif

static double g_hakios_display_scale = 3.0;

static void disp_setScale(id self, SEL _cmd, CGFloat s) {
	if (s <= 0.5 || s > 10.0) {
		say("hakios: -[HakiosCAWindowServerDisplay setScale:%g] CLAMPED -> 3.0\n", (double)s);
		g_hakios_display_scale = 3.0;
	} else {
		g_hakios_display_scale = (double)s;
		say("hakios: -[HakiosCAWindowServerDisplay setScale:%g]\n", (double)s);
	}
}

static CGSize hakios_cawsd_touchScales(id self, SEL _cmd) {
	return CGSizeMake(1.0, 1.0);
}

static CGRect hakios_cawsd_normalizedDigitizerRect(id self, SEL _cmd) {
	return CGRectMake(0.0, 0.0, 1.0, 1.0);
}

static int hakios_cadisplay_nativeOrientation(id self, SEL _cmd) {
	return 0; // Portrait / 0 deg
}

static id hakios_display_description_safe(id self, SEL _cmd) {
	Class nsStrCls = objc_getClass("NSString");
	if (nsStrCls) {
		SEL sel = sel_registerName("stringWithUTF8String:");
		id (*msgSend)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))objc_msgSend;
		return msgSend(nsStrCls, sel, "<HakiosDisplay: 1125x2436 @3x id=1>");
	}
	return nil;
}

static void hakios_display_encodeWithOSLogCoder_noop(id self, SEL _cmd, id coder, unsigned long options, unsigned long maxLength) {
	(void)self;
	(void)_cmd;
	(void)coder;
	(void)options;
	(void)maxLength;
}

static BOOL hakios_resolve_instance_method(id self, SEL _cmd, SEL name) {
	const char *sName = sel_getName(name);
	say("hakios: AUTO-STUB: -[%s %s]\n", class_getName((Class)self), sName);
	if (strcmp(sName, "scale") == 0 || strcmp(sName, "pointScale") == 0 || strcmp(sName, "preferredScale") == 0 ||
	    strcmp(sName, "maximumScale") == 0 || strcmp(sName, "minimumScale") == 0) {
		class_addMethod((Class)self, name, (IMP)hakios_universal_scale_3x, "d@:");
		return YES;
	}
	if (strcmp(sName, "touchScales") == 0) {
		class_addMethod((Class)self, name, (IMP)hakios_cawsd_touchScales, "{CGSize=dd}@:");
		return YES;
	}
	if (strcmp(sName, "normalizedDigitizerRect") == 0) {
		class_addMethod((Class)self, name, (IMP)hakios_cawsd_normalizedDigitizerRect, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
		return YES;
	}
	if (strcmp(sName, "nativeOrientation") == 0) {
		class_addMethod((Class)self, name, (IMP)hakios_cadisplay_nativeOrientation, "i@:");
		return YES;
	}
	if (strcmp(sName, "description") == 0 || strcmp(sName, "debugDescription") == 0) {
		class_addMethod((Class)self, name, (IMP)hakios_display_description_safe, "@@:");
		return YES;
	}
	if (strcmp(sName, "encodeWithOSLogCoder:options:maxLength:") == 0) {
		class_addMethod((Class)self, name, (IMP)hakios_display_encodeWithOSLogCoder_noop, "v@:@QQ");
		return YES;
	}
	class_addMethod((Class)self, name, (IMP)hakios_zero_imp, "@@:");
	return YES;
}

static BOOL hakios_resolve_class_method(id self, SEL _cmd, SEL name) {
	say("hakios: AUTO-STUB: +[%s %s]\n", class_getName((Class)self), sel_getName(name));
	class_addMethod(object_getClass((id)self), name, (IMP)hakios_zero_imp, "@@:");
	return YES;
}

/* ============================================================================
 * 3. INDEPENDENT (NSObject-BASED) HakiosCAWindowServerDisplay
 * ============================================================================ */
static id g_fake_server  = nil;
static id g_fake_display = nil;
static BOOL g_display_blanked = NO;
static BOOL g_display_disabled = NO;
static BOOL g_display_disables_updates = NO;
static long g_display_tag = 0;
static int g_display_pid = 0;
static unsigned int g_renderer_flags = 3;

static BOOL disp_isKindOfClass(id self, SEL _cmd, Class cls) {
	if (!cls) return NO;
	const char *cname = class_getName(cls);
	if (cname && (strcmp(cname, "CAWindowServerDisplay") == 0 || strcmp(cname, "NSObject") == 0)) {
		return YES;
	}
	return NO;
}

static CGRect disp_bounds(id self, SEL _cmd) {
	return CGRectMake(0.0, 0.0, D22_WIDTH, D22_HEIGHT);
}
static CGSize disp_nativeSize(id self, SEL _cmd) {
	return CGSizeMake(D22_WIDTH, D22_HEIGHT);
}

static CGFloat disp_scale(id self, SEL _cmd)        { return D22_SCALE; }
static CGFloat disp_minScale(id self, SEL _cmd)     { return 1.0; }
static CGFloat disp_maxScale(id self, SEL _cmd)     { return 3.0; }
static unsigned int disp_displayId(id self, SEL _cmd) { return 1; }
static long disp_displayType(id self, SEL _cmd)     { return 0; }
static id disp_name(id self, SEL _cmd)              { return CFSTR("LCD"); }
static id disp_deviceName(id self, SEL _cmd)        { return CFSTR("primary"); }
static id disp_uniqueId(id self, SEL _cmd)          { return CFSTR("4429216-E95D-4B5F-8B57-112524360001"); }
static id disp_orientation(id self, SEL _cmd)       { return CFSTR("Portrait"); }
static void disp_setOrientation(id self, SEL _cmd, id o) {
	say("hakios: -[HakiosCAWindowServerDisplay setOrientation:%p]\n", (void *)o);
}
static id disp_colorMode(id self, SEL _cmd)         { return CFSTR("auto"); }
static void disp_setColorMode(id self, SEL _cmd, id m) {}
static float disp_refreshRate(id self, SEL _cmd)    { return 60.0f; }
static float disp_minFrameTime(id self, SEL _cmd)   { return 1.0f / 60.0f; }
static float disp_overscanAmount(id self, SEL _cmd) { return 0.0f; }
static CGSize disp_overscanAmounts(id self, SEL _cmd) { return CGSizeZero; }
static BOOL disp_isBlanked(id self, SEL _cmd)       { return g_display_blanked; }
static void disp_setBlanked(id self, SEL _cmd, BOOL b) {
	g_display_blanked = b;
	say("hakios: -[HakiosCAWindowServerDisplay setBlanked:%d]\n", (int)b);
	if (!b) {
		hakios_fb_init_and_draw_boot_frame();
	}
}
static BOOL disp_isFlipBookEnabled(id self, SEL _cmd) {
	say("hakios: -[HakiosCAWindowServerDisplay isFlipBookEnabled] -> NO\n");
	input_start();
	return NO;
}
static void disp_setFlipBookEnabled(id self, SEL _cmd, BOOL b) {
	say("hakios: -[HakiosCAWindowServerDisplay setFlipBookEnabled:%d]\n", (int)b);
}
static BOOL disp_disabled(id self, SEL _cmd)        { return g_display_disabled; }
static void disp_setDisabled(id self, SEL _cmd, BOOL d) {
	g_display_disabled = d;
	say("hakios: -[HakiosCAWindowServerDisplay setDisabled:%d]\n", (int)d);
}
static BOOL disp_disablesUpdates(id self, SEL _cmd) { return g_display_disables_updates; }
static void disp_setDisablesUpdates(id self, SEL _cmd, BOOL d) {
	g_display_disables_updates = d;
	say("hakios: -[HakiosCAWindowServerDisplay setDisablesUpdates:%d]\n", (int)d);
}
static id disp_cloneMaster(id self, SEL _cmd)       { return nil; }
static id disp_clones(id self, SEL _cmd)            { return (id)CFArrayCreate(kCFAllocatorDefault, NULL, 0, &kCFTypeArrayCallBacks); }
static long disp_tag(id self, SEL _cmd)             { return g_display_tag; }
static void disp_setTag(id self, SEL _cmd, long t)  {
	g_display_tag = t;
	say("hakios: -[HakiosCAWindowServerDisplay setTag:%ld]\n", t);
}
static int disp_processId(id self, SEL _cmd)        { return g_display_pid; }
static void disp_setProcessId(id self, SEL _cmd, int p) {
	g_display_pid = p;
	say("hakios: -[HakiosCAWindowServerDisplay setProcessId:%d]\n", p);
}
static unsigned int disp_rendererFlags(id self, SEL _cmd) { return g_renderer_flags; }
static unsigned int disp_framebufferFormat(id self, SEL _cmd) { return 1; }
static unsigned int disp_contextIdAtPosition(id self, SEL _cmd, CGPoint pt) { return 0; }
static unsigned int disp_clientPortAtPosition(id self, SEL _cmd, CGPoint pt) { return MACH_PORT_NULL; }
static unsigned int disp_clientPortOfContextId(id self, SEL _cmd, unsigned int cid) { return MACH_PORT_NULL; }
static unsigned int disp_taskPortOfContextId(id self, SEL _cmd, unsigned int cid) { return MACH_PORT_NULL; }
static unsigned int disp_contextIdHostingContextId(id self, SEL _cmd, unsigned int cid) { return cid; }
static CGPoint disp_convertPoint(id self, SEL _cmd, CGPoint pt, unsigned int cid) { return pt; }
static void disp_noop(id self, SEL _cmd) {}
static BOOL disp_bool_no(id self, SEL _cmd) { return NO; }
static BOOL disp_bool_yes(id self, SEL _cmd) { return YES; }

static id hakios_get_or_create_display(void) {
	if (g_fake_display) return g_fake_display;

	Class nsObj = objc_getClass("NSObject");
	Class dispCls = objc_allocateClassPair(nsObj, "HakiosCAWindowServerDisplay", 0);
	if (dispCls) {
		Class dispMeta = object_getClass((id)dispCls);
		class_addMethod(dispMeta, sel_registerName("resolveInstanceMethod:"), (IMP)hakios_resolve_instance_method, "B@::");
		class_addMethod(dispMeta, sel_registerName("resolveClassMethod:"), (IMP)hakios_resolve_class_method, "B@::");

		class_addMethod(dispCls, sel_registerName("isKindOfClass:"), (IMP)disp_isKindOfClass, "B@:#");
		class_addMethod(dispCls, sel_registerName("bounds"), (IMP)disp_bounds, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
		class_addMethod(dispCls, sel_registerName("nativeSize"), (IMP)disp_nativeSize, "{CGSize=dd}@:");
		class_addMethod(dispCls, sel_registerName("scale"), (IMP)hakios_universal_scale_3x, "d@:");
		class_addMethod(dispCls, sel_registerName("setScale:"), (IMP)disp_setScale, "v@:d");
		class_addMethod(dispCls, sel_registerName("minimumScale"), (IMP)hakios_universal_scale_3x, "d@:");
		class_addMethod(dispCls, sel_registerName("maximumScale"), (IMP)hakios_universal_scale_3x, "d@:");
		class_addMethod(dispCls, sel_registerName("touchScales"), (IMP)hakios_cawsd_touchScales, "{CGSize=dd}@:");
		class_addMethod(dispCls, sel_registerName("normalizedDigitizerRect"), (IMP)hakios_cawsd_normalizedDigitizerRect, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
		class_addMethod(dispCls, sel_registerName("description"), (IMP)hakios_display_description_safe, "@@:");
		class_addMethod(dispCls, sel_registerName("debugDescription"), (IMP)hakios_display_description_safe, "@@:");
		class_addMethod(dispCls, sel_registerName("frame"), (IMP)disp_bounds, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
		class_addMethod(dispCls, sel_registerName("size"), (IMP)disp_nativeSize, "{CGSize=dd}@:");
		class_addMethod(dispCls, sel_registerName("displayId"), (IMP)disp_displayId, "I@:");
		class_addMethod(dispCls, sel_registerName("displayType"), (IMP)disp_displayType, "q@:");
		class_addMethod(dispCls, sel_registerName("name"), (IMP)disp_name, "@@:");
		class_addMethod(dispCls, sel_registerName("deviceName"), (IMP)disp_deviceName, "@@:");
		class_addMethod(dispCls, sel_registerName("uniqueId"), (IMP)disp_uniqueId, "@@:");
		class_addMethod(dispCls, sel_registerName("orientation"), (IMP)disp_orientation, "@@:");
		class_addMethod(dispCls, sel_registerName("setOrientation:"), (IMP)disp_setOrientation, "v@:@");
		class_addMethod(dispCls, sel_registerName("colorMode"), (IMP)disp_colorMode, "@@:");
		class_addMethod(dispCls, sel_registerName("setColorMode:"), (IMP)disp_setColorMode, "v@:@");
		class_addMethod(dispCls, sel_registerName("refreshRate"), (IMP)disp_refreshRate, "f@:");
		class_addMethod(dispCls, sel_registerName("minimumFrameTime"), (IMP)disp_minFrameTime, "f@:");
		class_addMethod(dispCls, sel_registerName("overscanAmount"), (IMP)disp_overscanAmount, "d@:");
		class_addMethod(dispCls, sel_registerName("overscanAmounts"), (IMP)disp_overscanAmounts, "{CGSize=dd}@:");
		class_addMethod(dispCls, sel_registerName("isBlanked"), (IMP)disp_isBlanked, "B@:");
		class_addMethod(dispCls, sel_registerName("setBlanked:"), (IMP)disp_setBlanked, "v@:B");
		class_addMethod(dispCls, sel_registerName("disabled"), (IMP)disp_disabled, "B@:");
		class_addMethod(dispCls, sel_registerName("setDisabled:"), (IMP)disp_setDisabled, "v@:B");
		class_addMethod(dispCls, sel_registerName("disablesUpdates"), (IMP)disp_disablesUpdates, "B@:");
		class_addMethod(dispCls, sel_registerName("setDisablesUpdates:"), (IMP)disp_setDisablesUpdates, "v@:B");
		class_addMethod(dispCls, sel_registerName("cloneMaster"), (IMP)disp_cloneMaster, "@@:");
		class_addMethod(dispCls, sel_registerName("clones"), (IMP)disp_clones, "@@:");
		class_addMethod(dispCls, sel_registerName("tag"), (IMP)disp_tag, "q@:");
		class_addMethod(dispCls, sel_registerName("setTag:"), (IMP)disp_setTag, "v@:q");
		class_addMethod(dispCls, sel_registerName("processId"), (IMP)disp_processId, "i@:");
		class_addMethod(dispCls, sel_registerName("setProcessId:"), (IMP)disp_setProcessId, "v@:i");
		class_addMethod(dispCls, sel_registerName("rendererFlags"), (IMP)disp_rendererFlags, "I@:");
		class_addMethod(dispCls, sel_registerName("framebufferFormat"), (IMP)disp_framebufferFormat, "I@:");
		class_addMethod(dispCls, sel_registerName("contextIdAtPosition:"), (IMP)disp_contextIdAtPosition, "I@:{CGPoint=dd}");
		class_addMethod(dispCls, sel_registerName("contextIdAtPosition:excludingContextIds:"), (IMP)disp_contextIdAtPosition, "I@:{CGPoint=dd}@");
		class_addMethod(dispCls, sel_registerName("clientPortAtPosition:"), (IMP)disp_clientPortAtPosition, "I@:{CGPoint=dd}");
		class_addMethod(dispCls, sel_registerName("clientPortOfContextId:"), (IMP)disp_clientPortOfContextId, "I@:I");
		class_addMethod(dispCls, sel_registerName("taskPortOfContextId:"), (IMP)disp_taskPortOfContextId, "I@:I");
		class_addMethod(dispCls, sel_registerName("contextIdHostingContextId:"), (IMP)disp_contextIdHostingContextId, "I@:I");
		class_addMethod(dispCls, sel_registerName("convertPoint:toContextId:"), (IMP)disp_convertPoint, "{CGPoint=dd}@:{CGPoint=dd}I");
		class_addMethod(dispCls, sel_registerName("convertPoint:fromContextId:"), (IMP)disp_convertPoint, "{CGPoint=dd}@:{CGPoint=dd}I");
		class_addMethod(dispCls, sel_registerName("update"), (IMP)disp_noop, "v@:");
		class_addMethod(dispCls, sel_registerName("freeze"), (IMP)disp_noop, "v@:");
		class_addMethod(dispCls, sel_registerName("invalidate"), (IMP)disp_noop, "v@:");
		class_addMethod(dispCls, sel_registerName("isMirroringEnabled"), (IMP)disp_bool_no, "B@:");
		class_addMethod(dispCls, sel_registerName("setMirroringEnabled:"), (IMP)disp_noop, "v@:B");
		class_addMethod(dispCls, sel_registerName("supportsExtendedColors"), (IMP)disp_bool_yes, "B@:");
		class_addMethod(dispCls, sel_registerName("isFlipBookEnabled"), (IMP)disp_isFlipBookEnabled, "B@:");
		class_addMethod(dispCls, sel_registerName("setFlipBookEnabled:"), (IMP)disp_setFlipBookEnabled, "v@:B");
		class_addMethod(dispCls, sel_registerName("encodeWithOSLogCoder:options:maxLength:"),
		    (IMP)hakios_display_encodeWithOSLogCoder_noop, "v@:@QQ");
		objc_registerClassPair(dispCls);
	} else {
		dispCls = objc_getClass("HakiosCAWindowServerDisplay");
	}

	g_fake_display = class_createInstance(dispCls, 0);
	say("hakios: Created NSObject-based HakiosCAWindowServerDisplay -> %p (1125x2436 @3x, id=1)\n", (void *)g_fake_display);
	return g_fake_display;
}

/* ============================================================================
 * 4. INDEPENDENT (NSObject-BASED) HakiosCAWindowServer
 * ============================================================================ */
static BOOL srv_isKindOfClass(id self, SEL _cmd, Class cls) {
	if (!cls) return NO;
	const char *cname = class_getName(cls);
	if (cname && (strcmp(cname, "CAWindowServer") == 0 || strcmp(cname, "NSObject") == 0)) {
		return YES;
	}
	return NO;
}

static id hooked_displays(id self, SEL _cmd) {
	id d = hakios_get_or_create_display();
	say("hakios: -[CAWindowServer displays] -> returning @[%p]\n", (void *)d);
	const void *values[] = { (const void *)d };
	return (id)CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
}

static id hooked_displayWithName(id self, SEL _cmd, id name) {
	say("hakios: -[CAWindowServer displayWithName:%p]\n", (void *)name);
	return hakios_get_or_create_display();
}

static id hooked_displayWithDisplayId(id self, SEL _cmd, unsigned int dispId) {
	say("hakios: -[CAWindowServer displayWithDisplayId:%u]\n", dispId);
	return hakios_get_or_create_display();
}

static id hooked_displayWithUniqueId(id self, SEL _cmd, id uid) {
	say("hakios: -[CAWindowServer displayWithUniqueId:%p]\n", (void *)uid);
	return hakios_get_or_create_display();
}

static void hooked_setRendererFlags(id self, SEL _cmd, unsigned int flags) {
	g_renderer_flags = flags;
	say("hakios: -[CAWindowServer setRendererFlags:0x%x]\n", flags);
}
static unsigned int hooked_rendererFlags(id self, SEL _cmd) {
	return g_renderer_flags;
}
static void hooked_setSecureMode(id self, SEL _cmd, BOOL sec) {
	say("hakios: -[CAWindowServer setSecureMode:%d]\n", (int)sec);
}
static BOOL hooked_secureMode(id self, SEL _cmd) { return NO; }
static id   hooked_insecureProcessIds(id self, SEL _cmd) {
	return (id)CFArrayCreate(kCFAllocatorDefault, NULL, 0, &kCFTypeArrayCallBacks);
}
static BOOL hooked_isMirroringEnabled(id self, SEL _cmd) { return NO; }
static void hooked_setMirroringEnabled(id self, SEL _cmd, BOOL m) {}
static void hooked_updatePreferencesIfNeeded(id self, SEL _cmd) {
	say("hakios: -[CAWindowServer updatePreferencesIfNeeded]\n");
}

/* ============================================================================
 * 4. SYNTHETIC CADisplay & CADisplayMode (QuartzCore Client-Side Display)
 * ============================================================================ */
static id g_fake_cadisplay = nil;
static id g_fake_cadisplay_mode = nil;

static BOOL camode_isKindOfClass(id self, SEL _cmd, Class cls) {
	if (!cls) return NO;
	const char *cname = class_getName(cls);
	if (cname && (strcmp(cname, "CADisplayMode") == 0 || strcmp(cname, "NSObject") == 0)) {
		return YES;
	}
	return NO;
}

static unsigned long camode_width(id self, SEL _cmd) { return (unsigned long)D22_WIDTH; }
static unsigned long camode_height(id self, SEL _cmd) { return (unsigned long)D22_HEIGHT; }
static double camode_preferredScale(id self, SEL _cmd) { return D22_SCALE; }
static double camode_pointScale(id self, SEL _cmd) { return D22_SCALE; }
static float camode_refreshRate(id self, SEL _cmd) { return 60.0f; }
static double camode_pixelAspectRatio(id self, SEL _cmd) { return 1.0; }
static id camode_colorMode(id self, SEL _cmd) { return CFSTR("auto"); }
static BOOL camode_isVirtual(id self, SEL _cmd) { return NO; }

static id hakios_get_or_create_cadisplay_mode(void) {
	if (g_fake_cadisplay_mode) return g_fake_cadisplay_mode;

	Class nsObj = objc_getClass("NSObject");
	Class modeCls = objc_allocateClassPair(nsObj, "HakiosCADisplayMode", 0);
	if (modeCls) {
		Class modeMeta = object_getClass((id)modeCls);
		class_addMethod(modeMeta, sel_registerName("resolveInstanceMethod:"), (IMP)hakios_resolve_instance_method, "B@::");
		class_addMethod(modeCls, sel_registerName("isKindOfClass:"), (IMP)camode_isKindOfClass, "B@:#");
		class_addMethod(modeCls, sel_registerName("width"), (IMP)camode_width, "Q@:");
		class_addMethod(modeCls, sel_registerName("height"), (IMP)camode_height, "Q@:");
		class_addMethod(modeCls, sel_registerName("preferredScale"), (IMP)hakios_universal_scale_3x, "d@:");
		class_addMethod(modeCls, sel_registerName("pointScale"), (IMP)hakios_universal_scale_3x, "d@:");
		class_addMethod(modeCls, sel_registerName("refreshRate"), (IMP)camode_refreshRate, "f@:");
		class_addMethod(modeCls, sel_registerName("pixelAspectRatio"), (IMP)camode_pixelAspectRatio, "d@:");
		class_addMethod(modeCls, sel_registerName("colorMode"), (IMP)camode_colorMode, "@@:");
		class_addMethod(modeCls, sel_registerName("isVirtual"), (IMP)camode_isVirtual, "B@:");
		class_addMethod(modeCls, sel_registerName("description"), (IMP)hakios_display_description_safe, "@@:");
		class_addMethod(modeCls, sel_registerName("debugDescription"), (IMP)hakios_display_description_safe, "@@:");
		objc_registerClassPair(modeCls);
	} else {
		modeCls = objc_getClass("HakiosCADisplayMode");
	}
	g_fake_cadisplay_mode = class_createInstance(modeCls, 0);
	say("hakios: Created HakiosCADisplayMode -> %p\n", (void *)g_fake_cadisplay_mode);
	return g_fake_cadisplay_mode;
}

static BOOL cadisp_isKindOfClass(id self, SEL _cmd, Class cls) {
	if (!cls) return NO;
	const char *cname = class_getName(cls);
	if (cname && (strcmp(cname, "CADisplay") == 0 || strcmp(cname, "NSObject") == 0)) {
		return YES;
	}
	return NO;
}

static id cadisp_currentMode(id self, SEL _cmd) {
	return hakios_get_or_create_cadisplay_mode();
}

static id cadisp_availableModes(id self, SEL _cmd) {
	id mode = hakios_get_or_create_cadisplay_mode();
	const void *values[] = { mode };
	return (id)CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
}

static id cadisp_windowServerDisplay(id self, SEL _cmd) {
	return hakios_get_or_create_display();
}

static void cadisp_setWindowServerDisplay(id self, SEL _cmd, id disp) {
	say("hakios: -[CADisplay setWindowServerDisplay:%p]\n", (void *)disp);
}

static BOOL cad_classShouldAlwaysRegisterSelectorNamed(id self, SEL _cmd, const char *sel) {
	return NO;
}
static void cad_setBounds(id self, SEL _cmd, CGRect r) {
	say("hakios: -[HakiosCADisplay setBounds:{{%f, %f}, {%f, %f}}]\n", r.origin.x, r.origin.y, r.size.width, r.size.height);
}
static void cad_addBounds(id self, SEL _cmd, CGRect r) {
	say("hakios: -[HakiosCADisplay addBounds:{{%f, %f}, {%f, %f}}]\n", r.origin.x, r.origin.y, r.size.width, r.size.height);
}

static id hakios_get_or_create_cadisplay(void) {
	if (g_fake_cadisplay) return g_fake_cadisplay;

	Class nsObj = objc_getClass("NSObject");
	Class cadCls = objc_allocateClassPair(nsObj, "HakiosCADisplay", 0);
	if (cadCls) {
		Class cadMeta = object_getClass((id)cadCls);
		class_addMethod(cadMeta, sel_registerName("resolveInstanceMethod:"), (IMP)hakios_resolve_instance_method, "B@::");
		class_addMethod(cadMeta, sel_registerName("resolveClassMethod:"), (IMP)hakios_resolve_class_method, "B@::");
		class_addMethod(cadMeta, sel_registerName("_classShouldAlwaysRegisterSelectorNamed:"),
		    (IMP)cad_classShouldAlwaysRegisterSelectorNamed, "B@:*");

		class_addMethod(cadCls, sel_registerName("isKindOfClass:"), (IMP)cadisp_isKindOfClass, "B@:#");
		class_addMethod(cadCls, sel_registerName("bounds"), (IMP)disp_bounds, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
		class_addMethod(cadCls, sel_registerName("frame"), (IMP)disp_bounds, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
		class_addMethod(cadCls, sel_registerName("safeBounds"), (IMP)disp_bounds, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
		class_addMethod(cadCls, sel_registerName("setBounds:"), (IMP)cad_setBounds, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
		class_addMethod(cadCls, sel_registerName("addBounds:"), (IMP)cad_addBounds, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
		class_addMethod(cadCls, sel_registerName("scale"), (IMP)hakios_universal_scale_3x, "d@:");
		class_addMethod(cadCls, sel_registerName("pointScale"), (IMP)hakios_universal_scale_3x, "d@:");
		class_addMethod(cadCls, sel_registerName("preferredScale"), (IMP)hakios_universal_scale_3x, "d@:");
		class_addMethod(cadCls, sel_registerName("nativeOrientation"), (IMP)hakios_cadisplay_nativeOrientation, "i@:");
		class_addMethod(cadCls, sel_registerName("description"), (IMP)hakios_display_description_safe, "@@:");
		class_addMethod(cadCls, sel_registerName("debugDescription"), (IMP)hakios_display_description_safe, "@@:");
		class_addMethod(cadCls, sel_registerName("size"), (IMP)disp_nativeSize, "{CGSize=dd}@:");
		class_addMethod(cadCls, sel_registerName("displayId"), (IMP)disp_displayId, "I@:");
		class_addMethod(cadCls, sel_registerName("displayType"), (IMP)disp_displayType, "q@:");
		class_addMethod(cadCls, sel_registerName("name"), (IMP)disp_name, "@@:");
		class_addMethod(cadCls, sel_registerName("deviceName"), (IMP)disp_deviceName, "@@:");
		class_addMethod(cadCls, sel_registerName("uniqueId"), (IMP)disp_uniqueId, "@@:");
		class_addMethod(cadCls, sel_registerName("orientation"), (IMP)disp_orientation, "@@:");
		class_addMethod(cadCls, sel_registerName("refreshRate"), (IMP)disp_refreshRate, "f@:");
		class_addMethod(cadCls, sel_registerName("heartbeatRate"), (IMP)disp_refreshRate, "f@:");
		class_addMethod(cadCls, sel_registerName("isExternal"), (IMP)disp_bool_no, "B@:");
		class_addMethod(cadCls, sel_registerName("setExternal:"), (IMP)disp_noop, "v@:B");
		class_addMethod(cadCls, sel_registerName("isCloned"), (IMP)disp_bool_no, "B@:");
		class_addMethod(cadCls, sel_registerName("isCloningSupported"), (IMP)disp_bool_no, "B@:");
		class_addMethod(cadCls, sel_registerName("allowsVirtualModes"), (IMP)disp_bool_no, "B@:");
		class_addMethod(cadCls, sel_registerName("currentMode"), (IMP)cadisp_currentMode, "@@:");
		class_addMethod(cadCls, sel_registerName("availableModes"), (IMP)cadisp_availableModes, "@@:");
		class_addMethod(cadCls, sel_registerName("preferredMode"), (IMP)cadisp_currentMode, "@@:");
		class_addMethod(cadCls, sel_registerName("windowServerDisplay"), (IMP)cadisp_windowServerDisplay, "@@:");
		class_addMethod(cadCls, sel_registerName("setWindowServerDisplay:"), (IMP)cadisp_setWindowServerDisplay, "v@:@");
		class_addMethod(cadCls, sel_registerName("update"), (IMP)disp_noop, "v@:");
		class_addMethod(cadCls, sel_registerName("encodeWithOSLogCoder:options:maxLength:"),
		    (IMP)hakios_display_encodeWithOSLogCoder_noop, "v@:@QQ");
		objc_registerClassPair(cadCls);
	} else {
		cadCls = objc_getClass("HakiosCADisplay");
	}

	g_fake_cadisplay = class_createInstance(cadCls, 0);
	say("hakios: Created HakiosCADisplay -> %p (1125x2436 @3x, id=1)\n", (void *)g_fake_cadisplay);
	return g_fake_cadisplay;
}

static id hooked_cadisplay_mainDisplay(id self, SEL _cmd) {
	say("hakios: +[CADisplay mainDisplay] CALLED -> returning %p\n", (void *)hakios_get_or_create_cadisplay());
	return hakios_get_or_create_cadisplay();
}

static id hooked_cadisplay_displays(id self, SEL _cmd) {
	id d = hakios_get_or_create_cadisplay();
	const void *values[] = { d };
	say("hakios: +[CADisplay displays] CALLED -> returning [ %p ]\n", (void *)d);
	return (id)CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
}

static id hooked_cadisplay_TVOutDisplay(id self, SEL _cmd) {
	return nil;
}

static id hakios_get_or_create_server(void) {
	if (g_fake_server) return g_fake_server;

	Class nsObj = objc_getClass("NSObject");
	Class srvCls = objc_allocateClassPair(nsObj, "HakiosCAWindowServer", 0);
	if (srvCls) {
		Class srvMeta = object_getClass((id)srvCls);
		class_addMethod(srvMeta, sel_registerName("resolveInstanceMethod:"), (IMP)hakios_resolve_instance_method, "B@::");
		class_addMethod(srvMeta, sel_registerName("resolveClassMethod:"), (IMP)hakios_resolve_class_method, "B@::");

		class_addMethod(srvCls, sel_registerName("isKindOfClass:"), (IMP)srv_isKindOfClass, "B@:#");
		class_addMethod(srvCls, sel_registerName("displays"), (IMP)hooked_displays, "@@:");
		class_addMethod(srvCls, sel_registerName("displayWithName:"), (IMP)hooked_displayWithName, "@@:@");
		class_addMethod(srvCls, sel_registerName("displayWithDisplayId:"), (IMP)hooked_displayWithDisplayId, "@@:I");
		class_addMethod(srvCls, sel_registerName("displayWithUniqueId:"), (IMP)hooked_displayWithUniqueId, "@@:@");
		class_addMethod(srvCls, sel_registerName("setRendererFlags:"), (IMP)hooked_setRendererFlags, "v@:I");
		class_addMethod(srvCls, sel_registerName("rendererFlags"), (IMP)hooked_rendererFlags, "I@:");
		class_addMethod(srvCls, sel_registerName("setSecureMode:"), (IMP)hooked_setSecureMode, "v@:B");
		class_addMethod(srvCls, sel_registerName("secureMode"), (IMP)hooked_secureMode, "B@:");
		class_addMethod(srvCls, sel_registerName("insecureProcessIds"), (IMP)hooked_insecureProcessIds, "@@:");
		class_addMethod(srvCls, sel_registerName("isMirroringEnabled"), (IMP)hooked_isMirroringEnabled, "B@:");
		class_addMethod(srvCls, sel_registerName("setMirroringEnabled:"), (IMP)hooked_setMirroringEnabled, "v@:B");
		class_addMethod(srvCls, sel_registerName("updatePreferencesIfNeeded"), (IMP)hooked_updatePreferencesIfNeeded, "v@:");
		objc_registerClassPair(srvCls);
	} else {
		srvCls = objc_getClass("HakiosCAWindowServer");
	}

	g_fake_server = class_createInstance(srvCls, 0);
	say("hakios: Allocated NSObject-based HakiosCAWindowServer -> %p\n", (void *)g_fake_server);
	return g_fake_server;
}

/* ============================================================================
 * CoreBrightness (BrightnessSystem) Bypass & /dev/hakios-backlight Bridge
 * ============================================================================ */

static id hooked_Brightness_init(id self, SEL _cmd) {
	const char *clsName = object_getClassName(self);
	say("hakios: -[%s init] ENTER (%p) -> bypassing BLControl/IOHID hang\n", clsName, (void *)self);
	return self;
}

static id hooked_Brightness_copyPropertyForKey(id self, SEL _cmd, id key) {
	const char *keyStr = NULL;
	if (key && CFGetTypeID((CFTypeRef)key) == CFStringGetTypeID()) {
		keyStr = CFStringGetCStringPtr((CFStringRef)key, kCFStringEncodingUTF8);
	}
	say("hakios: -[%s copyPropertyForKey:%s]\n", object_getClassName(self), keyStr ? keyStr : "?");

	if (keyStr) {
		if (strstr(keyStr, "Brightness") || strstr(keyStr, "brightness")) {
			float val = 1.0f;
			CFNumberRef num = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &val);
			return (id)num;
		}
		if (strstr(keyStr, "Als") || strstr(keyStr, "ALS")) {
			CFRetain(kCFBooleanFalse);
			return (id)kCFBooleanFalse;
		}
	}
	return nil;
}

static bool hooked_Brightness_setPropertyForKey(id self, SEL _cmd, id prop, id key) {
	const char *keyStr = NULL;
	if (key && CFGetTypeID((CFTypeRef)key) == CFStringGetTypeID()) {
		keyStr = CFStringGetCStringPtr((CFStringRef)key, kCFStringEncodingUTF8);
	}
	say("hakios: -[%s setProperty:%p forKey:%s] -> true\n",
	    object_getClassName(self), (void *)prop, keyStr ? keyStr : "?");

	if (keyStr && (strcmp(keyStr, "DisplayBrightness") == 0 || strcmp(keyStr, "Brightness") == 0)) {
		int fd = open("/dev/hakios-backlight", O_WRONLY | O_NONBLOCK);
		if (fd >= 0) {
			const char *lvl = "255\n";
			write(fd, lvl, 4);
			close(fd);
		}
	}
	return true;
}

static bool hooked_Brightness_isAlsSupported(id self, SEL _cmd) {
	return false;
}

static void hooked_Brightness_registerNotificationBlock(id self, SEL _cmd, id block) {
	say("hakios: -[%s registerNotificationBlock:%p] (noop)\n", object_getClassName(self), (void *)block);
}

static void hooked_Brightness_registerNotificationBlockForProps(id self, SEL _cmd, id block, id props) {
	say("hakios: -[%s registerNotificationBlock:%p forProperties:%p] (noop)\n",
	    object_getClassName(self), (void *)block, (void *)props);
}

static bool hooked_CBClient_supported(id self, SEL _cmd) {
	return false;
}

static id hooked_CBClient_subClient(id self, SEL _cmd) {
	return nil;
}

static void hakios_hook_brightness_class(const char *className) {
	Class cls = objc_getClass(className);
	if (!cls) {
		say("hakios: Brightness class '%s' not loaded yet, loading CoreBrightness...\n", className);
		dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness", RTLD_NOW);
		cls = objc_getClass(className);
	}
	if (!cls) {
		say("hakios: WARNING: Brightness class '%s' not found\n", className);
		return;
	}

	unsigned int mCount = 0;
	Method *methods = class_copyMethodList(cls, &mCount);
	for (unsigned int i = 0; i < mCount; i++) {
		SEL s = method_getName(methods[i]);
		const char *sName = sel_getName(s);
		if (strcmp(sName, "init") == 0 || strncmp(sName, "initWith", 8) == 0) {
			method_setImplementation(methods[i], (IMP)hooked_Brightness_init);
			say("hakios: Hooked -[%s %s]\n", className, sName);
		}
	}
	if (methods) free(methods);

	class_replaceMethod(cls, sel_registerName("init"), (IMP)hooked_Brightness_init, "@@:");
	class_replaceMethod(cls, sel_registerName("copyPropertyForKey:"), (IMP)hooked_Brightness_copyPropertyForKey, "@@:@");
	class_replaceMethod(cls, sel_registerName("copyPropertyForKey:client:"), (IMP)hooked_Brightness_copyPropertyForKey, "@@:@@");
	class_replaceMethod(cls, sel_registerName("setProperty:forKey:"), (IMP)hooked_Brightness_setPropertyForKey, "B@:@@");
	class_replaceMethod(cls, sel_registerName("setProperty:withKey:"), (IMP)hooked_Brightness_setPropertyForKey, "B@:@@");
	class_replaceMethod(cls, sel_registerName("isAlsSupported"), (IMP)hooked_Brightness_isAlsSupported, "B@:");
	class_replaceMethod(cls, sel_registerName("registerNotificationBlock:"), (IMP)hooked_Brightness_registerNotificationBlock, "v@:@?");
	class_replaceMethod(cls, sel_registerName("registerNotificationBlock:forProperties:"), (IMP)hooked_Brightness_registerNotificationBlockForProps, "v@:@?@");
}

static void hakios_patch_display_classes(void) {
	const char *dispClasses[] = {
		"HakiosCADisplay",
		"HakiosCADisplayMode",
		"HakiosCAWindowServerDisplay",
		NULL
	};

	const char *scaleSelectors[] = {
		"scale",
		"pointScale",
		"preferredScale",
		"maximumScale",
		"minimumScale",
		NULL
	};

	for (int c = 0; dispClasses[c] != NULL; c++) {
		Class cls = objc_getClass(dispClasses[c]);
		if (!cls) continue;

		for (int s = 0; scaleSelectors[s] != NULL; s++) {
			SEL sel = sel_registerName(scaleSelectors[s]);
			class_replaceMethod(cls, sel, (IMP)hakios_universal_scale_3x, "d@:");
		}

		class_replaceMethod(cls, sel_registerName("description"), (IMP)hakios_display_description_safe, "@@:");
		class_replaceMethod(cls, sel_registerName("debugDescription"), (IMP)hakios_display_description_safe, "@@:");
		class_replaceMethod(cls, sel_registerName("encodeWithOSLogCoder:options:maxLength:"), (IMP)hakios_display_encodeWithOSLogCoder_noop, "v@:@QQ");
	}

	Class cawsd = objc_getClass("HakiosCAWindowServerDisplay");
	if (cawsd) {
		class_replaceMethod(cawsd, sel_registerName("setScale:"), (IMP)disp_setScale, "v@:d");
		class_replaceMethod(cawsd, sel_registerName("touchScales"), (IMP)hakios_cawsd_touchScales, "{CGSize=dd}@:");
		class_replaceMethod(cawsd, sel_registerName("normalizedDigitizerRect"), (IMP)hakios_cawsd_normalizedDigitizerRect, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
		class_replaceMethod(cawsd, sel_registerName("bounds"), (IMP)disp_bounds, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
		class_replaceMethod(cawsd, sel_registerName("frame"), (IMP)disp_bounds, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
		class_replaceMethod(cawsd, sel_registerName("size"), (IMP)disp_nativeSize, "{CGSize=dd}@:");
	}

	Class cad = objc_getClass("HakiosCADisplay");
	if (cad) {
		class_replaceMethod(cad, sel_registerName("nativeOrientation"), (IMP)hakios_cadisplay_nativeOrientation, "i@:");
		class_replaceMethod(cad, sel_registerName("bounds"), (IMP)disp_bounds, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
		class_replaceMethod(cad, sel_registerName("frame"), (IMP)disp_bounds, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
		class_replaceMethod(cad, sel_registerName("size"), (IMP)disp_nativeSize, "{CGSize=dd}@:");
	}

	say("hakios: Patched HakiosCADisplay & HakiosCAWindowServerDisplay universal 3x scale + rects + safe logging\n");
}

static id hooked_dummy_self(id self, SEL _cmd) {
	return self;
}

static id hooked_dummy_haptics_controller(id self, SEL _cmd) {
	say("hakios: +[BKButtonHapticsController sharedInstance] intercepted -> returning dummy\n");
	static id s_dummy_haptics = nil;
	if (!s_dummy_haptics) {
		Class cls = objc_getClass("BKButtonHapticsController");
		if (cls) {
			s_dummy_haptics = class_createInstance(cls, 0);
		} else {
			s_dummy_haptics = self;
		}
	}
	return s_dummy_haptics;
}

static void hooked_noop_method(id self, SEL _cmd) {
	say("hakios: -[%s %s] NO-OP intercepted\n", class_getName(object_getClass(self)), sel_getName(_cmd));
}

static void hakios_install_brightness_and_display_fixes(void) {
	const char *cbClasses[] = {
		"BrightnessSystem",
		"BrightnessSystemClient",
		"BrightnessSystemInternal",
		"BrightnessSystemClientInternal",
		"CBClient",
		"CBBlueLightClient",
		"CBAdaptationClient",
		NULL
	};
	for (int i = 0; cbClasses[i] != NULL; i++) {
		hakios_hook_brightness_class(cbClasses[i]);
	}

	Class cbClientMeta = objc_getMetaClass("CBClient");
	if (cbClientMeta) {
		class_replaceMethod(cbClientMeta, sel_registerName("supportsBlueLightReduction"), (IMP)hooked_CBClient_supported, "B@:");
		class_replaceMethod(cbClientMeta, sel_registerName("supportsAdaptation"), (IMP)hooked_CBClient_supported, "B@:");
	}
	Class cbClient = objc_getClass("CBClient");
	if (cbClient) {
		class_replaceMethod(cbClient, sel_registerName("blueLightClient"), (IMP)hooked_CBClient_subClient, "@@:");
		class_replaceMethod(cbClient, sel_registerName("adaptationClient"), (IMP)hooked_CBClient_subClient, "@@:");
	}

	/* Hook Button Haptics Controller to avoid hanging on missing ButtonResolver daemon */
	Class bhMeta = objc_getMetaClass("BKButtonHapticsController");
	if (bhMeta) {
		class_replaceMethod(bhMeta, sel_registerName("sharedInstance"), (IMP)hooked_dummy_haptics_controller, "@@:");
		say("hakios: BKButtonHapticsController sharedInstance hooked!\n");
	}
	Class bhCls = objc_getClass("BKButtonHapticsController");
	if (bhCls) {
		class_replaceMethod(bhCls, sel_registerName("init"), (IMP)hooked_dummy_self, "@@:");
		class_replaceMethod(bhCls, sel_registerName("initWithBHButtonHapticsController:"), (IMP)hooked_dummy_self, "@@:@");
		class_replaceMethod(bhCls, sel_registerName("playHapticForClickState:clickSpeed:"), (IMP)hooked_noop_method, "v@:qq");
		class_replaceMethod(bhCls, sel_registerName("removeAllHaptics"), (IMP)hooked_noop_method, "v@:");
		class_replaceMethod(bhCls, sel_registerName("setClickHapticAssetType:"), (IMP)hooked_noop_method, "v@:q");
	}

	/* Hook ALS sync transactions */
	Class alsTxCls = objc_getClass("BKDisplayBrightnessUpdateTransaction");
	if (alsTxCls) {
		class_replaceMethod(alsTxCls, sel_registerName("synchronizeALSPreferencesAndSystemDisplayBrightness"), (IMP)hooked_noop_method, "v@:");
		class_replaceMethod(alsTxCls, sel_registerName("synchronizeALSPreferencesAndDisplayBrightness"), (IMP)hooked_noop_method, "v@:");
		class_replaceMethod(alsTxCls, sel_registerName("synchronize"), (IMP)hooked_noop_method, "v@:");
		say("hakios: BKDisplayBrightnessUpdateTransaction synchronize hooks installed!\n");
	}

	Class alsCtrlCls = objc_getClass("BKDisplayBrightnessController");
	if (alsCtrlCls) {
		class_replaceMethod(alsCtrlCls, sel_registerName("synchronizeALSPreferencesAndSystemDisplayBrightness"), (IMP)hooked_noop_method, "v@:");
		class_replaceMethod(alsCtrlCls, sel_registerName("synchronizeALSPreferencesAndDisplayBrightness"), (IMP)hooked_noop_method, "v@:");
	}

	/* Also hook BRButtonResolverController and HAButtonHapticsLoader if present */
	Class brClsMeta = objc_getMetaClass("BRButtonResolverController");
	if (brClsMeta) {
		class_replaceMethod(brClsMeta, sel_registerName("sharedInstance"), (IMP)hooked_dummy_self, "@@:");
	}
	Class haClsMeta = objc_getMetaClass("HAButtonHapticsLoader");
	if (haClsMeta) {
		class_replaceMethod(haClsMeta, sel_registerName("sharedInstance"), (IMP)hooked_dummy_self, "@@:");
	}

	/* hakios_patch_display_classes(); - disabled to preserve real QuartzCore CADisplay */
}

static id (*orig_serverWithOptions)(id self, SEL _cmd, id options) = NULL;

/*
 * fix65: the gates that keep the first frame from ever being committed.  fix49 -- the last boot
 * where backboardd lived -- shows the sequence in full: StartWindowServer asks for the server with
 * kCAWindowServerDisableUpdatesOnMainDisplay set, the display comes up blanked and disabled, and
 * although backboardd clears the disable option four seconds itself ([RenderOverlay] Enabling
 * framebuffer changes), the display is never unblanked: kFBMethodRequestPowerChange (selector 12)
 * and registerNotificationPort (selector 72) are never called, so CA::WindowServer's IOMFBDisplay
 * stays _blanked and no frame pump ever starts.  Nothing on this board will ever send the power
 * response that unblanks it, so we do it here: hand the server nil options (the freeze option
 * never gets set), hook the display's three gate setters to refuse YES, and kick the display
 * itself until the render loop wakes up.
 */
static void (*orig_real_setBlanked)(id self, SEL _cmd, BOOL b) = NULL;
static void (*orig_real_setDisablesUpdates)(id self, SEL _cmd, BOOL d) = NULL;
static void (*orig_real_setDisabled)(id self, SEL _cmd, BOOL d) = NULL;

static void
hooked_real_setBlanked(id self, SEL _cmd, BOOL b)
{
	say("hakios: -[CAWindowServerDisplay setBlanked:%d] -> FORCING NO\n", (int) b);
	if (orig_real_setBlanked) {
		orig_real_setBlanked(self, _cmd, NO);
	}
}

static void
hooked_real_setDisablesUpdates(id self, SEL _cmd, BOOL d)
{
	say("hakios: -[CAWindowServerDisplay setDisablesUpdates:%d] -> FORCING NO\n", (int) d);
	if (orig_real_setDisablesUpdates) {
		orig_real_setDisablesUpdates(self, _cmd, NO);
	}
}

static void
hooked_real_setDisabled(id self, SEL _cmd, BOOL d)
{
	say("hakios: -[CAWindowServerDisplay setDisabled:%d] -> FORCING NO\n", (int) d);
	if (orig_real_setDisabled) {
		orig_real_setDisabled(self, _cmd, NO);
	}
}

/*
 * dispmode: the mode question from the object side.  The IOMFB trace shows selectors 4/5/12/72
 * never called and every kick reads curMode=<CADisplayMode 1125x2436 internal_panel> yet
 * begin=0 finish=0 render=0: the frame pump never starts.  One suspect is the mode handshake --
 * setDisplayMode: answering an error (or currentDisplayMode answering -1) keeps CoreAnimation
 * from ever arming the swap loop.  So: accept every mode request as success, and never answer
 * -1 for the current mode.  The selectors exist in the DSC (grep-verified): setDisplayMode:,
 * currentDisplayMode, setCurrentMode:, setDisablesUpdates:, disablesUpdates.
 */
/* NSInteger is not visible through this SDK's plain C headers here; long matches it on arm64. */
typedef long hakios_NSInteger;

static void *orig_setDisplayMode;
static void *orig_currentDisplayMode_id;
static void *orig_currentDisplayMode_int;
static void *orig_setCurrentMode;
static unsigned calls_setmode;
static unsigned calls_curmode;
static unsigned calls_setcurmode;

static void
hooked_setDisplayMode(id self, SEL _cmd, id arg)
{
	calls_setmode++;
	say("hakios: DISPMODE -[%s setDisplayMode:] -> 0 (call #%u, arg=%s)\n",
	    class_getName(object_getClass(self)), calls_setmode,
	    arg != nil ? "non-nil" : "nil");
	/* The real argument count is not known from the DSC strings; touching the arguments we
	 * did not read would risk a crash, so log and answer kIOReturnSuccess without forwarding. */
	return;
}

static id
hooked_currentDisplayMode_id(id self, SEL _cmd)
{
	calls_curmode++;
	id (*real)(id, SEL) = (id (*)(id, SEL)) orig_currentDisplayMode_id;
	id r = real != NULL ? real(self, _cmd) : nil;

	say("hakios: DISPMODE -[%s currentDisplayMode] -> %s (id, call #%u)\n",
	    class_getName(object_getClass(self)), r != nil ? "kept" : "forced 0", calls_curmode);
	return r != nil ? r : (id) 0;
}

static hakios_NSInteger
hooked_currentDisplayMode_int(id self, SEL _cmd)
{
	calls_curmode++;
	hakios_NSInteger (*real)(id, SEL) = (hakios_NSInteger (*)(id, SEL)) orig_currentDisplayMode_int;
	hakios_NSInteger r = real != NULL ? real(self, _cmd) : -1;

	say("hakios: DISPMODE -[%s currentDisplayMode] -> %ld (NSInteger, call #%u)\n",
	    class_getName(object_getClass(self)), (long) (r != -1 ? r : 0), calls_curmode);
	return r != -1 ? r : 0;
}

static void
hooked_setCurrentMode(id self, SEL _cmd, hakios_NSInteger mode)
{
	calls_setcurmode++;
	say("hakios: DISPMODE -[%s setCurrentMode:%ld] -> forcing 0 (call #%u)\n",
	    class_getName(object_getClass(self)), (long) mode, calls_setcurmode);
	if (orig_setCurrentMode != NULL) {
		((void (*)(id, SEL, hakios_NSInteger)) orig_setCurrentMode)(self, _cmd, 0);
	}
}

/* one method, one hook, one log line; returns 1 when the method was found and swapped */
static int
dispmode_hook_method(Class cls, SEL sel, IMP imp, void **save)
{
	Method m = class_getInstanceMethod(cls, sel);

	if (m == NULL || *save != NULL) {
		return 0;
	}
	*save = (void *) method_setImplementation(m, imp);
	say("hakios: DISPMODE hook %s %s installed\n", class_getName(cls), sel_getName(sel));
	return 1;
}

static void
dispmode_hook_one_class(Class cls, BOOL check_int_return)
{
	static SEL selSetMode, selCurMode, selSetCurMode;
	static SEL selDisUpd;

	if (cls == NULL) {
		return;
	}
	if (selSetMode == NULL) {
		selSetMode = sel_registerName("setDisplayMode:");
		selCurMode = sel_registerName("currentDisplayMode");
		selSetCurMode = sel_registerName("setCurrentMode:");
		selDisUpd = sel_registerName("setDisablesUpdates:");
	}
	dispmode_hook_method(cls, selSetMode, (IMP) hooked_setDisplayMode, &orig_setDisplayMode);
	if (check_int_return) {
		dispmode_hook_method(cls, selCurMode, (IMP) hooked_currentDisplayMode_int,
		    &orig_currentDisplayMode_int);
	} else {
		dispmode_hook_method(cls, selCurMode, (IMP) hooked_currentDisplayMode_id,
		    &orig_currentDisplayMode_id);
	}
	dispmode_hook_method(cls, selSetCurMode, (IMP) hooked_setCurrentMode, &orig_setCurrentMode);
	/* the same force-NO gate the CAWindowServerDisplay path uses, now wherever it exists:
	 * hooked_real_setDisablesUpdates keeps forcing NO through whatever orig it saved. */
	{
		Method m = class_getInstanceMethod(cls, selDisUpd);

		if (m != NULL && orig_real_setDisablesUpdates == NULL) {
			orig_real_setDisablesUpdates = (void (*)(id, SEL, BOOL)) method_setImplementation(
			    m, (IMP) hooked_real_setDisablesUpdates);
			say("hakios: DISPMODE hook %s setDisablesUpdates: force-NO installed\n",
			    class_getName(cls));
		}
	}
}

static void
hakios_install_display_mode_hooks(void)
{
	static int done;
	static const char *const named[] = {
		"CAWindowServerDisplay", "CAWindowServer", "BKDisplay", "CADisplay"
	};
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	if (done) {
		return;
	}
	/* fix94: opt-in.  fix92 installed these unconditionally and the window server never
	 * reached StartWindowServer again (fix92/fix93 logs: no "[Display] StartWindowServer",
	 * no kick_display, no hb beacon -- 58 KB of bring-up lines missing versus fix91).  The
	 * class sweep and the force-NO setDisablesUpdates: hook on CAWindowServerDisplay are the
	 * only difference in that build, so they stay behind ca_dismode=1 until they are shown
	 * to be safe. */
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0 ||
	    strstr(arguments, "ca_dismode=1") == NULL) {
		say("hakios: DISPMODE hooks off (ca_dismode=1 enables them)\n");
		return;
	}
	done = 1;
	for (unsigned i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
		dispmode_hook_one_class(objc_getClass(named[i]), NO);
	}
	/* the named list is a guess; sweep the runtime's own list for any other class that
	 * implements one of the three mode selectors (first 8 hits).  Note: the methodList the
	 * sweep reads is process-wide, so classes from every loaded image are seen here. */
	{
		int n = objc_getClassList(NULL, 0);

		if (n > 0) {
			Class *all = calloc((size_t) n, sizeof(Class));
			int hooked = 0;

			if (all != NULL) {
				n = objc_getClassList(all, n);
				for (int i = 0; i < n && hooked < 8; i++) {
					/* class_getInstanceMethod sees inherited methods too, which would
					 * pull in every NSObject subclass that merely inherits a stub. */
					if (class_getInstanceMethod(all[i], sel_registerName("setDisplayMode:")) == NULL &&
					    class_getInstanceMethod(all[i], sel_registerName("currentDisplayMode")) == NULL &&
					    class_getInstanceMethod(all[i], sel_registerName("setCurrentMode:")) == NULL) {
						continue;
					}
					dispmode_hook_one_class(all[i], YES);
					hooked++;
				}
				free(all);
			}
			say("hakios: DISPMODE sweep done: %d extra class(es) scanned\n", hooked);
		}
	}
	say("hakios: DISPMODE hooks up: setmode=%p curmode=%p setcurmode=%p\n",
	    orig_setDisplayMode, orig_currentDisplayMode_id != NULL ?
	    orig_currentDisplayMode_id : orig_currentDisplayMode_int, orig_setCurrentMode);
	if (orig_setDisplayMode == NULL) {
		say("hakios: DISPMODE setDisplayMode: NOT FOUND in any class\n");
	}
}

static int safe_copy(uintptr_t from, void *into, size_t length);   /* defined below: read without taking a fault */

/* fix89: the kick has many callers -- serverWithOptions calls it directly, and ten dispatch_after
 * blocks fire it on the main queue; serverWithOptions itself is not guaranteed to run on that queue.
 * The dump/scan state below is static and unsynchronised, and the kick drives -update and
 * -[CATransaction flush], which should not re-enter each other from two threads.  Note: fix86revert's
 * whole-stream log doubling is NOT this race (every line doubles, launchd's included) -- that sits in
 * the kernel log transport; this lock only makes a second kicker wait its turn. */
static pthread_mutex_t kick_lock = PTHREAD_MUTEX_INITIALIZER;

static void
hakios_kick_real_display(id disp, const char *why)
{
	SEL selBlanked = sel_registerName("isBlanked");
	SEL selDisUpd = sel_registerName("disablesUpdates");
	SEL selDis = sel_registerName("disabled");
	SEL selUpdate = sel_registerName("update");

	if (disp == nil) {
		return;
	}
	pthread_mutex_lock(&kick_lock);
	BOOL b = ((BOOL (*)(id, SEL)) objc_msgSend)(disp, selBlanked);
	BOOL du = ((BOOL (*)(id, SEL)) objc_msgSend)(disp, selDisUpd);
	BOOL d = ((BOOL (*)(id, SEL)) objc_msgSend)(disp, selDis);

	/* fix66: the mode question.  "without a mode it draws nothing" -- if the kernel's mode
	 * publish is good now, CADisplay has to show it: a non-nil curMode and at least one mode.
	 * This is the same question the C++ side answers through selector 79 block 0x40, asked
	 * from the object side, and safe_copy reads cost nothing. */
	Class cadCls = objc_getClass("CADisplay");
	id mainCad = cadCls != nil ? ((id (*)(id, SEL)) objc_msgSend)((id) cadCls, sel_registerName("mainDisplay")) : nil;
	id curMode = mainCad != nil ? ((id (*)(id, SEL)) objc_msgSend)(mainCad, sel_registerName("currentMode")) : nil;
	CFArrayRef modes = mainCad != nil ? ((CFArrayRef (*)(id, SEL)) objc_msgSend)(mainCad, sel_registerName("availableModes")) : NULL;
	CFIndex modeCount = modes != NULL ? CFArrayGetCount(modes) : -1;

	/* fix67: what mode exactly?  CFCopyDescription prints size and refresh rate. */
	char modeDesc[128] = "nil";

	if (curMode != nil) {
		CFStringRef md = CFCopyDescription((CFTypeRef) curMode);

		if (md != NULL) {
			CFStringGetCString(md, modeDesc, sizeof(modeDesc), kCFStringEncodingUTF8);
			CFRelease(md);
		}
	}

	unsigned disp_id = 0, cad_id = 0;
	if (disp != nil && class_getInstanceMethod(object_getClass(disp), sel_registerName("displayId")) != NULL) {
		disp_id = ((unsigned (*)(id, SEL)) objc_msgSend)(disp, sel_registerName("displayId"));
	}
	if (mainCad != nil && class_getInstanceMethod(object_getClass(mainCad), sel_registerName("displayId")) != NULL) {
		cad_id = ((unsigned (*)(id, SEL)) objc_msgSend)(mainCad, sel_registerName("displayId"));
	}
	say("hakios: kick_display(%s): disp=%p id=%u blank=%d disUpd=%d dis=%d CADisplay=%p id=%u curMode=%s modes=%ld "
	    "(calls: ready=%u ignore=%u begin=%u finish=%u render=%u)\n",
	    why, (void *) disp, disp_id, (int) b, (int) du, (int) d, (void *) mainCad, cad_id, modeDesc, (long) modeCount,
	    calls_ready, calls_ignore, calls_begin, calls_finish, calls_render);

	/*
	 * fix67: find the C++ display behind the ObjC one and step on the starter motor.
	 * fix66 proved _impl's first 64 bytes are a pthread_mutex ("MUTX" at every word): the real
	 * CA::WindowServer::AppleCLCDDisplay pointer lives further in, so scan words[8..23] for a
	 * pointer whose vtable is VT_APPLE_CLCD_DISPLAY + 0x10 (the first method slot of that
	 * vtable).  Disassembling is_ready() out of the shared cache on the host (0x1882f0908)
	 * showed it is not a query: with flags bit 0x8 set (a swap wait still pending) it calls
	 * the IOMobileFramebuffer connection at this+0x318 with 0x80000003 -- the driver logs that
	 * as selector 6, kFBMethodSwapWait, not the power request we first read into it -- clears
	 * bit 0x8 when the wait returns, and returns !(flags & 4).  Nobody in backboardd ever
	 * calls it (calls: ready=0), so we can call it ourselves -- from a periodic kick, not
	 * here: inside serverWithOptions the server is still being constructed.
	 */
	static uintptr_t c_disp = 0;              /* the real AppleCLCDDisplay, once found */
	static int prodded = 0;

	/* fix77: off unless ca_prod=1.  fix76's forced flags made is_ready answer 1, but the
	 * board died after it; until that is separated from the prod, the enumeration round
	 * runs without it. */
	if (hakios_ca_prod && c_disp != 0 && prodded < 6) {
		prodded++;
		uint64_t before = 0;

		safe_copy(c_disp + 0x9f2, &before, sizeof(before));
		say("hakios: PROD display=%p flags=%llx (bit0x8=%d bit0x4=%d) prod=%d/6 why=%s\n",
		    (void *) c_disp, (unsigned long long) before,
		    (int) ((before >> 3) & 1ULL), (int) ((before >> 2) & 1ULL),
		    prodded, why);
		/*
		 * fix76: the starter motor, really engaged.  is_ready() asks one question of flags:
		 * with bit 0x8 (a power change pending) it walks the IOMobileFramebuffer connection at
		 * this+0x318 with 0x80000003 -- our selector 12, power ON -- and clears the bit when
		 * the request succeeds; its return value is !(flags & 4).  The real one we found
		 * (fix75: flags 0x11804804) has no 0x8 and has 0x4 -- power never requested, display
		 * not ready, so backboardd's frame path has nothing to start.  Nothing else in the
		 * process ever sets the request bit either (ready=0 calls in nine rounds).  So we set
		 * the request and clear not-ready ourselves; is_ready does the rest through its own
		 * code path.  If this+0x318 were not a connection, we get an access error at a known
		 * address instead of a mystery -- which is also an answer.
		 */
		if (before != 0) {
			uint64_t armed = (before | 0x8ULL) & ~0x4ULL;

			*(volatile uint64_t *)((char *) (void *) c_disp + 0x9f2) = armed;
		}
		int rdy = orig_is_ready != NULL ? orig_is_ready((void *) c_disp) : -1;
		uint64_t after = 0;

		safe_copy(c_disp + 0x9f2, &after, sizeof(after));
		say("hakios: prod %d: orig_is_ready(%p) = %d, flags %#llx -> %#llx (after: bit0x8=%d bit0x4=%d "
		    "is_ready=%d)\n",
		    prodded, (void *) c_disp, rdy, (unsigned long long) before, (unsigned long long) after,
		    (int) ((after >> 3) & 1ULL), (int) ((after >> 2) & 1ULL), (int) (!(after & 0x4ULL)));
	}

	/*
	 * fix75: the fix67 scan ran exactly once -- at the serverWithOptions moment, before the
	 * server finishes building its hardware display -- and looked only at words[8..23].  On
	 * fix68/74 _impl held one pointer, to CA::WindowServer's EAGLServer: the AppleCLCDDisplay
	 * may not hang off the ObjC display's _impl at all but off the server side.  So: scan on
	 * every kick until found, widen to words[8..95], and walk the EAGLServer object's own
	 * fields one dereference deep.  The full dump prints once; later rounds print only hits.
	 */
	static unsigned scan_round = 0;

	scan_round++;
	if (c_disp == 0 && (scan_round == 1 || (scan_round & 7) == 0)) {
		uintptr_t impl = 0;

		if (safe_copy((uintptr_t) disp + 8, &impl, sizeof(impl)) && impl != 0) {
			uintptr_t words[96];

			bzero(words, sizeof(words));
			if (safe_copy(impl, words, sizeof(words))) {
				if (scan_round == 1) {
					for (int i = 8; i < 64; i += 4) {
						say("hakios: disp->_impl[+%#x..+%#x]: %#lx %#lx %#lx %#lx\n",
						    i * 8, (i + 3) * 8,
						    (unsigned long) words[i], (unsigned long) words[i + 1],
						    (unsigned long) words[i + 2], (unsigned long) words[i + 3]);
					}
				}
				for (int i = 8; i < 96 && c_disp == 0; i++) {
					uintptr_t vt = 0;

					if (words[i] > 0x100000000UL && safe_copy(words[i], &vt, sizeof(vt))) {
						uintptr_t unslid_vt = vt - slide;

						if (scan_round == 1) {
							say("hakios:   disp->_impl[%d] (%#lx) -> *ptr=%#lx (unslid %#lx)\n",
							    i, (unsigned long) words[i], (unsigned long) vt, (unsigned long) unslid_vt);
						}
						if (unslid_vt == VT_APPLE_CLCD_DISPLAY + 0x10) {
							c_disp = words[i];
						} else if (unslid_vt == VT_EAGL_SERVER + 0x10) {
							for (unsigned off = 0x10; off < 0x100 && c_disp == 0; off += 8) {
								uintptr_t f = 0, fvt = 0;

								if (safe_copy(words[i] + off, &f, sizeof(f)) && f > 0x100000000UL &&
								    safe_copy(f, &fvt, sizeof(fvt)) &&
								    fvt - slide == VT_APPLE_CLCD_DISPLAY + 0x10) {
									c_disp = f;
									say("hakios:   FOUND AppleCLCDDisplay=%p via _impl[%d]->EAGLServer+%#x!\n",
									    (void *) c_disp, i, off);
								}
							}
						}
						if (c_disp != 0) {
							uint64_t dflags = 0;
							uint8_t b233 = 0;

							safe_copy(c_disp + 0x9f2, &dflags, sizeof(dflags));
							safe_copy(c_disp + 0x233, &b233, sizeof(b233));
							say("hakios:   FOUND AppleCLCDDisplay=%p! flags=%#llx b233=%#x (ready will be prodded)\n",
							    (void *) c_disp, (unsigned long long) dflags, (unsigned) b233);
							/* fix93: guarantee the ca_prod path runs at least once without
							 * waiting for the 3s periodic kick: one dispatch_after, 2s out,
							 * calling the very same kick function on the main queue. */
							if (hakios_ca_prod) {
								id prod_disp = disp;

								CFRetain((CFTypeRef) prod_disp);
								dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) 2 * NSEC_PER_SEC),
								    dispatch_get_main_queue(), ^{
									hakios_kick_real_display(prod_disp, "ca_prod_direct_2s");
								});
								say("hakios: PROD armed: direct kick scheduled at +2s (ca_prod=1)\n");
							}
						}
					}
				}
			}
		}
	}

	if (orig_real_setDisablesUpdates) {
		orig_real_setDisablesUpdates(disp, sel_registerName("setDisablesUpdates:"), NO);
	}
	if (orig_real_setDisabled) {
		orig_real_setDisabled(disp, sel_registerName("setDisabled:"), NO);
	}
	if (orig_real_setBlanked) {
		orig_real_setBlanked(disp, sel_registerName("setBlanked:"), NO);
	}
	if (class_respondsToSelector(object_getClass(disp), selUpdate)) {
		((void (*)(id, SEL)) objc_msgSend)(disp, selUpdate);
	}
	/* fix67: push whatever is pending through the client side too. */
	Class caTx = objc_getClass("CATransaction");

	if (caTx != nil && class_respondsToSelector(object_getClass((id) caTx), sel_registerName("flush"))) {
		((void (*)(id, SEL)) objc_msgSend)((id) caTx, sel_registerName("flush"));
	}
	pthread_mutex_unlock(&kick_lock);
}

static id hooked_serverWithOptions(id self, SEL _cmd, id options) {
	/*
	 * fix66: hand the server its own options back with only the freeze key removed.  fix65 passed
	 * nil wholesale, which is a blunt instrument: whatever else StartWindowServer put in that
	 * dictionary (display lists, flags) vanished with it.  Describe it first -- what backboardd
	 * really asks for has never been on the record.
	 */
	CFMutableDictionaryRef cleanOpts = NULL;

	if (options != nil) {
		CFStringRef desc = CFCopyDescription((CFTypeRef) options);

		if (desc != NULL) {
			char buf[256] = "?";

			CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8);
			say("hakios: +[CAWindowServer serverWithOptions:] orig options = %s\n", buf);
			CFRelease(desc);
		}
		if (CFGetTypeID((CFTypeRef) options) == CFDictionaryGetTypeID()) {
			cleanOpts = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, (CFDictionaryRef) options);
			if (cleanOpts != NULL) {
				/* fix67: fix66's log named the key -- it is spelled with a SMALL d:
				 * "disableUpdatesOnMainDisplay = 1;".  The capital-D spellings never matched
				 * and the freeze flag went through untouched (disUpd=1 at birth). */
				CFDictionaryRemoveValue(cleanOpts, CFSTR("disableUpdatesOnMainDisplay"));
				CFDictionaryRemoveValue(cleanOpts, CFSTR("DisableUpdatesOnMainDisplay"));
				CFDictionaryRemoveValue(cleanOpts, CFSTR("kCAWindowServerDisableUpdatesOnMainDisplay"));
			}
		}
	}
	id passOpts = (cleanOpts != NULL && CFDictionaryGetCount(cleanOpts) > 0) ? (id) cleanOpts : nil;
	id srv = orig_serverWithOptions ? orig_serverWithOptions(self, _cmd, passOpts) : nil;

	if (cleanOpts != NULL) {
		CFRelease(cleanOpts);
	}
	say("hakios: +[CAWindowServer serverWithOptions:] DONE -> %p\n", (void *) srv);
	if (srv != nil) {
		CFArrayRef disps = ((CFArrayRef (*)(id, SEL)) objc_msgSend)(srv, sel_registerName("displays"));
		CFIndex count = disps ? CFArrayGetCount(disps) : 0;

		say("hakios: CAWindowServer has %ld display(s)\n", (long) count);
		if (count > 0) {
			id disp = (id) CFArrayGetValueAtIndex(disps, 0);
			Class dCls = object_getClass(disp);

			say("hakios: main display=%p class=%s\n", (void *) disp, dCls ? class_getName(dCls) : "?");
			Method mBlank = class_getInstanceMethod(dCls, sel_registerName("setBlanked:"));
			if (mBlank != NULL && orig_real_setBlanked == NULL) {
				orig_real_setBlanked = (void (*)(id, SEL, BOOL)) method_setImplementation(
				    mBlank, (IMP) hooked_real_setBlanked);
			}
			Method mDisUpd = class_getInstanceMethod(dCls, sel_registerName("setDisablesUpdates:"));
			if (mDisUpd != NULL && orig_real_setDisablesUpdates == NULL) {
				orig_real_setDisablesUpdates = (void (*)(id, SEL, BOOL)) method_setImplementation(
				    mDisUpd, (IMP) hooked_real_setDisablesUpdates);
			}
			Method mDis = class_getInstanceMethod(dCls, sel_registerName("setDisabled:"));
			if (mDis != NULL && orig_real_setDisabled == NULL) {
				orig_real_setDisabled = (void (*)(id, SEL, BOOL)) method_setImplementation(
				    mDis, (IMP) hooked_real_setDisabled);
			}
			hakios_kick_real_display(disp, "serverWithOptions");
			CFRetain((CFTypeRef) disp);
			for (int delay_s = 3; delay_s <= 30; delay_s += 3) {
				dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) delay_s * NSEC_PER_SEC),
				    dispatch_get_main_queue(), ^{
					hakios_kick_real_display(disp, "periodic_unblank");
				});
			}
		}
	}
	return srv;
}


static void
cawindowserver_trace_fixes(void)
{
	Class wsCls = objc_getClass("CAWindowServer");
	if (wsCls) {
		Method m_swo = class_getClassMethod(wsCls, sel_registerName("serverWithOptions:"));
		if (m_swo) {
			orig_serverWithOptions = (id (*)(id, SEL, id))method_getImplementation(m_swo);
			method_setImplementation(m_swo, (IMP)hooked_serverWithOptions);
			say("hakios: CAWindowServer real passthrough trace hook installed!\n");
		}
	} else {
		say("hakios: CAWindowServer class not found\n");
	}
	/* dispmode: same init path, before the server is asked for: the mode selectors have to be
	 * hooked before StartWindowServer's first setDisplayMode:/currentDisplayMode round. */
	hakios_install_display_mode_hooks();
}

/* ============================================================================
 * The window server's own "take no updates" flag
 * ============================================================================ */

#include <mach-o/dyld.h>
#include <mach-o/loader.h>

/*
 * QuartzCore keeps a flag that tells the window server to take no updates at all, and backboardd
 * sets it on its way up -- that is its own log line "StartWindowServer: disabling framebuffer
 * changes on display (1) at boot until we are ready for them" -- meaning to clear it when the
 * display becomes ready.  While it is set the window server allocates no display surfaces and never
 * swaps, which is exactly what the phone does (fix49: no MiniIOSurface at all, no selector 4 or 5,
 * no registerNotificationPort, and the panel left the colour the shim gave it).  On a real phone the
 * ready signal comes from the display subsystem (IOMobileFramebuffer's power state and
 * CoreBrightness); both are bypassed here, so nobody ever clears the flag.
 *
 * fix51 could not name the global through dyld and stopped.  fix52 let dlsym name it -- 0x1f2bc2730,
 * holding 120, no boolean -- and the first write to it cost backboardd a SIGBUS.  The kernel logged
 * the reason: "[UX_EXCEPTION] exc=1 code=0x2" is EXC_BAD_ACCESS / KERN_PROTECTION_FAILURE, and it
 * repeated for every generation of the window server (fix52 crashed four of them).  Writable data
 * in QuartzCore would take the write, so what dlsym named sits in a read-only mapping: it is not
 * where backboardd's own write lands.
 *
 * Where it lands can be read out of the dyld shared cache without dyld's help.  dyld binds an
 * executable's references through the export trie of the defining image, the trie's offsets are
 * offsets into the cache file, and the file begins where the cache is mapped -- so the address the
 * trie gives for _kCAWindowServerDisableUpdatesOnMainDisplay is the address dyld handed to
 * backboardd.  That is the flag.  Even its page can be mapped read-only until somebody's first
 * write unnests it, so nothing is ever written before make_writable() has made the page take
 * writes: ask for the bit, and if that is refused, put a private copy of the page in its place.
 *
 * The watch of QuartzCore's writable data from fix52 stays as the witness: when backboardd sets the
 * flag it must show as one bit turning 0 -> 1, independent proof of where the flag really lives.
 */
struct hakios_data_region {
	uintptr_t start;
	uint64_t  words;                 /* 8-byte words of it */
};

/* The writable segments of QuartzCore, and where the image starts.  For an image in the dyld
 * shared cache the slide that _dyld_get_image_vmaddr_slide() reports cannot always be trusted,
 * so the segment's runtime address is worked out from the header's own place in memory instead:
 * the header is __TEXT's start, and every segment is at its vmaddr's distance from __TEXT's. */
static int
hakios_quartzcore_data(uintptr_t *base_out, struct hakios_data_region *regions, int max_regions)
{
	for (uint32_t image = 0; image < _dyld_image_count(); image++) {
		const char *path = _dyld_get_image_name(image);
		const struct mach_header_64 *header;
		const uint8_t *command;
		uint64_t text_vmaddr = 0;
		int found = 0;

		if (path == NULL || strstr(path, "/QuartzCore") == NULL) {
			continue;
		}
		header = (const struct mach_header_64 *) _dyld_get_image_header(image);
		command = (const uint8_t *) (header + 1);
		/* first pass: __TEXT's vmaddr, to measure every other segment's distance from it */
		for (uint32_t n = 0; n < header->ncmds; n++) {
			const struct segment_command_64 *seg = (const struct segment_command_64 *) command;

			if (seg->cmd == LC_SEGMENT_64 && strcmp(seg->segname, "__TEXT") == 0) {
				text_vmaddr = seg->vmaddr;
				break;
			}
			command += seg->cmdsize;
		}
		if (text_vmaddr == 0) {
			continue;
		}
		if (base_out != NULL) {
			*base_out = (uintptr_t) header;
		}
		command = (const uint8_t *) (header + 1);
		for (uint32_t n = 0; n < header->ncmds; n++) {
			const struct segment_command_64 *seg = (const struct segment_command_64 *) command;

			if (seg->cmd == LC_SEGMENT_64 && seg->vmsize >= 8 && seg->vmsize <= 0x4000000
			    && (strcmp(seg->segname, "__DATA") == 0 || strcmp(seg->segname, "__DATA_DIRTY") == 0)) {
				uintptr_t at = (uintptr_t) header + (uintptr_t) (seg->vmaddr - text_vmaddr);

				if (regions != NULL && found < max_regions) {
					regions[found].start = at;
					regions[found].words = seg->vmsize / 8;
				}
				found++;
			}
			command += seg->cmdsize;
		}
		return found;
	}
	return 0;
}

/* A ULEB128; NULL when it would run past `end` or past 64 bits. */
static const uint8_t *
uleb128(const uint8_t *p, const uint8_t *end, uint64_t *out)
{
	uint64_t value = 0;
	int shift = 0;

	while (p < end) {
		uint8_t byte = *p++;

		value |= (uint64_t) (byte & 0x7f) << shift;
		if ((byte & 0x80) == 0) {
			*out = value;
			return p;
		}
		shift += 7;
		if (shift >= 64) {
			return NULL;
		}
	}
	return NULL;
}

/*
 * Read from our own address space without taking a fault.  vm_read_overwrite answers
 * KERN_INVALID_ADDRESS for what is not mapped, where a plain dereference answers with a signal --
 * fix54 computed a trie address (0x8dfbf730) that nothing had mapped, read it while walking what
 * it believed was a trie, and the window server fell with SIGSEGV before printing one line.  Every
 * address this file computes is proven readable here before anything touches it.
 */
static int
safe_copy(uintptr_t from, void *into, size_t length)
{
	mach_msg_type_number_t given = 0;
	kern_return_t kr = vm_read_overwrite(mach_task_self(), (vm_address_t) from, (vm_size_t) length,
	                                     (vm_address_t) into, &given);

	return kr == KERN_SUCCESS && given == (mach_msg_type_number_t) length;
}

static int
peek_byte(uintptr_t where, unsigned char *out)
{
	return safe_copy(where, out, 1);
}

struct hakios_cache_mapping {
	uint64_t address;      /* where these bytes are mapped, as the cache prefers it */
	uint64_t size;
	uint64_t file_offset;  /* which bytes of the cache file they are */
};

/*
 * The shared cache header sits at the first byte of the mapping and its mapping table is the
 * ground truth between a file offset in the cache file and the address it lives at.  fix54
 * bridged through the image's __LINKEDIT segment alone and trusted the answer; the table is the
 * second opinion, and whatever either bridge says is proven with safe_copy() before it is read.
 */
static int
hakios_cache_mappings(struct hakios_cache_mapping *into, int max)
{
	uint8_t head[0x20];
	/*
	 * fix65: this passed NULL, and iOS 16.5's dyld writes the size without checking:
	 * _dyld_get_shared_cache_range (libdyld's APIs, unslid 0x1a45d0928) stores *mappedSize
	 * unconditionally, so asking it for the range with NULL faulted at 0x0 -- si_addr 0x0,
	 * pc 0x1a45d0928 -- and that was the real killer of every backboardd generation from
	 * fix55 to fix64, not the export trie.  Always hand it a real variable.
	 */
	size_t cache_len = 0;
	const void *cache = _dyld_get_shared_cache_range(&cache_len);
	uint32_t offset;
	uint32_t count;
	int n;

	if (cache == NULL || !safe_copy((uintptr_t) cache, head, sizeof(head))
	    || memcmp(head, "dyld_v1", 7) != 0) {
		return 0;
	}
	memcpy(&offset, head + 0x10, sizeof(offset));
	memcpy(&count, head + 0x14, sizeof(count));
	if (count == 0 || count > (uint32_t) max) {
		return 0;
	}
	if (offset == 0 || !safe_copy((uintptr_t) cache + offset, into, count * sizeof(*into))) {
		return 0;
	}
	for (n = 0; n < (int) count; n++) {
		if (into[n].size == 0 || into[n].size > 0x400000000ULL) {
			return 0;
		}
	}
	return (int) count;
}

static uintptr_t
hakios_runtime_for_file(uint64_t fileoff, int count, const struct hakios_cache_mapping *mappings,
                        uintptr_t slide)
{
	int n;

	for (n = 0; n < count; n++) {
		if (fileoff >= mappings[n].file_offset && fileoff - mappings[n].file_offset < mappings[n].size) {
			return (uintptr_t) (mappings[n].address + slide + (fileoff - mappings[n].file_offset));
		}
	}
	return 0;
}

/*
 * Walk the export trie for one symbol name and read the address it exports.  The trie is the one
 * from Apple's dyld: a node is a terminal length, that many bytes of terminal payload, an edge
 * count, and per edge a label and the child's offset from the trie's start.  A terminal payload is
 * flags and then, for an ordinary export, the unslid address.
 */
static int
hakios_trie_lookup(const uint8_t *trie, const uint64_t size, const char *symbol, uint64_t *address_out)
{
	const uint8_t *end = trie + size;
	uint64_t node = 0;
	const char *rest = symbol;

	/* One hop consumes at least one character of the name, so 64 is plenty for any symbol. */
	for (int hop = 0; hop < 64; hop++) {
		const uint8_t *p;
		uint64_t terminal;
		uint64_t edges;
		uint64_t descended = 0;

		if (node >= size) {
			return 0;
		}
		p = trie + node;
		if ((p = uleb128(p, end, &terminal)) == NULL || (uint64_t) (p - trie) + terminal > size) {
			return 0;
		}
		p += terminal;
		if ((p = uleb128(p, end, &edges)) == NULL || edges > 1024) {
			return 0;
		}
		for (uint64_t edge = 0; edge < edges; edge++) {
			const char *label = (const char *) p;
			uint64_t child;
			size_t length;

			if (p >= end) {
				return 0;
			}
			length = strnlen(label, (size_t) (end - p));
			if (length == 0 || length == (size_t) (end - p)) {
				return 0;
			}
			p += length + 1;
			if ((p = uleb128(p, end, &child)) == NULL) {
				return 0;
			}
			/*
			 * A trie is serialized with every child behind its parent, so a pointer backwards or
			 * out of the trie means the bytes are not a trie after all.  fix53 learned this the
			 * hard way: it walked what it thought was a trie and spun inside a cycle forever.
			 */
			if (child <= node || child >= size) {
				return 0;
			}
			if (descended == 0 && strncmp(rest, label, length) == 0) {
				descended = child;
				rest += length;
			}
		}
		if (*rest == 0 && descended != 0) {
			uint64_t flags;
			uint64_t target;
			const uint8_t *q;

			node = descended;
			if ((q = uleb128(trie + node, end, &terminal)) == NULL || terminal == 0) {
				return 0;
			}
			if ((q = uleb128(q, end, &flags)) == NULL) {
				return 0;
			}
			if (flags & 0x10) {
				return 0;                                    /* a re-export: the name lives in another image */
			}
			if ((q = uleb128(q, end, &target)) == NULL) {
				return 0;
			}
			*address_out = target;
			return 1;
		}
		if (descended == 0) {
			return 0;
		}
		node = descended;
	}
	return 0;
}

/*
 * Where dyld binds `symbol` for the process: the export trie of the image that defines it
 * (QuartzCore here) holds the address dyld hands to every executable that references it.
 *
 * The load commands give the trie's offset in the cache FILE, and the cache is split on disk
 * (.01 ... .42, .dylddata, .dyldlinkedit), so that offset does not count from where the cache is
 * mapped in this process -- fix53 assumed it did, walked a text region as if it were a trie, and
 * spun in a cycle forever without printing one line.  fix54 bridged through the image's own
 * __LINKEDIT segment, which is right only while that segment's bytes lie contiguous in memory,
 * and then trusted the computed address: it was wrong, unmapped, and reading it took the window
 * server down with SIGSEGV before a single line was printed.  Since fix55 there are two bridges
 * (the segment and the cache header's own mapping table), every candidate address is proven with
 * safe_copy() and copied locally before it is walked, and the answer must land inside one of
 * QuartzCore's own data segments to be believed at all.
 */
static int
hakios_export_address(const char *symbol, uintptr_t *address_out)
{
	size_t cache_length = 0;
	const void *cache = _dyld_get_shared_cache_range(&cache_length);
	struct hakios_cache_mapping mappings[16];
	int count = hakios_cache_mappings(mappings, 16);
	uintptr_t cache_slide = 0;
	static int told = 0;
	int n;

	for (n = 0; n < count; n++) {
		if (mappings[n].file_offset == 0) {
			cache_slide = (uintptr_t) cache - (uintptr_t) mappings[n].address;
		}
	}

	for (uint32_t image = 0; image < _dyld_image_count(); image++) {
		const char *path = _dyld_get_image_name(image);
		const struct mach_header_64 *header;
		const uint8_t *command;
		uintptr_t slide;
		uint64_t link_vmaddr = 0;
		uint64_t link_fileoff = 0;
		uint64_t link_filesize = 0;
		uint64_t trie_offset = 0;
		uint64_t trie_size = 0;
		uintptr_t candidates[3];
		uint64_t data_start[8];
		uint64_t data_size[8];
		uint32_t codes[192];
		int code_count = 0;
		int data_count = 0;
		int candidate_count = 0;

		if (path == NULL || strstr(path, "/QuartzCore") == NULL) {
			continue;
		}
		header = (const struct mach_header_64 *) _dyld_get_image_header(image);
		slide = (uintptr_t) _dyld_get_image_vmaddr_slide(image);
		command = (const uint8_t *) (header + 1);
		for (uint32_t c = 0; c < header->ncmds; c++) {
			const struct segment_command_64 *segment = (const struct segment_command_64 *) command;

			if (code_count < 192) {
				codes[code_count++] = segment->cmd;
			}
			if (segment->cmd == LC_SEGMENT_64) {
				if (strcmp(segment->segname, "__LINKEDIT") == 0) {
					link_vmaddr = segment->vmaddr;
					link_fileoff = segment->fileoff;
					link_filesize = segment->filesize;
				} else if (strncmp(segment->segname, "__DATA", 6) == 0
				           || strncmp(segment->segname, "__AUTH", 6) == 0) {
					if (data_count < 8) {
						data_start[data_count] = segment->vmaddr + (uint64_t) slide;
						data_size[data_count] = segment->vmsize;
					}
					data_count++;
				}
			} else if (segment->cmd == LC_DYLD_INFO_ONLY) {
				const struct dyld_info_command *info = (const struct dyld_info_command *) command;

				trie_offset = info->export_off;
				trie_size = info->export_size;
			} else if (segment->cmd == LC_DYLD_EXPORTS_TRIE) {
				const struct linkedit_data_command *tried = (const struct linkedit_data_command *) command;

				trie_offset = tried->dataoff;
				trie_size = tried->datasize;
			}
			command += segment->cmdsize;
		}
		/*
		 * fix55 printed none of this because the check below ran first and QuartzCore's
		 * trie_size was zero -- so the load commands come out whatever they hold, export
		 * table or not.  The command codes join the report for the same reason: guessing
		 * which load commands a 16.5 cache image carries is guessing blind.
		 */
		if (!told) {
			told = 1;
			say("hakios: cache at %p length %#lx, its slide %#lx\n", cache,
			    (unsigned long) cache_length, (unsigned long) cache_slide);
			say("hakios: QuartzCore at %#lx linkedit vmaddr %#llx fileoff %#llx size %#llx; trie "
			    "fileoff %#llx size %#llx\n", (unsigned long) header,
			    (unsigned long long) link_vmaddr, (unsigned long long) link_fileoff,
			    (unsigned long long) link_filesize, (unsigned long long) trie_offset,
			    (unsigned long long) trie_size);
			for (n = 0; n < count; n++) {
				say("hakios:   cache file %#llx -> %#llx (%#llx bytes)\n",
				    (unsigned long long) mappings[n].file_offset,
				    (unsigned long long) mappings[n].address,
				    (unsigned long long) mappings[n].size);
			}
			for (n = 0; n < code_count; n++) {
				say("hakios:   cmd %d: %#x%s", n, codes[n], (n % 4 == 3 || n + 1 == code_count) ? "\n" : "  ");
			}
		}

		if (trie_size == 0 || trie_size > (1u << 22)) {
			continue;
		}

		/* Candidate one: the image's __LINKEDIT bridge, valid while the segment is contiguous. */
		if (link_filesize != 0 && trie_offset >= link_fileoff
		    && trie_offset + trie_size <= link_fileoff + link_filesize) {
			candidates[candidate_count++] =
			    slide + (uintptr_t) link_vmaddr + (uintptr_t) (trie_offset - link_fileoff);
		}
		/* Candidate two: the cache header's mapping table, which knows the file's layout. */
		if (count != 0) {
			uintptr_t by_table = hakios_runtime_for_file(trie_offset, count, mappings, cache_slide);

			if (by_table != 0) {
				candidates[candidate_count++] = by_table;
			}
		}
		/* Candidate three: fix53's naive guess, harmless now that reads go through safe_copy. */
		if (cache != NULL) {
			candidates[candidate_count++] = (uintptr_t) cache + (uintptr_t) trie_offset;
		}

		{
			static int told_candidates = 0;

			if (!told_candidates) {
				told_candidates = 1;
				for (n = 0; n < candidate_count; n++) {
					say("hakios:   trie candidate %d at %#lx\n", n, (unsigned long) candidates[n]);
				}
			}
		}

		for (n = 0; n < candidate_count; n++) {
			uint8_t *local;
			uint64_t target = 0;

			if (candidates[n] == 0 || (local = malloc((size_t) trie_size)) == NULL) {
				continue;
			}
			if (safe_copy(candidates[n], local, (size_t) trie_size)
			    && hakios_trie_lookup(local, trie_size, symbol, &target)) {
				uintptr_t runtime = (uintptr_t) slide + (uintptr_t) target;
				int holds = 0;
				int r;

				free(local);
				for (r = 0; r < data_count && r < 8; r++) {
					if (runtime >= data_start[r] && runtime - data_start[r] < data_size[r]) {
						holds = 1;
					}
				}
				if (data_count != 0 && !holds) {
					say("hakios: the trie's answer %#lx is outside QuartzCore's data segments, "
					    "not believed\n", (unsigned long) runtime);
					continue;
				}
				*address_out = runtime;
				return 1;
			}
			free(local);
		}
	}
	return 0;
}

/*
 * Make the page holding `where` take a write.  fix52 showed the fault the naive way: the cache
 * maps some of its data read-only and the kernel answers a write with KERN_PROTECTION_FAILURE --
 * SIGBUS, not a refusal a program can see coming.  Asking for the bit is the cheap way; when it is
 * refused, the page is replaced by a private anonymous copy with the same bytes on it.  Only this
 * process sees the copy, which for a flag the window server sets for itself is exactly right.
 */
static int
make_writable(uintptr_t where)
{
	size_t pagesize = (size_t) getpagesize();
	uintptr_t page = where & ~(pagesize - 1);
	unsigned char *spare;
	void *placed;

	if (mprotect((void *) page, pagesize, PROT_READ | PROT_WRITE) == 0) {
		return 1;
	}
	say("hakios: mprotect of %#lx failed (errno %d), copying the page private\n",
	    (unsigned long) page, errno);
	if ((spare = malloc(pagesize)) == NULL) {
		return 0;
	}
	memcpy(spare, (const void *) page, pagesize);
	placed = mmap((void *) page, pagesize, PROT_READ | PROT_WRITE,
	              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (placed != (void *) page) {
		free(spare);
		say("hakios: no page to give, %#lx wanted %#p\n", (unsigned long) page, placed);
		return 0;
	}
	memcpy((void *) page, spare, pagesize);
	free(spare);
	return 1;
}

static void *
unfreeze_window_server(void *arg)
{
	static const char *const symbol = "_kCAWindowServerDisableUpdatesOnMainDisplay";
	/*
	 * fix55 learned the hard way what names dlsym may be given here: the C name without the
	 * leading underscore sent dyld's own lookup down a path that executed data -- pc landed in
	 * the cache's local symbols blob (0x1a45d0928) dereferencing NULL, and every generation of
	 * the window server died on it.  Only the underscored name, as the symbol table spells it,
	 * is safe to ask for.
	 */
	static const char *const names[] = {
		"_kCAWindowServerDisableUpdatesOnMainDisplay",     /* as the symbol table spells it */
	};
	volatile unsigned char *flag = NULL;                   /* where the export trie puts it */
	volatile unsigned char *lookalike = NULL;              /* where dlsym puts it, for the record */
	uintptr_t quartzcore = 0;
	uintptr_t exported = 0;
	struct hakios_data_region regions[4];
	uint64_t *was;
	uint64_t words = 0;
	int region_count;
	int cleared = 0;
	int reported = 0;
	int complained = 0;
	unsigned int started = 0;
	unsigned char byte = 0;

	(void) arg;
	/*
	 * Wait for QuartzCore to be mapped -- its load commands are what every step below reads --
	 * and snapshot its writable data immediately.  The snapshot has to precede backboardd's own
	 * write of the freeze flag (~60 s into its boot) or the watch sees nothing; whether the trie
	 * bridges reach the export data is decided the same moment QuartzCore is mapped, so that hunt
	 * comes second and is tried once.
	 */
	for (int attempt = 0; attempt < 240; attempt++) {
		region_count = hakios_quartzcore_data(&quartzcore, regions, 4);
		if (region_count != 0) {
			break;
		}
		usleep(250000);
	}
	if (region_count == 0) {
		say("hakios: QuartzCore is not loaded, giving up on the freeze flag\n");
		return NULL;
	}
	for (int region = 0; region < region_count; region++) {
		words += regions[region].words;
	}
	if ((was = calloc((size_t) words, sizeof(*was))) == NULL) {
		return NULL;
	}
	say("hakios: watching %llu words of QuartzCore's data for the flag\n", (unsigned long long) words);
	{
		uint64_t start_of = 0;

		for (int region = 0; region < region_count; region++) {
			memcpy(was + start_of, (const void *) regions[region].start, (size_t) regions[region].words * 8);
			start_of += regions[region].words;
		}
	}
	if (hakios_export_address(symbol, &exported)) {
		flag = (volatile unsigned char *) exported;
	}
	if (flag != NULL) {
		unsigned char window[128];
		uintptr_t low = (uintptr_t) flag & ~(uintptr_t) 7;

		if (!peek_byte((uintptr_t) flag, &byte)) {
			say("hakios: the export trie puts the freeze flag at %p, but nothing is mapped there\n",
			    (void *) flag);
			flag = NULL;
		} else {
			started = byte;
			say("hakios: the freeze flag is at %p (where dyld binds it), it starts at %u\n",
			    (void *) flag, started);
			if (safe_copy(low, window, sizeof(window))) {
				for (int n = 0; n < 16; n++) {
					say("hakios:   %#lx: %#02x\n", (unsigned long) (low + n * 8),
					    (unsigned int) window[n * 8]);
				}
			}
		}
	} else {
		say("hakios: QuartzCore's export trie does not name the freeze flag\n");
	}
	for (unsigned name = 0; name < sizeof(names) / sizeof(names[0]) && lookalike == NULL; name++) {
		lookalike = (volatile unsigned char *) dlsym(RTLD_DEFAULT, names[name]);
	}
	if (lookalike != NULL) {
		Dl_info info;

		if (dladdr((const void *) lookalike, &info) && info.dli_sname != NULL
		    && strcmp(info.dli_sname, symbol) == 0) {
			say("hakios: dlsym's %p is the flag too, by dladdr (%s)\n", (void *) lookalike, info.dli_sname);
			if (flag == NULL && peek_byte((uintptr_t) lookalike, &byte)) {
				flag = lookalike;
				started = byte;
			}
		} else {
			say("hakios: dlsym names %p: %s in %s -- not the flag, reading only\n", (void *) lookalike,
			    info.dli_sname != NULL ? info.dli_sname : "no name",
			    info.dli_fname != NULL ? info.dli_fname : "no image");
		}
	}
	if (flag == NULL) {
		say("hakios: no way to the freeze flag, watching QuartzCore's data alone\n");
	}
	for (int round = 0; round < 1200; round++) {
		uint64_t cursor = 0;

		for (int region = 0; region < region_count; region++) {
			for (uint64_t word = 0; word < regions[region].words; word++) {
				uint64_t now = *(const uint64_t *) (regions[region].start + word * 8);

				if (now != was[cursor + word]) {
					uint64_t at = regions[region].start + word * 8 - quartzcore;
					uint64_t xor = now ^ was[cursor + word];
					int bits = 0, bit = -1, set = 0;

					for (int n = 0; n < 64; n++) {
						if ((xor >> n) & 1) {
							bits++;
							bit = n;
						}
					}
					if (bits == 1) {
						set = (int) ((now >> bit) & 1);
					}
					if (bits != 1 || !set) {
						/* data that moves in QuartzCore is normal; what is looked for is a flag
						 * being set, one bit, 0 -> 1 */
						was[cursor + word] = now;
						continue;
					}
					say("hakios: QuartzCore + 0x%llx (living at %#lx): %#llx -> %#llx (bit set)\n",
					    (unsigned long long) at, (unsigned long) (regions[region].start + word * 8),
					    (unsigned long long) was[cursor + word], (unsigned long long) now);
					was[cursor + word] = now;
					if (++reported >= 400) {
						free(was);
						say("hakios: enough changes reported, the freeze flag watch is over\n");
						return NULL;
					}
				}
			}
			cursor += regions[region].words;
		}
		if (flag != NULL && cleared < 16) {
			unsigned int frozen;

			if (!peek_byte((uintptr_t) flag, &byte)) {
				say("hakios: the freeze flag at %p went unreadable; leaving it alone\n", (void *) flag);
				flag = NULL;
			} else if ((frozen = byte) != 0) {
				if (frozen > 1 && started > 1) {
					/*
					 * Not boolean, and not boolean from the first read either: this is somebody
					 * else's byte, not the flag (fix52: dlsym's address held 120 and taking a
					 * write there put the window server down with a protection fault).  Leave
					 * the byte to its owner.
					 */
					if (!complained++) {
						say("hakios: the freeze flag's byte holds %u, %u when found: no boolean, so "
						    "no write\n", frozen, started);
					}
				} else if (make_writable((uintptr_t) flag)) {
					unsigned char after = 0;

					*flag = 0;
					peek_byte((uintptr_t) flag, &after);
					say("hakios: nothing ever tells the window server the display is ready: cleared "
					    "the freeze flag (%d), %u -> %u\n", ++cleared, frozen, (unsigned int) after);
				} else {
					say("hakios: the freeze flag's page would not take the write, %u stays\n", frozen);
					flag = NULL;
				}
			}
		}
		usleep(250000);
	}
	free(was);
	say("hakios: freeze flag watch over, %d clears\n", cleared);
	return NULL;
}

/*
 * The rain: when /redwood/etc/rain is in the image, the window server brings the board down
 * five minutes after it starts.  There is no other way back to fastboot from a booted XNU --
 * halt_all_cpus() was turned into a wfe spin, so reboot(2) stops rather than resets -- and a
 * person at the desk should not be required to hold VOL-DOWN through every iteration.  A signal
 * that init is not shielded from (the kernel only spares launchd's SIGKILL, SIGTERM and SIGSTOP)
 * ends launchd; XNU panics on that ("Always panic for launchd"), the patched panic path calls
 * loader_return(), and the shim's USB shell is listening again on the same CDC-ACM port where
 * "reboot" sends the board to fastboot.  tools/autoloop.py reads the shell's prompt and says it.
 */
static void *
rain_later(void *arg)
{
	(void) arg;
	for (int tick = 0; tick < 30; tick++) {
		usleep(10000000);
	}
	say("hakios: the watch was enough; the rain comes (launchd will fall, the shim will catch)\n");
	if (kill(1, SIGSEGV) != 0) {
		say("hakios: launchd would not take the signal (errno %d)\n", errno);
	}
	for (;;) {
		sleep(60);
	}
	return NULL;
}

/* ============================================================================
 * CAContext definesDisplayBounds SWAP TRIGGER & CPU RENDERER
 * ============================================================================ */
static IMP orig_CAContext_localWithOptions  = NULL;
static IMP orig_CAContext_remoteWithOptions = NULL;
static IMP orig_CAContext_local              = NULL;
static IMP orig_CAContext_remote             = NULL;

static CFDictionaryRef hakios_patch_cacontext_options(CFDictionaryRef opts) {
	CFMutableDictionaryRef m = opts
		? CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, opts)
		: CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
		                            &kCFTypeDictionaryKeyCallBacks,
		                            &kCFTypeDictionaryValueCallBacks);
	if (m) {
		CFDictionarySetValue(m, CFSTR("definesDisplayBounds"), kCFBooleanTrue);
	}
	return m;
}

static id hooked_CAContext_localWithOptions(id self, SEL _cmd, CFDictionaryRef opts) {
	CFDictionaryRef patched = hakios_patch_cacontext_options(opts);
	say("hakios: +[CAContext localContextWithOptions:] -> forcing definesDisplayBounds=YES\n");
	id ctx = ((id (*)(id, SEL, CFDictionaryRef))orig_CAContext_localWithOptions)(
		self, _cmd, patched ? patched : opts);
	if (patched) CFRelease(patched);
	return ctx;
}

static id hooked_CAContext_remoteWithOptions(id self, SEL _cmd, CFDictionaryRef opts) {
	CFDictionaryRef patched = hakios_patch_cacontext_options(opts);
	say("hakios: +[CAContext remoteContextWithOptions:] -> forcing definesDisplayBounds=YES\n");
	id ctx = ((id (*)(id, SEL, CFDictionaryRef))orig_CAContext_remoteWithOptions)(
		self, _cmd, patched ? patched : opts);
	if (patched) CFRelease(patched);
	return ctx;
}

static id hooked_CAContext_local(id self, SEL _cmd) {
	say("hakios: +[CAContext localContext] called -> redirecting with definesDisplayBounds=YES\n");
	CFDictionaryRef patched = hakios_patch_cacontext_options(NULL);
	id ctx = nil;
	if (orig_CAContext_localWithOptions) {
		ctx = ((id (*)(id, SEL, CFDictionaryRef))orig_CAContext_localWithOptions)(
			self, sel_registerName("localContextWithOptions:"), patched);
	} else if (orig_CAContext_local) {
		ctx = ((id (*)(id, SEL))orig_CAContext_local)(self, _cmd);
	}
	if (patched) CFRelease(patched);
	return ctx;
}

static id hooked_CAContext_remote(id self, SEL _cmd) {
	say("hakios: +[CAContext remoteContext] called -> redirecting with definesDisplayBounds=YES\n");
	CFDictionaryRef patched = hakios_patch_cacontext_options(NULL);
	id ctx = nil;
	if (orig_CAContext_remoteWithOptions) {
		ctx = ((id (*)(id, SEL, CFDictionaryRef))orig_CAContext_remoteWithOptions)(
			self, sel_registerName("remoteContextWithOptions:"), patched);
	} else if (orig_CAContext_remote) {
		ctx = ((id (*)(id, SEL))orig_CAContext_remote)(self, _cmd);
	}
	if (patched) CFRelease(patched);
	return ctx;
}

static void hakios_install_cacontext_swap_trigger(void) {
	// CoreAnimation'ın yerleşik CPU Software Renderer (sw_new_context) kullanmasını sağla
	setenv("CA_DISABLE_GPU", "1", 1);
	setenv("CA_FORCE_SW_RENDERER", "1", 1);

	Class caCtxCls = objc_getClass("CAContext");
	if (!caCtxCls) {
		say("hakios: CAContext class not found\n");
		return;
	}

	Method mLocalOpts = class_getClassMethod(caCtxCls, sel_registerName("localContextWithOptions:"));
	if (mLocalOpts) {
		orig_CAContext_localWithOptions = method_setImplementation(
			mLocalOpts, (IMP)hooked_CAContext_localWithOptions);
	}

	Method mRemoteOpts = class_getClassMethod(caCtxCls, sel_registerName("remoteContextWithOptions:"));
	if (mRemoteOpts) {
		orig_CAContext_remoteWithOptions = method_setImplementation(
			mRemoteOpts, (IMP)hooked_CAContext_remoteWithOptions);
	}

	Method mLocal = class_getClassMethod(caCtxCls, sel_registerName("localContext"));
	if (mLocal) {
		orig_CAContext_local = method_setImplementation(
			mLocal, (IMP)hooked_CAContext_local);
	}

	Method mRemote = class_getClassMethod(caCtxCls, sel_registerName("remoteContext"));
	if (mRemote) {
		orig_CAContext_remote = method_setImplementation(
			mRemote, (IMP)hooked_CAContext_remote);
	}

	say("hakios: CAContext definesDisplayBounds swap trigger installed!\n");
}

/* ============================================================================
 * 5. BKHIDSystemInterface SUB-OBJECT PROTECTION (deliveryManager, senderCache)
 * ============================================================================ */
static id g_hid_stub_proxy = nil;
static id (*orig_hid_deliveryManager)(id, SEL);
static id (*orig_hid_senderCache)(id, SEL);
static id (*orig_hid_clientConnectionManager)(id, SEL);

static id hakios_get_hid_stub(const char *getter_name) {
	if (!g_hid_stub_proxy) {
		Class nsObj = objc_getClass("NSObject");
		Class cls = objc_allocateClassPair(nsObj, "HakiosNilSafeStub", 0);
		if (cls) {
			Class meta = object_getClass((id)cls);
			class_addMethod(meta, sel_registerName("resolveInstanceMethod:"), (IMP)hakios_resolve_instance_method, "B@::");
			class_addMethod(meta, sel_registerName("resolveClassMethod:"), (IMP)hakios_resolve_class_method, "B@::");
			objc_registerClassPair(cls);
		} else {
			cls = objc_getClass("HakiosNilSafeStub");
		}
		g_hid_stub_proxy = class_createInstance(cls, 0);
	}
	say("hakios: -[BKHIDSystemInterface %s] -> returning nil-safe stub %p\n", getter_name, (void *)g_hid_stub_proxy);
	return g_hid_stub_proxy;
}

static id
my_hid_deliveryManager(id self, SEL cmd)
{
	id ret = orig_hid_deliveryManager ? orig_hid_deliveryManager(self, cmd) : nil;
	if (ret) return ret;
	return hakios_get_hid_stub("deliveryManager");
}

static id
my_hid_senderCache(id self, SEL cmd)
{
	id ret = orig_hid_senderCache ? orig_hid_senderCache(self, cmd) : nil;
	if (ret) return ret;
	return hakios_get_hid_stub("senderCache");
}

static id
my_hid_clientConnectionManager(id self, SEL cmd)
{
	id ret = orig_hid_clientConnectionManager ? orig_hid_clientConnectionManager(self, cmd) : nil;
	if (ret) return ret;
	return hakios_get_hid_stub("clientConnectionManager");
}

/* ============================================================================
 * 6. INTERPOSITION: FAST-PATH BYPASS FOR OSCARD, COREACCESSORIES & CFPREFSD
 * ============================================================================ */
typedef void * xpc_connection_t;
typedef void * xpc_object_t;
extern void *xpc_get_type(xpc_object_t object);
extern const char *xpc_type_get_name(void *type);
extern char *xpc_copy_description(xpc_object_t object);
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, void *targetq, uint64_t flags);
extern xpc_object_t xpc_connection_send_message_with_reply_sync(xpc_connection_t connection, xpc_object_t message);
extern const char *xpc_connection_get_name(xpc_connection_t connection);
extern xpc_object_t xpc_dictionary_create(const char * const *keys, const xpc_object_t *values, size_t count);
extern void xpc_dictionary_set_value(xpc_object_t xdict, const char *key, xpc_object_t value);
extern void xpc_dictionary_set_uint64(xpc_object_t xdict, const char *key, uint64_t value);
extern void xpc_release(xpc_object_t object);
extern uint64_t xpc_dictionary_get_uint64(xpc_object_t xdict, const char *key);
extern int pthread_main_np(void);
extern xpc_object_t xpc_retain(xpc_object_t object);
extern void xpc_connection_resume(xpc_connection_t connection);
extern void xpc_connection_activate(xpc_connection_t connection);
extern int CoreAccessoriesInitForHID(void);

static int my_CoreAccessoriesInitForHID(void)
{
	say("hakios: CoreAccessoriesInitForHID called -> fast return 0\n");
	return 0;
}

static int hakios_daemon_should_bypass_xpc(const char *name)
{
	const char *prog = getprogname();
	if (name == NULL) {
		/* fix149: the anonymous lsd call is made by SpringBoard, not by lsd;
		 * the lsd-side fix147 bypass only stalled lsd's own startup.  Leave lsd
		 * alone and let inject_sb.c answer the SpringBoard call. */
		return 0;
	}
	/* /usr/sbin/cfprefsd imajda yok; runningboardd/backboardd senkron cfprefsd
	 * cagrisinda sonsuza dek mach_msg2_trap icinde kilitlenmesin (fix136). */
	if (strcmp(name, "com.apple.cfprefsd.daemon") == 0 ||
	    strcmp(name, "com.apple.cfprefsd.agent") == 0 ||
	    strcmp(name, "com.apple.cfprefsd.daemon.system") == 0 ||
	    strcmp(name, "com.apple.mobileactivationd") == 0 ||
	    strcmp(name, "com.apple.diagnosticd") == 0 ||
	    strcmp(name, "com.apple.analyticsd") == 0 ||
	    strcmp(name, "com.apple.powerlog.plxpclogger.xpc") == 0) {
		return 1;
	}
	if (prog != NULL && strcmp(prog, "runningboardd") == 0) {
		if (strcmp(name, "com.apple.containermanagerd") == 0 ||
		    strcmp(name, "com.apple.lsd.mapdb") == 0 ||
		    strcmp(name, "com.apple.mobile.keybagd.xpc") == 0 ||
		    strcmp(name, "com.apple.mobile.keybagd.UserManager.xpc") == 0) {
			return 1;
		}
	}
	return 0;
}

static xpc_connection_t my_xpc_connection_create_mach_service(const char *name, void *targetq, uint64_t flags)
{
	if (name && (strcmp(name, "com.apple.oscard") == 0 ||
	             strcmp(name, "com.apple.ACCHWComponentAuthService") == 0 ||
	             strcmp(name, "com.apple.mobileactivationd") == 0 ||
	             strcmp(name, "com.apple.cfprefsd.daemon") == 0 ||
	             strcmp(name, "com.apple.cfprefsd.agent") == 0)) {
		say("hakios: %s xpc_connection_create_mach_service(%s) -> fast NULL\n",
		    getprogname() ? getprogname() : "?", name);
		return (xpc_connection_t)0;
	}
	if (name && (strcmp(name, "com.apple.backboard.display.services") == 0 ||
	             strcmp(name, "com.apple.runningboard") == 0 ||
	             strncmp(name, "com.apple.lsd", 13) == 0)) {
		say("hakios: %s xpc_connection_create_mach_service(%s) flags %#llx\n",
		    getprogname() ? getprogname() : "?", name, (unsigned long long)flags);
	}
	return xpc_connection_create_mach_service(name, targetq, flags);
}

static xpc_object_t my_xpc_connection_send_message_with_reply_sync(xpc_connection_t connection, xpc_object_t message)
{
	const char *name = connection != NULL ? xpc_connection_get_name(connection) : NULL;
	const char *prog = getprogname();
	static unsigned daemon_sync_logged;
	static xpc_object_t lsd_cm_class10;


	{
		int bypass = hakios_daemon_should_bypass_xpc(name);
		static unsigned bypass_logged;
		if (bypass == 2) {
			void *sym = dlsym(RTLD_DEFAULT, "xpc_error_connection_interrupted");
			xpc_object_t err = sym != NULL ? *(xpc_object_t *)sym : NULL;
			if (bypass_logged < 24) {
				bypass_logged++;
				say("hakios: %s fix147 lsd anonymous bypass -> XPC error %p (sym %p)\n",
				    prog ? prog : "?", err, sym);
			}
			if (err != NULL) {
				return xpc_retain(err);
			}
			say("hakios: %s fix147 XPC error symbol missing; empty dict\n",
			    prog ? prog : "?");
			return xpc_dictionary_create(NULL, NULL, 0);
		}
		if (bypass) {
			if (bypass_logged < 24) {
				bypass_logged++;
				say("hakios: %s fix138 xpc sync bypass \"%s\" -> empty dict\n",
				    prog ? prog : "?", name ? name : "?");
			}
			return xpc_dictionary_create(NULL, NULL, 0);
		}
	}
	if (prog != NULL &&
	    (strcmp(prog, "runningboardd") == 0 || strcmp(prog, "lsd") == 0) &&
	    daemon_sync_logged < 200) {
		daemon_sync_logged++;
		say("hakios: %s xpc sync send[%u] -> \"%s\"\n",
		    prog, daemon_sync_logged, name ? name : "(anon)");
	}
	if (prog != NULL && strcmp(prog, "lsd") == 0 && name != NULL &&
	    (strcmp(name, "com.apple.containermanagerd") == 0 ||
	     strcmp(name, "com.apple.mobile.usermanagerd.xpc") == 0)) {
		static unsigned lsd_req_logged;
		if (lsd_req_logged < 200) {
			char *desc = xpc_copy_description(message);
			lsd_req_logged++;
			say("hakios: lsd %s request[%u] main=%d: %.600s\n", name,
			    lsd_req_logged, pthread_main_np(), desc != NULL ? desc : "(null)");
			free(desc);
		}
	}
	/* fix157: the anonymous lsd handler asks containermanagerd for the
	 * com.apple.installd class-10 container.  Reuse the real class-10 reply
	 * that lsd already got on the main thread during startup. */
	if (prog != NULL && strcmp(prog, "lsd") == 0 && name != NULL &&
	    strcmp(name, "com.apple.containermanagerd") == 0 && !pthread_main_np()) {
		static unsigned fix157_logged;
		if (lsd_cm_class10 != NULL) {
			if (fix157_logged < 32) {
				fix157_logged++;
				say("hakios: lsd fix157 worker containermanagerd -> cached class10 reply %p\n",
				    lsd_cm_class10);
			}
			return xpc_retain(lsd_cm_class10);
		}
		if (fix157_logged < 32) {
			fix157_logged++;
			say("hakios: lsd fix157 worker containermanagerd -> no cached class10 reply\n");
		}
	}
	{
		xpc_object_t reply = xpc_connection_send_message_with_reply_sync(connection, message);

		if (prog != NULL && strcmp(prog, "lsd") == 0 && daemon_sync_logged < 200) {
			void *type = reply != NULL ? xpc_get_type(reply) : NULL;

			say("hakios: lsd sync reply[%u] -> %s\n", daemon_sync_logged,
			    type != NULL ? xpc_type_get_name(type) : "null");
			if (name != NULL &&
			    (strcmp(name, "com.apple.containermanagerd") == 0 ||
			     strcmp(name, "com.apple.mobile.usermanagerd.xpc") == 0)) {
				static unsigned lsd_rep_logged;
				if (lsd_rep_logged < 40 && reply != NULL) {
					char *desc = xpc_copy_description(reply);
					lsd_rep_logged++;
					say("hakios: lsd %s reply[%u]: %.600s\n", name, lsd_rep_logged,
					    desc != NULL ? desc : "(null)");
					free(desc);
				}
			}
		}
		if (prog != NULL && strcmp(prog, "lsd") == 0 && name != NULL &&
		    strcmp(name, "com.apple.containermanagerd") == 0 && reply != NULL &&
		    xpc_get_type(reply) != NULL &&
		    strcmp(xpc_type_get_name(xpc_get_type(reply)), "dictionary") == 0 &&
		    xpc_dictionary_get_uint64(message, "ContainerClass") == 10) {
			if (lsd_cm_class10 != NULL)
				xpc_release(lsd_cm_class10);
			lsd_cm_class10 = xpc_retain(reply);
			say("hakios: lsd fix157 cached class10 containermanagerd reply %p\n",
			    lsd_cm_class10);
		}
		return reply;
	}
}

static void my_xpc_connection_resume(xpc_connection_t connection)
{
	const char *prog = getprogname();
	if (prog != NULL && strcmp(prog, "runningboardd") == 0 && connection != NULL) {
		static unsigned rbd_resume_logged;
		if (rbd_resume_logged < 32) {
			const char *name = xpc_connection_get_name(connection);
			rbd_resume_logged++;
			say("hakios: rbd xpc_connection_resume[%u] conn=%p name=\"%s\"\n",
			    rbd_resume_logged, connection, name ? name : "(anon/peer)");
		}
	}
	xpc_connection_resume(connection);
}

static void my_xpc_connection_activate(xpc_connection_t connection)
{
	const char *prog = getprogname();
	if (prog != NULL && strcmp(prog, "runningboardd") == 0 && connection != NULL) {
		static unsigned rbd_activate_logged;
		if (rbd_activate_logged < 32) {
			const char *name = xpc_connection_get_name(connection);
			rbd_activate_logged++;
			say("hakios: rbd xpc_connection_activate[%u] conn=%p name=\"%s\"\n",
			    rbd_activate_logged, connection, name ? name : "(anon/peer)");
		}
	}
	xpc_connection_activate(connection);
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} hakios_interposes[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)my_CoreAccessoriesInitForHID, (const void *)CoreAccessoriesInitForHID },
	{ (const void *)my_xpc_connection_create_mach_service, (const void *)xpc_connection_create_mach_service },
	{ (const void *)my_xpc_connection_send_message_with_reply_sync, (const void *)xpc_connection_send_message_with_reply_sync },
	{ (const void *)my_xpc_connection_resume, (const void *)xpc_connection_resume },
	{ (const void *)my_xpc_connection_activate, (const void *)xpc_connection_activate },
};

static void
hook_hid_method(Class cls, const char *name, IMP replacement, void *original)
{
	Method m = class_getInstanceMethod(cls, sel_registerName(name));

	if (m == NULL) {
		say("hakios: -[%s %s]: no such method; left alone\n", class_getName(cls), name);
		return;
	}
	if (original) {
		*(IMP *)original = method_setImplementation(m, replacement);
	} else {
		method_setImplementation(m, replacement);
	}
}

static void
hook_class_method(Class cls, const char *name, IMP replacement, void *original)
{
	Method m = class_getClassMethod(cls, sel_registerName(name));
	if (m == NULL) {
		return;
	}
	if (original) {
		*(IMP *)original = method_setImplementation(m, replacement);
	} else {
		method_setImplementation(m, replacement);
	}
}

static BOOL my_deviceAvailableWithFailure(id self, SEL _cmd, id failure) {
	say("hakios: BKDevice deviceAvailableWithFailure -> fast NO\n");
	return NO;
}

static id my_availableDevices(id self, SEL _cmd) {
	say("hakios: BKDevice availableDevices -> fast []\n");
	Class clsArray = objc_getClass("NSArray");
	if (clsArray) {
		return ((id (*)(id, SEL))objc_msgSend)((id)clsArray, sel_registerName("array"));
	}
	return nil;
}

static id my_nil_init_stub(id self, SEL _cmd) {
	say("hakios: biometric init stub -> fast nil\n");
	return nil;
}

static void
biometric_bypass_fixes(void)
{
	const char *classes[] = { "BKDevice", "BKDeviceTouchID", "BKDevicePearl", "BiometricKit", NULL };
	for (int i = 0; classes[i]; i++) {
		Class cls = objc_getClass(classes[i]);
		if (cls) {
			hook_class_method(cls, "deviceAvailableWithFailure:", (IMP)my_deviceAvailableWithFailure, NULL);
			hook_class_method(cls, "availableDevices", (IMP)my_availableDevices, NULL);
			hook_class_method(cls, "isAvailable", (IMP)my_deviceAvailableWithFailure, NULL);
			say("hakios: Biometric class %s fast-bypassed\n", classes[i]);
		}
	}

	Class clsXPC = objc_getClass("BiometricKitXPCClient");
	if (clsXPC) {
		hook_class_method(clsXPC, "sharedInstance", (IMP)my_nil_init_stub, NULL);
		hook_hid_method(clsXPC, "init", (IMP)my_nil_init_stub, NULL);
		say("hakios: BiometricKitXPCClient fast-bypassed\n");
	}

	Class clsPearl = objc_getClass("PearlEventFilter");
	if (clsPearl) {
		hook_hid_method(clsPearl, "init", (IMP)my_nil_init_stub, NULL);
		say("hakios: PearlEventFilter fast-bypassed\n");
	}
}

static void
hid_system_fixes(void)
{
	biometric_bypass_fixes();
	Class cls = objc_getClass("BKHIDSystemInterface");
	if (cls) {
		say("hakios: BKHIDSystemInterface found (%p), super=%s\n", (void *)cls,
		    class_getName(class_getSuperclass(cls)));

		Method m = class_getClassMethod(cls, sel_registerName("sharedInstance"));
		if (m) {
			IMP imp = method_getImplementation(m);
			say("hakios: +[BKHIDSystemInterface sharedInstance] imp=%p (unslid: %#lx)\n",
			    (void *)imp, (uintptr_t)imp - slide);
			orig_BKHIDSystemInterface_sharedInstance = (id (*)(id, SEL))imp;
			method_setImplementation(m, (IMP)my_BKHIDSystemInterface_sharedInstance);
		}
		Method im = class_getInstanceMethod(cls, sel_registerName("init"));
		if (im) {
			IMP imp = method_getImplementation(im);
			say("hakios: -[BKHIDSystemInterface init] imp=%p (unslid: %#lx)\n",
			    (void *)imp, (uintptr_t)imp - slide);
			orig_BKHIDSystemInterface_init = (id (*)(id, SEL))imp;
			method_setImplementation(im, (IMP)my_BKHIDSystemInterface_init);
		}

		unsigned int mcount = 0;
		Method *mlist = class_copyMethodList(cls, &mcount);
		for (unsigned int i = 0; i < mcount; i++) {
			say("hakios: BKHIDSystemInterface method: %s\n", sel_getName(method_getName(mlist[i])));
		}
		free(mlist);

		/*
		 * -startHIDSystem must be SKIPPED on bare-metal MSM8952 (no IOHIDSystem kext).
		 * Virtual event system Mach RPC is avoided in favor of direct in-process injection.
		 */
		hid_bypass_start = 1;
		hook_hid_method(cls, "startHIDSystem", (IMP)my_hid_startHIDSystem, &orig_hid_startHIDSystem);
		hook_hid_method(cls, "startEventRouting", (IMP)my_hid_startEventRouting, &orig_hid_startEventRouting);
		hook_hid_method(cls, "startEventProcessing", (IMP)my_hid_startEventProcessing,
		    &orig_hid_startEventProcessing);
		hook_hid_method(cls, "startServer", (IMP)my_hid_startServer, &orig_hid_startServer);
		hook_hid_method(cls, "registerWatchdog", (IMP)my_hid_registerWatchdog, &orig_hid_registerWatchdog);
		hook_hid_method(cls, "deliveryManager", (IMP)my_hid_deliveryManager, &orig_hid_deliveryManager);
		hook_hid_method(cls, "senderCache", (IMP)my_hid_senderCache, &orig_hid_senderCache);
		hook_hid_method(cls, "clientConnectionManager", (IMP)my_hid_clientConnectionManager, &orig_hid_clientConnectionManager);
		say("hakios: HID start steps hooked (startHIDSystem SKIPPED for bare-metal MSM8952)\n");
	} else {
		say("hakios: BKHIDSystemInterface class not found\n");
	}
}

/* ---------------------------------------------------------------------------------------------- */

/*
 * An OpenGL ES implementation for CoreAnimation (src/eagl.c, Mesa behind it): loaded, it puts
 * itself behind EAGLContext, and the renderer the server then gets is the OpenGL ES one. That
 * is worth it where a GPU does the drawing, and only there: on the phone, when the kernel has
 * started the GPU and keeps it (boot-arg gpu=1), which the service HakiosGPU says (it is there
 * before the first program runs, or not at all). Everywhere else -- QEMU, a run without the
 * boot-arg, a phone without the GPU's firmware in the image, a GPU that did not start --
 * nothing is loaded, and CoreAnimation draws with its own software renderer as in an image
 * without the library.
 * Boot-arg gl=0: not even with a GPU; gl=1: also without one, by Mesa's software driver (slow;
 * for working on eagl.c in QEMU).
 */
static void
opengl_es(void)
{
	/* MSM8952 has Adreno 405 (no Metal/EAGL kernel driver).
	 * CoreAnimation must use its built-in CPU Software Renderer (sw_new_context). */
}

/* ---- fix107/fix108: main-thread samplers (backboardd, logd) ------------------------------------- */

/*
 * fix107 found backboardd's main thread blocked, for tens of seconds, in a Mach send made by
 * libsystem_trace: BackBoardServices' os_log_create("com.apple.BackBoard", "Common") registers the
 * log object with logd, whose receive queue is not drained.  While the main thread waits there,
 * the display-services MIG server it also runs never answers SpringBoard (msgh_id 0x5b9171) and
 * launchd's guard kills SpringBoard.  fix108 samples both sides: backboardd's main thread (the
 * blocked send, the message itself, the remote port's kobject, the thread list) and logd's
 * (where it is instead of draining).  A line is written once: stderr already is the console.
 */
static volatile unsigned bbd_mainq_beats, logd_mainq_beats;

typedef struct {
	uint64_t x[29];
	uint64_t fp;
	uint64_t lr;
	uint64_t sp;
	uint64_t pc;
	uint32_t cpsr;
	uint32_t flags;
} watch_thread_state64_t;

struct watch {
	const char *who;                        /* "bbd" or "logd" */
	mach_port_t thread;
	volatile unsigned *beats;
	unsigned dumps;
	uint64_t tid;                           /* thread_id, to tell a dead thread from a dead name */
	unsigned dump_every;                    /* seconds between all-thread dumps, 0 = never */
	unsigned scan_every;                    /* seconds between port scans, 0 = never */
};

static struct watch bbd_watch = { "bbd", MACH_PORT_NULL, &bbd_mainq_beats, 0, 0, 10, 10 };
static struct watch logd_watch = { "logd", MACH_PORT_NULL, &logd_mainq_beats, 0, 0, 0, 0 };

/* fix132: SpringBoard's sync "handshakeWithRequest:" to com.apple.runningboard never gets a
 * reply; runningboardd (pid 6) is alive and initialized, so its threads are where the answer is. */
static unsigned rbd_mainq_beats;
static struct watch rbd_watch = { "rbd", MACH_PORT_NULL, &rbd_mainq_beats, 0, 0, 10, 10 };
static unsigned lsd_mainq_beats;
static struct watch lsd_watch = { "lsd", MACH_PORT_NULL, &lsd_mainq_beats, 0, 0, 10, 10 };

static void say_once(const char *format, ...);

/*
 * fix110 candidate: does backboardd check "com.apple.backboard.display.services" in at all?
 * If it does not, SpringBoard's message waits in launchd's dead-name queue and no thread of
 * backboardd can answer it, however healthy the rest of the process looks.  Interposing the two
 * bootstrap calls turns that question into a log line, and the port it names gets its own queue
 * report in watch_scan_ports().
 */
extern kern_return_t bootstrap_check_in(mach_port_t bp, const char *service_name,
    mach_port_t *service_port);
extern kern_return_t bootstrap_register(mach_port_t bp, const char *service_name,
    mach_port_t service_port);
extern kern_return_t bootstrap_look_up(mach_port_t bp, const char *service_name,
    mach_port_t *service_port);
extern mach_port_t mach_reply_port(void);

static mach_port_t bbd_service_port;

typedef kern_return_t (*bootstrap_check_in_fn)(mach_port_t, const char *, mach_port_t *);
typedef kern_return_t (*bootstrap_register_fn)(mach_port_t, const char *, mach_port_t);

static kern_return_t
hakios_bootstrap_check_in(mach_port_t bp, const char *service_name, mach_port_t *service_port)
{
	kern_return_t kr;

	kr = bootstrap_check_in(bp, service_name, service_port);
	say_once("hakios: %s check-in \"%s\" kr %d port %#x\n", getprogname(), service_name, kr,
	    kr == KERN_SUCCESS ? *service_port : 0);
	if (kr == KERN_SUCCESS && service_name != NULL &&
	    strcmp(service_name, "com.apple.backboard.display.services") == 0) {
		bbd_service_port = *service_port;
	}
	return kr;
}

static kern_return_t
hakios_bootstrap_register(mach_port_t bp, const char *service_name, mach_port_t service_port)
{
	kern_return_t kr;

	kr = bootstrap_register(bp, service_name, service_port);
	say_once("hakios: %s register \"%s\" port %#x kr %d\n", getprogname(), service_name,
	    service_port, kr);
	if (kr == KERN_SUCCESS && service_name != NULL &&
	    strcmp(service_name, "com.apple.backboard.display.services") == 0) {
		bbd_service_port = service_port;
	}
	return kr;
}

__attribute__((used)) static struct {
	const void *replacement;
	const void *replacee;
} hakios_bootstrap_interposers[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)hakios_bootstrap_check_in, (const void *)bootstrap_check_in },
	{ (const void *)hakios_bootstrap_register, (const void *)bootstrap_register },
};

static double
watch_since_boot(void)
{
	mach_timebase_info_data_t tb;

	mach_timebase_info(&tb);
	return (double)mach_absolute_time() * (double)tb.numer / (double)tb.denom / 1e9;
}

/* one copy of a line: for these daemons stderr already is the console (say() writes both) */
static void
say_once(const char *format, ...)
{
	char line[1280];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
	if (!isatty(2)) {
		int cfd = open("/dev/console", O_WRONLY);

		if (cfd >= 0) {
			write(cfd, line, (size_t)n);
			close(cfd);
		}
	}
}

static void
watch_say_pc(const char *who, const char *what, uintptr_t pc)
{
	Dl_info info;
	const char *name = "?";
	uintptr_t base = 0;

	if (pc != 0 && dladdr((void *)pc, &info) != 0) {
		const char *slash = strrchr(info.dli_fname, '/');

		name = slash ? slash + 1 : info.dli_fname;
		base = (uintptr_t)info.dli_fbase;
	}
	say_once("hakios: %s %.1f %s %#lx = %s+%#lx (cache %#lx)\n", who, watch_since_boot(), what, pc,
	    name, pc - base, pc - slide);
}

static void
watch_say_chain(const char *who, uintptr_t fp)
{
	for (unsigned frame = 0; frame < 12 && fp >= 0x100000000UL && (fp & 7) == 0; frame++) {
		uintptr_t pair[2];
		vm_size_t got = 0;

		if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof(pair), (vm_address_t)pair,
		    &got) != KERN_SUCCESS || got < sizeof(pair)) {
			break;
		}
		if (pair[1] != 0) {
			watch_say_pc(who, "chain", pair[1] & 0x0000000fffffffffUL);
		}
		if (pair[0] <= fp) {
			break;
		}
		fp = pair[0];
	}
}

static void
watch_say_strings(const char *who, const unsigned char *buf, unsigned len)
{
	char run[65];
	unsigned n = 0;
	unsigned said = 0;

	for (unsigned i = 0; i <= len; i++) {
		unsigned char c = i < len ? buf[i] : 0;

		if (c >= 0x20 && c < 0x7f) {
			if (n < sizeof(run) - 1) {
				run[n++] = (char)c;
			}
			continue;
		}
		if (n >= 4 && said < 8) {
			run[n] = 0;
			say_once("hakios: %s msg str: %s\n", who, run);
			said++;
		}
		n = 0;
	}
}

static void
watch_probe_port(const char *who, mach_port_name_t name)
{
	mach_port_type_t type = 0;
	natural_t object_type = 0;
	mach_vm_address_t object_addr = 0;
	kobject_description_t description;

	mach_port_type(mach_task_self(), name, &type);
	memset(description, 0, sizeof(description));
	if (mach_port_kobject_description(mach_task_self(), name, &object_type, &object_addr,
	    description) == KERN_SUCCESS) {
		say_once("hakios: %s port %#x type %#x kobject %#x addr %#llx says \"%s\"\n", who, name, type,
		    object_type, (unsigned long long)object_addr, description);
	} else {
		say_once("hakios: %s port %#x type %#x: no kobject description\n", who, name, type);
	}
}

/*
 * The blocked message: header, first bytes, printable runs, then each descriptor.  OOL memory is
 * read and scanned, OOL ports and port rights get their kobject description; the remote port of
 * the header is named too -- the side that stopped draining is what all of this says.
 */
static void
watch_dump_msg(const char *who, const watch_thread_state64_t *st)
{
	uint64_t option = st->x[1], bits_ss = st->x[2], ports = st->x[3];
	uint64_t vou_id = st->x[4], dc_rn = st->x[5], rsize_pri = st->x[6];
	uint32_t bits = (uint32_t)bits_ss, ssize = (uint32_t)(bits_ss >> 32);
	uint32_t remote = (uint32_t)ports, local = (uint32_t)(ports >> 32);
	uint32_t voucher = (uint32_t)vou_id, id = (uint32_t)(vou_id >> 32);
	uint32_t dcount = (uint32_t)dc_rn, rname = (uint32_t)(dc_rn >> 32);
	uint32_t rsize = (uint32_t)rsize_pri;
	unsigned char buf[0x300];
	vm_size_t got = 0;
	unsigned want = ssize > sizeof(buf) ? sizeof(buf) : ssize;

	say_once("hakios: %s msg opt %#llx bits %#x ssize %u remote %#x local %#x voucher %#x id %#x "
	    "dcnt %u rname %#x rsize %u timeout %#llx\n", who, (unsigned long long)option, bits, ssize,
	    remote, local, voucher, id, dcount, rname, rsize, (unsigned long long)st->x[7]);
	if (want == 0 || st->x[0] < 0x100000000UL ||
	    vm_read_overwrite(mach_task_self(), (vm_address_t)st->x[0], want, (vm_address_t)buf,
	    &got) != KERN_SUCCESS || got < 8) {
		return;
	}
	for (unsigned off = 0; off + 16 <= got && off < 64; off += 16) {
		say_once("hakios: %s msg hex %08x %08x %08x %08x\n", who,
		    *(uint32_t *)(buf + off), *(uint32_t *)(buf + off + 4),
		    *(uint32_t *)(buf + off + 8), *(uint32_t *)(buf + off + 12));
	}
	watch_say_strings(who, buf, (unsigned)got);
	for (unsigned d = 0; d < dcount && d < 8; d++) {
		unsigned off = 24 + d * 16;
		unsigned char *p;
		unsigned type;

		if (off + 16 > got) {
			break;
		}
		p = buf + off;
		type = p[11];
		say_once("hakios: %s desc %u type %u words %08x %08x %08x %08x\n", who, d, type,
		    *(uint32_t *)(p + 0), *(uint32_t *)(p + 4), *(uint32_t *)(p + 8),
		    *(uint32_t *)(p + 12));
		if (type == 1 /* OOL memory */) {
			uint64_t address = *(uint64_t *)p;
			uint32_t size = *(uint32_t *)(p + 12);
			unsigned char ool[256];
			vm_size_t ool_got = 0;
			unsigned ool_want = size > sizeof(ool) ? sizeof(ool) : size;

			if (address >= 0x100000000UL && ool_want > 0 &&
			    vm_read_overwrite(mach_task_self(), (vm_address_t)address, ool_want,
			    (vm_address_t)ool, &ool_got) == KERN_SUCCESS) {
				watch_say_strings(who, ool, (unsigned)ool_got);
			}
		} else if (type == 2 /* OOL ports */) {
			uint64_t address = *(uint64_t *)p;
			uint32_t count = *(uint32_t *)(p + 12);
			mach_port_name_t names[8];
			vm_size_t names_got = 0;
			unsigned names_want = count > 8 ? 8 : count;

			if (address >= 0x100000000UL && names_want > 0 &&
			    vm_read_overwrite(mach_task_self(), (vm_address_t)address,
			    names_want * sizeof(mach_port_name_t), (vm_address_t)names,
			    &names_got) == KERN_SUCCESS) {
				for (unsigned k = 0; k < names_got / sizeof(mach_port_name_t); k++) {
					watch_probe_port(who, names[k]);
				}
			}
		} else if (type == 0 /* port right */) {
			watch_probe_port(who, *(uint32_t *)p);
		}
	}
	watch_probe_port(who, remote);
}

#define WATCH_TRAP_MACH_MSG2 0x1c1852ff0UL

/*
 * fix113: the probe moves into backboardd and gets a loopback in front of it.
 *
 * fix112 proved two things: the endpoint is healthy (the fire-and-forget send returned
 * MACH_MSG_SUCCESS) and every guard named the reply port that had just been allocated as a
 * MACH_PORT_RIGHT_RECEIVE (mach_port_type 0x20000, urefs 1).  The one difference left between
 * the probe and every working send in the same log -- SpringBoard's own RPC and backboardd's
 * threads -- is the call shape: they send and receive in one mach_msg (MACH_SEND_MSG|MACH_RCV_MSG,
 * rcv_name == msgh_local_port); the probe sent with a reply port and rcv_name MACH_PORT_NULL.
 *
 * The loopback runs the whole exchange against our own ports first, with step 1 in exactly the
 * display request's shape.  A fault there is ours; a fault only at the service is the endpoint's.
 * Either way only backboardd dies, never SpringBoard.
 *
 * fix114: fix113 showed that the call shape makes no difference -- the combined send died with
 * the same guard naming the reply port.  The source names the culprit: the display service is
 * created with MPO_ENFORCE_REPLY_PORT_SEMANTICS (ipc_port.c:771, and the CA telemetry comment in
 * ipc_kmsg.c), so ipc_right_copyin_check_reply() rejects a reply port that is not a *reply port*
 * (ipc_right.c:2069).  mach_port_allocate(RECEIVE) produces a plain receive right.  The kernel
 * marks a port as a reply port only when it is created with IPC_PORT_INIT_REPLY -- mach_reply_port.
 */
extern mach_port_name_t mach_reply_port(void);

/*
 * fix115: a port the kernel will recognise as a reply port.  mach_reply_port() does not: its
 * trap allocates with IPC_PORT_INIT_MESSAGE_QUEUE only (ipc_tt.c:1424), so the port carries no
 * reply marking and the display endpoint's enforced reply-port semantics rejects it.  Only
 * MPO_REPLY_PORT (ipc_port.c:760) runs ip_mark_reply_port / ip_mark_provisional_reply_port.
 */
static mach_port_t
watch_make_reply_port(void)
{
	mach_port_options_t options;
	mach_port_t name = MACH_PORT_NULL;

	memset(&options, 0, sizeof(options));
	options.flags = MPO_REPLY_PORT;
	if (mach_port_construct(mach_task_self(), &options, 0, &name) != KERN_SUCCESS) {
		return MACH_PORT_NULL;
	}
	return name;
}

/* fix115: a local stand-in for the display endpoint: a port that enforces reply-port semantics,
 * plus a send right so the probe can send to it the way it sends to the service (COPY_SEND). */
static mach_port_t
watch_make_enforced_port(void)
{
	mach_port_options_t options;
	mach_port_t name = MACH_PORT_NULL;

	memset(&options, 0, sizeof(options));
	options.flags = MPO_ENFORCE_REPLY_PORT_SEMANTICS | MPO_INSERT_SEND_RIGHT;
	if (mach_port_construct(mach_task_self(), &options, 0, &name) != KERN_SUCCESS) {
		return MACH_PORT_NULL;
	}
	return name;
}

/*
 * fix116: mach_msg with MACH_SEND_MSG|MACH_RCV_MSG receives into the same address it sends from.
 * fix115 passed a 24-byte request with rcv_size 128, so the service's answer was written past
 * the request and tripped the stack canary; the SIGABRT right after "combined kr 0" was ours,
 * not the service's.  One buffer now carries both directions.
 *
 * watch_loopback runs one full exchange against a port of our own in the display request's
 * shape.  mbits is the full header bits: 0x1514 for a bare receive right (MAKE_SEND is the only
 * legal destination disposition there), 0x1513 for a port that also holds a send right (the
 * service case, COPY_SEND).  A timeout on the first receive is the healthy answer: the send
 * half passed the reply check and this single thread is the server that has not replied yet.
 */
static int
watch_loopback(mach_port_t dest, uint32_t mbits, const char *tag)
{
	char buffer[384];
	mach_msg_header_t *header = (mach_msg_header_t *)buffer;
	mach_port_t reply = watch_make_reply_port();
	mach_port_t from;
	kern_return_t kr;

	if (reply == MACH_PORT_NULL) {
		say_once("hakios: bbd %s t=%.1f reply port alloc failed\n", tag, watch_since_boot());
		return -1;
	}
	memset(buffer, 0, sizeof(buffer));
	*(uint32_t *)(buffer + 0) = mbits;
	*(uint32_t *)(buffer + 4) = 24;
	*(uint32_t *)(buffer + 8) = dest;
	*(uint32_t *)(buffer + 12) = reply;
	*(uint32_t *)(buffer + 20) = 0x5b9171;
	kr = mach_msg(header, MACH_SEND_MSG | MACH_RCV_MSG | MACH_SEND_TIMEOUT | MACH_RCV_TIMEOUT,
	    24, sizeof(buffer), reply, 200, MACH_PORT_NULL);
	say_once("hakios: bbd %s t=%.1f send kr %d (%s)\n", tag, watch_since_boot(), kr,
	    mach_error_string(kr));
	if (kr != MACH_RCV_TIMED_OUT && kr != KERN_SUCCESS) {
		return -2;
	}
	memset(buffer, 0, sizeof(buffer));
	kr = mach_msg(header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(buffer), dest, 200,
	    MACH_PORT_NULL);
	say_once("hakios: bbd %s t=%.1f dest rcv kr %d (%s)\n", tag, watch_since_boot(), kr,
	    mach_error_string(kr));
	if (kr != KERN_SUCCESS) {
		return -3;
	}
	from = header->msgh_remote_port;
	say_once("hakios: bbd %s t=%.1f got bits %#x size %u remote %#x local %#x id %#x\n", tag,
	    watch_since_boot(), header->msgh_bits, header->msgh_size, from,
	    header->msgh_local_port, header->msgh_id);
	memset(buffer, 0, sizeof(buffer));
	*(uint32_t *)(buffer + 0) = 0x00000012;   /* MOVE_SEND_ONCE to the reply port */
	*(uint32_t *)(buffer + 4) = 24;
	*(uint32_t *)(buffer + 8) = from;
	*(uint32_t *)(buffer + 20) = 0x5b91d5;
	kr = mach_msg(header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, 24, 0, MACH_PORT_NULL, 200,
	    MACH_PORT_NULL);
	say_once("hakios: bbd %s t=%.1f reply send kr %d (%s)\n", tag, watch_since_boot(), kr,
	    mach_error_string(kr));
	if (kr != KERN_SUCCESS) {
		return -4;
	}
	memset(buffer, 0, sizeof(buffer));
	kr = mach_msg(header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(buffer), reply, 200,
	    MACH_PORT_NULL);
	say_once("hakios: bbd %s t=%.1f reply rcv kr %d (%s)\n", tag, watch_since_boot(), kr,
	    mach_error_string(kr));
	if (kr != KERN_SUCCESS) {
		return -5;
	}
	say_once("hakios: bbd %s t=%.1f REPLY bits %#x size %u id %#x\n", tag, watch_since_boot(),
	    header->msgh_bits, header->msgh_size, header->msgh_id);
	return 0;
}

static void *
watch_service_probe(__unused void *argument)
{
	static const char service[] = "com.apple.backboard.display.services";
	mach_port_t bootstrap = MACH_PORT_NULL, server = MACH_PORT_NULL;
	mach_port_t inbox = MACH_PORT_NULL, reply = MACH_PORT_NULL;
	char buffer[384];
	mach_msg_header_t *header = (mach_msg_header_t *)buffer;
	kern_return_t kr;

	sleep(25);

	/* fix113: one-shot gate.  A fault in our own loopback would kill backboardd again and
	 * again; the marker keeps a bad recipe from turning into a respawn storm. */
	{
		int fd = open("/tmp/hakios-bbd-probe-done", O_CREAT | O_EXCL | O_WRONLY, 0644);

		if (fd < 0) {
			say_once("hakios: bbd probe t=%.1f mark errno %d (%s), skipping\n",
			    watch_since_boot(), errno, strerror(errno));
			if (errno == EEXIST) {
				return NULL;
			}
		} else {
			close(fd);
			say_once("hakios: bbd probe t=%.1f mark created, running\n", watch_since_boot());
		}
	}

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &inbox);
	if (kr != KERN_SUCCESS) {
		say_once("hakios: bbd loop t=%.1f inbox alloc kr %d\n", watch_since_boot(), kr);
		return NULL;
	}
	{
		mach_port_t enforced = watch_make_enforced_port();
		mach_port_type_t type = 0;
		mach_port_urefs_t refs = 0;
		int plain, strict;

		if (enforced != MACH_PORT_NULL) {
			mach_port_type(mach_task_self(), enforced, &type);
			mach_port_get_refs(mach_task_self(), enforced, MACH_PORT_RIGHT_SEND, &refs);
		}
		say_once("hakios: bbd loop t=%.1f inbox %#x enforced %#x type %#x send-urefs %u\n",
		    watch_since_boot(), inbox, enforced, type, refs);

		/* A: bare receive right, MAKE_SEND. */
		plain = watch_loopback(inbox, 0x00001514, "loop");
		/* B: the display endpoint's rule, locally: an enforcing destination with a send
		 * right, COPY_SEND as on the wire, and a constructed reply port. */
		strict = enforced != MACH_PORT_NULL ?
		    watch_loopback(enforced, 0x00001513, "enf") : -9;
		say_once("hakios: bbd loop t=%.1f plain %d strict %d\n", watch_since_boot(),
		    plain, strict);
	}
	mach_port_mod_refs(mach_task_self(), inbox, MACH_PORT_RIGHT_RECEIVE, -1);

	/* step 5: the real service, in the shape the loopback just proved. */
	task_get_bootstrap_port(mach_task_self(), &bootstrap);
	kr = bootstrap_look_up(bootstrap, service, &server);
	say_once("hakios: bbd svc probe t=%.1f look up kr %d port %#x\n", watch_since_boot(), kr,
	    server);
	if (kr != KERN_SUCCESS) {
		return NULL;
	}
	/* 5a: no reply port at all, so the reply-port check is not entered. */
	memset(buffer, 0, sizeof(buffer));
	*(uint32_t *)(buffer + 0) = 0x00000013;
	*(uint32_t *)(buffer + 4) = 24;
	*(uint32_t *)(buffer + 8) = server;
	*(uint32_t *)(buffer + 20) = 0x5b9171;
	kr = mach_msg(header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, 24, 0, MACH_PORT_NULL, 200,
	    MACH_PORT_NULL);
	say_once("hakios: bbd svc probe t=%.1f ff send kr %d (%s)\n", watch_since_boot(), kr,
	    mach_error_string(kr));

	/* 5b: the real request, with the reply port the endpoint insists on. */
	reply = watch_make_reply_port();
	{
		mach_port_type_t type = 0;
		mach_port_urefs_t refs = 0;

		mach_port_type(mach_task_self(), reply, &type);
		mach_port_get_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, &refs);
		say_once("hakios: bbd svc probe t=%.1f reply %#x (MPO_REPLY_PORT) type %#x urefs %u\n",
		    watch_since_boot(), reply, type, refs);
	}
	if (reply == MACH_PORT_NULL) {
		mach_port_deallocate(mach_task_self(), server);
		return NULL;
	}
	memset(buffer, 0, sizeof(buffer));
	*(uint32_t *)(buffer + 0) = 0x00001513;
	*(uint32_t *)(buffer + 4) = 24;
	*(uint32_t *)(buffer + 8) = server;
	*(uint32_t *)(buffer + 12) = reply;
	*(uint32_t *)(buffer + 20) = 0x5b9171;
	say_once("hakios: bbd svc probe t=%.1f hdr bits 0x1513 remote %#x local %#x id 0x5b9171\n",
	    watch_since_boot(), server, reply);
	kr = mach_msg(header,
	    MACH_SEND_MSG | MACH_RCV_MSG | MACH_SEND_TIMEOUT | MACH_RCV_TIMEOUT,
	    24, sizeof(buffer), reply, 300, MACH_PORT_NULL);
	say_once("hakios: bbd svc probe t=%.1f combined kr %d (%s)\n", watch_since_boot(), kr,
	    mach_error_string(kr));
	if (kr == KERN_SUCCESS) {
		unsigned size = header->msgh_size;
		unsigned words = (size <= sizeof(buffer)) ? (size + 3) / 4 : 0;

		say_once("hakios: bbd svc probe t=%.1f reply bits %#x size %u remote %#x local %#x id %#x\n",
		    watch_since_boot(), header->msgh_bits, size, header->msgh_remote_port,
		    header->msgh_local_port, header->msgh_id);
		if (words > 6) {
			unsigned i;

			for (i = 6; i < words && i < 18; i += 4) {
				say_once("hakios: bbd svc probe body %u %#x %#x %#x %#x\n", i,
				    *(uint32_t *)(buffer + i * 4),
				    *(uint32_t *)(buffer + i * 4 + 4),
				    *(uint32_t *)(buffer + i * 4 + 8),
				    *(uint32_t *)(buffer + i * 4 + 12));
			}
		}
	} else if (kr == MACH_RCV_TIMED_OUT) {
		say_once("hakios: bbd svc probe t=%.1f sent, no reply (endpoint silent)\n",
		    watch_since_boot());
	}
	mach_port_deallocate(mach_task_self(), server);

	/* fix123: drive the swap pipeline by hand from the one thread we know runs.
	 * CoreAnimation has never called kFBMethodSwapBegin (4) or kFBMethodSwapEnd (5), so the
	 * kernel presenter thread has slept since boot.  Open the MiniIOMobileFramebuffer user
	 * client, begin a swap, hand the driver the 0x46c-byte request, end it.
	 * fix124: that request now names a real surface -- IOSurfaceCreate through the stock
	 * framework (surf_test has proven it against MiniIOSurface), painted with four colour
	 * quadrants and a white bar that moves one per swap.  Something changing on the panel
	 * means the pixels travelled surface -> present() -> show() -> pushPanel for real. */
	{
		const char *fb_names[] = { "MiniIOMobileFramebuffer", "IOMobileFramebuffer",
		    "AppleCLCD", NULL };
		io_service_t fbservice = MACH_PORT_NULL;
		io_connect_t fbconn = MACH_PORT_NULL;
		kern_return_t fbkr;
		int chosen = -1;

		for (int i = 0; fb_names[i] != NULL; i++) {
			fbservice = IOServiceGetMatchingService(kIOMasterPortDefault,
			    IOServiceMatching(fb_names[i]));
			if (fbservice != MACH_PORT_NULL) {
				chosen = i;
				break;
			}
		}
		if (chosen < 0) {
			say_once("hakios: fix123 no framebuffer service in IORegistry\n");
			return NULL;
		}
		say_once("hakios: fix123 matched '%s' (service %#x)\n", fb_names[chosen], fbservice);
		fbkr = IOServiceOpen(fbservice, mach_task_self(), 0, &fbconn);
		if (fbkr != KERN_SUCCESS) {
			say_once("hakios: fix123 IOServiceOpen kr %#x\n", fbkr);
			return NULL;
		}
		{
			uint64_t swap_id = 0;
			uint32_t out_count = 1;
			uint8_t *swap = calloc(1, 0x46c);
			IOSurfaceRef surf = NULL;
			uint32_t surf_sid = 0;
			CFMutableDictionaryRef props;
			CFNumberRef n;

			/* the driver's own default surface: selector 3 hands out its real id (fix66),
			 * which says MiniIOSurface is answering -- worth a line before we make our own */
			{
				uint64_t def_sid = 0;
				uint32_t def_count = 1;

				fbkr = IOConnectCallMethod(fbconn, 3, NULL, 0, NULL, 0, &def_sid,
				    &def_count, NULL, NULL);
				say_once("hakios: fix124 default surface sid %llu kr %#x\n",
				    (unsigned long long) def_sid, fbkr);
			}
			props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
			    &kCFTypeDictionaryValueCallBacks);
			if (props != NULL) {
				n = CFNumberCreate(NULL, kCFNumberIntType, &(int){ 1125 });
				CFDictionarySetValue(props, kIOSurfaceWidth, n); CFRelease(n);
				n = CFNumberCreate(NULL, kCFNumberIntType, &(int){ 2436 });
				CFDictionarySetValue(props, kIOSurfaceHeight, n); CFRelease(n);
				n = CFNumberCreate(NULL, kCFNumberIntType, &(int){ 'BGRA' });
				CFDictionarySetValue(props, kIOSurfacePixelFormat, n); CFRelease(n);
				n = CFNumberCreate(NULL, kCFNumberIntType, &(int){ 4 });
				CFDictionarySetValue(props, kIOSurfaceBytesPerElement, n); CFRelease(n);
				surf = IOSurfaceCreate(props);
				CFRelease(props);
			}
			if (surf == NULL) {
				say_once("hakios: fix124 IOSurfaceCreate got NULL\n");
			} else {
				surf_sid = (uint32_t) IOSurfaceGetID(surf);
				say_once("hakios: fix124 surface id %u at %p, row %u, %u bytes\n",
				    surf_sid, IOSurfaceGetBaseAddress(surf),
				    IOSurfaceGetBytesPerRow(surf), IOSurfaceGetAllocSize(surf));
			}
			for (int frame = 0; surf != NULL && frame < 6; frame++) {
				uint32_t *pixel;
				uint32_t w = IOSurfaceGetWidth(surf), h = IOSurfaceGetHeight(surf);
				uint32_t row_stride = IOSurfaceGetBytesPerRow(surf) / 4;

				/* four quadrants -- red, green, blue, yellow -- and a white bar that
				 * sits one fifth lower with every swap.  The word is A R G B in memory,
				 * which is what the panel and the recorder both take (fix63). */
				IOSurfaceLock(surf, 0, NULL);
				pixel = (uint32_t *) IOSurfaceGetBaseAddress(surf);
				for (uint32_t y = 0; y < h; y++) {
					uint32_t *row = pixel + (size_t) y * row_stride;
					uint32_t bar = (uint32_t) (frame * h / 6);

					for (uint32_t x = 0; x < w; x++) {
						uint32_t colour;

						if (y < h / 2) {
							colour = x < w / 2 ? 0xffff0000 : 0xff00ff00;
						} else {
							colour = x < w / 2 ? 0xff0000ff : 0xffffff00;
						}
						/* fix164: one quadrant per frame, so each swap is visibly
						 * different on the panel and the test pattern itself names
						 * which frames arrived */
						if (frame == 0 && x >= w / 2 && y < h / 2) {
							colour = 0xff000000;
						} else if (frame == 1 && x < w / 2 && y >= h / 2) {
							colour = 0xff000000;
						} else if (frame == 2 && x >= w / 2 && y >= h / 2) {
							colour = 0xff000000;
						} else if (frame == 3 && x < w / 2 && y < h / 2) {
							colour = 0xff000000;
						} else if (frame == 4 && (x + y) % 64 < 32) {
							colour = 0xff202020;
						} else if (frame == 5) {
							colour = 0xffffffff;
						}
						if (y >= bar && y < bar + h / 30) {
							colour = 0xffffffff;
						}
						row[x] = colour;
					}
				}
				IOSurfaceUnlock(surf, 0, NULL);
				fbkr = IOConnectCallMethod(fbconn, 4, NULL, 0, NULL, 0, &swap_id,
				    &out_count, NULL, NULL);
				if (swap != NULL) {
					*(uint32_t *)(swap + 0x50) = (uint32_t) swap_id;   /* SWAP_ID */
					*(uint32_t *)(swap + 0x54) = surf_sid;   /* SWAP_SURFACE(0) */
					*(uint32_t *)(swap + 0x104) = 1;         /* layer 0 present */
					*(uint32_t *)(swap + 0x64 + 8) = 1125;  /* source w */
					*(uint32_t *)(swap + 0x64 + 12) = 2436; /* source h */
					*(uint32_t *)(swap + 0xa4) = 1;         /* enabled */
					*(uint32_t *)(swap + 0xc4 + 8) = 1125;  /* dest w */
					*(uint32_t *)(swap + 0xc4 + 12) = 2436; /* dest h */
					fbkr = IOConnectCallMethod(fbconn, 5, NULL, 0, swap, 0x46c, NULL,
					    NULL, NULL, NULL);
				}
				say("hakios: fix124 swap %d id %llu sid %u kr %#x\n", frame,
				    (unsigned long long) swap_id, surf_sid, fbkr);
				sleep(2);
			}
			free(swap);
		}
		IOServiceClose(fbconn);
	}
	return NULL;
}

static void
watch_dump_threads(const char *who)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t count = 0;
	unsigned probes = 0;

	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		return;
	}
	say_once("hakios: %s threads %u\n", who, (unsigned)count);
	for (mach_msg_type_number_t i = 0; i < count; i++) {
		watch_thread_state64_t st;
		mach_msg_type_number_t st_count = ARM_THREAD_STATE64_COUNT;

		if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&st,
		    &st_count) == KERN_SUCCESS) {
			uint32_t remote = (uint32_t)st.x[3];
			uint32_t options = (uint32_t)st.x[1];

			say_once("hakios: %s thread %u t=%.1f pc %#lx lr %#lx fp %#lx x0 %#llx "
			    "x1 %#llx x2 %#llx x3 %#llx x4 %#llx x5 %#llx x6 %#llx x7 %#llx\n",
			    who, i, watch_since_boot(), (uintptr_t)st.pc, (uintptr_t)st.lr,
			    (uintptr_t)st.fp, (unsigned long long)st.x[0],
			    (unsigned long long)st.x[1], (unsigned long long)st.x[2],
			    (unsigned long long)st.x[3], (unsigned long long)st.x[4],
			    (unsigned long long)st.x[5], (unsigned long long)st.x[6],
			    (unsigned long long)st.x[7]);
			watch_say_pc(who, "thread", (uintptr_t)st.pc);
			/*
			 * A thread sitting in mach_msg2 with a send option is either passing through or
			 * the stall itself.  The remote port's kobject and the caller chain say which:
			 * two of them per dump is enough to name the services and the code paths.
			 */
			if ((uintptr_t)st.pc - slide == WATCH_TRAP_MACH_MSG2 && (options & 1) &&
			    probes < 2) {
				probes++;
				watch_probe_port(who, remote);
				watch_say_chain(who, (uintptr_t)st.fp);
			}
		}
		mach_port_deallocate(mach_task_self(), threads[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_t));
}

/*
 * Is the main thread still in the task?  fix108's sampler saw thread_get_state fail and read it
 * as logd's main thread dying; the same sample deallocated the send rights task_threads had just
 * handed it, so the name may have gone instead.  The thread_id (THREAD_IDENTIFIER_INFO) survives
 * the port name: if it is still in the list, the thread lives and only our name was stale.
 * -1 means task_threads itself failed and says nothing.
 */
static int
watch_thread_alive(struct watch *w)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t count = 0;
	int alive = 0;

	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		return -1;
	}
	for (mach_msg_type_number_t i = 0; i < count; i++) {
		thread_identifier_info_data_t id;
		mach_msg_type_number_t id_count = THREAD_IDENTIFIER_INFO_COUNT;

		memset(&id, 0, sizeof(id));
		if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &id_count) ==
		    KERN_SUCCESS && id.thread_id == w->tid) {
			/* the thread lives: keep this name, the old one may be what went bad */
			mach_port_mod_refs(mach_task_self(), threads[i], MACH_PORT_RIGHT_SEND, 1);
			w->thread = threads[i];
			alive = 1;
		}
		mach_port_deallocate(mach_task_self(), threads[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_t));
	return alive;
}

/*
 * The receive rights of this task and what is waiting on them.  A Mach port reports its queue,
 * not the sender: a non-zero mps_msgcount is the difference between "the request arrived and
 * nobody dequeued it" (the handler or its queue is stuck) and "the request never arrived" (the
 * service endpoint is still somewhere else, e.g. queued at launchd).  The display-services
 * request from SpringBoard is msgh_id 0x5b9171; if it is sitting in one of these queues, the
 * queue number and name printed here are its location.
 */
/*
 * fix135: fix133/fix134 found the runningboardd handshake request enqueued on one of its ports
 * and never received, while the main thread waits on port set 0x903.  Two questions decide who
 * is wrong: is the queued port a member of a port set (then the kernel's port-set receive never
 * sees it), and does a one-message port/port-set round trip work in this process at all?  The
 * first is asked here, the second by watch_portset_selftest().
 */
static void
watch_portset_membership(const char *who, mach_port_name_t queued, mach_port_name_t *names,
    mach_port_type_t *types, mach_msg_type_number_t count)
{
	for (mach_msg_type_number_t i = 0; i < count; i++) {
		mach_port_name_t *members = NULL;
		mach_msg_type_number_t mcount = 0;
		mach_port_status_t ss;
		mach_msg_type_number_t sc = MACH_PORT_RECEIVE_STATUS_COUNT;

		if ((types[i] & MACH_PORT_TYPE_PORT_SET) == 0) {
			continue;
		}
		if (mach_port_get_set_status(mach_task_self(), names[i], &members, &mcount) != KERN_SUCCESS) {
			continue;
		}
		for (mach_msg_type_number_t j = 0; j < mcount; j++) {
			if (members[j] != queued) {
				continue;
			}
			if (mach_port_get_attributes(mach_task_self(), names[i],
			    MACH_PORT_RECEIVE_STATUS, (mach_port_info_t)&ss, &sc) == KERN_SUCCESS) {
				say("hakios: %s queued %#x is member %u of set %#x (set msgs %u seqno %u)\n",
				    who, queued, (unsigned)j, names[i],
				    (unsigned)ss.mps_msgcount, (unsigned)ss.mps_seqno);
			} else {
				say("hakios: %s queued %#x is member %u of set %#x\n", who,
				    queued, (unsigned)j, names[i]);
			}
		}
		vm_deallocate(mach_task_self(), (vm_address_t)members,
		    mcount * sizeof(mach_port_name_t));
	}
}

/* one message to ourselves, through a port and then through a port set */
static void
watch_portset_selftest(const char *who)
{
	static bool done;
	mach_port_t port = MACH_PORT_NULL, set = MACH_PORT_NULL;
	mach_msg_header_t *out;
	char *in;
	mach_msg_return_t sr, rr;
	kern_return_t kr;

	if (done) {
		return;
	}
	done = true;
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
	if (kr == KERN_SUCCESS) {
		kr = mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
	}
	if (kr != KERN_SUCCESS) {
		say("hakios: %s selftest: no port (%s)\n", who, mach_error_string(kr));
		return;
	}
	out = calloc(1, sizeof(*out));
	in = calloc(1, 256);
	if (out == NULL || in == NULL) {
		free(out);
		free(in);
		return;
	}
	out->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	out->msgh_size = sizeof(*out);
	out->msgh_remote_port = port;
	sr = mach_msg(out, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(*out), 0, MACH_PORT_NULL,
	    100, MACH_PORT_NULL);
	rr = mach_msg((mach_msg_header_t *)in, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, 256, port,
	    1000, MACH_PORT_NULL);
	say("hakios: %s selftest port: send %d (%s) recv %d (%s)\n", who, sr,
	    mach_error_string(sr), rr, mach_error_string(rr));
	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &set) == KERN_SUCCESS &&
	    mach_port_move_member(mach_task_self(), port, set) == KERN_SUCCESS) {
		memset(in, 0, 256);
		memset(out, 0, sizeof(*out));
		out->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		out->msgh_size = sizeof(*out);
		out->msgh_remote_port = port;
		sr = mach_msg(out, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(*out), 0,
		    MACH_PORT_NULL, 100, MACH_PORT_NULL);
		rr = mach_msg((mach_msg_header_t *)in, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, 256, set,
		    1000, MACH_PORT_NULL);
		say("hakios: %s selftest set: send %d (%s) recv %d (%s)\n", who, sr,
		    mach_error_string(sr), rr, mach_error_string(rr));
	} else {
		say("hakios: %s selftest set: no port set\n", who);
	}
	if (set != MACH_PORT_NULL) {
		mach_port_deallocate(mach_task_self(), set);
	}
	mach_port_deallocate(mach_task_self(), port);
	free(out);
	free(in);
}

/* fix136: what the runloop's receive set actually contains, and what the stranded port is.
 * watch_portset_membership() says the queued port is in no set; this says which sets exist. */
static void
watch_portset_dump(const char *who, mach_port_name_t *names, mach_port_type_t *types,
    mach_msg_type_number_t count)
{
	static bool done;

	if (done) {
		return;
	}
	done = true;
	for (mach_msg_type_number_t i = 0; i < count; i++) {
		mach_port_name_t *members = NULL;
		mach_msg_type_number_t mcount = 0;

		if ((types[i] & MACH_PORT_TYPE_PORT_SET) == 0) {
			continue;
		}
		if (mach_port_get_set_status(mach_task_self(), names[i], &members, &mcount) != KERN_SUCCESS) {
			continue;
		}
		say("hakios: %s set %#x members %u\n", who, names[i], (unsigned)mcount);
		for (mach_msg_type_number_t j = 0; j < mcount && j < 8; j++) {
			mach_port_type_t mtype = 0;

			mach_port_type(mach_task_self(), members[j], &mtype);
			say("hakios: %s set %#x member %u: %#x type %#x\n", who, names[i],
			    (unsigned)j, members[j], (unsigned)mtype);
		}
		vm_deallocate(mach_task_self(), (vm_address_t)members,
		    mcount * sizeof(mach_port_name_t));
	}
}

static void
watch_scan_ports(const char *who)
{
	mach_port_name_t *names = NULL;
	mach_port_type_t *types = NULL;
	mach_msg_type_number_t count = 0, type_count = 0;
	kern_return_t kr;
	unsigned receive = 0, queued = 0;

	say_once("hakios: %s scan start t=%.1f\n", who, watch_since_boot());
	watch_portset_selftest(who);
	/* the types array is the same information mach_port_type() would give, per name */
	kr = mach_port_names(mach_task_self(), &names, &count, &types, &type_count);
	if (kr != KERN_SUCCESS) {
		say_once("hakios: %s scan: mach_port_names kr %d (%s)\n", who, kr,
		    mach_error_string(kr));
		return;
	}
	if (type_count < count) {
		count = type_count;
	}
	watch_portset_dump(who, names, types, count);
	for (mach_msg_type_number_t i = 0; i < count; i++) {		mach_port_status_t status;
		mach_msg_type_number_t scount = MACH_PORT_RECEIVE_STATUS_COUNT;

		if ((types[i] & MACH_PORT_TYPE_RECEIVE) == 0) {
			continue;
		}
		receive++;
		if (mach_port_get_attributes(mach_task_self(), names[i], MACH_PORT_RECEIVE_STATUS,
		    (mach_port_info_t)&status, &scount) != KERN_SUCCESS) {
			continue;
		}
		if (status.mps_msgcount == 0) {
			continue;
		}
		queued++;
		{
			mach_port_seqno_t seq = 0;
			mach_msg_size_t msize = 0;
			mach_msg_id_t mid = 0;
			mach_msg_max_trailer_t trailer;
			mach_msg_type_number_t tsize = sizeof(trailer);
			kern_return_t pkr;

			memset(&trailer, 0, sizeof(trailer));
			pkr = mach_port_peek(mach_task_self(), names[i], 0, &seq, &msize, &mid,
			    (mach_msg_trailer_info_t)&trailer, &tsize);
			say_once("hakios: %s queue %#x msgs %u qlimit %u mscount %u seqno %u "
			    "first %#x size %u peek %d\n", who, names[i],
			    (unsigned)status.mps_msgcount, (unsigned)status.mps_qlimit,
			    (unsigned)status.mps_mscount, (unsigned)status.mps_seqno,
			    (unsigned)mid, (unsigned)msize, pkr);
		}
		{
			mach_port_type_t qtype = 0;

			mach_port_type(mach_task_self(), names[i], &qtype);
			say_once("hakios: %s port %#x type %#x flags %#x srights %u sorights %u pd %u ns %u\n",
			    who, names[i], (unsigned)qtype, (unsigned)status.mps_flags,
			    (unsigned)status.mps_srights, (unsigned)status.mps_sorights,
			    (unsigned)status.mps_pdrequest, (unsigned)status.mps_nsrequest);
		}
		watch_probe_port(who, names[i]);
		watch_portset_membership(who, names[i], names, types, count);
	}
	if (bbd_service_port != 0) {
		mach_port_status_t ss;
		mach_msg_type_number_t sc = MACH_PORT_RECEIVE_STATUS_COUNT;

		if (mach_port_get_attributes(mach_task_self(), bbd_service_port,
		    MACH_PORT_RECEIVE_STATUS, (mach_port_info_t)&ss, &sc) == KERN_SUCCESS) {
			say_once("hakios: %s svc %#x msgs %u qlimit %u mscount %u seqno %u t=%.1f\n",
			    who, bbd_service_port, (unsigned)ss.mps_msgcount,
			    (unsigned)ss.mps_qlimit, (unsigned)ss.mps_mscount,
			    (unsigned)ss.mps_seqno, watch_since_boot());
		} else {
			say_once("hakios: %s svc %#x: no receive right here any more\n", who,
			    bbd_service_port);
		}
	}
	say_once("hakios: %s ports %u receive %u queued %u t=%.1f\n", who, (unsigned)count,
	    receive, queued, watch_since_boot());
	vm_deallocate(mach_task_self(), (vm_address_t)names, count * sizeof(mach_port_name_t));
	if (types != NULL) {
		vm_deallocate(mach_task_self(), (vm_address_t)types, count * sizeof(mach_port_type_t));
	}
}

static void *
watch_main_thread(__unused void *argument)
{
	struct watch *w = argument;
	uintptr_t last_pc = 0;

	for (unsigned i = 0; i < 300; i++) {
		watch_thread_state64_t state;
		mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
		uintptr_t pc, lr, sp, fp;

		usleep(1000 * 1000);
		if (thread_get_state(w->thread, ARM_THREAD_STATE64, (thread_state_t)&state,
		    &count) != KERN_SUCCESS) {
			int alive = watch_thread_alive(w);

			say_once("hakios: %s main thread: thread_get_state failed at sample %u, tid %#llx %s\n",
			    w->who, i, (unsigned long long)w->tid,
			    alive > 0 ? "still in the task (stale name)" :
			    alive == 0 ? "gone from the task" : "task_threads failed");
			if (alive == 0) {
				break;
			}
			continue;
		}
		pc = (uintptr_t)state.pc;
		lr = (uintptr_t)state.lr;
		sp = (uintptr_t)state.sp;
		fp = (uintptr_t)state.fp;
		say_once("hakios: %s t=%.1f sample %u pc %#lx lr %#lx sp %#lx fp %#lx "
		    "x0 %#llx x1 %#llx x2 %#llx x3 %#llx x4 %#llx x5 %#llx x6 %#llx x7 %#llx mainq %u\n",
		    w->who, watch_since_boot(), i, pc, lr, sp, fp,
		    (unsigned long long)state.x[0], (unsigned long long)state.x[1],
		    (unsigned long long)state.x[2], (unsigned long long)state.x[3],
		    (unsigned long long)state.x[4], (unsigned long long)state.x[5],
		    (unsigned long long)state.x[6], (unsigned long long)state.x[7], *w->beats);
		if (pc != last_pc) {
			watch_say_pc(w->who, "pc", pc);
			watch_say_pc(w->who, "lr", lr);
		}
		if ((i % 10) == 0) {
			watch_say_chain(w->who, fp);
		}
		if (w->dump_every != 0 && i > 0 && (i % w->dump_every) == 0) {
			watch_dump_threads(w->who);
		}
		if (w->scan_every != 0 && i > 0 && (i % w->scan_every) == 0) {
			watch_scan_ports(w->who);
		}
		if (pc - slide == WATCH_TRAP_MACH_MSG2 && (state.x[1] & 1) && w->dumps < 3 &&
		    (i % 5) == 0) {
			w->dumps++;
			watch_dump_msg(w->who, &state);
			watch_dump_threads(w->who);
		}
		if ((i & 1) == 0) {
			dispatch_async(dispatch_get_main_queue(), ^{
				(*w->beats)++;
				say_once("hakios: %s mainq serviced t=%.1f beats=%u\n", w->who,
				    watch_since_boot(), *w->beats);
			});
		}
		last_pc = pc;
	}
	return NULL;
}

static void
watch_start(struct watch *w)
{
	pthread_t thread;
	thread_identifier_info_data_t id;
	mach_msg_type_number_t id_count = THREAD_IDENTIFIER_INFO_COUNT;

	w->thread = pthread_mach_thread_np(pthread_self());
	/* keep the name alive: task_threads returns and deallocates its own send rights */
	if (w->thread != MACH_PORT_NULL) {
		mach_port_mod_refs(mach_task_self(), w->thread, MACH_PORT_RIGHT_SEND, 1);
	}
	memset(&id, 0, sizeof(id));
	thread_info(w->thread, THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &id_count);
	w->tid = id.thread_id;
	say_once("hakios: %s main thread: sampler from %s (main=%d) port %#x tid %#llx\n", w->who,
	    pthread_main_np() ? "the main thread" : "NOT the main thread", (int)pthread_main_np(),
	    w->thread, (unsigned long long)id.thread_id);
	if (pthread_create(&thread, NULL, watch_main_thread, w) == 0) {
		pthread_detach(thread);
	}
}

__attribute__((constructor))
static void
hakios_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (name == NULL || cache == NULL) {
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	if (strcmp(name, "backboardd") == 0) {
		pthread_t probe;

		watch_start(&bbd_watch);
		if (pthread_create(&probe, NULL, watch_service_probe, NULL) == 0) {
			pthread_detach(probe);
		}
		/* fix66: the CA_DISABLE_GPU / CA_FORCE_SW_RENDERER setenvs are gone -- redwood's original
		 * never set them, QuartzCore falls back to its software renderer on its own when no GPU
		 * service matches, and the my_renderer vtable hook still forces the software path from
		 * the other side. */

		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
			arguments[0] = '\0';
		}
		hakios_ca_prod = strstr(arguments, "ca_prod=1") != NULL;

		/*
		 * Boot-arg ca_vbl=1 (fix73, taken from upstream HakiOS): CoreAnimation's frame clock from
		 * the display driver's refresh notifications. QuartzCore takes them
		 * (IOMobileFramebufferEnableVSyncNotifications) only on the phones with an A7 to A10, or
		 * when CA_FORCE_TIMER is in the environment (CADeviceUseVBL()); on the iPhone X it runs a
		 * timer of its own instead (CADisplayTimer), whose phase it would set from the frame
		 * timings of a display driver this one is not.  fix50's negative verdict was measured
		 * while the unfreeze thread was still killing every backboardd generation -- never valid.
		 */
		if (strstr(arguments, "ca_vbl=1") != NULL) {
			setenv("CA_FORCE_TIMER", "1", 1);
			say("hakios: ca_vbl=1: CoreAnimation is to take the display driver's refresh notifications\n");
		}
		/*
		 * Boot-arg caenv=NAME=VALUE (as many as wanted): into the window server's environment, for
		 * CoreAnimation's own switches (CA_LOG_TRIPLE_BUFFERING=1, CA_FORCE_DOUBLE_BUFFERING=1 ...).
		 */
		for (const char *argument = arguments; (argument = strstr(argument, "caenv=")) != NULL; argument += 6) {
			char setting[128];
			size_t length = strcspn(argument + 6, " ");
			char *equals;

			if ((argument != arguments && argument[-1] != ' ') || length == 0 || length >= sizeof(setting)) {
				continue;
			}
			memcpy(setting, argument + 6, length);
			setting[length] = 0;
			if ((equals = strchr(setting, '=')) != NULL) {
				*equals = 0;
				setenv(setting, equals + 1, 1);
				say("hakios: caenv: %s=%s for the window server\n", setting, equals + 1);
			}
		}
		opengl_es();
		window_server_fixes();
		watch_for_faults();
		cawindowserver_trace_fixes();
		/* fix65: unfreeze_window_server is switched off.  fix49 proved the freeze option
		 * was never the gate: backboardd cleared it itself four seconds later ("[RenderOverlay]
		 * Enabling framebuffer changes on display (1)").  The thread only killed the window
		 * server, through _dyld_get_shared_cache_range(NULL) inside hakios_cache_mappings.
		 * The display gates (blanked / disablesUpdates / disabled) are handled where they
		 * are set: hooked_serverWithOptions. */
		{
			struct stat rainy;

			if (stat("/redwood/etc/rain", &rainy) == 0) {
				pthread_t t;

				say("hakios: /redwood/etc/rain is here: the board goes to the shim after 300 s\n");
				pthread_create(&t, NULL, rain_later, NULL);
			}
		}
		/* fix66: hakios_install_cacontext_swap_trigger() is off: it forced
		 * definesDisplayBounds=YES into every CAContext (scene ownership weirdness) and its
		 * CA_DISABLE_GPU setenvs went with it. */
		hakios_install_brightness_and_display_fixes();
		hid_system_fixes();
		input_start();
	} else if (strncmp(name, "containermanagerd", 17) == 0) {
		container_manager_fixes();
	} else if (strcmp(name, "logd") == 0) {
		watch_start(&logd_watch);
	} else if (strcmp(name, "runningboardd") == 0) {
		watch_start(&rbd_watch);
	} else if (strcmp(name, "lsd") == 0) {
		say("hakios: lsd injected (pid=%d)\n", getpid());
		watch_start(&lsd_watch);
	}
}
