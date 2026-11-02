#include "ci_usb_xnu.h"
#include "ci_hw.h"
#include <mach/mach_types.h>
#include <machine/atomic.h>
#include <arm/machine_routines.h>
#include <arm/pmap/pmap.h>
#include <kern/startup.h>

#define LOG_RING_SIZE   65536
#define RX_RING_SIZE    2048
#define LOADER_LOG_SIZE (1536 * 1024)

#define CI_DMA_PHYS_BASE        0x831F0000UL
#define CI_DMA_MAP_SIZE         0x00010000UL
#define PERSIST_LOG_OFFSET      0x00004000UL
#define PERSIST_LOG_MAGIC       0x53484D52U   /* 'SHMR' */

#ifndef NULL
#define NULL ((void *)0)
#endif

/*
 * Endpoint Queue Head (dQH) - 64 bytes
 */
struct __attribute__((packed, aligned(64))) ci_dqh {
	uint32_t cap;
	uint32_t curr_dtd;
	uint32_t next_dtd;
	uint32_t token;
	uint32_t page[5];
	uint32_t reserved;
	uint8_t  setup_buffer[8];
	uint32_t reserved2[4];
};
_Static_assert(sizeof(struct ci_dqh) == 64, "ci_dqh size must be 64 bytes");

/*
 * Device Transfer Descriptor (dTD) - 32 bytes
 */
struct __attribute__((packed, aligned(64))) ci_dtd {
	uint32_t next_dtd;
	uint32_t token;
	uint32_t page[5];
	uint32_t reserved;
	uint8_t  pad[32];
};
_Static_assert(sizeof(struct ci_dtd) == 64, "ci_dtd size aligned to 64 bytes");

struct __attribute__((packed)) usb_setup_req {
	uint8_t  bmRequestType;
	uint8_t  bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
};
_Static_assert(sizeof(struct usb_setup_req) == 8, "usb_setup_req must be 8 bytes");

enum ci_op_reg {
	OP_USBMODE,
	OP_ENDPTSETUPSTAT,
	OP_ENDPTPRIME,
	OP_ENDPTFLUSH,
	OP_ENDPTSTAT,
	OP_ENDPTCOMPLETE,
};

/*
 * Dedicated 32-bit DMA Arena at fixed physical 0x831F0000.
 * Mapped uncached via ml_io_map: NO cache flushing needed, NO >4GB address overflow!
 */
struct ci_dma_arena {
	struct ci_dqh dqh[16] __attribute__((aligned(2048))); /* 0x0000 - 0x03FF */
	struct ci_dtd dtd_ep0_out;                            /* 0x0400 - 0x043F */
	struct ci_dtd dtd_ep0_in;                             /* 0x0440 - 0x047F */
	struct ci_dtd dtd_ep1_out;                            /* 0x0480 - 0x04BF */
	struct ci_dtd dtd_ep1_in;                             /* 0x04C0 - 0x04FF */
	struct ci_dtd dtd_ep2_in;                             /* 0x0500 - 0x053F */
	uint8_t  ep0_rx_buf[128]  __attribute__((aligned(64)));/* 0x0540 */
	uint8_t  ep0_tx_buf[512]  __attribute__((aligned(64)));/* 0x05C0 */
	uint8_t  ep1_rx_buf[512]  __attribute__((aligned(64)));/* 0x07C0 */
	uint8_t  ep1_tx_buf[512]  __attribute__((aligned(64)));/* 0x09C0 */
	uint8_t  ep2_tx_buf[64]   __attribute__((aligned(64)));/* 0x0BC0 */
};

static volatile struct ci_dma_arena *g_dma = NULL;
static volatile char *g_persist_log = NULL;
static uint32_t g_persist_idx = 0;

/* Software Ring Buffers */
static uint8_t  log_ring[LOG_RING_SIZE];
static volatile uint32_t log_head = 0, log_tail = 0, log_dropped = 0;

static uint8_t  rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head = 0, rx_tail = 0;

/* fix89b: the [HB] line shows this counter.  A log that goes quiet with dropped=0 died host-side
 * (or in the pump); quiet with dropped>0 means the ring filled because the pump stopped draining.
 * Until now the counter was written at both ring-full sites and printed nowhere. */
uint32_t
ci_usb_log_dropped(void)
{
	return log_dropped;
}

/* Controller State */
static uintptr_t base = 0;
static uintptr_t cap_base = 0;
static uintptr_t phy_base = 0;
static uint32_t  lpm_active = 0;
static uint32_t  bulk_max_pkt = 64;
static uint8_t   curr_config = 0;
static volatile bool configured = false;
static volatile bool ep1_rx_busy = false;
static volatile bool ep1_tx_busy = false;
static volatile uint32_t ep1_tx_inflight = 0;
static uint64_t ep1_prime_tick = 0;

static volatile bool     g_ci_usb_inited = false;
static volatile uint32_t ci_poll_active = 0;
static volatile uint32_t ci_tx_lock = 0;

/* CDC Line Coding state (115200 8N1) */
static uint8_t line_coding[7] = {
	0x00, 0xC2, 0x01, 0x00, /* 115200 baud */
	0x00,                   /* 1 stop bit */
	0x00,                   /* no parity */
	0x08                    /* 8 data bits */
};

/* Timer reading */
static inline uint64_t get_ticks(void)
{
	__builtin_arm_isb(ISB_SY);
	return __builtin_arm_rsr64("CNTVCT_EL0");
}

