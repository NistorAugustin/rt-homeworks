package ssatr.service;

import java.util.Scanner;

/**
 * Consola operatorului: citește comenzi de la tastatură.
 *
 *   show                               afișează dashboard-ul
 *   <deviceId> <COMANDA> [parametru]   trimite o comandă unui device, de exemplu
 *                                      device-1 SET_INTERVAL 1000
 *   help                               afișează comenzile
 *   exit                               oprește serviciul
 */
public class CommandConsole implements Runnable {

    private final MqttConnection mqtt;
    private final Dashboard dashboard;

    public CommandConsole(MqttConnection mqtt, Dashboard dashboard) {
        this.mqtt = mqtt;
        this.dashboard = dashboard;
    }

    @Override
    public void run() {
        Scanner scanner = new Scanner(System.in);
        printHelp();

        while (scanner.hasNextLine()) {
            String line = scanner.nextLine().trim();

            if (line.isEmpty()) {
                continue;
            } else if (line.equals("exit")) {
                break;
            } else if (line.equals("help")) {
                printHelp();
            } else if (line.equals("show")) {
                dashboard.print();
            } else {
                sendCommand(line);
            }
        }
    }

    private void sendCommand(String line) {

        String[] parts = line.split("\\s+", 2);
        if (parts.length < 2) {
            System.out.println();
            return;
        }
        String deviceId = parts[0];
        String command = parts[1].trim();


        if (deviceId.contains("/") || deviceId.contains("+") || deviceId.contains("#")) {
            System.out.println("deviceId invalid: " + deviceId);
            return;
        }

        String name = command.split("\\s+")[0].toUpperCase();
        if (!KNOWN_COMMANDS.contains(name)) {
            System.out.println();
        }

        mqtt.publish(Config.commandTopic(deviceId), command);
        System.out.println("-> " + deviceId + ": " + command);
    }

    private static final java.util.Set<String> KNOWN_COMMANDS =
            java.util.Set.of("SET_INTERVAL", "SET_THRESHOLD", "RESET", "STATUS");

    private void printHelp() {
        System.out.println();
        System.out.println();
    }
}
