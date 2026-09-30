package com.igg.lordsbot.panel

/**
 * Thin JNI surface onto libbot.so. The engine is the existing C bot; the
 * panel hosts it rather than reimplementing it.
 */
object NativeBridge {

    init {
        System.loadLibrary("bot")
    }

    /** Receives one line of bot stdout/stderr. Called from a native thread. */
    interface LogSink {
        fun onLine(line: String)
    }

    @Volatile
    var sink: LogSink? = null

    @JvmStatic
    @Synchronized
    fun start(configPath: String) {
        val s = object : LogSink {
            override fun onLine(line: String) {
                sink?.onLine(line)
            }
        }
        nativeStart(configPath, s)
    }

    @JvmStatic
    @Synchronized
    fun stop() = nativeStop()

    @JvmStatic
    fun isRunning(): Boolean = nativeIsRunning()

    // @JvmStatic is load-bearing, not decoration: without it these are
    // instance methods on the singleton and the JNI layer would receive a
    // jobject where it declares a jclass.
    @JvmStatic private external fun nativeStart(configPath: String, sink: LogSink)
    @JvmStatic private external fun nativeStop()
    @JvmStatic private external fun nativeIsRunning(): Boolean
}
