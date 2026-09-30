package com.igg.lordsm.panel

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.ScrollView
import android.widget.PopupMenu
import android.widget.Spinner
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import com.igg.lordsm.panel.data.Account
import com.igg.lordsm.panel.data.AccountStore
import com.igg.lordsm.panel.feature.FeatureRegistry
import com.igg.lordsm.panel.feature.FeatureSpec
import com.igg.lordsm.panel.feature.FieldType
import com.igg.lordsm.panel.feature.Risk
import com.igg.lordsm.panel.feature.RiskyKeys
import com.igg.lordsm.panel.pcap.PcapdroidImporter
import java.io.File

private const val REQ_PICK_CAPTURE = 4711

private val GOLD = Color.parseColor("#D9A441")
private val CYAN = Color.parseColor("#4FB3C4")
private val DANGER = Color.parseColor("#E05B4B")
private val SLATE = Color.parseColor("#14171C")
private val SLATE_UP = Color.parseColor("#1C2027")
private val DIM = Color.parseColor("#B4BAC4")
private val FG = Color.parseColor("#E6E8EC")

/**
 * lordsM control panel.
 *
 * Deliberately framework-only: no AndroidX and no Compose. The whole APK is
 * produced by build.sh with javac + kotlinc + d8 + aapt, and every third
 * party dependency removed is one fewer thing that can fail to resolve on
 * a machine with no working Gradle/SDK. The cost is a less decorative UI
 * than Compose would have given.
 */
class MainActivity : Activity(), NativeBridge.LogSink {

    private lateinit var store: AccountStore
    private var accounts = mutableListOf<Account>()
    private var activeId: String? = null
    private var running = false

    private lateinit var content: FrameLayout
    private lateinit var titleView: TextView
    private lateinit var subtitleView: TextView
    private val navButtons = mutableListOf<Button>()
    private val mainHandler = Handler(Looper.getMainLooper())

