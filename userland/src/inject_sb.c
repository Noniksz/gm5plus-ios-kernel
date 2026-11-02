/*
 * libhakios.sb.dylib: code of ours inside SpringBoard (see inject.c for how it gets there).
 *
 *   - The phone is "activated". SpringBoard asks mobileactivationd, which is not running (and
 *     could not talk to Apple anyway); without an answer it treats the device as bricked and
 *     shows the first-boot setup ("hello") instead of the lock screen.
 *   - The setup assistant is never needed, whatever the reason: nobody is going to go through
 *     it here.
 *   - SpringBoard starts a few seconds late where the kernel's boot-args say so (sb_delay=N;
 *     "xl run" does for the phone). /private/var is empty at every boot, so the applications
 *     are registered anew each time, one after another; one that is registered after
 *     SpringBoard has laid out the home screen ends up on a later page, and which one that is
 *     differs from boot to boot. On the phone that happens; under QEMU, where everything takes
 *     five times as long, it does not -- and there the delay is harmful: for a reason not yet
 *     found the window server then never takes our touch screen service for its own.
 *     (A wrapper program in the launchd job would be the obvious way to wait; launchd panics
 *     the system when a system job runs anything that is not Apple's.)
 *   - The picture SpringBoard keeps of an application that leaves the screen (for the application
 *     switcher and the next launch) is saved as JPEG or PNG, not as an ASTC texture: see
 *     snapshot_output_format().
 *   - Where CoreAnimation draws with its software renderer (QEMU; the phone without gpu=1) the
 *     "vibrant" views draw their content as it is: see plain_vibrancy().
 *   - A press of the side button that is long over is not taken for a long press (Siri) because
 *     SpringBoard was busy waking the display: see lock_long_press().
 *   - A long press on the lock screen's wallpaper does not open the poster switcher, which the
 *     window server here shows as a black screen: see poster_switcher_allowed().
 *   - Darwin notifications as a remote control, for scripts (rwtouch notify NAME):
 *       com.hakios.unlock      what the lock screen does after "swipe up to open"
 *       com.hakios.layers      write down the layers SpringBoard has on the screen: see layers()
 *       com.hakios.switcher    open the switcher of running applications: see switcher()
 *     In any other program given this library (INJECT in build.sh) only the last of them is
 *     answered, with that program's layers.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <time.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdint.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <mach/semaphore.h>
#include <mach_debug/mach_debug_types.h>
#include <mach-o/dyld.h>
#include <mach/arm/thread_status.h>
#include <notify.h>
#include <pthread.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* where what is said goes: the standard error, which is the console's for SpringBoard (an application's is nowhere) */
static int say_fd = 2;

static void
say(const char *format, ...)
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
	write(say_fd, line, (size_t)n);
}

/*
 * Boot-arg touch_lat=1: a line for every turn of SpringBoard's main run loop -- when it woke up (a
 * touch, a timer, the display link ...), when CoreAnimation's commit of what that turn changed
 * began and when it was done (our observers just before and after its own, of order 2000000), in
 * microseconds of mach_absolute_time, as the other "tl:" lines (tools/latency.py, tools/frames.py):
 *   tl: a WOKE COMMIT DONE
 * (a commit with nothing in it takes some microseconds; one that sends changes, a good deal more)
 */
#define CA_COMMIT_ORDER 2000000
static uint64_t main_woke, commit_began;
static mach_timebase_info_data_t timebase;

static uint64_t
now_us(void)
{
	return mach_absolute_time() * timebase.numer / timebase.denom / 1000;
}

static void
main_loop_turn(__unused CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
	if (activity == kCFRunLoopAfterWaiting) {
		main_woke = now_us();
	} else if (activity == kCFRunLoopBeforeWaiting && info == NULL) {
		commit_began = now_us();
	} else if (activity == kCFRunLoopBeforeWaiting && main_woke != 0) {
		say("tl: a %llu %llu %llu\n", main_woke, commit_began, now_us());
		main_woke = 0;
	}
}

static void
watch_main_loop(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *arg;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (arg = strstr(args, "touch_lat=1")) == NULL ||
	    (arg != args && arg[-1] != ' ')) {
		return;
	}
	mach_timebase_info(&timebase);
	CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopAfterWaiting, true, LONG_MIN,
	    main_loop_turn, NULL), kCFRunLoopCommonModes);
	CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true, CA_COMMIT_ORDER - 1,
	    main_loop_turn, NULL), kCFRunLoopCommonModes);
	{
		CFRunLoopObserverContext after = { .info = (void *)1 };

		CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true,
		    CA_COMMIT_ORDER + 1, main_loop_turn, &after), kCFRunLoopCommonModes);
	}
}

/* ---- where SpringBoard's main thread is, when it is not turning its run loop -------------------- */

/*
 * touch_lat=1's +1 s block (the constructor, below) never ran in fix100/101/102: no "battery: SOC="
 * line and not one "tl:" line.  fix102's sampler put the main thread in a mach_msg send (os_log to
 * logd) early and in SpringBoard code around what looked like 90 s -- but its lines had no clock,
 * and the serial log interleaves an older boot, so that time was a guess.  This one says it: every
 * sample says the seconds since boot, every fifth (and every change of pc) walks the stack for
 * return addresses, and every two seconds a block says when the main queue serviced it.  Nothing is
 * suspended: thread_get_state on a running thread is what inject_fairplay.c's sampler uses.
 */
#define WATCH_SAMPLES 6000                  /* 1 Hz: the process started at 168 s in fix104 */
static mach_port_t sb_main_thread;
static uintptr_t slide;                    /* the shared cache's, for unslid addresses in the log */
static volatile unsigned mainq_beats;      /* main-queue blocks that have run */

/* thread_get_state writes the raw arm64 state; the SDK's struct is opaque, this is not. */
typedef struct {
	uint64_t x[29];
	uint64_t fp;
	uint64_t lr;
	uint64_t sp;
	uint64_t pc;
	uint32_t cpsr;
	uint32_t flags;
} sb_thread_state64_t;

/* Seconds since boot, the clock the kernel's "[HB] heartbeat N, M s since start" counts in. */
static double
since_boot(void)
{
	mach_timebase_info_data_t tb;

	mach_timebase_info(&tb);
	return (double)mach_absolute_time() * (double)tb.numer / (double)tb.denom / 1e9;
}

/* A second way out of the process, for the case where stderr (say's fd 2) has been closed on us. */
static void
say_console(const char *line)
{
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY);

	if (fd >= 0) {
		write(fd, line, strlen(line));
		close(fd);
	}
}

static void
say_pc(const char *what, uintptr_t pc)
{
	Dl_info info;
	const char *name = "?";
	uintptr_t base = 0;

	if (pc != 0 && dladdr((void *)pc, &info) != 0) {
		const char *slash = strrchr(info.dli_fname, '/');

		name = slash ? slash + 1 : info.dli_fname;
		base = (uintptr_t)info.dli_fbase;
	}
	say("hakios: sb %.1f %s %#lx = %s+%#lx (cache %#lx)\n", since_boot(), what, pc, name, pc - base, pc - slide);
}

/*
 * The frame-pointer chain is not enough on arm64e (a leaf frame and a stub have no chain).  The
 * stack itself is: walk a kilobyte of it for words that dyld resolves to an image, drop the
 * arm64e signature bits, and say each distinct one once.  This is a reading of a running thread's
 * stack, so a line can be torn; the same address in two samples is the signal.
 */
static void
say_stack(uintptr_t sp)
{
	uintptr_t words[128];
	uintptr_t seen[12];
	vm_size_t got = 0;
	unsigned shown = 0;

	sp &= ~(uintptr_t)0xf;
	if (vm_read_overwrite(mach_task_self(), (vm_address_t)sp, sizeof(words), (vm_address_t)words,
	    &got) != KERN_SUCCESS) {
		return;
	}
	for (unsigned i = 0; i < got / sizeof(uintptr_t) && shown < 12; i++) {
		uintptr_t v = words[i] & 0x0000000fffffffffUL;
		Dl_info info;
		bool dup = false;

		if (v < 0x100000000UL || dladdr((void *)v, &info) == 0 || info.dli_fbase == NULL) {
			continue;
		}
		for (unsigned j = 0; j < shown; j++) {
			dup |= seen[j] == v;
		}
		if (dup) {
			continue;
		}
		seen[shown++] = v;
		say_pc("stack", v);
	}
}

/*
 * fix107: the frame-pointer chain, frame by frame -- [fp] is the caller's fp, [fp + 8] the
 * return address.  The stack scan above drowns the call path in stale words; the chain is the
 * one reading that says which function called into BackBoardServices, and through which.
 */
static void
say_chain(uintptr_t fp)
{
	for (unsigned frame = 0; frame < 12 && fp >= 0x100000000UL && (fp & 7) == 0; frame++) {
		uintptr_t pair[2];
		vm_size_t got = 0;

		if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof(pair), (vm_address_t)pair,
		    &got) != KERN_SUCCESS || got < sizeof(pair)) {
			break;
		}
		if (pair[1] != 0) {
			say_pc("chain", pair[1] & 0x0000000fffffffffUL);
		}
		if (pair[0] <= fp) {
			break;
		}
		fp = pair[0];
	}
}

/*
 * fix107: the call SpringBoard's main thread waits on is BackBoardServices' synchronous MIG call
 * to "com.apple.backboard.display.services" (msgh_id 0x5b9171, 24 bytes, no arguments).  Its peer
 * is the display-services server inside backboardd, whose own log stops at t ~ 60 s.  Ask the same
 * question ourselves at two times: look the name up in launchd's namespace, send the same
 * 24-byte request with a two-second send timeout, then wait two seconds for the reply.  A server
 * that does not take the message, or takes it and never answers, is the stall itself, seen from
 * outside; an answer before SpringBoard blocks and none after says when it went away.
 */
extern kern_return_t bootstrap_look_up(mach_port_t bootstrap, const char *service_name,
    mach_port_t *service_port);
extern mach_port_t mach_reply_port(void);

static void
probe_display_services(void)
{
	static const char service[] = "com.apple.backboard.display.services";
	mach_port_t bootstrap = MACH_PORT_NULL, server = MACH_PORT_NULL;
	mach_port_t reply = MACH_PORT_NULL;
	kern_return_t kr;
	char request[24];

	task_get_bootstrap_port(mach_task_self(), &bootstrap);
	kr = bootstrap_look_up(bootstrap, service, &server);
	say("hakios: sb probe t=%.1f look up \"%s\": kr %d port %#x\n", since_boot(), service, kr, server);
	if (kr != KERN_SUCCESS) {
		return;
	}
	/*
	 * fix112 step 1: send with no reply port at all.  The kernel only walks the reply-port
	 * check when msgh_local_port is valid, so this send cannot trip kGUARD_EXC_SEND_INVALID_REPLY
	 * whatever state the reply right is in.  Its return value alone says whether the endpoint
	 * launchd hands out for this service accepts ordinary sends.
	 */
	memset(request, 0, sizeof(request));
	*(uint32_t *)(request + 0) = 0x00000013;       /* COPY_SEND to the server, no local port */
	*(uint32_t *)(request + 4) = sizeof(request);
	*(uint32_t *)(request + 8) = server;
	*(uint32_t *)(request + 20) = 0x5b9171;
	say("hakios: sb probe t=%.1f ff send id 0x5b9171 to %#x\n", since_boot(), server);
	kr = mach_msg((mach_msg_header_t *)request, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(request), 0,
	    MACH_PORT_NULL, 80, MACH_PORT_NULL);
	say("hakios: sb probe t=%.1f ff send kr %d (%s)\n", since_boot(), kr, mach_error_string(kr));

	/*
	 * fix112 step 2: an explicitly allocated receive right, with its rights read back before
	 * the send.  If this send still dies, launchd's guard code names the port and the log says
	 * what the kernel thought of that same name a few microseconds earlier.
	 */
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply);
	{
		mach_port_type_t type = 0;
		mach_port_urefs_t refs = 0;

		if (kr == KERN_SUCCESS) {
			mach_port_type(mach_task_self(), reply, &type);
			mach_port_get_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, &refs);
		}
		say("hakios: sb probe t=%.1f reply %#x alloc kr %d type %#x urefs %u\n", since_boot(),
		    reply, kr, type, refs);
	}
	if (kr != KERN_SUCCESS) {
		mach_port_deallocate(mach_task_self(), server);
		return;
	}
	say("hakios: sb probe t=%.1f hdr bits %#x size %u remote %#x local %#x voucher 0 id %#x\n",
	    since_boot(), 0x1513, (unsigned)sizeof(request), server, reply, 0x5b9171);
	memset(request, 0, sizeof(request));
	*(uint32_t *)(request + 0) = 0x00001513;       /* COPY_SEND to the server, MAKE_SEND_ONCE reply */
	*(uint32_t *)(request + 4) = sizeof(request);
	*(uint32_t *)(request + 8) = server;
	*(uint32_t *)(request + 12) = reply;
	*(uint32_t *)(request + 20) = 0x5b9171;
	say("hakios: sb probe t=%.1f sending id 0x5b9171 to %#x\n", since_boot(), server);
	kr = mach_msg((mach_msg_header_t *)request, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(request), 0,
	    MACH_PORT_NULL, 80, MACH_PORT_NULL);
	say("hakios: sb probe t=%.1f send id 0x5b9171: kr %d (%s)\n", since_boot(), kr, mach_error_string(kr));
	if (kr != KERN_SUCCESS) {
		mach_port_deallocate(mach_task_self(), server);
		mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1);
		return;
	}
	{
		char buffer[128];
		mach_msg_header_t *header = (mach_msg_header_t *)buffer;

		memset(buffer, 0, sizeof(buffer));
		kr = mach_msg(header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(buffer), reply, 80,
		    MACH_PORT_NULL);
		if (kr == KERN_SUCCESS) {
			say("hakios: sb probe t=%.1f reply bits %#x size %u id %#x\n", since_boot(),
			    header->msgh_bits, header->msgh_size, header->msgh_id);
		} else {
			say("hakios: sb probe t=%.1f no reply: kr %d (%s)\n", since_boot(), kr,
			    mach_error_string(kr));
		}
	}
	mach_port_deallocate(mach_task_self(), server);
	if (reply != MACH_PORT_NULL) {
		mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1);
	}
}

static void * __attribute__((unused))
sb_probe_thread(__unused void *ignored)
{
	/*
	 * fix108: relative waits, not the absolute mach clock: on a warm reboot the hardware
	 * counter keeps running, so since_boot() is already far past 75 s when SpringBoard starts
	 * (fix107 showed t ~ 1284 s at launch, and both probes fired at once).
	 */
	sleep(2);
	probe_display_services();
	sleep(3);
	probe_display_services();
	return NULL;
}

/*
 * mach_msg2_trap's eight arguments are the whole story: options say send, receive or both;
 * msgh_bits_and_send_size and msgh_remote_and_local_port name the ports; voucher_and_id carries
 * the message id; desc_count_and_rcv_name the port we receive on; rcv_size_and_priority the
 * buffer size; the last is the timeout.  For a send, the buffer at x0 holds an XPC body, whose
 * printable runs say which service or key the call is about.  Said twice per kind.
 */
static void
say_strings(const unsigned char *buf, unsigned len)
{
	char run[49];
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
		if (n >= 4 && said < 6) {
			run[n] = 0;
			say("hakios: sb msg str: %s\n", run);
			said++;
		}
		n = 0;
	}
}

