package ssatr.service;

import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * Ultima stare cunoscută a fiecărui device: ultima temperatură, momentul
 * ultimului mesaj, numărul de alarme etc.
 *
 * Este actualizată de thread-urile MessageProcessor și citită de CommandConsole,
 * deci accesul trebuie sincronizat: toate metodele publice sunt synchronized
 * pe același obiect, așa că print() vede o stare consistentă.
 */
public class Dashboard {

    /** Starea unui device. Modificată doar din metodele synchronized ale Dashboard. */
    private static class DeviceState {
        double lastTemperature = Double.NaN;
        long lastMessageMillis;
        long telemetryCount;
        long alarmCount;
        double lastAlarmTemperature = Double.NaN;
        String lastStatus = "-";
    }


    private final Map<String, DeviceState> devices = new TreeMap<>();

    private DeviceState state(String deviceId) {
        DeviceState s = devices.computeIfAbsent(deviceId, id -> new DeviceState());
        s.lastMessageMillis = System.currentTimeMillis();
        return s;
    }

    public synchronized void updateTelemetry(String deviceId, double temperature) {
        DeviceState s = state(deviceId);
        s.lastTemperature = temperature;
        s.telemetryCount++;
    }

    public synchronized void updateAlarm(String deviceId, double temperature) {
        DeviceState s = state(deviceId);
        s.alarmCount++;
        s.lastAlarmTemperature = temperature;
    }

    public synchronized void updateStatus(String deviceId, String text) {
        state(deviceId).lastStatus = text;
    }


    public synchronized void print() {
        if (devices.isEmpty()) {
            System.out.println();
            return;
        }
        long now = System.currentTimeMillis();
        String format = "%-12s %8s %15s %8s %8s  %s%n";
        System.out.printf(format, "DEVICE", "TEMP", "ULTIMUL MESAJ", "MESAJE", "ALARME", "ULTIMUL STATUS");
        for (Map.Entry<String, DeviceState> e : devices.entrySet()) {
            DeviceState s = e.getValue();
            String temp = Double.isNaN(s.lastTemperature) ? "-" : String.format(Locale.US, "%.2f", s.lastTemperature);
            String age = "acum " + formatAge(now - s.lastMessageMillis);
            System.out.printf(format, e.getKey(), temp, age, s.telemetryCount, s.alarmCount, s.lastStatus);
        }
    }

    private static String formatAge(long millis) {
        long seconds = Math.max(0, millis / 1000);
        if (seconds < 60) {
            return seconds + " s";
        }
        if (seconds < 3600) {
            return (seconds / 60) + " min " + (seconds % 60) + " s";
        }
        return (seconds / 3600) + " h " + (seconds % 3600 / 60) + " min";
    }
}
