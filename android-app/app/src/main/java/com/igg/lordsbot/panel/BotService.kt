package com.igg.lordsbot.panel

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import androidx.core.app.NotificationCompat
import java.io.File

/**
 * Foreground service owning the engine's lifetime.
 *
 * Foreground rather than background because the bot's whole value is that
 * it keeps running while the user is elsewhere; a background service gets
 * killed within minutes of leaving the app.
 */
class BotService : Service(), NativeBridge.LogSink {

    companion object {
        const val ACTION_START = "com.igg.lordsbot.panel.START"
        const val ACTION_STOP = "com.igg.lordsbot.panel.STOP"
        const val CHANNEL_ID = "bot_engine"
        const val NOTIF_ID = 1001
        const val EXTRA_ACCOUNT_ID = "accountId"
        const val EXTRA_CONFIG_PATH = "configPath"

        fun start(ctx: Context, accountId: String, configPath: String) {
            ctx.startForegroundService(Intent(ctx, BotService::class.java).apply {
                action = ACTION_START
                putExtra(EXTRA_ACCOUNT_ID, accountId)
                putExtra(EXTRA_CONFIG_PATH, configPath)
            })
        }

        fun stop(ctx: Context) {
            ctx.startService(Intent(ctx, BotService::class.java).apply {
                action = ACTION_STOP
            })
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            shutdown()
            return START_NOT_STICKY
        }

        createChannel()
        startForeground(NOTIF_ID, buildNotification("Starting engine\u2026"))

        val cfg = intent?.getStringExtra(EXTRA_CONFIG_PATH)
        if (cfg == null || !File(cfg).exists()) {
            LogBus.emit("[panel] no config at $cfg - nothing started")
            stopSelf()
            return START_NOT_STICKY
        }

        NativeBridge.sink = this
        Thread({
            try {
                NativeBridge.start(cfg)
            } catch (t: Throwable) {
                LogBus.emit("[panel] engine failed: ${t.message}")
                shutdown()
            }
        }, "bot-engine").start()

        return START_STICKY
    }

    override fun onLine(line: String) {
        LogBus.emit(line)
        updateNotification(line)
    }

    private fun shutdown() {
        try {
            NativeBridge.stop()
        } catch (t: Throwable) {
            LogBus.emit("[panel] stop error: ${t.message}")
        }
        NativeBridge.sink = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        try { NativeBridge.stop() } catch (_: Throwable) {}
        NativeBridge.sink = null
        super.onDestroy()
    }

    private fun createChannel() {
        val mgr = getSystemService(NotificationManager::class.java)
        if (mgr.getNotificationChannel(CHANNEL_ID) != null) return
        mgr.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Bot engine", NotificationManager.IMPORTANCE_LOW)
                .apply { description = "Shows the running bot session" }
        )
    }

    private fun buildNotification(text: String): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Lords bot engine")
            .setContentText(text.take(120))
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setOngoing(true)
            .setContentIntent(open)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    private fun updateNotification(line: String) {
        getSystemService(NotificationManager::class.java).notify(NOTIF_ID, buildNotification(line))
    }
}

/** Process-wide log fan-out so Compose can observe engine output. */
object LogBus {
    private val lines = ArrayDeque<String>()
    private val listeners = mutableListOf<(String) -> Unit>()

    @Synchronized
    fun emit(line: String) {
        lines.addLast(line)
        while (lines.size > 2000) lines.removeFirst()
        listeners.toList().forEach { it(line) }
    }

    @Synchronized
    fun snapshot(): List<String> = lines.toList()

    @Synchronized
    fun clear() { lines.clear() }

    @Synchronized
    fun addListener(l: (String) -> Unit) { listeners.add(l) }

    @Synchronized
    fun removeListener(l: (String) -> Unit) { listeners.remove(l) }
}