static void
dump_mach_msg(const sb_thread_state64_t *st)
{
	uint64_t option = st->x[1], bits_ss = st->x[2], ports = st->x[3];
	uint64_t vou_id = st->x[4], dc_rn = st->x[5], rsize_pri = st->x[6];
	uint32_t bits = (uint32_t)bits_ss, ssize = (uint32_t)(bits_ss >> 32);
	uint32_t remote = (uint32_t)ports, local = (uint32_t)(ports >> 32);
	uint32_t voucher = (uint32_t)vou_id, id = (uint32_t)(vou_id >> 32);
	uint32_t dcount = (uint32_t)dc_rn, rname = (uint32_t)(dc_rn >> 32);
	uint32_t rsize = (uint32_t)rsize_pri, pri = (uint32_t)(rsize_pri >> 32);
	unsigned char buf[0x300];
	vm_size_t got = 0;
	unsigned want = ssize > sizeof(buf) ? sizeof(buf) : ssize;

	say("hakios: sb msg2 opt %#llx bits %#x ssize %u remote %#x local %#x voucher %#x id %#x "
	    "dcnt %u rname %#x rsize %u pri %u timeout %#llx\n", (unsigned long long)option, bits,
	    ssize, remote, local, voucher, id, dcount, rname, rsize, pri,
	    (unsigned long long)st->x[7]);
	if (want == 0 || st->x[0] < 0x100000000UL ||
	    vm_read_overwrite(mach_task_self(), (vm_address_t)st->x[0], want, (vm_address_t)buf,
	    &got) != KERN_SUCCESS || got < 8) {
		return;
	}
	say("hakios: sb msg hex %08x %08x %08x %08x %08x %08x %08x %08x\n",
	    *(uint32_t *)(buf + 0), *(uint32_t *)(buf + 4), *(uint32_t *)(buf + 8),
	    *(uint32_t *)(buf + 12), *(uint32_t *)(buf + 16), *(uint32_t *)(buf + 20),
	    *(uint32_t *)(buf + 24), *(uint32_t *)(buf + 28));
	say_strings(buf, (unsigned)got);
}

/* What kind of right a name is and whether it is the task's bootstrap port (launchd). */
static void
probe_port(const char *what, mach_port_t name)
{
	mach_port_type_t type = 0;
	mach_port_urefs_t refs = 0;
	mach_port_t bootstrap = MACH_PORT_NULL;
	kern_return_t kr = mach_port_type(mach_task_self(), name, &type);

	task_get_bootstrap_port(mach_task_self(), &bootstrap);
	say("hakios: sb port %s %#x type %#x kr %d, bootstrap %#x%s\n", what, name, type, kr,
	    bootstrap, name == bootstrap ? " THIS IS BOOTSTRAP" : "");
	if (mach_port_get_refs(mach_task_self(), name, MACH_PORT_RIGHT_SEND, &refs) == KERN_SUCCESS) {
		say("hakios: sb port %s %#x send refs %u\n", what, name, refs);
	}
	{
		natural_t object_type = 0;
		mach_vm_address_t object_addr = 0;

		kr = mach_port_kobject(mach_task_self(), name, &object_type, &object_addr);
		say("hakios: sb port %s %#x kobject kr %d type %#x addr %#llx\n", what, name, kr,
		    object_type, (unsigned long long)object_addr);
		if (kr == KERN_SUCCESS) {
			kobject_description_t description;

			memset(description, 0, sizeof(description));
			if (mach_port_kobject_description(mach_task_self(), name, &object_type, &object_addr,
			    description) == KERN_SUCCESS) {
				say("hakios: sb port %s %#x kobject says \"%s\"\n", what, name, description);
			}
			if (object_type == 2 /* IKOT_TASK */) {
				int pid = 0;

				if (pid_for_task(name, &pid) == KERN_SUCCESS) {
					say("hakios: sb port %s %#x is task port of pid %d\n", what, name, pid);
				}
			}
		}
	}
}


/*
 * fix138: the ulock owner word is a Mach thread port name (masked with ~3), not the
 * kernel tid.  Match task_threads() ports, mark the owner, and dump its lr/chain so a
 * framework thread that holds a lock across a sync XPC is visible in the log.
 */
static void
dump_threads(unsigned owner_word)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t count = 0;
	mach_port_t self_port = pthread_mach_thread_np(pthread_self());

	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		return;
	}
	say("hakios: sb threads %u, ulock word %#x, owner port guess %#x\n", (unsigned)count,
	    owner_word, (owner_word & ~3U) | 3U);
	for (mach_msg_type_number_t i = 0; i < count; i++) {
		thread_identifier_info_data_t id;
		mach_msg_type_number_t id_count = THREAD_IDENTIFIER_INFO_COUNT;
		sb_thread_state64_t st;
		mach_msg_type_number_t st_count = ARM_THREAD_STATE64_COUNT;
		unsigned long long tid = 0;
		bool is_owner = ((threads[i] & ~3U) == (owner_word & ~3U));
		const char *mark = is_owner ? "*OWNER*" : " ";

		if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &id_count) == KERN_SUCCESS) {
			tid = (unsigned long long)id.thread_id;
		}
		if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&st, &st_count) == KERN_SUCCESS) {
			Dl_info info;
			const char *name = "?";
			uintptr_t base = 0;

			if (st.pc != 0 && dladdr((void *)(uintptr_t)st.pc, &info) != 0 && info.dli_fbase != NULL) {
				const char *slash = strrchr(info.dli_fname, '/');

				name = slash ? slash + 1 : info.dli_fname;
				base = (uintptr_t)info.dli_fbase;
			}
			say("hakios: sb thread %u port %#x tid %#llx %s pc %#llx = %s+%#lx (cache %#lx)\n",
			    i, threads[i], tid, mark, (unsigned long long)st.pc, name,
			    (uintptr_t)st.pc - base, (uintptr_t)st.pc - slide);
			if (threads[i] != self_port && (is_owner || ((uintptr_t)st.pc - slide == 0x1c1852ff0UL && i > 0))) {
				say_pc("owner-lr", (uintptr_t)st.lr);
				say_chain((uintptr_t)st.fp);
				if ((uintptr_t)st.pc - slide == 0x1c1852ff0UL) {
					dump_mach_msg(&st);
					probe_port("owner-rname", (mach_port_t)(st.x[5] >> 32));
					probe_port("owner-remote", (mach_port_t)(uint32_t)st.x[3]);
				}
			}
		}
		mach_port_deallocate(mach_task_self(), threads[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_t));
}

extern kern_return_t mach_msg_send(mach_msg_header_t *msg);

static void
sb_wake_reply(uint32_t id, mach_port_t rname)
{
	struct {
		mach_msg_header_t h;
		uint32_t ndr;
		int32_t retcode;
	} r;

	if (rname == MACH_PORT_NULL || rname == MACH_PORT_DEAD) {
		return;
	}
	memset(&r, 0, sizeof r);
	r.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND, 0);
	r.h.msgh_size = sizeof r; /* 32: header + NDR + RetCode */
	r.h.msgh_remote_port = rname;
	r.h.msgh_local_port = MACH_PORT_NULL;
	r.h.msgh_voucher_port = MACH_PORT_NULL;
	r.h.msgh_id = (mach_msg_id_t)id;
	r.ndr = 0;
	r.retcode = (int32_t)0x10000003; /* MIG_BAD_ARGUMENTS: clean error, no out-param parse */
	mach_port_insert_right(mach_task_self(), rname, rname, MACH_MSG_TYPE_MAKE_SEND);
	say("hakios: sb wake: sending err-reply id %#x -> rname %#x\n", id, rname);
	kern_return_t kr = mach_msg_send(&r.h);
	say("hakios: sb wake: sent id %#x rname %#x kr=%d\n", id, rname, kr);
}

static void *
sb_main_watchdog(__unused void *ignored)
{
	uintptr_t last_pc = 0;
	unsigned dumps = 0;
	unsigned lock_dumps = 0;

	for (unsigned i = 0; i < WATCH_SAMPLES; i++) {
		sb_thread_state64_t state;
		mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
		uintptr_t pc, lr, sp, fp;

		usleep(100 * 1000);
		if (thread_get_state(sb_main_thread, ARM_THREAD_STATE64, (thread_state_t)&state, &count) != KERN_SUCCESS) {
			say("hakios: sb main thread: thread_get_state failed at sample %u\n", i);
			break;
		}
		pc = (uintptr_t)state.pc;
		lr = (uintptr_t)state.lr;
		sp = (uintptr_t)state.sp;
		fp = (uintptr_t)state.fp;
		say("hakios: sb t=%.1f sample %u pc %#lx lr %#lx sp %#lx fp %#lx "
		    "x0 %#llx x1 %#llx x2 %#llx x3 %#llx x4 %#llx x5 %#llx x6 %#llx x7 %#llx mainq %u\n",
		    since_boot(), i, pc, lr, sp, fp,
		    (unsigned long long)state.x[0], (unsigned long long)state.x[1],
		    (unsigned long long)state.x[2], (unsigned long long)state.x[3],
		    (unsigned long long)state.x[4], (unsigned long long)state.x[5],
		    (unsigned long long)state.x[6], (unsigned long long)state.x[7], mainq_beats);
		{
			uint64_t opt = state.x[1];
			bool snd = (opt & 1) != 0, rcv = (opt & 2) != 0;
			if (snd && rcv) {
				say("hakios: wdbg i=%u bid=%#x rn=%#x\n", i,
				    (unsigned)(state.x[4] >> 32), (unsigned)(state.x[5] >> 32));
				uint32_t bid = (uint32_t)(state.x[4] >> 32);
				mach_port_t rname = (mach_port_t)(state.x[5] >> 32);
				{
					static unsigned woke;
					if (woke < 400 && rname != MACH_PORT_NULL && rname != MACH_PORT_DEAD) {
						woke++;
						sb_wake_reply(bid, rname);
					}
				}
			}
		}
		if (i == 0) {
			say_console("hakios: sb watchdog console write works\n");
		}
		if (pc != last_pc || i % 5 == 0) {
			say_pc("pc", pc);
			say_pc("lr", lr);
			say_stack(sp);
		}
		if ((pc - slide == 0x1c1852ff0UL || (i % 10) == 0)) {
			say_chain(fp);
		}
		if (pc - slide == 0x1c1852ff0UL) {
			uint64_t opt = state.x[1];
			bool send = (opt & 1) != 0;
			bool recv = (opt & 2) != 0;


			if (send && dumps < 2 && (i % 5) == 0) {
				dumps++;
				dump_mach_msg(&state);
			}
			if (!send && recv && dumps < 3 && (i % 10) == 0) {
				dumps++;
				say("hakios: sb receive t=%.1f sample %u: waiting for a reply\n",
				    since_boot(), i);
				dump_mach_msg(&state);
				probe_port("rname", (mach_port_t)(state.x[5] >> 32));
				probe_port("remote", (mach_port_t)(uint32_t)state.x[3]);
				probe_port("local", (mach_port_t)(uint32_t)(state.x[3] >> 32));
			}
		}
		if (lock_dumps < 2 && pc - slide == 0x1c1853a90UL && (i % 10) == 0) {
			uint32_t word = 0;
			vm_size_t got = 0;

			lock_dumps++;
			say("hakios: sb ulock op %#llx addr %#llx value %#llx timeout %#llx\n",
			    (unsigned long long)state.x[0], (unsigned long long)state.x[1],
			    (unsigned long long)state.x[2], (unsigned long long)state.x[3]);
			if (vm_read_overwrite(mach_task_self(), (vm_address_t)state.x[1], sizeof(word),
			    (vm_address_t)&word, &got) == KERN_SUCCESS) {
				say("hakios: sb ulock word now %#x\n", word);
			}
			dump_threads((unsigned)state.x[2]);
		}
		if ((i & 1) == 0) {
			dispatch_async(dispatch_get_main_queue(), ^{
				mainq_beats++;
				say("hakios: sb mainq serviced t=%.1f beats=%u\n", since_boot(), mainq_beats);
			});
		}
		last_pc = pc;
	}
	return NULL;
}

static void
watch_main_thread(void)
{
	pthread_t thread;
	thread_identifier_info_data_t id;
	mach_msg_type_number_t id_count = THREAD_IDENTIFIER_INFO_COUNT;
	mach_port_t bootstrap = MACH_PORT_NULL;

	sb_main_thread = pthread_mach_thread_np(pthread_self());
	memset(&id, 0, sizeof(id));
	thread_info(sb_main_thread, THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &id_count);
	task_get_bootstrap_port(mach_task_self(), &bootstrap);
	say("hakios: sb main thread: watchdog from %s (main=%d) port %#x tid %#llx bootstrap %#x\n",
	    pthread_main_np() ? "the main thread" : "NOT the main thread", (int)pthread_main_np(),
	    sb_main_thread, (unsigned long long)id.thread_id, bootstrap);
	{
		const char *image = _dyld_get_image_name(0);

		say("hakios: sb image 0 %s slide %#lx, shared cache slide %#lx\n", image ? image : "?",
		    (uintptr_t)_dyld_get_image_vmaddr_slide(0), slide);
	}
	if (pthread_create(&thread, NULL, sb_main_watchdog, NULL) == 0) {
		pthread_detach(thread);
	}
	/*
	 * fix113: the display-services probe is gone from SpringBoard.  fix112 showed that every
	 * reply-carrying send dies with kGUARD_EXC_SEND_INVALID_REPLY naming the reply port, and
	 * launchd then kills SpringBoard -- thirteen spawns, twelve guards, not one line of data.
	 * backboardd runs the same probe now (src/inject.c, watch_service_probe) and restarts
	 * harmlessly, so the window server and the UI stay up while the call shape is worked out.
	 */
}

/* ---- methods of SpringBoard's classes, replaced --------------------------------------------------- */

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

/*
 * Replace -[CLASS SELECTOR] (or +[CLASS SELECTOR]: `kind` is that first character), whose code must now
 * be at the unslid address `expected` (iOS 16.5, 20F66: what a replacement does depends on what the
 * original did). Returns the original.
 * (dyld interposing of the C functions underneath does not reach calls made from inside the
 * shared cache: there one library branches straight into the other.)
 */
static IMP
replace_method_of_kind(char kind, const char *class_name, const char *selector, uintptr_t expected, IMP replacement)
{
	Class cls = objc_getClass(class_name);
	Method method = cls == NULL ? NULL : kind == '+' ? class_getClassMethod(cls, sel_registerName(selector)) :
	    class_getInstanceMethod(cls, sel_registerName(selector));
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != expected + slide) {
		say("hakios: %c[%s %s]: found %#lx, expected %#lx; left alone\n", kind, class_name, selector,
		    original ? (uintptr_t)original - slide : 0, expected);
		return NULL;
	}
	method_setImplementation(method, replacement);
	return original;
}

static IMP
replace_method(const char *class_name, const char *selector, uintptr_t expected, IMP replacement)
{
	return replace_method_of_kind('-', class_name, selector, expected, replacement);
}

/*
 * -[SBLockdownManager _setupActivationState] asks MobileActivation and keeps the answer in the
 * int at +8: 2 = activated, 1 = not, 3 = unknown; it stays 0 when there is no answer.
 */
#define LOCKDOWN_STATE(manager)         (*(int *)((char *)(manager) + 8))
#define LOCKDOWN_ACTIVATED              2

