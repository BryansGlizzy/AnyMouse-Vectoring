#!/usr/bin/env python3
"""
gui.py - Interactive 8-Octant Polar Mouse Sensitivity Calibration Studio
Integrated with OpenMouse Protocol for native hardware mouse telemetry and CPI sync.
Features:
  - Interactive Polar Radar chart with real-time draggable control handles
  - Native hardware detection (Finalmouse ULX, Attack Shark R3 / GearHub) over /dev/hidraw*
  - Real-time battery %, polling rate (Hz), Motion Sync, and physical CPI auto-sync
  - Direct hardware CPI write controls (Set 3200 / 6400 CPI on device)
  - 8-axis sliders with precision DoubleSpinBoxes & Effective DPI calculations
  - Master Sensitivity scale multiplier & Left/Right, Up/Down symmetry linkers
  - Preset manager (Neutral, Vertical Boost, Horizontal Boost, Diagonal ULX, Low)
  - Profile persistence (JSON export/import)
  - Zero-lag UDP streaming (port 42069) to octant_daemon on every adjustment
"""

import sys
import math
import json
import os
from PyQt6.QtCore import Qt, QPointF, pyqtSignal, QTimer
from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QGridLayout, QLabel, QSlider, QDoubleSpinBox, QPushButton,
    QGroupBox, QComboBox, QCheckBox, QFrame, QFileDialog, QMessageBox
)
from PyQt6.QtGui import (
    QPainter, QColor, QPen, QBrush, QPolygonF, QFont,
    QRadialGradient, QLinearGradient
)

from send_weights import send_weights, UDP_IP, UDP_PORT, PRESETS
from mouse_protocol import MouseHardwareManager, find_supported_devices

OCTANTS = [
    {"name": "East (0°)",       "short": "E",  "angle_deg": 0.0,   "desc": "Right lateral swipe"},
    {"name": "North-East (45°)", "short": "NE", "angle_deg": 45.0,  "desc": "Up-Right flick"},
    {"name": "North (90°)",      "short": "N",  "angle_deg": 90.0,  "desc": "Upward push"},
    {"name": "North-West (135°)","short": "NW", "angle_deg": 135.0, "desc": "Up-Left flick"},
    {"name": "West (180°)",      "short": "W",  "angle_deg": 180.0, "desc": "Left lateral swipe"},
    {"name": "South-West (225°)","short": "SW", "angle_deg": 225.0, "desc": "Down-Left pull"},
    {"name": "South (270°)",     "short": "S",  "angle_deg": 270.0, "desc": "Downward pull"},
    {"name": "South-East (315°)","short": "SE", "angle_deg": 315.0, "desc": "Down-Right pull"},
]

DARK_STYLE = """
QMainWindow, QWidget {
    background-color: #111318;
    color: #e2e8f0;
    font-family: 'Segoe UI', -apple-system, BlinkMacSystemFont, 'Inter', Roboto, sans-serif;
    font-size: 13px;
}
QGroupBox {
    background-color: #171922;
    border: 1px solid #262a38;
    border-radius: 10px;
    margin-top: 14px;
    padding-top: 18px;
    font-weight: bold;
    color: #94a3b8;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    padding: 2px 10px;
    color: #00f0ff;
}
QSlider::groove:horizontal {
    height: 6px;
    background: #242938;
    border-radius: 3px;
}
QSlider::sub-page:horizontal {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #00b4d8, stop:1 #00f0ff);
    border-radius: 3px;
}
QSlider::handle:horizontal {
    background: #ffffff;
    border: 2px solid #00f0ff;
    width: 16px;
    margin-top: -5px;
    margin-bottom: -5px;
    border-radius: 8px;
}
QSlider::handle:horizontal:hover {
    background: #00f0ff;
    border: 2px solid #ffffff;
}
QDoubleSpinBox, QComboBox {
    background-color: #1d212c;
    border: 1px solid #2e3547;
    border-radius: 6px;
    padding: 4px 8px;
    color: #00f0ff;
    font-weight: bold;
}
QDoubleSpinBox:focus, QComboBox:focus {
    border: 1px solid #00f0ff;
}
QPushButton {
    background-color: #212636;
    border: 1px solid #31384e;
    border-radius: 6px;
    padding: 6px 12px;
    font-weight: 600;
    color: #f1f5f9;
}
QPushButton:hover {
    background-color: #2c3348;
    border-color: #00f0ff;
    color: #00f0ff;
}
QPushButton:pressed {
    background-color: #00f0ff;
    color: #0f172a;
}
QPushButton#primaryBtn {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0ea5e9, stop:1 #00f0ff);
    color: #0f172a;
    border: none;
}
QPushButton#primaryBtn:hover {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #38bdf8, stop:1 #67e8f9);
}
QCheckBox {
    color: #94a3b8;
    spacing: 8px;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border-radius: 4px;
    border: 1px solid #334155;
    background-color: #1e293b;
}
QCheckBox::indicator:checked {
    background-color: #00f0ff;
    border-color: #00f0ff;
}
QLabel#badgeLabel {
    background-color: #1e2536;
    border: 1px solid #334155;
    border-radius: 6px;
    padding: 3px 8px;
    font-size: 11px;
    font-weight: bold;
}
"""

