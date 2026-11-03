// Device-ul IoT din tema 01, de data aceasta pe un ESP32-C3 Super Mini.
//
// Aceeași logică și același protocol ca device-simulator din tema 01, cu trei
// taskuri FreeRTOS în loc de trei thread-uri Java:
//
//   task periodic --(coada de alarme)--> task sporadic
//        |                                     |
//        +---- telemetry ---> MQTT <--- alarm -+
//                              |
//                          commands --> command_received() --(coada de comenzi)--> task aperiodic ---> status
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, valorile inițiale; DEVICE_ID
//                    se citește din adresa MAC a plăcii
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.
//
// PRIORITĂȚI (ESP32-C3 are un singur nucleu, planificare preemptivă cu priorități fixe):
//   periodic  4  termene hard (D = T) și joburi scurte: cea mai mare prioritate dintre ale noastre
//   sporadic  3  termene hard, dar interval minim între sosiri de 5 s; nu trebuie să întârzie telemetria
//   aperiodic 2  fără garanții (termene soft): rulează când cele două de mai sus nu au de lucru
// Toate sunt peste loopTask (1) și sub taskurile sistemului (clientul MQTT 5,
// evenimente 20, Wi-Fi 23), ca rețeaua să nu fie blocată de taskurile noastre.

#include "config.h"
#include "connection.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

// ---------------------------------------------------------------- configurare taskuri

const UBaseType_t PRIO_PERIODIC = 4;
const UBaseType_t PRIO_SPORADIC = 3;
const UBaseType_t PRIO_APERIODIC = 2;

const uint32_t TASK_STACK = 4096;  // în octeți; snprintf cu "%f" are nevoie de stivă

const UBaseType_t ALARM_QUEUE_LEN = 8;
const UBaseType_t COMMAND_QUEUE_LEN = 8;

// limite pentru SET_INTERVAL, ca o comandă greșită să nu blocheze placa
const uint32_t PERIOD_MS_MIN = 100;
const uint32_t PERIOD_MS_MAX = 60000;

// ---------------------------------------------------------------- stare partajată

// Citite de taskul periodic, modificate de taskul aperiodic: volatile, ca fiecare
// job să citească valoarea curentă din memorie. Pe un nucleu de 32 de biți,
// citirea și scrierea unui uint32_t sau float sunt atomice.
static volatile uint32_t g_period_ms = PERIOD_MS_INITIAL;
static volatile float g_threshold = THRESHOLD_INITIAL;

// Elementul cozii de alarme. Se copiază prin valoare (xQueueSend copiază
// elementul), cu pragul care a declanșat alarma: alarma publicată descrie exact
// situația măsurată, chiar dacă între timp pragul a fost schimbat cu SET_THRESHOLD.
typedef struct {
    int64_t timestamp;  // epoch_ms() la măsurare
    float temperature;
    float threshold;
} alarm_event_t;

// Elementul cozii de comenzi: textul însuși, nu un pointer. Textul primit în
// command_received() este valid doar în timpul apelului.
typedef struct {
    char text[COMMAND_MAX_LEN];
} command_t;

static QueueHandle_t g_alarm_queue = NULL;
static QueueHandle_t g_command_queue = NULL;

// Temperatura cipului, în °C, de la senzorul intern al ESP32-C3.
// Este temperatura siliciului, nu a camerei: imediat după pornirea Wi-Fi-ului
// poate fi în jur de 50 °C, apoi scade în câteva minute spre 35-40 °C.
// Rezoluția este de aproximativ 1 °C. Ca să provocați alarme, puneți pragul
// cu SET_THRESHOLD cu un grad sub valoarea citită.
float read_temperature(void) {
    return temperatureRead();
}

// ---------------------------------------------------------------- TODO 1: task periodic

// Eliberări pe grila r_k = r_0 + kT, cu așteptare ABSOLUTĂ: xTaskDelayUntil
// așteaptă până la last_wake + T și apoi mută last_wake pe acel moment. Timpul
// de calcul al jobului nu se adună la perioadă, deci nu apare derivă (cu
// vTaskDelay(T) perioada reală ar fi T + C + întârzieri).
static void periodic_task(void* arg) {
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();  // r_0
    uint32_t job = 0;

    for (;;) {
        // întârzierea jobului față de eliberarea lui de pe grilă (jitter), în tick-uri de 1 ms
        uint32_t late_ms = (uint32_t)((xTaskGetTickCount() - last_wake) * portTICK_PERIOD_MS);

        float temperature = read_temperature();
        int64_t timestamp = epoch_ms();
        float threshold = g_threshold;  // citit o singură dată: comparația și evenimentul folosesc aceeași valoare
        uint32_t period_ms = g_period_ms;

        char msg[96];
        snprintf(msg, sizeof(msg), "%s;%lld;%.2f", DEVICE_ID, (long long)timestamp, temperature);
        mqtt_publish("telemetry", msg);

        bool over = temperature > threshold;
        if (over) {
            // doar semnalăm evenimentul, fără să ne blocăm (timeout 0);
            // alarma o tratează taskul sporadic
            alarm_event_t ev = {timestamp, temperature, threshold};
            if (xQueueSend(g_alarm_queue, &ev, 0) != pdTRUE) {
                Serial.println("[periodic] coada de alarme este plină, eveniment pierdut");
            }
        }

        Serial.printf("[periodic] job %lu  întârziere %lu ms  T=%lu ms  %s%s\n", (unsigned long)job,
                      (unsigned long)late_ms, (unsigned long)period_ms, msg, over ? "  > prag!" : "");

        // Următoarea eliberare. g_period_ms se citește din nou aici, deci
        // SET_INTERVAL are efect de la următoarea eliberare.
        TickType_t period = pdMS_TO_TICKS(g_period_ms);
        if (xTaskDelayUntil(&last_wake, period) == pdFALSE) {
            // Overrun: momentul last_wake + T trecuse deja, deci nu s-a așteptat
            // deloc. Sărim eliberările ratate, ca să nu pornim joburi în rafală.
            TickType_t now = xTaskGetTickCount();
            uint32_t missed = 0;
            while ((TickType_t)(now - last_wake) >= period) {
                last_wake += period;
                missed++;
            }
            Serial.printf("[periodic] overrun: %lu eliberări ratate\n", (unsigned long)missed);
        }
        job++;
    }
}