    private val active: Account? get() = accounts.firstOrNull { it.id == activeId }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = AccountStore(this)
        accounts = store.load()
        if (accounts.isEmpty()) {
            accounts = mutableListOf(Account(id = AccountStore.newId(), label = "Main"))
            store.save(accounts)
        }
        activeId = accounts.first().id
        setContentView(buildRoot())
        showDashboard()
        pollEngine()
    }

    // ---------------------------------------------------------------- ui

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun buildRoot(): View {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(SLATE)
        }

        // Top bar with the overflow button.
        val top = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setBackgroundColor(SLATE_UP)
            setPadding(dp(14), dp(10), dp(6), dp(10))
        }
        val titles = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        titleView = TextView(this).apply {
            setTextColor(FG); textSize = 17f; typeface = Typeface.DEFAULT_BOLD
            text = "lordsM"
        }
        subtitleView = TextView(this).apply {
            setTextColor(DIM); textSize = 11f; text = "-"
        }
        titles.addView(titleView)
        titles.addView(subtitleView)
        top.addView(titles, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))

        val menuBtn = Button(this).apply {
            text = "\u22EE"          // vertical ellipsis
            setTextColor(GOLD); textSize = 20f
            setBackgroundColor(Color.TRANSPARENT)
            setOnClickListener { showOverflow(it) }
        }
        top.addView(menuBtn, LinearLayout.LayoutParams(dp(52), dp(44)))
        root.addView(top)

        content = FrameLayout(this)
        root.addView(content, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))

        // Bottom nav.
        val nav = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(SLATE_UP)
        }
        for (i in 0 until 4) {
            val b = Button(this).apply {
                textSize = 10f
                setBackgroundColor(Color.TRANSPARENT)
                val screens = arrayOf<() -> Unit>(
                    { showDashboard() }, { showFeatures() },
                    { showAccounts() }, { showLogs() }
                )
                setOnClickListener { screens[i].invoke() }
            }
            navButtons.add(b)
            nav.addView(b, LinearLayout.LayoutParams(0, dp(52), 1f))
        }
        root.addView(nav)
        return root
    }

    private fun markNav(activeIndex: Int) {
        val labels = arrayOf("Dash", "Feat", "Accs", "Logs")
        navButtons.forEachIndexed { i, b ->
            b.text = labels[i]
            b.setTextColor(if (i == activeIndex) GOLD else DIM)
        }
        subtitleView.text = active?.label ?: "No account"
    }

    private fun scroll(body: View): ScrollView =
        ScrollView(this).apply {
            setBackgroundColor(SLATE)
            isFillViewport = true
            addView(body, ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }

    private fun card(title: String? = null): LinearLayout =
        LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(SLATE_UP)
            setPadding(dp(14), dp(12), dp(14), dp(12))
            if (title != null) {
                addView(TextView(this@MainActivity).apply {
                    text = title; setTextColor(GOLD); textSize = 13f
                    typeface = Typeface.DEFAULT_BOLD
                })
            }
        }

    private fun page(vararg children: View): ScrollView {
        val body = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(12), dp(12), dp(12))
        }
        for (c in children)
            body.addView(c, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = dp(10)
            })
        return scroll(body)
    }

    private fun swap(v: View, navIndex: Int) {
        content.removeAllViews()
        content.addView(v)
        markNav(navIndex)
    }

    // --------------------------------------------------------- dashboard

    private fun showDashboard() {
        val body = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }

        val status = card("Engine")
        val statusText = TextView(this).apply {
            setTextColor(FG); textSize = 15f
            text = if (running) "Running" else "Stopped"
        }
        status.addView(statusText)
        val toggle = Button(this).apply {
            text = if (running) "Stop engine" else "Start engine"
            setOnClickListener {
                if (running) stopBot() else startBot()
                showDashboard()
            }
        }
        status.addView(toggle, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(8)
        })
        body.addView(status)

        body.addView(card("Controls").apply {
            addView(stat("Features on", "${active?.let { ConfigWriter.enabledCount(it) } ?: 0}"))
            addView(stat("Total controls", "${FeatureRegistry.all.size}"))
            addView(stat("Mode", if (active?.ultraSafe == true) "Ultra Safe" else "Normal"))
            addView(stat("Pinned off always", "${RiskyKeys.ALWAYS_BLOCKED.size}"))
        })

        body.addView(card("Credential").apply {
            addView(TextView(this@MainActivity).apply {
                text = active?.maskedKey() ?: "-"
                setTextColor(DIM); textSize = 11f; typeface = Typeface.MONOSPACE
            })
            addView(TextView(this@MainActivity).apply {
                text = "IGG ${active?.iggId?.ifBlank { "-" } ?: "-"}"
                setTextColor(DIM); textSize = 11f
            })
        })

        body.addView(card("Ultra Safe Mode").apply {
            addView(TextView(this@MainActivity).apply {
                text = "Forces every RISKY key off, plus the ${RiskyKeys.ALWAYS_BLOCKED.size} " +
                    "keys that were empirically found to close this session. The clamp is " +
                    "applied when the config file is written, not in the UI, so a screen " +
                    "that forgets to check cannot bypass it."
                setTextColor(DIM); textSize = 11f
            })
        })

        swap(page(body), 0)
    }

    private fun stat(label: String, value: String): View =
        LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(TextView(this@MainActivity).apply {
                text = label; setTextColor(DIM); textSize = 12f
            }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(TextView(this@MainActivity).apply {
                text = value; setTextColor(FG); textSize = 12f
                typeface = Typeface.DEFAULT_BOLD
            })
        }

    // ---------------------------------------------------------- features

    private fun showFeatures() {
        val acct = active ?: run { swap(page(card("No account selected")), 1); return }
        val list = ListView(this)
        val adapter = FeatureAdapter(acct)
        list.adapter = adapter
        list.setOnItemClickListener { _, _, _, _ -> }

        val search = EditText(this).apply {
            hint = "Search settings"
            setTextColor(FG); setHintTextColor(DIM)
            setBackgroundColor(SLATE_UP)
            setPadding(dp(12), dp(10), dp(12), dp(10))
        }
        val groups = Spinner(this)
        val groupList = ArrayList(FeatureRegistry.groups)
        groups.adapter = ArrayAdapter(this,
            android.R.layout.simple_spinner_dropdown_item, groupList)
        groups.setBackgroundColor(SLATE_UP)
        groups.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) {
                adapter.group = groupList[pos]
                adapter.notifyDataSetChanged()
            }
            override fun onNothingSelected(p: AdapterView<*>?) {}
        }
        search.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun afterTextChanged(s: android.text.Editable?) {
                adapter.query = s?.toString() ?: ""
                adapter.notifyDataSetChanged()
            }
        })

        val outer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(SLATE)
            addView(search, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(groups, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(list, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        swap(outer, 1)
    }

    private inner class FeatureAdapter(private val acct: Account) : BaseAdapter() {
        var group: String = FeatureRegistry.groups.firstOrNull { it != "Identity" }
            ?: FeatureRegistry.groups.first()
        var query: String = ""

        private val rows: List<FeatureSpec>
            get() = FeatureRegistry.forGroup(group).filter {
                query.isBlank() || it.label.contains(query, true) ||
                    it.key.contains(query, true)
            }

        override fun getCount() = rows.size
        override fun getItem(i: Int): Any = rows[i]
        override fun getItemId(i: Int): Long = i.toLong()

        override fun getView(position: Int, convert: View?, parent: ViewGroup?): View {
            val spec = rows[position]
            val root = LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.VERTICAL
                setBackgroundColor(SLATE_UP)
                setPadding(dp(12), dp(10), dp(12), dp(10))
            }
            val head = LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
            }
            val labels = LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.VERTICAL
            }
            labels.addView(TextView(this@MainActivity).apply {
                text = spec.label
                setTextColor(FG); textSize = 14f; typeface = Typeface.DEFAULT_BOLD
            })
            labels.addView(TextView(this@MainActivity).apply {
                text = spec.key
                setTextColor(DIM); textSize = 10f; typeface = Typeface.MONOSPACE
            })
            if (spec.help.isNotBlank())
                labels.addView(TextView(this@MainActivity).apply {
                    text = spec.help; setTextColor(DIM); textSize = 11f
                })
            head.addView(labels, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))

            val blocked = spec.blocked || (acct.ultraSafe && spec.risk == Risk.RISKY)
            val raw = acct.values[spec.key] ?: spec.defaultValue

            when (spec.type) {
                FieldType.TOGGLE -> {
                    val on = if (blocked) false else raw == "true"
                    head.addView(Switch(this@MainActivity).apply {
                        isChecked = on
                        isEnabled = !blocked
                        setOnCheckedChangeListener { _, checked ->
                            setValue(spec.key, checked.toString())
                        }
                    })
                }
                FieldType.INT -> head.addView(intField(raw, blocked) { setValue(spec.key, it) })
                FieldType.CHOICE -> head.addView(
                    choiceField(raw, spec.choices, blocked) { setValue(spec.key, it) })
                FieldType.TEXT -> head.addView(textField(raw, blocked) { setValue(spec.key, it) })
                FieldType.READONLY -> head.addView(TextView(this@MainActivity).apply {
                    text = if (raw.isBlank()) "-" else "set"; setTextColor(DIM); textSize = 11f
                })
            }
            root.addView(head)

            val badge = when (spec.risk) {
                Risk.SAFE -> "SAFE"
                Risk.CAUTION -> "CAUTION"
                Risk.RISKY -> "RISKY"
            }
            val badgeColor = when (spec.risk) {
                Risk.SAFE -> CYAN
                Risk.CAUTION -> GOLD
                Risk.RISKY -> DANGER
            }
            root.addView(TextView(this@MainActivity).apply {
                text = if (blocked && spec.blocked)
                    "Pinned off: this opcode was found to close the session."
                else if (blocked) "Blocked by Ultra Safe Mode (RISKY)."
                else badge
                setTextColor(if (blocked) DANGER else badgeColor)
                textSize = 10f
            })
            return root
        }

        private fun setValue(key: String, value: String) {
            val a = active ?: return
            val updated = accounts.map {
                if (it.id == a.id) it.copy(values = it.values.toMutableMap().apply {
                    put(key, value)
                }) else it
            }
            accounts = updated.toMutableList()
            store.save(accounts)
        }
    }

    private fun intField(value: String, enabled: Boolean, onSet: (String) -> Unit) =
        EditText(this).apply {
            setText(value); isEnabled = enabled
            setTextColor(FG); textSize = 13f
            inputType = InputType.TYPE_CLASS_NUMBER
            setWidth(dp(100))
            addTextChangedListener(object : android.text.TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun afterTextChanged(s: android.text.Editable?) {
                    onSet(s?.toString()?.filter { it.isDigit() } ?: "")
                }
            })
        }

    private fun textField(value: String, enabled: Boolean, onSet: (String) -> Unit) =
        EditText(this).apply {
            setText(value); isEnabled = enabled
            setTextColor(FG); textSize = 12f
            setWidth(dp(140))
            addTextChangedListener(object : android.text.TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun afterTextChanged(s: android.text.Editable?) {
                    onSet(s?.toString() ?: "")
                }
            })
        }

    private fun choiceField(value: String, choices: List<String>, enabled: Boolean,
                            onSet: (String) -> Unit) =
        Spinner(this).apply {
            val list = if (choices.isEmpty()) listOf(value) else choices
            adapter = ArrayAdapter(this@MainActivity,
                android.R.layout.simple_spinner_dropdown_item, list)
            isEnabled = enabled
            setSelection(list.indexOf(value).coerceAtLeast(0))
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) {
                    onSet(list[pos])
                }
                override fun onNothingSelected(p: AdapterView<*>?) {}
            }
        }

    // ---------------------------------------------------------- accounts

    private fun showAccounts() {
        val body = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }

        body.addView(card("Add an account").apply {
            addView(TextView(this@MainActivity).apply {
                text = "Import a PCAPdroid text export of the game's login exchange to " +
                    "fill the credential automatically."
                setTextColor(DIM); textSize = 11f
            })
            addView(Button(this@MainActivity).apply {
                text = "Import PCAPdroid capture"
                setOnClickListener { pickCapture() }
            }, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(8)
            })
            addView(Button(this@MainActivity).apply {
                text = "Add blank account"
                setOnClickListener {
                    accounts.add(Account(id = AccountStore.newId(),
                        label = "Account ${accounts.size + 1}"))
                    persist(); showAccounts()
                }
            })
        })

        for (acct in accounts) {
            body.addView(card().apply {
                addView(TextView(this@MainActivity).apply {
                    text = acct.label
                    setTextColor(if (acct.id == activeId) GOLD else FG)
                    textSize = 15f; typeface = Typeface.DEFAULT_BOLD
                })
                addView(TextView(this@MainActivity).apply {
                    text = "IGG ${acct.iggId.ifBlank { "-" }}   ${acct.maskedKey()}"
                    setTextColor(DIM); textSize = 10f; typeface = Typeface.MONOSPACE
                })
                val row = LinearLayout(this@MainActivity).apply {
                    orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
                }
                row.addView(Switch(this@MainActivity).apply {
                    isChecked = acct.ultraSafe
                    setOnCheckedChangeListener { _, checked ->
                        update(acct.copy(ultraSafe = checked)); showAccounts()
                    }
                })
                row.addView(TextView(this@MainActivity).apply {
                    text = "  Ultra Safe Mode"; setTextColor(DIM); textSize = 12f
                })
                addView(row)
                val btns = LinearLayout(this@MainActivity).apply {
                    orientation = LinearLayout.HORIZONTAL
                }
                btns.addView(Button(this@MainActivity).apply {
                    text = "Select"; setOnClickListener {
                        activeId = acct.id; showDashboard()
                    }
                })
                btns.addView(Button(this@MainActivity).apply {
                    text = "Edit"; setOnClickListener { editAccount(acct) }
                })
                btns.addView(Button(this@MainActivity).apply {
                    text = "Delete"; setOnClickListener {
                        accounts.removeAll { it.id == acct.id }
                        if (activeId == acct.id) activeId = accounts.firstOrNull()?.id
                        persist(); showAccounts()
                    }
                })
                addView(btns)
            })
        }
        swap(page(body), 2)
    }

    private fun editAccount(acct: Account) {
        val fields = arrayOf("Label", "Gateway", "Port", "IGG ID", "Device UUID",
            "Access key", "Bot name")
        val inputs = arrayOf(
            field(acct.label), field(acct.gatewayAddr), field(acct.gatewayPort),
            field(acct.iggId, InputType.TYPE_CLASS_NUMBER),
            field(acct.deviceUuid), field(acct.accessKey), field(acct.adminName)
        )
        val form = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            fields.forEachIndexed { i, name ->
                addView(TextView(this@MainActivity).apply {
                    text = name; setTextColor(DIM); textSize = 11f
                })
                addView(inputs[i])
            }
        }
        AlertDialog.Builder(this).setTitle("Edit ${acct.label}")
            .setView(ScrollView(this).apply { addView(form) })
            .setPositiveButton("Save") { _, _ ->
                update(acct.copy(
                    label = inputs[0].text.toString(),
                    gatewayAddr = inputs[1].text.toString(),
                    gatewayPort = inputs[2].text.toString(),
                    iggId = inputs[3].text.toString(),
                    deviceUuid = inputs[4].text.toString(),
                    accessKey = inputs[5].text.toString(),
                    adminName = inputs[6].text.toString()
                ))
                showAccounts()
            }
            .setNegativeButton("Cancel", null).show()
    }

    private fun field(v: String, type: Int = InputType.TYPE_CLASS_TEXT) =
        EditText(this).apply {
            setText(v); setTextColor(FG); inputType = type; textSize = 13f
            setSingleLine()
        }

    // --------------------------------------------------------------- logs

    private fun showLogs() {
        val tv = TextView(this).apply {
            setTextColor(FG); textSize = 10f; typeface = Typeface.MONOSPACE
            setPadding(dp(10), dp(10), dp(10), dp(10))
            text = LogBus.snapshot().joinToString("\n")
        }
        val sc = scroll(tv)
        val listener: (String) -> Unit = { line ->
            mainHandler.post {
                tv.append("\n" + line)
                sc.post { sc.fullScroll(View.FOCUS_DOWN) }
            }
        }
        LogBus.addListener(listener)
        sc.tag = listener
        swap(sc, 3)
    }

    // --------------------------------------------------------- lifecycle

    private var logListener: ((String) -> Unit)? = null

    override fun onResume() {
        super.onResume()
        accounts = store.load()
        if (accounts.none { it.id == activeId }) activeId = accounts.firstOrNull()?.id
    }

    override fun onDestroy() {
        logListener?.let { LogBus.removeListener(it) }
        super.onDestroy()
    }

    private fun pollEngine() {
        mainHandler.postDelayed(object : Runnable {
            override fun run() {
                running = NativeBridge.isRunning()
                pollEngine()
            }
        }, 1500)
    }

    override fun onLine(line: String) {
        LogBus.emit(line)
    }

    // ------------------------------------------------------------- menu

    private fun showOverflow(anchor: View) {
        val pm = PopupMenu(this, anchor)
        pm.menu.add("Ultra Safe Mode: " +
            if (active?.ultraSafe == true) "ON" else "off").setOnMenuItemClickListener {
            val a = active
            if (a != null) { update(a.copy(ultraSafe = !a.ultraSafe)); showDashboard() }
            true
        }
        pm.menu.add("Add account").setOnMenuItemClickListener {
            accounts.add(Account(id = AccountStore.newId(),
                label = "Account ${accounts.size + 1}"))
            persist(); showAccounts(); true
        }
        pm.menu.add("Import PCAPdroid capture").setOnMenuItemClickListener {
            pickCapture(); true
        }
        pm.menu.add("Clear log").setOnMenuItemClickListener {
            LogBus.clear(); showLogs(); true
        }
        pm.show()
    }

    // -------------------------------------------------------- start/stop

    private fun startBot() {
        val acct = active
        if (acct == null) { toast("Pick an account first"); return }
        if (!acct.hasCredential) {
            toast("No credential - import a PCAPdroid export")
            showAccounts(); return
        }
        val cfg = File(filesDir, "cfg/${acct.id}.cfg")
        ConfigWriter.write(cfg, acct)
        BotService.start(this, acct.id, cfg.absolutePath)
        running = true
        toast("Engine starting for ${acct.label}")
    }

    private fun stopBot() {
        BotService.stop(this)
        running = false
        toast("Engine stopping")
    }

    // ---------------------------------------------------------- capture

    private fun pickCapture() {
        val i = Intent(Intent.ACTION_GET_CONTENT).apply {
            type = "*/*"
            addCategory(Intent.CATEGORY_OPENABLE)
        }
        try {
            startActivityForResult(i, REQ_PICK_CAPTURE)
        } catch (e: Exception) {
            toast("No file picker available")
        }
    }

    override fun onActivityResult(req: Int, res: Int, data: Intent?) {
        if (req != REQ_PICK_CAPTURE || res != RESULT_OK) return
        val uri: Uri = data?.data ?: return
        val text = runCatching {
            contentResolver.openInputStream(uri)?.bufferedReader()?.use { it.readText() }
        }.getOrNull()
        val res2 = if (text == null)
            PcapdroidImporter.Result.Bad("Could not read the selected file")
        else
            PcapdroidImporter.parse(text, uri.lastPathSegment ?: "capture.txt")
        showImportResult(res2)
    }

    private fun showImportResult(res: PcapdroidImporter.Result) {
        val ok = res is PcapdroidImporter.Result.Ok
        val msg = when (res) {
            is PcapdroidImporter.Result.Ok ->
                PcapdroidImporter.describe(res.credential)
            is PcapdroidImporter.Result.Bad -> res.reason
        }
        AlertDialog.Builder(this)
            .setTitle(if (ok) "Credential imported" else "Import failed")
            .setMessage(msg)
            .setPositiveButton(if (ok) "Apply" else "Close") { _, _ ->
                val a = active
                if (ok && a != null) {
                    val c = (res as PcapdroidImporter.Result.Ok).credential
                    update(a.copy(
                        accessKey = c.accessKey,
                        iggId = c.akid,
                        deviceUuid = c.kmd
                    ))
                    toast("Credential applied to ${a.label}")
                    showAccounts()
                }
            }
            .setNegativeButton("Cancel", null).show()
    }

    // ------------------------------------------------------------ utils

    private fun persist() { store.save(accounts) }

    private fun update(acct: Account) {
        accounts = accounts.map { if (it.id == acct.id) acct else it }.toMutableList()
        store.save(accounts)
    }

    private fun toast(m: String) = Toast.makeText(this, m, Toast.LENGTH_SHORT).show()
}