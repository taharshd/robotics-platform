#include <stdio.h>
#include <unistd.h>
#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <driver/gptimer.h>

struct userData {
        bool ledOn;
        esp_cpu_cycle_count_t timestamps[1000], deltas[1000];
        int read, write;
} us;

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

void app_main(void)
{
    gpio_set_direction(4, GPIO_MODE_OUTPUT);
    
    gptimer_handle_t gptimer = NULL;
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1 * 1000 * 1000,
    };
    gptimer_new_timer(&timer_config, &gptimer);

    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count = 1 * 1000 * 10, // 1000 * 10 = 100hz
        .flags.auto_reload_on_alarm = true,
    };

    gptimer_set_alarm_action(gptimer, &alarm_config);

    gptimer_event_callbacks_t callback = {
        .on_alarm = timerLED,
    };

    gptimer_register_event_callbacks(gptimer, &callback, (void*)&us);
    
    gptimer_enable(gptimer);

    gptimer_start(gptimer);

    vTaskDelay(10000 / portTICK_PERIOD_MS);
    char* str = malloc(1000);
    vTaskList(str);
    printf("%s", str);
    int min = INT32_MAX, max = 0;
    unsigned long lu;
    int jitter = 0; 
    printf("Us.write is %i\n", us.write);
    for (int i = 2; i < 999; ++i) {
        lu = us.deltas[i];
        jitter = ((int)lu) - (160000000 * 0.01);
        printf("Delta %i: %lu\n", i, lu);
        if (jitter > max)
            max = jitter;
        if (jitter < min)
            min = jitter;
    }
printf("Max jitter is: %i, Min jitter is: %i", max, min);
}

