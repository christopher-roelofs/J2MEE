package org.j2me.launcher

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.FolderOpen
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.VideogameAsset
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Snackbar
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.vector.rememberVectorPainter
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.application
import androidx.compose.ui.window.rememberWindowState
import androidx.compose.ui.window.WindowState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import javax.swing.JFileChooser
import javax.swing.UIManager

/**
 * Where we last scanned. Persisted to ~/.j2me/launcher.cfg so the library
 * doesn't have to be re-picked every run. Minimal file format — one line,
 * the absolute path — because there's only one setting worth remembering
 * for now.
 */
private val CONFIG_FILE = File(System.getProperty("user.home"), ".j2me/launcher.cfg")

private fun loadLastLibrary(): File? {
    return runCatching { File(CONFIG_FILE.readText().trim()).takeIf { it.isDirectory } }
        .getOrNull()
}

private fun saveLastLibrary(dir: File) {
    runCatching {
        CONFIG_FILE.parentFile?.mkdirs()
        CONFIG_FILE.writeText(dir.absolutePath)
    }
}

fun main() = application {
    val windowState = rememberWindowState(width = 960.dp, height = 720.dp)
    // Visibility is state-driven so the launcher can hide itself for the
    // duration of a game run — see LauncherScreen's onLaunch handler.
    // Compose's Window composable recomposes on `visible` changes without
    // tearing down its contents, so library state stays put.
    var launcherVisible by remember { mutableStateOf(true) }
    Window(
        onCloseRequest = ::exitApplication,
        title = "j2me launcher",
        state = windowState,
        visible = launcherVisible,
    ) {
        MaterialTheme(colorScheme = darkColorScheme()) {
            Surface(color = MaterialTheme.colorScheme.background) {
                LauncherScreen(
                    setLauncherVisible = { launcherVisible = it },
                )
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LauncherScreen(setLauncherVisible: (Boolean) -> Unit) {
    var libraryDir by remember { mutableStateOf(loadLastLibrary()) }
    // Where in the directory tree the user is currently viewing. Diverges
    // from libraryDir when they descend into a subfolder; pops back when
    // they hit the back button. Always null iff libraryDir is null.
    var currentDir by remember { mutableStateOf(libraryDir) }
    var items by remember { mutableStateOf(emptyList<LibraryItem>()) }
    var scanning by remember { mutableStateOf(false) }
    val snackbar = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()

    // Keep currentDir in sync when the root changes — picking a new library
    // folder should reset navigation to that folder, not leave us stranded
    // inside a subdir of the previous root.
    LaunchedEffect(libraryDir) {
        currentDir = libraryDir
    }

    // Rescan whenever the active folder changes. Off the main thread because
    // parsing 100+ JARs for manifest + icon blocks for several hundred ms.
    LaunchedEffect(currentDir) {
        val dir = currentDir ?: return@LaunchedEffect
        scanning = true
        items = withContext(Dispatchers.IO) { scanDir(dir) }
        scanning = false
    }

    // Whether we're below the chosen library root — drives the back button.
    val canGoBack = libraryDir != null && currentDir != null &&
        currentDir!!.absoluteFile != libraryDir!!.absoluteFile

    Scaffold(
        snackbarHost = { SnackbarHost(snackbar) },
        topBar = {
            CenterAlignedTopAppBar(
                navigationIcon = {
                    if (canGoBack) {
                        IconButton(onClick = {
                            currentDir = currentDir?.parentFile ?: libraryDir
                        }) {
                            Icon(
                                Icons.AutoMirrored.Filled.ArrowBack,
                                contentDescription = "Back",
                            )
                        }
                    }
                },
                title = {
                    val games = items.count { it is LibraryItem.Game }
                    val folders = items.count { it is LibraryItem.Folder }
                    val where = currentDir?.name ?: "j2me launcher"
                    val counts = buildString {
                        if (folders > 0) append("$folders folder${if (folders == 1) "" else "s"}")
                        if (games > 0) {
                            if (isNotEmpty()) append(" · ")
                            append("$games game${if (games == 1) "" else "s"}")
                        }
                    }
                    Text(
                        if (counts.isNotEmpty()) "$where · $counts" else where,
                    )
                },
                actions = {
                    IconButton(onClick = {
                        val picked = pickDirectory()
                        if (picked != null) {
                            libraryDir = picked
                            saveLastLibrary(picked)
                        }
                    }) {
                        Icon(Icons.Default.FolderOpen, contentDescription = "Choose library folder")
                    }
                    IconButton(
                        enabled = currentDir != null && !scanning,
                        onClick = { currentDir = currentDir },  // retrigger LaunchedEffect
                    ) {
                        Icon(Icons.Default.Refresh, contentDescription = "Rescan")
                    }
                },
            )
        },
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding)) {
            when {
                libraryDir == null -> EmptyStatePrompt(onPick = {
                    val picked = pickDirectory()
                    if (picked != null) {
                        libraryDir = picked
                        saveLastLibrary(picked)
                    }
                })
                scanning -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text("Scanning…")
                }
                items.isEmpty() -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(
                        "Nothing here — ${currentDir?.absolutePath}",
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                else -> LibraryList(
                    items = items,
                    onOpenFolder = { folder -> currentDir = folder },
                    onLaunch = { entry ->
                        scope.launch {
                            val process = try {
                                LaunchRuntime.launch(entry)
                            } catch (e: Exception) {
                                snackbar.showSnackbar("Launch failed: ${e.message}")
                                return@launch
                            }
                            // Hide the launcher while the game is in focus, then
                            // restore when the runtime subprocess exits.
                            setLauncherVisible(false)
                            try {
                                withContext(Dispatchers.IO) { process.waitFor() }
                            } finally {
                                setLauncherVisible(true)
                            }
                        }
                    },
                )
            }
        }
    }
}

@Composable
private fun EmptyStatePrompt(onPick: () -> Unit) {
    Column(
        Modifier.fillMaxSize(),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Icon(
            Icons.Default.VideogameAsset,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(64.dp),
        )
        Spacer(Modifier.height(16.dp))
        Text("Pick your games folder to get started",
             color = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.height(8.dp))
        IconButton(onClick = onPick) {
            Icon(Icons.Default.FolderOpen, contentDescription = "Choose library folder")
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun LibraryList(
    items: List<LibraryItem>,
    onOpenFolder: (File) -> Unit,
    onLaunch: (GameEntry) -> Unit,
) {
    // Discover keypad layouts once per list render — cheap (just a dir
    // listing) and the result feeds every per-game config dialog opened
    // from this list.
    val layouts = remember { discoverKeypadLayouts() }
    // Which game's config dialog is currently open (if any).
    var configFor by remember { mutableStateOf<GameEntry?>(null) }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(vertical = 8.dp),
    ) {
        items(
            items = items,
            key = { item ->
                when (item) {
                    is LibraryItem.Folder -> "F:" + item.dir.absolutePath
                    is LibraryItem.Game   -> "G:" + item.entry.jarPath.absolutePath
                }
            },
        ) { item ->
            when (item) {
                is LibraryItem.Folder ->
                    FolderRow(item, onClick = { onOpenFolder(item.dir) })
                is LibraryItem.Game ->
                    GameRow(
                        item.entry,
                        onClick = { onLaunch(item.entry) },
                        onConfig = { configFor = item.entry },
                    )
            }
        }
    }

    configFor?.let { entry ->
        GameConfigDialog(
            entry = entry,
            layouts = layouts,
            onDismiss = { configFor = null },
        )
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun FolderRow(folder: LibraryItem.Folder, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .combinedClickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // Mirror GameRow's icon slot so folder and game rows align to the
        // same left edge. Folder icon uses the same tinted surface as the
        // game icon fallback for visual consistency.
        Box(
            modifier = Modifier
                .size(48.dp)
                .background(
                    MaterialTheme.colorScheme.surfaceVariant,
                    RoundedCornerShape(8.dp),
                ),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                Icons.Default.Folder,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.size(14.dp))
        Column(Modifier.fillMaxWidth()) {
            Text(
                folder.displayName,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                "Folder",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun GameRow(
    entry: GameEntry,
    onClick: () -> Unit,
    onConfig: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .combinedClickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        GameIcon(entry)
        Spacer(Modifier.size(14.dp))
        // Title column takes whatever's left after icon (left) and the
        // overflow button (right). weight(1f) lets the title truncate on
        // narrow windows instead of pushing the menu button off-screen.
        Column(Modifier.weight(1f)) {
            Text(
                entry.displayTitle,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
            )
            val subtitle = buildString {
                entry.vendor?.let { append(it) }
                entry.version?.let {
                    if (isNotEmpty()) append(" · ")
                    append(it)
                }
            }
            if (subtitle.isNotEmpty()) {
                Text(
                    subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        // Three-dot overflow → per-game config dialog. Stops the row's
        // outer click so opening settings doesn't also launch the game.
        IconButton(onClick = onConfig) {
            Icon(
                Icons.Default.MoreVert,
                contentDescription = "Game settings",
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun GameIcon(entry: GameEntry) {
    Box(
        modifier = Modifier
            .size(48.dp)
            .background(
                MaterialTheme.colorScheme.surfaceVariant,
                RoundedCornerShape(8.dp),
            ),
        contentAlignment = Alignment.Center,
    ) {
        val icon = entry.icon
        if (icon != null) {
            Image(
                bitmap = icon,
                contentDescription = null,
                modifier = Modifier.size(36.dp),
                // MIDP icons are tiny (typically 12-24 px). Use nearest-neighbour
                // so sprites don't get smeared when upscaled into the row.
                filterQuality = FilterQuality.None,
            )
        } else {
            Icon(
                Icons.Default.VideogameAsset,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

/**
 * Directory picker. Swing's JFileChooser with DIRECTORIES_ONLY is the only
 * cross-platform option that reliably picks a folder *containing subfolders*
 * — AWT's FileDialog in directory mode silently refuses those on Linux.
 *
 * Uses the system look-and-feel so the dialog matches GTK/KDE/macOS/Windows
 * chrome rather than Swing's Metal default.
 */
private fun pickDirectory(): File? {
    runCatching {
        UIManager.setLookAndFeel(UIManager.getSystemLookAndFeelClassName())
    }
    val chooser = JFileChooser().apply {
        dialogTitle = "Pick games folder"
        fileSelectionMode = JFileChooser.DIRECTORIES_ONLY
        // Starting point: prior selection > user home. Users almost always
        // want to re-select from the same parent tree.
        val start = loadLastLibrary()?.parentFile ?: File(System.getProperty("user.home"))
        currentDirectory = start
    }
    val result = chooser.showOpenDialog(null)
    return if (result == JFileChooser.APPROVE_OPTION) chooser.selectedFile else null
}
