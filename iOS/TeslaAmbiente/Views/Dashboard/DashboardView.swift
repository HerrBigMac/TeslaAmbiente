// MARK: - DashboardView.swift
// Tesla Ambiente iOS App
// Haupt-Dashboard: Verbindungsstatus, Fahrzeugstatus, Quick-Controls

import SwiftUI

struct DashboardView: View {
    @EnvironmentObject var vm: MainViewModel
    @ObservedObject var ble = BLEManager.shared
    @State private var showScanner = false
    @State private var showPresetSaveSheet = false
    @State private var presetName = ""
    @State private var animatePulse = false

    var body: some View {
        NavigationStack {
            ZStack {
                LinearGradient(
                    colors: [Color(red: 0.06, green: 0.06, blue: 0.1), Color(red: 0.02, green: 0.02, blue: 0.06)],
                    startPoint: .top, endPoint: .bottom
                )
                .ignoresSafeArea()

                ScrollView {
                    VStack(spacing: 16) {
                        connectionHeaderCard
                        if ble.connectionState.isConnected {
                            vehicleStatusCard
                            quickControlsCard
                            presetsSection
                        } else {
                            disconnectedPlaceholder
                        }
                    }
                    .padding(.horizontal, 16)
                    .padding(.top, 8)
                    .padding(.bottom, 32)
                }
            }
            .navigationTitle("Tesla Ambiente")
            .navigationBarTitleDisplayMode(.large)
            .toolbarBackground(.ultraThinMaterial, for: .navigationBar)
            .sheet(isPresented: $showScanner) { BLEScannerView() }
            .sheet(isPresented: $showPresetSaveSheet) { presetSaveSheet }
            .overlay(alignment: .bottom) {
                if let feedback = vm.showFeedback {
                    FeedbackBanner(message: feedback.message, isError: feedback.isError)
                        .transition(.move(edge: .bottom).combined(with: .opacity))
                        .padding(.bottom, 8)
                }
            }
            .animation(.spring(response: 0.3), value: vm.showFeedback != nil)
        }
    }

    private var connectionHeaderCard: some View {
        GlassCard {
            HStack(spacing: 14) {
                ConnectionDot(state: ble.connectionState)
                VStack(alignment: .leading, spacing: 2) {
                    Text(ble.connectionState.displayName).font(.headline.weight(.semibold))
                    if let name = ble.peripheral?.name { Text(name).font(.caption).foregroundStyle(.secondary) }
                }
                Spacer()
                if ble.connectionState.isConnected {
                    Button { ble.requestDeviceInfo() } label: { Image(systemName: "info.circle.fill").symbolRenderingMode(.hierarchical).font(.title3) }.buttonStyle(.plain).foregroundStyle(.secondary)
                    Button { ble.disconnect() } label: { Image(systemName: "xmark.circle.fill").symbolRenderingMode(.hierarchical).font(.title3) }.buttonStyle(.plain).foregroundStyle(.red)
                } else {
                    Button { showScanner = true } label: {
                        Label("Verbinden", systemImage: "bluetooth").font(.subheadline.weight(.semibold)).padding(.horizontal, 14).padding(.vertical, 8).background(Color.accentColor.opacity(0.2)).clipShape(Capsule())
                    }.foregroundStyle(.accentColor)
                }
            }
        }
    }

