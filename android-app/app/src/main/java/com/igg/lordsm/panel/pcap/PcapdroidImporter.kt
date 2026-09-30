package com.igg.lordsm.panel.pcap

/**
 * Imports a PCAPdroid text export and pulls the game credential out of it.
 *
 * The credential is a three-part token:
 *
 *     eyJ... . <signature> . <checksum>
 *
 * where the first segment is base64url JSON holding akid / kmd / g / v.
 * Rather than reproducing the offset arithmetic that the offline decoder
 * used - offsets into a payload whose non-UTF8 bytes PCAPdroid has already
 * replaced - this matches the token itself in the export text. That is
 * robust to PCAPdroid changing how it renders unprintable bytes, which is
 * exactly what made the earlier byte-offset decoder brittle.
 */
object PcapdroidImporter {

    data class Credential(
        val accessKey: String,
        val akid: String,
        val kmd: String,
        val gid: String,
        val version: Int,
        val rawSegment: String,
    )

    sealed class Result {
        data class Ok(val credential: Credential, val sourceName: String) : Result()
        data class Bad(val reason: String) : Result()
    }

    // base64url segments; the middle signature is long and mixed case.
    private val TOKEN = Regex(
        "eyJ[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{20,}\\.[A-Za-z0-9_-]{20,}"
    )

    /** Accepts either a raw export or a hex dump; both are handled. */
    fun parse(text: String, sourceName: String): Result {
        if (text.isBlank()) return Result.Bad("File is empty")

        val match = TOKEN.find(text)
            ?: return Result.Bad(
                "No access key found. Export the Lords Mobile login exchange " +
                    "from PCAPdroid (Text export) and pick that file."
            )

        val token = match.value
        val header = token.substringBefore('.')
        val json = runCatching {
            String(
                android.util.Base64.decode(
                    pad(header), android.util.Base64.URL_SAFE or android.util.Base64.NO_WRAP
                )
            )
        }.getOrElse { return Result.Bad("Key header is not valid base64: ${it.message}") }

        val obj = runCatching {
            org.json.JSONObject(json)
        }.getOrElse {
            return Result.Bad("Key header decoded but was not JSON - not a Lords key")
        }

        val version = obj.optInt("v", 0)
        if (version == 0)
            return Result.Bad("Key has no version field; v=${obj.opt("v")}")

        return Result.Ok(
            Credential(
                accessKey = token,
                akid = obj.optString("akid"),
                kmd = obj.optString("kmd"),
                gid = obj.optString("g"),
                version = version,
                rawSegment = header,
            ),
            sourceName
        )
    }

    private fun pad(s: String): String = when (s.length % 4) {
        2 -> s + "=="
        3 -> s + "="
        0 -> s
        else -> s
    }

    /** Human summary for the import screen. */
    fun describe(c: Credential): String = buildString {
        appendLine("version   v${c.version}")
        appendLine("akid      ${c.akid}")
        appendLine("kmd       ${c.kmd}")
        appendLine("gid       ${c.gid}")
        appendLine("key       ${c.accessKey.length} chars")
    }
}
