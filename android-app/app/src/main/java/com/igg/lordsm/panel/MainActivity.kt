package com.igg.lordsm.panel

import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.igg.lordsm.panel.data.Account
import com.igg.lordsm.panel.data.AccountStore
import com.igg.lordsm.panel.feature.*
import com.igg.lordsm.panel.pcap.PcapdroidImporter
import com.igg.lordsm.panel.ui.LordsBotTheme
import kotlinx.coroutines.flow.MutableStateFlow
import java.io.File

private enum class Tab(val label: String) {
    Dashboard("Dashboard"), Features("Features"), Accounts("Accounts"), Logs("Logs")
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val store = AccountStore(this)
        if (store.load().isEmpty()) {
            store.save(mutableListOf(Account(id = AccountStore.newId(), label = "Main")))
        }
        setContent {
            LordsBotTheme {
                Surface(Modifier.fillMaxSize()) { AppRoot(store) }
            }
        }
    }
}

@Composable
private fun AppRoot(store: AccountStore) {
    val accounts = remember { MutableStateFlow(store.load()) }
    var activeId by remember { mutableStateOf(accounts.value.firstOrNull()?.id) }
    var tab by remember { mutableStateOf(Tab.Dashboard) }
    var menuOpen by remember { mutableStateOf(false) }
    var notice by remember { mutableStateOf<String?>(null) }
    var running by remember { mutableStateOf(false) }

    val active = accounts.value.firstOrNull { it.id == activeId }

    val persist: (List<Account>) -> Unit = { list ->
        accounts.value = list
        store.save(list)
    }

    // ---- PCAP import -------------------------------------------------
    var importResult by remember { mutableStateOf<PcapdroidImporter.Result?>(null) }
    val picker = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        val text = runCatching {
            contentResolver.openInputStream(uri)?.bufferedReader()?.use { it.readText() }
        }.getOrNull()
        importResult = if (text == null)
            PcapdroidImporter.Result.Bad("Could not read the selected file")
        else
            PcapdroidImporter.parse(text, uri.lastPathSegment ?: "capture.txt")
    }

    // ---- Start / stop --------------------------------------------------
    fun startBot() {
        val acct = active ?: run {
            notice = "Pick an account first"
            return
        }
        if (!acct.hasCredential) {
            notice = "This account has no credential. Import a PCAPdroid export."
            tab = Tab.Accounts
            return
        }
        val cfg = File(filesDir, "cfg/${acct.id}.cfg")
        ConfigWriter.write(cfg, acct)
        BotService.start(this, acct.id, cfg.absolutePath)
        running = true
        notice = "Engine starting for ${acct.label}"
    }

    fun stopBot() {
        BotService.stop(this)
        running = false
        notice = "Engine stopping"
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("Lords Bot Panel", fontWeight = FontWeight.Bold)
                        Text(
                            active?.label ?: "No account",
                            style = MaterialTheme.typography.labelSmall
                        )
                    }
                },
                actions = {
                    IconButton(onClick = { menuOpen = true }) {
                        Icon(Icons.Default.MoreVert, contentDescription = "Menu")
                    }
                    DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                        DropdownMenuItem(
                            text = { Text("Safe mode: ${if (active?.ultraSafe == true) "Ultra" else "Normal"}") },
                            onClick = {
                                menuOpen = false
                                val a = active ?: return@DropdownMenuItem
                                persist(accounts.value.map {
                                    if (it.id == a.id) it.copy(ultraSafe = !it.ultraSafe) else it
                                })
                                notice = if (!a.ultraSafe)
                                    "Ultra Safe Mode on - risky features pinned off" else "Ultra Safe Mode off"
                            }
                        )
                        DropdownMenuItem(
                            text = { Text("Add account") },
                            onClick = {
                                menuOpen = false
                                persist(accounts.value + Account(
                                    id = AccountStore.newId(),
                                    label = "Account ${accounts.value.size + 1}"
                                ))
                                tab = Tab.Accounts
                            }
                        )
                        DropdownMenuItem(
                            text = { Text("Import PCAPdroid capture") },
                            onClick = {
                                menuOpen = false
                                picker.launch(arrayOf("text/plain", "*/*"))
                            }
                        )
                        HorizontalDivider()
                        DropdownMenuItem(
                            text = { Text("Clear log") },
                            onClick = {
                                menuOpen = false
                                LogBus.clear()
                            }
                        )
                    }
                }
            )
        },
        bottomBar = {
            NavigationBar {
                Tab.entries.forEach { t ->
                    NavigationBarItem(
                        selected = tab == t,
                        onClick = { tab = t },
                        icon = { Text(t.label.take(1)) },
                        label = { Text(t.label, fontSize = 10.sp) }
                    )
                }
            }
        }
    ) { pad ->
        Box(Modifier.padding(pad).fillMaxSize()) {
            when (tab) {
                Tab.Dashboard -> DashboardScreen(active, running, ::startBot, ::stopBot)
                Tab.Features -> FeaturesScreen(active) { a -> persist(accounts.value.map { if (it.id == a.id) a else it }) }
                Tab.Accounts -> AccountsScreen(
                    accounts.value, activeId,
                    onSelect = { activeId = it },
                    onEdit = { a -> persist(accounts.value.map { if (it.id == a.id) a else it }) },
                    onDelete = { id ->
                        persist(accounts.value.filter { it.id != id })
                        if (activeId == id) activeId = accounts.value.firstOrNull { it.id != id }?.id
                    },
                    onImport = { picker.launch(arrayOf("text/plain", "*/*")) }
                )
                Tab.Logs -> LogScreen()
            }

            notice?.let { msg ->
                Snackbar(
                    Modifier.align(Alignment.BottomCenter).padding(12.dp),
                    action = { TextButton(onClick = { notice = null }) { Text("Dismiss") } }
                ) { Text(msg) }
                LaunchedEffect(msg) {
                    kotlinx.coroutines.delay(3500)
                    notice = null
                }
            }
        }
    }

    // Apply an imported credential to the active account.
    importResult?.let { res ->
        AlertDialog(
            onDismissRequest = { importResult = null },
            title = {
                Text(if (res is PcapdroidImporter.Result.Ok) "Credential imported" else "Import failed")
            },
            text = {
                Text(
                    when (res) {
                        is PcapdroidImporter.Result.Ok -> PcapdroidImporter.describe(res.credential)
                        is PcapdroidImporter.Result.Bad -> res.reason
                    },
                    fontFamily = FontFamily.Monospace,
                    fontSize = 12.sp
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    val a = active
                    if (res is PcapdroidImporter.Result.Ok && a != null) {
                        persist(accounts.value.map {
                            if (it.id == a.id) it.copy(
                                accessKey = res.credential.accessKey,
                                iggId = res.credential.akid,
                                deviceUuid = res.credential.kmd,
                                adminName = it.adminName.ifBlank { "spyxlight" }
                            ) else it
                        })
                        notice = "Credential applied to ${a.label}"
                    }
                    importResult = null
                }) { Text("Apply") }
            },
            dismissButton = {
                TextButton(onClick = { importResult = null }) { Text("Close") }
            }
        )
    }
}

