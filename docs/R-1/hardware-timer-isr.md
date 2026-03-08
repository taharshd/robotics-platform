## Week 1 learnings

### Hardware timer

The ESP-IDF framework provides an API to interact with timers, `gptimer`. Hardware timers have the added benefit of being deterministically triggered instead of relying on functions like `sleep(1000)` or busy-wait loops, and can be triggered and invoked without pausing other tasks, facilitating real-time efforts.
The timer is set up through `gptimer_new_timer`, with the config present in `week1.c`.

In the hardware, ESP32 timers use the Advanced Peripheral Bus (APB) Clock, which has a 80MHz clock rate. A prescaler is further used to reduce the frequency and increase the range of the timer, as it's stored in 64 bits, through two 32-bit registers.
The `.resolution_hz = 1 * 1000 * 1000` argument in the `gptimer_config_t` utilizes the prescaler for this.
The frequency of the timer is `Resolution = APB_CLK Frequency / Divider`, in this case the divider is 80.

### Jitter measurement

Jitter, the varying cycle delta between subsequent ISR invocations was measured for an ISR that blinked an LED at 100Hz, 1kHz, and 5kHz.

| Frequency | Period | Expected Cycles | Steady-state jitter | Max spike |
| --- | --- | --- | --- | --- |
| 100 hz | 10 ms | 1,600,000 | +/- 4 cycles | N/A |
| 1 kHz | 1 ms | 160,000 | +/- 4-8 cycles | +/- 412 cycles (2.6µs) |
| 5 kHz | 200 µs | 32,000 | +/- 6-16 cycles | +/- 476 cycles (3µs) |

Timestamps are stored via `esp_cpu_get_cycle_count()` on every ISR invocation and deltas are computed based on these, both are stored in a circular buffer of size 1,000. The first ISR invocation fetches data from the SPI flash, taking extra CPU cycles. The timer for the second ISR invocation fires at the same interval, so the second delta is roughly shorter by the same amount of CPU cycles the first was extended by, they compensate. First two deltas are excluded due to this.
The default tick rate of FreeRTOS on the ESP32 is 10ms. This synchronizes perfectly with the 100 Hz case. Any pre-emption due to a different task being run happens at the same predictable time, causing the deltas to remain the same and the jitter to remain constant.

For 1kHz and 5kHz, occasionally a pair crops up where the jitter spikes, i.e the delta being +400 cycles for delta 501 and -400 cycles for delta 502. This happens due to pre-emption because of another interrupt of a higher priority firing at or before the timer. First ISR takes some extra cycles, next one takes fewer due to the timer firing at the same time, resulting in a jitter value that's roughly equal in +/- cycle spike.

### IRAM attr

The `timerLED` function is paired with the `IRAM_ATTR` macro, this places the ISR into IRAM, internal memory. Firmware code is stored on an external SPI flash, which is connected to the ESP32's CPU over a serial bus. When the CPU must execute code from flash, it fetches it from the SPI flash, but first from the instruction cache which sits in between. If the code is in the instruction cache, it only takes ~1 cycle. If not, fetching through the SPI flash can take tens to hundreds of cycles. Placing the code in IRAM makes every fetch cost the same, every time. ISRs thus have deterministic timing. This was further confirmed by inspecting the mapfile, `week1.map`:
``` 
 .iram1.0       0x403767d8       0x65 esp-idf/main/libmain.a(week1.c.obj)
 *fill*         0x4037683d        0x3
```
The named symbol `timerLED` is absent, likely due to the symbol being stripped or the function being inlined by the compiler. Irrelevant for the code execution however, as `week1.c.obj` is confirmed to be in IRAM at `0x403767d8`, containing the `timerLED` code.

