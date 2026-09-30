/*
 * JNI bridge between the Android panel app and the C bot.
 *
 * Design notes, because two of these choices are load-bearing:
 *
 * 1. We do NOT fork/exec the bot. Termux is not guaranteed to be present,
 *    and the whole point of bundling the engine as a libbot.so is that the
 *    APK is self-contained. So the bot's own main() is called directly on
 *    a pthread with a synthesised argv. That means the bot's source needs
 *    zero modification to be embeddable, apart from the cooperative exit
 *    hook in main.c.
 *
 * 2. Log capture works by redirecting fd 1 and fd 2 into a pipe and
 *    draining it on a second thread. Every existing LOGI/printf in the
 *    bot keeps working untouched - we are not adding a logging abstraction
 *    to 20k lines of C just to get lines into Java. The pipe buffer is
 *    drained continuously by the reader thread, so a chatty run cannot
 *    deadlock on a full pipe.
 */

#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "connection.h"

/* Defined in main.c. */
extern int main(int argc, const char *argv[]);
extern void bot_request_exit(void);

static pthread_t       g_bot_thread;
static pthread_t       g_log_thread;
static int             g_bot_running = 0;
static int             g_log_pipe[2] = { -1, -1 };
static int             g_saved_stdout = -1;
static int             g_saved_stderr = -1;

/* Callback plumbing into Java. */
static JavaVM  *g_vm = NULL;
static int      g_log_thread_started = 0;
static jobject g_listener = NULL;   /* global ref */
static jmethodID g_on_line = NULL;

static void emit_line(const char *s) {
    if (!g_vm || !g_listener || !g_on_line)
        return;
    JNIEnv *env = NULL;
    int attached = 0;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, (void **)&env, NULL) != JNI_OK)
            return;
        attached = 1;
    }
    jstring js = (*env)->NewStringUTF(env, s);
    if (js) {
        (*env)->CallVoidMethod(env, g_listener, g_on_line, js);
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env, NULL);
        (*env)->DeleteLocalRef(env, js);
    }
    if (attached)
        (*g_vm)->DetachCurrentThread(g_vm);
}

/* Drain stdout+stderr, split on newlines, forward each line to Java. */
static void *log_reader(void *arg) {
    (void)arg;
    char buf[1024];
    char line[4096];
    size_t used = 0;

    for (;;) {
        ssize_t n = read(g_log_pipe[0], buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[used] = '\0';
                emit_line(line);
                used = 0;
            } else if (used < sizeof(line) - 1) {
                line[used++] = buf[i];
            }
        }
    }
    if (used) {
        line[used] = '\0';
        emit_line(line);
    }
    return NULL;
}

static int start_log_capture(JNIEnv *env, jobject listener) {
    if (pipe(g_log_pipe) != 0)
        return -1;

    /* Keep the read end non-blocking-ish by leaving it blocking; the
     * reader thread blocks in read() which is what we want. The write end
     * must not block the bot, and the kernel gives us ~64KB of slack
     * which the reader keeps drained. */
    fcntl(g_log_pipe[1], F_SETFL, O_NONBLOCK);

    g_saved_stdout = dup(STDOUT_FILENO);
    g_saved_stderr = dup(STDERR_FILENO);
    dup2(g_log_pipe[1], STDOUT_FILENO);
    dup2(g_log_pipe[1], STDERR_FILENO);
    close(g_log_pipe[1]);

    if (listener) {
        g_listener = (*env)->NewGlobalRef(env, listener);
        jclass cls = (*env)->GetObjectClass(env, listener);
        g_on_line = (*env)->GetMethodID(env, cls, "onLine", "(Ljava/lang/String;)V");
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env, NULL);
        (*env)->DeleteLocalRef(env, cls);
    }

    if (pthread_create(&g_log_thread, NULL, log_reader, NULL) == 0)
        g_log_thread_started = 1;
    return 0;
}

static void stop_log_capture(JNIEnv *env) {
    fflush(stdout);
    fflush(stderr);
    if (g_saved_stdout >= 0) {
        dup2(g_saved_stdout, STDOUT_FILENO);
        close(g_saved_stdout);
        g_saved_stdout = -1;
    }
    if (g_saved_stderr >= 0) {
        dup2(g_saved_stderr, STDERR_FILENO);
        close(g_saved_stderr);
        g_saved_stderr = -1;
    }
    if (g_log_pipe[0] >= 0) {
        close(g_log_pipe[0]);   /* unblocks the reader with EBADF-ish EOF */
        g_log_pipe[0] = -1;
    }
    if (g_log_thread_started)
        pthread_join(g_log_thread, NULL);
    g_log_thread_started = 0;
    if (g_listener && env) {
        (*env)->DeleteGlobalRef(env, g_listener);
        g_listener = NULL;
    }
    g_on_line = NULL;
}

static void *bot_thread_main(void *arg) {
    char *cfg = (char *)arg;
    const char *argv[2];
    argv[0] = "bot";
    argv[1] = cfg;
    main(2, argv);
    free(cfg);
    emit_line("[panel] bot engine returned, session ended");
    return NULL;
}

JNIEXPORT void JNICALL
Java_com_igg_lordsm_panel_NativeBridge_nativeStart(JNIEnv *env, jclass cls,
                                                     jstring cfg_path,
                                                     jobject listener) {
    (void)cls;
    if (g_bot_running)
        return;

    const char *cfg = (*env)->GetStringUTFChars(env, cfg_path, NULL);
    char *cfg_copy = strdup(cfg);
    (*env)->ReleaseStringUTFChars(env, cfg_path, cfg);

    if (!g_vm) {
        emit_line("[panel] JNI_OnLoad never ran; refusing to start");
        free(cfg_copy);
        return;
    }

    start_log_capture(env, listener);
    g_bot_running = 1;
    pthread_create(&g_bot_thread, NULL, bot_thread_main, cfg_copy);
}

JNIEXPORT void JNICALL
Java_com_igg_lordsm_panel_NativeBridge_nativeStop(JNIEnv *env, jclass cls) {
    (void)cls;
    if (!g_bot_running)
        return;
    bot_request_exit();          /* ProcessConnection leaves its loop */
    pthread_join(g_bot_thread, NULL);
    g_bot_running = 0;
    stop_log_capture(env);
}

JNIEXPORT jboolean JNICALL
Java_com_igg_lordsm_panel_NativeBridge_nativeIsRunning(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    return g_bot_running ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;
    return JNI_VERSION_1_6;
}
