# HackiOS (General Mobile GM 5 Plus / MSM8952 -> iOS 16.5 Bare-Metal) Devir ve Durum Raporu

**Tarih:** 30 Ekim 2026 (sabah 06:55; fix56 turu koşuyor — görev `bymcn4w0m`)
**Hedef Cihaz:** General Mobile GM 5 Plus (`shamrock` / MSM8952 / Adreno 405 / 3GB RAM)
**Hedef Sistem:** iOS 16.5 (Darwin / XNU xnu-8796.121.2)
**GitHub Deposu:** `https://github.com/Noniksz/gm5plus-ios-kernel.git` (son commit: `edce03e`; çalışan ağaçta commit'siz düzeltmeler var)

---

## 0. Şu anki anlık durum (30 Ekim 06:40)

- Cihaz hâlâ fix54 XNU'sunun içinde (mavi ekran); `/dev/ttyACM1` var ama XNU girdi kabul etmiyor (bilinen
  gerçek: XNU çalışırken ACM yalnızca yazar). Sabah `--reboot-only` denendi: sessiz, shell değil.
- fix54 boot oldu (sabah kullanıcı VOL-+Power basmış, log `out/capture-windowserver-fix54.log` 3400 satır):
  unfreeze thread'i **trie gezintisinin ilk okumasında** SIGSEGV yiyor → backboardd 4 generation (pid 12,19,23,24)
  çöküyor, hiçbir "freeze flag" satırı basılmıyor. Sabit adres `0x8dfbf730` (`exc=1 code=0x1` =
  KERN_INVALID_ADDRESS): __LINKEDIT segment köprüsü yanlış adres üretti ve fix54 ona güvenip okudu.