static void
setup_activation_state(id self, __unused SEL selector)
{
	LOCKDOWN_STATE(self) = LOCKDOWN_ACTIVATED;
}

/* -[SBSetupManager _setSetupRequiredReason:]: 0 = no need for the setup assistant */
static void (*original_set_setup_required_reason)(id self, SEL selector, long reason);

static void
set_setup_required_reason(id self, SEL selector, long reason)
{
	if (reason != 0) {
		say("hakios: SpringBoard wants the setup assistant (reason %ld); not here\n", reason);
	}
	original_set_setup_required_reason(self, selector, 0);
}

/*
 * +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:] chooses the file format of the picture
 * SpringBoard takes of an application that leaves the screen: 0 = PNG, 1 and 2 = ATX (an ASTC texture),
 * 3 = JPEG. A device with a wide colour display that can decode ASTC gets ATX, and iOS compresses it on
 * the processor: fast with the vector code of Apple's cores, but here it takes one core more than ten
 * seconds per picture (under QEMU; seconds on the phone) -- and all that time the application waits for
 * the answer, until the watchdog for scene updates (10 s, there is no setting to scale it) kills it:
 * no application survived going to the background. JPEG takes a fraction of a second; it has no
 * transparency, so a picture that needs it becomes a PNG (which is what iOS itself does on a device
 * without ASTC).
 */
#define SNAPSHOT_FORMAT_PNG             0
#define SNAPSHOT_FORMAT_JPEG            3

static long
snapshot_output_format(__unused id self, __unused SEL selector, id snapshot)
{
	SEL opaque = sel_registerName("isImageOpaque");

	if (snapshot != NULL && class_getInstanceMethod(object_getClass(snapshot), opaque) != NULL &&
	    ((BOOL (*)(id, SEL))objc_msgSend)(snapshot, opaque)) {
		return SNAPSHOT_FORMAT_JPEG;
	}
	return SNAPSHOT_FORMAT_PNG;
}

/* ---- battery state from redwood's Qualcomm charger stack ----------------------------------------- */

/*
 * iOS reads battery level through UIDevice.batteryLevel and UIDevice.batteryState; SpringBoard's
 * status bar reads it the same way via SBBatteryController. Where the battery has no power source
 * of its own in the kernel (battery=1, the loader's way through Qualcomm's UEFI charger protocols),
 * we hook the UIDevice accessors to return what XNU publishes as kern.hakios.battery_soc (0..100)
 * and kern.hakios.battery_charging.
 * When the sysctl is missing (battery=1 not in boot-args, or the driver never started) we hand
 * back the value the real implementation computed; the status bar then shows whatever iOS last
 * knew, which is better than crashing.
 * With adsp=1 the battery is a power source (the service HakiosBattery, an IOPMPowerSource:
 * iokit/Kernel/hakios.battery.cpp), which powerd reads like an iPhone's own, and from it iOS knows
 * everything by itself; once that service is there, the hooks hand everything to iOS's own code.
 */
static float  (*original_battery_level)(id self, SEL sel);
static long   (*original_battery_state)(id self, SEL sel);
static BOOL   (*original_battery_view_shows_percentage)(id self, SEL sel);
static void   (*original_battery_view_set_shows_percentage)(id self, SEL sel, BOOL shows);
static BOOL   (*original_battery_view_should_show_bolt)(id self, SEL sel);
static double (*original_battery_view_charge_percent)(id self, SEL sel);
static void   (*original_battery_view_set_charge_percent)(id self, SEL sel, double p);
static double (*original_ring_charging_level)(id self, SEL sel);

static int
sysctl_int(const char *name)
{
	int value = -1;
	size_t size = sizeof(value);

	sysctlbyname(name, &value, &size, NULL, 0);
	return value;
}

/* Is the battery a power source that iOS reads by itself (see above)? Once it is, it stays one. */
static bool
battery_power_source(void)
{
	static bool found;
	io_service_t service;

	if (!found && (service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosBattery"))) != 0) {
		IOObjectRelease(service);
		found = true;
	}
	return found;
}

/* ---- the cellular items of the status bar, from the kernel's modem ------------------------------ */

/*
 * SpringBoard's SBStatusBarStateAggregator fills the raw status bar data from CoreTelephony, which
 * has no baseband here. Its item updaters take the data and a getter and setter for the field
 * (iOS 16.5: -[SBStatusBarStateAggregator _updateSignalStrengthItem:withData:SIMInfo:barsGetter:
 * barsSetter:], and the like for the service string): the signal one is replaced so that, when the
 * kernel's modem is registered (sysctl kern.hakios.cellular_registered, _bars: hakios.mpss.c),
 * the bars are set and the item switched on (-[... _setItem:enabled:]). The aggregator's whole
 * method list is said once in the log, for the posting to be found; and the signal item is asked
 * to update itself every few seconds while the kernel's state changes.
 */
static void (*original_update_signal_item)(id self, SEL sel, long item, void *data, id sim, void *getter, void *setter);
static id cellular_aggregator;
static int cellular_last_bars = -2, cellular_said;

/* The getters and setters are blocks (their type is "@?"): called through the block's own invoke pointer, with the block first. */
struct block_layout {
	void *isa;
	int flags, reserved;
	void (*invoke)(void *, ...);
};
#define BLOCK_CALL(block, type, ...) ((type)((struct block_layout *)(block))->invoke)((block), __VA_ARGS__)

static void
update_signal_item(id self, SEL sel, long item, void *data, id sim, void *getter, void *setter)
{
	int up = sysctl_int("kern.hakios.cellular_up"), registered = sysctl_int("kern.hakios.cellular_registered");
	int bars = sysctl_int("kern.hakios.cellular_bars"), dbm = sysctl_int("kern.hakios.cellular_dbm");
	int shown;

	cellular_aggregator = self;
	if (original_update_signal_item) {
		original_update_signal_item(self, sel, item, data, sim, getter, setter);
	}
	if (cellular_said++ < 8) {
		say("hakios: cellular: signal item %ld: data %p, SIM info %p, getter %p, setter %p; the kernel says up %d registered %d bars %d dbm %d\n",
		    item, data, (void *)sim, getter, setter, up, registered, bars, dbm);
	}
	/*
	 * Force the signal item on. iOS hides it unless its own cellular stack (CommCenter, which is
	 * not here) says there is service, so the bars never appear on their own. The kernel knows
	 * the modem's state, so once it is up the item is shown here with the kernel's bars; when the
	 * kernel has a reading but zero bars (a brief "no signal" sample, or a reading just after
	 * registration) at least one bar is forced so the icon is visibly present rather than empty.
	 * modem_bars_off=1 (as a sysctl) is not honoured; set kern.hakios.cellular_up=0 to hide it.
	 */
	/*
	 * The kernel's NAS state alternates between two reports -- "registered, signal not read" and
	 * "not registered, -86 dBm" -- so at the moments it says registered the dBm reads 0. The last
	 * real reading (within a minute) stands in for it.
	 */
	{
		static int last_dbm;
		static time_t last_dbm_at;
		time_t now = time(NULL);

		if (dbm < 0) {
			last_dbm = dbm;
			last_dbm_at = now;
		} else if (last_dbm < 0 && now - last_dbm_at < 60) {
			dbm = last_dbm;
		}
	}
	shown = bars;
	if (shown <= 0 && dbm < 0) {
		shown = dbm >= -95 ? 4 : dbm >= -105 ? 3 : dbm >= -115 ? 2 : 1;   /* derive from dBm when bars is 0 */
	}
	if (shown <= 0 && (up > 0 || registered > 0)) {
		shown = 1;                      /* up but no reading yet: still show the icon */
	}
	if (shown > 5) {
		shown = 5;
	}
	if ((up > 0 || registered > 0) && shown > 0 && setter != NULL && data != NULL) {
		BLOCK_CALL(setter, void (*)(void *, void *, int), data, shown);
		((void (*)(id, SEL, long, BOOL))objc_msgSend)(self, sel_registerName("_setItem:enabled:"), item, YES);
		if (shown != cellular_last_bars) {
			say("hakios: cellular: %d bars shown (item %ld; kernel bars %d, %d dBm, registered %d)\n", shown, item, bars, dbm, registered);
		}
	}
	cellular_last_bars = shown;
}

static void (*original_update_service_item)(id self, SEL sel, long item, void *data, id sim, id other, void *g1, void *s1, void *g2, void *s2,
    void *g3, void *s3, void *g4, void *s4);

/*
 * The service item ("No SIM", the network's name): once the kernel's modem is registered, the
 * network's name (sysctl kern.hakios.cellular_operator) goes in through the item's string setter
 * and the item is switched on.
 */
static void
update_service_item(id self, SEL sel, long item, void *data, id sim, id other, void *g1, void *s1, void *g2, void *s2, void *g3, void *s3,
    void *g4, void *s4)
{
	static int said;
	static char shown[64];
	char name[64] = "";
	size_t size = sizeof(name) - 1;
	int registered = sysctl_int("kern.hakios.cellular_registered");

	if (original_update_service_item) {
		original_update_service_item(self, sel, item, data, sim, other, g1, s1, g2, s2, g3, s3, g4, s4);
	}
	sysctlbyname("kern.hakios.cellular_operator", name, &size, NULL, 0);
	if (said++ < 4) {
		say("hakios: cellular: service item %ld: data %p, SIM info %p, other %p; string getter %p setter %p, content type getter %p "
		    "setter %p; the kernel's network \"%s\"\n", item, data, (void *)sim, (void *)other, g1, s1, g3, s3, name);
	}
	if (registered > 0 && name[0] != 0 && s1 != NULL && data != NULL) {
		CFStringRef string = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);

		if (string != NULL) {
			BLOCK_CALL(s1, void (*)(void *, void *, CFStringRef), data, string);
			((void (*)(id, SEL, long, BOOL))objc_msgSend)(self, sel_registerName("_setItem:enabled:"), item, YES);
			if (strcmp(shown, name) != 0) {
				snprintf(shown, sizeof(shown), "%s", name);
				say("hakios: cellular: service \"%s\" shown (item %ld)\n", name, item);
			}
			CFRelease(string);
		}
	}
}

/*
 * The bars are drawn by UIKit's signal view, whose "signal mode" says how: inactive (all bars grey),
 * searching, or active (the bars that count dark). iOS's cellular stack (CommCenter, not here)
 * puts it in active mode once there is service; without it the bars stay grey whatever their
 * number. So for the cellular signal view (not the Wi-Fi one), while the kernel says the modem is
 * registered, the mode is made active. The modes seen are said in the log once each.
 */
static void (*original_set_signal_mode)(id self, SEL sel, long mode);
static long cellular_active_mode = 2;  /* _UIStatusBarSignalView: 0 inactive, 1 searching, 2 active */

static void
set_signal_mode(id self, SEL sel, long mode)
{
	static unsigned said_modes;
	const char *name = object_getClassName(self);
	bool cellular = name != NULL && strstr(name, "Cellular") != NULL;
	long wanted = mode;

	if (cellular && mode < 8 && !(said_modes & (1u << mode))) {
		said_modes |= 1u << mode;
		say("hakios: cellular: %s asked for signal mode %ld\n", name, mode);
	}
	if (cellular && sysctl_int("kern.hakios.cellular_registered") > 0 && mode != cellular_active_mode) {
		wanted = cellular_active_mode;
	}
	original_set_signal_mode(self, sel, wanted);
}

static void
signal_view_hook(void)
{
	static const char *const classes[] = { "_UIStatusBarCellularSignalView", "_UIStatusBarSignalView" };

	for (unsigned c = 0; c < sizeof(classes) / sizeof(classes[0]); c++) {
		Class cls = objc_getClass(classes[c]);
		Method method = cls ? class_getInstanceMethod(cls, sel_registerName("setSignalMode:")) : NULL;

		if (method == NULL) {
			say("hakios: cellular: no -[%s setSignalMode:]\n", classes[c]);
			continue;
		}
		original_set_signal_mode = (void (*)(id, SEL, long))method_getImplementation(method);
		method_setImplementation(method, (IMP)set_signal_mode);
		say("hakios: cellular: -[%s setSignalMode:] replaced (types \"%s\"): active while the kernel says registered\n",
		    classes[c], method_getTypeEncoding(method));
		return;         /* the base class's method serves the subclasses */
	}
}

static void
cellular_hook(void)
{
	signal_view_hook();
	{
		const char *service_selector = "_updateServiceItem:withData:SIMInfo:otherSIMInfo:serviceStringGetter:serviceStringSetter:"
		    "serviceCrossfadeStringGetter:serviceCrossfadeStringSetter:serviceContentTypeGetter:serviceContentTypeSetter:"
		    "serviceBadgeStringGetter:serviceBadgeStringSetter:";
		Class cls = objc_getClass("SBStatusBarStateAggregator");
		Method method = cls ? class_getInstanceMethod(cls, sel_registerName(service_selector)) : NULL;

		if (method) {
			say("hakios: cellular: -[SBStatusBarStateAggregator %s] at %#lx, types \"%s\": looked at\n", service_selector,
			    (uintptr_t)method_getImplementation(method) - slide, method_getTypeEncoding(method));
			original_update_service_item = (void (*)(id, SEL, long, void *, id, id, void *, void *, void *, void *, void *, void *, void *,
			    void *))method_getImplementation(method);
			method_setImplementation(method, (IMP)update_service_item);
		}
	}
	const char *selector = "_updateSignalStrengthItem:withData:SIMInfo:barsGetter:barsSetter:";
	Class cls = objc_getClass("SBStatusBarStateAggregator");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

	if (method == NULL) {
		say("hakios: cellular: no -[SBStatusBarStateAggregator %s]\n", selector);
		return;
	}
	say("hakios: cellular: -[SBStatusBarStateAggregator %s] at %#lx, types \"%s\": replaced\n", selector,
	    (uintptr_t)method_getImplementation(method) - slide, method_getTypeEncoding(method));
	original_update_signal_item = (void (*)(id, SEL, long, void *, id, void *, void *))method_getImplementation(method);
	method_setImplementation(method, (IMP)update_signal_item);
	/* every 3 s: when the kernel's registration or bars change, the item is asked to update itself */
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());

	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), 3 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
	dispatch_source_set_event_handler(timer, ^{
		static int last_registered = -2, last_bars = -2;
		int registered = sysctl_int("kern.hakios.cellular_registered"), bars = sysctl_int("kern.hakios.cellular_bars");

		if ((registered != last_registered || bars != last_bars) && cellular_aggregator != nil) {
			SEL update = sel_registerName("_updateSignalStrengthItem");

			last_registered = registered;
			last_bars = bars;
			if (class_getInstanceMethod(object_getClass(cellular_aggregator), update)) {
				((void (*)(id, SEL))objc_msgSend)(cellular_aggregator, update);
				say("hakios: cellular: the signal item asked to update (registered %d, bars %d)\n", registered, bars);
			}
		}
	});
	dispatch_resume(timer);
}

/* UIDevice.batteryLevel: 0.0..1.0; -1.0 if monitoring not enabled */
static float
battery_level(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc < 0 || soc > 100 || battery_power_source()) {
		return original_battery_level ? original_battery_level(self, sel) : -1.0f;
	}
	return (float)soc / 100.0f;
}