@Composable
private fun DashboardScreen(
    account: Account?,
    running: Boolean,
    onStart: () -> Unit,
    onStop: () -> Unit,
) {
    Column(
        Modifier.padding(16.dp).fillMaxSize().verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(14.dp)
    ) {
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        if (running) Icons.Default.Stop else Icons.Default.PlayArrow,
                        contentDescription = null,
                        tint = if (running) MaterialTheme.colorScheme.error
                        else MaterialTheme.colorScheme.primary
                    )
                    Spacer(Modifier.width(10.dp))
                    Column {
                        Text(
                            if (running) "Engine running" else "Engine stopped",
                            style = MaterialTheme.typography.titleMedium
                        )
                        Text(
                            account?.label ?: "No account selected",
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                }
                Button(
                    onClick = if (running) onStop else onStart,
                    modifier = Modifier.fillMaxWidth()
                ) { Text(if (running) "Stop engine" else "Start engine") }
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            StatCard("Features on", "${account?.let { ConfigWriter.enabledCount(it) } ?: 0}",
                Modifier.weight(1f))
            StatCard("Total keys", "${FeatureRegistry.all.size}", Modifier.weight(1f))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            StatCard("Mode", if (account?.ultraSafe == true) "Ultra Safe" else "Normal",
                Modifier.weight(1f))
            StatCard("Pinned off", "${RiskyKeys.ALWAYS_BLOCKED.size}", Modifier.weight(1f))
        }

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("Credential", style = MaterialTheme.typography.titleSmall)
                Text(
                    account?.maskedKey() ?: "-",
                    fontFamily = FontFamily.Monospace, fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Text(
                    "IGG ${account?.iggId?.ifBlank { "-" } ?: "-"}",
                    fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("What Ultra Safe Mode does", style = MaterialTheme.typography.titleSmall)
                Text(
                    "Pins every opcode that was empirically found to close this " +
                        "session to false (${RiskyKeys.ALWAYS_BLOCKED.size} keys), and " +
                        "forbids all RISKY features. The clamp is applied when the " +
                        "config file is written, not in the UI, so it cannot be bypassed.",
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}

@Composable
private fun StatCard(label: String, value: String, modifier: Modifier = Modifier) {
    Card(modifier) {
        Column(Modifier.padding(14.dp)) {
            Text(value, style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.Bold)
            Text(label, style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun FeaturesScreen(account: Account?, onChange: (Account) -> Unit) {
    if (account == null) {
        Empty("No account selected")
        return
    }
    var group by remember { mutableStateOf(FeatureRegistry.groups.firstOrNull { it != "Identity" }) }
    var query by remember { mutableStateOf("") }

    Column(Modifier.fillMaxSize()) {
        OutlinedTextField(
            value = query,
            onValueChange = { query = it },
            label = { Text("Search settings") },
            singleLine = true,
            modifier = Modifier.padding(12.dp).fillMaxWidth()
        )
        LazyColumn(
            Modifier.height(44.dp).padding(horizontal = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            items(FeatureRegistry.groups) { g ->
                FilterChip(
                    selected = group == g,
                    onClick = { group = g },
                    label = { Text(g, fontSize = 11.sp) }
                )
            }
        }
        LazyColumn(Modifier.fillMaxSize().padding(horizontal = 12.dp)) {
            val specs = FeatureRegistry.forGroup(group!!).filter {
                query.isBlank() ||
                    it.label.contains(query, true) || it.key.contains(query, true)
            }
            items(specs, key = { it.key }) { spec ->
                FeatureRow(spec, account) { newValue ->
                    onChange(account.copy(values = account.values.toMutableMap().apply {
                        put(spec.key, newValue)
                    }))
                }
            }
        }
    }
}

@Composable
private fun FeatureRow(spec: FeatureSpec, account: Account, onSet: (String) -> Unit) {
    val raw = account.values[spec.key] ?: spec.defaultValue
    val blocked = spec.blocked || (account.ultraSafe && spec.risk == Risk.RISKY)
    val effective = if (blocked && spec.type == FieldType.TOGGLE) "false" else raw

    Card(
        Modifier.fillMaxWidth().padding(vertical = 4.dp),
        shape = RoundedCornerShape(12.dp)
    ) {
        Row(
            Modifier.padding(14.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(spec.label, style = MaterialTheme.typography.bodyMedium,
                        fontWeight = FontWeight.Medium)
                    RiskBadge(spec.risk)
                }
                Text(
                    spec.key, fontSize = 10.sp, fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                if (spec.help.isNotBlank())
                    Text(spec.help, fontSize = 11.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Spacer(Modifier.width(10.dp))
            when (spec.type) {
                FieldType.TOGGLE -> Switch(
                    checked = effective == "true",
                    enabled = !blocked,
                    onCheckedChange = { onSet(it.toString()) }
                )
                FieldType.INT -> CompactNumber(
                    value = raw,
                    enabled = !blocked,
                    onSet = onSet
                )
                FieldType.CHOICE -> CompactChoice(
                    value = raw, choices = spec.choices, enabled = !blocked, onSet = onSet
                )
                FieldType.TEXT -> CompactText(value = raw, enabled = !blocked, onSet = onSet)
                FieldType.READONLY -> Text(
                    if (raw.isBlank()) "-" else "set",
                    fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
        if (blocked) {
            Text(
                if (spec.blocked)
                    "Pinned off: this opcode was found to close the session."
                else "Blocked by Ultra Safe Mode (RISKY).",
                fontSize = 10.sp,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.padding(start = 14.dp, bottom = 10.dp)
            )
        }
    }
}

@Composable
private fun RiskBadge(risk: Risk) {
    val (label, color) = when (risk) {
        Risk.SAFE -> "SAFE" to MaterialTheme.colorScheme.secondary
        Risk.CAUTION -> "CAUTION" to MaterialTheme.colorScheme.primary
        Risk.RISKY -> "RISKY" to MaterialTheme.colorScheme.error
    }
    Spacer(Modifier.width(6.dp))
    Surface(color = color.copy(alpha = 0.18f), shape = RoundedCornerShape(4.dp)) {
        Text(label, fontSize = 8.sp, color = color, modifier = Modifier.padding(horizontal = 4.dp))
    }
}

@Composable
private fun CompactNumber(value: String, enabled: Boolean, onSet: (String) -> Unit) {
    var text by remember(value) { mutableStateOf(value) }
    OutlinedTextField(
        value = text,
        onValueChange = { text = it.filter { c -> c.isDigit() }; onSet(text) },
        singleLine = true,
        enabled = enabled,
        modifier = Modifier.width(96.dp),
        textStyle = MaterialTheme.typography.bodyMedium
    )
}

@Composable
private fun CompactText(value: String, enabled: Boolean, onSet: (String) -> Unit) {
    var text by remember(value) { mutableStateOf(value) }
    OutlinedTextField(
        value = text,
        onValueChange = { text = it; onSet(it) },
        singleLine = true,
        enabled = enabled,
        modifier = Modifier.width(130.dp),
        textStyle = MaterialTheme.typography.bodySmall
    )
}

@Composable
private fun CompactChoice(value: String, choices: List<String>, enabled: Boolean, onSet: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        OutlinedButton(onClick = { open = true }, enabled = enabled) {
            Text(value.ifBlank { "-" }, fontSize = 11.sp)
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (ch in choices.ifEmpty { listOf(value) }) {
                DropdownMenuItem(text = { Text(ch, fontSize = 12.sp) },
                    onClick = { onSet(ch); open = false })
            }
        }
    }
}

@Composable
private fun AccountsScreen(
    accounts: List<Account>,
    activeId: String?,
    onSelect: (String) -> Unit,
    onEdit: (Account) -> Unit,
    onDelete: (String) -> Unit,
    onImport: () -> Unit,
) {
    var editing by remember { mutableStateOf<Account?>(null) }

    LazyColumn(Modifier.fillMaxSize().padding(12.dp)) {
        item {
            Card(Modifier.fillMaxWidth()) {
                Row(Modifier.padding(14.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text("Add an account", style = MaterialTheme.typography.titleSmall)
                        Text(
                            "Import a PCAPdroid text export of the game's login " +
                                "exchange to fill the credential automatically.",
                            fontSize = 11.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                    FilledTonalButton(onClick = onImport) { Text("Import") }
                }
            }
        }
        items(accounts, key = { it.id }) { acc ->
            Card(
                Modifier.fillMaxWidth().padding(top = 8.dp).clickable { onSelect(acc.id) },
                colors = if (acc.id == activeId)
                    CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.primaryContainer)
                else CardDefaults.cardColors()
            ) {
                Column(Modifier.padding(14.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(acc.label, fontWeight = FontWeight.SemiBold)
                            Text(
                                "IGG ${acc.iggId.ifBlank { "-" }}  ${acc.maskedKey()}",
                                fontSize = 10.sp, fontFamily = FontFamily.Monospace,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                        TextButton(onClick = { editing = acc }) { Text("Edit") }
                        TextButton(onClick = { onDelete(acc.id) }) { Text("Delete") }
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Switch(
                            checked = acc.ultraSafe,
                            onCheckedChange = { onEdit(acc.copy(ultraSafe = it)) }
                        )
                        Spacer(Modifier.width(8.dp))
                        Text("Ultra Safe Mode", fontSize = 12.sp)
                    }
                }
            }
        }
    }

    editing?.let { acct ->
        AccountEditor(acct, onDismiss = { editing = null }) { updated ->
            onEdit(updated); editing = null
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AccountEditor(account: Account, onDismiss: () -> Unit, onSave: (Account) -> Unit) {
    var a by remember { mutableStateOf(account) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Edit ${account.label}") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                EditField("Label", a.label) { a = a.copy(label = it) }
                EditField("Gateway", a.gatewayAddr) { a = a.copy(gatewayAddr = it) }
                EditField("Port", a.gatewayPort, numeric = true) { a = a.copy(gatewayPort = it) }
                EditField("IGG ID", a.iggId, numeric = true) { a = a.copy(iggId = it) }
                EditField("Device UUID", a.deviceUuid) { a = a.copy(deviceUuid = it) }
                EditField("Access key", a.accessKey, secret = true) { a = a.copy(accessKey = it) }
                EditField("Bot name", a.adminName) { a = a.copy(adminName = it) }
            }
        },
        confirmButton = { TextButton(onClick = { onSave(a) }) { Text("Save") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } }
    )
}

@Composable
private fun EditField(
    label: String,
    value: String,
    numeric: Boolean = false,
    secret: Boolean = false,
    onChange: (String) -> Unit,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
        visualTransformation = if (secret) PasswordVisualTransformation()
        else androidx.compose.ui.text.input.VisualTransformation.None,
        keyboardOptions = if (numeric)
            KeyboardOptions(keyboardType = KeyboardType.Number) else KeyboardOptions.Default
    )
}

@Composable
private fun LogScreen() {
    val lines = remember { mutableStateOf(LogBus.snapshot()) }
    DisposableEffect(Unit) {
        val l: (String) -> Unit = { lines.value = (lines.value + it).takeLast(2000) }
        LogBus.addListener(l)
        onDispose { LogBus.removeListener(l) }
    }
    LaunchedEffect(Unit) { lines.value = LogBus.snapshot() }

    LazyColumn(
        Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)
    ) {
        items(lines.value.reversed()) { line ->
            Text(
                line,
                fontFamily = FontFamily.Monospace,
                fontSize = 10.sp,
                modifier = Modifier.padding(horizontal = 10.dp, vertical = 1.dp),
                color = when {
                    line.contains("WARN") -> MaterialTheme.colorScheme.primary
                    line.contains("ERROR") || line.contains("Disconnected") ->
                        MaterialTheme.colorScheme.error
                    line.startsWith("[panel]") -> MaterialTheme.colorScheme.secondary
                    else -> MaterialTheme.colorScheme.onBackground
                }
            )
        }
    }
}

@Composable
private fun Empty(msg: String) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Text(msg, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
