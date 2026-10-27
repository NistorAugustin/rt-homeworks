// 03_isr_coada - butonul BOOT, o rutină de întrerupere și o coadă
//
// Fiecare front descrescător pe GPIO9 (butonul BOOT, LOW cât timp e apăsat)
// declanșează rutina de întrerupere (ISR). ISR-ul notează momentul și pune un
// element în coadă cu xQueueSendFromISR(), atât. Un task preia elementele din
// coadă și le afișează pe Serial.
//
// Ce faceți: apăsați BOOT de câteva ori, scurt și apoi lung. La o singură
// apăsare apar adesea mai multe întreruperi, la distanță de microsecunde sau
// câteva milisecunde: contactele butonului „sar” (bounce). Exemplul nu le
// filtrează, ca să le vedeți; filtrarea este în exemplul 04.
//
// Reguli pentru ISR:
//   - se declară cu IRAM_ATTR (codul stă în RAM, nu în flash)
//   - este scurt: fără Serial, fără mutex, fără așteptări
//   - folosește doar funcțiile FreeRTOS cu sufixul FromISR
//   - elementul cozii conține datele însele (aici, momentul), nu un pointer
//
// Nu țineți BOOT apăsat când apăsați RST: așa porniți bootloaderul, nu schița.

const int BUTTON_PIN = 9;

// Un element al cozii: momentul întreruperii, în ms (ticks) și în µs
struct Press {
    uint32_t at_ms;
    uint32_t at_us;
};

static QueueHandle_t g_queue = NULL;
static volatile uint32_t g_lost = 0;  // întreruperi pierdute: coada era plină

void IRAM_ATTR button_isr(void) {
    Press p = {(uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS), (uint32_t)micros()};
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(g_queue, &p, &woken) != pdTRUE) g_lost = g_lost + 1;
    portYIELD_FROM_ISR(woken);  // dacă a fost trezit un task mai prioritar, rulează imediat
}

static void press_task(void* arg) {
    (void)arg;
    uint32_t n = 0;
    uint32_t prev_us = 0;
    uint32_t lost_reported = 0;
    for (;;) {
        Press p;
        xQueueReceive(g_queue, &p, portMAX_DELAY);  // blocat până sosește o întrerupere
        n = n + 1;
        uint32_t delta_us = p.at_us - prev_us;
        prev_us = p.at_us;

        if (n == 1 || delta_us > 1000000) {
            Serial.printf("\n#%lu  t = %lu ms\n", (unsigned long)n, (unsigned long)p.at_ms);
        } else {
            Serial.printf("#%lu  t = %lu ms, la %lu µs după precedenta%s\n", (unsigned long)n,
                          (unsigned long)p.at_ms, (unsigned long)delta_us, delta_us < 50000 ? "  <- bounce?" : "");
        }
        if (g_lost != lost_reported) {
            lost_reported = g_lost;
            Serial.printf("   întreruperi pierdute (coada plină): %lu\n", (unsigned long)lost_reported);
        }
    }
}

void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);
    delay(200);
    Serial.println();
    Serial.println("=== tema 03, exemplul 03: isr_coada ===");
    Serial.println("apăsați butonul BOOT");

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    g_queue = xQueueCreate(20, sizeof(Press));  // coada, înainte de ISR și de task

    xTaskCreate(press_task, "apasari", 4096, NULL, 3, NULL);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING);  // la sfârșit
}

void loop() {
    delay(1000);
}