/* UIDevice.batteryState: 0=unknown 1=unplugged 2=charging 3=full */
static long
battery_state(id self, SEL sel)
{
	int charging = sysctl_int("kern.hakios.battery_charging");
	int present = sysctl_int("kern.hakios.battery_present");
	int plugged = sysctl_int("kern.hakios.battery_charger_present");
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (charging == -1 || present == -1 || plugged == -1 || soc < 0 || soc > 100 || battery_power_source()) {
		return original_battery_state ? original_battery_state(self, sel) : 0;
	}
	if (!present) return 0;
	if (plugged && soc >= 100) return 3;
	return charging ? 2 : 1;
}

/*
 * UIKitCore's status-bar view has its own presentation policy, separate from UIDevice. With the power
 * source it is iOS's own again: the switch Battery Percentage in Settings > Battery, which the image
 * turns on (SBShowBatteryPercentage in userland/var/mobile/Library/Preferences/com.apple.springboard.plist).
 */
static BOOL
battery_view_shows_percentage(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return YES;
	return original_battery_view_shows_percentage ? original_battery_view_shows_percentage(self, sel) : NO;
}

static void
battery_view_set_shows_percentage(id self, SEL sel, BOOL shows)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	original_battery_view_set_shows_percentage(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? YES : shows);
}

static BOOL
battery_view_should_show_bolt(id self, SEL sel)
{
	int charging = sysctl_int("kern.hakios.battery_charging");
	int present = sysctl_int("kern.hakios.battery_present");
	int plugged = sysctl_int("kern.hakios.battery_charger_present");

	if (charging != -1 && present != -1 && plugged != -1 && !battery_power_source()) return charging && present && plugged;
	return original_battery_view_should_show_bolt ? original_battery_view_should_show_bolt(self, sel) : NO;
}

/*
 * The actual number the status bar draws. _UIBatteryView keeps the fill fraction (0.0..1.0) in
 * chargePercent; it is set through setChargePercent: and read back for layout. iOS feeds it from
 * BatteryCenter / IOPowerSources, which is empty on this phone, so without us it sits near zero
 * (the "1%" the user saw). We override both sides: setChargePercent: so the stored value is ours
 * no matter who writes it, and the getter so any layout path that reads it agrees.
 */
static double
battery_view_charge_percent(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
	return original_battery_view_charge_percent ? original_battery_view_charge_percent(self, sel) : 0.0;
}

static void
battery_view_set_charge_percent(id self, SEL sel, double p)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	original_battery_view_set_charge_percent(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? (double)soc / 100.0 : p);
}

/* The big charging ring on the lock screen (CoverSheet) reads its fill level the same way. */
static double
ring_charging_level(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
	return original_ring_charging_level ? original_ring_charging_level(self, sel) : 0.0;
}

/* Keep SpringBoard's normal battery observers awake while the XNU cache changes. */
static void
battery_post_notifications(void)
{
	Class uidev = objc_getClass("UIDevice");
	Class notification_center = objc_getClass("NSNotificationCenter");
	id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, sel_registerName("currentDevice")) : nil;
	id center = notification_center ? ((id (*)(Class, SEL))objc_msgSend)(notification_center,
	    sel_registerName("defaultCenter")) : nil;

	if (center != nil && !battery_power_source()) {
		SEL post = sel_registerName("postNotificationName:object:");
		((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
		    (id)CFSTR("UIDeviceBatteryLevelDidChangeNotification"), device);
		((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
		    (id)CFSTR("UIDeviceBatteryStateDidChangeNotification"), device);
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		battery_post_notifications();
	});
}

/* ---- the lock screen's clock without a GPU ------------------------------------------------------- */

#define HAKIOS_GL "/redwood/lib/libhakios.gl.dylib"

/* Does the window server draw with OpenGL ES? The same question, asked the same way, as opengl_es() in inject.c. */
static bool
gpu_draws(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;
	io_service_t gpu;

	if (access(HAKIOS_GL, R_OK) != 0) {
		return false;
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
			if (argument == arguments || argument[-1] == ' ') {
				return argument[3] != '0';
			}
		}
	}
	gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
	if (gpu != 0) {
		IOObjectRelease(gpu);
	}
	return gpu != 0;
}

/*
 * The clock of the lock screen is a "vibrant" view (BSUIVibrancyEffectView in BaseBoardUI): its
 * digits are not drawn, they are a mask -- a copy of them through a colour matrix that keeps red
 * as alpha -- through which the filter vibrantColorMatrix lets the wallpaper behind come out
 * lighter; and a gradient over the whole screen, red at the top, tones the wallpaper down the
 * same way. On the GPU that is what is seen (eagl.c draws the two filters). CoreAnimation's
 * software renderer shows neither: the digits come out red, and the top of the wallpaper as it
 * was. Why was not found out; that renderer is in QuartzCore, not here.
 * Such views have a switch, the user default BSUIVibrancyEffectViewDisabled: with it they show
 * their content as it is drawn, the clock in plain white. It is set for this run of SpringBoard
 * alone (nothing is written), and only where the software renderer draws.
 */
static void
plain_vibrancy(void)
{
	if (gpu_draws()) {
		return;
	}
	CFPreferencesSetAppValue(CFSTR("BSUIVibrancyEffectViewDisabled"), kCFBooleanTrue, kCFPreferencesCurrentApplication);
	say("hakios: SpringBoard: no GPU draws, vibrant views are plain (the lock screen's clock is white)\n");
}

/* ---- the side button's long press ---------------------------------------------------------------- */

/*
 * -[SBLockHardwareButton longPress:] is what the side button's long press recognizer calls: Siri
 * (here, its "turn on Siri" sheet, nobody having turned it on). The recognizer is a timer of
 * SpringBoard's own: started when the main thread sees the press, stopped when it sees the release
 * -- and a release it has not got round to is no release. A press of the side button with the
 * display off wakes the display from inside the handling of that very press, which on an iPhone
 * takes no time. Here it takes as long as the software renderer needs for the first picture of the
 * lock screen: a tenth of a second on the phone, a second under QEMU. That is longer than a long
 * press, so there every wake by the button (and by a tap, which is a press of that button made in
 * inject.c) ended with Siri's sheet over the lock screen, however short the press had been.
 * Where the button's events come from -- inject.c, in backboardd -- it is known whether the button
 * is still held; a long press of a button that is not is dropped here.
 */
#define GESTURE_BEGAN                   1       /* UIGestureRecognizerState */
#define GESTURE_ENDED                   3       /* and cancelled, failed after it */

static void (*original_lock_long_press)(id self, SEL selector, id recognizer);

static bool
lock_button_held(void)
{
	static int token = NOTIFY_TOKEN_INVALID;
	uint64_t state = 0;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.hakios.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return true;
	}
	/* 1 = released; held, or not known: as SpringBoard has it */
	return notify_get_state(token, &state) != NOTIFY_STATUS_OK || state != 1;
}

static void
lock_long_press(id self, SEL selector, id recognizer)
{
	static bool dropping;
	SEL state_selector = sel_registerName("state");
	long state = recognizer != NULL && class_getInstanceMethod(object_getClass(recognizer), state_selector) != NULL ?
	    ((long (*)(id, SEL))objc_msgSend)(recognizer, state_selector) : 0;

	if (state == GESTURE_BEGAN) {
		dropping = !lock_button_held();
		if (dropping) {
			say("hakios: SpringBoard: a long press of the side button, which was released long ago: dropped\n");
		}
	}
	if (dropping) {
		if (state >= GESTURE_ENDED) {
			dropping = false;
		}
		return;
	}
	original_lock_long_press(self, selector, recognizer);
}

/* ---- remote control ------------------------------------------------------------------------------ */

#define UNLOCK_SOURCE_COVER_SHEET       24      /* what the lock screen itself passes */

static void
unlock(void)
{
	Class cls = objc_getClass("SBLockScreenManager");
	SEL shared = sel_registerName("sharedInstance"), locked = sel_registerName("isUILocked");
	SEL unlock_ui = sel_registerName("unlockUIFromSource:withOptions:");
	id manager;

	if (cls == NULL || class_getClassMethod(cls, shared) == NULL || class_getInstanceMethod(cls, locked) == NULL ||
	    class_getInstanceMethod(cls, unlock_ui) == NULL) {
		say("hakios: unlock: SBLockScreenManager is not what it was\n");
		return;
	}
	manager = ((id (*)(Class, SEL))objc_msgSend)(cls, shared);
	if (manager == NULL) {
		say("hakios: unlock: no lock screen manager yet\n");
	} else if (!((BOOL (*)(id, SEL))objc_msgSend)(manager, locked)) {
		say("hakios: unlock: not locked\n");
	} else {
		BOOL done = ((BOOL (*)(id, SEL, long, id))objc_msgSend)(manager, unlock_ui, UNLOCK_SOURCE_COVER_SHEET, NULL);

		say("hakios: unlock: asked, answer %d\n", done);
	}
}

/*
 * The poster switcher: on iOS 16 a long press on the lock screen's wallpaper shrinks it into a
 * row of posters to choose from or customize. The row is PosterBoard's, an application scene of
 * that process which the lock screen hosts (CSPosterSwitcherViewController, a CALayerHost of the
 * context of PosterBoard's window over an opaque black layer). PosterBoard does lay it out -- its
 * window has the posters, "Customize" and "+", the hosted context is that window's, the scene is
 * in the foreground -- but the window server draws nothing of that context, with either
 * renderer, so what is left is the black layer under it, until a swipe up takes the switcher
 * away. Why is not known: the display is not in secure mode, SpringBoard counts the user as
 * authenticated, the same nesting of hosted contexts works for the wallpaper itself.
 * Until it is, the switcher is not offered, the way SpringBoard itself withholds it where it has
 * none (an iPad, Guided Access): its policy says the capability is not there, and the long press
 * does nothing.
 */
static BOOL
poster_switcher_allowed(__unused id self, __unused SEL selector, id *explanation)
{
	if (explanation != NULL) {
		*explanation = (id)CFSTR("the poster switcher is not shown here (see inject_sb.c)");
	}
	return NO;
}

/*
 * What SpringBoard has put on the screen, as CoreAnimation has it: every layer of every window, a
 * line each -- class and name, place, opacity, what it shows (an image, a colour, another
 * process's layers), and what is done to it: its filters by type with their inputs, in order,
 * the compositing filter, the mask. That is where a picture that comes out wrong on one renderer
 * and right on another is described: the same description goes to both.
 */
#define SEND(type, object, name)        ((type (*)(id, SEL))objc_msgSend)((id)(object), sel_registerName(name))
#define SEND1(type, object, name, argument_type, argument) \
	((type (*)(id, SEL, argument_type))objc_msgSend)((id)(object), sel_registerName(name), argument)

static const char *layers_of = "";               /* in another program than SpringBoard: " of" and its name */

static const char *
text_of(id object)
{
	id text = object ? SEND(id, object, "description") : NULL;

	return text ? SEND(const char *, text, "UTF8String") : "-";
}

/*
 * The switcher of running applications lays a wash of wallpaper over every card behind the front
 * one, above the dimming: -[SBAppSwitcherPageView _createOrDestroyWallpaperOverlayView] makes it,
 * PaperBoardUI fills it from the image provider of the wallpaper, and the result is drawn at about
 * fifteen per cent (the layer tree: SBWallpaperEffectView at opacity 0.1497 -> PBUIFakeBlurView ->
 * PBUIWallpaperView -> PBUISnapshotReplicaView -> a UIImageView).
 *
 * Here there is no picture of the wallpaper to put in it. The wallpaper itself is drawn live by
 * PosterBoard, in another process, and hosted into SpringBoard's layer tree; SpringBoard has no
 * bitmap of it, and nothing ever bakes one -- after three and a half minutes of an idle phone
 * /var/mobile/Library holds sixty directories and not one of them is PaperBoard, PosterBoard or
 * Wallpaper. So the provider hands over a placeholder: 846 x 1827 pixels of one colour,
 * (179, 77, 179), which CoreAnimation compresses to ASTC and draws over the two cards on the left
 * at 14.9 per cent. That is what made them purple instead of merely dimmed.
 *
 * The dimming itself is right and stays: two draws of a flat black at 11.4 per cent, which is the
 * shadow one expects to see. Only the wash is left out, because a wash of a colour that is not the
 * wallpaper's is worse than no wash at all. When something here can give PaperBoardUI a real
 * picture of the wallpaper, this hook is what should go -- and until then switcher_wallpaper=1
 * among the boot-args puts it back as iOS asks for it, so that whoever works on the wallpaper can
 * see at once whether their picture arrives, without rebuilding an image to find out.
 */
static void (*original_switcher_wallpaper_overlay)(id self, SEL selector);

static void
switcher_wallpaper_overlay(id self, SEL selector)
{
	static int wanted = -1;         /* boot-arg switcher_wallpaper=1: have it back, placeholder and all */
	static unsigned told;

	if (wanted < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		wanted = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
		    (at = strstr(args, "switcher_wallpaper=1")) != NULL && (at == args || at[-1] == ' ');
	}
	if (told++ == 0) {
		say("hakios: the switcher's wash of wallpaper is %s\n", wanted ?
		    "drawn as iOS asks (switcher_wallpaper=1)" : "left out: there is no picture of the wallpaper for it");
	}
	if (wanted) {
		original_switcher_wallpaper_overlay(self, selector);
	}
}

static void
layer_filter(id filter, const char *what, int depth)
{
	static const char *const inputs[] = { "inputColorMatrix", "inputRadius", "inputAmount", "inputColor", "inputAngle",
		                              "inputPremultipliedValues", "inputNormalizeEdges", "inputReversed" };
	char line[1024];
	size_t n;

	if (!class_respondsToSelector(object_getClass(filter), sel_registerName("type"))) {
		say("hakios: layers%s: %*s  %s %s\n", layers_of, 2 * depth, "", what, text_of(filter));     /* a name alone */
		return;
	}
	n = (size_t)snprintf(line, sizeof(line), "%s %s \"%s\"%s", what, text_of(SEND(id, filter, "type")),
	    text_of(SEND(id, filter, "name")), SEND(BOOL, filter, "isEnabled") ? "" : " (disabled)");
	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]) && n < sizeof(line); i++) {
		id key = SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, inputs[i]);
		id value = SEND1(id, filter, "valueForKey:", id, key);
		float m[20];

		if (value == NULL) {
			continue;
		}
		if (i == 0 && class_respondsToSelector(object_getClass(value), sel_registerName("CAColorMatrixValue"))) {
			SEND1(void, value, "getValue:", void *, m);
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %s [%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g | "
			    "%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g]", inputs[i], m[0], m[1], m[2], m[3], m[4], m[5], m[6],
			    m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], m[16], m[17], m[18], m[19]);
		} else {
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %s %s", inputs[i], text_of(value));
		}
	}
	say("hakios: layers%s: %*s  %s\n", layers_of, 2 * depth, "", line);
}