The disassembly for the function is:
```
403767d8 <timerLED>:
403767d8:	004136        	entry	a1, 32 // entry into the function, sets up the stack frame, allocating 32 bytes.
403767db:	000482        	l8ui	a8, a4, 0 // a8 = us->ledOn, reads the first byte of user_ctx (a4).
403767de:	e88c      	beqz.n	a8, 403767f0 <timerLED+0x18> // branches if us->ledOn == false
403767e0:	0b0c      	movi.n	a11, 0 // moves arg2 of gpio_set_level(4, 0) into a11
403767e2:	4a0c      	movi.n	a10, 4 // moves arg1 of gpio_set_level(4, 0) into a10
403767e4:	f7b981        	l32r	a8, 403746c8 <_iram_text_start+0x2c4> (42009400 <gpio_set_level>) // loads the 32-bit address of gpio_set_level() into a8
403767e7:	0008e0        	callx8	a8 // calls the address stored in a8, gpio_set_level()
403767ea:	080c      	movi.n	a8, 0 // us->ledOn = false;
403767ec:	000306        	j	403767fc <timerLED+0x24> // jump over the else branch
403767ef:	1b0c00        	un.s	b0, f12, f0 // disassembler losing sync between branches. this is NOT A REAL INSTRUCTION!
403767f2:	4a0c      	movi.n	a10, 4 // else branch, load first arg of gpio_set_level(4, 1)
403767f4:	f7b581        	l32r	a8, 403746c8 <_iram_text_start+0x2c4> (42009400 <gpio_set_level>) // load gpio_set_level() into a8
403767f7:	0008e0        	callx8	a8 // call address stored in a8
403767fa:	180c      	movi.n	a8, 1 // us->ledOn = true;
403767fc:	1fd4b2        	addmi	a11, a4, 0x1f00
403767ff:	122b92        	l32i	a9, a11, 72 // computes a11 = a4 + 0x1f00 + 72 in two instructions because Xtensa immediate range is limited. same for 4037680f and 40376812. a9 = us->write
40376802:	004482        	s8i	a8, a4, 0 // writes the us->ledOn value that was stored in a8, into a4 (us->ledOn).
40376805:	03ea80        	rsr.ccount	a8 // read special register CCOUNT into a8. esp_cpu_get_cycle_count() directly expands into this
40376808:	a0a940        	addx4	a10, a9, a4 // a10 = a9*4 + a4  (a10 = us->write * 4 + us_ctx)
4037680b:	1a89      	s32i.n	a8, a10, 4 // stores ccount (a8) into us->timestamps[us->write], with displacement 4 folding in the array's offset from the struct base.
4037680d:	09dc      	bnez.n	a9, 40376821 <timerLED+0x49> // if (us->write == 0), branch if a9 ≠ zero (jumps to the else branch)
4037680f:	0fd442        	addmi	a4, a4, 0xf00 // calculating the timestamps[999] offset, 0xf00 = 3840 (256 * 15)
40376812:	2824a2        	l32i	a10, a4, 160 // us->timestamps[999] (3840 + 160 = 4000)
40376815:	ba9c      	beqz.n	a10, 40376834 <timerLED+0x5c> // if (us->timestamps[999] != 0), branch if eq zero 
40376817:	c088a0        	sub	a8, a8, a10 // a8 -= a10 (a8 = us->timestamps[us->write], a10 = us->timestamps[999])
4037681a:	296482        	s32i	a8, a4, 164 // a4 + 164 = 4004, which is us->deltas. In this branch us->write = 0, us->deltas[us->write] = a8
4037681d:	0004c6        	j	40376834 <timerLED+0x5c> // Jump over else branch
40376820:	0ac800        	add.s	f12, f8, f0 // --- Disassembler misalignment (off by one byte, should start at 40376821.)
40376823:	0fdaa2        	addmi	a10, a10, 0xf00
40376826:	c088c0        	sub	a8, a8, a12 
40376829:	296a82        	s32i	a8, a10, 164 // --- Disassembler misalignment
4037682c:	e7a3a2        	movi	a10, 0x3e7 // a10 = 999
4037682f:	080c      	movi.n	a8, 0 // a8 = 0 for the branch where us->write = 0
40376831:	0119a7        	beq	a9, a10, 40376836 <timerLED+0x5e> // if (us->write == 999)
40376834:	891b      	addi.n	a8, a9, 1 // a8 = us->write + 1 // else branch, a8 = us->write + 1
40376836:	126b82        	s32i	a8, a11, 72 // us->write (a11+72) = 0 (a8)
40376839:	020c      	movi.n	a2, 0 // Setting return value register to 0 (false)
4037683b:	f01d      	retw.n // return false;
4037683d:	000000        	ill
```

The corresponding C code:

```
static bool IRAM_ATTR timerLED(gptimer_handle_t gptimer, const gptimer_alarm_event_data_t*, void *user_ctx) {
    struct userData* us = (struct userData*) user_ctx;
    if (us->ledOn == true) {
        gpio_set_level(4, 0);
        us->ledOn = false;
    }
    else {
        gpio_set_level(4, 1);
        us->ledOn = true;
    }
    us->timestamps[us->write] = esp_cpu_get_cycle_count();
    if (us->write == 0) {
        if (us->timestamps[999] != 0)
            us->deltas[us->write] = us->timestamps[us->write] - us->timestamps[999];
    }
    else
        us->deltas[us->write] = us->timestamps[us->write] - us->timestamps[us->write - 1];
    if (us->write == 999)
        us->write = 0;
    else
        us->write += 1;
    return false;
}
```

Note that calling library functions such as `gpio_set_level()` within the ISR is bad practice, as these come with their own structured calls that can be redundant. `gpio_set_level()` has a validation check that checks if the input pin number is valid, for a 1kHz / 5kHz control loop this is extra CPU cycles, and this damages determinism and timing guarantees. For this reason, it is preferred to manipulate the registers directly within ISRs to minimize the instructions executed. This can be done through the internal implementation of `gpio_ll.h` here:
```
if (level) {
    if (gpio_num < 32) {
        hw->out_w1ts = 1 << gpio_num;
    } else {
        hw->out1_w1ts.val = 1 << (gpio_num - 32);
    }
} else {
    if (gpio_num < 32) {
        hw->out_w1tc = 1 << gpio_num;
    } else {
        hw->out1_w1tc.val = 1 << (gpio_num - 32);
    }
}
```

Testing was also performed w/ `-O0` and `-O2`, the latter decreased binary size by 30% from 314KB down to 202KB and decreased steady state jitter by 20-30%, along with max spike jitter by 20% as well. Loop unrolling, fewer instructions, instruction scheduling, dead code elimination all play a role.