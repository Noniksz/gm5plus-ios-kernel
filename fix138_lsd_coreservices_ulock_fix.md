# GÖREV: fix138 — SpringBoard CoreServices (`0x234bba4b8` ulock) / `lsd.mapdb` Kilidi ve `lsd` MobileActivation Bypass Düzeltmesi

## Durum Özeti (`capture-windowserver-fix137.log` Analizi)

`fix136` (`big_fix.md`) ile **RBSHandshake ve runningboardd sorunu %100 çözüldü**:
1. `runningboardd` başlangıçta `com.apple.cfprefsd.daemon` çağrılarını bypass ederek (`t=216..218`) `job state = running` durumuna geçti (`t=219.4`).
2. `SpringBoard`'un `handshakeWithRequest:`, `SystemShell-Initialize` ve `SystemShell-PreventIdleSleep` isteklerine gerçek XPC dictionary yanıtları döndü (`t=219.4..219.6`).
3. `SpringBoard` başarıyla `backboardd`'ye ana sistem kabuğu (`BKSystemShellClient: com.apple.springboard`) olarak bağlandı (`t=231.0..238.6`).

### Yeni Takılma Noktası (`t=233.0..271.3`, `sample 6..39`)
`SpringBoard` şu an `RBSHandshake` sonrasında **`CoreServices` (`LaunchServices`) içindeki global `os_unfair_lock` (`0x234bba4b8`, `word = 0x3002`) üzerinde** bekliyor:
1. **`SpringBoard` Thread 3 (`tid 0x106`, Mach port `0x3003`):**
   - `t=232.0`'da `CoreServices` (`_LSContextInit`) kilidini (`0x234bba4b8`) aldı (`0x3003 & ~3 = 0x3000`) ve `"com.apple.lsd.mapdb"` servisine senkron XPC mesajı (`0x2804e9ce0`) gönderdi.
   - `lsd[22]` (`com.apple.lsd`) bu ilk mesaja **15.7 saniye sonra (`t=247.7`)** yanıt verdi.
   - `t=247.7`'de Thread 3, `0x234bba4b8` kilidini **bırakmadan** `lsd`'den dönen anonim XPC endpoint bağlantısı (`xpc_connection_get_name == NULL`, logda `"(no name)"`) üzerinden ikinci bir senkron XPC mesajı (`xpc_connection_send_message_with_reply_sync`) gönderdi ve `mach_msg2_trap` (`libsystem_kernel.dylib+0xff0`) içinde sonsuz beklemeye girdi.
2. **`SpringBoard` Thread 0 (Ana Thread, `tid 0xbf`, Mach port `0x103`):**
   - `t=233.0`'da `ControlCenterServices` (`CCSModuleRepository`) -> `CoreServices` çağrısına girdi, `0x234bba4b8` kilidini almaya çalıştı, kilidin Thread 3 (`0x3003`) tarafından tutulduğunu görünce waiter bitini (`0x3002`) set edip `__ulock_wait` (`libsystem_kernel.dylib+0x1a90`) içinde uykuya yattı.
3. **`lsd[22]` Neden Yanıt Vermiyor?**
   - `lsd[22]` (`/usr/libexec/lsd`), `build.sh` içindeki `INJECT` listesinde olmadığı için `libhakios.dylib` bypass'larına sahip değil.
   - Logda `t=248.3`, `t=253.8`, `t=259.1`, `t=264.5` ve `t=270.0` anlarında (her **5.3 saniyede bir**) `lsd[22]`'nin sistemde olmayan **`com.apple.mobileactivationd`** servisini aradığı (`failed lookup: name = com.apple.mobileactivationd, flags = 0x8, requestor = lsd[22], error = 3: No such process` x3) ve her denemede 5 saniye uyuduğu görülüyor.
   - Aynı anda `t=248.0`'da `backboardd` içindeki `fix124` (`watch_service_probe`) 6 kare boyunca `1125x2436` (2.74 milyon piksel) yazılımsal test deseni çizerek tek çekirdekli (`cpus=1`) CPU'yu meşgul ediyor.

---

## Değişiklik 1: `userland/build.sh` (`lsd` Enjeksiyonu)

`userland/build.sh` dosyasında **satır 130**'daki `INJECT` tanımında `/redwood/lib/libhakios.dylib` satırının sonuna `lsd` ekle:

```sh
INJECT=${INJECT-"/redwood/lib/libhakios.dylib backboardd containermanagerd containermanagerd_system runningboardd lsd
/redwood/lib/libhakios.sb.dylib SpringBoard
/redwood/lib/libhakios.media.dylib mediaserverd
/redwood/lib/libhakios.app.dylib Sileo
/redwood/lib/libhakios.tweaks.dylib Preferences"}
```

---

## Değişiklik 2: `userland/src/inject.c` (`lsd` Bypass + `fix124` Tek Kare Optimizasyonu)

### 2A. `hakios_daemon_should_bypass_xpc` ve `my_xpc_connection_create_mach_service` Güncellemesi
`userland/src/inject.c` içindeki `6. INTERPOSITION` bloğunda (`hakios_daemon_should_bypass_xpc`, `my_xpc_connection_create_mach_service` ve `my_xpc_connection_send_message_with_reply_sync`), `com.apple.mobileactivationd` servisini bypass listesine ekle ve `lsd`'nin senkron XPC çağrılarını logla:

```c
static int hakios_daemon_should_bypass_xpc(const char *name)
{
	const char *prog;
	if (name == NULL) {
		return 0;
	}
	/* cfprefsd ve mobileactivationd imajda yok veya senkron cagrida kilitliyor (fix138). */
	if (strcmp(name, "com.apple.cfprefsd.daemon") == 0 ||
	    strcmp(name, "com.apple.cfprefsd.agent") == 0 ||
	    strcmp(name, "com.apple.cfprefsd.daemon.system") == 0 ||
	    strcmp(name, "com.apple.mobileactivationd") == 0 ||
	    strcmp(name, "com.apple.diagnosticd") == 0 ||
	    strcmp(name, "com.apple.analyticsd") == 0 ||
	    strcmp(name, "com.apple.powerlog.plxpclogger.xpc") == 0) {
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

	if (hakios_daemon_should_bypass_xpc(name)) {
		static unsigned bypass_logged;
		if (bypass_logged < 32) {
			bypass_logged++;
			say("hakios: %s fix138 xpc sync bypass \"%s\" -> empty dict\n",
			    prog ? prog : "?", name ? name : "?");
		}
		return xpc_dictionary_create(NULL, NULL, 0);
	}
	if (prog != NULL &&
	    (strcmp(prog, "runningboardd") == 0 || strcmp(prog, "lsd") == 0) &&
	    daemon_sync_logged < 48) {
		daemon_sync_logged++;
		say("hakios: %s xpc sync send[%u] -> \"%s\"\n",
		    prog, daemon_sync_logged, name ? name : "(anon)");
	}
	return xpc_connection_send_message_with_reply_sync(connection, message);
}
```

### 2B. `inject.c` İçinde `fix124` Döngüsünü 6 Kare Yerine 1 Kareye İndirme ve `lsd` Logu
- `userland/src/inject.c` içinde `watch_service_probe` fonksiyonundaki (**satır 4625 civarı**) `for (int frame = 0; surf != NULL && frame < 6; frame++)` satırını tek çekirdekte (`cpus=1`) `SpringBoard` ve `lsd`'den CPU çalmaması için `frame < 1` yap:
```c
			for (int frame = 0; surf != NULL && frame < 1; frame++) {
```
- `userland/src/inject.c` en sonundaki `hakios_init` fonksiyonuna (`else if (strcmp(name, "runningboardd") == 0)` bloğunun altına) şunu ekle:
```c
	} else if (strcmp(name, "lsd") == 0) {
		say("hakios: lsd injected (pid=%d)\n", getpid());
	}
```

---

## Değişiklik 3: `userland/src/inject_sb.c` (`ulock` Sahibi Tespiti + `lsd.mapdb` / `(no name)` Timeout Koruması)

### 3A. `dump_threads` Fonksiyonunu Güncelle (`userland/src/inject_sb.c` Satır 491–534)
Darwin `libsystem_platform` içinde `os_unfair_lock` (`0x234bba4b8`), kilit sahibinin `tid` (`id.thread_id`) değerini değil, **Mach thread port adını (`threads[i]`, örn. `0x3003`)** düşük 2 biti maskelenmiş olarak (`0x3002`) tutar. `dump_threads` fonksiyonunu kilit sahibi thread'in (`threads[3]`) tam çağrı zincirini ve `mach_msg2_trap` hedefini basacak şekilde değiştir:

```c
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
					probe_port("owner-rname", (mach_port_name_t)(st.x[5] >> 32));
					probe_port("owner-remote", (mach_port_name_t)(uint32_t)st.x[3]);
				}
			}
		}
		mach_port_deallocate(mach_task_self(), threads[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_t));
}
```

### 3B. `sb_xpc_send_sync` Fonksiyonunu Güncelle (`userland/src/inject_sb.c`)
`userland/src/inject_sb.c` içindeki `sb_xpc_send_sync` fonksiyonunu, `"com.apple.lsd.mapdb"`, `"com.apple.lsd.modifydb"` ve `name == NULL` (`"(no name)"` — `lsd`'nin döndürdüğü anonim XPC endpoint'leri) çağrılarını loglayacak ve **10 saniyelik timeout** ile koruyacak şekilde değiştir (böylece `lsd` yanıt vermezse veya gecikirse Thread 3 sonsuza dek bloklanmaz, `0x234bba4b8` kilidini serbest bırakır ve `SpringBoard` ana thread'i ulock'tan çıkar):

```c
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
	/* fix118/fix138: cfprefsd ve mobileactivationd senkron cagrilarinda hemen bos sozluk don */
	if (name != NULL && (strcmp(name, "com.apple.cfprefsd.daemon") == 0 ||
	                     strcmp(name, "com.apple.cfprefsd.agent") == 0 ||
	                     strcmp(name, "com.apple.mobileactivationd") == 0)) {
		return xpc_dictionary_create(NULL, NULL, 0);
	}
	/* fix136: runningboardd icin 20 sn timeout + BSXPCCoder fallback */
	if (name != NULL && strcmp(name, "com.apple.runningboard") == 0) {
		static dispatch_queue_t replyq;
		__block xpc_object_t reply = NULL;
		dispatch_semaphore_t done;

		if (replyq == NULL) {
			replyq = dispatch_queue_create("hakios.rbs.reply", DISPATCH_QUEUE_CONCURRENT);
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
	/* fix138: CoreServices (com.apple.lsd.mapdb ve lsd'nin dondurdugu "(no name)" anonim XPC
	 * endpoint'i) cagrilarinda Thread 3 global 0x234bba4b8 os_unfair_lock kilidini tutarken
	 * sonsuz mach_msg2_trap icine girip SpringBoard ana thread'ini ulock_wait'te dondurmasin!
	 * 10 sn icinde gercek yanit gelirse kullan, gelmezse veya error donerse bos dict donerek
	 * CoreServices kilidini serbest birak. */
	if (name == NULL || strncmp(name, "com.apple.lsd", 13) == 0) {
		static dispatch_queue_t lsd_replyq;
		__block xpc_object_t reply = NULL;
		dispatch_semaphore_t done;
		const char *label = name != NULL ? name : "(no name)";

		if (lsd_replyq == NULL) {
			lsd_replyq = dispatch_queue_create("hakios.lsd.reply", DISPATCH_QUEUE_CONCURRENT);
		}
		done = dispatch_semaphore_create(0);
		xpc_connection_send_message_with_reply(connection, message, lsd_replyq, ^(xpc_object_t object) {
			reply = object != NULL ? xpc_retain(object) : NULL;
			dispatch_semaphore_signal(done);
		});
		if (dispatch_semaphore_wait(done,
		    dispatch_time(DISPATCH_TIME_NOW, 10000000000ull)) == 0 && reply != NULL) {
			xpc_type_t t = xpc_get_type(reply);
			const char *tname = t != NULL ? xpc_type_get_name(t) : "null";
			say("hakios: sb fix138 sync reply from \"%s\" arrived (type=%s)\n", label, tname);
			sb_xpc_say_object("fix138 reply", label, reply);
			if (tname != NULL && strcmp(tname, "dictionary") == 0) {
				return reply;
			}
			say("hakios: sb fix138 reply from \"%s\" is %s, returning empty dict to release ulock\n",
			    label, tname ? tname : "null");
			return xpc_dictionary_create(NULL, NULL, 0);
		}
		say("hakios: sb fix138 sync send to \"%s\" timed out after 10 s -> returning empty dict to release CoreServices ulock!\n",
		    label);
		return xpc_dictionary_create(NULL, NULL, 0);
	}
	return xpc_connection_send_message_with_reply_sync(connection, message);
}
```

---

## Değişiklik 4: Derleme ve Paketleme

Değişiklikleri kaydettikten sonra sırasıyla şu komutları çalıştır:

```bash
cp -p userland/build.sh /home/atakan/hackios-work/userland/build.sh
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