static void
layer_line(id layer, const char *role, int depth)
{
	struct { double x, y, w, h; } frame = SEND(typeof(frame), layer, "frame");
	const char *class_name = object_getClassName(layer);
	id contents = SEND(id, layer, "contents"), filters = SEND(id, layer, "filters"), mask = SEND(id, layer, "mask");
	id compositing = SEND(id, layer, "compositingFilter"), sublayers = SEND(id, layer, "sublayers");
	id background = SEND(id, layer, "backgroundFilters");
	void *colour = SEND(void *, layer, "backgroundColor");
	char more[1024] = "";
	size_t n = 0;

	if (SEND(BOOL, layer, "isHidden")) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " HIDDEN");
	}
	if (contents != NULL) {
		id text = (id)CFCopyDescription((CFTypeRef)contents);
		const char *from = text_of(text);
		char flat[300];
		size_t k = 0;

		/*
		 * What a CGImage says about itself runs over several lines -- its colour space, how big
		 * it is, how its pixels are laid out -- and all of that is worth having on the one line,
		 * because the size and the colour space are the first things to look at when a picture
		 * comes out wrong. Every run of spaces and newlines becomes one space.
		 */
		while (from != NULL && *from != 0 && k + 1 < sizeof(flat)) {
			if (*from == ' ' || *from == '\n' || *from == '\t' || *from == '\r') {
				if (k > 0 && flat[k - 1] != ' ') {
					flat[k++] = ' ';
				}
				from++;
				continue;
			}
			flat[k++] = *from++;
		}
		flat[k] = 0;
		n += (size_t)snprintf(more + n, sizeof(more) - n, " contents %s", flat);
		CFRelease((CFTypeRef)text);
	}
	if (contents != NULL && class_respondsToSelector(object_getClass(layer), sel_registerName("contentsMultiplyColor")) &&
	    SEND(void *, layer, "contentsMultiplyColor") != NULL) {
		id text = (id)CFCopyDescription(SEND(void *, layer, "contentsMultiplyColor"));
		const char *numbers = strstr(text_of(text), "( ");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " times %.40s", numbers ? numbers : "?");
		CFRelease((CFTypeRef)text);
	}
	if (colour != NULL) {
		id text = (id)CFCopyDescription(colour);
		const char *numbers = strstr(text_of(text), "( ");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " colour %.40s", numbers ? numbers : "?");
		CFRelease((CFTypeRef)text);
	}
	if (strcmp(class_name, "CAPortalLayer") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " PORTAL of %p%s%s", (void *)SEND(id, layer, "sourceLayer"),
		    SEND(BOOL, layer, "hidesSourceLayer") ? " (hides it)" : "", SEND(BOOL, layer, "matchesOpacity") ? " (its opacity)" : "");
	} else if (strcmp(class_name, "CABackdropLayer") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " BACKDROP group \"%s\" scale %g%s", text_of(SEND(id, layer, "groupName")),
		    SEND(double, layer, "scale"), SEND(BOOL, layer, "captureOnly") ? " (capture only)" : "");
	} else if (strcmp(class_name, "CAGradientLayer") == 0) {
		struct { double x, y; } from = SEND(typeof(from), layer, "startPoint"), to = SEND(typeof(to), layer, "endPoint");
		id colours = SEND(id, layer, "colors");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " GRADIENT %s from %g,%g to %g,%g at %.40s:", text_of(SEND(id, layer, "type")),
		    from.x, from.y, to.x, to.y, text_of(SEND(id, layer, "locations")));
		for (long i = 0; colours != NULL && i < SEND(long, colours, "count") && n < sizeof(more); i++) {
			id text = (id)CFCopyDescription((CFTypeRef)SEND1(id, colours, "objectAtIndex:", long, i));
			const char *numbers = strstr(text_of(text), "( ");

			n += (size_t)snprintf(more + n, sizeof(more) - n, " %.30s", numbers ? numbers : "?");
			CFRelease((CFTypeRef)text);
		}
	} else if (strcmp(class_name, "CAShapeLayer") == 0) {
		void *stroke = SEND(void *, layer, "strokeColor"), *fill = SEND(void *, layer, "fillColor");
		id text_stroke = stroke ? (id)CFCopyDescription(stroke) : NULL, text_fill = fill ? (id)CFCopyDescription(fill) : NULL;
		const char *ns = text_stroke ? strstr(text_of(text_stroke), "( ") : NULL, *nf = text_fill ? strstr(text_of(text_fill), "( ") : NULL;

		n += (size_t)snprintf(more + n, sizeof(more) - n, " SHAPE line %g stroke %.30s fill %.30s ends %g-%g", SEND(double, layer,
		    "lineWidth"), ns ? ns : "-", nf ? nf : "-", SEND(double, layer, "strokeStart"), SEND(double, layer, "strokeEnd"));
		if (text_stroke != NULL) {
			CFRelease((CFTypeRef)text_stroke);
		}
		if (text_fill != NULL) {
			CFRelease((CFTypeRef)text_fill);
		}
	} else if (strcmp(class_name, "CALayerHost") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " HOST of context %#x%s%s", SEND(unsigned, layer, "contextId"),
		    SEND(BOOL, layer, "inheritsSecurity") ? " (inherits security)" : "",
		    SEND(BOOL, layer, "rendersAsynchronously") ? " (asynchronously)" : "");
	}
	{
		/* the view a layer belongs to, which is the only thing that names a layer UIKit left
		   unnamed -- and the switcher's wash of wallpaper is such a layer */
		id delegate = SEND(id, layer, "delegate");

		if (delegate != NULL) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, " of %s", object_getClassName(delegate));
		}
	}
	{
		id keys = SEND(id, layer, "animationKeys");

		if (SEND(BOOL, layer, "shouldRasterize")) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, " RASTERIZED");
		}
		for (long i = 0; keys != NULL && i < SEND(long, keys, "count") && n < sizeof(more); i++) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, "%s%s", i == 0 ? " ANIMATIONS " : ",",
			    text_of(SEND1(id, keys, "objectAtIndex:", long, i)));
		}
	}
	say("hakios: layers%s: %*s%s%s %p \"%s\" %g,%g %gx%g opacity %g%s\n", layers_of, 2 * depth, "", role, class_name, (void *)layer,
	    text_of(SEND(id, layer, "name")), frame.x, frame.y, frame.w, frame.h, (double)SEND(float, layer, "opacity"), more);
	for (long i = 0; filters != NULL && i < SEND(long, filters, "count"); i++) {
		layer_filter(SEND1(id, filters, "objectAtIndex:", long, i), "filter", depth);
	}
	for (long i = 0; background != NULL && i < SEND(long, background, "count"); i++) {
		layer_filter(SEND1(id, background, "objectAtIndex:", long, i), "background filter", depth);
	}
	if (compositing != NULL) {
		layer_filter(compositing, "compositing filter", depth);
	}
	if (mask != NULL) {
		layer_line(mask, "MASK ", depth + 1);
	}
	for (long i = 0; sublayers != NULL && i < SEND(long, sublayers, "count"); i++) {
		layer_line(SEND1(id, sublayers, "objectAtIndex:", long, i), "", depth + 1);
	}
}

/* Every WKWebView under a view: whether WebKit takes it for one in the background, its web process and whether that answers. */
static void
web_views(id view, int depth)
{
	Class web = objc_getClass("WKWebView");
	id subviews;
	bool is_web = false;

	if (view == NULL || depth > 40) {
		return;
	}

	for (Class c = object_getClass(view); web != NULL && c != NULL && !is_web; c = class_getSuperclass(c)) {
		is_web = c == web;
	}
	if (is_web) {
		SEL background = sel_registerName("_isBackground"), pid = sel_registerName("_webProcessIdentifier");
		SEL responsive = sel_registerName("_webProcessIsResponsive");

		say("hakios: layers%s: a WKWebView %p: in the background %s, web process %d, responsive %s, in a window %s, hidden %s\n",
		    layers_of, (void *)view, class_respondsToSelector(object_getClass(view), background) ?
		    (SEND(BOOL, view, "_isBackground") ? "YES" : "no") : "?", class_respondsToSelector(object_getClass(view), pid) ?
		    SEND(int, view, "_webProcessIdentifier") : -1, class_respondsToSelector(object_getClass(view), responsive) ?
		    (SEND(BOOL, view, "_webProcessIsResponsive") ? "yes" : "NO") : "?", SEND(id, view, "window") ? "yes" : "NO",
		    SEND(BOOL, view, "isHidden") ? "YES" : "no");
	}
	subviews = SEND(id, view, "subviews");
	for (long i = 0; subviews != NULL && i < SEND(long, subviews, "count"); i++) {
		web_views(SEND1(id, subviews, "objectAtIndex:", long, i), depth + 1);
	}
}

static void
layers(void)
{
	Class window = objc_getClass("UIWindow");
	SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
	id windows = window && class_getClassMethod(window, all) ? ((id (*)(Class, SEL, BOOL, BOOL))objc_msgSend)(window, all, 1, 1) : NULL;
	id application = SEND(id, objc_getClass("UIApplication"), "sharedApplication");
	id authentication = application && class_respondsToSelector(object_getClass(application),
	    sel_registerName("authenticationController")) ? SEND(id, application, "authenticationController") : NULL;

	say("hakios: layers%s: %ld windows\n", layers_of, windows ? SEND(long, windows, "count") : 0L);
	if (authentication != NULL) {
		say("hakios: layers: the user is %sauthenticated, %s passcode, the display %sin secure mode\n",
		    SEND(BOOL, authentication, "isAuthenticated") ? "" : "NOT ", SEND(BOOL, authentication, "hasPasscodeSet") ? "a" : "no",
		    SEND(BOOL, authentication, "inSecureDisplayMode") ? "" : "not ");
	}
	for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
		id w = SEND1(id, windows, "objectAtIndex:", long, i);
		id scene = class_respondsToSelector(object_getClass(w), sel_registerName("windowScene")) ? SEND(id, w, "windowScene") : NULL;
		id context = class_respondsToSelector(object_getClass(w), sel_registerName("_boundContext")) ? SEND(id, w, "_boundContext") : NULL;

		say("hakios: layers%s: window %s level %g, context %#x (%ssecure, %u commits), scene %s (activation state %ld)\n",
		    layers_of, object_getClassName(w), SEND(double, w, "windowLevel"), context ? SEND(unsigned, context, "contextId") : 0,
		    context && SEND(BOOL, context, "isSecure") ? "" : "not ", context ? (unsigned)SEND(unsigned long long, context, "commitId") : 0,
		    scene ? text_of(SEND(id, scene, "_persistenceIdentifier")) : "none", scene ? SEND(long, scene, "activationState") : -1L);
		layer_line(SEND(id, w, "layer"), "", 1);
	}
	if (application != NULL) {
		say("hakios: layers%s: the application's state %ld (0 active, 1 inactive, 2 in the background)\n", layers_of,
		    SEND(long, application, "applicationState"));
	}
	for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
		web_views(SEND1(id, windows, "objectAtIndex:", long, i), 0);
	}
	say("hakios: layers%s: end\n", layers_of);
}

/*
 * fix160: trace the CoreAnimation contexts SpringBoard asks for.  The window server reaches
 * its display, but no CA update ever arrives; the question is whether SpringBoard creates a
 * remote context at all, and what options (display id, definesDisplayBounds) it passes.
 */
static IMP sb_orig_cacontext_local_opts;
static IMP sb_orig_cacontext_remote_opts;

static void
sb_say_cacontext_options(const char *what, CFDictionaryRef options)
{
	char text[900] = "nil";

	if (options != NULL) {
		CFStringRef d = CFCopyDescription(options);

		if (d != NULL) {
			CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(d);
		}
	}
	say("hakios: SB CAContext %s options: %s\n", what, text);
}

static void
sb_say_cacontext_result(id ctx)
{
	unsigned context_id = 0, display_id = 0;
	Class cls;

	if (ctx == nil) {
		say("hakios: SB CAContext -> nil\n");
		return;
	}
	cls = object_getClass(ctx);
	if (class_getInstanceMethod(cls, sel_registerName("contextId")) != NULL) {
		context_id = SEND(unsigned, ctx, "contextId");
	}
	if (class_getInstanceMethod(cls, sel_registerName("displayId")) != NULL) {
		display_id = SEND(unsigned, ctx, "displayId");
	}
	say("hakios: SB CAContext -> %p contextId=%u displayId=%u\n", (void *) ctx, context_id, display_id);
}

static id
sb_cacontext_local_opts(id self, SEL _cmd, CFDictionaryRef options)
{
	id ctx;

	sb_say_cacontext_options("localContextWithOptions:", options);
	ctx = ((id (*)(id, SEL, CFDictionaryRef)) sb_orig_cacontext_local_opts)(self, _cmd, options);
	sb_say_cacontext_result(ctx);
	return ctx;
}

static id
sb_cacontext_remote_opts(id self, SEL _cmd, CFDictionaryRef options)
{
	CFMutableDictionaryRef patched = NULL;
	id ctx;

	sb_say_cacontext_options("remoteContextWithOptions:", options);
	if (options != NULL) {
		patched = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, options);
	}
	if (patched == NULL) {
		patched = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
		    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	if (patched != NULL) {
		int one = 1;
		CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &one);

		if (n != NULL) {
			CFDictionarySetValue(patched, CFSTR("kCAContextDisplayId"), n);
			CFRelease(n);
		}
		CFDictionarySetValue(patched, CFSTR("secure"), kCFBooleanFalse);
		say("hakios: SB CAContext remote options + kCAContextDisplayId=1, secure=0\n");
	}
	ctx = ((id (*)(id, SEL, CFDictionaryRef)) sb_orig_cacontext_remote_opts)(self, _cmd,
	    patched != NULL ? patched : options);
	if (patched != NULL) {
		CFRelease(patched);
	}
	sb_say_cacontext_result(ctx);
	return ctx;
}

static void
sb_install_cacontext_trace(void)
{
	Class cls = objc_getClass("CAContext");
	Method m;

	if (cls == NULL) {
		say("hakios: SB CAContext class not found\n");
		return;
	}
	m = class_getClassMethod(cls, sel_registerName("localContextWithOptions:"));
	if (m != NULL) {
		sb_orig_cacontext_local_opts = method_setImplementation(m, (IMP) sb_cacontext_local_opts);
	}
	m = class_getClassMethod(cls, sel_registerName("remoteContextWithOptions:"));
	if (m != NULL) {
		sb_orig_cacontext_remote_opts = method_setImplementation(m, (IMP) sb_cacontext_remote_opts);
	}
	say("hakios: SB CAContext trace installed (local=%d remote=%d)\n",
	    sb_orig_cacontext_local_opts != NULL, sb_orig_cacontext_remote_opts != NULL);
}

/*
 * The switcher of running applications, on the notification com.hakios.switcher (rwtouch notify).
 * A drag up from the bottom edge is the way to it on a phone, but the touches a script puts in do
 * not reach iOS's system gestures -- nothing comes of such a drag, not even going home -- and two
 * presses of the home button only go home (there is no home button on a phone of this shape, and
 * what is sent as one goes home and no more). SpringBoard opens it itself for anything that is
 * not a gesture (a hardware keyboard, accessibility): SBMainSwitcherControllerCoordinator, which
 * is one for the whole program, takes "activate the main switcher not interactively, from this
 * source". The source is an analytics label; zero is "none", which is what the ones that have no
 * gesture behind them pass.
 * As it stands this is not enough: with three applications running it answers that it took the
 * request (source 0, animated and not animated alike) and the screen does not change -- the
 * application in front stays, and SpringBoard says nothing more. So the switcher still cannot be
 * reached from a script; what is left to try is the transition with a window scene
 * (_activateSwitcherValidatorWithEventLabel:animated:windowScene:), which is what its own
 * gestures use.
 */
