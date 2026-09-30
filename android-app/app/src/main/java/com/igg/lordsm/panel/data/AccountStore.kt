package com.igg.lordsm.panel.data

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

/**
 * Accounts are stored as a JSON array in SharedPreferences. Deliberately
 * not a database: the dataset is a handful of rows that the user edits by
 * hand, and a JSON blob keeps the whole store readable in one adb pull
 * when something goes wrong.
 *
 * Access keys are stored in app-private storage. There is no obfuscation
 * theatre here - on a rooted device any app-private file is readable, and
 * pretending otherwise would only hide the fact.
 */
class AccountStore(context: Context) {

    private val prefs = context.getSharedPreferences("accounts", Context.MODE_PRIVATE)

    fun load(): MutableList<Account> {
        val raw = prefs.getString(KEY, null) ?: return mutableListOf()
        return try {
            val arr = JSONArray(raw)
            (0 until arr.length()).map { accountFrom(arr.getJSONObject(it)) }.toMutableList()
        } catch (e: Exception) {
            mutableListOf()
        }
    }

    fun save(accounts: List<Account>) {
        val arr = JSONArray()
        accounts.forEach { arr.put(accountTo(it)) }
        prefs.edit().putString(KEY, arr.toString()).apply()
    }

    private fun readValues(jo: JSONObject?): MutableMap<String, String> {
        if (jo == null) return mutableMapOf()
        val out = mutableMapOf<String, String>()
        val keys = jo.keys()
        while (keys.hasNext()) {
            val k = keys.next()
            out[k] = jo.optString(k)
        }
        return out
    }

    private fun accountTo(a: Account) = JSONObject().apply {
        put("id", a.id)
        put("label", a.label)
        put("gatewayAddr", a.gatewayAddr)
        put("gatewayPort", a.gatewayPort)
        put("iggId", a.iggId)
        put("deviceUuid", a.deviceUuid)
        put("accessKey", a.accessKey)
        put("adminName", a.adminName)
        put("ultraSafe", a.ultraSafe)
        put("values", JSONObject(a.values))
    }

    private fun accountFrom(o: JSONObject) = Account(
        id = o.optString("id"),
        label = o.optString("label", "Account"),
        gatewayAddr = o.optString("gatewayAddr", "192.243.44.63"),
        gatewayPort = o.optString("gatewayPort", "5999"),
        iggId = o.optString("iggId"),
        deviceUuid = o.optString("deviceUuid"),
        accessKey = o.optString("accessKey"),
        adminName = o.optString("adminName"),
        values = readValues(o.optJSONObject("values")),
        ultraSafe = o.optBoolean("ultraSafe", false),
    )

    companion object {
        private const val KEY = "list"
        fun newId(): String = java.util.UUID.randomUUID().toString().take(8)
    }
}