    private var vehicleStatusCard: some View {
        GlassCard {
            VStack(spacing: 14) {
                HStack {
                    Label("Fahrzeugstatus", systemImage: "car.fill").font(.headline.weight(.semibold))
                    Spacer()
                    if ble.vehicleState.canDataFresh {
                        Circle().fill(.green).frame(width: 6, height: 6); Text("Live").font(.caption).foregroundStyle(.green)
                    } else {
                        Circle().fill(.orange).frame(width: 6, height: 6); Text("Veraltet").font(.caption).foregroundStyle(.orange)
                    }
                }
                Divider()
                HStack(alignment: .top, spacing: 20) {
                    CarDiagramView(vehicleState: ble.vehicleState, size: 120)
                    VStack(alignment: .leading, spacing: 10) {
                        statusRow(icon: "battery.100.bolt", label: "Akku") { BatteryView(percentage: ble.vehicleState.batterySOC, isCharging: ble.vehicleState.isCharging) }
                        statusRow(icon: "gearshift.layout.sixspeed", label: "Gang") { Text(ble.vehicleState.gear.displayName).font(.body.weight(.bold).monospacedDigit()) }
                        statusRow(icon: "sun.max.fill", label: "Helligkeit") { Text("\(ble.vehicleState.displayBrightness)%").font(.body.monospacedDigit()).foregroundStyle(.secondary) }
                        statusRow(icon: "gauge.with.needle", label: "Dashboard") { Text(ble.vehicleState.dashMode.displayName).font(.body).foregroundStyle(.accentColor) }
                        if ble.vehicleState.autopilotActive { statusRow(icon: "brain.head.profile", label: "Autopilot") { Text("Aktiv").font(.body.weight(.semibold)).foregroundStyle(.blue) } }
                    }.frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
    }

    private var quickControlsCard: some View {
        GlassCard {
            VStack(spacing: 14) {
                HStack { Label("Schnellzugriff", systemImage: "bolt.fill").font(.headline.weight(.semibold)); Spacer() }
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 10) {
                    QuickButton(title: "Alle An", icon: "lightbulb.fill", color: .yellow) { vm.turnOn(zone: .all) }
                    QuickButton(title: "Alle Aus", icon: "lightbulb.slash.fill", color: .gray) { vm.turnOff(zone: .all) }
                    QuickButton(title: "Rot", icon: "circle.fill", color: .red) { vm.ledSettings.color1 = .teslaRed; vm.ledSettings.effect = .staticColor; vm.sendCurrentSettings(to: .all) }
                    QuickButton(title: "Blau", icon: "circle.fill", color: .blue) { vm.ledSettings.color1 = .teslaBlue; vm.ledSettings.effect = .staticColor; vm.sendCurrentSettings(to: .all) }
                    QuickButton(title: "Regenbogen", icon: "rainbow", color: .purple) { vm.ledSettings.effect = .rainbow; vm.sendCurrentSettings(to: .all) }
                    QuickButton(title: "Atmen", icon: "lungs.fill", color: .teal) { vm.ledSettings.effect = .breathing; vm.sendCurrentSettings(to: .all) }
                    QuickButton(title: "Feuer", icon: "flame.fill", color: .orange) { vm.ledSettings.color1 = LEDColor(r:255,g:80,b:0); vm.ledSettings.effect = .fire; vm.sendCurrentSettings(to: .all) }
                    QuickButton(title: "Preset +", icon: "plus.circle.fill", color: .accentColor) { showPresetSaveSheet = true }
                }
            }
        }
    }

    private var presetsSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            if !vm.presets.isEmpty {
                HStack { Text("Presets").font(.headline.weight(.semibold)); Spacer() }.padding(.horizontal, 2)
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 10) {
                        ForEach(vm.presets) { preset in PresetChip(preset: preset) { vm.applyPreset(preset) } onDelete: { vm.deletePreset(preset) } }
                    }
                }
            }
        }
    }

    private var disconnectedPlaceholder: some View {
        GlassCard(padding: 40) {
            VStack(spacing: 20) {
                Image(systemName: "bluetooth.slash").font(.system(size: 60)).symbolRenderingMode(.hierarchical).foregroundStyle(.secondary)
                VStack(spacing: 8) {
                    Text("Nicht verbunden").font(.title3.weight(.semibold))
                    Text("Verbinde dich mit deinem Tesla Ambiente System, um die LEDs zu steuern.").font(.subheadline).foregroundStyle(.secondary).multilineTextAlignment(.center)
                }
                Button { showScanner = true } label: {
                    Label("Gerät suchen", systemImage: "bluetooth").font(.body.weight(.semibold)).frame(maxWidth: .infinity).padding(.vertical, 14).background(Color.accentColor).foregroundStyle(.white).clipShape(RoundedRectangle(cornerRadius: 14))
                }
            }
        }
    }

    private var presetSaveSheet: some View {
        NavigationStack {
            VStack(spacing: 20) {
                TextField("Preset-Name", text: $presetName).textFieldStyle(.roundedBorder).padding(.horizontal)
                HStack { Text("Farbe:"); ColorCircle(color: vm.ledSettings.color1, size: 32); Text(LEDEffect(rawValue: vm.ledSettings.effect.rawValue)?.displayName ?? "").foregroundStyle(.secondary) }
                Spacer()
            }
            .padding(.top, 20)
            .navigationTitle("Preset speichern")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Abbrechen") { showPresetSaveSheet = false } }
                ToolbarItem(placement: .confirmationAction) { Button("Speichern") { if !presetName.isEmpty { vm.saveCurrentAsPreset(name: presetName); presetName = ""; showPresetSaveSheet = false } }.disabled(presetName.isEmpty) }
            }
        }.presentationDetents([.medium])
    }

    @ViewBuilder
    private func statusRow<V: View>(icon: String, label: String, @ViewBuilder value: () -> V) -> some View {
        HStack(spacing: 8) { Image(systemName: icon).symbolRenderingMode(.hierarchical).foregroundStyle(.secondary).frame(width: 18); Text(label).font(.caption).foregroundStyle(.secondary); Spacer(); value() }
    }
}