static void
switcher(void)
{
	Class coordinator = objc_getClass("SBMainSwitcherControllerCoordinator");
	SEL activate = sel_registerName("activateMainSwitcherNoninteractivelyWithSource:animated:");
	id shared = coordinator != NULL ? SEND(id, coordinator, "sharedInstance") : NULL;
	Method method = shared != NULL ? class_getInstanceMethod(object_getClass(shared), activate) : NULL;

	if (method == NULL) {
		say("hakios: the switcher: %s has no %s\n",
		    coordinator != NULL ? "SBMainSwitcherControllerCoordinator" : "nothing", sel_getName(activate));
		return;
	}
	say("hakios: the switcher: opening it (%s, %s)\n", object_getClassName(shared),
	    method_getTypeEncoding(method) != NULL ? method_getTypeEncoding(method) : "?");
	/*
	 * It answers whether it took the request. The source is an analytics label and not every one
	 * of them is allowed from every state, so they are tried in turn until one is taken.
	 */
	/*
	 * Not animated: the animated one is taken and then nothing comes of it -- the transition it
	 * starts is the gesture's to carry, and there is no gesture here.
	 */
	for (long source = 0; source < 10; source++) {
		BOOL taken = ((BOOL (*)(id, SEL, long, BOOL))objc_msgSend)(shared, activate, source, NO);

		say("hakios: the switcher: from source %ld: %s\n", source, taken ? "taken" : "refused");
		if (taken) {
			return;
		}
	}
}

/*
 * Control Center's glyphs whole from its first frames. Its modules and their parts fade in as it
 * comes down: opacity animations, mostly springs that take up to a second to settle at 1. While a
 * layer's opacity is below 1, the window server's OpenGL ES renderer draws the vibrant glyphs in it
 * from a flattened copy that lacks the strokes composited against what is behind them, and draws them
 * whole only at full opacity: the strokes of the media controls, orientation lock, screen mirroring
 * and Focus came in only when the springs had settled, at the end of the presentation. (On an iPhone
 * they are whole all along.) So in Control Center's window an opacity animation is replaced with an
 * eased-out fade of cc_fade=MS between the same values: the modules still fade in and out, and are at
 * full opacity, glyphs whole, after a few frames. Off by default (iOS's own animations stay as they
 * are; the real fix belongs in the drawing, not in the animations): a workaround, for checks only.
 */
#define CC_FADE_MS      0
static void (*original_add_animation_cc)(id self, SEL selector, id animation, id key);
static double cc_fade_s = CC_FADE_MS / 1000.0;
static unsigned cc_fades;

static bool
in_control_center(id layer)
{
	for (int depth = 0; layer != NULL && depth < 64; depth++) {
		id delegate = SEND(id, layer, "delegate");

		if (delegate != NULL && strcmp(object_getClassName(delegate), "SBControlCenterWindow") == 0) {
			return true;
		}
		layer = SEND(id, layer, "superlayer");
	}
	return false;
}

static void
add_animation_cc(id self, SEL selector, id animation, id key)
{
	Class basic = objc_getClass("CABasicAnimation");
	id path = animation != NULL && class_respondsToSelector(object_getClass(animation), sel_registerName("keyPath")) ?
	    SEND(id, animation, "keyPath") : NULL;

	if (path != NULL && strcmp(text_of(path), "opacity") == 0 &&
	    class_respondsToSelector(object_getClass(animation), sel_registerName("fromValue")) && in_control_center(self)) {
		id fade = SEND1(id, basic, "animationWithKeyPath:", id, path);
		id ease = SEND1(id, objc_getClass("CAMediaTimingFunction"), "functionWithName:", id,
		    SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, "easeOut"));

		SEND1(void, fade, "setFromValue:", id, SEND(id, animation, "fromValue"));
		SEND1(void, fade, "setToValue:", id, SEND(id, animation, "toValue"));
		SEND1(void, fade, "setByValue:", id, SEND(id, animation, "byValue"));
		SEND1(void, fade, "setAdditive:", BOOL, SEND(BOOL, animation, "isAdditive"));
		SEND1(void, fade, "setBeginTime:", double, SEND(double, animation, "beginTime"));
		SEND1(void, fade, "setFillMode:", id, SEND(id, animation, "fillMode"));
		SEND1(void, fade, "setRemovedOnCompletion:", BOOL, SEND(BOOL, animation, "isRemovedOnCompletion"));
		SEND1(void, fade, "setDelegate:", id, SEND(id, animation, "delegate"));
		SEND1(void, fade, "setDuration:", double, cc_fade_s);
		SEND1(void, fade, "setTimingFunction:", id, ease);
		if (cc_fades++ < 3) {
			say("hakios: Control Center: an opacity animation of %.3f s (%s) made a fade of %.3f s\n",
			    SEND(double, animation, "duration"), object_getClassName(animation), cc_fade_s);
		}
		animation = fade;
	}
	original_add_animation_cc(self, selector, animation, key);
}

static void
control_center_fades(const char *arguments)
{
	const char *setting = strstr(arguments, "cc_fade=");
	Method add = class_getInstanceMethod(objc_getClass("CALayer"), sel_registerName("addAnimation:forKey:"));

	if (setting != NULL) {
		cc_fade_s = atof(setting + 8) / 1000.0;
	}
	if (cc_fade_s <= 0 || add == NULL) {
		return;
	}
	original_add_animation_cc = (void (*)(id, SEL, id, id))method_getImplementation(add);
	method_setImplementation(add, (IMP)add_animation_cc);
}

/*
 * Open the tweak loader of the package manager's prefix, when there is one. Nothing here depends on
 * it: without a prefix (or without ElleKit in it) the file is not there and this does nothing.
 */
#define HAKIOS_TWEAK_LOADER    "/redwood/lib/libhakios.tweaks.dylib"

static void
load_tweaks(void)
{
	struct stat st;

	if (stat("/var/jb/usr/lib/TweakInject", &st) != 0) {
		return;                 /* no package manager's prefix here, so no tweaks */
	}
	if (dlopen(HAKIOS_TWEAK_LOADER, RTLD_NOW | RTLD_GLOBAL) == NULL) {
		say("hakios: the tweak loader %s: %s\n", HAKIOS_TWEAK_LOADER, dlerror());
	} else {
		say("hakios: tweaks: %s opened\n", HAKIOS_TWEAK_LOADER);
	}
}

/* ============================================================================
 * fix117: what SpringBoard asks for.
 *
 * The display stack is not what keeps the panel dark.  backboardd registers its vsync port
 * (selector 72), the driver sends every refresh, and no swap is asked for because there is
 * nothing to show -- and what does show is SpringBoard sitting in mach_msg since t ~ 59 s with
 * one line to its name, a failed lookup of com.apple.chronoservices.  So name every service it
 * asks bootstrap for and every mach service it opens an XPC connection to, with the answer it
 * got.  The last ones before it goes quiet are the question the next round answers.
 * ============================================================================ */

/* this SDK has no xpc headers; the one call and its type are all the interposition needs */
typedef struct _xpc_object_s *xpc_object_t;
typedef struct _xpc_type_s *xpc_type_t;    /* fix125: xpc_get_type's answer, opaque */
typedef struct _xpc_connection_s *xpc_connection_t;
typedef void (^sb_xpc_handler_t)(xpc_object_t);

extern kern_return_t bootstrap_look_up(mach_port_t bp, const char *service_name, mach_port_t *sp);
extern kern_return_t bootstrap_look_up2(mach_port_t bp, const char *service_name, mach_port_t *sp,
    int target_pid, uint64_t flags, uint8_t type);
extern xpc_object_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t targetq,
    uint64_t flags);
extern xpc_object_t xpc_connection_send_message_with_reply_sync(xpc_connection_t connection,
    xpc_object_t message);
extern void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
extern void xpc_connection_set_event_handler(xpc_connection_t connection, sb_xpc_handler_t handler);
extern void xpc_connection_send_message_with_reply(xpc_connection_t connection,
    xpc_object_t message, dispatch_queue_t queue, sb_xpc_handler_t handler);
extern const char *xpc_connection_get_name(xpc_connection_t connection);
extern xpc_object_t xpc_dictionary_create(const char * const *keys, xpc_object_t *values,
    size_t count);
extern const char *xpc_dictionary_get_string(xpc_object_t xdict, const char *key);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t xdict, const char *key);
extern const void *xpc_data_get_bytes_ptr(xpc_object_t xdata);
extern size_t xpc_data_get_length(xpc_object_t xdata);
extern void xpc_dictionary_set_string(xpc_object_t xdict, const char *key, const char *string);
extern void xpc_dictionary_set_value(xpc_object_t xdict, const char *key, xpc_object_t value);
extern xpc_object_t xpc_retain(xpc_object_t object);
extern void xpc_release(xpc_object_t object);
extern xpc_object_t xpc_data_create(const void *bytes, size_t length);
extern const char *xpc_type_get_name(xpc_type_t type);
extern xpc_type_t xpc_get_type(xpc_object_t object);
extern char *xpc_copy_description(xpc_object_t object);

static void sb_xpc_say_object(const char *what, const char *name, xpc_object_t object);

/* a name is worth saying three times: the twelfth repeat of it says nothing new */
static bool
sb_worth_saying(const char *name)
{
	static struct {
		const char *name;
		unsigned n;
	} seen[128];
	static unsigned count;

	for (unsigned i = 0; i < count; i++) {
		if (strcmp(seen[i].name, name) == 0) {
			return ++seen[i].n <= 3;
		}
	}
	if (count < 128 && (seen[count].name = strdup(name)) != NULL) {
		seen[count].n = 1;
		count++;
		return true;
	}
	return false;
}

/* fix136: __DATA,__interpose icindeki fonksiyonlarda ASLA dlsym(RTLD_NEXT, ...) kullanma!
 * dyld4 interpose edilen sembol icin dlsym(RTLD_NEXT) cagrildiginda yine bizim interposer'i
 * dondurup sonsuz ozyineleme (SIGBUS stack overflow) yaratiyor. Orijinal extern fonksiyonu
 * dogrudan cagirmak dyld tarafindan interpose edilmez ve dogrudan libxpc'ye gider. */
__attribute__((used)) static kern_return_t
sb_bootstrap_look_up(mach_port_t bp, const char *name, mach_port_t *port)
{
	kern_return_t kr = bootstrap_look_up(bp, name, port);
	if (name != NULL && sb_worth_saying(name)) {
		say("hakios: sb look up \"%s\" kr %d port %#x\n", name, kr,
		    (kr == KERN_SUCCESS && port != NULL) ? *port : 0);
	}
	return kr;
}

__attribute__((used)) static xpc_object_t
sb_xpc_connection_create_mach_service(const char *name, dispatch_queue_t targetq, uint64_t flags)
{
	if (name != NULL && sb_worth_saying(name)) {
		say("hakios: sb xpc service \"%s\" flags %#llx\n", name, (unsigned long long) flags);
	}
	/* fix120: cfprefsd is not in the root image, so launchd can never start it. */
	if (name != NULL && (strcmp(name, "com.apple.cfprefsd.daemon") == 0 ||
	                     strcmp(name, "com.apple.cfprefsd.agent") == 0)) {
		return (xpc_object_t)0;
	}
	return xpc_connection_create_mach_service(name, targetq, flags);
}

__attribute__((used)) static kern_return_t
sb_bootstrap_look_up2(mach_port_t bp, const char *name, mach_port_t *port, int target,
    uint64_t flags, uint8_t type)
{
	kern_return_t kr = bootstrap_look_up2(bp, name, port, target, flags, type);
	if (name != NULL && sb_worth_saying(name)) {
		say("hakios: sb look up2 \"%s\" flags %#llx type %u kr %d port %#x\n", name,
		    (unsigned long long) flags, (unsigned) type, kr,
		    (kr == KERN_SUCCESS && port != NULL) ? *port : 0);
	}
	return kr;
}

/* fix136: RBSConnection ve RBSProcessHandle sentetik yanit ureticileri (Yol B fallback) */
static void
sb_populate_rbs_connection_ivars(id handle)
{
	if (!handle) return;
	Class rbsConnCls = objc_getClass("RBSConnection");
	if (!rbsConnCls) return;
	SEL sharedSel = sel_registerName("sharedInstance");
	if (!class_respondsToSelector(object_getClass((id)rbsConnCls), sharedSel)) return;
	id conn = ((id (*)(id, SEL))objc_msgSend)((id)rbsConnCls, sharedSel);
	if (!conn) return;

	Ivar handleIvar = class_getInstanceVariable(rbsConnCls, "_handle");
	if (handleIvar) {
		object_setIvar(conn, handleIvar, handle);
	}
	Ivar stateIvar = class_getInstanceVariable(rbsConnCls, "_state");
	if (stateIvar) {
		ptrdiff_t off = ivar_getOffset(stateIvar);
		*(uint64_t *)((uintptr_t)conn + off) = 2; /* RBSConnectionStateReady */
	}
	say("hakios: sb fix136 populated RBSConnection(%p)->_handle=%p _state=2\n",
	    (void *)conn, (void *)handle);
}

static id
sb_build_rbs_process_handle(void)
{
	pid_t mypid = getpid();
	Class idCls = objc_getClass("RBSProcessIdentifier");
	Class identCls = objc_getClass("RBSProcessIdentity");
	Class handleCls = objc_getClass("RBSProcessHandle");
	if (!handleCls) return NULL;

	id procId = NULL;
	if (idCls) {
		SEL s = sel_registerName("identifierWithPid:");
		if (class_respondsToSelector(object_getClass((id)idCls), s)) {
			procId = ((id (*)(id, SEL, int))objc_msgSend)((id)idCls, s, (int)mypid);
		}
	}

	id procIdent = NULL;
	if (identCls) {
		SEL s = sel_registerName("identityForEmbeddedApplicationIdentifier:jobLabel:auid:platform:");
		if (class_respondsToSelector(object_getClass((id)identCls), s)) {
			CFStringRef bundleId = CFSTR("com.apple.springboard");
			procIdent = ((id (*)(id, SEL, id, id, unsigned int, int))objc_msgSend)(
			    (id)identCls, s, (id)bundleId, (id)bundleId, 0, 2);
		}
		if (!procIdent) {
			SEL s2 = sel_registerName("identityForExecutablePath:pid:auid:");
			if (class_respondsToSelector(object_getClass((id)identCls), s2)) {
				CFStringRef path = CFSTR("/System/Library/CoreServices/SpringBoard.app/SpringBoard");
				procIdent = ((id (*)(id, SEL, id, int, unsigned int))objc_msgSend)(
				    (id)identCls, s2, (id)path, (int)mypid, 0);
			}
		}
	}

	id handle = ((id (*)(id, SEL))objc_msgSend)((id)handleCls, sel_registerName("alloc"));
	if (handle) {
		if (procId) {
			Ivar iv = class_getInstanceVariable(handleCls, "_pid");
			if (iv) *(int *)((uintptr_t)handle + ivar_getOffset(iv)) = (int)mypid;
		}
		if (procIdent) {
			Ivar iv = class_getInstanceVariable(handleCls, "_identity");
			if (iv) object_setIvar(handle, iv, procIdent);
		}
	}
	return handle;
}

