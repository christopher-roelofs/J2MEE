package org.j2me.launcher

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

/**
 * Per-game settings dialog. Reads the current config from disk on open,
 * lets the user edit, writes back on Save. Cancel discards.
 *
 * Field set is the same one the runtime understands today (resolution,
 * keypad layout, keypad visible). Adding more is "extend GameConfig +
 * one widget here" — no plumbing in between.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun GameConfigDialog(
    entry: GameEntry,
    layouts: List<String>,
    onDismiss: () -> Unit,
) {
    val stem = jarStem(entry.jarPath)
    val initial = remember(stem) { GameConfig.load(stem) }

    var resolution by remember { mutableStateOf(initial.resolution) }
    var layout by remember { mutableStateOf(initial.keypadLayout) }
    var visible by remember { mutableStateOf(initial.keypadVisible) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(entry.displayTitle) },
        text = {
            Column {
                LabeledField("Resolution") {
                    DropdownPicker(resolution, COMMON_RESOLUTIONS) { resolution = it }
                }
                Spacer(Modifier.height(12.dp))
                LabeledField("Keypad layout") {
                    DropdownPicker(layout, layouts) { layout = it }
                }
                Spacer(Modifier.height(12.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Show keypad on launch", Modifier.weight(1f))
                    Switch(checked = visible, onCheckedChange = { visible = it })
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                GameConfig(resolution.trim().ifEmpty { "auto" },
                           layout.trim().ifEmpty { "minimal" },
                           visible).save(stem)
                onDismiss()
            }) { Text("Save") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        },
    )
}

// Resolution suggestions — the same canonical feature-phone screen set
// freej2me-plus ships in AWTGUI.supportedResolutions. "auto" goes first
// to keep it the natural default. Users can still type a custom WxH
// straight into the field; the dropdown is just the suggestion list.
private val COMMON_RESOLUTIONS = listOf(
    "auto",
    "96x65", "101x64", "101x80",
    "128x128", "130x130",
    "120x160", "128x160",
    "132x176",
    "208x173", "176x208", "176x220", "220x176", "208x208",
    "180x320", "320x180",
    "240x240",
    "208x320", "240x320", "320x240",
    "240x400", "400x240",
    "240x432", "240x480",
    "360x360",
    "352x416",
    "360x640", "640x360",
    "640x480",
    "345x800", "800x345",
    "480x800", "800x480",
)

@Composable
private fun LabeledField(label: String, content: @Composable () -> Unit) {
    Column {
        Text(label, style = androidx.compose.material3.MaterialTheme.typography.labelMedium)
        Spacer(Modifier.height(4.dp))
        content()
    }
}

/**
 * Minimal Compose dropdown — text field + chevron acts as the anchor,
 * a popup with the layout names underneath. Material 3 ships an
 * ExposedDropdownMenuBox for this on Android but not yet on Desktop, so
 * we build it manually from primitives.
 */
@Composable
private fun DropdownPicker(
    selected: String,
    options: List<String>,
    onPick: (String) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    Box(Modifier.fillMaxWidth()) {
        OutlinedTextField(
            value = selected,
            onValueChange = onPick,
            singleLine = true,
            trailingIcon = {
                androidx.compose.material3.IconButton(onClick = { open = !open }) {
                    Icon(Icons.Default.ArrowDropDown, contentDescription = "Open layouts")
                }
            },
            modifier = Modifier.fillMaxWidth(),
        )
        DropdownMenu(
            expanded = open,
            onDismissRequest = { open = false },
        ) {
            options.forEach { name ->
                DropdownMenuItem(
                    text = { Text(name) },
                    onClick = {
                        onPick(name)
                        open = false
                    },
                )
            }
        }
    }
}