// ---------------------------------------------------------------- TODO 2: task sporadic

// Impune intervalul minim între sosiri: între două alarme publicate (două
// eliberări acceptate) trebuie să treacă cel puțin MIN_INTERARRIVAL_MS.
// Evenimentele care sosesc mai devreme sunt respinse și nu resetează cronometrul.
// Timpul se măsoară cu millis() (monoton), nu cu epoch_ms(), care poate sări
// la sincronizarea orei.
static void sporadic_task(void* arg) {
    (void)arg;
    uint32_t last_release_ms = 0;
    bool released = false;  // încă nu a existat nicio alarmă publicată

    for (;;) {
        alarm_event_t ev;
        if (xQueueReceive(g_alarm_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint32_t arrival = millis();
        uint32_t since_last = arrival - last_release_ms;  // corect și la depășirea lui millis()

        if (released && since_last < MIN_INTERARRIVAL_MS) {
            Serial.printf("[sporadic] RESPINS: %.2f > %.2f, la doar %lu ms de ultima alarmă (minim %lu ms)\n",
                          ev.temperature, ev.threshold, (unsigned long)since_last,
                          (unsigned long)MIN_INTERARRIVAL_MS);
            continue;
        }

        last_release_ms = arrival;
        released = true;

        char msg[96];
        snprintf(msg, sizeof(msg), "%s;%lld;%.2f;%.2f", DEVICE_ID, (long long)ev.timestamp, ev.temperature,
                 ev.threshold);
        mqtt_publish("alarm", msg);
        Serial.printf("[sporadic] ALARMĂ publicată: %s  (R = %lu ms)\n", msg, (unsigned long)(millis() - arrival));
    }
}

// ---------------------------------------------------------------- TODO 3: task aperiodic

// Taie spațiile de la început și de la sfârșit (comenzile pot veni cu "\r\n").
static char* trim(char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = '\0';
    return s;
}

// Execută o comandă și scrie confirmarea în result: "OK ..." sau "ERROR ...".
// Mesajele nu conțin ';' (separatorul câmpurilor) și nici diacritice.
static void execute_command(char* command, char* result, size_t size) {
    char* save = NULL;
    char* name = strtok_r(command, " \t", &save);
    char* param = strtok_r(NULL, " \t", &save);
    char* extra = strtok_r(NULL, " \t", &save);

    if (name == NULL) {
        snprintf(result, size, "ERROR comanda goala");
    } else if (strcasecmp(name, "SET_INTERVAL") == 0) {
        if (param == NULL || extra != NULL) {
            snprintf(result, size, "ERROR SET_INTERVAL necesita un parametru: SET_INTERVAL <ms>");
            return;
        }
        char* end = NULL;
        long ms = strtol(param, &end, 10);
        if (end == param || *end != '\0') {
            snprintf(result, size, "ERROR SET_INTERVAL: '%s' nu este un numar intreg", param);
        } else if (ms < (long)PERIOD_MS_MIN || ms > (long)PERIOD_MS_MAX) {
            snprintf(result, size, "ERROR SET_INTERVAL: perioada trebuie sa fie intre %lu si %lu ms",
                     (unsigned long)PERIOD_MS_MIN, (unsigned long)PERIOD_MS_MAX);
        } else {
            g_period_ms = (uint32_t)ms;
            snprintf(result, size, "OK SET_INTERVAL %ld", ms);
        }
    } else if (strcasecmp(name, "SET_THRESHOLD") == 0) {
        if (param == NULL || extra != NULL) {
            snprintf(result, size, "ERROR SET_THRESHOLD necesita un parametru: SET_THRESHOLD <valoare>");
            return;
        }
        char* end = NULL;
        float value = strtof(param, &end);
        if (end == param || *end != '\0' || isnan(value) || isinf(value)) {
            snprintf(result, size, "ERROR SET_THRESHOLD: '%s' nu este un numar valid", param);
        } else {
            g_threshold = value;
            snprintf(result, size, "OK SET_THRESHOLD %.2f", value);
        }
    } else if (strcasecmp(name, "RESET") == 0) {
        if (param != NULL) {
            snprintf(result, size, "ERROR RESET nu are parametri");
            return;
        }
        // senzorul intern nu se poate reseta: readucem perioada și pragul la valorile inițiale
        g_period_ms = PERIOD_MS_INITIAL;
        g_threshold = THRESHOLD_INITIAL;
        snprintf(result, size, "OK RESET interval=%lu threshold=%.2f", (unsigned long)PERIOD_MS_INITIAL,
                 THRESHOLD_INITIAL);
    } else if (strcasecmp(name, "STATUS") == 0) {
        if (param != NULL) {
            snprintf(result, size, "ERROR STATUS nu are parametri");
            return;
        }
        snprintf(result, size, "OK STATUS interval=%lu threshold=%.2f", (unsigned long)g_period_ms,
                 (float)g_threshold);
    } else {
        snprintf(result, size, "ERROR comanda necunoscuta: %s", name);
    }
}

// Fiecare comandă din coadă eliberează un job; joburile se execută în ordinea
// sosirii. O comandă greșită produce "ERROR ..." și taskul continuă.
static void aperiodic_task(void* arg) {
    (void)arg;
    for (;;) {
        command_t cmd;
        if (xQueueReceive(g_command_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint32_t release = millis();

        char original[COMMAND_MAX_LEN];
        char* command = trim(cmd.text);
        strncpy(original, command, sizeof(original) - 1);  // pentru afișare: strtok_r modifică textul
        original[sizeof(original) - 1] = '\0';

        char result[96];
        execute_command(command, result, sizeof(result));

        char msg[160];
        snprintf(msg, sizeof(msg), "%s;%lld;%s", DEVICE_ID, (long long)epoch_ms(), result);
        mqtt_publish("status", msg);
        Serial.printf("[aperiodic] '%s' -> %s  (R = %lu ms)\n", original, result,
                      (unsigned long)(millis() - release));
    }
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
// Rulează pe taskul clientului MQTT: doar copiem comanda în coadă, fără să ne
// blocăm (timeout 0). Execuția este jobul taskului aperiodic.
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    if (g_command_queue == NULL) {
        Serial.println("[mqtt] coada de comenzi nu există încă, comandă ignorată");
        return;
    }
    command_t cmd;
    strncpy(cmd.text, text, COMMAND_MAX_LEN - 1);
    cmd.text[COMMAND_MAX_LEN - 1] = '\0';
    if (xQueueSend(g_command_queue, &cmd, 0) != pdTRUE) {
        Serial.println("[mqtt] coada de comenzi este plină, comandă pierdută");
    }
}

// ---------------------------------------------------------------- TODO 0: structura aplicației

static void start_task(TaskFunction_t fn, const char* name, UBaseType_t prio) {
    if (xTaskCreate(fn, name, TASK_STACK, NULL, prio, NULL) != pdPASS) {
        Serial.printf("EROARE: taskul %s nu a putut fi creat (memorie insuficientă)\n", name);
    }
}

void setup() {
    Serial.begin(115200);
    // Fără așteptare la scriere: cu placa la USB, dar fără Serial Monitor deschis,
    // fiecare Serial.printf() ar aștepta până la 2 s să fie citit și ar întârzia
    // taskul care scrie. Așa, textul pe care nu-l citește nimeni se pierde.
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);  // așteptăm Serial Monitor, cel mult 3 s
    delay(200);
    Serial.println();
    Serial.printf("=== tema 02: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    wifi_connect();
    time_sync();

    // TODO 0 (a): cozile, înainte de mqtt_start(): de acolo încolo pot sosi
    // comenzi, iar command_received() le pune în coada de comenzi.
    g_alarm_queue = xQueueCreate(ALARM_QUEUE_LEN, sizeof(alarm_event_t));
    g_command_queue = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(command_t));
    if (g_alarm_queue == NULL || g_command_queue == NULL) {
        Serial.println("EROARE: cozile nu au putut fi create (memorie insuficientă)");
    }

    mqtt_start();

    // TODO 0 (b): taskurile, după mqtt_start(), ca primele mesaje publicate să
    // nu se piardă. Consumatorii (sporadic, aperiodic) pornesc primii, ca să
    // aștepte deja la cozile lor când apare primul eveniment.
    start_task(aperiodic_task, "aperiodic", PRIO_APERIODIC);
    start_task(sporadic_task, "sporadic", PRIO_SPORADIC);
    start_task(periodic_task, "periodic", PRIO_PERIODIC);

    Serial.printf("taskuri pornite: periodic (prio %u, T=%lu ms), sporadic (prio %u, min %lu ms), aperiodic (prio %u)\n",
                  (unsigned)PRIO_PERIODIC, (unsigned long)g_period_ms, (unsigned)PRIO_SPORADIC,
                  (unsigned long)MIN_INTERARRIVAL_MS, (unsigned)PRIO_APERIODIC);
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}