/* Guaranteed 32-bit physical address inside DMA arena */
static inline uint32_t ci_dma_pa(const volatile void *va)
{
	uintptr_t off = (uintptr_t)va - (uintptr_t)g_dma;
	return (uint32_t)(CI_DMA_PHYS_BASE + off);
}

/* Persistent RAM Log Ring (survives panics and resets) */
static inline void persist_log_putc(char c)
{
	if (!g_persist_log) return;
	if (g_persist_idx < (CI_DMA_MAP_SIZE - PERSIST_LOG_OFFSET - 16)) {
		g_persist_log[16 + g_persist_idx++] = c;
		*(volatile uint32_t *)&g_persist_log[0] = PERSIST_LOG_MAGIC;
		*(volatile uint32_t *)&g_persist_log[4] = g_persist_idx;
	}
}

/* Register access helpers */
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uintptr_t op_reg(enum ci_op_reg reg_name)
{
	switch (reg_name) {
	case OP_USBMODE:
		return cap_base + (lpm_active ? CI_LPM_USBMODE : CI_STD_USBMODE);
	case OP_ENDPTSETUPSTAT:
		return cap_base + (lpm_active ? CI_LPM_ENDPTSETUPSTAT : CI_STD_ENDPTSETUPSTAT);
	case OP_ENDPTPRIME:
		return cap_base + (lpm_active ? CI_LPM_ENDPTPRIME : CI_STD_ENDPTPRIME);
	case OP_ENDPTFLUSH:
		return cap_base + (lpm_active ? CI_LPM_ENDPTFLUSH : CI_STD_ENDPTFLUSH);
	case OP_ENDPTSTAT:
		return cap_base + (lpm_active ? CI_LPM_ENDPTSTAT : CI_STD_ENDPTSTAT);
	case OP_ENDPTCOMPLETE:
		return cap_base + (lpm_active ? CI_LPM_ENDPTCOMPLETE : CI_STD_ENDPTCOMPLETE);
	default:
		return 0;
	}
}

static uintptr_t ctrl_reg(int ep)
{
	return cap_base + (lpm_active ? CI_LPM_ENDPTCTRL(ep) : CI_STD_ENDPTCTRL(ep));
}

/* ULPI Viewport Helpers */
static int ulpi_read(uint8_t addr, uint8_t *val)
{
	uintptr_t vp = cap_base + CI_ULPI_VIEWPORT;
	wr32(vp, ULPI_WRITE | ULPI_WAKEUP);
	uint64_t start = get_ticks();
	while (rd32(vp) & ULPI_WAKEUP) {
		if (get_ticks() - start > 192000)
			break;
	}
	wr32(vp, ULPI_RUN | ULPI_ADDR(addr));
	start = get_ticks();
	while (rd32(vp) & ULPI_RUN) {
		if (get_ticks() - start > 192000)
			return -1;
	}
	*val = (uint8_t)ULPI_DATA_RD(rd32(vp));
	return 0;
}

static int ulpi_write(uint8_t addr, uint8_t val)
{
	uintptr_t vp = cap_base + CI_ULPI_VIEWPORT;
	wr32(vp, ULPI_WRITE | ULPI_WAKEUP);
	uint64_t start = get_ticks();
	while (rd32(vp) & ULPI_WAKEUP) {
		if (get_ticks() - start > 192000)
			break;
	}
	wr32(vp, ULPI_RUN | ULPI_WRITE | ULPI_ADDR(addr) | ULPI_DATA_WR(val));
	start = get_ticks();
	while (rd32(vp) & ULPI_RUN) {
		if (get_ticks() - start > 192000)
			return -1;
	}
	return 0;
}

/* Standard USB Descriptors */
static const uint8_t dev_desc[] = {
	18,         /* bLength */
	0x01,       /* bDescriptorType = Device */
	0x00, 0x02, /* bcdUSB = 2.00 */
	0x02,       /* bDeviceClass = CDC */
	0x00,       /* bDeviceSubClass */
	0x00,       /* bDeviceProtocol */
	64,         /* bMaxPacketSize0 = 64 */
	0x09, 0x12, /* idVendor = 0x1209 */
	0x01, 0x00, /* idProduct = 0x0001 */
	0x00, 0x01, /* bcdDevice = 1.00 */
	1,          /* iManufacturer */
	2,          /* iProduct */
	3,          /* iSerialNumber */
	1           /* bNumConfigurations = 1 */
};

static const uint8_t cfg_desc[] = {
	/* Configuration Descriptor: 67 bytes, Bus-Powered (0x80), bMaxPower = 250 (500 mA) */
	9, 0x02, 67, 0, 2, 1, 0, 0x80, 0xFA,
	/* Interface 0: CDC Communication */
	9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
	/* CDC Header Functional Descriptor */
	5, 0x24, 0x00, 0x10, 0x01,
	/* CDC Call Management Functional Descriptor */
	5, 0x24, 0x01, 0x00, 0x01,
	/* CDC ACM Functional Descriptor */
	4, 0x24, 0x02, 0x02,
	/* CDC Union Functional Descriptor */
	5, 0x24, 0x06, 0x00, 0x01,
	/* EP2 IN (Interrupt) */
	7, 0x05, 0x82, 0x03, 10, 0, 9,
	/* Interface 1: CDC Data */
	9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
	/* EP1 OUT (Bulk) */
	7, 0x05, 0x01, 0x02, 0x00, 0x02, 0,
	/* EP1 IN (Bulk) */
	7, 0x05, 0x81, 0x02, 0x00, 0x02, 0
};

