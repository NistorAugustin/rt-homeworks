// 05_aperiodic_comenzi - un task aperiodic care execută comenzi text
//
// loop() citește linii din Serial Monitor și pune fiecare linie, ca text, în
// coada de comenzi, fără să se blocheze (timeout 0). Așa face și
// command_received() cu comenzile venite prin MQTT. Elementul cozii conține
// textul însuși (un tablou char), nu un pointer: bufferul din loop() se
// refolosește pentru linia următoare.
//
// Fiecare comandă eliberează un job al taskului aperiodic, care o
// interpretează, modifică starea LED-ului (cu mutexul) și răspunde cu
// "OK ..." sau "ERROR ...". O comandă greșită nu oprește taskul.
//
// Un al doilea task, periodic (T = 50 ms), aplică starea pe LED.
//
// Comenzi (Serial Monitor cu „New Line” sau „Both NL & CR”):
//   LED ON | LED OFF    LED-ul aprins / stins
//   BLINK <ms>          LED-ul clipește, cu semiperioada <ms>, între 100 și 5000
//   STATUS              starea curentă
// Încercați și comenzi greșite: BLINK, BLINK 20, BLINK 300 400, LED, xyz.

const int LED_PIN = 8;  // LED-ul albastru: aprins pe LOW
const size_t kCommandMaxLen = 32;
const uint32_t kLedPeriodMs = 50;
const long kBlinkMinMs = 100;
const long kBlinkMaxMs = 5000;

enum LedMode { MODE_OFF, MODE_ON, MODE_BLINK };
const char* const MODE_NAMES[] = {"OFF", "ON", "BLINK"};

// Starea partajată de taskul aperiodic (scrie) și taskul periodic (citește)
struct LedState {
    LedMode mode;
    uint32_t blink_ms;
};

static LedState g_led = {MODE_OFF, 500};
static SemaphoreHandle_t g_mutex = NULL;

struct Command {
    char text[kCommandMaxLen];
};
static QueueHandle_t g_commands = NULL;

static void reply(const char* text) {
    Serial.printf("[comenzi] %s\n", text);
}

static void command_task(void* arg) {
    (void)arg;
    for (;;) {
        Command c;
        xQueueReceive(g_commands, &c, portMAX_DELAY);  // blocat până sosește o comandă

        char out[80];
        char name[16];
        long value = 0;
        char extra;  // orice caracter după număr face comanda greșită ("BLINK 300 400", "BLINK 300ms")
        int n = sscanf(c.text, "%15s %ld %c", name, &value, &extra);  // n = câte câmpuri s-au citit

        if (strcmp(c.text, "LED ON") == 0 || strcmp(c.text, "LED OFF") == 0) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_led.mode = strcmp(c.text, "LED ON") == 0 ? MODE_ON : MODE_OFF;
            xSemaphoreGive(g_mutex);
            snprintf(out, sizeof(out), "OK %s", c.text);
        } else if (n >= 1 && strcmp(name, "BLINK") == 0) {
            if (n != 2 || value < kBlinkMinMs || value > kBlinkMaxMs) {
                snprintf(out, sizeof(out), "ERROR BLINK: se așteaptă un singur număr, între %ld și %ld", kBlinkMinMs, kBlinkMaxMs);
            } else {
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                g_led.mode = MODE_BLINK;
                g_led.blink_ms = value;
                xSemaphoreGive(g_mutex);
                snprintf(out, sizeof(out), "OK BLINK %ld", value);
            }
        } else if (strcmp(c.text, "STATUS") == 0) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            LedState s = g_led;  // copia, ca să nu formatăm textul cu mutexul luat
            xSemaphoreGive(g_mutex);
            snprintf(out, sizeof(out), "OK STATUS mod=%s blink=%lu", MODE_NAMES[s.mode], (unsigned long)s.blink_ms);
        } else {
            snprintf(out, sizeof(out), "ERROR comandă necunoscută: %s", c.text);
        }
        reply(out);
    }
}

static void led_task(void* arg) {
    (void)arg;
    bool on = false;
    uint32_t since_toggle_ms = 0;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        LedState s = g_led;
        xSemaphoreGive(g_mutex);

        since_toggle_ms += kLedPeriodMs;
        if (s.mode == MODE_ON) {
            on = true;
        } else if (s.mode == MODE_OFF) {
            on = false;
        } else if (since_toggle_ms >= s.blink_ms) {
            on = !on;
            since_toggle_ms = 0;
        }
        digitalWrite(LED_PIN, on ? LOW : HIGH);
        xTaskDelayUntil(&last, pdMS_TO_TICKS(kLedPeriodMs));
    }
}

// Pune o linie în coada de comenzi, ca command_received() din temă.
static void command_received(const char* text) {
    Command c;
    strncpy(c.text, text, sizeof(c.text) - 1);
    c.text[sizeof(c.text) - 1] = '\0';
    if (xQueueSend(g_commands, &c, 0) != pdTRUE) Serial.println("[loop] coada de comenzi plină");
}

void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);
    delay(200);
    Serial.println();
    Serial.println("=== tema 03, exemplul 05: aperiodic_comenzi ===");
    Serial.println("comenzi: LED ON, LED OFF, BLINK <ms>, STATUS");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED stins

    g_mutex = xSemaphoreCreateMutex();
    g_commands = xQueueCreate(5, sizeof(Command));

    xTaskCreate(command_task, "comenzi", 4096, NULL, 2, NULL);
    xTaskCreate(led_task, "led", 4096, NULL, 3, NULL);
}

void loop() {
    static char line[kCommandMaxLen];
    static size_t len = 0;
    while (Serial.available() > 0) {
        char ch = Serial.read();
        if (ch == '\n' || ch == '\r') {
            if (len > 0) {
                line[len] = '\0';
                command_received(line);
                len = 0;
            }
        } else if (len < sizeof(line) - 1) {
            line[len++] = ch;
        }
    }
    delay(20);
}
