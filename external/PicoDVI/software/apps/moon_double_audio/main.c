#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/structs/bus_ctrl.h"
#include "pico/multicore.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode.h"
#include "tmds_double.h"
#include "audio.h"

#include "moon_1bpp_640x480.h"
#define moon_img moon_1bpp_640x480
#define IMAGE_WIDTH 640
//#define AUDIO_RATE 32000
//#define AUDIO_RATE 44100
#define AUDIO_RATE 48000

#if (AUDIO_RATE == 32000)
#define HDMI_N     4096     // From HDMI standard for 32kHz
#elif (AUDIO_RATE == 44100)
#define HDMI_N     6272     // From HDMI standard for 44.1kHz
#else
#define HDMI_N     6144     // From HDMI standard for 48kHz
#endif

#define FIFTYHZMS   20      // ms between frames at 50Hz
#define TICKMS      2       // Time between audio callbacks
#define TICKCOUNT   (FIFTYHZMS/TICKMS)
#define MAX_SIZE    (TICKMS * AUDIO_RATE / 1000)

static semaphore_t fifty_hz;
static semaphore_t init;
static void core1_main();

// Display 2x2 pixels for each pixel in B&W image
// Also plays audio
// Note: Audio will sound slow, as recorded at 44.1kHz,
// but playback set to 32kHz

// Pick one:
//#define MODE_640x480_60Hz
//#define MODE_720x540_50Hz
#define MODE_720x576_50Hz

#if defined(MODE_640x480_60Hz)
#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480
#define DVI_TIMING dvi_timing_640x480p_60hz
#elif defined(MODE_720x540_50Hz)
#define FRAME_WIDTH 720
#define FRAME_HEIGHT 540
#define DVI_TIMING dvi_timing_720x540p_50hz
#elif defined(MODE_720x576_50Hz)
#define FRAME_WIDTH 720
#define FRAME_HEIGHT 576
#define DVI_TIMING dvi_timing_720x576p_50hz

#else
#error "Select a video mode!"
#endif

struct dvi_inst dvi0;
audio_ring_t* ring;
static uint32_t call_count = 0;

//Audio Related
#define AUDIO_BUFFER_SIZE   (0x1 << 9)  // Must be power of two
audio_sample_t      audio_buffer[AUDIO_BUFFER_SIZE];
struct repeating_timer audio_timer;

static bool __not_in_flash_func(audio_timer_callback)(struct repeating_timer *t) 
{
    static uint sample_count = 0;
    // write in chunks
    int size = get_write_size(&dvi0.audio_ring, true);
#if (AUDIO_RATE != 44100)
    if (size >= MAX_SIZE)
    {
        size = (size > ((3*AUDIO_BUFFER_SIZE)>>2)) ? (MAX_SIZE<<1) : MAX_SIZE;
#else
    static uint8_t sam_size[TICKCOUNT] = {88, 88, 88, 88, 89, 88, 88, 88, 88,89};

    // write in chunks
    if (size >= sam_size[call_count])
    {
        size = (size > ((3*AUDIO_BUFFER_SIZE)>>2)) ? sam_size[call_count] + sam_size[(call_count+1)%TICKCOUNT]: sam_size[call_count];
#endif

        uint audio_offset = get_write_offset(&dvi0.audio_ring);
        for (int cnt = 0; cnt < size; cnt++)
        {
            audio_buffer[audio_offset].channels[0] = commodore_argentina[sample_count % commodore_argentina_len] << 8;
            audio_buffer[audio_offset].channels[1] = commodore_argentina[(sample_count+1024) % commodore_argentina_len] << 8;
            sample_count = (sample_count + 1) % commodore_argentina_len;
            audio_offset = (audio_offset + 1) & (AUDIO_BUFFER_SIZE-1);
        }
        set_write_offset(&dvi0.audio_ring, audio_offset);
    }
    ++call_count;
    if (call_count == TICKCOUNT)
    {
        call_count = 0;
        sem_release(&fifty_hz);
    }
    return true;
}

int main()
{
	set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);
	setup_default_uart();

	dvi0.timing = &DVI_TIMING;
	dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
	dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Give priority to 2nd core
	hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);

    // HDMI Audio related
    dvi_get_blank_settings(&dvi0)->top    = 0;
    dvi_get_blank_settings(&dvi0)->bottom = 0;
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);
    ring = &dvi0.audio_ring;
    set_write_offset(ring, AUDIO_BUFFER_SIZE>>1);

    dvi_set_audio_freq(&dvi0, AUDIO_RATE, dvi0.timing->bit_clk_khz*HDMI_N/(AUDIO_RATE/100)/128, HDMI_N);

	printf("Freq %i Set CTS %i\n", dvi0.timing->bit_clk_khz, dvi0.timing->bit_clk_khz*HDMI_N/(AUDIO_RATE/100)/128);


    sem_init(&fifty_hz, 0, 1);
    sem_init(&init, 0, 1);

    // launch all the video on core 1
    multicore_launch_core1(core1_main);
    sem_acquire_blocking(&init);
    add_repeating_timer_ms(-TICKMS, audio_timer_callback, NULL, &audio_timer);

	while(1)
    {
        sem_acquire_blocking(&fifty_hz);
    }
}
	
static void __not_in_flash_func(render_loop)()
{
    uint32_t *tmdsbuf = 0;

	while (true)
    {
		for (uint y = 0; y < FRAME_HEIGHT/2; ++y)
        {
			const uint8_t *colourbuf = (const uint8_t*)&moon_img[(y + (FRAME_HEIGHT >> 3))* IMAGE_WIDTH / 8];
			queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);

			tmds_double_1bpp(colourbuf, tmdsbuf, FRAME_WIDTH);
			queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
		}
	}
}

static void core1_main()
{
	dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
	dvi_start(&dvi0);
    sem_release(&init);
    render_loop();
}