static xpc_object_t
sb_make_fake_rbs_reply(xpc_object_t request_msg)
{
	const char *selector = request_msg != NULL ?
	    xpc_dictionary_get_string(request_msg, "rbs_selector") : NULL;
	say("hakios: sb fix136 synthesizing RBS reply for selector=\"%s\"\n",
	    selector ? selector : "(null)");

	Class coderCls = objc_getClass("BSXPCCoder");
	if (coderCls) {
		SEL coderSel = sel_registerName("coder");
		SEL encodeSel = sel_registerName("encodeObject:forKey:");
		SEL createMsgSel = sel_registerName("createMessage");

		if (class_respondsToSelector(object_getClass((id)coderCls), coderSel)) {
			id coder = ((id (*)(id, SEL))objc_msgSend)((id)coderCls, coderSel);
			if (coder &&
			    class_respondsToSelector(coderCls, encodeSel) &&
			    class_respondsToSelector(coderCls, createMsgSel)) {

				if (selector && strcmp(selector, "acquireAssertionWithDescriptor:error:") == 0) {
					Class assertIdCls = objc_getClass("RBSAssertionIdentifier");
					id assertId = NULL;
					if (assertIdCls) {
						SEL s = sel_registerName("identifierWithClientPid:");
						if (class_respondsToSelector(object_getClass((id)assertIdCls), s)) {
							assertId = ((id (*)(id, SEL, int))objc_msgSend)(
							    (id)assertIdCls, s, (int)getpid());
						}
					}
					if (assertId) {
						((void (*)(id, SEL, id, id))objc_msgSend)(
						    coder, encodeSel, assertId, (id)CFSTR("rbs_return"));
						xpc_object_t reply = ((xpc_object_t (*)(id, SEL))objc_msgSend)(
						    coder, createMsgSel);
						if (reply) {
							say("hakios: sb fix136 encoded RBSAssertionIdentifier(%p) -> %p\n",
							    (void *)assertId, (void *)reply);
							return reply;
						}
					}
				} else {
					Class respCls = objc_getClass("RBSHandshakeResponse");
					id resp = respCls ? ((id (*)(id, SEL))objc_msgSend)(
					    ((id (*)(id, SEL))objc_msgSend)((id)respCls, sel_registerName("alloc")),
					    sel_registerName("init")) : NULL;
					id handle = sb_build_rbs_process_handle();

					if (resp && handle) {
						SEL setHandleSel = sel_registerName("setHandle:");
						if (class_respondsToSelector(respCls, setHandleSel)) {
							((void (*)(id, SEL, id))objc_msgSend)(resp, setHandleSel, handle);
						} else {
							Ivar iv = class_getInstanceVariable(respCls, "_handle");
							if (iv) object_setIvar(resp, iv, handle);
						}
					}
					if (handle) {
						sb_populate_rbs_connection_ivars(handle);
					}
					if (resp) {
						((void (*)(id, SEL, id, id))objc_msgSend)(
						    coder, encodeSel, resp, (id)CFSTR("rbs_return"));
						xpc_object_t reply = ((xpc_object_t (*)(id, SEL))objc_msgSend)(
						    coder, createMsgSel);
						if (reply) {
							say("hakios: sb fix136 encoded RBSHandshakeResponse(%p, handle=%p) -> %p\n",
							    (void *)resp, (void *)handle, (void *)reply);
							return reply;
						}
					}
				}
			}
		}
	}

	xpc_object_t fallback = xpc_dictionary_create(NULL, NULL, 0);
	xpc_object_t ret_dict = xpc_dictionary_create(NULL, NULL, 0);
	if (fallback && ret_dict) {
		xpc_dictionary_set_string(ret_dict, "bsx_class",
		    (selector && strcmp(selector, "acquireAssertionWithDescriptor:error:") == 0)
		        ? "RBSAssertionIdentifier"
		        : "RBSHandshakeResponse");
		xpc_dictionary_set_value(fallback, "rbs_return", ret_dict);
	}
	return fallback;
}

/* fix144: the lsd mapdb/anonymous protocol hides an NSKeyedArchiver bplist in
 * the "root" data; xpc_copy_description only shows 40 bytes, so dump it whole. */
static void
sb_say_root_data(const char *label, xpc_object_t message)
{
	xpc_object_t root = message != NULL ? xpc_dictionary_get_value(message, "root") : NULL;
	xpc_type_t type = root != NULL ? xpc_get_type(root) : NULL;
	const char *tname = type != NULL ? xpc_type_get_name(type) : NULL;
	const uint8_t *bytes;
	size_t len, n, i;
	char hex[3 * 256 + 1];

	if (tname == NULL || strcmp(tname, "data") != 0) {
		return;
	}
	bytes = xpc_data_get_bytes_ptr(root);
	len = xpc_data_get_length(root);
	n = len < 256 ? len : 256;
	for (i = 0; i < n; i++) {
		snprintf(hex + i * 3, 4, "%02x ", bytes[i]);
	}
	hex[n * 3] = '\0';
	say("hakios: sb %s root len=%zu hex=%s\n", label, len, hex);
}

struct sb_reply_wait {
	semaphore_t sem;
	unsigned seconds;
	volatile int replied;
	volatile int timed_out;
	xpc_object_t reply;
};

static void *
sb_reply_timeout_thread(void *arg)
{
	struct sb_reply_wait *w = arg;

	for (unsigned i = 0; i < w->seconds; i++) {
		usleep(1000000);
	}
	if (!w->replied) {
		w->timed_out = 1;
		semaphore_signal(w->sem);
	}
	return NULL;
}

/* fix143: this XNU's waitq deadlines do not wake timed semaphore/ulock waits, so a
 * timed wait can hang forever.  A detached watchdog thread (usleep works) signals
 * the semaphore at the deadline; the caller waits without a kernel timeout. */
static xpc_object_t
sb_send_with_timeout(xpc_connection_t connection, xpc_object_t message, dispatch_queue_t queue,
    unsigned seconds, bool *timed_out)
{
	struct sb_reply_wait *w = calloc(1, sizeof(*w));
	pthread_t thread;
	bool has_watchdog = false;

	if (timed_out != NULL) {
		*timed_out = false;
	}
	if (w == NULL) {
		return NULL;
	}
	w->seconds = seconds;
	if (semaphore_create(mach_task_self(), &w->sem, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) {
		free(w);
		return NULL;
	}
	if (seconds > 0 && pthread_create(&thread, NULL, sb_reply_timeout_thread, w) == 0) {
		pthread_detach(thread);
		has_watchdog = true;
	}
	xpc_connection_send_message_with_reply(connection, message, queue, ^(xpc_object_t object) {
		w->reply = object != NULL ? xpc_retain(object) : NULL;
		w->replied = 1;
		semaphore_signal(w->sem);
	});
	/* fix144: seconds == 0 means wait for the real reply (no watchdog); the
	 * anonymous lsd endpoint answers late, but answers. */
	if (has_watchdog || seconds == 0) {
		semaphore_wait(w->sem);
	} else {
		mach_timespec_t wait_time = { seconds, 0 };

		semaphore_timedwait(w->sem, wait_time);
	}
	if (timed_out != NULL) {
		*timed_out = (w->reply == NULL);
	}
	return w->reply;
}

static xpc_object_t sb_lsd_mapdb_reply;

static int
sb_message_has_root_selector(xpc_object_t message, const char *selector)
{
	xpc_object_t root = xpc_dictionary_get_value(message, "root");
	const void *bytes;
	size_t length, i, slen;

	if (root == NULL) {
		return 0;
	}
	bytes = xpc_data_get_bytes_ptr(root);
	length = xpc_data_get_length(root);
	if (bytes == NULL || length == 0 || selector == NULL) {
		return 0;
	}
	slen = strlen(selector);
	if (length < slen) {
		return 0;
	}
	for (i = 0; i + slen <= length; i++) {
		if (memcmp((const char *)bytes + i, selector, slen) == 0) {
			return 1;
		}
	}
	return 0;
}

__attribute__((used)) static xpc_object_t
sb_xpc_send_sync(xpc_connection_t connection, xpc_object_t message)
{
	static unsigned anonymous;
	const char *name = connection != NULL ? xpc_connection_get_name(connection) : NULL;
	bool said = name != NULL ? sb_worth_saying(name) : (++anonymous <= 20);

	if (said) {
		say("hakios: sb xpc sync send \"%s\" message %p\n", name ? name : "(no name)", message);
		if (name == NULL ||
		    strcmp(name, "com.apple.runningboard") == 0 ||
		    strncmp(name, "com.apple.lsd", 13) == 0 ||
		    strncmp(name, "com.apple.iconservices", 22) == 0) {
			sb_xpc_say_object("fix127 sync request", name ? name : "(no name)", message);
		}
	}
	/* fix118/fix138: cfprefsd ve mobileactivationd imajda yok, aninda bos sozluk don */
	if (name != NULL && (strcmp(name, "com.apple.cfprefsd.daemon") == 0 ||
	                     strcmp(name, "com.apple.cfprefsd.agent") == 0 ||
	                     strcmp(name, "com.apple.mobileactivationd") == 0)) {
		return xpc_dictionary_create(NULL, NULL, 0);
	}
	/* fix136: runningboardd icin 20 sn watchdog + BSXPCCoder fallback */
	if (name != NULL && strcmp(name, "com.apple.runningboard") == 0) {
		static dispatch_queue_t replyq;
		xpc_object_t reply;
		bool timed_out = false;

		if (replyq == NULL) {
			replyq = dispatch_queue_create("hakios.rbs.reply", NULL);
		}
		reply = sb_send_with_timeout(connection, message, replyq, 20, &timed_out);
		if (reply != NULL) {
			xpc_type_t t = xpc_get_type(reply);
			const char *tname = t != NULL ? xpc_type_get_name(t) : "null";

			say("hakios: sb fix136 runningboard reply arrived (type=%s)\n", tname);
			sb_xpc_say_object("fix136 reply", name, reply);
			if (tname != NULL && strcmp(tname, "dictionary") == 0) {
				return reply;
			}
			say("hakios: sb fix136 runningboard reply is %s (not dictionary), using BSXPCCoder fallback\n",
			    tname ? tname : "null");
			return sb_make_fake_rbs_reply(message);
		}
		say("hakios: sb fix136 runningboard reply did not come in 20 s, using BSXPCCoder fallback\n");
		return sb_make_fake_rbs_reply(message);
	}
	/* fix148: the anonymous lsd endpoint is called by SpringBoard, not by lsd.
	 * fix147's lsd-side bypass therefore never fired.  Waiting forever here pins
	 * CoreServices' ulock; return an XPC error at once so the caller sees a
	 * failed connection instead of a fake empty reply.  Only the lsd anonymous
	 * request is matched, not every anonymous sync call. */
	if (name == NULL &&
	    sb_message_has_root_selector(message, "getServerStoreBlockingWithCompletionHandler:")) {
		/* fix156: do not send the blocking call into lsd at all.  The caller's
		 * completion block is v32@?0@8@"FSNode"16@"NSError"24, so answer with a
		 * bplist17 invocation carrying FSNode=nil and NSError=nil.  This releases
		 * CoreServices' ulock without waiting for lsd/containermanagerd. */
		static const unsigned char blocking_root[] = {
			0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x31, 0x37, 0xa0, 0x38,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x7f, 0x11,
			0x18, 0x76, 0x40, 0x3f, 0x40, 0x40, 0x22, 0x46, 0x53, 0x4e,
			0x6f, 0x64, 0x65, 0x22, 0x40, 0x22, 0x4e, 0x53, 0x45, 0x72,
			0x72, 0x6f, 0x72, 0x22, 0x00, 0xa0, 0x38, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0xe0, 0xe0, 0xe0
		};
		xpc_object_t data = xpc_data_create(blocking_root, sizeof(blocking_root));
		xpc_object_t synth = xpc_dictionary_create(NULL, NULL, 0);

		xpc_dictionary_set_value(synth, "root", data);
		xpc_release(data);
		say("hakios: sb fix158 anonymous lsd -> synthetic blocking reply (FSNode=nil NSError=nil)\n");
		return synth;
	}
	/* fix138/fix144: lsd.mapdb 60 sn icinde yanitlamazsa bos dict don. */
	if (name != NULL && strncmp(name, "com.apple.lsd", 13) == 0) {
		static dispatch_queue_t lsd_replyq;
		xpc_object_t reply;
		bool timed_out = false;
		const char *label = name;
		unsigned wait = 60;

		if (lsd_replyq == NULL) {
			lsd_replyq = dispatch_queue_create("hakios.lsd.reply", NULL);
		}
		sb_say_root_data("fix144 request", message);
		reply = sb_send_with_timeout(connection, message, lsd_replyq, wait, &timed_out);
		if (reply != NULL) {
			xpc_type_t t = xpc_get_type(reply);
			const char *tname = t != NULL ? xpc_type_get_name(t) : "null";

			say("hakios: sb fix138 sync reply from \"%s\" arrived (type=%s)\n", label, tname);
			sb_xpc_say_object("fix138 reply", label, reply);
			sb_say_root_data("fix144 reply", reply);
			if (tname != NULL && strcmp(tname, "dictionary") == 0) {
				if (strcmp(label, "com.apple.lsd.mapdb") == 0) {
					if (sb_lsd_mapdb_reply != NULL) {
						xpc_release(sb_lsd_mapdb_reply);
					}
					sb_lsd_mapdb_reply = xpc_retain(reply);
					say("hakios: sb fix150 cached lsd.mapdb reply %p\n",
					    sb_lsd_mapdb_reply);
				}
				return reply;
			}
			say("hakios: sb fix138 reply from \"%s\" is %s, returning empty dict to release ulock\n",
			    label, tname ? tname : "null");
			return xpc_dictionary_create(NULL, NULL, 0);
		}
		say("hakios: sb fix138 sync send to \"%s\" timed out after %u s -> returning empty dict to release CoreServices ulock!\n",
		    label, wait);
		return xpc_dictionary_create(NULL, NULL, 0);
	}
	/* fix141/fix143: any other sync XPC (FrontBoard/CoreServices/Foundation) that
	 * does not reply must not pin an os_unfair_lock forever. */
	{
		static dispatch_queue_t generic_replyq;
		static unsigned generic_logged;
		xpc_object_t reply;
		bool timed_out = false;
		const char *label = name != NULL ? name : "(no name)";

		if (generic_logged < 48) {
			generic_logged++;
			say("hakios: sb fix141 generic sync send[%u] \"%s\" message %p\n",
			    generic_logged, label, message);
		}
		if (generic_replyq == NULL) {
			generic_replyq = dispatch_queue_create("hakios.generic.reply", NULL);
		}
		reply = sb_send_with_timeout(connection, message, generic_replyq, 10, &timed_out);
		if (reply != NULL) {
			return reply;
		}
		say("hakios: sb fix141 sync send to \"%s\" timed out after 10 s -> returning empty dict\n",
		    label);
		return xpc_dictionary_create(NULL, NULL, 0);
	}
}

static bool
sb_board_connection(const char *name)
{
	return name != NULL && strstr(name, "board") != NULL;
}

/* four of each kind about a connection says the story; the fifth repeats it */
static bool
sb_board_worth_saying(const char *kind, const char *name)
{
	static struct {
		const char *kind;
		char *name;
		unsigned n;
	} seen[64];
	static unsigned count;

	for (unsigned i = 0; i < count; i++) {
		if (strcmp(seen[i].kind, kind) == 0 && strcmp(seen[i].name, name) == 0) {
			return ++seen[i].n <= 4;
		}
	}
	if (count < 64 && (seen[count].kind = strdup(kind)) != NULL &&
	    (seen[count].name = strdup(name)) != NULL) {
		seen[count++].n = 1;
		return true;
	}
	return false;
}

/* Bounded descriptions; the proven sync path gets enough room to expose request keys. */
static void
sb_xpc_say_object(const char *what, const char *name, xpc_object_t object)
{
	char *desc = object != NULL ? xpc_copy_description(object) : NULL;
	size_t limit = strcmp(what, "fix127 sync request") == 0 ? 960 : 190;

	if (desc != NULL) {
		for (char *p = desc; *p != '\0'; p++) {
			if (*p == '\n') {
				*p = ' ';
			}
		}
		if (strlen(desc) > limit) {
			desc[limit] = '\0';
		}
		say("hakios: sb %s \"%s\": %s\n", what, name, desc);
		free(desc);
	} else {
		say("hakios: sb %s \"%s\": (nothing to describe)\n", what, name);
	}
}

__attribute__((used)) static void
sb_xpc_set_event_handler(xpc_connection_t connection, sb_xpc_handler_t handler)
{
	const char *name = connection != NULL ? xpc_connection_get_name(connection) : NULL;

	if (!sb_board_connection(name) || handler == NULL) {
		xpc_connection_set_event_handler(connection, handler);
		return;
	}
	sb_xpc_handler_t target = handler;
	char *who = strdup(name);

	say("hakios: sb event handler for \"%s\" wrapped\n", name);
	xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
		xpc_type_t type = event != NULL ? xpc_get_type(event) : NULL;

		if (sb_board_worth_saying("event", who)) {
			say("hakios: sb event for \"%s\" type %s\n", who,
			    type != NULL ? xpc_type_get_name(type) : "null");
			sb_xpc_say_object("event body", who, event);
		}
		target(event);
	});
}

