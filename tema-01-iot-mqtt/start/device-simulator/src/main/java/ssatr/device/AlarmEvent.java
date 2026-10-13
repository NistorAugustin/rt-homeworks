package ssatr.device;

/**
 * Evenimentul pus de taskul periodic în coada de alarme.
 *
 * Conține atât temperatura măsurată, cât și pragul care era activ în momentul
 * măsurării. Astfel alarma publicată descrie exact situația care a declanșat-o,
 * chiar dacă între timp pragul a fost schimbat (comanda SET_THRESHOLD).
 *
 * @param timestamp   momentul măsurării (System.currentTimeMillis())
 * @param temperature temperatura măsurată
 * @param threshold   pragul activ la momentul măsurării
 */
public record AlarmEvent(long timestamp, double temperature, double threshold) {
}
