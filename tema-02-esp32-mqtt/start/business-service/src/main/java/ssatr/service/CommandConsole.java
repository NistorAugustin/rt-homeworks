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
        // "device-1 SET_INTERVAL 1000" -> "device-1" și "SET_INTERVAL 1000"
        String[] parts = line.split("\\s+", 2);
        if (parts.length < 2) {
            System.out.println("Format greșit. Folosiți: <deviceId> <COMANDA> [parametru], de ex. device-1 SET_INTERVAL 1000");
            return;
        }
        String deviceId = parts[0];
        String command = parts[1].trim();

        // deviceId devine nivel de topic: nu poate conține wildcard-uri sau "/"
        if (deviceId.contains("/") || deviceId.contains("+") || deviceId.contains("#")) {
            System.out.println("deviceId invalid: " + deviceId);
            return;
        }
        // comanda se verifică doar superficial; validarea completă o face device-ul,
        // care răspunde cu ERROR pe topic-ul status
        String name = command.split("\\s+")[0].toUpperCase();
        if (!KNOWN_COMMANDS.contains(name)) {
            System.out.println("Atenție: comanda " + name + " nu este cunoscută; o trimit oricum, device-ul va răspunde cu ERROR.");
        }

        mqtt.publish(Config.commandTopic(deviceId), command);
        System.out.println("-> " + deviceId + ": " + command);
    }

    private static final java.util.Set<String> KNOWN_COMMANDS =
            java.util.Set.of("SET_INTERVAL", "SET_THRESHOLD", "RESET", "STATUS");

    private void printHelp() {
        System.out.println("Comenzi: show | <deviceId> SET_INTERVAL <ms> | <deviceId> SET_THRESHOLD <valoare>");
        System.out.println("         <deviceId> RESET | <deviceId> STATUS | help | exit");
    }
}