static const uint8_t str0_desc[] = {
	4, 0x03, 0x09, 0x04
};

static const uint8_t str1_desc[] = {
	30, 0x03,
	'G', 0, 'e', 0, 'n', 0, 'e', 0, 'r', 0, 'a', 0, 'l', 0, ' ', 0,
	'M', 0, 'o', 0, 'b', 0, 'i', 0, 'l', 0, 'e', 0
};

static const uint8_t str2_desc[] = {
	48, 0x03,
	'H', 0, 'a', 0, 'c', 0, 'k', 0, 'i', 0, 'O', 0, 'S', 0, ' ', 0,
	'X', 0, 'N', 0, 'U', 0, ' ', 0, 'C', 0, 'o', 0, 'n', 0, 's', 0,
	'o', 0, 'l', 0, 'e', 0
};

static const uint8_t str3_desc[] = {
	24, 0x03,
	'S', 0, 'H', 0, 'A', 0, 'M', 0, 'R', 0, 'O', 0, 'C', 0, 'K', 0,
	'0', 0, '0', 0, '1', 0
};

static const uint8_t dev_qual[] = {
	10, 0x06, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 1, 0
};

/* dTD and Queue Head Helpers */
static void build_dtd(volatile struct ci_dtd *d, const volatile void *buf, uint32_t len, int ioc)
{
	uint32_t pa = ci_dma_pa(buf);
	d->next_dtd = TD_TERMINATE;
	d->token = (len << 16) | (ioc ? TD_IOC : 0) | TD_STATUS_ACTIVE;
	d->page[0] = pa;
	d->page[1] = (pa + 0x1000) & ~0xFFFU;
	d->page[2] = 0;
	d->page[3] = 0;
	d->page[4] = 0;
}

static void prime_ep(int ep, int dir, volatile struct ci_dtd *d)
{
	int idx = ep * 2 + dir;
	g_dma->dqh[idx].next_dtd = ci_dma_pa(d);
	g_dma->dqh[idx].token &= ~(TD_STATUS_ACTIVE | TD_STATUS_HALTED);

	__builtin_arm_dsb(DSB_SY);

	uint32_t mask = 1U << (ep + (dir ? 16 : 0));
	wr32(op_reg(OP_ENDPTPRIME), mask);
}

/* Control Transfers */
static void ep0_send_zlp(void)
{
	build_dtd(&g_dma->dtd_ep0_in, g_dma->ep0_tx_buf, 0, 1);
	prime_ep(0, 1, &g_dma->dtd_ep0_in);
}

static void ep0_recv_zlp(void)
{
	build_dtd(&g_dma->dtd_ep0_out, g_dma->ep0_rx_buf, 0, 1);
	prime_ep(0, 0, &g_dma->dtd_ep0_out);
}

static void ep0_send_data(const void *data, uint32_t len)
{
	if (len > sizeof(g_dma->ep0_tx_buf)) len = sizeof(g_dma->ep0_tx_buf);
	for (uint32_t i = 0; i < len; i++)
		g_dma->ep0_tx_buf[i] = ((const uint8_t *)data)[i];
	build_dtd(&g_dma->dtd_ep0_in, g_dma->ep0_tx_buf, len, 1);
	prime_ep(0, 1, &g_dma->dtd_ep0_in);
	ep0_recv_zlp();
}

static void ep2_send_serial_state(uint16_t state)
{
	g_dma->ep2_tx_buf[0] = 0xA1; /* bmRequestType */
	g_dma->ep2_tx_buf[1] = 0x20; /* SERIAL_STATE */
	g_dma->ep2_tx_buf[2] = 0x00; g_dma->ep2_tx_buf[3] = 0x00;
	g_dma->ep2_tx_buf[4] = 0x00; g_dma->ep2_tx_buf[5] = 0x00;
	g_dma->ep2_tx_buf[6] = 0x02; g_dma->ep2_tx_buf[7] = 0x00;
	g_dma->ep2_tx_buf[8] = (uint8_t)(state & 0xFF);
	g_dma->ep2_tx_buf[9] = (uint8_t)((state >> 8) & 0xFF);
	build_dtd(&g_dma->dtd_ep2_in, g_dma->ep2_tx_buf, 10, 1);
	prime_ep(2, 1, &g_dma->dtd_ep2_in);
}

static void ci_rx_prime(void)
{
	if (ep1_rx_busy || !configured || !g_dma)
		return;
	build_dtd(&g_dma->dtd_ep1_out, g_dma->ep1_rx_buf, bulk_max_pkt, 1);
	ep1_rx_busy = true;
	prime_ep(1, 0, &g_dma->dtd_ep1_out);
}

static void ci_usb_poll_internal(void);

static void ci_usb_xnu_putc_ring_only(char c)
{
	uint64_t daif = __builtin_arm_rsr64("DAIF");
	__builtin_arm_wsr("DAIFSet", DAIFSC_IRQF | DAIFSC_FIQF);

	for (uint32_t tries = 0; tries < 100000; tries++) {
		if (os_atomic_cmpxchg(&ci_tx_lock, 0, 1, acquire))
			break;
	}

	uint32_t next = (log_head + 1) % LOG_RING_SIZE;
	if (next != log_tail) {
		log_ring[log_head] = (uint8_t)c;
		log_head = next;
	} else {
		log_dropped++;
	}

	os_atomic_store(&ci_tx_lock, 0, release);
	__builtin_arm_wsr64("DAIF", daif);
}