__attribute__((used)) static void
sb_xpc_send_async(xpc_connection_t connection, xpc_object_t message)
{
	const char *name = connection != NULL ? xpc_connection_get_name(connection) : NULL;

	if (sb_board_connection(name) && sb_board_worth_saying("send", name)) {
		sb_xpc_say_object("async send", name, message);
	}
	xpc_connection_send_message(connection, message);
}

__attribute__((used)) static void
sb_xpc_send_with_reply(xpc_connection_t connection, xpc_object_t message,
    dispatch_queue_t queue, sb_xpc_handler_t handler)
{
	const char *name = connection != NULL ? xpc_connection_get_name(connection) : NULL;

	if (!sb_board_connection(name) || handler == NULL) {
		xpc_connection_send_message_with_reply(connection, message, queue, handler);
		return;
	}
	char *who = strdup(name);
	sb_xpc_handler_t target = handler;
	if (sb_board_worth_saying("send-reply", who)) {
		sb_xpc_say_object("async send-with-reply", who, message);
	}
	xpc_connection_send_message_with_reply(connection, message, queue, ^(xpc_object_t reply) {
		xpc_type_t type = reply != NULL ? xpc_get_type(reply) : NULL;

		if (sb_board_worth_saying("reply", who)) {
			say("hakios: sb async reply for \"%s\" type %s\n", who,
			    type != NULL ? xpc_type_get_name(type) : "null");
			sb_xpc_say_object("async reply body", who, reply);
		}
		target(reply);
	});
}


__attribute__((used)) static struct {
	const void *replacement;
	const void *replacee;
} sb_interposers[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *) sb_bootstrap_look_up, (const void *) bootstrap_look_up },
	{ (const void *) sb_bootstrap_look_up2, (const void *) bootstrap_look_up2 },
	{ (const void *) sb_xpc_connection_create_mach_service,
		(const void *) xpc_connection_create_mach_service },
	{ (const void *) sb_xpc_send_sync, (const void *) xpc_connection_send_message_with_reply_sync },
	{ (const void *) sb_xpc_send_async, (const void *) xpc_connection_send_message },
	{ (const void *) sb_xpc_send_with_reply,
		(const void *) xpc_connection_send_message_with_reply },
	{ (const void *) sb_xpc_set_event_handler, (const void *) xpc_connection_set_event_handler },
};


static void
sb_main_thread_start(void)
{
	say("hakios: main loop timer: the +1 s work runs\n");
	say_console("hakios: main loop timer console write works\n");
	Class uidev = objc_getClass("UIDevice");
	SEL current = sel_registerName("currentDevice");
	SEL monitoring = sel_registerName("setBatteryMonitoringEnabled:");
	id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, current) : nil;

	if (device && class_getInstanceMethod(object_getClass(device), monitoring)) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(device, monitoring, YES);
	}
	watch_main_loop();
	{
		CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + 15.0, 0, 0, 0,
		    ^(__unused CFRunLoopTimerRef fired) {
			say("hakios: SB delayed layers dump (+15 s, run loop timer)\n");
			layers();
		    });

		if (timer != NULL) {
			CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
			CFRelease(timer);
		}
	}
	{
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			control_center_fades(arguments);
		}
	}
	say("hakios: battery: SOC=%d%% battery=%d charger=%d charging=%d\n",
	    sysctl_int("kern.hakios.battery_soc"), sysctl_int("kern.hakios.battery_present"),
	    sysctl_int("kern.hakios.battery_charger_present"), sysctl_int("kern.hakios.battery_charging"));
	battery_post_notifications();
}


__attribute__((constructor))
static void
sb_interpose_hello(void)
{
	say("hakios: sb fix118 interposers installed (lookup v1/v2, xpc create, xpc sync send; "
	    "fix125 async send + event handler; fix127 sync-request trace)\n");
}

__attribute__((constructor))
static void
hakios_springboard_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	IMP activation, snapshot_format, battery_lvl, battery_st, poster_switcher;
	int token;

	if (name == NULL) {
		return;
	}
	if (strcmp(name, "SpringBoard") != 0) {
		static char who[64];

		snprintf(who, sizeof(who), " of %s", name);
		layers_of = who;
		if ((say_fd = open("/dev/console", O_WRONLY | O_NOCTTY)) < 0) {
			say_fd = 2;
		}
		say("hakios: in %s[%d]: its layers are said on the notification com.hakios.layers\n", name, getpid());
		notify_register_dispatch("com.hakios.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
			layers();
		});
		return;
	}
	/*
	 * The tweaks of the package manager's prefix, if one is installed: ElleKit's loader
	 * (/var/jb/usr/lib/TweakLoader.dylib) reads /var/jb/usr/lib/TweakInject and opens the tweaks
	 * whose Filter names this process. The kernel could insert it as well (etc/inject.conf), but
	 * only one inserted library per program is reliable here, and SpringBoard's is this one; so
	 * this one opens it, the way a jailbreak's own hook does.
	 */
	load_tweaks();
	sb_install_cacontext_trace();
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	watch_main_thread();
	activation = replace_method("SBLockdownManager", "_setupActivationState", 0x1c4bc8dfcUL, (IMP)setup_activation_state);
	original_set_setup_required_reason = (void (*)(id, SEL, long))replace_method("SBSetupManager",
	    "_setSetupRequiredReason:", 0x1c4cdd3a4UL, (IMP)set_setup_required_reason);
	snapshot_format = replace_method_of_kind('+', "XBApplicationSnapshotManifestImpl", "_outputFormatForSnapshot:",
	    0x1f1c91480UL, (IMP)snapshot_output_format);
	plain_vibrancy();
	original_lock_long_press = (void (*)(id, SEL, id))replace_method("SBLockHardwareButton", "longPress:", 0x1c4f6ab44UL,
	    (IMP)lock_long_press);
	poster_switcher = replace_method("SBMainDisplayPolicyAggregator", "_allowsCapabilityPosterSwitcherWithExplanation:",
	    0x1c507e0fcUL, (IMP)poster_switcher_allowed);
	original_switcher_wallpaper_overlay = (void (*)(id, SEL))replace_method("SBAppSwitcherPageView",
	    "_createOrDestroyWallpaperOverlayView", 0x1c4af7860UL, (IMP)switcher_wallpaper_overlay);
	/*
	 * Battery hooks on UIDevice: these are the methods every app and SpringBoard itself calls, so
	 * hooking them here in the SpringBoard process covers both the status bar and any app that
	 * reads [UIDevice currentDevice].batteryLevel. The addresses are for iOS 16.5 (20F66).
	 */
	original_battery_level = (float (*)(id, SEL))replace_method("UIDevice", "batteryLevel", 0x1897021b0UL, (IMP)battery_level);
	original_battery_state = (long (*)(id, SEL))replace_method("UIDevice", "batteryState", 0x189209794UL, (IMP)battery_state);
	original_battery_view_shows_percentage = (BOOL (*)(id, SEL))replace_method("_UIBatteryView", "showsPercentage",
	    0x189a9370cUL, (IMP)battery_view_shows_percentage);
	original_battery_view_set_shows_percentage = (void (*)(id, SEL, BOOL))replace_method("_UIBatteryView", "setShowsPercentage:",
	    0x18926e150UL, (IMP)battery_view_set_shows_percentage);
	original_battery_view_should_show_bolt = (BOOL (*)(id, SEL))replace_method("_UIBatteryView", "_shouldShowBolt",
	    0x189203e28UL, (IMP)battery_view_should_show_bolt);
	original_battery_view_charge_percent = (double (*)(id, SEL))replace_method("_UIBatteryView", "chargePercent",
	    0x189a935d8UL, (IMP)battery_view_charge_percent);
	original_battery_view_set_charge_percent = (void (*)(id, SEL, double))replace_method("_UIBatteryView", "setChargePercent:",
	    0x18928d310UL, (IMP)battery_view_set_charge_percent);
	original_ring_charging_level = (double (*)(id, SEL))replace_method("CSBatteryChargingRingView", "batteryChargingLevel",
	    0x1c48c2cf4UL, (IMP)ring_charging_level);
	battery_lvl = (IMP)original_battery_level;
	battery_st  = (IMP)original_battery_state;
	say("hakios: SpringBoard: activation %s, setup assistant %s, application snapshots %s, the side button's long press %s, "
	    "battery level %s, battery state %s, poster switcher %s\n",
	    activation ? "answered here" : "NOT replaced", original_set_setup_required_reason ? "refused" : "NOT replaced",
	    snapshot_format ? "as JPEG" : "NOT changed", original_lock_long_press ? "checked" : "NOT checked",
	    battery_lvl ? "from QG" : "NOT replaced", battery_st ? "from QG" : "NOT replaced",
	    poster_switcher ? "not offered" : "NOT withheld");
	/*
	 * Cellular, a survey first (the modem's bars come from the kernel: sysctl kern.hakios.cellular_*,
	 * bsd/dev/hakios.mpss.c): where UIKit makes the status bar's cellular entry from the raw data,
	 * and with which signature -- said in the log with the unslid address, for the hook to come.
	 */
	{
		static const char *const classes[] = { "_UIStatusBarDataCellularEntry", "_UIStatusBarData", "_UIStatusBarDataEntry" };
		const char *selector = "_cellularEntryWithData:signalStrengthEnabled:serviceEnabled:dataNetworkEnabled:serviceString:"
		    "serviceCrossfadeString:serviceBadgeString:serviceContentType:dataNetworkType:gsmSignalStrengthRaw:"
		    "gsmSignalStrengthBars:callForwardingType:lowDataModeActive:";

		for (unsigned c = 0; c < sizeof(classes) / sizeof(classes[0]); c++) {
			Class cls = objc_getClass(classes[c]);
			Method plus = cls ? class_getClassMethod(cls, sel_registerName(selector)) : NULL;
			Method minus = cls ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

			if (plus) {
				say("hakios: cellular: +[%s %s] at %#lx, types \"%s\"\n", classes[c], selector,
				    (uintptr_t)method_getImplementation(plus) - slide, method_getTypeEncoding(plus));
			}
			if (minus) {
				say("hakios: cellular: -[%s %s] at %#lx, types \"%s\"\n", classes[c], selector,
				    (uintptr_t)method_getImplementation(minus) - slide, method_getTypeEncoding(minus));
			}
			if (cls && !plus && !minus) {
				say("hakios: cellular: %s has no such method\n", classes[c]);
			}
		}
		say("hakios: cellular: the kernel says up=%d registered=%d bars=%d dbm=%d radio=%d\n", sysctl_int("kern.hakios.cellular_up"),
		    sysctl_int("kern.hakios.cellular_registered"), sysctl_int("kern.hakios.cellular_bars"),
		    sysctl_int("kern.hakios.cellular_dbm"), sysctl_int("kern.hakios.cellular_radio"));
	}
	cellular_hook();
	say("hakios: battery UI: shows%% %s, set-shows%% %s, bolt %s, chargePercent %s, setChargePercent %s, lockscreen ring %s\n",
	    original_battery_view_shows_percentage ? "ok" : "NO",
	    original_battery_view_set_shows_percentage ? "ok" : "NO",
	    original_battery_view_should_show_bolt ? "ok" : "NO",
	    original_battery_view_charge_percent ? "ok" : "NO",
	    original_battery_view_set_charge_percent ? "ok" : "NO",
	    original_ring_charging_level ? "ok" : "NO");
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *delay;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (delay = strstr(args, "sb_delay=")) != NULL &&
		    (delay == args || delay[-1] == ' ')) {
			usleep((useconds_t)(atof(delay + 9) * 1e6));
		}
	}
	/*
	 * fix163: SpringBoard's main dispatch queue is not serviced here (the main thread is in
	 * UIApplicationMain/GSEventRunModal and the mainq count stays 0), so dispatch_after never
	 * fires. The main run loop itself does turn; schedule the same work with a run loop timer.
	 */
	{
		CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + 1.0, 0, 0, 0,
		    ^(__unused CFRunLoopTimerRef fired) {
			sb_main_thread_start();
		    });

		if (timer != NULL) {
			CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
			CFRelease(timer);
			CFRunLoopWakeUp(CFRunLoopGetMain());
		}
	}
	notify_register_dispatch("com.hakios.unlock", &token, dispatch_get_main_queue(), ^(__unused int t) {
		unlock();
	});
	notify_register_dispatch("com.hakios.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
		layers();
	});
	notify_register_dispatch("com.hakios.switcher", &token, dispatch_get_main_queue(), ^(__unused int t) {
		switcher();
	});
	/*
	 * Belt-and-suspenders unlock. The user can always swipe up to open the home screen -- that
	 * works fine when the touch driver is doing its job. But if touch is still probing, has
	 * failed, or the panel is a FocalTech and the firmware upload is still chewing through
	 * ~114 KB of SPI (that takes a hot second during bring-up), the user ends up staring at a
	 * dark lock screen with nothing to swipe. So we just forcibly dismiss it after 10 seconds
	 * so iOS always lands on the home screen even in the worst case. Harmless if touch is
	 * already working: the user just doesn't need to swipe themselves.
	 * With autounlock=0 among the kernel's arguments the lock screen stays until it is swiped away
	 * (to show the phone starting the way an iPhone does).
	 */
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *arg;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (arg = strstr(args, "autounlock=0")) != NULL &&
		    (arg == args || arg[-1] == ' ')) {
			say("hakios: the lock screen stays until it is swiped away (autounlock=0)\n");
			return;
		}
	}
	{
		CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + 10.0, 0, 0, 0,
		    ^(__unused CFRunLoopTimerRef fired) {
			unlock();
		    });

		if (timer != NULL) {
			CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
			CFRelease(timer);
			CFRunLoopWakeUp(CFRunLoopGetMain());
		}
	}
}
