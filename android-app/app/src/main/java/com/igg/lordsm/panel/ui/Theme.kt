package com.igg.lordsm.panel.ui

import android.app.Activity
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat

// Lords-adjacent palette: deep slate ground, IGG gold for primary, a cool
// cyan for interactive accents so "on" states read at a glance.
private val Gold = Color(0xFFD9A441)
private val GoldDim = Color(0xFF8A6524)
private val Cyan = Color(0xFF4FB3C4)
private val Danger = Color(0xFFE05B4B)
private val Slate = Color(0xFF14171C)
private val SlateUp = Color(0xFF1C2027)

private val DarkColors = darkColorScheme(
    primary = Gold,
    onPrimary = Color(0xFF1A1305),
    primaryContainer = GoldDim,
    onPrimaryContainer = Color(0xFFFFF3DC),
    secondary = Cyan,
    onSecondary = Color(0xFF04212A),
    tertiary = Danger,
    background = Slate,
    onBackground = Color(0xFFE6E8EC),
    surface = SlateUp,
    onSurface = Color(0xFFE6E8EC),
    surfaceVariant = Color(0xFF262B33),
    onSurfaceVariant = Color(0xFFB4BAC4),
    outline = Color(0xFF3C434E),
    error = Danger,
)

private val LightColors = lightColorScheme(
    primary = Color(0xFF8A6524),
    onPrimary = Color.White,
    secondary = Color(0xFF1F6E7C),
    background = Color(0xFFF7F8FA),
    surface = Color.White,
)

@Composable
fun LordsBotTheme(
    dark: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit,
) {
    val colors = if (dark) DarkColors else LightColors
    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as Activity).window
            window.statusBarColor = colors.background.toArgb()
            WindowCompat.getInsetsController(window, view).isAppearanceLightStatusBars = !dark
        }
    }
    MaterialTheme(colorScheme = colors, content = content)
}
