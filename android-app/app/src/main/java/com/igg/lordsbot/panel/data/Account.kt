package com.igg.lordsbot.panel.data

/**
 * One bot profile. Credentials live here and nowhere else; the config file
 * written for the native engine is generated from this on every start, so
 * the two can never drift.
 */
data class Account(
    val id: String,
    var label: String = "Account",
    var gatewayAddr: String = "192.243.44.63",
    var gatewayPort: String = "5999",
    var iggId: String = "",
    var deviceUuid: String = "",
    var accessKey: String = "",
    var adminName: String = "",
    var values: MutableMap<String, String> = mutableMapOf(),
    var ultraSafe: Boolean = false,
) {
    val hasCredential: Boolean get() = iggId.isNotBlank() && accessKey.isNotBlank()

    /** Access keys are long; show enough to recognise, not enough to leak. */
    fun maskedKey(): String =
        if (accessKey.length <= 12) accessKey
        else accessKey.take(8) + "\u2026" + accessKey.takeLast(4) +
            " (" + accessKey.length + " chars)"
}