static void ci_log_pump(void)
{
	if (!configured || !g_dma)
		return;

	/* Check if previous EP1 IN completed */
	if (ep1_tx_busy) {
		uint32_t comp = rd32(op_reg(OP_ENDPTCOMPLETE));
		if (comp & (1U << 17)) {
			wr32(op_reg(OP_ENDPTCOMPLETE), (1U << 17));
			log_tail = (log_tail + ep1_tx_inflight) % LOG_RING_SIZE;
			ep1_tx_inflight = 0;
			ep1_tx_busy = false;
		} else if (!(rd32(op_reg(OP_ENDPTSTAT)) & (1U << 17)) &&
		           !(rd32(op_reg(OP_ENDPTPRIME)) & (1U << 17)) &&
		           !(g_dma->dtd_ep1_in.token & TD_STATUS_ACTIVE)) {
			log_tail = (log_tail + ep1_tx_inflight) % LOG_RING_SIZE;
			ep1_tx_inflight = 0;
			ep1_tx_busy = false;
		} else if (get_ticks() - ep1_prime_tick > 192000) { /* 10ms timeout @ 19.2MHz */
			wr32(op_reg(OP_ENDPTFLUSH), (1U << 17));
			while (rd32(op_reg(OP_ENDPTFLUSH)) & (1U << 17)) { }
			g_dma->dtd_ep1_in.token = 0;
			log_tail = (log_tail + ep1_tx_inflight) % LOG_RING_SIZE;
			ep1_tx_inflight = 0;
			ep1_tx_busy = false;
		}
	}

	if (ep1_tx_busy)
		return;

	uint32_t head = log_head;
	uint32_t tail = log_tail;
	if (head == tail)
		return;

	uint32_t avail = (head >= tail) ? (head - tail) : (LOG_RING_SIZE - tail);
	if (avail > bulk_max_pkt)
		avail = bulk_max_pkt;

	for (uint32_t i = 0; i < avail; i++) {
		g_dma->ep1_tx_buf[i] = log_ring[(tail + i) % LOG_RING_SIZE];
	}

	build_dtd(&g_dma->dtd_ep1_in, g_dma->ep1_tx_buf, avail, 1);
	ep1_tx_inflight = avail;
	ep1_tx_busy = true;
	ep1_prime_tick = get_ticks();
	prime_ep(1, 1, &g_dma->dtd_ep1_in);

	/*
	 * fix74: packet-boundary watchdog food and tx beacon.  This is the one choke point every
	 * byte the kernel has actually handed the host passes through -- the loader-log mirror,
	 * the printf lines, all of it rides this ring (fix73's beacon sat in putc_raw, which the
	 * console never walks: zero beacons in a 188 KB log).  Feeding here feeds the path that
	 * runs during the boot storm, when the thread_call and idle contexts are both starved;
	 * one beacon per 4 KB sent brackets any death by the kernel's own clock.
	 */
	{
		extern void hv_wdt_pet_now(void);
		extern int hakios_bringup_log_ready;
		static uint64_t tx_sent, tx_ord;

		tx_sent += avail;
		hv_wdt_pet_now();

		if (hakios_bringup_log_ready && (tx_sent >> 12) != tx_ord) {
			static const char nib[] = "0123456789abcdef";
			char line[40];
			char *p = line;
			uint64_t v;
			int i;

			tx_ord = tx_sent >> 12;
			*p++ = '\r'; *p++ = '\n';
			*p++ = 't'; *p++ = 'x'; *p++ = ' ';
			for (i = 60; i >= 0; i -= 4) {
				*p++ = nib[(mach_absolute_time() >> i) & 0xF];
			}
			*p++ = ' ';
			v = tx_ord;
			{
				char tmp[16];
				char *q = tmp;
				do { *q++ = nib[v & 0xF]; v >>= 4; } while (v);
				while (q != tmp) { *p++ = *--q; }
			}
			*p++ = '\n';
			for (i = 0; i < p - line; i++) {
				ci_usb_xnu_putc_ring_only(line[i]);
			}
		}
	}

	/* Synchronous drain: spin-wait up to 1.5ms (28800 ticks) so during DAIF.I=1 boot,
	 * data actually gets sent to host over USB! */
	uint64_t t0 = get_ticks();
	while (ep1_tx_busy && (get_ticks() - t0 < 28800)) {
		ci_usb_poll_internal();
	}
}

