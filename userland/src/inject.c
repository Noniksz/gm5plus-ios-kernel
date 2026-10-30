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
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
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

static void
say(const char *format, ...)
{
	char line[256];
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
		say("hakios: is_ready -> %d (call %u, flags %#llx, b233 %#x)\n", ready, calls_ready,
		    (unsigned long long)DISPLAY_FLAGS(display), DISPLAY_B233(display));
		last_ready = ready;
	}
	calls_ready++;
	return ready;
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

static id hooked_serverWithOptions(id self, SEL _cmd, id options) {
	say("hakios: +[CAWindowServer serverWithOptions:%p] ENTER (real QuartzCore)\n", (void *)options);
	id srv = orig_serverWithOptions ? orig_serverWithOptions(self, _cmd, options) : nil;
	say("hakios: +[CAWindowServer serverWithOptions:] DONE -> %p\n", (void *)srv);
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
	const void *cache = _dyld_get_shared_cache_range(NULL);
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
 * 6. INTERPOSITION: FAST-PATH BYPASS FOR OSCARD & COREACCESSORIES
 * ============================================================================ */
typedef void * xpc_connection_t;
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, void *targetq, uint64_t flags);
extern int CoreAccessoriesInitForHID(void);

static int my_CoreAccessoriesInitForHID(void)
{
	say("hakios: CoreAccessoriesInitForHID called -> fast return 0\n");
	return 0;
}

static xpc_connection_t my_xpc_connection_create_mach_service(const char *name, void *targetq, uint64_t flags)
{
	if (name && (strcmp(name, "com.apple.oscard") == 0 ||
	             strcmp(name, "com.apple.ACCHWComponentAuthService") == 0)) {
		say("hakios: xpc_connection_create_mach_service(%s) -> fast NULL\n", name);
		return (xpc_connection_t)0;
	}
	return xpc_connection_create_mach_service(name, targetq, flags);
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} hakios_interposes[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)my_CoreAccessoriesInitForHID, (const void *)CoreAccessoriesInitForHID },
	{ (const void *)my_xpc_connection_create_mach_service, (const void *)xpc_connection_create_mach_service },
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
		setenv("CA_DISABLE_GPU", "1", 1);
		setenv("CA_FORCE_SW_RENDERER", "1", 1);

		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		/*
		 * CA_FORCE_TIMER=0 (fix50 tried 1: the window server never reached "StartWindowServer"
		 * at all and the board reset, so QuartzCore really does wait for a hardware VSync it
		 * cannot get here).  With 0 the window server comes up and finishes its start, but it
		 * never asks for refresh notifications either (no registerNotificationPort), and it never
		 * commits a frame: no surface is ever allocated (fix49).  So the frame clock is not what
		 * is holding the first frame back; something before it is.  See unfreeze_window_server().
		 */
		setenv("CA_FORCE_TIMER", "0", 1);
		say("hakios: CA_FORCE_TIMER=0 set: CoreAnimation CADisplayTimer runs autonomously\n");
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
		{ pthread_t t; pthread_create(&t, NULL, unfreeze_window_server, NULL); }
		{
			struct stat rainy;

			if (stat("/redwood/etc/rain", &rainy) == 0) {
				pthread_t t;

				say("hakios: /redwood/etc/rain is here: the board goes to the shim after 300 s\n");
				pthread_create(&t, NULL, rain_later, NULL);
			}
		}
		hakios_install_cacontext_swap_trigger();
		hakios_install_brightness_and_display_fixes();
		hid_system_fixes();
		input_start();
	} else if (strncmp(name, "containermanagerd", 17) == 0) {
		container_manager_fixes();
	}
}