- Rain hiç ateşlenmedi (thread her generation ile ölüyor); son generation (25) log sonunda canlıydı.
- **fix55 hazır ve sıraya girdi:** `out/shamrock-shim-boot.img` (06:39, rain dosyalı; yeniden sıralanmış
  unfreeze thread'li son sürüm). Arka planda
  `tools/autoloop.py --tag fix55 --wait 21600 --seconds 660 --idle 240` (görev `by5r2zcnn`, çıktı
  `out/autoloop-fix55.txt`) fastboot'u kolla; cihaz düşünce (VOL-+Power / pil -> Android -> adb fallback)
  fix55'i boot eder, loglar, shell'den reboot ile fastboot'a döner.

## 1. Döngü artık insansız çalışabiliyor (yeni!)

Kapan yol haritası: **rain** — `/redwood/etc/rain` imajda varsa backboardd 300. saniyede launchd'a
SIGSEGV atar -> XNU "Always panic for launchd" ile panic eder (`kern_exit.c:1670`) -> yamalı
`panic_trap_to_debugger` **`loader_return()`** çağırır -> shim USB shell'i aynı CDC-ACM portunda
dirilir -> `tools/autoloop.py` shell'e `reboot` yazar -> fastboot. `reboot(2)` kendisi işe yaramaz:
`halt_all_cpus` wfe spin'e çevrili (hakios.patch).

- `RAIN=1 ./build.sh` -> `root/etc/rain` eklenir (build.sh'e hook eklendi, hem repoda hem worktree'de).
- Rain'siz normal boot için RAIN=1 verme; inject `stat()` ile bakar, yoksa hiçbir şey olmaz.
- `tools/autoloop.py` tek tur yapar: boot-capture + shell'e ulaşıp reboot + logda anahtar desenleri
  sayan özet. `--reboot-only` ayrı mod: sadece seri portta kim var bakar, shell ise reboot eder.
- `fastboot boot` zinciri bozulmadı: lk2nd -> shim -> `boot`. Shim shell komutları: `log`, `peek`,
  `poke`, `test`, `disp`, `reboot`/`fastboot`...

## 2. Freeze-flag dosyası — ne biliniyor (fix51->fix54)

Hedef: QuartzCore `_kCAWindowServerDisableUpdatesOnMainDisplay`. backboardd boot'ta set ediyor
("disabling framebuffer changes on display (1)..."), display hazır olunca temizlemesi gerekiyor;
bu portta kimse temizlemiyor -> window server hiç yüzey ayırmıyor -> mavi ekran.

| Tur | Ne denendi | Sonuç |
|---|---|---|
| fix51 | dlsym (underscore'lu ad) | sembol bulunamadı; backboardd `startup complete` (ilk temiz boot) |
| fix52 | dlsym C-adı ile bulundu: `0x1f2bc2730`, değer **120** | ilk `*flag=0` yazımı **KERN_PROTECTION_FAILURE** (UX_EXCEPTION exc=1 code=2) -> SIGBUS, backboardd 4 kez crash-loop. RO sayfaya yazıldı; üstelik değer boolean değil -> dlsym'in adresi backboardd'ın yazdığı yer değil |
| fix53 | Export trie'yi **cache dosyası ofseti**nden okumayı denedi (`cache_base + export_off`) | Thread **hiçbir şey loglamadan sonsuz döngüye girdi**: cache split (.01...42, .dyldlinkedit), ofsetler lineer değil -> trie sanılan text bölgesinde cycle. Zararsızdı (backboardd 102 sn'de `startup complete` — fix51'denberi ilk tam boot) ama verimsiz |
| fix54 | Trie, QuartzCore'un **kendi `__LINKEDIT` segmenti köprüsü**nden: `runtime = slide + linkedit_vmaddr + (trie_off - linkedit_fileoff)`; döngü korumalı gezgin | **SIGSEGV**: köprü yanlış adres üretti (0x8dfbf730, haritasız), gezginin ilk okuması thread'i öldürdü; 4 backboardd generation düştü, hiç satır basılmadı. Segment köprüsü yalnız segment bellekte bitişikse doğru — split cache'te değil |
| fix55 (hazır, sırada — img 06:39) | **Hiçbir hesaplanan adrese dokunmadan önce kanıt**: `safe_copy()` = `vm_read_overwrite` (sinyal yerine KERN_* döner); trie **yerel tampona kopyalanıp** üzerinde geziniyor; üç aday: segment köprüsü, cache header mapping tablosu, fix53'ün `cache+off` tahmini (hepsi artık zararsız); hedef **QuartzCore'un `__DATA*`/`__AUTH*` segmentlerine düşmüyorsa reddedilir**; tüm ilk okumalar `peek_byte`. **Host dry-run sonucu** (`/tmp/dsc-x/fix55_dryrun.py`, gerçek 20F66 cache'i): yük komutlarındaki fileoff'lar *lojik* (birleşik cache) semantiğinde ve **ana header'ın mapping tablosunda tek kayıt var** (kendi 0x5c000 baytı) -> aday 1 fix54'teki gibi taşar (emniyetli fail), aday 2 boştan döner; **yani trie muhtemelen bulunamayacak** | Cihaz bekleniyor |
| fix55 asıl kozu | **Veri-izleyici yeniden sıralandı**: thread artık (1) QuartzCore'un haritalanmasını bekliyor (60 sn'ye kadar), (2) **bayrak set edilmeden (~60 sn) hemen snapshot alıyor**, (3) trie'yi bir kez deniyor, (4) 300 sn izliyor. Tek bit 0->1 gördüğünde **`QuartzCore + 0x... (living at ADRES): ... (bit set)`** — mutlak adresi yazıyor. Snapshot→set sırası garantili | — |

**fix55 SONUÇ (koştu, `out/capture-windowserver-fix55.log` 308 KB):**
- **Büyük ilerleme:** unfreeze thread'i artık çökmüyor — izleyici `watching 5213 words of QuartzCore's data`
  satırını bastı (fix54'te hiç basılmıyordu). `safe_copy`/`peek_byte` sınıfı kriz bitti.
- **Trie teşhis satırları hiç basılmadı** (`cache at`, `QuartzCore at`, `trie candidate` = 0): çünkü
  `trie_size == 0` continue'u teşhis bloğundan ÖNCE çalışıyordu. Bu QC imajında export trie load komutu
  yok demek (iOS16 chained-fixups beklentisi). fix56'da teşhis bloğu continue'dan ÖNCEYE alındı +
  **tüm load-komut kodları dökülüyor** (`cmd N: 0x...`) -> gerçek neden logda görünecek.
- **backboardd hâlâ düşüyor ama artık başka yerde:** `UX_EXCEPTION p=backboardd[12] exc=1 code=0x1
  subcode=0x0 -> sig=11` (NULL/0x0), `pc 0x1a45d0928` — bu adres cache local-symbols blob'u içinde
  (header 0x1a45bf000..0x1a45d41b0), yani bir fonksiyon pointer'ı bozuk veriaya atlıyor. fix52/53/54'teki
  bizim-okuma krizimiz DEĞİL; muhtemelen backboardd'ın kendi yolunda. İzleyici snapshot'ı bundan ÖNCE
  alındığı için veri-izleyici yaşıyor olabilir — fix56 logunda `bit set` satırları aranacak.
- Rain satırı basıldı; ama `XNU KERNEL PANIC` / shell yok (backboardd erken öldü, 300 sn'ye ulaşamadı).
- **fix56:** dlsym'i yalnız underscore'lu isimle daralttı (underscore'suz C-adı lookup'ı aynı bozuk
  blob'a dalıp her generation'ı öldürüyordu — fix55 krizinin kökü bu); teşhis bloğu öne alındı;
  load-komut dökümü eklendi. Tur koşuyor (görev `bymcn4w0m`).

**fix56 karar ağacı** (`out/capture-windowserver-fix56.log`):
- `cmd N: 0x80000033` (EXPORTS_TRIE) veya `0x80000022` (DYLD_INFO_ONLY) görürsem -> trie var, neden
  `trie_size==0` gördüğüm netleşir (muhtemelen yanlış load-komut yürüyüşü); `trie candidate` satırları gelir.
- Hiç export komutu yoksa -> export'u cache global export trie'sundan bulmam lazım (dyld4 exportedSymbols);
  o ayrı iş. Bu durumda tek umut veri-izleyici: `living at 0x...` + `bit set` satırları.
- `living at 0x...` + `disabling framebuffer changes` satırı yakınındaki bit-set → gerçek bayrak;
  fix57 = aynı adresi N kez 0->1 görürse make_writable + sıfırla.
- Autoloop'un kick ETIMEDOUT'u artık düşürmüyor (try/except).

## 3. Sıradaki ajan için açık konular

1. **fix54 logu okundu** (30 Ekim sabah): tek bulgu SIGSEGV (yukarıdaki tablo). **fix55 logunu oku**
   (`out/capture-windowserver-fix55.log`): yukarıdaki teşhis satırları. Frame aktıysa kullanıcıdan panel
   fotoğrafı iste (renk kanalları ters olabilir: shim `con_clear(RED)` bu panelde mavi görünüyor).
   Not: rain bu turda ateşlenmedi (thread backboardd ile birlikte ölüyor); fix55 backboardd'ı ayakta
   tutarsa panic→shell→reboot zinciri çalışır. Çalışmazsa: shell'e dönüş yoksa cihaz XNU içinde kalır,
   pil/tuş döngüsü gerekir.
2. Host statik analizi: ipsw ve r2 bu cache için `.symbols` subcache'i istiyor (bizde yok) —
   **runtime export-trie okuması çalışıyor**, buna güven. ipsw: `/tmp/opencode/ipswbin/ipsw`.
3. `+[CAWindowServer serverWithOptions:]` hâlâ hiç çağrılmıyor; `CAWindowServerDisplay` metod
   dökümü (`class_copyMethodList` + tip koduna göre trampolin) sırasını bekliyor.
4. **Commit bekliyor:** `xnu/hakios.patch` + `MiniIOMobileFramebuffer.cpp` (üç kernel düzeltmesi:
   serial_putc_options guard, vc_clear_screen/vc_progress_task guard, VM_WIMG_WCOMB) + bu gecenin
   inject.c/build.sh/autoloop.py değişiklikleri. Kullanıcı onayıyla commit'lenmeli.
5. Varsa `no page to give` / `would not take` satırları: MAP_FIXED anonim sayfa değişimi bu portun
   vm_map'inde çalışmıyor demek — o zaman `vm_remap(VM_PROT_COPY)` alternatifini dene.

## 4. Kritik Teknik Notlar (kaybolmasın)

- **Kernel'i yalnız mutlak yolla derle:** `/home/atakan/GM5PlusPort/hackios/xnu-build.sh -j16 -k`.
- **`cpus=1` bilinçli**; **`IOSleep` kullanılamaz**; **`PE_initialize_console(NULL, kPEDisableScreen)`
  swap yolunda çağrılmamalı**. Kernel text base `0xfffffff007004000`; sembolize: `llvm-nm`/`llvm-objdump`.
- **Panel gerçeği** (MDP5 probe): `rev=100C0000`, `ctl=00001F20` (video mode), `base=83200000`,
  `stride=0CA8` (3240), `fmt=0002243F`, 1080x1920@24bpp; yüzey 1125x2436 -> `drawLayer` ölçekliyor.
- **Stok kök:** `/home/atakan/hackios-work/sysimage/rootfs`; DSC `.../20F66__iPhone10,3_6/dyld_shared_cache_arm64*`
  (split). `/home/atakan/hackios-work/userland` gerçek derleme ağacı; repodan `src/inject.c` ve
  `build.sh` kopyalanır (`cp -p`).
- **`shim-shamrock/` gitignore'da**; `shim-shamrock/build.sh ../out/shamrock-payload.bin` hem kendi
  out/'una hem repo out/'una yazar.
- **Loglar ikili** (NUL): grep'te `-a` kullan. Her satır konsol aynası yüzünden iki kez basılıyor.
- **`-v` boot-arg'ı mavi ekran sebebi değildi**; `CA_FORCE_TIMER=1` erken reset attırtıyor (0'da kalsın).

## 5. İlgili Dosyalar

| Ne | Repo | Derlemede kullanılan |
|---|---|---|
| Kernel fb sürücüsü | `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` | `$XNU_SRC/iokit/Kernel/…` |
| Log aynası guard'ı + WCOMB | `xnu/hakios.patch` | `pe_kprintf.c`, `video_console.c` |
| Userland kancaları (unfreeze, rain, trie) | `userland/src/inject.c` | `/home/atakan/hackios-work/userland/src/inject.c` |
| Rain hook'u | `userland/build.sh` (RAIN=1) | aynı |
| İnsansız tur | `tools/autoloop.py` | — |
| Boot + log | `tools/boot-capture.py` (adb->bootloader fallback'lı) | — |
| Shim (shell, loader_return, charger) | `shim-shamrock/` (gitignore) | `shim.c` satır 263+: shell |

## 6. fix128 / fix129 (1 Kasım 2026) — panel mavi, ama artık kök neden belli

fix127 (`ca_vbl=1`, 60 Hz vsync çalışıyor, 27 swap, kr 0): panel hâlâ mavi/siyah. 60 Hz vsync geçişi kare oranını 0,5 fps'ten hareket ettirmedi -> vsync açlığı değil.

fix128 (çekirdekte `surface probe` eklendi, `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`):
- swap 1: `head ffffffff mid ff0000ff tail ff0000ff nz=1024/1k` — yüzey **boş değil**, başlangıçta renk barları.
- swap 2..6: `head 0000ffff mid ff0000ff tail ff0000ff nz=512/1k` — **her swapte aynı statik mavi-tonlu içerik**; dst=fBase, pushPanel/CTL_START çalışıyor.
- Çıkarım: kernel→panel yazım yolu tamamen çalışıyor; panel mavi çünkü CoreAnimation tek statik bir kare (mui muhtemelen ilk temizleme) çizip asla güncellemiyor. `kick_display`: begin=finish=render=0.

fix129 (userland/src/inject.c `ca_display_probe_fixes`): backboardd'ın boot'ta yazdığı ve asla temizlemediği
  `disabling framebuffer changes on display (1)` (`_kCAWindowServerDisableUpdatesOnMainDisplay`, fix128 log line 1615) işletim sistemi dondurma bayrağı. `-disablesUpdates` getter'ını loglar, `-setDisablesUpdates:` çağrısında değeri her zaman **NO** yapar.
- Imaj hazır: `out/shamrock-shim-boot.img` (1 Kasım 12:48, fix127 cmdline'ıyla aynı: `ca_vbl=1 fb_vsync_*`...).
- **ENGEL:** cihaz şu an XNU içinde (fix128'den sonra kalıyor, rain yok, otomatik geri dönüş yok). `fastboot devices` boş. fix129'u boot edebilmek için cihaz `VOL- + Power` ile fastboot'a alınmalı, sonra:
  `python3 tools/autoloop.py --tag fix129 --wait 30 --seconds 660 --idle 240`

fix129 başarı kriteri: `disablesUpdates -> 0` satırları + swap'ler arasındaki yüzey içeriği değişmeye başlayıp panelde canlı içerik. Başarısız olursa sonraki adım `+[CAWindowServer serverWithOptions:]` çağrısı / CADisplayTimer akışı (fix77/78 yöntemiyle `-update` çağrılarının gerçekten düştüğü yeri bulmak).

## 7. Derleme ve Test (tek tur)

```bash
# userland değiştiyse
cp -p userland/build.sh /home/atakan/hackios-work/userland/build.sh
cp -p userland/src/inject.c /home/atakan/hackios-work/userland/src/inject.c
(cd /home/atakan/hackios-work/userland && RAIN=1 ./build.sh)
cp -f /home/atakan/hackios-work/userland/root.apfs out/root.apfs

# paketle
XNU_SRC=/home/atakan/hackios-work/xnu-src/xnu-8796.121.2   # ayrı satır: mkpayload args'ta genleşsin
python3 ios-sweet/tools/mkpayload.py --ramdisk out/root.apfs \
  --cmdline "-v debug=0xc cpus=1 keepsyms=1 rd=md0 emmc_slice_part=userdata root_apfs=disk1 hv_wdt_pet=1" \
  $XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  out/shamrock.adt out/shamrock-payload.bin
(cd shim-shamrock && PATH="$HOME/.local/bin:$PATH" ./build.sh ../out/shamrock-payload.bin)

# insansız tur (fastboot'a düşünce boot eder, loglar, shell'den reboot, fastboot'a geri döner)
python3 tools/autoloop.py --tag fixNN --wait 120 --seconds 660 --idle 240
```

Kernel değiştiyse: `cp -p xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp $XNU_SRC/iokit/Kernel/` +
`PATH="$HOME/.local/bin:$PATH" /home/atakan/GM5PlusPort/hackios/xnu-build.sh -j16 -k`.
