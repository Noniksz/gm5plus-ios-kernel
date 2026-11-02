#!/usr/bin/env python3
"""Build the Apple Device Tree (ADT) for General Mobile GM 5 Plus (shamrock - MSM8952).

SoC & Hardware:
  - Qualcomm Snapdragon 617 (MSM8952), 8x Cortex-A53 (ARMv8.0-A)
  - 3 GB LPDDR3 RAM (Base: 0x80000000)
  - GICv2 (Distributor: 0x0b000000, CPU Interface: 0x0b002000)
  - Framebuffer: 0x83200000, 1080x1920
  - Arch Timer: 19.2 MHz (19200000)
  - SDHCI: eMMC sdhc_1 @ 0x7824900, MicroSD sdhc_2 @ 0x7864900
  - I2C: BLSP1 QUP3 @ 0x78b7000 (Goodix GT970/GT911 @ 0x5d)
  - Device Identity: iPhone X (iPhone10,3 / D22AP) with Hactivation flags
"""
import hashlib, os, struct, sys

# Allow importing adt from ios-sweet/tools
TOOLS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../ios-sweet/tools')
sys.path.insert(0, TOOLS_DIR)
import adt

# Memory Map (MSM8952)
# Low DDR starts at 0x80000000. Carveouts (FB, modem, TZ, LK) occupy 0x80000000 - 0x8e000000.
# Upper safe DRAM pool for XNU kernel and iOS userland:
PHYS_BASE = 0x90000000
MEM_SIZE  = 0xb0000000  # 2.75 GB usable DDR RAM (0x90000000 - 0x140000000)

PANIC_LOG_SIZE = 0x100000  # 1 MiB embedded panic log at top of RAM

# MSM8952 MMIO
SOC_BASE = 0x01000000
GICD, GICD_SIZE = 0x0b000000, 0x1000
GICC, GICC_SIZE = 0x0b002000, 0x1000
FB, FB_SIZE = 0x83200000, 0x800000
FB_W, FB_H, FB_STRIDE = 1080, 1920, 1080 * 3

# QEMU virt MMIO (Cortex-A53 + GICv2)
QEMU_SOC_BASE = 0x08000000
QEMU_GICD = 0x08000000
QEMU_GICC = 0x08010000
QEMU_TIMEBASE = 62500000
QEMU_VTIMER_INTID = 27

SDHC1, SDHC1_SIZE = 0x07824900, 0x500  # eMMC
SDHC2, SDHC2_SIZE = 0x07864900, 0x500  # MicroSD
BLSP_I2C3, BLSP_I2C3_SIZE = 0x078b7000, 0x600  # Goodix touch QUP I2C
TLMM_GPIO, TLMM_GPIO_SIZE = 0x01000000, 0x300000

TIMEBASE = 19200000
VTIMER_INTID = 20  # Qualcomm wiring: GIC_PPI 4 -> INTID 20 (not the ARM-standard 27)

SHAMROCK_ECID = 0x5348414d524f434b  # 'SHAMROCK' in ASCII

BOOT_MANIFEST_HASH = hashlib.sha384(b'shamrock boot manifest').digest()

D22AP_ADT = os.path.join(TOOLS_DIR, '../research/ipsw-x/d22ap.adt')