class PolarRadarWidget(QWidget):
    """Interactive Polar Radar chart visualizing and manipulating the 8 octants."""
    weightsChanged = pyqtSignal(list)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumSize(380, 380)
        self.weights = [0.25] * 8
        self.max_scale = 0.60
        self.dragging_idx = None
        self.hover_idx = None
        self.setMouseTracking(True)

    def setWeights(self, weights):
        self.weights = list(weights)
        max_w = max(self.weights)
        if max_w > self.max_scale * 0.9:
            self.max_scale = max_w * 1.25
        elif max_w < 0.35 and self.max_scale > 0.60:
            self.max_scale = 0.60
        self.update()

    def _get_center_and_radius(self):
        w = self.width()
        h = self.height()
        cx = w / 2.0
        cy = h / 2.0
        radius = min(cx, cy) - 45.0
        return cx, cy, max(radius, 50.0)

    def _octant_pos_screen(self, idx, weight, cx, cy, radius):
        deg = OCTANTS[idx]["angle_deg"]
        rad = math.radians(deg)
        norm_r = (weight / self.max_scale) * radius
        sx = cx + norm_r * math.cos(rad)
        sy = cy - norm_r * math.sin(rad)
        return QPointF(sx, sy)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        cx, cy, radius = self._get_center_and_radius()

        # Background circular gradient
        bg_grad = QRadialGradient(cx, cy, radius * 1.1)
        bg_grad.setColorAt(0.0, QColor(24, 28, 40))
        bg_grad.setColorAt(1.0, QColor(14, 16, 22))
        painter.setBrush(QBrush(bg_grad))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawEllipse(QPointF(cx, cy), radius * 1.05, radius * 1.05)

        # Concentric guideline circles
        grid_pen = QPen(QColor(46, 56, 77), 1, Qt.PenStyle.DashLine)
        painter.setPen(grid_pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)

        rings = 4
        font = QFont("sans-serif", 8)
        painter.setFont(font)
        for i in range(1, rings + 1):
            r = (radius / rings) * i
            val = (self.max_scale / rings) * i
            painter.drawEllipse(QPointF(cx, cy), r, r)
            painter.setPen(QColor(100, 116, 139))
            painter.drawText(int(cx + 4), int(cy - r + 12), f"{val:.2f}x")
            painter.setPen(grid_pen)

        # Draw spoke axes and labels
        spoke_pen = QPen(QColor(42, 50, 70), 1.5)
        painter.setPen(spoke_pen)
        label_font = QFont("sans-serif", 10, QFont.Weight.Bold)
        painter.setFont(label_font)

        for i, oct_info in enumerate(OCTANTS):
            deg = oct_info["angle_deg"]
            rad = math.radians(deg)
            ex = cx + radius * math.cos(rad)
            ey = cy - radius * math.sin(rad)
            painter.drawLine(QPointF(cx, cy), QPointF(ex, ey))

            lx = cx + (radius + 24.0) * math.cos(rad)
            ly = cy - (radius + 24.0) * math.sin(rad)
            painter.setPen(QColor(0, 240, 255) if i == self.hover_idx else QColor(148, 163, 184))
            rect_w, rect_h = 36, 18
            painter.drawText(
                int(lx - rect_w / 2), int(ly - rect_h / 2),
                rect_w, rect_h,
                int(Qt.AlignmentFlag.AlignCenter),
                oct_info["short"]
            )
            painter.setPen(spoke_pen)

        # Draw 8-octant sensitivity polygon
        polygon = QPolygonF()
        for i in range(8):
            polygon.append(self._octant_pos_screen(i, self.weights[i], cx, cy, radius))

        poly_grad = QRadialGradient(cx, cy, radius * 0.9)
        poly_grad.setColorAt(0.0, QColor(0, 240, 255, 110))
        poly_grad.setColorAt(0.7, QColor(14, 165, 233, 70))
        poly_grad.setColorAt(1.0, QColor(99, 102, 241, 30))

        painter.setBrush(QBrush(poly_grad))
        painter.setPen(QPen(QColor(0, 240, 255), 2.5, Qt.PenStyle.SolidLine))
        painter.drawPolygon(polygon)

        # Draw control handles
        for i in range(8):
            pos = polygon[i]
            is_active = (i == self.dragging_idx or i == self.hover_idx)
            handle_radius = 6.0 if is_active else 4.5

            painter.setPen(QPen(QColor(255, 255, 255), 2 if is_active else 1.5))
            painter.setBrush(QColor(255, 0, 128) if is_active else QColor(0, 240, 255))
            painter.drawEllipse(pos, handle_radius, handle_radius)

    def _find_nearest_octant(self, pos):
        cx, cy, radius = self._get_center_and_radius()
        dx = pos.x() - cx
        dy = -(pos.y() - cy)
        angle = math.degrees(math.atan2(dy, dx))
        if angle < 0:
            angle += 360.0

        best_idx = 0
        min_diff = 360.0
        for i, oct_info in enumerate(OCTANTS):
            diff = abs(angle - oct_info["angle_deg"])
            if diff > 180:
                diff = 360 - diff
            if diff < min_diff:
                min_diff = diff
                best_idx = i

        current_pt = self._octant_pos_screen(best_idx, self.weights[best_idx], cx, cy, radius)
        dist_to_handle = math.hypot(pos.x() - current_pt.x(), pos.y() - current_pt.y())

        if dist_to_handle < 25.0 or min_diff < 15.0:
            return best_idx
        return None

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            idx = self._find_nearest_octant(event.position())
            if idx is not None:
                self.dragging_idx = idx
                self._update_from_mouse(event.position())
                self.update()

    def mouseMoveEvent(self, event):
        if self.dragging_idx is not None:
            self._update_from_mouse(event.position())
            self.update()
        else:
            old_hover = self.hover_idx
            self.hover_idx = self._find_nearest_octant(event.position())
            if old_hover != self.hover_idx:
                self.update()

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.dragging_idx = None
            self.update()

    def _update_from_mouse(self, mouse_pos):
        if self.dragging_idx is None:
            return
        cx, cy, radius = self._get_center_and_radius()
        dist = math.hypot(mouse_pos.x() - cx, mouse_pos.y() - cy)
        new_weight = max(0.02, min((dist / radius) * self.max_scale, 2.5))
        self.weights[self.dragging_idx] = round(new_weight, 4)
        self.weightsChanged.emit(self.weights)


