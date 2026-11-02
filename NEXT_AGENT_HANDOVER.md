# HackiOS — GM 5 Plus (MSM8952) → iOS 16.5 Bare-Metal: Devir ve Durum Raporu

**Tarih:** 2 Kasım 2026, 18:05
**Cihaz:** General Mobile GM 5 Plus (`shamrock`), Snapdragon 617 (MSM8952), 8x Cortex-A53, Adreno 405, 3 GB RAM, 1080x1920 panel
**Hedef:** iOS 16.5 userland (Darwin / XNU `xnu-8796.121.2`), bare-metal
**Repo:** `https://github.com/Noniksz/gm5plus-ios-kernel.git`
**Son commit:** `ac9bc9e` — *revert: sb wake'i fix168 haline döndür* (önceki: `4d2a00c` fix174, `2679132` fix63)
**Çalışma ağacı:** temiz (revert commit'lendi; `venv/` artık `.gitignore`'da)

> **Not:** Eski dokümanlarda geçen `/media/ogl/ogl` **artık yok**. Doğru repo kökü
> `/media/atakan/Yeni Birim/GM5PlusPort`; `/home/atakan/GM5PlusPort` aynı içeriğin diğer mount'u.

---

## 0. Anlık durum (bu satırlar yazılırken)

- Kullanıcı cihazı **fastboot'a** aldı (`dfe7d673`, `18d1:d00d`).
- `out/shamrock-shim-boot.img` **fix168 kaynağıyla yeniden derlendi** (2 Kas 10:58) ve şu an boot testi
  çalışıyor: `out/live-fix168.log`, sürücü çıktısı `out/live-fix168-run.txt`.
  Zincir: lk2nd stage-1 `OKAY` → shim stage-2 `OKAY` → XNU konsolu `1209:0001` açıldı.