def d22ap_product(path=D22AP_ADT):
    """Read /product properties from the real iPhone X device tree."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'

def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n

def build(seed=None, qemu=False):
    phys_base = PHYS_BASE
    mem_size  = MEM_SIZE

    soc_base = QEMU_SOC_BASE if qemu else SOC_BASE
    gicd     = QEMU_GICD if qemu else GICD
    gicc     = QEMU_GICC if qemu else GICC
    timebase = QEMU_TIMEBASE if qemu else TIMEBASE
    vtimer_intid = QEMU_VTIMER_INTID if qemu else VTIMER_INTID

    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE

    seed = seed if seed is not None else os.urandom(64)
    gic_ph_num = 10
    phandle = iter(range(11, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('shamrock-shim')),
        ('system-firmware-version', cstr('shamrock-shim')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        ('display-scale', u32(3)),
        ('unique-chip-id', struct.pack('<Q', SHAMROCK_ECID)),
        ('chip-id', u32(0x8015)),  # Apple A11 Bionic
        # Hactivation flags (bypasses activation lock / server requirement)
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
        # NVRAM properties for IODTNVRAM / IONVRAMCHRPHandler
        ('nvram-bank-size', u32(0x2000)),
        ('nvram-bank-count', u32(1)),
        ('nvram-current-bank', u32(0)),
        ('nvram-proxy-data', b'\0' * 0x2000),
    ], [
        node('memory-map', [ph(), ('Display-FB', struct.pack('<2Q', FB, FB_SIZE))]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])
    ])

    # 8x Cortex-A53 CPUs (MSM8952):
    # Cluster 0 (cpu0-cpu3): reg 0x000, 0x001, 0x002, 0x003 (1.2 GHz)
    # Cluster 1 (cpu4-cpu7): reg 0x100, 0x101, 0x102, 0x103 (1.5 GHz)
    cpu_nodes = []
    for core in range(8):
        cluster = 0 if core < 4 else 1
        core_in_cluster = core if cluster == 0 else (core - 4)
        mpidr_reg = (cluster << 8) | core_in_cluster
        freq = 1500000000 if cluster == 1 else 1200000000
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('ARM,cortex-a53') + cstr('ARM,v8')),
            ('reg', u32(mpidr_reg)),
            ('cpu-id', u32(core)),
            ('cluster-id', u32(cluster)),
            ('cluster-type', u32(0)),  # SMP
            ('die-cluster-id', u32(cluster)),
            ('cluster-core-id', u32(core_in_cluster)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(freq)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(933000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
            ('interrupt-parent', u32(gic_ph_num)),
        ]))
    cpus = node('cpus', [
        ph(),
        ('#address-cells', u32(1)),
        ('#size-cells', u32(0)),
        ('#interrupt-cells', u32(1)),
        ('interrupt-parent', u32(gic_ph_num)),
    ], cpu_nodes)

    # GICv2 Interrupt Controller (MSM8952)
    gic = node('gic', [
        ('AAPL,phandle', u32(gic_ph_num)),
        ('compatible', cstr('arm,cortex-a15-gic') + cstr('arm,gic-400') + cstr('arm,gic-v2')),
        ('interrupt-controller', cstr('master')),
        ('#interrupt-cells', u32(1)),
        ('virtual-timer-intid', u32(vtimer_intid)),
        ('reg', u64(gicd - soc_base, GICD_SIZE, gicc - soc_base, GICC_SIZE)),
    ])

    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('shamrock-io')),
        ('compatible', cstr('shamrock-io') + cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('#interrupt-cells', u32(1)),
        ('interrupt-parent', u32(gic_ph_num)),
        ('ranges', u64(0, soc_base, 0x100000000 - soc_base)),
    ], [
        gic,
        node('wdt', [ph(), ('no-pmu', u32(1)), ('interrupt-parent', u32(gic_ph_num))]),
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('display,shamrock') + cstr('iomfb') + cstr('AppleCLCD')),
            ('canvas-width', u32(FB_W)),
            ('canvas-height', u32(FB_H)),
            ('reg', u64(FB - soc_base, FB_SIZE)),
            ('iomfb-source-limits', u32(4096, 4096, 1, 1, 4, 1, 4)),
        ]),
        node('sdhc1', [
            ph(),
            ('compatible', cstr('qcom,sdhci-msm')),
            ('reg', u64(SDHC1 - SOC_BASE, SDHC1_SIZE)),
            ('interrupt-parent', u32(gic_ph_num)),
            ('interrupts', u32(123)),
        ]),
        node('sdhc2', [
            ph(),
            ('compatible', cstr('qcom,sdhci-msm')),
            ('reg', u64(SDHC2 - SOC_BASE, SDHC2_SIZE)),
            ('interrupt-parent', u32(gic_ph_num)),
            ('interrupts', u32(125)),
        ]),
        node('i2c3', [
            ph(),
            ('compatible', cstr('qcom,i2c-msm-v2')),
            ('reg', u64(BLSP_I2C3 - SOC_BASE, BLSP_I2C3_SIZE)),
            ('interrupt-parent', u32(gic_ph_num)),
            ('interrupts', u32(97)),
        ])
    ] + ([
        node('uart0', [
            ph(),
            ('compatible', cstr('arm,pl011')),
            ('reg', u64(0x09000000 - soc_base, 0x1000)),
            ('interrupt-parent', u32(gic_ph_num)),
            ('interrupts', u32(33)),
        ])
    ] if qemu else []))

    fbcon = node('shamrock-fbcon', [
        ph(),
        ('compatible', cstr('sweet-fbcon')),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
        ('depth', u32(24)),
    ])

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),  # Face ID style (no home button)
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])
    options = node('options', [ph()])

    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('generalmobile,shamrock') + cstr('qcom,msm8952') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('#interrupt-cells', u32(1)),
        ('interrupt-parent', u32(gic_ph_num)),
    ], [chosen, options, defaults, cpus, arm_io, fbcon, buttons, pram] + product)

if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    out_path = args[0] if args else 'out/shamrock.adt'
    qemu_mode = '--qemu' in sys.argv
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    root = build(qemu=qemu_mode)
    blob = adt.serialize(root)
    with open(out_path, 'wb') as f:
        f.write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'Successfully generated {out_path} (qemu={qemu_mode}): {len(blob)} bytes')