struct QuickButton: View {
    let title: String; let icon: String; let color: Color; let action: () -> Void
    @State private var pressed = false
    var body: some View {
        Button(action: { withAnimation(.spring(response: 0.15)) { pressed = true }; DispatchQueue.main.asyncAfter(deadline: .now()+0.15) { withAnimation(.spring(response: 0.15)) { pressed = false } }; action() }) {
            VStack(spacing: 5) { Image(systemName: icon).font(.system(size: 20)).foregroundStyle(color); Text(title).font(.system(size: 10, weight: .medium)).foregroundStyle(.secondary).lineLimit(1).minimumScaleFactor(0.7) }
            .frame(maxWidth: .infinity).padding(.vertical, 12).background(color.opacity(0.1)).clipShape(RoundedRectangle(cornerRadius: 12)).overlay(RoundedRectangle(cornerRadius: 12).stroke(color.opacity(0.2), lineWidth: 1)).scaleEffect(pressed ? 0.93 : 1.0)
        }.buttonStyle(.plain)
    }
}

struct PresetChip: View {
    let preset: LEDPreset; let onApply: () -> Void; let onDelete: () -> Void
    var body: some View {
        Button(action: onApply) {
            HStack(spacing: 8) { ColorCircle(color: preset.color, size: 22); Text(preset.name).font(.subheadline.weight(.medium)).lineLimit(1); Button(action: onDelete) { Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary).font(.footnote) } }
            .padding(.horizontal, 12).padding(.vertical, 8).background(.ultraThinMaterial).clipShape(Capsule()).overlay(Capsule().stroke(.secondary.opacity(0.2), lineWidth: 1))
        }.buttonStyle(.plain)
    }
}

struct FeedbackBanner: View {
    let message: String; let isError: Bool
    var body: some View {
        HStack(spacing: 10) { Image(systemName: isError ? "exclamationmark.circle.fill" : "checkmark.circle.fill").foregroundStyle(isError ? .red : .green); Text(message).font(.subheadline.weight(.medium)) }
        .padding(.horizontal, 20).padding(.vertical, 12).background(.ultraThickMaterial).clipShape(Capsule()).shadow(color: .black.opacity(0.2), radius: 10, y: 4)
    }
}