- Bu koşuda gözlenen imza (fix168, ~8. dakika): `startup complete` = 1, `XNU KERNEL PANIC` = 0,
  `StartWindowServer` = 6, `swap` = 18 (artıyor), **`render=0`** (57 örnek; ekran siyah), `vsync #` = 0,
  `registerNotificationPort` = 0, `System app disconnected` = 0 (fix173'te 1 idi; tura göre değişiyor).
- **Boot partition ERASE durumda.** Dün gece `fastboot flash boot` + `fastboot erase boot` yapıldı.
  Normal power-on boot partition'dan gidemez; cihaz fastboot'a alınıp lk2nd zinciriyle boot edilmeli.
- Dün gece fix169–174 denemeleri **geri alındı** (ayrıntı §3). `inject_sb.c` birebir fix168 snapshot'ından
  geri yüklendi; `inject.c`, kernel ve shim `Image.dtb` zaten fix168 ile aynıydı.

## 1. Ortam ve yollar

| Ne | Yol |
|---|---|
| Repo kökü | `/media/atakan/Yeni Birim/GM5PlusPort` (`/home/atakan/GM5PlusPort` eş içerik) |
| Gerçek derleme ağacı | `/home/atakan/hackios-work/userland` |
| XNU kaynağı | `/home/atakan/hackios-work/xnu-src/xnu-8796.121.2` |
| XNU derleme çıktısı | `$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837` |
| Kernel derleme script'i | `/home/atakan/GM5PlusPort/hackios/xnu-build.sh -j16 -k` (**mutlak yol şart**) |
| Stok kök | `/home/atakan/hackios-work/sysimage/rootfs` |
| DSC (split) | `/home/atakan/hackios-work/20F66__iPhone10,3_6/dyld_shared_cache_arm64*` |
| Shim (gitignore) | `shim-shamrock/` (`shim.c`, `loader.c`, `display.c`, `ci_usb.c`, …) |
| Çıktı imajları | `out/root.apfs`, `out/shamrock-payload.bin`, `out/shamrock-shim-boot.img` |

- **`tools/`, `scripts/`, `shim-shamrock/` `.gitignore`'da.** Yeni araç dosyaları `git add -f` ister;
  mevcut `tools/*.py` dosyaları geçmişte izlenmiş olduğu için takip edilmeye devam eder.
- `RAIN=1 ./build.sh` → `root/etc/rain` ekler (panic → `loader_return` → shim shell → reboot otomasyonu).

## 2. Boot zinciri ve komutlar

```
PBL → stok lk (fastboot) ──fastboot boot tools/lk2nd-msm8952.img──→ lk2nd (kendi fastboot'u)
    ──fastboot boot out/shamrock-shim-boot.img──→ shim (Image.dtb)
    → ABL ramdisk'indeki SWEETXNU payload (out/shamrock-payload.bin) → XNU
```

- **Doğrudan shim'i stok lk ile boot etmek ÇALIŞMAZ:** `remote: 'dtb not found'`. lk2nd zorunlu.
- Tek tur (stage-1 + stage-2 + seri yakalama):
  ```bash
  python3 tools/boot-capture.py --wait 20 --seconds 420 --idle 90 --log out/live-fixNN.log
  ```
- Cihaz zaten lk2nd fastboot'ta ise: `--skip-lk2nd`.
- İnsansız tur (boot + shell'e `reboot` + log özeti): `python3 tools/autoloop.py --tag fixNN --wait 120 --seconds 660 --idle 240`
- Cihazı fastboot'a alma: **VOL− + Power** (kullanıcı yapar). `fastboot devices` yalnız `18d1:d00d`'de görünür.
- XNU konsolu: `1209:0001` ("HackiOS XNU Console"), `/dev/ttyACM*`. XNU çalışırken port **yalnız yazar**;
  `shamrock>` shell'i yalnız XNU öncesi / panic dönüşünde vardır. Seri porta yazmadan önce DTR/RTS
  toggle'ı (kick) gerekir; `ttyACM` numarası yeniden enumere olunca değişebilir.

## 3. Siyah ekranın kök neden zinciri (ana açık iş)

1. SpringBoard ana thread'i backboardd'a giden bir `mach_msg2_trap` içinde **sonsuza bloklanıyor**
   (gözlenen id: `0x5b9171`; diğeri `0x5b8db2`).
   - `mach_msg2` alan eşlemesi (inject_sb.c'deki `dump_mach_msg`/watchdog kodu): `x1` = options
     (bit0 = SEND, bit1 = RECV), `x4 >> 32` = `msgh_id`, `x5 >> 32` = reply port (`rname`).
2. backboardd'ın display server'ı vsync bekliyor; **`vsync #` hiç ateşlenmiyor**.
3. `MiniIOMobileFramebuffer` (`xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`):
   - `vsync()` yalnız `fCallbacks[kFBNotifyVSync] != 0 && fPorts[kFBNotifyVSync] != NULL` ise panel push eder
     (`vsync()` ~1562; `kFBNotifyVSync` = 5).
   - `registerNotificationPort` ("notification port for type %u" loglar) **hiç çağrılmıyor** → sayaç 0.
   - selector **72** (`kFBMethodNotifications`, ~1757) **hiç çağrılmıyor** → callback/port kurulmuyor.
   - backboardd'ın gerçekten çağırdığı selector'lar: 3, 4, 5, 8, 15, 28, 29, 33, 57, 74, 79 (72 yok).
4. Sonuç: CA hiç render etmiyor (`kick_display`: begin=finish=render=0; 88 örnek), `swap` ≈ 33 (kernel
   renk barlarını gönderiyor, panel yolu sağlam), watchdog **"System app disconnected"** ile SB'yi öldürüyor,
   SB yeniden spawn (pid 7/23/26/30), ekran siyah.
5. Dün geceki uyandırma denemeleri (fix169–174) **başarısız**: `mach_msg` + `MACH_SEND_TIMEOUT` (10 ms) bile
   dönmüyordu; `mach_msg_send`'e geçiş test edilemeden cihaz düştü → **fix168'e geri dönüldü**.
   fix168'deki wake yolu: 1 s örnekleme (`WATCH_SAMPLES 300`), yalnız `0x5b8db2`/`0x5b9171` için 40 kez
   "fake reply" (sadece `mach_msg_header_t`, `MACH_SEND_MSG`).

**Panel/CA notları (fix128/129):**
- MDP5 **video mode**; kernel→panel yazımı çalışıyor (`pushPanel`/`CTL_START`; swap 1'de renk barları görünür).
- Panel mavi/siyah çünkü CoreAnimation tek statik kare çizip güncellemiyor; vsync açlığı birincil şüpheli.
- `inject.c` (fix129, hâlâ aktif): backboardd'ın `disabling framebuffer changes on display (1)` bayrağına
  karşı `-setDisablesUpdates:` her zaman **NO** yapar; `-disablesUpdates` getter'ı loglanır.

## 4. Dün gece (fix165→174), geri alma ve gerçekler

- fix165–168: inject_sb.c'de SB watchdog/wake denemeleri; hepsinde boot tamamlandı, render=0.
- fix169–174: 100 ms örnekleme (`WATCH_SAMPLES 6000`), her `snd&&rcv`'de uyandırma (cap 400),
  `mach_port_insert_right`, 32 baytlık MIG_BAD_ARGUMENTS (0x10000003) yanıtı, `mach_msg_send`.
- **fix174 sonrası cihaz EDL'ye düştü.** Kök neden: **düşük pil** — lk2nd stage-1 boot sırasında brownout →
  watchdog → EDL. 90 saniye şarjdan sonra aynı `fastboot boot tools/lk2nd-msm8952.img` **sorunsuz `OKAY`**
  verdi. Yani "lk2nd bozuldu" değildi.
- Kullanıcı "önceki durum daha iyiydi" dedi → **fix168'e geri dönüldü** (`ac9bc9e`):
  - fix169–174 boyunca değişen **tek dosya** `userland/src/inject_sb.c` idi (diğer dosyaların mtime'ları
    fix165'ten önce).
  - Geri yükleme yöntemi: opencode snapshot ağacından birebir çekildi (bkz. §8). Doğrulama: snapshot ile
    `git diff` **boş**; `WATCH_SAMPLES 300`, `"sb wake: fake reply"` geri geldi; `mach_msg_send` yok.
  - `root.apfs` + `out/shamrock-shim-boot.img` bu kaynakla yeniden derlenip paketlendi (2 Kas 10:58).

## 5. EDL: tanıma, nedenler, kurtarma

| USB PID | Anlam |
|---|---|
| `18d1:d00d` | Fastboot (stok lk veya lk2nd) |
| `05c6:900e` | EDL **crash-dump** modu (hello'ya cevap vermez) |
| `05c6:9008` | EDL / Sahara |
| `1209:0001` | XNU seri konsolu (CDC-ACM) |

- EDL'ye düşme nedenleri: **(a) düşük pil** (en sık; şarj edince düzelir), (b) gerçek çökme.
- `fastboot devices` EDL'de **boş** döner. `sudo` yok, `qdl` kurulamıyor; `pyusb` sistem python'una
  `--break-system-packages` ile kurulu.
- **Kurtarma püf noktası:** `900e` modunda OUT yazımı, IN tarafı doluyken timeout verir. Çözüm: **IN
  endpoint'ini sürekli drain eden bir thread** çalıştırırken HELLO + RESET_REQ gönder. Aşağıdaki script
  bunu yapar; birkaç denemede `18d1:d00d`'ye döndürür:

```python
# /tmp/opencode/edl_reset.py  (python3, pyusb)
import usb.core, usb.util, struct, threading, time
def fb(): return usb.core.find(idVendor=0x18d1, idProduct=0xd00d)
for it in range(8):
    if fb(): print("FASTBOOT"); break
    dev = None
    for pid in (0x900e, 0x9008):
        dev = usb.core.find(idVendor=0x05c6, idProduct=pid)
        if dev: break
    if not dev:
        print(it, "nodev"); time.sleep(3); continue
    try:
        cfg = dev.get_active_configuration(); intf = cfg[(0, 0)]
        eo = ei = None
        for ep in intf:
            if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_OUT: eo = ep
            else: ei = ep
        try: dev.detach_kernel_driver(intf.bInterfaceNumber)
        except Exception: pass
        usb.util.claim_interface(dev, intf.bInterfaceNumber)
        stop = False
        def drain():
            while not stop:
                try: ei.read(512, timeout=150)
                except Exception: pass
        threading.Thread(target=drain, daemon=True).start()
        try:
            eo.write(struct.pack('<12I', 1, 48, 2, 1, 0x1000, 0, 0, 0, 0, 0, 0, 0), timeout=1500)  # HELLO
            eo.write(struct.pack('<2I', 7, 8), timeout=1500)                                        # RESET_REQ
            print(it, "sent hello+reset")
        except Exception as e:
            print(it, "werr", repr(e)[:40])
        stop = True
    except Exception as e:
        print(it, "err", repr(e)[:50])
    time.sleep(6)
    if fb(): print("BACK TO FASTBOOT it", it); break
```

- `usbreset` / `pyusb dev.reset()` cihazı bus'tan düşürebilir; panik yapma, yeniden enumere olmasını bekle.

## 6. Derleme ve paketleme (tek tur, kopyala-çalıştır)

```bash
cd "/media/atakan/Yeni Birim/GM5PlusPort"

# 1) userland (inject.c / inject_sb.c / build.sh değiştiyse)
cp -p userland/src/inject.c userland/src/inject_sb.c ~/hackios-work/userland/src/
cp -p userland/build.sh ~/hackios-work/userland/build.sh
(cd ~/hackios-work/userland && RAIN=1 ./build.sh)

# 2) payload + boot.img
cp -f ~/hackios-work/userland/root.apfs out/root.apfs
python3 ios-sweet/tools/mkpayload.py --ramdisk out/root.apfs \
  --cmdline "-v debug=0xc cpus=1 keepsyms=1 rd=md0 emmc_slice_part=userdata root_apfs=disk1 hv_wdt_pet=1" \
  ~/hackios-work/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  out/shamrock.adt out/shamrock-payload.bin
mkbootimg --header_version 0 --kernel shim-shamrock/out/Image.dtb --ramdisk out/shamrock-payload.bin \
  --base 0x80000000 --kernel_offset 0x00080000 --ramdisk_offset 0x01000000 --tags_offset 0x00000100 \
  --pagesize 2048 --cmdline "console=ttyHSL0,115200,n8 androidboot.hardware=qcom" \
  -o out/shamrock-shim-boot.img

# 3) boot + log
python3 tools/boot-capture.py --wait 20 --seconds 420 --idle 90 --log out/live-fixNN.log
```

- **Kernel değiştiyse** (`MiniIOMobileFramebuffer.cpp` vb.):
  `cp -p xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp $XNU_SRC/iokit/Kernel/` +
  `PATH="$HOME/.local/bin:$PATH" /home/atakan/GM5PlusPort/hackios/xnu-build.sh -j16 -k`.
- **Shim kaynakları değiştiyse:** `(cd shim-shamrock && PATH="$HOME/.local/bin:$PATH" ./build.sh ../out/shamrock-payload.bin)`.
  Shim payload'ı **ABL ramdisk'inden** okur; yalnız shim `.c`/`.S` değişince gerekir (dün gece gerekmedi).
- Derleme çıktısı `mkapfs.py: root.apfs: ...` satırıyla biter. `-j16` kullan.

## 7. Log okuma ve doğrulama

- Loglar **ikili** (NUL içerir): `grep -a` kullan. Her satır konsol aynası yüzünden iki kez basılır.
- Anahtar desenler ve sağlıklı değerler:

| Desen | Beklenen |
|---|---|
| `startup complete` | 1 |
| `XNU KERNEL PANIC` | 0 |
| `StartWindowServer` | 6 (yeniden spawn'larla) |
| `swap ` | 31–33 |
| `render=` | **0 (sorun)** |
| `vsync #` | **0 (sorun)** |
| `registerNotificationPort` | **0 (sorun)** |
| `System app disconnected` | 1 |
| `sb wake` / `sb mainq serviced` | 0 / 0 |

- Panik/istisna ayıklarken yalnız `panic()`, `ESR_EL1`, `FAR_EL1`, `PC`, `LR`, backtrace satırlarına bak.
- Sembolizasyon: `llvm-nm` / `llvm-objdump`; kernel text base `0xfffffff007004000`.

## 8. Eski duruma geri alma (çok işe yarar)

opencode her adımda çalışma ağacının git tree'sini saklıyor (proje `.git` objelerini alternate olarak
kullanır):

```
SNAP=/home/atakan/.local/share/opencode/snapshot/cde28fcf090263bf0b39a0a5008ad6161eb6e278/61dc0e6d12f38f71b258f3ef618e7540357c2282
git -C "$SNAP" show 273afc8024c3cba34a84962ba200e24f5c83f012:userland/src/inject_sb.c   # = fix168 hali
```

- `273afc80` = fix168 çalışma anı; `39288263` = fix168 derleme girdisi (tek satır fark: `msgh_voucher_port`).
- Zaman damgaları opencode DB'sinde: `~/.local/share/opencode/opencode.db` → `part` tablosu
  (`time_created`, `data` JSON; `type:"tool"`, `tool:"bash"|"edit"`, `state.input`).

## 9. Açık işler / önerilen sıra

1. **fix168 boot logunu doğrula** (`out/live-fix168.log`): `startup complete=1`, `render=0`, `vsync #=0`.
2. **vsync'i çalıştır (birincil):** `MiniIOMobileFramebuffer`'da `vsync()` guard'ını gevşet —
   `fCallbacks[VSync]`/`fPorts[VSync]` yoksa da `armVSync` zamanlayıcısı panel push etsin; backboardd'un
   display server'ı ilerlesin. Değişiklik sonrası bekle: `vsync #` > 0 ve `render` > 0.
3. **Alternatif/tamamlayıcı:** selector 72 çağrılmadığı için kernel içinden `kFBNotifyVSync` callback'i
   kur (IOFramebuffer/IOFBCursor user client yolu) veya SB wake'i kernel tarafında `mach_msg2_trap`
   seviyesinde çöz (kullanıcı tarafı `mach_msg` bu portta dönmüyor).
4. **Hızlı görünürlük kazanımı:** shim zaten boot sonunda `display_paint_test` + `display_flush` yapıyor
   (`loader.c:162`). XNU'ya geçmeden shim'de kalıp panelde desen görünüyor mu diye kullanıcıdan fotoğraf iste;
   görünüyorsa sorun tamamen XNU/backboardd katmanında demektir.
5. `+[CAWindowServer serverWithOptions:]`'in gerçekten çağrılıp çağrılmadığını doğrula; çağrılmıyorsa
   `CAWindowServerDisplay` metod dökümü + trampolin (fix77/78 yöntemi).
6. **boot partition:** normal power-on için lk2nd'i bootloader'a kur veya boot partition'a lk2nd'in
   boot edebileceği bir imaj koy (şu an ERASE). Karar kullanıcıya sorulmalı (brick riski).
7. `no page to give` / `would not take` görülürse: MAP_FIXED anonim sayfa değişimi bu vm_map'te çalışmıyor;
   `vm_remap(VM_PROT_COPY)` dene.

## 10. Tuzaklar (kaybolmasın)

- **Pil:** her boot denemesi pil yer; düşük pilde lk2nd → EDL. Boot öncesi 60–90 s şarj et.
- **Seri port:** XNU çalışırken ACM yalnız yazar; shell yok. DTR/RTS toggle (kick) olmadan konsol sessiz kalır.
  `ttyACM` numarası değişebilir; hung node'u zorlamak yerine yeniden enumere et.
- **`cpus=1` bilinçli.**
- **`IOSleep` kullanılamaz.**
- **`PE_initialize_console(NULL, kPEDisableScreen)` swap yolunda çağrılmamalı.**
- **`CA_FORCE_TIMER=1` erken reset attırır** (0'da kalsın). `-v` boot-arg'ı mavi ekran nedeni değildi.
- **Panel gerçeği (MDP5 probe):** `rev=100C0000`, `ctl=00001F20` (video mode), `base=83200000`,
  `stride=0CA8` (3240), `fmt=0002243F`, 1080x1920@24bpp; yüzey 1125x2436 → `drawLayer` ölçekliyor.
- **`fastboot erase boot` sonrası** normal power-on EDL'ye düşer; kurtarma fastboot + lk2nd zinciri.

## 11. Dosya haritası

| Ne | Repo | Derlemede kullanılan |
|---|---|---|
| Kernel fb sürücüsü / vsync | `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` | `$XNU_SRC/iokit/Kernel/` |
| Kernel yamaları (log aynası, WCOMB, loader_return) | `xnu/hakios.patch`, `xnu/files/osfmk/arm64/*` | XNU ağacı |
| Userland kancaları (unfreeze, rain, CA bayrağı) | `userland/src/inject.c` | `~/hackios-work/userland/src/` |
| SpringBoard kancaları (RBS, xpc, SB watchdog/wake) | `userland/src/inject_sb.c` | aynı |
| Rain hook'u | `userland/build.sh` (`RAIN=1`) | aynı |
| Boot + log | `tools/boot-capture.py` | — |
| İnsansız tur | `tools/autoloop.py` | — |
| lk2nd | `tools/lk2nd-msm8952.img` (stage-1) | — |
| Shim (shell, loader, display) | `shim-shamrock/` (gitignore) | `shim.c`, `loader.c`, `display.c` |
| Çıktı imajları | `out/root.apfs`, `out/shamrock-payload.bin`, `out/shamrock-shim-boot.img` | — |
| EDL kurtarma | §5'teki inline script | — |
