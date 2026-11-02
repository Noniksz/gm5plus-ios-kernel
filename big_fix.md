# GÖREV: fix136 — SpringBoard RBSHandshake SIGBUS/Deadlock ve runningboardd XPC Kuyruk Kilidi Düzeltmesi

## Hedef
XNU çekirdeğine dokunmadan `userland/src/inject.c` (`libhakios.dylib`) ve `userland/src/inject_sb.c` (`libhakios.sb.dylib`) dosyalarında aşağıdaki değişiklikleri uygula, `/home/atakan/hackios-work/userland` ağacına kopyala ve derle.

## Kök Neden Özeti
1. **fix134/135 SpringBoard SIGBUS (Stack Overflow):** `inject_sb.c` içinde `__DATA,__interpose` tablosunda (`sb_interposers`) yer alan `xpc_connection_send_message_with_reply` için `dlsym(RTLD_NEXT, ...)` çağrıldığında, dyld4 gerçek `libxpc` fonksiyonunu değil yine bizim `sb_xpc_send_with_reply` fonksiyonumuzu döndürüyor (`inject.c:3818`'de olduğu gibi `extern` sembol doğrudan çağrılmalı, `dlsym(RTLD_NEXT)` kullanılmamalı).
2. **runningboardd `0x210b` Kuyruk Kilidi (`msgs 1 size 8496 seqno 3`):** `runningboardd` ilk 3 mesajı okuduktan sonra Thread 2'de `com.apple.cfprefsd.daemon` servisine senkron XPC mesajı (`xpc_connection_send_message_with_reply_sync`) gönderiyor ve imajda `cfprefsd` olmadığı için Thread 2 sonsuza dek `mach_msg2_trap` içinde bloklanıyor. `inject_sb.c` içinde `SpringBoard` için olan `cfprefsd` bypass'ı `inject.c` (`libhakios.dylib`) içinde yoktu.
3. **`cpus=1` Zamanlaması ve İkili RBS Fallback (`handshakeWithRequest:` + `acquireAssertionWithDescriptor:error:`):** Tek çekirdekte `runningboardd` runloop'u `SpringBoard`'dan ~14 sn sonra hazır olduğu için bekleme süresi 20 saniye olmalı; yanıt gelmezse veya hata dönerse boş `{}` yerine `BSXPCCoder` ile `RBSHandshakeResponse` (`handshakeWithRequest:` için) ve `RBSAssertionIdentifier` (`acquireAssertionWithDescriptor:error:` için) üretilmeli.

---

## Değişiklik 1: `userland/src/inject.c` (Satır 3794–3827 arası)

`userland/src/inject.c` dosyasındaki `6. INTERPOSITION: FAST-PATH BYPASS FOR OSCARD & COREACCESSORIES` bloğunu (satır 3794–3827) aşağıdaki kodla birebir değiştir:

```c
/* ============================================================================
 * 6. INTERPOSITION: FAST-PATH BYPASS FOR OSCARD, COREACCESSORIES & CFPREFSD
 * ============================================================================ */
typedef void * xpc_connection_t;
typedef void * xpc_object_t;
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, void *targetq, uint64_t flags);
extern xpc_object_t xpc_connection_send_message_with_reply_sync(xpc_connection_t connection, xpc_object_t message);
extern const char *xpc_connection_get_name(xpc_connection_t connection);
extern xpc_object_t xpc_dictionary_create(const char * const *keys, const xpc_object_t *values, size_t count);
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
	const char *prog;
	if (name == NULL) {
		return 0;
	}
	/* /usr/sbin/cfprefsd imajda yok; runningboardd/backboardd senkron cfprefsd
	 * cagrisinda sonsuza dek mach_msg2_trap icinde kilitlenmesin (fix136). */
	if (strcmp(name, "com.apple.cfprefsd.daemon") == 0 ||
	    strcmp(name, "com.apple.cfprefsd.agent") == 0 ||
	    strcmp(name, "com.apple.cfprefsd.daemon.system") == 0 ||
	    strcmp(name, "com.apple.diagnosticd") == 0 ||
	    strcmp(name, "com.apple.analyticsd") == 0) {
		return 1;
	}
	prog = getprogname();
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
	             strcmp(name, "com.apple.cfprefsd.daemon") == 0 ||
	             strcmp(name, "com.apple.cfprefsd.agent") == 0)) {
		say("hakios: %s xpc_connection_create_mach_service(%s) -> fast NULL\n",
		    getprogname() ? getprogname() : "?", name);
		return (xpc_connection_t)0;
	}
	if (name && (strcmp(name, "com.apple.backboard.display.services") == 0 ||
	             strcmp(name, "com.apple.runningboard") == 0)) {
		say("hakios: %s xpc_connection_create_mach_service(%s) flags %#llx\n",
		    getprogname() ? getprogname() : "?", name, (unsigned long long)flags);
	}
	return xpc_connection_create_mach_service(name, targetq, flags);
}

static xpc_object_t my_xpc_connection_send_message_with_reply_sync(xpc_connection_t connection, xpc_object_t message)
{
	const char *name = connection != NULL ? xpc_connection_get_name(connection) : NULL;
	const char *prog = getprogname();
	static unsigned rbd_sync_logged;

	if (hakios_daemon_should_bypass_xpc(name)) {
		static unsigned bypass_logged;
		if (bypass_logged < 24) {
			bypass_logged++;
			say("hakios: %s fix136 xpc sync bypass \"%s\" -> empty dict\n",
			    prog ? prog : "?", name ? name : "?");
		}
		return xpc_dictionary_create(NULL, NULL, 0);
	}
	if (prog != NULL && strcmp(prog, "runningboardd") == 0 && rbd_sync_logged < 32) {
		rbd_sync_logged++;
		say("hakios: rbd xpc sync send[%u] -> \"%s\"\n",
		    rbd_sync_logged, name ? name : "(anon)");
	}
	return xpc_connection_send_message_with_reply_sync(connection, message);
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
```

*(Ayrıca `inject.c` satır 4049 ve 4068'deki `hakios_bootstrap_check_in` ve `hakios_bootstrap_register` içindeki `dlsym(RTLD_NEXT, ...)` çağrılarını da doğrudan `kr = bootstrap_check_in(bp, service_name, service_port);` ve `kr = bootstrap_register(bp, service_name, service_port);` olarak değiştir.)*

---

## Değişiklik 2: `userland/src/inject_sb.c` (Satır 1757–2105 arası)

`userland/src/inject_sb.c` dosyasında satır 1757 (`/* this SDK has no xpc headers... */`) ile satır 2105 (`sb_interposers[]` dizisinin sonu) arasındaki bloğu aşağıdaki kodla birebir değiştir:

```c
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
extern void xpc_dictionary_set_string(xpc_object_t xdict, const char *key, const char *string);
extern void xpc_dictionary_set_value(xpc_object_t xdict, const char *key, xpc_object_t value);
extern xpc_object_t xpc_retain(xpc_object_t object);
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

__attribute__((used)) static xpc_object_t
sb_xpc_send_sync(xpc_connection_t connection, xpc_object_t message)
{
	static unsigned anonymous;
	const char *name = connection != NULL ? xpc_connection_get_name(connection) : NULL;
	bool said = name != NULL ? sb_worth_saying(name) : (++anonymous <= 20);

	if (said) {
		say("hakios: sb xpc sync send \"%s\" message %p\n", name ? name : "(no name)", message);
		if (name != NULL && strcmp(name, "com.apple.runningboard") == 0) {
			sb_xpc_say_object("fix127 sync request", name, message);
		}
	}
	/* fix118: cfprefsd imajda yok, aninda bos sozluk don */
	if (name != NULL && (strcmp(name, "com.apple.cfprefsd.daemon") == 0 ||
	                     strcmp(name, "com.apple.cfprefsd.agent") == 0)) {
		return xpc_dictionary_create(NULL, NULL, 0);
	}
	/* fix136: runningboardd icin dlsym(RTLD_NEXT) OLMADAN dogrudan gercek
	 * xpc_connection_send_message_with_reply cagir, cpus=1 icin 20 sn bekle;
	 * yanit dictionary degilse veya timeout olursa BSXPCCoder ile gercek RBS nesnesi don. */
	if (name != NULL && strcmp(name, "com.apple.runningboard") == 0) {
		static dispatch_queue_t replyq;
		__block xpc_object_t reply = NULL;
		dispatch_semaphore_t done;

		if (replyq == NULL) {
			replyq = dispatch_queue_create("hakios.rbs.reply", NULL);
		}
		done = dispatch_semaphore_create(0);
		xpc_connection_send_message_with_reply(connection, message, replyq, ^(xpc_object_t object) {
			reply = object != NULL ? xpc_retain(object) : NULL;
			dispatch_semaphore_signal(done);
		});
		if (dispatch_semaphore_wait(done,
		    dispatch_time(DISPATCH_TIME_NOW, 20000000000ull)) == 0 && reply != NULL) {
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
	return xpc_connection_send_message_with_reply_sync(connection, message);
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
```

---

## Değişiklik 3: Derleme ve Paketleme (`NEXT_AGENT_HANDOVER.md` Bölüm 7)

Değişiklikleri kaydettikten sonra sırasıyla şu komutları çalıştır:

```bash
cp -p userland/src/inject.c /home/atakan/hackios-work/userland/src/inject.c
cp -p userland/src/inject_sb.c /home/atakan/hackios-work/userland/src/inject_sb.c
(cd /home/atakan/hackios-work/userland && RAIN=1 ./build.sh)
cp -f /home/atakan/hackios-work/userland/root.apfs out/root.apfs

XNU_SRC=/home/atakan/hackios-work/xnu-src/xnu-8796.121.2
python3 ios-sweet/tools/mkpayload.py --ramdisk out/root.apfs \
  --cmdline "-v debug=0xc cpus=1 keepsyms=1 rd=md0 emmc_slice_part=userdata root_apfs=disk1 hv_wdt_pet=1" \
  $XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  out/shamrock.adt out/shamrock-payload.bin
(cd shim-shamrock && PATH="$HOME/.local/bin:$PATH" ./build.sh ../out/shamrock-payload.bin)
```
