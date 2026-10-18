package ssatr.service;

import java.util.concurrent.BlockingQueue;

/**
 * Preia mesajele de un anumit tip ("telemetry", "alarm" sau "status") din coada
 * inbox, le salvează și actualizează dashboard-ul.
 *
 * Formatul mesajelor (câmpuri separate prin ";"):
 *   telemetry:  deviceId;timestamp;temperatura
 *   alarm:      deviceId;timestamp;temperatura;prag
 *   status:     deviceId;timestamp;text
 */
public class MessageProcessor implements Runnable {

    private final String kind;
    private final BlockingQueue<String> inbox;
    private final CsvStorage storage;
    private final Dashboard dashboard;

    public MessageProcessor(String kind, BlockingQueue<String> inbox, CsvStorage storage, Dashboard dashboard) {
        this.kind = kind;
        this.inbox = inbox;
        this.storage = storage;
        this.dashboard = dashboard;
    }

    @Override
    public void run() {
        while (!Thread.currentThread().isInterrupted()) {
            String message;
            try {
                message = inbox.take();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }

            // fiecare mesaj se salvează exact cum a sosit, chiar și unul invalid
            storage.append(kind, message);

            try {
                process(message.trim());
            } catch (RuntimeException e) {
                // un mesaj greșit nu oprește thread-ul
                System.out.println("[" + kind + "] mesaj invalid ignorat: '" + message + "' (" + e.getMessage() + ")");
            }
        }
    }

    private void process(String message) {
        switch (kind) {
            case "telemetry": {
                String[] f = fields(message, 3);
                Long.parseLong(f[1]);                       // validăm timestamp-ul
                dashboard.updateTelemetry(f[0], Double.parseDouble(f[2]));
                break;
            }
            case "alarm": {
                String[] f = fields(message, 4);
                Long.parseLong(f[1]);
                double temperature = Double.parseDouble(f[2]);
                double threshold = Double.parseDouble(f[3]);
                dashboard.updateAlarm(f[0], temperature);
                System.out.printf("%n*** ALARMĂ %s: temperatura %s > prag %s ***%n", f[0], f[2], f[3]);
                break;
            }
            case "status": {
                String[] f = message.split(";", 3);
                if (f.length != 3 || f[0].isBlank()) {
                    throw new IllegalArgumentException("se așteaptă deviceId;timestamp;text");
                }
                Long.parseLong(f[1]);
                dashboard.updateStatus(f[0], f[2]);
                System.out.printf("%n[status] %s: %s%n", f[0], f[2]);
                break;
            }
            default:
                throw new IllegalStateException("tip de mesaj necunoscut: " + kind);
        }
    }

    private static String[] fields(String message, int count) {
        String[] f = message.split(";");
        if (f.length != count || f[0].isBlank()) {
            throw new IllegalArgumentException("se așteaptă " + count + " câmpuri separate prin ';'");
        }
        return f;
    }
}
