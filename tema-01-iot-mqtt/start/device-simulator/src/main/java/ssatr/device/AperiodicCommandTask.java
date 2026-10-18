package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * TASK APERIODIC: execută comenzile primite de la business service.
 *
 * Comenzile sosesc oricând, pe MQTT, fără un interval minim între ele, și sunt
 * puse în coada commands. Fiecare comandă eliberează un job al taskului.
 * Comenzi care trebuie suportate:
 *
 *   SET_INTERVAL <ms>       schimbă perioada T a taskului periodic
 *   SET_THRESHOLD <valoare> schimbă pragul de alarmă
 *   RESET                   readuce senzorul la valoarea inițială
 *   STATUS                  raportează perioada și pragul curente
 *
 * După executarea fiecărei comenzi, device-ul publică pe Config.statusTopic(deviceId)
 * o confirmare: "deviceId;timestamp;text", de exemplu
 * "device-1;1727600000000;OK SET_INTERVAL 1000".
 */
public class AperiodicCommandTask implements Runnable {

    /** limite rezonabile pentru perioadă, ca o comandă greșită să nu blocheze device-ul */
    static final long MIN_PERIOD_MS = 100;
    static final long MAX_PERIOD_MS = 60_000;

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<String> commands;
    private final PeriodicTelemetryTask periodic;

    public AperiodicCommandTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                BlockingQueue<String> commands, PeriodicTelemetryTask periodic) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.commands = commands;
        this.periodic = periodic;
    }

    @Override
    public void run() {
        while (!Thread.currentThread().isInterrupted()) {
            String command;
            try {
                command = commands.take();           // eliberarea unui job (ordinea sosirii)
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }

            long release = System.nanoTime();
            String result;
            try {
                result = execute(command.trim());
            } catch (Exception e) {
                // orice problemă devine ERROR, taskul continuă
                result = "ERROR " + e.getMessage();
            }

            String message = deviceId + ";" + System.currentTimeMillis() + ";" + result;
            mqtt.publish(Config.statusTopic(deviceId), message);

            long responseMs = (System.nanoTime() - release) / 1_000_000;
            System.out.println("[aperiodic] '" + command + "' -> " + result + "  (R = " + responseMs + " ms)");
        }
        System.out.println("[aperiodic] oprit");
    }

    /** Execută o comandă și întoarce textul confirmării ("OK ..." sau "ERROR ..."). */
    private String execute(String command) {
        if (command.isEmpty()) {
            return "ERROR comanda goala";
        }
        String[] parts = command.split("\\s+");
        String name = parts[0].toUpperCase(Locale.ROOT);

        switch (name) {
            case "SET_INTERVAL": {
                if (parts.length != 2) {
                    return "ERROR SET_INTERVAL necesita un parametru: SET_INTERVAL <ms>";
                }
                long ms;
                try {
                    ms = Long.parseLong(parts[1]);
                } catch (NumberFormatException e) {
                    return "ERROR SET_INTERVAL: '" + parts[1] + "' nu este un numar intreg";
                }
                if (ms < MIN_PERIOD_MS || ms > MAX_PERIOD_MS) {
                    return "ERROR SET_INTERVAL: perioada trebuie sa fie intre "
                            + MIN_PERIOD_MS + " si " + MAX_PERIOD_MS + " ms";
                }
                periodic.setPeriodMs(ms);
                return "OK SET_INTERVAL " + ms;
            }
            case "SET_THRESHOLD": {
                if (parts.length != 2) {
                    return "ERROR SET_THRESHOLD necesita un parametru: SET_THRESHOLD <valoare>";
                }
                double value;
                try {
                    value = Double.parseDouble(parts[1]);
                } catch (NumberFormatException e) {
                    return "ERROR SET_THRESHOLD: '" + parts[1] + "' nu este un numar";
                }
                if (Double.isNaN(value) || Double.isInfinite(value)) {
                    return "ERROR SET_THRESHOLD: valoare invalida";
                }
                periodic.setThreshold(value);
                return "OK SET_THRESHOLD " + String.format(Locale.US, "%.2f", value);
            }
            case "RESET": {
                if (parts.length != 1) {
                    return "ERROR RESET nu are parametri";
                }
                sensor.reset();
                return "OK RESET";
            }
            case "STATUS": {
                if (parts.length != 1) {
                    return "ERROR STATUS nu are parametri";
                }
                return String.format(Locale.US, "OK STATUS interval=%d threshold=%.2f",
                        periodic.getPeriodMs(), periodic.getThreshold());
            }
            default:
                return "ERROR comanda necunoscuta: " + parts[0];
        }
    }
}