/* EP0 Setup Request Handler */
static void handle_setup(const struct usb_setup_req *req)
{
	uint8_t type = req->bmRequestType & 0x60;

	if (type == 0x00) {
		/* Standard Request */
		switch (req->bRequest) {
		case 0x00: /* GET_STATUS */
		{
			uint16_t status = 0;
			ep0_send_data(&status, sizeof(status));
			break;
		}
		case 0x05: /* SET_ADDRESS */
		{
			uint8_t new_addr = (uint8_t)(req->wValue & 0x7F);
			ep0_send_zlp();
			wr32(cap_base + CI_DEVICEADDR, DEVICEADDR_USBADRA | DEVICEADDR_USBADR(new_addr));
			break;
		}
		case 0x06: /* GET_DESCRIPTOR */
		{
			uint8_t desc_type = req->wValue >> 8;
			uint8_t desc_idx  = req->wValue & 0xFF;
			const uint8_t *p = NULL;
			uint32_t len = 0;

			if (desc_type == 1) {
				p = dev_desc;
				len = sizeof(dev_desc);
			} else if (desc_type == 2) {
				p = cfg_desc;
				len = sizeof(cfg_desc);
			} else if (desc_type == 6) {
				p = dev_qual;
				len = sizeof(dev_qual);
			} else if (desc_type == 3) {
				if (desc_idx == 0) { p = str0_desc; len = sizeof(str0_desc); }
				else if (desc_idx == 1) { p = str1_desc; len = sizeof(str1_desc); }
				else if (desc_idx == 2) { p = str2_desc; len = sizeof(str2_desc); }
				else if (desc_idx == 3) { p = str3_desc; len = sizeof(str3_desc); }
			}

			if (p && len) {
				if (len > req->wLength) len = req->wLength;
				ep0_send_data(p, len);
			} else {
				/* Stall EP0 */
				wr32(ctrl_reg(0), rd32(ctrl_reg(0)) | ENDPTCTRL_RXS | ENDPTCTRL_TXS);
			}
			break;
		}
		case 0x08: /* GET_CONFIGURATION */
		{
			ep0_send_data(&curr_config, 1);
			break;
		}
		case 0x09: /* SET_CONFIGURATION */
		{
			curr_config = (uint8_t)(req->wValue & 0xFF);
			ep0_send_zlp();
			if (curr_config != 0) {
				uint32_t portsc = rd32(cap_base + CI_PORTSC);
				bulk_max_pkt = (portsc & PORTSC_HSP) ? 512 : 64;

				/* Flush EP1 IN & OUT */
				wr32(op_reg(OP_ENDPTFLUSH), (1U << 17) | (1U << 1));
				while (rd32(op_reg(OP_ENDPTFLUSH)) & ((1U << 17) | (1U << 1))) { }
				wr32(op_reg(OP_ENDPTCOMPLETE), (1U << 17) | (1U << 1));

				g_dma->dqh[2].cap = QH_MAX_PKT(bulk_max_pkt) | QH_ZLT;
				g_dma->dqh[2].next_dtd = TD_TERMINATE;
				g_dma->dqh[3].cap = QH_MAX_PKT(bulk_max_pkt) | QH_ZLT;
				g_dma->dqh[3].next_dtd = TD_TERMINATE;
				g_dma->dqh[5].cap = QH_MAX_PKT(10) | QH_ZLT;
				g_dma->dqh[5].next_dtd = TD_TERMINATE;

				/* Reset data toggles and configure EP1 and EP2 */
				wr32(ctrl_reg(1), ENDPTCTRL_RXE | ENDPTCTRL_RXR | ENDPTCTRL_RXT_BULK |
				                  ENDPTCTRL_TXE | ENDPTCTRL_TXR | ENDPTCTRL_TXT_BULK);
				wr32(ctrl_reg(2), ENDPTCTRL_TXE | ENDPTCTRL_TXR | ENDPTCTRL_TXT_INT);

				ep1_tx_busy = false;
				configured = true;
				ci_rx_prime();
				ci_log_pump();
			}
			break;
		}
		default:
			ep0_send_zlp();
			break;
		}
	} else if (type == 0x20) {
		/* CDC Class Request */
		switch (req->bRequest) {
		case 0x20: /* SET_LINE_CODING */
		{
			build_dtd(&g_dma->dtd_ep0_out, g_dma->ep0_rx_buf, 7, 1);
			prime_ep(0, 0, &g_dma->dtd_ep0_out);
			uint64_t t0 = get_ticks();
			while ((rd32(op_reg(OP_ENDPTPRIME)) & 1) && (get_ticks() - t0 < 38400)) { }
			ep0_send_zlp();
			break;
		}
		case 0x21: /* GET_LINE_CODING */
		{
			ep0_send_data(line_coding, sizeof(line_coding));
			break;
		}
		case 0x22: /* SET_CONTROL_LINE_STATE */
		{
			ep0_send_zlp();
			if (req->wValue & 1) {
				ep2_send_serial_state(0x03); /* DCD | DSR active */
				/* Host terminal just opened / asserted DTR: replay persistent boot log! */
				if (g_persist_log && (*(volatile uint32_t *)&g_persist_log[0] == PERSIST_LOG_MAGIC)) {
					uint32_t total = *(volatile uint32_t *)&g_persist_log[4];
					const volatile char *text = (const volatile char *)&g_persist_log[16];
					for (uint32_t i = 0; i < total; i++) {
						ci_usb_xnu_putc_raw(text[i]);
					}
				}
			}
			/* Clear any stale busy state when terminal attaches */
			if (!(rd32(op_reg(OP_ENDPTSTAT)) & (1U << 17)) &&
			    !(rd32(op_reg(OP_ENDPTPRIME)) & (1U << 17))) {
				ep1_tx_busy = false;
			}
			ci_log_pump();
			break;
		}
		default:
			ep0_send_zlp();
			break;
		}
	} else {
		ep0_send_zlp();
	}
}

extern void hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
extern void loader_return(void);
static char touch_cmd_buf[48];
static uint32_t touch_cmd_len = 0;

