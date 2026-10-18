package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * TASK PERIODIC, cu perioada T = periodMs: fiecare job citește senzorul și
 * publică valoarea pe topic-ul de telemetrie.
 *
 * Dacă temperatura depășește pragul, taskul NU tratează el alarma, ci doar
 * semnalează un eveniment în coada alarmEvents. Alarma este tratată de
 * taskul sporadic.
 *
 * Eliberările sunt pe grila r_k = r_(k-1) + T, calculată cu un timp monoton
 * (System.nanoTime()). Taskul așteaptă absolut până la următoarea eliberare,
 * deci timpul de calcul C al jobului nu se adună la perioadă (fără derivă).
 */
public class PeriodicTelemetryTask implements Runnable {

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<AlarmEvent> alarmEvents;

    // volatile: sunt citite aici, dar modificate de taskul aperiodic (alt thread)
    private volatile long periodMs = 2000;
    private volatile double threshold = 28.0;

    public PeriodicTelemetryTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                 BlockingQueue<AlarmEvent> alarmEvents) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
    }

    @Override
    public void run() {
        // prima eliberare: acum (r_0)
        long nextRelease = System.nanoTime();
        long jobIndex = 0;

        while (!Thread.currentThread().isInterrupted()) {
            // ---------- jobul k ----------
            long start = System.nanoTime();
            double lateMs = (start - nextRelease) / 1_000_000.0; // întârzierea față de grilă

            double temperature = sensor.read();
            long timestamp = System.currentTimeMillis();
            String message = deviceId + ";" + timestamp + ";" + String.format(Locale.US, "%.2f", temperature);
            mqtt.publish(Config.telemetryTopic(deviceId), message);

            double currentThreshold = threshold;   // citim o singură dată (volatile)
            boolean overThreshold = temperature > currentThreshold;
            if (overThreshold) {
                alarmEvents.offer(new AlarmEvent(timestamp, temperature, currentThreshold));
            }

            System.out.printf(Locale.US, "[periodic] job %d  întârziere %.2f ms  T=%d ms  %s%s%n",
                    jobIndex, lateMs, periodMs, message, overThreshold ? "  > prag!" : "");

            long period = TimeUnit.MILLISECONDS.toNanos(periodMs);
            nextRelease += period;

            long now = System.nanoTime();
            if (now - nextRelease > 0) {
                long missed = (now - nextRelease) / period + 1;
                System.out.println("[periodic] overrun: " + missed + " eliberări ratate");
                nextRelease += missed * period;
            }

            if (!sleepUntil(nextRelease)) {
                break;
            }
            jobIndex++;
        }
        System.out.println("[periodic] oprit");
    }

    private static boolean sleepUntil(long deadlineNanos) {
        long remaining;
        while ((remaining = deadlineNanos - System.nanoTime()) > 0) {
            try {
                TimeUnit.NANOSECONDS.sleep(remaining);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        return true;
    }

    public void setPeriodMs(long periodMs) {
        this.periodMs = periodMs;
    }

    public void setThreshold(double threshold) {
        this.threshold = threshold;
    }

    public long getPeriodMs() {
        return periodMs;
    }

    public double getThreshold() {
        return threshold;
    }
}