class OctantDpiGui(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("AnyMouse Vectoring - 8-Octant Polar DPI Studio")
        self.resize(1060, 760)
        self.setStyleSheet(DARK_STYLE)

        self.hardware_cpi = 3200
        self.weights = [0.25] * 8
        self.mirror_horizontal = False
        self.mirror_vertical = False
        self.active_mouse_node = None

        self._setup_ui()
        self._apply_preset_values("neutral")

        # Initial hardware telemetry query
        QTimer.singleShot(150, self._query_mouse_telemetry)

        # Periodic telemetry refresh timer (every 8 seconds)
        self.telemetry_timer = QTimer(self)
        self.telemetry_timer.timeout.connect(self._query_mouse_telemetry)
        self.telemetry_timer.start(8000)

    def _setup_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QHBoxLayout(central)
        main_layout.setContentsMargins(16, 16, 16, 16)
        main_layout.setSpacing(16)

        # Left Column: Polar Radar Chart + Presets + Hardware Config
        left_col = QVBoxLayout()
        left_col.setSpacing(12)

        # Hardware Sensor Header Box
        hw_box = QGroupBox("Hardware Mouse & Sensor Sync (OpenMouse Protocol)")
        hw_layout = QVBoxLayout(hw_box)
        hw_layout.setSpacing(8)

        # Row 1: Mouse Device Identity & Node
        row1 = QHBoxLayout()
        self.mouse_ident_lbl = QLabel("🖱️ Detecting Mouse...")
        self.mouse_ident_lbl.setStyleSheet("font-weight: bold; color: #f1f5f9; font-size: 13px;")
        row1.addWidget(self.mouse_ident_lbl)

        row1.addStretch()
        self.udp_status = QLabel("🟢 UDP: 127.0.0.1:42069 (Active)")
        self.udp_status.setStyleSheet("color: #10b981; font-weight: bold;")
        row1.addWidget(self.udp_status)
        hw_layout.addLayout(row1)

        # Row 2: Live Telemetry Badges
        self.badge_row = QHBoxLayout()
        self.badge_rate = QLabel("⚡ Polling: -- Hz")
        self.badge_rate.setObjectName("badgeLabel")
        self.badge_rate.setStyleSheet("color: #38bdf8;")
        self.badge_row.addWidget(self.badge_rate)

        self.badge_bat = QLabel("🔋 Battery: --%")
        self.badge_bat.setObjectName("badgeLabel")
        self.badge_bat.setStyleSheet("color: #4ade80;")
        self.badge_row.addWidget(self.badge_bat)

        self.badge_sync = QLabel("🎯 Motion Sync: --")
        self.badge_sync.setObjectName("badgeLabel")
        self.badge_sync.setStyleSheet("color: #f472b6;")
        self.badge_row.addWidget(self.badge_sync)

        self.badge_rssi = QLabel("📶 Signal: -- dBm")
        self.badge_rssi.setObjectName("badgeLabel")
        self.badge_rssi.setStyleSheet("color: #fbbf24;")
        self.badge_row.addWidget(self.badge_rssi)

        self.badge_row.addStretch()
        hw_layout.addLayout(self.badge_row)

        # Row 3: Physical CPI Controls & Auto-Detect
        row3 = QHBoxLayout()
        row3.addWidget(QLabel("Locked Sensor CPI:"))

        self.cpi_combo = QComboBox()
        self.cpi_combo.addItems(["800", "1600", "3200", "6400", "8000", "12000", "26000"])
        self.cpi_combo.setCurrentText("3200")
        self.cpi_combo.currentTextChanged.connect(self._on_cpi_changed)
        row3.addWidget(self.cpi_combo)

        btn_set_3200 = QPushButton("Set 3200 CPI")
        btn_set_3200.clicked.connect(lambda: self._set_device_cpi(3200))
        row3.addWidget(btn_set_3200)

        btn_set_6400 = QPushButton("Set 6400 CPI")
        btn_set_6400.clicked.connect(lambda: self._set_device_cpi(6400))
        row3.addWidget(btn_set_6400)

        btn_refresh_hw = QPushButton("🔄 Refresh")
        btn_refresh_hw.setToolTip("Query mouse hardware parameters over /dev/hidraw")
        btn_refresh_hw.clicked.connect(self._query_mouse_telemetry)
        row3.addWidget(btn_refresh_hw)

        row3.addStretch()
        hw_layout.addLayout(row3)
        left_col.addWidget(hw_box)

        # Interactive Radar Chart
        radar_box = QGroupBox("Polar Anisotropic Sensitivity Map (Click & Drag Axes)")
        radar_layout = QVBoxLayout(radar_box)
        self.radar = PolarRadarWidget()
        self.radar.weightsChanged.connect(self._on_radar_weights_changed)
        radar_layout.addWidget(self.radar)
        left_col.addWidget(radar_box, stretch=1)

        # Preset Quick Bar
        preset_box = QGroupBox("Quick Presets")
        preset_layout = QHBoxLayout(preset_box)
        
        btn_neutral = QPushButton("Neutral (0.25x)")
        btn_neutral.clicked.connect(lambda: self._apply_preset_values("neutral"))
        preset_layout.addWidget(btn_neutral)

        btn_vert = QPushButton("Vertical Boost")
        btn_vert.clicked.connect(lambda: self._apply_preset_values("vertical_boost"))
        preset_layout.addWidget(btn_vert)

        btn_horiz = QPushButton("Horizontal Boost")
        btn_horiz.clicked.connect(lambda: self._apply_preset_values("horizontal_boost"))
        preset_layout.addWidget(btn_horiz)

        btn_diag = QPushButton("Diagonal ULX")
        btn_diag.clicked.connect(lambda: self._apply_preset_values("diagonal_ulx"))
        preset_layout.addWidget(btn_diag)

        btn_low = QPushButton("Low (0.125x)")
        btn_low.clicked.connect(lambda: self._apply_preset_values("low"))
        preset_layout.addWidget(btn_low)

        left_col.addWidget(preset_box)
        main_layout.addLayout(left_col, stretch=5)

        # Right Column: Octant Sliders & Fine Controls
        right_col = QVBoxLayout()
        right_col.setSpacing(12)

        # Master Controls
        master_box = QGroupBox("Master Vector Scaling")
        master_layout = QGridLayout(master_box)

        master_layout.addWidget(QLabel("Global Multiplier:"), 0, 0)
        self.master_slider = QSlider(Qt.Orientation.Horizontal)
        self.master_slider.setRange(50, 400) # 0.05 to 4.00
        self.master_slider.setValue(100)
        self.master_slider.valueChanged.connect(self._on_master_slider_changed)
        master_layout.addWidget(self.master_slider, 0, 1)

        self.master_spin = QDoubleSpinBox()
        self.master_spin.setRange(0.05, 4.00)
        self.master_spin.setSingleStep(0.01)
        self.master_spin.setValue(1.00)
        self.master_spin.valueChanged.connect(self._on_master_spin_changed)
        master_layout.addWidget(self.master_spin, 0, 2)

        # Symmetry linking checkboxes
        sym_layout = QHBoxLayout()
        self.chk_mirror_lr = QCheckBox("Link Left / Right (E ↔ W, NE ↔ NW, SE ↔ SW)")
        self.chk_mirror_lr.toggled.connect(self._toggle_mirror_lr)
        sym_layout.addWidget(self.chk_mirror_lr)

        self.chk_mirror_ud = QCheckBox("Link Up / Down (N ↔ S)")
        self.chk_mirror_ud.toggled.connect(self._toggle_mirror_ud)
        sym_layout.addWidget(self.chk_mirror_ud)
        master_layout.addLayout(sym_layout, 1, 0, 1, 3)

        right_col.addWidget(master_box)

        # Individual 8 Octant Sliders
        oct_box = QGroupBox("Octant Fine Tuning")
        oct_layout = QGridLayout(oct_box)
        oct_layout.setVerticalSpacing(8)
        oct_layout.setHorizontalSpacing(10)

        oct_layout.addWidget(QLabel("<b>Octant Ray</b>"), 0, 0)
        oct_layout.addWidget(QLabel("<b>Multiplier</b>"), 0, 1)
        oct_layout.addWidget(QLabel("<b>Value</b>"), 0, 2)
        oct_layout.addWidget(QLabel("<b>Effective DPI</b>"), 0, 3)

        self.oct_sliders = []
        self.oct_spins = []
        self.oct_dpi_labels = []

        for i, oct_info in enumerate(OCTANTS):
            lbl = QLabel(f"<b>{oct_info['short']}</b> ({oct_info['angle_deg']}°)")
            lbl.setToolTip(f"{oct_info['name']} - {oct_info['desc']}")
            oct_layout.addWidget(lbl, i + 1, 0)

            slider = QSlider(Qt.Orientation.Horizontal)
            slider.setRange(20, 1000)
            slider.setValue(250)
            slider.valueChanged.connect(lambda val, idx=i: self._on_slider_changed(idx, val))
            oct_layout.addWidget(slider, i + 1, 1)
            self.oct_sliders.append(slider)

            spin = QDoubleSpinBox()
            spin.setRange(0.01, 3.00)
            spin.setDecimals(3)
            spin.setSingleStep(0.005)
            spin.setValue(0.250)
            spin.valueChanged.connect(lambda val, idx=i: self._on_spin_changed(idx, val))
            oct_layout.addWidget(spin, i + 1, 2)
            self.oct_spins.append(spin)

            dpi_lbl = QLabel("800 DPI")
            dpi_lbl.setStyleSheet("color: #38bdf8; font-weight: bold;")
            oct_layout.addWidget(dpi_lbl, i + 1, 3)
            self.oct_dpi_labels.append(dpi_lbl)

        right_col.addWidget(oct_box, stretch=1)

        # Profile Save / Load Actions
        action_layout = QHBoxLayout()
        btn_save = QPushButton("💾 Save Profile (JSON)")
        btn_save.clicked.connect(self._save_profile)
        action_layout.addWidget(btn_save)

        btn_load = QPushButton("📂 Load Profile")
        btn_load.clicked.connect(self._load_profile)
        action_layout.addWidget(btn_load)

        btn_send_now = QPushButton("🚀 Flush to Daemon")
        btn_send_now.setObjectName("primaryBtn")
        btn_send_now.clicked.connect(self._push_weights_to_daemon)
        action_layout.addWidget(btn_send_now)

        right_col.addLayout(action_layout)
        main_layout.addLayout(right_col, stretch=5)

    def _query_mouse_telemetry(self):
        """Query live hardware telemetry over WebHID/hidraw."""
        status = MouseHardwareManager.get_active_mouse_status()
        if not status or not status.get("connected"):
            self.mouse_ident_lbl.setText("🖱️ No OpenMouse compatible device detected")
            self.mouse_ident_lbl.setStyleSheet("color: #94a3b8; font-weight: bold;")
            return

        self.active_mouse_node = status.get("node")
        name = f"🖱️ {status.get('brand')} {status.get('model')}"
        if "serial" in status:
            name += f" ({status['serial']})"
        if self.active_mouse_node:
            name += f" · {self.active_mouse_node}"
        self.mouse_ident_lbl.setText(name)
        self.mouse_ident_lbl.setStyleSheet("color: #38bdf8; font-weight: bold;")

        # Update badges
        if "polling_rate_hz" in status:
            self.badge_rate.setText(f"⚡ {status['polling_rate_hz']} Hz")
        if "battery_pct" in status:
            mv_str = f" ({status.get('battery_mv', 0)/1000:.2f}V)" if "battery_mv" in status else ""
            self.badge_bat.setText(f"🔋 {status['battery_pct']}%{mv_str}")
        if "motion_sync" in status:
            sync_on = status['motion_sync']
            self.badge_sync.setText(f"🎯 Motion Sync: {'ON' if sync_on else 'OFF'}")
            self.badge_sync.setStyleSheet(f"color: {'#4ade80' if sync_on else '#f87171'};")
        if "rssi_dbm" in status:
            self.badge_rssi.setText(f"📶 {status['rssi_dbm']} dBm")

        # Auto-synchronize physical CPI setting
        if "dpi" in status:
            reported_dpi = status["dpi"]
            if reported_dpi != self.hardware_cpi:
                self.hardware_cpi = reported_dpi
                self.cpi_combo.blockSignals(True)
                self.cpi_combo.setCurrentText(str(reported_dpi))
                self.cpi_combo.blockSignals(False)
                self._update_all_dpi_labels()

    def _set_device_cpi(self, cpi: int):
        """Send hardware CPI command directly to the optical sensor."""
        success = MouseHardwareManager.set_mouse_cpi(cpi)
        if success:
            self.hardware_cpi = cpi
            self.cpi_combo.blockSignals(True)
            self.cpi_combo.setCurrentText(str(cpi))
            self.cpi_combo.blockSignals(False)
            self._update_all_dpi_labels()
            self._query_mouse_telemetry()
            QMessageBox.information(self, "Sensor CPI Updated", f"Mouse physical sensor successfully set to {cpi} CPI.")
        else:
            QMessageBox.warning(self, "Failed to Update CPI", "Could not send CPI command. Ensure mouse is connected and hidraw permissions are set.")

    def _on_cpi_changed(self, text):
        try:
            val = int(text)
            self.hardware_cpi = val
            self._update_all_dpi_labels()
        except ValueError:
            pass

    def _update_all_dpi_labels(self):
        for i in range(8):
            eff_dpi = int(round(self.weights[i] * self.hardware_cpi))
            self.oct_dpi_labels[i].setText(f"{eff_dpi} DPI")

    def _on_slider_changed(self, idx, val):
        w = val / 1000.0
        if abs(self.weights[idx] - w) > 0.0001:
            self._set_weight_val(idx, w)

    def _on_spin_changed(self, idx, val):
        if abs(self.weights[idx] - val) > 0.0001:
            self._set_weight_val(idx, val)

    def _set_weight_val(self, idx, val):
        val = max(0.01, min(val, 3.0))
        self.weights[idx] = val

        # Handle symmetry links
        if self.mirror_horizontal:
            mirror_pairs = {0: 4, 4: 0, 1: 3, 3: 1, 7: 5, 5: 7}
            if idx in mirror_pairs:
                partner = mirror_pairs[idx]
                self.weights[partner] = val

        if self.mirror_vertical:
            if idx == 2:
                self.weights[6] = val
            elif idx == 6:
                self.weights[2] = val

        self._sync_ui_from_weights()
        self._push_weights_to_daemon()

    def _on_radar_weights_changed(self, weights):
        self.weights = list(weights)
        self._sync_ui_from_weights()
        self._push_weights_to_daemon()

    def _sync_ui_from_weights(self):
        self.radar.blockSignals(True)
        self.radar.setWeights(self.weights)
        self.radar.blockSignals(False)

        for i in range(8):
            self.oct_sliders[i].blockSignals(True)
            self.oct_sliders[i].setValue(int(round(self.weights[i] * 1000.0)))
            self.oct_sliders[i].blockSignals(False)

            self.oct_spins[i].blockSignals(True)
            self.oct_spins[i].setValue(self.weights[i])
            self.oct_spins[i].blockSignals(False)

        self._update_all_dpi_labels()

    def _push_weights_to_daemon(self):
        try:
            send_weights(self.weights, host=UDP_IP, port=UDP_PORT)
            self.udp_status.setText(f"🟢 UDP: {UDP_IP}:{UDP_PORT} (Synced)")
            self.udp_status.setStyleSheet("color: #10b981; font-weight: bold;")
        except Exception as e:
            self.udp_status.setText(f"🔴 UDP Error: {e}")
            self.udp_status.setStyleSheet("color: #ef4444; font-weight: bold;")

    def _apply_preset_values(self, preset_key):
        if preset_key in PRESETS:
            self.weights = list(PRESETS[preset_key])
            self._sync_ui_from_weights()
            self._push_weights_to_daemon()

    def _on_master_slider_changed(self, val):
        ratio = val / 100.0
        self.master_spin.blockSignals(True)
        self.master_spin.setValue(ratio)
        self.master_spin.blockSignals(False)
        self._apply_master_scale(ratio)

    def _on_master_spin_changed(self, val):
        self.master_slider.blockSignals(True)
        self.master_slider.setValue(int(round(val * 100)))
        self.master_slider.blockSignals(False)
        self._apply_master_scale(val)

    def _apply_master_scale(self, factor):
        base = 0.25 * factor
        self.weights = [base] * 8
        self._sync_ui_from_weights()
        self._push_weights_to_daemon()

    def _toggle_mirror_lr(self, checked):
        self.mirror_horizontal = checked
        if checked:
            self.weights[4] = self.weights[0]
            self.weights[3] = self.weights[1]
            self.weights[5] = self.weights[7]
            self._sync_ui_from_weights()
            self._push_weights_to_daemon()

    def _toggle_mirror_ud(self, checked):
        self.mirror_vertical = checked
        if checked:
            self.weights[6] = self.weights[2]
            self._sync_ui_from_weights()
            self._push_weights_to_daemon()

    def _save_profile(self):
        path, _ = QFileDialog.getSaveFileName(self, "Save Octant Profile", "profile.json", "JSON Files (*.json)")
        if path:
            data = {
                "hardware_cpi": self.hardware_cpi,
                "octant_weights": self.weights,
                "effective_dpi": [int(round(w * self.hardware_cpi)) for w in self.weights]
            }
            with open(path, "w") as f:
                json.dump(data, f, indent=2)
            QMessageBox.information(self, "Profile Saved", f"Saved octant configuration to:\n{path}")

    def _load_profile(self):
        path, _ = QFileDialog.getOpenFileName(self, "Open Octant Profile", "", "JSON Files (*.json)")
        if path and os.path.exists(path):
            try:
                with open(path, "r") as f:
                    data = json.load(f)
                if "octant_weights" in data and len(data["octant_weights"]) == 8:
                    self.weights = data["octant_weights"]
                    if "hardware_cpi" in data:
                        self.hardware_cpi = data["hardware_cpi"]
                        self.cpi_combo.setCurrentText(str(self.hardware_cpi))
                    self._sync_ui_from_weights()
                    self._push_weights_to_daemon()
                    QMessageBox.information(self, "Profile Loaded", f"Loaded profile from:\n{path}")
            except Exception as e:
                QMessageBox.critical(self, "Error Loading Profile", f"Failed to parse JSON file:\n{e}")


def launch_gui():
    app = QApplication(sys.argv)
    window = OctantDpiGui()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    launch_gui()