static void parse_touch_cmd(const char *cmd)
{
	while (*cmd == ' ') cmd++;
	if ((cmd[0] == 'R' || cmd[0] == 'r') &&
	    (cmd[1] == 'E' || cmd[1] == 'e') &&
	    (cmd[2] == 'B' || cmd[2] == 'b') &&
	    (cmd[3] == 'O' || cmd[3] == 'o') &&
	    (cmd[4] == 'O' || cmd[4] == 'o') &&
	    (cmd[5] == 'T' || cmd[5] == 't')) {
		loader_return();
		return;
	}
	if (cmd[0] != 'T' && cmd[0] != 't') return;
	const char *p = cmd + 1;
	while (*p == ' ') p++;
	uint32_t type = 0, finger = 0, x = 0, y = 0;
	while (*p >= '0' && *p <= '9') type = type * 10 + (*p++ - '0');
	while (*p == ' ') p++;
	while (*p >= '0' && *p <= '9') finger = finger * 10 + (*p++ - '0');
	while (*p == ' ') p++;
	while (*p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0');
	while (*p == ' ') p++;
	while (*p >= '0' && *p <= '9') y = y * 10 + (*p++ - '0');

	if (type > 0) {
		hakios_touch_post(type, finger, x, y);
	}
}

/* Polling Loop */
static void ci_usb_poll_internal(void)
{
	if (!cap_base || !g_dma) return;

	uint32_t sts = rd32(cap_base + CI_USBSTS);
	if (sts)
		wr32(cap_base + CI_USBSTS, sts);

	/* Bus Reset */
	if (sts & USBSTS_URI) {
		wr32(cap_base + CI_DEVICEADDR, 0);
		wr32(op_reg(OP_ENDPTFLUSH), 0xFFFFFFFF);
		wr32(op_reg(OP_ENDPTCOMPLETE), 0xFFFFFFFF);
		configured = false;
		ep1_rx_busy = false;
		ep1_tx_busy = false;
		curr_config = 0;

		g_dma->dqh[0].cap = QH_MAX_PKT(64) | QH_ZLT | QH_IOS;
		g_dma->dqh[0].next_dtd = TD_TERMINATE;
		g_dma->dqh[1].cap = QH_MAX_PKT(64) | QH_ZLT;
		g_dma->dqh[1].next_dtd = TD_TERMINATE;
	}

	/* Port Change */
	if (sts & USBSTS_PCI) {
		uint32_t portsc = rd32(cap_base + CI_PORTSC);
		bulk_max_pkt = (portsc & PORTSC_HSP) ? 512 : 64;
	}

	/* Setup Packet on EP0 */
	uint32_t setupstat = rd32(op_reg(OP_ENDPTSETUPSTAT));
	if (setupstat & 1) {
		wr32(op_reg(OP_ENDPTSETUPSTAT), 1);

		struct usb_setup_req req;
		do {
			wr32(cap_base + CI_USBCMD, rd32(cap_base + CI_USBCMD) | USBCMD_SUTW);
			for (int i = 0; i < 8; i++)
				((uint8_t *)&req)[i] = g_dma->dqh[0].setup_buffer[i];
		} while (!(rd32(cap_base + CI_USBCMD) & USBCMD_SUTW));

		wr32(cap_base + CI_USBCMD, rd32(cap_base + CI_USBCMD) & ~USBCMD_SUTW);

		wr32(op_reg(OP_ENDPTFLUSH), (1U << 0) | (1U << 16));
		while (rd32(op_reg(OP_ENDPTFLUSH)) & ((1U << 0) | (1U << 16)))
			;
		wr32(op_reg(OP_ENDPTCOMPLETE), (1U << 0) | (1U << 16));

		handle_setup(&req);
	}

	/* Transfer Complete */
	uint32_t complete = rd32(op_reg(OP_ENDPTCOMPLETE));
	if (complete) {
		wr32(op_reg(OP_ENDPTCOMPLETE), complete);

		/* EP1 OUT (Bulk RX) complete */
		if (complete & (1U << 1)) {
			ep1_rx_busy = false;
			uint32_t remaining = (g_dma->dtd_ep1_out.token >> 16) & 0x7FFF;
			uint32_t received = bulk_max_pkt > remaining ? (bulk_max_pkt - remaining) : 0;

			for (uint32_t i = 0; i < received; i++) {
				uint8_t c = g_dma->ep1_rx_buf[i];
				if (c == '\r' || c == '\n') {
					if (touch_cmd_len > 0) {
						touch_cmd_buf[touch_cmd_len] = '\0';
						parse_touch_cmd(touch_cmd_buf);
						touch_cmd_len = 0;
					}
				} else if (touch_cmd_len < sizeof(touch_cmd_buf) - 1) {
					touch_cmd_buf[touch_cmd_len++] = (char)c;
				}

				if (c == '?') {
					if (g_persist_log && (*(volatile uint32_t *)&g_persist_log[0] == PERSIST_LOG_MAGIC)) {
						uint32_t total = *(volatile uint32_t *)&g_persist_log[4];
						const volatile char *text = (const volatile char *)&g_persist_log[16];
						for (uint32_t k = 0; k < total; k++) {
							ci_usb_xnu_putc_ring_only(text[k]);
						}
					}
				}
				uint32_t next = (rx_head + 1) % RX_RING_SIZE;
				if (next != rx_tail) {
					rx_ring[rx_head] = c;
					rx_head = next;
				}
			}
			ci_rx_prime();
		}

		/* EP1 IN (Bulk TX / Logging) complete */
		if (complete & (1U << 17)) {
			log_tail = (log_tail + ep1_tx_inflight) % LOG_RING_SIZE;
			ep1_tx_inflight = 0;
			ep1_tx_busy = false;
			ci_log_pump();
		}

		/* EP2 IN (Interrupt) complete */
		if (complete & (1U << 18)) {
			/* Serial state packet ACKed */
		}
	}

	ci_log_pump();
	ci_rx_prime();
}

void ci_usb_xnu_poll(void)
{
	if (!g_ci_usb_inited)
		return;

	/* Non-blocking try-lock to prevent reentrant polling */
	if (os_atomic_cmpxchg(&ci_poll_active, 0, 1, acquire)) {
		ci_usb_poll_internal();
		os_atomic_store(&ci_poll_active, 0, release);
	}
}

/* Logging API */
void ci_usb_xnu_putc_raw(char c)
{
	persist_log_putc(c);

	uint64_t daif = __builtin_arm_rsr64("DAIF");
	__builtin_arm_wsr("DAIFSet", DAIFSC_IRQF | DAIFSC_FIQF);

	for (uint32_t tries = 0; tries < 100000; tries++) {
		if (os_atomic_cmpxchg(&ci_tx_lock, 0, 1, acquire))
			break;
	}

	uint32_t next = (log_head + 1) % LOG_RING_SIZE;
	if (next != log_tail) {
		log_ring[log_head] = (uint8_t)c;
		log_head = next;
	} else {
		log_dropped++;
	}

	os_atomic_store(&ci_tx_lock, 0, release);
	__builtin_arm_wsr64("DAIF", daif);

	/*
	 * fix73: this is the funnel -- every character the kernel prints passes here, at all times,
	 * from every context.  Two things ride on it, both proven homeless elsewhere: the hypervisor
	 * watchdog's food (its thread_call starves in the boot storm, and a 65-second-old feed is
	 * exactly the boards' 40-90 s death), and one tx beacon per 4 KB so that the last beacon
	 * brackets a death in kernel time.  Recursion: the beacon's own characters re-enter here,
	 * guarded by the flag; the pet is rate-limited in its own body.
	 */
	{
		extern void hv_wdt_pet_now(void);
		extern int hakios_bringup_log_ready;
		static uint64_t tx_count;
		static int in_beacon;

		hv_wdt_pet_now();

		if (!in_beacon) {
			uint64_t n = ++tx_count;
			if (hakios_bringup_log_ready && (n & 0xFFFU) == 0) {
				static const char nib[] = "0123456789abcdef";
				char line[40];
				char *p = line;
				uint64_t v;
				int i;

				in_beacon = 1;
				*p++ = 't'; *p++ = 'x'; *p++ = ' ';
				for (i = 60; i >= 0; i -= 4) {
					*p++ = nib[(mach_absolute_time() >> i) & 0xF];
				}
				*p++ = ' ';
				v = n >> 12;
				{
					char tmp[16];
					char *q = tmp;
					do { *q++ = nib[v & 0xF]; v >>= 4; } while (v);
					while (q != tmp) { *p++ = *--q; }
				}
				*p++ = '\n';
				for (i = 0; i < p - line; i++) {
					if (line[i] == '\n') {
						ci_usb_xnu_putc_raw('\r');
					}
					ci_usb_xnu_putc_raw(line[i]);
				}
				in_beacon = 0;
			}
		}
	}
}

void ci_usb_xnu_putc(char c)
{
	if (!g_ci_usb_inited)
		return;

	if (c == '\n') {
		ci_usb_xnu_putc_raw('\r');
	}
	ci_usb_xnu_putc_raw(c);

	ci_usb_xnu_poll();
}

void ci_usb_xnu_puts(const char *s)
{
	if (!s) return;
	while (*s) {
		ci_usb_xnu_putc(*s++);
	}
}

/* RX Command API */
uint32_t ci_usb_xnu_rx_available(void)
{
	ci_usb_xnu_poll();
	uint32_t head = rx_head;
	uint32_t tail = rx_tail;
	return (head >= tail) ? (head - tail) : (RX_RING_SIZE + head - tail);
}

int ci_usb_xnu_getc(bool wait)
{
	do {
		ci_usb_xnu_poll();
		if (rx_head != rx_tail) {
			uint8_t c = rx_ring[rx_tail];
			rx_tail = (rx_tail + 1) % RX_RING_SIZE;
			return (int)c;
		}
	} while (wait);

	return -1;
}

uint32_t ci_usb_xnu_rx_read(void *dst, uint32_t max_len)
{
	ci_usb_xnu_poll();
	uint8_t *d = (uint8_t *)dst;
	uint32_t count = 0;
	while (count < max_len && rx_head != rx_tail) {
		d[count++] = rx_ring[rx_tail];
		rx_tail = (rx_tail + 1) % RX_RING_SIZE;
	}
	return count;
}

/* Initialization */
void ci_usb_xnu_init(void)
{
	if (g_ci_usb_inited)
		return;

	extern vm_offset_t pe_arm_get_soc_base_phys(void);
	if (pe_arm_get_soc_base_phys() == 0) {
		return; /* QEMU virt: do not touch Qualcomm USB MMIO */
	}

	/* 1. Map DMA Arena and Persistent RAM Log buffer (0x831F0000, 64KB uncached) */
	uintptr_t dma_va = (uintptr_t)ml_io_map(CI_DMA_PHYS_BASE, CI_DMA_MAP_SIZE);
	if (!dma_va) {
		return;
	}
	g_dma = (volatile struct ci_dma_arena *)dma_va;
	g_persist_log = (volatile char *)(dma_va + PERSIST_LOG_OFFSET);
	g_persist_idx = 0;

	/* Initialize persistent log header */
	*(volatile uint32_t *)&g_persist_log[0] = PERSIST_LOG_MAGIC;
	*(volatile uint32_t *)&g_persist_log[4] = 0;

	/* 2. Map ChipIdea USB controller and Synopsys Femto PHY MMIO */
	base = (uintptr_t)ml_io_map(MSM8952_USB_BASE, 0x1000);
	phy_base = (uintptr_t)ml_io_map(MSM8952_USB_PHY_CSR_BASE, 0x1000);
	if (!base || !phy_base) {
		return;
	}
	cap_base = base + 0x0140;

	lpm_active = (rd32(base + CI_HCCPARAMS) >> 17) & 1;

	/* 3. Device Prepare: Preserve aboot's ULPI session state! Do not issue destructive USBCMD_RST */
	wr32(base + CI_ABS_AHBMODE, 0x08); /* AHB transactor mode, posted data writes */

	/* Qualcomm MSM ChipIdea hardware workarounds and session valid control */
	wr32(base + CI_MSM_GENCONFIG, rd32(base + CI_MSM_GENCONFIG) & ~GENCONFIG_TXFIFO_IDLE_DIS);
	uint32_t gen2 = rd32(base + CI_MSM_GENCONFIG_2);
	gen2 &= ~GENCONFIG_2_TX_CLR_FIX;
	gen2 |= GENCONFIG_2_SESS_VLD_EN;
	wr32(base + CI_MSM_GENCONFIG_2, gen2);

	wr32(cap_base + CI_USBCMD, rd32(cap_base + CI_USBCMD) & ~USBCMD_RS);    /* Drop D+ briefly */

	for (volatile int d = 0; d < 200000; d++)
		__asm__ volatile("nop");

	wr32(op_reg(OP_ENDPTFLUSH), 0xFFFFFFFF);
	while (rd32(op_reg(OP_ENDPTFLUSH)))
		;

	/* Clear pending status & completions & OTG status bits */
	wr32(cap_base + CI_USBSTS, 0xFFFFFFFF);
	wr32(op_reg(OP_ENDPTCOMPLETE), 0xFFFFFFFF);
	wr32(cap_base + 0x0064, rd32(cap_base + 0x0064) | (0x3F << 16));

	/* Apply Femto PHY tuning sequence */
	ulpi_write(0x80, 0x73);
	ulpi_write(0x81, 0x68);
	ulpi_write(0x82, 0x24);

	/* Disable internal comparator and FORCE external VBUS valid on Femto PHY */
	ulpi_write(0x88, 0x01); /* Direct write */
	ulpi_write(0x89, 0x01); /* ULPI_SET(0x88) OTG_COMP_DISABLE */
	ulpi_write(0x96, 0x03); /* Direct write */
	ulpi_write(0x97, 0x03); /* ULPI_SET(0x96) VBUSVLDEXTSEL | VBUSVLDEXT */

	/* Device mode, Setup Lockout off */
	wr32(op_reg(OP_USBMODE), USBMODE_CM_DEVICE | USBMODE_SLOM);

	/* Zero interrupt threshold for immediate STS updates */
	wr32(cap_base + CI_USBCMD, USBCMD_ITC(0));

	/* Setup Queue Heads */
	for (int i = 0; i < 16; i++) {
		g_dma->dqh[i].cap = 0;
		g_dma->dqh[i].curr_dtd = 0;
		g_dma->dqh[i].next_dtd = TD_TERMINATE;
		g_dma->dqh[i].token = 0;
	}

	uint32_t phys_dqh = ci_dma_pa(g_dma->dqh);
	wr32(cap_base + CI_ENDPTLISTADDR, phys_dqh);
	wr32(cap_base + CI_USBSTS, 0xFFFFFFFF);
	wr32(cap_base + CI_USBINTR, USBINTR_UIE | USBINTR_UEIE | USBINTR_PCIE | USBINTR_URE | USBINTR_SLE);

	/* 4. EP0 Start */
	g_dma->dqh[0].cap = QH_MAX_PKT(64) | QH_ZLT | QH_IOS;
	g_dma->dqh[0].next_dtd = TD_TERMINATE;
	g_dma->dqh[1].cap = QH_MAX_PKT(64) | QH_ZLT;
	g_dma->dqh[1].next_dtd = TD_TERMINATE;

	wr32(ctrl_reg(0), ENDPTCTRL_RXE | ENDPTCTRL_TXE);

	/* 5. Device Run: Assert USBCMD.RS and USBCMD_SESS_VLD_CTRL to pull up D+ */
	wr32(cap_base + CI_USBCMD, rd32(cap_base + CI_USBCMD) | USBCMD_RS | USBCMD_SESS_VLD_CTRL);

	g_ci_usb_inited = true;

	/* Wait for host enumeration and configuration (up to 3 seconds) */
	uint64_t t_start = get_ticks();
	while (!configured && (get_ticks() - t_start < 57600000)) { /* 3 sec at 19.2MHz */
		ci_usb_poll_internal();
	}

	/* Replay any prior early log from loader_log into USB CDC ring */
	extern char loader_log[];
	extern uint32_t loader_log_len;
	if (loader_log_len > 0) {
		for (uint32_t i = 0; i < loader_log_len && i < LOADER_LOG_SIZE; i++) {
			ci_usb_xnu_putc_raw(loader_log[i]);
			if ((i & 255) == 0) {
				ci_usb_poll_internal();
			}
		}
	}

	ci_usb_xnu_puts("\r\n[CI_USB] MSM8952 ChipIdea USB CDC-ACM online in XNU!\r\n");
	ci_usb_xnu_poll();
}

STARTUP(EARLY_BOOT, STARTUP_RANK_FIRST, ci_usb_xnu_init);
