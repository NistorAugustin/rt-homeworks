package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * TASK SPORADIC: tratează alarmele de temperatură.
 *
 * Evenimentele sosesc în coada alarmEvents (puse de taskul periodic).
 * Intervalul minim între sosiri este MIN_INTERARRIVAL_MS: între două eliberări
 * ale taskului (două alarme publicate) trebuie să treacă cel puțin atât.
 * Intervalul este impus de cod: evenimentele care sosesc mai des sunt
 * respinse, ca să nu fie inundat brokerul.
 */

public class SporadicAlarmTask implements Runnable {

    static final long MIN_INTERARRIVAL_MS = 5000;

    private final String deviceId;
    private final MqttConnection mqtt;
    private final BlockingQueue<AlarmEvent> alarmEvents;

    public SporadicAlarmTask(String deviceId, MqttConnection mqtt, BlockingQueue<AlarmEvent> alarmEvents) {
        this.deviceId = deviceId;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
    }

    @Override
    public void run() {
        long minInterarrivalNanos = MIN_INTERARRIVAL_MS * 1_000_000L;
        long lastRelease = 0;
        boolean released = false;   // încă nu a existat nicio eliberare

        while (!Thread.currentThread().isInterrupted()) {
            AlarmEvent event;
            try {
                event = alarmEvents.take();          // sosirea unui eveniment
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }

            long arrival = System.nanoTime();
            long sinceLastMs = (arrival - lastRelease) / 1_000_000;

            if (released && arrival - lastRelease < minInterarrivalNanos) {
                // prea devreme: se încalcă intervalul minim între sosiri -> respins
                System.out.printf(Locale.US,
                        "[sporadic] RESPINS: %.2f > %.2f, la doar %d ms de ultima alarmă (minim %d ms)%n",
                        event.temperature(), event.threshold(), sinceLastMs, MIN_INTERARRIVAL_MS);
                continue;
            }

            // eliberare acceptată: jobul publică alarma
            lastRelease = arrival;
            released = true;

            String message = deviceId + ";" + event.timestamp() + ";"
                    + String.format(Locale.US, "%.2f;%.2f", event.temperature(), event.threshold());
            mqtt.publish(Config.alarmTopic(deviceId), message);

            long responseMs = (System.nanoTime() - arrival) / 1_000_000;
            System.out.println("[sporadic] ALARMĂ publicată: " + message + "  (R = " + responseMs + " ms)");
        }
        System.out.println("[sporadic] oprit");
    }
}
